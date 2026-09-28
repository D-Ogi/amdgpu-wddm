// SPDX-License-Identifier: MIT
#include "ddi-draw.h"
#include "ddi-input-layout.h"
#include "ddi-raster.h"
#include "ddi-shader.h"
#include "ddi-sampler.h"
#include "ddi-fixed-state.h"
#include <cstring>
#include <cstdlib>
#include <iostream>
using namespace bc250::umd;
namespace {
unsigned errors=0;
DeviceOwner *expected;
void APIENTRY error(D3D10DDI_HRTCORELAYER,HRESULT hr) {
    if (hr!=E_FAIL || !expected->runtime().domain.entered()) std::abort();
    ++errors;
}
}
int main() {
    DeviceOwner owner; expected=&owner; owner.runtime().UMCallbacks.pfnSetErrorCb=error;
    DdiDeviceHandle storage{&owner}; D3D10DDI_HDEVICE h{}; h.pDrvPrivate=&storage;
    D3D11_1DDI_DEVICEFUNCS table{};
    install_draw_ddi(table);
    install_input_layout_ddi(table);
    install_raster_ddi(table);
    install_shader_ddi(table);
    install_sampler_ddi(table);
    install_fixed_state_ddi(table);
    if (!table.pfnDraw || !table.pfnDispatch || table.pfnCreateResource) std::abort();
    // An uninitialized engine must report failure in the device domain, never
    // silently claim a successful draw or dereference a null COM context.
    table.pfnDraw(h,3,7);
    table.pfnDrawIndexed(h,6,2,-5);
    table.pfnDrawInstanced(h,3,2,7,11);
    table.pfnDrawIndexedInstanced(h,6,2,4,-3,9);
    table.pfnDrawAuto(h);
    table.pfnDispatch(h,2,3,4);
    if (errors!=6 || owner.runtime().domain.entered()) std::abort();
    DdiInputLayout layout{};
    D3D10DDI_HELEMENTLAYOUT lh{}; lh.pDrvPrivate=&layout;
    D3D10DDIARG_CREATEELEMENTLAYOUT desc{};
    if (table.pfnCalcPrivateElementLayoutSize(h,&desc)!=sizeof(layout)) std::abort();
    table.pfnCreateElementLayout(h,&desc,lh,{});
    table.pfnIaSetInputLayout(h,{});
    table.pfnDestroyElementLayout(h,lh);
    if (errors!=9 || layout.object || owner.runtime().domain.entered()) std::abort();
    Viewports viewports{};
    const D3D10_DDI_VIEWPORT viewport{-3.5f,2.25f,128.0f,64.0f,0.2f,0.8f};
    if (convert_viewports(1,15,&viewport,viewports)!=S_OK || viewports[0].TopLeftX!=-3.5f || viewports[0].MaxDepth!=0.8f) std::abort();
    if (convert_viewports(1,UINT_MAX,&viewport,viewports)!=E_INVALIDARG || viewports[0].TopLeftX!=-3.5f) std::abort();
    if (convert_viewports(0,16,nullptr,viewports)!=S_OK || viewports[0].Width!=0) std::abort();
    if (valid_topology(static_cast<D3D10_DDI_PRIMITIVE_TOPOLOGY>(6)) || !valid_topology(D3D11_DDI_PRIMITIVE_TOPOLOGY_32_CONTROL_POINT_PATCHLIST)) std::abort();
    table.pfnSetViewports(h,1,0,&viewport);
    table.pfnSetScissorRects(h,0,0,nullptr);
    table.pfnIaSetTopology(h,D3D10_DDI_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    if (errors!=12 || owner.runtime().domain.entered()) std::abort();
    D3D11_1DDIARG_SIGNATURE_ENTRY signature;
    std::memset(&signature,0xcc,sizeof(signature));
    signature.SystemValue=D3D10_SB_NAME_POSITION; signature.Register=7; signature.Mask=15;
    signature.RegisterComponentType=D3D10_SB_REGISTER_COMPONENT_FLOAT32;
    signature.MinPrecision=D3D11_SB_OPERAND_MIN_PRECISION_DEFAULT;
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> converted;
    if (copy_legacy_signature(&signature,1,converted)!=S_OK || converted[0].Stream!=0 || converted[0].Register!=7 || converted[0].Mask!=15) std::abort();
    if (copy_legacy_signature(nullptr,1,converted)!=E_INVALIDARG || converted.size()!=1) std::abort();
    DdiShader shader{}; D3D10DDI_HSHADER sh{}; sh.pDrvPrivate=&shader;
    D3D11_1DDIARG_STAGE_IO_SIGNATURES signatures{}; const UINT code[]={0,2};
    if (table.pfnCalcPrivateShaderSize(h,code,&signatures)!=sizeof(shader)) std::abort();
    table.pfnCreateVertexShader(h,code,sh,{},&signatures);
    table.pfnVsSetShader(h,sh); table.pfnPsSetShader(h,{}); table.pfnGsSetShader(h,{});
    table.pfnDestroyShader(h,sh);
    if (errors!=17 || shader.object || owner.runtime().domain.entered()) std::abort();
    D3D11_1DDIARG_TESSELLATION_IO_SIGNATURES tess{};
    if (table.pfnCalcPrivateTessellationShaderSize(h,code,&tess)!=sizeof(shader)) std::abort();
    table.pfnCreateComputeShader(h,code,sh,{});
    if (shader.stage!=ShaderStage::compute) std::abort();
    table.pfnCsSetShader(h,{});
    table.pfnCreateHullShader(h,code,sh,{},&tess);
    if (shader.stage!=ShaderStage::hull) std::abort();
    table.pfnHsSetShader(h,{});
    table.pfnCreateDomainShader(h,code,sh,{},&tess);
    if (shader.stage!=ShaderStage::domain) std::abort();
    table.pfnDsSetShader(h,{});
    if (errors!=23 || owner.runtime().domain.entered()) std::abort();
    D3D10_DDI_SAMPLER_DESC sampler{};
    sampler.Filter=D3D10_DDI_FILTER_COMPARISON_ANISOTROPIC;
    sampler.AddressU=D3D10_DDI_TEXTURE_ADDRESS_BORDER;
    sampler.MipLODBias=-0.5f; sampler.MaxAnisotropy=16; sampler.MinLOD=2; sampler.MaxLOD=8;
    sampler.BorderColor[3]=0.75f;
    const auto sample=convert_sampler(sampler);
    if (sample.Filter!=D3D11_FILTER_COMPARISON_ANISOTROPIC || sample.AddressU!=D3D11_TEXTURE_ADDRESS_BORDER || sample.MipLODBias!=-0.5f || sample.BorderColor[3]!=0.75f || sample.MaxLOD!=8) std::abort();
    DdiSampler ss{}; D3D10DDI_HSAMPLER shandle{}; shandle.pDrvPrivate=&ss;
    if (table.pfnCalcPrivateSamplerSize(h,&sampler)!=sizeof(ss)) std::abort();
    table.pfnCreateSampler(h,&sampler,shandle,{});
    table.pfnVsSetSamplers(h,0,1,&shandle); table.pfnPsSetSamplers(h,0,0,nullptr);
    table.pfnGsSetSamplers(h,0,0,nullptr); table.pfnCsSetSamplers(h,0,0,nullptr);
    table.pfnHsSetSamplers(h,0,0,nullptr); table.pfnDsSetSamplers(h,0,0,nullptr);
    table.pfnDestroySampler(h,shandle);
    if (errors!=31 || ss.object || owner.runtime().domain.entered()) std::abort();
    D3D10_DDI_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable=TRUE; depth.DepthWriteMask=D3D10_DDI_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc=D3D10_DDI_COMPARISON_GREATER;
    depth.StencilEnable=TRUE; depth.BackEnable=TRUE;
    depth.StencilReadMask=0x37; depth.StencilWriteMask=0xa5;
    // Disabled-face fields must not affect fragment acceptance or stencil writes.
    std::memset(&depth.FrontFace,0xcc,sizeof(depth.FrontFace));
    depth.BackFace={D3D10_DDI_STENCIL_OP_INCR_SAT,D3D10_DDI_STENCIL_OP_INVERT,
        D3D10_DDI_STENCIL_OP_REPLACE,D3D10_DDI_COMPARISON_NOT_EQUAL};
    auto ds=convert_depth_stencil(depth);
    if (ds.FrontFace.StencilFunc!=D3D11_COMPARISON_ALWAYS ||
        ds.FrontFace.StencilFailOp!=D3D11_STENCIL_OP_KEEP ||
        ds.FrontFace.StencilDepthFailOp!=D3D11_STENCIL_OP_KEEP ||
        ds.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_KEEP ||
        ds.BackFace.StencilFailOp!=D3D11_STENCIL_OP_INCR_SAT ||
        ds.BackFace.StencilDepthFailOp!=D3D11_STENCIL_OP_INVERT ||
        ds.BackFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE ||
        ds.BackFace.StencilFunc!=D3D11_COMPARISON_NOT_EQUAL ||
        ds.StencilReadMask!=0x37 || ds.StencilWriteMask!=0xa5 ||
        ds.DepthFunc!=D3D11_COMPARISON_GREATER) std::abort();
    depth.FrontEnable=TRUE; depth.FrontFace=depth.BackFace; depth.BackEnable=FALSE;
    ds=convert_depth_stencil(depth);
    if (ds.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_REPLACE || ds.BackFace.StencilFunc!=D3D11_COMPARISON_ALWAYS) std::abort();
    depth.StencilEnable=FALSE; ds=convert_depth_stencil(depth);
    if (ds.StencilEnable || ds.FrontFace.StencilPassOp!=D3D11_STENCIL_OP_KEEP) std::abort();
    D3D11_1_DDI_RASTERIZER_DESC raster{};
    raster.FillMode=D3D10_DDI_FILL_WIREFRAME; raster.CullMode=D3D10_DDI_CULL_FRONT;
    raster.FrontCounterClockwise=TRUE; raster.DepthBias=-71; raster.DepthBiasClamp=-0.25f;
    raster.SlopeScaledDepthBias=1.75f; raster.ScissorEnable=TRUE; raster.ForcedSampleCount=4;
    auto rs=convert_rasterizer(raster);
    if (rs.FillMode!=D3D11_FILL_WIREFRAME || rs.CullMode!=D3D11_CULL_FRONT ||
        !rs.FrontCounterClockwise || rs.DepthBias!=-71 || rs.DepthBiasClamp!=-0.25f ||
        rs.SlopeScaledDepthBias!=1.75f || !rs.ScissorEnable || rs.ForcedSampleCount!=4) std::abort();
    DdiDepthStencil dstate{}; D3D10DDI_HDEPTHSTENCILSTATE dh{}; dh.pDrvPrivate=&dstate;
    DdiRasterizer rstate{}; D3D10DDI_HRASTERIZERSTATE rh{}; rh.pDrvPrivate=&rstate;
    if (table.pfnCalcPrivateDepthStencilStateSize(h,&depth)!=sizeof(dstate) ||
        table.pfnCalcPrivateRasterizerStateSize(h,&raster)!=sizeof(rstate)) std::abort();
    table.pfnCreateDepthStencilState(h,&depth,dh,{});
    table.pfnSetDepthStencilState(h,{},0xa7); table.pfnDestroyDepthStencilState(h,dh);
    table.pfnCreateRasterizerState(h,&raster,rh,{});
    table.pfnSetRasterizerState(h,{}); table.pfnDestroyRasterizerState(h,rh);
    if (errors!=37 || dstate.object || rstate.object || owner.runtime().domain.entered()) std::abort();
    std::cout << "PASS draw DDI signatures and uninitialized-engine error/domain control (no rendering test)\n";
}
