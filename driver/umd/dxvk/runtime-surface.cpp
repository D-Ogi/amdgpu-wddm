// SPDX-License-Identifier: MIT
#include "runtime-surface.h"
#include "runtime-surface-format.h"
#include "diagnostics.h"
#include <cstring>
namespace bc250::umd {
namespace {
HRESULT planar_record(const RuntimeImageLayout &layout,BC250_WDDM_NV12_PRIVATE &out) {
    if (layout.planes!=2 || layout.format!=VK_FORMAT_G8_B8R8_2PLANE_420_UNORM ||
        layout.memory.alignment>UINT32_MAX || layout.plane[0].rowPitch>UINT32_MAX ||
        layout.plane[1].rowPitch>UINT32_MAX) return E_INVALIDARG;
    BC250_WDDM_NV12_PRIVATE n{};
    n.Surface={BC250_WDDM_ALLOCATION_PRIVATE_MAGIC,BC250_WDDM_NV12_VERSION,layout.width,layout.height,
        ULONG(layout.plane[0].rowPitch),BC250_NV12_D3DDDI_FORMAT,layout.memory.size};
    n.ChromaPitch=ULONG(layout.plane[1].rowPitch); n.Layout=BC250_WDDM_NV12_LINEAR;
    n.SurfaceOffset=layout.plane[0].offset; n.ChromaOffset=layout.plane[1].offset; n.Alignment=ULONG(layout.memory.alignment);
    if (!Bc250Nv12PrivateValid(&n,sizeof(n))) return E_INVALIDARG;
    out=n; return S_OK;
}
bool preparation_valid(const RuntimeSurfacePreparation *p) { return p && p->device && p->vk && p->engine; }
}
HRESULT begin_runtime_surface(RuntimeDevice &runtime,const SurfacePagingQueue &queue,
    const RuntimeSurfaceRequest &request,const D3D11_TEXTURE2D_DESC1 &desc,RuntimeSurface &out,
    const RuntimeSurfacePreparation *prepare) {
    if (out.phase!=SurfacePhase::empty || out.owner) return E_UNEXPECTED;
    const bool planar=desc.Format==DXGI_FORMAT_NV12;
    if (!runtime.domain.entered() || !queue.queue || !queue.sync || !queue.cpu ||
        desc.Width!=request.surface.Width || desc.Height!=request.surface.Height ||
        desc.MipLevels!=1 || desc.ArraySize!=1 || desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality ||
        !(planar ? request.surface.Format==BC250_NV12_D3DDDI_FORMAT && preparation_valid(prepare) :
          runtime_surface_format(request.surface.Format,desc.Format)!=nullptr)) return E_INVALIDARG;
    out.owner=&runtime; out.queue=queue; out.desc=desc;
    out.primary=request.primary; out.vidpn_source=request.vidpn_source;
    out.scanout=(request.texture.Access&BC250_SURFACE_RESOURCE_SCANOUT)!=0;
    out.phase=SurfacePhase::failed;
    RuntimeSurfaceRequest actual=request;
    HRESULT hr=S_OK;
    if (planar) {
        hr=prepare_runtime_nv12(runtime,prepare->device,*prepare->vk,*prepare->engine,desc,out.texture);
        if (FAILED(hr)) return hr;
        hr=planar_record(out.texture.preparedLayout,actual.nv12);
        if (FAILED(hr)) return hr;
        actual.surface=actual.nv12.Surface; out.nv12=actual.nv12;
    }
    out.pitch=actual.surface.Pitch; out.bytes=actual.surface.Size;
    hr=allocate_runtime_surface(runtime,actual,out.allocation);
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
    const D3D11_TEXTURE2D_DESC1 &desc,RuntimeSurface &out,const BC250_WDDM_NV12_PRIVATE *nv12,
    const RuntimeSurfacePreparation *prepare) {
    if (out.phase!=SurfacePhase::empty || out.owner) return E_UNEXPECTED;
    const auto *row=runtime_surface_format(metadata.Format,desc.Format);
    const bool planar=desc.Format==DXGI_FORMAT_NV12;
    if (!runtime.domain.entered() || !runtime.hDevice || !queue.queue || !queue.sync || !queue.cpu ||
        !allocation.allocation || !allocation.runtime_resource ||
        !(planar ? nv12 && Bc250Nv12PrivateValid(nv12,sizeof(*nv12)) && preparation_valid(prepare) &&
                    !std::memcmp(&nv12->Surface,&metadata,sizeof(metadata)) :
                  row && runtime_surface_geometry(metadata,row->bytes_per_pixel)) ||
        desc.Width!=metadata.Width || desc.Height!=metadata.Height || desc.MipLevels!=1 || desc.ArraySize!=1 ||
        desc.SampleDesc.Count!=1 || desc.SampleDesc.Quality)
        return E_INVALIDARG;
    if (planar) {
        RuntimeTexture prepared{}; BC250_WDDM_NV12_PRIVATE measured{};
        HRESULT hr=prepare_runtime_nv12(runtime,prepare->device,*prepare->vk,*prepare->engine,desc,prepared);
        if (FAILED(hr)) return hr;
        hr=planar_record(prepared.preparedLayout,measured);
        if (FAILED(hr) || std::memcmp(&measured,nv12,sizeof(measured))) {
            destroy_runtime_image(prepare->device,*prepare->vk,prepared.image);
            return E_INVALIDARG;
        }
        out.texture=prepared; out.nv12=*nv12;
    }
    out.owner=&runtime; out.queue=queue; out.desc=desc;
    out.pitch=metadata.Pitch; out.bytes=metadata.Size;
    out.allocation=allocation; allocation={};
    out.phase=SurfacePhase::failed;
    HRESULT hr=map_runtime_surface(runtime,queue,out.allocation.allocation,out.bytes,out.mapping);
    if (FAILED(hr)) return hr;
    out.phase=SurfacePhase::paging; return S_FALSE;
}
HRESULT begin_engine_surface(RuntimeDevice &runtime,const SurfacePagingQueue &queue,HANDLE runtimeResource,
    const EngineResourceDesc &desc,RuntimeSurface &out,const RuntimeSurfacePreparation &prepare,const char **why) {
    const char *ignored=nullptr; if (!why) why=&ignored;
    *why=BC250_SR_WHY_ARGUMENTS;
    if (out.phase!=SurfacePhase::empty || out.owner) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !queue.queue || !queue.sync || !queue.cpu || !runtimeResource ||
        !prepare.device || !prepare.resource) return E_INVALIDARG;
    out.owner=&runtime; out.queue=queue; out.form=SurfaceForm::engine_layout;
    if (!desc.buffer) out.desc=desc.texture;
    out.phase=SurfacePhase::failed;
    HRESULT hr=prepare_engine_resource(runtime,prepare.device,*prepare.resource,desc,out.engine,why);
    if (FAILED(hr)) return hr;
    hr=allocate_engine_surface(runtime,runtimeResource,out.engine.record,out.allocation,out.bytes);
    if (FAILED(hr)) {
        *why="allocate";
        surface_diagnostic("engine-layout-allocate",hr,out.engine.record.VkFormat,out.engine.record.Width,
            out.engine.record.Height,out.engine.record.MemorySize,out.engine.record.MemoryAlignment,out.bytes);
        return hr;
    }
    hr=map_runtime_surface(runtime,queue,out.allocation.allocation,out.bytes,out.mapping);
    if (FAILED(hr)) { *why="map"; return hr; }
    *why=BC250_SR_WHY_OK;
    out.phase=SurfacePhase::paging;
    return S_FALSE;
}
HRESULT adopt_engine_surface(RuntimeDevice &runtime,const SurfacePagingQueue &queue,RuntimeSurfaceAllocation &allocation,
    const BC250_SHARED_RESOURCE &record,const void *blob,UINT blobBytes,RuntimeSurface &out,
    const RuntimeSurfacePreparation &prepare,const char **why) {
    const char *ignored=nullptr; if (!why) why=&ignored;
    *why=BC250_SR_WHY_ARGUMENTS;
    if (out.phase!=SurfacePhase::empty || out.owner) return E_UNEXPECTED;
    if (!runtime.domain.entered() || !runtime.hDevice || !queue.queue || !queue.sync || !queue.cpu ||
        !allocation.allocation || !allocation.runtime_resource || !prepare.device || !prepare.resource) return E_INVALIDARG;
    UINT64 bytes=0;
    HRESULT hr=engine_surface_blob(blob,blobBytes,record,bytes,why);
    if (FAILED(hr)) return hr;
    RuntimeEngineResource matched{};
    hr=match_engine_resource(runtime,prepare.device,*prepare.resource,record,bytes,matched,why);
    if (FAILED(hr)) return hr;
    out.owner=&runtime; out.queue=queue; out.form=SurfaceForm::engine_layout; out.engine=matched;
    if (!matched.desc.buffer) out.desc=matched.desc.texture;
    out.bytes=bytes;
    out.allocation=allocation; allocation={};
    out.phase=SurfacePhase::failed;
    hr=map_runtime_surface(runtime,queue,out.allocation.allocation,out.bytes,out.mapping);
    if (FAILED(hr)) { *why="map"; return hr; }
    out.phase=SurfacePhase::paging; return S_FALSE;
}
HRESULT finish_runtime_surface(VkDevice device,const RuntimeImageDispatch &vk,
    const TextureImportDispatch &engine,const VkPhysicalDeviceMemoryProperties &properties,RuntimeSurface &surface,
    const EngineResourceDispatch *resource) {
    if (!surface.owner || !surface.owner->domain.entered()) return E_INVALIDARG;
    if (surface.phase==SurfacePhase::ready) return S_OK;
    if (surface.phase!=SurfacePhase::paging || !surface.mapping.resident) return E_UNEXPECTED;
    if (surface.form==SurfaceForm::engine_layout && !resource) return E_INVALIDARG;
    HRESULT hr=surface_paging_status(surface.queue,surface.mapping);
    if (hr==S_FALSE) return hr;
    surface.phase=SurfacePhase::failed;
    if (FAILED(hr)) return hr;
    bc250_host_import imported{};
    imported.sType=BC250_HOST_IMPORT_STYPE; imported.identity=surface.owner->host_identity();
    imported.allocation=surface.allocation.allocation; imported.va=surface.mapping.address;
    imported.size=surface.bytes;
    if (surface.form==SurfaceForm::engine_layout) {
        hr=bind_engine_resource(*surface.owner,device,*resource,properties,imported,surface.engine);
        if (FAILED(hr)) {
            surface_diagnostic("engine-layout-bind",hr,surface.engine.record.VkFormat,surface.engine.record.Width,
                surface.engine.record.Height,surface.engine.record.MemorySize,surface.bytes,surface.engine.record.BindFlags);
            return hr;
        }
        surface.phase=SurfacePhase::ready;
        return S_OK;
    }
    hr=surface.nv12.Surface.Version==BC250_WDDM_NV12_VERSION
        ? bind_runtime_nv12(*surface.owner,device,vk,engine,properties,surface.desc,imported,surface.texture)
        : create_runtime_texture(*surface.owner,device,vk,engine,properties,surface.desc,
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
    const TextureImportDispatch &engine,RuntimeSurface &surface,const EngineResourceDispatch *resource) {
    if (!bridge.device || !bridge.device->domain.entered()) return E_INVALIDARG;
    if (surface.phase==SurfacePhase::empty) return S_OK;
    if (surface.phase==SurfacePhase::quarantined) return E_UNEXPECTED;
    if (bridge.device!=surface.owner) return E_INVALIDARG;
    if (surface.form==SurfaceForm::engine_layout && !resource) return E_INVALIDARG;
    surface.phase=SurfacePhase::closing;
    HRESULT hr=surface.form==SurfaceForm::engine_layout ? close_engine_resource(bridge,device,*resource,surface.engine) :
        close_runtime_texture(bridge,device,vk,engine,surface.texture);
    if (hr!=S_OK) return hr;
    // Deallocate2 with flags zero owns VA retirement and defers allocation
    // destruction behind queued work. Explicit FreeGpuVirtualAddress here could
    // make the address reusable before pending mapping/submission retires.
    hr=deallocate_runtime_surface(*surface.owner,surface.allocation);
    if (hr!=S_OK) {
        // BD-253: on a lost device whose work ended, the kernel frees the allocation together with the device. No
        // wait here (settle_device_work): DestroyResource calls this on the application's thread.
        if (!device_lost_status(hr) || !settle_device_work(bridge,0)) return hr;
        surface.allocation={};
    }
    surface.mapping={};
    surface.owner=nullptr; surface.queue={}; surface.desc={}; surface.pitch=0; surface.bytes=0;
    surface.nv12={}; surface.texture.preparedInfo={}; surface.texture.preparedLayout={};
    surface.engine={}; surface.form=SurfaceForm::linear;
    surface.phase=SurfacePhase::empty;
    return S_OK;
}
}
