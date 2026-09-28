// SPDX-License-Identifier: MIT
#include "ddi-resource-status.h"
namespace bc250::umd {
namespace {
HRESULT subresource_count(ID3D11Resource *r,UINT &out) {
    UINT mips=1,layers=1; D3D11_RESOURCE_DIMENSION type{}; r->GetType(&type);
    switch(type) {
    case D3D11_RESOURCE_DIMENSION_BUFFER: break;
    case D3D11_RESOURCE_DIMENSION_TEXTURE1D: {
        D3D11_TEXTURE1D_DESC d{}; static_cast<ID3D11Texture1D *>(r)->GetDesc(&d); mips=d.MipLevels; layers=d.ArraySize; break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE2D: {
        D3D11_TEXTURE2D_DESC d{}; static_cast<ID3D11Texture2D *>(r)->GetDesc(&d); mips=d.MipLevels; layers=d.ArraySize; break;
    }
    case D3D11_RESOURCE_DIMENSION_TEXTURE3D: {
        D3D11_TEXTURE3D_DESC d{}; static_cast<ID3D11Texture3D *>(r)->GetDesc(&d); mips=d.MipLevels; break;
    }
    default: return E_INVALIDARG;
    }
    if (!mips || !layers || layers>UINT_MAX/mips) return E_INVALIDARG;
    out=mips*layers; return S_OK;
}
BOOL APIENTRY busy(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle) {
    BOOL result=TRUE; // Failure must not authorize access to outstanding work.
    enter_context(h,[&](ID3D11DeviceContext4 &) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
        if (!s || !s->object || !owner.engine() || !owner.device()) { report_ddi_error(owner,E_INVALIDARG); return; }
        HRESULT hr=owner.device()->GetDeviceRemovedReason();
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        if (owner.bridge().device_lost || owner.bridge().submission_failed) { report_ddi_error(owner,DXGI_ERROR_DEVICE_REMOVED); return; }
        UINT count=0; hr=subresource_count(s->object,count);
        if (SUCCEEDED(hr)) hr=query_subresources_idle(count,[&](UINT i) { return owner.engine()->IsResourceBusy(s->object,i); });
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        result=hr==S_FALSE;
    });
    return result;
}
void APIENTRY min_lod(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle,FLOAT lod) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
        if (!s || !s->object) { report_ddi_error(*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner,E_INVALIDARG); return; }
        context.SetResourceMinLOD(s->object,lod);
    });
}
}
void install_resource_status_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnResourceIsStagingBusy=busy; t.pfnSetResourceMinLOD=min_lod;
}
}
