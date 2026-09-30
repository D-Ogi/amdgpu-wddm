// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
struct DdiDepthStencil { ID3D11DepthStencilState *object; };
struct DdiRasterizer { ID3D11RasterizerState1 *object; };
D3D11_DEPTH_STENCIL_DESC convert_depth_stencil(const D3D10_DDI_DEPTH_STENCIL_DESC &input);
D3D11_RASTERIZER_DESC1 convert_rasterizer(const D3D11_1_DDI_RASTERIZER_DESC &input);
void install_fixed_state_ddi(D3D11_1DDI_DEVICEFUNCS &table);
// WDDM 2.0: the rasterizer description adds the conservative rasterization mode.
D3D11_RASTERIZER_DESC2 convert_rasterizer2(const D3DWDDM2_0DDI_RASTERIZER_DESC &input);
void install_fixed_state_wddm2_0_ddi(D3DWDDM2_0DDI_DEVICEFUNCS &table);
}
