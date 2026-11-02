// SPDX-License-Identifier: MIT
// M14.8 oracle (BD-283): offer and reclaim on the installed D3D11 driver, with no window. Each cycle draws a colour
// that only this cycle uses, offers the resource, trims the device, waits, reclaims it, and reads the colour back
// when the reclaim says the content was kept. Three cases:
//   shared     a keyed-mutex shared texture: the driver sees MISC_SHARED, so the shell is expected to give it a
//              kernel allocation, and Offer and Reclaim to reach the kernel (OfferAllocationsCb,
//              ReclaimAllocations2Cb); the shell's residency lines show it. DXGI allows the offer of a shared resource only
//              while the keyed mutex is held (IDXGIDevice2::OfferResources, remarks).
//   swapchain  the back buffer of a composition swap chain (flip model, BIND_PRESENT), after a Present. The runtime
//              refuses to offer a swap-chain buffer with E_INVALIDARG (seen on a vendor driver, 2026-11-02); the
//              answer is recorded and is not a failure by itself, so this case covers the Present path only.
//   private    a texture the engine keeps for itself: the shell treats Offer and Reclaim as hints (control).
//   shared-nt  as shared, created with D3D11_RESOURCE_MISC_SHARED_NTHANDLE.
// --negative makes cycle 1 of every case expect a colour it did not draw: a working probe must then FAIL.
// Odd cycles use OfferResources/ReclaimResources (IDXGIDevice2), even cycles OfferResources1/ReclaimResources1
// (IDXGIDevice4) with DXGI_OFFER_RESOURCE_FLAG_ALLOW_DECOMMIT. Output: one JSON file. Exit 0 pass, 1 fail,
// 2 usage, 3 setup (no adapter, no device, no output file).
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
using Microsoft::WRL::ComPtr;

