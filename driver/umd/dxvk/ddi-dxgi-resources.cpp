// SPDX-License-Identifier: MIT
#include "ddi-dxgi-resources.h"
#include <d3d9.h> // S_NOT_RESIDENT / S_RESIDENT_IN_SHARED_MEMORY, DXGI DDI contract.
#include <climits>
#include <cstdint>
namespace bc250::umd {
namespace {
template<typename F> HRESULT entry(DXGI_DDI_HDEVICE h,F &&call) noexcept {
    auto *storage=reinterpret_cast<DdiDeviceHandle *>(h);
    if (!storage || !storage->owner) return E_INVALIDARG;
    auto &owner=*storage->owner;
    RuntimeDomain::Scope scope(owner.runtime().domain);
    try { return ddi_device_status(call(owner)); }
    catch (const std::bad_alloc &) { return E_OUTOFMEMORY; }
    catch (...) { return E_FAIL; }
}
HRESULT allocation(DeviceOwner &owner,DXGI_DDI_HRESOURCE handle,D3DKMT_HANDLE &out) {
    auto *r=reinterpret_cast<DdiResource *>(handle);
    if (!r || !r->object) return E_INVALIDARG;
    auto *surface=r->runtime_surface;
    if (!surface) return E_NOTIMPL; // Engine-private allocation introspection remains pending.
    if (surface->owner!=&owner.runtime() || surface->phase!=SurfacePhase::ready || surface->texture.retained ||
        surface->texture.texture!=r->object || !surface->allocation.allocation ||
        surface->allocation.allocation!=r->present_allocation) return E_INVALIDARG;
    out=surface->allocation.allocation; return S_OK;
}
// Offer, Reclaim, SetResourcePriority and QueryResourceResidency are hints for an engine-private resource (no runtime
// surface, so no kernel allocation the shell can name): its memory stays as it is, Reclaim reports its content kept
// and residency reports it resident. Refusing them is not an option: DXGI turns a failed Offer into
// DXGI_ERROR_DRIVER_INTERNAL_ERROR and removes the device (M14.1: Task Manager's XAML offers its surfaces right after
// device creation, and its device was removed and recreated in a loop, 2026-10-01). The runtime surfaces in the same
// batch still go to the kernel by allocation handle. `slot[i]` is the index of resource i in `handles`, or UINT_MAX
// for an engine-private resource.
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
HRESULT APIENTRY offer(DXGI_DDI_ARG_OFFERRESOURCES *args) {
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
        if (args->Priority<D3DDDI_OFFER_PRIORITY_LOW || args->Priority>D3DDDI_OFFER_PRIORITY_AUTO)
            return E_INVALIDARG;
        std::vector<D3DKMT_HANDLE> handles; std::vector<UINT> slot;
        HRESULT hr=allocation_batch(owner,args->pResources,args->Resources,handles,slot);
        if (FAILED(hr) || handles.empty()) return hr;
        return offer_after_submit(owner.runtime(),handles.data(),UINT(handles.size()),args->Priority,[&]() -> HRESULT {
            if (!owner.context() || !owner.device()) return E_FAIL;
            auto status=[&]() -> HRESULT {
                if (owner.bridge().device_lost || owner.bridge().submission_failed) return DXGI_ERROR_DEVICE_REMOVED;
                return owner.device()->GetDeviceRemovedReason();
            };
            HRESULT result=status(); if (FAILED(result)) return result;
            owner.context()->Flush(); // Engine E3: all prior commands submitted, not a CPU wait.
            return status();
        });
    });
}
HRESULT APIENTRY reclaim(DXGI_DDI_ARG_RECLAIMRESOURCES *args) {
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
        std::vector<D3DKMT_HANDLE> handles; std::vector<UINT> slot;
        HRESULT hr=allocation_batch(owner,args->pResources,args->Resources,handles,slot);
        if (FAILED(hr)) return hr;
        std::vector<BOOL> discarded(handles.size(),FALSE);
        if (!handles.empty()) {
            auto &runtime=owner.runtime();
            if (!runtime.KTCallbacks.pfnReclaimAllocationsCb) return E_NOTIMPL;
            D3DDDICB_RECLAIMALLOCATIONS request{};
            // BIND_PRESENT resources must be reclaimed by allocation handle.
            request.HandleList=handles.data(); request.NumAllocations=UINT(handles.size());
            request.pDiscarded=discarded.data();
            hr=runtime.KTCallbacks.pfnReclaimAllocationsCb(runtime.hDevice,&request);
            if (FAILED(hr)) return hr;
            // This synchronous callback returns resident allocations. Reclaim2 would
            // require retaining and waiting its paging fence before further GPU use.
        }
        if (args->pDiscarded)
            for (UINT i=0;i<args->Resources;++i) args->pDiscarded[i]=slot[i]==UINT_MAX ? FALSE : discarded[slot[i]];
        return hr;
    });
}
// DXGI 1.6.1 (BD-099). OfferResources1 adds D3DDDI_OFFER_FLAGS, whose one flag is AllowDecommit: the
// kernel may then give the pages back and the resource comes back without its contents, which Reclaim
// reports as D3DDDI_RECLAIM_RESULT_NOT_COMMITTED. The shell does not take that offer. It reclaims with the
// synchronous WDDM 2.0 callback, which has no paging fence of its own to wait for and no "not committed"
// answer, so a decommitted allocation would have no result it could report. The offer itself, which is the
// work of the entry, goes to the kernel as before: the priority stays, and the allocation stays committed.
// A flag the shell does not take is not a reason to refuse the offer: DXGI turns a failed Offer into
// DXGI_ERROR_DRIVER_INTERNAL_ERROR and removes the device (see the batch helper above). The flags are
// dropped, which keeps every offered allocation committed.
HRESULT APIENTRY offer_resources1(DXGI_DDI_ARG_OFFERRESOURCES1 *args) {
    if (!args) return E_INVALIDARG;
    DXGI_DDI_ARG_OFFERRESOURCES older{};
    older.hDevice=args->hDevice; older.pResources=args->pResources;
    older.Resources=args->Resources; older.Priority=args->Priority;
    return offer(&older);
}
// ReclaimResources1 answers with a result per resource. The kernel callback is the WDDM 2.0 one, which
// answers with a discarded flag per allocation: a reclaimed resource is OK, a discarded one DISCARDED.
// NOT_COMMITTED cannot arise, because no offer of this shell allows a decommit (offer_resources1). An
// engine-private resource is a hint only, as in the batch helper above, so it reports OK.
HRESULT APIENTRY reclaim_resources1(DXGI_DDI_ARG_RECLAIMRESOURCES1 *args) {
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
        std::vector<D3DKMT_HANDLE> handles; std::vector<UINT> slot;
        HRESULT hr=allocation_batch(owner,args->pResources,args->Resources,handles,slot);
        if (FAILED(hr)) return hr;
        std::vector<BOOL> discarded(handles.size(),FALSE);
        if (!handles.empty()) {
            auto &runtime=owner.runtime();
            if (!runtime.KTCallbacks.pfnReclaimAllocationsCb) return E_NOTIMPL;
            D3DDDICB_RECLAIMALLOCATIONS request{};
            // BIND_PRESENT resources must be reclaimed by allocation handle.
            request.HandleList=handles.data(); request.NumAllocations=UINT(handles.size());
            request.pDiscarded=discarded.data();
            hr=runtime.KTCallbacks.pfnReclaimAllocationsCb(runtime.hDevice,&request);
            if (FAILED(hr)) return hr;
        }
        if (args->pResults)
            for (UINT i=0;i<args->Resources;++i)
                args->pResults[i]=slot[i]!=UINT_MAX && discarded[slot[i]] ?
                    D3DDDI_RECLAIM_RESULT_DISCARDED : D3DDDI_RECLAIM_RESULT_OK;
        return hr;
    });
}
HRESULT APIENTRY display_mode(DXGI_DDI_ARG_SETDISPLAYMODE *args) {
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
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
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
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
HRESULT APIENTRY residency(DXGI_DDI_ARG_QUERYRESOURCERESIDENCY *args) {
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
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
        HRESULT aggregate=S_OK;
        for (SIZE_T i=0;i<args->Resources;++i) {
            if (slot[i]==SIZE_MAX) continue;
            switch (status[slot[i]]) {
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINGPUMEMORY: result[i]=DXGI_DDI_RESIDENCY_FULLY_RESIDENT; break;
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINSHAREDMEMORY:
                result[i]=DXGI_DDI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY;
                if (aggregate==S_OK) aggregate=S_RESIDENT_IN_SHARED_MEMORY; break;
            case D3DDDI_RESIDENCYSTATUS_NOTRESIDENT:
                result[i]=DXGI_DDI_RESIDENCY_EVICTED_TO_DISK; aggregate=S_NOT_RESIDENT; break;
            default: return E_FAIL;
            }
        }
        for (SIZE_T i=0;i<args->Resources;++i) args->pStatus[i]=result[i];
        return aggregate;
    });
}
HRESULT APIENTRY resolve(DXGI_DDI_ARG_RESOLVESHAREDRESOURCE *args) {
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
        D3DKMT_HANDLE handle=0; HRESULT hr=allocation(owner,args->hResource,handle);
        if (FAILED(hr)) return hr;
        if (!owner.engine()) return E_FAIL;
        hr=wait_present_idle(owner.bridge());
        if (FAILED(hr)) return hr;
        // Single GPU, one shared image: no merge or CPU copy. The engine flushes
        // pending resource work and waits before ownership is handed away.
        return owner.engine()->WaitForResourceIdle(reinterpret_cast<DdiResource *>(args->hResource)->object);
    });
}
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
