// SPDX-License-Identifier: MIT
#include "ddi-dsv.h"
#include "ddi-rtv.h"
namespace bc250::umd {
HRESULT convert_dsv(const D3D11DDIARG_CREATEDEPTHSTENCILVIEW &s,UINT layers,UINT samples,D3D11_DEPTH_STENCIL_VIEW_DESC &out) {
    if (s.Flags & ~UINT(D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH|D3D11_DDI_CREATE_DSV_READ_ONLY_STENCIL)) return E_INVALIDARG;
    // Reuse the same layer/MSAA validation as RTV without aliasing DDI unions
    // or assuming equality between RTV and DSV dimension enums.
    D3D10DDIARG_CREATERENDERTARGETVIEW r{}; r.Format=s.Format; r.ResourceDimension=s.ResourceDimension;
    switch(s.ResourceDimension) {
    case D3D10DDIRESOURCE_TEXTURE1D: r.Tex1D={s.Tex1D.MipSlice,s.Tex1D.FirstArraySlice,s.Tex1D.ArraySize}; break;
    case D3D10DDIRESOURCE_TEXTURE2D: r.Tex2D={s.Tex2D.MipSlice,s.Tex2D.FirstArraySlice,s.Tex2D.ArraySize}; break;
    case D3D10DDIRESOURCE_TEXTURECUBE: r.TexCube={s.TexCube.MipSlice,s.TexCube.FirstArraySlice,s.TexCube.ArraySize}; break;
    default: return E_INVALIDARG;
    }
    D3D11_RENDER_TARGET_VIEW_DESC v{};
    HRESULT hr=convert_rtv(r,layers,samples,v); if (FAILED(hr)) return hr;
    D3D11_DEPTH_STENCIL_VIEW_DESC d{}; d.Format=s.Format;
    if (s.Flags & D3D11_DDI_CREATE_DSV_READ_ONLY_DEPTH) d.Flags|=D3D11_DSV_READ_ONLY_DEPTH;
    if (s.Flags & D3D11_DDI_CREATE_DSV_READ_ONLY_STENCIL) d.Flags|=D3D11_DSV_READ_ONLY_STENCIL;
    switch(v.ViewDimension) {
    case D3D11_RTV_DIMENSION_TEXTURE1D: d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE1D; d.Texture1D.MipSlice=v.Texture1D.MipSlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE1DARRAY; d.Texture1DArray={v.Texture1DArray.MipSlice,v.Texture1DArray.FirstArraySlice,v.Texture1DArray.ArraySize}; break;
    case D3D11_RTV_DIMENSION_TEXTURE2D: d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D; d.Texture2D.MipSlice=v.Texture2D.MipSlice; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray={v.Texture2DArray.MipSlice,v.Texture2DArray.FirstArraySlice,v.Texture2DArray.ArraySize}; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMS: d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS; break;
    case D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY: d.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMSARRAY; d.Texture2DMSArray={v.Texture2DMSArray.FirstArraySlice,v.Texture2DMSArray.ArraySize}; break;
    default: return E_INVALIDARG;
    }
    out=d; return S_OK;
}
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *) { return sizeof(DdiDepthStencilView); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATEDEPTHSTENCILVIEW *desc,
    D3D10DDI_HDEPTHSTENCILVIEW handle,D3D10DDI_HRTDEPTHSTENCILVIEW) {
    auto *s=static_cast<DdiDepthStencilView *>(handle.pDrvPrivate); if (s) s->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        auto *r=desc ? static_cast<DdiResource *>(desc->hDrvResource.pDrvPrivate) : nullptr;
        if (!s || !r || !r->object || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        UINT layers=1,samples=1;
        D3D11_RESOURCE_DIMENSION actual{}; r->object->GetType(&actual);
        if (desc->ResourceDimension==D3D10DDIRESOURCE_TEXTURE1D && actual==D3D11_RESOURCE_DIMENSION_TEXTURE1D) {
            D3D11_TEXTURE1D_DESC d{}; static_cast<ID3D11Texture1D *>(r->object)->GetDesc(&d); layers=d.ArraySize;
        } else if ((desc->ResourceDimension==D3D10DDIRESOURCE_TEXTURE2D || desc->ResourceDimension==D3D10DDIRESOURCE_TEXTURECUBE) && actual==D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
            D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D *>(r->object)->GetDesc(&d); layers=d.ArraySize; samples=d.SampleDesc.Count;
        } else { report_ddi_error(owner,E_INVALIDARG); return; }
        D3D11_DEPTH_STENCIL_VIEW_DESC d{};
        HRESULT hr=convert_dsv(*desc,layers,samples,d);
        if (SUCCEEDED(hr)) hr=owner.device()->CreateDepthStencilView(r->object,&d,&s->object);
        if (FAILED(hr)) {
            if (s->object) { s->object->Release(); s->object=nullptr; }
            report_ddi_error(owner,hr);
        } else if (!s->object) report_ddi_error(owner,E_FAIL);
    });
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILVIEW handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiDepthStencilView *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
void APIENTRY clear(D3D10DDI_HDEVICE h,D3D10DDI_HDEPTHSTENCILVIEW handle,UINT flags,FLOAT depth,UINT8 stencil) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *s=static_cast<DdiDepthStencilView *>(handle.pDrvPrivate);
        if (!s || !s->object || (flags & ~UINT(D3D10_DDI_CLEAR_DEPTH|D3D10_DDI_CLEAR_STENCIL))) {
            report_ddi_error(*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner,E_INVALIDARG); return;
        }
        UINT converted=0;
        if (flags & D3D10_DDI_CLEAR_DEPTH) converted|=D3D11_CLEAR_DEPTH;
        if (flags & D3D10_DDI_CLEAR_STENCIL) converted|=D3D11_CLEAR_STENCIL;
        if (converted) context.ClearDepthStencilView(s->object,converted,depth,stencil);
    });
}
}
void install_dsv_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateDepthStencilViewSize=size; t.pfnCreateDepthStencilView=create;
    t.pfnDestroyDepthStencilView=destroy; t.pfnClearDepthStencilView=clear;
}
}
