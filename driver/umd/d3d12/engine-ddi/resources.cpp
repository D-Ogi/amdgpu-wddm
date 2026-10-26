// SPDX-License-Identifier: MIT
// engine-ddi: heaps and resources (D59-D61, D57-D58, D74, D75, D84, D0, D1), reserved resources (the creation;
// their tile slots are in tiles.cpp), resource_allocation, object_allocation, and the list slots that move data
// between resources (L10, L13, L17).
#include "internal.h"
#include "replay.h"
#include "format-list.h"
#include "tight-alignment.h"
#include "../../../contract/amdgpu_wddm_surface_format.h"
#include "../../../contract/bc250_shared_surface.h"
#include "../present-copy.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdint>
#include <new>
#include <type_traits>

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
// The barrier layouts are swept value by value in the L68 section below (kBarrierLayouts), over the whole range
// UNDEFINED and COMMON to VIDEO_QUEUE_COMMON, which is the range initial_layout below hands to a create as an API
// layout: the static_cast<D3D12_BARRIER_LAYOUT> of a create is therefore an identity the sweep covers, and every
// DDI-only value above that range leaves initial_layout as a legacy state or as E_INVALIDARG, never as a cast.
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

HRESULT to_api_desc(DeviceContext* c, const D3D12DDIARG_CREATERESOURCE_0088& in, D3D12_RESOURCE_DESC1& out) noexcept {
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
#if defined(D3D12DDI_BUILD_VERSION_0119)
    if (f & D3D12DDI_RESOURCE_FLAG_0111_USE_TIGHT_ALIGNMENT) {
        out.Flags |= tight::flag;
        if (!tight::layout_valid(out) || !tight::supported(c->device)) return E_INVALIDARG;
    }
#else
    (void)c;
#endif
    out.SamplerFeedbackMipRegion = {in.SamplerFeedbackMipRegion.Width, in.SamplerFeedbackMipRegion.Height,
                                    in.SamplerFeedbackMipRegion.Depth};
    return S_OK;
}

D3D12_RESOURCE_DESC to_desc0(const D3D12_RESOURCE_DESC1& d) noexcept {
    return {d.Dimension, d.Alignment, d.Width, d.Height, d.DepthOrArraySize, d.MipLevels, d.Format, d.SampleDesc,
            d.Layout, d.Flags};
}

// The engine's size and alignment for desc at the given alignment, with the castable formats of the create. A
// compressed image with unordered access is valid only with an uncompressed castable format (RelaxedFormatCasting,
// which the runtime reports from DDI 0094 on). The engine's GetResourceAllocationInfo takes no castable formats and
// refuses such an image, and Unreal Engine 5 then reports that video memory is exhausted. GetResourceAllocationInfo3
// takes them. Only a description without castable formats goes to the older query: it cannot carry a list, so a list
// without an ID3D12Device12 (or a count without a list) is refused with UINT64_MAX at the default alignment, the
// engine's own refusal shape, and each caller handles it as it handles an engine refusal.
namespace {
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
std::atomic<uint64_t> legacy_sizing_calls{0}, castable_sizing_calls{0};
std::atomic<int64_t> live_castable_copies{0};
std::atomic<bool> fail_next_castable_copy{false};
#endif
std::atomic<bool> castable_refusal_logged{false};
} // namespace

D3D12_RESOURCE_ALLOCATION_INFO engine_allocation_info(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc,
                                                      uint64_t alignment, UINT castable_count,
                                                      const DXGI_FORMAT* castable) noexcept {
    const bool tight_alignment = tight::enabled(desc);
    D3D12_RESOURCE_ALLOCATION_INFO result{};
    if (castable_count) {
        if (!castable || !c->device12) {
            if (!castable_refusal_logged.exchange(true, std::memory_order_relaxed))
                log_line("allocation info: %u castable formats %s: unsized (UINT64_MAX)", castable_count,
                         castable ? "and the engine has no ID3D12Device12" : "without a list");
            return {UINT64_MAX, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT};
        }
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
        castable_sizing_calls.fetch_add(1, std::memory_order_relaxed);
#endif
        D3D12_RESOURCE_DESC1 d1 = desc;
        d1.Alignment = tight_alignment ? 0 : alignment;
        const UINT32 count = castable_count;
        result = c->device12->GetResourceAllocationInfo3(0, 1, &d1, &count, &castable, nullptr);
    } else {
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    legacy_sizing_calls.fetch_add(1, std::memory_order_relaxed);
#endif
    D3D12_RESOURCE_DESC d0 = to_desc0(desc);
    d0.Alignment = tight_alignment ? 0 : alignment;
    result = c->device->GetResourceAllocationInfo(0, 1, &d0);
    }
    if (!tight::normalize(desc, alignment, result)) return {UINT64_MAX, 0};
    return result;
}

} // namespace

// Declared in internal.h: external linkage, outside the file-local namespace.
HRESULT copy_castable(UINT count, const DXGI_FORMAT* formats, DXGI_FORMAT** out) noexcept {
    *out = nullptr;
    if (!count) return S_OK;
    if (!formats) return E_INVALIDARG;
#if SIZE_MAX < UINT64_MAX
    if (uint64_t{count} * sizeof(DXGI_FORMAT) > SIZE_MAX) return E_OUTOFMEMORY;  // a 32-bit size_t
#endif
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    if (fail_next_castable_copy.exchange(false)) return E_OUTOFMEMORY;
#endif
    auto* copy = new (std::nothrow) DXGI_FORMAT[count];
    if (!copy) return E_OUTOFMEMORY;
    std::memcpy(copy, formats, size_t{count} * sizeof(DXGI_FORMAT));
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    live_castable_copies.fetch_add(1);
#endif
    *out = copy;
    return S_OK;
}

void free_castable(DXGI_FORMAT* formats) noexcept {
    if (!formats) return;
#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
    live_castable_copies.fetch_sub(1);
#endif
    delete[] formats;
}

#ifdef AMDGPU_WDDM_ENGINE_DDI_HARNESS
void harness_fail_next_castable_copy() noexcept { fail_next_castable_copy.store(true); }
uint64_t harness_legacy_sizing_calls() noexcept { return legacy_sizing_calls.load(); }
uint64_t harness_castable_sizing_calls() noexcept { return castable_sizing_calls.load(); }
int64_t harness_live_castable_copies() noexcept { return live_castable_copies.load(); }
#endif

namespace {

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

// The initial layout of a create: a buffer's is UNDEFINED, whatever the runtime passed, and the rest is the
// shared rule (internal.h, legacy_initial_state): S_OK and *state for one of the five DDI-only layouts, S_FALSE
// for a layout the engine takes as it is, E_INVALIDARG for a value the DDI does not define. The S_FALSE range is
// the one kBarrierLayouts sweeps, so the caller's static_cast to the API layout is an identity there.
HRESULT initial_layout(const D3D12_RESOURCE_DESC1& desc, D3D12DDI_BARRIER_LAYOUT* layout,
                       D3D12_RESOURCE_STATES* state) noexcept {
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) *layout = D3D12DDI_BARRIER_LAYOUT_UNDEFINED;
    return legacy_initial_state(*layout, state);
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
        info = engine_allocation_info(c, desc, alignment, castable_count, castable);
        if (info.SizeInBytes == UINT64_MAX || !info.Alignment || offset % info.Alignment) continue;
        if (alignment && !tight::enabled(desc) && info.Alignment != alignment) continue; // exact legacy alignment
        if (offset > b->desc.ByteSize || info.SizeInBytes > b->desc.ByteSize - offset) continue;  // a smaller may fit
        fits = true;
        break;
    }
    if (!fits) return E_INVALIDARG;
    desc.Alignment = d0.Alignment;
    D3D12_RESOURCE_DESC1 engine_desc = desc;
    if (tight::enabled(desc)) engine_desc.Alignment = 0; // The DDI floor is checked above; engine owns its alignment.
    const D3D12_CLEAR_VALUE* cv = clear_value(desc, clear);
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const HRESULT legacy = initial_layout(desc, &layout, &state);
    if (FAILED(legacy)) return legacy;
    if (legacy == S_OK) {
        if (castable_count) {                           // CreatePlacedResource1 takes no castable formats
            log_line("placed resource: %u castable formats with a legacy initial state: E_NOTIMPL", castable_count);
            return E_NOTIMPL;
        }
        return c->device8->CreatePlacedResource1(b->heap, offset, &engine_desc, state, cv, __uuidof(ID3D12Resource),
                                                 reinterpret_cast<void**>(out));
    }
    return c->device10->CreatePlacedResource2(b->heap, offset, &engine_desc, static_cast<D3D12_BARRIER_LAYOUT>(layout), cv,
                                              castable_count, castable, __uuidof(ID3D12Resource),
                                              reinterpret_cast<void**>(out));
}

// ---- The linear surface: the primary and the shared resource (engine ABI 1.3 V13) ---------------------------------
// A committed texture a reader outside the engine opens and reads by row pitch has linear tiling. There are two of
// them and they are selected in two different ways:
//   - the primary, which the desktop compositor opens: the heap's D3D12DDI_HEAP_FLAG_PRIMARY at the create and the
//     PRIMARY optimization flag at CheckResourceAllocationInfo say so;
//   - a resource in the shared-capable envelope (BD-075, BD-217): the DDI cannot
//     distinguish an actual shared create, so the shell requests this layout
//     before allocating. See create_heap_and_resource and kMemoryShareable.
// Ordinary textures inside that envelope use the same linear representation.

// Every create this process turned into a shared linear surface. Relaxed: it is read for a report, never for a
// decision, and one retry per shared create is not a hot path.
std::atomic<uint64_t> shared_retries{0};

struct LinearSurface {
    BC250_VKD3D_LINEAR_IMAGE_INFO info;
    uint64_t backing_size;
};

inline constexpr uint32_t kLinearMaxEdge = 8192;
inline constexpr uint64_t kLinearPage = 4096;

// The formats a linear surface may have: a COMPOSED row of the surface format table by its storage format, or by
// the sRGB view that shares that storage and that LB7A format. This is the one lookup the whole stack uses -
// Bc250SharedSurfaceFormat (driver/contract/bc250_shared_surface.h), which the wire's encoder and the D3D11
// shell's runtime_surface_format call as well. It used to be amdgpu_wddm_surface_format_by_dxgi, the storage
// column alone, and that one column narrower than the wire was a gap, not a policy: a D3D11-created
// B8G8R8A8_UNORM_SRGB shared surface decoded here and then died in query_linear_surface with E_NOTIMPL, and an
// sRGB shared create never reached its retry because shared_surface_shape refused the description (BD-075
// review, 2026-10-06). A format the wire admits and this module refuses is unopenable and uncreatable, and
// nothing in either shell said so.
const AMDGPU_WDDM_SURFACE_FORMAT* composed_format(DXGI_FORMAT f) noexcept {
    return Bc250SharedSurfaceFormat(static_cast<unsigned long>(f));
}

// The descriptions a linear surface exists for. The engine's image has the format's own compatibility list, so
// castable formats beyond the row's storage format and its sRGB view, where the row has one, are not these. The
// description's own format is one of those two, so both are admitted whichever of them it is.
bool linear_surface_shape(const D3D12_RESOURCE_DESC1& desc, const D3D12DDIARG_CREATERESOURCE_0088& in) noexcept {
    const AMDGPU_WDDM_SURFACE_FORMAT* row = composed_format(desc.Format);
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !row) return false;
    const auto stored = static_cast<DXGI_FORMAT>(row->dxgi);
    const auto sibling = static_cast<DXGI_FORMAT>(row->dxgi_srgb);
    if (!desc.Width || desc.Width > kLinearMaxEdge || !desc.Height || desc.Height > kLinearMaxEdge) return false;
    if (desc.DepthOrArraySize != 1 || desc.MipLevels != 1 || desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality)
        return false;
    if (desc.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN && desc.Layout != D3D12_TEXTURE_LAYOUT_ROW_MAJOR) return false;
    if (desc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER |
                      D3D12_RESOURCE_FLAG_VIDEO_DECODE_REFERENCE_ONLY | D3D12_RESOURCE_FLAG_VIDEO_ENCODE_REFERENCE_ONLY |
                      D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE))
        return false;
    for (UINT i = 0; i < in.NumCastableFormats; ++i) {
        const DXGI_FORMAT castable = in.pCastableFormats[i];
        if (castable != stored && (sibling == DXGI_FORMAT_UNKNOWN || castable != sibling)) return false;
    }
    return true;
}

// The shareable envelope (BD-075, BD-217): permission to request the linear
// shared-capable representation, not proof that the runtime asked for sharing.
// No callback HRESULT identifies sharing: GPU-only BC2A can be accepted too.
// Every retry is counted and named on the debugger channel, and the shell's
// experiment switch shared-create-retry-off
// turns the whole behaviour off for a bisect.
//   - the heap is a GPU-only L1 heap without PRIMARY and without COHERENT_SYSTEMWIDE: a shared surface is nobody's
//     primary, and a CPU-visible or system-wide coherent shared heap needs its own contract;
//   - the resource is one of the shapes a linear surface exists for (linear_surface_shape), so a 3D texture, a mip
//     chain, an array, MSAA, a block-compressed or typeless format and a buffer are all outside it;
//   - the resource may ask for a render target, a shader resource, an unordered access view and simultaneous
//     access, and nothing else (linear_surface_shape already refuses depth-stencil, cross-adapter, the two video
//     reference-only flags and a raytracing acceleration structure).
bool shared_surface_shape(const D3D12DDIARG_CREATEHEAP_0001& heap, const D3D12_RESOURCE_DESC1& desc,
                          const D3D12DDIARG_CREATERESOURCE_0088& in) noexcept {
    if (heap.Flags & (D3D12DDI_HEAP_FLAG_PRIMARY | D3D12DDI_HEAP_FLAG_COHERENT_SYSTEMWIDE)) return false;
    if (heap.CPUPageProperty != D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE) return false;
    if (heap.MemoryPool != D3D12DDI_MEMORY_POOL_L1) return false;
    return linear_surface_shape(desc, in);
}

