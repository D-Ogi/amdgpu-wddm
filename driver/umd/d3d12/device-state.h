// SPDX-License-Identifier: MIT
#pragma once
#include "queue-registry.h"
#include "memory-registry.h"
#include <atomic>
#include "adapter-contract.h"
#include "ddi-trace.h"
#include "../app-settings/app-settings.h"
namespace native12 {
struct AdapterCapsOwner;
class DeviceEngine;
struct RecordingBinding;
struct Adapter {
    D3D12DDI_HRTADAPTER runtime;
    D3DDDI_ADAPTERCALLBACKS callbacks;
    std::atomic<unsigned> devices{0};
    std::atomic<unsigned> retained_engines{0};
    AdapterContract contract{};
    SRWLOCK caps_lock=SRWLOCK_INIT;
    AdapterCapsOwner* engine_caps{};
    HRESULT caps_status{E_PENDING};
    bool caps_attempted{};
    // BC250_HOST_POLICY_* of every hosted instance of this adapter. Written once, under caps_lock, before
    // the capability query; a device exists only after that query succeeded.
    UINT32 instance_policy{};
    SRWLOCK tables_lock=SRWLOCK_INIT;
    D3D12DDI_HRTTABLE list_tables[2]{};
};
struct Device {
    Adapter* adapter{};
    D3D12DDI_HRTDEVICE runtime{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 callbacks{};
    D3DDDI_DEVICECALLBACKS kernel_callbacks{};
    QueueRegistry queues;
    MemoryRegistry memory;
    DeviceEngine* engine{};
    // ddi_trace_mode() of this device, read once when its engine is created (device-engine.cpp).
    int trace_mode{};
    // The recording binding of this device's command lists (RecordingScope, device-engine.h), or null for the
    // full entry. Fixed while the engine is open: set after it opened, cleared before it closes.
    const RecordingBinding* recording{};
    std::atomic<bool> lost{false};
    // The queue domain: one queue operation of this device at a time (QueueDomainScope).
    SRWLOCK queue_domain=SRWLOCK_INIT;
    // The per-application FrameRateLimit of this device's presents (docs/design/per-app-graphics-settings.md),
    // taken before the queue domain.
    amdgpu_wddm::app_settings::FrameLimiter frame_limiter;
    // The per-application MaxFrameLatency of this device's presents lives with the engine, not here
    // (device-engine.h, engine_frame_gate): its ring of progress snapshots is half a kilobyte, and the runtime
    // allocates this structure for every device it creates.
    // Every failing DDI reports, also after the loss: pfnSetErrorCb fails the runtime's current API call
    // on the calling thread, so a call left unreported would return S_OK to the application. DDI threads
    // run at once; the callback is the runtime's per-call error state.
    // The call site comes from the caller's own __builtin_LINE()/__builtin_FILE(), as
    // HostedDispatch::remove_device takes its site, so the first removal of a process names the
    // decision that made it without a switch and without a debugger (ddi_first_removal).
    void remove(int line=__builtin_LINE(),const char* file=__builtin_FILE()) noexcept {
        lost.store(true);
        ddi_first_removal("device-remove",file,line);
        ddi_failure_note("device-remove",D3DDDIERR_DEVICEREMOVED);
        if(callbacks.pfnSetErrorCb) callbacks.pfnSetErrorCb(runtime,D3DDDIERR_DEVICEREMOVED);
    }
};
// Serializes the queue operations of one device: queue creation and destruction, ExecuteCommandLists,
// UpdateTileMappings, CopyTileMappings and Present. The runtime serializes each of them per queue
// (DirectX-Specs CPUEfficiency.md, "Command Queue Operations"), but not across the queues of a device,
// and the engine's queue work is not independent across queues: an ExecuteCommandLists on a compute or
// copy queue submits the pending initializations on a direct queue (engine-ddi queue.cpp,
// flush_initializations), and the shell's queue registry refuses that queue's binding while another
// thread executes on it. Outermost: taken at a queue DDI's entry only, never inside a callback and never
// under another lock of this module. The same thread may enter again (a nested entry holds nothing).
class QueueDomainScope final {
    inline static thread_local const Device* held_{};
    Device* device_{};
    const Device* previous_{};
public:
    explicit QueueDomainScope(Device* device) noexcept {
        if(!device || held_==device)return;
        AcquireSRWLockExclusive(&device->queue_domain);
        device_=device;previous_=held_;held_=device;
    }
    ~QueueDomainScope() {
        if(!device_)return;
        held_=previous_;ReleaseSRWLockExclusive(&device_->queue_domain);
    }
    QueueDomainScope(const QueueDomainScope&)=delete;
    QueueDomainScope& operator=(const QueueDomainScope&)=delete;
};
}
