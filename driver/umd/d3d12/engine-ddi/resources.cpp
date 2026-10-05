// SPDX-License-Identifier: MIT
// engine-ddi: heaps and resources (D59-D61, D57-D58, D74, D75, D84, D0, D1), reserved resources (the creation;
// their tile slots are in tiles.cpp), resource_allocation, object_allocation, and the list slots that move data
// between resources (L10, L13, L17).
#include "internal.h"
#include "replay.h"
#include "format-list.h"
#include "../../../contract/amdgpu_wddm_surface_format.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

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
    // A packed video format is its storage (internal.h, StoredFormat): a 2D texture of one sample, and of one mip level
    // for a 4:2:2 format, whose mip widths in pixels and in elements part ways (6 pixels are 3 elements, the next
    // level 3 pixels, 2 elements, while the image's next level is 1 element). Sizing and creation both come here.
    if (const StoredFormat* s = stored_format(in.Format)) {
        if (in.ResourceType != D3D12DDI_RT_TEXTURE2D || in.SampleDesc.Count > 1 ||
            (s->pixels > 1 && (in.MipLevels != 1 || in.Width % s->pixels)))
            return E_INVALIDARG;
        out.Format = s->storage;
        out.Width = in.Width / s->pixels;
    }
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
    // A heap for a resource granted a small placement alignment comes with that alignment (4 KB), which a D3D12 heap
    // description does not take: the engine heap gets the default, a stricter one the heap's offset 0 satisfies.
    out.Alignment = in.Alignment < D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT ? 0 : in.Alignment;
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

// The engine's placed resource at offset in the backing (engine-ddi.h placement rules, D3D12 part). The DDI
// description carries no alignment: an application that asked for a small one (4 KB, or 64 KB for multisampling)
// and got it from CheckResourceAllocationInfo places the resource at an offset that only that alignment divides.
// The first of default, 64 KB and 4 KB that the engine grants and the offset satisfies is the resource's, and it
// is written back into desc for the record.
HRESULT place(DeviceContext* c, Backing* b, uint64_t offset, D3D12_RESOURCE_DESC1& desc,
              D3D12DDI_BARRIER_LAYOUT layout, const D3D12DDI_CLEAR_VALUES* clear, UINT castable_count,
              const DXGI_FORMAT* castable, ID3D12Resource** out) noexcept {
    *out = nullptr;
    if (!(b->desc.Flags & category_of(desc))) return E_INVALIDARG;
    D3D12_RESOURCE_DESC d0 = to_desc0(desc);
    D3D12_RESOURCE_ALLOCATION_INFO info{UINT64_MAX, 0};
    const uint64_t requested = d0.Alignment;
    const uint64_t candidates[] = {requested, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT,
                                   D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT};
    bool fits = false;
    for (const uint64_t alignment : candidates) {
        if (alignment && requested && alignment != requested) break;  // an explicit alignment is the only one
        d0.Alignment = alignment;
        info = c->device->GetResourceAllocationInfo(0, 1, &d0);
        if (info.SizeInBytes == UINT64_MAX || !info.Alignment || offset % info.Alignment) continue;
        if (alignment && info.Alignment != alignment) continue;          // not granted
        if (offset > b->desc.ByteSize || info.SizeInBytes > b->desc.ByteSize - offset) continue;  // a smaller may fit
        fits = true;
        break;
    }
    if (!fits) return E_INVALIDARG;
    desc.Alignment = d0.Alignment;
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

// The storage formats a linear primary may have: the rows of the surface format table the compositor may open
// (driver/contract/amdgpu_wddm_surface_format.h), which the shell and the kernel driver admit by the same table.
const AMDGPU_WDDM_SURFACE_FORMAT* composed_format(DXGI_FORMAT f) noexcept {
    return amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_dxgi(static_cast<unsigned>(f)),
                                     AMDGPU_WDDM_SURFACE_COMPOSED);
}

// The descriptions the linear primary exists for. The engine's image has the format's own compatibility list, so
// castable formats beyond the format and its sRGB sibling, where the format has one, are not these.
bool linear_primary_shape(const D3D12_RESOURCE_DESC1& desc, const D3D12DDIARG_CREATERESOURCE_0088& in) noexcept {
    const AMDGPU_WDDM_SURFACE_FORMAT* row = composed_format(desc.Format);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !row) return false;
    const auto sibling = static_cast<DXGI_FORMAT>(row->dxgi_srgb);
    if (!desc.Width || desc.Width > kLinearMaxEdge || !desc.Height || desc.Height > kLinearMaxEdge) return false;
    if (desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality)
        return false;
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN && desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) return false;
    if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
                      D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
                      D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE))
        return false;
    for (UINT i = 0; i < in.NumCastableFormats; ++i)
        if (in.pCastableFormats[i] != desc.Format &&
            (sibling == DXGI_FORMAT_UNKNOWN || in.pCastableFormats[i] != sibling))
            return false;
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
    const AMDGPU_WDDM_SURFACE_FORMAT* row = composed_format(desc.Format);
    if (!c->funcs.QueryLinearImage || !row) return E_NOTIMPL;
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
        info.RowPitch < width4 * row->bytes_per_pixel || !info.MemorySize || !info.MemoryAlignment || !info.MemoryTypeBits ||
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

