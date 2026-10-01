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
        // Whole-resource copy up, region copy down: both buffers have the same size.
        t.pfnResourceCopy(rec.hlist(), gpu.hres(), upload.hres());
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
               "copy: barrier, ResourceCopy, CopyBufferRegion, Close and execute_command_lists (hr %08lx)",
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

// Pitched placements of several slices whose slice pitch is not the row pitch times the rows (272: The Ascent's
// first 3D upload). The engine's footprint cannot state such a pitch, so the slot copies one slice at a time:
// up from an UPLOAD placement with padded slices into a 3D texture (no box), down into a READBACK placement with
// another padding (no box: the whole subresource, sized from the shell's record), and slices 1-2 again with a box
// into a second placement. Every texel must come back where its slice pitch puts it, the padding untouched.
namespace {
constexpr UINT kSliceW = 8, kSliceH = 4, kSliceD = 3, kRowPitch = 256;
constexpr UINT kUpPitch = 6 * kRowPitch, kDownPitch = 5 * kRowPitch;   // derived would be 4 rows
constexpr UINT64 kBoxOffset = 4096;
constexpr UINT32 texel(UINT x, UINT y, UINT z) { return 0xC0000000u | (z << 16) | (y << 8) | x; }
constexpr UINT32 kPad = 0xDEADBEEFu;

HRESULT create_volume(Env& env, Device& device, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE3D;
    res.Width = kSliceW;
    res.Height = kSliceH;
    res.DepthOrArraySize = kSliceD;
    res.MipLevels = 1;
    res.Format = DXGI_FORMAT_R32_UINT;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    if (!info.ResourceDataSize) return E_FAIL;
    const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = info.ResourceDataSize;
    heap.Alignment = info.ResourceDataAlignment;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), &heap, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.heap = env.storage.alloc(sizes.Heap);
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.heap || !out.resource) return E_OUTOFMEMORY;
    return env.core.pfnCreateHeapAndResource(device.h(), &heap, out.hheap(), D3D12DDI_HRTRESOURCE{&out.rt}, &res, nullptr,
                                             D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
}

D3D12DDIARG_BUFFER_PLACEMENT placement(const Buffer& b, UINT64 offset) {
    D3D12DDIARG_BUFFER_PLACEMENT p{};
    p.BaseAddress.UMD = {b.hres(), offset};
    return p;
}

// Mismatches of `slices` slices starting at texture slice `first`, read at `pitch` bytes per slice.
UINT slice_mismatches(const UINT32* words, UINT pitch, UINT first, UINT slices) {
    UINT bad = 0;
    for (UINT z = 0; z < slices; ++z)
        for (UINT y = 0; y < kSliceH; ++y)
            for (UINT x = 0; x < kSliceW; ++x)
                bad += words[(z * pitch + y * kRowPitch) / 4 + x] != texel(x, y, first + z) ? 1u : 0u;
    return bad;
}
} // namespace

