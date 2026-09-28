// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include "hosted-instance.h"
#include "engine-session.h"
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
    HRESULT initialize(const D3D10DDIARG_CREATEDEVICE &args, UINT64 luid,
        PFN_vkGetInstanceProcAddr get, const BC250_DXVK_ENGINE_FUNCS &funcs,
        D3D_FEATURE_LEVEL level, const BC250_DXVK_SHELL_SERVICES &services);
    HRESULT close();
    // Acquire loader references by code address, never by a searched DLL name.
    HRESULT retain_code_modules(const void *engineEntry,const void *icdEntry);
    unsigned retained_module_count() const {
        unsigned count=0; for (auto module:modules_) if (module) ++count; return count;
    }
    bool has_live_objects() const {
        return runtime_.present_context || bridge_.present_sync || instance_.info().Instance ||
            session_.device() || session_.must_retain_owner() || device_ || context_ || retained_module_count();
    }
private:
    RuntimeDevice runtime_;
    HostBridge bridge_{};
    HostedInstance instance_;
    EngineSession session_;
    ID3D11Device5 *device_=nullptr;
    ID3D11DeviceContext4 *context_=nullptr;
    HMODULE modules_[3]{};
    bool initialized_=false;
};
}
