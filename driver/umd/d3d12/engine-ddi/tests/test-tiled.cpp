// SPDX-License-Identifier: MIT
// Round trip 6: tiled resources on RuntimeBacked heaps, the path FL 12_0 needs (tiled resources tier 2). The stub
// shell of round trip 5 allocates every heap's memory; the tile heaps are DEFAULT heaps of the committed shape with a
// heap-wide buffer (the INFERENCE of engine-ddi.h for the runtime's heaps), which lets the harness read a heap's tiles
// back through that buffer. Mappings go through update_tile_mappings and copy_tile_mappings, as the shell's queue
// slots will call them (INTEGRATION.md, "Tiled resources"); everything else through the DDI tables.
//
//   1. A reserved buffer of 4 tiles and a reserved R32_UINT texture of 256 x 256 (2 x 2 tiles) are mapped to tiles
//      0-3 and 4-7 of heap A, written by copies and read back word for word.
//   2. Buffer tile 1 is remapped to tile 0 of heap B and written again: the buffer reads the new words in tile 1 and
//      the old ones elsewhere, heap B's tile 0 holds the new words, heap A's tile 1 still holds the old ones.
//   3. CopyTileMappings gives a second reserved buffer the mappings of buffer tiles 2-3: it reads their words.
//   4. CopyTiles round trip: the texture's tiles go to a linear buffer (each tile's 128 x 128 texels row by row),
//      from there into a second reserved texture mapped to heap B, which reads back as the first one.
// Around them: the reserved resources' GPU VA, allocation handle and residency answer, GetMipPacking against the
// engine's GetResourceTiling, the tiled multisample quality levels, and the release of every allocation.
#include "harness.h"
#include <cstring>

