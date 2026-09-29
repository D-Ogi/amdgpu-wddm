// SPDX-License-Identifier: MIT
#pragma once
#include "queue-registry.h"
#include "memory-registry.h"
#include <atomic>
#include "adapter-contract.h"
namespace native12 {
struct AdapterCapsOwner;
class DeviceEngine;
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
    std::atomic<bool> lost{false};
    void remove() noexcept {
        lost.store(true);
        if(callbacks.pfnSetErrorCb) callbacks.pfnSetErrorCb(runtime,D3DDDIERR_DEVICEREMOVED);
    }
};
}
