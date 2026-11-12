// SPDX-License-Identifier: MIT
#include "ddi-dxgi-resources.h"
#include "runtime-surface-allocation.h"
#include "diagnostics.h"
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <iterator>
namespace bc250::umd {
namespace {
// BD-198: d3d11.dll removes the device for every status of these entries except S_OK. Its wrappers call
// NDXGI::CDevice::RemoveDevice for D3DDDIERR_DEVICEREMOVED and NDXGI::CDevice::DriverInternalError for any other
// nonzero value, success codes included (d3d11.dll 10.0.26100, SetEvictionPriority, QueryResourceResidency,
// ResolveSharedResourceImpl, OfferResourcesImpl, ReclaimResourcesImpl). Such a removal reaches no pfnSetErrorCb, so
// before this line it left no record in the game log. One line for each nonzero status, 16 for each process, in
// both flavours.
void dxgi_entry_note(const char *name,HRESULT status) {
    static std::atomic_uint left{16};
    unsigned count=left.load(std::memory_order_relaxed);
    while (count && !left.compare_exchange_weak(count,count-1,std::memory_order_relaxed)) {}
    if (!count) return;
    char text[192];
    std::snprintf(text,sizeof(text),"BC250 BD-198: DXGI %s returns %08X; the runtime removes the device for it\n",
        name,unsigned(status));
    OutputDebugStringA(text);
    amdgpu_wddm_log::print("%s",text);
}
template<typename F> HRESULT entry(const char *name,DXGI_DDI_HDEVICE h,F &&call) noexcept {
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(h);
    HRESULT status=E_INVALIDARG;
    if (storage && storage->owner) {
        auto &owner=*storage->owner;
        RuntimeDomain::Scope scope(owner.runtime().domain);
        try { status=ddi_device_status(call(owner)); }
        catch (const std::bad_alloc &) { status=E_OUTOFMEMORY; }
        catch (...) { status=E_FAIL; }
    }
    if (status!=S_OK) dxgi_entry_note(name,status);
    return status;
}
HRESULT allocation(DeviceOwner &owner,DXGI_DDI_HRESOURCE handle,D3DKMT_HANDLE &out) {
    auto *r=reinterpret_cast<DdiResource *>(handle);
    if (!r || !r->object) return E_INVALIDARG;
    auto *surface=r->runtime_surface;
    if (!surface) return E_NOTIMPL; // Engine-private: offer and reclaim ask the engine (BD-309), the rest are hints.
    // BD-373 step 3: a shared resource of the ENGINE LAYOUT form is a runtime surface too, offered and reclaimed by its
    // kernel allocation like a shared LINEAR surface. It is never a present source, so it has no present_allocation.
    const D3DKMT_HANDLE named=surface->form==SurfaceForm::engine_layout ? surface->allocation.allocation : r->present_allocation;
    if (surface->owner!=&owner.runtime() || surface->phase!=SurfacePhase::ready || surface->retained() ||
        surface->resource()!=r->object || !surface->allocation.allocation ||
        surface->allocation.allocation!=named) return E_INVALIDARG;
    out=surface->allocation.allocation; return S_OK;
}
// Offer, Reclaim, SetResourcePriority and QueryResourceResidency are hints for an engine-private resource (no runtime
// surface, so no kernel allocation the shell can name): its memory stays as it is, Reclaim reports its content kept
// and residency reports it resident. Refusing them is not an option: DXGI turns a failed Offer into
// DXGI_ERROR_DRIVER_INTERNAL_ERROR and removes the device (M14.1: Task Manager's XAML offers its surfaces right after
// device creation, and its device was removed and recreated in a loop, 2026-10-01). The runtime surfaces in the same
// batch still go to the kernel by allocation handle. `slot[i]` is the index of resource i in `handles`, or UINT_MAX
// for an engine-private resource. Since BD-309 Offer and Reclaim give an engine-private resource to the kernel when the
// engine and the ICD can name its allocations (engine_prepare_offer); it is a hint only when they cannot.
HRESULT allocation_batch(DeviceOwner &owner,const DXGI_DDI_HRESOURCE *resources,UINT count,
                         std::vector<D3DKMT_HANDLE> &handles,std::vector<UINT> &slot) {
    if (count && !resources) return E_INVALIDARG;
    handles.clear(); handles.reserve(count); slot.assign(count,UINT_MAX);
    for (UINT i=0;i<count;++i) {
        D3DKMT_HANDLE handle=0; HRESULT hr=allocation(owner,resources[i],handle);
        if (hr==E_NOTIMPL) continue; // Engine-private: a hint only.
        if (FAILED(hr)) return hr;
        for (D3DKMT_HANDLE seen:handles) if (seen==handle) return E_INVALIDARG;
        slot[i]=UINT(handles.size()); handles.push_back(handle);
    }
    return S_OK;
}
// BD-309: the memory of an engine-private resource as kernel allocations. The engine (IBc250DxvkDevice5::PrepareOffer)
// names the memory of a resource that owns its memory and releases its CPU maps; it never moves storage (BD-373). The
// hosted ICD names the one kernel allocation of each memory object (bc250GetMemoryAllocation). Such a resource then goes
// to the kernel with the runtime surfaces of the batch, and its reclaim reports what the kernel reports. A resource that
// the engine refuses (S_FALSE: shared, dynamic, packed into shared memory blocks, small staging storage), and every
// resource of an engine or ICD without these calls, stays a hint: nothing reaches the kernel, no EndOffer is owed, and
// the reclaim reports its content kept. An offer that fails ends what PrepareOffer started.
HRESULT engine_offer_ended(DeviceOwner &owner,HRESULT end) {
    if (SUCCEEDED(end)) return S_OK;
    // EndOffer: E_OUTOFMEMORY when the map of the memory failed. The resource is then unusable: report the device lost.
    if (end==E_OUTOFMEMORY) { latch_device_lost(owner.bridge()); return DXGI_ERROR_DEVICE_REMOVED; }
    return end;
}
HRESULT engine_end_offer(DeviceOwner &owner,DdiResource &r,UINT flags) {
    auto *engine=owner.offer_path().engine;
    const HRESULT end=engine ? engine->EndOffer(r.object,flags) : S_OK;
    std::fill(std::begin(r.engine_offered),std::end(r.engine_offered),D3DKMT_HANDLE(0)); r.engine_offered_count=0;
    return engine_offer_ended(owner,end);
}
HRESULT engine_end_offers(DeviceOwner &owner,const std::vector<DdiResource *> &prepared) {
    HRESULT result=S_OK;
    for (DdiResource *r:prepared) {
        const HRESULT end=engine_end_offer(owner,*r,0);
        if (FAILED(end) && SUCCEEDED(result)) result=end;
    }
    return result;
}
// S_OK with `taken` set and the allocations of the resource appended to `handles`, or S_OK with `taken` clear for a
// hint; a failure is the engine's device-loss status.
HRESULT prepare_engine_offer(DeviceOwner &owner,DdiResource &r,std::vector<D3DKMT_HANDLE> &handles,bool &taken) {
    taken=false;
    const EngineOfferPath &path=owner.offer_path();
    if (!path.ready() || !r.object || r.engine_offered_count || r.engine_decommitted) return S_OK;
    // Nothing is offered that the shell cannot reclaim. The reclaim of an engine allocation goes through
    // ReclaimAllocations3Cb or ReclaimAllocations2Cb on the device's paging queue (reclaim_allocations): the
    // synchronous ReclaimAllocationsCb answered E_FAIL for it on the lab, and d3d11.dll removed the device (kit-bd309
    // F-full, 2026-11-05). A device without a runtime surface has no paging queue yet, so the first engine offer
    // creates it; without the queue or a queued callback the resource stays a hint.
    const auto &callbacks=owner.runtime().KTCallbacks;
    if ((!callbacks.pfnReclaimAllocations3Cb && !callbacks.pfnReclaimAllocations2Cb) ||
        owner.ensure_paging_queue()!=S_OK) return S_OK;
    constexpr UINT capacity=DdiResource::kEngineOfferCapacity;
    BC250_DXVK_OFFER_MEMORY memory[capacity]{}; UINT count=0;
    const HRESULT hr=path.engine->PrepareOffer(r.object,capacity,memory,&count);
    if (FAILED(hr)) return hr;
    if (hr!=S_OK || !count || count>capacity) return S_OK;
    D3DKMT_HANDLE found[capacity]{};
    for (UINT i=0;i<count;++i) {
        MemoryAllocationInfo info{kMemoryAllocationRevision,UINT32(sizeof(MemoryAllocationInfo))};
        const VkResult vr=path.query(path.device,memory[i].Memory,&info);
        bool usable=vr==VK_SUCCESS && info.allocation && !(info.flags&kMemoryAllocationCpuMapped);
        for (UINT j=0;usable && j<i;++j) usable=found[j]!=info.allocation;
        for (D3DKMT_HANDLE seen:handles) usable=usable && seen!=info.allocation;
        if (!usable) {
            // Not one allocation of its own, or still CPU-locked: the offer of this resource is a hint after all.
            return engine_offer_ended(owner,path.engine->EndOffer(r.object,0));
        }
        found[i]=info.allocation;
    }
    std::copy(found,found+count,r.engine_offered); r.engine_offered_count=count;
    handles.insert(handles.end(),found,found+count);
    taken=true;
    return S_OK;
}
// The reclaim of a batch of runtime surfaces and offered engine resources. `result` gets the result of each resource:
// a runtime surface takes the result of its allocation. An offered engine resource takes the worst result of its
// allocations (NOT_COMMITTED, then DISCARDED, then OK); EndOffer receives DISCARDED or nothing, and a NOT_COMMITTED
// resource gets no EndOffer (DdiResource::engine_decommitted). A hint (an engine resource that was not offered)
// reports its content kept.
HRESULT reclaim_batch(DeviceOwner &owner,const char *name,const DXGI_DDI_HRESOURCE *resources,UINT count,
                      std::vector<D3DDDI_RECLAIM_RESULT> &result,UINT &kernel,UINT &engine,UINT &lost,UINT &gone);
// OfferResources and OfferResources1: the batch goes to the kernel with `asked` reduced by kernel_offer_flags.
HRESULT offer_batch(const char *name,DXGI_DDI_HDEVICE device,const DXGI_DDI_HRESOURCE *resources,UINT count,
                    D3DDDI_OFFER_PRIORITY priority,D3DDDI_OFFER_FLAGS asked) {
    UINT kernel=0,engine=0; D3DDDI_OFFER_FLAGS flags{};
    const HRESULT status=entry(name,device,[&](DeviceOwner &owner) {
        if (priority<D3DDDI_OFFER_PRIORITY_LOW || priority>D3DDDI_OFFER_PRIORITY_AUTO)
            return E_INVALIDARG;
        std::vector<D3DKMT_HANDLE> handles; std::vector<UINT> slot;
        HRESULT hr=allocation_batch(owner,resources,count,handles,slot);
        if (FAILED(hr)) return hr;
        // BD-309: the engine-private resources that can go to the kernel join the batch
        std::vector<DdiResource *> prepared;
        for (UINT i=0;i<count;++i) {
            if (slot[i]!=UINT_MAX) continue;
            auto *r=reinterpret_cast<DdiResource *>(resources[i]);
            bool taken=false;
            hr=prepare_engine_offer(owner,*r,handles,taken);
            if (FAILED(hr)) { engine_end_offers(owner,prepared); return hr; }
            if (taken) prepared.push_back(r);
        }
        kernel=UINT(handles.size()); engine=UINT(prepared.size());
        if (handles.empty()) return hr;
        // After the engine offers, so that the paging queue that the first one creates counts.
        flags=kernel_offer_flags(owner.runtime(),owner.surface_queue().queue,asked);
        hr=offer_after_submit(owner.runtime(),handles.data(),UINT(handles.size()),priority,flags,[&]() -> HRESULT {
            if (!owner.context() || !owner.device()) return E_FAIL;
            auto status=[&]() -> HRESULT {
                if (owner.bridge().device_lost || owner.bridge().submission_failed) return DXGI_ERROR_DEVICE_REMOVED;
                return owner.device()->GetDeviceRemovedReason();
            };
            HRESULT result=status(); if (FAILED(result)) return result;
            owner.context()->Flush(); // Engine E3: all prior commands submitted, not a CPU wait.
            return status();
        });
        // An offer that did not happen leaves nothing offered: the engine maps its memory again.
        if (FAILED(hr)) { engine_end_offers(owner,prepared); engine=0; }
        return hr;
    });
    residency_note("BC250 residency: %s resources=%u kernel=%u engine=%u priority=%u decommit=%u/%u returns %08X\n",
        name,count,kernel,engine,unsigned(priority),unsigned(flags.AllowDecommit),unsigned(asked.AllowDecommit),
        unsigned(status));
    return status;
}
HRESULT APIENTRY offer(DXGI_DDI_ARG_OFFERRESOURCES *args) {
    if (!args) { dxgi_entry_note("OfferResources",E_INVALIDARG); return E_INVALIDARG; }
    return offer_batch("OfferResources",args->hDevice,args->pResources,args->Resources,args->Priority,{});
}
// BD-283: one line for each reclaim that did not take the plain path, the answers of both callbacks and the fence in
// it, 16 for each process. The line is written for a recovered reclaim too (Reclaim2 refused, the synchronous
// callback took it), which ddi_status_note leaves out because the entry returns S_OK.
// `queued` is the version of the queued callback (3 or 2, "reclaim3=" or "reclaim2=" in the line).
HRESULT reclaim_note(const char *name,HRESULT result,UINT count,unsigned queued,HRESULT second,UINT64 fence,
                     HRESULT wait,HRESULT first) {
    static std::atomic_uint left{16};
    unsigned n=left.load(std::memory_order_relaxed);
    while (n && !left.compare_exchange_weak(n,n-1,std::memory_order_relaxed)) {}
    if (!n) return result;
    char text[256];
    std::snprintf(text,sizeof(text),"BC250 BD-283: %s returns %08X: allocations=%u reclaim%u=%08X fence=%llu "
        "wait=%08X sync=%08X\n",name,unsigned(result),count,queued,unsigned(second),
        static_cast<unsigned long long>(fence),unsigned(wait),unsigned(first));
    OutputDebugStringA(text);
    amdgpu_wddm_log::print("%s",text);
    return result;
}
// BD-283: the reclaim of the runtime surfaces in a batch, by allocation handle (BIND_PRESENT resources must be
// reclaimed that way). The surfaces are mapped and made resident on the device's surface paging queue, so the
// WDDM 2.0 callback ReclaimAllocations2Cb pages them in on that queue, and the entry waits for its paging fence
// before it returns: "the allocation is guaranteed to be resident and usable on the GPU when the fence is signaled"
// (display/offer-and-reclaim-changes.md), and the engine's later submissions do not wait on that queue. In
// 20261101T230251Z the synchronous ReclaimAllocationsCb returned E_FAIL, a status its reference page does not
// name, for the first reclaim of a ShellExperienceHost shown again after it was hidden, and d3d11.dll removed the
// device for it (Reason 6). The synchronous callback stays as the path for a device without a surface queue or
// without Reclaim2, and as the second try when Reclaim2 refuses with anything but removal. Every success is S_OK:
// d3d11.dll removes the device for any other status of this entry, success codes included.
// Unused answers read 1 (S_FALSE) in the line: "not called".
// BD-372: the queued callback is ReclaimAllocations3Cb where the runtime gives it (a WDDM 2.2 device, initialize), and
// ReclaimAllocations2Cb otherwise. Both page in on the same queue and give the same fence; the third answers a result
// per allocation, with NOT_COMMITTED for an allocation that an offer with AllowDecommit lost its backing to. The
// older two answer a discarded flag, read as DISCARDED or OK; kernel_offer_flags asks for a decommit only where the
// third is there. A value outside the enumeration counts as NOT_COMMITTED: it claims no usable memory.
// reclaim_batch takes the worst answer of a resource's allocations as the largest value (d3dukmdt.h).
static_assert(D3DDDI_RECLAIM_RESULT_OK<D3DDDI_RECLAIM_RESULT_DISCARDED &&
              D3DDDI_RECLAIM_RESULT_DISCARDED<D3DDDI_RECLAIM_RESULT_NOT_COMMITTED);
D3DDDI_RECLAIM_RESULT reclaim_result(D3DDDI_RECLAIM_RESULT value) {
    return value==D3DDDI_RECLAIM_RESULT_OK || value==D3DDDI_RECLAIM_RESULT_DISCARDED ? value :
        D3DDDI_RECLAIM_RESULT_NOT_COMMITTED;
}
void reclaim_results(const std::vector<BOOL> &discarded,std::vector<D3DDDI_RECLAIM_RESULT> &results) {
    for (size_t i=0;i<results.size();++i)
        results[i]=discarded[i] ? D3DDDI_RECLAIM_RESULT_DISCARDED : D3DDDI_RECLAIM_RESULT_OK;
}
HRESULT reclaim_allocations(DeviceOwner &owner,const char *name,std::vector<D3DKMT_HANDLE> &handles,
                            std::vector<D3DDDI_RECLAIM_RESULT> &results) {
    auto &runtime=owner.runtime();
    const auto &callbacks=runtime.KTCallbacks;
    const SurfacePagingQueue &queue=owner.surface_queue();
    const UINT count=UINT(handles.size());
    HRESULT second=S_FALSE,wait=S_FALSE,first=S_FALSE; UINT64 fence=0;
    const unsigned version=callbacks.pfnReclaimAllocations3Cb ? 3u : 2u;
    const bool queued=queue.queue && (callbacks.pfnReclaimAllocations3Cb || callbacks.pfnReclaimAllocations2Cb);
    std::vector<BOOL> discarded(count,FALSE);
    if (queued) {
        if (version==3) {
            D3DDDICB_RECLAIMALLOCATIONS3 request{};
            request.PagingQueue=queue.queue; request.NumAllocations=count;
            request.HandleList=handles.data(); request.pResults=results.data();
            second=callbacks.pfnReclaimAllocations3Cb(runtime.hDevice,&request);
            fence=request.PagingFenceValue;
            for (auto &value:results) value=reclaim_result(value);
        } else {
            D3DDDICB_RECLAIMALLOCATIONS2 request{};
            request.PagingQueue=queue.queue; request.NumAllocations=count;
            request.HandleList=handles.data(); request.pDiscarded=discarded.data();
            second=callbacks.pfnReclaimAllocations2Cb(runtime.hDevice,&request);
            fence=request.PagingFenceValue;
            reclaim_results(discarded,results);
        }
        // E_PENDING is not on the page; it is read as MakeResidentCb defines it: accepted, wait for the fence.
        if (SUCCEEDED(second) || second==E_PENDING) {
            wait=wait_paging_fence(runtime,queue,fence);
            if (wait==S_OK) return S_OK;
            return reclaim_note(name,ddi_device_status(wait),count,version,second,fence,wait,first);
        }
        if (second==D3DDDIERR_DEVICEREMOVED) return reclaim_note(name,second,count,version,second,fence,wait,first);
        std::fill(discarded.begin(),discarded.end(),FALSE);
    }
    if (!callbacks.pfnReclaimAllocationsCb)
        return queued ? reclaim_note(name,second,count,version,second,fence,wait,first) : E_NOTIMPL;
    D3DDDICB_RECLAIMALLOCATIONS request{};
    request.HandleList=handles.data(); request.NumAllocations=count; request.pDiscarded=discarded.data();
    first=callbacks.pfnReclaimAllocationsCb(runtime.hDevice,&request);
    reclaim_results(discarded,results);
    // The synchronous callback leaves the allocations resident when it returns.
    const HRESULT result=SUCCEEDED(first) ? S_OK : first;
    if (result==S_OK && !queued) return S_OK;
    return reclaim_note(name,result,count,version,second,fence,wait,first);
}
HRESULT reclaim_batch(DeviceOwner &owner,const char *name,const DXGI_DDI_HRESOURCE *resources,UINT count,
                      std::vector<D3DDDI_RECLAIM_RESULT> &result,UINT &kernel,UINT &engine,UINT &lost,UINT &gone) {
    std::vector<D3DKMT_HANDLE> handles; std::vector<UINT> slot;
    HRESULT hr=allocation_batch(owner,resources,count,handles,slot);
    kernel=UINT(handles.size()); engine=0; lost=0; gone=0;
    if (FAILED(hr)) return hr;
    // BD-309: the allocations of each offered engine resource follow those of the runtime surfaces
    std::vector<UINT> first(count,UINT_MAX);
    for (UINT i=0;i<count;++i) {
        if (slot[i]!=UINT_MAX) continue;
        const auto *r=reinterpret_cast<const DdiResource *>(resources[i]);
        if (!r->engine_offered_count) continue;
        first[i]=UINT(handles.size());
        for (UINT j=0;j<r->engine_offered_count;++j) {
            for (D3DKMT_HANDLE seen:handles) if (seen==r->engine_offered[j]) return E_INVALIDARG;
            handles.push_back(r->engine_offered[j]);
        }
    }
    kernel=UINT(handles.size());
    std::vector<D3DDDI_RECLAIM_RESULT> answers(handles.size(),D3DDDI_RECLAIM_RESULT_OK);
    if (!handles.empty()) {
        // A failed reclaim leaves the engine resources offered: their memory is not usable, so they are not ended.
        hr=reclaim_allocations(owner,name,handles,answers);
        if (FAILED(hr)) return hr;
    }
    // The counts of the residency line are kernel answers per allocation, as before BD-372.
    lost=UINT(std::count(answers.begin(),answers.end(),D3DDDI_RECLAIM_RESULT_DISCARDED));
    gone=UINT(std::count(answers.begin(),answers.end(),D3DDDI_RECLAIM_RESULT_NOT_COMMITTED));
    result.assign(count,D3DDDI_RECLAIM_RESULT_OK);
    HRESULT ended=S_OK;
    for (UINT i=0;i<count;++i) {
        auto *r=reinterpret_cast<DdiResource *>(resources[i]);
        if (slot[i]!=UINT_MAX) {
            result[i]=answers[slot[i]];
        } else if (first[i]!=UINT_MAX) {
            D3DDDI_RECLAIM_RESULT worst=D3DDDI_RECLAIM_RESULT_OK;
            for (UINT j=0;j<r->engine_offered_count;++j) worst=std::max(worst,answers[first[i]+j]);
            result[i]=worst; ++engine;
            if (worst==D3DDDI_RECLAIM_RESULT_NOT_COMMITTED) {
                // No EndOffer: it would map memory that has no backing. The engine keeps the resource as PrepareOffer
                // left it (unmapped, on memory of its own) until the application destroys it.
                r->engine_decommitted=true;
                std::fill(std::begin(r->engine_offered),std::end(r->engine_offered),D3DKMT_HANDLE(0));
                r->engine_offered_count=0;
            } else {
                const HRESULT end=engine_end_offer(owner,*r,
                    worst==D3DDDI_RECLAIM_RESULT_DISCARDED ? BC250_DXVK_END_OFFER_DISCARDED : 0u);
                if (FAILED(end) && SUCCEEDED(ended)) ended=end;
            }
        } else if (r->engine_decommitted) {
            result[i]=D3DDDI_RECLAIM_RESULT_NOT_COMMITTED; // no backing since an earlier reclaim
        }
    }
    return FAILED(ended) ? ended : hr;
}
// ReclaimResources answers a discarded flag per resource: NOT_COMMITTED, which only an offer with AllowDecommit
// (OfferResources1) can cause, is reported as discarded there, the nearest answer the flag has.
HRESULT APIENTRY reclaim(DXGI_DDI_ARG_RECLAIMRESOURCES *args) {
    if (!args) { dxgi_entry_note("ReclaimResources",E_INVALIDARG); return E_INVALIDARG; }
    UINT kernel=0,engine=0,lost=0,gone=0;
    const HRESULT status=entry("ReclaimResources",args->hDevice,[&](DeviceOwner &owner) {
        std::vector<D3DDDI_RECLAIM_RESULT> results;
        HRESULT hr=reclaim_batch(owner,"ReclaimResources",args->pResources,args->Resources,results,kernel,engine,lost,
            gone);
        if (FAILED(hr)) return hr;
        if (args->pDiscarded)
            for (UINT i=0;i<args->Resources;++i) args->pDiscarded[i]=results[i]!=D3DDDI_RECLAIM_RESULT_OK;
        return hr;
    });
    residency_note("BC250 residency: ReclaimResources resources=%u kernel=%u engine=%u discarded=%u notcommitted=%u "
        "returns %08X\n",args->Resources,kernel,engine,lost,gone,unsigned(status));
    return status;
}
// DXGI 1.6.1 (BD-099, BD-372). OfferResources1 adds D3DDDI_OFFER_FLAGS, whose one flag is AllowDecommit: the kernel
// may then decommit the backing store of an offered allocation, and the reclaim reports NOT_COMMITTED for it
// (offerresources1.md, dxgi_reclaim_resource_results.md: the resource and its content are lost for good). The offer
// goes to the kernel with the flag (OfferAllocations2Cb) where the shell can hear that result at the reclaim
// (kernel_offer_flags), and as the plain offer otherwise, which keeps the allocation committed. An offer without
// the flag is the OfferResources offer. A flag that the shell cannot take is not a reason to refuse the offer:
// DXGI turns a failed Offer into DXGI_ERROR_DRIVER_INTERNAL_ERROR and removes the device (see allocation_batch).
// For an engine-private resource (BD-309) the kernel decommits the memory that PrepareOffer gave it; the engine
// keeps that memory unmapped from the offer on, so a decommit leaves no CPU view of it, and reclaim_batch gives the
// engine no EndOffer for it.
HRESULT APIENTRY offer_resources1(DXGI_DDI_ARG_OFFERRESOURCES1 *args) {
    if (!args) { dxgi_entry_note("OfferResources1",E_INVALIDARG); return E_INVALIDARG; }
    return offer_batch("OfferResources1",args->hDevice,args->pResources,args->Resources,args->Priority,args->Flags);
}
// ReclaimResources1 answers with a result per resource: what the kernel answers for the allocation of a runtime
// surface, the worst answer of its allocations for an engine-private resource that went to the kernel (BD-309),
// NOT_COMMITTED again for an engine-private resource that lost its backing at an earlier reclaim, and OK for a
// resource whose offer was a hint only (see allocation_batch).
HRESULT APIENTRY reclaim_resources1(DXGI_DDI_ARG_RECLAIMRESOURCES1 *args) {
    if (!args) { dxgi_entry_note("ReclaimResources1",E_INVALIDARG); return E_INVALIDARG; }
    UINT kernel=0,engine=0,lost=0,gone=0;
    const HRESULT status=entry("ReclaimResources1",args->hDevice,[&](DeviceOwner &owner) {
        std::vector<D3DDDI_RECLAIM_RESULT> results;
        HRESULT hr=reclaim_batch(owner,"ReclaimResources1",args->pResources,args->Resources,results,kernel,engine,
            lost,gone);
        if (FAILED(hr)) return hr;
        if (args->pResults)
            for (UINT i=0;i<args->Resources;++i) args->pResults[i]=results[i];
        return hr;
    });
    residency_note("BC250 residency: ReclaimResources1 resources=%u kernel=%u engine=%u discarded=%u notcommitted=%u "
        "returns %08X\n",args->Resources,kernel,engine,lost,gone,unsigned(status));
    return status;
}
HRESULT APIENTRY display_mode(DXGI_DDI_ARG_SETDISPLAYMODE *args) {
    if (!args) { dxgi_entry_note("SetDisplayMode",E_INVALIDARG); return E_INVALIDARG; }
    return entry("SetDisplayMode",args->hDevice,[&](DeviceOwner &owner) {
        D3DKMT_HANDLE handle=0; HRESULT hr=allocation(owner,args->hResource,handle);
        if (FAILED(hr)) return hr;
        auto *resource=reinterpret_cast<DdiResource *>(args->hResource);
        // Runtime-imported textures currently have exactly one subresource.
        if (args->SubResourceIndex || resource->present_subresource) return E_INVALIDARG;
        auto &runtime=owner.runtime();
        if (!runtime.KTCallbacks.pfnSetDisplayModeCb) return E_NOTIMPL;
        D3DDDICB_SETDISPLAYMODE request{};
        request.hPrimaryAllocation=handle;
        // The runtime/KMD validates primary suitability and programs scanout.
        // No direct hardware access or CPU frame copy belongs in this entry.
        return runtime.KTCallbacks.pfnSetDisplayModeCb(runtime.hDevice,&request);
    });
}
HRESULT APIENTRY priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *args) {
    if (!args) { dxgi_entry_note("SetResourcePriority",E_INVALIDARG); return E_INVALIDARG; }
    return entry("SetResourcePriority",args->hDevice,[&](DeviceOwner &owner) {
        D3DKMT_HANDLE handle=0; HRESULT hr=allocation(owner,args->hResource,handle);
        if (hr==E_NOTIMPL) return S_OK; // Engine-private: a hint only (see allocation_batch).
        if (FAILED(hr)) return hr;
        auto &runtime=owner.runtime();
        if (!runtime.KTCallbacks.pfnSetPriorityCb) return E_NOTIMPL;
        D3DDDICB_SETPRIORITY request{}; request.NumAllocations=1;
        request.HandleList=&handle; request.pPriorities=&args->Priority;
        return runtime.KTCallbacks.pfnSetPriorityCb(runtime.hDevice,&request);
    });
}
// The answer is the status of each resource in pStatus, and the entry returns S_OK. The reference page asks for
// S_NOT_RESIDENT or S_RESIDENT_IN_SHARED_MEMORY as the return value, but d3d11.dll treats each nonzero return as a
// driver error and removes the device (NDXGI::CDevice::QueryResourceResidency, d3d11.dll 10.0.26100): it reads the
// residency from pStatus only. A status that the callback does not define is reported as shared memory, which
// claims no GPU-local residency: the query is a hint (an approximation, says the page), not a reason to remove a
// device.
HRESULT APIENTRY residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *args) {
    if (!args) { dxgi_entry_note("QueryResourceResidency",E_INVALIDARG); return E_INVALIDARG; }
    return entry("QueryResourceResidency",args->hDevice,[&](DeviceOwner &owner) {
        if (!args->Resources) return S_OK;
        if (args->Resources>UINT32_MAX || !args->pResources || !args->pStatus) return E_INVALIDARG;
        auto &runtime=owner.runtime();
        std::vector<D3DKMT_HANDLE> handles; handles.reserve(args->Resources);
        std::vector<SIZE_T> slot(args->Resources,SIZE_MAX);
        std::vector<DXGI_DDI_RESIDENCY> result(args->Resources,DXGI_DDI_RESIDENCY_FULLY_RESIDENT);
        for (SIZE_T i=0;i<args->Resources;++i) {
            D3DKMT_HANDLE handle=0; HRESULT hr=allocation(owner,args->pResources[i],handle);
            if (hr==E_NOTIMPL) continue; // Engine-private: reported resident (see allocation_batch).
            if (FAILED(hr)) return hr;
            slot[i]=handles.size(); handles.push_back(handle);
        }
        std::vector<D3DDDI_RESIDENCYSTATUS> status(handles.size());
        if (!handles.empty()) {
            if (!runtime.KTCallbacks.pfnQueryResidencyCb) return E_NOTIMPL;
            D3DDDICB_QUERYRESIDENCY request{}; request.NumAllocations=UINT(handles.size());
            request.HandleList=handles.data(); request.pResidencyStatus=status.data();
            HRESULT hr=runtime.KTCallbacks.pfnQueryResidencyCb(runtime.hDevice,&request);
            if (FAILED(hr)) return hr;
        }
        for (SIZE_T i=0;i<args->Resources;++i) {
            if (slot[i]==SIZE_MAX) continue;
            switch (status[slot[i]]) {
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINGPUMEMORY: result[i]=DXGI_DDI_RESIDENCY_FULLY_RESIDENT; break;
            case D3DDDI_RESIDENCYSTATUS_NOTRESIDENT: result[i]=DXGI_DDI_RESIDENCY_EVICTED_TO_DISK; break;
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINSHAREDMEMORY:
            default: result[i]=DXGI_DDI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY; break;
            }
        }
        for (SIZE_T i=0;i<args->Resources;++i) args->pStatus[i]=result[i];
        return S_OK;
    });
}
// The runtime resolves a resource at IDXGIKeyedMutex::ReleaseSync and at IDXGISurface1::GetDC of a GDI-compatible
// surface (dxgiddi.md, ResolveSharedResourceDXGI): the driver finishes the work that writes the resource. An
// engine-private resource has no kernel allocation to name, but its work is still the engine's to finish, so it
// takes the same engine wait (before BD-198 it returned E_NOTIMPL, and the runtime removed the device). Only a
// runtime surface can be on a Present, so only a runtime surface waits for the Presents. A positive status of the
// engine wait is S_OK here: the runtime treats every nonzero return as a driver error.
HRESULT APIENTRY resolve(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *args) {
    if (!args) { dxgi_entry_note("ResolveSharedResource",E_INVALIDARG); return E_INVALIDARG; }
    return entry("ResolveSharedResource",args->hDevice,[&](DeviceOwner &owner) {
        D3DKMT_HANDLE handle=0; HRESULT hr=allocation(owner,args->hResource,handle);
        if (FAILED(hr) && hr!=E_NOTIMPL) return hr;
        const bool runtimeSurface=hr==S_OK;
        if (!owner.engine()) return E_FAIL;
        if (runtimeSurface) {
            hr=wait_present_idle(owner.bridge());
            if (FAILED(hr)) return hr;
        }
        // Single GPU, one shared image: no merge or CPU copy. The engine flushes
        // pending resource work and waits before ownership is handed away.
        hr=owner.engine()->WaitForResourceIdle(reinterpret_cast<DdiResource *>(args->hResource)->object);
        return FAILED(hr) ? hr : S_OK;
    });
}
}
HRESULT engine_prepare_offer(DeviceOwner &owner,DdiResource &resource,std::vector<D3DKMT_HANDLE> &handles,bool &taken) {
    return prepare_engine_offer(owner,resource,handles,taken);
}
void residency_note(const char *format,...) noexcept {
    static LineAllowance allowance{64,8};
    unsigned skipped=0; bool last=false;
    if (!allowance.take(GetTickCount64()/60000,skipped,last)) return;
    char text[256];
    if (skipped) {
        std::snprintf(text,sizeof(text),"BC250 residency: %u lines skipped by the limit of 8 a minute\n",skipped);
        OutputDebugStringA(text); amdgpu_wddm_log::print("%s",text);
    }
    va_list args;
    va_start(args,format);
    std::vsnprintf(text,sizeof(text),format,args);
    va_end(args);
    OutputDebugStringA(text); amdgpu_wddm_log::print("%s",text);
}
void install_dxgi_resource_ddi(DXGI1_2_DDI_BASE_FUNCTIONS &table) {
    table.pfnOfferResources=offer;
    table.pfnReclaimResources=reclaim;
    table.pfnSetDisplayMode=display_mode;
    table.pfnSetResourcePriority=priority;
    table.pfnQueryResourceResidency=residency;
    table.pfnResolveSharedResource=resolve;
}
void install_dxgi1_6_1_resource_ddi(DXGI1_6_1_DDI_BASE_FUNCTIONS &table) {
    table.pfnOfferResources1=offer_resources1;
    table.pfnReclaimResources1=reclaim_resources1;
}
}
