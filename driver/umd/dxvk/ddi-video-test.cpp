// SPDX-License-Identifier: MIT
// The video DDIs (BD-223): RetrieveSubObject and its five table layouts (the two 2.4 layouts: BD-371), the
// processor's formats, colour spaces and caps, the view plans, the statuses of every entry that cannot succeed
// against the reference pages, and the answers of a device whose engine is lost. No GPU: the owner has no engine, so every entry under test answers before the
// engine is reached, or reports the lost engine. Each checker also runs on a mutant that it has to reject (negative
// controls), so a checker that passes everything cannot hide here.
#include "ddi-video.h"
#include "ddi-resource.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <set>
#include <string>
#include <vector>
using namespace bc250::umd;
#define CHECK(x) do { if(!(x)) { std::printf("FAIL video line %d\n",__LINE__);std::fflush(stdout);std::abort(); } } while(0)
namespace {
unsigned reported; HRESULT lastReported;
void APIENTRY count_error(D3D10DDI_HRTCORELAYER,HRESULT hr) { ++reported; lastReported=hr; }

// Every member of a video table is a function pointer. True when none is empty.
bool table_complete(const void *table,SIZE_T size) {
    if (!size || size%sizeof(void *)) return false;
    void *slot=nullptr;
    for (SIZE_T at=0;at<size;at+=sizeof(void *)) {
        std::memcpy(&slot,static_cast<const unsigned char *>(table)+at,sizeof(slot));
        if (!slot) return false;
    }
    return true;
}
using SpaceMap=VideoColorSpace (*)(DXGI_COLOR_SPACE_TYPE);
// The legacy word of each admitted space, from the DXGI names: the matrix from P601/P709 (X601 is the BT.601
// matrix), the YCbCr range from STUDIO/FULL, and the RGB range of an RGB space from STUDIO/FULL.
bool spaces_correct(SpaceMap map) {
    struct Want { DXGI_COLOR_SPACE_TYPE space; bool ycbcr; unsigned rgbRange; unsigned matrix; unsigned nominal; };
    constexpr unsigned studio=D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE_16_235,full=D3D11_1DDI_VIDEO_PROCESSOR_NOMINAL_RANGE_0_255;
    const Want admitted[]={
        {DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,false,0,1,full},
        {DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709,false,1,1,studio},
        {DXGI_COLOR_SPACE_RGB_STUDIO_G24_NONE_P709,false,1,1,studio},
        {DXGI_COLOR_SPACE_YCBCR_FULL_G22_NONE_P709_X601,true,0,0,full},
        {DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601,true,0,0,studio},
        {DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P601,true,0,0,full},
        {DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,true,0,1,studio},
        {DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_LEFT_P709,true,0,1,studio},
        {DXGI_COLOR_SPACE_YCBCR_FULL_G22_LEFT_P709,true,0,1,full},
    };
    std::set<int> seen;
    for (const auto &w:admitted) {
        const auto m=map(w.space);
        if (!m.admitted || m.ycbcr!=w.ycbcr || m.legacy.RGB_Range!=w.rgbRange || m.legacy.YCbCr_Matrix!=w.matrix ||
            m.legacy.Nominal_Range!=w.nominal || m.legacy.Usage || m.legacy.YCbCr_xvYCC) return false;
        seen.insert(int(w.space));
    }
    // Every other defined space is refused: P2020, PQ, HLG, scRGB and the custom value.
    for (int s=0;s<=int(DXGI_COLOR_SPACE_YCBCR_STUDIO_G24_TOPLEFT_P2020);++s)
        if (!seen.count(s) && map(DXGI_COLOR_SPACE_TYPE(s)).admitted) return false;
    if (map(DXGI_COLOR_SPACE_CUSTOM).admitted) return false;
    return true;
}
VideoColorSpace swapped_matrix(DXGI_COLOR_SPACE_TYPE s) {
    auto m=video_color_space(s);
    m.legacy.YCbCr_Matrix=m.legacy.YCbCr_Matrix ? 0u : 1u;
    return m;
}
VideoColorSpace admits_pq(DXGI_COLOR_SPACE_TYPE s) {
    auto m=video_color_space(s);
    if (s==DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) m.admitted=true;
    return m;
}
// The CheckFormatSupport bits of the processor: the two processor bits only, never a decoder or encoder bit.
bool format_bits_correct(UINT (*bits)(DXGI_FORMAT,bool)) {
    constexpr UINT in=D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT,out=D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
    if (bits(DXGI_FORMAT_NV12,true)!=(in|out) || bits(DXGI_FORMAT_P010,true)!=(in|out) || bits(DXGI_FORMAT_YUY2,true)!=(in|out)) return false;
    if (bits(DXGI_FORMAT_B8G8R8A8_UNORM,true)!=(in|out) || bits(DXGI_FORMAT_R10G10B10A2_UNORM,true)!=(in|out)) return false;
    if (bits(DXGI_FORMAT_NV12,false) || bits(DXGI_FORMAT_R16G16B16A16_FLOAT,true) || bits(DXGI_FORMAT_AYUV,true) ||
        bits(DXGI_FORMAT_UNKNOWN,true)) return false;
    for (int f=0;f<=int(DXGI_FORMAT_A4B4G4R4_UNORM);++f)
        if (bits(DXGI_FORMAT(f),true)&~(in|out)) return false;
    return true;
}
constexpr UINT bits_with_decoder(DXGI_FORMAT f,bool engine) {
    return video_format_support_bits(f,engine)|(f==DXGI_FORMAT_NV12 ? UINT(D3D11_1DDI_FORMAT_SUPPORT_DECODER_OUTPUT) : 0u);
}
constexpr UINT real_bits(DXGI_FORMAT f,bool engine) { return video_format_support_bits(f,engine); }

// The reference pages (ref/ddi-display/d3d10umddi.md, outside the repository): page -> the statuses it names.
std::string find_reference() {
    std::vector<std::string> roots;
    char path[MAX_PATH]{};
    const DWORD rootLength=GetEnvironmentVariableA("BC250_ROOT",path,MAX_PATH);
    if (rootLength && rootLength<MAX_PATH) roots.emplace_back(path);
    if (GetCurrentDirectoryA(MAX_PATH,path)) roots.emplace_back(path);
    if (GetModuleFileNameA(nullptr,path,MAX_PATH)) roots.emplace_back(path);
    for (auto dir:roots) {
        for (int up=0;up<12 && !dir.empty();++up) {
            const std::string candidate=dir+"\\ref\\ddi-display\\d3d10umddi.md";
            if (std::ifstream(candidate).good()) return candidate;
            const auto cut=dir.find_last_of("\\/");
            if (cut==std::string::npos) break;
            dir.resize(cut);
        }
    }
    return {};
}
bool page_statuses(const std::string &file,const std::string &page,std::set<std::string> &out,bool &found) {
    std::ifstream in(file);
    std::string line;
    found=false;
    const std::string head="## "+page+" ";
    while (std::getline(in,line)) {
        if (!line.empty() && line.back()=='\r') line.pop_back();
        if (!found) { if (line.rfind(head,0)==0) found=true; continue; }
        if (line.rfind("## ",0)==0) break;
        if (line.size()<3 || line[0]!='|') continue;
        auto end=line.find('|',1);
        if (end==std::string::npos) continue;
        std::string token=line.substr(1,end-1);
        while (!token.empty() && token.front()==' ') token.erase(token.begin());
        while (!token.empty() && token.back()==' ') token.pop_back();
        if (token=="S_OK" || token.rfind("E_",0)==0 || token.rfind("D3DDDIERR_",0)==0 || token.rfind("D3DERR_",0)==0 ||
            token.rfind("DXGI_",0)==0) out.insert(token);
    }
    return found;
}
const char *status_name(HRESULT hr) {
    switch (hr) {
    case S_OK: return "S_OK";
    case E_OUTOFMEMORY: return "E_OUTOFMEMORY";
    case E_INVALIDARG: return "E_INVALIDARG";
    case E_FAIL: return "E_FAIL";
    case D3DDDIERR_DEVICEREMOVED: return "D3DDDIERR_DEVICEREMOVED";
    case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
    default: return "?";
    }
}
// A status the page allows: one it names, or, where it names none, E_OUTOFMEMORY for a create and E_INVALIDARG
// for any other entry (the rule of ddi-video.cpp).
bool status_allowed(const std::set<std::string> &named,const char *entry,HRESULT status) {
    const std::string name=status_name(status);
    if (!named.empty()) return named.count(name)!=0;
    return std::strncmp(entry,"Create",6)==0 ? status==E_OUTOFMEMORY : status==E_INVALIDARG;
}
}
void test_video_ddi() {
    // 1. The three layouts: each sub ID asks for its own size, and one table of the 2.1 layout fills each.
    CHECK(video_table_size(D3D11_1DDI_VIDEO_FUNCTIONS)==sizeof(D3D11_1DDI_VIDEODEVICEFUNCS));
    CHECK(video_table_size(D3DWDDM2_0DDI_VIDEO_FUNCTIONS)==sizeof(D3DWDDM2_0DDI_VIDEODEVICEFUNCS));
    CHECK(video_table_size(D3DWDDM2_1DDI_VIDEO_FUNCTIONS)==sizeof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS));
    CHECK(video_table_size(D3DWDDM2_4DDI_VIDEO_FUNCTIONS_RS4_0)==sizeof(D3DWDDM2_4DDI_VIDEODEVICEFUNCS_RS4_0));
    CHECK(video_table_size(D3DWDDM2_4DDI_VIDEO_FUNCTIONS)==sizeof(D3DWDDM2_4DDI_VIDEODEVICEFUNCS));
    for (UINT32 id:{0u,1u,4u,8u}) CHECK(video_table_size(id)==0);
    const auto table=make_video_device_table();
    CHECK(table_complete(&table,sizeof(table)));
    CHECK(sizeof(table)/sizeof(void *)==96);
    {   // Negative control: one empty slot.
        auto mutant=table; mutant.pfnVideoProcessorBlt=nullptr;
        CHECK(!table_complete(&mutant,sizeof(mutant)));
        auto last=table; last.pfnVideoProcessorSetStreamHDRMetaData=nullptr;
        CHECK(!table_complete(&last,sizeof(last)));
    }
    // 2. RetrieveSubObject: fills exactly the table of the sub ID, and nothing after it.
    const unsigned notesBefore=video_subobject_notes_left();
    for (UINT32 id:{UINT32(D3D11_1DDI_VIDEO_FUNCTIONS),UINT32(D3DWDDM2_0DDI_VIDEO_FUNCTIONS),UINT32(D3DWDDM2_1DDI_VIDEO_FUNCTIONS)}) {
        const SIZE_T size=video_table_size(id);
        std::vector<unsigned char> buffer(size+64,0xCD);
        D3D11_1DDI_VIDEO_INPUT input{FALSE,reinterpret_cast<D3D11_1DDI_VIDEODEVICEFUNCS *>(buffer.data())};
        D3D11_1DDI_VIDEO_OUTPUT output{0x5A};
        CHECK(retrieve_subobject({},id,sizeof(input),&input,sizeof(output),&output)==S_OK);
        CHECK(output.Reserved==0);
        CHECK(table_complete(buffer.data(),size));
        CHECK(std::memcmp(buffer.data(),&table,size)==0);
        for (SIZE_T i=size;i<buffer.size();++i) CHECK(buffer[i]==0xCD);
        input.Relocate=TRUE; // a relocation fills the table again
        std::memset(buffer.data(),0,size);
        CHECK(retrieve_subobject({},id,sizeof(input),&input,0,nullptr)==S_OK);
        CHECK(std::memcmp(buffer.data(),&table,size)==0);
    }
    // The two 2.4 layouts (BD-371): each sub ID gets its own table, exactly its size, and every entry the 2.1 layout
    // shares stays the 2.1 entry. Only the four adapted entries differ, and the MT key exchange is the same entry.
    const auto rs4=make_video_device_table_rs4_0();
    const auto v24=make_video_device_table_2_4();
    CHECK(table_complete(&rs4,sizeof(rs4)) && table_complete(&v24,sizeof(v24)));
    CHECK(sizeof(rs4)/sizeof(void *)==96 && sizeof(v24)/sizeof(void *)==97);
    {
        void *base[96],*a[96],*b[97];
        std::memcpy(base,&table,sizeof(base)); std::memcpy(a,&rs4,sizeof(a)); std::memcpy(b,&v24,sizeof(b));
        const size_t begin=offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnVideoDecoderBeginFrame)/sizeof(void *);
        const size_t submit=offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnVideoDecoderSubmitBuffers1)/sizeof(void *);
        const size_t key=offsetof(D3DWDDM2_1DDI_VIDEODEVICEFUNCS,pfnNegotiateCryptoSessionKeyExchange)/sizeof(void *);
        unsigned changedRs4=0,changed24=0;
        for (size_t i=0;i<96;++i) {
            const bool rs4Changes=i==begin || i==submit, changes24=rs4Changes || i==key;
            if (rs4Changes) { CHECK(a[i]!=base[i]); ++changedRs4; } else CHECK(a[i]==base[i]);
            if (changes24) { CHECK(b[i]!=base[i]); ++changed24; } else CHECK(b[i]==base[i]);
        }
        CHECK(changedRs4==2 && changed24==3 && a[submit]==b[submit] && a[begin]!=b[begin]);
        CHECK(b[96]==b[key]);
    }
    for (UINT32 id:{UINT32(D3DWDDM2_4DDI_VIDEO_FUNCTIONS_RS4_0),UINT32(D3DWDDM2_4DDI_VIDEO_FUNCTIONS)}) {
        const SIZE_T size=video_table_size(id);
        const void *want=id==D3DWDDM2_4DDI_VIDEO_FUNCTIONS ? static_cast<const void *>(&v24) : static_cast<const void *>(&rs4);
        std::vector<unsigned char> buffer(size+64,0xCD);
        D3DWDDM2_4DDI_VIDEO_INPUT input{FALSE,reinterpret_cast<D3DWDDM2_4DDI_VIDEODEVICEFUNCS *>(buffer.data())};
        D3DWDDM2_4DDI_VIDEO_OUTPUT output{0x5A};
        CHECK(retrieve_subobject({},id,sizeof(input),&input,sizeof(output),&output)==S_OK);
        CHECK(output.Reserved==0 && table_complete(buffer.data(),size) && std::memcmp(buffer.data(),want,size)==0);
        for (SIZE_T i=size;i<buffer.size();++i) CHECK(buffer[i]==0xCD);
        input.Relocate=TRUE;
        std::memset(buffer.data(),0,size);
        CHECK(retrieve_subobject({},id,sizeof(input),&input,0,nullptr)==S_OK && std::memcmp(buffer.data(),want,size)==0);
    }
    {   // Unknown IDs are refused and touch nothing; bad arguments are refused.
        std::vector<unsigned char> buffer(sizeof(D3DWDDM2_4DDI_VIDEODEVICEFUNCS),0xCD);
        D3D11_1DDI_VIDEO_INPUT input{FALSE,reinterpret_cast<D3D11_1DDI_VIDEODEVICEFUNCS *>(buffer.data())};
        for (UINT32 id:{1u,4u,8u,99u}) CHECK(retrieve_subobject({},id,sizeof(input),&input,0,nullptr)==E_NOTIMPL);
        for (auto b:buffer) CHECK(b==0xCD);
        CHECK(retrieve_subobject({},D3DWDDM2_1DDI_VIDEO_FUNCTIONS,sizeof(input),nullptr,0,nullptr)==E_INVALIDARG);
        CHECK(retrieve_subobject({},D3DWDDM2_1DDI_VIDEO_FUNCTIONS,sizeof(input)-1,&input,0,nullptr)==E_INVALIDARG);
        D3D11_1DDI_VIDEO_INPUT empty{FALSE,nullptr};
        CHECK(retrieve_subobject({},D3DWDDM2_1DDI_VIDEO_FUNCTIONS,sizeof(empty),&empty,0,nullptr)==E_INVALIDARG);
    }
    // The diagnostic line is bounded: eight in a process, then none.
    CHECK(notesBefore==8 && video_subobject_notes_left()==0);
    // 3. The formats of the processor and their CheckFormatSupport bits.
    CHECK(video_processor_format_support(DXGI_FORMAT_NV12)==(D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT|D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT));
    CHECK(video_processor_format_support(DXGI_FORMAT_P010)==(D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT|D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT));
    CHECK(video_processor_format_support(DXGI_FORMAT_YUY2)==(D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT|D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT));
    CHECK(video_processor_format_support(DXGI_FORMAT_R16G16B16A16_FLOAT)==0 && video_processor_format_support(DXGI_FORMAT_UNKNOWN)==0);
    CHECK(format_bits_correct(real_bits));
    CHECK(!format_bits_correct(bits_with_decoder)); // negative control: a decoder bit
    // The resource bit is separate from the processor bits and codec profiles.
    // Each missing prerequisite must refuse it; unrelated bits cannot admit it.
    for(UINT mask=0;mask<8;mask++) {
        const UINT engine=((mask&1)?UINT(D3D11_FORMAT_SUPPORT_TEXTURE2D):0u)|
            ((mask&2)?UINT(D3D11_FORMAT_SUPPORT_CPU_LOCKABLE):0u)|
            ((mask&4)?UINT(D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT):0u)|
            D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
        for(auto f:{DXGI_FORMAT_NV12,DXGI_FORMAT_P010,DXGI_FORMAT_YUY2})
            CHECK(video_upload_resource_support(f,engine)==
                (mask==7 ? UINT(D3D11_1DDI_FORMAT_SUPPORT_DECODER_OUTPUT):0u));
    }
    for(auto f:{DXGI_FORMAT_UNKNOWN,DXGI_FORMAT_AYUV,DXGI_FORMAT_P016,
                DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_FORMAT_R10G10B10A2_UNORM})
        CHECK(video_upload_resource_support(f,~0u)==0);
    // Directional cache: successful input/output/neither/both queries and a
    // failed query that left plausible bits must all retain their real meaning.
    for(UINT direction:{0u,1u,2u,3u}) {
        VideoFormatSupport queried;
        queried.fill([&](DXGI_FORMAT,UINT *s){*s=direction;return S_OK;});
        CHECK(queried.get(DXGI_FORMAT_YUY2)==direction);
        CHECK(queried.get(DXGI_FORMAT_P010)==direction);
        CHECK(queried.get(DXGI_FORMAT_AYUV)==0);
        UINT engine=((direction&1)?UINT(D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT):0u)|
            ((direction&2)?UINT(D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT):0u);
        CHECK(video_format_support_bits_masked(DXGI_FORMAT_YUY2,engine)==
            (((direction&1)?UINT(D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT):0u)|
             ((direction&2)?UINT(D3D11_1DDI_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT):0u)));
    }
    VideoFormatSupport failedQuery;
    failedQuery.fill([](DXGI_FORMAT,UINT *s){*s=3;return E_FAIL;});
    CHECK(failedQuery.get(DXGI_FORMAT_YUY2)==0);
    // 4. The colour spaces.
    CHECK(spaces_correct(video_color_space));
    CHECK(!spaces_correct(swapped_matrix)); // negative control: 601 and 709 exchanged
    CHECK(!spaces_correct(admits_pq));      // negative control: PQ admitted
    CHECK(video_conversion_supported(DXGI_FORMAT_NV12,DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,
        DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709));
    CHECK(video_conversion_supported(DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
        DXGI_FORMAT_NV12,DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709));
    CHECK(video_conversion_supported(DXGI_FORMAT_P010,DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P601,
        DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709));
    CHECK(!video_conversion_supported(DXGI_FORMAT_NV12,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,
        DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709)); // an RGB space on YCbCr data
    CHECK(!video_conversion_supported(DXGI_FORMAT_P010,DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020,
        DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)); // HDR10: no tone mapping
    CHECK(video_conversion_supported(DXGI_FORMAT_NV12,DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,
        DXGI_FORMAT_P010,DXGI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709));
    // 5. Caps, rates, content.
    const auto caps=video_processor_caps();
    CHECK(caps.MaxInputStreams==1 && caps.MaxStreamStates==1 && caps.RateConversionCapsCount==1);
    CHECK(caps.FeatureCaps==0 && caps.FilterCaps==0 && caps.StereoCaps==0 && caps.AutoStreamCaps==0 && caps.InputFormatCaps==0);
    CHECK(caps.DeviceCaps==(D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_RGB_RANGE_CONVERSION|
        D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_YCbCr_MATRIX_CONVERSION|D3D11_1DDI_VIDEO_PROCESSOR_DEVICE_CAPS_NOMINAL_RANGE));
    const auto rate=video_rate_conversion_caps();
    CHECK(!rate.PastFrames && !rate.FutureFrames && !rate.ConversionCaps && !rate.ITelecineCaps && !rate.CustomRateCount);
    D3D11_1DDI_VIDEO_PROCESSOR_CONTENT_DESC content{D3D11_1DDI_VIDEO_FRAME_FORMAT_PROGRESSIVE,{60,1},1920,1080,{60,1},1920,1080,
        D3D11_1DDI_VIDEO_USAGE_PLAYBACK_NORMAL};
    CHECK(video_content_admitted(content));
    content.InputFrameFormat=D3D11_1DDI_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST;
    CHECK(video_content_admitted(content)); // CreateVideoProcessorEnum has no status for "interlaced"
    const auto engineContent=convert_video_content(content);
    CHECK(engineContent.InputFrameFormat==D3D11_VIDEO_FRAME_FORMAT_INTERLACED_TOP_FIELD_FIRST && engineContent.InputWidth==1920 &&
        engineContent.OutputHeight==1080 && engineContent.InputFrameRate.Numerator==60);
    content.OutputWidth=0;
    CHECK(!video_content_admitted(content));
    content.OutputWidth=16385;
    CHECK(!video_content_admitted(content));
    {   // The legacy word passes through bit for bit, and a colour's four floats too.
        D3D11_1DDI_VIDEO_PROCESSOR_COLOR_SPACE s{}; s.Usage=1; s.RGB_Range=1; s.YCbCr_Matrix=1; s.YCbCr_xvYCC=1; s.Nominal_Range=2;
        const auto e=convert_video_space(s);
        CHECK(e.Usage==1 && e.RGB_Range==1 && e.YCbCr_Matrix==1 && e.YCbCr_xvYCC==1 && e.Nominal_Range==2);
        D3D11_1DDI_VIDEO_COLOR c{}; c.YCbCr={0.25f,0.5f,0.75f,1.0f};
        const auto ec=convert_video_color(c);
        CHECK(ec.YCbCr.Y==0.25f && ec.YCbCr.Cb==0.5f && ec.YCbCr.Cr==0.75f && ec.YCbCr.A==1.0f);
    }
    // 6. The view plans.
    {
        D3D11_1DDIARG_CREATEVIDEOPROCESSORINPUTVIEW in{}; in.MipSlice=0; in.FirstArraySlice=0; in.ArraySize=1;
        D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
        CHECK(plan_video_input_view(in,DXGI_FORMAT_NV12,id) && id.ViewDimension==D3D11_VPIV_DIMENSION_TEXTURE2D && id.Texture2D.ArraySlice==0);
        in.FirstArraySlice=3;
        CHECK(plan_video_input_view(in,DXGI_FORMAT_P010,id) && id.Texture2D.ArraySlice==3);
        in.ArraySize=2;
        CHECK(!plan_video_input_view(in,DXGI_FORMAT_NV12,id));
        in.ArraySize=1;
        CHECK(!plan_video_input_view(in,DXGI_FORMAT_R16G16B16A16_FLOAT,id));
        D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW out{}; out.ArraySize=1;
        D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
        CHECK(plan_video_output_view(out,DXGI_FORMAT_NV12,od) && od.ViewDimension==D3D11_VPOV_DIMENSION_TEXTURE2D);
        out.FirstArraySlice=1;
        CHECK(plan_video_output_view(out,DXGI_FORMAT_B8G8R8A8_UNORM,od) && od.ViewDimension==D3D11_VPOV_DIMENSION_TEXTURE2DARRAY &&
            od.Texture2DArray.FirstArraySlice==1 && od.Texture2DArray.ArraySize==1);
        out.FirstArraySlice=0;
        CHECK(plan_video_output_view(out,DXGI_FORMAT_P010,od) && plan_video_output_view(out,DXGI_FORMAT_YUY2,od));
        out.ArraySize=2;
        CHECK(!plan_video_output_view(out,DXGI_FORMAT_NV12,od));
    }
    // 7. The entries that cannot succeed return the status of their row; each row's status is one its page names.
    {
        D3D10DDI_HDEVICE none{};
        CHECK(table.pfnCreateVideoDecoder(none,nullptr,{},{})==E_OUTOFMEMORY);
        CHECK(table.pfnVideoDecoderExtension(none,{},nullptr)==E_INVALIDARG);
        CHECK(table.pfnVideoDecoderBeginFrame(none,{},nullptr)==E_OUTOFMEMORY);
        CHECK(table.pfnVideoDecoderSubmitBuffers(none,{},0,nullptr)==E_OUTOFMEMORY);
        CHECK(table.pfnVideoDecoderSubmitBuffers1(none,{},0,nullptr)==E_OUTOFMEMORY);
        HANDLE handle=reinterpret_cast<HANDLE>(1);
        CHECK(table.pfnVideoDecoderGetHandle(none,{},&handle)==E_OUTOFMEMORY && !handle);
        CHECK(table.pfnCreateVideoDecoderOutputView(none,nullptr,{},{})==E_OUTOFMEMORY);
        CHECK(table.pfnCreateCryptoSession(none,nullptr,{},{})==E_OUTOFMEMORY);
        CHECK(table.pfnCreateAuthenticatedChannel(none,nullptr,{},{})==E_OUTOFMEMORY);
        CHECK(table.pfnQueryAuthenticatedChannel(none,{},0,nullptr,0,nullptr)==E_INVALIDARG);
        CHECK(table.pfnVideoProcessorGetOutputExtension(none,{},nullptr,0,nullptr)==E_FAIL);
        UINT count=7; table.pfnGetVideoDecoderProfileCount(none,&count); CHECK(count==0);
        BOOL supported=TRUE; table.pfnCheckVideoDecoderFormat(none,nullptr,DXGI_FORMAT_NV12,&supported); CHECK(!supported);
        D3D11_1DDI_VIDEO_CONTENT_PROTECTION_CAPS cp{7,7,7,7};
        CHECK(table.pfnGetContentProtectionCaps(none,nullptr,nullptr,&cp)==S_OK && !cp.Caps && !cp.KeyExchangeTypeCount);
        UINT hints=7;
        CHECK(table.pfnVideoProcessorGetBehaviorHints(none,{},1920,1080,DXGI_FORMAT_NV12,0,nullptr,&hints)==S_OK && hints==0);
        D3DWDDM2_0DDI_CHECK_VIDEO_PROCESSOR_FORMAT_CONVERSION c{DXGI_FORMAT_NV12,D3DDDI_COLOR_SPACE_YCBCR_STUDIO_G22_LEFT_P709,
            DXGI_FORMAT_B8G8R8A8_UNORM,D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709,FALSE};
        table.pfnCheckVideoProcessorFormatConversion(none,{},&c);
        CHECK(!c.Supported); // no live or cached enumerator, no fabricated support
        DdiVideoProcessorEnum known{};
        known.formats.fill([](DXGI_FORMAT,UINT *s){*s=3;return S_OK;});
        D3D11_1DDI_HVIDEOPROCESSORENUM knownHandle{&known};
        table.pfnCheckVideoProcessorFormatConversion(none,knownHandle,&c);
        CHECK(c.Supported);
        known.formats.fill([](DXGI_FORMAT,UINT *s){*s=1;return S_OK;});
        table.pfnCheckVideoProcessorFormatConversion(none,knownHandle,&c);
        CHECK(!c.Supported); // input-only cannot serve the output direction
        D3D11_1DDI_VIDEO_PROCESSOR_CAPS answered{};
        table.pfnGetVideoProcessorCaps(none,{},&answered);
        CHECK(std::memcmp(&answered,&caps,sizeof(caps))==0);
        // The four adapted 2.4 entries answer as their 2.1 entries do, with or without the arguments that only the
        // 2.4 layouts carry: a histogram request, a stripe pattern, a key exchange flag.
        D3DWDDM2_4DDIARG_VIDEODECODERBEGINFRAME_RS4_0 beginRs4{};
        CHECK(rs4.pfnVideoDecoderBeginFrame(none,{},nullptr)==E_OUTOFMEMORY);
        CHECK(rs4.pfnVideoDecoderBeginFrame(none,{},&beginRs4)==E_OUTOFMEMORY);
        beginRs4.Histograms[2].hBuffer.pDrvPrivate=&beginRs4;
        CHECK(rs4.pfnVideoDecoderBeginFrame(none,{},&beginRs4)==E_OUTOFMEMORY);
        D3DWDDM2_4DDIARG_VIDEODECODERBEGINFRAME begin24{};
        CHECK(v24.pfnVideoDecoderBeginFrame(none,{},&begin24)==E_OUTOFMEMORY);
        begin24.Histograms[0].hBuffer.pDrvPrivate=&begin24;
        CHECK(v24.pfnVideoDecoderBeginFrame(none,{},&begin24)==E_OUTOFMEMORY);
        D3DWDDM2_4DDI_VIDEO_DECODER_BUFFER_DESC buffers[2]{};
        CHECK(v24.pfnVideoDecoderSubmitBuffers2(none,{},0,nullptr)==E_OUTOFMEMORY);
        CHECK(v24.pfnVideoDecoderSubmitBuffers2(none,{},2,buffers)==E_OUTOFMEMORY);
        buffers[1].cBlocksStripeEncrypted=1;
        CHECK(rs4.pfnVideoDecoderSubmitBuffers2(none,{},2,buffers)==E_OUTOFMEMORY);
        BYTE key[4]{};
        CHECK(v24.pfnNegotiateCryptoSessionKeyExchange(none,{},D3DWDDM2_4DDI_CRYPTO_SESSION_KEY_EXCHANGE_FLAG_NONE,sizeof(key),key)==E_INVALIDARG);
        CHECK(v24.pfnNegotiateCryptoSessionKeyExchangeMT(none,{},D3DWDDM2_4DDI_CRYPTO_SESSION_KEY_EXCHANGE_FLAG_NONE,sizeof(key),key)==E_INVALIDARG);
        CHECK(v24.pfnNegotiateCryptoSessionKeyExchange(none,{},static_cast<D3DWDDM2_4DDI_CRYPTO_SESSION_KEY_EXCHANGE_FLAGS>(1),
            sizeof(key),key)==E_INVALIDARG);
        CHECK(rs4.pfnNegotiateCryptoSessionKeyExchange(none,{},sizeof(key),key)==E_INVALIDARG);
    }
    const std::string reference=find_reference();
    if (reference.empty()) {
        std::printf("note: ref\\ddi-display\\d3d10umddi.md not found; the video statuses were not checked against the pages\n");
    } else {
        struct Row { const char *entry; const char *page; HRESULT status; };
        // The statuses of the processor entries for a failure (a lost engine, a refused view), next to the refusals.
        const Row processor[]={
            {"CreateVideoProcessorEnum","PFND3D11_1DDI_CREATEVIDEOPROCESSORENUM",E_OUTOFMEMORY},
            {"CreateVideoProcessor","PFND3D11_1DDI_CREATEVIDEOPROCESSOR",E_OUTOFMEMORY},
            {"CreateVideoProcessor","PFND3D11_1DDI_CREATEVIDEOPROCESSOR",D3DDDIERR_DEVICEREMOVED},
            {"CreateVideoProcessorInputView","PFND3D11_1DDI_CREATEVIDEOPROCESSORINPUTVIEW",DXGI_ERROR_UNSUPPORTED},
            {"CreateVideoProcessorInputView","PFND3D11_1DDI_CREATEVIDEOPROCESSORINPUTVIEW",E_OUTOFMEMORY},
            {"CreateVideoProcessorOutputView","PFND3D11_1DDI_CREATEVIDEOPROCESSOROUTPUTVIEW",DXGI_ERROR_UNSUPPORTED},
            {"CreateVideoProcessorOutputView","PFND3D11_1DDI_CREATEVIDEOPROCESSOROUTPUTVIEW",E_OUTOFMEMORY},
            {"VideoProcessorBlt","PFND3D11_1DDI_VIDEOPROCESSORBLT",E_OUTOFMEMORY},
            {"GetContentProtectionCaps","PFND3D11_1DDI_GETCONTENTPROTECTIONCAPS",S_OK},
            {"GetCryptoKeyExchangeType","PFND3D11_1DDI_GETCRYPTOKEYEXCHANGETYPE",S_OK},
            {"VideoProcessorGetBehaviorHints","PFND3DWDDM2_0DDI_VIDEOPROCESSORGETBEHAVIORHINTS",S_OK},
        };
        std::vector<Row> rows(std::begin(processor),std::end(processor));
        for (unsigned i=0;i<kVideoRefusalCount;++i) rows.push_back({kVideoRefusals[i].entry,kVideoRefusals[i].page,kVideoRefusals[i].status});
        for (const auto &r:rows) {
            std::set<std::string> named; bool found=false;
            page_statuses(reference,r.page,named,found);
            if (!found) std::printf("FAIL video: page %s not in the reference\n",r.page);
            CHECK(found);
            if (r.status==S_OK) continue; // S_OK is the answer of every HRESULT entry
            if (!status_allowed(named,r.entry,r.status))
                std::printf("FAIL video: %s returns %s, which %s does not name\n",r.entry,status_name(r.status),r.page);
            CHECK(status_allowed(named,r.entry,r.status));
        }
        {   // Negative controls: device removal from Blt and E_INVALIDARG from CreateVideoProcessorEnum.
            std::set<std::string> named; bool found=false;
            page_statuses(reference,"PFND3D11_1DDI_VIDEOPROCESSORBLT",named,found);
            CHECK(found && !status_allowed(named,"VideoProcessorBlt",D3DDDIERR_DEVICEREMOVED));
            named.clear();
            page_statuses(reference,"PFND3D11_1DDI_CREATEVIDEOPROCESSORENUM",named,found);
            CHECK(found && !status_allowed(named,"CreateVideoProcessorEnum",E_INVALIDARG));
        }
        std::printf("video: %zu statuses checked against %s\n",rows.size(),reference.c_str());
    }
    // 8. A device with no engine: the HRESULT entries answer their page's failure status, the void setters report
    // device removal once each, and the capability answers hold without an engine and report nothing.
    {
        DeviceOwner owner; owner.runtime().UMCallbacks.pfnSetErrorCb=count_error;
        DdiDeviceHandle storage{&owner}; D3D10DDI_HDEVICE h{}; h.pDrvPrivate=&storage;
        reported=0;
        DdiVideoProcessorEnum e{reinterpret_cast<ID3D11VideoProcessorEnumerator *>(1)};
        D3D11_1DDIARG_CREATEVIDEOPROCESSORENUM ea{}; ea.Desc.InputWidth=ea.Desc.OutputWidth=64; ea.Desc.InputHeight=ea.Desc.OutputHeight=64;
        D3D11_1DDI_HVIDEOPROCESSORENUM eh{}; eh.pDrvPrivate=&e;
        CHECK(table.pfnCreateVideoProcessorEnum(h,&ea,eh,{})==E_OUTOFMEMORY && e.object==nullptr);
        DdiVideoProcessor p{reinterpret_cast<ID3D11VideoProcessor *>(1)};
        D3D11_1DDIARG_CREATEVIDEOPROCESSOR pa{}; pa.hVideoProcessorEnum=eh;
        D3D11_1DDI_HVIDEOPROCESSOR ph{}; ph.pDrvPrivate=&p;
        CHECK(table.pfnCreateVideoProcessor(h,&pa,ph,{})==E_OUTOFMEMORY && p.object==nullptr);
        DdiVideoProcessorOutputView ov{reinterpret_cast<ID3D11VideoProcessorOutputView *>(1)};
        D3D11_1DDI_HVIDEOPROCESSOROUTPUTVIEW oh{}; oh.pDrvPrivate=&ov;
        D3D11_1DDIARG_CREATEVIDEOPROCESSOROUTPUTVIEW oa{}; oa.hDrvVideoProcessorEnum=eh; oa.ArraySize=1;
        CHECK(table.pfnCreateVideoProcessorOutputView(h,&oa,oh,{})==E_OUTOFMEMORY && ov.object==nullptr);
        CHECK(table.pfnVideoProcessorBlt(h,ph,oh,0,0,nullptr)==E_OUTOFMEMORY);
        CHECK(reported==0); // an HRESULT entry answers through its return value
        const RECT rect{0,0,64,64};
        table.pfnVideoProcessorSetStreamDestRect(h,ph,0,TRUE,&rect);
        CHECK(reported==1 && lastReported==D3DDDIERR_DEVICEREMOVED);
        table.pfnVideoProcessorSetOutputColorSpace1(h,ph,D3DDDI_COLOR_SPACE_RGB_FULL_G22_NONE_P709);
        CHECK(reported==2 && lastReported==D3DDDIERR_DEVICEREMOVED);
        UINT support=7;
        table.pfnCheckVideoProcessorFormat(h,eh,DXGI_FORMAT_NV12,&support);
        CHECK(support==0); // the failed CreateEnum cleared all cached directions
        e.formats.fill([](DXGI_FORMAT,UINT *s){*s=3;return S_OK;});
        table.pfnCheckVideoProcessorFormat(h,eh,DXGI_FORMAT_NV12,&support);
        CHECK(support==(D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT|D3D11_1DDI_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT));
        D3D11_1DDI_VIDEO_PROCESSOR_RATE_CONVERSION_CAPS rc{7,7,7,7,7};
        table.pfnGetVideoProcessorRateConversionCaps(h,eh,0,&rc);
        CHECK(!rc.PastFrames && !rc.ConversionCaps && !rc.CustomRateCount);
        CHECK(reported==2); // the capability answers report nothing
        // A null device storage: the HRESULT entries still answer their page's status.
        D3D10DDI_HDEVICE none{};
        CHECK(table.pfnCreateVideoProcessorEnum(none,&ea,eh,{})==E_OUTOFMEMORY);
        CHECK(table.pfnVideoProcessorBlt(none,ph,oh,0,0,nullptr)==E_OUTOFMEMORY);
    }
    std::printf("video DDI: ok\n");
}
