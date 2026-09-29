// SPDX-License-Identifier: MIT
// Round trip 7: the linear primary (boundary r4, engine ABI 1.3 V13), RuntimeBacked on the stub shell. A committed
// texture on a heap with D3D12DDI_HEAP_FLAG_PRIMARY gets a linear image at offset 0 of memory the shell was asked
// for with the image's row pitch, size and memory types; it is cleared through a render target view, copied to a
// READBACK buffer and compared texel by texel.
#include "harness.h"

#include <algorithm>
#include <cstring>

namespace harness {
namespace {

struct Shape {
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT16 mips = 1;
    bool primary = true;
    UINT64 heap_bytes = 0;                      // 0: what CheckResourceAllocationInfo answers
    bool heap_alignment = false;                // the heap takes the answer's alignment as well
    bool every_category = false;                // the heap allows buffers and both kinds of texture
};

D3D12DDIARG_CREATERESOURCE_0088 description(const Shape& s) {
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = s.width;
    res.Height = s.height;
    res.DepthOrArraySize = 1;
    res.MipLevels = s.mips;
    res.Format = s.format;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    res.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    return res;
}

D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 allocation_info(Env& env, Device& device, const Shape& s) {
    const D3D12DDIARG_CREATERESOURCE_0088 res = description(s);
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res,
                                            s.primary ? D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_PRIMARY
                                                      : D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE,
                                            0, 1, &info);
    return info;
}

HRESULT create_target(Env& env, Device& device, const Shape& s, Buffer& out) {
    out = Buffer{};
    const D3D12DDIARG_CREATERESOURCE_0088 res = description(s);
    const D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info = allocation_info(env, device, s);
    if (!info.ResourceDataSize) return E_FAIL;
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = s.heap_bytes ? s.heap_bytes : info.ResourceDataSize;
    heap.Alignment = s.heap_alignment ? info.ResourceDataAlignment : 0;
    heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    if (s.every_category) heap.Flags |= D3D12DDI_HEAP_FLAG_BUFFERS | D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
    if (s.primary) heap.Flags |= D3D12DDI_HEAP_FLAG_PRIMARY;
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

constexpr UINT align_up(UINT value, UINT to) { return (value + to - 1) / to * to; }

// What the engine itself answers for the linear image of this shape; 0 when it has none.
UINT64 engine_alignment(Device& device, const Shape& s) {
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = s.width;
    d.Height = s.height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = s.format;
    d.SampleDesc = {1, 0};
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    BC250_VKD3D_LINEAR_IMAGE_INFO info{};
    info.Size = sizeof(info);
    if (FAILED(device.context->funcs.QueryLinearImage(device.context->device, &d, &info))) return 0;
    return info.MemoryAlignment;
}

// Creates the primary, clears it, reads it back. Returns false when a later case cannot run.
bool round_trip(Env& env, Device& device, StubMemory& m, engine_ddi::EngineQueue* queue, const Shape& s,
                const FLOAT colour[4], UINT32 expected) {
    const D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info = allocation_info(env, device, s);
    Shape plain = s;
    plain.primary = false;
    const D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 tiled = allocation_info(env, device, plain);
    checkf(info.ResourceDataSize && !(info.ResourceDataSize % 4096) && info.ResourceDataAlignment >= 65536 &&
               tiled.ResourceDataSize,
           "linear primary %ux%u: CheckResourceAllocationInfo with the PRIMARY optimization flag answers %llu bytes "
           "aligned to %u (without the flag: %llu bytes aligned to %u)",
           s.width, s.height, static_cast<unsigned long long>(info.ResourceDataSize), info.ResourceDataAlignment,
           static_cast<unsigned long long>(tiled.ResourceDataSize), tiled.ResourceDataAlignment);

    Buffer target, readback, beside;
    const uint32_t allocations = m.allocations;
    const uint32_t init_before = engine_ddi::harness_pending_initializations(device.context);
    HRESULT hr = create_target(env, device, s, target);
    const bool asked = m.allocations == allocations + 1;
    const uint32_t want = engine_ddi::kMemoryDedicated | engine_ddi::kMemoryPrimary | engine_ddi::kMemoryLinearSurface;
    const UINT64 rows = UINT64{m.last_row_pitch} * align_up(s.height, 4);
    const UINT64 queried = engine_alignment(device, s);
    checkf(hr == S_OK && asked && m.last_flags == want && m.last_type_bits && m.last_alignment &&
               m.last_byte_size == info.ResourceDataSize && queried && m.last_alignment == queried &&
               m.last_row_pitch >= align_up(s.width, 4) * 4 &&
               !(m.last_row_pitch % 16) && m.last_layout_size && rows <= m.last_byte_size &&
               m.last_layout_size <= m.last_byte_size,
           "linear primary %ux%u: one memory request with the linear surface (hr %08lx, flags %x, types %08x, "
           "alignment %llu, %llu bytes, row pitch %u, layout %llu bytes)",
           s.width, s.height, static_cast<unsigned long>(hr), m.last_flags, m.last_type_bits,
           static_cast<unsigned long long>(m.last_alignment), static_cast<unsigned long long>(m.last_byte_size),
           m.last_row_pitch, static_cast<unsigned long long>(m.last_layout_size));
    if (hr != S_OK) return false;
    checkf(engine_ddi::harness_pending_initializations(device.context) == init_before + 1,
           "linear primary %ux%u: queued for its initialization", s.width, s.height);

    D3DKMT_HANDLE presented = 0;
    hr = engine_ddi::present_allocation(device.context, target.hres(), &presented);
    checkf(hr == S_OK && presented, "linear primary %ux%u: present_allocation gives its allocation (hr %08lx)",
           s.width, s.height, static_cast<unsigned long>(hr));
    hr = create_placed_buffer(env, device, target, 0, 4096, beside);
    checkf(hr == E_INVALIDARG, "linear primary %ux%u: a buffer placed on the primary's memory: E_INVALIDARG (hr %08lx)",
           s.width, s.height, static_cast<unsigned long>(hr));

    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 existing{};
    env.core.pfnCheckExistingResourceAllocationInfo(device.h(), target.hres(), &existing);
    checkf(existing.ResourceDataSize == m.last_byte_size &&
               existing.ResourceDataAlignment == info.ResourceDataAlignment,
           "linear primary %ux%u: CheckExistingResourceAllocationInfo answers the same (%llu bytes)", s.width, s.height,
           static_cast<unsigned long long>(existing.ResourceDataSize));

    const UINT pitch = align_up(s.width * 4, 256);
    hr = create_buffer(env, device, HeapKind::Readback, UINT64{pitch} * s.height, false, readback);
    D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                      D3D12DDI_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    void* heap_storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &heap_args));
    const D3D12DDI_HDESCRIPTORHEAP hheap{heap_storage};
    const HRESULT hr_h = heap_storage ? env.core.pfnCreateDescriptorHeap(device.h(), &heap_args, hheap) : E_OUTOFMEMORY;
    D3D12DDI_CPU_DESCRIPTOR_HANDLE rtv{};
    if (hr_h == S_OK) rtv = env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), hheap);
    const uint32_t errors = device.shell.device_errors;
    if (rtv.ptr) {
        D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view{};
        view.hDrvResource = target.hres();
        view.Format = s.format;
        view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
        view.Tex2D = {0, 0, 1, 0};
        env.core.pfnCreateRenderTargetView(device.h(), &view, rtv);
    }
    const bool ready = hr == S_OK && hr_h == S_OK && rtv.ptr && device.shell.device_errors == errors;
    checkf(ready, "linear primary %ux%u: READBACK buffer, RTV heap and view (hr %08lx %08lx)", s.width, s.height,
           static_cast<unsigned long>(hr), static_cast<unsigned long>(hr_h));

    Recording rec;
    const HRESULT hr_rec = ready ? open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec) : E_ABORT;
    if (hr_rec == S_OK) {
        const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_target =
            transition(target, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_RENDER_TARGET);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_target);
        t.pfnClearRenderTargetView(rec.hlist(), rtv, colour, 0, nullptr);
        const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
            transition(target, D3D12DDI_RESOURCE_STATE_RENDER_TARGET, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
        const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{s.format, s.width, s.height, 1, pitch,
                                                                        pitch * s.height};
        D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
        dst.BaseAddress.UMD = {readback.hres(), 0};
        src.BaseAddress.UMD = {target.hres(), 0};
        t.pfnCopyTextureRegion(rec.hlist(), &dst, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint}, 0, 0, 0,
                               &src, {D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr}, nullptr);
        t.pfnCloseCommandList(rec.hlist());
        const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
        hr = engine_ddi::execute_command_lists(queue, 1, lists);
        checkf(hr == S_OK && !device.shell.list_errors, "linear primary %ux%u: clear and copy executed (hr %08lx)",
               s.width, s.height, static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "linear primary");
        void* cpu = nullptr;
        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu);
        if (hr == S_OK && cpu) {
            UINT bad = 0;
            for (UINT y = 0; y < s.height; ++y) {
                const BYTE* row = static_cast<const BYTE*>(cpu) + UINT64{pitch} * y;
                for (UINT x = 0; x < s.width; ++x) {
                    UINT32 texel = 0;
                    std::memcpy(&texel, row + 4 * x, sizeof(texel));
                    bad += texel != expected ? 1u : 0u;
                }
            }
            checkf(bad == 0, "linear primary %ux%u: every texel reads %08x (%u of %u differ)", s.width, s.height,
                   expected, bad, s.width * s.height);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            checkf(false, "linear primary %ux%u: MapHeap of the READBACK heap (hr %08lx)", s.width, s.height,
                   static_cast<unsigned long>(hr));
        }
    } else {
        checkf(!ready, "linear primary %ux%u: recording (hr %08lx)", s.width, s.height,
               static_cast<unsigned long>(hr_rec));
    }
    destroy_recording(env, device, rec);
    if (hr_h == S_OK) env.core.pfnDestroyDescriptorHeap(device.h(), hheap);
    if (readback.resource) destroy_buffer(env, device, readback);
    destroy_buffer(env, device, target);
    return true;
}

} // namespace

