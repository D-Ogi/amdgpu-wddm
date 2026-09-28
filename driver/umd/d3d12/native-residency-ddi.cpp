// SPDX-License-Identifier: MIT
#include "native-residency-ddi.h"
#include "device-state.h"
#include "device-engine.h"
#include "residency.h"
#include "engine-ddi/engine-ddi.h"
#include <algorithm>
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
void install_native_residency_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088& table) noexcept {
    table.pfnMakeResident=native_make_resident;table.pfnEvict=native_evict;
}
}
