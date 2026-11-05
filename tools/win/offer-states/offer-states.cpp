// SPDX-License-Identifier: MIT
// BD-308 and BD-309 repro: the cases of HLK "WDDM - OfferReclaim11 - Offer States" and "WDDM2 - OfferReclaim11 -
// Working Set" that failed on tester.37, as a console program with no window.
//
// Offer states (BD-308). For each offer state, workload and mode it creates a texture, sets the VidMm verifier flags of
// the adapter with D3DKMT_ESCAPE_ADAPTER_VERIFIER_OPTION (escape 29, d3dkmthk.h:2645; option struct d3dkmthk.h:3320,
// flags union d3dkmthk.h:3276), queues a GPU workload on the texture, offers it, reclaims it and prints bDiscarded and
// the time of each call. Two kinds of texture:
//   shared   D3D11_RESOURCE_MISC_SHARED. The HLK test opens its texture again through GetSharedHandle in the
//            WorkloadType 0 cases only (hlk-or12 log-00036, metadataSet0-5), and those cases pass on tester.37.
//   private  no misc flag, with a shader resource view, plus a staging texture of the same size that the CPU writes
//            through Map(WRITE): the sequence of the WorkloadType 1-4 cases, which use no shared handle (the four
//            failures are state 5 of these). Both are offered, as the HLK does (texture, then staging); the texture's
//            reclaim gives bDiscarded, the staging texture is reclaimed after it and read through Map(READ).
// A private texture is engine-private in the D3D11 shell; before the BD-309 engine offer, its offer was a hint and its
// reclaim answered FALSE whatever the verifier flags.
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
// drain with EnqueueSetEvent, a VidMm worker wake-up with flush, K32GetProcessMemoryInfo, OfferResources of the
// staging texture, then the HLK read point: a VidMm worker wake-up (escape 1, D3DKMT_VIDMMESCAPETYPE_WAKE) and
// K32GetProcessMemoryInfo at once. VidMm processes the offer list on its worker thread, so a read without the
// wake-up can come before the decommit (kit-bd309c: the drop of one row showed in the next row). The HLK log does not
// give bFlush, so the probe reads after a wake-up without flush (drop_wake), after a second one with flush (drop, the
// value of hlk_limit_met), and again every 20 ms for at most 1000 ms until the limit is met (drop_settled, information
// only: the HLK does not wait). Then ReclaimResources. Each row's texture stays alive until the three rows are done, so
// that a late free of an earlier row's memory cannot fall into the window of a later row. The HLK test asks for a drop of
// TEXTURE_SIZE - WORKING_SET_THRESHOLD, which its log gives as 3932160 bytes. Run with the original flags, with AlwaysDiscardOffer, AlwaysProcessOfferList
// and AlwaysDecommitOffer (d3dkmthk.h:3283, 3295, 3296), and with AlwaysDiscardOffer and AlwaysProcessOfferList only.
// The offer itself is always the plain OfferResources (no DXGI_OFFER_RESOURCE_FLAG_ALLOW_DECOMMIT). With the verifier
// flags off, VidMm may keep the offered pages until memory pressure, so the first row is information only. The HLK test
// sets adapter verifier flags (escape 29) before its offer; its log does not give their value, so the third row shows
// whether the drop needs the decommit flag.
//
// The program restores the flags of the adapter after each case and at its end. It writes the flags it found to
// --save before it sets any, so that "--set-flags <hex>" can restore them if it is stopped in the middle.
// Output: one line for each case, and one JSON file; the last line is "OFFER-STATES <all> shared <s> private <p>".
// The words judge the state 5 cases only: KEPT (a state 5 case kept the content), DISCARDED (every state 5 case of the
// kind discarded it), NONE (no state 5 case of that kind ran, for example a run of "--states 0"); <all> is REMOVED
// when a device was removed, and NONE when no state 5 case ran. The rows of other states carry their own bDiscarded.
// Exit 0 every state 5 case discarded (or none ran), 1 a state 5 case kept the
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
    unsigned state=0,workload=0,copies=0; Mode mode=kNow; bool shared=true;
    UINT32 flags=0; NTSTATUS set=0;
    HRESULT setup=S_OK,offer=S_OK,reclaim=S_OK,removed=S_OK;
    bool discarded=false; const char *result="";
    double drain_ms=0,offer_ms=0,reclaim_ms=0;
};
struct WsRow {
    UINT32 flags=0; NTSTATUS set=0,wake_pre=0,wake=0,wake_flush=0;
    HRESULT setup=S_OK,offer=S_OK,reclaim=S_OK,removed=S_OK;
    // after_wake: after the wake-up without flush; after: after the wake-up with flush (the HLK read point);
    // after_settled: the last of the bounded re-reads (settle_ms after the flush wake-up).
    unsigned long long before=0,after_wake=0,after=0,after_settled=0; bool discarded=false;
    double offer_ms=0,wake_ms=0,wake_flush_ms=0,settle_ms=0;
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

// Wakes the VidMm worker thread: escape 1 (D3DKMT_ESCAPE_VIDMM, d3dkmthk.h:2614) of type D3DKMT_VIDMMESCAPETYPE_WAKE
// (d3dkmthk.h:2736, Wake.bFlush d3dkmthk.h:3042). The HLK Working Set test sends this escape after its offers and reads
// the working set at once ("Waking VidMm worker thread", hlk-or16 log-00037). Its log does not give bFlush.
NTSTATUS wake_vidmm(D3DKMT_HANDLE adapter,BOOL flush) {
    D3DKMT_VIDMM_ESCAPE v{}; v.Type=D3DKMT_VIDMMESCAPETYPE_WAKE; v.Wake.bFlush=flush;
    D3DKMT_ESCAPE e{}; e.hAdapter=adapter; e.Type=D3DKMT_ESCAPE_VIDMM;
    e.pPrivateDriverData=&v; e.PrivateDriverDataSize=sizeof(v);
    return D3DKMTEscape(&e);
}

struct Probe {
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context; ComPtr<IDXGIDevice4> dxgi;
    D3DKMT_HANDLE adapter=0; UINT32 original=0;
    // The staging textures of the working-set rows stay alive until all of those rows are done: the hosted ICD frees
    // memory later than the release (deferred destroy), and a free in the window of a later row would show as that
    // row's drop.
    std::vector<ComPtr<ID3D11Texture2D>> keep;
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
    // The HLK sequence of the WorkloadType 1-4 cases (hlk-or12 log-00036): CreateTexture2D, CreateShaderResourceView,
    // CreateTexture2D of the staging copy, Map(WRITE), escape 29, the workload, OfferResources of the texture and of the
    // staging copy, ReclaimResources of both, Map(READ) of the staging copy. The texture has no misc flag.
    Row run_private(unsigned state,unsigned workload,Mode mode) {
        Row r; r.state=state; r.workload=workload; r.mode=mode; r.copies=kCopies[workload]; r.shared=false;
        D3D11_TEXTURE2D_DESC d{}; d.Width=2048; d.Height=2048; d.MipLevels=1; d.ArraySize=1;
        d.Format=DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc={1,0}; d.Usage=D3D11_USAGE_DEFAULT;
        d.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        D3D11_TEXTURE2D_DESC sd=d; sd.Usage=D3D11_USAGE_STAGING; sd.BindFlags=0;
        sd.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE|D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> t,other,staging; ComPtr<ID3D11ShaderResourceView> srv; ComPtr<ID3D11RenderTargetView> rtv;
        r.setup=device->CreateTexture2D(&d,nullptr,&t);
        if (SUCCEEDED(r.setup)) r.setup=device->CreateShaderResourceView(t.Get(),nullptr,&srv);
        if (SUCCEEDED(r.setup)) r.setup=device->CreateTexture2D(&sd,nullptr,&staging);
        if (SUCCEEDED(r.setup)) r.setup=device->CreateTexture2D(&d,nullptr,&other);
        if (SUCCEEDED(r.setup)) r.setup=device->CreateRenderTargetView(t.Get(),nullptr,&rtv);
        if (FAILED(r.setup)) return r;
        D3D11_MAPPED_SUBRESOURCE m{}; r.setup=context->Map(staging.Get(),0,D3D11_MAP_WRITE,0,&m);
        if (FAILED(r.setup)) return r;
        for (UINT y=0;y<sd.Height;++y) std::memset(static_cast<unsigned char *>(m.pData)+size_t(y)*m.RowPitch,0x3C,size_t(sd.Width)*4);
        context->Unmap(staging.Get(),0);
        const float colour[4]={0.25f,0.5f,0.75f,1.f};
        context->ClearRenderTargetView(rtv.Get(),colour);
        drain();
        r.flags=state==5 ? (original|kAlwaysDiscardOffer) : original;
        UINT32 v=r.flags; r.set=verifier_flags(adapter,D3DKMT_VERIFIER_OPTION_SET,v);
        for (unsigned i=0;i<r.copies;++i) { context->CopyResource(other.Get(),t.Get()); context->CopyResource(t.Get(),other.Get()); }
        context->Flush();
        if (mode==kDrained) r.drain_ms=drain();
        ComPtr<IDXGIResource> res,sres; t.As(&res); staging.As(&sres);
        IDXGIResource *list[]={res.Get()}, *slist[]={sres.Get()};
        double a=now_ms();
        r.offer=dxgi->OfferResources(1,list,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL);
        r.offer_ms=now_ms()-a;
        const HRESULT soffer=SUCCEEDED(r.offer) ? dxgi->OfferResources(1,slist,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL) : E_ABORT;
        if (mode==kLater) Sleep(200);
        if (SUCCEEDED(r.offer)) {
            a=now_ms();
            BOOL discarded=FALSE; r.reclaim=dxgi->ReclaimResources(1,list,&discarded);
            r.discarded=discarded!=FALSE; r.result=r.discarded ? "discarded" : "ok";
            r.reclaim_ms=now_ms()-a;
            if (SUCCEEDED(soffer)) {
                BOOL dummy=FALSE; const HRESULT sr=dxgi->ReclaimResources(1,slist,&dummy);
                if (SUCCEEDED(sr) && SUCCEEDED(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&m))) context->Unmap(staging.Get(),0);
                else if (FAILED(sr)) r.reclaim=sr;
            }
        } else r.result="not-offered";
        if (FAILED(soffer) && SUCCEEDED(r.offer)) r.offer=soffer;
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
        keep.push_back(staging);
        D3D11_MAPPED_SUBRESOURCE m{}; w.setup=context->Map(staging.Get(),0,D3D11_MAP_WRITE,0,&m);
        if (FAILED(w.setup)) return w;
        for (UINT y=0;y<d.Height;++y) std::memset(static_cast<unsigned char *>(m.pData)+size_t(y)*m.RowPitch,0x5A,size_t(d.Width)*4);
        context->Unmap(staging.Get(),0);
        drain();
        // A flush wake-up before the first read: work that VidMm still has from the row before (a late decommit) ends
        // before this row's "before" value.
        w.wake_pre=wake_vidmm(adapter,TRUE);
        UINT32 v=flags; w.set=verifier_flags(adapter,D3DKMT_VERIFIER_OPTION_SET,v);
        w.before=working_set();
        ComPtr<IDXGIResource> res; staging.As(&res); IDXGIResource *list[]={res.Get()};
        double a=now_ms(); w.offer=dxgi->OfferResources(1,list,DXGI_OFFER_RESOURCE_PRIORITY_NORMAL); w.offer_ms=now_ms()-a;
        a=now_ms(); w.wake=wake_vidmm(adapter,FALSE); w.wake_ms=now_ms()-a;
        w.after_wake=working_set();
        a=now_ms(); w.wake_flush=wake_vidmm(adapter,TRUE); w.wake_flush_ms=now_ms()-a;
        w.after=working_set();
        // Bounded settle: re-read every 20 ms for at most 1000 ms, stop when the drop meets the limit.
        a=now_ms(); w.after_settled=w.after;
        while ((long long)w.before-(long long)w.after_settled<(long long)kWorkingSetDrop && now_ms()-a<1000.0) {
            Sleep(20); w.after_settled=working_set();
        }
        w.settle_ms=now_ms()-a;
        if (SUCCEEDED(w.offer)) { BOOL discarded=FALSE; w.reclaim=dxgi->ReclaimResources(1,list,&discarded); w.discarded=discarded!=FALSE; }
        UINT32 back=original; verifier_flags(adapter,D3DKMT_VERIFIER_OPTION_SET,back);
        w.removed=device->GetDeviceRemovedReason();
        return w;
    }
};
}

