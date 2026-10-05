// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
struct DdiInputLayout { ID3D11InputLayout *object; };
void install_input_layout_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
