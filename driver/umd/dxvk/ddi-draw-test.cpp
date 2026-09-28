// SPDX-License-Identifier: MIT
#include "ddi-draw.h"
#include "ddi-input-layout.h"
#include "ddi-raster.h"
#include "ddi-shader.h"
#include "ddi-sampler.h"
#include "ddi-fixed-state.h"
#include "ddi-blend.h"
#include "ddi-resource.h"
#include "ddi-buffer-binding.h"
#include "ddi-transfer.h"
#include "ddi-map.h"
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
    install_blend_ddi(table);
    install_resource_ddi(table);
    install_buffer_binding_ddi(table);
    install_transfer_ddi(table);
    install_map_ddi(table);
    if (!table.pfnDraw || !table.pfnDispatch || !table.pfnCreateResource) std::abort();
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
    D3D11_1_DDI_BLEND_DESC blend;
    std::memset(&blend,0xcc,sizeof(blend));
    blend.AlphaToCoverageEnable=TRUE; blend.IndependentBlendEnable=FALSE;
    blend.RenderTarget[0]={TRUE,FALSE,D3D10_DDI_BLEND_SRC_ALPHA,D3D10_DDI_BLEND_INV_SRC_ALPHA,
        D3D10_DDI_BLEND_OP_ADD,D3D10_DDI_BLEND_ONE,D3D10_DDI_BLEND_ZERO,
        D3D10_DDI_BLEND_OP_MAX,D3D11_1_DDI_LOGIC_OP_NOOP,0x5};
    D3D11_BLEND_DESC1 b{};
    if (convert_blend(blend,b)!=S_OK || !b.AlphaToCoverageEnable || b.IndependentBlendEnable) std::abort();
    // IndependentBlend=FALSE must ignore poisoned RT1..7 and replicate RT0.
    for (auto &rt:b.RenderTarget) if (rt.SrcBlend!=D3D11_BLEND_SRC_ALPHA ||
        rt.DestBlend!=D3D11_BLEND_INV_SRC_ALPHA || rt.BlendOpAlpha!=D3D11_BLEND_OP_MAX || rt.RenderTargetWriteMask!=5) std::abort();
    blend.IndependentBlendEnable=TRUE;
    for (auto &rt:blend.RenderTarget) rt=blend.RenderTarget[0];
    blend.RenderTarget[7].BlendEnable=FALSE; blend.RenderTarget[7].LogicOpEnable=TRUE;
    blend.RenderTarget[7].LogicOp=D3D11_1_DDI_LOGIC_OP_XOR; blend.RenderTarget[7].RenderTargetWriteMask=0xa;
    blend.RenderTarget[7].SrcBlend=static_cast<D3D10_DDI_BLEND>(0xcccccccc);
    if (convert_blend(blend,b)!=S_OK || b.RenderTarget[7].LogicOp!=D3D11_LOGIC_OP_XOR ||
        b.RenderTarget[7].RenderTargetWriteMask!=0xa || b.RenderTarget[0].RenderTargetWriteMask!=5) std::abort();
    blend.RenderTarget[7].BlendEnable=TRUE;
    if (convert_blend(blend,b)!=E_INVALIDARG || b.RenderTarget[7].BlendEnable) std::abort();
    blend.RenderTarget[7]=blend.RenderTarget[0];
    blend.RenderTarget[7].SrcBlend=D3D10_DDI_BLEND_ALPHA_FACTOR;
    if (convert_blend(blend,b)!=E_NOTIMPL) std::abort();
    DdiBlend bs{}; D3D10DDI_HBLENDSTATE bh{}; bh.pDrvPrivate=&bs;
    if (table.pfnCalcPrivateBlendStateSize(h,&blend)!=sizeof(bs)) std::abort();
    table.pfnCreateBlendState(h,&blend,bh,{});
    const FLOAT factors[4]={0.2f,0.4f,0.6f,0.8f};
    table.pfnSetBlendState(h,{},factors,0x55555555); table.pfnDestroyBlendState(h,bh);
    if (errors!=40 || bs.object || owner.runtime().domain.entered()) std::abort();
    D3D11DDIARG_CREATERESOURCE resource{};
    D3D10DDI_MIPINFO mips[3]={{8,4,2,16,8,4},{4,2,1,8,4,2},{2,1,1,4,2,1}};
    resource.pMipInfoList=mips; resource.ResourceDimension=D3D10DDIRESOURCE_TEXTURE3D;
    resource.MipLevels=3; resource.ArraySize=1; resource.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    resource.Usage=D3D10_DDI_USAGE_STAGING; resource.MapFlags=D3D10_DDI_CPU_ACCESS_READ;
    resource.TextureLayout=static_cast<D3DWDDM2_0DDI_TEXTURE_LAYOUT>(0xcccccccc);
    ResourceDescription rd;
    if (convert_resource(resource,rd)!=S_OK || rd.texture3d.Width!=8 || rd.texture3d.Height!=4 ||
        rd.texture3d.Depth!=2 || rd.texture3d.CPUAccessFlags!=D3D11_CPU_ACCESS_READ) std::abort();
    unsigned payload=7;
    D3D10_DDIARG_SUBRESOURCE_UP initial[6]{};
    for (UINT i=0;i<6;++i) initial[i]={&payload,32+i,128+i};
    resource.pInitialDataUP=initial; resource.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;
    resource.ArraySize=2; resource.SampleDesc={1,0};
    if (convert_resource(resource,rd)!=S_OK || rd.initial.size()!=6 || rd.initial[5].SysMemPitch!=37 ||
        rd.initial[3].SysMemSlicePitch!=131 || rd.texture2d.ArraySize!=2) std::abort();
    resource.MipLevels=1; resource.ArraySize=6; resource.ResourceDimension=D3D10DDIRESOURCE_TEXTURECUBE;
    mips[0].TexelHeight=8;
    if (convert_resource(resource,rd)!=S_OK || !(rd.texture2d.MiscFlags&D3D11_RESOURCE_MISC_TEXTURECUBE) ||
        rd.texture2d.ArraySize!=6 || rd.initial.size()!=6) std::abort();
    resource.ArraySize=5;
    if (convert_resource(resource,rd)!=E_INVALIDARG || rd.texture2d.ArraySize!=6) std::abort();
    resource.ResourceDimension=D3D11DDIRESOURCE_BUFFEREX; resource.ArraySize=1;
    resource.Usage=D3D10_DDI_USAGE_DEFAULT; resource.MapFlags=0;
    resource.BindFlags=D3D11_DDI_BIND_UNORDERED_ACCESS|D3D10_DDI_BIND_SHADER_RESOURCE;
    resource.MiscFlags=D3D11_DDI_RESOURCE_MISC_BUFFER_STRUCTURED; resource.ByteStride=4;
    if (convert_resource(resource,rd)!=S_OK || rd.buffer.BindFlags!=(D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE) ||
        rd.buffer.MiscFlags!=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED || rd.buffer.StructureByteStride!=4 ||
        rd.buffer.ByteWidth!=8 || rd.initial.size()!=1) std::abort();
    resource.BindFlags|=D3D10_DDI_BIND_PRESENT;
    if (convert_resource(resource,rd)!=E_NOTIMPL) std::abort();
    resource.BindFlags=0; resource.MiscFlags=D3D10_DDI_RESOURCE_MISC_SHARED;
    if (convert_resource(resource,rd)!=E_NOTIMPL) std::abort();
    resource.MiscFlags=0; resource.MipLevels=UINT_MAX;
    if (convert_resource(resource,rd)!=E_INVALIDARG) std::abort();
    DdiResource robj{}; D3D10DDI_HRESOURCE rhandle{}; rhandle.pDrvPrivate=&robj;
    if (table.pfnCalcPrivateResourceSize(h,&resource)!=sizeof(robj)) std::abort();
    table.pfnCreateResource(h,&resource,rhandle,{}); table.pfnDestroyResource(h,rhandle);
    if (errors!=42 || robj.object || owner.runtime().domain.entered()) std::abort();
    ID3D11Buffer *resolved=nullptr;
    if (resource_buffer({},resolved)!=S_OK || resolved) std::abort();
    if (resource_buffer(rhandle,resolved)!=E_INVALIDARG) std::abort();
    const UINT stride=20,offset=12,firstConstant=16,numConstants=32;
    table.pfnIaSetVertexBuffers(h,3,1,&rhandle,&stride,&offset);
    table.pfnIaSetIndexBuffer(h,{},DXGI_FORMAT_R16_UINT,6);
    table.pfnVsSetConstantBuffers(h,2,1,&rhandle,&firstConstant,&numConstants);
    table.pfnPsSetConstantBuffers(h,0,0,nullptr,nullptr,nullptr);
    table.pfnGsSetConstantBuffers(h,0,0,nullptr,nullptr,nullptr);
    table.pfnHsSetConstantBuffers(h,0,0,nullptr,nullptr,nullptr);
    table.pfnDsSetConstantBuffers(h,0,0,nullptr,nullptr,nullptr);
    table.pfnCsSetConstantBuffers(h,0,0,nullptr,nullptr,nullptr);
    if (errors!=50 || owner.runtime().domain.entered()) std::abort();
    D3D10_DDI_BOX box{4,5,1,24,13,3}; D3D11_BOX cb{}; bool empty=false; UINT cf=99;
    if (convert_copy_box(&box,cb,empty)!=S_OK || empty || cb.left!=4 || cb.back!=3 || cb.right!=24) std::abort();
    box.left=-1;
    if (convert_copy_box(&box,cb,empty)!=E_INVALIDARG || cb.left!=4) std::abort();
    box.left=box.right;
    if (convert_copy_box(&box,cb,empty)!=S_OK || !empty) std::abort();
    if (convert_copy_box(nullptr,cb,empty)!=S_OK || empty) std::abort();
    if (convert_copy_flags(D3D11_1DDI_COPY_DISCARD|D3D11_1DDI_COPY_TILEABLE,cf)!=S_OK || cf!=D3D11_COPY_DISCARD) std::abort();
    if (convert_copy_flags(D3D11_1DDI_COPY_NO_OVERWRITE,cf)!=S_OK || cf!=D3D11_COPY_NO_OVERWRITE) std::abort();
    if (convert_copy_flags(0x80000000,cf)!=E_INVALIDARG || cf!=D3D11_COPY_NO_OVERWRITE) std::abort();
    if (convert_copy_flags(D3D11_1DDI_COPY_DISCARD|D3D11_1DDI_COPY_NO_OVERWRITE,cf)!=E_INVALIDARG) std::abort();
    table.pfnResourceCopy(h,rhandle,rhandle);
    table.pfnResourceCopyRegion(h,rhandle,2,4,5,1,rhandle,1,&box,0);
    table.pfnResourceUpdateSubresourceUP(h,rhandle,1,nullptr,&payload,4,4,0);
    table.pfnResourceResolveSubresource(h,rhandle,0,rhandle,1,DXGI_FORMAT_R8G8B8A8_UNORM);
    if (errors!=54 || owner.runtime().domain.entered()) std::abort();
    D3D11_MAP mt=D3D11_MAP_READ; UINT mf=0;
    const D3D10_DDI_MAP types[]={D3D10_DDI_MAP_READ,D3D10_DDI_MAP_WRITE,D3D10_DDI_MAP_READWRITE,
        D3D10_DDI_MAP_WRITE_DISCARD,D3D10_DDI_MAP_WRITE_NOOVERWRITE};
    const D3D11_MAP expectedTypes[]={D3D11_MAP_READ,D3D11_MAP_WRITE,D3D11_MAP_READ_WRITE,
        D3D11_MAP_WRITE_DISCARD,D3D11_MAP_WRITE_NO_OVERWRITE};
    for (unsigned i=0;i<5;++i) {
        if (convert_map(types[i],0,mt,mf)!=S_OK || mt!=expectedTypes[i] || mf) std::abort();
    }
    if (convert_map(D3D10_DDI_MAP_READ,D3D10_DDI_MAP_FLAG_DONOTWAIT,mt,mf)!=S_OK || mf!=D3D11_MAP_FLAG_DO_NOT_WAIT) std::abort();
    if (convert_map(static_cast<D3D10_DDI_MAP>(0),0,mt,mf)!=E_INVALIDARG || mt!=D3D11_MAP_READ || mf!=D3D11_MAP_FLAG_DO_NOT_WAIT) std::abort();
    if (convert_map(D3D10_DDI_MAP_READ,0x80000000,mt,mf)!=E_INVALIDARG) std::abort();
    const PFND3D10DDI_RESOURCEMAP mapEntries[]={table.pfnResourceMap,table.pfnStagingResourceMap,
        table.pfnDynamicIABufferMapNoOverwrite,table.pfnDynamicIABufferMapDiscard,
        table.pfnDynamicConstantBufferMapDiscard,table.pfnDynamicResourceMapDiscard};
    for (auto call:mapEntries) {
        D3D10DDI_MAPPED_SUBRESOURCE mapped{&payload,111,222};
        call(h,rhandle,2,D3D10_DDI_MAP_READ,0,&mapped);
        if (mapped.pData || mapped.RowPitch || mapped.DepthPitch) std::abort();
    }
    table.pfnResourceUnmap(h,rhandle,2); table.pfnStagingResourceUnmap(h,rhandle,2);
    table.pfnDynamicIABufferUnmap(h,rhandle,0); table.pfnDynamicConstantBufferUnmap(h,rhandle,0);
    table.pfnDynamicResourceUnmap(h,rhandle,0);
    table.pfnDefaultConstantBufferUpdateSubresourceUP(h,rhandle,0,nullptr,&payload,4,4,0);
    if (errors!=66 || owner.runtime().domain.entered()) std::abort();
    std::cout << "PASS draw DDI signatures and uninitialized-engine error/domain control (no rendering test)\n";
}
