// SPDX-License-Identifier: MIT
// Round trip 9, BD-075: the shared surface's open (boundary r5), RuntimeBacked on the stub shell. The runtime opens
// an allocation another process (or the D3D11 shell) created and hands the two private-data records with it:
// D3D12DDIARG_OPENHEAP_0003 carries no resource description, so those records are the whole description. The test
// writes them with the one writer of the format (bc250_shared_surface.h), as a create does, opens them, and checks
// that the open borrowed its memory - one adopt, no allocate callback, the handle the runtime gave - that the opened
// resource reads and writes like any other texture, and that its destroy frees the import without deallocating an
// allocation this driver never created. Every malformed record is refused with nothing constructed.
#include "harness.h"

#include "../../../../contract/bc250_shared_surface.h"

#include <algorithm>
#include <cstring>

namespace harness {
namespace {

struct Surface {
    UINT width = 256;
    UINT height = 256;
    DXGI_FORMAT format = DXGI_FORMAT_B8G8R8A8_UNORM;
    UINT pitch = 0;                             // the creator's row pitch
    UINT64 size = 0;                            // and its backing size
    UINT bind = BC250_SHARED_BIND_SHADER_RESOURCE | BC250_SHARED_BIND_RENDER_TARGET;
    UINT shared = 1;
    UINT access = 0;
    UINT misc = 0;
};

// The records a creator published for this surface, written exactly as the shell's allocation request writes them.
bool records(const Surface& s, BC250_WDDM_ALLOCATION_PRIVATE& allocation, BC250_SURFACE_RESOURCE_PRIVATE& resource) {
    const AMDGPU_WDDM_SURFACE_FORMAT* row = Bc250SharedSurfaceFormat(static_cast<unsigned long>(s.format));
    if (!row) return false;
    BC250_SHARED_SURFACE shared{};
    shared.Width = s.width;
    shared.Height = s.height;
    shared.Pitch = s.pitch;
    shared.DxgiFormat = static_cast<unsigned long>(s.format);
    shared.D3dDdiFormat = row->d3dddi;
    shared.BindFlags = s.bind;
    shared.BytesPerPixel = row->bytes_per_pixel;
    shared.Size = s.size;
    shared.MiscFlags = s.misc;
    shared.Shared = s.shared;
    shared.Access = s.access;
    return Bc250SharedSurfaceEncode(&shared, &allocation, &resource) == BC250_SHARED_SURFACE_OK;
}

struct Opened {
    void* heap = nullptr;
    void* resource = nullptr;
    int rt = 0;
    D3D12DDI_HHEAP hheap() const { return D3D12DDI_HHEAP{heap}; }
    D3D12DDI_HRESOURCE hres() const { return D3D12DDI_HRESOURCE{resource}; }
    Buffer as_buffer() const {                  // for transition(), which names a resource handle
        Buffer b;
        b.heap = heap;
        b.resource = resource;
        return b;
    }
};

HRESULT open_surface(Env& env, Device& device, D3DKMT_HANDLE handle, const BC250_WDDM_ALLOCATION_PRIVATE& allocation,
                     const BC250_SURFACE_RESOURCE_PRIVATE& resource, UINT resource_bytes, UINT allocation_bytes,
                     Opened& out) {
    out = Opened{};
    D3DDDI_OPENALLOCATIONINFO info{};
    info.hAllocation = handle;
    info.pPrivateDriverData = &allocation;
    info.PrivateDriverDataSize = allocation_bytes;
    D3D12DDIARG_OPENHEAP_0003 args{};
    args.NumAllocations = 1;
    args.pOpenAllocationInfo = &info;
    args.pPrivateDriverData = const_cast<BC250_SURFACE_RESOURCE_PRIVATE*>(&resource);
    args.PrivateDriverDataSize = resource_bytes;
    args.InitialResourceState = D3D12DDI_RESOURCE_STATE_COMMON;
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes = env.core.pfnCalcPrivateOpenedHeapAndResourceSizes(
        device.h(), &args, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    if (!sizes.Heap || !sizes.Resource) return E_FAIL;
    out.heap = env.storage.alloc(sizes.Heap);
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.heap || !out.resource) return E_OUTOFMEMORY;
    const HRESULT hr = env.core.pfnOpenHeapAndResource(device.h(), &args, out.hheap(), D3D12DDI_HRTRESOURCE{&out.rt},
                                                       D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
    if (FAILED(hr)) out = Opened{};             // nothing was constructed
    return hr;
}

void destroy_opened(Env& env, Device& device, Opened& opened) {
    if (opened.heap || opened.resource)
        env.core.pfnDestroyHeapAndResource(device.h(), opened.hheap(), opened.hres());
    opened = Opened{};
}

// A refused open: no adopt, no allocation, nothing constructed. E_OUTOFMEMORY is what the slot reports for every
// refusal but a lost device (the AllowOutOfMemory category).
void refused(Env& env, Device& device, StubMemory& m, D3DKMT_HANDLE handle,
             const BC250_WDDM_ALLOCATION_PRIVATE& allocation, const BC250_SURFACE_RESOURCE_PRIVATE& resource,
             UINT resource_bytes, UINT allocation_bytes, const char* what) {
    const uint32_t adoptions = m.adoptions, allocations = m.allocations;
    Opened opened;
    const HRESULT hr =
        open_surface(env, device, handle, allocation, resource, resource_bytes, allocation_bytes, opened);
    checkf(hr == E_OUTOFMEMORY && m.adoptions == adoptions && m.allocations == allocations,
           "shared open: %s is refused with nothing adopted (hr %08lx, %u adopts, %u allocations)", what,
           static_cast<unsigned long>(hr), m.adoptions - adoptions, m.allocations - allocations);
    if (SUCCEEDED(hr)) destroy_opened(env, device, opened);
}

constexpr UINT align_up(UINT value, UINT to) { return (value + to - 1) / to * to; }

// What the engine's linear image of this surface's description is: the pitch a record must carry and the backing the
// open computes from it (resources.cpp, query_linear_surface).
bool engine_layout(Device& device, const Surface& s, UINT& pitch, UINT64& backing) {
    pitch = 0;
    backing = 0;
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
    if (FAILED(device.context->funcs.QueryLinearImage(device.context->device, &d, &info)) || !info.RowPitch) return false;
    const UINT64 rows = info.RowPitch * UINT64{align_up(s.height, 4)};
    pitch = static_cast<UINT>(info.RowPitch);
    backing = (std::max<UINT64>(rows, info.MemorySize) + 4095) & ~UINT64{4095};
    return backing != 0;
}

void unserved_open(Env& env);

} // namespace

void test_shared_open(Env& env) {
    StubMemory m;
    check(load_stub(env, m), "shared open: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free, nullptr, stub_adopt);
    checkf(hr == S_OK && device.context, "shared open: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "shared open: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;

    // The geometry a creator of this surface would have published: the engine's own linear image of the same
    // description, which is what the open checks the record against.
    Surface s;
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
    BC250_VKD3D_LINEAR_IMAGE_INFO linear{};
    linear.Size = sizeof(linear);
    hr = device.context->funcs.QueryLinearImage(device.context->device, &d, &linear);
    const UINT64 rows = linear.RowPitch * UINT64{align_up(s.height, 4)};
    const UINT64 backing = (std::max<UINT64>(rows, linear.MemorySize) + 4095) & ~UINT64{4095};
    checkf(hr == S_OK && linear.RowPitch && !(linear.RowPitch % 16) && backing,
           "shared open: the engine's linear image of 256x256 BGRA8 (hr %08lx, pitch %llu, %llu bytes)",
           static_cast<unsigned long>(hr), static_cast<unsigned long long>(linear.RowPitch),
           static_cast<unsigned long long>(backing));
    if (hr != S_OK) return;
    s.pitch = static_cast<UINT>(linear.RowPitch);
    s.size = backing;

    BC250_WDDM_ALLOCATION_PRIVATE allocation{};
    BC250_SURFACE_RESOURCE_PRIVATE resource{};
    check(records(s, allocation, resource), "shared open: the creator's two records are written by the contract header");
    const UINT resource_bytes = sizeof(resource), allocation_bytes = sizeof(allocation);
    const D3DKMT_HANDLE handle = 0x50000001u;   // the runtime's, never one of the shell's

    // The open itself.
    {
        Opened opened;
        const uint32_t allocations = m.allocations;
        hr = open_surface(env, device, handle, allocation, resource, resource_bytes, allocation_bytes, opened);
        checkf(hr == S_OK && m.adoptions == 1 && m.allocations == allocations && m.last_adopt_handle == handle &&
                   m.last_adopt_flags == (engine_ddi::kMemoryDedicated | engine_ddi::kMemoryShareable |
                                          engine_ddi::kMemoryLinearSurface) &&
                   m.last_adopt_byte_size == backing && m.last_adopt_alignment == linear.MemoryAlignment &&
                   m.last_adopt_type_bits == linear.MemoryTypeBits,
               "shared open: one adopt of the runtime's allocation and no allocate callback (hr %08lx, %u adopts, "
               "handle %08x, flags %x, %llu bytes, alignment %llu)",
               static_cast<unsigned long>(hr), m.adoptions, m.last_adopt_handle, m.last_adopt_flags,
               static_cast<unsigned long long>(m.last_adopt_byte_size),
               static_cast<unsigned long long>(m.last_adopt_alignment));
        if (hr == S_OK) {
            // An opened resource is not a primary and names nothing to a present: only the creator's own destroy
            // may release the allocation, and this process is not the creator.
            D3DKMT_HANDLE presented = 1;
            const HRESULT named = engine_ddi::present_allocation(device.context, opened.hres(), &presented);
            checkf(named == E_INVALIDARG && !presented,
                   "shared open: present_allocation refuses an opened resource (hr %08lx)",
                   static_cast<unsigned long>(named));
            // Nothing is placed beside it, as on any linear surface.
            Buffer beside;
            const Buffer base = opened.as_buffer();
            const HRESULT placed = create_placed_buffer(env, device, base, 0, 4096, beside);
            checkf(placed == E_OUTOFMEMORY,
                   "shared open: a buffer placed on the opened surface's memory is refused (hr %08lx)",
                   static_cast<unsigned long>(placed));
            D3D12DDI_RESOURCE_ALLOCATION_INFO_0022 existing{};
            env.core.pfnCheckExistingResourceAllocationInfo(device.h(), opened.hres(), &existing);
            checkf(existing.ResourceDataSize == backing,
                   "shared open: CheckExistingResourceAllocationInfo answers the surface's %llu bytes (%llu)",
                   static_cast<unsigned long long>(backing),
                   static_cast<unsigned long long>(existing.ResourceDataSize));

            // The opened surface renders: cleared through a render target view, copied into a READBACK buffer and
            // compared texel by texel. The clear is what proves the image was placed over the adopted memory.
            const UINT bpp = 4, pitch = align_up(s.width * bpp, 256);
            Buffer readback;
            HRESULT hr_rb = create_buffer(env, device, HeapKind::Readback, UINT64{pitch} * s.height, false, readback);
            D3D12DDIARG_CREATE_DESCRIPTOR_HEAP_0001 heap_args{D3D12DDI_DESCRIPTOR_HEAP_TYPE_RTV, 1,
                                                              D3D12DDI_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            void* heap_storage = env.storage.alloc(env.core.pfnCalcPrivateDescriptorHeapSize(device.h(), &heap_args));
            const D3D12DDI_HDESCRIPTORHEAP hheap{heap_storage};
            const HRESULT hr_h =
                heap_storage ? env.core.pfnCreateDescriptorHeap(device.h(), &heap_args, hheap) : E_OUTOFMEMORY;
            D3D12DDI_CPU_DESCRIPTOR_HANDLE rtv{};
            if (hr_h == S_OK) rtv = env.core.pfnGetCPUDescriptorHandleForHeapStart(device.h(), hheap);
            if (rtv.ptr) {
                D3D12DDIARG_CREATE_RENDER_TARGET_VIEW_0002 view{};
                view.hDrvResource = opened.hres();
                view.Format = s.format;
                view.ResourceDimension = D3D12DDI_RD_TEXTURE2D;
                view.Tex2D = {0, 0, 1, 0};
                env.core.pfnCreateRenderTargetView(device.h(), &view, rtv);
            }
            Recording rec;
            const HRESULT hr_rec = hr_rb == S_OK && rtv.ptr ? open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec)
                                                            : E_ABORT;
            if (hr_rec == S_OK) {
                const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
                const FLOAT colour[4] = {0.2f, 0.4f, 0.6f, 1.0f};
                const D3D12DDIARG_RESOURCE_BARRIER_0022 to_target =
                    transition(base, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_RENDER_TARGET);
                t.pfnResourceBarrier(rec.hlist(), 1, &to_target);
                t.pfnClearRenderTargetView(rec.hlist(), rtv, colour, 0, nullptr);
                const D3D12DDIARG_RESOURCE_BARRIER_0022 to_source =
                    transition(base, D3D12DDI_RESOURCE_STATE_RENDER_TARGET, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
                t.pfnResourceBarrier(rec.hlist(), 1, &to_source);
                const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{s.format, s.width, s.height, 1, pitch,
                                                                                pitch * s.height};
                D3D12DDIARG_BUFFER_PLACEMENT dst{}, src{};
                dst.BaseAddress.UMD = {readback.hres(), 0};
                src.BaseAddress.UMD = {opened.hres(), 0};
                t.pfnCopyTextureRegion(rec.hlist(), &dst, {D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint},
                                       0, 0, 0, &src, {D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr}, nullptr);
                t.pfnCloseCommandList(rec.hlist());
                const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
                hr_rb = engine_ddi::execute_command_lists(queue, 1, lists);
                wait_queue_idle(env, queue, "shared open");
                void* cpu = nullptr;
                const HRESULT hr_map = env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu);
                UINT bad = 0;
                if (hr_map == S_OK && cpu) {
                    for (UINT y = 0; y < s.height; ++y) {
                        const BYTE* row = static_cast<const BYTE*>(cpu) + UINT64{pitch} * y;
                        for (UINT x = 0; x < s.width; ++x) {
                            UINT32 texel = 0;
                            std::memcpy(&texel, row + UINT64{bpp} * x, bpp);
                            bad += texel != 0xff336699u ? 1u : 0u;
                        }
                    }
                    env.core.pfnUnmapHeap(device.h(), readback.hheap());
                }
                checkf(hr_rb == S_OK && hr_map == S_OK && !bad && !device.shell.list_errors,
                       "shared open: the opened surface clears and reads back ff336699 everywhere (hr %08lx %08lx, %u "
                       "of %u texels differ)",
                       static_cast<unsigned long>(hr_rb), static_cast<unsigned long>(hr_map), bad,
                       s.width * s.height);
            } else {
                checkf(false, "shared open: READBACK buffer, RTV and recording (hr %08lx %08lx)",
                       static_cast<unsigned long>(hr_rb), static_cast<unsigned long>(hr_h));
            }
            destroy_recording(env, device, rec);
            if (hr_h == S_OK) env.core.pfnDestroyDescriptorHeap(device.h(), hheap);
            if (readback.resource) destroy_buffer(env, device, readback);

            // The destroy: the import goes back through free_memory, and the allocation is not this driver's to
            // destroy. The stub counts frees; that the shell makes no deallocate callback is heap-import-test's.
            const uint32_t frees = m.frees;
            destroy_opened(env, device, opened);
            checkf(m.frees == frees + 1 && !engine_ddi::harness_pending_releases(device.context),
                   "shared open: the destroy returns the borrowed memory once and leaves nothing pending (%u frees, %u "
                   "pending)",
                   m.frees - frees, engine_ddi::harness_pending_releases(device.context));
        }
    }

    // Two opens, each adopting its own import, as two resources over one allocation do. engine-ddi keeps no index
    // of allocation handles, so it asks the shell once per open and nothing here is shared between the two. The
    // production shell does keep such an index and refuses a second adopt of a handle it already holds
    // (heap-import.cpp, "the allocation is already imported"), which is why the handle below is the same one: this
    // pins the boundary's own behaviour, and the shell's rule is heap-import-test's.
    {
        Opened a, b;
        const HRESULT hr_a = open_surface(env, device, handle, allocation, resource, resource_bytes, allocation_bytes, a);
        const HRESULT hr_b = open_surface(env, device, handle, allocation, resource, resource_bytes, allocation_bytes, b);
        checkf(hr_a == S_OK && hr_b == S_OK && m.adoptions == 3,
               "shared open: the same handle opened twice adopts twice (hr %08lx %08lx, %u adopts)",
               static_cast<unsigned long>(hr_a), static_cast<unsigned long>(hr_b), m.adoptions);
        if (hr_b == S_OK) destroy_opened(env, device, b);
        if (hr_a == S_OK) destroy_opened(env, device, a);
    }

    // Every malformed record, each refused before anything is adopted.
    {
        refused(env, device, m, 0, allocation, resource, resource_bytes, allocation_bytes, "a zero allocation handle");
        refused(env, device, m, handle, allocation, resource, resource_bytes - 1, allocation_bytes,
                "a resource record of 63 bytes");
        refused(env, device, m, handle, allocation, resource, resource_bytes, allocation_bytes - 1,
                "an allocation record of 31 bytes");
        BC250_SURFACE_RESOURCE_PRIVATE bad = resource;
        bad.Version = 2;
        refused(env, device, m, handle, allocation, bad, resource_bytes, allocation_bytes, "an E26R v2 record");
        bad = resource;
        bad.Shared = 0;
        refused(env, device, m, handle, allocation, bad, resource_bytes, allocation_bytes, "a record that shares nothing");
        bad = resource;
        bad.Access = BC250_SURFACE_RESOURCE_PRIMARY;
        refused(env, device, m, handle, allocation, bad, resource_bytes, allocation_bytes, "a primary's record");
        bad = resource;
        bad.MipLevels = 2;
        refused(env, device, m, handle, allocation, bad, resource_bytes, allocation_bytes, "a record of two mips");
        bad = resource;
        bad.Width = s.width - 1;
        refused(env, device, m, handle, allocation, bad, resource_bytes, allocation_bytes,
                "a record whose width is not the allocation's");
        bad = resource;
        bad.Format = DXGI_FORMAT_R32_UINT;
        refused(env, device, m, handle, allocation, bad, resource_bytes, allocation_bytes, "an uncomposed format");
        // The agreement that cannot be assumed: a record whose pitch is not the one this engine's image of the same
        // description has. The record itself is well formed - a wider pitch with a backing to match - and only the
        // comparison with the engine's answer refuses it.
        Surface wide = s;
        wide.pitch = s.pitch + 16;
        wide.size = s.size + 65536;
        BC250_WDDM_ALLOCATION_PRIVATE wide_allocation{};
        BC250_SURFACE_RESOURCE_PRIVATE wide_resource{};
        check(records(wide, wide_allocation, wide_resource), "shared open: the record of a wider pitch is well formed");
        refused(env, device, m, handle, wide_allocation, wide_resource, resource_bytes, allocation_bytes,
                "a pitch the engine's image does not have");
        // And a record whose backing is smaller than the image needs: a height that is not a multiple of four, with
        // only the rows the record itself describes, where the engine's image still covers whole four-row blocks. No
        // creator of ours writes such a record - it rounds the size up to a page over the blocks - which is why the
        // opener compares instead of trusting.
        Surface short_rows = s;
        short_rows.height = s.height - 2;
        UINT short_pitch = 0;
        UINT64 short_backing = 0;
        if (engine_layout(device, short_rows, short_pitch, short_backing)) {
            short_rows.pitch = short_pitch;
            short_rows.size = UINT64{short_pitch} * short_rows.height;
            BC250_WDDM_ALLOCATION_PRIVATE short_allocation{};
            BC250_SURFACE_RESOURCE_PRIVATE short_resource{};
            checkf(short_rows.size < short_backing && records(short_rows, short_allocation, short_resource),
                   "shared open: the record of %u rows is well formed and smaller than the image's %llu bytes (%llu)",
                   short_rows.height, static_cast<unsigned long long>(short_backing),
                   static_cast<unsigned long long>(short_rows.size));
            if (short_rows.size < short_backing)
                refused(env, device, m, handle, short_allocation, short_resource, resource_bytes, allocation_bytes,
                        "a backing below what the engine's image needs");
        }
    }

    // A shell that refuses the adopt: the open fails, nothing is constructed, the device lives.
    {
        m.refuse_adopt = true;
        const uint32_t errors = device.shell.device_errors;
        Opened opened;
        hr = open_surface(env, device, handle, allocation, resource, resource_bytes, allocation_bytes, opened);
        checkf(hr == E_OUTOFMEMORY && m.adopt_refusals == 1 && device.shell.device_errors == errors,
               "shared open: a shell that refuses the adopt fails the open and keeps the device (hr %08lx, %u "
               "refusals, %u errors)",
               static_cast<unsigned long>(hr), m.adopt_refusals, device.shell.device_errors - errors);
        m.refuse_adopt = false;
    }

    unserved_open(env);

    checkf(m.frees == m.allocations + m.adoptions,
           "shared open: every allocation and every adopted import came back through free_memory (%u frees, %u "
           "allocations, %u adopts)",
           m.frees, m.allocations, m.adoptions);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "shared open: destroy_engine_queue reports Retired");
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && !device.shell.device_errors && !device.shell.list_errors,
           "shared open: destroy_device_context S_OK with no live object and no error (hr %08lx, %u live, %u device "
           "errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors);
}

namespace {

// A device whose shell serves no shared open at all (adopt_memory null, as every shell before r5): the slot refuses
// and the device lives.
void unserved_open(Env& env) {
    StubMemory m;
    if (!load_stub(env, m) || !m.address) return;
    Device device;
    device.shell.memory = &m;
    const HRESULT hr = open_device(env, device, stub_allocate, stub_free);
    if (hr != S_OK) return;
    Surface s;
    UINT pitch = 0;
    UINT64 backing = 0;
    HRESULT refused_hr = E_FAIL;
    Opened opened;
    if (engine_layout(device, s, pitch, backing)) {
        s.pitch = pitch;
        s.size = backing;
        BC250_WDDM_ALLOCATION_PRIVATE allocation{};
        BC250_SURFACE_RESOURCE_PRIVATE resource{};
        if (records(s, allocation, resource))
            refused_hr = open_surface(env, device, 0x50000002u, allocation, resource, sizeof(resource),
                                      sizeof(allocation), opened);
    }
    checkf(refused_hr == E_OUTOFMEMORY && !m.adoptions && !device.shell.device_errors,
           "shared open: a shell without adopt_memory refuses the open and keeps the device (hr %08lx)",
           static_cast<unsigned long>(refused_hr));
    if (SUCCEEDED(refused_hr)) destroy_opened(env, device, opened);
    uint32_t live = UINT32_MAX;
    (void)engine_ddi::destroy_device_context(device.context, &live);
}

} // namespace

} // namespace harness
