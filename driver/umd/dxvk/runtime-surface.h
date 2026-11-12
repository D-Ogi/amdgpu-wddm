// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-surface-allocation.h"
#include "runtime-texture.h"
#include "runtime-engine-resource.h"
namespace bc250::umd {
enum class SurfacePhase { empty, paging, ready, failed, closing, quarantined };
// BD-373 step 3: the wire form of the surface's memory (driver/contract/bc250_shared_resource.h). linear: an LB7A
// allocation that holds a LINEAR 2D image (texture). engine_layout: a BC2A allocation that holds the engine's buffer
// or LINEAR image of any shareable description (engine).
enum class SurfaceForm { linear, engine_layout };
// Explicit close in the owning runtime domain. A failed close retains ownership.
// The device-owned paging queue must outlive every surface borrowing it.
struct RuntimeSurface {
    RuntimeSurface()=default;
    RuntimeSurface(const RuntimeSurface &)=delete;
    RuntimeSurface &operator=(const RuntimeSurface &)=delete;
    RuntimeDevice *owner=nullptr;
    SurfacePagingQueue queue{};
    RuntimeSurfaceAllocation allocation{};
    SurfaceGpuMapping mapping{};
    RuntimeTexture texture{};
    SurfaceForm form=SurfaceForm::linear;
    RuntimeEngineResource engine{};
    BC250_WDDM_NV12_PRIVATE nv12{};
    D3D11_TEXTURE2D_DESC1 desc{};
    // What the runtime said about this surface when it was created: a primary is the buffer of a
    // flip-model or fullscreen swap chain, and vidpn_source is the video present source its primary
    // descriptor named. An adopted (opened) surface is nobody's primary here: the record travels with
    // the creator, not with the handle. CheckDirectFlipSupport reads both (M15.14).
    bool primary=false;
    // Whether this surface's own resource record carried BC250_SURFACE_RESOURCE_SCANOUT, which is what
    // makes the kernel driver place it in the local segment and admit a flip of it at all. A primary
    // without the bit is aperture-resident: the display core cannot read it, and telling the runtime a
    // flip of it is supported would skip a copy the OS can no longer put back (M15.14).
    bool scanout=false;
    UINT vidpn_source=0;
    UINT pitch=0;
    UINT64 bytes=0;
    SurfacePhase phase=SurfacePhase::empty;
    // The engine object of the surface, of either form (borrowed), and whether its last Release found another owner.
    ID3D11Resource *resource() const { return form==SurfaceForm::engine_layout ? engine.object : texture.texture; }
    bool retained() const { return form==SurfaceForm::engine_layout ? engine.retained : texture.retained; }
};
struct RuntimeSurfacePreparation {
    VkDevice device;
    const RuntimeImageDispatch *vk;
    const TextureImportDispatch *engine;
    const EngineResourceDispatch *resource=nullptr; // the ENGINE LAYOUT form only
};
// S_FALSE: accepted; finish must be polled before publication. No busy wait.
HRESULT begin_runtime_surface(RuntimeDevice &,const SurfacePagingQueue &,
    const RuntimeSurfaceRequest &,const D3D11_TEXTURE2D_DESC1 &,RuntimeSurface &,
    const RuntimeSurfacePreparation *prepare=nullptr);
// On validation success, consumes allocation even if mapping later fails.
// Caller retains allocation when validation fails before transfer.
HRESULT adopt_runtime_surface(RuntimeDevice &,const SurfacePagingQueue &,
    RuntimeSurfaceAllocation &,const BC250_WDDM_ALLOCATION_PRIVATE &,
    const D3D11_TEXTURE2D_DESC1 &,RuntimeSurface &,const BC250_WDDM_NV12_PRIVATE *nv12=nullptr,
    const RuntimeSurfacePreparation *prepare=nullptr);
// BD-373 step 3, creator: the engine's buffer or LINEAR image for desc (prepare.resource), one runtime allocation of
// the ENGINE LAYOUT form (allocate_engine_surface) and its mapping. S_FALSE: accepted, finish must be polled. *why
// names a refusal of the description or of the engine (BC250_SR_WHY_* or a reason of runtime-engine-resource).
HRESULT begin_engine_surface(RuntimeDevice &,const SurfacePagingQueue &,HANDLE runtime_resource,
    const EngineResourceDesc &,RuntimeSurface &,const RuntimeSurfacePreparation &,const char **why);
// Opener: the received S26R record and BC2A blob, matched against this device's engine (match_engine_resource), then
// the mapping. On validation success it consumes allocation, as adopt_runtime_surface does.
HRESULT adopt_engine_surface(RuntimeDevice &,const SurfacePagingQueue &,RuntimeSurfaceAllocation &,
    const BC250_SHARED_RESOURCE &,const void *blob,UINT blobBytes,RuntimeSurface &,const RuntimeSurfacePreparation &,
    const char **why);
// resource is the ENGINE LAYOUT dispatch; a surface of that form needs it.
HRESULT finish_runtime_surface(VkDevice,const RuntimeImageDispatch &,
    const TextureImportDispatch &,const VkPhysicalDeviceMemoryProperties &,RuntimeSurface &,
    const EngineResourceDispatch *resource=nullptr);
// Unbind/release views first. Any non-S_OK means retain the surface and device.
HRESULT close_runtime_surface(HostBridge &,VkDevice,const RuntimeImageDispatch &,
    const TextureImportDispatch &,RuntimeSurface &,const EngineResourceDispatch *resource=nullptr);
}
