// SPDX-License-Identifier: MIT
// BD-308 and BD-309 repro: the cases of HLK "WDDM - OfferReclaim11 - Offer States" and "WDDM2 - OfferReclaim11 -
// Working Set" that failed on tester.37, as a console program with no window.
//
// Offer states (BD-308). For each offer state, workload and mode it creates a shared texture
// (D3D11_RESOURCE_MISC_SHARED, as the HLK test: it opens the texture again through GetSharedHandle), sets the VidMm
// verifier flags of the adapter with D3DKMT_ESCAPE_ADAPTER_VERIFIER_OPTION (escape 29, d3dkmthk.h:2645; option struct
// d3dkmthk.h:3320, flags union d3dkmthk.h:3276), queues a GPU workload on the texture, offers it, reclaims it and prints
// bDiscarded and the time of each call.
//   state 0  the flags the adapter had before the program started (no verifier flag of this program)
//   state 5  those flags with AlwaysDiscardOffer (d3dkmthk.h:3283): a reclaim after a completed offer must discard
// Workload 0 queues no work on the texture after its clear. Workloads 1-4 queue 4, 20, 100 and 500 copy round trips of
// the 2048x2048 texture through a second texture (the HLK workloads grow the same way: 4 ... 500 draws).
// Modes:
//   now      OfferResources, then ReclaimResources at once (the HLK order)
//   now1     OfferResources1 with DXGI_OFFER_RESOURCE_FLAG_ALLOW_DECOMMIT, then ReclaimResources1 at once
//   later    OfferResources, 200 ms, ReclaimResources (a deferred offer has completed by then)
//   drained  IDXGIDevice2::EnqueueSetEvent and a wait for the event before the offer, then as "now": the runtime's own
//            drain of the device's work. If "now" keeps the content and "drained" discards, the kept content comes
//            from work in flight at the offer.
//
// Working set (BD-309). A 1024x1024 BGRA staging texture (4 MiB) that the CPU writes in full through Map(WRITE), a
// drain with EnqueueSetEvent, K32GetProcessMemoryInfo, OfferResources of the staging texture, 200 ms,
// K32GetProcessMemoryInfo again, ReclaimResources. The HLK test asks for a drop of TEXTURE_SIZE - WORKING_SET_THRESHOLD,
// which its log gives as 3932160 bytes. Run with the original flags and with AlwaysDiscardOffer, AlwaysProcessOfferList
// and AlwaysDecommitOffer (d3dkmthk.h:3283, 3295, 3296).
//
// The program restores the flags of the adapter after each case and at its end. It writes the flags it found to
// --save before it sets any, so that "--set-flags <hex>" can restore them if it is stopped in the middle.
// Output: one line for each case, and one JSON file. Exit 0 every state 5 case discarded, 1 a state 5 case kept the
// content or a device was removed, 2 usage, 3 setup (no adapter, no device, no output file), 4 the verifier flags
// could not be queried or set. The working-set rows do not change the exit code.
#include <windows.h>
#include <winternl.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_5.h>
#include <d3dkmthk.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
using Microsoft::WRL::ComPtr;

