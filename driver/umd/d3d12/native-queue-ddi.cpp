// SPDX-License-Identifier: MIT
#include "native-queue-ddi.h"
#include "device-engine.h"
#include "fence-ddi.h"
#include "ddi-trace.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <new>
#include "stdio-log.h"

namespace native12 {
namespace {
QueueEngineSlot* slot_of(D3D12DDI_HCOMMANDQUEUE h) noexcept {
    if (!h.pDrvPrivate || reinterpret_cast<uintptr_t>(h.pDrvPrivate) % alignof(QueueEngineSlot)) return nullptr;
    return static_cast<QueueEngineSlot*>(h.pDrvPrivate);
}
HRESULT device_status(D3D12DDI_HDEVICE h) noexcept {
    if (!h.pDrvPrivate) return E_INVALIDARG;
    return static_cast<Device*>(h.pDrvPrivate)->lost.load() ? D3DDDIERR_DEVICEREMOVED : S_OK;
}
void queue_failure(D3D12DDI_HCOMMANDQUEUE h) noexcept {
    // The runtime guarantees private-storage lifetime. Do not use an invalid
    // registry cookie to call the engine; report via the slot's owning device.
    if (auto slot = slot_of(h); slot && slot->owner) slot->owner->remove();
}
// Private storage has one fixed size. Sizing does not judge the request or the
// device; pfnCreateCommandQueue validates both before the first write.
SIZE_T APIENTRY native_queue_size(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATECOMMANDQUEUE_0050*) {
    return sizeof(QueueEngineSlot);
}
HRESULT APIENTRY native_queue_create(D3D12DDI_HDEVICE h, const D3D12DDIARG_CREATECOMMANDQUEUE_0050* args,
    D3D12DDI_HCOMMANDQUEUE queue, D3D12DDI_HRTCOMMANDQUEUE runtime) {
    if (!args || !slot_of(queue)) return E_INVALIDARG;
    HRESULT hr = device_status(h); if (hr != S_OK) return hr;
    auto& device = *static_cast<Device*>(h.pDrvPrivate);
    QueueDomainScope serial(&device);
    auto registry = engine_queues(device);
    if (!engine_context(device) || !registry) return E_UNEXPECTED;
    { ContextRequest probe; hr = probe.prepare(*args); if (hr != S_OK) return hr; }
    auto slot = new(queue.pDrvPrivate) QueueEngineSlot{};
    hr = registry->create(*args, runtime, *slot);
    if (hr != S_OK) slot->~QueueEngineSlot();
    return hr;
}
void APIENTRY native_queue_destroy(D3D12DDI_HDEVICE h, D3D12DDI_HCOMMANDQUEUE queue) {
    auto slot = slot_of(queue);
    if (!h.pDrvPrivate || !slot) return;
    auto& device = *static_cast<Device*>(h.pDrvPrivate);
    QueueDomainScope serial(&device);
    auto registry = engine_queues(device);
    if (slot->owner != &device || !registry || !registry->owns(*slot)) { device.remove(); return; }
    const HRESULT hr = registry->destroy(*slot);
    if (hr != S_OK) device.remove();
    // E_PENDING retains a live slot: the lifetime/authority integration must
    // drain pins before allowing runtime storage and callbacks to expire.
    if (!slot->cookie) slot->~QueueEngineSlot();
}
void APIENTRY native_execute(D3D12DDI_HCOMMANDQUEUE h, UINT count, const D3D12DDI_HCOMMANDLIST* lists) {
    auto device = resolve_queue_device(h);
    if (!device) { queue_failure(h); return; }
    QueueDomainScope serial(device);
    const HRESULT hr = engine_queues(*device)->execute(*slot_of(h), count, lists);
    if (hr != S_OK) device->remove();
}
// The two fence slots are the only DDI entry points of this shell that no measurement has ever reached, so each
// records its first call per process on the always-on channel (ddi_refusal: the AMDGPU_WDDM_LOG sink when the
// process names one, and the debugger channel always, which capture-share copies into a cell's own log). Once a
// process, never per frame: a queue wait happens per submission. Until a lab row prints one of these lines, every
// statement about these slots is an argument from absence, which is what round 2 of BD-075 is here to stop.
void fence_slot_seen(bool is_wait) noexcept {
    static std::atomic<bool> seen[2]{};
    if (seen[is_wait ? 1 : 0].exchange(true, std::memory_order_relaxed)) return;
    ddi_refusal("fence slot entered: %s, the first in this process; the slot selects this single node and performs "
                "no fence operation of its own", is_wait ? "WaitForFence" : "SignalFence");
}
void APIENTRY native_signal(D3D12DDI_HCOMMANDQUEUE h, D3D12DDIARG_FENCE_OPERATION* args) {
    if (args) args->PhysicalAdapterMask = 0;
    fence_slot_seen(false);
    auto device = resolve_queue_device(h);
    if (!device || !args || !args->Fence.pDrvPrivate ||
        reinterpret_cast<uintptr_t>(args->Fence.pDrvPrivate) % alignof(FenceState)) {
        queue_failure(h); return;
    }
    const auto fence = static_cast<const FenceState*>(args->Fence.pDrvPrivate);
    if (device->lost.load() || fence->device != device) { device->remove(); return; }
    // PhysicalAdapterMask is an out parameter only - "the set of adapters to broadcast the operation to"
    // (d3d12umddi.h, D3D12DDIARG_FENCE_OPERATION) - and this adapter is a single node, so 1 is the answer.
    // INLINE Execute must already have submitted all work on that same context.
    // No separate engine fence, CPU fence write or extra GPU submission occurs.
    args->PhysicalAdapterMask = 1;
    if (ddi_trace_enabled())
        amdgpu_wddm_log::print("d3d12-ddi SignalFence runtime-context mask=1 value=%llu\n",
            static_cast<unsigned long long>(args->Value));
}
// The wait is the signal's mirror and is answered the same way: PhysicalAdapterMask is an out parameter only -
// "the set of adapters to broadcast the operation to" (d3d12umddi.h, D3D12DDIARG_FENCE_OPERATION) - so the slot
// selects this single node and performs no fence operation. It used to call queue_failure unconditionally, so
// the first pfnWaitForFence the runtime ever sent would have lost the device; a slot that cannot perform an
// operation must not remove the device, while a record it cannot trust (a null, misaligned or foreign fence)
// still does, which is why the two checks below stay (BD-075 round 2).
//
// What is measured, and what is not. The system runtime completes a single-adapter fence operation without this
// slot: w12's w12-pending control queues an ID3D12CommandQueue::Wait on a value nobody has signalled yet and
// shows the dependent signal still unreached 300 ms later (capture-share Side12::LocalWaits), so the ordering is
// real and the driver was not asked for it. What that does NOT cover is a fence another process opened: the
// eleven traced rows of 2026-10-06 contain no such wait (s11to12-fence died at open-shared and f12to12 ran
// untraced), so the cross-process case is untested in either direction. Hence fence_slot_seen above: a row that
// does reach a slot now says so in its own log, with no trace switch.
void APIENTRY native_wait(D3D12DDI_HCOMMANDQUEUE h, D3D12DDIARG_FENCE_OPERATION* args) {
    if (args) args->PhysicalAdapterMask = 0;
    fence_slot_seen(true);
    auto device = resolve_queue_device(h);
    if (!device || !args || !args->Fence.pDrvPrivate ||
        reinterpret_cast<uintptr_t>(args->Fence.pDrvPrivate) % alignof(FenceState)) {
        queue_failure(h); return;
    }
    const auto fence = static_cast<const FenceState*>(args->Fence.pDrvPrivate);
    if (device->lost.load() || fence->device != device) { device->remove(); return; }
    args->PhysicalAdapterMask = 1;
    if (ddi_trace_enabled())
        amdgpu_wddm_log::print("d3d12-ddi WaitForFence runtime-context mask=1 value=%llu\n",
            static_cast<unsigned long long>(args->Value));
}
// The tile mappings are one operation of the queue, admitted as an execute is. The engine checks
// that the resources and the heap are the device's; a refusal or a failure removes the device.
void APIENTRY native_update_tiles(D3D12DDI_HCOMMANDQUEUE h, D3D12DDI_HRESOURCE resource, UINT region_count,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* region_starts, const D3D12DDI_TILE_REGION_SIZE* region_sizes,
    D3D12DDI_HHEAP heap, UINT range_count, const D3D12DDI_TILE_RANGE_FLAGS* range_flags,
    const UINT* heap_range_starts, const UINT* range_tile_counts, D3D12DDI_TILE_MAPPING_FLAGS flags) {
    auto device = resolve_queue_device(h);
    if (!device) { queue_failure(h); return; }
    QueueDomainScope serial(device);
    const HRESULT hr = engine_queues(*device)->update_tiles(*slot_of(h), resource, region_count, region_starts,
        region_sizes, heap, range_count, range_flags, heap_range_starts, range_tile_counts, flags);
    if (hr != S_OK) device->remove();
}
void APIENTRY native_copy_tiles(D3D12DDI_HCOMMANDQUEUE h, D3D12DDI_HRESOURCE dst,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* dst_start, D3D12DDI_HRESOURCE src,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* src_start, const D3D12DDI_TILE_REGION_SIZE* size,
    D3D12DDI_TILE_MAPPING_FLAGS flags) {
    auto device = resolve_queue_device(h);
    if (!device) { queue_failure(h); return; }
    QueueDomainScope serial(device);
    const HRESULT hr = engine_queues(*device)->copy_tiles(*slot_of(h), dst, dst_start, src, src_start, size, flags);
    if (hr != S_OK) device->remove();
}
HRESULT APIENTRY extended_features(D3D12DDI_HDEVICE h, UINT32* count, D3D12DDI_FEATURE_0020*) {
    if (!count) return E_INVALIDARG;
    const HRESULT hr = device_status(h); if (hr != S_OK) return hr;
    *count = 0; return S_OK; // No extended-feature table is advertised.
}
HRESULT APIENTRY extended_versions(D3D12DDI_HDEVICE h, D3D12DDI_FEATURE_0020, UINT32* count, UINT32*) {
    if (!count) return E_INVALIDARG;
    const HRESULT hr = device_status(h); if (hr != S_OK) return hr;
    *count = 0; return E_NOTIMPL;
}
HRESULT APIENTRY extended_enable(D3D12DDI_HDEVICE h, D3D12DDI_FEATURE_0020, UINT32) {
    const HRESULT hr = device_status(h); return hr == S_OK ? E_NOTIMPL : hr;
}
HRESULT APIENTRY extended_callbacks(D3D12DDI_HDEVICE h, D3D12DDI_TABLE_TYPE, const void* table, SIZE_T size) {
    if (size && !table) return E_INVALIDARG;
    const HRESULT hr = device_status(h); return hr == S_OK ? E_NOTIMPL : hr;
}
} // namespace
Device* resolve_queue_device(D3D12DDI_HCOMMANDQUEUE h) noexcept {
    auto slot = slot_of(h);
    if (!slot || !slot->owner) return nullptr;
    auto registry = engine_queues(*slot->owner);
    return registry && registry->owns(*slot) ? slot->owner : nullptr;
}
HRESULT queue_present_context(D3D12DDI_HCOMMANDQUEUE h, Device& expected, HANDLE* context) noexcept {
    if (!context) return E_INVALIDARG;
    *context = nullptr;
    if (resolve_queue_device(h) != &expected) return E_INVALIDARG;
    return engine_queues(expected)->present_context(*slot_of(h), context);
}
void install_native_queue_entries(D3D12DDI_DEVICE_FUNCS_CORE_0088& table) noexcept {
    table.pfnCalcPrivateCommandQueueSize = native_queue_size;
    table.pfnCreateCommandQueue = native_queue_create;
    table.pfnDestroyCommandQueue = native_queue_destroy;
}
HRESULT fill_native_queue_table(D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001* output, SIZE_T size) noexcept {
    if (!output || size != sizeof(*output)) return E_INVALIDARG;
    D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001 candidate{};
    candidate.pfnExecuteCommandLists = native_execute;
    candidate.pfnUpdateTileMappings = native_update_tiles;
    candidate.pfnCopyTileMappings = native_copy_tiles;
    candidate.pfnSignalFence = native_signal;
    candidate.pfnWaitForFence = native_wait;
    // pfnUnused/pfnUnused2 are reserved void* entries, not callable slots.
    *output = candidate; return S_OK;
}
HRESULT fill_native_extended_table(D3D12DDI_EXTENDED_FEATURES_FUNCS_0021* output, SIZE_T size) noexcept {
    if (!output || size != sizeof(*output)) return E_INVALIDARG;
    *output = {extended_features, extended_versions, extended_enable, extended_callbacks};
    return S_OK;
}
} // namespace native12
