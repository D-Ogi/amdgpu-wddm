// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
HRESULT resource_buffer(D3D10DDI_HRESOURCE handle,ID3D11Buffer *&buffer);
bool valid_stream_output_buffer(const D3D11_BUFFER_DESC &desc,UINT offset);
void install_buffer_binding_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
