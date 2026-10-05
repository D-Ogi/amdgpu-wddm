// SPDX-License-Identifier: MIT
// Read-only probe of the adapter's instance policy: what the system answers for the policy value of the
// BC-250's software key. No device, no ICD, no engine. Run under an external bounded process Job.
#include "instance-policy.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
namespace {
template<class Function> Function system_entry(HMODULE module,const char* name) noexcept {
    const auto address=GetProcAddress(module,name);Function entry{};
    static_assert(sizeof(entry)==sizeof(address));std::memcpy(&entry,&address,sizeof(entry));return entry;
}
}
int wmain(int argc,wchar_t**) {
    if(argc!=1){std::puts("instance-policy-probe; no arguments, no device, external watchdog required");return 2;}
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;if(FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))return 1;
    LUID luid{};bool found=false;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> adapter;const HRESULT hr=factory->EnumAdapters1(i,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return 1;
        DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc)))return 1;
        if(desc.VendorId==0x1002 && desc.DeviceId==0x13fe){if(found)return 1;luid=desc.AdapterLuid;found=true;}
    }
    if(!found){std::puts("BC-250 adapter not found");return 1;}
    HMODULE gdi=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!gdi)return 1;
    const native12::MemoryPolicyKmt kmt{
        system_entry<PFND3DKMT_OPENADAPTERFROMLUID>(gdi,"D3DKMTOpenAdapterFromLuid"),
        system_entry<PFND3DKMT_QUERYADAPTERINFO>(gdi,"D3DKMTQueryAdapterInfo"),
        system_entry<PFND3DKMT_CLOSEADAPTER>(gdi,"D3DKMTCloseAdapter")};
    native12::SparsePolicy policy{};D3DKMT_HANDLE unresolved{};
    const HRESULT hr=native12::query_sparse_policy(luid,kmt,&policy,&unresolved);
    FreeLibrary(gdi);
    std::printf("instance-policy hr=%08lx sparse=%u source=%u status=%08lx unresolved=%u\n",static_cast<unsigned long>(hr),
        unsigned(policy.sparse),static_cast<unsigned>(policy.source),static_cast<unsigned long>(policy.status),
        unsigned(unresolved!=0));
    return hr==S_OK && !unresolved?0:1;
}
