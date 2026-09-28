// SPDX-License-Identifier: MIT
// Round trip 1: the first functional path. UPLOAD -> DEFAULT -> READBACK through DDI heaps and resources, one
// DIRECT command list with CopyBufferRegion and a transition barrier, execute_command_lists on an engine queue,
// fence completion, MapHeap readback and a word-exact compare. The upload buffer is destroyed while its copy is
// still unretired, so its release must wait for the queue's retirement fence and run at a later DDI call.
#include "harness.h"
#include <cstring>

namespace harness {

namespace {
struct Observed {
    std::vector<engine_ddi::ReleaseEvent> events;
};
void observe(void* user, const engine_ddi::ReleaseEvent* event) {
    static_cast<Observed*>(user)->events.push_back(*event);
}
} // namespace

void test_copy(Env& env, Device& device) {
    constexpr UINT kWords = 16384;
    constexpr UINT64 kBytes = kWords * sizeof(UINT32);
    Observed observed;
    engine_ddi::harness_set_release_observer(device.context, observe, &observed);

    Buffer upload, gpu, readback;
    HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, kBytes, false, upload);
    HRESULT hr_d = create_buffer(env, device, HeapKind::Default, kBytes, false, gpu);
    HRESULT hr_r = create_buffer(env, device, HeapKind::Readback, kBytes, false, readback);
    checkf(hr_u == S_OK && hr_d == S_OK && hr_r == S_OK,
           "copy: committed UPLOAD, DEFAULT and READBACK buffers of %llu bytes (hr %08lx %08lx %08lx)",
           static_cast<unsigned long long>(kBytes), static_cast<unsigned long>(hr_u), static_cast<unsigned long>(hr_d),
           static_cast<unsigned long>(hr_r));
    if (hr_u != S_OK || hr_d != S_OK || hr_r != S_OK) return;
    const D3D12DDI_GPU_VIRTUAL_ADDRESS va = env.core.pfnCheckResourceVirtualAddress(device.h(), gpu.hres());
    checkf(va != 0, "copy: CheckResourceVirtualAddress of the DEFAULT buffer is %llx", static_cast<unsigned long long>(va));

    void* cpu = nullptr;
    HRESULT hr = env.core.pfnMapHeap(device.h(), upload.hheap(), &cpu);
    checkf(hr == S_OK && cpu, "copy: MapHeap of the UPLOAD heap (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK || !cpu) return;
    auto* words = static_cast<UINT32*>(cpu);
    for (UINT i = 0; i < kWords; ++i) words[i] = 0x9e3779b9u * (i + 1);
    env.core.pfnUnmapHeap(device.h(), upload.hheap());

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "copy: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    // An empty submission first: its signal (value 1) is below the copy's (value 2), which lets the harness hold
    // the copy's retirement back deterministically below.
    hr = engine_ddi::execute_command_lists(queue, 0, nullptr);
    check(hr == S_OK, "copy: execute_command_lists with no list signals the retirement fence");

    Recording rec;
    hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
    checkf(hr == S_OK && rec.table == 1, "copy: pool, recorder and DIRECT list, bound to the graphics table (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr == S_OK)
        check(engine_ddi::command_list_shell(rec.hlist()) == &device.shell,
              "copy: command_list_shell names the shell of the list's device");
    if (hr == S_OK) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_dest =
            transition(gpu, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_dest);
        dst.BaseAddress.UMD = {gpu.hres(), 0};
        src.BaseAddress.UMD = {upload.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), dst, src, kBytes);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(gpu, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
        dst.BaseAddress.UMD = {readback.hres(), 0};
        src.BaseAddress.UMD = {gpu.hres(), 0};
        t.pfnCopyBufferRegion(rec.hlist(), dst, src, kBytes);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors,
               "copy: barrier, two CopyBufferRegion, Close and execute_command_lists (hr %08lx)",
               static_cast<unsigned long>(hr));

        // Hold retirement back (the fence looks stuck at the empty submission's 1), destroy the upload buffer
        // while its copy is unretired: ownership must stay in the release sequence.
        engine_ddi::harness_force_completed(queue, 1);
        destroy_buffer(env, device, upload);
        const uint32_t pending = engine_ddi::harness_pending_releases(device.context);
        checkf(pending == 1 && observed.events.empty(),
               "copy: destroying the upload buffer before retirement defers its release (%u pending, %zu run)",
               pending, observed.events.size());
        engine_ddi::harness_force_completed(queue, 0);

        wait_queue_idle(env, queue, "copy");
        checkf(engine_ddi::completed_value(queue) >= 2, "copy: the queue's retirement fence reached the copy's signal (%llu)",
               static_cast<unsigned long long>(engine_ddi::completed_value(queue)));

        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu);
        checkf(hr == S_OK && cpu, "copy: MapHeap of the READBACK heap (hr %08lx)", static_cast<unsigned long>(hr));
        if (hr == S_OK && cpu) {
            words = static_cast<UINT32*>(cpu);
            UINT bad = 0;
            for (UINT i = 0; i < kWords; ++i) bad += words[i] != 0x9e3779b9u * (i + 1) ? 1u : 0u;
            checkf(bad == 0, "copy: readback matches the upload pattern word for word (%u of %u differ)", bad, kWords);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
    }
    destroy_recording(env, device, rec);

    // Any DDI call into the device is a retirement point; DestroyHeapAndResource of the next buffer runs the
    // deferred release on this thread.
    destroy_buffer(env, device, gpu);
    destroy_buffer(env, device, readback);
    const DWORD self = GetCurrentThreadId();
    size_t deferred = 0, immediate = 0, wrong_thread = 0;
    for (const engine_ddi::ReleaseEvent& e : observed.events) {
        (e.deferred ? deferred : immediate) += 1;
        wrong_thread += e.thread != self ? 1 : 0;
    }
    checkf(deferred == 1 && immediate == 2 && !wrong_thread && !engine_ddi::harness_pending_releases(device.context),
           "copy: the deferred release ran at a later DDI call, the other two at their destroy, all on the DDI "
           "thread (%zu deferred, %zu immediate, %zu elsewhere)",
           deferred, immediate, wrong_thread);
    const engine_ddi::QueueClose closed = engine_ddi::destroy_engine_queue(queue);
    engine_ddi::harness_set_release_observer(device.context, nullptr, nullptr);
    checkf(closed == engine_ddi::QueueClose::Retired && !engine_ddi::harness_live_objects(device.context) &&
               !engine_ddi::harness_retirement_lost(device.context),
           "copy: destroy_engine_queue reports Retired, no live object left, retirement never lost (%u live)",
           engine_ddi::harness_live_objects(device.context));
}

} // namespace harness
