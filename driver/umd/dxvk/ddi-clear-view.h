// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
ID3D11View *clear_view_object(D3D11DDI_HANDLETYPE,void *);
void install_clear_view_ddi(D3D11_1DDI_DEVICEFUNCS &);
}
