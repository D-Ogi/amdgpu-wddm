// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-surface-allocation.h"
#include "runtime-texture.h"
namespace bc250::umd {
enum class SurfacePhase { empty, paging, ready, failed, closing, quarantined };
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
    D3D11_TEXTURE2D_DESC1 desc{};
    UINT pitch=0;
    UINT64 bytes=0;
    SurfacePhase phase=SurfacePhase::empty;
};
// S_FALSE: accepted; finish must be polled before publication. No busy wait.
HRESULT begin_runtime_surface(RuntimeDevice &,const SurfacePagingQueue &,
    const RuntimeSurfaceRequest &,const D3D11_TEXTURE2D_DESC1 &,RuntimeSurface &);
// On validation success, consumes allocation even if mapping later fails.
// Caller retains allocation when validation fails before transfer.
HRESULT adopt_runtime_surface(RuntimeDevice &,const SurfacePagingQueue &,
    RuntimeSurfaceAllocation &,const BC250_WDDM_ALLOCATION_PRIVATE &,
    const D3D11_TEXTURE2D_DESC1 &,RuntimeSurface &);
HRESULT finish_runtime_surface(VkDevice,const RuntimeImageDispatch &,
    const TextureImportDispatch &,const VkPhysicalDeviceMemoryProperties &,RuntimeSurface &);
// Unbind/release views first. Any non-S_OK means retain the surface and device.
HRESULT close_runtime_surface(HostBridge &,VkDevice,const RuntimeImageDispatch &,
    const TextureImportDispatch &,RuntimeSurface &);
}