// stored_from is the format the application named, kept when desc stores it (internal.h, StoredFormat).
void construct_resource(ResourceRecord* r, DeviceContext* c, ID3D12Resource* engine, Backing* b, uint64_t offset,
                        const D3D12_RESOURCE_DESC1& desc, D3D12DDI_HRTRESOURCE rt, ResourceKind kind,
                        DXGI_FORMAT stored_from) noexcept {
    if (b) backing_acquire(b);                          // a reserved resource has none
    if (!stored_format(stored_from)) stored_from = DXGI_FORMAT_UNKNOWN;
    new (r) ResourceRecord{{Tag::Resource, 0, engine, c}, b, offset, desc, rt, kind, kInitNone, nullptr, nullptr,
                           0, 0, stored_from};
    c->live.fetch_add(1);
}

D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_heap_and_resource(D3D12DDI_HDEVICE, const D3D12DDIARG_CREATEHEAP_0001*,
                                                                 const D3D12DDIARG_CREATERESOURCE_0088*,
                                                                 D3D12DDI_HPROTECTEDRESOURCESESSION_0030) {
    return {sizeof(HeapRecord), sizeof(ResourceRecord)};
}

void log_untiled_reserved(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc, HRESULT result) noexcept;

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
            log_untiled_reserved(c, desc, hr);
            if (FAILED(hr)) return hr;
            construct_resource(static_cast<ResourceRecord*>(hres.pDrvPrivate), c, engine, nullptr, 0, desc, rt,
                               ResourceKind::Reserved, res_desc->Format);
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
                           ResourceKind::Placed, res_desc->Format);
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
            // A heap alignment below the default is the small placement alignment the resource was granted: its
            // size is the one for that alignment (a 16 KB texture in a 16 KB heap), and place() picks it again.
            D3D12_RESOURCE_DESC d0 = to_desc0(desc);
            if (sized.Alignment && sized.Alignment < D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
                d0.Alignment = sized.Alignment;
            D3D12_RESOURCE_ALLOCATION_INFO info = c->device->GetResourceAllocationInfo(0, 1, &d0);
            if (info.SizeInBytes == UINT64_MAX && d0.Alignment) {
                d0.Alignment = 0;
                info = c->device->GetResourceAllocationInfo(0, 1, &d0);
            }
            if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes) return E_INVALIDARG;
            desc.Alignment = d0.Alignment;              // the image is placed with the alignment its memory has
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
        construct_resource(record, c, engine, b, 0, desc, rt, ResourceKind::Committed, res_desc->Format);
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
    HRESULT hr = E_INVALIDARG;
    if (c) {
        c->retire_at_resource();
        if (session.pDrvPrivate) {                      // protected resource sessions
            log_line("CreateHeapAndResource: protected resource session given: not implemented");
            hr = E_NOTIMPL;
        } else {
            hr = create_heap_and_resource(c, heap_desc, hheap, rt, res_desc, clear, hres);
        }
    }
    if (FAILED(hr)) {
        // The call's shape (which of heap only, committed, placed or reserved) and both descriptions: which
        // shape the runtime uses for which API call is otherwise unlogged (engine-ddi.h).
        const D3D12DDIARG_CREATEHEAP_0001 h = heap_desc ? *heap_desc : D3D12DDIARG_CREATEHEAP_0001{};
        const D3D12DDIARG_CREATERESOURCE_0088* r = res_desc;
        log_refusal("CreateHeapAndResource: %08lx; heap description %s (%llu bytes, alignment %llu, flags 0x%x, "
                    "pool %u, cpu page %u), heap handle %s; resource description %s (type %u, %llux%u, depth %u, "
                    "mips %u, format %u, samples %u, layout %u, flags 0x%x, castable %u), base resource %s, "
                    "offset %llu",
                    static_cast<unsigned long>(hr), heap_desc ? "given" : "none",
                    static_cast<unsigned long long>(h.ByteSize), static_cast<unsigned long long>(h.Alignment),
                    static_cast<unsigned>(h.Flags), static_cast<unsigned>(h.MemoryPool),
                    static_cast<unsigned>(h.CPUPageProperty), hheap.pDrvPrivate ? "given" : "none",
                    r ? "given" : "none", r ? static_cast<unsigned>(r->ResourceType) : 0u,
                    r ? static_cast<unsigned long long>(r->Width) : 0ull, r ? r->Height : 0u,
                    r ? static_cast<unsigned>(r->DepthOrArraySize) : 0u, r ? static_cast<unsigned>(r->MipLevels) : 0u,
                    r ? static_cast<unsigned>(r->Format) : 0u, r ? r->SampleDesc.Count : 0u,
                    r ? static_cast<unsigned>(r->Layout) : 0u, r ? static_cast<unsigned>(r->Flags) : 0u,
                    r ? r->NumCastableFormats : 0u,
                    r && r->ReuseBufferGPUVA.BaseAddress.UMD.hResource.pDrvPrivate ? "given" : "none",
                    r ? static_cast<unsigned long long>(r->ReuseBufferGPUVA.BaseAddress.UMD.Offset) : 0ull);
        // A create DDI may only report E_OUTOFMEMORY (engine-ddi.h, admitted_create_failure): the refusal above
        // is the driver's answer, not a reason for the runtime to remove the application's device. BD-075.
        const HRESULT admitted = admitted_create_failure(hr);
        if (admitted != hr)
            log_refusal("CreateHeapAndResource: %08lx reported to the runtime as %08lx, so that the device stays",
                        static_cast<unsigned long>(hr), static_cast<unsigned long>(admitted));
        return admitted;
    }
    return hr;
}

