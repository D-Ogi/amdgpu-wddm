// SPDX-License-Identifier: MIT
#pragma once
#include "queue-engine.h"
#include "../dxvk/runtime-domain.h"
#include <bc250_host_bootstrap.h>

namespace native12 {
// Application-queue adapter for HostedDispatchHooks::queue. Internal null-cookie
// queues belong to HostedDispatch and never enter this alias namespace.
class HostedQueue final {
    struct Alias {
        void* cookie{};
        uint32_t token{};
        bool busy{};
        HANDLE context{};
        D3D12DDI_HRTCOMMANDQUEUE runtime_queue{};
        uint32_t progress_sync{};
        UINT64 progress_value{};
    };
    struct Call;
    bc250::umd::RuntimeDomain& domain_;
    QueueEngineRegistry& registry_;
    Device& device_;
    HANDLE runtime_device_{};
    PFND3DDDI_SUBMITCOMMANDCB submit_{};
    PFND3DDDI_WAITFORSYNCHRONIZATIONOBJECTFROMGPUCB wait_{};
    PFND3DDDI_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2CB signal_{};
    PFND3DDDI_UPDATEGPUVIRTUALADDRESSCB update_{};
    SRWLOCK lock_=SRWLOCK_INIT;
    Alias aliases_[64]{};
    uint32_t next_token_{};
    bool active_{true};
    Alias* find(uint32_t token) noexcept; // lock held
    HRESULT invoke(uint32_t op,void* argument) noexcept;
    static HRESULT APIENTRY pinned(void* user,const QueueBindingView* view);
public:
    // Snapshot actual device KT callback pointers. KT submission/synchronization
    // takes hRTDevice.handle, never HRTCOMMANDQUEUE.handle. The full context is
    // resolved under a QueueEngineRegistry pin for every call.
    HostedQueue(bc250::umd::RuntimeDomain& domain,QueueEngineRegistry& registry,Device& device) noexcept;
    HostedQueue(const HostedQueue&)=delete;
    HostedQueue& operator=(const HostedQueue&)=delete;
    static HRESULT dispatch(void* user,uint32_t op,void* argument) noexcept;
    // Terminal metadata release only after all engine calls/workers stopped.
    // No physical context destruction or GPU-retirement assertion is performed.
    unsigned discard_metadata() noexcept;
};
}
