// SPDX-License-Identifier: MIT
// engine-ddi: heaps and resources (D59-D61, D57-D58, D74, D75, D84, D0, D1), reserved resources (the creation;
// their tile slots are in tiles.cpp), resource_allocation, object_allocation, and the list slots that move data
// between resources (L10, L13, L17).
#include "internal.h"
#include "format-list.h"
#include <algorithm>

namespace engine_ddi {

// DDI and API enums that are passed through by value.
static_assert(D3D12DDI_RT_BUFFER == static_cast<int>(D3D12_RESOURCE_DIMENSION_BUFFER) &&
              D3D12DDI_RT_TEXTURE1D == static_cast<int>(D3D12_RESOURCE_DIMENSION_TEXTURE1D) &&
              D3D12DDI_RT_TEXTURE2D == static_cast<int>(D3D12_RESOURCE_DIMENSION_TEXTURE2D) &&
              D3D12DDI_RT_TEXTURE3D == static_cast<int>(D3D12_RESOURCE_DIMENSION_TEXTURE3D), "resource dimension");
static_assert(D3D12DDI_TL_UNDEFINED == static_cast<int>(D3D12_TEXTURE_LAYOUT_UNKNOWN) &&
              D3D12DDI_TL_ROW_MAJOR == static_cast<int>(D3D12_TEXTURE_LAYOUT_ROW_MAJOR) &&
              D3D12DDI_TL_64KB_TILE_UNDEFINED_SWIZZLE == static_cast<int>(D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE) &&
              D3D12DDI_TL_64KB_TILE_STANDARD_SWIZZLE == static_cast<int>(D3D12_TEXTURE_LAYOUT_64KB_STANDARD_SWIZZLE),
              "texture layout");
static_assert(static_cast<unsigned>(D3D12DDI_BARRIER_LAYOUT_UNDEFINED) == static_cast<unsigned>(D3D12_BARRIER_LAYOUT_UNDEFINED) &&
              D3D12DDI_BARRIER_LAYOUT_COMMON == static_cast<int>(D3D12_BARRIER_LAYOUT_COMMON) &&
              D3D12DDI_BARRIER_LAYOUT_RENDER_TARGET == static_cast<int>(D3D12_BARRIER_LAYOUT_RENDER_TARGET) &&
              D3D12DDI_BARRIER_LAYOUT_COPY_DEST == static_cast<int>(D3D12_BARRIER_LAYOUT_COPY_DEST) &&
              D3D12DDI_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON == static_cast<int>(D3D12_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON),
              "barrier layout");
static_assert(D3D12DDI_RESOURCE_STATE_COMMON == static_cast<int>(D3D12_RESOURCE_STATE_COMMON) &&
              D3D12DDI_RESOURCE_STATE_RENDER_TARGET == static_cast<int>(D3D12_RESOURCE_STATE_RENDER_TARGET) &&
              D3D12DDI_RESOURCE_STATE_UNORDERED_ACCESS == static_cast<int>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
              D3D12DDI_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE == static_cast<int>(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) &&
              D3D12DDI_RESOURCE_STATE_PIXEL_SHADER_RESOURCE == static_cast<int>(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) &&
              D3D12DDI_RESOURCE_STATE_COPY_DEST == static_cast<int>(D3D12_RESOURCE_STATE_COPY_DEST) &&
              D3D12DDI_RESOURCE_STATE_COPY_SOURCE == static_cast<int>(D3D12_RESOURCE_STATE_COPY_SOURCE) &&
              D3D12DDI_RESOURCE_STATE_RESOLVE_SOURCE == static_cast<int>(D3D12_RESOURCE_STATE_RESOLVE_SOURCE) &&
              D3D12DDI_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE ==
                  static_cast<int>(D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE),
              "resource states");
static_assert(sizeof(D3D12DDI_CLEAR_VALUES) == sizeof(D3D12_CLEAR_VALUE) &&
              offsetof(D3D12DDI_CLEAR_VALUES, Color) == offsetof(D3D12_CLEAR_VALUE, Color), "clear value layout");
static_assert(D3D12DDI_RESOURCE_BARRIER_FLAG_BEGIN_ONLY == static_cast<int>(D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY) &&
              D3D12DDI_RESOURCE_BARRIER_FLAG_END_ONLY == static_cast<int>(D3D12_RESOURCE_BARRIER_FLAG_END_ONLY),
              "barrier flags");

namespace {
// A heap ByteSize that names no size. H and DDI-ref give ByteSize as "Size of the heap, in bytes" and define no
// such value; engine-ddi accepts it only together with a resource description, as the size that resource needs
// (INTEGRATION.md, "Heap size left to the resource").
constexpr uint64_t kSizeOfResource = UINT64_MAX;

constexpr uint32_t kCategoryMask =
    D3D12DDI_HEAP_FLAG_BUFFERS | D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES | D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;

HRESULT to_api_desc(const D3D12DDIARG_CREATERESOURCE_0088& in, D3D12_RESOURCE_DESC1& out) noexcept {
    out = D3D12_RESOURCE_DESC1{};
    if (in.ResourceType < D3D12DDI_RT_BUFFER || in.ResourceType > D3D12DDI_RT_TEXTURE3D) return E_INVALIDARG;
    if (in.Layout > D3D12DDI_TL_64KB_TILE_STANDARD_SWIZZLE) return E_INVALIDARG;
    if (in.pRowMajorLayout) {                           // a custom row-major layout has no API form
        // The pointee is not read: it is refused unread, whatever Layout says about its meaning.
        log_line("resource description: row-major layout given (type %d, layout %d): E_NOTIMPL",
                 static_cast<int>(in.ResourceType), static_cast<int>(in.Layout));
        return E_NOTIMPL;
    }
    out.Dimension = static_cast<D3D12_RESOURCE_DIMENSION>(in.ResourceType);
    out.Width = in.Width;
    out.Height = in.Height;
    out.DepthOrArraySize = in.DepthOrArraySize;
    out.MipLevels = in.MipLevels;
    out.Format = in.Format;
    out.SampleDesc = in.SampleDesc;
    out.Layout = static_cast<D3D12_TEXTURE_LAYOUT>(in.Layout);
    const UINT f = in.Flags;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET) flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL) flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (f & D3D12DDI_RESOURCE_FLAG_0022_UNORDERED_ACCESS) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if ((f & D3D12DDI_RESOURCE_FLAG_0003_DEPTH_STENCIL) && !(f & D3D12DDI_RESOURCE_FLAG_0003_SHADER_RESOURCE))
        flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_CROSS_ADAPTER) flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    if (f & D3D12DDI_RESOURCE_FLAG_0003_SIMULTANEOUS_ACCESS) flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
    if (f & D3D12DDI_RESOURCE_FLAG_0020_VIDEO_DECODE_REFERENCE_ONLY) flags |= D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY;
    if (f & D3D12DDI_RESOURCE_FLAG_0080_VIDEO_ENCODE_REFERENCE_ONLY) flags |= D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY;
    if (f & D3D12DDI_RESOURCE_FLAG_0088_RAYTRACING_ACCELERATION_STRUCTURE)
        flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
    out.Flags = flags;
    out.SamplerFeedbackMipRegion = {in.SamplerFeedbackMipRegion.Width, in.SamplerFeedbackMipRegion.Height,
                                    in.SamplerFeedbackMipRegion.Depth};
    return S_OK;
}

D3D12_RESOURCE_DESC to_desc0(const D3D12_RESOURCE_DESC1& d) noexcept {
    return {d.Dimension, d.Alignment, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, d.SampleDesc,
            d.Layout, d.Flags};
}

uint32_t category_of(const D3D12_RESOURCE_DESC1& d) noexcept {
    if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) return D3D12DDI_HEAP_FLAG_BUFFERS;
    if (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        return D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    return D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES;
}

