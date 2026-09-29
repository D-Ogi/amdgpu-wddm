// SPDX-License-Identifier: MIT
#include "queue-engine.h"
#include <limits>
#include <new>

namespace native12 {
struct QueueEngineOwner {
    QueueContext context;
    D3D12DDI_HRTCOMMANDQUEUE runtime;
    engine_ddi::EngineQueue* engine{};
    QueueEngineOwner* next{};
    QueueEngineState state{QueueEngineState::Creating};
    unsigned pins{};
    UINT64 serial{};
    HANDLE binding_context{};
    DWORD operation_thread{};
    QueueEngineOwner(D3D12DDI_HRTCOMMANDQUEUE q,
        const D3D12DDI_CORELAYER_DEVICECALLBACKS_0062& cb) : context(q, cb), runtime(q) {}
};
QueueEngineOps QueueEngineOps::native() noexcept {
    QueueEngineOps ops{engine_ddi::create_engine_queue, engine_ddi::execute_command_lists};
    ops.update_tiles = engine_ddi::update_tile_mappings; ops.copy_tiles = engine_ddi::copy_tile_mappings;
    return ops;
}
QueueEngineRegistry::QueueEngineRegistry(Device& device, engine_ddi::DeviceContext* engine,
    QueueEngineOps ops) noexcept : device_(device), engine_(engine), ops_(ops) {}

// Caller holds lock_. Comparing the opaque cookie does not dereference it.
QueueEngineOwner* QueueEngineRegistry::find(void* cookie, UINT64 serial) noexcept {
    for (auto q = head_; q; q = q->next)
        if (q == cookie && (!serial || q->serial == serial)) return q;
    return nullptr;
}
void QueueEngineRegistry::erase(QueueEngineOwner* q) noexcept {
    AcquireSRWLockExclusive(&lock_);
    auto link = &head_;
    while (*link && *link != q) link = &(*link)->next;
    if (*link) *link = q->next;
    ReleaseSRWLockExclusive(&lock_);
    delete q;
}
HRESULT QueueEngineRegistry::release_context(QueueEngineOwner* q) noexcept {
    // Engine destruction may report a timeout through ShellHooks. In that case
    // its return alone is not retirement proof: retain the runtime context.
    AcquireSRWLockExclusive(&lock_);
    q->state = QueueEngineState::Retired;
    const bool pinned = q->pins != 0;
    ReleaseSRWLockExclusive(&lock_);
    const HRESULT engine_health = ops_.check_health(ops_.health_cookie);
    if (engine_health != S_OK) device_.remove();
    HRESULT hr = pinned ? E_PENDING :
        device_.lost.load() ? D3DDDIERR_DEVICEREMOVED : q->context.close();
    if (hr != S_OK) {
        q->context.invalidate_runtime();
        AcquireSRWLockExclusive(&lock_);
        q->state = QueueEngineState::Retired;
        ReleaseSRWLockExclusive(&lock_);
        device_.remove();
    } else {
        erase(q);
    }
    return hr;
}
HRESULT QueueEngineRegistry::create(const D3D12DDIARG_CREATECOMMANDQUEUE_0050& args,
    D3D12DDI_HRTCOMMANDQUEUE runtime, QueueEngineSlot& slot) noexcept {
    if (slot.cookie || slot.serial || slot.owner || !engine_ || !ops_.create || !ops_.execute || !ops_.close || !ops_.check_health)
        return E_INVALIDARG;
    if (device_.lost.load()) return D3DDDIERR_DEVICEREMOVED;
    HRESULT health = ops_.check_health(ops_.health_cookie);
    if (health != S_OK) { device_.remove(); return FAILED(health) ? health : E_UNEXPECTED; }
    ContextRequest request;
    HRESULT hr = request.prepare(args);
    if (hr != S_OK) return hr;
    auto q = new(std::nothrow) QueueEngineOwner(runtime, device_.callbacks);
    if (!q) return E_OUTOFMEMORY;
    AcquireSRWLockExclusive(&lock_);
    if (serial_ == std::numeric_limits<UINT64>::max()) {
        ReleaseSRWLockExclusive(&lock_); delete q; return E_OUTOFMEMORY;
    }
    q->serial = ++serial_; q->operation_thread = GetCurrentThreadId();
    q->next = head_; head_ = q;
    ReleaseSRWLockExclusive(&lock_);
    hr = q->context.open(request.args);
    if (hr != S_OK) {
        if (q->context.handle()) release_context(q); else erase(q);
        return FAILED(hr) ? hr : E_UNEXPECTED;
    }
    AcquireSRWLockExclusive(&lock_); q->binding_context = q->context.handle();
    ReleaseSRWLockExclusive(&lock_);
    BC250_VKD3D_COMMAND_QUEUE_DESC desc{};
    desc.Size = sizeof(desc);
    desc.Type = (args.QueueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_3D) ? D3D12_COMMAND_LIST_TYPE_DIRECT :
        (args.QueueFlags & D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE) ? D3D12_COMMAND_LIST_TYPE_COMPUTE :
        D3D12_COMMAND_LIST_TYPE_COPY;
    desc.NodeMask = args.NodeMask;
    // Create has no priority parameter in DDI0050. Zero priority/flags are the
    // default engine policy; ContextRequest already rejects creation flags.
    hr = ops_.create(engine_, &desc, q, &q->engine);
    if (hr != S_OK || !q->engine) {
        const HRESULT failure = FAILED(hr) ? hr : E_UNEXPECTED;
        if (q->engine) {
            // Malformed engine ownership result: do not dereference an object
            // that the failed create never transferred. Keep its context.
            q->context.invalidate_runtime();
            AcquireSRWLockExclusive(&lock_); q->state = QueueEngineState::Retired;
            ReleaseSRWLockExclusive(&lock_); device_.remove();
        } else {
            release_context(q);
        }
        return failure;
    }
    if (device_.lost.load() || ops_.check_health(ops_.health_cookie) != S_OK) {
        device_.remove();
        AcquireSRWLockExclusive(&lock_); q->state = QueueEngineState::Destroying;
        ReleaseSRWLockExclusive(&lock_);
        const HRESULT close_hr = ops_.close(&q->engine);
        if (close_hr == S_OK && !q->engine) release_context(q);
        else {
            q->context.invalidate_runtime();
            AcquireSRWLockExclusive(&lock_); q->state = QueueEngineState::Retired;
            ReleaseSRWLockExclusive(&lock_);
        }
        return D3DDDIERR_DEVICEREMOVED;
    }
    AcquireSRWLockExclusive(&lock_);
    q->state = QueueEngineState::Live; q->operation_thread = 0;
    slot = {q, q->serial, &device_};
    ReleaseSRWLockExclusive(&lock_);
    return S_OK;
}
HRESULT QueueEngineRegistry::admit(const QueueEngineSlot& slot, QueueEngineOwner*& queue) noexcept {
    queue = nullptr;
    if (slot.owner != &device_ || !slot.cookie || !slot.serial) return E_INVALIDARG;
    if (device_.lost.load()) return D3DDDIERR_DEVICEREMOVED;
    AcquireSRWLockExclusive(&lock_);
    auto q = find(slot.cookie, slot.serial);
    if (!q) { ReleaseSRWLockExclusive(&lock_); return E_INVALIDARG; }
    if (q->state != QueueEngineState::Live || q->pins) {
        ReleaseSRWLockExclusive(&lock_); return E_PENDING;
    }
    q->state = QueueEngineState::Executing; q->operation_thread = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&lock_);
    queue = q; return S_OK;
}
HRESULT QueueEngineRegistry::leave(QueueEngineOwner* q, HRESULT hr) noexcept {
    if (hr == S_OK) hr = ops_.check_health(ops_.health_cookie);
    AcquireSRWLockExclusive(&lock_); q->state = QueueEngineState::Live; q->operation_thread = 0;
    ReleaseSRWLockExclusive(&lock_);
    if (hr != S_OK) { device_.remove(); if (SUCCEEDED(hr)) hr = E_UNEXPECTED; }
    return hr == S_OK && device_.lost.load() ? D3DDDIERR_DEVICEREMOVED : hr;
}
HRESULT QueueEngineRegistry::execute(const QueueEngineSlot& slot, UINT count,
    const D3D12DDI_HCOMMANDLIST* lists) noexcept {
    if (count && !lists) return E_INVALIDARG;
    QueueEngineOwner* q; const HRESULT hr = admit(slot, q);
    return hr != S_OK ? hr : leave(q, ops_.execute(q->engine, count, lists));
}
HRESULT QueueEngineRegistry::update_tiles(const QueueEngineSlot& slot, D3D12DDI_HRESOURCE resource, UINT region_count,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* region_starts, const D3D12DDI_TILE_REGION_SIZE* region_sizes,
    D3D12DDI_HHEAP heap, UINT range_count, const D3D12DDI_TILE_RANGE_FLAGS* range_flags,
    const UINT* heap_range_starts, const UINT* range_tile_counts, D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept {
    if (!ops_.update_tiles) return E_NOTIMPL;
    QueueEngineOwner* q; const HRESULT hr = admit(slot, q);
    return hr != S_OK ? hr : leave(q, ops_.update_tiles(q->engine, resource, region_count, region_starts,
        region_sizes, heap, range_count, range_flags, heap_range_starts, range_tile_counts, flags));
}
HRESULT QueueEngineRegistry::copy_tiles(const QueueEngineSlot& slot, D3D12DDI_HRESOURCE dst,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* dst_start, D3D12DDI_HRESOURCE src,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* src_start, const D3D12DDI_TILE_REGION_SIZE* size,
    D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept {
    if (!ops_.copy_tiles) return E_NOTIMPL;
    QueueEngineOwner* q; const HRESULT hr = admit(slot, q);
    return hr != S_OK ? hr : leave(q, ops_.copy_tiles(q->engine, dst, dst_start, src, src_start, size, flags));
}
HRESULT QueueEngineRegistry::destroy(QueueEngineSlot& slot) noexcept {
    if (!slot.cookie && !slot.serial && !slot.owner) return S_OK;
    if (slot.owner != &device_) return E_INVALIDARG;
    AcquireSRWLockExclusive(&lock_);
    auto q = slot.serial ? find(slot.cookie, slot.serial) : nullptr;
    if (!q) { ReleaseSRWLockExclusive(&lock_); return E_INVALIDARG; }
    if (q->state != QueueEngineState::Live || q->pins) {
        ReleaseSRWLockExclusive(&lock_); return E_PENDING;
    }
    q->state = QueueEngineState::Destroying; q->operation_thread = GetCurrentThreadId();
    ReleaseSRWLockExclusive(&lock_);
    HRESULT close_hr = ops_.close(&q->engine);
    if (close_hr == S_OK && q->engine) close_hr = E_UNEXPECTED;
    // All engine callbacks have finished. Prevent a new binding pin between
    // this check and context release; existing pins require retaining ownership.
    AcquireSRWLockExclusive(&lock_);
    const bool pinned = q->pins != 0;
    q->state = QueueEngineState::Retired;
    slot = {};
    ReleaseSRWLockExclusive(&lock_);
    if (pinned || close_hr != S_OK) {
        q->context.invalidate_runtime(); device_.remove();
        return pinned ? E_PENDING : FAILED(close_hr) ? close_hr : E_UNEXPECTED;
    }
    return release_context(q);
}
HRESULT QueueEngineRegistry::present_context(const QueueEngineSlot& slot, HANDLE* context) noexcept {
    if (!context) return E_INVALIDARG;
    *context = nullptr;
    if (slot.owner != &device_ || !slot.cookie || !slot.serial) return E_INVALIDARG;
    if (device_.lost.load()) return D3DDDIERR_DEVICEREMOVED;
    AcquireSRWLockShared(&lock_);
    const auto q = find(slot.cookie, slot.serial);
    HRESULT hr = !q ? E_INVALIDARG : (q->state != QueueEngineState::Live || q->pins) ? E_PENDING : S_OK;
    if (hr == S_OK) {
        *context = q->context.handle();
        if (!*context) hr = E_UNEXPECTED;
    }
    ReleaseSRWLockShared(&lock_);
    return hr;
}
bool QueueEngineRegistry::owns(const QueueEngineSlot& slot) noexcept {
    if (slot.owner != &device_ || !slot.cookie || !slot.serial) return false;
    AcquireSRWLockShared(&lock_);
    const auto q = find(slot.cookie, slot.serial);
    const bool owned = q && q->state != QueueEngineState::Retired;
    ReleaseSRWLockShared(&lock_);
    return owned;
}
HRESULT QueueEngineRegistry::with_binding(void* cookie, Device& expected_device,
    QueueBindingCallback callback, void* user) noexcept {
    if (!cookie || &expected_device != &device_ || !callback) return E_INVALIDARG;
    AcquireSRWLockExclusive(&lock_);
    auto q = find(cookie, 0);
    if (!q || q->state == QueueEngineState::Retired || !q->binding_context) {
        ReleaseSRWLockExclusive(&lock_); return E_INVALIDARG;
    }
    if (q->state != QueueEngineState::Live && q->operation_thread != GetCurrentThreadId()) {
        ReleaseSRWLockExclusive(&lock_); return E_PENDING;
    }
    if (q->pins == std::numeric_limits<unsigned>::max()) {
        ReleaseSRWLockExclusive(&lock_); return E_OUTOFMEMORY;
    }
    ++q->pins;
    const QueueBindingView view{q->runtime, q->binding_context, q->state};
    ReleaseSRWLockExclusive(&lock_);
    HRESULT hr = callback(user, &view);
    AcquireSRWLockExclusive(&lock_); --q->pins; ReleaseSRWLockExclusive(&lock_);
    return hr;
}
HRESULT QueueEngineRegistry::discard_retired_metadata(unsigned& unresolved_contexts) noexcept {
    unresolved_contexts = 0;
    AcquireSRWLockExclusive(&lock_);
    for (auto q = head_; q; q = q->next) {
        if (q->state != QueueEngineState::Retired || q->pins || q->engine) {
            ReleaseSRWLockExclusive(&lock_); return E_PENDING;
        }
    }
    auto list = head_; head_ = nullptr;
    ReleaseSRWLockExclusive(&lock_);
    while (list) {
        auto q = list; list = q->next;
        if (q->context.handle()) ++unresolved_contexts;
        q->context.invalidate_runtime(); delete q;
    }
    return unresolved_contexts ? S_FALSE : S_OK;
}
} // namespace native12
