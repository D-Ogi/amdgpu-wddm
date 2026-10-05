// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-domain.h"
#include "host-bootstrap.h"
#include "ddi/bc250_dxvk_engine.h"
namespace bc250::umd {
// Explicit lifetime, as EngineSession: close only after its VkDevice is gone.
// The ICD module, host userdata and extension strings remain caller-owned.
class HostedInstance final {
public:
    explicit HostedInstance(RuntimeDomain &domain) : domain_(domain) {}
    HostedInstance(const HostedInstance &) = delete;
    HostedInstance &operator=(const HostedInstance &) = delete;
    // policy_flags (BC250_HOST_POLICY_*) is the adapter's resolved instance policy, chained behind host.
    HRESULT open(PFN_vkGetInstanceProcAddr get, const bc250_host &host, UINT32 policy_flags=0,
        UINT32 api_version=VK_API_VERSION_1_3, UINT32 extension_count=0,
        const char *const *extensions=nullptr);
    HRESULT close();
    const BC250_DXVK_VULKAN_INSTANCE &info() const { return info_; }
private:
    RuntimeDomain &domain_;
    bc250_host host_{};
    bc250_host_policy policy_{};
    BC250_DXVK_VULKAN_INSTANCE info_{};
    PFN_vkDestroyInstance destroy_=nullptr;
};
}