void test_copy_slices(Env& env, Device& device) {
    constexpr UINT64 kUpBytes = UINT64{kUpPitch} * kSliceD, kDownBytes = 8192;
    Buffer upload, readback, volume;
    const HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, kUpBytes, false, upload);
    const HRESULT hr_r = create_buffer(env, device, HeapKind::Readback, kDownBytes, false, readback);
    const HRESULT hr_v = create_volume(env, device, volume);
    checkf(hr_u == S_OK && hr_r == S_OK && hr_v == S_OK,
           "copy slices: UPLOAD and READBACK buffers and an R32_UINT %ux%ux%u volume (hr %08lx %08lx %08lx)", kSliceW,
           kSliceH, kSliceD, static_cast<unsigned long>(hr_u), static_cast<unsigned long>(hr_r),
           static_cast<unsigned long>(hr_v));
    engine_ddi::EngineQueue* queue = nullptr;
    Recording rec;
    if (hr_u == S_OK && hr_r == S_OK && hr_v == S_OK) {
        void* cpu = nullptr;
        HRESULT hr = env.core.pfnMapHeap(device.h(), upload.hheap(), &cpu);
        checkf(hr == S_OK && cpu, "copy slices: MapHeap of the UPLOAD heap (hr %08lx)", static_cast<unsigned long>(hr));
        if (hr == S_OK && cpu) {
            auto* words = static_cast<UINT32*>(cpu);
            for (UINT i = 0; i < kUpBytes / 4; ++i) words[i] = kPad;
            for (UINT z = 0; z < kSliceD; ++z)
                for (UINT y = 0; y < kSliceH; ++y)
                    for (UINT x = 0; x < kSliceW; ++x) words[(z * kUpPitch + y * kRowPitch) / 4 + x] = texel(x, y, z);
            env.core.pfnUnmapHeap(device.h(), upload.hheap());
        }
        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu);
        if (hr == S_OK && cpu) {
            auto* words = static_cast<UINT32*>(cpu);
            for (UINT i = 0; i < kDownBytes / 4; ++i) words[i] = kPad;
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
        BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
        hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
        checkf(hr == S_OK && queue, "copy slices: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));
        if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
        if (hr == S_OK) {
            const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
            const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT up{DXGI_FORMAT_R32_UINT, kSliceW, kSliceH, kSliceD,
                                                                     kRowPitch, kUpPitch};
            const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT down{DXGI_FORMAT_R32_UINT, kSliceW, kSliceH, kSliceD,
                                                                       kRowPitch, kDownPitch};
            const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT down2{DXGI_FORMAT_R32_UINT, kSliceW, kSliceH, 2,
                                                                        kRowPitch, kDownPitch};
            const D3D12DDIARG_PLACED_RESOURCE subresource{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
            const D3D12DDIARG_BUFFER_PLACEMENT tex = placement(volume, 0), from = placement(upload, 0),
                                               out = placement(readback, 0), out2 = placement(readback, kBoxOffset);
            const D3D12DDIARG_RESOURCE_BARRIER_0022 to_dest =
                transition(volume, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
            t.pfnResourceBarrier(rec.hlist(), 1, &to_dest);
            t.pfnCopyTextureRegion(rec.hlist(), &tex, subresource, 0, 0, 0, &from,
                                   {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &up}, nullptr);
            const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
                transition(volume, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
            t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
            t.pfnCopyTextureRegion(rec.hlist(), &out, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &down}, 0, 0, 0,
                                   &tex, subresource, nullptr);
            const D3D12DDI_BOX slices12{0, 0, 1, static_cast<LONG>(kSliceW), static_cast<LONG>(kSliceH), 3};
            t.pfnCopyTextureRegion(rec.hlist(), &out2, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &down2}, 0, 0,
                                   0, &tex, subresource, &slices12);
            t.pfnCloseCommandList(rec.hlist());
            const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
            hr = engine_ddi::execute_command_lists(queue, 1, lists);
            checkf(hr == S_OK && !device.shell.list_errors && !device.shell.device_errors,
                   "copy slices: three slice-pitched CopyTextureRegion calls recorded and executed (hr %08lx, %u list "
                   "errors, %u device errors)",
                   static_cast<unsigned long>(hr), device.shell.list_errors, device.shell.device_errors);
            if (hr == S_OK && wait_queue_idle(env, queue, "copy slices") &&
                env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) == S_OK && cpu) {
                const auto* words = static_cast<const UINT32*>(cpu);
                const UINT bad_whole = slice_mismatches(words, kDownPitch, 0, kSliceD);
                const UINT bad_box = slice_mismatches(words + kBoxOffset / 4, kDownPitch, 1, 2);
                UINT pad_hit = 0;   // the rows between the slices of the whole copy stay as written
                for (UINT z = 0; z < kSliceD; ++z)
                    for (UINT i = kSliceH * kRowPitch; i < kDownPitch; i += 4)
                        pad_hit += words[(z * kDownPitch + i) / 4] != kPad ? 1u : 0u;
                checkf(bad_whole == 0 && bad_box == 0 && pad_hit == 0,
                       "copy slices: up (pitch %u) and down (pitch %u) every texel in place, slices 1-2 by box too, "
                       "padding untouched (%u, %u and %u words differ)",
                       kUpPitch, kDownPitch, bad_whole, bad_box, pad_hit);
                env.core.pfnUnmapHeap(device.h(), readback.hheap());
            }
        }
    }
    destroy_recording(env, device, rec);
    if (queue) engine_ddi::destroy_engine_queue(queue);
    destroy_buffer(env, device, volume);
    destroy_buffer(env, device, readback);
    destroy_buffer(env, device, upload);
}

} // namespace harness
