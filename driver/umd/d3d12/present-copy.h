// SPDX-License-Identifier: MIT
#pragma once
#include <windows.h>
#include <d3d12.h>                              // D3D12_RESOURCE_DESC1: the source's description as we hold it
#include <d3d12umddi.h>
#include "../../contract/amdgpu_wddm_surface_format.h"
#include "../../contract/bc250_shared_surface.h"
#include <cstdint>

// BD-133: the rule that decides whether a Present source this driver did not make linear can be presented by
// copying it, and the description of the surface the copy writes into. Pure: no engine, no runtime callback, no
// kernel call, so the whole decision is host-tested (present-outputs-test.cpp).
//
// Why a copy exists at all. PFND3D12DDI_PRESENT_0051 "copies content from source allocations to a primary surface
// (and sometimes to off-screen system memory allocations)" (ref\ddi-display\d3d12umddi.md:14693), and the runtime
// hands the DDI a command list it closes and executes immediately afterwards (the BD-133 trace: pfnPresent 3704,
// pfnCloseCommandList 3705, pfnExecuteCommandLists 3706 on the same list). Until BD-133 the shell answered the
// call with the allocation of the source resource itself and admitted one shape only - a linear surface this
// device created (engine-ddi resources.cpp, linear_surface_allocation) - because that allocation is the only one
// a reader outside the engine can read row by row. A D3D9On12 back buffer is an ordinary texture of the device,
// so the DDI refused it and the device was removed at the first windowed present (BD-133). The copy admits that
// source: the pixels are copied into a surface of the admitted kind inside the Present DDI, and the kernel
// present is given the copy.
//
// What the kernel present takes as a source, and why these fields matter:
//   - driver\kmd\wddm_allocation_identity.inc:88 WddmSnapshotPresentAllocations admits a source only if its
//     handle is an object this device opened, is not a BC2A blob (opened->UmdAlloc, set at
//     driver\kmd\wddm.c:4865 when a UMD allocation is opened, 4518 when one is created) and its opened
//     descriptor equals the backing allocation's;
//     the two sides must also carry the same format ("CP DMA moves bytes and cannot convert",
//     driver\kmd\present_snapshot.h Bc250SnapshotFormat). So the source must be an LB7A surface allocation -
//     the 32-byte BC250_WDDM_ALLOCATION_PRIVATE description that AllocationRequest::prepare_surface and
//     ::prepare_shared_surface write (allocation-request.h) - and never the BC2A blob of an ordinary heap.
//   - driver\kmd\wddm.c:7249 then refuses any format outside WddmLinearColorFormat (driver\kmd\wddm.c:6835:
//     A8R8G8B8, X8R8G8B8, A8B8G8R8, X8B8G8R8) with STATUS_GRAPHICS_CANNOTCOLORCONVERT, and needs a nonzero
//     page-aligned address and size.
// kernel_blt_format below carries that last fact per format, so a 10-bit or FP16 back buffer is admitted and
// copied (the architecture is not 8-bit only) while the one line the refusal costs names the component that
// cannot read it. Nothing here converts: the copy's destination has the source's own format, which is what
// keeps the copy a byte copy.
//   What the kernel side needs for a 10-bit or FP16 present to reach the screen (review of BD-133, finding 6;
// not this change): WddmLinearColorFormat (driver\kmd\wddm.c:6835) admits four 8-bit D3DDDI formats and the blit
// emitter is handed Bc250BltRgba8 or Bc250BltBgra8 only, so the rows of BC250_BLIT_SURFACE's format enum are
// what have to grow by the 4-byte RGB10A2 and the 8-byte FP16 cases. The present is a byte copy of two equal
// formats, so nothing about the copy itself changes with it.

