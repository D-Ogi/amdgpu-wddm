// SPDX-License-Identifier: MIT
#pragma once
#include "input-layout.h"
#include "ddi/bc250_dxvk_engine.h"
HRESULT bc250_create_engine_input_layout(IBc250DxvkDevice *engine,
    const D3D10DDIARG_CREATEELEMENTLAYOUT &input, ID3D11InputLayout **output);