HRESULT heap_desc_of(const D3D12DDIARG_CREATEHEAP_0001& in, D3D12_HEAP_DESC& out) noexcept {
    if (in.CPUPageProperty > D3D12DDI_CPU_PAGE_PROPERTY_WRITE_BACK || in.MemoryPool > D3D12DDI_MEMORY_POOL_L1 ||
        !in.ByteSize || !(in.Flags & kCategoryMask))
        return E_INVALIDARG;
    out = D3D12_HEAP_DESC{};
    out.SizeInBytes = in.ByteSize;
    out.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    out.Properties.CPUPageProperty = static_cast<D3D12_CPU_PAGE_PROPERTY>(in.CPUPageProperty + 1);
    out.Properties.MemoryPoolPreference = static_cast<D3D12_MEMORY_POOL>(in.MemoryPool + 1);
    out.Properties.CreationNodeMask = 1;
    out.Properties.VisibleNodeMask = 1;
    out.Alignment = in.Alignment;
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    if (!(in.Flags & D3D12DDI_HEAP_FLAG_BUFFERS)) flags |= D3D12_HEAP_FLAG_DENY_BUFFERS;
    if (!(in.Flags & D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES)) flags |= D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
    if (!(in.Flags & D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES)) flags |= D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    out.Flags = flags;
    return S_OK;
}

// The initial layout of a create: a buffer's is UNDEFINED. The DDI-only LEGACY_* layouts name a legacy initial state:
// S_OK and *state for one of them, S_FALSE for a barrier layout the engine takes as it is, E_INVALIDARG for an
// unknown DDI-only layout.
HRESULT initial_layout(const D3D12_RESOURCE_DESC1& desc, D3D12DDI_BARRIER_LAYOUT* layout,
                       D3D12_RESOURCE_STATES* state) noexcept {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) *layout = D3D12DDI_BARRIER_LAYOUT_UNDEFINED;
    if (static_cast<uint32_t>(*layout) < 0x80000000u || *layout == D3D12DDI_BARRIER_LAYOUT_UNDEFINED) return S_FALSE;
    switch (*layout) {
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_SOURCE: *state = D3D12_RESOURCE_STATE_COPY_SOURCE; return S_OK;
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_DEST: *state = D3D12_RESOURCE_STATE_COPY_DEST; return S_OK;
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_SHADER_RESOURCE:
        *state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return S_OK;
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_PIXEL_SHADER_RESOURCE:
        *state = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        return S_OK;
    default: return E_INVALIDARG;
    }
}

const D3D12_CLEAR_VALUE* clear_value(const D3D12_RESOURCE_DESC1& desc, const D3D12DDI_CLEAR_VALUES* clear) noexcept {
    const bool target = desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    return (clear && target) ? reinterpret_cast<const D3D12_CLEAR_VALUE*>(clear) : nullptr;
}

// The engine's placed resource at offset in the backing (engine-ddi.h placement rules, D3D12 part).
HRESULT place(DeviceContext* c, Backing* b, uint64_t offset, const D3D12_RESOURCE_DESC1& desc,
              D3D12DDI_BARRIER_LAYOUT layout, const D3D12DDI_CLEAR_VALUES* clear, UINT castable_count,
              const DXGI_FORMAT* castable, ID3D12Resource** out) noexcept {
    *out = nullptr;
    if (!(b->desc.Flags & category_of(desc))) return E_INVALIDARG;
    const D3D12_RESOURCE_DESC d0 = to_desc0(desc);
    const D3D12_RESOURCE_ALLOCATION_INFO info = c->device->GetResourceAllocationInfo(0, 1, &d0);
    if (info.SizeInBytes == UINT64_MAX || !info.Alignment) return E_INVALIDARG;
    if (offset % info.Alignment || offset > b->desc.ByteSize || info.SizeInBytes > b->desc.ByteSize - offset)
        return E_INVALIDARG;
    const D3D12_CLEAR_VALUE* cv = clear_value(desc, clear);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const HRESULT legacy = initial_layout(desc, &layout, &state);
    if (FAILED(legacy)) return legacy;
    if (legacy == S_OK) {
        if (castable_count) {                           // CreatePlacedResource1 takes no castable formats
            log_line("placed resource: %u castable formats with a legacy initial state: E_NOTIMPL", castable_count);
            return E_NOTIMPL;
        }
        return c->device8->CreatePlacedResource1(b->heap, offset, &desc, state, cv, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(out));
    }
    return c->device10->CreatePlacedResource2(b->heap, offset, &desc, static_cast<D3D12_BARRIER_LAYOUT>(layout), cv,
                                              castable_count, castable, __uuidof(ID3D12Resource),
                                              reinterpret_cast<void**>(out));
}

// ---- The linear primary (engine ABI 1.3 V13) ----------------------------------------------------------------------
// A committed texture on a heap with D3D12DDI_HEAP_FLAG_PRIMARY is what the desktop compositor opens and reads by
// row pitch, so its image has linear tiling. Only the PRIMARY flags select this: the heap's at the create and the
// optimization flag at CheckResourceAllocationInfo. No description becomes linear by its shape.
struct LinearSurface {
    BC250_VKD3D_LINEAR_IMAGE_INFO info;
    uint64_t backing_size;
};

inline constexpr uint32_t kLinearMaxEdge = 8192;
inline constexpr uint64_t kLinearPage = 4096;

DXGI_FORMAT srgb_sibling(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

// The descriptions the linear primary exists for. The engine's image has the format's own compatibility list, so
// castable formats beyond the format and its sRGB sibling are not these.
bool linear_primary_shape(const D3D12_RESOURCE_DESC1& desc, const D3D12DDIARG_CREATERESOURCE_0088& in) noexcept {
    const DXGI_FORMAT sibling = srgb_sibling(desc.Format);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sibling == DXGI_FORMAT_UNKNOWN) return false;
    if (!desc.Width || desc.Width > kLinearMaxEdge || !desc.Height || desc.Height > kLinearMaxEdge) return false;
    if (desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality)
        return false;
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN && desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) return false;
    if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
                      D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
                      D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE))
        return false;
    for (UINT i = 0; i < in.NumCastableFormats; ++i)
        if (in.pCastableFormats[i] != desc.Format && in.pCastableFormats[i] != sibling) return false;
    return true;
}

// The description the engine is asked about and creates: the linear request is the call, not the layout field.
D3D12_RESOURCE_DESC1 linear_desc(D3D12_RESOURCE_DESC1 desc) noexcept {
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Alignment = 0;
    return desc;
}

// What the engine's linear image of desc needs, before any memory exists, and the size of the backing: enough for
// the image's memory requirement and for a reader that takes pitch * (height rounded up to 4) bytes.
HRESULT query_linear_primary(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc, LinearSurface* out) noexcept {
    *out = LinearSurface{};
    if (!c->funcs.QueryLinearImage) return E_NOTIMPL;
    const D3D12_RESOURCE_DESC1 d = linear_desc(desc);
    BC250_VKD3D_LINEAR_IMAGE_INFO info{};
    info.Size = sizeof(info);
    const HRESULT hr = c->funcs.QueryLinearImage(c->device, &d, &info);
    if (FAILED(hr)) {
        log_line("linear primary: the engine has no linear image for format %d, %llux%u: %08lx",
                 static_cast<int>(desc.Format), static_cast<unsigned long long>(desc.Width), desc.Height,
                 static_cast<unsigned long>(hr));
        return hr;
    }
    const uint64_t width4 = (desc.Width + 3) & ~3ull;
    const uint64_t height4 = (static_cast<uint64_t>(desc.Height) + 3) & ~3ull;
    if (info.Offset || !info.RowPitch || info.RowPitch > UINT32_MAX || info.RowPitch % 16 ||
        info.RowPitch < width4 * 4 || !info.MemorySize || !info.MemoryAlignment || !info.MemoryTypeBits ||
        info.MemoryAlignment > UINT32_MAX || (info.MemoryAlignment & (info.MemoryAlignment - 1)) ||
        info.LayoutSize > info.MemorySize) {
        log_line("linear primary: layout not usable (offset %llu, pitch %llu, layout %llu, memory %llu, alignment "
                 "%llu, types %08x)",
                 static_cast<unsigned long long>(info.Offset), static_cast<unsigned long long>(info.RowPitch),
                 static_cast<unsigned long long>(info.LayoutSize), static_cast<unsigned long long>(info.MemorySize),
                 static_cast<unsigned long long>(info.MemoryAlignment), info.MemoryTypeBits);
        return E_NOTIMPL;
    }
    // pitch <= UINT32_MAX and height4 <= 8192: the product cannot overflow 64 bits.
    const uint64_t rows = info.RowPitch * height4;
    const uint64_t need = std::max<uint64_t>(rows, info.MemorySize);
    if (need > UINT32_MAX - kLinearPage) return E_NOTIMPL; // the surface description has 32-bit fields
    out->info = info;
    out->backing_size = (need + kLinearPage - 1) & ~(kLinearPage - 1);
    return S_OK;
}

