// SPDX-License-Identifier: MIT
// engine-ddi: tiled resources. The mip packing query (D2), CopyTiles (L12), and the engine parts of the shell's queue
// slots UpdateTileMappings and CopyTileMappings (Q3, Q4). Reserved resources themselves are created in resources.cpp
// (pfnCreateHeapAndResource with neither a heap nor a base resource).
//
// The DDI tile structures and flags have the API's layout and values (static_asserts below), so the arrays pass to
// the engine's ID3D12CommandQueue and ID3D12GraphicsCommandList methods unchanged. The heap of a mapping is the
// engine heap of a heap record: in RuntimeBacked mode that is the heap CreateHeapFromMemory made over the shell's
// import of a runtime allocation (resources.cpp), so the tiles are bound to that memory.
#include "internal.h"

namespace engine_ddi {

static_assert(sizeof(D3D12DDI_TILED_RESOURCE_COORDINATE) == sizeof(D3D12_TILED_RESOURCE_COORDINATE) &&
                  offsetof(D3D12DDI_TILED_RESOURCE_COORDINATE, X) == offsetof(D3D12_TILED_RESOURCE_COORDINATE, X) &&
                  offsetof(D3D12DDI_TILED_RESOURCE_COORDINATE, Y) == offsetof(D3D12_TILED_RESOURCE_COORDINATE, Y) &&
                  offsetof(D3D12DDI_TILED_RESOURCE_COORDINATE, Z) == offsetof(D3D12_TILED_RESOURCE_COORDINATE, Z) &&
                  offsetof(D3D12DDI_TILED_RESOURCE_COORDINATE, Subresource) ==
                      offsetof(D3D12_TILED_RESOURCE_COORDINATE, Subresource),
              "tiled resource coordinate layout");
static_assert(sizeof(D3D12DDI_TILE_REGION_SIZE) == sizeof(D3D12_TILE_REGION_SIZE) &&
                  offsetof(D3D12DDI_TILE_REGION_SIZE, NumTiles) == offsetof(D3D12_TILE_REGION_SIZE, NumTiles) &&
                  offsetof(D3D12DDI_TILE_REGION_SIZE, UseBox) == offsetof(D3D12_TILE_REGION_SIZE, UseBox) &&
                  offsetof(D3D12DDI_TILE_REGION_SIZE, Width) == offsetof(D3D12_TILE_REGION_SIZE, Width) &&
                  offsetof(D3D12DDI_TILE_REGION_SIZE, Height) == offsetof(D3D12_TILE_REGION_SIZE, Height) &&
                  offsetof(D3D12DDI_TILE_REGION_SIZE, Depth) == offsetof(D3D12_TILE_REGION_SIZE, Depth),
              "tile region size layout");
static_assert(sizeof(D3D12DDI_TILE_RANGE_FLAGS) == sizeof(D3D12_TILE_RANGE_FLAGS) &&
                  D3D12DDI_TILE_RANGE_FLAG_NONE == static_cast<int>(D3D12_TILE_RANGE_FLAG_NONE) &&
                  D3D12DDI_TILE_RANGE_FLAG_NULL == static_cast<int>(D3D12_TILE_RANGE_FLAG_NULL) &&
                  D3D12DDI_TILE_RANGE_FLAG_SKIP == static_cast<int>(D3D12_TILE_RANGE_FLAG_SKIP) &&
                  D3D12DDI_TILE_RANGE_FLAG_REUSE_SINGLE_TILE == static_cast<int>(D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE),
              "tile range flags");
static_assert(D3D12DDI_TILE_MAPPING_FLAG_NONE == static_cast<int>(D3D12_TILE_MAPPING_FLAG_NONE) &&
                  D3D12DDI_TILE_MAPPING_FLAG_NO_HAZARD == static_cast<int>(D3D12_TILE_MAPPING_FLAG_NO_HAZARD),
              "tile mapping flags");
static_assert(D3D12DDI_TILE_COPY_FLAG_NONE == static_cast<int>(D3D12_TILE_COPY_FLAG_NONE) &&
                  D3D12DDI_TILE_COPY_FLAG_NO_HAZARD == static_cast<int>(D3D12_TILE_COPY_FLAG_NO_HAZARD) &&
                  D3D12DDI_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE ==
                      static_cast<int>(D3D12_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE) &&
                  D3D12DDI_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER ==
                      static_cast<int>(D3D12_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER),
              "tile copy flags");

namespace {
constexpr uint64_t kTileBytes = D3D12_TILED_RESOURCE_TILE_SIZE_IN_BYTES;

ResourceRecord* reserved_of(D3D12DDI_HRESOURCE h, const DeviceContext* c) noexcept {
    auto* r = record_of<ResourceRecord>(h.pDrvPrivate, Tag::Resource, c);
    return (r && r->kind == ResourceKind::Reserved) ? r : nullptr;
}

// The tiles of all regions of an UpdateTileMappings call. Without sizes each region is one tile, and a single region
// without a start is the whole resource (the API's defaults, which vkd3d-proton applies the same way).
uint64_t region_tiles(ID3D12Device* device, ID3D12Resource* resource, UINT region_count,
                      const D3D12DDI_TILED_RESOURCE_COORDINATE* starts, const D3D12DDI_TILE_REGION_SIZE* sizes) noexcept {
    if (sizes) {
        uint64_t tiles = 0;
        for (UINT r = 0; r < region_count; ++r) tiles += sizes[r].NumTiles;
        return tiles;
    }
    if (starts) return region_count;
    UINT total = 0;
    UINT no_subresources = 0;           // the last argument is annotated _Out_writes_, not optional
    D3D12_SUBRESOURCE_TILING unused{};
    device->GetResourceTiling(resource, &total, nullptr, nullptr, &no_subresources, 0, &unused);
    return total;
}

// The checks engine-ddi can make before the engine binds anything. The heap is needed by every range that maps
// memory (neither NULL nor SKIP), and the heap tiles such a range uses must lie in the heap. Without tile counts the
// first range covers every tile of the regions and the others are never reached (vkd3d-proton's reading of a NULL
// pRangeTileCounts). Region bounds against the resource are the engine's: it logs and drops a tile out of bounds.
HRESULT check_ranges(const Backing* heap, uint64_t tiles_in_regions, UINT range_count,
                     const D3D12DDI_TILE_RANGE_FLAGS* range_flags, const UINT* starts, const UINT* counts) noexcept {
    constexpr UINT known = D3D12DDI_TILE_RANGE_FLAG_NULL | D3D12DDI_TILE_RANGE_FLAG_SKIP |
                           D3D12DDI_TILE_RANGE_FLAG_REUSE_SINGLE_TILE;
    const UINT checked = counts ? range_count : (range_count ? 1u : 0u);
    for (UINT i = 0; i < checked; ++i) {
        const UINT flag = range_flags ? static_cast<UINT>(range_flags[i]) : 0u;
        if (flag & ~known) return E_INVALIDARG;
        if (flag == D3D12DDI_TILE_RANGE_FLAG_NULL || flag == D3D12DDI_TILE_RANGE_FLAG_SKIP) continue;
        if (flag & (D3D12DDI_TILE_RANGE_FLAG_NULL | D3D12DDI_TILE_RANGE_FLAG_SKIP)) return E_INVALIDARG;   // mixed
        if (!heap) return E_INVALIDARG;
        uint64_t tiles = counts ? counts[i] : tiles_in_regions;
        if ((flag & D3D12DDI_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) && tiles) tiles = 1;   // one heap tile, repeated
        const uint64_t start = starts ? starts[i] : 0;
        if ((start + tiles) * kTileBytes > heap->desc.ByteSize) return E_INVALIDARG;
    }
    return S_OK;
}

// One tile mapping on the queue, under its submission lock, then the retirement signal (engine-ddi.h,
// update_tile_mappings). call issues the engine's UpdateTileMappings or CopyTileMappings; in INLINE mode the engine
// submits its sparse bind before the call returns (vkd3d-proton d3d12_command_queue_process_inline_locked).
template <class Call> HRESULT map_on_queue(EngineQueue* q, Call call) noexcept {
    DeviceContext* c = q->context;
    AcquireSRWLockExclusive(&q->submit_lock);
    call(q->queue);
    const HRESULT hr = submit_locked(q, 0, nullptr);
    ReleaseSRWLockExclusive(&q->submit_lock);
    if (FAILED(hr)) {
        c->report(hr);
        return hr;
    }
    c->process_retired();
    return S_OK;
}

// ---- D2 pfnGetMipPacking ------------------------------------------------------------------------------------------
// The packed part of the engine's GetResourceTiling. The runtime derives the rest of the API's GetResourceTiling
// (standard tile shapes, tiles per subresource) itself (INFERENCE: the DDI has no other tiling query). A resource
// that is not reserved has nothing packed: 0 and 0, not an error.
void APIENTRY get_mip_packing(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE hres, UINT* packed_mips,
                              UINT* packed_tiles) {
    if (packed_mips) *packed_mips = 0;
    if (packed_tiles) *packed_tiles = 0;
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r || !packed_mips || !packed_tiles) {
        c->report(E_INVALIDARG);
        return;
    }
    if (r->kind != ResourceKind::Reserved) return;
    D3D12_PACKED_MIP_INFO packed{};
    UINT no_subresources = 0;
    D3D12_SUBRESOURCE_TILING unused{};
    c->device->GetResourceTiling(static_cast<ID3D12Resource*>(r->h.engine), nullptr, &packed, nullptr,
                                 &no_subresources, 0, &unused);
    *packed_mips = packed.NumPackedMips;
    *packed_tiles = packed.NumTilesForPackedMips;
}

// ---- L12 pfnCopyTiles -----------------------------------------------------------------------------------------------
// The tiled resource must be reserved and the other one a buffer, both of the list's device. At most one direction
// flag: none copies from the tiles to the buffer, as the API does.
void APIENTRY copy_tiles(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HRESOURCE htiled,
                         const D3D12DDI_TILED_RESOURCE_COORDINATE* start, const D3D12DDI_TILE_REGION_SIZE* size,
                         D3D12DDI_HRESOURCE hbuffer, UINT64 buffer_offset, D3D12DDI_TILE_COPY_FLAGS flags) {
    CommandListRecord* l = list_of(hlist, "CopyTiles");
    if (!l) return;
    constexpr UINT to_tiles = D3D12DDI_TILE_COPY_FLAG_LINEAR_BUFFER_TO_SWIZZLED_TILED_RESOURCE;
    constexpr UINT to_buffer = D3D12DDI_TILE_COPY_FLAG_SWIZZLED_TILED_RESOURCE_TO_LINEAR_BUFFER;
    const UINT f = static_cast<UINT>(flags);
    ResourceRecord* tiled = reserved_of(htiled, l->h.device);
    auto* buffer = record_of<ResourceRecord>(hbuffer.pDrvPrivate, Tag::Resource, l->h.device);
    if (!tiled || !buffer || buffer->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || !start || !size ||
        (f & ~(D3D12DDI_TILE_COPY_FLAG_NO_HAZARD | to_tiles | to_buffer)) || ((f & to_tiles) && (f & to_buffer))) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->CopyTiles(static_cast<ID3D12Resource*>(tiled->h.engine),
                         reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(start),
                         reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(size),
                         static_cast<ID3D12Resource*>(buffer->h.engine), buffer_offset,
                         static_cast<D3D12_TILE_COPY_FLAGS>(f));
}
} // namespace

