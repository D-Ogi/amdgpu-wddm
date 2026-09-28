// SPDX-License-Identifier: MIT
#include "ddi-dxgi-resources.h"
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
HRESULT APIENTRY priority(DXGI_DDI_ARG_SETRESOURCEPRIORITY *args) {
    if (!args) return E_INVALIDARG;
    return entry(args->hDevice,[&](DeviceOwner &owner) {
        D3DKMT_HANDLE handle=0; HRESULT hr=allocation(owner,args->hResource,handle);
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
        if (!runtime.KTCallbacks.pfnQueryResidencyCb) return E_NOTIMPL;
        std::vector<D3DKMT_HANDLE> handles(args->Resources);
        std::vector<D3DDDI_RESIDENCYSTATUS> status(args->Resources);
        std::vector<DXGI_DDI_RESIDENCY> result(args->Resources);
        for (SIZE_T i=0;i<args->Resources;++i) {
            HRESULT hr=allocation(owner,args->pResources[i],handles[i]); if (FAILED(hr)) return hr;
        }
        D3DDDICB_QUERYRESIDENCY request{}; request.NumAllocations=UINT(args->Resources);
        request.HandleList=handles.data(); request.pResidencyStatus=status.data();
        HRESULT hr=runtime.KTCallbacks.pfnQueryResidencyCb(runtime.hDevice,&request);
        if (FAILED(hr)) return hr;
        for (SIZE_T i=0;i<args->Resources;++i) {
            switch (status[i]) {
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINGPUMEMORY: result[i]=DXGI_DDI_RESIDENCY_FULLY_RESIDENT; break;
            case D3DDDI_RESIDENCYSTATUS_RESIDENTINSHAREDMEMORY: result[i]=DXGI_DDI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY; break;
            case D3DDDI_RESIDENCYSTATUS_NOTRESIDENT: result[i]=DXGI_DDI_RESIDENCY_EVICTED_TO_DISK; break;
            default: return E_FAIL;
            }
        }
        for (SIZE_T i=0;i<args->Resources;++i) args->pStatus[i]=result[i];
        return hr;
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
    table.pfnSetResourcePriority=priority;
    table.pfnQueryResourceResidency=residency;
    table.pfnResolveSharedResource=resolve;
}
}