// The linear image at offset 0 of the backing. The bound image must be the one the query described: the memory was
// sized, typed and described to the shell from that answer.
HRESULT place_linear(DeviceContext* c, Backing* b, const D3D12_RESOURCE_DESC1& desc, D3D12DDI_BARRIER_LAYOUT layout,
                     const D3D12DDI_CLEAR_VALUES* clear, const LinearSurface& surface, ID3D12Resource** out) noexcept {
    *out = nullptr;
    if (!c->funcs.CreateLinearPlacedResource) return E_NOTIMPL;
    const D3D12_RESOURCE_DESC1 d = linear_desc(desc);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const HRESULT legacy = initial_layout(desc, &layout, &state);
    if (FAILED(legacy)) return legacy;
    // A barrier layout has no state form here. The engine keeps a linear image in one Vulkan layout whatever
    // the initial state, so COMMON stands for it.
    BC250_VKD3D_LINEAR_IMAGE_INFO bound{};
    bound.Size = sizeof(bound);
    ID3D12Resource* engine = nullptr;
    const HRESULT hr = c->funcs.CreateLinearPlacedResource(c->device, b->heap, 0, &d, static_cast<UINT32>(state),
                                                           clear_value(desc, clear), __uuidof(ID3D12Resource),
                                                           reinterpret_cast<void**>(&engine), &bound);
    if (FAILED(hr)) return hr;
    if (!engine) return E_UNEXPECTED;
    const BC250_VKD3D_LINEAR_IMAGE_INFO& q = surface.info;
    if (bound.Offset != q.Offset || bound.RowPitch != q.RowPitch || bound.LayoutSize != q.LayoutSize ||
        bound.MemorySize != q.MemorySize || bound.MemoryAlignment != q.MemoryAlignment ||
        bound.MemoryTypeBits != q.MemoryTypeBits) {
        log_line("linear primary: the bound image differs from the query (pitch %llu/%llu, layout %llu/%llu, memory "
                 "%llu/%llu)",
                 static_cast<unsigned long long>(bound.RowPitch), static_cast<unsigned long long>(q.RowPitch),
                 static_cast<unsigned long long>(bound.LayoutSize), static_cast<unsigned long long>(q.LayoutSize),
                 static_cast<unsigned long long>(bound.MemorySize), static_cast<unsigned long long>(q.MemorySize));
        engine->Release();
        return E_FAIL;
    }
    *out = engine;
    return S_OK;
}

// The engine's reserved (tiled) resource: no memory until UpdateTileMappings maps heap tiles into it (tiles.cpp).
// The engine refuses a reserved texture when its tiled resources tier is 0; a format it cannot make sparse becomes
// its committed fallback, on which tile mappings are ignored (vkd3d-proton d3d12_resource_create_reserved).
HRESULT reserve(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc, D3D12DDI_BARRIER_LAYOUT layout,
                const D3D12DDI_CLEAR_VALUES* clear, UINT castable_count, const DXGI_FORMAT* castable,
                ID3D12Resource** out) noexcept {
    *out = nullptr;
    const D3D12_RESOURCE_DESC d0 = to_desc0(desc);
    const D3D12_CLEAR_VALUE* cv = clear_value(desc, clear);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const HRESULT legacy = initial_layout(desc, &layout, &state);
    if (FAILED(legacy)) return legacy;
    if (legacy == S_OK) {
        if (castable_count) {                           // CreateReservedResource1 takes no castable formats
            log_line("reserved resource: %u castable formats with a legacy initial state: E_NOTIMPL", castable_count);
            return E_NOTIMPL;
        }
        return c->device4->CreateReservedResource1(&d0, state, cv, nullptr, __uuidof(ID3D12Resource),
                                                   reinterpret_cast<void**>(out));
    }
    return c->device10->CreateReservedResource2(&d0, static_cast<D3D12_BARRIER_LAYOUT>(layout), cv, nullptr,
                                                castable_count, castable, __uuidof(ID3D12Resource),
                                                reinterpret_cast<void**>(out));
}

// The backing and its release record, allocated together while the create can still fail (internal.h,
// PendingRelease).
Backing* new_backing(DeviceContext* c, const D3D12DDIARG_CREATEHEAP_0001& desc, ID3D12Heap* heap) noexcept {
    auto* node = make_new<PendingRelease>();
    if (!node) return nullptr;
    auto* b = make_new<Backing>();
    if (!b) {
        delete node;
        return nullptr;
    }
    b->release_node = node;
    b->refs.store(1);
    b->device = c;
    b->heap = heap;
    b->desc = desc;
    b->id = c->next_id.fetch_add(1);
    return b;
}

// An engine heap over the shell's memory (engine ABI 1.2 V10). The memory stays the shell's: the engine never
// frees, clears or maps it beyond the one mapping of a CPU-visible heap, and CreateHeapFromMemory refuses (with
// E_INVALIDARG, creating nothing) a memory type the engine would not pick for this heap. allocate_memory allocates
// with VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT always (engine-ddi.h, ImportedMemory), hence the flag.
HRESULT engine_heap_from_memory(DeviceContext* c, const D3D12_HEAP_DESC& desc, const ImportedMemory& m,
                                ID3D12Heap** heap) noexcept {
    *heap = nullptr;
    BC250_VKD3D_IMPORTED_MEMORY imported{};
    imported.Size = sizeof(imported);
    imported.Memory = m.memory;
    imported.AllocationSize = m.byte_size;
    imported.MemoryTypeIndex = m.memory_type_index;
    imported.Flags = BC250_VKD3D_IMPORTED_MEMORY_FLAG_DEVICE_ADDRESS;
    return c->funcs.CreateHeapFromMemory(c->device, &imported, &desc, __uuidof(ID3D12Heap),
                                         reinterpret_cast<void**>(heap));
}

void construct_resource(ResourceRecord* r, DeviceContext* c, ID3D12Resource* engine, Backing* b, uint64_t offset,
                        const D3D12_RESOURCE_DESC1& desc, D3D12DDI_HRTRESOURCE rt, ResourceKind kind) noexcept {
    if (b) backing_acquire(b);                          // a reserved resource has none
    new (r) ResourceRecord{{Tag::Resource, 0, engine, c}, b, offset, desc, rt, kind, kInitNone, nullptr, nullptr,
                           0, 0};
    c->live.fetch_add(1);
}

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_heap_and_resource(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001*,
                                                                 const D3D12DDIARG_CREATERESOURCE_0088*,
                                                                 D3D12DDI_HPROTECTEDRESOURCESESSION_0030) {
    return {sizeof(HeapRecord), sizeof(ResourceRecord)};
}