namespace harness {

namespace {
constexpr UINT kTile = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;        // 64 KiB
constexpr UINT kTileWords = kTile / sizeof(UINT32);
constexpr UINT kTexSize = 256;                                          // R32_UINT: 2 x 2 tiles of 128 x 128
constexpr UINT kTexTile = 128;
constexpr UINT kPitch = kTexSize * sizeof(UINT32);

constexpr UINT32 p1(UINT i) { return 0x9e3779b9u * (i + 11); }         // reserved buffer, 4 tiles
constexpr UINT32 p2(UINT x, UINT y) { return 0xa5000000u | (y << 8) | x; }   // texture texel
constexpr UINT32 p3(UINT i) { return 0xc2b2ae35u * (i + 3); }          // the remapped buffer tile

constexpr UINT kAllCategories =
    D3D12DDI_HEAP_FLAG_BUFFERS | D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES | D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;

struct Observed {
    uint32_t with_memory = 0;
    uint32_t freed_ok = 0;
};
void observe(void* user, const engine_ddi::ReleaseEvent* event) {
    auto* o = static_cast<Observed*>(user);
    o->with_memory += event->had_memory ? 1u : 0u;
    o->freed_ok += (event->had_memory && event->free_result == S_OK) ? 1u : 0u;
}

// A DEFAULT heap that allows every resource category (resource heap tier 2), with a buffer over all of it: one
// CreateHeapAndResource call, one allocate_memory.
HRESULT create_tile_heap(Env& env, Device& device, UINT tiles, Buffer& out) {
    out = Buffer{};
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_BUFFER;
    res.Width = UINT64{tiles} * kTile;
    res.Height = 1;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = DXGI_FORMAT_UNKNOWN;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_ROW_MAJOR;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_UNDEFINED;
    const D3D12_HEAP_PROPERTIES props = env.engine->GetCustomHeapProperties(0, D3D12_HEAP_TYPE_DEFAULT);
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    heap.ByteSize = res.Width;
    heap.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    heap.CPUPageProperty = static_cast<D3D12DDI_CPU_PAGE_PROPERTY>(props.CPUPageProperty - 1);
    heap.MemoryPool = static_cast<D3D12DDI_MEMORY_POOL>(props.MemoryPoolPreference - 1);
    heap.Flags = static_cast<D3D12DDI_HEAP_FLAGS>(kAllCategories);
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

D3D12DDIARG_CREATERESOURCE_0088 reserved_buffer_desc(UINT tiles) {
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_BUFFER;
    res.Width = UINT64{tiles} * kTile;
    res.Height = 1;
    res.DepthOrArraySize = 1;
    res.MipLevels = 1;
    res.Format = DXGI_FORMAT_UNKNOWN;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_ROW_MAJOR;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_UNDEFINED;
    return res;
}

// A reserved R32_UINT texture of kTexSize squared, in the API's layout for reserved textures (64KB undefined swizzle),
// starting in the legacy COPY_DEST state.
D3D12DDIARG_CREATERESOURCE_0088 reserved_texture_desc(UINT16 mips) {
    D3D12DDIARG_CREATERESOURCE_0088 res{};
    res.ResourceType = D3D12DDI_RT_TEXTURE2D;
    res.Width = kTexSize;
    res.Height = kTexSize;
    res.DepthOrArraySize = 1;
    res.MipLevels = mips;
    res.Format = DXGI_FORMAT_R32_UINT;
    res.SampleDesc = {1, 0};
    res.Layout = D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE;
    res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_DEST;
    return res;
}

// Reserved: a resource description with neither a heap nor a base resource (engine-ddi.h, "Reserved").
HRESULT create_reserved(Env& env, Device& device, const D3D12DDIARG_CREATERESOURCE_0088& res, Buffer& out) {
    out = Buffer{};
    const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes =
        env.core.pfnCalcPrivateHeapAndResourceSizes(device.h(), nullptr, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
    out.resource = env.storage.alloc(sizes.Resource);
    if (!out.resource) return E_OUTOFMEMORY;
    return env.core.pfnCreateHeapAndResource(device.h(), nullptr, D3D12DDI_HHEAP{}, D3D12DDI_HRTRESOURCE{&out.rt}, &res,
                                             nullptr, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
}

D3D12DDIARG_BUFFER_PLACEMENT at(const Buffer& b, UINT64 offset) {
    D3D12DDIARG_BUFFER_PLACEMENT p{};
    p.BaseAddress.UMD = {b.hres(), offset};
    return p;
}

// Everything the device has to its engine queue, then the queue drained and the readback heap mapped.
const UINT32* run(Env& env, Device& device, engine_ddi::EngineQueue* queue, Recording& rec, const Buffer& readback,
                  const char* what) {
    env.lists[rec.table].pfnCloseCommandList(rec.hlist());
    const D3D12DDI_HCOMMANDLIST lists[] = {rec.hlist()};
    const HRESULT hr = engine_ddi::execute_command_lists(queue, 1, lists);
    checkf(hr == S_OK && !device.shell.list_errors && !device.shell.device_errors,
           "tiled: %s executed (hr %08lx, %u list errors, %u device errors)", what, static_cast<unsigned long>(hr),
           device.shell.list_errors, device.shell.device_errors);
    if (hr != S_OK || !wait_queue_idle(env, queue, what)) return nullptr;
    void* cpu = nullptr;
    if (env.core.pfnMapHeap(device.h(), readback.hheap(), &cpu) != S_OK || !cpu) return nullptr;
    return static_cast<const UINT32*>(cpu);
}

void reopen(Env& env, Recording& rec) {
    D3D12DDIARG_RESETCOMMANDLIST_0040 reset{D3D12DDI_HCOMMANDRECORDER_0040{rec.recorder}, 1,
                                           D3D12DDI_COMMAND_LIST_FLAG_NONE};
    env.lists[rec.table].pfnResetCommandList(rec.hlist(), &reset);
}

UINT texture_mismatches(const UINT32* words) {
    UINT bad = 0;
    for (UINT y = 0; y < kTexSize; ++y)
        for (UINT x = 0; x < kTexSize; ++x) bad += words[y * kTexSize + x] != p2(x, y) ? 1u : 0u;
    return bad;
}
} // namespace

void test_tiled(Env& env) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    const HRESULT hr_options = env.engine->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options));
    checkf(hr_options == S_OK && options.TiledResourcesTier >= D3D12_TILED_RESOURCES_TIER_2,
           "tiled: the engine reports tiled resources tier %d on this GPU (2 or above needed for FL 12_0)",
           static_cast<int>(options.TiledResourcesTier));
    if (hr_options != S_OK || options.TiledResourcesTier < D3D12_TILED_RESOURCES_TIER_2) return;

