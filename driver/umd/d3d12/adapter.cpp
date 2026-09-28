// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12umddi.h>
#include <cstdio>
#include <new>
namespace {
struct Adapter { D3D12DDI_HRTADAPTER runtime; D3DDDI_ADAPTERCALLBACKS callbacks; };
void trace(const char* operation,unsigned long long value=0) noexcept {
    fprintf(stderr,"d3d12-ddi %s %llu\n",operation,value);fflush(stderr);
}
SIZE_T APIENTRY device_size(D3D12DDI_HADAPTER,const D3D12DDIARG_CALCPRIVATEDEVICESIZE* a) {
    trace("CalcPrivateDeviceSize",a?a->Interface:0);return 0;
}
HRESULT APIENTRY create_device(D3D12DDI_HADAPTER,const D3D12DDIARG_CREATEDEVICE_0003* a) {
    trace("CreateDevice-unimplemented",a?a->Interface:0);return E_NOTIMPL;
}
HRESULT APIENTRY close_adapter(D3D12DDI_HADAPTER h) {
    if(!h.pDrvPrivate) return E_INVALIDARG;
    delete static_cast<Adapter*>(h.pDrvPrivate);trace("CloseAdapter");return S_OK;
}
HRESULT APIENTRY versions(D3D12DDI_HADAPTER h,UINT32* count,UINT64* values) {
    if(!h.pDrvPrivate || !count) return E_INVALIDARG;
    const UINT32 capacity=*count;*count=1;trace("GetSupportedVersions",capacity);
    if(!values) return S_OK;
    if(capacity<1) return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
    // Diagnostic negotiation target only. Device/table creation remains fail-closed.
    values[0]=D3D12DDI_SUPPORTED_0108;return S_OK;
}
HRESULT APIENTRY caps(D3D12DDI_HADAPTER,const D3D12DDIARG_GETCAPS* a) {
    if(!a || (!a->pData && a->DataSize)) return E_INVALIDARG;
    trace("GetCaps-unimplemented",a->Type);return E_NOTIMPL;
}
HRESULT APIENTRY optional_tables(D3D12DDI_HADAPTER h,UINT32* count,D3D12DDI_TABLE_REQUEST*) {
    if(!h.pDrvPrivate || !count) return E_INVALIDARG;
    *count=0;trace("GetOptionalDDITables");return S_OK;
}
HRESULT APIENTRY fill_table(D3D12DDI_HADAPTER,D3D12DDI_TABLE_TYPE type,void*,SIZE_T,UINT,D3D12DDI_HRTTABLE) {
    trace("FillDDITable-unimplemented",type);return E_NOTIMPL;
}
void APIENTRY destroy_device(D3D12DDI_HDEVICE) {trace("DestroyDevice-unimplemented");}
}
extern "C" __declspec(dllexport) HRESULT APIENTRY OpenAdapter12(D3D12DDIARG_OPENADAPTER* a) {
    if(!a) return E_INVALIDARG;
    a->hAdapter.pDrvPrivate=nullptr;
    if(!a->pAdapterFuncs) return E_INVALIDARG;
    *a->pAdapterFuncs={};
    if(!a->pAdapterCallbacks || !a->pAdapterCallbacks->pfnQueryAdapterInfoCb) return E_INVALIDARG;
    auto adapter=new(std::nothrow) Adapter{a->hRTAdapter,*a->pAdapterCallbacks};
    if(!adapter) return E_OUTOFMEMORY;
    *a->pAdapterFuncs={device_size,create_device,close_adapter,versions,caps,optional_tables,fill_table,destroy_device};
    a->hAdapter.pDrvPrivate=adapter;trace("OpenAdapter12");return S_OK;
}
