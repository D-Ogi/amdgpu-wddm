// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
struct DdiBlend { ID3D11BlendState1 *object; };
HRESULT convert_blend(const D3D11_1_DDI_BLEND_DESC &input,D3D11_BLEND_DESC1 &output);
void install_blend_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