// The description the engine is asked about and creates: the linear request is the call, not the layout field.
D3D12_RESOURCE_DESC1 linear_desc(D3D12_RESOURCE_DESC1 desc) noexcept {
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Alignment = 0;
    return desc;
}

// What the engine's linear image of desc needs, before any memory exists, and the size of the backing: enough for
// the image's memory requirement and for a reader that takes pitch * (height rounded up to 4) bytes. row is the
// surface format table's row the surface will carry; its bytes a pixel bound the pitch.
HRESULT query_linear_surface(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc,
                             const AMDGPU_WDDM_SURFACE_FORMAT* row, LinearSurface* out) noexcept {
    *out = LinearSurface{};
    if (!c->funcs.QueryLinearImage || !row) return E_NOTIMPL;
    const D3D12_RESOURCE_DESC1 d = linear_desc(desc);
    BC250_VKD3D_LINEAR_IMAGE_INFO info{};
    info.Size = sizeof(info);
    const HRESULT hr = c->funcs.QueryLinearImage(c->device, &d, &info);
    if (FAILED(hr)) {
        log_line("linear surface: the engine has no linear image for format %d, %llux%u: %08lx",
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
        log_line("linear surface: layout not usable (offset %llu, pitch %llu, layout %llu, memory %llu, alignment "
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

// Every linear surface but the Present copy's: the composed row of the description's format.
HRESULT query_linear_surface(DeviceContext* c, const D3D12_RESOURCE_DESC1& desc, LinearSurface* out) noexcept {
    return query_linear_surface(c, desc, composed_format(desc.Format), out);
}

// The linear image at offset 0 of the backing. The bound image must be the one the query described: the memory was
// sized, typed and described to the shell from that answer.
HRESULT place_linear_at(DeviceContext* c, Backing* b, const D3D12_RESOURCE_DESC1& desc, D3D12_RESOURCE_STATES state,
                        const D3D12DDI_CLEAR_VALUES* clear, const LinearSurface& surface,
                        ID3D12Resource** out) noexcept {
    *out = nullptr;
    if (!c->funcs.CreateLinearPlacedResource) return E_NOTIMPL;
    const D3D12_RESOURCE_DESC1 d = linear_desc(desc);
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
        log_line("linear surface: the bound image differs from the query (pitch %llu/%llu, layout %llu/%llu, memory "
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

// The create's form: the initial state comes from the DDI's barrier layout.
HRESULT place_linear(DeviceContext* c, Backing* b, const D3D12_RESOURCE_DESC1& desc, D3D12DDI_BARRIER_LAYOUT layout,
                     const D3D12DDI_CLEAR_VALUES* clear, const LinearSurface& surface, ID3D12Resource** out) noexcept {
    *out = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const HRESULT legacy = initial_layout(desc, &layout, &state);
    if (FAILED(legacy)) return legacy;
    return place_linear_at(c, b, desc, state, clear, surface, out);
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
                        DXGI_FORMAT stored_from, UINT castable_count = 0, DXGI_FORMAT* castable = nullptr) noexcept {
    if (b) backing_acquire(b);                          // a reserved resource has none
    if (!stored_format(stored_from)) stored_from = DXGI_FORMAT_UNKNOWN;
    new (r) ResourceRecord{{Tag::Resource, 0, engine, c}, b, offset, desc, rt, kind, kInitNone, nullptr, nullptr,
                           0, 0, stored_from};
    r->castable_count = castable ? castable_count : 0;  // the record owns castable (copy_castable)
    r->castable = castable;
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
        HRESULT hr = to_api_desc(c, *res_desc, desc);
        if (FAILED(hr)) return hr;
        if (res_desc->NumCastableFormats && !res_desc->pCastableFormats) return E_INVALIDARG;
    }
    // The record's copy of the castable formats, made first: a failed copy fails the create before any engine object
    // or backing reference exists. The guard frees it on every return that does not hand it to construct_resource.
    struct CastableCopy {
        DXGI_FORMAT* formats = nullptr;
        ~CastableCopy() { free_castable(formats); }
        DXGI_FORMAT* take() noexcept { DXGI_FORMAT* f = formats; formats = nullptr; return f; }
    } owned;
    if (res_desc) {
        const HRESULT copied = copy_castable(res_desc->NumCastableFormats, res_desc->pCastableFormats, &owned.formats);
        if (FAILED(copied)) return copied;
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
            // Tight ROW_MAJOR reserved buffers are valid. Swizzled tight layouts
            // have already been rejected by to_api_desc; the engine owns sparse sizing.
            ID3D12Resource* engine = nullptr;
            HRESULT hr = reserve(c, desc, res_desc->InitialBarrierLayout, clear, res_desc->NumCastableFormats,
                                 res_desc->pCastableFormats, &engine);
            log_untiled_reserved(c, desc, hr);
            if (FAILED(hr)) return hr;
            construct_resource(static_cast<ResourceRecord*>(hres.pDrvPrivate), c, engine, nullptr, 0, desc, rt,
                               ResourceKind::Reserved, res_desc->Format, res_desc->NumCastableFormats, owned.take());
            return S_OK;
        }
        if (!base->backing) return E_INVALIDARG;        // a reserved resource names no heap memory
        if (base->backing->linear) {
            log_line("placed: the base is a linear surface, its memory holds that image alone");
            return E_INVALIDARG;
        }
        const uint64_t offset = base->offset + res_desc->ReuseBufferGPUVA.BaseAddress.UMD.Offset;
        if (offset < base->offset) return E_INVALIDARG;
        ID3D12Resource* engine = nullptr;
        HRESULT hr = place(c, base->backing, offset, desc, res_desc->InitialBarrierLayout, clear,
                           res_desc->NumCastableFormats, res_desc->pCastableFormats, &engine);
        if (FAILED(hr)) return hr;
        construct_resource(static_cast<ResourceRecord*>(hres.pDrvPrivate), c, engine, base->backing, offset, desc, rt,
                           ResourceKind::Placed, res_desc->Format, res_desc->NumCastableFormats, owned.take());
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
    bool linear = res_desc && c->mode == MemoryMode::RuntimeBacked &&
                  (sized.Flags & D3D12DDI_HEAP_FLAG_PRIMARY) &&
                  sized.CPUPageProperty == D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE &&
                  linear_surface_shape(desc, *res_desc);
    // BD-075, BD-217: allow the shell to select shared-capable storage for this
    // envelope. The first request has no linear layout; kShareRequired asks for it.
    const bool shareable = res_desc && !linear && c->mode == MemoryMode::RuntimeBacked &&
                           shared_surface_shape(sized, desc, *res_desc);
    if (res_desc) {
        if (!(sized.Flags & category_of(desc))) return E_INVALIDARG;
        if (linear) {
            const HRESULT query = query_linear_surface(c, desc, &surface);
            if (FAILED(query)) return query;
            need = surface.backing_size;
            align = surface.info.MemoryAlignment;
            if (sized.ByteSize != kSizeOfResource && sized.ByteSize < need)
                log_line("linear surface: the heap has %llu bytes, the surface needs %llu",
                         static_cast<unsigned long long>(sized.ByteSize), static_cast<unsigned long long>(need));
        } else {
            // A heap alignment below the default is the small placement alignment the resource was granted: its
            // size is the one for that alignment (a 16 KB texture in a 16 KB heap), and place() picks it again.
            D3D12_RESOURCE_DESC d0 = to_desc0(desc);
            if (sized.Alignment && sized.Alignment < D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
                d0.Alignment = sized.Alignment;
#if defined(D3D12DDI_BUILD_VERSION_0119)
            const bool implicit_tight = tight::enabled(desc) && (sized.Flags & D3D12DDI_HEAP_FLAG_0115_IMPLICIT);
            if (implicit_tight) d0.Alignment = std::max<UINT64>(d0.Alignment, 4096);
#endif
            D3D12_RESOURCE_ALLOCATION_INFO info = engine_allocation_info(
                c, desc, d0.Alignment, res_desc->NumCastableFormats, res_desc->pCastableFormats);
            if (info.SizeInBytes == UINT64_MAX && d0.Alignment && !tight::enabled(desc)) {
                d0.Alignment = 0;
                info = engine_allocation_info(c, desc, 0, res_desc->NumCastableFormats, res_desc->pCastableFormats);
            }
            if (info.SizeInBytes == UINT64_MAX || !info.SizeInBytes) return E_INVALIDARG;
            desc.Alignment = d0.Alignment;              // the image is placed with the alignment its memory has
            need = info.SizeInBytes;
            align = info.Alignment;
#if defined(D3D12DDI_BUILD_VERSION_0119)
            // A new backing is a VidMM allocation even before 0115 can identify an implicit heap.
            // Its resource data remains Width bytes; its storage must cover whole 4 KiB pages.
            if (tight::enabled(desc)) {
                if (need > UINT64_MAX - 4095) return E_INVALIDARG;
                need = (need + 4095) & ~UINT64(4095);
                align = std::max<UINT64>(align, 4096);
            }
#endif
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
                log_line("linear surface: heap of %llu bytes aligned to %llu becomes %llu bytes, default alignment",
                         static_cast<unsigned long long>(sized.ByteSize),
                         static_cast<unsigned long long>(sized.Alignment), static_cast<unsigned long long>(need));
            sized.ByteSize = need;
            sized.Alignment = 0;
        }
    } else if (sized.ByteSize == kSizeOfResource) {
        log_line("heap: ByteSize left to a resource, but the heap has none");
        return E_INVALIDARG;
    }
    // The heap holds its one linear image at offset 0 and nothing beside it: the engine is told the image's
    // category alone, whatever else the runtime's flags allow. The shell still sees the runtime's. A shared create
    // calls this after its retry, so the lambda exists instead of a second copy of the three lines.
    const auto narrow_heap_to_image = [&](D3D12_HEAP_DESC& hd) noexcept {
        const uint32_t category = category_of(desc);
        hd.Flags = D3D12_HEAP_FLAG_NONE;
        if (category != D3D12DDI_HEAP_FLAG_BUFFERS) hd.Flags |= D3D12_HEAP_FLAG_DENY_BUFFERS;
        if (category != D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES) hd.Flags |= D3D12_HEAP_FLAG_DENY_RT_DS_TEXTURES;
        if (category != D3D12DDI_HEAP_FLAG_NON_RT_DS_TEXTURES) hd.Flags |= D3D12_HEAP_FLAG_DENY_NON_RT_DS_TEXTURES;
    };
    D3D12_HEAP_DESC hd{};
    HRESULT hr = heap_desc_of(sized, hd);
    if (FAILED(hr)) return hr;
    if (linear) narrow_heap_to_image(hd);

    ID3D12Heap* heap = nullptr;
    ImportedMemory memory{};
    bool imported = false;
    if (c->mode == MemoryMode::RuntimeBacked) {
        if (c->lost()) return DXGI_ERROR_DEVICE_REMOVED;
        // One request, filled from the heap description as it stands and from the surface when there is one.
        const auto build_request = [&](bool share) noexcept {
            MemoryRequest request{};
            request.size = sizeof(MemoryRequest);
            request.flags = (res_desc ? kMemoryDedicated : 0u) |
                            ((sized.Flags & D3D12DDI_HEAP_FLAG_PRIMARY) ? kMemoryPrimary : 0u) |
                            (share ? kMemoryShareable : 0u);
            request.rt_owner = rt;
            request.heap = heap_desc;
            request.resource = res_desc;
            request.byte_size = sized.ByteSize;
            request.alignment = std::max<uint64_t>(align, sized.Alignment);
            request.memory_type_bits = 0;               // engine-ddi.h, MemoryRequest: the engine checks the type
            if (linear) {
                request.flags |= kMemoryLinearSurface;
                request.alignment = align;
                request.memory_type_bits = surface.info.MemoryTypeBits;
                request.surface_row_pitch = static_cast<uint32_t>(surface.info.RowPitch);
                request.surface_layout_size = surface.info.LayoutSize;
            }
            return request;
        };
        hr = import_memory(c, build_request(shareable), &memory);
        if (hr == kShareRequired) {
            // BD-075, BD-217: the shell requests a shared-capable surface before
            // allocating. Obtain the actual linear layout before the retry.
            const HRESULT query = query_linear_surface(c, desc, &surface);
            if (FAILED(query)) {
                log_line("shared surface: the shell requested LB7A but the engine has no linear image "
                         "for this description: %08lx",
                         static_cast<unsigned long>(query));
                return query;
            }
            linear = true;
            need = surface.backing_size;
            align = surface.info.MemoryAlignment;
            // The runtime sized the heap from the tiled answer, so its ByteSize is not compared with the linear
            // need: the heap becomes the surface, exactly as a linear primary's does.
            // Keep the representation choice observable, including private
            // textures inside the shared-capable envelope.
            // It goes to the debugger channel with no trace build, under the refusal budget, and it is counted
            // here so that a trial and a game can both be asked how many retries happened (BD-075 review).
            shared_retries.fetch_add(1, std::memory_order_relaxed);
            log_refusal("shared surface: the shell requested a shareable surface; retrying as a shared "
                        "linear surface (format %d, %llux%u, heap of %llu bytes aligned to %llu becomes %llu bytes, "
                        "default alignment, pitch %llu)",
                        static_cast<int>(desc.Format), static_cast<unsigned long long>(desc.Width), desc.Height,
                        static_cast<unsigned long long>(sized.ByteSize),
                        static_cast<unsigned long long>(sized.Alignment), static_cast<unsigned long long>(need),
                        static_cast<unsigned long long>(surface.info.RowPitch));
            sized.ByteSize = need;
            sized.Alignment = 0;
            hd = D3D12_HEAP_DESC{};
            hr = heap_desc_of(sized, hd);
            if (FAILED(hr)) return hr;
            narrow_heap_to_image(hd);
            hr = import_memory(c, build_request(true), &memory);
        }
        if (FAILED(hr)) {
            // BD-101. The code this slot reports to the runtime is clamped to E_OUTOFMEMORY, because a create
            // function of a user-mode display driver is in the AllowOutOfMemory category and the runtime treats
            // every other code as a critical driver failure: it removes the application's device and sets the
            // removed reason to DXGI_ERROR_DRIVER_INTERNAL_ERROR (windows-driver-docs display
            // handling-errors.md; measured on this slot in BD-075, where E_NOTIMPL cost the device in all 13
            // shared-resource cells). Passing an argument refusal through as E_INVALIDARG would therefore turn
            // a failed create into a lost device, which is worse than the wrong code. So the line is the place
            // that must not mislead, and it says which of the two this was: two lab sessions read
            // "0x8007000e, out of memory" for a 4 MiB alignment refusal with 8 GB free. The shell's own
            // "heap import refused" line of the same call names the stage that declined.
            const bool argument = hr == E_INVALIDARG || hr == E_NOTIMPL;
            log_refusal("heap: the shell refused the memory request: %08lx%s; it is reported as %08lx, the only "
                        "code this slot may report (%llu bytes, alignment %llu, heap flags 0x%x)",
                        static_cast<unsigned long>(hr),
                        argument ? " - an argument refusal, not a memory shortage" : "",
                        static_cast<unsigned long>(admitted_create_failure(hr)),
                        static_cast<unsigned long long>(hd.SizeInBytes),
                        static_cast<unsigned long long>(hd.Alignment), static_cast<unsigned>(hd.Flags));
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
        construct_resource(record, c, engine, b, 0, desc, rt, ResourceKind::Committed, res_desc->Format,
                           res_desc->NumCastableFormats, owned.take());
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
        // A create DDI may only report E_OUTOFMEMORY or D3DDDIERR_DEVICEREMOVED (engine-ddi.h,
        // admitted_create_failure): the refusal below is the driver's answer, not a reason for the runtime to
        // remove the application's device. BD-075. Both codes are on the one line, because a refused create is
        // now survivable: an application may probe by creating and tolerating failure, and the debugger channel
        // costs a raised-and-swallowed exception per line.
        const HRESULT admitted = admitted_create_failure(hr);
        // The call's shape (which of heap only, committed, placed or reserved) and both descriptions: which
        // shape the runtime uses for which API call is otherwise unlogged (engine-ddi.h).
        const D3D12DDIARG_CREATEHEAP_0001 h = heap_desc ? *heap_desc : D3D12DDIARG_CREATEHEAP_0001{};
        const D3D12DDIARG_CREATERESOURCE_0088* r = res_desc;
        log_refusal("CreateHeapAndResource: %08lx reported as %08lx; heap description %s (%llu bytes, "
                    "alignment %llu, flags 0x%x, "
                    "pool %u, cpu page %u), heap handle %s; resource description %s (type %u, %llux%u, depth %u, "
                    "mips %u, format %u, samples %u, layout %u, flags 0x%x, castable %u), base resource %s, "
                    "offset %llu",
                    static_cast<unsigned long>(hr), static_cast<unsigned long>(admitted),
                    heap_desc ? "given" : "none",
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
        return admitted;
    }
    return hr;
}

// ---- D64, D65: OpenSharedHandle of a heap or a resource ----------------------------------------------------------
// BD-075. An opened shared resource is the same thing a shared create makes: a linear 2D image of a COMPOSED
// format at offset 0 of memory a reader outside the engine reads by row pitch. What differs is the ownership of the
// allocation: the runtime opened it, so the shell adopts it (ShellHooks::adopt_memory) and never deallocates it.
// D3D12DDIARG_OPENHEAP_0003 carries no resource description at all, so the two private-data records are the only
// source there is, and they are decoded by the one reader in driver/contract/bc250_shared_surface.h that the D3D11
// shell uses as well. The runtime keeps its own copy of the description for the application's GetDesc; this code
// has to agree with it, not supply it.
// Still refused, each with E_OUTOFMEMORY and a line naming the step that declined: a shared heap with no resource
// (nothing to place and no description to decode), a placed resource on such a heap, anything but exactly one
// allocation, a record the shared decoder declines, and a record whose pitch or size this engine's linear image of
// the same description disagrees with.
// Calc answers the private sizes the open constructs, the same records a create builds, so a runtime that calls
// Calc and then Open sees one shape.
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

// The D3D12 description of a decoded shared surface: the record's own geometry, one mip, one slice, one sample, an
// unknown layout (the linear request is the call, not the field), and the resource flags the record's D3D11 bind
// flags mean. Without the shader-resource bind flag the image denies shader resources, which is what the creator
// asked for.
D3D12_RESOURCE_DESC1 opened_desc(const BC250_SHARED_SURFACE& surface) noexcept {
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = surface.Width;
    desc.Height = surface.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = static_cast<DXGI_FORMAT>(surface.DxgiFormat);
    desc.SampleDesc = {1, 0};
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (surface.BindFlags & BC250_SHARED_BIND_RENDER_TARGET) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (surface.BindFlags & BC250_SHARED_BIND_UNORDERED_ACCESS)
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (!(surface.BindFlags & BC250_SHARED_BIND_SHADER_RESOURCE))
        desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    return desc;
}

// The open itself. On a refusal nothing is constructed, nothing of the runtime's is kept, and *why names the step
// that declined for the caller's one log line.
HRESULT open_shared_surface(DeviceContext* c, const D3D12DDIARG_OPENHEAP_0003* args, D3D12DDI_HHEAP hheap,
                            D3D12DDI_HRTRESOURCE rt, D3D12DDI_HRESOURCE hres, const char** why) noexcept {
    *why = "arguments";
    if (!c || !args || !hheap.pDrvPrivate || !hres.pDrvPrivate) return E_INVALIDARG;
    if (c->mode != MemoryMode::RuntimeBacked) return E_NOTIMPL;
    if (args->NumAllocations != 1 || !args->pOpenAllocationInfo) return E_INVALIDARG;
    const D3DDDI_OPENALLOCATIONINFO& first = args->pOpenAllocationInfo[0];
    if (!first.hAllocation) return E_INVALIDARG;
    if (c->lost()) return DXGI_ERROR_DEVICE_REMOVED;

    // The decoder names the check it decided at, and *why is that name: "record" alone covered seventeen checks, and
    // the three cross-API failures of round 1 all declined here. The refusal line below already printed the record's
    // own length and version, so the name puts the reason first rather than adding information (BD-075 round 2,
    // 2026-10-06). The strings are the header's literals, so the pointer outlives this call.
    *why = BC250_SHARED_SURFACE_WHY_ARGUMENTS;
    BC250_SHARED_SURFACE shared{};
    switch (Bc250SharedSurfaceDecodeWhy(args->pPrivateDriverData, args->PrivateDriverDataSize,
                                        first.pPrivateDriverData, first.PrivateDriverDataSize, &shared, why)) {
    case BC250_SHARED_SURFACE_OK:
        break;
    case BC250_SHARED_SURFACE_FORMAT:
        return E_NOTIMPL;
    default:
        return E_INVALIDARG;
    }

    // The open's envelope is the decoder's, not linear_surface_shape's, and it is one rule wider on purpose: the
    // decoder bounds an edge at BC250_SHARED_MAX_EDGE (16384), which is what the D3D11 shell creates up to, while
    // a D3D12 shared create stops at kLinearMaxEdge (8192). An opener that refused the larger surface would refuse
    // a surface the other shell legitimately created; the engine's image answers for the rest, and the pitch and
    // size agreement below still has to hold. Every other rule is the same, because the decoder admits one mip,
    // one slice, one sample and a COMPOSED format row only (BD-075 review, 2026-10-06: documented, not changed).
    *why = "engine image";
    const D3D12_RESOURCE_DESC1 desc = opened_desc(shared);
    LinearSurface surface{};
    HRESULT hr = query_linear_surface(c, desc, &surface);
    if (FAILED(hr)) return hr;
    // The layout agreement, checked and not assumed: two drivers that compute different addresses from the same
    // bytes produce silent corruption, which is worse than a refusal. The producer is usually the same RADV build
    // on the same part, but it does not have to be - the application router's GpuUmdPath may point at a D3D11
    // quartet of its own, with another ICD than the installed D3D12 triplet (the lab's gpu11 arm does exactly
    // that, STATE.md), and a shared surface outlives the build that made it. So this is a cross-build check, not
    // an assertion about our own code, and a divergence names both numbers on one line instead of showing wrong
    // pixels (BD-075 review, 2026-10-06).
    *why = "layout agreement";
    if (surface.info.RowPitch != shared.Pitch || surface.backing_size > shared.Size) {
        log_line("shared open: the engine's linear image of this description has pitch %llu and needs %llu bytes; the "
                 "record says pitch %lu and %llu bytes",
                 static_cast<unsigned long long>(surface.info.RowPitch),
                 static_cast<unsigned long long>(surface.backing_size), shared.Pitch,
                 static_cast<unsigned long long>(shared.Size));
        return E_INVALIDARG;
    }

    *why = "adopt";
    AdoptRequest adopt{};
    adopt.size = sizeof(adopt);
    adopt.flags = kMemoryDedicated | kMemoryShareable | kMemoryLinearSurface;
    adopt.rt_owner = rt;
    adopt.allocation = first.hAllocation;
    adopt.memory_type_bits = surface.info.MemoryTypeBits;
    adopt.byte_size = surface.backing_size;
    adopt.alignment = surface.info.MemoryAlignment;
    ImportedMemory memory{};
    hr = adopt_memory(c, adopt, &memory);
    if (FAILED(hr)) return hr;

    // From here on the import is engine-ddi's and goes back through the release sequence on every path.
    *why = "engine heap";
    D3D12DDIARG_CREATEHEAP_0001 heap_desc{};
    heap_desc.ByteSize = surface.backing_size;
    heap_desc.Alignment = 0;
    heap_desc.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    heap_desc.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    heap_desc.Flags = static_cast<D3D12DDI_HEAP_FLAGS>(category_of(desc));
    heap_desc.CreationNodeMask = 1;
    heap_desc.VisibleNodeMask = 1;
    D3D12_HEAP_DESC hd{};
    hr = heap_desc_of(heap_desc, hd);
    ID3D12Heap* heap = nullptr;
    if (SUCCEEDED(hr)) hr = engine_heap_from_memory(c, hd, memory, &heap);
    if (FAILED(hr)) {
        log_line("shared open: the engine refused the heap over the adopted memory: %08lx (%llu bytes, type %u)",
                 static_cast<unsigned long>(hr), static_cast<unsigned long long>(memory.byte_size),
                 memory.memory_type_index);
        ReleasePayload payload{{nullptr, nullptr}, true, memory, 0};
        run_release(c->hooks, payload);
        return hr;
    }

    Backing* b = new_backing(c, heap_desc, heap);
    if (!b) {
        ReleasePayload payload{{heap, nullptr}, true, memory, 0};
        run_release(c->hooks, payload);
        return E_OUTOFMEMORY;
    }
    b->imported = true;
    b->memory = memory;
    b->dedicated = true;
    b->linear = true;
    b->adopted = true;

    *why = "engine image placement";
    ID3D12Resource* engine = nullptr;
    hr = place_linear_at(c, b, desc, static_cast<D3D12_RESOURCE_STATES>(args->InitialResourceState), nullptr, surface,
                         &engine);
    if (FAILED(hr)) {
        backing_release(b);
        return hr;
    }
    heap->AddRef();
    new (hheap.pDrvPrivate) HeapRecord{{Tag::Heap, 0, heap, c}, b};
    c->live.fetch_add(1);
    auto* record = static_cast<ResourceRecord*>(hres.pDrvPrivate);
    construct_resource(record, c, engine, b, 0, desc, rt, ResourceKind::Opened, desc.Format);
    record->linear_row_pitch = static_cast<uint32_t>(surface.info.RowPitch);
    record->linear_size = surface.backing_size;
    // No render-target initialisation: the surface already holds its creator's content, and a discard here would
    // throw away the very pixels the application opened it for.
    *why = "";
    return S_OK;
}

HRESULT APIENTRY open_heap_and_resource(D3D12DDI_HDEVICE device, const D3D12DDIARG_OPENHEAP_0003* args,
                                        D3D12DDI_HHEAP hheap, D3D12DDI_HRTRESOURCE rt,
                                        D3D12DDI_HPROTECTEDRESOURCESESSION_0030 session, D3D12DDI_HRESOURCE hres) {
    DeviceContext* c = resolve(device);
    const char* why = "device";
    HRESULT hr = c ? S_OK : E_INVALIDARG;
    if (SUCCEEDED(hr) && session.pDrvPrivate) {
        why = "protected resource session";
        hr = E_NOTIMPL;
    }
    if (SUCCEEDED(hr)) {
        c->retire_at_resource();
        hr = open_shared_surface(c, args, hheap, rt, hres, &why);
    }
    if (SUCCEEDED(hr)) return hr;
    // Same rule as the create slot: the open of a shared handle fails, the device lives (BD-075).
    const HRESULT admitted = admitted_create_failure(hr);
    const UINT count = args ? args->NumAllocations : 0u;
    const D3DDDI_OPENALLOCATIONINFO* first =
        args && args->pOpenAllocationInfo && count ? &args->pOpenAllocationInfo[0] : nullptr;
    log_refusal("OpenHeapAndResource: %08lx reported as %08lx, declined at %s; %u allocation(s), "
                "kernel resource %s, "
                "resource private data %u bytes (magic 0x%08x, version %u), first allocation private data %u bytes "
                "(magic 0x%08x, version %u), allocation handle %s, initial state 0x%x, heap handle %s, "
                "resource handle %s",
                static_cast<unsigned long>(hr), static_cast<unsigned long>(admitted), why, count,
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
            // BD-133: the copy surfaces of a presented source go with the source, those the shell proved idle;
            // the others are kept (release_present_copy). drain_all above has already made every pending entry of
            // every ring replay, so no recorded copy still names the image of one that goes.
            release_present_copy(c, r);
            if (r->kind == ResourceKind::Committed && cancel_initialization(c, r) && b && !b->retained) {
                b->retained = r->h.engine;              // an initialization batch names it: see Backing::retained
                r->h.engine = nullptr;
            }
            release_engine(r->h);
            free_castable(r->castable);
            r->castable = nullptr;
            r->castable_count = 0;
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

// The engine's size and alignment for desc and its castable formats, with desc.Alignment first and 0 if the engine
// refuses that (a small-alignment request).
HRESULT allocation_info(DeviceContext* c, D3D12_RESOURCE_DESC1 desc, D3D12DDI_RESOURCE_ALLOCATION_INFO_0022* out,
                        UINT castable_count = 0, const DXGI_FORMAT* castable = nullptr) noexcept {
    D3D12_RESOURCE_ALLOCATION_INFO info = engine_allocation_info(c, desc, desc.Alignment, castable_count, castable);
    if (info.SizeInBytes == UINT64_MAX && desc.Alignment && !tight::enabled(desc))
        info = engine_allocation_info(c, desc, 0, castable_count, castable);
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
    const HRESULT hr = query_linear_surface(c, desc, &surface);
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
// The first image of each format whose memory layout check_subresource_info does not know. The same bound.
FormatSet g_subresource_info_refusals;

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
    HRESULT hr = to_api_desc(c, *in, desc);
    if (SUCCEEDED(hr)) {
        if (c->mode == MemoryMode::RuntimeBacked && (optimization & D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_PRIMARY) &&
            linear_surface_shape(desc, *in)) {
            hr = linear_allocation_info(c, desc, out);
        } else {
            desc.Alignment = alignment_restriction;
            hr = allocation_info(c, desc, out, in->NumCastableFormats, in->pCastableFormats);
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

// D76. Where a subresource lies in its resource's own memory, asked after MapHeap. The answer comes from the
// description the resource was created with (the record's desc) and, for an image, from the row pitch its linear
// surface was created with; no engine call is made, because this slot is a query and the engine has no entry that
// describes an image's memory layout.
//
// Two shapes are answered, the two that can reach a CPU-visible heap here:
//   - a buffer, which is one row from its first byte: offset 0, both strides its width, nothing swizzled. The
//     strides are inferred; the header does not define them for a buffer (H:603-611).
//   - the linear primary, the one image whose layout is known: offset 0, the row pitch of its linear surface as
//     the row stride, a plane of Height rows as the depth stride, nothing swizzled. linear_surface_shape admits
//     one mip, one array slice and one sample, so it too has exactly one subresource.
// Any other image gets the zeroed answer and one log line, and no error code. The rule this slot follows is to
// report only when it has no answer to give, because the reference documents no error category for it
// (ref\ddi-display, PFND3D12DDI_CHECK_SUBRESOURCE_INFO: a declaration and one line of description) and the
// runtime takes an error a function does not allow as critical and removes the device ("Handling Errors"), which
// is what BD-075 measured on pfnOpenHeapAndResource. An image whose layout is not known has an answer - zeros,
// the honest form of "nothing is known", not a layout claim - so the former E_NOTIMPL there was a device the slot
// could lose for nothing. The engine creates no ROW_MAJOR image (INTEGRATION.md, 1060) and the swizzle of a 64 KiB
// image is not described anywhere here, so the case is unreachable through the heaps this driver creates.
// A malformed call is the other half of the rule and keeps E_INVALIDARG - an unknown handle, no out structure, or
// a subresource index the resource does not have - because there is no answer to give for it and because the
// runtime makes no such call: it owns the handle it passes and knows the resource's subresource count. The same
// E_INVALIDARG stands in the neighbouring void query of this module (check_existing_resource_allocation_info) and
// is what the debug layer reads; native-policy-test holds every one of these shapes as a host witness.
void APIENTRY check_subresource_info(D3D12DDI_HDEVICE device, D3D12DDI_HRESOURCE hres, UINT subresource,
                                     D3D12DDI_SUBRESOURCE_INFO* out) {
    if (out) *out = D3D12DDI_SUBRESOURCE_INFO{};
    DeviceContext* c = resolve(device);
    if (!c) return;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r || !out) return c->report(E_INVALIDARG);
    const bool buffer = r->desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
    const bool linear = !buffer && r->linear_row_pitch != 0;
    if (!buffer && !linear) {
        if (g_subresource_info_refusals.insert(static_cast<uint32_t>(r->desc.Format)))
            log_refusal("CheckSubresourceInfo: the zeroed answer for the first image of format %u with no known "
                        "layout: dimension %u, %llu x %u, depth or array %u, mips %u, samples %u, layout %u",
                        static_cast<unsigned>(r->desc.Format), static_cast<unsigned>(r->desc.Dimension),
                        static_cast<unsigned long long>(r->desc.Width), r->desc.Height,
                        static_cast<unsigned>(r->desc.DepthOrArraySize), static_cast<unsigned>(r->desc.MipLevels),
                        r->desc.SampleDesc.Count, static_cast<unsigned>(r->desc.Layout));
        return;
    }
    if (subresource != 0) return c->report(E_INVALIDARG);
    if (buffer) {
        out->RowStride = r->desc.Width;
        out->DepthStride = r->desc.Width;
        return;
    }
    out->RowStride = r->linear_row_pitch;
    out->DepthStride = uint64_t{r->linear_row_pitch} * r->desc.Height;
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
                                             : allocation_info(c, r->desc, out, r->castable_count, r->castable);
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
// list: R1_UNORM, P208, V208, V408, the sampler feedback formats and anything past the enum.
// The generated list includes the VulkanOn12 A4B4G4R4_UNORM extension to the original D3D11.3 table.
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

// ---- L68 pfnBarrier: enhanced barriers --------------------------------------------------------------------------
// PFND3D12DDI_BARRIER_0088 (H:10785-10788; the slot of the 0092 command list table at H:11241) takes one flat array
// of tagged barriers, D3D12DDIARG_BARRIER_0088 (H:10772-10783). The engine's
// ID3D12GraphicsCommandList7::Barrier takes groups instead, each group one type with an array of its own (d3d12.h,
// D3D12_BARRIER_GROUP), so this slot has to group what the runtime flattened. The order of the DDI array is the
// order the barriers must be queued in: "Barriers are queued in forward order (API-call order,
// barrier-group-index, barrier-array-index). Multiple barriers on the same subresource must function as though the
// barriers complete in queued order" (DirectX-Specs d3d/D3D12EnhancedBarriers.md, "Barrier Ordering"). Each
// maximal run of one type therefore becomes one group, in place, so a call whose barriers are all of one type -
// the common shape - is one group and one engine call. A call with more runs than kBarrierGroups is split into
// several Barrier calls in order, which the same rule admits: API-call order is its outermost key.
//
// Why the slot is filled now: 1006 EnhancedBarriersSupported (caps.cpp) reports the engine's own OPTIONS12 answer,
// and the reference states that "drivers that report support for enhanced barriers must implement this function",
// and that such a driver "must expect that legacy ResourceBarrier calls will also use the pfnBarrier function"
// (ref\ddi-display d3d12umddi.md, PFND3D12DDI_BARRIER_0088 Remarks). pfnResourceBarrier above stays filled; which
// of the two the runtime uses is the runtime's decision, and both reach the engine's own implementation of that
// barrier model (vkd3d-proton command.c, d3d12_command_list_ResourceBarrier and d3d12_command_list_Barrier). The
// three other callbacks the feature asks for - CreateHeapAndResource, CalcPrivateHeapAndResourceSizes and
// CheckResourceAllocationInfo at 0088 (windows-driver-docs display\enhanced-barriers.md) - are this module's slots
// already, with the initial barrier layout each of them takes.
//
// The engine owns every rule about a barrier's contents: a split barrier, a texture barrier naming a buffer, a
// layout a queue type may not use. This slot translates and refuses only what it cannot express at all.
constexpr EnumPair kBarrierLayouts[] = {
    {D3D12DDI_BARRIER_LAYOUT_UNDEFINED, D3D12_BARRIER_LAYOUT_UNDEFINED},
    {D3D12DDI_BARRIER_LAYOUT_COMMON, D3D12_BARRIER_LAYOUT_COMMON},
    {D3D12DDI_BARRIER_LAYOUT_PRESENT, D3D12_BARRIER_LAYOUT_PRESENT},
    {D3D12DDI_BARRIER_LAYOUT_GENERIC_READ, D3D12_BARRIER_LAYOUT_GENERIC_READ},
    {D3D12DDI_BARRIER_LAYOUT_RENDER_TARGET, D3D12_BARRIER_LAYOUT_RENDER_TARGET},
    {D3D12DDI_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS},
    {D3D12DDI_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE},
    {D3D12DDI_BARRIER_LAYOUT_DEPTH_STENCIL_READ, D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ},
    {D3D12DDI_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE},
    {D3D12DDI_BARRIER_LAYOUT_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COPY_SOURCE},
    {D3D12DDI_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_COPY_DEST},
    {D3D12DDI_BARRIER_LAYOUT_RESOLVE_SOURCE, D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE},
    {D3D12DDI_BARRIER_LAYOUT_RESOLVE_DEST, D3D12_BARRIER_LAYOUT_RESOLVE_DEST},
    {D3D12DDI_BARRIER_LAYOUT_SHADING_RATE_SOURCE, D3D12_BARRIER_LAYOUT_SHADING_RATE_SOURCE},
    {D3D12DDI_BARRIER_LAYOUT_VIDEO_DECODE_READ, D3D12_BARRIER_LAYOUT_VIDEO_DECODE_READ},
    {D3D12DDI_BARRIER_LAYOUT_VIDEO_DECODE_WRITE, D3D12_BARRIER_LAYOUT_VIDEO_DECODE_WRITE},
    {D3D12DDI_BARRIER_LAYOUT_VIDEO_PROCESS_READ, D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_READ},
    {D3D12DDI_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE, D3D12_BARRIER_LAYOUT_VIDEO_PROCESS_WRITE},
    {D3D12DDI_BARRIER_LAYOUT_VIDEO_ENCODE_READ, D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_READ},
    {D3D12DDI_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE, D3D12_BARRIER_LAYOUT_VIDEO_ENCODE_WRITE},
    {D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COMMON},
    {D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_GENERIC_READ},
    {D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_UNORDERED_ACCESS},
    {D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_SHADER_RESOURCE},
    {D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_SOURCE},
    {D3D12DDI_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST, D3D12_BARRIER_LAYOUT_DIRECT_QUEUE_COPY_DEST},
    {D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COMMON},
    {D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_GENERIC_READ},
    {D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_UNORDERED_ACCESS},
    {D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_SHADER_RESOURCE},
    {D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_SOURCE},
    {D3D12DDI_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST, D3D12_BARRIER_LAYOUT_COMPUTE_QUEUE_COPY_DEST},
    // SDK 26100 and the pinned engine vkd3d_d3d12.idl retain VIDEO_QUEUE_COMMON=30.
    // Agility 1.619 removes its API spelling; preserve the existing engine wire contract.
    {D3D12DDI_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON, static_cast<D3D12_BARRIER_LAYOUT>(30)},
};
static_assert(agree(kBarrierLayouts), "barrier layouts");
// VIDEO_QUEUE_COMMON is the last layout the API has, and the DDI's first own layout is the next value
// (H:10629-10630). api_barrier_layout below passes everything up to that boundary through and maps the five DDI
// layouts above it, which have no API value at all.
static_assert(static_cast<unsigned>(D3D12DDI_BARRIER_LAYOUT_LEGACY_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE) ==
                  static_cast<unsigned>(D3D12DDI_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON) + 1,
              "the DDI-only barrier layouts start one past the last shared layout");
// The sweep leaves no value of that range unchecked: one row per value from COMMON to VIDEO_QUEUE_COMMON, plus
// UNDEFINED and the PRESENT second name of COMMON. initial_layout hands exactly this range to a create as an API
// layout, and this count is what says the cast there is covered value by value.
static_assert(std::size(kBarrierLayouts) == static_cast<size_t>(D3D12DDI_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON) + 3,
              "the barrier layout sweep covers every value a create may cast");

// The synchronisation scopes (H:10638-10665) and the access types (H:10668-10696), bit for bit the API's, so both
// pass by value. The DDI's INPUT_ASSEMBLER and PREDICATION are second names of INDEX_INPUT and EXECUTE_INDIRECT,
// as they are in the API.
constexpr EnumPair kBarrierSync[] = {
    {D3D12DDI_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_NONE},
    {D3D12DDI_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_ALL},
    {D3D12DDI_BARRIER_SYNC_DRAW, D3D12_BARRIER_SYNC_DRAW},
    {D3D12DDI_BARRIER_SYNC_INPUT_ASSEMBLER, D3D12_BARRIER_SYNC_INDEX_INPUT},
    {D3D12DDI_BARRIER_SYNC_INDEX_INPUT, D3D12_BARRIER_SYNC_INDEX_INPUT},
    {D3D12DDI_BARRIER_SYNC_VERTEX_SHADING, D3D12_BARRIER_SYNC_VERTEX_SHADING},
    {D3D12DDI_BARRIER_SYNC_PIXEL_SHADING, D3D12_BARRIER_SYNC_PIXEL_SHADING},
    {D3D12DDI_BARRIER_SYNC_DEPTH_STENCIL, D3D12_BARRIER_SYNC_DEPTH_STENCIL},
    {D3D12DDI_BARRIER_SYNC_RENDER_TARGET, D3D12_BARRIER_SYNC_RENDER_TARGET},
    {D3D12DDI_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING},
    {D3D12DDI_BARRIER_SYNC_RAYTRACING, D3D12_BARRIER_SYNC_RAYTRACING},
    {D3D12DDI_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_COPY},
    {D3D12DDI_BARRIER_SYNC_RESOLVE, D3D12_BARRIER_SYNC_RESOLVE},
    {D3D12DDI_BARRIER_SYNC_EXECUTE_INDIRECT, D3D12_BARRIER_SYNC_EXECUTE_INDIRECT},
    {D3D12DDI_BARRIER_SYNC_PREDICATION, D3D12_BARRIER_SYNC_PREDICATION},
    {D3D12DDI_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_SYNC_ALL_SHADING},
    {D3D12DDI_BARRIER_SYNC_NON_PIXEL_SHADING, D3D12_BARRIER_SYNC_NON_PIXEL_SHADING},
    {D3D12DDI_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO,
     D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO},
    {D3D12DDI_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW, D3D12_BARRIER_SYNC_CLEAR_UNORDERED_ACCESS_VIEW},
    {D3D12DDI_BARRIER_SYNC_VIDEO_DECODE, D3D12_BARRIER_SYNC_VIDEO_DECODE},
    {D3D12DDI_BARRIER_SYNC_VIDEO_PROCESS, D3D12_BARRIER_SYNC_VIDEO_PROCESS},
    {D3D12DDI_BARRIER_SYNC_VIDEO_ENCODE, D3D12_BARRIER_SYNC_VIDEO_ENCODE},
    {D3D12DDI_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE,
     D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE},
    {D3D12DDI_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE,
     D3D12_BARRIER_SYNC_COPY_RAYTRACING_ACCELERATION_STRUCTURE},
    {D3D12DDI_BARRIER_SYNC_SPLIT, D3D12_BARRIER_SYNC_SPLIT},
};
static_assert(agree(kBarrierSync), "barrier synchronisation scopes");
constexpr EnumPair kBarrierAccess[] = {
    {D3D12DDI_BARRIER_ACCESS_COMMON, D3D12_BARRIER_ACCESS_COMMON},
    {D3D12DDI_BARRIER_ACCESS_VERTEX_BUFFER, D3D12_BARRIER_ACCESS_VERTEX_BUFFER},
    {D3D12DDI_BARRIER_ACCESS_CONSTANT_BUFFER, D3D12_BARRIER_ACCESS_CONSTANT_BUFFER},
    {D3D12DDI_BARRIER_ACCESS_INDEX_BUFFER, D3D12_BARRIER_ACCESS_INDEX_BUFFER},
    {D3D12DDI_BARRIER_ACCESS_RENDER_TARGET, D3D12_BARRIER_ACCESS_RENDER_TARGET},
    {D3D12DDI_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS},
    {D3D12DDI_BARRIER_ACCESS_DEPTH_STENCIL_WRITE, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_WRITE},
    {D3D12DDI_BARRIER_ACCESS_DEPTH_STENCIL_READ, D3D12_BARRIER_ACCESS_DEPTH_STENCIL_READ},
    {D3D12DDI_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_SHADER_RESOURCE},
    {D3D12DDI_BARRIER_ACCESS_STREAM_OUTPUT, D3D12_BARRIER_ACCESS_STREAM_OUTPUT},
    {D3D12DDI_BARRIER_ACCESS_INDIRECT_ARGUMENT, D3D12_BARRIER_ACCESS_INDIRECT_ARGUMENT},
    {D3D12DDI_BARRIER_ACCESS_PREDICATION, D3D12_BARRIER_ACCESS_PREDICATION},
    {D3D12DDI_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_COPY_DEST},
    {D3D12DDI_BARRIER_ACCESS_COPY_SOURCE, D3D12_BARRIER_ACCESS_COPY_SOURCE},
    {D3D12DDI_BARRIER_ACCESS_RESOLVE_DEST, D3D12_BARRIER_ACCESS_RESOLVE_DEST},
    {D3D12DDI_BARRIER_ACCESS_RESOLVE_SOURCE, D3D12_BARRIER_ACCESS_RESOLVE_SOURCE},
    {D3D12DDI_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ,
     D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ},
    {D3D12DDI_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE,
     D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE},
    {D3D12DDI_BARRIER_ACCESS_SHADING_RATE_SOURCE, D3D12_BARRIER_ACCESS_SHADING_RATE_SOURCE},
    {D3D12DDI_BARRIER_ACCESS_VIDEO_DECODE_READ, D3D12_BARRIER_ACCESS_VIDEO_DECODE_READ},
    {D3D12DDI_BARRIER_ACCESS_VIDEO_DECODE_WRITE, D3D12_BARRIER_ACCESS_VIDEO_DECODE_WRITE},
    {D3D12DDI_BARRIER_ACCESS_VIDEO_PROCESS_READ, D3D12_BARRIER_ACCESS_VIDEO_PROCESS_READ},
    {D3D12DDI_BARRIER_ACCESS_VIDEO_PROCESS_WRITE, D3D12_BARRIER_ACCESS_VIDEO_PROCESS_WRITE},
    {D3D12DDI_BARRIER_ACCESS_VIDEO_ENCODE_READ, D3D12_BARRIER_ACCESS_VIDEO_ENCODE_READ},
    {D3D12DDI_BARRIER_ACCESS_VIDEO_ENCODE_WRITE, D3D12_BARRIER_ACCESS_VIDEO_ENCODE_WRITE},
    {D3D12DDI_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_NO_ACCESS},
};
static_assert(agree(kBarrierAccess), "barrier access types");
// The three barrier types the API has (H:10709-10715; d3d12.h, D3D12_BARRIER_TYPE). RANGED is the DDI's fourth and
// has no API value, which is why api_barrier_type maps it instead of passing it through.
constexpr EnumPair kBarrierTypes[] = {
    {D3D12DDI_BARRIER_TYPE_GLOBAL, D3D12_BARRIER_TYPE_GLOBAL},
    {D3D12DDI_BARRIER_TYPE_TEXTURE, D3D12_BARRIER_TYPE_TEXTURE},
    {D3D12DDI_BARRIER_TYPE_BUFFER, D3D12_BARRIER_TYPE_BUFFER},
};
static_assert(agree(kBarrierTypes), "barrier types");
constexpr EnumPair kTextureBarrierFlags[] = {
    {D3D12DDI_TEXTURE_BARRIER_FLAG_NONE, D3D12_TEXTURE_BARRIER_FLAG_NONE},
    {D3D12DDI_TEXTURE_BARRIER_FLAG_DISCARD, D3D12_TEXTURE_BARRIER_FLAG_DISCARD},
};
static_assert(agree(kTextureBarrierFlags), "texture barrier flags");
// The subresource range (H:10699-10707) is the API's structure, so it is copied whole. The global barrier is the
// API's structure as well. The texture barrier shares the API's layout, the resource field included, because a DDI
// handle and a resource pointer are both one pointer - the fields are still assigned one by one below, since the
// handle has to be resolved. The buffer barrier is the API's first five fields and nothing else: the API's Offset
// and Size have no DDI counterpart.
static_assert(sizeof(D3D12DDI_BARRIER_SUBRESOURCE_RANGE_0088) == sizeof(D3D12_BARRIER_SUBRESOURCE_RANGE) &&
                  offsetof(D3D12DDI_BARRIER_SUBRESOURCE_RANGE_0088, NumMipLevels) ==
                      offsetof(D3D12_BARRIER_SUBRESOURCE_RANGE, NumMipLevels) &&
                  offsetof(D3D12DDI_BARRIER_SUBRESOURCE_RANGE_0088, FirstArraySlice) ==
                      offsetof(D3D12_BARRIER_SUBRESOURCE_RANGE, FirstArraySlice) &&
                  offsetof(D3D12DDI_BARRIER_SUBRESOURCE_RANGE_0088, NumArraySlices) ==
                      offsetof(D3D12_BARRIER_SUBRESOURCE_RANGE, NumArraySlices) &&
                  offsetof(D3D12DDI_BARRIER_SUBRESOURCE_RANGE_0088, FirstPlane) ==
                      offsetof(D3D12_BARRIER_SUBRESOURCE_RANGE, FirstPlane) &&
                  offsetof(D3D12DDI_BARRIER_SUBRESOURCE_RANGE_0088, NumPlanes) ==
                      offsetof(D3D12_BARRIER_SUBRESOURCE_RANGE, NumPlanes),
              "barrier subresource range layout");
static_assert(sizeof(D3D12DDI_GLOBAL_BARRIER_0088) == sizeof(D3D12_GLOBAL_BARRIER) &&
                  offsetof(D3D12DDI_GLOBAL_BARRIER_0088, SyncAfter) == offsetof(D3D12_GLOBAL_BARRIER, SyncAfter) &&
                  offsetof(D3D12DDI_GLOBAL_BARRIER_0088, AccessBefore) ==
                      offsetof(D3D12_GLOBAL_BARRIER, AccessBefore) &&
                  offsetof(D3D12DDI_GLOBAL_BARRIER_0088, AccessAfter) == offsetof(D3D12_GLOBAL_BARRIER, AccessAfter),
              "global barrier layout");
static_assert(sizeof(D3D12DDI_TEXTURE_BARRIER_0088) == sizeof(D3D12_TEXTURE_BARRIER) &&
                  offsetof(D3D12DDI_TEXTURE_BARRIER_0088, LayoutBefore) ==
                      offsetof(D3D12_TEXTURE_BARRIER, LayoutBefore) &&
                  offsetof(D3D12DDI_TEXTURE_BARRIER_0088, LayoutAfter) ==
                      offsetof(D3D12_TEXTURE_BARRIER, LayoutAfter) &&
                  offsetof(D3D12DDI_TEXTURE_BARRIER_0088, hResource) == offsetof(D3D12_TEXTURE_BARRIER, pResource) &&
                  offsetof(D3D12DDI_TEXTURE_BARRIER_0088, Subresources) ==
                      offsetof(D3D12_TEXTURE_BARRIER, Subresources) &&
                  offsetof(D3D12DDI_TEXTURE_BARRIER_0088, Flags) == offsetof(D3D12_TEXTURE_BARRIER, Flags),
              "texture barrier layout");
static_assert(offsetof(D3D12DDI_BUFFER_BARRIER_0088, SyncAfter) == offsetof(D3D12_BUFFER_BARRIER, SyncAfter) &&
                  offsetof(D3D12DDI_BUFFER_BARRIER_0088, AccessBefore) ==
                      offsetof(D3D12_BUFFER_BARRIER, AccessBefore) &&
                  offsetof(D3D12DDI_BUFFER_BARRIER_0088, AccessAfter) == offsetof(D3D12_BUFFER_BARRIER, AccessAfter) &&
                  offsetof(D3D12DDI_BUFFER_BARRIER_0088, hResource) == offsetof(D3D12_BUFFER_BARRIER, pResource) &&
                  sizeof(D3D12DDI_BUFFER_BARRIER_0088) < sizeof(D3D12_BUFFER_BARRIER),
              "buffer barrier layout");

// Barriers of one type, before the heap. A call with more of a type than this allocates once for that type, as
// resource_barrier does.
constexpr size_t kInlineBarriers = 32;
// Groups per engine call. The groups are rebuilt on the worker's own stack, so their number is bounded there; a
// call with more runs than this is split into several Barrier calls in order.
constexpr UINT32 kBarrierGroups = 64;

// One run of barriers of one type in the DDI array: where its API barriers start in that type's array, and how
// many. The group the lambda builds from it points into the entry's copy of that array.
struct BarrierRun {
    D3D12_BARRIER_TYPE type;
    UINT32 first;
    UINT32 count;
};

// The API layout of a DDI barrier layout. Everything up to VIDEO_QUEUE_COMMON, and UNDEFINED, is the API's own
// value (kBarrierLayouts). The five DDI-only layouts above that boundary (H:10630-10635) name a layout a legacy
// resource state asked for, and each maps to the queue-agnostic API layout that admits the same access: the
// queue-specific API layouts would be refused on a command list of another type, so they are not used here.
// false for a value the DDI does not define at all.
bool api_barrier_layout(D3D12DDI_BARRIER_LAYOUT in, D3D12_BARRIER_LAYOUT* out) noexcept {
    if (in == D3D12DDI_BARRIER_LAYOUT_UNDEFINED ||
        static_cast<unsigned>(in) <= static_cast<unsigned>(D3D12DDI_BARRIER_LAYOUT_VIDEO_QUEUE_COMMON)) {
        *out = static_cast<D3D12_BARRIER_LAYOUT>(in);
        return true;
    }
    switch (in) {
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_DIRECT_QUEUE_GENERIC_READ_COMPUTE_QUEUE_ACCESSIBLE:
        *out = D3D12_BARRIER_LAYOUT_GENERIC_READ;
        return true;
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_SOURCE:
        *out = D3D12_BARRIER_LAYOUT_COPY_SOURCE;
        return true;
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_COPY_DEST:
        *out = D3D12_BARRIER_LAYOUT_COPY_DEST;
        return true;
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_SHADER_RESOURCE:
    case D3D12DDI_BARRIER_LAYOUT_LEGACY_PIXEL_SHADER_RESOURCE:
        *out = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
        return true;
    default:
        return false;
    }
}

// The API barrier type a DDI barrier becomes. A RANGED barrier becomes a global barrier: the API has no ranged
// barrier, and at 0088 a ranged barrier carries no layout and only the ATOMIC_COPY flag (H:10753-10770), so it
// names a cache flush and a synchronisation over one memory range of one resource. A global barrier is the same
// flush and synchronisation at the scope of the whole queue, a superset of that range, and it changes no layout
// either. It is therefore a conservative answer and not a refusal, which matters because this slot also receives
// the runtime's translation of legacy ResourceBarrier calls: refusing would cost an application its device the
// first time a ranged barrier arrived. Nothing here claims atomic copy support - L51 pfnAtomicCopyBufferRegion is
// still a fail-safe and 1006 reports no such capability. false for a type the DDI does not define.
bool api_barrier_type(D3D12DDI_BARRIER_TYPE in, D3D12_BARRIER_TYPE* out) noexcept {
    switch (in) {
    case D3D12DDI_BARRIER_TYPE_GLOBAL:
    case D3D12DDI_BARRIER_TYPE_RANGED:
        *out = D3D12_BARRIER_TYPE_GLOBAL;
        return true;
    case D3D12DDI_BARRIER_TYPE_TEXTURE:
        *out = D3D12_BARRIER_TYPE_TEXTURE;
        return true;
    case D3D12DDI_BARRIER_TYPE_BUFFER:
        *out = D3D12_BARRIER_TYPE_BUFFER;
        return true;
    default:
        return false;
    }
}

// At 0094 the slot takes D3D12DDIARG_BARRIER_0094 (H:11285-11301). Its global, texture and buffer members are the
// 0088 types; its ranged member, D3D12DDI_RANGED_BARRIER_0094 (H:11268-11283), is new: a resource, a subresource, a
// range and one flag of NONE, ATOMIC_COPY, ACQUIRE and RELEASE (the last two acquire or release a buffer range for
// an atomic copy), and no synchronisation or access scopes. The 0088 ranged member is the largest of its union and
// the 0094 one is not, so the array stride is not the same (x64: 80 bytes at 0088, 72 at 0094; x86: 72 and 64), and
// each build has its own entry over this one implementation.
// A 0094 ranged barrier becomes a global barrier of the widest scopes: SYNC_ALL before and after, ACCESS_COMMON
// before and after, which the engine makes a dependency of all commands on all memory reads and writes
// (vkd3d-proton command.c, d3d12_command_list_process_enhanced_barrier_global). That over-synchronises a range;
// it does not make atomic copy supported, which 1006 still does not report and L51 still refuses.
static_assert(offsetof(D3D12DDIARG_BARRIER_0094, GlobalBarrier) == offsetof(D3D12DDIARG_BARRIER_0088, GlobalBarrier) &&
                  offsetof(D3D12DDIARG_BARRIER_0094, TextureBarrier) ==
                      offsetof(D3D12DDIARG_BARRIER_0088, TextureBarrier) &&
                  offsetof(D3D12DDIARG_BARRIER_0094, BufferBarrier) == offsetof(D3D12DDIARG_BARRIER_0088, BufferBarrier),
              "0094 keeps the 0088 global, texture and buffer members at the same place");
#if defined(_WIN64)
static_assert(sizeof(D3D12DDIARG_BARRIER_0088) == 80 && sizeof(D3D12DDIARG_BARRIER_0094) == 72,
              "x64 strides of the two argument layouts");
#else
static_assert(sizeof(D3D12DDIARG_BARRIER_0088) == 72 && sizeof(D3D12DDIARG_BARRIER_0094) == 64,
              "x86 strides of the two argument layouts");
#endif

template <class Arg> void barrier_of(D3D12DDI_HCOMMANDLIST hlist, UINT32 count, const Arg* ddi) {
    static_assert(std::is_same_v<Arg, D3D12DDIARG_BARRIER_0088> || std::is_same_v<Arg, D3D12DDIARG_BARRIER_0094>);
    CommandListRecord* l = list_of(hlist, "Barrier");
    if (!l) return;
    if (count && !ddi) {
        l->h.device->report_list(l->rt, E_INVALIDARG);
        return;
    }
    if (!count) return;                 // no barrier, no engine call: what the engine answers to no groups
    // First pass: the API type of each barrier, how many of each type, and how many runs the array falls into.
    // Each array is then reserved for exactly what it holds.
    UINT32 per_type[3] = {0, 0, 0};     // by D3D12_BARRIER_TYPE
    UINT32 runs = 0;
    int last = -1;
    for (UINT32 i = 0; i < count; ++i) {
        D3D12_BARRIER_TYPE type{};
        if (!api_barrier_type(ddi[i].Type, &type)) {
            log_refusal("Barrier: barrier %u of %u has type %u", i, count, static_cast<unsigned>(ddi[i].Type));
            l->h.device->report_list(l->rt, E_INVALIDARG);
            return;
        }
        ++per_type[static_cast<size_t>(type)];
        if (static_cast<int>(type) != last) ++runs;
        last = static_cast<int>(type);
    }
    InlineArray<D3D12_GLOBAL_BARRIER, kInlineBarriers> globals;
    InlineArray<D3D12_TEXTURE_BARRIER, kInlineBarriers> textures;
    InlineArray<D3D12_BUFFER_BARRIER, kInlineBarriers> buffers;
    InlineArray<BarrierRun, kInlineBarriers> table;
    const UINT32 global_count = per_type[static_cast<size_t>(D3D12_BARRIER_TYPE_GLOBAL)];
    const UINT32 texture_count = per_type[static_cast<size_t>(D3D12_BARRIER_TYPE_TEXTURE)];
    const UINT32 buffer_count = per_type[static_cast<size_t>(D3D12_BARRIER_TYPE_BUFFER)];
    if (!globals.reserve(global_count) || !textures.reserve(texture_count) || !buffers.reserve(buffer_count) ||
        !table.reserve(runs)) {
        l->h.device->report_list(l->rt, E_OUTOFMEMORY);
        return;
    }
    D3D12_GLOBAL_BARRIER* global = globals.data();
    D3D12_TEXTURE_BARRIER* texture = textures.data();
    D3D12_BUFFER_BARRIER* buffer = buffers.data();
    BarrierRun* run = table.data();
    UINT32 filled[3] = {0, 0, 0};
    UINT32 groups = 0;
    // Second pass: translate in place, and start a new run wherever the type changes.
    for (UINT32 i = 0; i < count; ++i) {
        const Arg& b = ddi[i];
        D3D12_BARRIER_TYPE type{};
        api_barrier_type(b.Type, &type);                // checked in the first pass
        const auto kind = static_cast<size_t>(type);
        if (!groups || run[groups - 1].type != type) {
            run[groups] = BarrierRun{type, filled[kind], 0};
            ++groups;
        }
        ++run[groups - 1].count;
        ++filled[kind];
        switch (type) {
        case D3D12_BARRIER_TYPE_GLOBAL: {
            const bool ranged = b.Type == D3D12DDI_BARRIER_TYPE_RANGED;
            D3D12_GLOBAL_BARRIER& out = *global++;
            if (!ranged) {
                out.SyncBefore = static_cast<D3D12_BARRIER_SYNC>(b.GlobalBarrier.SyncBefore);
                out.SyncAfter = static_cast<D3D12_BARRIER_SYNC>(b.GlobalBarrier.SyncAfter);
                out.AccessBefore = static_cast<D3D12_BARRIER_ACCESS>(b.GlobalBarrier.AccessBefore);
                out.AccessAfter = static_cast<D3D12_BARRIER_ACCESS>(b.GlobalBarrier.AccessAfter);
                break;
            }
            static std::atomic<bool> once{false};
            if constexpr (std::is_same_v<Arg, D3D12DDIARG_BARRIER_0088>) {
                // At 0088 the four scopes of a ranged barrier are the same four values under another member.
                out.SyncBefore = static_cast<D3D12_BARRIER_SYNC>(b.RangedBarrier.SyncBefore);
                out.SyncAfter = static_cast<D3D12_BARRIER_SYNC>(b.RangedBarrier.SyncAfter);
                out.AccessBefore = static_cast<D3D12_BARRIER_ACCESS>(b.RangedBarrier.AccessBefore);
                out.AccessAfter = static_cast<D3D12_BARRIER_ACCESS>(b.RangedBarrier.AccessAfter);
                if (!once.exchange(true))
                    log_line("Barrier: a ranged barrier (flags %#x) is made a global barrier of the same scopes",
                             static_cast<unsigned>(b.RangedBarrier.Flags));
            } else {
                out.SyncBefore = D3D12_BARRIER_SYNC_ALL;
                out.SyncAfter = D3D12_BARRIER_SYNC_ALL;
                out.AccessBefore = D3D12_BARRIER_ACCESS_COMMON;
                out.AccessAfter = D3D12_BARRIER_ACCESS_COMMON;
                if (!once.exchange(true))
                    log_line("Barrier: a 0094 ranged barrier (flags %#x, resource %p) is made a global barrier "
                             "SYNC_ALL, ACCESS_COMMON",
                             static_cast<unsigned>(b.RangedBarrier.Flags), b.RangedBarrier.hResource.pDrvPrivate);
            }
            break;
        }
        case D3D12_BARRIER_TYPE_TEXTURE: {
            const D3D12DDI_TEXTURE_BARRIER_0088& t = b.TextureBarrier;
            D3D12_TEXTURE_BARRIER& out = *texture++;
            out.SyncBefore = static_cast<D3D12_BARRIER_SYNC>(t.SyncBefore);
            out.SyncAfter = static_cast<D3D12_BARRIER_SYNC>(t.SyncAfter);
            out.AccessBefore = static_cast<D3D12_BARRIER_ACCESS>(t.AccessBefore);
            out.AccessAfter = static_cast<D3D12_BARRIER_ACCESS>(t.AccessAfter);
            out.pResource = engine_resource(l, t.hResource);
            std::memcpy(&out.Subresources, &t.Subresources, sizeof(out.Subresources));
            out.Flags = static_cast<D3D12_TEXTURE_BARRIER_FLAGS>(t.Flags);
            // "Must provide a valid, non-NULL resource pointer" (windows-driver-docs display\enhanced-barriers.md,
            // texture barriers), so an unresolvable or absent handle is this slot's refusal, not the engine's.
            if (!out.pResource || !api_barrier_layout(t.LayoutBefore, &out.LayoutBefore) ||
                !api_barrier_layout(t.LayoutAfter, &out.LayoutAfter)) {
                log_refusal("Barrier: texture barrier %u of %u, resource %p, layouts %#x to %#x", i, count,
                            t.hResource.pDrvPrivate, static_cast<unsigned>(t.LayoutBefore),
                            static_cast<unsigned>(t.LayoutAfter));
                l->h.device->report_list(l->rt, E_INVALIDARG);
                return;
            }
            break;
        }
        default: {
            const D3D12DDI_BUFFER_BARRIER_0088& f = b.BufferBarrier;
            D3D12_BUFFER_BARRIER& out = *buffer++;
            out.SyncBefore = static_cast<D3D12_BARRIER_SYNC>(f.SyncBefore);
            out.SyncAfter = static_cast<D3D12_BARRIER_SYNC>(f.SyncAfter);
            out.AccessBefore = static_cast<D3D12_BARRIER_ACCESS>(f.AccessBefore);
            out.AccessAfter = static_cast<D3D12_BARRIER_ACCESS>(f.AccessAfter);
            out.pResource = engine_resource(l, f.hResource);
            // The DDI has no Offset and no Size, and the API reserves both: "Offset must be zero and Size must be
            // either the buffer size in bytes or UINT64_MAX ... The current enhanced buffer barrier DDI's do not
            // use Offset or Size" (DirectX-Specs d3d/D3D12EnhancedBarriers.md). The engine reads neither
            // (vkd3d-proton command.c, d3d12_command_list_process_enhanced_barrier_buffer).
            out.Offset = 0;
            out.Size = UINT64_MAX;
            if (!out.pResource) {
                log_refusal("Barrier: buffer barrier %u of %u, resource %p", i, count, f.hResource.pDrvPrivate);
                l->h.device->report_list(l->rt, E_INVALIDARG);
                return;
            }
            break;
        }
        }
    }
    auto* l7 = list_as<ID3D12GraphicsCommandList7>(l);
    if (!l7) return;                                    // reported as E_NOINTERFACE by list_as
    record(l,
           [=](ID3D12GraphicsCommandList*, const BarrierRun* r, const D3D12_GLOBAL_BARRIER* g,
               const D3D12_TEXTURE_BARRIER* t, const D3D12_BUFFER_BARRIER* f) {
               D3D12_BARRIER_GROUP group[kBarrierGroups];
               for (UINT32 done = 0; done < groups;) {
                   const UINT32 n = groups - done < kBarrierGroups ? groups - done : kBarrierGroups;
                   for (UINT32 k = 0; k < n; ++k) {
                       const BarrierRun& one = r[done + k];
                       group[k].Type = one.type;
                       group[k].NumBarriers = one.count;
                       switch (one.type) {
                       case D3D12_BARRIER_TYPE_GLOBAL: group[k].pGlobalBarriers = g + one.first; break;
                       case D3D12_BARRIER_TYPE_TEXTURE: group[k].pTextureBarriers = t + one.first; break;
                       default: group[k].pBufferBarriers = f + one.first; break;
                       }
                   }
                   l7->Barrier(n, group);
                   done += n;
               }
               l7->Release();
           },
           in(table.data(), groups), in(globals.data(), global_count), in(textures.data(), texture_count),
           in(buffers.data(), buffer_count));
}

void APIENTRY barrier(D3D12DDI_HCOMMANDLIST hlist, UINT32 count, const D3D12DDIARG_BARRIER_0088* ddi) {
    barrier_of(hlist, count, ddi);
}
void APIENTRY barrier_0094(D3D12DDI_HCOMMANDLIST hlist, UINT32 count, const D3D12DDIARG_BARRIER_0094* ddi) {
    barrier_of(hlist, count, ddi);
}
} // namespace

// ---- Backing lifetime -------------------------------------------------------------------------------------------
void backing_acquire(Backing* b) noexcept { b->refs.fetch_add(1); }

void backing_release(Backing* b) noexcept {
    if (b->refs.fetch_sub(1) != 1) return;
    DeviceContext* c = b->device;
    PendingRelease* node = b->release_node;
    // in_ddi: a linear surface's memory, and borrowed memory of either kind (BD-075), is released inside the destroy
    // that ends it. For an adopted allocation that is not a preference but the contract: the runtime destroys the
    // allocation as soon as pfnDestroyHeapAndResource returns, so a release recorded for later would unmap a handle
    // that is no longer ours.
    node->payload = ReleasePayload{{b->retained, b->heap}, b->imported, b->memory, b->id,
                                   b->linear || b->adopted};                    // resource first
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

// BD-075: the borrowed memory of an opened shared resource. The validation is the import's, written against a
// MemoryRequest, so the adopt request is translated into the three fields it reads; the handle itself must be the
// one the runtime opened, because the shell made no allocation of its own.
HRESULT adopt_memory(DeviceContext* c, const AdoptRequest& request, ImportedMemory* out) noexcept {
    *out = ImportedMemory{};
    if (!c->hooks.adopt_memory) return E_NOTIMPL;       // the shell serves no shared open
    ImportedMemory m{};
    HRESULT hr = c->hooks.adopt_memory(c->hooks.shell, &request, &m);
    if (FAILED(hr)) return hr;                          // on failure engine-ddi owns nothing
    MemoryRequest checked{};
    checked.size = sizeof(checked);
    checked.flags = request.flags;
    checked.byte_size = request.byte_size;
    checked.alignment = request.alignment;
    checked.memory_type_bits = request.memory_type_bits;
    hr = validate_import(checked, m);
    if (SUCCEEDED(hr) && m.allocation != request.allocation) hr = E_INVALIDARG;
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

// One rule, two admitted kinds. `opened` is the destination form: see engine-ddi.h for why the two questions
// must not share an answer.
HRESULT linear_surface_allocation(DeviceContext* c, D3D12DDI_HRESOURCE hres, bool opened,
                                 D3DKMT_HANDLE* allocation) noexcept {
    if (!allocation) return E_INVALIDARG;
    *allocation = 0;
    if (!c) return E_INVALIDARG;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    const bool kind = r && (r->kind == ResourceKind::Committed || (opened && r->kind == ResourceKind::Opened));
    if (!r || !kind || c->mode != MemoryMode::RuntimeBacked || !r->backing || !r->backing->imported ||
        !r->backing->linear || r->offset || !r->linear_row_pitch || !r->backing->memory.allocation)
        return E_INVALIDARG;
    *allocation = r->backing->memory.allocation;
    return S_OK;
}

HRESULT present_allocation(DeviceContext* c, D3D12DDI_HRESOURCE hres, D3DKMT_HANDLE* allocation) noexcept {
    return linear_surface_allocation(c, hres, false, allocation);
}

HRESULT present_destination_allocation(DeviceContext* c, D3D12DDI_HRESOURCE hres, D3DKMT_HANDLE* allocation) noexcept {
    return linear_surface_allocation(c, hres, true, allocation);
}

// ---- BD-133: the Present copy ------------------------------------------------------------------------------------
// present_allocation above answers for the one source the kernel present can read without help. This is the other
// source: an ordinary texture of this device, which is what a D3D9On12 back buffer is. engine-ddi.h states the
// contract; present-copy.h states the rule and why the kernel needs a surface of this kind, with the kernel's own
// checks by file and line; present-copy-sync.h states what orders the copy and the kernel present that reads it.
namespace {
std::atomic<uint64_t> present_copy_count{0}, present_copy_surface_count{0}, present_copy_reuse_count{0},
    present_copy_refusal_count{0}, present_copy_kept_count{0};
// context.cpp has the same one-liner for its own release lines; it is file-local there.
bool present_copy_doubling(uint64_t n) noexcept { return n && !(n & (n - 1)); }
// One line per refusal reason per process: a refused present repeats every frame, and the reason does not change.
LogOnce present_copy_refused[static_cast<size_t>(native12::PresentCopy::Count)];
// Its own slot, not one of the reasons above: a source with no memory of its own is refused by this function and
// not by the rule, and sharing a slot would let whichever fired first silence the other (review finding 7).
LogOnce present_copy_no_memory;
// One line per format whose copy the kernel driver's GPU present could not blit.
FormatSet present_copy_unblittable;
LogOnce present_copy_first;
// The shape-changed backstop below, which no path reaches today.
LogOnce present_copy_reshaped;
// A fence that read UINT64_MAX: the device is removed, and the read is no witness of a retired surface.
LogOnce present_copy_removed;

// One more copy destination for a source resource, at the end of its pool. Everything here is the create path's own
// machinery (query_linear_surface, import_memory, engine_heap_from_memory, new_backing, place_linear_at), asked for
// the one shape the shell admits for a linear surface that is nobody's primary. *over_budget: the device's copy
// surfaces would pass kPresentCopyDeviceBytes with this one, so nothing was asked of the memory at all.
HRESULT make_present_copy(DeviceContext* c, ResourceRecord* r, const native12::PresentCopyShape& shape,
                          bool primary, bool* over_budget) noexcept {
    native12::PresentCopyRequest request{};
    native12::present_copy_description(shape, &request);
    if (primary) native12::present_copy_as_primary(&request);
    D3D12_RESOURCE_DESC1 desc{};
    HRESULT hr = to_api_desc(c, request.resource, desc);
    if (FAILED(hr)) return hr;
    LinearSurface surface{};
    // The copy's own row (present-copy.h, present_copy_format): the composed row of every other linear surface,
    // and for a D3D9 X8R8G8B8 back buffer (88 or 93) the A8R8G8B8 row, the A tag of the same bytes, which the
    // kernel present needs to match the A8R8G8B8 redirection surface it blits into (present_copy_alpha_tag).
    hr = query_linear_surface(c, desc, native12::present_copy_format(static_cast<uint32_t>(shape.format)), &surface);
    if (FAILED(hr)) return hr;
    if (!native12::present_copy_within_budget(c->present_copy_bytes.load(std::memory_order_relaxed),
                                              surface.backing_size)) {
        *over_budget = true;
        return E_OUTOFMEMORY;
    }
    request.heap.ByteSize = surface.backing_size;
    MemoryRequest ask{};
    ask.size = sizeof(ask);
    // The shared linear surface of BD-075, with one difference: rt_owner stays empty, because this surface is the
    // driver's own and no runtime resource owns its allocation. The shell releases such a surface by handle
    // instead of by its runtime resource inside that resource's DDI (heap-import.h, driver_surface).
    ask.flags = kMemoryDedicated | kMemoryLinearSurface | (primary ? kMemoryPrimary : kMemoryShareable);
    ask.heap = &request.heap;
    ask.resource = &request.resource;
    ask.byte_size = surface.backing_size;
    ask.alignment = surface.info.MemoryAlignment;
    ask.memory_type_bits = surface.info.MemoryTypeBits;
    ask.surface_row_pitch = static_cast<uint32_t>(surface.info.RowPitch);
    ask.surface_layout_size = surface.info.LayoutSize;
    ImportedMemory memory{};
    hr = import_memory(c, ask, &memory);
    if (FAILED(hr)) return hr;                  // on a failure the shell owns nothing (engine-ddi.h)
    D3D12_HEAP_DESC hd{};
    hr = heap_desc_of(request.heap, hd);
    ID3D12Heap* heap = nullptr;
    if (SUCCEEDED(hr)) hr = engine_heap_from_memory(c, hd, memory, &heap);
    if (FAILED(hr)) {
        ReleasePayload payload{{nullptr, nullptr}, true, memory, 0};
        run_release(c->hooks, payload);         // exactly once; nothing used the memory yet
        return hr;
    }
    Backing* b = new_backing(c, request.heap, heap);
    if (!b) {
        ReleasePayload payload{{heap, nullptr}, true, memory, 0};
        run_release(c->hooks, payload);
        return E_OUTOFMEMORY;
    }
    b->imported = true;
    b->memory = memory;
    b->dedicated = true;
    // Deliberately not linear. Backing::linear is what makes a release run inside the DDI that ends it, because a
    // linear primary's allocation is destroyed by the runtime as soon as pfnDestroyHeapAndResource returns
    // (heap-import.h, in_ddi). This allocation has no runtime resource, so nothing destroys it behind us and its
    // release waits for retirement like every other heap's - which is what the copies that named it need.
    b->linear = false;
    ID3D12Resource* engine = nullptr;
    hr = place_linear_at(c, b, desc, D3D12_RESOURCE_STATE_COMMON, nullptr, surface, &engine);
    if (FAILED(hr)) {
        backing_release(b);                     // takes the heap and the memory with it
        return hr;
    }
    auto* s = make_new<PresentCopySurface>();
    if (!s) {
        engine->Release();
        backing_release(b);
        return E_OUTOFMEMORY;
    }
    s->backing = b;                             // the reference new_backing made; released in release_present_copy
    s->engine = engine;
    s->allocation = memory.allocation;
    s->width = shape.width;
    s->height = shape.height;
    s->row_pitch = static_cast<uint32_t>(surface.info.RowPitch);
    s->format = shape.format;
    s->d3dddi = shape.d3dddi;
    s->kernel_blt_format = shape.kernel_blt_format;
    s->bytes = surface.backing_size;
    s->next = nullptr;
    PresentCopySurface** tail = &r->present_copy;
    while (*tail) tail = &(*tail)->next;
    *tail = s;
    const uint64_t bytes = c->present_copy_bytes.fetch_add(s->bytes, std::memory_order_relaxed) + s->bytes;
    // Not counted in DeviceContext::live: this surface is reachable only from its source resource's record, the
    // runtime destroys every resource before the device, and the release this destroy starts counts itself.
    const uint64_t made = present_copy_surface_count.fetch_add(1, std::memory_order_relaxed) + 1;
    // log_line, not log_refusal: this is a surface that was created, and log_refusal's own contract puts its text
    // on the debugger channel as a refusal (internal.h), where a successful create does not belong.
    log_line("present copy: surface %llu created for a source the kernel present cannot read directly: "
                "%s form, format %d, %ux%u, pitch %u, %llu bytes (%llu of the device's %llu), LB7A format %u%s",
                static_cast<unsigned long long>(made), primary ? "primary" : "shared",
                static_cast<int>(shape.format), shape.width, shape.height,
                s->row_pitch, static_cast<unsigned long long>(surface.backing_size),
                static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(native12::kPresentCopyDeviceBytes), shape.d3dddi,
                shape.kernel_blt_format ? "" : " (the kernel driver's GPU present cannot blit this format)");
    return S_OK;
}

// Both forms of the surface, in order, because neither is proven for an allocation with no runtime resource and
// the two differ in exactly the fields a runtime refusal could be about. The shared form goes first: it is what
// the surface is (nobody's primary, of no video present source) and it carries the 64-byte E26R v3 record the
// wire defines for a surface another component opens. The primary form is the fallback: the same LB7A
// description under a 12-byte E26R v1 record, with the PRIMARY allocation flag, which is the shape the runtime
// takes for every swap-chain buffer this driver has ever allocated - with a runtime resource, which is the one
// difference left. A composed primary's format comes from the storage column alone (heap-import.cpp, "primary
// format"), so the sRGB view of a composed row has no primary form and the fallback is skipped for it with a line;
// the X8 pair of the D3D9 back buffer has both forms (present-copy.h, present_copy_primary_form). A refusal of the
// budget is the same for both forms, so it is not asked twice.
HRESULT make_present_copy_either(DeviceContext* c, ResourceRecord* r, const native12::PresentCopyShape& shape,
                                 bool* over_budget) noexcept {
    const HRESULT shared = make_present_copy(c, r, shape, false, over_budget);
    if (SUCCEEDED(shared) || *over_budget) return shared;
    if (!native12::present_copy_primary_form(static_cast<uint32_t>(shape.format))) {
        log_refusal("present copy: the shared surface form was refused (%08lx) and format %d is a view of a "
                    "storage format, which has no primary form",
                    static_cast<unsigned long>(shared), static_cast<int>(shape.format));
        return shared;
    }
    log_refusal("present copy: the shared surface form was refused (%08lx); asking for the primary form of the "
                "same linear surface",
                static_cast<unsigned long>(shared));
    const HRESULT as_primary = make_present_copy(c, r, shape, true, over_budget);
    return SUCCEEDED(as_primary) ? as_primary : shared;
}

// A present that found no surface to take: one line at each power of two of these refusals, which are counted.
void present_copy_refuse(const char* why, const native12::PresentCopyShape& shape, unsigned surfaces,
                         uint64_t bytes, HRESULT hr) noexcept {
    const uint64_t n = present_copy_refusal_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (present_copy_doubling(n))
        log_refusal("present copy: no copy surface of the source is reusable (its last copy or its last reader is "
                    "still outstanding) and %s (format %d, %ux%u, %u surfaces of %u for the source, %llu of %llu "
                    "bytes for the device, %08lx; %llu such refusals): the present is refused before its copy is "
                    "recorded or any wait is queued, and because pfnPresent names no status the runtime removes the "
                    "device",
                    why, static_cast<int>(shape.format), shape.width, shape.height, surfaces,
                    native12::kPresentCopySurfacesPerSource, static_cast<unsigned long long>(bytes),
                    static_cast<unsigned long long>(native12::kPresentCopyDeviceBytes),
                    static_cast<unsigned long>(hr), static_cast<unsigned long long>(n));
}
} // namespace

void release_present_copy(DeviceContext* c, ResourceRecord* r) noexcept {
    PresentCopySurface* s = r ? r->present_copy : nullptr;
    if (!s) return;
    r->present_copy = nullptr;
    while (s) {
        PresentCopySurface* next = s->next;
        s->next = nullptr;
        if (s->slot.fence || s->slot.retained.load(std::memory_order_acquire)) {
            // Not proven idle (PresentContext::retire): a kernel present may still read it, or wait for its fence.
            // Its memory, image and fence stay with the device until the OS tears it down.
            const uint64_t kept = present_copy_kept_count.fetch_add(1, std::memory_order_relaxed) + 1;
            if (present_copy_doubling(kept))
                log_refusal("present copy: a copy surface of a destroyed source is kept, because its retirement was "
                            "not proven (%llu kept, %llu bytes): its memory, its image and its fence go with the "
                            "device's OS teardown",
                            static_cast<unsigned long long>(kept), static_cast<unsigned long long>(s->bytes));
        } else {
            if (s->engine) s->engine->Release();
            if (s->backing) backing_release(s->backing);
            if (c) c->present_copy_bytes.fetch_sub(s->bytes, std::memory_order_relaxed);
            delete s;
        }
        s = next;
    }
}

HRESULT present_copy_allocation(DeviceContext* c, D3D12DDI_HCOMMANDLIST hlist, D3D12DDI_HRESOURCE hres,
                                const native12::PresentCopyPick& pick, native12::PresentCopySlot** slot,
                                D3DKMT_HANDLE* allocation) noexcept {
    if (!allocation || !slot) return E_INVALIDARG;
    *allocation = 0;
    *slot = nullptr;
    if (!c || !pick.prepare) return E_INVALIDARG;
    // Only the runtime-backed shell has LB7A surface memory; the engine-private test mode allocates nothing the
    // kernel knows about.
    if (c->mode != MemoryMode::RuntimeBacked) return E_INVALIDARG;
    CommandListRecord* l = list_of(hlist, "Present");
    if (!l || l->h.device != c || !l->h.engine) return E_INVALIDARG;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r || !r->h.engine) return E_INVALIDARG;
    if (r->kind == ResourceKind::Reserved || !r->backing) {
        if (present_copy_no_memory.first())
            log_refusal("present copy: the source has no memory of its own (kind %u): the present is not made, "
                        "and because pfnPresent names no status the runtime loses the device "
                        "(handling-errors.md: an error the DDI does not allow is critical)",
                        static_cast<unsigned>(r->kind));
        return E_INVALIDARG;
    }
    native12::PresentCopyShape shape{};
    const native12::PresentCopy why = native12::present_copy_admit(r->desc, &shape);
    if (why != native12::PresentCopy::Admitted) {
        if (present_copy_refused[static_cast<size_t>(why)].first())
            // What the refusal costs, named as it is: pfnPresent returns void and may report no status of its
            // own, so the caller's report_device_error reaches pfnSetErrorCb with a code this DDI does not
            // allow, and "the runtime ... will then lose the device on purpose"
            // (ref\windows-driver-docs\windows-driver-docs-pr\display\handling-errors.md:15 and :17). There is
            // no softer answer at this DDI: fill_present has none for a present with no source.
            log_refusal("present copy: the source cannot be copied into a linear surface (%s): format %d, "
                        "%llux%u, %u slices, %u mips, %u samples, flags 0x%x; the present is not made, and "
                        "because pfnPresent names no status the runtime loses the device",
                        native12::present_copy_text(why), static_cast<int>(r->desc.Format),
                        static_cast<unsigned long long>(r->desc.Width), r->desc.Height,
                        static_cast<unsigned>(r->desc.DepthOrArraySize), static_cast<unsigned>(r->desc.MipLevels),
                        r->desc.SampleDesc.Count, static_cast<unsigned>(r->desc.Flags));
        return E_INVALIDARG;
    }
    if (c->lost()) return DXGI_ERROR_DEVICE_REMOVED;
    // The source's pool, in the order its surfaces were made.
    PresentCopySurface* pool[native12::kPresentCopySurfacesPerSource]{};
    const native12::PresentCopySlot* slots[native12::kPresentCopySurfacesPerSource]{};
    unsigned count = 0;
    for (PresentCopySurface* s = r->present_copy; s; s = s->next) {
        if (s->width != shape.width || s->height != shape.height || s->format != shape.format) {
            // A resource's description cannot change; this is the backstop for one that did, and never a resize
            // path (a resize destroys the chain's resources and creates new ones, each with a pool of its own).
            // Refused, not remade: a surface of the old shape may still be read by a kernel present.
            if (present_copy_reshaped.first())
                log_refusal("present copy: the source's shape changed under its copy surface (%ux%u format %d became "
                            "%ux%u format %d): the present is not made",
                            s->width, s->height, static_cast<int>(s->format), shape.width, shape.height,
                            static_cast<int>(shape.format));
            return E_UNEXPECTED;
        }
        if (count == native12::kPresentCopySurfacesPerSource) return E_UNEXPECTED;    // never made: the cap holds
        pool[count] = s;
        slots[count] = &s->slot;
        ++count;
    }
    unsigned index = 0;
    const native12::PresentCopyChoice choice = native12::present_copy_choose(slots, count, pick.present_cpu, &index);
    PresentCopySurface* s = nullptr;
    switch (choice) {
    case native12::PresentCopyChoice::Reuse:
        s = pool[index];
        present_copy_reuse_count.fetch_add(1, std::memory_order_relaxed);
        break;
    case native12::PresentCopyChoice::Grow: {
        bool over_budget = false;
        const HRESULT made = make_present_copy_either(c, r, shape, &over_budget);
        if (FAILED(made)) {
            present_copy_refuse(over_budget ? "the device's copy surfaces have no room for another one"
                                            : "another one could not be made",
                                shape, count, c->present_copy_bytes.load(std::memory_order_relaxed), made);
            return over_budget ? E_OUTOFMEMORY : made;
        }
        s = r->present_copy;
        while (s && s->next) s = s->next;
        break;
    }
    case native12::PresentCopyChoice::Budget:
        present_copy_refuse("the source has its whole pool", shape, count,
                            c->present_copy_bytes.load(std::memory_order_relaxed), E_OUTOFMEMORY);
        return E_OUTOFMEMORY;
    case native12::PresentCopyChoice::Removed:
    default:
        if (present_copy_removed.first())
            log_refusal("present copy: a fence of the copy reads UINT64_MAX, the value of a removed device: the "
                        "present is not made");
        return DXGI_ERROR_DEVICE_REMOVED;
    }
    // A made surface is recorded on its resource; this says so to the reader and to /analyze, and is the one
    // place that would catch a maker that answered S_OK and left nothing behind.
    if (!s || !s->engine) return E_UNEXPECTED;
    // The surface's completion fence, before anything is recorded: a failure here leaves the list untouched, and a
    // new surface stays in the pool, unused, for the next present.
    HRESULT hr = pick.prepare(pick.state, s->slot);
    if (hr != S_OK) return FAILED(hr) ? hr : E_UNEXPECTED;
    auto* source = static_cast<ID3D12Resource*>(r->h.engine);
    ID3D12Resource* destination = s->engine;
    // The source is the presented back buffer, which the runtime's own rule leaves in
    // D3D12_RESOURCE_STATE_PRESENT - the same value as COMMON (d3d12.h: both are 0) - and the destination was
    // created in COMMON and is touched by nothing but this copy. Both pairs are balanced, so the list leaves the
    // state of both resources exactly as it found it and no later call of the application sees a state this
    // driver invented.
    struct Barriers {
        D3D12_RESOURCE_BARRIER b[2];
    } before{}, after{};
    for (unsigned i = 0; i < 2; ++i) {
        ID3D12Resource* resource = i ? destination : source;
        const D3D12_RESOURCE_STATES state = i ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
        before.b[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        before.b[i].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        before.b[i].Transition.pResource = resource;
        before.b[i].Transition.Subresource = 0;
        before.b[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        before.b[i].Transition.StateAfter = state;
        after.b[i] = before.b[i];
        after.b[i].Transition.StateBefore = state;
        after.b[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    }
    // One entry each, in this order, on the list the runtime handed to pfnPresent and closes and executes right
    // afterwards. record() makes the engine call here, or writes the entry for the list's replay worker; either
    // way the order within the list is this order (replay.h, R1 and R2).
    record(l, [=](ID3D12GraphicsCommandList* e) { e->ResourceBarrier(2, before.b); });
    record(l, [=](ID3D12GraphicsCommandList* e) { e->CopyResource(destination, source); });
    record(l, [=](ID3D12GraphicsCommandList* e) { e->ResourceBarrier(2, after.b); });
    *allocation = s->allocation;
    *slot = &s->slot;
    ++s->presents;
    const uint64_t calls = present_copy_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (present_copy_first.first())
        log_refusal("present copy: the first present of a source that is not a linear primary is served by a copy "
                    "(format %d, %ux%u, pitch %u, allocation %#x)",
                    static_cast<int>(shape.format), shape.width, shape.height, s->row_pitch,
                    static_cast<unsigned>(s->allocation));
    if (!s->kernel_blt_format && present_copy_unblittable.insert(static_cast<uint32_t>(shape.format)))
        log_refusal("present copy: format %d is copied into LB7A format %u, which the kernel driver's GPU present "
                    "cannot blit (driver\\kmd\\wddm.c WddmLinearColorFormat): the present is legal and the frame "
                    "may stay unpainted",
                    static_cast<int>(shape.format), shape.d3dddi);
    if (present_copy_doubling(calls))
        log_line("present copy: %llu presents copied (%llu through this surface, %ux%u, pitch %u; %llu surfaces "
                 "made, %llu reuses, %llu refusals)",
                 static_cast<unsigned long long>(calls), static_cast<unsigned long long>(s->presents), s->width,
                 s->height, s->row_pitch,
                 static_cast<unsigned long long>(present_copy_surface_count.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(present_copy_reuse_count.load(std::memory_order_relaxed)),
                 static_cast<unsigned long long>(present_copy_refusal_count.load(std::memory_order_relaxed)));
    return S_OK;
}

HRESULT present_copy_list_mark(DeviceContext* c, D3D12DDI_HCOMMANDLIST hlist,
                               native12::PresentCopyListMark** mark) noexcept {
    if (!mark) return E_INVALIDARG;
    *mark = nullptr;
    if (!c) return E_INVALIDARG;
    auto* l = record_of<CommandListRecord>(hlist.pDrvPrivate, Tag::CommandList, c);
    if (!l) return E_INVALIDARG;
    *mark = &l->present_copy;
    return S_OK;
}

HRESULT present_copy_slots(DeviceContext* c, D3D12DDI_HRESOURCE hres, native12::PresentCopySlot** slots,
                           unsigned capacity, unsigned* count) noexcept {
    if (!count) return E_INVALIDARG;
    *count = 0;
    if (!c || (capacity && !slots)) return E_INVALIDARG;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r) return E_INVALIDARG;
    if (!r->present_copy) return S_FALSE;
    unsigned n = 0;
    for (PresentCopySurface* s = r->present_copy; s; s = s->next) {
        if (n == capacity) return E_UNEXPECTED;
        slots[n++] = &s->slot;
    }
    *count = n;
    return S_OK;
}

HRESULT present_copy_surface_format(DeviceContext* c, D3D12DDI_HRESOURCE hres, uint32_t* lb7a) noexcept {
    if (!lb7a) return E_INVALIDARG;
    *lb7a = 0;
    if (!c) return E_INVALIDARG;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r) return E_INVALIDARG;
    if (!r->present_copy) return S_FALSE;
    *lb7a = r->present_copy->d3dddi;
    return S_OK;
}

HRESULT present_destination_format(DeviceContext* c, D3D12DDI_HRESOURCE hres, uint32_t* dxgi, uint32_t* lb7a) noexcept {
    if (!dxgi || !lb7a) return E_INVALIDARG;
    *dxgi = 0;
    *lb7a = 0;
    if (!c) return E_INVALIDARG;
    auto* r = record_of<ResourceRecord>(hres.pDrvPrivate, Tag::Resource, c);
    if (!r) return E_INVALIDARG;
    *dxgi = static_cast<uint32_t>(r->desc.Format);
    if (const AMDGPU_WDDM_SURFACE_FORMAT* row = native12::present_copy_format(*dxgi)) *lb7a = row->d3dddi;
    return S_OK;
}

uint64_t present_copies() noexcept { return present_copy_count.load(std::memory_order_relaxed); }
uint64_t present_copy_surfaces() noexcept { return present_copy_surface_count.load(std::memory_order_relaxed); }
uint64_t present_copy_reuses() noexcept { return present_copy_reuse_count.load(std::memory_order_relaxed); }
uint64_t present_copy_refusals() noexcept { return present_copy_refusal_count.load(std::memory_order_relaxed); }
uint64_t present_copy_kept() noexcept { return present_copy_kept_count.load(std::memory_order_relaxed); }

uint64_t shared_surface_retries() noexcept { return shared_retries.load(std::memory_order_relaxed); }

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
    // D64 and D65 are this module's slots, not fail-safes: a fail-safe answers E_NOTIMPL, which the runtime takes
    // as a critical driver failure and pays for with the application's device (BD-075). D65 opens a shared surface
    // now; what it declines, it declines in a code the runtime survives.
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
    // L68 is in both tables: a compute list needs barriers as much as a 3D list does, and the runtime may send
    // legacy ResourceBarrier calls through this slot on either (ref\ddi-display d3d12umddi.md,
    // PFND3D12DDI_BARRIER_0088 Remarks). A bundle reaches it too; the engine drops a barrier recorded in a bundle,
    // which is what D3D12 says of one (vkd3d-proton bundle.c, d3d12_bundle_Barrier).
    t->pfnBarrier = barrier;
}

bool select_barrier(D3D12DDI_COMMAND_LIST_FUNCS_3D_0092* t, uint32_t ddi_build) noexcept {
    if (!t) return false;
    if (ddi_build == D3D12DDI_BUILD_VERSION_0092 || ddi_build == D3D12DDI_BUILD_VERSION_0093) {
        t->pfnBarrier = barrier;
        return true;
    }
    log_refusal("Barrier: no 0092-layout L68 entry for DDI build %u", static_cast<unsigned>(ddi_build));
    return false;
}

bool make_list_0094(const D3D12DDI_COMMAND_LIST_FUNCS_3D_0092& from, D3D12DDI_COMMAND_LIST_FUNCS_3D_0094* to) noexcept {
    // D3D12DDI_COMMAND_LIST_FUNCS_3D_0094 (H:11303-11382) is the 0092 table with PFND3D12DDI_BARRIER_0094 in L68:
    // the same 70 members in the same order, and every member other than L68 has the same type in both. Those are
    // copied as they are; L68 is written with the 0094 entry under its own type, and the 0088 entry of from is not
    // carried over.
    using T92 = D3D12DDI_COMMAND_LIST_FUNCS_3D_0092;
    using T94 = D3D12DDI_COMMAND_LIST_FUNCS_3D_0094;
    static_assert(sizeof(T94) == sizeof(T92) && offsetof(T94, pfnBarrier) == offsetof(T92, pfnBarrier) &&
                  offsetof(T94, pfnDispatchMesh) == offsetof(T92, pfnDispatchMesh) &&
                  offsetof(T94, pfnOmSetAlphaBlendFactor) == offsetof(T92, pfnOmSetAlphaBlendFactor) &&
                  std::is_same_v<decltype(T94::pfnDispatchMesh), decltype(T92::pfnDispatchMesh)> &&
                  std::is_same_v<decltype(T94::pfnOmSetAlphaBlendFactor), decltype(T92::pfnOmSetAlphaBlendFactor)>);
    if (!to) return false;
    std::memcpy(to, &from, sizeof(*to));
    to->pfnBarrier = barrier_0094;
    return true;
}

} // namespace engine_ddi