// ---- Q3 and Q4, the engine parts --------------------------------------------------------------------------------------
HRESULT update_tile_mappings(EngineQueue* q, D3D12DDI_HRESOURCE hres, UINT region_count,
                             const D3D12DDI_TILED_RESOURCE_COORDINATE* region_starts,
                             const D3D12DDI_TILE_REGION_SIZE* region_sizes, D3D12DDI_HHEAP hheap, UINT range_count,
                             const D3D12DDI_TILE_RANGE_FLAGS* range_flags, const UINT* heap_range_starts,
                             const UINT* range_tile_counts, D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept {
    if (!q) return E_INVALIDARG;
    DeviceContext* c = q->context;
    ResourceRecord* r = reserved_of(hres, c);
    const HeapRecord* h = hheap.pDrvPrivate ? record_of<HeapRecord>(hheap.pDrvPrivate, Tag::Heap, c) : nullptr;
    // The memory of a linear primary holds that image alone: no tile is mapped from it.
    HRESULT hr = (!r || (hheap.pDrvPrivate && !h) || (h && h->backing->linear) ||
                  (region_count && !region_starts && region_count != 1) ||
                  (static_cast<UINT>(flags) & ~static_cast<UINT>(D3D12DDI_TILE_MAPPING_FLAG_NO_HAZARD)))
                     ? E_INVALIDARG
                     : check_ranges(h ? h->backing : nullptr,
                                    region_tiles(c->device, static_cast<ID3D12Resource*>(r->h.engine), region_count,
                                                 region_starts, region_sizes),
                                    range_count, range_flags, heap_range_starts, range_tile_counts);
    if (FAILED(hr)) {
        log_line("UpdateTileMappings: refused (%u regions, %u ranges, heap %p)", region_count, range_count,
                 hheap.pDrvPrivate);
        c->report(hr);
        return hr;
    }
    ID3D12Heap* heap = h ? h->backing->heap : nullptr;
    return map_on_queue(q, [&](ID3D12CommandQueue* queue) {
        queue->UpdateTileMappings(static_cast<ID3D12Resource*>(r->h.engine), region_count,
                                  reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(region_starts),
                                  reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(region_sizes), heap, range_count,
                                  reinterpret_cast<const D3D12_TILE_RANGE_FLAGS*>(range_flags), heap_range_starts,
                                  range_tile_counts, static_cast<D3D12_TILE_MAPPING_FLAGS>(flags));
    });
}

HRESULT copy_tile_mappings(EngineQueue* q, D3D12DDI_HRESOURCE hdst, const D3D12DDI_TILED_RESOURCE_COORDINATE* dst_start,
                           D3D12DDI_HRESOURCE hsrc, const D3D12DDI_TILED_RESOURCE_COORDINATE* src_start,
                           const D3D12DDI_TILE_REGION_SIZE* size, D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept {
    if (!q) return E_INVALIDARG;
    DeviceContext* c = q->context;
    ResourceRecord* dst = reserved_of(hdst, c);
    ResourceRecord* src = reserved_of(hsrc, c);
    if (!dst || !src || !dst_start || !src_start || !size || !size->NumTiles ||
        (static_cast<UINT>(flags) & ~static_cast<UINT>(D3D12DDI_TILE_MAPPING_FLAG_NO_HAZARD))) {
        log_line("CopyTileMappings: refused");
        c->report(E_INVALIDARG);
        return E_INVALIDARG;
    }
    return map_on_queue(q, [&](ID3D12CommandQueue* queue) {
        queue->CopyTileMappings(static_cast<ID3D12Resource*>(dst->h.engine),
                                reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(dst_start),
                                static_cast<ID3D12Resource*>(src->h.engine),
                                reinterpret_cast<const D3D12_TILED_RESOURCE_COORDINATE*>(src_start),
                                reinterpret_cast<const D3D12_TILE_REGION_SIZE*>(size),
                                static_cast<D3D12_TILE_MAPPING_FLAGS>(flags));
    });
}

void fill_core_tiles(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept { t->pfnGetMipPacking = get_mip_packing; }

void fill_list_tiles(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t) noexcept { t->pfnCopyTiles = copy_tiles; }

} // namespace engine_ddi
