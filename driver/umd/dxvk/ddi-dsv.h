// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
struct DdiDepthStencilView { ID3D11DepthStencilView *object; };
HRESULT convert_dsv(const D3D11DDIARG_CREATEDEPTHSTENCILVIEW &input,
    UINT resourceArraySize,UINT samples,D3D11_DEPTH_STENCIL_VIEW_DESC &output);
void install_dsv_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
