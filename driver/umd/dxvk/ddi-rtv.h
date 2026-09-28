// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
struct DdiRenderTargetView { ID3D11RenderTargetView *object; };
HRESULT convert_rtv(const D3D10DDIARG_CREATERENDERTARGETVIEW &input,
    UINT resourceArraySize,UINT samples,D3D11_RENDER_TARGET_VIEW_DESC &output);
void install_rtv_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