// ---- D64, D65: OpenSharedHandle of a heap or a resource ----------------------------------------------------------
// Not implemented: a D3D12 resource of this driver publishes no description a second driver could decode, and the
// shell has no entry point that adopts an allocation the runtime opened instead of allocating one
// (heap-import.h, RuntimeHeapImports). What the two slots do is refuse without losing the device, and name
// everything the runtime handed over, because that is what an implementation has to decode: the allocation
// handles, the per-allocation private data (the kernel driver's LB7A record for a surface this driver's D3D11
// shell created) and the per-resource private data (its E26R record). docs/d3d12-shared-resources.md holds the
// plan; SLOTS.md keeps the two slots at P4.
// Calc answers the private sizes an implemented open would construct, the same records a create builds, so that
// a later implementation needs no second ABI step and a runtime that calls Calc and then Open sees one shape.
D3D12DDI_HEAP_AND_RESOURCE_SIZES APIENTRY calc_opened_heap_and_resource(D3D12DDI_HDEVICE,
                                                                       const D3D12DDIARG_OPENHEAP_0003*,
                                                                       D3D12DDI_HPROTECTEDRESOURCESESSION_0030) {
    return {sizeof(HeapRecord), sizeof(ResourceRecord)};
}

// The first four bytes of a private-data blob, as its writer's magic: 0 when there are not four bytes to read.
uint32_t blob_magic(const void* data, UINT bytes) noexcept {
    uint32_t magic = 0;
    if (data && bytes >= sizeof(magic)) std::memcpy(&magic, data, sizeof(magic));
    return magic;
}

uint32_t blob_version(const void* data, UINT bytes) noexcept {
    uint32_t version = 0;
    if (data && bytes >= 2 * sizeof(version))
        std::memcpy(&version, static_cast<const char*>(data) + sizeof(version), sizeof(version));
    return version;
}

HRESULT APIENTRY open_heap_and_resource(D3D12DDI_HDEVICE device, const D3D12DDIARG_OPENHEAP_0003* args,
                                        D3D12DDI_HHEAP hheap, D3D12DDI_HRTRESOURCE rt,
                                        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE hres) {
    DeviceContext* c = resolve(device);
    (void)rt;
    (void)session;
    const HRESULT hr = c && args ? E_NOTIMPL : E_INVALIDARG;
    // Same rule as the create slot: the open of a shared handle fails, the device lives (BD-075).
    const HRESULT admitted = admitted_create_failure(hr);
    const UINT count = args ? args->NumAllocations : 0u;
    const D3DDDI_OPENALLOCATIONINFO* first =
        args && args->pOpenAllocationInfo && count ? &args->pOpenAllocationInfo[0] : nullptr;
    log_refusal("OpenHeapAndResource: %08lx reported as %08lx, no shared open is implemented; %u allocation(s), "
                "kernel resource %s, "
                "resource private data %u bytes (magic 0x%08x, version %u), first allocation private data %u bytes "
                "(magic 0x%08x, version %u), allocation handle %s, initial state 0x%x, heap handle %s, "
                "resource handle %s",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(admitted), count,
                args && args->hKMResource.handle ? "given" : "none", args ? args->PrivateDriverDataSize : 0u,
                blob_magic(args ? args->pPrivateDriverData : nullptr, args ? args->PrivateDriverDataSize : 0u),
                blob_version(args ? args->pPrivateDriverData : nullptr, args ? args->PrivateDriverDataSize : 0u),
                first ? first->PrivateDriverDataSize : 0u,
                blob_magic(first ? first->pPrivateDriverData : nullptr, first ? first->PrivateDriverDataSize : 0u),
                blob_version(first ? first->pPrivateDriverData : nullptr, first ? first->PrivateDriverDataSize : 0u),
                first && first->hAllocation ? "given" : "none",
                args ? static_cast<unsigned>(args->InitialResourceState) : 0u,
                hheap.pDrvPrivate ? "given" : "none", hres.pDrvPrivate ? "given" : "none");
    return admitted;
}

void APIENTRY destroy_heap_and_resource(D3D12DDI_HDEVICE device, D3D12DDI_HHEAP hheap, D3D12DDI_HRESOURCE hres) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    drain_all(c, Drain::Destroy);
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
    c->retire_at_resource();
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

// No additional data: engine-ddi keeps none next to a resource. Its alignments still name a power of two, the
// resource's own: the runtime aligns its API answer up to them, and a zero wraps it (GetResourceAllocationInfo
// answered 0xFFFFFFFFFFFF0000 for every description while CreateCommittedResource, which does not, worked).
void no_additional_data(D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) noexcept {
    out->AdditionalDataHeaderSize = 0;
    out->AdditionalDataSize = 0;
    out->AdditionalDataHeaderAlignment = out->ResourceDataAlignment;
    out->AdditionalDataAlignment = out->ResourceDataAlignment;
}

// The engine's size and alignment for desc, with desc.Alignment first and 0 if the engine refuses that (a
// small-alignment request).
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
    no_additional_data(out);
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
    no_additional_data(out);
    return S_OK;
}