HRESULT create_heap_and_resource(DeviceContext* c, const D3D12DDIARG_CREATEHEAP_0001* heap_desc, D3D12DDI_HHEAP hheap,
                                 D3D12DDI_HRTRESOURCE rt, const D3D12DDIARG_CREATERESOURCE_0088* res_desc,
                                 const D3D12DDI_CLEAR_VALUES* clear, D3D12DDI_HRESOURCE hres) noexcept {
    D3D12_RESOURCE_DESC1 desc{};
    if (res_desc) {
        if (!hres.pDrvPrivate) return E_INVALIDARG;
        HRESULT hr = to_api_desc(*res_desc, desc);
        if (FAILED(hr)) return hr;
        if (res_desc->NumCastableFormats && !res_desc->pCastableFormats) return E_INVALIDARG;
    }
    if (!heap_desc) {
        // Placed: the base resource names the heap memory.
        if (!res_desc) return E_INVALIDARG;
        auto* base = record_of<ResourceRecord>(res_desc->ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate,
                                               Tag::Resource, c);
        if (!base) {
            if (res_desc->ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate || hheap.pDrvPrivate)
                return E_INVALIDARG;
            // Reserved: no heap and no base resource (engine-ddi.h, "Reserved resources"). No memory is allocated.
            ID3D12Resource* engine = nullptr;
            HRESULT hr = reserve(c, desc, res_desc->InitialBarrierLayout, clear, res_desc->NumCastableFormats,
                                 res_desc->pCastableFormats, &engine);
            if (FAILED(hr)) return hr;
            construct_resource(static_cast<ResourceRecord*>(hres.pDrvPrivate), c, engine, nullptr, 0, desc, rt,
                               ResourceKind::Reserved);
            return S_OK;
        }
        if (!base->backing) return E_INVALIDARG;        // a reserved resource names no heap memory
        if (base->backing->linear) {
            log_line("placed: the base is a linear primary, its memory holds that image alone");
            return E_INVALIDARG;
        }
        const uint64_t offset = base->offset + res_desc->ReuseBufferGPUVA.BaseAddress.UMD.Offset;
        if (offset < base->offset) return E_INVALIDARG;
        ID3D12Resource* engine = nullptr;
        HRESULT hr = place(c, base->backing, offset, desc, res_desc->InitialBarrierLayout, clear,
                           res_desc->NumCastableFormats, res_desc->pCastableFormats, &engine);
        if (FAILED(hr)) return hr;
        construct_resource(static_cast<ResourceRecord*>(hres.pDrvPrivate), c, engine, base->backing, offset, desc, rt,
                           ResourceKind::Placed);
        return S_OK;
    }
    if (!hheap.pDrvPrivate) return E_INVALIDARG;
    // The heap description as engine-ddi uses it from here on: a copy, so that a ByteSize of kSizeOfResource can be
    // replaced by a size. Neither the memory request nor the engine ever sees kSizeOfResource.
    D3D12DDIARG_CREATEHEAP_0001 sized = *heap_desc;
    heap_desc = &sized;
    uint64_t need = 0, align = 0;
    LinearSurface surface{};
    // The linear primary: a failure of the query fails the create, no other tiling takes its place.
    const bool linear = res_desc && c->mode == MemoryMode::RuntimeBacked &&
                        (sized.Flags & D3D12DDI_HEAP_FLAG_PRIMARY) &&
                        sized.CPUPageProperty == D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
                        linear_primary_shape(desc, *res_desc);
    if (res_desc) {
        if (!(sized.Flags & category_of(desc))) return E_INVALIDARG;
        if (linear) {
            const HRESULT query = query_linear_primary(c, desc, &surface);
            if (FAILED(query)) return query;
            need = surface.backing_size;
            align = surface.info.MemoryAlignment;
            if (sized.ByteSize != kSizeOfResource && sized.ByteSize < need)
                log_line("linear primary: the heap has %llu bytes, the surface needs %llu",
                         static_cast<unsigned long long>(sized.ByteSize), static_cast<unsigned long long>(need));
        } else {
            const D3D12_RESOURCE_DESC d0 = to_desc0(desc);
            const D3D12_RESOURCE_ALLOCATION_INFO info = c->device->GetResourceAllocationInfo(0, 1, &d0);
            if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes) return E_INVALIDARG;
            need = info.SizeInBytes;
            align = info.Alignment;
        }
        if (sized.ByteSize == kSizeOfResource) {
            sized.ByteSize = need;                      // committed: the heap is as large as its one resource
            log_line("heap: ByteSize left to the resource, %llu bytes", static_cast<unsigned long long>(need));
        }
        if (sized.ByteSize < need) return E_INVALIDARG;
        if (linear) {
            // The heap is the surface: as large as the memory asked of the shell, whatever the runtime gave
            // beyond it, and with the default heap alignment, whatever alignment the runtime handed back.
            if (sized.ByteSize != need || sized.Alignment)
                log_line("linear primary: heap of %llu bytes aligned to %llu becomes %llu bytes, default alignment",
                         static_cast<unsigned long long>(sized.ByteSize),
                         static_cast<unsigned long long>(sized.Alignment), static_cast<unsigned long long>(need));
            sized.ByteSize = need;
            sized.Alignment = 0;
        }
    } else if (sized.ByteSize == kSizeOfResource) {
        log_line("heap: ByteSize left to a resource, but the heap has none");
        return E_INVALIDARG;
    }
    D3D12_HEAP_DESC hd{};
    HRESULT hr = heap_desc_of(sized, hd);
    if (FAILED(hr)) return hr;
    if (linear) {
        // The heap holds this one image at offset 0 and nothing beside it: the engine is told the image's
        // category alone, whatever else the runtime's flags allow. The shell still sees the runtime's.
        const uint32_t category = category_of(desc);
        hd.Flags = D3D12_HEAP_FLAG_NONE;
        if (category != D3D12DDI_HEAP_FLAG_BUFFERS) hd.Flags |= D3D12_HEAP_FLAG_DENY_BUFFERS;
        if (category != D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES) hd.Flags |= D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
        if (category != D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES) hd.Flags |= D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    }

    ID3D12Heap* heap = nullptr;
    ImportedMemory memory{};
    bool imported = false;
    if (c->mode == MemoryMode::RuntimeBacked) {
        if (c->lost()) return DXGI_ERROR_DEVICE_REMOVED;
        MemoryRequest request{};
        request.size = sizeof(MemoryRequest);
        request.flags = (res_desc ? kMemoryDedicated : 0u) |
                        ((heap_desc->Flags & D3D12DDI_HEAP_FLAG_PRIMARY) ? kMemoryPrimary : 0u);
        request.rt_owner = rt;
        request.heap = heap_desc;
        request.resource = res_desc;
        request.byte_size = heap_desc->ByteSize;
        request.alignment = std::max<uint64_t>(align, heap_desc->Alignment);
        request.memory_type_bits = 0;                   // engine-ddi.h, MemoryRequest: the engine checks the type
        if (linear) {
            request.flags |= kMemoryLinearSurface;
            request.alignment = align;
            request.memory_type_bits = surface.info.MemoryTypeBits;
            request.surface_row_pitch = static_cast<uint32_t>(surface.info.RowPitch);
            request.surface_layout_size = surface.info.LayoutSize;
        }
        hr = import_memory(c, request, &memory);
        if (FAILED(hr)) {
            log_line("heap: the shell's memory request failed: %08lx", static_cast<unsigned long>(hr));
            return hr;
        }
        hr = engine_heap_from_memory(c, hd, memory, &heap);
        if (FAILED(hr)) {
            log_line("heap: the engine refused the heap over the supplied memory: %08lx (heap flags %x, %llu bytes, "
                     "alignment %llu, memory of %llu bytes, type %u)",
                     static_cast<unsigned long>(hr), static_cast<unsigned>(hd.Flags),
                     static_cast<unsigned long long>(hd.SizeInBytes), static_cast<unsigned long long>(hd.Alignment),
                     static_cast<unsigned long long>(memory.byte_size), memory.memory_type_index);
            // Nothing uses the memory yet: hand it straight back (engine-ddi.h, exactly once).
            ReleasePayload payload{{nullptr, nullptr}, true, memory, 0};
            run_release(c->hooks, payload);
            return hr;
        }
        imported = true;
    } else {
        if (align > 64 * 1024 && !hd.Alignment) hd.Alignment = align;
        hr = c->device->CreateHeap(&hd, __uuidof(ID3D12Heap), reinterpret_cast<void**>(&heap));
        if (FAILED(hr)) return hr;
    }

    Backing* b = new_backing(c, *heap_desc, heap);
    if (!b) {
        ReleasePayload payload{{heap, nullptr}, imported, memory, 0};
        run_release(c->hooks, payload);
        return E_OUTOFMEMORY;
    }
    b->imported = imported;
    b->memory = memory;
    b->dedicated = res_desc != nullptr;
    b->linear = linear;

    ID3D12Resource* engine = nullptr;
    if (res_desc) {
        hr = linear ? place_linear(c, b, desc, res_desc->InitialBarrierLayout, clear, surface, &engine)
                    : place(c, b, 0, desc, res_desc->InitialBarrierLayout, clear, res_desc->NumCastableFormats,
                            res_desc->pCastableFormats, &engine);
        if (FAILED(hr)) {
            backing_release(b);
            return hr;
        }
    }
    heap->AddRef();
    new (hheap.pDrvPrivate) HeapRecord{{Tag::Heap, 0, heap, c}, b};
    c->live.fetch_add(1);
    if (res_desc) {
        auto* record = static_cast<ResourceRecord*>(hres.pDrvPrivate);
        construct_resource(record, c, engine, b, 0, desc, rt, ResourceKind::Committed);
        if (linear) {
            record->linear_row_pitch = static_cast<uint32_t>(surface.info.RowPitch);
            record->linear_size = surface.backing_size;
        }
        // A committed render target or depth-stencil texture starts in VK_IMAGE_LAYOUT_UNDEFINED: engine-ddi
        // discards it before any work of the device runs (queue.cpp, INTEGRATION.md "Committed render targets").
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)))
            queue_initialization(c, record);
    }
    return S_OK;
}

