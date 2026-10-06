// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-resource.h"
namespace bc250::umd {
struct DdiShaderResourceView { ID3D11ShaderResourceView *object; };
// `plane` is the PlaneSlice of the WDDM 2.0 create argument. A planar resource (NV12 and the other
// video formats) carries one plane per view, and the runtime names the plane next to the view format:
// plane 0 with R8_UNORM is the luma of an NV12 texture, plane 1 with R8G8_UNORM its chroma. Only a
// Texture2D view has a plane; every other dimension takes 0.
HRESULT convert_srv(const D3D11DDIARG_CREATESHADERRESOURCEVIEW &input,
    UINT layers,UINT samples,UINT plane,D3D11_SHADER_RESOURCE_VIEW_DESC1 &output);
// The same view without the plane field, for the engine entry that derives the plane from the view
// format. Every other member is the D3D11.1 member, field for field.
void demote_srv(const D3D11_SHADER_RESOURCE_VIEW_DESC1 &input,D3D11_SHADER_RESOURCE_VIEW_DESC &output);
// One create call, decided without an engine: which description the engine gets, and whether it
// derives the plane. `plane` is a plane slice, or ddi_plane_from_view_format when the runtime named
// none. This is where every plane decision lives, so the host test covers all of them.
struct SrvRequest {
    D3D11_SHADER_RESOURCE_VIEW_DESC1 desc1{};
    D3D11_SHADER_RESOURCE_VIEW_DESC legacy{};
    bool derive_plane=false;
};
HRESULT plan_srv(const D3D11DDIARG_CREATESHADERRESOURCEVIEW &input,
    UINT layers,UINT samples,UINT plane,SrvRequest &output);
void create_shader_resource_view(D3D10DDI_HDEVICE device,const D3D11DDIARG_CREATESHADERRESOURCEVIEW *input,
    UINT plane,D3D10DDI_HSHADERRESOURCEVIEW view,D3D10DDI_HRTSHADERRESOURCEVIEW runtime);
void install_srv_ddi(D3D11_1DDI_DEVICEFUNCS &table);
}