namespace {
struct Cycle {
    unsigned index=0; bool api1=false;
    HRESULT offer=S_OK,reclaim=S_OK,removed=S_OK;
    bool offered=false,discarded=false,checked=false,match=false;
    unsigned expected=0,actual=0;
    const char *result="";
};
struct Case {
    std::string name; HRESULT setup=S_OK; bool runtimeRefusedOffer=false; std::vector<Cycle> cycles;
    bool pass=false; std::string why;
};
std::string hex(HRESULT hr) { char b[16]; std::snprintf(b,sizeof(b),"%08X",unsigned(hr)); return b; }
std::string module_path(const wchar_t *name) {
    HMODULE m=GetModuleHandleW(name); if (!m) return "";
    wchar_t w[MAX_PATH]{}; if (!GetModuleFileNameW(m,w,MAX_PATH)) return "";
    char a[MAX_PATH*3]{}; WideCharToMultiByte(CP_UTF8,0,w,-1,a,sizeof(a),nullptr,nullptr);
    std::string s; for (const char *p=a;*p;++p) { if (*p=='\\') s+="\\\\"; else s+=*p; } return s;
}
// B8G8R8A8: the colour of cycle i, never black and different for each cycle.
unsigned colour_of(unsigned i) { return 0xFF000000u | ((40u+i*7u)&0xFFu)<<16 | ((200u-i*5u)&0xFFu)<<8 | ((90u+i*11u)&0xFFu); }

class Probe {
public:
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGIDevice4> dxgi; ComPtr<IDXGIFactory2> factory;
    bool negative=false; // --negative: cycle 1 expects a colour it did not draw, so a working probe must FAIL
    void clear(ID3D11Texture2D *t,unsigned c) {
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(device->CreateRenderTargetView(t,nullptr,&rtv))) return;
        const float f[4]={float((c>>16)&0xFF)/255.f,float((c>>8)&0xFF)/255.f,float(c&0xFF)/255.f,1.f};
        context->ClearRenderTargetView(rtv.Get(),f);
    }
    bool read_centre(ID3D11Texture2D *t,unsigned &out) {
        D3D11_TEXTURE2D_DESC d{}; t->GetDesc(&d);
        D3D11_TEXTURE2D_DESC s=d; s.Usage=D3D11_USAGE_STAGING; s.BindFlags=0; s.MiscFlags=0;
        s.CPUAccessFlags=D3D11_CPU_ACCESS_READ; s.MipLevels=1; s.ArraySize=1; s.SampleDesc={1,0};
        ComPtr<ID3D11Texture2D> staging; if (FAILED(device->CreateTexture2D(&s,nullptr,&staging))) return false;
        context->CopyResource(staging.Get(),t);
        D3D11_MAPPED_SUBRESOURCE m{}; if (FAILED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m))) return false;
        const auto *row=static_cast<const unsigned char *>(m.pData)+size_t(d.Height/2)*m.RowPitch;
        std::memcpy(&out,row+size_t(d.Width/2)*4,4); context->Unmap(staging.Get(),0); return true;
    }
    // One cycle on `t` (`mutex` held around it when there is one). `present` presents the swap chain first.
    Cycle cycle(unsigned i,ID3D11Texture2D *t,IDXGIKeyedMutex *mutex,IDXGISwapChain1 *present) {
        Cycle c; c.index=i; c.api1=(i%2)==0; c.expected=colour_of(i);
        if (mutex && FAILED(mutex->AcquireSync(0,2000))) { c.result="mutex"; return c; }
        clear(t,c.expected);
        if (present) present->Present(0,0); else context->Flush();
        ComPtr<IDXGIResource> r; t->QueryInterface(IID_PPV_ARGS(&r));
        IDXGIResource *list[]={r.Get()};
        c.offer=c.api1 ? dxgi->OfferResources1(1,list,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL,DXGI_OFFER_RESOURCE_FLAG_ALLOW_DECOMMIT)
                       : dxgi->OfferResources(1,list,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL);
        c.offered=SUCCEEDED(c.offer);
        context->Flush();
        dxgi->Trim(); // IDXGIDevice3::Trim, as a suspended app does after its offers
        Sleep(50);
        if (c.offered) {
            if (c.api1) {
                DXGI_RECLAIM_RESOURCE_RESULTS res=DXGI_RECLAIM_RESOURCE_RESULT_OK;
                c.reclaim=dxgi->ReclaimResources1(1,list,&res);
                c.discarded=res!=DXGI_RECLAIM_RESOURCE_RESULT_OK;
                c.result=res==DXGI_RECLAIM_RESOURCE_RESULT_OK ? "ok" : res==DXGI_RECLAIM_RESOURCE_RESULT_DISCARDED ? "discarded" : "not-committed";
            } else {
                BOOL d=FALSE; c.reclaim=dxgi->ReclaimResources(1,list,&d);
                c.discarded=d!=FALSE; c.result=d ? "discarded" : "ok";
            }
        } else c.result="not-offered";
        if (c.offered && SUCCEEDED(c.reclaim) && !c.discarded) {
            c.checked=read_centre(t,c.actual);
            c.match=c.checked && c.actual==(negative && i==1 ? ~c.expected : c.expected);
        }
        if (mutex) mutex->ReleaseSync(0);
        c.removed=device->GetDeviceRemovedReason();
        return c;
    }
};
void write_case(FILE *f,const Case &k,bool first) {
    std::fprintf(f,"%s\n  {\"case\":\"%s\",\"setup\":\"%s\",\"runtime_refused_offer\":%s,\"pass\":%s,\"why\":\"%s\",\"cycles\":[",
        first?"":",",k.name.c_str(),hex(k.setup).c_str(),k.runtimeRefusedOffer?"true":"false",k.pass?"true":"false",k.why.c_str());
    for (size_t i=0;i<k.cycles.size();++i) {
        const Cycle &c=k.cycles[i];
        std::fprintf(f,"%s\n    {\"i\":%u,\"api\":\"%s\",\"offer\":\"%s\",\"reclaim\":\"%s\",\"result\":\"%s\",\"checked\":%s,"
            "\"expected\":\"%08X\",\"actual\":\"%08X\",\"match\":%s,\"removed_reason\":\"%s\"}",i?",":"",c.index,c.api1?"1":"0",
            hex(c.offer).c_str(),hex(c.reclaim).c_str(),c.result,c.checked?"true":"false",c.expected,c.actual,
            c.match?"true":"false",hex(c.removed).c_str());
    }
    std::fprintf(f,"]}");
}
// A case passes when every cycle left the device in place, every successful reclaim either discarded the content
// or kept exactly the colour drawn before the offer, and no reclaim failed. A refused offer passes only for the
// swap chain, where the runtime may refuse it before the driver is called.
void judge(Case &k) {
    if (FAILED(k.setup)) { k.why="setup "+hex(k.setup); return; }
    unsigned refused=0;
    for (const Cycle &c:k.cycles) {
        if (FAILED(c.removed)) { k.why="device removed at cycle "+std::to_string(c.index)+" reason "+hex(c.removed); return; }
        if (!c.offered) { ++refused; continue; }
        if (FAILED(c.reclaim)) { k.why="reclaim "+hex(c.reclaim)+" at cycle "+std::to_string(c.index); return; }
        if (!c.discarded && !c.match) { k.why="content changed at cycle "+std::to_string(c.index); return; }
    }
    k.runtimeRefusedOffer=refused!=0;
    if (refused && k.name!="swapchain") { k.why=std::to_string(refused)+" offers refused"; return; }
    k.pass=true; k.why=refused ? "the runtime refused the offer of a swap-chain buffer" : "every cycle kept or discarded";
}
}

