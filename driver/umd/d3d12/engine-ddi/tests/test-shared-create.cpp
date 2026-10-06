// SPDX-License-Identifier: MIT
// Round trip 8, BD-075: the shared surface's create (boundary r5, engine ABI 1.3 V13), RuntimeBacked on the stub
// shell. Nothing in the D3D12 DDI says that a resource is shared, so the create cannot be steered by the
// description: it asks for the ordinary allocation shape with kMemoryShareable set when the description is inside
// the shareable envelope, and only the shell's kShareRequired - the runtime's refusal, in the lab - makes it ask the
// engine for a linear image and try again. This test plays that refusal with StubMemory::refuse_shareable and
// checks what each attempt asked for, that the surface the second attempt produced renders and reads back texel for
// texel, and that every description outside the envelope still asks once for ordinary memory.
#include "harness.h"

#include "../../../../contract/bc250_shared_surface.h"

#include <algorithm>
#include <cstring>

namespace harness {
namespace {

struct Shape {
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT16 mips = 1;
    UINT samples = 1;
    UINT flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET | D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE;
    D3D12DDI_CPU_PAGE_PROPERTY cpu = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    D3D12DDI_MEMORY_POOL pool = D3D12DDI_MEMORY_POOL_L1;
    UINT16 slices = 1;
};

D3D12DDIARG_CREATERESOURCE_0088 description(const Shape& s) {
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = s.width;
    res.Height = s.height;
    res.DepthOrArraySize = s.slices;
    res.MipLevels = s.mips;
    res.Format = s.format;
    res.SampleDesc = {s.samples, 0};
    res.Layout = D3D12DDI_TL_UNDEFINED;
    res.Flags = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(s.flags);
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
    return res;
}

// An ordinary committed texture: the runtime sizes its heap from the tiled answer, because it has no idea that this
// resource is about to be shared either.
HRESULT create_shared(Env& env, Device& device, const Shape& s, Buffer& out) {
    out = Buffer{};
    const D3D12DDIARG_CREATERESOURCE_0088 res = description(s);
    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 info{};
    env.core.pfnCheckResourceAllocationInfo(device.h(), &res, D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_NONE, 0, 1, &info);
    if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return E_FAIL;
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = info.ResourceDataSize;
    heap.Alignment = 0;
    heap.CPUPageProperty = s.cpu;
    heap.MemoryPool = s.pool;
    heap.Flags = (s.flags & (D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET | D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL))
                     ? D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES
                     : D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
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

UINT texel_bytes(DXGI_FORMAT format) {
    const AMDGPU_WDDM_SURFACE_FORMAT* row = Bc250SharedSurfaceFormat(static_cast<unsigned long>(format));
    return row ? row->bytes_per_pixel : 0;
}

// Clears the shared surface through a render target view, copies it into a READBACK buffer and compares every texel
// with the word the clear writes to memory. The surface is linear, so this also says that the engine placed the
// image the record's pitch describes.
void round_trip(Env& env, Device& device, engine_ddi::EngineQueue* queue, const Shape& s, const Buffer& target,
                const FLOAT colour[4], UINT64 expected) {
    const UINT bpp = texel_bytes(s.format);
    const UINT pitch = align_up(s.width * bpp, 256);
    Buffer readback;
    HRESULT hr = create_buffer(env, device, HeapKind::Readback, UINT64{pitch} * s.height, false, readback);
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
    checkf(ready, "shared create %ux%u: READBACK buffer, RTV heap and view (hr %08lx %08lx)", s.width, s.height,
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
        checkf(hr == S_OK && !device.shell.list_errors, "shared create %ux%u: clear and copy executed (hr %08lx)",
               s.width, s.height, static_cast<unsigned long>(hr));
        wait_queue_idle(env, queue, "shared create");
        void* cpu = nullptr;
        hr = env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu);
        if (hr == S_OK && cpu) {
            UINT bad = 0;
            for (UINT y = 0; y < s.height; ++y) {
                const BYTE* row = static_cast<const BYTE*>(cpu) + UINT64{pitch} * y;
                for (UINT x = 0; x < s.width; ++x) {
                    UINT64 texel = 0;
                    std::memcpy(&texel, row + UINT64{bpp} * x, bpp);
                    bad += texel != expected ? 1u : 0u;
                }
            }
            checkf(bad == 0, "shared create %ux%u: every texel reads %0*llx (%u of %u differ)", s.width, s.height,
                   static_cast<int>(bpp * 2), static_cast<unsigned long long>(expected), bad, s.width * s.height);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        } else {
            checkf(false, "shared create %ux%u: MapHeap of the READBACK heap (hr %08lx)", s.width, s.height,
                   static_cast<unsigned long>(hr));
        }
    }
    destroy_recording(env, device, rec);
    if (hr_h == S_OK) env.core.pfnDestroyDescriptorHeap(device.h(), hheap);
    if (readback.resource) destroy_buffer(env, device, readback);
}

// One description outside the shareable envelope: one request with ordinary flags, no retry, whether the create then
// succeeds or is refused for a reason of its own (a texture on a CPU-visible heap is refused by the shell, and the
// slot reports that as E_OUTOFMEMORY). What matters here is the shape of the request and the absence of a retry.
void outside(Env& env, Device& device, StubMemory& m, const Shape& s, const char* what) {
    Buffer plain;
    const uint32_t allocations = m.allocations, shared = m.share_required;
    const HRESULT hr = create_shared(env, device, s, plain);
    checkf((hr == S_OK || hr == E_OUTOFMEMORY) && m.allocations == allocations + 1 && m.share_required == shared &&
               m.last_flags == engine_ddi::kMemoryDedicated && !m.last_row_pitch && !m.last_layout_size,
           "shared create: %s asks once for ordinary memory and is never retried (hr %08lx, flags %x, retries %u)",
           what, static_cast<unsigned long>(hr), m.last_flags, m.share_required - shared);
    if (hr == S_OK) destroy_buffer(env, device, plain);
}

} // namespace

void test_shared_create(Env& env) {
    StubMemory m;
    check(load_stub(env, m), "shared create: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free, nullptr, stub_adopt);
    checkf(hr == S_OK && device.context, "shared create: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "shared create: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;

    // Before the refusal: the same description asks once, with the shareable flag, and gets its memory. The flag
    // alone changes nothing about the allocation - which is the point of attempt 1.
    {
        Buffer first;
        const uint32_t allocations = m.allocations;
        hr = create_shared(env, device, Shape{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM}, first);
        checkf(hr == S_OK && m.allocations == allocations + 1 && !m.share_required &&
                   m.last_flags == (engine_ddi::kMemoryDedicated | engine_ddi::kMemoryShareable) && !m.last_row_pitch &&
                   !m.last_layout_size,
               "shared create: a shell that admits the shareable request gets one ordinary allocation (hr %08lx, "
               "flags %x)",
               static_cast<unsigned long>(hr), m.last_flags);
        if (hr == S_OK) destroy_buffer(env, device, first);
    }

    // From here the stub plays the runtime's refusal.
    m.refuse_shareable = true;
    const UINT want_second =
        engine_ddi::kMemoryDedicated | engine_ddi::kMemoryShareable | engine_ddi::kMemoryLinearSurface;
    struct Case {
        Shape shape;
        FLOAT colour[4];
        UINT64 expected;
    };
    // B8G8R8A8 in memory is blue first, R8G8B8A8 red first: the same word for both clears.
    const Case cases[] = {
        {Shape{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM}, {0.2f, 0.4f, 0.6f, 1.0f}, 0xff336699u},
        {Shape{127, 79, DXGI_FORMAT_R8G8B8A8_UNORM}, {0.6f, 0.4f, 0.2f, 1.0f}, 0xff336699u},
        {Shape{200, 120, DXGI_FORMAT_R10G10B10A2_UNORM}, {0.2f, 0.4f, 0.6f, 1.0f}, 0xe66664cdu},
    };
    for (const Case& c : cases) {
        const Shape& s = c.shape;
        const UINT bpp = texel_bytes(s.format);
        Buffer target;
        const uint32_t allocations = m.allocations, shared = m.share_required;
        hr = create_shared(env, device, s, target);
        const UINT64 rows = UINT64{m.last_row_pitch} * align_up(s.height, 4);
        checkf(hr == S_OK && m.share_required == shared + 1 && m.allocations == allocations + 1 &&
                   m.last_flags == want_second && m.last_type_bits && m.last_row_pitch && !(m.last_row_pitch % 16) &&
                   m.last_row_pitch >= align_up(s.width, 4) * bpp && m.last_byte_size && !(m.last_byte_size % 4096) &&
                   rows <= m.last_byte_size && m.last_layout_size && m.last_layout_size <= m.last_byte_size &&
                   m.last_alignment,
               "shared create %ux%u: the refusal is retried once as a linear surface (hr %08lx, flags %x, pitch %u, "
               "%llu bytes, layout %llu, alignment %llu)",
               s.width, s.height, static_cast<unsigned long>(hr), m.last_flags, m.last_row_pitch,
               static_cast<unsigned long long>(m.last_byte_size), static_cast<unsigned long long>(m.last_layout_size),
               static_cast<unsigned long long>(m.last_alignment));
        if (hr != S_OK) continue;
        // The destroy needs the surface's allocation by name, exactly as a primary's destroy does: the shell
        // releases it by its runtime resource, inside that resource's own DDI.
        D3DKMT_HANDLE presented = 0;
        const HRESULT named = engine_ddi::present_allocation(device.context, target.hres(), &presented);
        checkf(named == S_OK && presented,
               "shared create %ux%u: the created surface names its allocation to its own destroy (hr %08lx)", s.width,
               s.height, static_cast<unsigned long>(named));
        // Nothing else lives on a linear surface's memory.
        Buffer beside;
        const HRESULT placed = create_placed_buffer(env, device, target, 0, 4096, beside);
        checkf(placed == E_OUTOFMEMORY,
               "shared create %ux%u: a buffer placed on the shared surface's memory is refused (hr %08lx)", s.width,
               s.height, static_cast<unsigned long>(placed));
        round_trip(env, device, queue, s, target, c.colour, c.expected);
        destroy_buffer(env, device, target);
    }

    // The envelope's edges, each asking once for ordinary memory: a mip chain, an array, MSAA, a format the surface
    // format table does not compose, a depth-stencil texture, a CPU-visible heap and the L0 pool.
    {
        Shape mipped{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        mipped.mips = 2;
        outside(env, device, m, mipped, "a mip chain");
        Shape array{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        array.slices = 2;
        outside(env, device, m, array, "an array");
        Shape msaa{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        msaa.samples = 4;
        outside(env, device, m, msaa, "a multisampled texture");
        outside(env, device, m, Shape{256, 256, DXGI_FORMAT_R32_UINT}, "an uncomposed format");
        Shape depth{256, 256, DXGI_FORMAT_D32_FLOAT};
        depth.flags = D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL;
        outside(env, device, m, depth, "a depth-stencil texture");
        Shape cpu{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        cpu.cpu = D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK;
        cpu.pool = D3D12DDI_MEMORY_POOL_L0;
        outside(env, device, m, cpu, "a CPU-visible heap");
        Shape system{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        system.pool = D3D12DDI_MEMORY_POOL_L0;
        outside(env, device, m, system, "the L0 pool");
    }

    // A heap alone never asks to share: there is no resource to describe and nothing to place in it yet.
    {
        Buffer heap_only;
        const uint32_t allocations = m.allocations, shared = m.share_required;
        hr = create_heap_alone(env, device, HeapKind::Default, 256 * 1024, heap_only);
        checkf(hr == S_OK && m.allocations == allocations + 1 && m.share_required == shared && !m.last_flags,
               "shared create: a heap without a resource asks for plain memory (hr %08lx, flags %x)",
               static_cast<unsigned long>(hr), m.last_flags);
        if (hr == S_OK) destroy_buffer(env, device, heap_only);
    }

    checkf(m.frees == m.allocations, "shared create: each allocation came back through free_memory (%u of %u)", m.frees,
           m.allocations);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "shared create: destroy_engine_queue reports Retired");
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && !device.shell.device_errors && !device.shell.list_errors,
           "shared create: destroy_device_context S_OK with no live object and no error (hr %08lx, %u live, %u device "
           "errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors);
}

} // namespace harness
