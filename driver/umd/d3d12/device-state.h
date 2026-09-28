// SPDX-License-Identifier: MIT
#pragma once
#include "queue-registry.h"
#include <atomic>
#include "adapter-contract.h"
namespace native12 {
struct Adapter { D3D12DDI_HRTADAPTER runtime; D3DDDI_ADAPTERCALLBACKS callbacks; std::atomic<unsigned> devices{0}; AdapterContract contract{}; };
struct Device {
    Adapter* adapter{};
    D3D12DDI_HRTDEVICE runtime{};
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 callbacks{};
    QueueRegistry queues;
    std::atomic<bool> lost{false};
    void remove() noexcept {
        lost.store(true);
        if(callbacks.pfnSetErrorCb) callbacks.pfnSetErrorCb(runtime,D3DDDIERR_DEVICEREMOVED);
    }
};
}