// The answer for a description that cannot be sized: ResourceDataSize UINT64_MAX, the API's own error answer
// (GetResourceAllocationInfo: "If an error occurs, then SizeInBytes equals UINT64_MAX"), with the default placement
// alignment for its sample count, as the engine answers such a description itself. No code goes to the device error
// callback: the DDI reference allows this slot none, and the runtime takes an error a function does not allow as
// critical and removes the device ("Handling Errors"). An application may ask about any description; creating the
// resource is refused by CreateHeapAndResource, which returns its HRESULT.
void unsized(const D3D12DDIARG_CREATERESOURCE_0088& in, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) noexcept {
    *out = D3D12DDI_RESOURCE_ALLOCATION_INFO_0022{};
    out->ResourceDataSize = UINT64_MAX;
    out->ResourceDataAlignment = in.SampleDesc.Count > 1 ? D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT
                                                         : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    out->Layout = in.ResourceType == D3D12DDI_RT_BUFFER ? D3D12DDI_TL_ROW_MAJOR : in.Layout;
    no_additional_data(out);
}

// The first refusal of each format, with its description and why it was refused. Once per format, at most 257 lines
// a process.
FormatSet g_allocation_refusals;

void APIENTRY check_resource_allocation_info(D3D12DDI_HDEVICE device, const D3D12DDIARG_CREATERESOURCE_0088* in,
                                             D3D12DDI_RESOURCE_OPTIMIZATION_FLAGS optimization,
                                             UINT32 alignment_restriction, UINT,
                                             D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out) {
    if (out) *out = D3D12DDI_RESOURCE_ALLOCATION_INFO_0022{};
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!in || !out || (in->NumCastableFormats && !in->pCastableFormats)) {
        c->report(E_INVALIDARG);                        // a malformed call, not a question about a description
        return;
    }
    D3D12_RESOURCE_DESC1 desc{};
    HRESULT hr = to_api_desc(*in, desc);
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
        unsized(*in, out);
        if (g_allocation_refusals.insert(static_cast<uint32_t>(in->Format)))
            log_refusal("CheckResourceAllocationInfo: UINT64_MAX answered for the first refusal of format %u (%08lx): "
                        "type %u, %llu x %u, depth or array %u, mips %u, samples %u, flags 0x%x, layout %u, "
                        "castable %u, alignment %u, optimization 0x%x",
                        static_cast<unsigned>(in->Format), static_cast<unsigned long>(hr),
                        static_cast<unsigned>(in->ResourceType), static_cast<unsigned long long>(in->Width), in->Height,
                        static_cast<unsigned>(in->DepthOrArraySize), static_cast<unsigned>(in->MipLevels),
                        in->SampleDesc.Count, static_cast<unsigned>(in->Flags), static_cast<unsigned>(in->Layout),
                        in->NumCastableFormats, alignment_restriction, static_cast<unsigned>(optimization));
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

// The engine's D3D12_FEATURE_FORMAT_SUPPORT answer for format, *hr its result; no support bits when it refuses.
D3D12_FEATURE_DATA_FORMAT_SUPPORT engine_format(DeviceContext* c, DXGI_FORMAT format, HRESULT* hr) noexcept {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT s{format};
    *hr = c->device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &s, sizeof(s));
    if (FAILED(*hr)) s = D3D12_FEATURE_DATA_FORMAT_SUPPORT{format};
    return s;
}

// The engine's D3D12_FEATURE_FORMAT_SUPPORT answer as D3D12DDI_FORMAT_SUPPORT bits, limited to the bits the D3D11.3
// format list allows for the format (an engine answer beyond it, such as SHADER_GATHER on a stencil view, or DISPLAY,
// which the DDI defines only from version 107 on, is dropped); 0 (no optional capability) when the engine refuses
// the format, which is never a device error. BLENDABLE needs RENDERTARGET (d3d12umddi.h), and so does the output
// merger's logic op. MULTISAMPLE_RENDERTARGET means a render target or depth-stencil target with some sample count
// above 1 (d3d12umddi.h), so it stays only while the engine reports quality levels for such a count: then this
// answer and CheckMultisampleQualityLevels agree.
UINT ddi_format_support(DeviceContext* c, const D3D12_FEATURE_DATA_FORMAT_SUPPORT& s) noexcept {
    const DXGI_FORMAT format = s.Format;
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

UINT engine_format_support(DeviceContext* c, DXGI_FORMAT format) noexcept {
    HRESULT hr = S_OK;
    return ddi_format_support(c, engine_format(c, format, &hr));
}

// The first reserved 2D texture the engine reports no tiled support for, logged once a process with what the slot
// returned for it. The engine makes a reserved texture of a single-aspect format it cannot make sparse its committed
// fallback, on which tile mappings are ignored, and refuses one of two aspects (vkd3d-proton
// d3d12_resource_create_reserved). The runtime asks CheckFormatSupport (CheckMultisampleQualityLevels with
// TILED_RESOURCE above 1 sample) about the resource's format, while the engine makes a texture with
// ALLOW_DEPTH_STENCIL of the depth format its table pairs with that format (vkd3d_depth_stencil_formats); the
// question here is about the format the engine makes. Until the line is written, a single-sample reserved texture
// asks the engine once per format and a multisample one each time; after it, nothing is asked.
LogOnce g_untiled_reserved;
FormatSet g_tiled_formats;

DXGI_FORMAT engine_depth_format(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_UNORM: return DXGI_FORMAT_D16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_R32_FLOAT: return DXGI_FORMAT_D32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default: return format;
    }
}

