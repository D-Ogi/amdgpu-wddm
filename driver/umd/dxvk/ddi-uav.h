// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
struct DdiUnorderedAccessView { ID3D11UnorderedAccessView *object; };
HRESULT convert_uav(const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW &input,
    UINT layers,UINT samples,D3D11_UNORDERED_ACCESS_VIEW_DESC &output);
void install_uav_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
