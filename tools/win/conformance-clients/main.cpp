// SPDX-License-Identifier: MIT
// amdgpu_wddm_conformance: native D3D12 conformance client for the FL 12_1 and DXR 1.1 gaps (ROV, conservative
// rasterization with SV_InnerCoverage, indirect DispatchRays with and without a count buffer). Offscreen only: no
// window, no swap chain. The Microsoft runtime is loaded from System32, never an application-local copy.
//
//   --interactive DIR --deadline S        lab protocol of tools/win/d3d12queue (BC-250 adapter); the copy verb runs
//   --interactive-warp DIR --deadline S   all three subtests, writes DIR\conformance.json, copy_success = all PASS
//   --adapter warp|bc250|hw|index:N --out DIR [--deadline S] [--debug-layer] [--only rov,cr,dxr]
//                                         one run without the protocol; exit 0 all PASS, 1 a FAIL or ERROR (or a
//                                         debug layer error), 4 SKIP only, 2 usage, 3 no device or GPU work unretired
//   --selftest                            CPU only: oracles, design invariants, negative controls, comparators
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <bcrypt.h>
#include <psapi.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <vector>
#include "gen/raster_vs.h"
#include "gen/rov_ps_ordered.h"
#include "gen/rov_ps_plain.h"
#include "gen/cr_ps_tag.h"
#include "gen/cr_ps_inner.h"
#include "gen/dxr_lib.h"
#include "gen/dxr_args_cs.h"
#include "interactive.h"