void log_untiled_reserved(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc, HRESULT result) noexcept {
    if (g_untiled_reserved.done() || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    const DXGI_FORMAT f =
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? engine_depth_format(desc.Format) : desc.Format;
    const UINT samples = desc.SampleDesc.Count;
    bool tiled = false;
    if (samples <= 1) {
        if (g_tiled_formats.contains(static_cast<uint32_t>(f))) return;
        HRESULT hr = S_OK;
        tiled = (engine_format(c, f, &hr).Support2 & D3D12_FORMAT_SUPPORT2_TILED) != 0;
        if (tiled) g_tiled_formats.insert(static_cast<uint32_t>(f));
    } else {
        tiled = engine_quality_levels(c, f, samples, D3D12_MULTISAMPLE_QUALITY_LEVELS_FLAG_TILED_RESOURCE) != 0;
    }
    if (tiled || !g_untiled_reserved.first()) return;
    log_refusal("CreateHeapAndResource: first reserved texture the engine reports no tiled support for: format %u "
                "(asked as %u), %llu x %u, array %u, mips %u, samples %u, flags 0x%x, result %08lx",
                static_cast<unsigned>(desc.Format), static_cast<unsigned>(f),
                static_cast<unsigned long long>(desc.Width), desc.Height, static_cast<unsigned>(desc.DepthOrArraySize),
                static_cast<unsigned>(desc.MipLevels), samples, static_cast<unsigned>(desc.Flags),
                static_cast<unsigned long>(result));
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
    // A stored video format, one sample here (the engine knows no such format, so no multisample target above), has
    // its view format's levels.
    if (const StoredFormat* stored = stored_format(format)) format = stored->view;
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

// The first answer for each format value, logged with the engine's own: what the runtime was told and from which
// engine answer. Once per format, so at most 257 lines a process.
FormatSet g_format_answers;

// The engine's answer (engine_format_support), with one exception. The runtime does not take 0 as "no such format"
// for R10G10B10_XR_BIAS_A2_UNORM: answered 0 while the engine had no such format, it offered the application a
// displayable 2D texture format with TEXTURE2D, DISPLAY, BACK_BUFFER_CAST and TILED (266), and a texture of it would
// then be refused by the engine. NOT_SUPPORTED says "not supported at all"; d3d12umddi.h defines it for this format
// only and as its only bit, so it is the answer while the engine makes no 2D texture of the format.
void APIENTRY check_format_support(D3D12DDI_HDEVICE device, DXGI_FORMAT format, UINT* out) {
    DeviceContext* c = resolve(device);
    if (!c) return;
    if (!out) {
        c->report(E_INVALIDARG);
        return;
    }
    HRESULT hr = S_OK;
    // A stored video format is answered from its view format (internal.h, StoredFormat), limited to the bits the list
    // allows the video format; neither allows a multisample target.
    const StoredFormat* stored = stored_format(format);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT s = engine_format(c, stored ? stored->view : format, &hr);
    s.Format = format;
    *out = ddi_format_support(c, s);
    if (format == DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM && !(s.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D))
        *out = D3D12DDI_FORMAT_SUPPORT_NOT_SUPPORTED;
    if (g_format_answers.insert(static_cast<uint32_t>(format))) {
        // A stored format's line names the view format the engine was asked about.
        char view[32] = "";
        if (stored) std::snprintf(view, sizeof(view), " view format %u,", static_cast<unsigned>(stored->view));
        log_refusal("CheckFormatSupport: format %u:%s engine %08lx, Support1 %#x, Support2 %#x; answer %#x",
                    static_cast<unsigned>(format), view, static_cast<unsigned long>(hr),
                    static_cast<unsigned>(s.Support1), static_cast<unsigned>(s.Support2), *out);
    }
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
    const UINT64 dst_offset = dst.BaseAddress.UMD.Offset, src_offset = src.BaseAddress.UMD.Offset;
    record(l, [=](ID3D12GraphicsCommandList* e) { e->CopyBufferRegion(d, dst_offset, s, src_offset, bytes); });
}

UINT block_size(DXGI_FORMAT f) noexcept {
    return ((f >= DXGI_FORMAT_BC1_TYPELESS && f <= DXGI_FORMAT_BC5_SNORM) ||
            (f >= DXGI_FORMAT_BC6H_TYPELESS && f <= DXGI_FORMAT_BC7_UNORM_SRGB))
               ? 4u
               : 1u;
}

// D3D12's placed footprint has no slice pitch: the engine derives it as the row pitch times the footprint's rows of
// blocks. The runtime's pitched layouts do carry one, and a placement of several slices whose slice pitch differs
// from the derived one cannot be stated as a footprint. Such a placement is answered with split_pitch = its slice
// pitch, and copy_texture_region copies it one slice at a time, slice n at Offset + n x SlicePitch. A slice pitch
// below the derived one would overlap the slices and stays refused (272 and 273: The Ascent lost its device to that
// E_NOTIMPL for a BC1 volume, the derived pitch taken from its texel height as if it counted rows of blocks). The
// first of each kind is logged on the debugger's output too (a game's stderr goes nowhere): two lines a process at
// most.
HRESULT pitched_slices(const char* kind, DXGI_FORMAT format, UINT width, UINT height, UINT depth, UINT pitch,
                       UINT slice_pitch, UINT rows, UINT64& split_pitch) noexcept {
    const uint64_t derived = static_cast<uint64_t>(pitch) * rows;
    if (depth <= 1 || slice_pitch == derived) return S_OK;
    const bool split = slice_pitch > derived;
    static std::atomic<bool> once_split{false}, once_refused{false};
    if (!(split ? once_split : once_refused).exchange(true))
        log_refusal("CopyTextureRegion: %s placement format %u, %u x %u x %u, pitch %u, slice pitch %u, "
                    "derived %llu: %s",
                    kind, static_cast<unsigned>(format), width, height, depth, pitch, slice_pitch,
                    static_cast<unsigned long long>(derived),
                    split ? "copied one slice at a time" : "refused (E_NOTIMPL)");
    if (!split) return E_NOTIMPL;
    split_pitch = slice_pitch;
    return S_OK;
}

HRESULT copy_location(CommandListRecord* l, const D3D12DDIARG_BUFFER_PLACEMENT* p, const D3D12DDIARG_PLACED_RESOURCE& r,
                      D3D12_TEXTURE_COPY_LOCATION& out, UINT64& split_pitch) noexcept {
    split_pitch = 0;
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
        // The physical size is in texels, rounded up to whole blocks (d3d12umddi.h's "Block dimensions"), not in
        // blocks: 273's BC1 volume came as 32 x 32 x 32, pitch 256, slice pitch 2048 = 8 rows of blocks, which only
        // texels explain (32 blocks a row would need 32 rows, 8192 bytes a slice). Multiplied by the block size it
        // was a footprint four times too wide and too high: clamped to the image in 2D, a fourfold slice pitch in 3D.
        const auto* f = static_cast<const D3D12DDIARG_PHYSICAL_SUBRESOURCE_PITCHED_LAYOUT*>(r.pLayout);
        if (!f) return E_INVALIDARG;
        const UINT bs = block_size(f->Format);
        HRESULT hr = pitched_slices("physical", f->Format, f->PhysicalWidth, f->PhysicalHeight, f->PhysicalDepth,
                                    f->Pitch, f->SlicePitch, (f->PhysicalHeight + bs - 1) / bs, split_pitch);
        if (FAILED(hr)) return hr;
        out.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        out.PlacedFootprint.Offset = p->BaseAddress.UMD.Offset;
        out.PlacedFootprint.Footprint = {f->Format, f->PhysicalWidth, f->PhysicalHeight, f->PhysicalDepth, f->Pitch};
        return S_OK;
    }
    case D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED: {
        const auto* f = static_cast<const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT*>(r.pLayout);
        if (!f) return E_INVALIDARG;
        // The footprint is the virtual size; the engine's rows of blocks follow from its height.
        const UINT bs = block_size(f->Format);
        HRESULT hr = pitched_slices("virtual", f->Format, f->VirtualWidth, f->VirtualHeight, f->VirtualDepth,
                                    f->Pitch, f->SlicePitch, (f->VirtualHeight + bs - 1) / bs, split_pitch);
        if (FAILED(hr)) return hr;
        out.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        out.PlacedFootprint.Offset = p->BaseAddress.UMD.Offset;
        out.PlacedFootprint.Footprint = {f->Format, f->VirtualWidth, f->VirtualHeight, f->VirtualDepth, f->Pitch};
        return S_OK;
    }
    default:
        return E_INVALIDARG;
    }
}

