// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-entry.h"
#include "ddi/bc250_dxvk_engine.h"
#include <vector>
namespace bc250::umd {
enum class ShaderStage { vertex, geometry, pixel, compute, hull, domain };
struct DdiShader { IUnknown *object; ShaderStage stage; };
HRESULT copy_legacy_signature(const D3D11_1DDIARG_SIGNATURE_ENTRY *entries,UINT count,
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> &out);
void install_shader_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
