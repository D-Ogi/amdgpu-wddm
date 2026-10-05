// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
struct DdiUnorderedAccessView { ID3D11UnorderedAccessView *object; };
// `plane` is the PlaneSlice of the WDDM 2.0 create argument; see ddi-srv.h. Only a Texture2D view
// has a plane, and every other dimension takes 0.
HRESULT convert_uav(const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW &input,
    UINT layers,UINT samples,UINT plane,D3D11_UNORDERED_ACCESS_VIEW_DESC1 &output);
// The same view without the plane field; see ddi-srv.h.
void demote_uav(const D3D11_UNORDERED_ACCESS_VIEW_DESC1 &input,D3D11_UNORDERED_ACCESS_VIEW_DESC &output);
struct UavRequest {
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 desc1{};
    D3D11_UNORDERED_ACCESS_VIEW_DESC legacy{};
    bool derive_plane=false;
};
HRESULT plan_uav(const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW &input,
    UINT layers,UINT samples,UINT plane,UavRequest &output);
void create_unordered_access_view(D3D10DDI_HDEVICE device,const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *input,
    UINT plane,D3D11DDI_HUNORDEREDACCESSVIEW view,D3D11DDI_HRTUNORDEREDACCESSVIEW runtime);
void install_uav_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
