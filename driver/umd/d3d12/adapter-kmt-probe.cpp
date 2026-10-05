// SPDX-License-Identifier: MIT
// Read-only live contract probe. This is a KMT-backed test callback, not the D3D12 runtime.
#include <windows.h>
#include <d3dkmthk.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include "adapter-contract.h"
using Microsoft::WRL::ComPtr;
static D3DKMT_HANDLE adapterHandle;
static HRESULT APIENTRY query(HANDLE token,const D3DDDICB_QUERYADAPTERINFO* request){
    if(token!=&adapterHandle || !adapterHandle || !request)return E_INVALIDARG;
    D3DKMT_QUERYADAPTERINFO args{};args.hAdapter=adapterHandle;args.Type=KMTQAITYPE_UMDRIVERPRIVATE;
    args.pPrivateDriverData=request->pPrivateDriverData;args.PrivateDriverDataSize=request->PrivateDriverDataSize;
    NTSTATUS status=D3DKMTQueryAdapterInfo(&args);
    printf("QueryAdapterInfo status=%08lx bytes=%u\n",static_cast<unsigned long>(status),args.PrivateDriverDataSize);
    return status<0?HRESULT_FROM_NT(status):S_OK;
}
static void report_driver_names(){
    for(unsigned version=KMTUMDVERSION_DX9;version<=KMTUMDVERSION_DX12;++version){
        D3DKMT_UMDFILENAMEINFO info{};info.Version=static_cast<KMTUMDVERSION>(version);
        D3DKMT_QUERYADAPTERINFO q{};q.hAdapter=adapterHandle;q.Type=KMTQAITYPE_UMDRIVERNAME;
        q.pPrivateDriverData=&info;q.PrivateDriverDataSize=sizeof(info);
        NTSTATUS status=D3DKMTQueryAdapterInfo(&q);
        info.UmdFileName[MAX_PATH-1]=0;
        const wchar_t* name=wcsrchr(info.UmdFileName,L'\\');name=name?name+1:info.UmdFileName;
        printf("UMD name version=%u status=%08lx basename=%ls\n",version,
            static_cast<unsigned long>(status),status<0?L"<unavailable>":name);
    }
}
static int inspect(const wchar_t* path,LUID luid){
    HMODULE module=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if(!module){printf("LoadLibrary error=%lu\n",GetLastError());return 1;}
    auto address=GetProcAddress(module,"OpenAdapter12");PFND3D12DDI_OPENADAPTER open{};
    static_assert(sizeof(open)==sizeof(address));memcpy(&open,&address,sizeof(open));
    if(!open){FreeLibrary(module);return 1;}
    D3DDDI_ADAPTERCALLBACKS callbacks{};callbacks.pfnQueryAdapterInfoCb=query;
    D3D12DDI_ADAPTERFUNCS functions{};D3D12DDIARG_OPENADAPTER args{};
    args.hRTAdapter.handle=&adapterHandle;args.pAdapterCallbacks=&callbacks;args.pAdapterFuncs=&functions;
    HRESULT hr=open(&args);printf("OpenAdapter12 hr=%08lx\n",static_cast<unsigned long>(hr));
    bool pass=SUCCEEDED(hr) && args.hAdapter.pDrvPrivate && functions.pfnCloseAdapter;
    if(pass){
        // Query through the same public contract separately to compare OS-assigned identity.
        native12::AdapterContract contract;hr=native12::query_contract(args.hRTAdapter,query,contract);
        UINT64 expected=(UINT64(static_cast<UINT32>(luid.HighPart))<<32)|luid.LowPart;
        pass=SUCCEEDED(hr) && contract.luid==expected;
        printf("identity_match=%u contract_version=%u node_mask=%u gfx_rings=%u flags=%u\n",
            unsigned(pass),contract.caps.version,contract.caps.submittable_node_mask,
            contract.caps.hw_ip[AMDGPU_HW_IP_GFX].available_rings,contract.caps.flags);
        hr=functions.pfnCloseAdapter(args.hAdapter);
        printf("CloseAdapter hr=%08lx\n",static_cast<unsigned long>(hr));pass=pass&&SUCCEEDED(hr);
    }
    FreeLibrary(module);return pass?0:1;
}
int wmain(int argc,wchar_t** argv){
    if(argc==2 && !wcscmp(argv[1],L"--help")){puts("adapter-kmt-probe absolute-UMD-path (read-only; external watchdog required)");return 0;}
    if(argc!=2 || wcslen(argv[1])<3 || argv[1][1]!=L':' || argv[1][2]!=L'\\')return 2;
    ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 1;
    LUID luid{};bool found=false;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> adapter;HRESULT hr=factory->EnumAdapters1(i,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return 1;
        DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc)))return 1;
        if(desc.VendorId==0x1002 && desc.DeviceId==0x13fe){if(found)return 1;luid=desc.AdapterLuid;found=true;}
    }
    if(!found){puts("BC-250 adapter not found");return 1;}
    D3DKMT_OPENADAPTERFROMLUID request{};request.AdapterLuid=luid;
    NTSTATUS status=D3DKMTOpenAdapterFromLuid(&request);if(status<0)return 1;
    adapterHandle=request.hAdapter;report_driver_names();int result=inspect(argv[1],luid);
    D3DKMT_CLOSEADAPTER close{};close.hAdapter=adapterHandle;status=D3DKMTCloseAdapter(&close);adapterHandle=0;
    printf("KMT close status=%08lx\n",static_cast<unsigned long>(status));
    if(status<0)result=1;
    puts(result?"FAILED":"PASSED (KMT-backed adapter admission only)");return result;
}