    StubMemory m;
    check(load_stub(env, m), "tiled: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free);
    checkf(hr == S_OK && device.context, "tiled: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    Observed observed;
    engine_ddi::harness_set_release_observer(device.context, observe, &observed);

    // Resources. Heaps and the committed buffers allocate; the reserved resources must not.
    Buffer heap_a, heap_b, upload, readback, linear, buffer, buffer2, texture, texture2;
    const HRESULT hr_heaps[] = {
        create_tile_heap(env, device, 8, heap_a),
        create_tile_heap(env, device, 8, heap_b),
        create_buffer(env, device, HeapKind::Upload, 9 * UINT64{kTile}, false, upload),
        create_buffer(env, device, HeapKind::Readback, 8 * UINT64{kTile}, false, readback),
        create_buffer(env, device, HeapKind::Default, 4 * UINT64{kTile}, false, linear),
    };
    const uint32_t allocations = m.allocations;
    const HRESULT hr_reserved[] = {
        create_reserved(env, device, reserved_buffer_desc(4), buffer),
        create_reserved(env, device, reserved_buffer_desc(2), buffer2),
        create_reserved(env, device, reserved_texture_desc(1), texture),
        create_reserved(env, device, reserved_texture_desc(1), texture2),
    };
    bool created = true;
    for (HRESULT h : hr_heaps) created = created && h == S_OK;
    checkf(created && allocations == 5,
           "tiled: two DEFAULT tile heaps of 8 tiles (every category), UPLOAD, READBACK and DEFAULT buffers, "
           "one allocate_memory each (%u allocations)",
           allocations);
    bool reserved = true;
    for (HRESULT h : hr_reserved) reserved = reserved && h == S_OK;
    checkf(reserved && m.allocations == allocations,
           "tiled: reserved buffers of 4 and 2 tiles and two reserved R32_UINT 256x256 textures, no allocate_memory "
           "(hr %08lx %08lx %08lx %08lx)",
           static_cast<unsigned long>(hr_reserved[0]), static_cast<unsigned long>(hr_reserved[1]),
           static_cast<unsigned long>(hr_reserved[2]), static_cast<unsigned long>(hr_reserved[3]));
    if (!created || !reserved) return;

    // What the other device slots say about a reserved resource.
    const D3D12DDI_GPU_VIRTUAL_ADDRESS va = env.core.pfnCheckResourceVirtualAddress(device.h(), buffer.hres());
    const D3DKMT_HANDLE handle = env.core.pfnCheckResourceAllocationHandle(device.h(), D3D10DDI_HRESOURCE{buffer.resource});
    D3DKMT_HANDLE resident = 1;
    const HRESULT hr_resident = engine_ddi::object_allocation(
        device.context, D3D12DDI_HANDLE_AND_TYPE{buffer.resource, D3D12DDI_HT_0012_RESOURCE}, &resident);
    checkf(va != 0 && handle == 0 && hr_resident == S_FALSE && resident == 0,
           "tiled: the reserved buffer has a GPU VA (%llx), no allocation handle, and object_allocation S_FALSE: no "
           "memory of its own to make resident",
           static_cast<unsigned long long>(va));
    UINT levels = 0;
    env.core.pfnCheckMultisampleQualityLevels(device.h(), DXGI_FORMAT_R32_UINT, 1,
                                              D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_TILED_RESOURCE, &levels);
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{DXGI_FORMAT_R32_UINT, 1,
                                                    D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE, 0};
    env.engine->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q));
    checkf(levels >= 1 && levels == q.NumQualityLevels,
           "tiled: CheckMultisampleQualityLevels TILED_RESOURCE, R32_UINT x1: %u levels, the engine's answer %u", levels,
           q.NumQualityLevels);
    {
        // GetMipPacking: a full mip chain has a packed tail; its size is the engine's GetResourceTiling answer.
        Buffer chain;
        const HRESULT hr_chain = create_reserved(env, device, reserved_texture_desc(9), chain);
        UINT packed = UINT_MAX, packed_tiles = UINT_MAX, single = UINT_MAX, single_tiles = UINT_MAX;
        D3D12_PACKED_MIP_INFO engine_packed{};
        if (hr_chain == S_OK) {
            env.core.pfnGetMipPacking(device.h(), chain.hres(), &packed, &packed_tiles);
            env.core.pfnGetMipPacking(device.h(), texture.hres(), &single, &single_tiles);
            UINT no_subresources = 0;
            D3D12_SUBRESOURCE_TILING unused{};
            env.engine->GetResourceTiling(static_cast<ID3D12Resource*>(engine_ddi::harness_engine_object(chain.resource)),
                                          nullptr, &engine_packed, nullptr, &no_subresources, 0, &unused);
        }
        checkf(hr_chain == S_OK && packed > 0 && packed == engine_packed.NumPackedMips &&
                   packed_tiles == engine_packed.NumTilesForPackedMips && single == 0 && single_tiles == 0,
               "tiled: GetMipPacking of a 9-mip reserved texture is the engine's packed tail (%u mips in %u tiles, "
               "engine %u in %u); one mip packs nothing (%u, %u)",
               packed, packed_tiles, engine_packed.NumPackedMips, engine_packed.NumTilesForPackedMips, single,
               single_tiles);
        destroy_buffer(env, device, chain);
    }