// The first virtual placement of a process, as the slot received it. The engine's footprint takes the virtual size;
// the line shows the physical size, both pitches, the offset and the box that came with it.
LogOnce g_virtual_placement;

void log_virtual_placement(const char* side, const D3D12DDIARG_BUFFER_PLACEMENT* p,
                           const D3D12DDIARG_PLACED_RESOURCE& r, const D3D12DDI_BOX* box) noexcept {
    if (r.Layout != D3D12DDI_RL_PLACED_VIRTUAL_SUBRESOURCE_PITCHED || g_virtual_placement.done()) return;
    const auto* f = static_cast<const D3D12DDIARG_VIRTUAL_SUBRESOURCE_PITCHED_LAYOUT*>(r.pLayout);
    if (!p || !f || !g_virtual_placement.first()) return;
    char b[96] = "none";
    if (box)
        std::snprintf(b, sizeof(b), "(%ld, %ld, %ld) to (%ld, %ld, %ld)", box->Left, box->Top, box->Front, box->Right,
                      box->Bottom, box->Back);
    log_refusal("CopyTextureRegion: first virtual placement, %s: format %u, virtual %u x %u x %u, physical %u x %u x "
                "%u, pitch %u, slice pitch %u, offset %llu, box %s",
                side, static_cast<unsigned>(f->Format), f->VirtualWidth, f->VirtualHeight, f->VirtualDepth,
                f->PhysicalWidth, f->PhysicalHeight, f->PhysicalDepth, f->Pitch, f->SlicePitch,
                static_cast<unsigned long long>(p->BaseAddress.UMD.Offset), b);
}

// Copies between a footprint and a depth-stencil texture: Vulkan wants a multiple of 4 as the buffer offset of a
// depth or stencil aspect, and the engine states no restriction (UnrestrictedBufferTextureCopyPitchSupported). The
// first such copy of a process is logged, and the first whose footprint offset is not a multiple of 4: two lines at
// most, the same one when the first copy is that. Once the first is written, only a footprint at an offset not a
// multiple of 4 makes a copy read the texture's record; once both are, a copy costs two relaxed loads.
LogOnce g_depth_copy, g_depth_copy_unaligned;

