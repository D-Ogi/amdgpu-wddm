// SPDX-License-Identifier: MIT
// Round trip 3: the removed-device sentinel of the release sequence. GetCompletedValue of a removed device returns
// UINT64_MAX, which proves nothing about retirement: a release that meets it, at its snapshot or at a later poll,
// stays pending for the device's life and its memory stays owned. The harness makes the queue's retirement fence
// read UINT64_MAX; no real device is removed. Runs on a second device context, which is left alive at the end
// because destroy_device_context must refuse it (S_FALSE with the stuck releases counted).
#include "harness.h"

namespace harness {

namespace {
struct Observed {
    size_t events = 0;
};
void observe(void* user, const engine_ddi::ReleaseEvent*) { ++static_cast<Observed*>(user)->events; }
} // namespace

void test_retirement(Env& env) {
    static Device device;                   // outlives the test: its context stays alive by design
    HRESULT hr = open_device(env, device);
    checkf(hr == S_OK && device.context, "retirement: second device context (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    Observed observed;
    engine_ddi::harness_set_release_observer(device.context, observe, &observed);

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    Buffer a, b;
    const HRESULT hr_a = create_buffer(env, device, HeapKind::Upload, 65536, false, a);
    const HRESULT hr_b = create_buffer(env, device, HeapKind::Upload, 65536, false, b);
    checkf(hr == S_OK && hr_a == S_OK && hr_b == S_OK, "retirement: DIRECT queue and two UPLOAD buffers (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK || hr_a != S_OK || hr_b != S_OK) return;

    // Buffer a: pending at its destroy (the fence reads 1, the mark is 2), then the fence reads the sentinel.
    hr = engine_ddi::execute_command_lists(queue, 0, nullptr);                        // signal 1
    engine_ddi::harness_force_completed(queue, 1);
    if (hr == S_OK) hr = engine_ddi::execute_command_lists(queue, 0, nullptr);         // signal 2
    destroy_buffer(env, device, a);
    const uint32_t pending_a = engine_ddi::harness_pending_releases(device.context);
    engine_ddi::harness_force_completed(queue, engine_ddi::kFenceRemoved);
    const HRESULT hr_poll = engine_ddi::execute_command_lists(queue, 0, nullptr);   // a retirement point
    checkf(hr == S_OK && pending_a == 1 && hr_poll == S_OK && engine_ddi::harness_pending_releases(device.context) == 1 &&
               engine_ddi::harness_stuck_releases(device.context) == 1 && !observed.events,
           "retirement: a pending release that polls UINT64_MAX is stuck, not retired (%u pending, %u stuck, %zu run)",
           engine_ddi::harness_pending_releases(device.context), engine_ddi::harness_stuck_releases(device.context),
           observed.events);

    // Buffer b: the snapshot itself reads the sentinel.
    destroy_buffer(env, device, b);
    checkf(engine_ddi::harness_pending_releases(device.context) == 2 &&
               engine_ddi::harness_stuck_releases(device.context) == 2 && !observed.events,
           "retirement: a release whose snapshot reads UINT64_MAX is stuck at once (%u pending, %u stuck)",
           engine_ddi::harness_pending_releases(device.context), engine_ddi::harness_stuck_releases(device.context));

    const engine_ddi::QueueClose closed = engine_ddi::destroy_engine_queue(queue);
    uint32_t live = 0;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(closed == engine_ddi::QueueClose::NotRetired && engine_ddi::harness_retirement_lost(device.context) &&
               hr == S_FALSE && live == 2 && !observed.events,
           "retirement: the queue's destroy reports NotRetired and loses retirement, and destroy_device_context keeps "
           "the context with its two stuck releases (hr %08lx, %u live)",
           static_cast<unsigned long>(hr), live);
    engine_ddi::harness_set_release_observer(device.context, nullptr, nullptr);
}

} // namespace harness