    // Source words in the upload heap: the buffer's 4 tiles, the texture's rows, the remap tile.
    void* cpu = nullptr;
    hr = env.core.pfnMapHeap(device.h(), upload.hheap(), &cpu);
    checkf(hr == S_OK && cpu, "tiled: MapHeap of the UPLOAD heap (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK || !cpu) return;
    auto* words = static_cast<UINT32*>(cpu);
    for (UINT i = 0; i < 4 * kTileWords; ++i) words[i] = p1(i);
    for (UINT y = 0; y < kTexSize; ++y)
        for (UINT x = 0; x < kTexSize; ++x) words[4 * kTileWords + y * kTexSize + x] = p2(x, y);
    for (UINT i = 0; i < kTileWords; ++i) words[8 * kTileWords + i] = p3(i);
    env.core.pfnUnmapHeap(device.h(), upload.hheap());

    BC250_VKD3D_COMMAND_QUEUE_DESC qdesc{sizeof(qdesc), D3D12_COMMAND_LIST_TYPE_DIRECT, 0, 0, 0};
    engine_ddi::EngineQueue* queue = nullptr;
    hr = engine_ddi::create_engine_queue(device.context, &qdesc, &queue, &queue);
    checkf(hr == S_OK && queue, "tiled: create_engine_queue DIRECT (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    Recording rec;
    hr = open_recording(env, device, D3D12DDI_COMMAND_QUEUE_FLAG_3D, rec);
    checkf(hr == S_OK, "tiled: pool, recorder and DIRECT list (hr %08lx)", static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& t = env.lists[rec.table];
    const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT footprint{DXGI_FORMAT_R32_UINT, kTexSize, kTexSize, 1, kPitch,
                                                                    kPitch * kTexSize};
    const D3D12DDIARG_PLACED_RESOURCE pitched{D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED, &footprint};
    const D3D12DDIARG_PLACED_RESOURCE subresource{D3D12DDI_RL_SELECT_SUBRESOURCE, nullptr};
    auto barrier = [&](const Buffer& b, D3D12DDI_RESOURCE_STATES before, D3D12DDI_RESOURCE_STATES after) {
        const D3D12DDIARG_RESOURCE_BARRIER_0022 one = transition(b, before, after);
        t.pfnResourceBarrier(rec.hlist(), 1, &one);
    };
    // Tiles reached through two resources: an aliasing barrier with no resource, as D3D12 asks between them.
    auto alias = [&]() {
        D3D12DDIARG_RESOURCE_BARRIER_0022 one{};
        one.Type = D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING;
        t.pfnResourceBarrier(rec.hlist(), 1, &one);
    };
    const D3D12DDI_TILED_RESOURCE_COORDINATE origin{0, 0, 0, 0};
    const D3D12DDI_TILE_REGION_SIZE four{4, FALSE, 0, 0, 0};
    const D3D12DDI_TILE_REGION_SIZE box2x2{4, TRUE, 2, 2, 1};
    const D3D12DDI_TILE_RANGE_FLAGS none = D3D12DDI_TILE_RANGE_FLAG_NONE;
    const UINT count4 = 4;

    // 1. Map, write through copies, read back.
    {
        const UINT start_buffer = 0, start_texture = 4;
        const HRESULT hr_b = engine_ddi::update_tile_mappings(queue, buffer.hres(), 1, &origin, &four, heap_a.hheap(), 1,
                                                              &none, &start_buffer, &count4, D3D12DDI_TILE_MAPPING_FLAG_NONE);
        const HRESULT hr_t = engine_ddi::update_tile_mappings(queue, texture.hres(), 1, &origin, &box2x2, heap_a.hheap(),
                                                              1, &none, &start_texture, &count4,
                                                              D3D12DDI_TILE_MAPPING_FLAG_NONE);
        checkf(hr_b == S_OK && hr_t == S_OK && !device.shell.device_errors,
               "tiled: update_tile_mappings maps the buffer to heap A tiles 0-3 and the texture's 2x2 box to tiles 4-7 "
               "(hr %08lx %08lx)",
               static_cast<unsigned long>(hr_b), static_cast<unsigned long>(hr_t));
        barrier(buffer, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        t.pfnCopyBufferRegion(rec.hlist(), at(buffer, 0), at(upload, 0), 4 * UINT64{kTile});
        const D3D12DDIARG_BUFFER_PLACEMENT tex = at(texture, 0), from = at(upload, 4 * UINT64{kTile});
        t.pfnCopyTextureRegion(rec.hlist(), &tex, subresource, 0, 0, 0, &from, pitched, nullptr);
        barrier(buffer, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        barrier(texture, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnCopyBufferRegion(rec.hlist(), at(readback, 0), at(buffer, 0), 4 * UINT64{kTile});
        const D3D12DDIARG_BUFFER_PLACEMENT out = at(readback, 4 * UINT64{kTile});
        t.pfnCopyTextureRegion(rec.hlist(), &out, pitched, 0, 0, 0, &tex, subresource, nullptr);
        if (const UINT32* back = run(env, device, queue, rec, readback, "tiled mapping")) {
            UINT bad = 0;
            for (UINT i = 0; i < 4 * kTileWords; ++i) bad += back[i] != p1(i) ? 1u : 0u;
            const UINT bad_texture = texture_mismatches(back + 4 * kTileWords);
            checkf(bad == 0 && bad_texture == 0,
                   "tiled: the reserved buffer and texture read back word for word (%u and %u of %u words differ)", bad,
                   bad_texture, 4 * kTileWords);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
    }

    // 2. Remap buffer tile 1 to heap B tile 0 and write it again.
    {
        const D3D12DDI_TILED_RESOURCE_COORDINATE tile1{1, 0, 0, 0};
        const D3D12DDI_TILE_REGION_SIZE one{1, FALSE, 0, 0, 0};
        const UINT start = 0, count = 1;
        hr = engine_ddi::update_tile_mappings(queue, buffer.hres(), 1, &tile1, &one, heap_b.hheap(), 1, &none, &start,
                                              &count, D3D12DDI_TILE_MAPPING_FLAG_NONE);
        checkf(hr == S_OK, "tiled: update_tile_mappings remaps buffer tile 1 to heap B tile 0 (hr %08lx)",
               static_cast<unsigned long>(hr));
        reopen(env, rec);
        barrier(buffer, D3D12DDI_RESOURCE_STATE_COPY_SOURCE, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        t.pfnCopyBufferRegion(rec.hlist(), at(buffer, kTile), at(upload, 8 * UINT64{kTile}), kTile);
        barrier(buffer, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        alias();
        t.pfnCopyBufferRegion(rec.hlist(), at(readback, 0), at(buffer, 0), 4 * UINT64{kTile});
        t.pfnCopyBufferRegion(rec.hlist(), at(readback, 4 * UINT64{kTile}), at(heap_b, 0), kTile);
        t.pfnCopyBufferRegion(rec.hlist(), at(readback, 5 * UINT64{kTile}), at(heap_a, kTile), kTile);
        if (const UINT32* back = run(env, device, queue, rec, readback, "tiled remap")) {
            UINT bad = 0, bad_b = 0, bad_a = 0;
            for (UINT i = 0; i < 4 * kTileWords; ++i) {
                const bool remapped = i >= kTileWords && i < 2 * kTileWords;
                bad += back[i] != (remapped ? p3(i - kTileWords) : p1(i)) ? 1u : 0u;
            }
            for (UINT i = 0; i < kTileWords; ++i) {
                bad_b += back[4 * kTileWords + i] != p3(i) ? 1u : 0u;
                bad_a += back[5 * kTileWords + i] != p1(kTileWords + i) ? 1u : 0u;
            }
            checkf(bad == 0 && bad_b == 0 && bad_a == 0,
                   "tiled: after the remap the buffer reads the new words in tile 1 and the old ones elsewhere, heap B "
                   "tile 0 holds the new words, heap A tile 1 the old ones (%u, %u, %u words differ)",
                   bad, bad_b, bad_a);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
    }

    // 3. CopyTileMappings: the second buffer takes the mappings of buffer tiles 2-3.
    {
        const D3D12DDI_TILED_RESOURCE_COORDINATE tile2{2, 0, 0, 0};
        const D3D12DDI_TILE_REGION_SIZE two{2, FALSE, 0, 0, 0};
        hr = engine_ddi::copy_tile_mappings(queue, buffer2.hres(), &origin, buffer.hres(), &tile2, &two,
                                            D3D12DDI_TILE_MAPPING_FLAG_NONE);
        checkf(hr == S_OK, "tiled: copy_tile_mappings gives the second buffer the mappings of buffer tiles 2-3 (hr %08lx)",
               static_cast<unsigned long>(hr));
        reopen(env, rec);
        alias();
        barrier(buffer2, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnCopyBufferRegion(rec.hlist(), at(readback, 0), at(buffer2, 0), 2 * UINT64{kTile});
        if (const UINT32* back = run(env, device, queue, rec, readback, "tiled copy mappings")) {
            UINT bad = 0;
            for (UINT i = 0; i < 2 * kTileWords; ++i) bad += back[i] != p1(2 * kTileWords + i) ? 1u : 0u;
            checkf(bad == 0, "tiled: the second buffer reads buffer tiles 2-3 word for word (%u of %u differ)", bad,
                   2 * kTileWords);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
    }

    // 4. CopyTiles: texture -> linear buffer -> second texture (heap B tiles 1-4) -> readback.
    {
        const UINT start = 1;
        hr = engine_ddi::update_tile_mappings(queue, texture2.hres(), 1, &origin, &box2x2, heap_b.hheap(), 1, &none,
                                              &start, &count4, D3D12DDI_TILE_MAPPING_FLAG_NONE);
        checkf(hr == S_OK, "tiled: update_tile_mappings maps the second texture to heap B tiles 1-4 (hr %08lx)",
               static_cast<unsigned long>(hr));
        reopen(env, rec);
        barrier(linear, D3D12DDI_RESOURCE_STATE_COMMON, D3D12DDI_RESOURCE_STATE_COPY_DEST);
        t.pfnCopyTiles(rec.hlist(), texture.hres(), &origin, &four, linear.hres(), 0,
                       D3D12DDI_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER);
        barrier(linear, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        t.pfnCopyTiles(rec.hlist(), texture2.hres(), &origin, &four, linear.hres(), 0,
                       D3D12DDI_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE);
        barrier(texture2, D3D12DDI_RESOURCE_STATE_COPY_DEST, D3D12DDI_RESOURCE_STATE_COPY_SOURCE);
        const D3D12DDIARG_BUFFER_PLACEMENT out = at(readback, 0), tex2 = at(texture2, 0);
        t.pfnCopyTextureRegion(rec.hlist(), &out, pitched, 0, 0, 0, &tex2, subresource, nullptr);
        t.pfnCopyBufferRegion(rec.hlist(), at(readback, 4 * UINT64{kTile}), at(linear, 0), 4 * UINT64{kTile});
        if (const UINT32* back = run(env, device, queue, rec, readback, "tiled copy tiles")) {
            const UINT bad_texture = texture_mismatches(back);
            // The linear buffer: tiles in region order (x, then y), each one's 128 x 128 texels row by row.
            UINT bad_linear = 0;
            for (UINT tile = 0; tile < 4; ++tile)
                for (UINT w = 0; w < kTileWords; ++w) {
                    const UINT x = (tile % 2) * kTexTile + w % kTexTile, y = (tile / 2) * kTexTile + w / kTexTile;
                    bad_linear += back[4 * kTileWords + tile * kTileWords + w] != p2(x, y) ? 1u : 0u;
                }
            checkf(bad_texture == 0 && bad_linear == 0,
                   "tiled: CopyTiles round trip: the second texture reads back as the first, the linear buffer holds "
                   "each tile's texels row by row (%u and %u of %u words differ)",
                   bad_texture, bad_linear, 4 * kTileWords);
            env.core.pfnUnmapHeap(device.h(), readback.hheap());
        }
    }
    // 5. What the engine parts refuse binds nothing and is reported once each: a resource that is not reserved, a
    // heap handle that names no heap, a mapping range without a heap, heap tiles past the heap's end, an unknown
    // flag; for the copy, a source that is not reserved. What the refusals left in GPU memory is not read.
    uint32_t refusals = 0;
    {
        const uint32_t before = device.shell.device_errors;
        const UINT first = 0, past = 5;   // tiles 5-8 of an 8-tile heap
        auto update = [&](D3D12DDI_HRESOURCE r, D3D12DDI_HHEAP h, const UINT* start, UINT flags) {
            return engine_ddi::update_tile_mappings(queue, r, 1, &origin, &four, h, 1, &none, start, &count4,
                                                    static_cast<D3D12DDI_TILE_MAPPING_FLAGS>(flags));
        };
        const HRESULT not_reserved = update(upload.hres(), heap_a.hheap(), &first, 0);
        const HRESULT not_a_heap = update(buffer2.hres(), D3D12DDI_HHEAP{upload.hres().pDrvPrivate}, &first, 0);
        const HRESULT no_heap = update(buffer2.hres(), D3D12DDI_HHEAP{}, &first, 0);
        const HRESULT past_end = update(buffer2.hres(), heap_a.hheap(), &past, 0);
        const HRESULT unknown_flag = update(buffer2.hres(), heap_a.hheap(), &first, 0x80);
        const HRESULT copy_source = engine_ddi::copy_tile_mappings(queue, buffer2.hres(), &origin, upload.hres(), &origin,
                                                                   &four, D3D12DDI_TILE_MAPPING_FLAG_NONE);
        refusals = device.shell.device_errors - before;
        checkf(not_reserved == E_INVALIDARG && not_a_heap == E_INVALIDARG && no_heap == E_INVALIDARG &&
                   past_end == E_INVALIDARG && unknown_flag == E_INVALIDARG && copy_source == E_INVALIDARG &&
                   refusals == 6,
               "tiled: six malformed tile mapping calls are refused and reported once each (%08lx %08lx %08lx %08lx "
               "%08lx %08lx, %u reports)",
               static_cast<unsigned long>(not_reserved), static_cast<unsigned long>(not_a_heap),
               static_cast<unsigned long>(no_heap), static_cast<unsigned long>(past_end),
               static_cast<unsigned long>(unknown_flag), static_cast<unsigned long>(copy_source), refusals);
    }
    destroy_recording(env, device, rec);

    // Reserved resources first, as an application releases them before their heaps; then every allocation must come
    // back through free_memory once.
    for (Buffer* b : {&buffer, &buffer2, &texture, &texture2, &heap_a, &heap_b, &upload, &readback, &linear})
        destroy_buffer(env, device, *b);
    checkf(m.frees == allocations && observed.with_memory == allocations && observed.freed_ok == allocations,
           "tiled: each of the %u allocations came back through free_memory once, after its engine heap (%u freed)",
           allocations, m.frees);
    check(engine_ddi::destroy_engine_queue(queue) == engine_ddi::QueueClose::Retired,
          "tiled: destroy_engine_queue reports Retired");
    engine_ddi::harness_set_release_observer(device.context, nullptr, nullptr);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && device.shell.device_errors == refusals && !device.shell.list_errors,
           "tiled: destroy_device_context S_OK with no live object, no error but the refusals' (hr %08lx, %u live, %u "
           "device, %u list errors)",
           static_cast<unsigned long>(hr), live, device.shell.device_errors, device.shell.list_errors);
}

// Placed textures that only the small placement alignment admits (native trials 093 and 094: a 16x16 texture at
// offset 4096 of a pool heap). The DDI description has no alignment; engine-ddi finds the one the offset needs among
// those the engine grants. Whether the engine grants 4 KB for the description is the device's answer, asked first.
void test_small_placement(Env& env) {
    StubMemory m;
    check(load_stub(env, m), "small placement: GetVulkanHandles and the stub shell's Vulkan entry points");
    if (!m.address) return;
    Device device;
    device.shell.memory = &m;
    HRESULT hr = open_device(env, device, stub_allocate, stub_free);
    checkf(hr == S_OK && device.context, "small placement: device context in RuntimeBacked mode (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;
    Buffer heap;
    hr = create_tile_heap(env, device, 1, heap);
    checkf(hr == S_OK, "small placement: a 64 KiB heap of every category with a buffer over it (hr %08lx)",
           static_cast<unsigned long>(hr));
    if (hr != S_OK) return;

    const auto describe = [](UINT edge, D3D12DDI_RESOURCE_FLAGS_0003 flags) {
        D3D12DDIARG_CREATERESOURCE_0088 res{};
        res.ResourceType = D3D12DDI_RT_TEXTURE2D;
        res.Width = edge;
        res.Height = edge;
        res.DepthOrArraySize = 1;
        res.MipLevels = 1;
        res.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        res.SampleDesc = {1, 0};
        res.Layout = D3D12DDI_TL_UNDEFINED;
        res.Flags = flags;
        res.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
        return res;
    };
    const auto place_at = [&](D3D12DDIARG_CREATERESOURCE_0088 res, UINT64 offset, Buffer& out) {
        out = Buffer{};
        res.ReuseBufferGPUVA.BaseAddress.UMD = {heap.hres(), offset};
        const D3D12DDI_HEAP_AND_RESOURCE_SIZES sizes = env.core.pfnCalcPrivateHeapAndResourceSizes(
            device.h(), nullptr, &res, D3D12DDI_HPROTECTEDRESOURCESESSION_0030{});
        out.resource = env.storage.alloc(sizes.Resource);
        if (!out.resource) return E_OUTOFMEMORY;
        const HRESULT result = env.core.pfnCreateHeapAndResource(device.h(), nullptr, D3D12DDI_HHEAP{},
                                                                 D3D12DDI_HRTRESOURCE{&out.rt}, &res, nullptr,
                                                                 D3D12DDI_HPROTECTEDRESOURCESESSION_0030{}, out.hres());
        if (FAILED(result)) out.resource = nullptr;
        return result;
    };
    // What the engine answers for the 16x16 texture with the small alignment asked explicitly.
    D3D12_RESOURCE_DESC probe{};
    probe.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    probe.Alignment = D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    probe.Width = 16;
    probe.Height = 16;
    probe.DepthOrArraySize = 1;
    probe.MipLevels = 1;
    probe.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    probe.SampleDesc = {1, 0};
    const D3D12_RESOURCE_ALLOCATION_INFO granted = env.engine->GetResourceAllocationInfo(0, 1, &probe);
    const bool grants = granted.Alignment == D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT &&
                        granted.SizeInBytes <= D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    checkf(true, "small placement: the engine %s 4 KiB for a 16x16 R8G8B8A8 texture (%llu bytes aligned to %llu)",
           grants ? "grants" : "does not grant", static_cast<unsigned long long>(granted.SizeInBytes),
           static_cast<unsigned long long>(granted.Alignment));

    const uint32_t errors = device.shell.device_errors;
    Buffer at4k, last, target, large;
    const HRESULT hr_4k = place_at(describe(16, D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE), 4096, at4k);
    const HRESULT hr_last = place_at(describe(16, D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE), 65536 - 4096, last);
    const HRESULT expected = grants ? S_OK : E_INVALIDARG;
    checkf(hr_4k == expected && hr_last == expected,
           "small placement: a 16x16 texture at offset 4096 and in the heap's last 4 KiB: %s (hr %08lx %08lx)",
           grants ? "placed" : "refused, the engine grants no 4 KiB", static_cast<unsigned long>(hr_4k),
           static_cast<unsigned long>(hr_last));
    // A render target never has the small alignment, and 256x256 does not fit in what is left of 64 KiB.
    const auto render_target = static_cast<D3D12DDI_RESOURCE_FLAGS_0003>(D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET |
                                                                         D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE);
    const HRESULT hr_target = place_at(describe(16, render_target), 4096, target);
    const HRESULT hr_large = place_at(describe(256, D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE), 4096, large);
    checkf(hr_target == E_INVALIDARG && hr_large == E_INVALIDARG,
           "small placement: a render target at 4096 and a 256x256 texture at 4096 are refused (hr %08lx %08lx)",
           static_cast<unsigned long>(hr_target), static_cast<unsigned long>(hr_large));
    for (Buffer* b : {&at4k, &last, &target, &large, &heap}) destroy_buffer(env, device, *b);
    uint32_t live = UINT32_MAX;
    hr = engine_ddi::destroy_device_context(device.context, &live);
    checkf(hr == S_OK && live == 0 && m.frees == m.allocations && !device.shell.list_errors,
           "small placement: destroy_device_context S_OK with no live object, every allocation freed (hr %08lx, %u "
           "live, %u of %u freed, %u device errors of which %u before the placements)",
           static_cast<unsigned long>(hr), live, m.frees, m.allocations, device.shell.device_errors, errors);
}

} // namespace harness