int main(int argc,char **argv) {
    if (argc==2 && !std::strcmp(argv[1],"--help")) {
        std::puts("offer-reclaim --out result.json [--cycles N] [--any-adapter] [--negative]"); return 0;
    }
    const char *out=nullptr; unsigned cycles=12; bool anyAdapter=false,negative=false;
    for (int i=1;i<argc;++i) {
        if (!std::strcmp(argv[i],"--out") && i+1<argc) out=argv[++i];
        else if (!std::strcmp(argv[i],"--cycles") && i+1<argc) cycles=unsigned(std::strtoul(argv[++i],nullptr,10));
        else if (!std::strcmp(argv[i],"--any-adapter")) anyAdapter=true;
        else if (!std::strcmp(argv[i],"--negative")) negative=true;
        else return 2;
    }
    if (!out || !cycles || cycles>200) return 2;
    Probe p; p.negative=negative;
    if (FAILED(CreateDXGIFactory2(0,IID_PPV_ARGS(&p.factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 ad{};
    ComPtr<IDXGIFactory1> f1; p.factory.As(&f1);
    for (UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> a; if (f1->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (anyAdapter || (d.VendorId==0x1002 && d.DeviceId==0x13FE)) { adapter=a; ad=d; break; }
    }
    if (!adapter) return 3;
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,levels,2,
        D3D11_SDK_VERSION,&p.device,nullptr,&p.context)) || FAILED(p.device.As(&p.dxgi))) return 3;
    FILE *f=nullptr; if (fopen_s(&f,out,"wb") || !f) return 3;

    D3D11_TEXTURE2D_DESC td{}; td.Width=256; td.Height=256; td.MipLevels=1; td.ArraySize=1;
    td.Format=DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc={1,0}; td.Usage=D3D11_USAGE_DEFAULT;
    td.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
    std::vector<Case> cases(4); cases[0].name="shared"; cases[1].name="swapchain"; cases[2].name="private"; cases[3].name="shared-nt";
    {   // shared, keyed mutex
        D3D11_TEXTURE2D_DESC d=td; d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> t; ComPtr<IDXGIKeyedMutex> m;
        cases[0].setup=p.device->CreateTexture2D(&d,nullptr,&t);
        if (SUCCEEDED(cases[0].setup)) cases[0].setup=t.As(&m);
        if (SUCCEEDED(cases[0].setup)) for (unsigned i=0;i<cycles;++i) cases[0].cycles.push_back(p.cycle(i,t.Get(),m.Get(),nullptr));
    }
    {   // composition swap chain, flip model, no window
        DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width=256; sd.Height=256; sd.Format=DXGI_FORMAT_B8G8R8A8_UNORM; sd.SampleDesc={1,0};
        sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount=2; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        sd.Scaling=DXGI_SCALING_STRETCH; sd.AlphaMode=DXGI_ALPHA_MODE_PREMULTIPLIED;
        ComPtr<IDXGISwapChain1> sc; ComPtr<ID3D11Texture2D> t;
        cases[1].setup=p.factory->CreateSwapChainForComposition(p.device.Get(),&sd,nullptr,&sc);
        for (unsigned i=0;SUCCEEDED(cases[1].setup) && i<cycles;++i) {
            t.Reset(); cases[1].setup=sc->GetBuffer(0,IID_PPV_ARGS(&t));
            if (SUCCEEDED(cases[1].setup)) cases[1].cycles.push_back(p.cycle(i,t.Get(),nullptr,sc.Get()));
        }
    }
    {   // engine-private control
        ComPtr<ID3D11Texture2D> t; cases[2].setup=p.device->CreateTexture2D(&td,nullptr,&t);
        if (SUCCEEDED(cases[2].setup)) for (unsigned i=0;i<cycles;++i) cases[2].cycles.push_back(p.cycle(i,t.Get(),nullptr,nullptr));
    }
    {   // shared through an NT handle, keyed mutex: the other kind of shared runtime surface
        D3D11_TEXTURE2D_DESC d=td; d.MiscFlags=D3D11_RESOURCE_MISC_SHARED_NTHANDLE|D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        ComPtr<ID3D11Texture2D> t; ComPtr<IDXGIKeyedMutex> m;
        cases[3].setup=p.device->CreateTexture2D(&d,nullptr,&t);
        if (SUCCEEDED(cases[3].setup)) cases[3].setup=t.As(&m);
        if (SUCCEEDED(cases[3].setup)) for (unsigned i=0;i<cycles;++i) cases[3].cycles.push_back(p.cycle(i,t.Get(),m.Get(),nullptr));
    }
    bool all=true; for (Case &k:cases) { judge(k); all=all && k.pass; }
    char name[256]{}; WideCharToMultiByte(CP_UTF8,0,ad.Description,-1,name,sizeof(name),nullptr,nullptr);
    std::fprintf(f,"{\"tool\":\"offer-reclaim\",\"pid\":%lu,\"adapter\":\"%s\",\"vendor\":\"%04X\",\"device\":\"%04X\","
        "\"cycles\":%u,\"d3d11_umd\":\"%s\",\"engine\":\"%s\",\"pass\":%s,\"cases\":[",GetCurrentProcessId(),name,
        ad.VendorId,ad.DeviceId,cycles,
        module_path(L"amdgpu_wddm_d3d11.dll").c_str(),module_path(L"amdgpu_wddm_dxvk.dll").c_str(),all?"true":"false");
    for (size_t i=0;i<cases.size();++i) write_case(f,cases[i],i==0);
    std::fprintf(f,"\n]}\n"); std::fclose(f);
    for (const Case &k:cases) std::printf("%-9s %s %s\n",k.name.c_str(),k.pass?"PASS":"FAIL",k.why.c_str());
    std::printf("OFFER-RECLAIM %s\n",all?"PASS":"FAIL");
    return all ? 0 : 1;
}
