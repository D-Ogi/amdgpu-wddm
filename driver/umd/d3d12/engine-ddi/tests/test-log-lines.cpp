// SPDX-License-Identifier: MIT
// The once-only diagnostic lines of engine-ddi (log_refusal, so that a game run's debugger log has them), on a device
// of their own: each is written for the first case of its kind and not again, and says what that case was. The lines
// are written once a process, so they are counted over the whole run (refusal_lines).
#include "harness.h"
#include <cstdio>
#include <string>
#include <vector>

namespace harness {

namespace {
D3D12DDIARG_BUFFER_PLACEMENT at(const Buffer& b, UINT64 offset) {
    D3D12DDIARG_BUFFER_PLACEMENT p{};
    p.BaseAddress.UMD = {b.hres(), offset};
    return p;
}

// A committed 2D texture of one mip in a DEFAULT heap, created in the COMMON layout. Its flags decide the heap's
// category, as the runtime's do.
HRESULT create_texture(Env& env, Device& device, DXGI_FORMAT format, UINT width, UINT height,
                       D3D12DDI_RESOURCE_FLAGS_0003 flags, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = width;
    res.Height = height;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = format;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    res.Flags = flags;
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
    heap.Flags = (flags & (D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET | D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL))
                     ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES
                     : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
    heap.CreationNodeMask = 1;
    heap.VisibleNodeMask = 1;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), &heap, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.heap = env.storage.alloc(sizes.Heap);
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.heap || !out.resource) return E_OUTOFMEMORY;
    return env.core.pfnCreateHeapAndResource(device.h(), &heap, out.hheap(), D3D12DDI_HRTRESOURCE{&out.rt}, &res,
                                             nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
}

// The first virtual placement: two copies from an UPLOAD buffer through virtual layouts into an R32_UINT texture, run
// on an engine queue. Only the first is logged, with its virtual size (the footprint's) and its physical size, which
// the engine never sees; the second, at another offset and with another box, adds nothing.
void virtual_placement(Env& env, Device& device) {
    Buffer upload, texture;
    const HRESULT hr_u = create_buffer(env, device, HeapKind::Upload, 4096, false, upload);
    const HRESULT hr_t =
        create_texture(env, device, DXGI_FORMAT_R32_UINT, 8, 4, D3D12DDI_RESOURCE_FLAG_0003_NONE, texture);
    checkf(hr_u == S_OK && hr_t == S_OK, "log lines: UPLOAD buffer and an R32_UINT 8x4 texture (hr %08lx %08lx)",
           static_cast<unsigned long>(hr_u), static_cast<unsigned long>(hr_t));
    engine_ddi::EngineQueue* queue = nullptr;
    Recording rec;
    HRESULT hr = hr_u == S_OK && hr_t == S_OK ? S_OK : E_FAIL;
    if (hr == S_OK) {
        BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
        hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    }
    if (hr == S_OK) hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
    if (hr == S_OK) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
        const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT first{DXGI_FORMAT_R32_UINT, 7, 3, 1, 8, 4, 1, 256, 1024};
        const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT second{DXGI_FORMAT_R32_UINT, 4, 2, 1, 4, 2, 1, 256, 512};
        const D3D12DDIARG_PLACED_RESOURCE subresource{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
        const D3D12DDIARG_BUFFER_PLACEMENT tex = at(texture, 0), from = at(upload, 0), from2 = at(upload, 1024);
        const D3D12DDI_BOX box{0, 0, 0, 7, 3, 1}, box2{0, 0, 0, 4, 2, 1};
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_dest =
            transition(texture, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_dest);
        t.pfnCopyTextureRegion(rec.hlist(), &tex, subresource, 0, 0, 0, &from,
                               {D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED, &first}, &box);
        t.pfnCopyTextureRegion(rec.hlist(), &tex, subresource, 1, 1, 0, &from2,
                               {D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED, &second}, &box2);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors && wait_queue_idle(env, queue, "log lines"),
               "log lines: two copies through virtual placements recorded and run (hr %08lx, %u list errors)",
               static_cast<unsigned long>(hr), device.shell.list_errors);
    }
    const std::vector<std::string> lines = refusal_lines("CopyTextureRegion: first virtual placement");
    const char* want = "CopyTextureRegion: first virtual placement, source: format 42, virtual 7 x 3 x 1, physical 8 x "
                       "4 x 1, pitch 256, slice pitch 1024, offset 0, box (0, 0, 0) to (7, 3, 1)";
    checkf(lines.size() == 1 && lines[0] == want, "log lines: one line for the first virtual placement (%zu): \"%s\"",
           lines.size(), lines.empty() ? "" : lines[0].c_str());
    destroy_recording(env, device, rec);
    if (queue) engine_ddi::destroy_engine_queue(queue);
    destroy_buffer(env, device, texture);
    destroy_buffer(env, device, upload);
}
} // namespace

void test_log_lines(Env& env) {
    Device device;
    HRESULT hr = open_device(env, device);
    checkf(hr == S_OK && device.context, "log lines: device context (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    virtual_placement(env, device);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && !device.shell.device_errors && !device.shell.list_errors,
           "log lines: destroy_device_context S_OK with no live object, no error reported (hr %08lx, %u live, %u "
           "device, %u list errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors, device.shell.list_errors);
}

} // namespace harness
