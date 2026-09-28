// SPDX-License-Identifier: MIT
#include "ddi-map.h"
namespace bc250::umd {
HRESULT convert_map(D3D10_DDI_MAP type,UINT flags,D3D11_MAP &outType,UINT &outFlags) {
    if (flags & ~UINT(D3D10_DDI_MAP_FLAG_DONOTWAIT)) return E_INVALIDARG;
    D3D11_MAP converted;
    switch(type) {
    case D3D10_DDI_MAP_READ: converted=D3D11_MAP_READ; break;
    case D3D10_DDI_MAP_WRITE: converted=D3D11_MAP_WRITE; break;
    case D3D10_DDI_MAP_READWRITE: converted=D3D11_MAP_READ_WRITE; break;
    case D3D10_DDI_MAP_WRITE_DISCARD: converted=D3D11_MAP_WRITE_DISCARD; break;
    case D3D10_DDI_MAP_WRITE_NOOVERWRITE: converted=D3D11_MAP_WRITE_NO_OVERWRITE; break;
    default: return E_INVALIDARG;
    }
    outType=converted;
    // DDI uses bit20; COM uses bit20 in this SDK too, but map explicitly.
    outFlags=(flags & D3D10_DDI_MAP_FLAG_DONOTWAIT) ? D3D11_MAP_FLAG_DO_NOT_WAIT : 0;
    return S_OK;
}
namespace {
void APIENTRY map(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle,UINT subresource,
    D3D10_DDI_MAP type,UINT flags,D3D10DDI_MAPPED_SUBRESOURCE *output) {
    if (output) *output={};
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
        if (!output || !s || !s->object) { report_ddi_error(owner,E_INVALIDARG); return; }
        D3D11_MAP mappedType{}; UINT mappedFlags=0;
        HRESULT hr=convert_map(type,flags,mappedType,mappedFlags);
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        D3D11_MAPPED_SUBRESOURCE result{};
        hr=ddi_map_status(context.Map(s->object,subresource,mappedType,mappedFlags,&result),
            (flags & D3D10_DDI_MAP_FLAG_DONOTWAIT)!=0);
        // Translate API status to DDI status. Do not retry a
        // DO_NOT_WAIT request or expose a stale pointer on failure.
        if (FAILED(hr)) { report_ddi_error(owner,hr); return; }
        if (!result.pData) { context.Unmap(s->object,subresource); report_ddi_error(owner,E_FAIL); return; }
        output->pData=result.pData; output->RowPitch=result.RowPitch; output->DepthPitch=result.DepthPitch;
    });
}
void APIENTRY unmap(D3D10DDI_HDEVICE h,D3D10DDI_HRESOURCE handle,UINT subresource) {
    enter_context(h,[&](ID3D11DeviceContext4 &context) {
        auto &owner=*static_cast<DdiDeviceHandle *>(h.pDrvPrivate)->owner;
        auto *s=static_cast<DdiResource *>(handle.pDrvPrivate);
        if (!s || !s->object) { report_ddi_error(owner,E_INVALIDARG); return; }
        context.Unmap(s->object,subresource);
    });
}
}
void install_map_ddi(D3D11_1DDI_DEVICEFUNCS &t) {
    t.pfnResourceMap=map; t.pfnResourceUnmap=unmap;
    t.pfnStagingResourceMap=map; t.pfnStagingResourceUnmap=unmap;
    t.pfnDynamicIABufferMapNoOverwrite=map;
    t.pfnDynamicIABufferMapDiscard=map; t.pfnDynamicIABufferUnmap=unmap;
    t.pfnDynamicConstantBufferMapNoOverwrite=map;
    t.pfnDynamicConstantBufferMapDiscard=map; t.pfnDynamicConstantBufferUnmap=unmap;
    t.pfnDynamicResourceMapDiscard=map; t.pfnDynamicResourceUnmap=unmap;
}
}
