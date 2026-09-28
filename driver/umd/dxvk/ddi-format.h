// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
bool defined_dxgi_format(DXGI_FORMAT);
// S_FALSE means a valid, completed fallback caps answer; other errors remain errors.
HRESULT classify_format_result(DXGI_FORMAT,HRESULT,UINT &fallback);
bool format_allows_not_supported(DXGI_FORMAT);
HRESULT classify_format_support2_result(HRESULT,UINT flags);
UINT convert_format_support(UINT support,UINT support2);
void install_format_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
