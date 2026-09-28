// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
HRESULT convert_map(D3D10_DDI_MAP type,UINT flags,D3D11_MAP &mappedType,UINT &mappedFlags);
void install_map_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