namespace native12 {

// Why a source cannot be copied. Admitted is the only value that lets a present proceed.
enum class PresentCopy : uint32_t {
    Admitted = 0,
    NoDescription,      // no resource description to read
    NotTexture2D,       // a buffer, a 1D or a 3D texture: not a surface a present reads row by row
    Subresources,       // more than one mip level, array slice or plane
    Multisampled,       // a multisampled source needs a resolve, which is not a copy
    Edge,               // zero, or wider or taller than one linear surface may be
    Format,             // no row a copy destination may take (present_copy_format): no linear surface of it exists
    DepthStencil,       // a depth-stencil surface is not a colour surface a present can read
    DenyShaderResource, // DENY_SHADER_RESOURCE, which D3D12 allows only with ALLOW_DEPTH_STENCIL
    CrossAdapter,       // another adapter's memory layout
    Count
};

// One short reason for the refusal line; never null.
inline const char* present_copy_text(PresentCopy why) noexcept {
    switch (why) {
    case PresentCopy::Admitted: return "admitted";
    case PresentCopy::NoDescription: return "no resource description";
    case PresentCopy::NotTexture2D: return "not a 2D texture";
    case PresentCopy::Subresources: return "more than one subresource";
    case PresentCopy::Multisampled: return "multisampled";
    case PresentCopy::Edge: return "width or height outside one linear surface";
    case PresentCopy::Format: return "format has no linear surface";
    case PresentCopy::DepthStencil: return "depth-stencil";
    case PresentCopy::DenyShaderResource: return "shader resources denied";
    case PresentCopy::CrossAdapter: return "cross-adapter";
    case PresentCopy::Count: break;
    }
    return "unknown";
}

// The widest linear surface this driver makes, as engine-ddi's kLinearMaxEdge and the wire's
// BC250_SHARED_MAX_EDGE both have it. Kept as its own constant here so that the pure rule compiles without
// engine-ddi, and checked against the wire below.
inline constexpr uint32_t kPresentCopyMaxEdge = 8192;
// The wire admits a wider surface than either module makes; the edge above is engine-ddi's kLinearMaxEdge and
// the shell's own edge in AllocationRequest::prepare_shared_surface, and it must stay inside the wire's.
static_assert(kPresentCopyMaxEdge <= BC250_SHARED_MAX_EDGE, "the linear surface edge is outside the wire's");

// What the copy's destination surface is: the source's own geometry and format.
struct PresentCopyShape {
    uint32_t width{};
    uint32_t height{};
    DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};    // the source's format, not a storage substitute
    uint32_t bytes_per_pixel{};                 // from the format's row; never a constant 4
    uint32_t d3dddi{};                          // LB7A.Format the surface will carry (D3DDDIFORMAT)
    bool kernel_blt_format{};                   // WddmLinearColorFormat admits that LB7A format
};

// Whether the LB7A format is one the kernel driver's GPU present can blit (driver\kmd\wddm.c:6835).
inline bool present_copy_kernel_blt_format(uint32_t d3dddi) noexcept {
    return d3dddi == AMDGPU_WDDM_D3DDDI_A8R8G8B8 || d3dddi == AMDGPU_WDDM_D3DDDI_X8R8G8B8 ||
           d3dddi == AMDGPU_WDDM_D3DDDI_A8B8G8R8 || d3dddi == static_cast<uint32_t>(D3DDDIFMT_X8B8G8R8);
}

// BD-133, the D3D9 back buffer (lab arms B and C, 2026-10-12 16:54Z). D3D9On12 gives a D3DFMT_X8R8G8B8 back
// buffer - the most common D3D9 back-buffer format - to this driver as DXGI_FORMAT_B8G8R8X8_UNORM (88), and the
// first version of this rule refused it ("format has no linear surface"), which removed the device at the first
// windowed present of every such game. The surface format table has a row for these bytes: the X8 row, LB7A
// format X8R8G8B8 (22). It has no DXGI number and no COMPOSED bit on purpose (amdgpu_wddm_surface_format.h), and
// that is what keeps it out of every lookup by DXGI number - the wire (Bc250SharedSurfaceFormat), the scan-out
// rule (bc250_scanout_primary_rule, runtime_scanout_format) and both shells' composed surfaces. Giving the row a
// DXGI number would admit a B8G8R8X8 scan-out primary and a B8G8R8X8 shared surface in every component built
// from the table, which none of them is tested for. So the table stays as it is, and the copy's destination - a
// surface of this driver's own, which no other component opens and which is never scanned out - takes the X8 row
// here, by its LB7A format, for exactly the two DXGI formats that store those bytes:
//   - DXGI_FORMAT_B8G8R8X8_UNORM (88) and its sRGB view DXGI_FORMAT_B8G8R8X8_UNORM_SRGB (93). Both are
//     D3DDDIFMT_X8R8G8B8 bytes; the sRGB view changes how a shader reads them, not what a byte copy moves.
//   - The kernel driver creates a type-0 LB7A surface of X8R8G8B8: its admission takes a COMPOSED row or a
//     SCANOUT_PRIMARY row (driver\kmd\surface_format.h, WddmSurfaceFormatBpp, BC250_SURFACE_COMPOSED), and the
//     X8 row is the second. Its GPU present blits X8R8G8B8 (driver\kmd\wddm.c:6835, WddmLinearColorFormat).
//   - The engine maps 88 to VK_FORMAT_B8G8R8A8_UNORM and 93 to VK_FORMAT_B8G8R8A8_SRGB, the Vulkan formats of
//     the composed BGRA8 row (vkd3d-proton libs\vkd3d\utils.c, the format table), so the linear image of either
//     is the BGRA8 linear image the linear primary already proves; CopyResource from an 88 source into an 88
//     destination is a copy within one format, as for every other row.
// The X8 row admitted for the copy, or null if the table ever loses it.
inline const AMDGPU_WDDM_SURFACE_FORMAT* present_copy_x8_row() noexcept {
    return amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_d3dddi(AMDGPU_WDDM_D3DDDI_X8R8G8B8),
                                     AMDGPU_WDDM_SURFACE_SCANOUT_PRIMARY);
}

