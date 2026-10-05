// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-domain.h"
#include "ddi/bc250_dxvk_engine.h"
namespace bc250::umd {
// Explicit lifetime: close inside a DDI scope before destroying the hosted
// instance, bridge, or engine DLL. On nonzero final Release retain this object
// and the entire hosted owner; freeing any borrowed callback state is unsafe.
class EngineSession final {
public:
    explicit EngineSession(RuntimeDomain &domain) : domain_(domain) {}
    EngineSession(const EngineSession &) = delete;
    EngineSession &operator=(const EngineSession &) = delete;
    HRESULT open(const BC250_DXVK_ENGINE_FUNCS &funcs,
        const BC250_DXVK_VULKAN_INSTANCE &instance, D3D_FEATURE_LEVEL level,
        const BC250_DXVK_SHELL_SERVICES &services);
    HRESULT close();
    IBc250DxvkDevice *engine() const { return engine_; } // borrowed
    VkDevice device() const { return device_.Device; }
    bool must_retain_owner() const { return leaked_; }
private:
    RuntimeDomain &domain_;
    BC250_DXVK_ENGINE_FUNCS funcs_{};
    BC250_DXVK_VULKAN_INSTANCE instance_{};
    BC250_DXVK_DEVICE_REQUIREMENTS requirements_{};
    BC250_DXVK_VULKAN_DEVICE device_{};
    BC250_DXVK_SHELL_SERVICES services_{};
    IBc250DxvkDevice *engine_=nullptr;
    PFN_vkDestroyDevice destroy_=nullptr;
    bool have_requirements_=false, leaked_=false;
};
}
