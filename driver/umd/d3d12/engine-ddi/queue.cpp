// SPDX-License-Identifier: MIT
// engine-ddi: the engine parts of the shell's queue slots (create_engine_queue, destroy_engine_queue,
// execute_command_lists), the retirement fence of each engine queue, and the initialization of committed render
// targets and depth-stencil resources.
#include "internal.h"

namespace engine_ddi {

namespace {
bool valid_queue_type(UINT32 type) noexcept {
    return type == D3D12_COMMAND_LIST_TYPE_DIRECT || type == D3D12_COMMAND_LIST_TYPE_COMPUTE ||
           type == D3D12_COMMAND_LIST_TYPE_COPY;
}
} // namespace

// Submits lists on q followed by the signal of its retirement fence. The caller holds q->submit_lock.
HRESULT submit_locked(EngineQueue* q, UINT count, ID3D12CommandList* const* lists) noexcept {
    // From here until a signal succeeds, the queue has work that no signal covers (state bit 0).
    const uint64_t signalled = q->state.fetch_or(1) >> 1;
    if (count) q->queue->ExecuteCommandLists(count, lists);
    const uint64_t value = signalled + 1;
    HRESULT hr;
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    if (q->harness_fail_signal.exchange(false))
        hr = E_FAIL;
    else
#endif
        hr = q->queue->Signal(q->fence, value);
    if (SUCCEEDED(hr)) q->state.store(value << 1);          // the signal covers everything submitted before it
    // On failure bit 0 stays set: releases wait for the queue's next successful signal, never for an older one.
    return hr;
}

// ---- Initialization of committed render targets and depth-stencil resources ------------------------------------
// engine-ddi makes a committed resource as a heap and a resource placed at 0 (resources.cpp). vkd3d-proton gives a
// placed render target or depth-stencil resource no initial layout transition, because D3D12 makes the application
// initialize a placed one; a committed one the application does not initialize. Each such committed resource is
// therefore queued at its create, and discarded as a whole (DiscardResource with no region) on engine-ddi's internal
// DIRECT list at the next execute_command_lists of the device, before that call's lists: nothing of the device
// reaches the resource on the GPU before an ExecuteCommandLists. One batch covers everything queued since the last.
// It runs on the executing queue when that is DIRECT, otherwise on a live DIRECT queue of the device; init_fence is
// signalled after it, and every queue waits for the last signalled value before its next lists. With no DIRECT queue
// the resources stay queued. vkd3d-proton does not check the D3D12 state for DiscardResource: every subresource goes
// from VK_IMAGE_LAYOUT_UNDEFINED to the resource's layout, and the content is undefined afterwards. A batch submits
// like any other work (submit_locked), so the release sequence covers it: a resource destroyed while a batch names
// it hands its engine resource to its backing, which releases it after retirement (resources.cpp).
namespace {
void unlink(DeviceContext* c, ResourceRecord* r, uint32_t state) noexcept {
    if (r->init_prev)
        r->init_prev->init_next = r->init_next;
    else
        c->init_head = r->init_next;
    if (r->init_next) r->init_next->init_prev = r->init_prev;
    r->init_prev = r->init_next = nullptr;
    r->init_state = state;
}

// Records the discards of every queued resource on an idle internal list and closes it; *index names the list.
// The caller holds init_lock and the submit_lock of the queue that will run the list. A list whose last batch has
// not completed is never reset and never waited for on the CPU: a batch can sit behind a queue Wait that only the
// application's later work satisfies. Another list is made instead.
HRESULT record_batch(DeviceContext* c, size_t* index) noexcept {
    HRESULT hr = S_OK;
    if (!c->init_fence) {
        hr = c->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                    reinterpret_cast<void**>(&c->init_fence));
        if (FAILED(hr)) return hr;
    }
    const uint64_t completed = c->init_fence->GetCompletedValue();
    size_t i = 0;
    while (i < c->init_lists.size() && c->init_lists[i].value > completed) ++i;
    if (i < c->init_lists.size()) {
        DeviceContext::InitList& idle = c->init_lists[i];
        hr = idle.allocator->Reset();
        if (SUCCEEDED(hr)) hr = idle.list->Reset(idle.allocator, nullptr);
        if (FAILED(hr)) return hr;
    } else {
        DeviceContext::InitList made{};
        hr = c->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                               reinterpret_cast<void**>(&made.allocator));
        if (SUCCEEDED(hr))
            hr = c->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, made.allocator, nullptr,
                                              __uuidof(ID3D12GraphicsCommandList),
                                              reinterpret_cast<void**>(&made.list));
        if (SUCCEEDED(hr)) {
            try {
                c->init_lists.push_back(made);
            } catch (...) {
                hr = E_OUTOFMEMORY;
            }
        }
        if (FAILED(hr)) {
            if (made.list) made.list->Release();
            if (made.allocator) made.allocator->Release();
            return hr;
        }
    }
    ID3D12GraphicsCommandList* list = c->init_lists[i].list;
    while (ResourceRecord* r = c->init_head) {
        list->DiscardResource(static_cast<ID3D12Resource*>(r->h.engine), nullptr);
        unlink(c, r, kInitRecorded);
    }
    *index = i;
    return list->Close();
}
} // namespace

void queue_initialization(DeviceContext* c, ResourceRecord* r) noexcept {
    AcquireSRWLockExclusive(&c->init_lock);
    r->init_state = kInitQueued;
    r->init_prev = nullptr;
    r->init_next = c->init_head;
    if (c->init_head) c->init_head->init_prev = r;
    c->init_head = r;
    ReleaseSRWLockExclusive(&c->init_lock);
}