// The row of a copy destination that only this driver's own surface may have: the X8 row for the two DXGI
// formats that store X8R8G8B8, null for every other format. The shell's memory request reads this for a surface
// that names no runtime resource (heap-import.cpp, driver_surface) and for nothing else.
inline const AMDGPU_WDDM_SURFACE_FORMAT* present_copy_own_format(uint32_t dxgi) noexcept {
    if (dxgi != static_cast<uint32_t>(DXGI_FORMAT_B8G8R8X8_UNORM) &&
        dxgi != static_cast<uint32_t>(DXGI_FORMAT_B8G8R8X8_UNORM_SRGB))
        return nullptr;
    return present_copy_x8_row();
}

// The row a copy destination of this source format takes: the composed row the wire describes - by its storage
// format or by the sRGB view that shares that storage, the lookup the whole stack uses for a linear surface
// (engine-ddi resources.cpp, composed_format; driver\contract\bc250_shared_surface.h, Bc250SharedSurfaceFormat) -
// or the X8 row of present_copy_own_format. Null: there is no linear surface of this format to copy into.
inline const AMDGPU_WDDM_SURFACE_FORMAT* present_copy_format(uint32_t dxgi) noexcept {
    if (const AMDGPU_WDDM_SURFACE_FORMAT* row = Bc250SharedSurfaceFormat(static_cast<unsigned long>(dxgi)))
        return row;
    return present_copy_own_format(dxgi);
}

// Whether the destination of this source format has the primary form (present_copy_as_primary below) to fall
// back to when the shared form is refused. A composed row's primary is looked up by its storage format alone
// (heap-import.cpp, "primary format"), so the sRGB view of a composed row has none - unchanged from the first
// version of this rule. The X8 pair has one for both formats: its records name no DXGI format in either form
// (the 12-byte E26R v1 record and the LB7A description alone, allocation-request.h prepare_surface), so the
// sRGB view is not a different surface there.
inline bool present_copy_primary_form(uint32_t dxgi) noexcept {
    if (amdgpu_wddm_surface_admit(amdgpu_wddm_surface_format_by_dxgi(dxgi), AMDGPU_WDDM_SURFACE_COMPOSED))
        return true;
    return present_copy_own_format(dxgi) != nullptr;
}