bool depth_stencil_texture(const D3D12_RESOURCE_DESC1& desc) noexcept {
    switch (desc.Format) {
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return true;
    default:
        return (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
    }
}

void log_depth_copy(CommandListRecord* l, const D3D12DDIARG_BUFFER_PLACEMENT* pdst,
                    const D3D12_TEXTURE_COPY_LOCATION& d, const D3D12DDIARG_BUFFER_PLACEMENT* psrc,
                    const D3D12_TEXTURE_COPY_LOCATION& s) noexcept {
    if (g_depth_copy.done() && g_depth_copy_unaligned.done()) return;
    constexpr auto kIndex = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    constexpr auto kFootprint = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    const bool up = d.Type == kIndex && s.Type == kFootprint;
    if (!up && !(d.Type == kFootprint && s.Type == kIndex)) return;
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& f = (up ? s : d).PlacedFootprint;
    if (g_depth_copy.done() && f.Offset % 4 == 0) return;
    const auto* r = record_of<ResourceRecord>((up ? pdst : psrc)->BaseAddress.UMD.hResource.pDrvPrivate, Tag::Resource,
                                              l->h.device);
    if (!r || !depth_stencil_texture(r->desc)) return;
    const UINT subresource = (up ? d : s).SubresourceIndex;
    const bool first = g_depth_copy.first();
    const bool first_unaligned = f.Offset % 4 != 0 && g_depth_copy_unaligned.first();
    if (!first && !first_unaligned) return;
    const UINT mips = r->desc.MipLevels ? r->desc.MipLevels : 1;
    const UINT layers =
        r->desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : std::max<UINT>(1, r->desc.DepthOrArraySize);
    log_refusal("CopyTextureRegion: first depth-stencil copy%s, %s a footprint: resource format %u, subresource %u, "
                "plane %u, footprint format %u, offset %llu (%% 4 = %u, %% 512 = %u), pitch %u",
                first_unaligned ? " with an offset not a multiple of 4" : "", up ? "from" : "to",
                static_cast<unsigned>(r->desc.Format), subresource, subresource / (mips * layers),
                static_cast<unsigned>(f.Footprint.Format), static_cast<unsigned long long>(f.Offset),
                static_cast<unsigned>(f.Offset % 4), static_cast<unsigned>(f.Offset % 512), f.Footprint.RowPitch);
}

// One CopyTextureRegion of a split placement as one engine copy per slice. The other side is a subresource: a
// buffer-to-buffer copy is not a texture copy. Source slices come from the box (or the whole footprint); destination
// slices from the box (or the whole source subresource, sized from the shell's record of the source resource).
HRESULT copy_slices(CommandListRecord* l, const D3D12_TEXTURE_COPY_LOCATION& d, UINT64 dpitch, UINT x, UINT y, UINT z,
                    const D3D12_TEXTURE_COPY_LOCATION& s, UINT64 spitch, const ResourceRecord* src, bool has_box,
                    const D3D12_BOX& b) noexcept {
    const bool src_split = spitch != 0;
    const D3D12_TEXTURE_COPY_LOCATION& other = src_split ? d : s;
    if ((dpitch && spitch) || other.Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX) return E_INVALIDARG;
    D3D12_BOX area = b;
    if (!has_box) {
        if (src_split) {
            const D3D12_SUBRESOURCE_FOOTPRINT& f = s.PlacedFootprint.Footprint;
            area = {0, 0, 0, f.Width, f.Height, f.Depth};
        } else {
            if (!src) return E_INVALIDARG;
            const D3D12_RESOURCE_DESC1& desc = src->desc;
            const UINT mips = desc.MipLevels ? desc.MipLevels : 1;
            const UINT mip = s.SubresourceIndex % mips;
            const UINT depth = desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? desc.DepthOrArraySize : 1;
            area = {0, 0, 0, static_cast<UINT>(std::max<UINT64>(1, desc.Width >> mip)),
                    std::max(1u, desc.Height >> mip), std::max(1u, depth >> mip)};
        }
    }
    if (area.back <= area.front) return S_OK;
    record(l, [=](ID3D12GraphicsCommandList* e) {
        for (UINT n = area.front; n < area.back; ++n) {
            D3D12_TEXTURE_COPY_LOCATION dn = d, sn = s;
            D3D12_BOX bn = area;
            UINT zn = z + (n - area.front);
            if (src_split) {
                // Slice n of the source footprint, one slice deep, to destination slice z + (n - front).
                sn.PlacedFootprint.Offset += n * spitch;
                sn.PlacedFootprint.Footprint.Depth = 1;
                bn.front = 0;
                bn.back = 1;
            } else {
                // Source slice n to footprint slice zn, each slice its own one-slice-deep footprint.
                dn.PlacedFootprint.Offset += static_cast<UINT64>(zn) * dpitch;
                dn.PlacedFootprint.Footprint.Depth = 1;
                bn.front = n;
                bn.back = n + 1;
                zn = 0;
            }
            e->CopyTextureRegion(&dn, x, y, zn, &sn, &bn);
        }
    });
    return S_OK;
}

// One side of a copy in the engine's elements (internal.h, StoredFormat): a footprint of a stored video format becomes
// one of its storage, as many elements wide as its pixels fill. The answer is the side's pixels per element, from the
// footprint's format or from the record of the subresource's resource; x and the box are the caller's to divide.
UINT stored_side(CommandListRecord* l, const D3D12DDIARG_BUFFER_PLACEMENT* p,
                 D3D12_TEXTURE_COPY_LOCATION& loc) noexcept {
    if (loc.Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT) {
        D3D12_SUBRESOURCE_FOOTPRINT& f = loc.PlacedFootprint.Footprint;
        const StoredFormat* s = stored_format(f.Format);
        if (!s) return 1;
        f.Format = s->storage;
        f.Width = (f.Width + s->pixels - 1) / s->pixels;
        return s->pixels;
    }
    const auto* r = record_of<ResourceRecord>(p->BaseAddress.UMD.hResource.pDrvPrivate, Tag::Resource, l->h.device);
    const StoredFormat* s = r ? stored_format(r->stored_from) : nullptr;
    return s ? s->pixels : 1;
}

void APIENTRY copy_texture_region(D3D12DDI_HCOMMANDLIST hlist, const D3D12DDIARG_BUFFER_PLACEMENT* pdst,
                                  D3D12DDIARG_PLACED_RESOURCE dst, UINT x, UINT y, UINT z,
                                  const D3D12DDIARG_BUFFER_PLACEMENT* psrc, D3D12DDIARG_PLACED_RESOURCE src,
                                  const D3D12DDI_BOX* box) {
    CommandListRecord* l = list_of(hlist, "CopyTextureRegion");
    if (!l) return;
    D3D12_TEXTURE_COPY_LOCATION d, s;
    UINT64 dpitch = 0, spitch = 0;
    HRESULT hr = copy_location(l, pdst, dst, d, dpitch);
    if (SUCCEEDED(hr)) hr = copy_location(l, psrc, src, s, spitch);
    if (SUCCEEDED(hr)) {
        // Diagnostic lines, before the box is checked: they describe the call as the slot received it.
        log_virtual_placement("destination", pdst, dst, box);
        log_virtual_placement("source", psrc, src, box);
        log_depth_copy(l, pdst, d, psrc, s);
    }
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
    const bool has_box = box != nullptr;
    // A 4:2:2 copy starts on an even pixel (the format's 2 x 1 block); a source box ending on an odd one, at the
    // edge of an odd-width footprint, takes its last element whole.
    const UINT dst_pixels = stored_side(l, pdst, d), src_pixels = stored_side(l, psrc, s);
    x /= dst_pixels;
    b.left /= src_pixels;
    b.right = (b.right + src_pixels - 1) / src_pixels;
    if (dpitch || spitch) {
        const auto* sr = record_of<ResourceRecord>(psrc->BaseAddress.UMD.hResource.pDrvPrivate, Tag::Resource, l->h.device);
        hr = copy_slices(l, d, dpitch, x, y, z, s, spitch, sr, has_box, b);
        if (FAILED(hr)) l->h.device->report_list(l->rt, hr);
        return;
    }
    record(l, [=](ID3D12GraphicsCommandList* e) { e->CopyTextureRegion(&d, x, y, z, &s, has_box ? &b : nullptr); });
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
    record(l, [=](ID3D12GraphicsCommandList* e) { e->CopyResource(d, s); });
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
        record(l, [=](ID3D12GraphicsCommandList* e) { e->DiscardResource(r, nullptr); });
        return;
    }
    const UINT rects = args->NumRects, first = args->FirstSubresource, subresources = args->NumSubresources;
    record(l,
           [=](ID3D12GraphicsCommandList* e, const D3D12_RECT* p) {
               const D3D12_DISCARD_REGION region{rects, p, first, subresources};
               e->DiscardResource(r, &region);
           },
           in(args->pRects, rects));
}

