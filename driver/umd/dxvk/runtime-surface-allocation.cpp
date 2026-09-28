// SPDX-License-Identifier: MIT
#include "runtime-surface-allocation.h"
namespace bc250::umd {
// E26R v2 resource-group ABI, consumed by KMD wddm.c resource-private parser.
struct ResourcePrivate { UINT magic,version,shared,access; };
static_assert(sizeof(ResourcePrivate)==16);
static_assert(sizeof(BC250_WDDM_ALLOCATION_PRIVATE)==32);
HRESULT allocate_runtime_surface(RuntimeDevice &device,const RuntimeSurfaceRequest &request,RuntimeSurfaceAllocation &out) {
    if (out.allocation || out.kernel_resource || out.runtime_resource) return E_UNEXPECTED;
    if (!device.domain.entered() || !device.hDevice || !device.KTCallbacks.pfnAllocateCb || !device.KTCallbacks.pfnDeallocate2Cb ||
        !WddmSurfaceGeometry(&request.surface,0,4) || (request.primary && request.cpu_read)) return E_INVALIDARG;
    switch (request.surface.Format) {
    case D3DDDIFMT_A8R8G8B8: case D3DDDIFMT_X8R8G8B8: case D3DDDIFMT_A8B8G8R8: break;
    default: return E_NOTIMPL;
    }
    auto surface=request.surface;
    ResourcePrivate group{0x52363245u,2,request.shared ? 1u : 0u,
        (request.primary ? 1u : 0u) | (request.cpu_read ? 2u : 0u)};
    D3DDDI_ALLOCATIONINFO2 info{};
    info.pPrivateDriverData=&surface; info.PrivateDriverDataSize=sizeof(surface);
    info.Flags.Primary=request.primary; info.VidPnSourceId=request.vidpn_source;
    D3DDDICB_ALLOCATE allocate{};
    allocate.hResource=request.runtime_resource;
    allocate.pPrivateDriverData=&group; allocate.PrivateDriverDataSize=sizeof(group);
    allocate.NumAllocations=1; allocate.pAllocationInfo2=&info;
    HRESULT hr=device.KTCallbacks.pfnAllocateCb(device.hDevice,&allocate);
    if (FAILED(hr)) return hr;
    out={request.runtime_resource,info.hAllocation,allocate.hKMResource};
    // Preserve any successful callback output for explicit cleanup, including
    // malformed success. Never claim an allocation is ready without its handle.
    return out.allocation ? S_OK : E_FAIL;
}
HRESULT deallocate_runtime_surface(RuntimeDevice &device,RuntimeSurfaceAllocation &surface) {
    if (!device.domain.entered()) return E_INVALIDARG;
    if (!surface.allocation && !surface.kernel_resource && !surface.runtime_resource) return S_OK;
    if (!device.KTCallbacks.pfnDeallocate2Cb) return E_NOTIMPL;
    D3DDDICB_DEALLOCATE2 free{};
    if (surface.runtime_resource) free.hResource=surface.runtime_resource;
    else {
        if (!surface.allocation) return E_UNEXPECTED;
        free.NumAllocations=1; free.HandleList=&surface.allocation;
    }
    HRESULT hr=device.KTCallbacks.pfnDeallocate2Cb(device.hDevice,&free);
    if (SUCCEEDED(hr)) surface={};
    return hr;
}
}
