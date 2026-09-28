// SPDX-License-Identifier: MIT
// Read-only adapter-policy probe; run under an external bounded process Job.
#include "adapter-query-scope.h"
#include "bc250_vkd3d_engine.h"
#include <dxgi1_4.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdio>
namespace {
unsigned queue_calls{};
HRESULT APIENTRY bind(void*,void*,VkQueue){++queue_calls;return E_NOTIMPL;}
void APIENTRY unbind(void*,void*,VkQueue){++queue_calls;}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=3){std::puts("adapter-caps-probe absolute-engine-DLL absolute-ICD-DLL; no device, external watchdog required");return argc==2 && !wcscmp(argv[1],L"--help")?0:2;}
    for(int i=1;i<3;++i)if(wcslen(argv[i])<4 || argv[i][1]!=L':' || argv[i][2]!=L'\\')return 2;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDXGIFactory1> factory;HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));if(FAILED(hr))return 1;
    LUID luid{};bool found=false;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1> adapter;hr=factory->EnumAdapters1(i,&adapter);
        if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return 1;
        DXGI_ADAPTER_DESC1 desc{};if(FAILED(adapter->GetDesc1(&desc)))return 1;
        if(desc.VendorId==0x1002 && desc.DeviceId==0x13fe){if(found)return 1;luid=desc.AdapterLuid;found=true;}
    }
    if(!found){std::puts("BC-250 adapter not found");return 1;}
    constexpr DWORD flags=LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32;
    HMODULE engine=LoadLibraryExW(argv[1],nullptr,flags);if(!engine)return 1;
    HMODULE icd=LoadLibraryExW(argv[2],nullptr,flags);if(!icd){FreeLibrary(engine);return 1;}
    auto getter=reinterpret_cast<PFN_BC250_VKD3D_ENGINE_GET_FUNCS>(GetProcAddress(engine,BC250_VKD3D_ENGINE_GET_FUNCS_NAME));
    auto get=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icd,"vk_icdGetInstanceProcAddr"));
    BC250_VKD3D_ENGINE_FUNCS funcs{};funcs.Size=sizeof(funcs);
    bool ok=false;
    if(getter && get && SUCCEEDED(getter(0x10002,&funcs)) && funcs.QueryAdapterCaps){
        UINT64 identity{};std::memcpy(&identity,&luid,sizeof(luid));
        native12::AdapterQueryScope scope(get,identity);
        BC250_VKD3D_SHELL_SERVICES services{sizeof(services),nullptr,bind,unbind};
        BC250_VKD3D_DEVICE_CREATE_INFO info{sizeof(info),0x10002,scope.entry(),luid,D3D_FEATURE_LEVEL_11_0,BC250_VKD3D_QUEUE_MODE_INLINE,&services,BC250_VKD3D_INSTANCE_MODE_PRIVATE};
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_12_2,D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0,D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_FEATURE_LEVELS feature_levels{5,levels,D3D_FEATURE_LEVEL_11_0};
        D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
        D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
        BC250_VKD3D_FEATURE_QUERY queries[]={
            {D3D12_FEATURE_FEATURE_LEVELS,sizeof(feature_levels),&feature_levels,E_UNEXPECTED,0},
            {D3D12_FEATURE_D3D12_OPTIONS,sizeof(options),&options,E_UNEXPECTED,0},
            {D3D12_FEATURE_D3D12_OPTIONS5,sizeof(options5),&options5,E_UNEXPECTED,0}};
        hr=funcs.QueryAdapterCaps(&info,3,queries);
        std::printf("QueryAdapterCaps hr=%08lx closed_without_device_callbacks=%u queue_calls=%u\n",static_cast<unsigned long>(hr),unsigned(scope.completed()),queue_calls);
        ok=SUCCEEDED(hr) && scope.completed() && !queue_calls;
        for(const auto& q:queries){std::printf("feature=%u hr=%08lx\n",q.Feature,static_cast<unsigned long>(q.Result));ok=ok&&SUCCEEDED(q.Result);}
        if(ok)std::printf("caps: max_fl=%x tiled_tier=%u binding_tier=%u raytracing_tier=%u\n",unsigned(feature_levels.MaxSupportedFeatureLevel),unsigned(options.TiledResourcesTier),unsigned(options.ResourceBindingTier),unsigned(options5.RaytracingTier));
    }
    FreeLibrary(icd);FreeLibrary(engine);
    std::puts(ok?"PASSED":"FAILED");return ok?0:1;
}
