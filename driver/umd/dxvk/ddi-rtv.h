// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
struct DdiRenderTargetView { ID3D11RenderTargetView *object; };
// `plane` is the PlaneSlice of the WDDM 2.0 create argument; see ddi-srv.h. Only a Texture2D view
// has a plane, and every other dimension takes 0.
HRESULT convert_rtv(const D3D10DDIARG_CREATERENDERTARGETVIEW &input,
    UINT resourceArraySize,UINT samples,UINT plane,D3D11_RENDER_TARGET_VIEW_DESC1 &output);
void create_render_target_view(D3D10DDI_HDEVICE device,const D3D10DDIARG_CREATERENDERTARGETVIEW *input,
    UINT plane,D3D10DDI_HRENDERTARGETVIEW view,D3D10DDI_HRTRENDERTARGETVIEW runtime);
void install_rtv_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