void APIENTRY resource_barrier(D3D12DDI_HCOMMANDLIST hlist, UINT count, const D3D12DDIARG_RESOURCE_BARRIER_0022* ddi) {
    CommandListRecord* l = list_of(hlist, "ResourceBarrier");
    if (!l) return;
    if (count && !ddi) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    InlineArray<D3D12_RESOURCE_BARRIER, 64> out;
    if (!out.reserve(count)) {
        l->h.device->report_list(l->rt, E_OUTOFMEMORY);
        return;
    }
    for (UINT i = 0; i < count; ++i) {
        const D3D12DDIARG_RESOURCE_BARRIER_0022& b = ddi[i];
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
        out.data()[i] = a;
    }
    if (count)
        record(l, [=](ID3D12GraphicsCommandList* e, const D3D12_RESOURCE_BARRIER* b) { e->ResourceBarrier(count, b); },
               in(out.data(), count));
}
} // namespace

// ---- Backing lifetime -------------------------------------------------------------------------------------------
void backing_acquire(Backing* b) noexcept { b->refs.fetch_add(1); }

void backing_release(Backing* b) noexcept {
    if (b->refs.fetch_sub(1) != 1) return;
    DeviceContext* c = b->device;
    PendingRelease* node = b->release_node;
    node->payload = ReleasePayload{{b->retained, b->heap}, b->imported, b->memory, b->id, b->linear};  // resource first
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

HRESULT present_allocation(DeviceContext* c, D3D12DDI_HRESOURCE hres, D3DKMT_HANDLE* allocation) noexcept {
    if (!allocation) return E_INVALIDARG;
    *allocation = 0;
    if (!c) return E_INVALIDARG;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r || c->mode != MemoryMode::RuntimeBacked || r->kind != ResourceKind::Committed || !r->backing ||
        !r->backing->imported || !r->backing->linear || r->offset || !r->linear_row_pitch ||
        !r->backing->memory.allocation)
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
    // D64 and D65 refuse every shared open, but they are this module's slots, not fail-safes: a fail-safe
    // answers E_NOTIMPL, which the runtime takes as a critical driver failure and pays for with the
    // application's device (BD-075).
    t->pfnCalcPrivateOpenedHeapAndResourceSizes = calc_opened_heap_and_resource;
    t->pfnOpenHeapAndResource = open_heap_and_resource;
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