int main(int argc,char **argv) {
    const char *usage="offer-states --out result.json --save flags.txt [--states 0,5] [--texture shared|private|both] [--any-adapter]\n"
                      "offer-states --set-flags <hex> [--any-adapter]";
    if (argc==2 && !std::strcmp(argv[1],"--help")) { std::puts(usage); return 0; }
    const char *out=nullptr,*save=nullptr; bool anyAdapter=false,setOnly=false; UINT32 setValue=0;
    std::vector<unsigned> states{0,5}; bool doShared=true,doPrivate=true;
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
        else if (!std::strcmp(argv[i],"--texture") && i+1<argc) { const char *k=argv[++i];
            if (!std::strcmp(k,"shared")) doPrivate=false; else if (!std::strcmp(k,"private")) doShared=false; else if (std::strcmp(k,"both")) return 2; }
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
    // The working-set rows first: the shell's residency lines have a start budget of 64 (BD-283), so these three
    // offers are logged whatever the number of offer-state rows.
    std::vector<WsRow> ws;
    ws.push_back(p.working(p.original));
    ws.push_back(p.working(p.original|kAlwaysDiscardOffer|kAlwaysProcessOfferList|kAlwaysDecommitOffer));
    ws.push_back(p.working(p.original|kAlwaysDiscardOffer|kAlwaysProcessOfferList));
    p.keep.clear();
    std::vector<Row> rows;
    if (doShared) for (unsigned s:states) for (unsigned w=0;w<5;++w) for (Mode m:{kNow,kNow1,kLater,kDrained}) rows.push_back(p.run(s,w,m));
    if (doPrivate) for (unsigned s:states) for (unsigned w=0;w<5;++w) for (Mode m:{kNow,kLater,kDrained}) rows.push_back(p.run_private(s,w,m));
    UINT32 back=p.original; const NTSTATUS restore=verifier_flags(p.adapter,D3DKMT_VERIFIER_OPTION_SET,back);
    UINT32 after=0; verifier_flags(p.adapter,D3DKMT_VERIFIER_OPTION_QUERY,after);
    bool setOk=true,kept=false,anyRemoved=false,keptShared=false,keptPrivate=false,anyShared=false,anyPrivate=false;
    for (const Row &r:rows) {
        setOk=setOk && r.set>=0;
        anyRemoved=anyRemoved || FAILED(r.removed);
        // The summary words judge the state 5 cases only (see the header): a run without one says NONE
        if (r.state==5)
            (r.shared ? anyShared : anyPrivate)=true;
        if (r.state==5 && SUCCEEDED(r.setup) && SUCCEEDED(r.offer) && SUCCEEDED(r.reclaim) && !r.discarded) { kept=true; (r.shared ? keptShared : keptPrivate)=true; }
    }
    for (const WsRow &w:ws) { setOk=setOk && w.set>=0; anyRemoved=anyRemoved || FAILED(w.removed); }
    char name[256]{}; WideCharToMultiByte(CP_UTF8,0,ad.Description,-1,name,sizeof(name),nullptr,nullptr);
    std::fprintf(f,"{\"tool\":\"offer-states\",\"version\":6,\"pid\":%lu,\"adapter\":\"%s\",\"vendor\":\"%04X\",\"device\":\"%04X\","
        "\"d3d11_umd\":\"%s\",\"original_flags\":\"%s\",\"restore_status\":\"%08X\",\"flags_after\":\"%s\",\"rows\":[",
        GetCurrentProcessId(),name,ad.VendorId,ad.DeviceId,module_path(L"amdgpu_wddm_d3d11.dll").c_str(),hex(p.original).c_str(),
        unsigned(restore),hex(after).c_str());
    for (size_t i=0;i<rows.size();++i) {
        const Row &r=rows[i];
        std::fprintf(f,"%s\n {\"texture\":\"%s\",\"state\":%u,\"workload\":%u,\"copies\":%u,\"mode\":\"%s\",\"flags\":\"%s\",\"set\":\"%08X\","
            "\"setup\":\"%s\",\"drain_ms\":%.3f,\"offer\":\"%s\",\"offer_ms\":%.3f,\"reclaim\":\"%s\",\"reclaim_ms\":%.3f,"
            "\"result\":\"%s\",\"discarded\":%s,\"removed_reason\":\"%s\"}",i?",":"",r.shared?"shared":"private",r.state,r.workload,r.copies,kModeName[r.mode],
            hex(r.flags).c_str(),unsigned(r.set),hex(r.setup).c_str(),r.drain_ms,hex(r.offer).c_str(),r.offer_ms,
            hex(r.reclaim).c_str(),r.reclaim_ms,r.result,r.discarded?"true":"false",hex(r.removed).c_str());
        std::printf("texture=%-7s state=%u workload=%u copies=%u mode=%-7s flags=%s set=%08X drain=%.1fms offer=%s %.2fms reclaim=%s %.2fms "
            "result=%s bDiscarded=%d removed=%s\n",r.shared?"shared":"private",r.state,r.workload,r.copies,kModeName[r.mode],hex(r.flags).c_str(),
            unsigned(r.set),r.drain_ms,hex(r.offer).c_str(),r.offer_ms,hex(r.reclaim).c_str(),r.reclaim_ms,r.result,
            r.discarded?1:0,hex(r.removed).c_str());
    }
    std::fprintf(f,"\n],\"working_set_limit\":%llu,\"working_set\":[",kWorkingSetDrop);
    for (size_t i=0;i<ws.size();++i) {
        const WsRow &w=ws[i];
        const long long drop=(long long)w.before-(long long)w.after;
        const long long dropWake=(long long)w.before-(long long)w.after_wake;
        const long long dropSettled=(long long)w.before-(long long)w.after_settled;
        const long long limit=(long long)kWorkingSetDrop;
        std::fprintf(f,"%s\n {\"flags\":\"%s\",\"set\":\"%08X\",\"setup\":\"%s\",\"offer\":\"%s\",\"offer_ms\":%.3f,"
            "\"wake_pre\":\"%08X\",\"wake\":\"%08X\",\"wake_ms\":%.3f,\"wake_flush\":\"%08X\",\"wake_flush_ms\":%.3f,"
            "\"before\":%llu,\"after_wake\":%llu,\"drop_wake\":%lld,\"limit_met_wake\":%s,"
            "\"after\":%llu,\"drop\":%lld,\"hlk_limit_met\":%s,"
            "\"after_settled\":%llu,\"drop_settled\":%lld,\"limit_met_settled\":%s,\"settle_ms\":%.3f,"
            "\"reclaim\":\"%s\",\"discarded\":%s,\"removed_reason\":\"%s\"}",
            i?",":"",hex(w.flags).c_str(),unsigned(w.set),hex(w.setup).c_str(),hex(w.offer).c_str(),w.offer_ms,
            unsigned(w.wake_pre),unsigned(w.wake),w.wake_ms,unsigned(w.wake_flush),w.wake_flush_ms,
            w.before,w.after_wake,dropWake,dropWake>=limit?"true":"false",
            w.after,drop,drop>=limit?"true":"false",
            w.after_settled,dropSettled,dropSettled>=limit?"true":"false",w.settle_ms,
            hex(w.reclaim).c_str(),w.discarded?"true":"false",hex(w.removed).c_str());
        std::printf("working-set flags=%s set=%08X offer=%s %.2fms wake=%08X/%08X/%08X before=%llu wake-only=%lld "
            "after=%llu drop=%lld limit=%llu %s settled=%lld after %.0fms reclaim=%s bDiscarded=%d removed=%s\n",
            hex(w.flags).c_str(),unsigned(w.set),hex(w.offer).c_str(),w.offer_ms,unsigned(w.wake_pre),unsigned(w.wake),
            unsigned(w.wake_flush),w.before,dropWake,w.after,drop,kWorkingSetDrop,drop>=limit?"MET":"NOT-MET",dropSettled,
            w.settle_ms,hex(w.reclaim).c_str(),w.discarded?1:0,hex(w.removed).c_str());
    }
    std::fprintf(f,"\n]}\n"); std::fclose(f);
    close();
    std::printf("flags original %s, restore %08X, after %s\n",hex(p.original).c_str(),unsigned(restore),hex(after).c_str());
    if (!setOk || restore<0 || after!=p.original) { std::puts("OFFER-STATES FLAGS"); return 4; }
    auto word=[&](bool any,bool k) { return !any ? "NONE" : k ? "KEPT" : "DISCARDED"; };
    std::printf("OFFER-STATES %s shared %s private %s\n",
        kept ? "KEPT" : anyRemoved ? "REMOVED" : (anyShared || anyPrivate) ? "DISCARDED" : "NONE",word(anyShared,keptShared),
        word(anyPrivate,keptPrivate));
    return kept || anyRemoved ? 1 : 0;
}