using interactive::ComPtr;
namespace {

const char* const usage=
    "amdgpu_wddm_conformance --interactive|--interactive-warp DIR --deadline SECONDS (1..150)\n"
    "                        | --adapter warp|bc250|hw|index:N --out DIR [--deadline SECONDS] [--debug-layer] [--only rov,cr,dxr]\n"
    "                        | --selftest\n"
    "Subtests: rov, conservative, dxr-indirect. One CONFORMANCE line each, then CONFORMANCE overall.";

// ------------------------------------------------------------------------------------------------- self-test
bool expect(bool condition,const char* what,unsigned& failures){
    std::printf("SELFTEST %s %s\n",condition?"ok  ":"FAIL",what);if(!condition)++failures;return condition;
}
int selftest(){
    using namespace interactive;unsigned failures=0;char t[256]{};
    // ROV: design invariants for two seeds, the oracle accepts itself, rejects the reversed order everywhere and a
    // single flipped word exactly once.
    for(UINT32 seed:{0x00000000u,0xC0FFEE11u}){
        const CfRovOracle o=cf_rov_oracle(seed);
        sprintf_s(t,"rov seed %08x: %u triangles, %u layers, coverage %u..%u, discriminating %u, centre-on-edge %u",seed,
            static_cast<unsigned>(o.tris.size()),o.layers,o.layers_min,o.layers_max,o.discriminating,o.ambiguous);
        expect(o.rects_ok && o.tris.size()==43 && o.layers==23 && !o.ambiguous && o.layers_min>=3 && o.discriminating==cf_pixels,t,failures);
        std::vector<UINT32> img(4*cf_pixels);
        for(UINT i=0;i<cf_pixels;++i){img[i]=img[cf_pixels+i]=o.ordered[i];img[2*cf_pixels+i]=img[3*cf_pixels+i]=o.last[i];}
        CfRovCompare c=cf_rov_compare(o,img.data());
        sprintf_s(t,"rov oracle image: rov %u, rt %u, reversed-control %u/%u",c.rov.count,c.targets.count,c.reversed.count,c.reversed.checked);
        expect(!c.rov.count && !c.targets.count && c.reversed.count==cf_pixels,t,failures);
        for(UINT i=0;i<cf_pixels;++i)img[i]=o.reversed[i];
        c=cf_rov_compare(o,img.data());
        sprintf_s(t,"rov reversed-order image rejected: %u/%u mismatches, first %s",c.rov.count,c.rov.checked,c.rov.first().c_str());
        expect(c.rov.count==cf_pixels,t,failures);
        for(UINT i=0;i<cf_pixels;++i)img[i]=o.ordered[i];
        img[2*cf_pixels+5*cf_side+9]^=1u;
        c=cf_rov_compare(o,img.data());
        sprintf_s(t,"rov flipped render-target word rejected: %u mismatch, first %s",c.targets.count,c.targets.first().c_str());
        expect(c.targets.count==1 && c.targets.x==9 && c.targets.y==5,t,failures);
        expect(cf_comparator_selftest(o.ordered),"comparator reports one flipped word at (17,42)",failures);
    }
    // Conservative rasterization: the counts of the exact rational-arithmetic prototype that chose the geometry (kept
    // in the workspace) for tiers 2 and 3, the tier 1 uncertainty band, and the comparators against ideal, swapped
    // and perturbed images.
    for(UINT tier=1;tier<=3;++tier){
        const CfCrOracle o=cf_cr_oracle(tier);
        sprintf_s(t,"cr tier %u: std %u, must %u, may %u, inner %u, differ %u, ambiguous std %u inner %u, overlap %u",
            tier,o.std_cover,o.outer_must,o.outer_may,o.inner_must,o.differ,o.std_ambiguous,o.inner_ambiguous,o.overlap);
        if(tier>=2)expect(o.std_cover==137 && o.outer_must==235 && !o.outer_may && o.inner_must==82 && o.differ==98 &&
                          !o.std_ambiguous && !o.inner_ambiguous && !o.overlap,t,failures);
        else expect(o.std_cover==137 && o.outer_must==235 && o.outer_may>0 && !o.std_ambiguous && !o.overlap,t,failures);
        const bool inner=tier>=3;
        bool front=true;for(const auto& tri:o.tris)front=front && cf_orientation(tri)>0;
        sprintf_s(t,"cr tier %u: all 8 triangles front-facing after reorienting %u",tier,o.reoriented);
        expect(front && o.reoriented==3,t,failures);
        std::vector<UINT32> img(4*cf_pixels);
        for(UINT i=0;i<cf_pixels;++i){img[i]=cf_cr_std_word(o.pixels[i]);img[cf_pixels+i]=cf_cr_cons_word(o.pixels[i]);
            img[2*cf_pixels+i]=img[3*cf_pixels+i]=cf_cr_inner_word(o.pixels[i]);}
        CfCrCompare c=cf_cr_compare(o,img.data(),inner);
        sprintf_s(t,"cr tier %u oracle image accepted: std %u cons %u inner %u; controls std-vs-cons %u cons-vs-std %u",
            tier,c.std_diff.count,c.cons_diff.count,c.inner_diff.count,c.std_vs_cons.count,c.cons_vs_std.count);
        expect(!c.std_diff.count && !c.cons_diff.count && !c.inner_diff.count && c.std_vs_cons.count==o.differ && c.cons_vs_std.count==o.differ,t,failures);
        // Conservative raster OFF where ON was asked (the pass images swapped) must fail both passes.
        std::vector<UINT32> swapped(img);
        for(UINT i=0;i<cf_pixels;++i)std::swap(swapped[i],swapped[cf_pixels+i]);
        c=cf_cr_compare(o,swapped.data(),inner);
        sprintf_s(t,"cr tier %u swapped passes rejected: std %u, cons %u (first %s)",tier,c.std_diff.count,c.cons_diff.count,c.cons_diff.first().c_str());
        expect(c.std_diff.count==o.differ+o.outer_may && c.cons_diff.count==o.differ,t,failures);
        // One must-not pixel set and one must pixel cleared.
        UINT must_not=cf_pixels,must=cf_pixels;
        for(UINT i=0;i<cf_pixels;++i){if(must_not==cf_pixels && o.pixels[i].outer==-1)must_not=i;if(must==cf_pixels && o.pixels[i].outer==1)must=i;}
        if(!expect(must_not<cf_pixels && must<cf_pixels,"cr must and must-not pixels exist",failures))continue;
        std::vector<UINT32> bad(img);bad[cf_pixels+must_not]=1;bad[cf_pixels+must]=0;
        c=cf_cr_compare(o,bad.data(),inner);
        sprintf_s(t,"cr tier %u false positive and false negative rejected: cons %u (first %s)",tier,c.cons_diff.count,c.cons_diff.first().c_str());
        expect(c.cons_diff.count==2,t,failures);
        if(inner){
            // Inner coverage claimed on every covered pixel must fail exactly at the not-fully-covered ones.
            std::vector<UINT32> all(img);unsigned partial=0;
            for(UINT i=0;i<cf_pixels;++i)if(o.pixels[i].outer==1){all[2*cf_pixels+i]|=1u;partial+=o.pixels[i].inner==-1;}
            c=cf_cr_compare(o,all.data(),true);
            sprintf_s(t,"cr inner bit on every covered pixel rejected: %u mismatches, expected %u (first %s); back-face observation %u",
                c.inner_diff.count,partial,c.inner_diff.first().c_str(),c.backface.count);
            expect(c.inner_diff.count==partial && partial>0 && !c.backface.count,t,failures);
        }
    }
    // Indirect DispatchRays: oracle invariants, the count-ignored image (two dispatches where count = 1) rejected in 32
    // words, an unexecuted indirect dispatch (prefill left) rejected in 64 words, and the GPU argument check.
    {
        const CfDxrOracle o=cf_dxr_oracle();
        sprintf_s(t,"dxr oracle: hits %u, misses %u, margin %s, count-discriminating words %u",o.hits,o.misses,o.margin?"kept":"violated",o.count_discriminating);
        expect(o.margin && o.hits && o.misses && o.count_discriminating==32,t,failures);
        unsigned char records[2*sizeof(D3D12_DISPATCH_RAYS_DESC)];cf_dxr_records(0x123456789AC0ull,records);
        std::vector<UINT32> img(cf_sections*cf_section_words),args(2*sizeof(D3D12_DISPATCH_RAYS_DESC)/4);UINT32 counts[2]{1,2};
        std::memcpy(args.data(),records,sizeof(records));
        for(UINT sct=0;sct<cf_sections;++sct)for(UINT i=0;i<cf_section_words;++i)img[sct*cf_section_words+i]=o.words[sct][i];
        CfDxrCompare c=cf_dxr_compare(o,img.data(),args.data(),counts,records);
        unsigned total=c.args.count;for(const auto& d:c.section)total+=d.count;
        sprintf_s(t,"dxr oracle image accepted: %u mismatches; control count1-vs-count2 %u",total,c.count_control.count);
        expect(!total && c.count_control.count==32,t,failures);
        std::vector<UINT32> ignored(img);for(UINT i=0;i<cf_section_words;++i)ignored[2*cf_section_words+i]=o.words[3][i];
        c=cf_dxr_compare(o,ignored.data(),args.data(),counts,records);
        sprintf_s(t,"dxr count buffer ignored rejected: count1-of-max2 %u mismatches (first %s)",c.section[2].count,c.section[2].first().c_str());
        expect(c.section[2].count==32,t,failures);
        std::vector<UINT32> skipped(img);for(UINT i=0;i<cf_section_words;++i)skipped[2*cf_section_words+i]=cf_prefill;
        c=cf_dxr_compare(o,skipped.data(),args.data(),counts,records);
        sprintf_s(t,"dxr skipped indirect dispatch rejected: count1-of-max2 %u mismatches (first %s)",c.section[2].count,c.section[2].first().c_str());
        expect(c.section[2].count==64,t,failures);
        std::vector<UINT32> first_only(img);for(UINT i=64;i<cf_section_words;++i)first_only[4*cf_section_words+i]=cf_prefill;
        c=cf_dxr_compare(o,first_only.data(),args.data(),counts,records);
        sprintf_s(t,"dxr max-2 truncated to one command rejected: max2-no-count %u mismatches",c.section[4].count);
        expect(c.section[4].count==32,t,failures);
        std::vector<UINT32> bad_args(args);bad_args[22]^=0x4u;  // Width of record 0 (byte 88)
        c=cf_dxr_compare(o,img.data(),bad_args.data(),counts,records);
        sprintf_s(t,"dxr wrong GPU argument rejected: %u mismatch (first %s)",c.args.count,c.args.first().c_str());
        expect(c.args.count==1 && c.args.x==22,t,failures);
        D3D12_DISPATCH_RAYS_DESC d{};std::memcpy(&d,records+sizeof(d),sizeof(d));
        expect(d.RayGenerationShaderRecord.StartAddress==0x123456789AC0ull+64 && d.Width==8 && d.Height==4 && d.Depth==1 &&
               d.MissShaderTable.StartAddress==0x123456789AC0ull+128 && d.HitGroupTable.StrideInBytes==32,"dxr record 1 fields",failures);
    }
    std::printf("SELFTEST %s failures=%u\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}

// ------------------------------------------------------------------------------------------------- standalone run
struct Options {std::string adapter,out;unsigned deadline{60},mask{7};bool debug{};};
bool parse_only(const char* text,unsigned& mask){
    mask=0;std::string list=text;size_t start=0;
    while(start<=list.size()){
        const size_t comma=list.find(',',start);const std::string word=list.substr(start,comma==std::string::npos?std::string::npos:comma-start);
        if(word=="rov")mask|=1;else if(word=="cr" || word=="conservative")mask|=2;else if(word=="dxr" || word=="dxr-indirect")mask|=4;else return false;
        if(comma==std::string::npos)break;start=comma+1;
    }
    return mask!=0;
}
HRESULT pick_adapter(interactive::Session& s,const std::string& which,decltype(&D3D12CreateDevice) create){
    using namespace interactive;
    if(which=="warp")return s.api("EnumWarpAdapter",[&]{return s.factory->EnumWarpAdapter(IID_PPV_ARGS(&s.adapter));});
    unsigned wanted=~0u;
    if(which.rfind("index:",0)==0){const unsigned n=seconds(which.c_str()+6);if(!n && which!="index:0")return E_INVALIDARG;wanted=n;}
    for(UINT index=0;;++index){
        ComPtr<IDXGIAdapter1> candidate;HRESULT hr=s.factory->EnumAdapters1(index,&candidate);
        if(hr==DXGI_ERROR_NOT_FOUND)break;if(FAILED(hr))return hr;
        DXGI_ADAPTER_DESC1 d{};if(FAILED(candidate->GetDesc1(&d)))continue;
        bool take=false;
        if(wanted!=~0u)take=index==wanted;
        else if(which=="bc250")take=d.VendorId==0x1002 && d.DeviceId==0x13fe;
        else if(which=="hw")take=!(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) && d.VendorId!=0x1414 &&
                                 SUCCEEDED(create(candidate.Get(),D3D_FEATURE_LEVEL_12_1,__uuidof(ID3D12Device),nullptr));
        if(take){s.adapter=candidate;return S_OK;}
    }
    return DXGI_ERROR_NOT_FOUND;
}
int standalone(const Options& o){
    using namespace interactive;
    const std::filesystem::path root=std::filesystem::absolute(o.out);
    std::error_code ec;std::filesystem::create_directories(root,ec);
    if(!std::filesystem::is_directory(root) || std::filesystem::exists(root/L"trace.jsonl") || std::filesystem::exists(root/L"conformance.json")){
        std::fprintf(stderr,"--out must name a new or empty directory\n");return 2;}
    Session s;s.mode=o.adapter=="warp"?AdapterMode::Warp:AdapterMode::Bc250;s.root=root;s.start=GetTickCount64();s.deadline=s.start+o.deadline*1000ull;
    s.trace=CreateFileW((root/L"trace.jsonl").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(s.trace==INVALID_HANDLE_VALUE)return 2;
    s.set_sequence(1);
    wchar_t path[MAX_PATH]{};
    if(!GetSystemDirectoryW(path,MAX_PATH) || wcscat_s(path,L"\\d3d12.dll"))return 3;
    s.runtime=LoadLibraryExW(path,nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);if(!s.runtime)return 3;
    auto proc=GetProcAddress(s.runtime,"D3D12CreateDevice");decltype(&D3D12CreateDevice) create=nullptr;
    static_assert(sizeof(create)==sizeof(proc));std::memcpy(&create,&proc,sizeof(create));if(!create)return 3;
    if(o.debug){
        auto debug_proc=GetProcAddress(s.runtime,"D3D12GetDebugInterface");decltype(&D3D12GetDebugInterface) get_debug=nullptr;
        std::memcpy(&get_debug,&debug_proc,sizeof(get_debug));ComPtr<ID3D12Debug> debug;
        const HRESULT hr=get_debug?s.api("D3D12GetDebugInterface",[&]{return get_debug(IID_PPV_ARGS(&debug));}):E_NOINTERFACE;
        if(FAILED(hr)){std::fprintf(stderr,"debug layer unavailable hr=%08lx\n",static_cast<unsigned long>(hr));return 3;}
        debug->EnableDebugLayer();s.event("after","EnableDebugLayer");
    }
    HRESULT hr=s.api("CreateDXGIFactory1",[&]{return CreateDXGIFactory1(IID_PPV_ARGS(&s.factory));});if(FAILED(hr))return 3;
    hr=pick_adapter(s,o.adapter,create);
    if(FAILED(hr)){std::fprintf(stderr,"adapter %s not found hr=%08lx\n",o.adapter.c_str(),static_cast<unsigned long>(hr));return 3;}
    const std::string adapter=cf_adapter_text(s.adapter.Get());
    std::printf("runtime=system32/d3d12.dll adapter=%s\n",adapter.c_str());std::fflush(stdout);
    hr=s.api("D3D12CreateDevice FL12_1",[&]{return create(s.adapter.Get(),D3D_FEATURE_LEVEL_12_1,IID_PPV_ARGS(&s.device));});
    if(FAILED(hr)){std::printf("CONFORMANCE overall ERROR reason=D3D12CreateDevice-FL12_1 hr=%08lx\n",static_cast<unsigned long>(hr));return 3;}
    D3D12_COMMAND_QUEUE_DESC queue{};queue.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr=s.api("CreateCommandQueue DIRECT",[&]{return s.device->CreateCommandQueue(&queue,IID_PPV_ARGS(&s.queue));});if(FAILED(hr))return 3;
    s.set_sequence(2);
    CfReport report;HRESULT result=S_OK;
    try{result=conformance_execute(s,report,o.mask);}
    catch(const std::exception& e){std::fprintf(stderr,"conformance failure: %s\n",e.what());result=E_FAIL;}
    // Debug layer: every stored message of severity corruption or error fails the run; warnings are counted.
    std::string debug_json;unsigned errors=0,warnings=0;
    if(o.debug){
        ComPtr<ID3D12InfoQueue> info;
        if(SUCCEEDED(s.device.As(&info))){
            const UINT64 stored=info->GetNumStoredMessages();std::string messages;unsigned listed=0;
            for(UINT64 i=0;i<stored;++i){
                SIZE_T bytes=0;if(FAILED(info->GetMessage(i,nullptr,&bytes)) || !bytes)continue;
                std::vector<unsigned char> buffer(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
                if(FAILED(info->GetMessage(i,m,&bytes)))continue;
                const bool error=m->Severity==D3D12_MESSAGE_SEVERITY_CORRUPTION || m->Severity==D3D12_MESSAGE_SEVERITY_ERROR;
                errors+=error;warnings+=m->Severity==D3D12_MESSAGE_SEVERITY_WARNING;
                if((error || m->Severity==D3D12_MESSAGE_SEVERITY_WARNING) && listed<12){
                    std::printf("DEBUGLAYER %s id=%d %s\n",error?"error":"warning",static_cast<int>(m->ID),m->pDescription);
                    messages+=(listed++?",":"")+cf_json_text(m->pDescription);
                }
            }
            debug_json="\"debug_layer\":{\"enabled\":true,\"stored\":"+std::to_string(stored)+",\"errors\":"+std::to_string(errors)+
                       ",\"warnings\":"+std::to_string(warnings)+",\"messages\":["+messages+"]}";
        } else debug_json="\"debug_layer\":{\"enabled\":true,\"info_queue\":false}";
        std::printf("DEBUGLAYER errors=%u warnings=%u\n",errors,warnings);
    }
    if(!publish(root/L"conformance.json",report.to_json(debug_json)))std::fprintf(stderr,"conformance.json not written\n");
    std::fflush(stdout);
    if(s.pending){CloseHandle(s.trace);s.trace=INVALID_HANDLE_VALUE;ExitProcess(3);}  // unretired GPU work: no Release
    CloseHandle(s.trace);s.trace=INVALID_HANDLE_VALUE;
    if(errors)return 1;
    if(result==S_OK)return 0;
    if(result==DXGI_ERROR_UNSUPPORTED)return 4;
    return report.outcomes.empty()?3:1;
}

}  // namespace

int main(int argc,char** argv){
    if(argc==2 && !std::strcmp(argv[1],"--help")){std::puts(usage);return 0;}
    if(argc==2 && !std::strcmp(argv[1],"--selftest")){
        try{return selftest();}catch(const std::exception& e){std::fprintf(stderr,"selftest failure: %s\n",e.what());return 3;}
    }
    if(argc==5 && interactive::adapter_mode(argv[1])!=interactive::AdapterMode::Invalid && !std::strcmp(argv[3],"--deadline")){
        try{return interactive::run(argv[2],interactive::seconds(argv[4]),interactive::adapter_mode(argv[1]));}
        catch(const std::exception& e){std::fprintf(stderr,"interactive failure: %s\n",e.what());return 3;}
    }
    Options o;
    for(int i=1;i<argc;++i){
        const char* a=argv[i];const bool value=i+1<argc;
        if(!std::strcmp(a,"--adapter") && value)o.adapter=argv[++i];
        else if(!std::strcmp(a,"--out") && value)o.out=argv[++i];
        else if(!std::strcmp(a,"--deadline") && value){o.deadline=interactive::seconds(argv[++i]);if(!o.deadline)return 2;}
        else if(!std::strcmp(a,"--only") && value){if(!parse_only(argv[++i],o.mask))return 2;}
        else if(!std::strcmp(a,"--debug-layer"))o.debug=true;
        else return 2;
    }
    if(o.out.empty() || !(o.adapter=="warp" || o.adapter=="bc250" || o.adapter=="hw" || o.adapter.rfind("index:",0)==0))return 2;
    try{return standalone(o);}
    catch(const std::exception& e){std::fprintf(stderr,"standalone failure: %s\n",e.what());return 3;}
}
