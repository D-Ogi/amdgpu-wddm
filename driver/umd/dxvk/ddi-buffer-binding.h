// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
HRESULT resource_buffer(D3D10DDI_HRESOURCE handle,ID3D11Buffer *&buffer);
void install_buffer_binding_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
