// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "hosted-instance.h"
#include "engine-session.h"
#include "engine-error.h"
#include "runtime-surface.h"
#include "present-shadow.h"
#include "vblank-pacer.h"
#include "../app-settings/app-settings.h"
#include "scanout-primary.h"
#include <memory>
#include <vector>
namespace bc250::umd {
// Heap-owned by the DDI device handle. Keep this entire owner and both DLLs
// alive if close fails. The runtime's private handle must never contain a
// by-value copy of this object: callback userdata and descriptors point into it.
class DeviceOwner final {
public:
    DeviceOwner() : instance_(runtime_.domain), session_(runtime_.domain) { bridge_.device=&runtime_; }
    DeviceOwner(const DeviceOwner &) = delete;
    DeviceOwner &operator=(const DeviceOwner &) = delete;
    RuntimeDevice &runtime() { return runtime_; }
    HostBridge &bridge() { return bridge_; }
    ID3D11Device5 *device() const { return device_; } // borrowed
    ID3D11DeviceContext4 *context() const { return context_; } // borrowed
    IBc250DxvkDevice *engine() const { return session_.engine(); } // borrowed
    IBc250DxvkDevice4 *engine4() const { return engine4_; } // borrowed
    // policy_flags: the adapter's resolved BC250_HOST_POLICY_* for the hosted instance.
    HRESULT initialize(const D3D10DDIARG_CREATEDEVICE &args, UINT64 luid,
        PFN_vkGetInstanceProcAddr get, const BC250_DXVK_ENGINE_FUNCS &funcs,
        D3D_FEATURE_LEVEL level, const BC250_DXVK_SHELL_SERVICES &services, UINT32 policy_flags=0);
    HRESULT close();
    HRESULT take_deferred_error(EngineErrorPolicy policy=EngineErrorPolicy::device_removed_only);
    // On any result, a non-null out belongs to this owner and needs close_surface.
    // S_FALSE means paging/import are pending; only finish_surface S_OK is publishable.
    HRESULT begin_surface(const RuntimeSurfaceRequest &,const D3D11_TEXTURE2D_DESC1 &,RuntimeSurface *&out);
    HRESULT adopt_surface(RuntimeSurfaceAllocation &,const BC250_WDDM_ALLOCATION_PRIVATE &,
        const D3D11_TEXTURE2D_DESC1 &,RuntimeSurface *&out);
    HRESULT finish_surface(RuntimeSurface &);
    HRESULT wait_surface(RuntimeSurface &);
    HRESULT close_surface(RuntimeSurface &);
    // Final callback opportunity before Create/Open failure or Destroy returns.
    // Failed cleanup is quarantined; its soon-invalid runtime handle is never reused.
    HRESULT release_surface_handle(RuntimeSurface &);
    size_t surface_count() const { return surfaces_.size(); }
    // BD-065: the B8G8R8A8 shadows of windowed Blt presents. Their surfaces are owned by surfaces_.
    PresentShadows &present_shadows() { return present_shadows_; }
    // The per-application FrameRateLimit of this device's presents (docs/design/per-app-graphics-settings.md).
    amdgpu_wddm::app_settings::FrameLimiter &frame_limiter() { return frame_limiter_; }
    // BD-099: the per-application VSync of this device's presents as vertical-blank waits.
    VBlankPacer &vblank_pacer() { return vblank_pacer_; }
    // BD-099: true where the runtime copies the whole DXGIDDICB_PRESENT, with SyncIntervalOverrideValid and
    // SyncIntervalOverride in it. That is the WDDM 2.2 interface at build 5 and later
    // (IS_DXGI1_6_1_BASE_FUNCTIONS). Below it the runtime copies a shorter structure, the override field is
    // not in it, and VSync needs the vertical-blank waits of the shell instead.
    bool full_present_callback() const { return full_present_callback_; }
    // M15.14: the adapter's scan-out source, copied at CreateDevice (scanout-primary.h).
    const ScanoutSource &scanout() const { return scanout_; }
    void set_scanout(const ScanoutSource &source) { scanout_=source; }

    // Acquire loader references by code address, never by a searched DLL name.
    HRESULT retain_code_modules(const void *engineEntry,const void *icdEntry);
    unsigned retained_module_count() const {
        unsigned count=0; for (auto module:modules_) if (module) ++count; return count;
    }
    bool has_live_objects() const {
        return !surfaces_.empty() || surface_queue_.queue || surface_queue_.sync || surface_queue_.cpu || runtime_.present_context || bridge_.present_sync || instance_.info().Instance ||
            session_.device() || session_.must_retain_owner() || engine4_ || device_ || context_ || retained_module_count();
    }
private:
    HRESULT prepare_surface_import();
    bool owns_surface(const RuntimeSurface &) const;
    SurfacePagingQueue surface_queue_{};
    RuntimeImageDispatch surface_vk_{};
    VkPhysicalDeviceMemoryProperties surface_memory_{};
    std::vector<std::unique_ptr<RuntimeSurface>> surfaces_;
    PresentShadows present_shadows_{};
    amdgpu_wddm::app_settings::FrameLimiter frame_limiter_;
    VBlankPacer vblank_pacer_;
    ScanoutSource scanout_{};
    RuntimeDevice runtime_;
    HostBridge bridge_{};
    HostedInstance instance_;
    EngineSession session_;
    IBc250DxvkDevice4 *engine4_=nullptr;
    ID3D11Device5 *device_=nullptr;
    ID3D11DeviceContext4 *context_=nullptr;
    HMODULE modules_[3]{};
    EngineErrorState errors_;
    bool initialized_=false,closing_=false,full_present_callback_=false;
};
}