bool cancel_initialization(DeviceContext* c, ResourceRecord* r) noexcept {
    AcquireSRWLockExclusive(&c->init_lock);
    if (r->init_state == kInitQueued) unlink(c, r, kInitNone);
    const bool recorded = r->init_state == kInitRecorded;
    ReleaseSRWLockExclusive(&c->init_lock);
    return recorded;
}

uint64_t flush_initializations(DeviceContext* c, EngineQueue* q) noexcept {
    HRESULT failed = S_OK;
    AcquireSRWLockExclusive(&c->init_lock);
    if (c->init_head && !c->init_broken) {
        EngineQueue* target = nullptr;
        if (q->type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            target = q;
            AcquireSRWLockExclusive(&target->submit_lock);
        } else {
            // Borrow a DIRECT queue. Its submit_lock is taken while lock is held, so a destroy, which marks the
            // queue closing under lock and then takes its submit_lock, cannot release it under us.
            AcquireSRWLockShared(&c->lock);
            for (uint32_t s = 0; s < kMaxEngineQueues && !target; ++s) {
                EngineQueue* e = ((c->queue_mask >> s) & 1) ? c->queues[s] : nullptr;
                if (e && e->type == D3D12_COMMAND_LIST_TYPE_DIRECT && !e->closing) target = e;
            }
            if (target) AcquireSRWLockExclusive(&target->submit_lock);
            ReleaseSRWLockShared(&c->lock);
        }
        if (target) {
            size_t index = 0;
            HRESULT hr = record_batch(c, &index);
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* list = c->init_lists[index].list;
                hr = submit_locked(target, 1, &list);
            }
            if (SUCCEEDED(hr)) hr = target->queue->Signal(c->init_fence, c->init_value + 1);
            if (SUCCEEDED(hr)) {
                c->init_lists[index].value = ++c->init_value;
                target->init_waited = c->init_value;
            } else {
                // The resources stay uninitialized, and no further batch is made.
                c->init_broken = true;
                failed = hr;
                while (ResourceRecord* r = c->init_head) unlink(c, r, kInitNone);
            }
            ReleaseSRWLockExclusive(&target->submit_lock);
        }
    }
    const uint64_t value = c->init_value;
    ReleaseSRWLockExclusive(&c->init_lock);
    if (FAILED(failed)) {                               // hooks are never called under a lock of engine-ddi
        log_line("initialization of committed render targets failed (hr %08lx): later ones are not initialized",
                 static_cast<unsigned long>(failed));
        c->report(failed);
    }
    return value;
}

void release_initialization(DeviceContext* c) noexcept {
    for (DeviceContext::InitList& l : c->init_lists) {
        l.list->Release();
        l.allocator->Release();
    }
    c->init_lists.clear();
    if (c->init_fence) c->init_fence->Release();
    c->init_fence = nullptr;
}

#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
uint32_t harness_pending_initializations(DeviceContext* c) noexcept {
    uint32_t n = 0;
    AcquireSRWLockShared(&c->init_lock);
    for (ResourceRecord* r = c->init_head; r; r = r->init_next) ++n;
    ReleaseSRWLockShared(&c->init_lock);
    return n;
}
#endif

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
    q->last_done.store(0);
    InitializeSRWLock(&q->submit_lock);
    q->init_waited = 0;
    q->closing = false;
    AcquireSRWLockExclusive(&c->lock);
    c->queue_mask |= uint64_t{1} << slot;
    ReleaseSRWLockExclusive(&c->lock);
    *out = q;
    return S_OK;
}

QueueClose destroy_engine_queue(EngineQueue* q) noexcept {
    if (!q) return QueueClose::Retired;
    DeviceContext* c = q->context;
    // No initialization batch may borrow the queue from here on, and one that has borrowed it has finished once
    // its submit_lock is free (flush_initializations).
    AcquireSRWLockExclusive(&c->lock);
    q->closing = true;
    ReleaseSRWLockExclusive(&c->lock);
    AcquireSRWLockExclusive(&q->submit_lock);
    ReleaseSRWLockExclusive(&q->submit_lock);
    // The final Release waits for the queue's last submission, the retirement signal included (engine V7).
    q->queue->Release();
    q->queue = nullptr;
    AcquireSRWLockExclusive(&c->lock);
    const uint64_t state = q->state.load();
    const uint64_t done = observed_value(q);
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
    return clean ? QueueClose::Retired : QueueClose::NotRetired;
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
    // Committed render targets created since the last call are initialized first; this queue then waits for them.
    const uint64_t init = flush_initializations(c, q);
    HRESULT hr = S_OK;
    AcquireSRWLockExclusive(&q->submit_lock);
    if (init > q->init_waited) {
        hr = q->queue->Wait(c->init_fence, init);
        if (SUCCEEDED(hr)) q->init_waited = init;
    }
    if (SUCCEEDED(hr)) hr = submit_locked(q, count, engine.data());
    ReleaseSRWLockExclusive(&q->submit_lock);
    if (FAILED(hr)) {
        c->report(hr);
        return hr;
    }
    c->retire_after_submit();
    return S_OK;
}

#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
ID3D12CommandQueue* harness_engine_queue(EngineQueue* q) noexcept { return q ? q->queue : nullptr; }
void harness_force_completed(EngineQueue* q, uint64_t value) noexcept { q->harness_completed.store(value); }
void harness_fail_next_signal(EngineQueue* q) noexcept { q->harness_fail_signal.store(true); }
#endif

} // namespace engine_ddi
