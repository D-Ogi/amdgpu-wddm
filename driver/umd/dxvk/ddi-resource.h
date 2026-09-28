// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include <vector>
namespace bc250::umd {
struct DdiResource { ID3D11Resource *object; D3D10DDIRESOURCE_TYPE dimension; };
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
