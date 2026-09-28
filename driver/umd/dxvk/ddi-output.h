// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-rtv.h"
#include "ddi-dsv.h"
#include "ddi-uav.h"
#include <array>
namespace bc250::umd {
struct OutputBindings {
    std::array<ID3D11RenderTargetView *,D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs{};
    ID3D11DepthStencilView *dsv=nullptr;
    std::array<ID3D11UnorderedAccessView *,D3D11_1_UAV_SLOT_COUNT> uavs{};
    std::array<UINT,D3D11_1_UAV_SLOT_COUNT> counters{};
    UINT numRtvs=0,uavFirst=0,uavCount=0;
};
HRESULT prepare_output_bindings(const D3D10DDI_HRENDERTARGETVIEW *rtvs,UINT numRtvs,UINT clearSlots,
    D3D10DDI_HDEPTHSTENCILVIEW dsv,const D3D11DDI_HUNORDEREDACCESSVIEW *uavs,const UINT *counters,
    UINT first,UINT count,UINT changedFirst,UINT changedCount,UINT limit,OutputBindings &out);
void install_output_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
