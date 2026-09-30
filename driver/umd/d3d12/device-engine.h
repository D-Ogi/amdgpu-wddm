// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include "../dxvk/runtime-domain.h"
#include "hosted-instance.h"
#include <optional>
namespace engine_ddi {class DeviceContext;}
namespace native12 {
struct Device;
class DeviceEngine;
class QueueEngineRegistry;
class RuntimeHeapImports;
HRESULT create_device_engine(Device&) noexcept;
void destroy_device_engine(Device&) noexcept;
engine_ddi::DeviceContext* engine_context(Device&) noexcept;
QueueEngineRegistry* engine_queues(Device&) noexcept;
RuntimeHeapImports* engine_imports(Device&) noexcept;
bool device_engine_entered(Device&) noexcept;
void report_device_error(Device&,HRESULT) noexcept;

// The authority of one DDI entry: this device's hosted callback tables and instance dispatch are bound
// to the entering thread for exactly this scope. It is not a lock. D3D12 device methods are
// free-threaded (DirectX-Specs CPUEfficiency.md, "Threading Model"), so entries of one device on several
// threads run at once, each with its own binding; every component the scope reaches guards its own
// state, and the queue operations of a device are serialized by QueueDomainScope (device-state.h).
// Nested calls on the same device are allowed; a different owner cannot inherit its authority.
class DeviceEngineScope final {
    inline static thread_local DeviceEngine* current_{};
    DeviceEngine* owner_{};
    DeviceEngine* previous_{};
    std::optional<bc250::umd::RuntimeDomain::Scope> runtime_;
    std::optional<HostedInstanceBootstrap::Scope> hosted_;
public:
    explicit DeviceEngineScope(Device&) noexcept;
    ~DeviceEngineScope() noexcept;
    DeviceEngineScope(const DeviceEngineScope&)=delete;
    DeviceEngineScope& operator=(const DeviceEngineScope&)=delete;
    bool entered() const noexcept {return owner_!=nullptr;}
};
#ifdef AMDGPU_WDDM_D3D12_HOST_TEST
// Host tests only, never in the driver build: an owner of the device without the engine device. It
// creates the hosted instance through the bootstrap's entry, from the given ICD entry, and the heap
// imports over the given Vulkan handles; no queue registry, no engine context. The device's adapter
// supplies the LUID and the instance policy. Close destroys the instance and releases the owner.
HRESULT host_test_open_device_engine(Device&,PFN_vkGetInstanceProcAddr icd,VkPhysicalDevice,VkDevice) noexcept;
bool host_test_close_device_engine(Device&) noexcept;
#endif
}
