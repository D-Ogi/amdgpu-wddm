// SPDX-License-Identifier: MIT
// engine-ddi: the engine parts of the shell's queue slots (create_engine_queue, destroy_engine_queue,
// execute_command_lists) and the retirement fence of each engine queue.
#include "internal.h"

namespace engine_ddi {

namespace {
bool valid_queue_type(UINT32 type) noexcept {
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
           type == D3D12_COMMAND_LIST_TYPE_COPY;
}
} // namespace

HRESULT create_engine_queue(DeviceContext* c, const BC250_VKD3D_COMMAND_QUEUE_DESC* desc, void* cookie,
                            EngineQueue** out) noexcept {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!c || !desc || desc->Size != sizeof(BC250_VKD3D_COMMAND_QUEUE_DESC) || !valid_queue_type(desc->Type) ||
        desc->NodeMask > 1)
        return E_INVALIDARG;
    c->process_retired();

    auto* q = make_new<EngineQueue>();
    if (!q) return E_OUTOFMEMORY;
    // Reserve a slot first, so that a device at kMaxEngineQueues creates nothing in the engine.
    uint32_t slot = kMaxEngineQueues;
    AcquireSRWLockExclusive(&c->lock);
    for (uint32_t s = 0; s < kMaxEngineQueues; ++s) {
        if (!((c->queue_mask >> s) & 1) && !c->queues[s]) {
            slot = s;
            c->queues[s] = q;                           // reserved: not in queue_mask until the queue exists
            break;
        }
    }
    ReleaseSRWLockExclusive(&c->lock);
    if (slot == kMaxEngineQueues) {
        delete q;
        return E_OUTOFMEMORY;
    }
    auto unreserve = [&]() noexcept {
        AcquireSRWLockExclusive(&c->lock);
        c->queues[slot] = nullptr;
        ReleaseSRWLockExclusive(&c->lock);
        delete q;
    };

    ID3D12CommandQueue* queue = nullptr;
    HRESULT hr = c->funcs.CreateCommandQueue(c->device, desc, cookie, __uuidof(ID3D12CommandQueue),
                                             reinterpret_cast<void**>(&queue));
    if (FAILED(hr)) {
        unreserve();
        return hr;
    }
    ID3D12Fence* fence = nullptr;
    hr = c->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (FAILED(hr)) {
        queue->Release();
        unreserve();
        return hr;
    }
    q->context = c;
    q->queue = queue;
    q->fence = fence;
    q->id = c->next_id.fetch_add(1);
    q->slot = slot;
    q->type = static_cast<D3D12_COMMAND_LIST_TYPE>(desc->Type);
    q->state.store(0);
    InitializeSRWLock(&q->submit_lock);
    AcquireSRWLockExclusive(&c->lock);
    c->queue_mask |= uint64_t{1} << slot;
    ReleaseSRWLockExclusive(&c->lock);
    *out = q;
    return S_OK;
}

void destroy_engine_queue(EngineQueue* q) noexcept {
    if (!q) return;
    DeviceContext* c = q->context;
    // The final Release waits for the queue's last submission, the retirement signal included (engine V7).
    q->queue->Release();
    q->queue = nullptr;
    AcquireSRWLockExclusive(&c->lock);
    const uint64_t state = q->state.load();
    const uint64_t done = completed_value(q);
    // Clean: the device is not removed, no submission lacks its signal, and the fence reached the last signal.
    const bool clean = done != kFenceRemoved && !(state & 1) && done >= (state >> 1);
    c->releases.queue_destroyed(q->slot, done);
    if (!clean) c->retirement_lost = true;
    c->queue_mask &= ~(uint64_t{1} << q->slot);
    c->queues[q->slot] = nullptr;
    ReleaseSRWLockExclusive(&c->lock);
    if (!clean)
        log_line("queue %llu destroyed without proven retirement (fence %llu, signalled %llu, unsignalled work %d): "
                 "later releases stay pending",
                 static_cast<unsigned long long>(q->id), static_cast<unsigned long long>(done),
                 static_cast<unsigned long long>(state >> 1), static_cast<int>(state & 1));
    q->fence->Release();
    delete q;
    c->process_retired();
}

HRESULT execute_command_lists(EngineQueue* q, UINT count, const D3D12DDI_HCOMMANDLIST* lists) noexcept {
    if (!q || (count && !lists)) return E_INVALIDARG;
    DeviceContext* c = q->context;
    std::vector<ID3D12CommandList*> engine;
    try {
        engine.reserve(count);
    } catch (...) {
        c->report(E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    }
    for (UINT i = 0; i < count; ++i) {
        auto* l = record_of<CommandListRecord>(lists[i].pDrvPrivate, Tag::CommandList, c);
        if (!l) {
            log_line("ExecuteCommandLists: list %u is not a live command list of this device", i);
            c->report(E_INVALIDARG);
            return E_INVALIDARG;
        }
        engine.push_back(l->list());
    }
    HRESULT hr = S_OK;
    AcquireSRWLockExclusive(&q->submit_lock);
    // From here until a signal succeeds, the queue has work that no signal covers (state bit 0).
    const uint64_t signalled = q->state.fetch_or(1) >> 1;
    if (count) q->queue->ExecuteCommandLists(count, engine.data());
    const uint64_t value = signalled + 1;
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    if (q->harness_fail_signal.exchange(false))
        hr = E_FAIL;
    else
#endif
        hr = q->queue->Signal(q->fence, value);
    if (SUCCEEDED(hr)) q->state.store(value << 1);          // the signal covers everything submitted before it
    ReleaseSRWLockExclusive(&q->submit_lock);
    if (FAILED(hr)) {
        // Bit 0 stays set: releases wait for the queue's next successful signal, never for an older one.
        c->report(hr);
        return hr;
    }
    c->process_retired();
    return S_OK;
}

#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
ID3D12CommandQueue* harness_engine_queue(EngineQueue* q) noexcept { return q ? q->queue : nullptr; }
void harness_force_completed(EngineQueue* q, uint64_t value) noexcept { q->harness_completed.store(value); }
void harness_fail_next_signal(EngineQueue* q) noexcept { q->harness_fail_signal.store(true); }
#endif

} // namespace engine_ddi