void test_linear_primary(Env& env) {
    StubMemory m;
    check(load_stub(env, m), "linear primary: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free);
    checkf(hr == S_OK && device.context, "linear primary: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "linear primary: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;

    const FLOAT first[4] = {0.2f, 0.4f, 0.6f, 1.0f};
    const FLOAT second[4] = {0.6f, 0.4f, 0.2f, 1.0f};
    // B8G8R8A8 in memory is blue first, R8G8B8A8 red first: the same word for both clears below.
    round_trip(env, device, m, queue, Shape{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM}, first, 0xff336699u);
    round_trip(env, device, m, queue, Shape{127, 79, DXGI_FORMAT_R8G8B8A8_UNORM}, second, 0xff336699u);
    // The runtime's heap may be larger than the surface, carry the answer's alignment and allow every
    // category: the memory request and the engine's heap are the surface's all the same.
    {
        Shape wide{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        wide.heap_bytes = 1024 * 1024;
        wide.heap_alignment = true;
        wide.every_category = true;
        Shape exact = wide;
        exact.heap_bytes = 0;
        const UINT64 surface_bytes = allocation_info(env, device, exact).ResourceDataSize;
        Buffer roomy;
        hr = create_target(env, device, wide, roomy);
        checkf(hr == S_OK && surface_bytes && m.last_byte_size == surface_bytes,
               "linear primary: a heap of 1 MiB for every category with the answer's alignment: created, %llu bytes "
               "asked of the shell "
               "(hr %08lx)",
               static_cast<unsigned long long>(m.last_byte_size), static_cast<unsigned long>(hr));
        if (hr == S_OK) destroy_buffer(env, device, roomy);
    }

    // Only the PRIMARY heap flag selects the linear image, and only for a description it exists for.
    {
        Buffer plain, mipped, tight;
        const uint32_t before = m.allocations;
        hr = create_target(env, device, Shape{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM, 1, false}, plain);
        const uint32_t plain_flags = m.last_flags;
        checkf(hr == S_OK && m.allocations == before + 1 && plain_flags == engine_ddi::kMemoryDedicated &&
                   !m.last_row_pitch && !m.last_layout_size,
               "linear primary: the same description without the PRIMARY flag asks for ordinary memory (hr %08lx, "
               "flags %x)",
               static_cast<unsigned long>(hr), plain_flags);
        if (hr == S_OK) {
            D3DKMT_HANDLE none = 1;
            const HRESULT refused = engine_ddi::present_allocation(device.context, plain.hres(), &none);
            checkf(refused == E_INVALIDARG && !none,
                   "linear primary: present_allocation refuses the texture that is not a primary (hr %08lx)",
                   static_cast<unsigned long>(refused));
            destroy_buffer(env, device, plain);
        }

        hr = create_target(env, device, Shape{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM, 2}, mipped);
        checkf(m.last_flags == (engine_ddi::kMemoryDedicated | engine_ddi::kMemoryPrimary) && !m.last_row_pitch,
               "linear primary: a PRIMARY texture of 2 mips is asked for as a primary without a linear surface "
               "(hr %08lx, flags %x)",
               static_cast<unsigned long>(hr), m.last_flags);
        if (hr == S_OK) destroy_buffer(env, device, mipped);

        const uint32_t requests = m.allocations;
        Shape cramped{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        cramped.heap_bytes = 4096;
        hr = create_target(env, device, cramped, tight);
        checkf(hr == E_INVALIDARG && m.allocations == requests,
               "linear primary: a heap of 4096 bytes for the 256x256 primary: E_INVALIDARG, no memory request "
               "(hr %08lx)",
               static_cast<unsigned long>(hr));
    }

    checkf(m.frees == m.allocations, "linear primary: each allocation came back through free_memory (%u of %u)",
           m.frees, m.allocations);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "linear primary: destroy_engine_queue reports Retired");
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && !device.shell.device_errors && !device.shell.list_errors,
           "linear primary: destroy_device_context S_OK with no live object, no error reported (hr %08lx, %u live, "
           "%u device errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors);
}

} // namespace harness
