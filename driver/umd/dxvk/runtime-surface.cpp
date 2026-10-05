// SPDX-License-Identifier: MIT
#include "runtime-surface.h"
#include "runtime-surface-format.h"
#include "diagnostics.h"
namespace bc250::umd {
HRESULT begin_runtime_surface(RuntimeDevice &runtime,const SurfacePagingQueue &queue,
    const RuntimeSurfaceRequest &request,const D3D11_TEXTURE2D_DESC1 &desc,RuntimeSurface &out) {
    if (out.phase!=SurfacePhase::empty || out.owner) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !queue.queue || !queue.sync || !queue.cpu ||
        desc.Width!=request.surface.Width || desc.Height!=request.surface.Height ||
        desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality ||
        !runtime_surface_format(request.surface.Format,desc.Format)) return E_INVALIDARG;
    out.owner=&runtime; out.queue=queue; out.desc=desc;
    out.pitch=request.surface.Pitch; out.bytes=request.surface.Size;
    out.phase=SurfacePhase::failed;
    HRESULT hr=allocate_runtime_surface(runtime,request,out.allocation);
    if (FAILED(hr)) {
        surface_diagnostic("allocate",hr,request.surface.Format,request.surface.Width,request.surface.Height,
            request.surface.Pitch,request.surface.Size,(request.primary ? 1ull : 0ull)<<32|request.vidpn_source);
        return hr;
    }
    hr=map_runtime_surface(runtime,queue,out.allocation.allocation,out.bytes,out.mapping);
    if (FAILED(hr)) {
        surface_diagnostic("map",hr,request.surface.Format,request.surface.Width,request.surface.Height,
            request.surface.Pitch,request.surface.Size,0);
        return hr;
    }
    out.phase=SurfacePhase::paging;
    return S_FALSE; // Even completed paging still needs an image and texture.
}
HRESULT adopt_runtime_surface(RuntimeDevice &runtime,const SurfacePagingQueue &queue,
    RuntimeSurfaceAllocation &allocation,const BC250_WDDM_ALLOCATION_PRIVATE &metadata,
    const D3D11_TEXTURE2D_DESC1 &desc,RuntimeSurface &out) {
    if (out.phase!=SurfacePhase::empty || out.owner) return E_UNEXPECTED;
    const auto *row=runtime_surface_format(metadata.Format,desc.Format);
    if (!runtime.domain.entered() || !runtime.hDevice || !queue.queue || !queue.sync || !queue.cpu ||
        !allocation.allocation || !allocation.runtime_resource || !row || !runtime_surface_geometry(metadata,row->bytes_per_pixel) ||
        desc.Width!=metadata.Width || desc.Height!=metadata.Height || desc.MipLevels!=1 || desc.ArraySize!=1 ||
        desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality)
        return E_INVALIDARG;
    out.owner=&runtime; out.queue=queue; out.desc=desc;
    out.pitch=metadata.Pitch; out.bytes=metadata.Size;
    out.allocation=allocation; allocation={};
    out.phase=SurfacePhase::failed;
    HRESULT hr=map_runtime_surface(runtime,queue,out.allocation.allocation,out.bytes,out.mapping);
    if (FAILED(hr)) return hr;
    out.phase=SurfacePhase::paging; return S_FALSE;
}
HRESULT finish_runtime_surface(VkDevice device,const RuntimeImageDispatch &vk,
    const TextureImportDispatch &engine,const VkPhysicalDeviceMemoryProperties &properties,RuntimeSurface &surface) {
    if (!surface.owner || !surface.owner->domain.entered()) return E_INVALIDARG;
    if (surface.phase==SurfacePhase::ready) return S_OK;
    if (surface.phase!=SurfacePhase::paging || !surface.mapping.resident) return E_UNEXPECTED;
    HRESULT hr=surface_paging_status(surface.queue,surface.mapping);
    if (hr==S_FALSE) return hr;
    surface.phase=SurfacePhase::failed;
    if (FAILED(hr)) return hr;
    bc250_host_import imported{};
    imported.sType=BC250_HOST_IMPORT_STYPE; imported.identity=surface.owner->hDevice;
    imported.allocation=surface.allocation.allocation; imported.va=surface.mapping.address;
    imported.size=surface.bytes;
    hr=create_runtime_texture(*surface.owner,device,vk,engine,properties,surface.desc,
        imported,surface.pitch,surface.texture);
    if (FAILED(hr)) {
        surface_diagnostic("texture",hr,UINT(surface.desc.Format),surface.desc.Width,surface.desc.Height,
            surface.pitch,surface.bytes,surface.desc.BindFlags);
        return hr;
    }
    surface.phase=SurfacePhase::ready;
    return S_OK;
}
HRESULT close_runtime_surface(HostBridge &bridge,VkDevice device,const RuntimeImageDispatch &vk,
    const TextureImportDispatch &engine,RuntimeSurface &surface) {
    if (!bridge.device || !bridge.device->domain.entered()) return E_INVALIDARG;
    if (surface.phase==SurfacePhase::empty) return S_OK;
    if (surface.phase==SurfacePhase::quarantined) return E_UNEXPECTED;
    if (bridge.device!=surface.owner) return E_INVALIDARG;
    surface.phase=SurfacePhase::closing;
    HRESULT hr=close_runtime_texture(bridge,device,vk,engine,surface.texture);
    if (hr!=S_OK) return hr;
    // Deallocate2 with flags zero owns VA retirement and defers allocation
    // destruction behind queued work. Explicit FreeGpuVirtualAddress here could
    // make the address reusable before pending mapping/submission retires.
    hr=deallocate_runtime_surface(*surface.owner,surface.allocation);
    if (hr!=S_OK) return hr;
    surface.mapping={};
    surface.owner=nullptr; surface.queue={}; surface.desc={}; surface.pitch=0; surface.bytes=0;
    surface.phase=SurfacePhase::empty;
    return S_OK;
}
}