HRESULT APIENTRY create_heap_and_resource_slot(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATEHEAP_0001* heap_desc,
                                               D3D12DDI_HHEAP hheap, D3D12DDI_HRTRESOURCE rt,
                                               const D3D12DDIARG_CREATERESOURCE_0088* res_desc,
                                               const D3D12DDI_CLEAR_VALUES* clear,
                                               D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE hres) {
    DeviceContext* c = resolve(device);
    if (!c) return E_INVALIDARG;
    c->process_retired();
    if (session.pDrvPrivate) {                          // protected resource sessions
        log_line("CreateHeapAndResource: protected resource session given: E_NOTIMPL");
        return E_NOTIMPL;
    }
    const HRESULT hr = create_heap_and_resource(c, heap_desc, hheap, rt, res_desc, clear, hres);
    if (FAILED(hr))
        log_line("CreateHeapAndResource: %08lx (heap description %s, resource description %s, castable formats %u)",
                 static_cast<unsigned long>(hr), heap_desc ? "given" : "none", res_desc ? "given" : "none",
                 res_desc ? res_desc->NumCastableFormats : 0u);
    return hr;
}

void APIENTRY destroy_heap_and_resource(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP hheap, D3D12DDI_HRESOURCE hres) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (hres.pDrvPrivate) {
        if (auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c)) {
            Backing* b = r->backing;
            if (r->kind == ResourceKind::Committed && cancel_initialization(c, r) && b && !b->retained) {
                b->retained = r->h.engine;              // an initialization batch names it: see Backing::retained
                r->h.engine = nullptr;
            }
            release_engine(r->h);
            poison(r->h);
            c->live.fetch_sub(1);
            if (b) backing_release(b);
        } else {
            c->report(E_INVALIDARG);
        }
    }
    if (hheap.pDrvPrivate) {
        if (auto* h = record_of<HeapRecord>(hheap.pDrvPrivate, Tag::Heap, c)) {
            Backing* b = h->backing;
            release_engine(h->h);
            poison(h->h);
            c->live.fetch_sub(1);
            backing_release(b);
        } else {
            c->report(E_INVALIDARG);
        }
    }
    c->process_retired();
}

HRESULT APIENTRY map_heap(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP hheap, void** data) {
    DeviceContext* c = resolve(device);
    if (!c || !data) return E_INVALIDARG;
    *data = nullptr;
    auto* h = record_of<HeapRecord>(hheap.pDrvPrivate, Tag::Heap, c);
    if (!h) return E_INVALIDARG;
    // Engine ABI 1.2 V10: the CPU address of heap offset 0 of a CPU-visible heap that allows buffers, over imported
    // memory or the engine's own; calls are counted and the address stays valid until the heap is destroyed.
    void* cpu = nullptr;
    const HRESULT hr = c->funcs.MapHeap(h->backing->heap, &cpu);
    if (SUCCEEDED(hr)) *data = cpu;
    return hr;
}

void APIENTRY unmap_heap(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP hheap) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* h = record_of<HeapRecord>(hheap.pDrvPrivate, Tag::Heap, c);
    const HRESULT hr = h ? c->funcs.UnmapHeap(h->backing->heap) : E_INVALIDARG;
    if (FAILED(hr)) c->report(hr);
}

D3D12DDI_GPU_VIRTUAL_ADDRESS APIENTRY check_resource_virtual_address(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE hres) {
    DeviceContext* c = resolve(device);
    if (!c) return 0;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return 0;
    }
    if (r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return 0;
    if (r->backing && r->backing->imported && !r->backing->memory.gpu_va) {
        c->report(E_INVALIDARG);                        // no completed mapping (engine-ddi.h, gpu_va)
        return 0;
    }
    return static_cast<ID3D12Resource*>(r->h.engine)->GetGPUVirtualAddress();
}

// The engine's size and alignment for desc, with desc.Alignment first and 0 if the engine refuses that (a
// small-alignment request). No additional data: engine-ddi keeps none next to a resource.
HRESULT allocation_info(DeviceContext* c, D3D12_RESOURCE_DESC1 desc, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) noexcept {
    D3D12_RESOURCE_DESC d0 = to_desc0(desc);
    D3D12_RESOURCE_ALLOCATION_INFO info = c->device->GetResourceAllocationInfo(0, 1, &d0);
    if (info.SizeInBytes == UINT64_MAX && d0.Alignment) {
        d0.Alignment = 0;
        info = c->device->GetResourceAllocationInfo(0, 1, &d0);
    }
    if (info.SizeInBytes == UINT64_MAX || info.Alignment > UINT32_MAX) return E_INVALIDARG;
    out->ResourceDataSize = info.SizeInBytes;
    out->ResourceDataAlignment = static_cast<UINT32>(info.Alignment);
    out->Layout = desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ? D3D12DDI_TL_ROW_MAJOR
                                                                    : static_cast<D3D12DDI_TEXTURE_LAYOUT>(desc.Layout);
    return S_OK;
}

// The answer for the linear primary: the backing's size. The alignment is a heap alignment of D3D12, which the
// runtime may hand back as the heap's: the image's own, smaller one goes to the shell with the memory request.
HRESULT linear_allocation_info(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc,
                               D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) noexcept {
    LinearSurface surface{};
    const HRESULT hr = query_linear_primary(c, desc, &surface);
    if (FAILED(hr)) return hr;
    out->ResourceDataSize = surface.backing_size;
    out->ResourceDataAlignment = static_cast<UINT32>(
        std::max<uint64_t>(surface.info.MemoryAlignment, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT));
    out->Layout = static_cast<D3D12DDI_TEXTURE_LAYOUT>(desc.Layout);
    return S_OK;
}

void APIENTRY check_resource_allocation_info(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATERESOURCE_0088* in,
                                             D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS optimization,
                                             UINT32 alignment_restriction, UINT,
                                             D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) {
    if (out) *out = D3D12DDI_RESOURCE_ALLOCATION_INFO_0022{};
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!in || !out) {
        c->report(E_INVALIDARG);
        return;
    }
    D3D12_RESOURCE_DESC1 desc{};
    HRESULT hr = to_api_desc(*in, desc);
    if (SUCCEEDED(hr) && in->NumCastableFormats && !in->pCastableFormats) hr = E_INVALIDARG;
    if (SUCCEEDED(hr)) {
        if (c->mode == MemoryMode::RuntimeBacked && (optimization & D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_PRIMARY) &&
            linear_primary_shape(desc, *in)) {
            hr = linear_allocation_info(c, desc, out);
        } else {
            desc.Alignment = alignment_restriction;
            hr = allocation_info(c, desc, out);
        }
    }
    if (FAILED(hr)) {
        *out = D3D12DDI_RESOURCE_ALLOCATION_INFO_0022{};
        c->report(hr);
    }
}

