// SPDX-License-Identifier: MIT
#include "ddi-rtv.h"
namespace bc250::umd {
HRESULT convert_rtv(const D3D10DDIARG_CREATERENDERTARGETVIEW &s,UINT layers,UINT samples,D3D11_RENDER_TARGET_VIEW_DESC &out) {
    D3D11_RENDER_TARGET_VIEW_DESC d{}; d.Format=s.Format;
    switch(s.ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER: case D3D11DDIRESOURCE_BUFFEREX:
        d.ViewDimension=D3D11_RTV_DIMENSION_BUFFER;
        d.Buffer={s.Buffer.FirstElement,s.Buffer.NumElements}; break;
    case D3D10DDIRESOURCE_TEXTURE1D: {
        const auto &v=s.Tex1D;
        if (!v.ArraySize || v.FirstArraySlice>=layers || v.ArraySize>layers-v.FirstArraySlice) return E_INVALIDARG;
        if (layers>1) { d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE1DARRAY; d.Texture1DArray={v.MipSlice,v.FirstArraySlice,v.ArraySize}; }
        else { d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE1D; d.Texture1D.MipSlice=v.MipSlice; }
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE2D: case D3D10DDIRESOURCE_TEXTURECUBE: {
        UINT mip,first,count;
        if (s.ResourceDimension==D3D10DDIRESOURCE_TEXTURECUBE) { mip=s.TexCube.MipSlice; first=s.TexCube.FirstArraySlice; count=s.TexCube.ArraySize; }
        else { mip=s.Tex2D.MipSlice; first=s.Tex2D.FirstArraySlice; count=s.Tex2D.ArraySize; }
        if (!samples || !count || first>=layers || count>layers-first || (samples>1 && mip)) return E_INVALIDARG;
        if (samples>1) {
            if (layers>1) { d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY; d.Texture2DMSArray={first,count}; }
            else d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DMS;
        } else if (layers>1) { d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray={mip,first,count}; }
        else { d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D; d.Texture2D.MipSlice=mip; }
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE3D:
        d.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE3D;
        d.Texture3D={s.Tex3D.MipSlice,s.Tex3D.FirstW,s.Tex3D.WSize}; break;
    default: return E_INVALIDARG;
    }
    out=d; return S_OK;
}
namespace {
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D10DDIARG_CREATERENDERTARGETVIEW *) { return sizeof(DdiRenderTargetView); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D10DDIARG_CREATERENDERTARGETVIEW *desc,
    D3D10DDI_HRENDERTARGETVIEW handle,D3D10DDI_HRTRENDERTARGETVIEW) {
    auto *s=static_cast<DdiRenderTargetView *>(handle.pDrvPrivate); if (s) s->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        auto *r=desc ? static_cast<DdiResource *>(desc->hDrvResource.pDrvPrivate) : nullptr;
        if (!s || !r || !r->object || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        UINT layers=1,samples=1;
        D3D11_RESOURCE_DIMENSION actual{}; r->object->GetType(&actual);
        switch(desc->ResourceDimension) {
        case D3D10DDIRESOURCE_BUFFER: case D3D11DDIRESOURCE_BUFFEREX:
            if (actual!=D3D11_RESOURCE_DIMENSION_BUFFER) { report_ddi_error(owner,E_INVALIDARG); return; } break;
        case D3D10DDIRESOURCE_TEXTURE1D: {
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE1D) { report_ddi_error(owner,E_INVALIDARG); return; }
            D3D11_TEXTURE1D_DESC d{}; static_cast<ID3D11Texture1D *>(r->object)->GetDesc(&d); layers=d.ArraySize; break;
        }
        case D3D10DDIRESOURCE_TEXTURE2D: case D3D10DDIRESOURCE_TEXTURECUBE: {
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE2D) { report_ddi_error(owner,E_INVALIDARG); return; }
            D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D *>(r->object)->GetDesc(&d); layers=d.ArraySize; samples=d.SampleDesc.Count; break;
        }
        case D3D10DDIRESOURCE_TEXTURE3D:
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE3D) { report_ddi_error(owner,E_INVALIDARG); return; } break;
        default: report_ddi_error(owner,E_INVALIDARG); return;
        }
        D3D11_RENDER_TARGET_VIEW_DESC d{};
        HRESULT hr=convert_rtv(*desc,layers,samples,d);
        if (SUCCEEDED(hr)) hr=owner.device()->CreateRenderTargetView(r->object,&d,&s->object);
        if (FAILED(hr)) {
            if (s->object) { s->object->Release(); s->object=nullptr; }
            report_ddi_error(owner,hr);
        } else if (!s->object) report_ddi_error(owner,E_FAIL);
    });
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HRENDERTARGETVIEW handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto *s=static_cast<DdiRenderTargetView *>(handle.pDrvPrivate);
        if (s && s->object) { s->object->Release(); s->object=nullptr; }
    });
}
void APIENTRY clear(D3D10DDI_HDEVICE h,D3D10DDI_HRENDERTARGETVIEW handle,FLOAT color[4]) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *s=static_cast<DdiRenderTargetView *>(handle.pDrvPrivate);
        if (!s || !s->object || !color) { report_ddi_error(*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner,E_INVALIDARG); return; }
        context.ClearRenderTargetView(s->object,color);
    });
}
}
void install_rtv_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateRenderTargetViewSize=size; t.pfnCreateRenderTargetView=create;
    t.pfnDestroyRenderTargetView=destroy; t.pfnClearRenderTargetView=clear;
}
}
