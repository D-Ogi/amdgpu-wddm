// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include "../dxvk/runtime-domain.h"
#include "hosted-instance.h"
#include <atomic>
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
    friend class RecordingScope;
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
// The authority of a command-list recording entry (lever L2, experiment "recording-bind", off by default):
// what DeviceEngineScope binds, bound inline from a binding the device fixed once. DeviceEngineScope's
// constructor and destructor are calls out of the thunk with two optionals, and the full entry adds two
// trace hooks that read the trace mode on every call; a game makes thousands of recording calls per frame
// (trial 217: 0.98 ms/frame of the main thread in that bookkeeping). The binding names the engine owner, its
// runtime domain, its hosted bootstrap and its active flag, all fixed while the engine is open: the device
// publishes it in Device::recording after the engine opened, only when the experiment is set and the
// device's trace mode is 0, and clears it before the engine closes (device-engine.cpp). A list resolves to
// its device (EntryOwner, engine_ddi::command_list_shell), so every list of the device shares the binding;
// a per-list copy would hold the same four pointers. The scope binds the same three thread-local states as
// DeviceEngineScope, so callbacks, hosted dispatch and device_engine_entered see an entered device exactly as
// they do under DeviceEngineScope. admit() refuses a thread already inside a DeviceEngineScope or a
// RecordingScope of any device: a nested entry takes the full path, which admits the same device and refuses
// another one, as before.
struct RecordingBinding {
    DeviceEngine* owner;
    const bc250::umd::RuntimeDomain* domain;
    HostedInstanceBootstrap* bootstrap;
    const std::atomic<bool>* active;
};
class RecordingScope final {
    bc250::umd::RuntimeDomain::Scope runtime_;
    HostedInstanceBootstrap::Scope hosted_;
    DeviceEngine* previous_;
public:
    static const RecordingBinding* admit(const RecordingBinding* binding) noexcept {
        return binding && !DeviceEngineScope::current_ && binding->active->load(std::memory_order_acquire)?
            binding:nullptr;
    }
    explicit RecordingScope(const RecordingBinding& binding) noexcept
      :runtime_(*binding.domain),hosted_(*binding.bootstrap),previous_(DeviceEngineScope::current_) {
        if(hosted_.entered())DeviceEngineScope::current_=binding.owner;
    }
    ~RecordingScope() noexcept {if(hosted_.entered())DeviceEngineScope::current_=previous_;}
    RecordingScope(const RecordingScope&)=delete;
    RecordingScope& operator=(const RecordingScope&)=delete;
    bool entered() const noexcept {return hosted_.entered();}
};
#ifdef AMDGPU_WDDM_D3D12_HOST_TEST
// Host tests only, never in the driver build: an owner of the device without the engine device. It
// creates the hosted instance through the bootstrap's entry, from the given ICD entry, and the heap
// imports over the given Vulkan handles; no queue registry, no engine context. The device's adapter
// supplies the LUID and the instance policy. Close destroys the instance and releases the owner.
HRESULT host_test_open_device_engine(Device&,PFN_vkGetInstanceProcAddr icd,VkPhysicalDevice,VkDevice) noexcept;
bool host_test_close_device_engine(Device&) noexcept;
// Publishes (or, with false, clears) Device::recording as create_device_engine does when the experiment is
// set; the device's trace mode still decides. Returns whether a binding is published.
bool host_test_bind_recording(Device&,bool) noexcept;
#endif
}