// Where a subresource lies in its resource's memory, asked after MapHeap. A buffer is one row from the
// resource's first byte: offset 0, both strides its width, nothing swizzled. The strides are inferred,
// the header does not define them for a buffer. A texture is not answered yet: no CPU-visible heap
// holds one here.
void APIENTRY check_subresource_info(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE hres, UINT subresource,
                                     D3D12DDI_SUBRESOURCE_INFO* out) {
    if (out) *out = D3D12DDI_SUBRESOURCE_INFO{};
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r || !out) return c->report(E_INVALIDARG);
    if (r->desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return c->report(E_NOTIMPL);
    if (subresource != 0) return c->report(E_INVALIDARG);
    out->RowStride = r->desc.Width;
    out->DepthStride = r->desc.Width;
}

// The same answer for a resource that exists, from the description it was created with (the record's desc,
// Alignment 0 as at creation). A reserved resource gets the answer for its description too, although it holds no
// memory of its own.
void APIENTRY check_existing_resource_allocation_info(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE hres,
                                                      D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) {
    if (out) *out = D3D12DDI_RESOURCE_ALLOCATION_INFO_0022{};
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    const HRESULT hr = !(r && out) ? E_INVALIDARG
                       : r->linear_row_pitch ? linear_allocation_info(c, r->desc, out)
                                             : allocation_info(c, r->desc, out);
    if (FAILED(hr)) {
        if (out) *out = D3D12DDI_RESOURCE_ALLOCATION_INFO_0022{};
        c->report(hr);
    }
}

// ---- Format queries (D0, D1) --------------------------------------------------------------------------------------
// The typeless parents: the formats a resource of a cast family is created with. A multisample resource may have one
// (D3D11.3 functional spec 19.2.2), so its quality levels are the family's although its own support answer is empty.
bool is_typeless_parent(DXGI_FORMAT f) noexcept {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_R8G8_TYPELESS:
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_BC1_TYPELESS:
    case DXGI_FORMAT_BC2_TYPELESS: case DXGI_FORMAT_BC3_TYPELESS: case DXGI_FORMAT_BC4_TYPELESS:
    case DXGI_FORMAT_BC5_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_BC6H_TYPELESS: case DXGI_FORMAT_BC7_TYPELESS:
        return true;
    default:
        return false;
    }
}

// The engine's D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS answer, 0 when the engine refuses the question.
UINT engine_quality_levels(DeviceContext* c, DXGI_FORMAT format, UINT sample_count,
                           D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags) noexcept {
    D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS q{format, sample_count, flags, 0};
    return SUCCEEDED(c->device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &q, sizeof(q)))
               ? q.NumQualityLevels
               : 0;
}

// The D3D12DDI_FORMAT_SUPPORT bits the format list allows for format (format-list.h), 0 for a value it does not
// list: R1_UNORM, the formats DXGI gained after the list (P208, V208, V408, the sampler feedback formats,
// A4B4G4R4_UNORM) and anything past the enum.
UINT format_list_allowed(DXGI_FORMAT format) noexcept {
    for (const FormatListEntry& e : kFormatList)
        if (e.format == format) return e.allowed;
    return 0;
}

// The engine's D3D12_FEATURE_FORMAT_SUPPORT answer as D3D12DDI_FORMAT_SUPPORT bits, limited to the bits the D3D11.3
// format list allows for the format (an engine answer beyond it, such as SHADER_GATHER on a stencil view, or DISPLAY,
// which the DDI defines only from version 107 on, is dropped); 0 (no optional capability) when the engine refuses
// the format, which is never a device error. BLENDABLE needs RENDERTARGET (d3d12umddi.h), and so does the output
// merger's logic op. MULTISAMPLE_RENDERTARGET means a render target or depth-stencil target with some sample count
// above 1 (d3d12umddi.h), so it stays only while the engine reports quality levels for such a count: then this
// answer and CheckMultisampleQualityLevels agree.
UINT engine_format_support(DeviceContext* c, DXGI_FORMAT format) noexcept {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT s{format};
    if (FAILED(c->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s)))) return 0;
    struct Bit { UINT api; UINT ddi; };
    static const Bit one[] = {
        {D3D12_FORMAT_SUPPORT1_BUFFER, D3D12DDI_FORMAT_SUPPORT_BUFFER},
        {D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER, D3D12DDI_FORMAT_SUPPORT_VERTEX_BUFFER},
        {D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE, D3D12DDI_FORMAT_SUPPORT_SHADER_SAMPLE},
        {D3D12_FORMAT_SUPPORT1_RENDER_TARGET, D3D12DDI_FORMAT_SUPPORT_RENDERTARGET},
        {D3D12_FORMAT_SUPPORT1_BLENDABLE, D3D12DDI_FORMAT_SUPPORT_BLENDABLE},
        {D3D12_FORMAT_SUPPORT1_DISPLAY, D3D12DDI_FORMAT_SUPPORT_DISPLAY},
        {D3D12_FORMAT_SUPPORT1_SHADER_GATHER, D3D12DDI_FORMAT_SUPPORT_SHADER_GATHER},
        {D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET, D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET},
        {D3D12_FORMAT_SUPPORT1_MULTISAMPLE_LOAD, D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_LOAD},
        {D3D12_FORMAT_SUPPORT1_DECODER_OUTPUT, D3D12DDI_FORMAT_SUPPORT_DECODER_OUTPUT},
        {D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_OUTPUT, D3D12DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT},
        {D3D12_FORMAT_SUPPORT1_VIDEO_PROCESSOR_INPUT, D3D12DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT},
        {D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW, D3D12DDI_FORMAT_SUPPORT_UAV_WRITES},
        {D3D12_FORMAT_SUPPORT1_VIDEO_ENCODER, D3D12DDI_FORMAT_SUPPORT_VIDEO_ENCODER},
    };
    static const Bit two[] = {
        {D3D12_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP, D3D12DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP},
        {D3D12_FORMAT_SUPPORT2_TILED, D3D12DDI_FORMAT_SUPPORT_TILED},
        {D3D12_FORMAT_SUPPORT2_MULTIPLANE_OVERLAY, D3D12DDI_FORMAT_SUPPORT_MULTIPLANE_OVERLAY},
        {D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD, D3D12DDI_FORMAT_SUPPORT_UAV_READS},
    };
    UINT bits = 0;
    for (const Bit& b : one) bits |= (s.Support1 & b.api) ? b.ddi : 0;
    for (const Bit& b : two) bits |= (s.Support2 & b.api) ? b.ddi : 0;
    bits &= format_list_allowed(format);
    if (!(bits & D3D12DDI_FORMAT_SUPPORT_RENDERTARGET))
        bits &= ~static_cast<UINT>(D3D12DDI_FORMAT_SUPPORT_BLENDABLE | D3D12DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP);
    if (bits & D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET) {
        bool any = false;
        for (UINT n = 2; n <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT && !any; n *= 2)
            any = engine_quality_levels(c, format, n, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_NONE) != 0;
        if (!any) bits &= ~static_cast<UINT>(D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET);
    }
    return bits;
}

// The engine's D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS answer for Flags NONE and TILED_RESOURCE (the values are
// equal) for a format that can itself be multisampled: one whose CheckFormatSupport answer carries
// MULTISAMPLE_RENDERTARGET, or a typeless parent, which shares its family's quality levels (D3D11.3 functional spec
// 19.2.3 (1)). Any other format is 0 levels above 1 sample whatever the engine says: a view-only sibling such as
// R32_FLOAT_X8X24_TYPELESS answers MULTISAMPLE_LOAD (an SRV of a multisample resource) but is never a multisample
// resource or target itself (D3D11.3 format list 19.1.4: no 4x, 8x or other-count multisample RenderTarget), while
// the engine answers it from its depth-stencil family. The format argument is a render-target format (WDK
// d3d10umddi pfnd3dwddm1_3ddi_checkmultisamplequalitylevels). A format or sample count the engine refuses is 0
// levels, as for CheckFormatSupport; so is an unknown flag.
void APIENTRY check_multisample_quality_levels(D3D12DDI_HDEVICE device, DXGI_FORMAT format, UINT sample_count,
                                               D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAGS flags, UINT* out) {
    static_assert(D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_TILED_RESOURCE ==
                      static_cast<int>(D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE), "MSAA flags");
    if (out) *out = 0;
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!out) {
        c->report(E_INVALIDARG);
        return;
    }
    if (flags & ~D3D12DDI_MULTISAMPLE_QUALITY_LEVEL_FLAG_TILED_RESOURCE) return;
    if (sample_count > 1 && !is_typeless_parent(format) &&
        !(engine_format_support(c, format) & D3D12DDI_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET))
        return;
    *out = engine_quality_levels(c, format, sample_count, static_cast<D3D12_MULTISAMPLE_QUALITY_LEVEL_FLAGS>(flags));
}

