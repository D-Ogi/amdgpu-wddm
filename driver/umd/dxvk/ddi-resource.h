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
    // with image storage. No importer is installed yet.
    D3DKMT_HANDLE present_allocation=0;
    UINT present_subresource=0;
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
void install_resource_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
