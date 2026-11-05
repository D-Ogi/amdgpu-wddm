// SPDX-License-Identifier: MIT
#include "native-residency-ddi.h"
#include "device-state.h"
#include "device-engine.h"
#include "residency.h"
#include "engine-ddi/engine-ddi.h"
#include <algorithm>
#include <atomic>
#include <vector>
#include <new>

namespace native12 {
namespace {
HRESULT collect(UINT count,const D3D12DDI_HANDLE_AND_TYPE* objects,
    ResidencyAllocationResolver resolve,void* cookie,std::vector<D3DKMT_HANDLE>& out) noexcept {
    if(count && (!objects || !resolve))return E_INVALIDARG;
    try{
        out.reserve(count);
        for(UINT i=0;i<count;++i){
            D3DKMT_HANDLE allocation=0;
            const HRESULT hr=resolve(cookie,objects[i],&allocation);
            if(hr==S_FALSE){if(allocation)return E_UNEXPECTED;continue;}
            if(hr!=S_OK)return FAILED(hr)?hr:E_UNEXPECTED;
            if(!allocation)return E_UNEXPECTED;
            out.push_back(allocation);
        }
        std::sort(out.begin(),out.end());out.erase(std::unique(out.begin(),out.end()),out.end());
    }catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
    catch(...){return E_UNEXPECTED;}
    return S_OK;
}
// BD-312: the allocation of each object, in the order of the objects; 0 for an object without one of its own (S_FALSE:
// descriptor and query heaps, reserved resources) and for one the resolver refuses. Offer and reclaim are no place to
// fail on such an object: an object that was not offered is not discarded, and the refusal is noted once.
HRESULT resolve_each(UINT count,const D3D12DDI_HANDLE_AND_TYPE* objects,
    ResidencyAllocationResolver resolve,void* cookie,std::vector<D3DKMT_HANDLE>& out,const char* slot) noexcept {
    if(count && (!objects || !resolve))return E_INVALIDARG;
    try{out.assign(count,0);}
    catch(...){return E_OUTOFMEMORY;}
    unsigned refused=0;HRESULT first=S_OK;
    for(UINT i=0;i<count;++i){
        D3DKMT_HANDLE allocation=0;
        const HRESULT hr=resolve(cookie,objects[i],&allocation);
        if(hr==S_OK && allocation)out[i]=allocation;
        else if(hr!=S_FALSE){if(!refused++)first=hr;}
    }
    static std::atomic<bool> noted{false};
    if(refused && !noted.exchange(true,std::memory_order_relaxed))
        ddi_refusal("%s: %u object(s) without a resolvable allocation (first %08lx): kept, not discarded",
            slot,refused,static_cast<unsigned long>(first));
    return S_OK;
}
// Under the device's offer_lock. offered stays sorted.
bool is_offered(const std::vector<D3DKMT_HANDLE>& offered,D3DKMT_HANDLE allocation) noexcept {
    return std::binary_search(offered.begin(),offered.end(),allocation);
}
bool removed_status(HRESULT hr) noexcept {
    return hr==D3DDDIERR_DEVICEREMOVED || hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET ||
        hr==DXGI_ERROR_DEVICE_HUNG;
}
class OfferLock final {
    SRWLOCK& lock_;
public:
    explicit OfferLock(SRWLOCK& lock) noexcept:lock_(lock) {AcquireSRWLockExclusive(&lock_);}
    ~OfferLock() {ReleaseSRWLockExclusive(&lock_);}
    OfferLock(const OfferLock&)=delete;
    OfferLock& operator=(const OfferLock&)=delete;
};
HRESULT resolve_engine(void* cookie,D3D12DDI_HANDLE_AND_TYPE object,D3DKMT_HANDLE* allocation) noexcept {
    if(!cookie){if(allocation)*allocation=0;return E_UNEXPECTED;}
    return engine_ddi::object_allocation(static_cast<engine_ddi::DeviceContext*>(cookie),object,allocation);
}
}
HRESULT native_residency_make(Device& device,D3D12DDIARG_MAKERESIDENT_0001* args,
    ResidencyAllocationResolver resolve,void* cookie) noexcept {
    if(!args)return E_INVALIDARG;
    args->WaitMask=0;
    // This device exposes exactly one physical adapter. Never touch an array
    // whose count does not match that supported shape.
    if(args->NumAdapters!=1)return E_NOTIMPL;
    if(args->pPagingFenceValue)args->pPagingFenceValue[0]=0;
    if(!args->pRTPagingQueue || !args->pRTPagingQueue[0].handle || !args->pPagingFenceValue || args->Flags.Reserved)return E_INVALIDARG;
    if(device.lost.load())return D3DDDIERR_DEVICEREMOVED;
    std::vector<D3DKMT_HANDLE> allocations;
    HRESULT hr=collect(args->NumObjects,args->pObjects,resolve,cookie,allocations);if(hr!=S_OK)return hr;
    // No residency is invented: no allocations means no kernel operation and
    // no valid wait fence (zero WaitMask/fence). S_FALSE resolver objects are
    // validated engine-internal heaps with no runtime-managed allocation.
    if(allocations.empty())return S_OK;
    RuntimeResidency runtime(device.runtime,device.callbacks);ResidencyResult result;
    hr=runtime.make_resident(args->pRTPagingQueue[0],allocations,args->Flags,result);
    if(result.state==ResidencyState::Unknown){device.remove();return FAILED(hr)?hr:E_UNEXPECTED;}
    if(hr==E_PENDING && result.state==ResidencyState::Pending){
        args->pPagingFenceValue[0]=result.paging_fence;args->WaitMask=1;
    }
    // BD-052: the runtime now holds the residency of these allocations, and the callback above is the one
    // residency operation this DDI makes - with the runtime's own flags on the runtime's paging queue, as
    // "the driver must forward the flags passed in to both MakeResident and Evict to the callback routines"
    // requires (DirectX-Specs ResourceHeaps.md @ 5a4139b, line 978). The driver takes no reference of its
    // own on top: a second one would be a reference the application's Evict cannot drop, which is the defect
    // this notice records the absence of. The notice is counted for the trace and the lab arm (heap-import.h,
    // note_runtime_residency).
    if(hr==S_OK || hr==E_PENDING)
        engine_note_runtime_residency(device,allocations.data(),static_cast<unsigned>(allocations.size()));
    return hr;
}
HRESULT native_residency_evict(Device& device,const D3D12DDIARG_EVICT* args,
    ResidencyAllocationResolver resolve,void* cookie) noexcept {
    if(!args || args->Flags.Reserved)return E_INVALIDARG;
    if(device.lost.load())return D3DDDIERR_DEVICEREMOVED;
    std::vector<D3DKMT_HANDLE> allocations;
    HRESULT hr=collect(args->NumObjects,args->pObjects,resolve,cookie,allocations);if(hr!=S_OK)return hr;
    if(allocations.empty())return S_OK;
    RuntimeResidency runtime(device.runtime,device.callbacks);
    hr=runtime.evict_after_gpu_retirement(allocations,args->Flags);
    if(SUCCEEDED(hr) && hr!=S_OK){device.remove();return E_UNEXPECTED;}
    // BD-052: the application's Evict reached the kernel. The driver holds no reference of its own to drop
    // by now; this is the retirement point that proves it (and the counter the lab arm reads).
    if(hr==S_OK)
        engine_note_runtime_eviction(device,allocations.data(),static_cast<unsigned>(allocations.size()));
    return hr;
}
// BD-312. "If the driver does not need to call pfnOfferAllocationsCb, it should return S_OK" (PFND3DDDI_OFFERALLOCATIONSCB,
// windows-driver-docs-ddi): an offer is the application's permission to discard, never an obligation, so every
// allocation the kernel does not take stays the application's with its content, and the slot still succeeds. The
// kernel refuses, for example, an allocation that is locked for a CPU mapping; the whole list is then offered one
// allocation at a time, so that one locked upload heap does not keep the others. Only a removed device fails it.
HRESULT native_residency_offer(Device& device,const D3D12DDIARG_OFFERRESOURCES* args,
    ResidencyAllocationResolver resolve,void* cookie) noexcept {
    if(!args)return E_INVALIDARG;
    if(device.lost.load())return D3DDDIERR_DEVICEREMOVED;
    std::vector<D3DKMT_HANDLE> want;
    if(resolve_each(args->NumObjects,args->pObjects,resolve,cookie,want,"OfferResources")!=S_OK)return S_OK;
    const auto offer=device.callbacks.pfnOfferAllocationsCb;
    // Without the reclaim callback nothing offered could be given back, so nothing is offered.
    if(!offer || !device.callbacks.pfnReclaimAllocations2Cb){
        static std::atomic<bool> noted{false};
        if(!noted.exchange(true,std::memory_order_relaxed))
            ddi_refusal("OfferResources: the runtime gave no offer or reclaim callback; nothing is offered");
        return S_OK;
    }
    OfferLock held(device.offer_lock);
    auto& offered=device.offered;
    try{
        std::sort(want.begin(),want.end());want.erase(std::unique(want.begin(),want.end()),want.end());
        // 0 (no allocation) and an allocation offered already leave the list: the kernel takes each offer once.
        want.erase(std::remove_if(want.begin(),want.end(),[&](D3DKMT_HANDLE a) noexcept {
            return !a || is_offered(offered,a);
        }),want.end());
        if(want.empty())return S_OK;
        // The room for every entry comes first: an allocation the kernel took must never miss the list, or its
        // reclaim would be skipped and the kernel would keep it offered under the application's next use.
        offered.reserve(offered.size()+want.size());
    }catch(...){return S_OK;}
    D3D12DDICB_OFFERALLOCATIONS cb{};
    cb.NumAllocations=static_cast<UINT>(want.size());cb.HandleList=want.data();cb.Priority=args->Priority;
    HRESULT hr=offer(device.runtime,&cb);
    unsigned kept=0;HRESULT refusal=S_OK;
    if(SUCCEEDED(hr)){
        offered.insert(offered.end(),want.begin(),want.end());
    } else if(removed_status(hr)){
        return D3DDDIERR_DEVICEREMOVED;
    } else {
        refusal=hr;
        for(auto allocation:want){
            D3D12DDICB_OFFERALLOCATIONS one{};one.NumAllocations=1;one.HandleList=&allocation;one.Priority=args->Priority;
            hr=offer(device.runtime,&one);
            if(SUCCEEDED(hr))offered.push_back(allocation);
            else if(removed_status(hr)){std::sort(offered.begin(),offered.end());return D3DDDIERR_DEVICEREMOVED;}
            else ++kept;
        }
    }
    std::sort(offered.begin(),offered.end());
    static std::atomic<bool> noted{false};
    if(kept && !noted.exchange(true,std::memory_order_relaxed))
        ddi_refusal("OfferResources: the kernel refused %u of %u allocation(s) (%08lx): kept with their content",
            kept,static_cast<unsigned>(want.size()),static_cast<unsigned long>(refusal));
    return S_OK;
}
// BD-312. Every allocation of the objects that this device offered goes back to the kernel in one callback on the
// runtime's paging queue; an object whose allocation was not offered (none of its own, or the kernel refused its
// offer) is not discarded. The kernel's per-allocation answer gives each object its pDiscarded. A pending page-in
// gives its fence: "the paging fence to synchronize against before submitting work to the GPU which references any of
// the resources" (D3D12DDICB_RECLAIMALLOCATIONS2). E_OUTOFMEMORY reclaims nothing, so the allocations stay offered.
// Any other failure leaves allocations the application now uses in an unknown state: the device is removed, and
// D3DDDIERR_DEVICEREMOVED is what the callback's contract says the DDI returns.
HRESULT native_residency_reclaim(Device& device,D3D12DDIARG_RECLAIMRESOURCES_0001* args,
    ResidencyAllocationResolver resolve,void* cookie) noexcept {
    if(!args)return E_INVALIDARG;
    args->WaitMask=0;
    // One physical adapter: an array of another count is never touched.
    if(args->NumAdapters!=1 || !args->pRTPagingQueue || !args->pPagingFenceValue)return E_INVALIDARG;
    args->pPagingFenceValue[0]=0;
    if(args->NumObjects && (!args->pObjects || !args->pDiscarded))return E_INVALIDARG;
    for(UINT i=0;i<args->NumObjects;++i)args->pDiscarded[i]=FALSE;
    if(device.lost.load())return D3DDDIERR_DEVICEREMOVED;
    std::vector<D3DKMT_HANDLE> of_object,list;std::vector<BOOL> discarded;
    if(resolve_each(args->NumObjects,args->pObjects,resolve,cookie,of_object,"ReclaimResources")!=S_OK)
        return E_OUTOFMEMORY;
    OfferLock held(device.offer_lock);
    auto& offered=device.offered;
    try{
        list.reserve(of_object.size());
        for(auto allocation:of_object)if(allocation && is_offered(offered,allocation))list.push_back(allocation);
        std::sort(list.begin(),list.end());list.erase(std::unique(list.begin(),list.end()),list.end());
        if(list.empty())return S_OK;
        discarded.assign(list.size(),FALSE);
    }catch(...){return E_OUTOFMEMORY;}
    const auto reclaim=device.callbacks.pfnReclaimAllocations2Cb;
    if(!reclaim || !args->pRTPagingQueue[0].handle){device.remove();return D3DDDIERR_DEVICEREMOVED;}
    D3D12DDICB_RECLAIMALLOCATIONS2 cb{};
    cb.NumAllocations=static_cast<UINT>(list.size());cb.HandleList=list.data();cb.pDiscarded=discarded.data();
    const HRESULT hr=reclaim(device.runtime,args->pRTPagingQueue[0],&cb);
    if(hr==E_OUTOFMEMORY)return hr;
    if(hr!=S_OK && hr!=E_PENDING){device.remove();return D3DDDIERR_DEVICEREMOVED;}
    if(hr==E_PENDING && (!cb.PagingFenceValue || cb.PagingFenceValue==UINT64_MAX)){device.remove();return D3DDDIERR_DEVICEREMOVED;}
    offered.erase(std::remove_if(offered.begin(),offered.end(),[&](D3DKMT_HANDLE a) noexcept {
        return std::binary_search(list.begin(),list.end(),a);
    }),offered.end());
    for(UINT i=0;i<args->NumObjects;++i){
        const auto allocation=of_object[i];
        if(!allocation)continue;
        const auto at=std::lower_bound(list.begin(),list.end(),allocation);
        if(at!=list.end() && *at==allocation)args->pDiscarded[i]=discarded[static_cast<size_t>(at-list.begin())]?TRUE:FALSE;
    }
    // "Bit i is set if PagingFenceValue[i] is valid. Only if MakeResident returns E_PENDING" (d3d12umddi.h,
    // D3D12DDIARG_RECLAIMRESOURCES_0001): S_OK means the allocations are usable now, so it carries no wait.
    if(hr==E_PENDING){args->pPagingFenceValue[0]=cb.PagingFenceValue;args->WaitMask=1;}
    return hr;
}
HRESULT APIENTRY native_make_resident(D3D12DDI_HDEVICE handle,D3D12DDIARG_MAKERESIDENT_0001* args){
    if(!handle.pDrvPrivate){if(args){args->WaitMask=0;if(args->NumAdapters==1 && args->pPagingFenceValue)args->pPagingFenceValue[0]=0;}return E_INVALIDARG;}
    auto& device=*static_cast<Device*>(handle.pDrvPrivate);
    return native_residency_make(device,args,resolve_engine,engine_context(device));
}
HRESULT APIENTRY native_evict(D3D12DDI_HDEVICE handle,const D3D12DDIARG_EVICT* args){
    if(!handle.pDrvPrivate)return E_INVALIDARG;
    auto& device=*static_cast<Device*>(handle.pDrvPrivate);
    return native_residency_evict(device,args,resolve_engine,engine_context(device));
}
HRESULT APIENTRY native_offer_resources(D3D12DDI_HDEVICE handle,const D3D12DDIARG_OFFERRESOURCES* args){
    if(!handle.pDrvPrivate)return E_INVALIDARG;
    auto& device=*static_cast<Device*>(handle.pDrvPrivate);
    return native_residency_offer(device,args,resolve_engine,engine_context(device));
}
HRESULT APIENTRY native_reclaim_resources(D3D12DDI_HDEVICE handle,D3D12DDIARG_RECLAIMRESOURCES_0001* args){
    if(!handle.pDrvPrivate){if(args){args->WaitMask=0;if(args->NumAdapters==1 && args->pPagingFenceValue)args->pPagingFenceValue[0]=0;}return E_INVALIDARG;}
    auto& device=*static_cast<Device*>(handle.pDrvPrivate);
    return native_residency_reclaim(device,args,resolve_engine,engine_context(device));
}
void install_native_residency_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088& table) noexcept {
    table.pfnMakeResident=native_make_resident;table.pfnEvict=native_evict;
    table.pfnOfferResources=native_offer_resources;table.pfnReclaimResources=native_reclaim_resources;
}
}
