// SPDX-License-Identifier: MIT
#include "ddi-fixed-state.h"
#include <cstddef>
#include <cstring>
namespace bc250::umd {
#define CHECK_ENUM(name) static_assert(unsigned(D3D10_DDI_##name)==unsigned(D3D11_##name))
CHECK_ENUM(DEPTH_WRITE_MASK_ZERO); CHECK_ENUM(DEPTH_WRITE_MASK_ALL);
CHECK_ENUM(STENCIL_OP_KEEP); CHECK_ENUM(STENCIL_OP_ZERO); CHECK_ENUM(STENCIL_OP_REPLACE);
CHECK_ENUM(STENCIL_OP_INCR_SAT); CHECK_ENUM(STENCIL_OP_DECR_SAT);
CHECK_ENUM(STENCIL_OP_INVERT); CHECK_ENUM(STENCIL_OP_INCR); CHECK_ENUM(STENCIL_OP_DECR);
CHECK_ENUM(COMPARISON_NEVER); CHECK_ENUM(COMPARISON_LESS); CHECK_ENUM(COMPARISON_EQUAL);
CHECK_ENUM(COMPARISON_LESS_EQUAL); CHECK_ENUM(COMPARISON_GREATER);
CHECK_ENUM(COMPARISON_NOT_EQUAL); CHECK_ENUM(COMPARISON_GREATER_EQUAL); CHECK_ENUM(COMPARISON_ALWAYS);
CHECK_ENUM(FILL_WIREFRAME); CHECK_ENUM(FILL_SOLID);
CHECK_ENUM(CULL_NONE); CHECK_ENUM(CULL_FRONT); CHECK_ENUM(CULL_BACK);
#undef CHECK_ENUM
namespace {
D3D11_DEPTH_STENCILOP_DESC face(const D3D10_DDI_DEPTH_STENCILOP_DESC &s,BOOL enabled) {
    // DDI has per-face enable flags; COM expresses a disabled face as an
    // always-passing test with no writes. Do not consume disabled face fields.
    if (!enabled) return {D3D11_STENCIL_OP_KEEP,D3D11_STENCIL_OP_KEEP,
        D3D11_STENCIL_OP_KEEP,D3D11_COMPARISON_ALWAYS};
    return {static_cast<D3D11_STENCIL_OP>(s.StencilFailOp),
        static_cast<D3D11_STENCIL_OP>(s.StencilDepthFailOp),
        static_cast<D3D11_STENCIL_OP>(s.StencilPassOp),
        static_cast<D3D11_COMPARISON_FUNC>(s.StencilFunc)};
}
}
D3D11_DEPTH_STENCIL_DESC convert_depth_stencil(const D3D10_DDI_DEPTH_STENCIL_DESC &s) {
    D3D11_DEPTH_STENCIL_DESC d{};
    d.DepthEnable=s.DepthEnable;
    d.DepthWriteMask=static_cast<D3D11_DEPTH_WRITE_MASK>(s.DepthWriteMask);
    d.DepthFunc=static_cast<D3D11_COMPARISON_FUNC>(s.DepthFunc);
    d.StencilEnable=s.StencilEnable; d.StencilReadMask=s.StencilReadMask; d.StencilWriteMask=s.StencilWriteMask;
    d.FrontFace=face(s.FrontFace,s.StencilEnable && s.FrontEnable);
    d.BackFace=face(s.BackFace,s.StencilEnable && s.BackEnable);
    return d;
}
D3D11_RASTERIZER_DESC1 convert_rasterizer(const D3D11_1_DDI_RASTERIZER_DESC &s) {
    D3D11_RASTERIZER_DESC1 d{};
    d.FillMode=static_cast<D3D11_FILL_MODE>(s.FillMode);
    d.CullMode=static_cast<D3D11_CULL_MODE>(s.CullMode);
    d.FrontCounterClockwise=s.FrontCounterClockwise;
    d.DepthBias=s.DepthBias; d.DepthBiasClamp=s.DepthBiasClamp; d.SlopeScaledDepthBias=s.SlopeScaledDepthBias;
    d.DepthClipEnable=s.DepthClipEnable; d.ScissorEnable=s.ScissorEnable;
    d.MultisampleEnable=s.MultisampleEnable; d.AntialiasedLineEnable=s.AntialiasedLineEnable;
    d.ForcedSampleCount=s.ForcedSampleCount;
    return d;
}
D3D11_RASTERIZER_DESC2 convert_rasterizer2(const D3DWDDM2_0DDI_RASTERIZER_DESC &s) {
    static_assert(offsetof(D3DWDDM2_0DDI_RASTERIZER_DESC,ForcedSampleCount)==offsetof(D3D11_1_DDI_RASTERIZER_DESC,ForcedSampleCount));
    D3D11_1_DDI_RASTERIZER_DESC prefix{};std::memcpy(&prefix,&s,sizeof(prefix));
    const auto first=convert_rasterizer(prefix);
    D3D11_RASTERIZER_DESC2 d{};std::memcpy(&d,&first,sizeof(first));
    static_assert(UINT(D3D11_CONSERVATIVE_RASTERIZATION_MODE_ON)==UINT(D3DWDDM2_0DDI_CONSERVATIVE_RASTERIZATION_ON));
    d.ConservativeRaster=static_cast<D3D11_CONSERVATIVE_RASTERIZATION_MODE>(s.ConservativeRasterizationMode);
    return d;
}
namespace {
SIZE_T APIENTRY depth_size(D3D10DDI_HDEVICE,const D3D10_DDI_DEPTH_STENCIL_DESC *) { return sizeof(DdiDepthStencil); }
SIZE_T APIENTRY raster_size(D3D10DDI_HDEVICE,const D3D11_1_DDI_RASTERIZER_DESC *) { return sizeof(DdiRasterizer); }
template<typename Storage,typename Handle,typename Desc,typename Convert,typename Create>
void create_state(D3D10DDI_HDEVICE h,const Desc *desc,Handle handle,Convert convert,Create create) {
    auto *s=static_cast<Storage *>(handle.pDrvPrivate);
    if (s) s->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !desc || !owner.device()) { report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return; }
        const auto d=convert(*desc);
        const HRESULT hr=(owner.device()->*create)(&d,&s->object);
        if (FAILED(hr)) {
            if (s->object) { s->object->Release(); s->object=nullptr; }
            report_ddi_error(owner,hr,DdiErrorClass::out_of_memory);
        } else if (!s->object) report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory);
    },DdiErrorClass::out_of_memory);
}
void APIENTRY depth_create(D3D10DDI_HDEVICE h,const D3D10_DDI_DEPTH_STENCIL_DESC *d,
    D3D10DDI_HDEPTHSTENCILSTATE s,D3D10DDI_HRTDEPTHSTENCILSTATE) {
    create_state<DdiDepthStencil>(h,d,s,convert_depth_stencil,&ID3D11Device5::CreateDepthStencilState);
}
void APIENTRY raster_create(D3D10DDI_HDEVICE h,const D3D11_1_DDI_RASTERIZER_DESC *d,
    D3D10DDI_HRASTERIZERSTATE s,D3D10DDI_HRTRASTERIZERSTATE) {
    create_state<DdiRasterizer>(h,d,s,convert_rasterizer,&ID3D11Device5::CreateRasterizerState1);
}
SIZE_T APIENTRY raster2_size(D3D10DDI_HDEVICE,const D3DWDDM2_0DDI_RASTERIZER_DESC *) { return sizeof(DdiRasterizer); }
void APIENTRY raster2_create(D3D10DDI_HDEVICE h,const D3DWDDM2_0DDI_RASTERIZER_DESC *desc,
    D3D10DDI_HRASTERIZERSTATE handle,D3D10DDI_HRTRASTERIZERSTATE) {
    auto *s=static_cast<DdiRasterizer *>(handle.pDrvPrivate);
    if (s) s->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        if (!s || !desc || !owner.device() ||
            UINT(desc->ConservativeRasterizationMode)>UINT(D3DWDDM2_0DDI_CONSERVATIVE_RASTERIZATION_ON)) {
            report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); return;
        }
        const auto d=convert_rasterizer2(*desc);
        ID3D11RasterizerState2 *state=nullptr;
        const HRESULT hr=owner.device()->CreateRasterizerState2(&d,&state);
        if (FAILED(hr)) {
            if (state) state->Release();
            report_ddi_error(owner,hr,DdiErrorClass::out_of_memory);
        } else if (!state) report_ddi_error(owner,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory);
        else s->object=state; // RasterizerState2 is a RasterizerState1: binding and release are unchanged.
    },DdiErrorClass::out_of_memory);
}
template<typename Storage,typename Handle> void APIENTRY destroy(D3D10DDI_HDEVICE h,Handle handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<Storage *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
void APIENTRY depth_bind(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILSTATE handle,UINT ref) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *s=static_cast<DdiDepthStencil *>(handle.pDrvPrivate);
        context.OMSetDepthStencilState(s ? s->object : nullptr,ref);
    });
}
void APIENTRY raster_bind(D3D10DDI_HDEVICE h,D3D10DDI_HRASTERIZERSTATE handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *s=static_cast<DdiRasterizer *>(handle.pDrvPrivate);
        context.RSSetState(s ? s->object : nullptr);
    });
}
}
void install_fixed_state_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateDepthStencilStateSize=depth_size; t.pfnCreateDepthStencilState=depth_create;
    t.pfnDestroyDepthStencilState=destroy<DdiDepthStencil,D3D10DDI_HDEPTHSTENCILSTATE>;
    t.pfnSetDepthStencilState=depth_bind;
    t.pfnCalcPrivateRasterizerStateSize=raster_size; t.pfnCreateRasterizerState=raster_create;
    t.pfnDestroyRasterizerState=destroy<DdiRasterizer,D3D10DDI_HRASTERIZERSTATE>;
    t.pfnSetRasterizerState=raster_bind;
}
void install_fixed_state_wddm2_0_ddi(D3DWDDM2_0DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateRasterizerStateSize=raster2_size; t.pfnCreateRasterizerState=raster2_create;
}
}