namespace {
constexpr unsigned kCopies[5]={0,4,20,100,500};
// D3DKMT_ADAPTER_VERIFIER_VIDMM_FLAGS, d3dkmthk.h:3283, 3295, 3296.
constexpr UINT32 kAlwaysDiscardOffer=1u<<3, kAlwaysProcessOfferList=1u<<15, kAlwaysDecommitOffer=1u<<16;
constexpr unsigned long long kWorkingSetDrop=3932160; // the HLK limit, from its logged values
enum Mode { kNow=0, kNow1=1, kLater=2, kDrained=3 };
const char *const kModeName[4]={"now","now1","later","drained"};

struct Row {
    unsigned state=0,workload=0,copies=0; Mode mode=kNow;
    UINT32 flags=0; NTSTATUS set=0;
    HRESULT setup=S_OK,offer=S_OK,reclaim=S_OK,removed=S_OK;
    bool discarded=false; const char *result="";
    double drain_ms=0,offer_ms=0,reclaim_ms=0;
};
struct WsRow {
    UINT32 flags=0; NTSTATUS set=0;
    HRESULT setup=S_OK,offer=S_OK,reclaim=S_OK,removed=S_OK;
    unsigned long long before=0,after=0; bool discarded=false; double offer_ms=0;
};
std::string hex(UINT32 v) { char b[16]; std::snprintf(b,sizeof(b),"%08X",unsigned(v)); return b; }
std::string module_path(const wchar_t *name) {
    HMODULE m=GetModuleHandleW(name); if (!m) return "";
    wchar_t w[MAX_PATH]{}; if (!GetModuleFileNameW(m,w,MAX_PATH)) return "";
    char a[MAX_PATH*3]{}; WideCharToMultiByte(CP_UTF8,0,w,-1,a,sizeof(a),nullptr,nullptr);
    std::string s; for (const char *p=a;*p;++p) { if (*p=='\\') s+="\\\\"; else s+=*p; } return s;
}
double now_ms() { LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return double(c.QuadPart)*1000.0/double(f.QuadPart); }
unsigned long long working_set() {
    PROCESS_MEMORY_COUNTERS pmc{}; pmc.cb=sizeof(pmc);
    return K32GetProcessMemoryInfo(GetCurrentProcess(),&pmc,sizeof(pmc)) ? pmc.WorkingSetSize : 0;
}

// The VidMm verifier flags of the adapter, through escape 29.
NTSTATUS verifier_flags(D3DKMT_HANDLE adapter,D3DKMT_VERIFIER_OPTION_MODE mode,UINT32 &value) {
    D3DKMT_ADAPTER_VERIFIER_OPTION option{};
    option.Type=D3DKMT_ADAPTER_VERIFIER_OPTION_VIDMM_FLAGS; option.Mode=mode; option.Data.VidMmFlags.Value=value;
    D3DKMT_ESCAPE e{}; e.hAdapter=adapter; e.Type=D3DKMT_ESCAPE_ADAPTER_VERIFIER_OPTION;
    e.pPrivateDriverData=&option; e.PrivateDriverDataSize=sizeof(option);
    const NTSTATUS s=D3DKMTEscape(&e);
    if (s>=0 && mode==D3DKMT_VERIFIER_OPTION_QUERY) value=option.Data.VidMmFlags.Value;
    return s;
}

struct Probe {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; ComPtr<IDXGIDevice4> dxgi;
    D3DKMT_HANDLE adapter=0; UINT32 original=0;
    // EnqueueSetEvent: the runtime signals the event when the device's submitted work is done.
    double drain() {
        const double a=now_ms();
        HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if (e) { if (SUCCEEDED(dxgi->EnqueueSetEvent(e))) WaitForSingleObject(e,2000); CloseHandle(e); }
        return now_ms()-a;
    }
    Row run(unsigned state,unsigned workload,Mode mode) {
        Row r; r.state=state; r.workload=workload; r.mode=mode; r.copies=kCopies[workload];
        D3D11_TEXTURE2D_DESC d{}; d.Width=2048; d.Height=2048; d.MipLevels=1; d.ArraySize=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc={1,0}; d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC sd=d; sd.MiscFlags=0;
        d.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
        ComPtr<ID3D11Texture2D> t,other; ComPtr<ID3D11RenderTargetView> rtv;
        r.setup=device->CreateTexture2D(&d,nullptr,&t);
        if (SUCCEEDED(r.setup)) r.setup=device->CreateTexture2D(&sd,nullptr,&other);
        if (SUCCEEDED(r.setup)) r.setup=device->CreateRenderTargetView(t.Get(),nullptr,&rtv);
        if (FAILED(r.setup)) return r;
        const float colour[4]={0.25f,0.5f,0.75f,1.f};
        context->ClearRenderTargetView(rtv.Get(),colour);
        drain(); // the clear has retired before the flags change: workload 0 offers an idle texture
        r.flags=state==5 ? (original|kAlwaysDiscardOffer) : original;
        UINT32 v=r.flags; r.set=verifier_flags(adapter,D3DKMT_VERIFIER_OPTION_SET,v);
        for (unsigned i=0;i<r.copies;++i) { context->CopyResource(other.Get(),t.Get()); context->CopyResource(t.Get(),other.Get()); }
        context->Flush();
        if (mode==kDrained) r.drain_ms=drain();
        ComPtr<IDXGIResource> res; t.As(&res); IDXGIResource *list[]={res.Get()};
        double a=now_ms();
        r.offer=mode==kNow1 ? dxgi->OfferResources1(1,list,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL,DXGI_OFFER_RESOURCE_FLAG_ALLOW_DECOMMIT)
                            : dxgi->OfferResources(1,list,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL);
        r.offer_ms=now_ms()-a;
        if (mode==kLater) Sleep(200);
        if (SUCCEEDED(r.offer)) {
            a=now_ms();
            if (mode==kNow1) {
                DXGI_RECLAIM_RESOURCE_RESULTS result=DXGI_RECLAIM_RESOURCE_RESULT_OK;
                r.reclaim=dxgi->ReclaimResources1(1,list,&result);
                r.discarded=result!=DXGI_RECLAIM_RESOURCE_RESULT_OK;
                r.result=result==DXGI_RECLAIM_RESOURCE_RESULT_OK ? "ok" : result==DXGI_RECLAIM_RESOURCE_RESULT_DISCARDED ? "discarded" : "not-committed";
            } else {
                BOOL discarded=FALSE; r.reclaim=dxgi->ReclaimResources(1,list,&discarded);
                r.discarded=discarded!=FALSE; r.result=r.discarded ? "discarded" : "ok";
            }
            r.reclaim_ms=now_ms()-a;
        } else r.result="not-offered";
        UINT32 back=original; verifier_flags(adapter,D3DKMT_VERIFIER_OPTION_SET,back);
        r.removed=device->GetDeviceRemovedReason();
        return r;
    }
    WsRow working(UINT32 flags) {
        WsRow w; w.flags=flags;
        D3D11_TEXTURE2D_DESC d{}; d.Width=1024; d.Height=1024; d.MipLevels=1; d.ArraySize=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc={1,0}; d.Usage=D3D11_USAGE_STAGING;
        d.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE|D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging; w.setup=device->CreateTexture2D(&d,nullptr,&staging);
        if (FAILED(w.setup)) return w;
        D3D11_MAPPED_SUBRESOURCE m{}; w.setup=context->Map(staging.Get(),0,D3D11_MAP_WRITE,0,&m);
        if (FAILED(w.setup)) return w;
        for (UINT y=0;y<d.Height;++y) std::memset(static_cast<unsigned char *>(m.pData)+size_t(y)*m.RowPitch,0x5A,size_t(d.Width)*4);
        context->Unmap(staging.Get(),0);
        drain();
        UINT32 v=flags; w.set=verifier_flags(adapter,D3DKMT_VERIFIER_OPTION_SET,v);
        w.before=working_set();
        ComPtr<IDXGIResource> res; staging.As(&res); IDXGIResource *list[]={res.Get()};
        const double a=now_ms(); w.offer=dxgi->OfferResources(1,list,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL); w.offer_ms=now_ms()-a;
        Sleep(200);
        w.after=working_set();
        if (SUCCEEDED(w.offer)) { BOOL discarded=FALSE; w.reclaim=dxgi->ReclaimResources(1,list,&discarded); w.discarded=discarded!=FALSE; }
        UINT32 back=original; verifier_flags(adapter,D3DKMT_VERIFIER_OPTION_SET,back);
        w.removed=device->GetDeviceRemovedReason();
        return w;
    }
};
}

