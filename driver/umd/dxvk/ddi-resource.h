// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include "umd-etw.h"
#include <vector>
namespace bc250::umd {
// The plane a D3D11.1 view create argument names: none. That table has no plane field, so the view
// format is the only thing that can name a chroma plane there, and the engine derives the plane from
// it. The three view implementations take this value instead of a plane and then call the engine
// entry that derives one, because sending plane 0 would make every planar view on that table fail
// (BD-071 review). The WDDM 2.0 table names the plane itself and never sends this value.
constexpr UINT ddi_plane_from_view_format=~0u;
struct DdiResource {
    ID3D11Resource *object;
    D3D10DDIRESOURCE_TYPE dimension;
    // Borrowed identity supplied by the runtime-allocation owner/importer.
    // Zero for ordinary engine-owned resources. Rotation must move this pair
    // with image storage.
    D3DKMT_HANDLE present_allocation=0;
    UINT present_subresource=0;
    // BD-065: a present buffer that is not shared, not a primary and not displayable. Flip-model buffers are
    // shared with the compositor or are primaries; only a blt-model swap chain presents a buffer like this.
    bool blt_model_buffer=false;
    // BD-407: a multisampled blt-model buffer (multisampled_present_buffer). It is an engine texture without a kernel
    // allocation, because a linear LB7A allocation holds one sample a pixel. present_allocation stays 0, and each
    // Present of it resolves into a present shadow (present_resolve_format).
    bool resolve_on_present=false;
    RuntimeSurface *runtime_surface=nullptr; // DeviceOwner owns; COM object is borrowed.
    // BD-309: the kernel allocations of an engine-private resource from its offer until its reclaim. The engine
    // names at most kEngineOfferCapacity memory objects; a resource with more stays an offer hint.
    static constexpr UINT kEngineOfferCapacity=8;
    D3DKMT_HANDLE engine_offered[kEngineOfferCapacity]{};
    UINT engine_offered_count=0;
    // BD-372: the reclaim of this engine-private resource reported an allocation NOT_COMMITTED. The resource has no
    // backing from then on, and the application must not use it again (DXGI_RECLAIM_RESOURCE_RESULTS): the engine
    // gets no EndOffer (no map of memory without backing), a later offer is a hint, and a later reclaim reports
    // NOT_COMMITTED again without a kernel call.
    bool engine_decommitted=false;
};
struct ResourceDescription {
    D3D10DDIRESOURCE_TYPE dimension{};
    D3D11_BUFFER_DESC buffer{};
    D3D11_TEXTURE1D_DESC texture1d{};
    D3D11_TEXTURE2D_DESC texture2d{};
    D3D11_TEXTURE3D_DESC texture3d{};
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
};
// The UMD logging mapping of a resource with a runtime surface (umd-etw.h): the hResource of the DDI, which is this
// DdiResource, holds the whole kernel allocation of its surface from offset 0. Only for a resource whose
// runtime_surface is set.
inline UINT64 umd_etw_resource(const DdiResource &resource) noexcept {
    return UINT64(reinterpret_cast<UINT_PTR>(&resource));
}
inline etw::AllocationMapping runtime_surface_mapping(const DdiResource &resource) noexcept {
    const RuntimeSurface &surface=*resource.runtime_surface;
    return {umd_etw_resource(resource),UINT64(surface.allocation.allocation),0,surface.bytes,etw::usage::none,
            etw::AllocationSemantic::none};
}
HRESULT convert_resource(const D3D11DDIARG_CREATERESOURCE &input,ResourceDescription &output);
// The status that CreateResource reports when it does not complete `desc` after the failure `hr`
// (ddi_create_resource_status): DXGI_DDI_ERR_UNSUPPORTED only for a primary, never device removal for a refusal.
HRESULT create_refusal(const D3D11DDIARG_CREATERESOURCE &desc,HRESULT hr);
// BD-407: a multisampled back buffer of a blt-model swap chain (DXGI_SWAP_EFFECT_DISCARD or SEQUENTIAL with
// SampleDesc.Count above 1): a TEXTURE2D with BIND_PRESENT that is not shared and not displayable, with no primary
// descriptor (windowed) or an optional one (DXGI_DDI_PRIMARY_OPTIONAL, a full-screen back buffer). DXGI moves its
// content with BltDXGI and the Resolve flag, or presents it with the Blt flag (dxgi-presentation-path.md), so it
// does not need a kernel allocation. A front buffer or a proxy (a primary that is not optional) is not one.
bool multisampled_present_buffer(const D3D11DDIARG_CREATERESOURCE &);
// The engine description of such a buffer: the same texture without the present flags. A format without a
// composed row (or A8) is refused, because present_resolve_format has no shadow for it.
HRESULT convert_multisampled_present_buffer(const D3D11DDIARG_CREATERESOURCE &,ResourceDescription &);
// BD-407: the scan-out opt-out of a multisampled primary. The shell cannot scan out a multisampled surface, so it
// sets DXGI_DDI_PRIMARY_DRIVER_FLAG_NO_SCANOUT in the descriptor's DriverFlags. For an optional back buffer, DXGI
// then presents with Blt instead of Flip; with the DXGI_DDI_ERR_UNSUPPORTED refusal of a front buffer or a proxy,
// DXGI puts a single-sample proxy between the buffers and the primary (passing-dxgi-information-at-resource-
// creation-time.md). Returns true when it set the flag. A single-sample primary is not changed.
bool opt_out_multisampled_scanout(const D3D11DDIARG_CREATERESOURCE &);
// True for a create whose E26R record an opener can reach, so a record that the contract's own writer must
// write (driver/contract/bc250_shared_surface.h, Bc250SharedSurfaceResourceEncode). convert_runtime_resource
// branches on this once: such a create is refused when the contract refuses its record, while a private
// create keeps this shell's write and D3D11's wider admission (a tiled texture2d, BD-419).
bool record_may_be_opened(const RuntimeSurfaceRequest &);
// scanout is the device's scan-out source (scanout-primary.h); nullptr asks for no scan-out primary.
HRESULT convert_runtime_resource(const D3D11DDIARG_CREATERESOURCE &,HANDLE,RuntimeSurfaceRequest &,D3D11_TEXTURE2D_DESC1 &,
    const ScanoutSource *scanout=nullptr);
HRESULT decode_open_resource(const D3D10DDIARG_OPENRESOURCE &,BC250_WDDM_ALLOCATION_PRIVATE &,D3D11_TEXTURE2D_DESC1 &,
    BC250_WDDM_NV12_PRIVATE *nv12=nullptr);
// BD-373 step 3: the form of a shared resource. engine_layout_resource is true for a shared resource that is not a
// primary, a present buffer or displayable and that the LINEAR SURFACE form cannot hold; convert_engine_resource
// makes its engine description (without the sharing flags) or refuses it with *why (a BC250_SR_WHY_* string).
// engine_layout_open is true for an OpenResource whose record is an S26R record (bc250_shared_resource.h).
bool engine_layout_resource(const D3D11DDIARG_CREATERESOURCE &);
HRESULT convert_engine_resource(const D3D11DDIARG_CREATERESOURCE &,EngineResourceDesc &,ResourceDescription &,
    const char **why);
bool engine_layout_open(const D3D10DDIARG_OPENRESOURCE &);
void install_resource_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
