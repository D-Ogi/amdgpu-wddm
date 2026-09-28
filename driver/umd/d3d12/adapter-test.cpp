// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12umddi.h>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>
static HRESULT APIENTRY query(HANDLE,const D3DDDICB_QUERYADAPTERINFO*) {return E_NOTIMPL;}
static void APIENTRY error(D3D10DDI_HRTDEVICE,HRESULT) {}
static HRESULT APIENTRY create_context(D3D12DDI_HRTCOMMANDQUEUE,D3DDDICB_CREATECONTEXTVIRTUAL*) {return E_NOTIMPL;}
static HRESULT APIENTRY destroy_context(D3D12DDI_HRTCOMMANDQUEUE,const D3DDDICB_DESTROYCONTEXT*) {return E_NOTIMPL;}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    HMODULE dll=LoadLibraryA(argv[1]);assert(dll);
    auto proc=GetProcAddress(dll,"OpenAdapter12");PFND3D12DDI_OPENADAPTER open=nullptr;
    static_assert(sizeof(open)==sizeof(proc));memcpy(&open,&proc,sizeof(open));assert(open);
    assert(open(nullptr)==E_INVALIDARG);
    D3D12DDI_ADAPTERFUNCS funcs{};D3DDDI_ADAPTERCALLBACKS callbacks{};
    D3D12DDIARG_OPENADAPTER a{};a.pAdapterFuncs=&funcs;
    assert(open(&a)==E_INVALIDARG && !a.hAdapter.pDrvPrivate && !funcs.pfnCreateDevice);
    callbacks.pfnQueryAdapterInfoCb=query;a.pAdapterCallbacks=&callbacks;
    assert(open(&a)==S_OK && a.hAdapter.pDrvPrivate);
    UINT32 count=0;assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,nullptr)==S_OK && count==1);
    UINT64 guard[2]={0xabcdef,0x123456};count=0;
    assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,guard)==HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    assert(count==1 && guard[0]==0xabcdef && guard[1]==0x123456);
    assert(funcs.pfnGetSupportedVersions(a.hAdapter,&count,guard)==S_OK && guard[0]==D3D12DDI_SUPPORTED_0108 && guard[1]==0x123456);
    assert(funcs.pfnCreateDevice(a.hAdapter,nullptr)==E_INVALIDARG);
    D3D12DDIARG_CALCPRIVATEDEVICESIZE sizeArgs{};
    assert(funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs)==0);
    sizeArgs.Interface=D3D12DDI_INTERFACE_VERSION_R8;sizeArgs.Version=D3D12DDI_BUILD_VERSION_0108<<16;
    SIZE_T size=funcs.pfnCalcPrivateDeviceSize(a.hAdapter,&sizeArgs);assert(size);
    void* storage=::operator new(size);
    D3D12DDIARG_CREATEDEVICE_0003 device{};device.hDrvDevice.pDrvPrivate=storage;
    device.Interface=sizeArgs.Interface;device.Version=sizeArgs.Version;
    assert(funcs.pfnCreateDevice(a.hAdapter,&device)==E_INVALIDARG);
    D3D12DDI_CORELAYER_DEVICECALLBACKS_0062 um{};D3DDDI_DEVICECALLBACKS km{};
    um.pfnSetErrorCb=error;um.pfnCreateContextVirtualCb=create_context;um.pfnDestroyContextCb=destroy_context;
    device.p12UMCallbacks_0062=&um;device.pKTCallbacks=&km;
    device.Version=0;assert(funcs.pfnCreateDevice(a.hAdapter,&device)==E_NOINTERFACE);
    device.Version=sizeArgs.Version;assert(funcs.pfnCreateDevice(a.hAdapter,&device)==S_OK);
    assert(funcs.pfnCloseAdapter(a.hAdapter)==E_UNEXPECTED);
    funcs.pfnDestroyDevice(device.hDrvDevice);::operator delete(storage);
    assert(funcs.pfnFillDDITable(a.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,guard,sizeof(guard),0,{})==E_NOTIMPL);
    assert(guard[1]==0x123456);
    assert(funcs.pfnCloseAdapter(a.hAdapter)==S_OK);
    FreeLibrary(dll);puts("adapter export/negotiation/fail-closed tests passed");return 0;
}