int main(int argc,char **argv) {
    const char *usage="offer-states --out result.json --save flags.txt [--states 0,5] [--any-adapter]\n"
                      "offer-states --set-flags <hex> [--any-adapter]";
    if (argc==2 && !std::strcmp(argv[1],"--help")) { std::puts(usage); return 0; }
    const char *out=nullptr,*save=nullptr; bool anyAdapter=false,setOnly=false; UINT32 setValue=0;
    std::vector<unsigned> states{0,5};
    auto list=[](const char *s,std::vector<unsigned> &v,unsigned max) {
        v.clear(); for (const char *p=s;*p;) { char *e=nullptr; unsigned long n=std::strtoul(p,&e,10);
            if (e==p || n>max) return false; v.push_back(unsigned(n)); p=*e==',' ? e+1 : e; if (*e && *e!=',') return false; }
        return !v.empty(); };
    for (int i=1;i<argc;++i) {
        if (!std::strcmp(argv[i],"--out") && i+1<argc) out=argv[++i];
        else if (!std::strcmp(argv[i],"--save") && i+1<argc) save=argv[++i];
        else if (!std::strcmp(argv[i],"--states") && i+1<argc) { if (!list(argv[++i],states,5)) return 2;
            for (unsigned s:states) if (s!=0 && s!=5) return 2; }
        else if (!std::strcmp(argv[i],"--set-flags") && i+1<argc) { setOnly=true; setValue=UINT32(std::strtoul(argv[++i],nullptr,16)); }
        else if (!std::strcmp(argv[i],"--any-adapter")) anyAdapter=true;
        else { std::fprintf(stderr,"%s\n",usage); return 2; }
    }
    if (!setOnly && (!out || !save)) { std::fprintf(stderr,"%s\n",usage); return 2; }
    ComPtr<IDXGIFactory1> factory; if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return 3;
    ComPtr<IDXGIAdapter1> adapter; DXGI_ADAPTER_DESC1 ad{};
    for (UINT i=0;;++i) {
        ComPtr<IDXGIAdapter1> a; if (factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) continue;
        if (anyAdapter || (d.VendorId==0x1002 && d.DeviceId==0x13FE)) { adapter=a; ad=d; break; }
    }
    if (!adapter) return 3;
    Probe p;
    D3DKMT_OPENADAPTERFROMLUID open{}; open.AdapterLuid=ad.AdapterLuid;
    if (D3DKMTOpenAdapterFromLuid(&open)<0) return 3;
    p.adapter=open.hAdapter;
    auto close=[&] { D3DKMT_CLOSEADAPTER c{}; c.hAdapter=p.adapter; D3DKMTCloseAdapter(&c); };
    if (setOnly) {
        UINT32 v=setValue; const NTSTATUS s=verifier_flags(p.adapter,D3DKMT_VERIFIER_OPTION_SET,v);
        UINT32 q=0; const NTSTATUS sq=verifier_flags(p.adapter,D3DKMT_VERIFIER_OPTION_QUERY,q);
        std::printf("OFFER-STATES SET %s status %08X, query %08X flags %s\n",hex(setValue).c_str(),unsigned(s),unsigned(sq),hex(q).c_str());
        close(); return s>=0 && sq>=0 && q==setValue ? 0 : 4;
    }
    const NTSTATUS sq=verifier_flags(p.adapter,D3DKMT_VERIFIER_OPTION_QUERY,p.original);
    if (sq<0) { std::printf("OFFER-STATES QUERY status %08X\n",unsigned(sq)); close(); return 4; }
    {   FILE *s=nullptr; if (fopen_s(&s,save,"wb") || !s) { close(); return 3; }
        std::fprintf(s,"%s\n",hex(p.original).c_str()); std::fclose(s); }
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    if (FAILED(D3D11CreateDevice(adapter.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,levels,2,D3D11_SDK_VERSION,&p.device,nullptr,
        &p.context)) || FAILED(p.device.As(&p.dxgi))) { close(); return 3; }
    FILE *f=nullptr; if (fopen_s(&f,out,"wb") || !f) { close(); return 3; }
    // The working-set rows first: the shell's residency lines have a start budget of 64 (BD-283), so these two
    // offers are logged whatever the number of offer-state rows.
    std::vector<WsRow> ws;
    ws.push_back(p.working(p.original));
    ws.push_back(p.working(p.original|kAlwaysDiscardOffer|kAlwaysProcessOfferList|kAlwaysDecommitOffer));
    std::vector<Row> rows;
    for (unsigned s:states) for (unsigned w=0;w<5;++w) for (Mode m:{kNow,kNow1,kLater,kDrained}) rows.push_back(p.run(s,w,m));
    UINT32 back=p.original; const NTSTATUS restore=verifier_flags(p.adapter,D3DKMT_VERIFIER_OPTION_SET,back);
    UINT32 after=0; verifier_flags(p.adapter,D3DKMT_VERIFIER_OPTION_QUERY,after);
    bool setOk=true,kept=false,anyRemoved=false;
    for (const Row &r:rows) {
        setOk=setOk && r.set>=0;
        anyRemoved=anyRemoved || FAILED(r.removed);
        if (r.state==5 && SUCCEEDED(r.setup) && SUCCEEDED(r.offer) && SUCCEEDED(r.reclaim) && !r.discarded) kept=true;
    }
    for (const WsRow &w:ws) { setOk=setOk && w.set>=0; anyRemoved=anyRemoved || FAILED(w.removed); }
    char name[256]{}; WideCharToMultiByte(CP_UTF8,0,ad.Description,-1,name,sizeof(name),nullptr,nullptr);
    std::fprintf(f,"{\"tool\":\"offer-states\",\"version\":2,\"pid\":%lu,\"adapter\":\"%s\",\"vendor\":\"%04X\",\"device\":\"%04X\","
        "\"d3d11_umd\":\"%s\",\"original_flags\":\"%s\",\"restore_status\":\"%08X\",\"flags_after\":\"%s\",\"rows\":[",
        GetCurrentProcessId(),name,ad.VendorId,ad.DeviceId,module_path(L"amdgpu_wddm_d3d11.dll").c_str(),hex(p.original).c_str(),
        unsigned(restore),hex(after).c_str());
    for (size_t i=0;i<rows.size();++i) {
        const Row &r=rows[i];
        std::fprintf(f,"%s\n {\"state\":%u,\"workload\":%u,\"copies\":%u,\"mode\":\"%s\",\"flags\":\"%s\",\"set\":\"%08X\","
            "\"setup\":\"%s\",\"drain_ms\":%.3f,\"offer\":\"%s\",\"offer_ms\":%.3f,\"reclaim\":\"%s\",\"reclaim_ms\":%.3f,"
            "\"result\":\"%s\",\"discarded\":%s,\"removed_reason\":\"%s\"}",i?",":"",r.state,r.workload,r.copies,kModeName[r.mode],
            hex(r.flags).c_str(),unsigned(r.set),hex(r.setup).c_str(),r.drain_ms,hex(r.offer).c_str(),r.offer_ms,
            hex(r.reclaim).c_str(),r.reclaim_ms,r.result,r.discarded?"true":"false",hex(r.removed).c_str());
        std::printf("state=%u workload=%u copies=%u mode=%-7s flags=%s set=%08X drain=%.1fms offer=%s %.2fms reclaim=%s %.2fms "
            "result=%s bDiscarded=%d removed=%s\n",r.state,r.workload,r.copies,kModeName[r.mode],hex(r.flags).c_str(),
            unsigned(r.set),r.drain_ms,hex(r.offer).c_str(),r.offer_ms,hex(r.reclaim).c_str(),r.reclaim_ms,r.result,
            r.discarded?1:0,hex(r.removed).c_str());
    }
    std::fprintf(f,"\n],\"working_set_limit\":%llu,\"working_set\":[",kWorkingSetDrop);
    for (size_t i=0;i<ws.size();++i) {
        const WsRow &w=ws[i];
        const long long drop=(long long)w.before-(long long)w.after;
        std::fprintf(f,"%s\n {\"flags\":\"%s\",\"set\":\"%08X\",\"setup\":\"%s\",\"offer\":\"%s\",\"offer_ms\":%.3f,\"before\":%llu,"
            "\"after\":%llu,\"drop\":%lld,\"hlk_limit_met\":%s,\"reclaim\":\"%s\",\"discarded\":%s,\"removed_reason\":\"%s\"}",
            i?",":"",hex(w.flags).c_str(),unsigned(w.set),hex(w.setup).c_str(),hex(w.offer).c_str(),w.offer_ms,w.before,w.after,
            drop,drop>=(long long)kWorkingSetDrop?"true":"false",hex(w.reclaim).c_str(),w.discarded?"true":"false",hex(w.removed).c_str());
        std::printf("working-set flags=%s set=%08X offer=%s %.2fms before=%llu after=%llu drop=%lld limit=%llu %s reclaim=%s "
            "bDiscarded=%d removed=%s\n",hex(w.flags).c_str(),unsigned(w.set),hex(w.offer).c_str(),w.offer_ms,w.before,w.after,
            drop,kWorkingSetDrop,drop>=(long long)kWorkingSetDrop?"MET":"NOT-MET",hex(w.reclaim).c_str(),w.discarded?1:0,
            hex(w.removed).c_str());
    }
    std::fprintf(f,"\n]}\n"); std::fclose(f);
    close();
    std::printf("flags original %s, restore %08X, after %s\n",hex(p.original).c_str(),unsigned(restore),hex(after).c_str());
    if (!setOk || restore<0 || after!=p.original) { std::puts("OFFER-STATES FLAGS"); return 4; }
    std::printf("OFFER-STATES %s\n",kept ? "KEPT" : anyRemoved ? "REMOVED" : "DISCARDED");
    return kept || anyRemoved ? 1 : 0;
}
