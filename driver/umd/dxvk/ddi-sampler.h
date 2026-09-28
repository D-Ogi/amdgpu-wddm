// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
namespace bc250::umd {
struct DdiSampler { ID3D11SamplerState *object; };
D3D11_SAMPLER_DESC convert_sampler(const D3D10_DDI_SAMPLER_DESC &input);
void install_sampler_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