D3DKMT_HANDLE APIENTRY check_resource_allocation_handle(D3D12DDI_HDEVICE device, D3D10DDI_HRESOURCE hres) {
    DeviceContext* c = resolve(device);
    if (!c) return 0;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r) {
        c->report(E_INVALIDARG);
        return 0;
    }
    return (r->backing && r->backing->imported) ? r->backing->memory.allocation : 0;
}

void APIENTRY check_format_support(D3D12DDI_HDEVICE device, DXGI_FORMAT format, UINT* out) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!out) {
        c->report(E_INVALIDARG);
        return;
    }
    *out = engine_format_support(c, format);
}

// ---- Command-list slots -------------------------------------------------------------------------------------------
ID3D12Resource* engine_resource(CommandListRecord* l, D3D12DDI_HRESOURCE h) noexcept {
    auto* r = record_of<ResourceRecord>(h.pDrvPrivate, Tag::Resource, l->h.device);
    return r ? static_cast<ID3D12Resource*>(r->h.engine) : nullptr;
}

void APIENTRY copy_buffer_region(D3D12DDI_HCOMMANDLIST hlist, D3D12DDIARG_BUFFER_PLACEMENT dst,
                                 D3D12DDIARG_BUFFER_PLACEMENT src, UINT64 bytes) {
    CommandListRecord* l = list_of(hlist, "CopyBufferRegion");
    if (!l) return;
    ID3D12Resource* d = engine_resource(l, dst.BaseAddress.UMD.hResource);
    ID3D12Resource* s = engine_resource(l, src.BaseAddress.UMD.hResource);
    if (!d || !s) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->CopyBufferRegion(d, dst.BaseAddress.UMD.Offset, s, src.BaseAddress.UMD.Offset, bytes);
}

UINT block_size(DXGI_FORMAT f) noexcept {
    return ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
            (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB))
               ? 4u
               : 1u;
}

