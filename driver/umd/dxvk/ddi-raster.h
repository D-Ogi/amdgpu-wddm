// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include <array>
namespace bc250::umd {
using Viewports=std::array<D3D11_VIEWPORT,D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE>;
HRESULT convert_viewports(UINT count,UINT clear,const D3D10_DDI_VIEWPORT *input,Viewports &out);
bool valid_topology(D3D10_DDI_PRIMITIVE_TOPOLOGY topology);
void install_raster_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
