// SPDX-License-Identifier: MIT
// Round trip 7: the linear primary (boundary r4, engine ABI 1.3 V13), RuntimeBacked on the stub shell. A committed
// texture on a heap with D3D12DDI_HEAP_FLAG_PRIMARY gets a linear image at offset 0 of memory the shell was asked
// for with the image's row pitch, size and memory types; it is cleared through a render target view, copied to a
// READBACK buffer and compared texel by texel.
#include "harness.h"

#include "../../../../contract/amdgpu_wddm_surface_format.h"
#include "../../present-copy-sync.h"

#include <algorithm>
#include <cstring>
#include <thread>
#include <vector>

namespace harness {
namespace {
// BD-133: the shell's half of a copied present, as far as the engine half asks for it: the present fence as the CPU
// read it and the hook that gives a chosen surface its completion fence (present-copy-sync.h, PresentCopyPick). The
// harness has no kernel fence and takes no ticket, so a surface stays fresh and is released with its source
// (release_present_copy); prepared counts the calls.
HRESULT harness_prepare(void* state, native12::PresentCopySlot&) noexcept {
    ++*static_cast<unsigned*>(state);
    return S_OK;
}
} // namespace

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
    if (!info.ResourceDataSize || info.ResourceDataSize == UINT64_MAX) return E_FAIL;
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

struct Observed {
    std::vector<engine_ddi::ReleaseEvent> events;
};
void observe(void* user, const engine_ddi::ReleaseEvent* event) {
    static_cast<Observed*>(user)->events.push_back(*event);
}

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

// The bytes of one texel of a composed format, from the surface format table; 0 for a format it does not compose.
UINT texel_bytes(DXGI_FORMAT format) {
    const AMDGPU_WDDM_SURFACE_FORMAT* row = amdgpu_wddm_surface_admit(
        amdgpu_wddm_surface_format_by_dxgi(static_cast<unsigned>(format)), AMDGPU_WDDM_SURFACE_COMPOSED);
    return row ? row->bytes_per_pixel : 0;
}

// Creates the primary, clears it, reads it back. Returns false when a later case cannot run. expected is the texel
// as it lies in memory, read little endian: its low texel_bytes(format) bytes are compared.
bool round_trip(Env& env, Device& device, StubMemory& m, engine_ddi::EngineQueue* queue, const Shape& s,
                const FLOAT colour[4], UINT64 expected) {
    const UINT bpp = texel_bytes(s.format);
    checkf(bpp == 4 || bpp == 8, "linear primary %ux%u: format %d is a composed row of the surface format table (%u bytes)",
           s.width, s.height, static_cast<int>(s.format), bpp);
    if (bpp != 4 && bpp != 8) return false;
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
               m.last_row_pitch >= align_up(s.width, 4) * bpp &&
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
    // A refused create reports E_OUTOFMEMORY and keeps its real reason in the log (BD-075).
    checkf(hr == E_OUTOFMEMORY, "linear primary %ux%u: a buffer placed on the primary's memory is refused (hr %08lx)",
           s.width, s.height, static_cast<unsigned long>(hr));

    D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 existing{};
    env.core.pfnCheckExistingResourceAllocationInfo(device.h(), target.hres(), &existing);
    checkf(existing.ResourceDataSize == m.last_byte_size &&
               existing.ResourceDataAlignment == info.ResourceDataAlignment,
           "linear primary %ux%u: CheckExistingResourceAllocationInfo answers the same (%llu bytes)", s.width, s.height,
           static_cast<unsigned long long>(existing.ResourceDataSize));

    const UINT pitch = align_up(s.width * bpp, 256);
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
                    UINT64 texel = 0;
                    std::memcpy(&texel, row + UINT64{bpp} * x, bpp);
                    bad += texel != expected ? 1u : 0u;
                }
            }
            checkf(bad == 0, "linear primary %ux%u: every texel reads %0*llx (%u of %u differ)", s.width, s.height,
                   static_cast<int>(bpp * 2), static_cast<unsigned long long>(expected), bad, s.width * s.height);
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
    // A 10-bit swap chain's storage, four bytes as well: red in the low ten bits, alpha in the top two. 0.2, 0.4
    // and 0.6 of 1023 are 204.6, 409.2 and 613.8, so no rounding tie decides the word.
    round_trip(env, device, m, queue, Shape{200, 120, DXGI_FORMAT_R10G10B10A2_UNORM}, first, 0xe66664cdu);
    // An FP16 swap chain's storage, eight bytes a texel: red in the low half. Values outside [0, 1] (scRGB) must
    // survive, so the surface is a float one and nothing clamps; each is exact in FP16 (2.0 0x4000, -0.25 0xb400,
    // 0.5 0x3800, 1.0 0x3c00), so no rounding rule decides the word.
    const FLOAT scrgb[4] = {2.0f, -0.25f, 0.5f, 1.0f};
    round_trip(env, device, m, queue, Shape{136, 72, DXGI_FORMAT_R16G16B16A16_FLOAT}, scrgb, 0x3c003800b4004000ull);
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
        // Without the PRIMARY flag this description is an ordinary committed texture, and the allocation asked for
        // is the ordinary one. It is also inside the shareable envelope (BD-075), so the request says that the
        // runtime may yet refuse it as a shared resource's: kMemoryShareable changes nothing about the allocation
        // and is the only way the shell can tell that refusal from any other E_INVALIDARG.
        const uint32_t plain_want = engine_ddi::kMemoryDedicated | engine_ddi::kMemoryShareable;
        checkf(hr == S_OK && m.allocations == before + 1 && plain_flags == plain_want && !m.last_row_pitch &&
                   !m.last_layout_size,
               "linear primary: the same description without the PRIMARY flag asks for ordinary memory (hr %08lx, "
               "flags %x)",
               static_cast<unsigned long>(hr), plain_flags);
        if (hr == S_OK) {
            D3DKMT_HANDLE none = 1;
            const HRESULT refused = engine_ddi::present_allocation(device.context, plain.hres(), &none);
            checkf(refused == E_INVALIDARG && !none,
                   "linear primary: present_allocation refuses the texture that is not a primary (hr %08lx)",
                   static_cast<unsigned long>(refused));
            // BD-133: that same texture is still a Present source, by copying it. This is the D3D9On12 back
            // buffer's shape, and refusing it removed the device at the first windowed present. The copy goes on
            // the command list the runtime hands to pfnPresent, and the answer is the allocation of the linear
            // surface it wrote - a surface of the source's own kind, not the source.
            const D3DKMT_HANDLE plain_allocation = m.last_allocation;
            // BD-133: before its first copied present the source has no copy surface, and as a destination its
            // formats are its DXGI format and the composed row's LB7A format (A8R8G8B8, 21).
            {
                uint32_t lb7a = 1, dxgi = 0, row = 0;
                const HRESULT none_yet = engine_ddi::present_copy_surface_format(device.context, plain.hres(), &lb7a);
                const HRESULT as_destination =
                    engine_ddi::present_destination_format(device.context, plain.hres(), &dxgi, &row);
                checkf(none_yet == S_FALSE && !lb7a && as_destination == S_OK &&
                           dxgi == static_cast<uint32_t>(DXGI_FORMAT_B8G8R8A8_UNORM) && row == 21,
                       "BD-133: no copy surface before the first copied present (hr %08lx), destination formats %u "
                       "and LB7A %u (hr %08lx)",
                       static_cast<unsigned long>(none_yet), dxgi, row, static_cast<unsigned long>(as_destination));
            }
            Recording copy_rec;
            const HRESULT hr_copy_rec = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, copy_rec);
            if (hr_copy_rec == S_OK) {
                const uint64_t copies_before = engine_ddi::present_copies();
                const uint64_t surfaces_before = engine_ddi::present_copy_surfaces();
                const uint32_t requests_before = m.allocations;
                unsigned prepared = 0;
                const native12::PresentCopyPick pick{0, &prepared, harness_prepare};
                native12::PresentCopySlot* slot = nullptr;
                D3DKMT_HANDLE copied = 0;
                HRESULT hr_copy = engine_ddi::present_copy_allocation(device.context, copy_rec.hlist(), plain.hres(),
                                                                      pick, &slot, &copied);
                {
                    uint32_t lb7a = 0;
                    const HRESULT made = engine_ddi::present_copy_surface_format(device.context, plain.hres(), &lb7a);
                    checkf(made == S_OK && lb7a == 21,
                           "BD-133: the copy surface of a B8G8R8A8 source has the composed row's LB7A format 21 "
                           "(hr %08lx, LB7A %u)",
                           static_cast<unsigned long>(made), lb7a);
                }
                const uint32_t want_copy = engine_ddi::kMemoryDedicated | engine_ddi::kMemoryShareable |
                                           engine_ddi::kMemoryLinearSurface;
                checkf(hr_copy == S_OK && copied && copied != plain_allocation && slot && prepared == 1 &&
                           m.allocations == requests_before + 1 && m.last_flags == want_copy && m.last_row_pitch &&
                           m.last_layout_size && engine_ddi::present_copies() == copies_before + 1 &&
                           engine_ddi::present_copy_surfaces() == surfaces_before + 1,
                       "BD-133: present_copy_allocation copies the ordinary texture into one new linear surface "
                       "(hr %08lx, allocation %08x, flags %x, row pitch %u)",
                       static_cast<unsigned long>(hr_copy), copied, m.last_flags, m.last_row_pitch);
                // The next present of the same source reuses that surface while it is idle (no ticket taken here):
                // no second memory request, and the allocation the kernel present reads stays the one it read before.
                D3DKMT_HANDLE again = 0;
                native12::PresentCopySlot* same = nullptr;
                hr_copy = engine_ddi::present_copy_allocation(device.context, copy_rec.hlist(), plain.hres(), pick,
                                                              &same, &again);
                checkf(hr_copy == S_OK && again == copied && same == slot && m.allocations == requests_before + 1 &&
                           engine_ddi::present_copy_surfaces() == surfaces_before + 1 &&
                           engine_ddi::present_copy_reuses() >= 1,
                       "BD-133: the next present of the same source reuses its idle surface (hr %08lx, allocation %08x)",
                       static_cast<unsigned long>(hr_copy), again);
                // A surface whose last copy has not completed is not taken: the present makes a second surface of
                // the pool, with a memory request of its own. The busy state is the shell's slot (a ticket k = 1
                // whose fence still reads 0); it is made idle again before the source's destroy releases both.
                if (slot) {
                    static volatile UINT64 copy_fence = 0;
                    slot->issued = 1;
                    slot->fence_cpu = &copy_fence;
                    D3DKMT_HANDLE grown = 0;
                    native12::PresentCopySlot* other = nullptr;
                    hr_copy = engine_ddi::present_copy_allocation(device.context, copy_rec.hlist(), plain.hres(),
                                                                  pick, &other, &grown);
                    checkf(hr_copy == S_OK && grown && grown != copied && other && other != slot &&
                               m.allocations == requests_before + 2 &&
                               engine_ddi::present_copy_surfaces() == surfaces_before + 2,
                           "BD-133: a busy copy surface is not reused; the present makes the pool's second surface "
                           "(hr %08lx, allocation %08x)",
                           static_cast<unsigned long>(hr_copy), grown);
                    slot->issued = 0;
                    slot->fence_cpu = nullptr;
                }
                // What was recorded is a legal list: it closes, executes and leaves no list or device error. The
                // barrier pairs are balanced, so both resources end in the state the runtime left them in.
                const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[copy_rec.table];
                t.pfnCloseCommandList(copy_rec.hlist());
                const D3D12DDI_HCOMMANDLIST lists[] = {copy_rec.hlist()};
                const HRESULT hr_exec = engine_ddi::execute_command_lists(queue, 1, lists);
                checkf(hr_exec == S_OK && !device.shell.list_errors && !device.shell.device_errors,
                       "BD-133: the recorded copies execute (hr %08lx)", static_cast<unsigned long>(hr_exec));
                wait_queue_idle(env, queue, "BD-133 present copy");
                destroy_recording(env, device, copy_rec);
            } else {
                checkf(false, "BD-133: a command list for the present copy (hr %08lx)",
                       static_cast<unsigned long>(hr_copy_rec));
            }
            // The negative control: a source no linear surface exists for is refused, answers 0 and removes no
            // device. Two mip levels is such a source, and it is the one the engine makes no linear image of
            // either (the PRIMARY case above).
            Buffer two_mips;
            const HRESULT hr_mips =
                create_target(env, device, Shape{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM, 2, false}, two_mips);
            if (hr_mips == S_OK) {
                Recording mip_rec;
                if (open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, mip_rec) == S_OK) {
                    const uint32_t errors_before = device.shell.device_errors;
                    const uint64_t copies_before = engine_ddi::present_copies();
                    const uint32_t requests_before = m.allocations;
                    D3DKMT_HANDLE refused_copy = 1;
                    unsigned prepared = 0;
                    native12::PresentCopySlot* no_slot = nullptr;
                    const HRESULT hr_refused = engine_ddi::present_copy_allocation(
                        device.context, mip_rec.hlist(), two_mips.hres(),
                        native12::PresentCopyPick{0, &prepared, harness_prepare}, &no_slot, &refused_copy);
                    checkf(hr_refused == E_INVALIDARG && !refused_copy && !no_slot && !prepared &&
                               device.shell.device_errors == errors_before &&
                               engine_ddi::present_copies() == copies_before &&
                               m.allocations == requests_before,
                           "BD-133: a source of 2 mip levels is refused, allocates nothing and removes no device "
                           "(hr %08lx)",
                           static_cast<unsigned long>(hr_refused));
                    destroy_recording(env, device, mip_rec);
                }
                destroy_buffer(env, device, two_mips);
            }
            // BD-133, the D3D9 back buffer (lab arms B and C, 2026-10-12: "format has no linear surface", format
            // 88). D3D9On12 gives a D3DFMT_X8R8G8B8 back buffer as B8G8R8X8_UNORM. Its copy surface is the same
            // shared linear surface, tagged with the A format of its bytes, A8R8G8B8 (present-copy.h,
            // present_copy_format and present_copy_alpha_tag), and the engine's linear image of it is the BGRA8 one:
            // vkd3d-proton maps 88 to VK_FORMAT_B8G8R8A8_UNORM. The copy is 88 into 88, so it executes like every
            // other.
            Buffer bgrx;
            const HRESULT hr_bgrx =
                create_target(env, device, Shape{256, 256, DXGI_FORMAT_B8G8R8X8_UNORM, 1, false}, bgrx);
            checkf(hr_bgrx == S_OK, "BD-133: an ordinary B8G8R8X8 render target (hr %08lx)",
                   static_cast<unsigned long>(hr_bgrx));
            if (hr_bgrx == S_OK) {
                Recording x8_rec;
                const HRESULT hr_x8_rec = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, x8_rec);
                if (hr_x8_rec == S_OK) {
                    const uint64_t copies_before = engine_ddi::present_copies();
                    const uint64_t surfaces_before = engine_ddi::present_copy_surfaces();
                    const uint32_t requests_before = m.allocations;
                    const D3DKMT_HANDLE bgrx_allocation = m.last_allocation;
                    D3DKMT_HANDLE copied = 0;
                    unsigned prepared = 0;
                    native12::PresentCopySlot* x8_slot = nullptr;
                    const HRESULT hr_x8 = engine_ddi::present_copy_allocation(
                        device.context, x8_rec.hlist(), bgrx.hres(),
                        native12::PresentCopyPick{0, &prepared, harness_prepare}, &x8_slot, &copied);
                    const uint32_t want_copy = engine_ddi::kMemoryDedicated | engine_ddi::kMemoryShareable |
                                               engine_ddi::kMemoryLinearSurface;
                    checkf(hr_x8 == S_OK && copied && copied != bgrx_allocation && x8_slot && prepared == 1 &&
                               m.allocations == requests_before + 1 && m.last_flags == want_copy &&
                               m.last_row_pitch >= 256 * 4 && m.last_layout_size &&
                               engine_ddi::present_copies() == copies_before + 1 &&
                               engine_ddi::present_copy_surfaces() == surfaces_before + 1,
                           "BD-133: present_copy_allocation copies a B8G8R8X8 texture into one new linear surface "
                           "(hr %08lx, allocation %08x, flags %x, row pitch %u)",
                           static_cast<unsigned long>(hr_x8), copied, m.last_flags, m.last_row_pitch);
                    // The LB7A format the shell compares with the destination's (native-tables.cpp,
                    // present_copy_destination_check) and the kernel present compares with the redirection
                    // surface's: A8R8G8B8 (21), the A tag of the X8 bytes, not X8R8G8B8 (22), which lab arm B of
                    // 2026-10-12 18:35Z saw refused 240 times out of 240 ("snapshot refused ... format"). The copy
                    // of the B8G8R8A8 source above still carries 21, so both meet an A8R8G8B8 destination with one
                    // format.
                    uint32_t lb7a = 0, a8_lb7a = 0;
                    const HRESULT x8_format =
                        engine_ddi::present_copy_surface_format(device.context, bgrx.hres(), &lb7a);
                    const HRESULT a8_format =
                        engine_ddi::present_copy_surface_format(device.context, plain.hres(), &a8_lb7a);
                    checkf(x8_format == S_OK && lb7a == 21 && a8_format == S_OK && a8_lb7a == 21,
                           "BD-133: the copy surface of a B8G8R8X8 source has LB7A format 21, A8R8G8B8, as the copy "
                           "surface of a B8G8R8A8 source has (hr %08lx, LB7A %u; hr %08lx, LB7A %u)",
                           static_cast<unsigned long>(x8_format), lb7a, static_cast<unsigned long>(a8_format),
                           a8_lb7a);
                    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[x8_rec.table];
                    t.pfnCloseCommandList(x8_rec.hlist());
                    const D3D12DDI_HCOMMANDLIST lists[] = {x8_rec.hlist()};
                    const HRESULT hr_exec = engine_ddi::execute_command_lists(queue, 1, lists);
                    checkf(hr_exec == S_OK && !device.shell.list_errors && !device.shell.device_errors,
                           "BD-133: the recorded B8G8R8X8 copy executes (hr %08lx)",
                           static_cast<unsigned long>(hr_exec));
                    wait_queue_idle(env, queue, "BD-133 B8G8R8X8 present copy");
                    destroy_recording(env, device, x8_rec);
                } else {
                    checkf(false, "BD-133: a command list for the B8G8R8X8 present copy (hr %08lx)",
                           static_cast<unsigned long>(hr_x8_rec));
                }
                destroy_buffer(env, device, bgrx);
            }
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
        checkf(hr == E_OUTOFMEMORY && m.allocations == requests,
               "linear primary: a heap of 4096 bytes for the 256x256 primary: refused, no memory request "
               "(hr %08lx)",
               static_cast<unsigned long>(hr));
    }

    // The primary's memory goes back inside its own destroy: the destroy waits for the work before it.
    uint32_t expected_errors = 0;
    {
        Observed observed;
        engine_ddi::harness_set_release_observer(device.context, observe, &observed);
        const Shape shape{256, 256, DXGI_FORMAT_B8G8R8A8_UNORM};
        Buffer waited, late, other;
        hr = create_target(env, device, shape, waited);
        if (hr == S_OK) {
            // The work retires 40 ms into the destroy.
            engine_ddi::harness_force_completed(queue, 1);
            std::thread retire([&] {
                Sleep(40);
                engine_ddi::harness_force_completed(queue, 0);
            });
            const ULONGLONG began = GetTickCount64();
            destroy_buffer(env, device, waited);
            const ULONGLONG took = GetTickCount64() - began;
            retire.join();
            checkf(observed.events.size() == 1 && !observed.events[0].deferred && observed.events[0].had_memory &&
                       observed.events[0].free_result == S_OK && took >= 15 && took < 2000 &&
                       !engine_ddi::harness_pending_releases(device.context) && !device.shell.device_errors,
                   "linear primary: a destroy before retirement waits and frees inside the same call (%zu events, "
                   "%llu ms, %u pending)",
                   observed.events.size(), static_cast<unsigned long long>(took),
                   engine_ddi::harness_pending_releases(device.context));
        } else {
            checkf(false, "linear primary: the primary for the waiting destroy (hr %08lx)", static_cast<unsigned long>(hr));
        }
        observed.events.clear();
        hr = create_target(env, device, shape, late);
        const HRESULT hr_other = create_target(env, device, Shape{64, 64, DXGI_FORMAT_B8G8R8A8_UNORM, 1, false}, other);
        if (hr == S_OK && hr_other == S_OK) {
            // The work does not retire within the bound: the error is reported, nothing is freed in this call.
            engine_ddi::harness_set_in_ddi_bound(device.context, 30);
            engine_ddi::harness_force_completed(queue, 1);
            destroy_buffer(env, device, late);
            expected_errors = 1;
            checkf(observed.events.empty() && engine_ddi::harness_pending_releases(device.context) == 1 &&
                       device.shell.device_errors == 1 && device.shell.last_device_error == HRESULT_FROM_WIN32(ERROR_TIMEOUT),
                   "linear primary: past the bound the destroy reports ERROR_TIMEOUT and records the release "
                   "(%zu events, %u pending, %u errors, last %08lx)",
                   observed.events.size(), engine_ddi::harness_pending_releases(device.context),
                   device.shell.device_errors, static_cast<unsigned long>(device.shell.last_device_error));
            // Ordinary memory destroyed before retirement is recorded at once, as before.
            const ULONGLONG began = GetTickCount64();
            destroy_buffer(env, device, other);
            checkf(GetTickCount64() - began < 25 && engine_ddi::harness_pending_releases(device.context) == 2 &&
                       observed.events.empty(),
                   "linear primary: ordinary memory does not wait (%u pending)",
                   engine_ddi::harness_pending_releases(device.context));
            engine_ddi::harness_force_completed(queue, 0);
            engine_ddi::harness_set_in_ddi_bound(device.context, 2000);
            Buffer next;
            hr = create_target(env, device, shape, next);      // any DDI call is a retirement point
            size_t deferred = 0;
            for (const engine_ddi::ReleaseEvent& e : observed.events) deferred += e.deferred ? 1 : 0;
            checkf(deferred == 2 && !engine_ddi::harness_pending_releases(device.context),
                   "linear primary: both recorded releases ran at the next DDI call (%zu deferred, %u pending)",
                   deferred, engine_ddi::harness_pending_releases(device.context));
            if (hr == S_OK) destroy_buffer(env, device, next);
        } else {
            checkf(false, "linear primary: the resources for the late destroy (hr %08lx %08lx)",
                   static_cast<unsigned long>(hr), static_cast<unsigned long>(hr_other));
        }
        engine_ddi::harness_set_release_observer(device.context, nullptr, nullptr);
    }

    checkf(m.frees == m.allocations, "linear primary: each allocation came back through free_memory (%u of %u)",
           m.frees, m.allocations);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "linear primary: destroy_engine_queue reports Retired");
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && device.shell.device_errors == expected_errors && !device.shell.list_errors,
           "linear primary: destroy_device_context S_OK with no live object, no error but the timeout's (hr %08lx, "
           "%u live, %u device errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors);
}

} // namespace harness
