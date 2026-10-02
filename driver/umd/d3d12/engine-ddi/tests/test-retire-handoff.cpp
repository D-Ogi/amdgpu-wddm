// SPDX-License-Identifier: MIT
// The retire hand-off (set_retire_policy). With the hand-off on, a submission leaves a retired release to the next
// pfnCreateHeapAndResource or pfnDestroyHeapAndResource, which runs it on its own thread; the backlog bound and the
// age bound make a submission run it again; with the hand-off off a submission runs it, as in r4. Pending and
// retired are forced through the queue's completed value (harness_force_completed), as in test-retirement.cpp: the
// fence values are the harness's, the GPU work is the engine's empty submissions. Runs on a device context of its
// own, which ends with no live object.
#include "harness.h"
#include <thread>

namespace harness {

namespace {
struct Observed {
    size_t events = 0;
    DWORD thread = 0;
    bool deferred = false;
};
void observe(void* user, const engine_ddi::ReleaseEvent* event) {
    auto* o = static_cast<Observed*>(user);
    ++o->events;
    o->thread = event->thread;
    o->deferred = event->deferred;
}
HRESULT policy(Device& device, uint32_t handoff, uint32_t backlog, uint32_t age_ms) {
    const engine_ddi::RetirePolicy p{sizeof(p), handoff, backlog, age_ms};
    return engine_ddi::set_retire_policy(device.context, &p);
}
} // namespace

void test_retire_handoff(Env& env) {
    Device device;
    HRESULT hr = open_device(env, device);
    checkf(hr == S_OK && device.context, "retire hand-off: device context (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    Observed observed;
    engine_ddi::harness_set_release_observer(device.context, observe, &observed);
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    check(policy(device, 1, 8, 10000) == S_OK, "retire hand-off: policy set before the first queue");
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    Buffer a, c, d, e, f;
    const HRESULT hr_a = create_buffer(env, device, HeapKind::Upload, 65536, false, a);
    checkf(hr == S_OK && hr_a == S_OK, "retire hand-off: DIRECT queue and an UPLOAD buffer (hr %08lx, %08lx)",
           static_cast<unsigned long>(hr), static_cast<unsigned long>(hr_a));
    if (hr != S_OK || hr_a != S_OK) return;
    const auto ecl = [&]() { return engine_ddi::execute_command_lists(queue, 0, nullptr); };
    const auto pending = [&]() { return engine_ddi::harness_pending_releases(device.context); };

    // 1. Hand-off: pending at its destroy (fence held at 1, mark 2), retired (fence 2), then a submission leaves it
    //    and a create on another thread runs it.
    hr = ecl();                                                             // signal 1
    engine_ddi::harness_force_completed(queue, 1);
    if (hr == S_OK) hr = ecl();                                             // signal 2
    destroy_buffer(env, device, a);
    const uint32_t pending_a = pending();
    engine_ddi::harness_force_completed(queue, 2);
    const uint64_t deferred_before = engine_ddi::harness_deferred_retire_points(device.context);
    if (hr == S_OK) hr = ecl();                                             // signal 3: a retirement point, deferred
    checkf(hr == S_OK && pending_a == 1 && pending() == 1 && !observed.events &&
               engine_ddi::harness_deferred_retire_points(device.context) == deferred_before + 1,
           "retire hand-off: a submission leaves a retired release pending (%u then %u pending, %zu run)", pending_a,
           pending(), observed.events);
    DWORD worker_id = 0;
    HRESULT hr_c = E_FAIL;
    std::thread worker([&]() {
        worker_id = GetCurrentThreadId();
        hr_c = create_buffer(env, device, HeapKind::Upload, 65536, false, c);
    });
    worker.join();
    checkf(hr_c == S_OK && observed.events == 1 && observed.thread == worker_id && observed.deferred && !pending(),
           "retire hand-off: the next create runs the release on its own thread (hr %08lx, %zu run, thread %lu, "
           "worker %lu)",
           static_cast<unsigned long>(hr_c), observed.events, observed.thread, worker_id);

    // 2. Backlog bound 1: the submission runs it.
    check(policy(device, 1, 1, 10000) == S_OK, "retire hand-off: backlog bound 1 set");
    destroy_buffer(env, device, c);                                         // mark 3, fence held at 2
    const uint32_t pending_c = pending();
    engine_ddi::harness_force_completed(queue, 3);
    if (hr == S_OK) hr = ecl();                                             // signal 4
    checkf(hr == S_OK && pending_c == 1 && observed.events == 2 && observed.thread == GetCurrentThreadId() &&
               !pending(),
           "retire hand-off: at the backlog bound the submission runs the release (%u pending before, %zu run)",
           pending_c, observed.events);

    // 3. Age bound 1 ms: after a pause with no resource DDI, the submission runs it.
    check(policy(device, 1, 8, 1) == S_OK, "retire hand-off: age bound 1 ms set");
    const HRESULT hr_d = create_buffer(env, device, HeapKind::Upload, 65536, false, d);
    destroy_buffer(env, device, d);                                         // mark 4, fence held at 3
    const uint32_t pending_d = pending();
    engine_ddi::harness_force_completed(queue, 4);
    Sleep(50);                                                              // GetTickCount64 steps by about 16 ms
    if (hr == S_OK) hr = ecl();                                             // signal 5
    checkf(hr == S_OK && hr_d == S_OK && pending_d == 1 && observed.events == 3 && !pending(),
           "retire hand-off: past the age bound the submission runs the release (%u pending before, %zu run)",
           pending_d, observed.events);

    // 4. Hand-off off (r4): the submission runs it.
    check(policy(device, 0, 0, 0) == S_OK, "retire hand-off: policy off");
    const HRESULT hr_e = create_buffer(env, device, HeapKind::Upload, 65536, false, e);
    destroy_buffer(env, device, e);                                         // mark 5, fence held at 4
    const uint32_t pending_e = pending();
    engine_ddi::harness_force_completed(queue, 5);
    const uint64_t deferred_off = engine_ddi::harness_deferred_retire_points(device.context);
    if (hr == S_OK) hr = ecl();                                             // signal 6
    checkf(hr == S_OK && hr_e == S_OK && pending_e == 1 && observed.events == 4 && !pending() &&
               engine_ddi::harness_deferred_retire_points(device.context) == deferred_off,
           "retire hand-off: off, the submission runs the release (%u pending before, %zu run)", pending_e,
           observed.events);

    // 5. Refusals keep the policy held before (off): a release is again run by the submission.
    const engine_ddi::RetirePolicy wrong_size{sizeof(engine_ddi::RetirePolicy) - 4, 1, 8, 250};
    const bool refused = engine_ddi::set_retire_policy(device.context, nullptr) == E_INVALIDARG &&
                         engine_ddi::set_retire_policy(device.context, &wrong_size) == E_INVALIDARG &&
                         policy(device, 2, 8, 250) == E_INVALIDARG && policy(device, 1, 0, 250) == E_INVALIDARG &&
                         policy(device, 1, 8, 0) == E_INVALIDARG && policy(device, 1, 8, 10001) == E_INVALIDARG;
    const HRESULT hr_f = create_buffer(env, device, HeapKind::Upload, 65536, false, f);
    destroy_buffer(env, device, f);                                         // mark 6, fence held at 5
    const uint32_t pending_f = pending();
    engine_ddi::harness_force_completed(queue, 6);
    if (hr == S_OK) hr = ecl();                                             // signal 7
    checkf(refused && hr == S_OK && hr_f == S_OK && pending_f == 1 && observed.events == 5 && !pending(),
           "retire hand-off: null, size, handoff and bounds refused, the policy before kept (%zu run)",
           observed.events);

    // The real fence from here on: the queue retires clean and the context ends with nothing live.
    engine_ddi::harness_force_completed(queue, 0);
    const engine_ddi::QueueClose closed = engine_ddi::destroy_engine_queue(queue);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(closed == engine_ddi::QueueClose::Retired && hr == S_OK && live == 0 && !device.shell.device_errors,
           "retire hand-off: queue retired, context destroyed with no live object (hr %08lx, %u live, %u errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors);
}

} // namespace harness
