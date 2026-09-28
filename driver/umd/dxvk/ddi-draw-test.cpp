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
#include "ddi-rtv.h"
#include "ddi-dsv.h"
#include "ddi-uav.h"
#include "ddi-output.h"
#include "ddi-srv.h"
#include "ddi-flush.h"
#include "ddi-query.h"
#include "ddi-table.h"
#include "ddi-present.h"
#include "ddi-dxgi-resources.h"
#include "ddi-blt.h"
#include "ddi-clear-view.h"
#include "ddi-lifecycle.h"
#include "ddi-device-create.h"
#include "ddi-negotiation.h"
#include "ddi-dxgi-table.h"
#include "adapter-identity.h"
#include "ddi-adapter.h"
#include "ddi-format.h"
#include "ddi-resource-status.h"
#include <cstring>
#include <d3d9.h>
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
HRESULT APIENTRY fail_retire(HANDLE,const D3DDDICB_DESTROYCONTEXT *) { return E_FAIL; }
HRESULT APIENTRY pass_retire(HANDLE,const D3DDDICB_DESTROYCONTEXT *) { return S_OK; }
namespace {
int createIdentity,createContext; unsigned createCount=0,destroyCount=0;
unsigned adapterMode=0;
HRESULT APIENTRY adapter_query(HANDLE h,const D3DDDICB_QUERYADAPTERINFO *q) {
    if(h!=&createIdentity || q->PrivateDriverDataSize!=BC250_ADAPTER_CAPS_BYTES) std::abort();
    auto *bytes=static_cast<unsigned char *>(q->pPrivateDriverData);
    for(unsigned i=0;i<BC250_ADAPTER_CAPS_BYTES;++i) if(bytes[i]) std::abort();
    if(adapterMode==1) return E_OUTOFMEMORY;
    if(adapterMode==2) return S_OK; // Old KMD leaves trailer unwritten.
    bc250_adapter_identity identity{BC250_ADAPTER_IDENTITY_MAGIC,1,24,0x12345678u,0xffffff85u,0};
    if(adapterMode==3) identity.magic=0;
    if(adapterMode==4) identity.version=2;
    if(adapterMode==5) identity.size=20;
    if(adapterMode==6) identity.reserved=1;
    std::memcpy(bytes+BC250_ADAPTER_IDENTITY_OFFSET,&identity,sizeof(identity));
    return S_OK;
}

bool rejectContext=false,rejectCleanup=false;
HRESULT APIENTRY creation_context(HANDLE h,D3DDDICB_CREATECONTEXTVIRTUAL *c) {
    if (h!=&createIdentity) std::abort(); ++createCount;
    if (rejectContext) return E_OUTOFMEMORY;
    c->hContext=&createContext; return S_OK;
}
HRESULT APIENTRY creation_destroy(HANDLE,const D3DDDICB_DESTROYCONTEXT *) { ++destroyCount; return rejectCleanup ? E_FAIL : S_OK; }
HRESULT APIENTRY creation_sync(HANDLE,const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *) { std::abort(); }
void APIENTRY unexpected_creation_error(D3D10DDI_HRTCORELAYER,HRESULT) { std::abort(); }
PFN_vkVoidFunction VKAPI_CALL no_instance(VkInstance,const char *) { return nullptr; }
HRESULT APIENTRY no_engine(const BC250_DXVK_DEVICE_CREATE_INFO *,IBc250DxvkDevice **) { std::abort(); }
}
namespace {
unsigned priorityCalls=0,residencyCalls=0; int residencyMode=0;
unsigned offerCalls=0,reclaimCalls=0,submitCalls=0;
HRESULT residencyChangeResult=S_OK;
HRESULT APIENTRY runtime_offer(HANDLE h,const D3DDDICB_OFFERALLOCATIONS *p) {
    if(h!=&createIdentity || !expected->runtime().domain.entered() || !submitCalls || p->pResources ||
       p->NumAllocations!=3 || p->Priority!=D3DDDI_OFFER_PRIORITY_NORMAL) std::abort();
    for(UINT i=0;i<3;++i) if(p->HandleList[i]!=61+i) std::abort();
    ++offerCalls; return residencyChangeResult;
}
HRESULT APIENTRY runtime_reclaim(HANDLE h,const D3DDDICB_RECLAIMALLOCATIONS *p) {
    if(h!=&createIdentity || !expected->runtime().domain.entered() || p->pResources ||
       p->NumAllocations!=3 || !p->pDiscarded) std::abort();
    for(UINT i=0;i<3;++i) { if(p->HandleList[i]!=61+i) std::abort(); p->pDiscarded[i]=(i==1); }
    ++reclaimCalls; return residencyChangeResult;
}
unsigned displayModeCalls=0; HRESULT displayModeResult=S_OK;
HRESULT APIENTRY runtime_display_mode(HANDLE h,D3DDDICB_SETDISPLAYMODE *p) {
    if (h!=&createIdentity || !expected->runtime().domain.entered() ||
        p->hPrimaryAllocation!=61 || p->PrivateDriverFormatAttribute) std::abort();
    ++displayModeCalls; return displayModeResult;
}

HRESULT APIENTRY runtime_priority(HANDLE h,D3DDDICB_SETPRIORITY *p) {
    if (h!=&createIdentity || !expected->runtime().domain.entered() || p->hResource || p->NumAllocations!=1 ||
        *p->HandleList!=61 || *p->pPriorities!=7) std::abort();
    ++priorityCalls; return S_OK;
}
HRESULT APIENTRY runtime_residency(HANDLE h,const D3DDDICB_QUERYRESIDENCY *p) {
    if (h!=&createIdentity || !expected->runtime().domain.entered() || p->hResource || p->NumAllocations!=3) std::abort();
    ++residencyCalls;
    for (UINT i=0;i<3;++i) {
        if (p->HandleList[i]!=61+i) std::abort();
        p->pResidencyStatus[i]=static_cast<D3DDDI_RESIDENCYSTATUS>(i+1);
    }
    if (residencyMode>=3) {
        for (UINT i=0;i<3;++i) p->pResidencyStatus[i]=D3DDDI_RESIDENCYSTATUS_RESIDENTINGPUMEMORY;
        if (residencyMode==4) p->pResidencyStatus[1]=D3DDDI_RESIDENCYSTATUS_RESIDENTINSHAREDMEMORY;
    }
    if (residencyMode==2) p->pResidencyStatus[1]=static_cast<D3DDDI_RESIDENCYSTATUS>(42);
    return residencyMode==1 ? E_FAIL : S_OK;
}
}
int main() {
    DeviceOwner owner; expected=&owner; owner.runtime().UMCallbacks.pfnSetErrorCb=error;
    DdiDeviceHandle storage{&owner}; D3D10DDI_HDEVICE h{}; h.pDrvPrivate=&storage;
    if (ddi_map_status(DXGI_ERROR_WAS_STILL_DRAWING,true)!=DXGI_DDI_ERR_WASSTILLDRAWING ||
        ddi_map_status(DXGI_ERROR_WAS_STILL_DRAWING,false)!=DXGI_ERROR_WAS_STILL_DRAWING ||
        ddi_map_status(E_OUTOFMEMORY,true)!=E_OUTOFMEMORY || ddi_map_status(S_OK,false)!=S_OK) std::abort();
    for (HRESULT loss:{DXGI_ERROR_DEVICE_REMOVED,DXGI_ERROR_DEVICE_RESET,DXGI_ERROR_DEVICE_HUNG,DXGI_ERROR_DRIVER_INTERNAL_ERROR}) {
        if (ddi_device_status(loss)!=D3DDDIERR_DEVICEREMOVED || ddi_map_status(loss,false)!=D3DDDIERR_DEVICEREMOVED ||
            query_ddi_status(loss)!=D3DDDIERR_DEVICEREMOVED) std::abort();
    }
    if (ddi_device_status(E_INVALIDARG)!=E_INVALIDARG || ddi_device_status(E_FAIL)!=E_FAIL) std::abort();
    if (!format_allows_not_supported(DXGI_FORMAT_Y410) || !format_allows_not_supported(DXGI_FORMAT_AYUV) ||
        format_allows_not_supported(DXGI_FORMAT_R1_UNORM) || format_allows_not_supported(DXGI_FORMAT_R8G8B8A8_UNORM) ||
        format_allows_not_supported(DXGI_FORMAT_FORCE_UINT)) std::abort();
    auto table=make_render_device_table();
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
    D3D10DDIARG_CREATERENDERTARGETVIEW rv{}; D3D11_RENDER_TARGET_VIEW_DESC rvout{};
    rv.Format=DXGI_FORMAT_R8G8B8A8_UNORM; rv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;
    rv.Tex2D={2,3,2};
    if (convert_rtv(rv,8,1,rvout)!=S_OK || rvout.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2DARRAY ||
        rvout.Texture2DArray.MipSlice!=2 || rvout.Texture2DArray.FirstArraySlice!=3 || rvout.Texture2DArray.ArraySize!=2) std::abort();
    if (convert_rtv(rv,8,4,rvout)!=E_INVALIDARG) std::abort();
    rv.Tex2D.MipSlice=0;
    if (convert_rtv(rv,8,4,rvout)!=S_OK || rvout.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY ||
        rvout.Texture2DMSArray.FirstArraySlice!=3 || rvout.Texture2DMSArray.ArraySize!=2) std::abort();
    rv.Tex2D={0,0,1};
    if (convert_rtv(rv,1,4,rvout)!=S_OK || rvout.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2DMS) std::abort();
    rv.ResourceDimension=D3D10DDIRESOURCE_TEXTURECUBE; rv.TexCube={1,6,6};
    if (convert_rtv(rv,12,1,rvout)!=S_OK || rvout.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE2DARRAY ||
        rvout.Texture2DArray.FirstArraySlice!=6 || rvout.Texture2DArray.ArraySize!=6) std::abort();
    rv.TexCube.FirstArraySlice=UINT_MAX;
    if (convert_rtv(rv,12,1,rvout)!=E_INVALIDARG || rvout.Texture2DArray.FirstArraySlice!=6) std::abort();
    rv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE3D; rv.Tex3D={2,3,4};
    if (convert_rtv(rv,1,1,rvout)!=S_OK || rvout.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE3D ||
        rvout.Texture3D.FirstWSlice!=3 || rvout.Texture3D.WSize!=4) std::abort();
    rv.ResourceDimension=D3D10DDIRESOURCE_BUFFER; rv.Buffer={7,11};
    if (convert_rtv(rv,1,1,rvout)!=S_OK || rvout.Buffer.FirstElement!=7 || rvout.Buffer.NumElements!=11) std::abort();
    rv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE1D; rv.Tex1D={3,0,1};
    if (convert_rtv(rv,1,1,rvout)!=S_OK || rvout.ViewDimension!=D3D11_RTV_DIMENSION_TEXTURE1D || rvout.Texture1D.MipSlice!=3) std::abort();
    DdiRenderTargetView rt{}; D3D10DDI_HRENDERTARGETVIEW rth{}; rth.pDrvPrivate=&rt;
    if (table.pfnCalcPrivateRenderTargetViewSize(h,&rv)!=sizeof(rt)) std::abort();
    table.pfnCreateRenderTargetView(h,&rv,rth,{});
    FLOAT clearColor[4]={0.1f,0.2f,0.3f,0.4f};
    table.pfnClearRenderTargetView(h,rth,clearColor); table.pfnDestroyRenderTargetView(h,rth);
    if (errors!=69 || rt.object || owner.runtime().domain.entered()) std::abort();
    D3D11DDIARG_CREATEDEPTHSTENCILVIEW dv{}; D3D11_DEPTH_STENCIL_VIEW_DESC dvout{};
    dv.Format=DXGI_FORMAT_D24_UNORM_S8_UINT; dv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D;
    dv.Tex2D={0,2,3}; dv.Flags=D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH|D3D11_DDI_CREATE_DSV_READ_ONLY_STENCIL;
    if (convert_dsv(dv,8,4,dvout)!=S_OK || dvout.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY ||
        dvout.Flags!=(D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL) ||
        dvout.Texture2DMSArray.FirstArraySlice!=2 || dvout.Texture2DMSArray.ArraySize!=3) std::abort();
    dv.Flags=D3D11_DDI_CREATE_DSV_READ_ONLY_STENCIL; dv.Tex2D={2,0,1};
    if (convert_dsv(dv,1,1,dvout)!=S_OK || dvout.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2D ||
        dvout.Flags!=D3D11_DSV_READ_ONLY_STENCIL || dvout.Texture2D.MipSlice!=2) std::abort();
    if (convert_dsv(dv,1,4,dvout)!=E_INVALIDARG || dvout.Texture2D.MipSlice!=2) std::abort();
    dv.Flags=0x80000000;
    if (convert_dsv(dv,1,1,dvout)!=E_INVALIDARG) std::abort();
    dv.Flags=D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH; dv.ResourceDimension=D3D10DDIRESOURCE_TEXTURECUBE; dv.TexCube={1,6,6};
    if (convert_dsv(dv,12,1,dvout)!=S_OK || dvout.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE2DARRAY ||
        dvout.Texture2DArray.FirstArraySlice!=6 || dvout.Flags!=D3D11_DSV_READ_ONLY_DEPTH) std::abort();
    dv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE1D; dv.Tex1D={1,3,2};
    if (convert_dsv(dv,8,1,dvout)!=S_OK || dvout.ViewDimension!=D3D11_DSV_DIMENSION_TEXTURE1DARRAY ||
        dvout.Texture1DArray.MipSlice!=1 || dvout.Texture1DArray.ArraySize!=2) std::abort();
    dv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE3D;
    if (convert_dsv(dv,1,1,dvout)!=E_INVALIDARG) std::abort();
    DdiDepthStencilView depthView{}; D3D10DDI_HDEPTHSTENCILVIEW dvh{}; dvh.pDrvPrivate=&depthView;
    if (table.pfnCalcPrivateDepthStencilViewSize(h,&dv)!=sizeof(depthView)) std::abort();
    table.pfnCreateDepthStencilView(h,&dv,dvh,{});
    table.pfnClearDepthStencilView(h,dvh,D3D10_DDI_CLEAR_STENCIL,0.25f,0xa7);
    table.pfnDestroyDepthStencilView(h,dvh);
    if (errors!=72 || depthView.object || owner.runtime().domain.entered()) std::abort();
    D3D11DDIARG_CREATEUNORDEREDACCESSVIEW uv{}; D3D11_UNORDERED_ACCESS_VIEW_DESC uvout{};
    uv.ResourceDimension=D3D11DDIRESOURCE_BUFFEREX; uv.Format=DXGI_FORMAT_UNKNOWN;
    uv.Buffer={5,17,D3D11_DDI_BUFFER_UAV_FLAG_COUNTER};
    if (convert_uav(uv,1,1,uvout)!=S_OK || uvout.Buffer.FirstElement!=5 || uvout.Buffer.NumElements!=17 || uvout.Buffer.Flags!=D3D11_BUFFER_UAV_FLAG_COUNTER) std::abort();
    uv.Buffer.Flags=D3D11_DDI_BUFFER_UAV_FLAG_APPEND;
    if (convert_uav(uv,1,1,uvout)!=S_OK || uvout.Buffer.Flags!=D3D11_BUFFER_UAV_FLAG_APPEND) std::abort();
    uv.Buffer.Flags|=D3D11_DDI_BUFFER_UAV_FLAG_RAW;
    if (convert_uav(uv,1,1,uvout)!=E_INVALIDARG || uvout.Buffer.Flags!=D3D11_BUFFER_UAV_FLAG_APPEND) std::abort();
    uv.Buffer.Flags=D3D11_DDI_BUFFER_UAV_FLAG_RAW;
    if (convert_uav(uv,1,1,uvout)!=S_OK || uvout.Buffer.Flags!=D3D11_BUFFER_UAV_FLAG_RAW) std::abort();
    uv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; uv.Format=DXGI_FORMAT_R32_UINT; uv.Tex2D={2,3,2};
    if (convert_uav(uv,8,1,uvout)!=S_OK || uvout.ViewDimension!=D3D11_UAV_DIMENSION_TEXTURE2DARRAY ||
        uvout.Texture2DArray.MipSlice!=2 || uvout.Texture2DArray.FirstArraySlice!=3 || uvout.Texture2DArray.ArraySize!=2) std::abort();
    if (convert_uav(uv,8,4,uvout)!=E_INVALIDARG) std::abort();
    uv.Tex2D.FirstArraySlice=UINT_MAX;
    if (convert_uav(uv,8,1,uvout)!=E_INVALIDARG) std::abort();
    uv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE3D; uv.Tex3D={1,2,7};
    if (convert_uav(uv,1,1,uvout)!=S_OK || uvout.Texture3D.FirstWSlice!=2 || uvout.Texture3D.WSize!=7) std::abort();
    uv.ResourceDimension=D3D10DDIRESOURCE_TEXTURECUBE;
    if (convert_uav(uv,6,1,uvout)!=E_INVALIDARG) std::abort();
    DdiUnorderedAccessView uview{}; D3D11DDI_HUNORDEREDACCESSVIEW uvh{}; uvh.pDrvPrivate=&uview;
    if (table.pfnCalcPrivateUnorderedAccessViewSize(h,&uv)!=sizeof(uview)) std::abort();
    table.pfnCreateUnorderedAccessView(h,&uv,uvh,{});
    const UINT uintClear[4]={1,2,3,4},keepCounter=UINT_MAX;
    table.pfnClearUnorderedAccessViewUint(h,uvh,uintClear);
    table.pfnClearUnorderedAccessViewFloat(h,uvh,clearColor);
    table.pfnCsSetUnorderedAccessViews(h,3,1,&uvh,&keepCounter);
    table.pfnCopyStructureCount(h,rhandle,12,uvh);
    table.pfnDestroyUnorderedAccessView(h,uvh);
    if (errors!=78 || uview.object || owner.runtime().domain.entered()) std::abort();
    OutputBindings ob;
    // Opaque interface identity tokens: preparation must not call COM or take
    // ownership. Only the later engine setter can retain the actual objects.
    int identity[3]{};
    DdiRenderTargetView ort{reinterpret_cast<ID3D11RenderTargetView *>(&identity[0])};
    DdiDepthStencilView ods{reinterpret_cast<ID3D11DepthStencilView *>(&identity[1])};
    DdiUnorderedAccessView ouv{reinterpret_cast<ID3D11UnorderedAccessView *>(&identity[2])};
    D3D10DDI_HRENDERTARGETVIEW orts[2]{}; orts[0].pDrvPrivate=&ort;
    D3D10DDI_HDEPTHSTENCILVIEW od{}; od.pDrvPrivate=&ods;
    D3D11DDI_HUNORDEREDACCESSVIEW ous[2]{}; ous[0].pDrvPrivate=&ouv;
    const UINT counts[2]={17,UINT_MAX};
    if (prepare_output_bindings(orts,2,6,od,ous,counts,5,2,5,1,64,ob)!=S_OK ||
        ob.rtvs[0]!=ort.object || ob.rtvs[1] || ob.dsv!=ods.object || ob.uavFirst!=2 || ob.uavCount!=62 ||
        ob.uavs[3]!=ouv.object || ob.counters[3]!=17 || ob.counters[4]!=UINT_MAX) std::abort();
    for (UINT i=0;i<ob.uavCount;++i) if (i!=3 && ob.uavs[i]) std::abort();
    // All old UAV bindings must be removed on a new RTV-only call.
    if (prepare_output_bindings(orts,2,0,{},nullptr,nullptr,0,0,0,0,8,ob)!=S_OK || ob.uavCount!=6 || ob.dsv) std::abort();
    for (UINT i=0;i<ob.uavCount;++i) if (ob.uavs[i] || ob.counters[i]!=UINT_MAX) std::abort();
    if (prepare_output_bindings(orts,2,UINT_MAX,{},nullptr,nullptr,0,0,0,0,8,ob)!=E_INVALIDARG || ob.uavCount!=6) std::abort();
    if (prepare_output_bindings(orts,2,0,{},ous,counts,1,2,0,0,8,ob)!=E_INVALIDARG) std::abort();
    if (prepare_output_bindings(nullptr,0,0,{},ous,counts,63,2,0,0,64,ob)!=E_INVALIDARG) std::abort();
    if (prepare_output_bindings(nullptr,0,0,{},ous,nullptr,63,1,63,1,64,ob)!=S_OK || ob.uavs[63]!=ouv.object || ob.counters[63]!=UINT_MAX) std::abort();
    table.pfnSetRenderTargets(h,orts,2,6,od,ous,counts,5,2,5,1);
    if (errors!=79 || owner.runtime().domain.entered()) std::abort();
    D3D11DDIARG_CREATESHADERRESOURCEVIEW sv{}; D3D11_SHADER_RESOURCE_VIEW_DESC svout{};
    sv.ResourceDimension=D3D10DDIRESOURCE_TEXTURECUBE; sv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.TexCube={2,3,6,2};
    if (convert_srv(sv,24,1,svout)!=S_OK || svout.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY ||
        svout.TextureCubeArray.First2DArrayFace!=6 || svout.TextureCubeArray.NumCubes!=2 || svout.TextureCubeArray.MipLevels!=3) std::abort();
    sv.TexCube.NumCubes=UINT_MAX;
    if (convert_srv(sv,24,1,svout)!=E_INVALIDARG || svout.TextureCubeArray.NumCubes!=2) std::abort();
    sv.TexCube={0,1,1,2};
    if (convert_srv(sv,24,1,svout)!=S_OK || svout.TextureCubeArray.First2DArrayFace!=1 || svout.TextureCubeArray.NumCubes!=2) std::abort();
    sv.TexCube={0,1,19,1};
    if (convert_srv(sv,24,1,svout)!=E_INVALIDARG) std::abort();
    sv.TexCube={1,UINT_MAX,0,1};
    if (convert_srv(sv,6,1,svout)!=S_OK || svout.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURECUBE || svout.TextureCube.MipLevels!=UINT_MAX) std::abort();
    sv.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; sv.Tex2D={0,2,1,3};
    if (convert_srv(sv,8,4,svout)!=S_OK || svout.ViewDimension!=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY ||
        svout.Texture2DMSArray.FirstArraySlice!=2 || svout.Texture2DMSArray.ArraySize!=3) std::abort();
    sv.Tex2D={2,1,4,3};
    if (convert_srv(sv,8,1,svout)!=S_OK || svout.Texture2DArray.MostDetailedMip!=2 || svout.Texture2DArray.MipLevels!=4) std::abort();
    if (convert_srv(sv,8,4,svout)!=E_INVALIDARG) std::abort();
    sv.ResourceDimension=D3D11DDIRESOURCE_BUFFEREX; sv.BufferEx={7,19,D3D11_DDI_BUFFEREX_SRV_FLAG_RAW};
    if (convert_srv(sv,1,1,svout)!=S_OK || svout.BufferEx.FirstElement!=7 || svout.BufferEx.NumElements!=19 || svout.BufferEx.Flags!=D3D11_BUFFEREX_SRV_FLAG_RAW) std::abort();
    sv.BufferEx.Flags=0x80000000;
    if (convert_srv(sv,1,1,svout)!=E_INVALIDARG) std::abort();
    DdiShaderResourceView sview{}; D3D10DDI_HSHADERRESOURCEVIEW svh{}; svh.pDrvPrivate=&sview;
    if (table.pfnCalcPrivateShaderResourceViewSize(h,&sv)!=sizeof(sview)) std::abort();
    table.pfnCreateShaderResourceView(h,&sv,svh,{});
    table.pfnVsSetShaderResources(h,127,1,&svh); table.pfnPsSetShaderResources(h,0,0,nullptr);
    table.pfnGsSetShaderResources(h,0,0,nullptr); table.pfnHsSetShaderResources(h,0,0,nullptr);
    table.pfnDsSetShaderResources(h,0,0,nullptr); table.pfnCsSetShaderResources(h,0,0,nullptr);
    table.pfnGenMips(h,svh); table.pfnDestroyShaderResourceView(h,svh);
    if (errors!=88 || sview.object || owner.runtime().domain.entered()) std::abort();
    D3D11_BUFFER_DESC indirectDesc{}; indirectDesc.ByteWidth=64;
    indirectDesc.MiscFlags=D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    if (!valid_indirect_arguments(indirectDesc,44,sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)) ||
        valid_indirect_arguments(indirectDesc,48,sizeof(D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS)) ||
        valid_indirect_arguments(indirectDesc,3,12) || valid_indirect_arguments(indirectDesc,UINT_MAX,12) ||
        valid_indirect_arguments(indirectDesc,0,0)) std::abort();
    indirectDesc.MiscFlags=0;
    if (valid_indirect_arguments(indirectDesc,0,12)) std::abort();
    table.pfnDrawInstancedIndirect(h,rhandle,16);
    table.pfnDrawIndexedInstancedIndirect(h,rhandle,20);
    table.pfnDispatchIndirect(h,rhandle,12);
    if (errors!=91 || owner.runtime().domain.entered()) std::abort();
    if (table.pfnFlush(h,0) || table.pfnFlush(h,D3D11_1DDI_FLUSH_UNLESS_NO_COMMANDS)) std::abort();
    if (errors!=93 || owner.runtime().domain.entered()) std::abort();
    if (table.pfnFlush({},0)) std::abort();
    D3D10DDI_MAPPED_SUBRESOURCE noOverwrite{&payload,11,22};
    table.pfnDynamicConstantBufferMapNoOverwrite(h,rhandle,0,D3D10_DDI_MAP_WRITE_NOOVERWRITE,0,&noOverwrite);
    if (errors!=94 || noOverwrite.pData || noOverwrite.RowPitch || noOverwrite.DepthPitch || owner.runtime().domain.entered()) std::abort();
    if (convert_format_support(D3D11_FORMAT_SUPPORT_BLENDABLE,0)) std::abort();
    const UINT fs=convert_format_support(D3D11_FORMAT_SUPPORT_RENDER_TARGET|D3D11_FORMAT_SUPPORT_BLENDABLE|
        D3D11_FORMAT_SUPPORT_SHADER_SAMPLE|D3D11_FORMAT_SUPPORT_IA_VERTEX_BUFFER|D3D11_FORMAT_SUPPORT_BUFFER,
        D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE|D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP);
    const UINT expectedSupport=D3D10_DDI_FORMAT_SUPPORT_RENDERTARGET|D3D10_DDI_FORMAT_SUPPORT_BLENDABLE|
        D3D10_DDI_FORMAT_SUPPORT_SHADER_SAMPLE|D3D11_1DDI_FORMAT_SUPPORT_VERTEX_BUFFER|
        D3D11_1DDI_FORMAT_SUPPORT_BUFFER|D3D11_1DDI_FORMAT_SUPPORT_UAV_WRITES|D3D11_1DDI_FORMAT_SUPPORT_OUTPUT_MERGER_LOGIC_OP;
    if (fs!=expectedSupport || convert_format_support(0,0)) std::abort();
    // No optional display/video support merely because the engine reports it.
    if (convert_format_support(D3D11_FORMAT_SUPPORT_DISPLAY,0)) std::abort();
    UINT supportOutput=UINT_MAX;
    table.pfnCheckFormatSupport(h,DXGI_FORMAT_R8G8B8A8_UNORM,&supportOutput);
    if (supportOutput) std::abort();
    supportOutput=UINT_MAX;
    table.pfnCheckMultisampleQualityLevels(h,DXGI_FORMAT_R8G8B8A8_UNORM,4,&supportOutput);
    if (supportOutput || errors!=96 || owner.runtime().domain.entered()) std::abort();
    D3D11_BUFFER_DESC soDesc{}; soDesc.ByteWidth=128; soDesc.BindFlags=D3D11_BIND_STREAM_OUTPUT;
    if (!valid_stream_output_buffer(soDesc,UINT_MAX) || !valid_stream_output_buffer(soDesc,0) ||
        !valid_stream_output_buffer(soDesc,128) || valid_stream_output_buffer(soDesc,132) ||
        valid_stream_output_buffer(soDesc,3)) std::abort();
    soDesc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
    if (valid_stream_output_buffer(soDesc,UINT_MAX)) std::abort();
    const UINT append=UINT_MAX;
    table.pfnSoSetTargets(h,1,3,&rhandle,&append);
    table.pfnSoSetTargets(h,0,4,nullptr,nullptr);
    if (errors!=98 || owner.runtime().domain.entered()) std::abort();
    UINT queried=0;
    if (query_subresources_idle(6,[&](UINT i) { ++queried; return i==5 ? S_FALSE : S_OK; })!=S_FALSE || queried!=6) std::abort();
    queried=0;
    if (query_subresources_idle(6,[&](UINT i) { ++queried; return i==2 ? E_FAIL : S_OK; })!=E_FAIL || queried!=3) std::abort();
    queried=0;
    if (query_subresources_idle(6,[&](UINT) { ++queried; return S_OK; })!=S_OK || queried!=6) std::abort();
    if (query_subresources_idle(0,[](UINT) { return S_OK; })!=E_INVALIDARG) std::abort();
    if (!table.pfnResourceIsStagingBusy(h,rhandle) || !table.pfnResourceIsStagingBusy({},{})) std::abort();
    table.pfnSetResourceMinLOD(h,rhandle,1.5f);
    if (errors!=100 || owner.runtime().domain.entered()) std::abort();
    DdiDeviceHandle retiring{new DeviceOwner};
    DeviceOwner *retained=retiring.owner; expected=retained;
    retained->runtime().UMCallbacks.pfnSetErrorCb=error;
    retained->runtime().present_context=&payload;
    retained->runtime().KTCallbacks.pfnDestroyContextCb=fail_retire;
    if (retire_device_handle(retiring)!=E_FAIL || retiring.owner!=retained ||
        !retained->has_live_objects() || retained->runtime().domain.entered() || errors!=101) std::abort();
    retained->runtime().KTCallbacks.pfnDestroyContextCb=pass_retire;
    if (retire_device_handle(retiring)!=S_OK || retiring.owner || retire_device_handle(retiring)!=S_OK) std::abort();
    DdiDeviceHandle emptyDevice{new DeviceOwner}; D3D10DDI_HDEVICE destroyHandle{}; destroyHandle.pDrvPrivate=&emptyDevice;
    table.pfnDestroyDevice(destroyHandle);
    if (emptyDevice.owner) std::abort();
    table.pfnDestroyDevice(destroyHandle); table.pfnDestroyDevice({});
    expected=&owner;
    D3D11_QUERY_DESC queryDesc{}; bool predicate=false;
    D3D10DDIARG_CREATEQUERY queryArgs{D3D10DDI_QUERY_EVENT,0};
    if (convert_query(queryArgs,queryDesc,predicate)!=S_OK || queryDesc.Query!=D3D11_QUERY_EVENT || predicate) std::abort();
    queryArgs.Query=D3D11DDI_QUERY_PIPELINESTATS;
    if (convert_query(queryArgs,queryDesc,predicate)!=S_OK || queryDesc.Query!=D3D11_QUERY_PIPELINE_STATISTICS) std::abort();
    queryArgs.Query=D3D10DDI_QUERY_PIPELINESTATS;
    if (convert_query(queryArgs,queryDesc,predicate)!=S_OK || queryDesc.Query!=D3D11_QUERY_PIPELINE_STATISTICS) std::abort();
    queryArgs.Query=D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3;
    if (convert_query(queryArgs,queryDesc,predicate)!=S_OK || !predicate || queryDesc.Query!=D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3) std::abort();
    queryArgs.Query=D3D10DDI_QUERY_OCCLUSIONPREDICATE; queryArgs.MiscFlags=D3D10DDI_QUERY_MISCFLAG_PREDICATEHINT;
    if (convert_query(queryArgs,queryDesc,predicate)!=S_OK || !predicate || queryDesc.MiscFlags!=D3D11_QUERY_MISC_PREDICATEHINT) std::abort();
    queryArgs.Query=D3D10DDI_QUERY_TIMESTAMP;
    if (convert_query(queryArgs,queryDesc,predicate)!=E_INVALIDARG) std::abort();
    queryArgs.MiscFlags=2;
    if (convert_query(queryArgs,queryDesc,predicate)!=E_INVALIDARG) std::abort();
    queryArgs.Query=D3D10DDI_COUNTER_GPU_IDLE; queryArgs.MiscFlags=0;
    if (convert_query(queryArgs,queryDesc,predicate)!=E_NOTIMPL) std::abort();
    if (query_ddi_status(S_FALSE)!=DXGI_DDI_ERR_WASSTILLDRAWING || query_ddi_status(S_OK)!=S_OK || query_ddi_status(E_FAIL)!=E_FAIL) std::abort();
    DdiQuery queryStorage{}; D3D10DDI_HQUERY queryHandle{}; queryHandle.pDrvPrivate=&queryStorage;
    if (table.pfnCalcPrivateQuerySize(h,&queryArgs)!=sizeof(DdiQuery)) std::abort();
    table.pfnCreateQuery(h,&queryArgs,queryHandle,{});
    table.pfnQueryBegin(h,queryHandle); table.pfnQueryEnd(h,queryHandle);
    UINT queryResult=0x12345678;
    auto pendingRead=[](void *p,UINT) { *static_cast<UINT *>(p)=0; return S_FALSE; };
    if (read_query_result(&queryResult,sizeof(queryResult),pendingRead)!=S_FALSE || queryResult!=0x12345678) std::abort();
    auto readyRead=[](void *p,UINT) { *static_cast<UINT *>(p)=42; return S_OK; };
    if (read_query_result(&queryResult,sizeof(queryResult),readyRead)!=S_OK || queryResult!=42) std::abort();
    queryResult=0x12345678;
    table.pfnQueryGetData(h,queryHandle,&queryResult,sizeof(queryResult),0);
    table.pfnSetPredication(h,queryHandle,TRUE); table.pfnDestroyQuery(h,queryHandle);
    if (errors!=107 || queryStorage.object || queryStorage.predicate || queryResult!=0x12345678 || owner.runtime().domain.entered()) std::abort();
    D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY declarations[2]{};
    std::memset(declarations,0xCD,sizeof(declarations));
    declarations[0].Stream=2; declarations[0].OutputSlot=3; declarations[0].RegisterIndex=7; declarations[0].RegisterMask=6;
    declarations[1].Stream=0; declarations[1].OutputSlot=0; declarations[1].RegisterIndex=UINT_MAX; declarations[1].RegisterMask=15;
    UINT strides[4]{16,32,48,64};
    D3D11DDIARG_CREATEGEOMETRYSHADERWITHSTREAMOUTPUT soArgs{};
    soArgs.pOutputStreamDecl=declarations; soArgs.NumEntries=2;
    soArgs.BufferStridesInBytes=strides; soArgs.NumStrides=4; soArgs.RasterizedStream=D3D11_SO_NO_RASTERIZED_STREAM;
    std::vector<BC250_DXVK_SO_ENTRY> soEntries;
    if (copy_stream_output(soArgs,soEntries)!=S_OK || soEntries.size()!=2 || soEntries[0].Stream!=2 ||
        soEntries[0].OutputSlot!=3 || soEntries[0].RegisterIndex!=7 || soEntries[0].RegisterMask!=6 ||
        soEntries[1].RegisterIndex!=UINT_MAX || soEntries[1].RegisterMask!=15) std::abort();
    soArgs.pOutputStreamDecl=nullptr;
    if (copy_stream_output(soArgs,soEntries)!=E_INVALIDARG) std::abort();
    soArgs.pOutputStreamDecl=declarations; soArgs.NumStrides=5;
    if (copy_stream_output(soArgs,soEntries)!=E_INVALIDARG) std::abort();
    soArgs.NumStrides=4; soArgs.RasterizedStream=4;
    if (copy_stream_output(soArgs,soEntries)!=E_INVALIDARG) std::abort();
    soArgs.RasterizedStream=0; declarations[0].RegisterMask=16;
    if (copy_stream_output(soArgs,soEntries)!=E_INVALIDARG) std::abort();
    declarations[0].RegisterMask=6;
    DdiShader soShader{}; D3D10DDI_HSHADER soHandle{}; soHandle.pDrvPrivate=&soShader;
    D3D11_1DDIARG_STAGE_IO_SIGNATURES soSignatures{};
    if (table.pfnCalcPrivateGeometryShaderWithStreamOutput(h,&soArgs,&soSignatures)!=sizeof(DdiShader)) std::abort();
    table.pfnCreateGeometryShaderWithStreamOutput(h,&soArgs,soHandle,{},&soSignatures);
    if (errors!=108 || soShader.object || soShader.stage!=ShaderStage::geometry || owner.runtime().domain.entered()) std::abort();
    D3DDDI_DEVICECALLBACKS creationCallbacks{};
    creationCallbacks.pfnCreateContextVirtualCb=creation_context; creationCallbacks.pfnDestroyContextCb=creation_destroy;
    creationCallbacks.pfnDestroySynchronizationObjectCb=creation_sync;
    D3D10DDI_CORELAYER_DEVICECALLBACKS creationUm{}; creationUm.pfnSetErrorCb=unexpected_creation_error;
    DXGI_DDI_BASE_CALLBACKS creationDxgi{};
    D3D11_1DDI_DEVICEFUNCS unchangedTable{};
    std::memset(&unchangedTable,0xA5,sizeof(unchangedTable));
    const auto originalTable=unchangedTable;
    DXGI1_2_DDI_BASE_FUNCTIONS unchangedDxgi{};
    std::memset(&unchangedDxgi,0xA5,sizeof(unchangedDxgi));
    const auto originalDxgi=unchangedDxgi;
    DdiDeviceHandle newHandle{},failedCleanup{};
    D3D10DDIARG_CREATEDEVICE createArgs{}; createArgs.Interface=D3D11_1_DDI_INTERFACE_VERSION;
    createArgs.Flags=UINT(D3D11DDI_3DPIPELINELEVEL_11_0)<<D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT;
    createArgs.hDrvDevice.pDrvPrivate=&newHandle; createArgs.p11_1DeviceFuncs=&unchangedTable;
    createArgs.hRTDevice.handle=reinterpret_cast<decltype(createArgs.hRTDevice.handle)>(&createIdentity);
    createArgs.pKTCallbacks=&creationCallbacks; createArgs.pUMCallbacks=&creationUm;
    createArgs.DXGIBaseDDI.pDXGIBaseCallbacks=&creationDxgi;
    createArgs.DXGIBaseDDI.pDXGIDDIBaseFunctions3=&unchangedDxgi;
    BC250_DXVK_ENGINE_FUNCS createFuncs{}; createFuncs.CreateDevice=no_engine;
    BC250_DXVK_SHELL_SERVICES createServices{};
    AdapterCaps advertised{}; advertised.maximum=D3D_FEATURE_LEVEL_11_0;
    advertised.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=TRUE;
    auto attemptCreate=[&]() { return create_render_device(createArgs,1,no_instance,createFuncs,D3D_FEATURE_LEVEL_11_0,createServices,failedCleanup,advertised); };
    createArgs.DXGIBaseDDI.pDXGIDDIBaseFunctions3=nullptr;
    if(attemptCreate()!=E_INVALIDARG || createCount || destroyCount) std::abort();
    createArgs.DXGIBaseDDI.pDXGIDDIBaseFunctions3=&unchangedDxgi;
    advertised.maximum=D3D_FEATURE_LEVEL_10_0;
    if(attemptCreate()!=E_INVALIDARG || createCount || destroyCount) std::abort();
    advertised.maximum=D3D_FEATURE_LEVEL_11_0;
    advertised.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=FALSE;
    if(attemptCreate()!=E_INVALIDARG || createCount || destroyCount) std::abort();
    advertised.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=TRUE;
    const UINT negotiatedFlags=createArgs.Flags;
    createArgs.Flags=0;
    if (attemptCreate()!=E_INVALIDARG || createCount || destroyCount || newHandle.owner || failedCleanup.owner ||
        std::memcmp(&unchangedTable,&originalTable,sizeof(unchangedTable))) std::abort();
    createArgs.Flags=negotiatedFlags;
    rejectContext=true;
    if (attemptCreate()!=E_OUTOFMEMORY || createCount!=1 || destroyCount || newHandle.owner || failedCleanup.owner) std::abort();
    rejectContext=false;
    if (attemptCreate()!=E_NOINTERFACE || createCount!=2 || destroyCount!=1 || newHandle.owner || failedCleanup.owner) std::abort();
    rejectCleanup=true;
    if (attemptCreate()!=E_NOINTERFACE || !failedCleanup.owner || newHandle.owner || failedCleanup.owner->runtime().domain.entered()) std::abort();
    if (attemptCreate()!=E_UNEXPECTED || createCount!=3) std::abort();
    rejectCleanup=false;
    if (retire_device_handle(failedCleanup)!=S_OK || failedCleanup.owner) std::abort();
    if (std::memcmp(&unchangedTable,&originalTable,sizeof(unchangedTable))) std::abort();
    if(std::memcmp(&unchangedDxgi,&originalDxgi,sizeof(unchangedDxgi))) std::abort();
    const auto dxgiTable=make_dxgi_device_table();
    if(!dxgiTable.pfnPresent || !dxgiTable.pfnBlt || !dxgiTable.pfnBlt1 || !dxgiTable.pfnSetDisplayMode ||
       !dxgiTable.pfnSetResourcePriority || !dxgiTable.pfnQueryResourceResidency ||
       !dxgiTable.pfnRotateResourceIdentities || !dxgiTable.pfnResolveSharedResource ||
       !dxgiTable.pfnOfferResources || !dxgiTable.pfnReclaimResources || !dxgiTable.pfnGetGammaCaps ||
       !dxgiTable.pfnGetMultiplaneOverlayCaps || !dxgiTable.pfnGetMultiplaneOverlayFilterRange ||
       !dxgiTable.pfnCheckMultiplaneOverlaySupport || !dxgiTable.pfnPresentMultiplaneOverlay) std::abort();
    DXGI_GAMMA_CONTROL_CAPABILITIES gamma{}; std::memset(&gamma,0xA5,sizeof(gamma));
    const auto oldGamma=gamma;
    DXGI_DDI_ARG_GET_GAMMA_CONTROL_CAPS gammaArgs{reinterpret_cast<DXGI_DDI_HDEVICE>(&storage),&gamma};
    if(dxgiTable.pfnGetGammaCaps(&gammaArgs)!=DXGI_ERROR_UNSUPPORTED ||
       std::memcmp(&gamma,&oldGamma,sizeof(gamma)) || dxgiTable.pfnGetGammaCaps(nullptr)!=E_INVALIDARG) std::abort();
    D3D11_1DDI_DEVICEFUNCS relocated{};
    table.pfnRelocateDeviceFuncs(h,&relocated);
    if (relocated.pfnClearView!=table.pfnClearView || relocated.pfnDraw!=table.pfnDraw ||
        relocated.pfnRelocateDeviceFuncs!=table.pfnRelocateDeviceFuncs || owner.runtime().domain.entered()) std::abort();
    int viewIdentity=0;
    DdiRenderTargetView clearRtv{reinterpret_cast<ID3D11RenderTargetView *>(&viewIdentity)};
    DdiUnorderedAccessView clearUav{reinterpret_cast<ID3D11UnorderedAccessView *>(&viewIdentity)};
    if (clear_view_object(D3D10DDI_HT_RENDERTARGETVIEW,&clearRtv)!=static_cast<ID3D11View *>(clearRtv.object) ||
        clear_view_object(D3D11DDI_HT_UNORDEREDACCESSVIEW,&clearUav)!=static_cast<ID3D11View *>(clearUav.object) ||
        clear_view_object(D3D10DDI_HT_DEPTHSTENCILVIEW,&clearRtv) || clear_view_object(D3D10DDI_HT_RENDERTARGETVIEW,nullptr)) std::abort();
    FLOAT regionColor[4]{0,0.5f,1,1}; D3D10_DDI_RECT clearRect{2,3,9,11};
    const unsigned beforeClear=errors;
    table.pfnClearView(h,D3D10DDI_HT_RENDERTARGETVIEW,&clearRtv,regionColor,&clearRect,1);
    if (errors!=beforeClear+1 || owner.runtime().domain.entered()) std::abort();
    DXGI1_2_DDI_BASE_FUNCTIONS dxgiPresentTable{}; install_present_ddi(dxgiPresentTable);
    DXGI_DDI_ARG_PRESENT presentArgs{};
    if (dxgiPresentTable.pfnPresent(nullptr)!=E_INVALIDARG || dxgiPresentTable.pfnPresent(&presentArgs)!=E_INVALIDARG) std::abort();
    presentArgs.hDevice=reinterpret_cast<DXGI_DDI_HDEVICE>(&storage);
    if (dxgiPresentTable.pfnPresent(&presentArgs)!=E_FAIL || owner.runtime().domain.entered()) std::abort();
    int rotateIdentity[3]{};
    DdiResource rotating[3]{}; DXGI_DDI_HRESOURCE rotatingHandles[3]{};
    for (UINT i=0;i<3;++i) {
        rotating[i].object=reinterpret_cast<ID3D11Resource *>(&rotateIdentity[i]);
        rotating[i].present_allocation=31+i; rotating[i].present_subresource=10+i;
        rotatingHandles[i]=reinterpret_cast<DXGI_DDI_HRESOURCE>(&rotating[i]);
    }
    unsigned rotations=0;
    auto failedRotate=[&](ID3D11Resource *const *objects,UINT count) {
        ++rotations; if (count!=3 || objects[0]!=rotating[0].object || objects[2]!=rotating[2].object) std::abort();
        return E_FAIL;
    };
    if (rotate_present_resources(rotatingHandles,3,failedRotate)!=E_FAIL || rotations!=1 || rotating[0].present_allocation!=31 || rotating[2].present_subresource!=12) std::abort();
    auto successfulRotate=[&](ID3D11Resource *const *,UINT) { ++rotations; return S_OK; };
    if (rotate_present_resources(rotatingHandles,3,successfulRotate)!=S_OK || rotations!=2 ||
        rotating[0].present_allocation!=32 || rotating[1].present_allocation!=33 || rotating[2].present_allocation!=31 ||
        rotating[0].present_subresource!=11 || rotating[2].present_subresource!=10 ||
        rotating[0].object!=reinterpret_cast<ID3D11Resource *>(&rotateIdentity[0])) std::abort();
    rotatingHandles[1]=rotatingHandles[0];
    if (rotate_present_resources(rotatingHandles,3,successfulRotate)!=E_INVALIDARG || rotations!=2 ||
        rotate_present_resources(nullptr,0,successfulRotate)!=E_INVALIDARG) std::abort();
    RuntimeSurface backing[3];
    for (UINT i=0;i<3;++i) {
        rotatingHandles[i]=reinterpret_cast<DXGI_DDI_HRESOURCE>(&rotating[i]);
        rotating[i].runtime_surface=&backing[i]; rotating[i].present_allocation=31+i; rotating[i].present_subresource=0;
        backing[i].owner=&owner.runtime(); backing[i].phase=SurfacePhase::ready;
        backing[i].queue.queue=7; backing[i].queue.sync=8;
        backing[i].allocation={&rotateIdentity[i],31+i,51+i};
        backing[i].mapping={65536*(UINT64(i)+1),8192,9,true};
        backing[i].texture.texture=reinterpret_cast<ID3D11Texture2D *>(rotating[i].object);
        backing[i].texture.image={reinterpret_cast<VkImage>(uintptr_t(21+i)),reinterpret_cast<VkDeviceMemory>(uintptr_t(41+i))};
        backing[i].pitch=256; backing[i].bytes=8192;
    }
    if (rotate_present_resources(rotatingHandles,3,failedRotate)!=E_FAIL || backing[0].allocation.allocation!=31 ||
        backing[2].texture.image.image!=reinterpret_cast<VkImage>(uintptr_t(23))) std::abort();
    if (rotate_present_resources(rotatingHandles,3,successfulRotate)!=S_OK) std::abort();
    for (UINT i=0;i<3;++i) {
        UINT next=(i+1)%3;
        if (backing[i].allocation.runtime_resource!=&rotateIdentity[i] || backing[i].allocation.allocation!=31+next ||
            backing[i].allocation.kernel_resource!=51+next || rotating[i].present_allocation!=31+next ||
            backing[i].mapping.address!=65536*(UINT64(next)+1) ||
            backing[i].texture.image.image!=reinterpret_cast<VkImage>(uintptr_t(21+next)) ||
            backing[i].texture.image.memory!=reinterpret_cast<VkDeviceMemory>(uintptr_t(41+next)) ||
            backing[i].texture.texture!=rotating[i].object || rotating[i].runtime_surface!=&backing[i]) std::abort();
    }
    const unsigned beforeInvalidRotation=rotations;
    RuntimeDevice foreignRuntime;
    if (rotate_present_resources(rotatingHandles,3,successfulRotate,&foreignRuntime)!=E_INVALIDARG ||
        rotations!=beforeInvalidRotation) std::abort();
    backing[1].phase=SurfacePhase::closing;
    if (rotate_present_resources(rotatingHandles,3,successfulRotate)!=E_INVALIDARG || rotations!=beforeInvalidRotation) std::abort();
    backing[1].phase=SurfacePhase::ready; rotating[1].runtime_surface=nullptr;
    if (rotate_present_resources(rotatingHandles,3,successfulRotate)!=E_INVALIDARG || rotations!=beforeInvalidRotation) std::abort();
    for (auto &r:rotating) r.runtime_surface=nullptr;
    DXGI_DDI_ARG_ROTATE_RESOURCE_IDENTITIES rotateArgs{};
    rotateArgs.hDevice=reinterpret_cast<DXGI_DDI_HDEVICE>(&storage);
    if (dxgiPresentTable.pfnRotateResourceIdentities(nullptr)!=E_INVALIDARG ||
        dxgiPresentTable.pfnRotateResourceIdentities(&rotateArgs)!=E_FAIL || owner.runtime().domain.entered()) std::abort();
    DXGI_DDI_ARG_BLT bltArgs{};
    bltArgs.hDevice=reinterpret_cast<DXGI_DDI_HDEVICE>(&storage);
    bltArgs.hSrcResource=reinterpret_cast<DXGI_DDI_HRESOURCE>(&rotating[0]);
    bltArgs.hDstResource=reinterpret_cast<DXGI_DDI_HRESOURCE>(&rotating[1]);
    bltArgs.SrcSubresource=2; bltArgs.DstSubresource=3;
    bltArgs.DstLeft=4; bltArgs.DstTop=5; bltArgs.DstRight=24; bltArgs.DstBottom=35;
    bltArgs.Rotate=DXGI_DDI_MODE_ROTATION_ROTATE180; bltArgs.Flags.Value=15;
    BC250_DXVK_BLT engineBlt{};
    if (prepare_blt(bltArgs,engineBlt)!=S_OK || engineBlt.Source!=rotating[0].object || engineBlt.Destination!=rotating[1].object ||
        engineBlt.SourceSubresource!=2 || engineBlt.DestinationSubresource!=3 || engineBlt.Flags!=15 ||
        engineBlt.DestinationRect.left!=4 || engineBlt.DestinationRect.bottom!=35 || engineBlt.Rotation!=3) std::abort();
    bltArgs.DstRight=UINT_MAX;
    if (prepare_blt(bltArgs,engineBlt)!=E_INVALIDARG || engineBlt.DestinationRect.right!=24) std::abort();
    bltArgs.DstRight=24; bltArgs.Flags.Value=16;
    if (prepare_blt(bltArgs,engineBlt)!=E_INVALIDARG) std::abort();
    bltArgs.Flags.Value=0;
    for (UINT rotation=0;rotation<=4;++rotation) {
        bltArgs.Rotate=static_cast<DXGI_DDI_MODE_ROTATION>(rotation);
        if (prepare_blt(bltArgs,engineBlt)!=S_OK || engineBlt.Rotation!=rotation) std::abort();
    }
    install_blt_ddi(dxgiPresentTable);
    if (dxgiPresentTable.pfnBlt(nullptr)!=E_INVALIDARG || dxgiPresentTable.pfnBlt(&bltArgs)!=E_FAIL || owner.runtime().domain.entered()) std::abort();
    DXGI_DDI_ARG_BLT1 subBlt{};
    subBlt.hDevice=bltArgs.hDevice; subBlt.hSrcResource=bltArgs.hSrcResource; subBlt.hDstResource=bltArgs.hDstResource;
    subBlt.SrcSubresource=2; subBlt.DstSubresource=3;
    subBlt.SrcLeft=7; subBlt.SrcTop=9; subBlt.SrcRight=17; subBlt.SrcBottom=19;
    subBlt.DstLeft=20; subBlt.DstTop=30; subBlt.DstRight=40; subBlt.DstBottom=60;
    subBlt.Flags.Present=1; subBlt.Rotate=DXGI_DDI_MODE_ROTATION_ROTATE180;
    BC250_DXVK_BLT1 mappedBlt{};
    if (prepare_blt1(subBlt,mappedBlt)!=S_OK || mappedBlt.SourceRect.left!=7 || mappedBlt.SourceRect.bottom!=19 ||
        mappedBlt.DestinationRect.left!=20 || mappedBlt.DestinationRect.bottom!=60 || mappedBlt.SourceSubresource!=2 ||
        mappedBlt.DestinationSubresource!=3 || mappedBlt.Flags!=BC250_DXVK_BLT_PRESENT || mappedBlt.Rotation!=3) std::abort();
    subBlt.SrcRight=UINT_MAX;
    if (prepare_blt1(subBlt,mappedBlt)!=E_INVALIDARG || mappedBlt.SourceRect.right!=17) std::abort();
    subBlt.SrcRight=subBlt.SrcLeft;
    if (prepare_blt1(subBlt,mappedBlt)!=E_INVALIDARG) std::abort();
    if (dxgiPresentTable.pfnBlt1(nullptr)!=E_INVALIDARG || dxgiPresentTable.pfnBlt1(&subBlt)!=E_FAIL || owner.runtime().domain.entered()) std::abort();
    struct QueryCase { D3D10DDI_QUERY ddi; D3D11_QUERY api; bool predicate; };
    const QueryCase queryCases[]={
        {D3D10DDI_QUERY_EVENT,D3D11_QUERY_EVENT,false},
        {D3D10DDI_QUERY_OCCLUSION,D3D11_QUERY_OCCLUSION,false},
        {D3D10DDI_QUERY_TIMESTAMP,D3D11_QUERY_TIMESTAMP,false},
        {D3D10DDI_QUERY_TIMESTAMPDISJOINT,D3D11_QUERY_TIMESTAMP_DISJOINT,false},
        {D3D10DDI_QUERY_PIPELINESTATS,D3D11_QUERY_PIPELINE_STATISTICS,false},
        {D3D11DDI_QUERY_PIPELINESTATS,D3D11_QUERY_PIPELINE_STATISTICS,false},
        {D3D10DDI_QUERY_OCCLUSIONPREDICATE,D3D11_QUERY_OCCLUSION_PREDICATE,true},
        {D3D10DDI_QUERY_STREAMOUTPUTSTATS,D3D11_QUERY_SO_STATISTICS,false},
        {D3D10DDI_QUERY_STREAMOVERFLOWPREDICATE,D3D11_QUERY_SO_OVERFLOW_PREDICATE,true},
        {D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM0,D3D11_QUERY_SO_STATISTICS_STREAM0,false},
        {D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM1,D3D11_QUERY_SO_STATISTICS_STREAM1,false},
        {D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM2,D3D11_QUERY_SO_STATISTICS_STREAM2,false},
        {D3D11DDI_QUERY_STREAMOUTPUTSTATS_STREAM3,D3D11_QUERY_SO_STATISTICS_STREAM3,false},
        {D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM0,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0,true},
        {D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM1,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM1,true},
        {D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM2,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM2,true},
        {D3D11DDI_QUERY_STREAMOVERFLOWPREDICATE_STREAM3,D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM3,true}
    };
    for (const auto &q:queryCases) {
        D3D10DDIARG_CREATEQUERY arg{q.ddi,0}; D3D11_QUERY_DESC mapped{}; bool isPredicate=!q.predicate;
        if (convert_query(arg,mapped,isPredicate)!=S_OK || mapped.Query!=q.api || isPredicate!=q.predicate) std::abort();
    }
    const DXGI_FORMAT unsupportedDefined[]={DXGI_FORMAT_R1_UNORM,DXGI_FORMAT_P208,DXGI_FORMAT_V208,DXGI_FORMAT_V408};
    for (auto value:unsupportedDefined) {
        UINT caps=UINT32_MAX;
        if (classify_format_result(value,E_FAIL,caps)!=S_FALSE || caps ||
            classify_format_result(value,E_INVALIDARG,caps)!=S_FALSE || caps) std::abort();
        if (classify_format_result(value,E_OUTOFMEMORY,caps)!=E_OUTOFMEMORY ||
            classify_format_result(value,DXGI_ERROR_DEVICE_REMOVED,caps)!=DXGI_ERROR_DEVICE_REMOVED) std::abort();
    }
    unsigned definedFormats=0,explicitUnsupported=0;
    for (UINT value=0;value<192;++value) {
        auto formatValue=static_cast<DXGI_FORMAT>(value); UINT caps=123;
        HRESULT hr=classify_format_result(formatValue,E_FAIL,caps);
        if (defined_dxgi_format(formatValue)) {
            ++definedFormats;
            if (hr!=S_FALSE) std::abort();
            if (caps==D3D10_DDI_FORMAT_SUPPORT_NOT_SUPPORTED) ++explicitUnsupported;
            else if (caps) std::abort();
        } else if (hr!=E_FAIL || caps!=123) std::abort();
    }
    if (definedFormats!=122 || explicitUnsupported!=13 || defined_dxgi_format(DXGI_FORMAT_FORCE_UINT)) std::abort();
    D3D11DDIARG_CREATERESOURCE runtimeDesc{};
    D3D10DDI_MIPINFO runtimeMip{}; runtimeMip.TexelWidth=65; runtimeMip.TexelHeight=17; runtimeMip.TexelDepth=1;
    runtimeDesc.pMipInfoList=&runtimeMip; runtimeDesc.MipLevels=runtimeDesc.ArraySize=1;
    runtimeDesc.ResourceDimension=D3D10DDIRESOURCE_TEXTURE2D; runtimeDesc.SampleDesc.Count=1;
    runtimeDesc.Usage=D3D10_DDI_USAGE_DEFAULT; runtimeDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    runtimeDesc.BindFlags=D3D10_DDI_BIND_PRESENT|D3D10_DDI_BIND_RENDER_TARGET;
    runtimeDesc.MiscFlags=D3D10_DDI_RESOURCE_MISC_SHARED|D3D10_DDI_RESOURCE_MISC_DISCARD_ON_PRESENT;
    DXGI_DDI_PRIMARY_DESC primary{}; primary.VidPnSourceId=2; runtimeDesc.pPrimaryDesc=&primary;
    RuntimeSurfaceRequest request{}; D3D11_TEXTURE2D_DESC1 importedDesc{};
    if (convert_runtime_resource(runtimeDesc,&rotateIdentity[0],request,importedDesc)!=S_OK ||
        !request.primary || !request.shared || request.vidpn_source!=2 || request.surface.Width!=65 ||
        request.surface.Height!=17 || request.surface.Pitch!=512 || request.surface.Size!=12288 ||
        request.runtime_resource!=&rotateIdentity[0] || importedDesc.Width!=65 || importedDesc.Height!=17 ||
        importedDesc.BindFlags!=D3D11_BIND_RENDER_TARGET || importedDesc.MiscFlags) std::abort();
    runtimeDesc.SampleDesc.Count=4;
    if (convert_runtime_resource(runtimeDesc,&rotateIdentity[0],request,importedDesc)!=E_NOTIMPL) std::abort();
    runtimeDesc.SampleDesc.Count=1; runtimeDesc.Format=DXGI_FORMAT_R16_FLOAT;
    if (convert_runtime_resource(runtimeDesc,&rotateIdentity[0],request,importedDesc)!=E_NOTIMPL) std::abort();
    runtimeDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; runtimeMip.TexelWidth=UINT32_MAX;
    if (convert_runtime_resource(runtimeDesc,&rotateIdentity[0],request,importedDesc)!=E_INVALIDARG) std::abort();
    runtimeMip.TexelWidth=65;
    const DXGI_FORMAT sharedFormats[]={DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    D3DDDI_OPENALLOCATIONINFO2 openAllocation{};
    D3D10DDIARG_OPENRESOURCE openArgs{}; openArgs.NumAllocations=1; openArgs.pOpenAllocationInfo2=&openAllocation;
    openAllocation.hAllocation=31; openAllocation.pPrivateDriverData=&request.surface; openAllocation.PrivateDriverDataSize=sizeof(request.surface);
    openArgs.pPrivateDriverData=&request.texture; openArgs.PrivateDriverDataSize=sizeof(request.texture);
    BC250_WDDM_ALLOCATION_PRIVATE decodedSurface{}; D3D11_TEXTURE2D_DESC1 decodedDesc{};
    for (auto format:sharedFormats) {
        runtimeDesc.Format=format;
        if (convert_runtime_resource(runtimeDesc,&rotateIdentity[0],request,importedDesc)!=S_OK ||
            decode_open_resource(openArgs,decodedSurface,decodedDesc)!=S_OK || decodedDesc.Format!=format ||
            decodedDesc.BindFlags!=importedDesc.BindFlags || decodedDesc.Width!=65 || decodedDesc.Height!=17 ||
            decodedSurface.Pitch!=512 || decodedSurface.Size!=12288) std::abort();
    }
    request.texture.Width=64;
    if (decode_open_resource(openArgs,decodedSurface,decodedDesc)!=E_INVALIDARG || decodedDesc.Width!=65) std::abort();
    request.texture.Width=65; request.texture.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    if (decode_open_resource(openArgs,decodedSurface,decodedDesc)!=E_INVALIDARG) std::abort();
    request.texture.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    for (UINT length=0;length<sizeof(request.texture);++length) {
        openArgs.PrivateDriverDataSize=length;
        if (decode_open_resource(openArgs,decodedSurface,decodedDesc)!=E_INVALIDARG) std::abort();
    }
    openArgs.PrivateDriverDataSize=sizeof(request.texture); request.texture.BindFlags=UINT32_MAX;
    if (decode_open_resource(openArgs,decodedSurface,decodedDesc)!=E_INVALIDARG) std::abort();
    if (!table.pfnOpenResource || table.pfnCalcPrivateOpenedResourceSize(h,&openArgs)!=sizeof(DdiResource)) std::abort();
    DXGI1_2_DDI_BASE_FUNCTIONS resourceTable{}; install_dxgi_resource_ddi(resourceTable);
    owner.runtime().hDevice=&createIdentity; expected=&owner;
    owner.runtime().KTCallbacks.pfnSetPriorityCb=runtime_priority;
    owner.runtime().KTCallbacks.pfnQueryResidencyCb=runtime_residency;
    RuntimeSurface tracked[3]; DdiResource sharedResources[3]{}; DXGI_DDI_HRESOURCE sharedHandles[3]{};
    DXGI_DDI_RESIDENCY statuses[3]{};
    for (UINT i=0;i<3;++i) {
        tracked[i].owner=&owner.runtime(); tracked[i].phase=SurfacePhase::ready;
        tracked[i].allocation.allocation=61+i;
        tracked[i].texture.texture=reinterpret_cast<ID3D11Texture2D *>(&rotateIdentity[i]);
        sharedResources[i].object=tracked[i].texture.texture;
        sharedResources[i].runtime_surface=&tracked[i]; sharedResources[i].present_allocation=61+i;
        sharedHandles[i]=reinterpret_cast<DXGI_DDI_HRESOURCE>(&sharedResources[i]);
    }
    BOOL discarded[3]={9,9,9};
    DXGI_DDI_ARG_RECLAIMRESOURCES reclaimArgs{reinterpret_cast<DXGI_DDI_HDEVICE>(&storage),sharedHandles,discarded,3};
    DXGI_DDI_ARG_OFFERRESOURCES offerArgs{reclaimArgs.hDevice,sharedHandles,3,D3DDDI_OFFER_PRIORITY_NORMAL};
    if(!resourceTable.pfnOfferResources || !resourceTable.pfnReclaimResources ||
       resourceTable.pfnOfferResources(nullptr)!=E_INVALIDARG ||
       resourceTable.pfnReclaimResources(nullptr)!=E_INVALIDARG ||
       resourceTable.pfnOfferResources(&offerArgs)!=E_NOTIMPL ||
       resourceTable.pfnReclaimResources(&reclaimArgs)!=E_NOTIMPL) std::abort();
    owner.runtime().KTCallbacks.pfnOfferAllocationsCb=runtime_offer;
    owner.runtime().KTCallbacks.pfnReclaimAllocationsCb=runtime_reclaim;
    {
        RuntimeDomain::Scope scope(owner.runtime().domain);
        const D3DKMT_HANDLE handles[]={61,62,63};
        auto submit=[&]() { ++submitCalls; return S_OK; };
        if(offer_after_submit(owner.runtime(),handles,3,D3DDDI_OFFER_PRIORITY_NORMAL,[](){return E_FAIL;})!=E_FAIL || offerCalls) std::abort();
        if(offer_after_submit(owner.runtime(),handles,3,D3DDDI_OFFER_PRIORITY_NORMAL,submit)!=S_OK || offerCalls!=1) std::abort();
        residencyChangeResult=D3DDDIERR_DEVICEREMOVED;
        if(offer_after_submit(owner.runtime(),handles,3,D3DDDI_OFFER_PRIORITY_NORMAL,submit)!=D3DDDIERR_DEVICEREMOVED || offerCalls!=2) std::abort();
    }
    if(resourceTable.pfnReclaimResources(&reclaimArgs)!=D3DDDIERR_DEVICEREMOVED || reclaimCalls!=1) std::abort();
    for(auto value:discarded) if(value!=9) std::abort();
    residencyChangeResult=S_OK;
    if(resourceTable.pfnReclaimResources(&reclaimArgs)!=S_OK || reclaimCalls!=2 || discarded[0] || !discarded[1] || discarded[2]) std::abort();
    reclaimArgs.pDiscarded=nullptr;
    if(resourceTable.pfnReclaimResources(&reclaimArgs)!=S_OK || reclaimCalls!=3) std::abort();
    sharedHandles[2]=sharedHandles[0];
    if(resourceTable.pfnReclaimResources(&reclaimArgs)!=E_INVALIDARG || reclaimCalls!=3 ||
       resourceTable.pfnOfferResources(&offerArgs)!=E_INVALIDARG || offerCalls!=2) std::abort();
    sharedHandles[2]=reinterpret_cast<DXGI_DDI_HRESOURCE>(&sharedResources[2]);
    offerArgs.Priority=static_cast<D3DDDI_OFFER_PRIORITY>(0);
    if(resourceTable.pfnOfferResources(&offerArgs)!=E_INVALIDARG || offerCalls!=2) std::abort();
    offerArgs.Priority=D3DDDI_OFFER_PRIORITY_NORMAL;
    if(resourceTable.pfnOfferResources(&offerArgs)!=E_FAIL || offerCalls!=2) std::abort(); // no engine/context
    reclaimArgs.Resources=0; reclaimArgs.pResources=nullptr;
    if(resourceTable.pfnReclaimResources(&reclaimArgs)!=S_OK || reclaimCalls!=3 || owner.runtime().domain.entered()) std::abort();
    DXGI_DDI_ARG_SETDISPLAYMODE modeArgs{reinterpret_cast<DXGI_DDI_HDEVICE>(&storage),sharedHandles[0],0};
    if (!resourceTable.pfnSetDisplayMode || resourceTable.pfnSetDisplayMode(nullptr)!=E_INVALIDARG ||
        resourceTable.pfnSetDisplayMode(&modeArgs)!=E_NOTIMPL || displayModeCalls) std::abort();
    owner.runtime().KTCallbacks.pfnSetDisplayModeCb=runtime_display_mode;
    if (resourceTable.pfnSetDisplayMode(&modeArgs)!=S_OK || displayModeCalls!=1 ||
        owner.runtime().domain.entered()) std::abort();
    displayModeResult=DXGI_ERROR_DEVICE_REMOVED;
    if (resourceTable.pfnSetDisplayMode(&modeArgs)!=D3DDDIERR_DEVICEREMOVED || displayModeCalls!=2) std::abort();
    displayModeResult=E_INVALIDARG;
    if (resourceTable.pfnSetDisplayMode(&modeArgs)!=E_INVALIDARG || displayModeCalls!=3) std::abort();
    modeArgs.SubResourceIndex=1;
    if (resourceTable.pfnSetDisplayMode(&modeArgs)!=E_INVALIDARG || displayModeCalls!=3) std::abort();
    modeArgs.SubResourceIndex=0; sharedResources[0].present_subresource=1;
    if (resourceTable.pfnSetDisplayMode(&modeArgs)!=E_INVALIDARG || displayModeCalls!=3) std::abort();
    sharedResources[0].present_subresource=0; tracked[0].owner=&foreignRuntime;
    if (resourceTable.pfnSetDisplayMode(&modeArgs)!=E_INVALIDARG || displayModeCalls!=3) std::abort();
    tracked[0].owner=&owner.runtime(); tracked[0].phase=SurfacePhase::closing;
    if (resourceTable.pfnSetDisplayMode(&modeArgs)!=E_INVALIDARG || displayModeCalls!=3) std::abort();
    tracked[0].phase=SurfacePhase::ready;
    DXGI_DDI_ARG_SETRESOURCEPRIORITY priorityArgs{reinterpret_cast<DXGI_DDI_HDEVICE>(&storage),sharedHandles[0],7};
    if (resourceTable.pfnSetResourcePriority(&priorityArgs)!=S_OK || priorityCalls!=1 || owner.runtime().domain.entered()) std::abort();
    DXGI_DDI_ARG_QUERYRESOURCERESIDENCY residencyArgs{priorityArgs.hDevice,sharedHandles,statuses,3};
    if (resourceTable.pfnQueryResourceResidency(&residencyArgs)!=S_NOT_RESIDENT || residencyCalls!=1 ||
        statuses[0]!=DXGI_DDI_RESIDENCY_FULLY_RESIDENT || statuses[1]!=DXGI_DDI_RESIDENCY_RESIDENT_IN_SHARED_MEMORY ||
        statuses[2]!=DXGI_DDI_RESIDENCY_EVICTED_TO_DISK || owner.runtime().domain.entered()) std::abort();
    for (int mode=1;mode<=2;++mode) {
        residencyMode=mode;
        for (auto &value:statuses) value=static_cast<DXGI_DDI_RESIDENCY>(99);
        if (resourceTable.pfnQueryResourceResidency(&residencyArgs)!=E_FAIL) std::abort();
        for (auto value:statuses) if (value!=99) std::abort();
    }
    residencyMode=3;
    if (resourceTable.pfnQueryResourceResidency(&residencyArgs)!=S_OK) std::abort();
    residencyMode=4;
    if (resourceTable.pfnQueryResourceResidency(&residencyArgs)!=S_RESIDENT_IN_SHARED_MEMORY) std::abort();
    tracked[1].owner=&foreignRuntime;
    if (resourceTable.pfnQueryResourceResidency(&residencyArgs)!=E_INVALIDARG || residencyCalls!=5) std::abort();
    sharedResources[0].runtime_surface=nullptr;
    if (resourceTable.pfnSetResourcePriority(&priorityArgs)!=E_NOTIMPL || priorityCalls!=1 ||
        resourceTable.pfnResolveSharedResource(nullptr)!=E_INVALIDARG) std::abort();
    UINT64 adapterLuid=77;
    if(query_adapter_identity(nullptr,adapter_query,adapterLuid)!=E_INVALIDARG ||
        query_adapter_identity(&createIdentity,nullptr,adapterLuid)!=E_INVALIDARG || adapterLuid!=77) std::abort();
    for(adapterMode=1;adapterMode<=6;++adapterMode) {
        if(query_adapter_identity(&createIdentity,adapter_query,adapterLuid)!=
            (adapterMode==1 ? E_OUTOFMEMORY : DXGI_ERROR_UNSUPPORTED) || adapterLuid!=77) std::abort();
    }
    adapterMode=0;
    if(query_adapter_identity(&createIdentity,adapter_query,adapterLuid)!=S_OK ||
        adapterLuid!=0xffffff8512345678ull) std::abort();
    UINT32 versionCount=0; UINT64 versions[2]={123,456};
    if (supported_ddi_versions(nullptr,versions)!=E_INVALIDARG ||
        supported_ddi_versions(&versionCount,nullptr)!=S_OK || versionCount!=1) std::abort();
    versionCount=0;
    if (supported_ddi_versions(&versionCount,versions)!=HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) ||
        versionCount!=0 || versions[0]!=123 || versions[1]!=456) std::abort();
    versionCount=2;
    if (supported_ddi_versions(&versionCount,versions)!=S_OK || versionCount!=1 ||
        versions[0]!=D3D11_1_DDI_SUPPORTED || versions[1]!=456) std::abort();
    const D3D_FEATURE_LEVEL featureLevels[]={D3D_FEATURE_LEVEL_10_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_11_1};
    for (UINT pipeline=0;pipeline<32;++pipeline) {
        const UINT flags=((pipeline&7)<<D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT)|
            ((pipeline&24)<<D3D11DDI_CREATEDEVICE_FLAG_3DPIPELINESUPPORT_SHIFT2)|
            D3D10DDI_CREATEDEVICE_FLAG_DISABLE_EXTRA_THREAD_CREATION|D3D11DDI_CREATEDEVICE_FLAG_SINGLETHREADED;
        D3D_FEATURE_LEVEL chosen=D3D_FEATURE_LEVEL_9_1;
        HRESULT status=requested_feature_level(flags,chosen);
        if (pipeline<4) { if (status!=S_OK || chosen!=featureLevels[pipeline]) std::abort(); }
        else if (status!=E_INVALIDARG || chosen!=D3D_FEATURE_LEVEL_9_1) std::abort();
    }
    AdapterConfiguration configuration{L"P:/BC-250/scratch/m14/missing-adapter-engine.dll",
        L"P:/BC-250/scratch/m14/missing-adapter-icd.dll",advertised};
    D3D10_2DDI_ADAPTERFUNCS adapterTable{};std::memset(&adapterTable,0xA5,sizeof(adapterTable));
    const auto untouchedAdapterTable=adapterTable;
    D3DDDI_ADAPTERCALLBACKS adapterCallbacks{};adapterCallbacks.pfnQueryAdapterInfoCb=adapter_query;
    D3D10DDIARG_OPENADAPTER adapterArgs{};adapterArgs.pAdapterFuncs_2=&adapterTable;
    adapterArgs.pAdapterCallbacks=&adapterCallbacks;
    adapterArgs.hRTAdapter.handle=&createIdentity;
    adapterArgs.hAdapter.pDrvPrivate=&createIdentity;
    adapterMode=1;
    if(open_render_adapter(adapterArgs,configuration)!=E_OUTOFMEMORY ||
       adapterArgs.hAdapter.pDrvPrivate!=&createIdentity ||
       std::memcmp(&adapterTable,&untouchedAdapterTable,sizeof(adapterTable))) std::abort();
    adapterMode=2;
    if(open_render_adapter(adapterArgs,configuration)!=DXGI_ERROR_UNSUPPORTED ||
       adapterArgs.hAdapter.pDrvPrivate!=&createIdentity ||
       std::memcmp(&adapterTable,&untouchedAdapterTable,sizeof(adapterTable))) std::abort();
    adapterMode=0;
    if(open_render_adapter(adapterArgs,configuration)!=S_OK || adapterArgs.hAdapter.pDrvPrivate==&createIdentity) std::abort();
    UINT32 adapterVersions=0;
    if(adapterTable.pfnGetSupportedVersions(adapterArgs.hAdapter,&adapterVersions,nullptr)!=S_OK || adapterVersions!=1) std::abort();
    D3D11DDI_3DPIPELINESUPPORT_CAPS adapterPipelines{};
    D3D10_2DDIARG_GETCAPS adapterCapsArgs{};adapterCapsArgs.Type=D3D11DDICAPS_3DPIPELINESUPPORT;
    adapterCapsArgs.pData=&adapterPipelines;adapterCapsArgs.DataSize=sizeof(adapterPipelines);
    if(adapterTable.pfnGetCaps(adapterArgs.hAdapter,&adapterCapsArgs)!=S_OK || adapterPipelines.Caps!=7) std::abort();
    D3D10DDIARG_CALCPRIVATEDEVICESIZE sizeArgs{};sizeArgs.Interface=D3D11_1_DDI_INTERFACE_VERSION;sizeArgs.Flags=createArgs.Flags;
    if(adapterTable.pfnCalcPrivateDeviceSize(adapterArgs.hAdapter,&sizeArgs)!=sizeof(DdiDeviceHandle)) std::abort();
    sizeArgs.Version=1u<<16;
    if(adapterTable.pfnCalcPrivateDeviceSize(adapterArgs.hAdapter,&sizeArgs)) std::abort();
    // Open and GetCaps work without loading either missing DLL. Only CreateDevice
    // attempts the load, and its failure cannot publish device function tables.
    HRESULT missingModules=adapterTable.pfnCreateDevice(adapterArgs.hAdapter,&createArgs);
    if(SUCCEEDED(missingModules) || newHandle.owner ||
       std::memcmp(&unchangedTable,&originalTable,sizeof(unchangedTable)) ||
       std::memcmp(&unchangedDxgi,&originalDxgi,sizeof(unchangedDxgi))) std::abort();
    if(adapterTable.pfnCloseAdapter(adapterArgs.hAdapter)!=S_OK) std::abort();
    std::cout << "PASS draw DDI signatures and uninitialized-engine error/domain control (no rendering test)\n";
}
