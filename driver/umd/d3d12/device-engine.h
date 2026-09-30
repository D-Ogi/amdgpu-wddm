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

// Initial integration serializes this device's DDI entry scopes. Nested calls
// on the same device are allowed; a different owner cannot inherit its authority.
// Hosted callback tables and instance dispatch remain live for exactly this scope.
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
}
