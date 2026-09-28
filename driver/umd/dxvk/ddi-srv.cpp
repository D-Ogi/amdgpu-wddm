// SPDX-License-Identifier: MIT
#include "ddi-srv.h"
#include <array>
namespace bc250::umd {
namespace {
bool layer_range(UINT first,UINT count,UINT layers) { return count && first<layers && count<=layers-first; }
}
HRESULT convert_srv(const D3D11DDIARG_CREATESHADERRESOURCEVIEW &s,UINT layers,UINT samples,D3D11_SHADER_RESOURCE_VIEW_DESC &out) {
    D3D11_SHADER_RESOURCE_VIEW_DESC d{}; d.Format=s.Format;
    switch(s.ResourceDimension) {
    case D3D10DDIRESOURCE_BUFFER:
        d.ViewDimension=D3D11_SRV_DIMENSION_BUFFER; d.Buffer={s.Buffer.FirstElement,s.Buffer.NumElements}; break;
    case D3D11DDIRESOURCE_BUFFEREX:
        if (s.BufferEx.Flags & ~UINT(D3D11_DDI_BUFFEREX_SRV_FLAG_RAW)) return E_INVALIDARG;
        d.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
        d.BufferEx={s.BufferEx.FirstElement,s.BufferEx.NumElements,
            (s.BufferEx.Flags & D3D11_DDI_BUFFEREX_SRV_FLAG_RAW) ? UINT(D3D11_BUFFEREX_SRV_FLAG_RAW) : 0u}; break;
    case D3D10DDIRESOURCE_TEXTURE1D: {
        const auto &v=s.Tex1D;
        if (!layer_range(v.FirstArraySlice,v.ArraySize,layers)) return E_INVALIDARG;
        if (layers>1) { d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE1DARRAY; d.Texture1DArray={v.MostDetailedMip,v.MipLevels,v.FirstArraySlice,v.ArraySize}; }
        else { d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE1D; d.Texture1D={v.MostDetailedMip,v.MipLevels}; }
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE2D: {
        const auto &v=s.Tex2D;
        if (!samples || !layer_range(v.FirstArraySlice,v.ArraySize,layers)) return E_INVALIDARG;
        if (samples>1) {
            if (v.MostDetailedMip || v.MipLevels!=1) return E_INVALIDARG;
            if (layers>1) { d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMSARRAY; d.Texture2DMSArray={v.FirstArraySlice,v.ArraySize}; }
            else d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else if (layers>1) { d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray={v.MostDetailedMip,v.MipLevels,v.FirstArraySlice,v.ArraySize}; }
        else { d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; d.Texture2D={v.MostDetailedMip,v.MipLevels}; }
        break;
    }
    case D3D10DDIRESOURCE_TEXTURE3D:
        d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE3D; d.Texture3D={s.Tex3D.MostDetailedMip,s.Tex3D.MipLevels}; break;
    case D3D10DDIRESOURCE_TEXTURECUBE: {
        const auto &v=s.TexCube;
        if (samples!=1 || layers%6 || v.First2DArrayFace>=layers ||
            !v.NumCubes || v.NumCubes>(layers-v.First2DArrayFace)/6) return E_INVALIDARG;
        if (layers==6) { d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBE; d.TextureCube={v.MostDetailedMip,v.MipLevels}; }
        else { d.ViewDimension=D3D11_SRV_DIMENSION_TEXTURECUBEARRAY; d.TextureCubeArray={v.MostDetailedMip,v.MipLevels,v.First2DArrayFace,v.NumCubes}; }
        break;
    }
    default: return E_INVALIDARG;
    }
    out=d; return S_OK;
}
namespace {
DeviceOwner &owner(D3D10DDI_HDEVICE h) { return *static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner; }
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATESHADERRESOURCEVIEW *) { return sizeof(DdiShaderResourceView); }
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATESHADERRESOURCEVIEW *desc,
    D3D10DDI_HSHADERRESOURCEVIEW handle,D3D10DDI_HRTSHADERRESOURCEVIEW) {
    auto *s=static_cast<DdiShaderResourceView *>(handle.pDrvPrivate); if (s) s->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &o=owner(h); auto *r=desc ? static_cast<DdiResource *>(desc->hDrvResource.pDrvPrivate) : nullptr;
        if (!s || !r || !r->object || !o.device()) { report_ddi_error(o,E_INVALIDARG); return; }
        UINT layers=1,samples=1; D3D11_RESOURCE_DIMENSION actual{}; r->object->GetType(&actual);
        switch(desc->ResourceDimension) {
        case D3D10DDIRESOURCE_BUFFER: case D3D11DDIRESOURCE_BUFFEREX:
            if (actual!=D3D11_RESOURCE_DIMENSION_BUFFER) { report_ddi_error(o,E_INVALIDARG); return; } break;
        case D3D10DDIRESOURCE_TEXTURE1D: {
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE1D) { report_ddi_error(o,E_INVALIDARG); return; }
            D3D11_TEXTURE1D_DESC d{}; static_cast<ID3D11Texture1D *>(r->object)->GetDesc(&d); layers=d.ArraySize; break;
        }
        case D3D10DDIRESOURCE_TEXTURE2D: case D3D10DDIRESOURCE_TEXTURECUBE: {
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE2D) { report_ddi_error(o,E_INVALIDARG); return; }
            D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D *>(r->object)->GetDesc(&d);
            if (desc->ResourceDimension==D3D10DDIRESOURCE_TEXTURECUBE && !(d.MiscFlags&D3D11_RESOURCE_MISC_TEXTURECUBE)) { report_ddi_error(o,E_INVALIDARG); return; }
            layers=d.ArraySize; samples=d.SampleDesc.Count; break;
        }
        case D3D10DDIRESOURCE_TEXTURE3D:
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE3D) { report_ddi_error(o,E_INVALIDARG); return; } break;
        default: report_ddi_error(o,E_INVALIDARG); return;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC d{}; HRESULT hr=convert_srv(*desc,layers,samples,d);
        if (SUCCEEDED(hr)) hr=o.device()->CreateShaderResourceView(r->object,&d,&s->object);
        if (FAILED(hr)) {
            if (s->object) { s->object->Release(); s->object=nullptr; }
            report_ddi_error(o,hr);
        } else if (!s->object) report_ddi_error(o,E_FAIL);
    });
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D10DDI_HSHADERRESOURCEVIEW handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) { auto *s=static_cast<DdiShaderResourceView *>(handle.pDrvPrivate); if (s && s->object) { s->object->Release(); s->object=nullptr; } });
}
template<auto Set> void APIENTRY bind(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D10DDI_HSHADERRESOURCEVIEW *handles) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        constexpr UINT limit=D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
        if (first>=limit || count>limit-first || (count && !handles)) { report_ddi_error(owner(h),E_INVALIDARG); return; }
        std::array<ID3D11ShaderResourceView *,limit> views{};
        for (UINT i=0;i<count;++i) {
            auto *s=static_cast<DdiShaderResourceView *>(handles[i].pDrvPrivate);
            if (s && !s->object) { report_ddi_error(owner(h),E_INVALIDARG); return; }
            views[i]=s ? s->object : nullptr;
        }
        (c.*Set)(first,count,views.data());
    });
}
void APIENTRY generate(D3D10DDI_HDEVICE h,D3D10DDI_HSHADERRESOURCEVIEW handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *s=static_cast<DdiShaderResourceView *>(handle.pDrvPrivate);
        if (!s || !s->object) { report_ddi_error(owner(h),E_INVALIDARG); return; }
        c.GenerateMips(s->object);
    });
}
}
void install_srv_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateShaderResourceViewSize=size; t.pfnCreateShaderResourceView=create; t.pfnDestroyShaderResourceView=destroy;
    t.pfnVsSetShaderResources=bind<&ID3D11DeviceContext4::VSSetShaderResources>;
    t.pfnPsSetShaderResources=bind<&ID3D11DeviceContext4::PSSetShaderResources>;
    t.pfnGsSetShaderResources=bind<&ID3D11DeviceContext4::GSSetShaderResources>;
    t.pfnHsSetShaderResources=bind<&ID3D11DeviceContext4::HSSetShaderResources>;
    t.pfnDsSetShaderResources=bind<&ID3D11DeviceContext4::DSSetShaderResources>;
    t.pfnCsSetShaderResources=bind<&ID3D11DeviceContext4::CSSetShaderResources>;
    t.pfnGenMips=generate;
}
}
