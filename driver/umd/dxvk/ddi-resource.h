// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include <vector>
namespace bc250::umd {
struct DdiResource {
    ID3D11Resource *object;
    D3D10DDIRESOURCE_TYPE dimension;
    // Borrowed identity supplied by the runtime-allocation owner/importer.
    // Zero for ordinary engine-owned resources. Rotation must move this pair
    // with image storage.
    D3DKMT_HANDLE present_allocation=0;
    UINT present_subresource=0;
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
HRESULT convert_runtime_resource(const D3D11DDIARG_CREATERESOURCE &,HANDLE,RuntimeSurfaceRequest &,D3D11_TEXTURE2D_DESC1 &);
HRESULT decode_open_resource(const D3D10DDIARG_OPENRESOURCE &,BC250_WDDM_ALLOCATION_PRIVATE &,D3D11_TEXTURE2D_DESC1 &);
void install_resource_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