// The one admission rule. desc is the source resource's description as the driver holds it; out is written only
// when the answer is Admitted.
inline PresentCopy present_copy_admit(const D3D12_RESOURCE_DESC1& desc, PresentCopyShape* out) noexcept {
    if (out) *out = PresentCopyShape{};
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return PresentCopy::NotTexture2D;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) return PresentCopy::DepthStencil;
    // Its own reason, not the depth-stencil one, because a refusal line that names the wrong flag sends the next
    // reader after the wrong resource (review of BD-133, finding 7). D3D12 allows DENY_SHADER_RESOURCE only
    // together with ALLOW_DEPTH_STENCIL (D3D12_RESOURCE_FLAGS), so a colour source should never carry it; a
    // source that does is refused by name instead of silently.
    if (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) return PresentCopy::DenyShaderResource;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER) return PresentCopy::CrossAdapter;
    if (desc.DepthOrArraySize != 1 || desc.MipLevels != 1) return PresentCopy::Subresources;
    if (desc.SampleDesc.Count != 1 || desc.SampleDesc.Quality) return PresentCopy::Multisampled;
    if (!desc.Width || desc.Width > kPresentCopyMaxEdge || !desc.Height || desc.Height > kPresentCopyMaxEdge)
        return PresentCopy::Edge;
    // The lookup the whole stack uses for a linear surface's format, widened by the X8 row for the D3D9 back
    // buffer and for nothing else (present_copy_format above). Any other format has no linear surface, so there
    // is nothing to copy into.
    const AMDGPU_WDDM_SURFACE_FORMAT* row = present_copy_format(static_cast<uint32_t>(desc.Format));
    if (!row || !row->bytes_per_pixel) return PresentCopy::Format;
    if (out) {
        out->width = static_cast<uint32_t>(desc.Width);
        out->height = desc.Height;
        out->format = desc.Format;
        out->bytes_per_pixel = row->bytes_per_pixel;
        out->d3dddi = row->d3dddi;
        out->kernel_blt_format = present_copy_kernel_blt_format(row->d3dddi);
    }
    return PresentCopy::Admitted;
}

// The heap and resource descriptions of the copy's destination, in the DDI's own structures, so that the shell's
// memory request for it is the one shape the shell already admits for a linear surface that is nobody's primary
// (heap-import.cpp, shared_flags): one dedicated 2D texture of one mip, one slice and one sample, on a GPU-only
// L1 heap without PRIMARY and without COHERENT_SYSTEMWIDE. ByteSize stays 0: only the engine's own answer for
// the linear image may size it (engine-ddi resources.cpp, query_linear_surface).
struct PresentCopyRequest {
    D3D12DDIARG_CREATEHEAP_0001 heap{};
    D3D12DDIARG_CREATERESOURCE_0088 resource{};
};

inline void present_copy_description(const PresentCopyShape& shape, PresentCopyRequest* out) noexcept {
    if (!out) return;
    *out = PresentCopyRequest{};
    out->heap.ByteSize = 0;                     // the caller fills it from the engine's linear image answer
    out->heap.Alignment = 0;
    out->heap.CPUPageProperty = D3D12DDI_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    out->heap.MemoryPool = D3D12DDI_MEMORY_POOL_L1;
    out->heap.Flags = D3D12DDI_HEAP_FLAG_RT_DS_TEXTURES;
    out->heap.CreationNodeMask = 1;
    out->heap.VisibleNodeMask = 1;
    out->resource.ResourceType = D3D12DDI_RT_TEXTURE2D;
    out->resource.Width = shape.width;
    out->resource.Height = shape.height;
    out->resource.DepthOrArraySize = 1;
    out->resource.MipLevels = 1;
    out->resource.Format = shape.format;
    out->resource.SampleDesc = {1, 0};
    out->resource.Layout = D3D12DDI_TL_UNDEFINED;
    // The shape the engine's linear image is proven to take (engine-ddi tests, test-linear-primary.cpp: a
    // committed render target of this description becomes a linear image at offset 0). The surface is only ever
    // a copy destination; the render-target flag is what puts it in the RT_DS_TEXTURES category of the heap
    // above, which is the category the linear primary uses as well.
    out->resource.Flags = D3D12DDI_RESOURCE_FLAG_0003_RENDER_TARGET;
    out->resource.InitialBarrierLayout = D3D12DDI_BARRIER_LAYOUT_COMMON;
}

// The same surface asked for as a composed primary instead of a shared surface: the heap says PRIMARY, which is
// what makes the shell write the 12-byte E26R v1 record and the PRIMARY allocation flag
// (AllocationRequest::prepare_surface) in place of the shared surface's 64-byte v3 record. Neither form is proven
// for an allocation with no runtime resource, so engine-ddi asks for this one when the other is refused. The LB7A
// description, and therefore everything the kernel present reads, is the same in both.
inline void present_copy_as_primary(PresentCopyRequest* out) noexcept {
    if (!out) return;
    out->heap.Flags = static_cast<D3D12DDI_HEAP_FLAGS>(out->heap.Flags | D3D12DDI_HEAP_FLAG_PRIMARY);
}

} // namespace native12
