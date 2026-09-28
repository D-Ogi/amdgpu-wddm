// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
bool format_allows_not_supported(DXGI_FORMAT);
UINT convert_format_support(UINT support,UINT support2);
void install_format_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
