// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
// Installs only the implemented operations. Not a complete table and not a
// feature-level advertisement. Other entries belong to subsequent modules.
bool valid_indirect_arguments(const D3D11_BUFFER_DESC &desc,UINT offset,UINT argumentBytes);
void install_draw_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
