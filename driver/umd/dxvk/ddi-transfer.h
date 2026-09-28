// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
HRESULT convert_copy_box(const D3D10_DDI_BOX *input,D3D11_BOX &output,bool &empty);
HRESULT convert_copy_flags(UINT input,UINT &output);
void install_transfer_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
