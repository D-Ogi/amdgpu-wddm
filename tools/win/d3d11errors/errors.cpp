// SPDX-License-Identifier: MIT
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
using Microsoft::WRL::ComPtr;
static bool removed(HRESULT hr) {
    return hr==DXGI_ERROR_DEVICE_REMOVED || hr==DXGI_ERROR_DEVICE_RESET ||
        hr==DXGI_ERROR_DEVICE_HUNG || hr==DXGI_ERROR_DRIVER_INTERNAL_ERROR;
}
static bool local_module(const wchar_t *name) {
    wchar_t expected[MAX_PATH]{},actual[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr,expected,MAX_PATH)) return false;
    auto slash=wcsrchr(expected,L'\\'); if (!slash) return false;
    if (wcscpy_s(slash+1,MAX_PATH-size_t(slash+1-expected),name)) return false;
    auto module=GetModuleHandleW(name);
    return module && GetModuleFileNameW(module,actual,MAX_PATH) && !_wcsicmp(actual,expected);
}
static bool system_d3d11() {
    wchar_t expected[MAX_PATH]{},actual[MAX_PATH]{};
    if (!GetSystemDirectoryW(expected,MAX_PATH) || wcscat_s(expected,L"\\d3d11.dll")) return false;
    auto module=GetModuleHandleW(L"d3d11.dll");
    return module && GetModuleFileNameW(module,actual,MAX_PATH) && !_wcsicmp(actual,expected);
}
int main(int argc,char **argv) {
    if (argc==2 && !strcmp(argv[1],"--help")) {
        puts("amdgpu_wddm_d3d11_errors --control|--inject --out result.json"); return 0;
    }
    if (argc!=4 || (strcmp(argv[1],"--control") && strcmp(argv[1],"--inject")) || strcmp(argv[2],"--out")) return 2;
    const bool inject=!strcmp(argv[1],"--inject");
    SetEnvironmentVariableA("BC250DXVK_TEST_OOM",inject ? "1" : nullptr);
    SetEnvironmentVariableA("BC250DXVK_TEST_OOM_NOW",nullptr);
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i=0;;i++) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapters1(i,&candidate)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (candidate && SUCCEEDED(candidate->GetDesc1(&desc)) && desc.VendorId==0x1002 && desc.DeviceId==0x13fe) {adapter=candidate;break;}
    }
    if (!adapter) return 3;
    std::ofstream out(argv[3]); if (!out) return 3;
    out<<"{\"inject\":"<<(inject?"true":"false")<<",\"cases\":[";
    bool all=true;
    for (unsigned test=0;test<3;test++) {
        ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;
        const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL level{};
        HRESULT create=D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,3,D3D11_SDK_VERSION,&device,&level,&context);
        if (FAILED(create)) {out<<(test?",":"")<<"{\"create_device\":"<<unsigned(create)<<",\"passed\":false}";all=false;break;}
        const bool modules=system_d3d11() && (inject ?
            local_module(L"bc250d3d11.dll") && local_module(L"bc250dxvk.dll") && local_module(L"bc250radv.dll") :
            !GetModuleHandleW(L"bc250d3d11.dll") && !GetModuleHandleW(L"bc250dxvk.dll"));
        D3D11_BUFFER_DESC desc{};desc.ByteWidth=1u<<20;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_VERTEX_BUFFER;
        ComPtr<ID3D11Buffer> base,dynamic,staging;
        HRESULT setup=device->CreateBuffer(&desc,nullptr,&base);
        auto dynDesc=desc;dynDesc.Usage=D3D11_USAGE_DYNAMIC;dynDesc.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (SUCCEEDED(setup)) setup=device->CreateBuffer(&dynDesc,nullptr,&dynamic);
        auto readDesc=desc;readDesc.Usage=D3D11_USAGE_STAGING;readDesc.BindFlags=0;readDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(setup)) setup=device->CreateBuffer(&readDesc,nullptr,&staging);
        HRESULT hr=E_FAIL,recovery=E_FAIL,before=device->GetDeviceRemovedReason(),after=before,sticky=before;
        bool nullOutput=true,readNull=true;HRESULT readHr=E_FAIL;
        std::vector<unsigned char> upload(1u<<20,0x5a);
        if (SUCCEEDED(setup)) {
            if (inject) SetEnvironmentVariableA("BC250DXVK_TEST_OOM_NOW","1");
            if (test==0) {
                ComPtr<ID3D11Buffer> failed;
                hr=device->CreateBuffer(&desc,nullptr,&failed);nullOutput=!failed;
            } else if (test==1) {
                D3D11_MAPPED_SUBRESOURCE mapped{};
                hr=context->Map(dynamic.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                nullOutput=!mapped.pData;
                if (SUCCEEDED(hr)) context->Unmap(dynamic.Get(),0);
            } else {
                context->UpdateSubresource(base.Get(),0,nullptr,upload.data(),0,0);
                context->Flush();hr=device->GetDeviceRemovedReason();
            }
            SetEnvironmentVariableA("BC250DXVK_TEST_OOM_NOW",nullptr);
            after=device->GetDeviceRemovedReason();
            if (test==1) {
                D3D11_MAPPED_SUBRESOURCE read{};
                readHr=context->Map(staging.Get(),0,D3D11_MAP_READ,0,&read);
                readNull=!read.pData;
                if (SUCCEEDED(readHr)) context->Unmap(staging.Get(),0);
            }
            if (test==0 || !inject) {
                ComPtr<ID3D11Buffer> recovered;recovery=device->CreateBuffer(&desc,nullptr,&recovered);
                if (SUCCEEDED(recovery) && !recovered) recovery=E_FAIL;
            }
            context->Flush();sticky=device->GetDeviceRemovedReason();
        }
        bool pass=modules && SUCCEEDED(setup) && before==S_OK;
        if (!inject) pass=pass && hr==S_OK && recovery==S_OK && after==S_OK && sticky==S_OK;
        else if (test==0) pass=pass && hr==E_OUTOFMEMORY && nullOutput && recovery==S_OK && after==S_OK && sticky==S_OK;
        else if (test==1) {
            // Write-only Map can use runtime backing memory after device removal.
            // CPU-read Map must expose removal; require both it and the sticky device state.
            const bool writeResult=(hr==S_OK && !nullOutput) || (removed(hr) && nullOutput);
            pass=pass && writeResult && removed(after) && sticky==after && removed(readHr) && readNull;
        } else pass=pass && removed(hr) && removed(after) && sticky==after;
        if (!inject && test==1) pass=pass && readHr==S_OK && !readNull;
        all=all && pass;
        out<<(test?",":"")<<"{\"case\":\""<<(test==0?"create":test==1?"map":"update")<<"\",\"level\":"<<unsigned(level)
            <<",\"modules\":"<<(modules?"true":"false")<<",\"setup\":"<<unsigned(setup)<<",\"hr\":"<<unsigned(hr)
            <<",\"read_hr\":"<<unsigned(readHr)<<",\"read_null\":"<<(readNull?"true":"false")
            <<",\"before\":"<<unsigned(before)<<",\"after\":"<<unsigned(after)<<",\"sticky\":"<<unsigned(sticky)
            <<",\"recovery\":"<<unsigned(recovery)<<",\"null_output\":"<<(nullOutput?"true":"false")<<",\"passed\":"<<(pass?"true":"false")<<"}";
        out.flush();
    }
    SetEnvironmentVariableA("BC250DXVK_TEST_OOM_NOW",nullptr);
    out<<"],\"passed\":"<<(all?"true":"false")<<"}\n";out.close();
    return all?0:1;
}
