// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "interactive.h"
using Microsoft::WRL::ComPtr;
static bool check(const char* name,HRESULT hr) {
    printf("%s hr=%08lx\n",name,static_cast<unsigned long>(hr)); fflush(stdout); return SUCCEEDED(hr);
}
static bool wait_value(ID3D12Fence* f,UINT64 value,HANDLE event) {
    ResetEvent(event);
    if (!check("SetEventOnCompletion",f->SetEventOnCompletion(value,event))) return false;
    DWORD w=WaitForSingleObject(event,5000);
    UINT64 completed=f->GetCompletedValue();
    printf("wait=%lu completed=%llu expected=%llu\n",w,completed,value);fflush(stdout);
    return w==WAIT_OBJECT_0 && completed>=value && completed!=UINT64_MAX;
}
int main(int argc,char** argv) {
    if(argc==2 && !strcmp(argv[1],"--help")) {puts("amdgpu_wddm_d3d12_queue --lab|--warp | --interactive DIR --deadline SECONDS (1..150; external process-tree deadline required)");return 0;}
    if(argc==5 && !strcmp(argv[1],"--interactive") && !strcmp(argv[3],"--deadline")){
        try{return interactive::run(argv[2],interactive::seconds(argv[4]));}
        catch(const std::exception& e){fprintf(stderr,"interactive failure: %s\n",e.what());return 3;}
    }
    if(argc!=2 || (strcmp(argv[1],"--lab") && strcmp(argv[1],"--warp"))) return 2;
    const bool warp=!strcmp(argv[1],"--warp");
    // Load the Microsoft runtime explicitly, never an application-local translator.
    wchar_t path[MAX_PATH]{};
    if(!GetSystemDirectoryW(path,MAX_PATH) || wcscat_s(path,L"\\d3d12.dll")) return 3;
    HMODULE runtime=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!runtime) return 3;
    auto proc=GetProcAddress(runtime,"D3D12CreateDevice");
    decltype(&D3D12CreateDevice) create=nullptr;
    static_assert(sizeof(create)==sizeof(proc));memcpy(&create,&proc,sizeof(create));
    if(!create) return 3;
    ComPtr<IDXGIFactory4> factory;
    if(!check("factory",CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter;
    if(warp) {
        if(!check("warp adapter",factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return 3;
    } else {
        for(UINT i=0;;i++) {
            ComPtr<IDXGIAdapter1> candidate;HRESULT hr=factory->EnumAdapters1(i,&candidate);
            if(hr==DXGI_ERROR_NOT_FOUND) break;
            if(FAILED(hr)) return 3;
            DXGI_ADAPTER_DESC1 desc{};
            if(SUCCEEDED(candidate->GetDesc1(&desc)) && desc.VendorId==0x1002 && desc.DeviceId==0x13fe) {adapter=candidate;break;}
        }
    }
    if(!adapter) return 3;
    printf("runtime=system32/d3d12.dll adapter=%s\n",warp?"WARP":"BC-250");fflush(stdout);
    ComPtr<ID3D12Device> device;
    if(!check("CreateDevice FL11_0",create(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)))) return 1;
    ComPtr<ID3D12CommandQueue> a,b;D3D12_COMMAND_QUEUE_DESC desc{};desc.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(!check("CreateQueue A",device->CreateCommandQueue(&desc,IID_PPV_ARGS(&a))) ||
       !check("CreateQueue B",device->CreateCommandQueue(&desc,IID_PPV_ARGS(&b)))) return 1;
    ComPtr<ID3D12Fence> done,gate;
    if(!check("CreateFence done",device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&done))) ||
       !check("CreateFence gate",device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate)))) return 1;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event) return 3;
    bool pass=check("A Signal(done,1)",a->Signal(done.Get(),1)) && wait_value(done.Get(),1,event);
    if(pass) {
        pass=check("B Wait(gate,1)",b->Wait(gate.Get(),1));
        if(pass) pass=check("B Signal(done,2)",b->Signal(done.Get(),2));
        if(pass) {
            ResetEvent(event);pass=check("arm blocked completion",done->SetEventOnCompletion(2,event));
            if(pass) {
                DWORD w=WaitForSingleObject(event,100);UINT64 value=done->GetCompletedValue();
                printf("blocked wait=%lu completed=%llu expected=1\n",w,value);fflush(stdout);
                pass=w==WAIT_TIMEOUT && value==1;
            }
        }
        // Always release the dependency, even after a failed assertion, before COM teardown.
        bool release=check("CPU Signal(gate,1)",gate->Signal(1));
        pass=release && pass;
        if(release && !wait_value(done.Get(),2,event)) pass=false;
    }
    pass=check("GetDeviceRemovedReason",device->GetDeviceRemovedReason()) && pass;
    CloseHandle(event);gate.Reset();done.Reset();b.Reset();a.Reset();device.Reset();
    puts(pass?"PASSED":"FAILED");return pass?0:1;
}
