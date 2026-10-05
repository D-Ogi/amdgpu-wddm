// SPDX-License-Identifier: MIT
#include "ddi-uav.h"
#include "ddi-rtv.h"
#include "ddi-buffer-binding.h"
#include <array>
namespace bc250::umd {
HRESULT convert_uav(const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW &s,UINT layers,UINT samples,UINT plane,
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 &out) {
    if (samples!=1) return E_INVALIDARG;
    if (plane && s.ResourceDimension!=D3D10DDIRESOURCE_TEXTURE2D) return E_INVALIDARG;
    D3D11_UNORDERED_ACCESS_VIEW_DESC1 d{}; d.Format=s.Format;
    if (s.ResourceDimension==D3D10DDIRESOURCE_BUFFER || s.ResourceDimension==D3D11DDIRESOURCE_BUFFEREX) {
        constexpr UINT known=D3D11_DDI_BUFFER_UAV_FLAG_RAW|D3D11_DDI_BUFFER_UAV_FLAG_APPEND|D3D11_DDI_BUFFER_UAV_FLAG_COUNTER;
        const UINT f=s.Buffer.Flags;
        if (f & ~known) return E_INVALIDARG;
        if ((f & D3D11_DDI_BUFFER_UAV_FLAG_APPEND) && (f & D3D11_DDI_BUFFER_UAV_FLAG_COUNTER)) return E_INVALIDARG;
        if ((f & D3D11_DDI_BUFFER_UAV_FLAG_RAW) && (f & (D3D11_DDI_BUFFER_UAV_FLAG_APPEND|D3D11_DDI_BUFFER_UAV_FLAG_COUNTER))) return E_INVALIDARG;
        d.ViewDimension=D3D11_UAV_DIMENSION_BUFFER; d.Buffer.FirstElement=s.Buffer.FirstElement; d.Buffer.NumElements=s.Buffer.NumElements;
        if (f & D3D11_DDI_BUFFER_UAV_FLAG_RAW) d.Buffer.Flags|=D3D11_BUFFER_UAV_FLAG_RAW;
        if (f & D3D11_DDI_BUFFER_UAV_FLAG_APPEND) d.Buffer.Flags|=D3D11_BUFFER_UAV_FLAG_APPEND;
        if (f & D3D11_DDI_BUFFER_UAV_FLAG_COUNTER) d.Buffer.Flags|=D3D11_BUFFER_UAV_FLAG_COUNTER;
    } else {
        D3D10DDIARG_CREATERENDERTARGETVIEW r{}; r.Format=s.Format; r.ResourceDimension=s.ResourceDimension;
        switch(s.ResourceDimension) {
        case D3D10DDIRESOURCE_TEXTURE1D: r.Tex1D={s.Tex1D.MipSlice,s.Tex1D.FirstArraySlice,s.Tex1D.ArraySize}; break;
        case D3D10DDIRESOURCE_TEXTURE2D: r.Tex2D={s.Tex2D.MipSlice,s.Tex2D.FirstArraySlice,s.Tex2D.ArraySize}; break;
        case D3D10DDIRESOURCE_TEXTURE3D: r.Tex3D={s.Tex3D.MipSlice,s.Tex3D.FirstW,s.Tex3D.WSize}; break;
        default: return E_INVALIDARG; // Cube storage is viewed as 2D array faces.
        }
        D3D11_RENDER_TARGET_VIEW_DESC1 v{};
        // The render-target conversion already validates the array range and the plane.
        HRESULT hr=convert_rtv(r,layers,1,plane,v); if (FAILED(hr)) return hr;
        switch(v.ViewDimension) {
        case D3D11_RTV_DIMENSION_TEXTURE1D: d.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE1D; d.Texture1D.MipSlice=v.Texture1D.MipSlice; break;
        case D3D11_RTV_DIMENSION_TEXTURE1DARRAY: d.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE1DARRAY; d.Texture1DArray={v.Texture1DArray.MipSlice,v.Texture1DArray.FirstArraySlice,v.Texture1DArray.ArraySize}; break;
        case D3D11_RTV_DIMENSION_TEXTURE2D: d.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D; d.Texture2D={v.Texture2D.MipSlice,v.Texture2D.PlaneSlice}; break;
        case D3D11_RTV_DIMENSION_TEXTURE2DARRAY: d.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2DARRAY; d.Texture2DArray={v.Texture2DArray.MipSlice,v.Texture2DArray.FirstArraySlice,v.Texture2DArray.ArraySize,v.Texture2DArray.PlaneSlice}; break;
        case D3D11_RTV_DIMENSION_TEXTURE3D: d.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE3D; d.Texture3D={v.Texture3D.MipSlice,v.Texture3D.FirstWSlice,v.Texture3D.WSize}; break;
        default: return E_INVALIDARG;
        }
    }
    out=d; return S_OK;
}
namespace {
DeviceOwner &owner(D3D10DDI_HDEVICE h) { return *static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner; }
ID3D11UnorderedAccessView *view(D3D11DDI_HUNORDEREDACCESSVIEW h) {
    auto *s=static_cast<DdiUnorderedAccessView *>(h.pDrvPrivate); return s ? s->object : nullptr;
}
SIZE_T APIENTRY size(D3D10DDI_HDEVICE,const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *) { return sizeof(DdiUnorderedAccessView); }
}
// CreateUnorderedAccessView may report E_OUTOFMEMORY or D3DDDIERR_DEVICEREMOVED, and nothing else.
void create_unordered_access_view(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *desc,
    UINT plane,D3D11DDI_HUNORDEREDACCESSVIEW handle,D3D11DDI_HRTUNORDEREDACCESSVIEW) {
    auto *s=static_cast<DdiUnorderedAccessView *>(handle.pDrvPrivate); if (s) s->object=nullptr;
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &o=owner(h); auto *r=desc ? static_cast<DdiResource *>(desc->hDrvResource.pDrvPrivate) : nullptr;
        const auto refuse=[&] { report_ddi_error(o,D3DDDIERR_DEVICEREMOVED,DdiErrorClass::out_of_memory); };
        if (!s || !r || !r->object || !o.device()) { refuse(); return; }
        UINT layers=1,samples=1; D3D11_RESOURCE_DIMENSION actual{}; r->object->GetType(&actual);
        switch(desc->ResourceDimension) {
        case D3D10DDIRESOURCE_BUFFER: case D3D11DDIRESOURCE_BUFFEREX:
            if (actual!=D3D11_RESOURCE_DIMENSION_BUFFER) { refuse(); return; } break;
        case D3D10DDIRESOURCE_TEXTURE1D: {
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE1D) { refuse(); return; }
            D3D11_TEXTURE1D_DESC d{}; static_cast<ID3D11Texture1D *>(r->object)->GetDesc(&d); layers=d.ArraySize; break;
        }
        case D3D10DDIRESOURCE_TEXTURE2D: {
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE2D) { refuse(); return; }
            D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D *>(r->object)->GetDesc(&d); layers=d.ArraySize; samples=d.SampleDesc.Count; break;
        }
        case D3D10DDIRESOURCE_TEXTURE3D:
            if (actual!=D3D11_RESOURCE_DIMENSION_TEXTURE3D) { refuse(); return; } break;
        default: refuse(); return;
        }
        D3D11_UNORDERED_ACCESS_VIEW_DESC1 d{}; HRESULT hr=convert_uav(*desc,layers,samples,plane,d);
        ID3D11UnorderedAccessView1 *created=nullptr;
        if (SUCCEEDED(hr)) hr=o.device()->CreateUnorderedAccessView1(r->object,&d,&created);
        if (FAILED(hr)) {
            if (created) created->Release();
            report_ddi_error(o,hr,DdiErrorClass::out_of_memory);
        } else if (!created) refuse();
        else s->object=created;
    },DdiErrorClass::out_of_memory);
}
namespace {
void APIENTRY create(D3D10DDI_HDEVICE h,const D3D11DDIARG_CREATEUNORDEREDACCESSVIEW *desc,
    D3D11DDI_HUNORDEREDACCESSVIEW handle,D3D11DDI_HRTUNORDEREDACCESSVIEW runtime) {
    create_unordered_access_view(h,desc,0,handle,runtime); // The D3D11 argument has no plane.
}
void APIENTRY destroy(D3D10DDI_HDEVICE h,D3D11DDI_HUNORDEREDACCESSVIEW handle) {
    enter_context(h,[&](ID3D11DeviceContext4 &) { auto *s=static_cast<DdiUnorderedAccessView *>(handle.pDrvPrivate); if (s && s->object) { s->object->Release(); s->object=nullptr; } });
}
template<typename T,auto Clear> void APIENTRY clear(D3D10DDI_HDEVICE h,D3D11DDI_HUNORDEREDACCESSVIEW handle,const T values[4]) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        auto *v=view(handle); if (!v || !values) { report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED); return; }
        (c.*Clear)(v,values);
    });
}
void APIENTRY bind(D3D10DDI_HDEVICE h,UINT first,UINT count,const D3D11DDI_HUNORDEREDACCESSVIEW *handles,const UINT *initialCounts) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        constexpr UINT limit=D3D11_1_UAV_SLOT_COUNT;
        if (first>=limit || count>limit-first || (count && !handles)) { report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED); return; }
        std::array<ID3D11UnorderedAccessView *,limit> views{};
        for (UINT i=0;i<count;++i) {
            views[i]=view(handles[i]);
            if (handles[i].pDrvPrivate && !views[i]) { report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED); return; }
        }
        // Preserve UINT_MAX (keep counter) and optional null count array.
        c.CSSetUnorderedAccessViews(first,count,views.data(),initialCounts);
    });
}
void APIENTRY copy_count(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE dst,UINT offset,D3D11DDI_HUNORDEREDACCESSVIEW src) {
    enter_context(h,[&](ID3D11DeviceContext4 &c) {
        ID3D11Buffer *b=nullptr; auto *v=view(src);
        if (FAILED(resource_buffer(dst,b)) || !b || !v || offset%4) { report_ddi_error(owner(h),D3DDDIERR_DEVICEREMOVED); return; }
        c.CopyStructureCount(b,offset,v);
    });
}
}
void install_uav_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnCalcPrivateUnorderedAccessViewSize=size; t.pfnCreateUnorderedAccessView=create; t.pfnDestroyUnorderedAccessView=destroy;
    t.pfnClearUnorderedAccessViewUint=clear<UINT,&ID3D11DeviceContext4::ClearUnorderedAccessViewUint>;
    t.pfnClearUnorderedAccessViewFloat=clear<FLOAT,&ID3D11DeviceContext4::ClearUnorderedAccessViewFloat>;
    t.pfnCsSetUnorderedAccessViews=bind; t.pfnCopyStructureCount=copy_count;
}
}
