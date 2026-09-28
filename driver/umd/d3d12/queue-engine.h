// SPDX-License-Identifier: MIT
#pragma once
#include "device-state.h"
#include "engine-ddi/engine-ddi.h"

namespace native12 {
enum class QueueEngineState { Creating, Live, Executing, Destroying, Retired };
struct QueueBindingView {
    D3D12DDI_HRTCOMMANDQUEUE runtime_queue;
    HANDLE context; // Full-width native callback handle, never a KMT bridge token.
    // Borrowed context: hosted op6 creates only its32-bit alias token; op7 retires
    // that alias, never this physical context. Registry closes it after the
    // engine queue is released and an explicit close result proves GPU retirement.
    QueueEngineState state;
};
using QueueBindingCallback = HRESULT (APIENTRY*)(void*, const QueueBindingView*);

// Exactly the production engine-ddi boundary; explicit dependencies let host
// tests prove ordering with an engine substitute without loading a GPU driver.
struct QueueEngineOps {
    decltype(&engine_ddi::create_engine_queue) create{};
    decltype(&engine_ddi::execute_command_lists) execute{};
    // Required retirement proof, unlike engine-ddi's old VOID destroy API.
    // Only S_OK with *queue == nullptr means GPU retirement and engine ownership
    // release are both proven. A failure may retain *queue; the registry keeps
    // that engine ownership and its native context, without an expired callback.
    HRESULT (*close)(engine_ddi::EngineQueue**) noexcept{};
    // Required integration hook: query the actual engine ID3D12Device removed
    // reason, not merely Device::lost. The engine's bounded retirement failure
    // can set its own removed reason without invoking a shell callback.
    HRESULT (*check_health)(void*) noexcept{};
    void* health_cookie{};
    // Native create/execute only. Caller must supply close and check_health/cookie.
    static QueueEngineOps native() noexcept;
};
struct QueueEngineSlot { void* cookie{}; UINT64 serial{}; };
struct QueueEngineOwner;

// One per live engine DeviceContext. Device and engine context outlive this
// registry. No worker threads: all engine/runtime callbacks run synchronously
// in the caller's valid DDI scope. Runtime serializes each queue's lifetime;
// different queues may run concurrently. Explicit operations own all cleanup.
class QueueEngineRegistry final {
    Device& device_;
    engine_ddi::DeviceContext* engine_;
    QueueEngineOps ops_;
    SRWLOCK lock_ = SRWLOCK_INIT;
    QueueEngineOwner* head_{};
    UINT64 serial_{};
    QueueEngineOwner* find(void* cookie, UINT64 serial) noexcept;
    void erase(QueueEngineOwner*) noexcept;
    HRESULT release_context(QueueEngineOwner*) noexcept;
public:
    QueueEngineRegistry(Device& device, engine_ddi::DeviceContext* engine,
        QueueEngineOps ops = QueueEngineOps::native()) noexcept;
    QueueEngineRegistry(const QueueEngineRegistry&) = delete;
    QueueEngineRegistry& operator=(const QueueEngineRegistry&) = delete;
    HRESULT create(const D3D12DDIARG_CREATECOMMANDQUEUE_0050& args,
        D3D12DDI_HRTCOMMANDQUEUE runtime, QueueEngineSlot& slot) noexcept;
    HRESULT execute(const QueueEngineSlot& slot, UINT count,
        const D3D12DDI_HCOMMANDLIST* lists) noexcept;
    HRESULT destroy(QueueEngineSlot& slot) noexcept;

    // Validates an opaque engine cookie by membership before any dereference.
    // Pins the owner across the callback, which runs outside the registry lock.
    // A concurrent/reentrant destroy returns E_PENDING without consuming slot.
    // The view is borrowed for this synchronous call only; never retain it or
    // use the runtime handle from a worker. Internal engine cookie NULL is not
    // an application queue and is deliberately refused here.
    HRESULT with_binding(void* cookie, Device& expected_device,
        QueueBindingCallback callback, void* user) noexcept;

    // Terminal cleanup of failed, detached queue metadata only. Never calls an
    // expired runtime queue handle or claims that OS context storage was freed.
    // Active engine queues/pins prevent cleanup; destroy them first. Caller must
    // ensure no future callback can refer to these retired cookies.
    HRESULT discard_retired_metadata(unsigned& unresolved_contexts) noexcept;
};
} // namespace native12
