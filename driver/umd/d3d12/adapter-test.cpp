// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12umddi.h>
#include <cassert>
#include <cstring>
#include <cstdio>
static HRESULT APIENTRY query(HANDLE,const D3DDDICB_QUERYADAPTERINFO*) {return E_NOTIMPL;}
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
    assert(funcs.pfnCreateDevice(a.hAdapter,nullptr)==E_NOTIMPL);
    assert(funcs.pfnFillDDITable(a.hAdapter,D3D12DDI_TABLE_TYPE_DEVICE_CORE,guard,sizeof(guard),0,{})==E_NOTIMPL);
    assert(guard[1]==0x123456);
    assert(funcs.pfnCloseAdapter(a.hAdapter)==S_OK);
    FreeLibrary(dll);puts("adapter export/negotiation/fail-closed tests passed");return 0;
}