HRESULT copy_location(CommandListRecord* l, const D3D12DDIARG_BUFFER_PLACEMENT* p, const D3D12DDIARG_PLACED_RESOURCE& r,
                      D3D12_TEXTURE_COPY_LOCATION& out) noexcept {
    if (!p) return E_INVALIDARG;
    out = D3D12_TEXTURE_COPY_LOCATION{};
    out.pResource = engine_resource(l, p->BaseAddress.UMD.hResource);
    if (!out.pResource) return E_INVALIDARG;
    switch (r.Layout) {
    case D3D12DDI_RL_SELECT_SUBRESOURCE:
        if (p->BaseAddress.UMD.Offset > UINT32_MAX) return E_INVALIDARG;
        out.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        out.SubresourceIndex = static_cast<UINT>(p->BaseAddress.UMD.Offset);
        return S_OK;
    case D3D12DDI_RL_PLACED_PHYSICAL_SUBRESOURCE_PITCHED: {
        const auto* f = static_cast<const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT*>(r.pLayout);
        if (!f) return E_INVALIDARG;
        if (static_cast<uint64_t>(f->SlicePitch) != static_cast<uint64_t>(f->Pitch) * f->PhysicalHeight)
            return E_NOTIMPL;                           // the API derives the slice pitch
        const UINT bs = block_size(f->Format);
        out.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        out.PlacedFootprint.Offset = p->BaseAddress.UMD.Offset;
        out.PlacedFootprint.Footprint = {f->Format, f->PhysicalWidth * bs, f->PhysicalHeight * bs, f->PhysicalDepth,
                                         f->Pitch};
        return S_OK;
    }
    case D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED: {
        const auto* f = static_cast<const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT*>(r.pLayout);
        if (!f) return E_INVALIDARG;
        if (static_cast<uint64_t>(f->SlicePitch) != static_cast<uint64_t>(f->Pitch) * f->PhysicalHeight)
            return E_NOTIMPL;
        out.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        out.PlacedFootprint.Offset = p->BaseAddress.UMD.Offset;
        out.PlacedFootprint.Footprint = {f->Format, f->VirtualWidth, f->VirtualHeight, f->VirtualDepth, f->Pitch};
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

void APIENTRY copy_texture_region(D3D12DDI_HCOMMANDLIST hlist, const D3D12DDIARG_BUFFER_PLACEMENT* pdst,
                                  D3D12DDIARG_PLACED_RESOURCE dst, UINT x, UINT y, UINT z,
                                  const D3D12DDIARG_BUFFER_PLACEMENT* psrc, D3D12DDIARG_PLACED_RESOURCE src,
                                  const D3D12DDI_BOX* box) {
    CommandListRecord* l = list_of(hlist, "CopyTextureRegion");
    if (!l) return;
    D3D12_TEXTURE_COPY_LOCATION d, s;
    HRESULT hr = copy_location(l, pdst, dst, d);
    if (SUCCEEDED(hr)) hr = copy_location(l, psrc, src, s);
    if (SUCCEEDED(hr) && box && (box->Left < 0 || box->Top < 0 || box->Front < 0 || box->Right < box->Left ||
                                 box->Bottom < box->Top || box->Back < box->Front))
        hr = E_INVALIDARG;
    if (FAILED(hr)) {
        l->h.device->report_list(l->rt, hr);
        return;
    }
    D3D12_BOX b{};
    if (box)
        b = {static_cast<UINT>(box->Left), static_cast<UINT>(box->Top), static_cast<UINT>(box->Front),
             static_cast<UINT>(box->Right), static_cast<UINT>(box->Bottom), static_cast<UINT>(box->Back)};
    l->list()->CopyTextureRegion(&d, x, y, z, &s, box ? &b : nullptr);
}

void APIENTRY resource_copy(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HRESOURCE dst, D3D12DDI_HRESOURCE src) {
    CommandListRecord* l = list_of(hlist, "ResourceCopy");
    if (!l) return;
    ID3D12Resource* d = engine_resource(l, dst);
    ID3D12Resource* s = engine_resource(l, src);
    if (!d || !s) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    l->list()->CopyResource(d, s);
}

// No argument structure means the whole resource.
void APIENTRY discard_resource(D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HRESOURCE resource,
                               const D3D12DDIARG_DISCARD_RESOURCE_0003* args) {
    CommandListRecord* l = list_of(hlist, "DiscardResource");
    if (!l) return;
    ID3D12Resource* r = engine_resource(l, resource);
    if (!r || (args && args->NumRects && !args->pRects)) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    if (!args) {
        l->list()->DiscardResource(r, nullptr);
        return;
    }
    const D3D12_DISCARD_REGION region{args->NumRects, args->pRects, args->FirstSubresource, args->NumSubresources};
    l->list()->DiscardResource(r, &region);
}

void APIENTRY resource_barrier(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDIARG_RESOURCE_BARRIER_0022* in) {
    CommandListRecord* l = list_of(hlist, "ResourceBarrier");
    if (!l) return;
    if (count && !in) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    std::vector<D3D12_RESOURCE_BARRIER> out;
    try {
        out.reserve(count);
    } catch (...) {
        l->h.device->report_list(l->rt, E_OUTOFMEMORY);
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        const D3D12DDIARG_RESOURCE_BARRIER_0022& b = in[i];
        D3D12_RESOURCE_BARRIER a{};
        a.Flags = static_cast<D3D12_RESOURCE_BARRIER_FLAGS>(
            b.Flags & (D3D12DDI_RESOURCE_BARRIER_FLAG_BEGIN_ONLY | D3D12DDI_RESOURCE_BARRIER_FLAG_END_ONLY));
        switch (b.Type) {
        case D3D12DDI_RESOURCE_BARRIER_TYPE_TRANSITION:
            a.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            a.Transition.pResource = engine_resource(l, b.Transition.hResource);
            a.Transition.Subresource = b.Transition.Subresource;
            a.Transition.StateBefore = static_cast<D3D12_RESOURCE_STATES>(b.Transition.StateBefore);
            a.Transition.StateAfter = static_cast<D3D12_RESOURCE_STATES>(b.Transition.StateAfter);
            if (!a.Transition.pResource) {
                l->h.device->report_list(l->rt, E_INVALIDARG);
                return;
            }
            break;
        case D3D12DDI_RESOURCE_BARRIER_TYPE_UAV:
            a.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            a.UAV.pResource = b.UAV.hResource.pDrvPrivate ? engine_resource(l, b.UAV.hResource) : nullptr;
            if (b.UAV.hResource.pDrvPrivate && !a.UAV.pResource) {
                l->h.device->report_list(l->rt, E_INVALIDARG);
                return;
            }
            break;
        case D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING:
        case D3D12DDI_RESOURCE_BARRIER_TYPE_0022_RANGED: {
            // RANGED with the aliasing flag, or the deprecated aliasing type: an aliasing barrier that activates
            // the resource. RANGED alone: a UAV-style barrier on the whole resource (INFERENCE on its runtime use).
            ID3D12Resource* r = b.Ranged.hResource.pDrvPrivate ? engine_resource(l, b.Ranged.hResource) : nullptr;
            if (b.Ranged.hResource.pDrvPrivate && !r) {
                l->h.device->report_list(l->rt, E_INVALIDARG);
                return;
            }
            if (b.Type == D3D12DDI_RESOURCE_BARRIER_TYPE_ALIASING || (b.Flags & D3D12DDI_RESOURCE_BARRIER_FLAG_0022_ALIASING)) {
                a.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
                a.Aliasing.pResourceBefore = nullptr;
                a.Aliasing.pResourceAfter = r;
            } else {
                a.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                a.UAV.pResource = r;
            }
            break;
        }
        default:
            l->h.device->report_list(l->rt, E_INVALIDARG);
            return;
        }
        out.push_back(a);
    }
    if (!out.empty()) l->list()->ResourceBarrier(static_cast<UINT>(out.size()), out.data());
}
} // namespace

// ---- Backing lifetime -------------------------------------------------------------------------------------------
void backing_acquire(Backing* b) noexcept { b->refs.fetch_add(1); }

void backing_release(Backing* b) noexcept {
    if (b->refs.fetch_sub(1) != 1) return;
    DeviceContext* c = b->device;
    PendingRelease* node = b->release_node;
    node->payload = ReleasePayload{{b->retained, b->heap}, b->imported, b->memory, b->id};  // resource first
    delete b;
    c->release(node);
}

// ---- RuntimeBacked import (engine-ddi.h: memory request and validation) -----------------------------------------
HRESULT validate_import(const MemoryRequest& request, const ImportedMemory& m) noexcept {
    if (m.size != sizeof(ImportedMemory) || m.reserved || m.memory == VK_NULL_HANDLE || !m.allocation) return E_INVALIDARG;
    if (m.byte_size < request.byte_size) return E_INVALIDARG;
    // memory_type_bits is 0 in r3: the type is checked by CreateHeapFromMemory (engine-ddi.h, MemoryRequest).
    if (request.memory_type_bits && (m.memory_type_index >= 32 || !(request.memory_type_bits & (1u << m.memory_type_index))))
        return E_INVALIDARG;
    if (!m.gpu_va) return E_INVALIDARG;                 // no completed mapping
    if (request.alignment && m.gpu_va % request.alignment) return E_INVALIDARG;
    return S_OK;
}

HRESULT import_memory(DeviceContext* c, const MemoryRequest& request, ImportedMemory* out) noexcept {
    *out = ImportedMemory{};
    ImportedMemory m{};
    HRESULT hr = c->hooks.allocate_memory(c->hooks.shell, &request, &m);
    if (FAILED(hr)) return hr;                          // on failure engine-ddi owns nothing
    hr = validate_import(request, m);
    if (FAILED(hr)) {
        ReleasePayload payload{{nullptr, nullptr}, true, m, 0};
        run_release(c->hooks, payload);                 // exactly once; a failure is reported there
        return hr;
    }
    *out = m;
    return S_OK;
}

HRESULT resource_allocation(DeviceContext* c, D3D12DDI_HRESOURCE hres, D3DKMT_HANDLE* allocation,
                            uint64_t* offset) noexcept {
    if (!c || !allocation || !offset) return E_INVALIDARG;
    *allocation = 0;
    *offset = 0;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r || c->mode != MemoryMode::RuntimeBacked || r->kind != ResourceKind::Committed || !r->backing ||
        !r->backing->imported)
        return E_INVALIDARG;
    *allocation = r->backing->memory.allocation;
    return S_OK;
}

HRESULT object_allocation(DeviceContext* c, D3D12DDI_HANDLE_AND_TYPE object, D3DKMT_HANDLE* allocation) noexcept {
    if (!allocation) return E_INVALIDARG;
    *allocation = 0;
    if (!c) return E_INVALIDARG;
    const Backing* b = nullptr;
    switch (object.Type) {
    case D3D12DDI_HT_HEAP:
        if (auto* h = record_of<HeapRecord>(object.Handle, Tag::Heap, c)) {
            b = h->backing;
            break;
        }
        return E_INVALIDARG;
    case D3D12DDI_HT_0012_RESOURCE:
        if (auto* r = record_of<ResourceRecord>(object.Handle, Tag::Resource, c)) {
            // A reserved resource has no memory of its own: its tiles live in heaps, which are resident as heaps.
            if (r->kind == ResourceKind::Reserved) return S_FALSE;
            b = r->backing;                             // a placed resource shares its heap's backing
            break;
        }
        return E_INVALIDARG;
    case D3D12DDI_HT_DESCRIPTOR_HEAP:
        return record_of<DescriptorHeapRecord>(object.Handle, Tag::DescriptorHeap, c) ? S_FALSE : E_INVALIDARG;
    case D3D12DDI_HT_QUERY_HEAP:
        return record_of<QueryHeapRecord>(object.Handle, Tag::QueryHeap, c) ? S_FALSE : E_INVALIDARG;
    default:
        return E_INVALIDARG;
    }
    if (!b) return E_INVALIDARG;                        // no heap memory
    if (!b->imported) return S_FALSE;                   // EnginePrivateTest memory: no runtime allocation
    *allocation = b->memory.allocation;
    return S_OK;
}

void fill_core_resources(D3D12DDI_DEVICE_FUNCS_CORE_0088* t) noexcept {
    t->pfnCheckFormatSupport = check_format_support;
    t->pfnCheckMultisampleQualityLevels = check_multisample_quality_levels;
    t->pfnCalcPrivateHeapAndResourceSizes = calc_heap_and_resource;
    t->pfnCreateHeapAndResource = create_heap_and_resource_slot;
    t->pfnDestroyHeapAndResource = destroy_heap_and_resource;
    t->pfnMapHeap = map_heap;
    t->pfnUnmapHeap = unmap_heap;
    t->pfnCheckResourceVirtualAddress = check_resource_virtual_address;
    t->pfnCheckResourceAllocationInfo = check_resource_allocation_info;
    t->pfnCheckExistingResourceAllocationInfo = check_existing_resource_allocation_info;
    t->pfnCheckSubresourceInfo = check_subresource_info;
    t->pfnCheckResourceAllocationHandle = check_resource_allocation_handle;
}

void fill_list_resources(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t) noexcept {
    t->pfnCopyBufferRegion = copy_buffer_region;
    t->pfnCopyTextureRegion = copy_texture_region;
    t->pfnResourceCopy = resource_copy;
    t->pfnDiscardResource = discard_resource;
    t->pfnResourceBarrier = resource_barrier;
}

} // namespace engine_ddi
