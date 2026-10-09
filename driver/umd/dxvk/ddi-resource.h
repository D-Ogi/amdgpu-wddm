// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
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
    RuntimeSurface *runtime_surface=nullptr; // DeviceOwner owns; COM object is borrowed.
};
struct ResourceDescription {
    D3D10DDIRESOURCE_TYPE dimension{};
    D3D11_BUFFER_DESC buffer{};
    D3D11_TEXTURE1D_DESC texture1d{};
    D3D11_TEXTURE2D_DESC texture2d{};
    D3D11_TEXTURE3D_DESC texture3d{};
    std::vector<D3D11_SUBRESOURCE_DATA> initial;
};
HRESULT convert_resource(const D3D11DDIARG_CREATERESOURCE &input,ResourceDescription &output);
// scanout is the device's scan-out source (scanout-primary.h); nullptr asks for no scan-out primary.
HRESULT convert_runtime_resource(const D3D11DDIARG_CREATERESOURCE &,HANDLE,RuntimeSurfaceRequest &,D3D11_TEXTURE2D_DESC1 &,
    const ScanoutSource *scanout=nullptr);
HRESULT decode_open_resource(const D3D10DDIARG_OPENRESOURCE &,BC250_WDDM_ALLOCATION_PRIVATE &,D3D11_TEXTURE2D_DESC1 &);
void install_resource_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
