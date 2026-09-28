// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
struct DdiShaderResourceView { ID3D11ShaderResourceView *object; };
HRESULT convert_srv(const D3D11DDIARG_CREATESHADERRESOURCEVIEW &input,
    UINT layers,UINT samples,D3D11_SHADER_RESOURCE_VIEW_DESC &output);
void install_srv_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
