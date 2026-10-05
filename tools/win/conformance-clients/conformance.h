// SPDX-License-Identifier: MIT
// Conformance variant of the interactive client: three FL 12_1 / DXR 1.1 subtests with exact CPU oracles.
//
//   rov          Rasterizer ordered views. 43 overlapping triangles in ONE draw, each pixel covered by 3 to 9 of
//                them, run an order-dependent read-modify-write (v = v * 0x01000193 + (tag ^ pixel)) through a
//                RasterizerOrderedTexture2D<uint>. Oracle: the fold over the covering primitives in API order.
//                Controls: the reversed-order fold differs at every pixel (the comparison can fail); the render
//                target (last tag, ordered by the output merger) checks the coverage the oracle assumed; the same
//                program through a plain RWTexture2D is recorded as an observation only (no ordering guarantee).
//   conservative Conservative rasterization at the reported tier. Eight triangles in separate 16x16 cells (six thin,
//                sub-pixel or corner cases, two larger ones); pass std (off), pass cons (on), and with tier 3 pass inner
//                (on, SV_InnerCoverage). Oracles from the rules of ConservativeRasterization.md (DirectX-Specs 5a4139be):
//                a pixel MUST be covered when the triangle overlaps its square (here: overlaps the square shrunk by
//                1/64), MUST NOT be covered when the triangle stays outside the square grown by the tier's
//                uncertainty region (tier 1: 1/2 pixel, tier 2 and 3: 1/256) plus 1/64; between the two it MAY be. The
//                geometry keeps that band empty under tier 2 and 3, so the expected set is exact; under tier 1 the band
//                is reported and accepted either way. Inner coverage bit 0 MUST be set when the square grown by 1/256
//                plus 1/64 is inside the triangle and MUST be clear when the square shrunk by 1/64 is not. Vertices lie
//                on a 1/16 grid, so the 16.8 snap is exact. Controls: the std oracle differs from the cons oracle (98
//                pixels at tier 2/3), so either image fails the other's oracle. The decisive passes draw every
//                triangle front-facing (clockwise); with tier 3 a fourth pass draws the same triangles back-facing with
//                SV_InnerCoverage, reported as an observation only (WARP 10.0.26100 sets bit 0 on every covered pixel
//                of a back-facing triangle, the RTX 4090 does not; the spec does not distinguish the facing).
//   dxr-indirect A DXR pipeline (two raygen records, miss, triangle hit group) over one BLAS/TLAS triangle. A compute
//                shader writes two D3D12_DISPATCH_RAYS_DESC records and the count words 1 and 2 into DEFAULT buffers
//                the CPU never maps; then five sections of one output buffer are written by: direct DispatchRays
//                (positive control), ExecuteIndirect max 1 without count, max 2 with count 1, max 2 with count 2, and
//                max 2 without count. Every output word names its raygen record and dispatch dimensions. Oracle: the
//                exact words per section, untouched words keep the prefill. Controls: the count-1 and count-2
//                oracles differ in 32 words; the GPU-written arguments are read back and compared with the CPU's. A
//                refused DISPATCH_RAYS command signature is a FAIL with that HRESULT: the direct section still runs,
//                the four indirect sections keep the prefill.
//
// Included by tools\win\d3d12queue\interactive.h inside namespace interactive, after Session (build define
// INTERACTIVE_CONFORMANCE); the "copy" verb then runs conformance_run(). main.cpp includes the standard headers and
// the dxc programs (gen\*.h) before interactive.h, since this file is inside the namespace. build.ps1 puts this
// directory and d3d12queue on the include path, so both quoted includes resolve.

constexpr UINT cf_side=64;
constexpr UINT cf_pixels=cf_side*cf_side;
constexpr UINT64 cf_image_bytes=UINT64{cf_pixels}*4;
static_assert(cf_side*4==D3D12_TEXTURE_DATA_PITCH_ALIGNMENT,"rows of the 64x64 R32 images are tight");
static_assert(cf_image_bytes%D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT==0,"image regions are placement aligned");
constexpr double cf_margin=1.0/64.0;   // robustness margin of every oracle classification, in pixels

// ---------------------------------------------------------------------------------------------------------------
// Report records

struct CfFeatures {
    HRESULT options_hr{E_FAIL},options5_hr{E_FAIL},levels_hr{E_FAIL},model_hr{E_FAIL};
    BOOL rovs{};UINT cr_tier{};UINT rt_tier{};UINT max_level{};UINT model{};
};
inline std::string cf_level_text(UINT level){char t[16]{};sprintf_s(t,"%u_%u",(level>>12)&0xfu,(level>>8)&0xfu);return t;}
inline std::string cf_rt_text(UINT tier){if(!tier)return "0";char t[16]{};sprintf_s(t,"%u.%u",tier/10,tier%10);return t;}
inline std::string cf_model_text(UINT model){char t[16]{};sprintf_s(t,"%u_%u",model>>4,model&0xfu);return t;}
inline std::string cf_json_text(const std::string& value){
    std::string out="\"";
    for(char c:value){
        if(c=='"' || c=='\\'){out+='\\';out+=c;}
        else if(static_cast<unsigned char>(c)<0x20){char t[8]{};sprintf_s(t,"\\u%04x",static_cast<unsigned>(static_cast<unsigned char>(c)));out+=t;}
        else out+=c;
    }
    return out+"\"";
}

// One subtest. status: PASS, FAIL, SKIP (a required tier is not reported) or ERROR (the test could not run).
struct CfOutcome {
    std::string name,status{"ERROR"},first{"none"},detail,reason,json;
    HRESULT hr{S_OK};unsigned mismatches{},checked{};
    std::string line()const{
        std::string text="CONFORMANCE "+name+" "+status+" mismatches="+std::to_string(mismatches)+" checked="+std::to_string(checked)+" first="+first;
        if(!detail.empty())text+=" "+detail;
        if(!reason.empty())text+=" reason="+reason;
        char t[24]{};sprintf_s(t," hr=%08lx",static_cast<unsigned long>(hr));return text+t;
    }
    std::string to_json()const{
        char t[24]{};sprintf_s(t,"%08lx",static_cast<unsigned long>(hr));
        return "{\"name\":"+cf_json_text(name)+",\"status\":"+cf_json_text(status)+",\"hr\":\""+t+"\",\"mismatches\":"+std::to_string(mismatches)+
            ",\"checked\":"+std::to_string(checked)+",\"first\":"+cf_json_text(first)+",\"reason\":"+cf_json_text(reason)+
            ",\"line\":"+cf_json_text(line())+(json.empty()?"":","+json)+"}";
    }
};

// Objects a submission may still use. leak is set from ExecuteCommandLists until the fence proves retirement; on any
// unproven path the references are detached, never released (as Session does for its own objects).
struct CfKeep {
    std::vector<ComPtr<IUnknown>> objects;bool leak{};
    CfKeep()=default;CfKeep(const CfKeep&)=delete;CfKeep& operator=(const CfKeep&)=delete;
    template<class T> T* hold(const ComPtr<T>& object){if(object)objects.emplace_back(object.Get());return object.Get();}
    ~CfKeep(){if(leak)for(auto& object:objects)object.Detach();}
};

// Mismatch counter of one comparison; the first mismatch keeps its coordinates (pixel x, y or word, section).
struct CfDiff {
    unsigned count{},checked{};int x{-1},y{-1};UINT32 got{},want{};std::string where;
    void check(bool ok,int px,int py,UINT32 g,UINT32 w,const char* in){++checked;if(!ok && !count++){x=px;y=py;got=g;want=w;where=in;}}
    std::string first()const{
        if(!count)return "none";
        char t[128]{};sprintf_s(t,"%s:x=%d,y=%d,got=0x%08x,want=0x%08x",where.c_str(),x,y,got,want);return t;
    }
};

// ---------------------------------------------------------------------------------------------------------------
// Geometry shared by the CPU oracles (exact: every coordinate is a multiple of 1/256 below 256, so the products below
// stay within 53 bits)

struct CfTri {double v[3][2];UINT32 tag;};
inline double cf_edge(const double* a,const double* b,double px,double py){return (b[0]-a[0])*(py-a[1])-(b[1]-a[1])*(px-a[0]);}
inline double cf_orientation(const CfTri& t){return cf_edge(t.v[0],t.v[1],t.v[2][0],t.v[2][1])>0?1.0:-1.0;}
// Pixel centre test of standard rasterization: 1 inside, 0 outside, -1 on an edge line (top-left rule territory,
// which an oracle must not depend on).
inline int cf_center_exact(const CfTri& t,double px,double py){
    int positive=0,negative=0,zero=0;
    for(int i=0;i<3;++i){const double e=cf_edge(t.v[i],t.v[(i+1)%3],px,py);positive+=e>0;negative+=e<0;zero+=e==0;}
    if(!zero)return (positive==3 || negative==3)?1:0;
    return (positive && negative)?0:-1;
}
// The same with a margin: 1 inside by at least margin from every edge, 0 outside by at least margin past one edge,
// -1 otherwise.
inline int cf_center_margin(const CfTri& t,double px,double py,double margin){
    const double sign=cf_orientation(t);bool edge_band=false;
    for(int i=0;i<3;++i){
        const double* a=t.v[i];const double* b=t.v[(i+1)%3];
        const double e=cf_edge(a,b,px,py)*sign,length=std::sqrt((b[0]-a[0])*(b[0]-a[0])+(b[1]-a[1])*(b[1]-a[1]));
        if(e<=-margin*length)return 0;
        if(e<margin*length)edge_band=true;
    }
    return edge_band?-1:1;
}
// Closed triangle against closed axis-aligned box, separating axis test on x, y and the three edge normals.
inline bool cf_intersects(const CfTri& t,double x0,double y0,double x1,double y1){
    double minx=t.v[0][0],maxx=minx,miny=t.v[0][1],maxy=miny;
    for(int i=1;i<3;++i){minx=(std::min)(minx,t.v[i][0]);maxx=(std::max)(maxx,t.v[i][0]);miny=(std::min)(miny,t.v[i][1]);maxy=(std::max)(maxy,t.v[i][1]);}
    if(maxx<x0 || minx>x1 || maxy<y0 || miny>y1)return false;
    const double box[4][2]{{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
    for(int i=0;i<3;++i){
        const double* a=t.v[i];const double* b=t.v[(i+1)%3];const double nx=-(b[1]-a[1]),ny=b[0]-a[0];
        double tmin=nx*t.v[0][0]+ny*t.v[0][1],tmax=tmin,bmin=nx*box[0][0]+ny*box[0][1],bmax=bmin;
        for(int k=1;k<3;++k){const double p=nx*t.v[k][0]+ny*t.v[k][1];tmin=(std::min)(tmin,p);tmax=(std::max)(tmax,p);}
        for(int k=1;k<4;++k){const double p=nx*box[k][0]+ny*box[k][1];bmin=(std::min)(bmin,p);bmax=(std::max)(bmax,p);}
        if(bmax<tmin || bmin>tmax)return false;
    }
    return true;
}
// Box strictly inside the triangle (all four corners strictly inside).
inline bool cf_box_inside(const CfTri& t,double x0,double y0,double x1,double y1){
    const double sign=cf_orientation(t);const double box[4][2]{{x0,y0},{x1,y0},{x1,y1},{x0,y1}};
    for(const auto& c:box)for(int i=0;i<3;++i)if(cf_edge(t.v[i],t.v[(i+1)%3],c[0],c[1])*sign<=0)return false;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// ROV oracle

// Rectangles (x0, y0, width, height); each is drawn as two triangles. Width and height have different 2-adic
// valuations, so the shared diagonal never passes through a pixel centre and the split never depends on the
// top-left rule.
constexpr int cf_rov_rects[20][4]{
    {3,5,20,13},{10,2,33,8},{0,20,64,7},{40,30,9,30},{12,12,40,21},{25,0,6,64},{5,40,50,3},
    {30,15,17,34},{48,4,14,11},{1,50,28,13},{20,28,24,5},{33,44,30,19},{7,7,56,9},{44,0,3,40},
    {16,36,12,25},{0,0,32,31},{32,32,32,30},{50,50,13,14},{22,10,10,48},{2,58,60,5}};
inline unsigned cf_valuation(int value){unsigned v=0;while(value && !(value&1)){value>>=1;++v;}return v;}
struct CfRovOracle {
    std::vector<CfTri> tris;std::vector<UINT32> init,ordered,reversed,last;
    unsigned layers_min{~0u},layers_max{},ambiguous{},discriminating{},layers{};bool rects_ok{true};
};
inline CfRovOracle cf_rov_oracle(UINT32 seed){
    CfRovOracle o;
    // Layer order: full-screen triangle, rects 0-6, full, rects 7-13, full, rects 14-19 (23 layers, 43 triangles).
    const auto tag=[](unsigned layer){return (layer+1u)*0x9E3779B1u;};
    unsigned layer=0,rect=0;
    for(int block=0;block<3;++block){
        o.tris.push_back({{{0,0},{128,0},{0,128}},tag(layer++)});
        for(int k=0;k<(block==2?6:7);++k,++rect){
            const int* r=cf_rov_rects[rect];const double x0=r[0],y0=r[1],x1=r[0]+r[2],y1=r[1]+r[3];
            if(cf_valuation(r[2])==cf_valuation(r[3]) || x1>cf_side || y1>cf_side)o.rects_ok=false;
            const UINT32 t=tag(layer++);
            o.tris.push_back({{{x0,y0},{x1,y0},{x1,y1}},t});
            o.tris.push_back({{{x0,y0},{x1,y1},{x0,y1}},t});
        }
    }
    o.layers=layer;
    o.init.resize(cf_pixels);o.ordered.resize(cf_pixels);o.reversed.resize(cf_pixels);o.last.assign(cf_pixels,0);
    std::vector<UINT32> covering;
    for(UINT y=0;y<cf_side;++y)for(UINT x=0;x<cf_side;++x){
        const UINT i=y*cf_side+x;covering.clear();
        for(const auto& t:o.tris){const int c=cf_center_exact(t,x+0.5,y+0.5);if(c<0)++o.ambiguous;if(c>0)covering.push_back(t.tag);}
        o.init[i]=seed^(i*0x85EBCA6Bu);
        UINT32 forward=o.init[i],backward=o.init[i];
        for(size_t k=0;k<covering.size();++k){
            forward=forward*0x01000193u+(covering[k]^i);
            backward=backward*0x01000193u+(covering[covering.size()-1-k]^i);
        }
        o.ordered[i]=forward;o.reversed[i]=backward;o.last[i]=covering.empty()?0:covering.back();
        o.layers_min=(std::min)(o.layers_min,static_cast<unsigned>(covering.size()));
        o.layers_max=(std::max)(o.layers_max,static_cast<unsigned>(covering.size()));
        o.discriminating+=forward!=backward;
    }
    return o;
}

// ---------------------------------------------------------------------------------------------------------------
// Conservative rasterization oracle

// Eight triangles in separate 16x16 cells, vertices on a 1/16 grid (sixteenths below). Tags 1..8.
constexpr int cf_cr_tris[8][6]{
    {36,114, 220,113, 220,118},     // horizontal sliver inside row 7 (no pixel centre), cell (0,0)
    {337,81, 342,81, 337,86},       // sub-pixel triangle inside pixel (21,5), cell (1,0)
    {636,125, 644,125, 640,132},    // small triangle around the pixel corner (40,8), cell (2,0)
    {788,28, 996,238, 1000,238},    // diagonal sliver along x - y = 47.5, cell (3,0)
    {21,275, 235,275, 21,489},      // right triangle, hypotenuse x + y = 31.875, cell (0,1)
    {386,276, 385,492, 390,492},    // vertical sliver inside column 24, cell (1,1)
    {608,471, 712,402, 548,285},    // general triangle, no axis-aligned edge, cell (2,1)
    {910,340, 914,340, 912,346}};   // sub-pixel triangle across the column boundary x = 57, cell (3,1)
struct CfCrPixel {UINT32 tag{};signed char std{},outer{-1},inner{-1};};
struct CfCrOracle {
    std::vector<CfCrPixel> pixels;std::vector<CfTri> tris;double uncertainty{};
    unsigned std_cover{},outer_must{},outer_may{},inner_must{},std_ambiguous{},inner_ambiguous{},overlap{},differ{},reoriented{};
};
// outer: 1 must be covered, -1 must not, 0 may (only inside the uncertainty band). inner: 1 bit 0 must be set, -1
// must be clear, 0 either. std: 1 centre inside, 0 outside.
inline CfCrOracle cf_cr_oracle(UINT tier){
    CfCrOracle o;o.uncertainty=tier>=2?1.0/256.0:0.5;o.pixels.resize(cf_pixels);
    for(UINT32 k=0;k<8;++k){
        const int* v=cf_cr_tris[k];CfTri t{};
        for(int i=0;i<3;++i){t.v[i][0]=v[2*i]/16.0;t.v[i][1]=v[2*i+1]/16.0;}
        // Front-facing (clockwise in render target space, FrontCounterClockwise FALSE) for the decisive passes; the
        // back-facing observation pass draws the same triangles with two vertices swapped.
        if(cf_orientation(t)<0){for(int i=0;i<2;++i)std::swap(t.v[1][i],t.v[2][i]);++o.reoriented;}
        t.tag=k+1;o.tris.push_back(t);
    }
    const double m=cf_margin,u=o.uncertainty,e=1.0/256.0+m;
    for(UINT y=0;y<cf_side;++y)for(UINT x=0;x<cf_side;++x){
        CfCrPixel& p=o.pixels[y*cf_side+x];bool claimed=false;
        for(const auto& t:o.tris){
            const bool must=cf_intersects(t,x+m,y+m,x+1-m,y+1-m);
            const bool may=cf_intersects(t,x-u-m,y-u-m,x+1+u+m,y+1+u+m);
            if(!must && !may)continue;
            if(claimed){++o.overlap;continue;}
            claimed=true;p.tag=t.tag;p.outer=must?1:0;
            const int c=cf_center_margin(t,x+0.5,y+0.5,m);
            if(c<0)++o.std_ambiguous;
            p.std=c>0?1:0;
            if(must){
                const bool set=cf_box_inside(t,x-e,y-e,x+1+e,y+1+e),clear=!cf_box_inside(t,x+m,y+m,x+1-m,y+1-m);
                p.inner=set?1:clear?-1:0;
                if(!set && !clear)++o.inner_ambiguous;
            }
        }
        o.std_cover+=p.std==1;o.outer_must+=p.outer==1;o.outer_may+=p.outer==0;o.inner_must+=p.outer==1 && p.inner==1;
        o.differ+=p.outer!=0 && (p.std==1)!=(p.outer==1);
    }
    return o;
}
// Expected words of the three passes; may = the pixel accepts 0 too (tier 1 uncertainty band only).
inline UINT32 cf_cr_std_word(const CfCrPixel& p){return p.std==1?p.tag:0u;}
inline UINT32 cf_cr_cons_word(const CfCrPixel& p){return p.outer!=-1?p.tag:0u;}
inline UINT32 cf_cr_inner_word(const CfCrPixel& p){return p.outer!=-1?(p.tag<<1)|(p.inner==1?1u:0u):0u;}

// ---------------------------------------------------------------------------------------------------------------
// Indirect DispatchRays oracle

constexpr UINT cf_grid=8,cf_section_words=128,cf_sections=5;
constexpr UINT32 cf_prefill=0xFFFFFFFFu;
constexpr float cf_ray_triangle[3][3]{{-0.8f,-0.7f,0.5f},{0.1f,-0.7f,0.5f},{-0.7f,0.9f,0.5f}};
static_assert(sizeof(D3D12_DISPATCH_RAYS_DESC)==104,"dxr_args.hlsl writes 104-byte records");
static_assert(offsetof(D3D12_DISPATCH_RAYS_DESC,MissShaderTable)==16 && offsetof(D3D12_DISPATCH_RAYS_DESC,HitGroupTable)==40 &&
              offsetof(D3D12_DISPATCH_RAYS_DESC,CallableShaderTable)==64 && offsetof(D3D12_DISPATCH_RAYS_DESC,Width)==88 &&
              offsetof(D3D12_DISPATCH_RAYS_DESC,Height)==92 && offsetof(D3D12_DISPATCH_RAYS_DESC,Depth)==96,
              "layout used by dxr_args.hlsl");
inline const char* cf_section_name(UINT section){
    static const char* const names[cf_sections]{"direct","indirect-max1","count1-of-max2","count2-of-max2","max2-no-count"};
    return names[section];
}
struct CfDxrOracle {
    UINT32 words[cf_sections][cf_section_words]{};UINT32 hitmiss[cf_grid*cf_grid]{};unsigned hits{},misses{},count_discriminating{};bool margin{true};
};
inline CfDxrOracle cf_dxr_oracle(){
    CfDxrOracle o;
    for(UINT y=0;y<cf_grid;++y)for(UINT x=0;x<cf_grid;++x){
        const double px=(x+0.5)/4.0-1.0,py=(y+0.5)/4.0-1.0;int positive=0;
        for(int e=0;e<3;++e){
            const float* a=cf_ray_triangle[e];const float* b=cf_ray_triangle[(e+1)%3];
            const double dx=double{b[0]}-a[0],dy=double{b[1]}-a[1],side=dx*(py-a[1])-dy*(px-a[0]);
            if(side*side<0.05*0.05*(dx*dx+dy*dy))o.margin=false;
            positive+=side>0?1:0;
        }
        const bool hit=positive==0 || positive==3;o.hitmiss[y*cf_grid+x]=hit?1u:2u;o.hits+=hit;o.misses+=!hit;
    }
    const auto word=[&o](UINT x,UINT y,UINT32 tag,UINT32 height){return o.hitmiss[y*cf_grid+x]|(tag<<8)|(UINT32{cf_grid}<<16)|(height<<24);};
    for(UINT s=0;s<cf_sections;++s){
        for(UINT i=0;i<cf_section_words;++i)o.words[s][i]=cf_prefill;
        for(UINT y=0;y<cf_grid;++y)for(UINT x=0;x<cf_grid;++x)o.words[s][y*cf_grid+x]=word(x,y,0x0A,8);
        if(s==3 || s==4)for(UINT y=0;y<4;++y)for(UINT x=0;x<cf_grid;++x)o.words[s][64+y*cf_grid+x]=word(x,y,0x0B,4);
    }
    for(UINT i=0;i<cf_section_words;++i)o.count_discriminating+=o.words[2][i]!=o.words[3][i];
    return o;
}
// The two records dxr_args.hlsl writes, byte for byte (the padding word is zero).
inline void cf_dxr_records(D3D12_GPU_VIRTUAL_ADDRESS table,unsigned char (&bytes)[2*sizeof(D3D12_DISPATCH_RAYS_DESC)]){
    std::memset(bytes,0,sizeof(bytes));
    for(int r=0;r<2;++r){
        D3D12_DISPATCH_RAYS_DESC d;std::memset(&d,0,sizeof(d));
        d.RayGenerationShaderRecord={table+(r?64u:0u),D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        d.MissShaderTable={table+128,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        d.HitGroupTable={table+192,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES,D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES};
        d.Width=cf_grid;d.Height=r?4u:cf_grid;d.Depth=1;
        std::memcpy(bytes+r*sizeof(d),&d,sizeof(d));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Comparisons (pure functions of oracle and read-back words, so --selftest exercises them without a GPU)

// got: four 64x64 images, ROV texture, plain UAV texture, render target of the ROV pass, render target of the plain pass.
struct CfRovCompare {CfDiff rov,targets,plain,reversed;};
inline CfRovCompare cf_rov_compare(const CfRovOracle& o,const UINT32* got){
    CfRovCompare c;
    for(UINT y=0;y<cf_side;++y)for(UINT x=0;x<cf_side;++x){
        const UINT i=y*cf_side+x;
        c.rov.check(got[i]==o.ordered[i],x,y,got[i],o.ordered[i],"rov");
        c.targets.check(got[2*cf_pixels+i]==o.last[i],x,y,got[2*cf_pixels+i],o.last[i],"rt-rov-pass");
        c.targets.check(got[3*cf_pixels+i]==o.last[i],x,y,got[3*cf_pixels+i],o.last[i],"rt-plain-pass");
        c.plain.check(got[cf_pixels+i]==o.ordered[i],x,y,got[cf_pixels+i],o.ordered[i],"plain");
        c.reversed.check(got[i]==o.reversed[i],x,y,got[i],o.reversed[i],"reversed");
    }
    return c;
}
// got: four 64x64 images, pass std, pass cons, pass inner and the back-facing inner observation (the last two read
// only when inner).
struct CfCrCompare {CfDiff std_diff,cons_diff,inner_diff,backface,std_vs_cons,cons_vs_std;unsigned may_covered{},inner_set{};};
inline bool cf_cr_inner_ok(const CfCrPixel& p,UINT32 g){
    if(p.outer==-1)return g==0;
    if(p.outer==0)return g==0 || g==(p.tag<<1) || g==((p.tag<<1)|1u);
    return p.inner==0?(g>>1)==p.tag:g==cf_cr_inner_word(p);
}
inline CfCrCompare cf_cr_compare(const CfCrOracle& o,const UINT32* got,bool inner){
    CfCrCompare c;
    for(UINT y=0;y<cf_side;++y)for(UINT x=0;x<cf_side;++x){
        const UINT i=y*cf_side+x;const CfCrPixel& p=o.pixels[i];
        const UINT32 g0=got[i],g1=got[cf_pixels+i];
        c.std_diff.check(g0==cf_cr_std_word(p),x,y,g0,cf_cr_std_word(p),"std");
        const bool cons_ok=p.outer==0?(g1==p.tag || g1==0):g1==cf_cr_cons_word(p);
        if(p.outer==0 && g1==p.tag)++c.may_covered;
        c.cons_diff.check(cons_ok,x,y,g1,cf_cr_cons_word(p),"cons");
        // Controls: each image against the other pass's oracle (only where that oracle is exact).
        if(p.outer!=0){c.std_vs_cons.check(g0==cf_cr_cons_word(p),x,y,g0,cf_cr_cons_word(p),"std-image-vs-cons-oracle");
                       c.cons_vs_std.check(g1==cf_cr_std_word(p),x,y,g1,cf_cr_std_word(p),"cons-image-vs-std-oracle");}
        if(inner){
            const UINT32 g2=got[2*cf_pixels+i],g3=got[3*cf_pixels+i];
            c.inner_set+=(g2&1u)!=0;
            c.inner_diff.check(cf_cr_inner_ok(p,g2),x,y,g2,cf_cr_inner_word(p),"inner");
            c.backface.check(cf_cr_inner_ok(p,g3),x,y,g3,cf_cr_inner_word(p),"inner-backface");
        }
    }
    return c;
}
// got: five sections of 128 words; args: 52 words of the two GPU-written records; counts: two words.
struct CfDxrCompare {CfDiff args,section[cf_sections],count_control;};
inline CfDxrCompare cf_dxr_compare(const CfDxrOracle& o,const UINT32* got,const UINT32* args,const UINT32* counts,const unsigned char* expected_args){
    CfDxrCompare c;
    for(UINT i=0;i<2*sizeof(D3D12_DISPATCH_RAYS_DESC)/4;++i){UINT32 w=0;std::memcpy(&w,expected_args+i*4,4);c.args.check(args[i]==w,i,0,args[i],w,"args");}
    for(UINT i=0;i<2;++i)c.args.check(counts[i]==i+1,i,1,counts[i],i+1,"counts");
    for(UINT sct=0;sct<cf_sections;++sct)for(UINT i=0;i<cf_section_words;++i){
        const UINT32 g=got[sct*cf_section_words+i];c.section[sct].check(g==o.words[sct][i],i,sct,g,o.words[sct][i],cf_section_name(sct));}
    // Control: section 2's result against section 3's oracle (one dispatch read as two).
    for(UINT i=0;i<cf_section_words;++i){const UINT32 g=got[2*cf_section_words+i];c.count_control.check(g==o.words[3][i],i,2,g,o.words[3][i],"control");}
    return c;
}

// The comparison itself can fail: one flipped word must be reported with its coordinates.
inline bool cf_comparator_selftest(const std::vector<UINT32>& want){
    std::vector<UINT32> got=want;got[42*cf_side+17]^=0x00010000u;CfDiff d;
    for(UINT y=0;y<cf_side;++y)for(UINT x=0;x<cf_side;++x)d.check(got[y*cf_side+x]==want[y*cf_side+x],x,y,got[y*cf_side+x],want[y*cf_side+x],"selftest");
    return d.count==1 && d.x==17 && d.y==42 && d.checked==cf_pixels;
}

// ---------------------------------------------------------------------------------------------------------------
// GPU helpers

inline HRESULT cf_buffer(Session& s,D3D12_HEAP_TYPE type,UINT64 bytes,D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags,
                         ComPtr<ID3D12Resource>& out,const char* what){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=type;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;desc.Height=1;desc.DepthOrArraySize=1;
    desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
    char label[96]{};sprintf_s(label,"CreateCommittedResource %s",what);
    return s.api(label,[&]{return s.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&out));});
}
// 64x64 R32_UINT texture in a DEFAULT heap; a render target gets the optimized clear value 0.
inline HRESULT cf_texture(Session& s,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,ComPtr<ID3D12Resource>& out,const char* what){
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;heap.CreationNodeMask=heap.VisibleNodeMask=1;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=cf_side;desc.Height=cf_side;
    desc.DepthOrArraySize=1;desc.MipLevels=1;desc.Format=DXGI_FORMAT_R32_UINT;desc.SampleDesc.Count=1;desc.Flags=flags;
    D3D12_CLEAR_VALUE clear{};clear.Format=DXGI_FORMAT_R32_UINT;
    const bool target=(flags&D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)!=0;
    char label[96]{};sprintf_s(label,"CreateCommittedResource %s",what);
    return s.api(label,[&]{return s.device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,target?&clear:nullptr,IID_PPV_ARGS(&out));});
}
inline HRESULT cf_root(Session& s,const D3D12_ROOT_SIGNATURE_DESC& desc,ComPtr<ID3D12RootSignature>& out,const char* what){
    auto proc=GetProcAddress(s.runtime,"D3D12SerializeRootSignature");
    decltype(&D3D12SerializeRootSignature) serialize=nullptr;
    static_assert(sizeof(serialize)==sizeof(proc));std::memcpy(&serialize,&proc,sizeof(serialize));
    if(!serialize)return E_NOINTERFACE;
    ComPtr<ID3DBlob> blob,errors;char label[96]{};
    sprintf_s(label,"D3D12SerializeRootSignature %s",what);
    HRESULT hr=s.api(label,[&]{return serialize(&desc,D3D_ROOT_SIGNATURE_VERSION_1_0,&blob,&errors);});if(FAILED(hr))return hr;
    sprintf_s(label,"CreateRootSignature %s",what);
    return s.api(label,[&]{return s.device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&out));});
}
inline HRESULT cf_write(Session& s,ID3D12Resource* resource,UINT64 offset,const void* data,size_t bytes,const char* what){
    void* mapped=nullptr;const D3D12_RANGE none{0,0};char label[96]{};sprintf_s(label,"Map %s",what);
    HRESULT hr=s.api(label,[&]{return resource->Map(0,&none,&mapped);});if(FAILED(hr))return hr;if(!mapped)return E_POINTER;
    std::memcpy(static_cast<unsigned char*>(mapped)+offset,data,bytes);
    const D3D12_RANGE written{static_cast<SIZE_T>(offset),static_cast<SIZE_T>(offset+bytes)};resource->Unmap(0,&written);
    return S_OK;
}
inline HRESULT cf_read(Session& s,ID3D12Resource* resource,UINT64 offset,void* data,size_t bytes,const char* what){
    void* mapped=nullptr;const D3D12_RANGE range{static_cast<SIZE_T>(offset),static_cast<SIZE_T>(offset+bytes)};char label[96]{};sprintf_s(label,"Map %s",what);
    HRESULT hr=s.api(label,[&]{return resource->Map(0,&range,&mapped);});if(FAILED(hr))return hr;if(!mapped)return E_POINTER;
    std::memcpy(data,static_cast<const unsigned char*>(mapped)+offset,bytes);
    const D3D12_RANGE none{0,0};resource->Unmap(0,&none);
    return S_OK;
}
// The READBACK starts at the complement of every expectation and is read again before the submission, so an untouched
// word can never compare equal (as in the d3d12queue variants).
inline HRESULT cf_prefill_readback(Session& s,ID3D12Resource* readback,const std::vector<UINT32>& words){
    HRESULT hr=cf_write(s,readback,0,words.data(),words.size()*4,"READBACK prefill");if(FAILED(hr))return hr;
    std::vector<UINT32> check(words.size());
    hr=cf_read(s,readback,0,check.data(),check.size()*4,"READBACK before submit");if(FAILED(hr))return hr;
    hr=check==words?S_OK:E_FAIL;s.event("after","Destination holds complement before submit",hr);return hr;
}
inline D3D12_RESOURCE_BARRIER cf_transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=resource;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=before;b.Transition.StateAfter=after;return b;
}
inline D3D12_PLACED_SUBRESOURCE_FOOTPRINT cf_footprint(UINT64 offset){
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};f.Offset=offset;f.Footprint={DXGI_FORMAT_R32_UINT,cf_side,cf_side,1,cf_side*4};return f;
}
inline void cf_copy_image(ID3D12GraphicsCommandList* list,ID3D12Resource* readback,UINT64 offset,ID3D12Resource* texture){
    D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=readback;destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint=cf_footprint(offset);
    D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=texture;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
}
inline HRESULT cf_list(Session& s,CfKeep& keep,ComPtr<ID3D12CommandAllocator>& allocator,ComPtr<ID3D12GraphicsCommandList>& list,ComPtr<ID3D12Fence>& fence){
    HRESULT hr=s.api("CreateCommandAllocator",[&]{return s.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));});if(FAILED(hr))return hr;
    keep.hold(allocator);
    hr=s.api("CreateCommandList",[&]{return s.device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list));});if(FAILED(hr))return hr;
    keep.hold(list);
    hr=s.api("CreateFence",[&]{return s.device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));});if(FAILED(hr))return hr;
    keep.hold(fence);return S_OK;
}
// Close, execute, signal 1 and wait at most 10 s (and never past the session deadline). Retirement is proven only by
// the fence; until then every held object stays referenced.
inline HRESULT cf_execute(Session& s,CfKeep& keep,ID3D12GraphicsCommandList* list,ID3D12Fence* fence){
    HRESULT hr=s.api("Close CommandList",[&]{return list->Close();});if(FAILED(hr))return hr;
    ID3D12CommandList* lists[]{list};s.pending=true;keep.leak=true;
    s.event("before","ExecuteCommandLists");s.queue->ExecuteCommandLists(1,lists);s.event("after","ExecuteCommandLists");
    hr=s.api("Queue Signal 1",[&]{return s.queue->Signal(fence,1);});if(FAILED(hr))return hr;
    HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!done)return HRESULT_FROM_WIN32(GetLastError());
    hr=s.api("SetEventOnCompletion 1",[&]{return fence->SetEventOnCompletion(1,done);});
    if(SUCCEEDED(hr)){
        const ULONGLONG end=(std::min)(s.deadline,GetTickCount64()+10000);DWORD wait=WAIT_TIMEOUT;
        s.event("before","WaitForFence bounded");
        while(GetTickCount64()<end && !s.abort_requested() && wait==WAIT_TIMEOUT)wait=WaitForSingleObject(done,20);
        s.event("after","WaitForFence bounded",wait==WAIT_OBJECT_0?S_OK:HRESULT_FROM_WIN32(WAIT_TIMEOUT));
        const UINT64 completed=fence->GetCompletedValue();
        s.event("after","GetCompletedValue",completed>=1 && completed!=UINT64_MAX?S_OK:E_FAIL);
        if(completed==UINT64_MAX)hr=DXGI_ERROR_DEVICE_REMOVED;
        else if(completed<1)hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        else{s.pending=false;keep.leak=false;}
    }
    CloseHandle(done);return hr;
}
inline D3D12_GRAPHICS_PIPELINE_STATE_DESC cf_raster_pso(ID3D12RootSignature* root,const D3D12_INPUT_ELEMENT_DESC (&elements)[2],
                                                       const void* ps,size_t ps_bytes,bool conservative){
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};d.pRootSignature=root;
    d.VS={g_raster_vs,sizeof(g_raster_vs)};d.PS={ps,ps_bytes};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;d.SampleMask=0xffffffffu;
    d.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;d.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;d.RasterizerState.DepthClipEnable=TRUE;
    d.RasterizerState.ConservativeRaster=conservative?D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON:D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
    d.InputLayout={elements,2};d.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets=1;d.RTVFormats[0]=DXGI_FORMAT_R32_UINT;d.SampleDesc.Count=1;
    return d;
}
struct CfVertex {float x,y;UINT32 tag;};
static_assert(sizeof(CfVertex)==12);
constexpr D3D12_INPUT_ELEMENT_DESC cf_elements[2]{
    {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
    {"TAG",0,DXGI_FORMAT_R32_UINT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
inline std::vector<CfVertex> cf_vertices(const std::vector<CfTri>& tris){
    std::vector<CfVertex> v;
    for(const auto& t:tris)for(int i=0;i<3;++i)v.push_back({static_cast<float>(t.v[i][0]),static_cast<float>(t.v[i][1]),t.tag});
    return v;
}
// Common state of the two raster subtests: viewport and scissor 64x64, triangle list from one UPLOAD vertex buffer.
inline void cf_raster_state(ID3D12GraphicsCommandList* list,ID3D12Resource* vertices,UINT count){
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VERTEX_BUFFER_VIEW view{vertices->GetGPUVirtualAddress(),static_cast<UINT>(count*sizeof(CfVertex)),sizeof(CfVertex)};
    list->IASetVertexBuffers(0,1,&view);
    const D3D12_VIEWPORT viewport{0.0f,0.0f,static_cast<FLOAT>(cf_side),static_cast<FLOAT>(cf_side),0.0f,1.0f};list->RSSetViewports(1,&viewport);
    const D3D12_RECT scissor{0,0,static_cast<LONG>(cf_side),static_cast<LONG>(cf_side)};list->RSSetScissorRects(1,&scissor);
}
inline void cf_fail(CfOutcome& out,HRESULT hr,const char* where){out.status="ERROR";out.hr=hr;out.reason=where;}
// Raw read-back words of a subtest next to conformance.json (little-endian UINT32, layout in README.md).
inline void cf_dump(Session& s,const wchar_t* name,const std::vector<UINT32>& words){
    const std::string bytes(reinterpret_cast<const char*>(words.data()),words.size()*4);
    char label[96]{};sprintf_s(label,"Dump %ls %u bytes",name,static_cast<unsigned>(bytes.size()));
    s.event("after",label,publish(s.root/name,bytes)?S_OK:E_FAIL);
}

// ---------------------------------------------------------------------------------------------------------------
// Subtest 1: rasterizer ordered views

inline CfOutcome cf_rov(Session& s,const CfFeatures& f,UINT32 seed){
    CfOutcome out;out.name="rov";
    if(!f.rovs){out.status="SKIP";out.hr=DXGI_ERROR_UNSUPPORTED;out.reason="ROVsSupported=0";return out;}
    const CfRovOracle o=cf_rov_oracle(seed);
    const bool design=o.rects_ok && !o.ambiguous && o.layers_min>=3 && o.discriminating==cf_pixels && cf_comparator_selftest(o.ordered);
    {char label[160]{};sprintf_s(label,"ROV oracle %u triangles %u layers, coverage %u to %u, discriminating %u, ambiguous %u",
        static_cast<unsigned>(o.tris.size()),o.layers,o.layers_min,o.layers_max,o.discriminating,o.ambiguous);s.event("after",label,design?S_OK:E_UNEXPECTED);}
    if(!design){cf_fail(out,E_UNEXPECTED,"oracle-design");return out;}
    CfKeep keep;HRESULT hr=S_OK;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,0};
    D3D12_ROOT_PARAMETER parameter{};parameter.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable={1,&range};parameter.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC signature{1,&parameter,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3D12RootSignature> root;
    if(FAILED(hr=cf_root(s,signature,root,"rov"))){cf_fail(out,hr,"root-signature");return out;}
    keep.hold(root);
    ComPtr<ID3D12PipelineState> ordered_pso,plain_pso;
    {auto d=cf_raster_pso(root.Get(),cf_elements,g_rov_ps_ordered,sizeof(g_rov_ps_ordered),false);
     hr=s.api("CreateGraphicsPipelineState ROV",[&]{return s.device->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&ordered_pso));});}
    if(FAILED(hr)){cf_fail(out,hr,"pso-rov");return out;}
    keep.hold(ordered_pso);
    {auto d=cf_raster_pso(root.Get(),cf_elements,g_rov_ps_plain,sizeof(g_rov_ps_plain),false);
     hr=s.api("CreateGraphicsPipelineState plain UAV",[&]{return s.device->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&plain_pso));});}
    if(FAILED(hr)){cf_fail(out,hr,"pso-plain");return out;}
    keep.hold(plain_pso);

    const std::vector<CfVertex> vertices=cf_vertices(o.tris);const UINT64 vertex_bytes=vertices.size()*sizeof(CfVertex);
    ComPtr<ID3D12Resource> vb,init,uav[2],rt[2],readback;
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_UPLOAD,vertex_bytes,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,vb,"vertices"))){cf_fail(out,hr,"vertices");return out;}
    keep.hold(vb);
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_UPLOAD,cf_image_bytes,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,init,"initial values"))){cf_fail(out,hr,"init");return out;}
    keep.hold(init);
    const char* const uav_names[2]{"ROV texture","plain UAV texture"};const char* const rt_names[2]{"render target ROV pass","render target plain pass"};
    for(int i=0;i<2;++i){
        if(FAILED(hr=cf_texture(s,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_DEST,uav[i],uav_names[i]))){cf_fail(out,hr,"uav-texture");return out;}
        keep.hold(uav[i]);
        if(FAILED(hr=cf_texture(s,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,rt[i],rt_names[i]))){cf_fail(out,hr,"render-target");return out;}
        keep.hold(rt[i]);
    }
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_READBACK,4*cf_image_bytes,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,readback,"READBACK"))){cf_fail(out,hr,"readback");return out;}
    keep.hold(readback);
    if(FAILED(hr=cf_write(s,vb.Get(),0,vertices.data(),static_cast<size_t>(vertex_bytes),"vertices"))){cf_fail(out,hr,"map");return out;}
    if(FAILED(hr=cf_write(s,init.Get(),0,o.init.data(),static_cast<size_t>(cf_image_bytes),"initial values"))){cf_fail(out,hr,"map");return out;}
    // The READBACK starts at the complement of every expectation: an untouched word can never compare equal.
    std::vector<UINT32> prefill(4*cf_pixels);
    for(UINT i=0;i<cf_pixels;++i){prefill[i]=prefill[cf_pixels+i]=~o.ordered[i];prefill[2*cf_pixels+i]=prefill[3*cf_pixels+i]=~o.last[i];}
    if(FAILED(hr=cf_prefill_readback(s,readback.Get(),prefill))){cf_fail(out,hr,"prefill");return out;}

    ComPtr<ID3D12DescriptorHeap> views,targets;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(FAILED(hr=s.api("CreateDescriptorHeap CBV_SRV_UAV",[&]{return s.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&views));}))){cf_fail(out,hr,"descriptor-heap");return out;}
    keep.hold(views);
    heap={D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(hr=s.api("CreateDescriptorHeap RTV",[&]{return s.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&targets));}))){cf_fail(out,hr,"descriptor-heap");return out;}
    keep.hold(targets);
    const UINT view_step=s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_step=s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[2]{targets->GetCPUDescriptorHandleForHeapStart(),targets->GetCPUDescriptorHandleForHeapStart()};rtv[1].ptr+=rtv_step;
    D3D12_GPU_DESCRIPTOR_HANDLE table[2]{views->GetGPUDescriptorHandleForHeapStart(),views->GetGPUDescriptorHandleForHeapStart()};table[1].ptr+=view_step;
    for(int i=0;i<2;++i){
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};u.Format=DXGI_FORMAT_R32_UINT;u.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu=views->GetCPUDescriptorHandleForHeapStart();cpu.ptr+=i*view_step;
        s.device->CreateUnorderedAccessView(uav[i].Get(),nullptr,&u,cpu);
        s.device->CreateRenderTargetView(rt[i].Get(),nullptr,rtv[i]);
    }
    s.event("after","Views created: 2 texture UAVs, 2 RTVs");

    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    if(FAILED(hr=cf_list(s,keep,allocator,list,fence))){cf_fail(out,hr,"command-list");return out;}
    ID3D12GraphicsCommandList* l=list.Get();
    for(int i=0;i<2;++i){
        D3D12_TEXTURE_COPY_LOCATION destination{};destination.pResource=uav[i].Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};source.pResource=init.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=cf_footprint(0);
        l->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    }
    {const D3D12_RESOURCE_BARRIER b[2]{cf_transition(uav[0].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        cf_transition(uav[1].Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};l->ResourceBarrier(2,b);}
    const FLOAT zero[4]{};
    for(int i=0;i<2;++i)l->ClearRenderTargetView(rtv[i],zero,0,nullptr);
    l->SetGraphicsRootSignature(root.Get());
    ID3D12DescriptorHeap* heaps[]{views.Get()};l->SetDescriptorHeaps(1,heaps);
    cf_raster_state(l,vb.Get(),static_cast<UINT>(vertices.size()));
    // One draw per program: all 43 triangles in a single DrawInstanced, so the hardware may overlap them.
    l->SetPipelineState(ordered_pso.Get());l->SetGraphicsRootDescriptorTable(0,table[0]);l->OMSetRenderTargets(1,&rtv[0],FALSE,nullptr);
    l->DrawInstanced(static_cast<UINT>(vertices.size()),1,0,0);
    l->SetPipelineState(plain_pso.Get());l->SetGraphicsRootDescriptorTable(0,table[1]);l->OMSetRenderTargets(1,&rtv[1],FALSE,nullptr);
    l->DrawInstanced(static_cast<UINT>(vertices.size()),1,0,0);
    {const D3D12_RESOURCE_BARRIER b[4]{
        cf_transition(uav[0].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
        cf_transition(uav[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
        cf_transition(rt[0].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE),
        cf_transition(rt[1].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE)};l->ResourceBarrier(4,b);}
    cf_copy_image(l,readback.Get(),0,uav[0].Get());cf_copy_image(l,readback.Get(),cf_image_bytes,uav[1].Get());
    cf_copy_image(l,readback.Get(),2*cf_image_bytes,rt[0].Get());cf_copy_image(l,readback.Get(),3*cf_image_bytes,rt[1].Get());
    s.event("after","ROV list recorded: copy, clear, 2 draws of 43 triangles, 4 readback copies");
    if(FAILED(hr=cf_execute(s,keep,l,fence.Get()))){cf_fail(out,hr,"execute");return out;}
    std::vector<UINT32> got(4*cf_pixels);
    if(FAILED(hr=cf_read(s,readback.Get(),0,got.data(),got.size()*4,"READBACK"))){cf_fail(out,hr,"readback-map");return out;}
    cf_dump(s,L"rov.bin",got);
    const CfRovCompare c=cf_rov_compare(o,got.data());
    const CfDiff &rov=c.rov,&targets_diff=c.targets,&plain=c.plain,&reversed=c.reversed;
    out.mismatches=rov.count+targets_diff.count;out.checked=rov.checked+targets_diff.checked;
    out.first=rov.count?rov.first():targets_diff.first();
    out.status=out.mismatches?"FAIL":"PASS";out.hr=out.mismatches?E_FAIL:S_OK;
    char t[512]{};
    sprintf_s(t,"rov=%u/%u rt=%u/%u layers=%u..%u reversed_oracle_mismatch=%u/%u plain_uav_observed=%u/%u comparator_selftest=ok",
        rov.count,rov.checked,targets_diff.count,targets_diff.checked,o.layers_min,o.layers_max,reversed.count,reversed.checked,plain.count,plain.checked);
    out.detail=t;
    sprintf_s(t,"\"seed\":\"%08x\",\"triangles\":%u,\"layers\":%u,\"coverage_min\":%u,\"coverage_max\":%u,\"rov_mismatches\":%u,\"rt_mismatches\":%u,"
        "\"control_reversed_order_mismatches\":%u,\"control_discriminating_pixels\":%u,\"observed_plain_uav_mismatches\":%u,\"observed_plain_uav_first\":%s,"
        "\"control_comparator_selftest\":true",seed,static_cast<unsigned>(o.tris.size()),o.layers,o.layers_min,o.layers_max,rov.count,targets_diff.count,
        reversed.count,o.discriminating,plain.count,cf_json_text(plain.first()).c_str());
    out.json=t;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Subtest 2: conservative rasterization

inline CfOutcome cf_conservative(Session& s,const CfFeatures& f){
    CfOutcome out;out.name="conservative";
    if(!f.cr_tier){out.status="SKIP";out.hr=DXGI_ERROR_UNSUPPORTED;out.reason="ConservativeRasterizationTier=0";return out;}
    const UINT tier=(std::min)(f.cr_tier,3u);const bool inner=tier>=3;
    const CfCrOracle o=cf_cr_oracle(tier);
    std::vector<UINT32> cons_words(cf_pixels);for(UINT i=0;i<cf_pixels;++i)cons_words[i]=cf_cr_cons_word(o.pixels[i]);
    bool front=true;for(const auto& t:o.tris)front=front && cf_orientation(t)>0;
    const bool design=front && !o.overlap && !o.std_ambiguous && (tier<2 || !o.outer_may) && (!inner || !o.inner_ambiguous) && o.differ>0 &&
                      cf_comparator_selftest(cons_words);
    {char label[200]{};sprintf_s(label,"CR oracle tier %u: std %u, must %u, may %u, inner %u, differ %u, ambiguous std %u inner %u, overlap %u",
        tier,o.std_cover,o.outer_must,o.outer_may,o.inner_must,o.differ,o.std_ambiguous,o.inner_ambiguous,o.overlap);s.event("after",label,design?S_OK:E_UNEXPECTED);}
    if(!design){cf_fail(out,E_UNEXPECTED,"oracle-design");return out;}
    CfKeep keep;HRESULT hr=S_OK;
    D3D12_ROOT_SIGNATURE_DESC signature{0,nullptr,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    ComPtr<ID3D12RootSignature> root;
    if(FAILED(hr=cf_root(s,signature,root,"conservative"))){cf_fail(out,hr,"root-signature");return out;}
    keep.hold(root);
    // Passes: 0 std, 1 conservative, 2 inner coverage, 3 inner coverage of the back-facing copies (observation).
    const UINT passes=inner?4u:2u;
    ComPtr<ID3D12PipelineState> pso[3];
    const char* const pso_names[3]{"CreateGraphicsPipelineState conservative OFF","CreateGraphicsPipelineState conservative ON",
                                   "CreateGraphicsPipelineState conservative ON SV_InnerCoverage"};
    for(UINT p=0;p<(std::min)(passes,3u);++p){
        auto d=p==2?cf_raster_pso(root.Get(),cf_elements,g_cr_ps_inner,sizeof(g_cr_ps_inner),true)
                   :cf_raster_pso(root.Get(),cf_elements,g_cr_ps_tag,sizeof(g_cr_ps_tag),p==1);
        if(FAILED(hr=s.api(pso_names[p],[&]{return s.device->CreateGraphicsPipelineState(&d,IID_PPV_ARGS(&pso[p]));}))){cf_fail(out,hr,"pso");return out;}
        keep.hold(pso[p]);
    }
    std::vector<CfTri> back=o.tris;for(auto& t:back)for(int i=0;i<2;++i)std::swap(t.v[1][i],t.v[2][i]);
    std::vector<CfVertex> vertices=cf_vertices(o.tris);const UINT front_vertices=static_cast<UINT>(vertices.size());
    for(const auto& v:cf_vertices(back))vertices.push_back(v);
    const UINT64 vertex_bytes=vertices.size()*sizeof(CfVertex);
    ComPtr<ID3D12Resource> vb,rt[4],readback;
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_UPLOAD,vertex_bytes,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,vb,"vertices"))){cf_fail(out,hr,"vertices");return out;}
    keep.hold(vb);
    const char* const rt_names[4]{"render target std","render target conservative","render target inner coverage","render target inner coverage back-facing"};
    for(UINT p=0;p<passes;++p){
        if(FAILED(hr=cf_texture(s,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET,rt[p],rt_names[p]))){cf_fail(out,hr,"render-target");return out;}
        keep.hold(rt[p]);
    }
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_READBACK,4*cf_image_bytes,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,readback,"READBACK"))){cf_fail(out,hr,"readback");return out;}
    keep.hold(readback);
    if(FAILED(hr=cf_write(s,vb.Get(),0,vertices.data(),static_cast<size_t>(vertex_bytes),"vertices"))){cf_fail(out,hr,"map");return out;}
    std::vector<UINT32> prefill(4*cf_pixels,0xFFFFFFFFu);
    for(UINT i=0;i<cf_pixels;++i)prefill[i]=~cf_cr_std_word(o.pixels[i]);
    if(FAILED(hr=cf_prefill_readback(s,readback.Get(),prefill))){cf_fail(out,hr,"prefill");return out;}
    ComPtr<ID3D12DescriptorHeap> targets;
    D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,4,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(FAILED(hr=s.api("CreateDescriptorHeap RTV",[&]{return s.device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&targets));}))){cf_fail(out,hr,"descriptor-heap");return out;}
    keep.hold(targets);
    const UINT rtv_step=s.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv[4]{};
    for(UINT p=0;p<passes;++p){rtv[p]=targets->GetCPUDescriptorHandleForHeapStart();rtv[p].ptr+=p*rtv_step;s.device->CreateRenderTargetView(rt[p].Get(),nullptr,rtv[p]);}
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    if(FAILED(hr=cf_list(s,keep,allocator,list,fence))){cf_fail(out,hr,"command-list");return out;}
    ID3D12GraphicsCommandList* l=list.Get();
    const FLOAT zero[4]{};
    l->SetGraphicsRootSignature(root.Get());
    cf_raster_state(l,vb.Get(),static_cast<UINT>(vertices.size()));
    for(UINT p=0;p<passes;++p){
        l->ClearRenderTargetView(rtv[p],zero,0,nullptr);
        l->SetPipelineState(pso[(std::min)(p,2u)].Get());l->OMSetRenderTargets(1,&rtv[p],FALSE,nullptr);
        l->DrawInstanced(front_vertices,1,p==3?front_vertices:0u,0);
        const D3D12_RESOURCE_BARRIER b=cf_transition(rt[p].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);l->ResourceBarrier(1,&b);
        cf_copy_image(l,readback.Get(),p*cf_image_bytes,rt[p].Get());
    }
    s.event("after",inner?"CR list recorded: passes std, conservative, inner coverage, inner coverage back-facing":"CR list recorded: passes std, conservative");
    if(FAILED(hr=cf_execute(s,keep,l,fence.Get()))){cf_fail(out,hr,"execute");return out;}
    std::vector<UINT32> got(4*cf_pixels);
    if(FAILED(hr=cf_read(s,readback.Get(),0,got.data(),got.size()*4,"READBACK"))){cf_fail(out,hr,"readback-map");return out;}
    cf_dump(s,L"conservative.bin",got);
    const CfCrCompare c=cf_cr_compare(o,got.data(),inner);
    const CfDiff &std_diff=c.std_diff,&cons_diff=c.cons_diff,&inner_diff=c.inner_diff,&std_vs_cons=c.std_vs_cons,&cons_vs_std=c.cons_vs_std;
    const unsigned may_covered=c.may_covered,inner_set=c.inner_set;
    out.mismatches=std_diff.count+cons_diff.count+inner_diff.count;out.checked=std_diff.checked+cons_diff.checked+inner_diff.checked;
    out.first=std_diff.count?std_diff.first():cons_diff.count?cons_diff.first():inner_diff.first();
    out.status=out.mismatches?"FAIL":"PASS";out.hr=out.mismatches?E_FAIL:S_OK;
    const std::string inner_text=inner?std::to_string(inner_diff.count)+"/"+std::to_string(inner_diff.checked):"n/a";
    const std::string backface_text=inner?std::to_string(c.backface.count)+"/"+std::to_string(c.backface.checked):"n/a";
    char t[768]{};
    sprintf_s(t,"tier=%u std=%u/%u cons=%u/%u inner=%s expected_std_pixels=%u expected_cons_pixels=%u expected_inner_pixels=%u "
        "uncertainty_band=%u uncertainty_band_covered=%u control_std_image_vs_cons_oracle=%u control_cons_image_vs_std_oracle=%u oracle_differ=%u "
        "backface_inner_observed=%s",
        tier,std_diff.count,std_diff.checked,cons_diff.count,cons_diff.checked,inner_text.c_str(),
        o.std_cover,o.outer_must,o.inner_must,o.outer_may,may_covered,std_vs_cons.count,cons_vs_std.count,o.differ,backface_text.c_str());
    out.detail=t;
    sprintf_s(t,"\"tier\":%u,\"uncertainty\":%.9g,\"margin\":%.9g,\"expected_std_pixels\":%u,\"expected_conservative_pixels\":%u,"
        "\"uncertainty_band_pixels\":%u,\"uncertainty_band_covered\":%u,\"expected_inner_pixels\":%s,\"std_mismatches\":%u,\"conservative_mismatches\":%u,"
        "\"inner_mismatches\":%s,\"inner_bits_set\":%u,\"control_oracle_differ_pixels\":%u,\"control_std_image_vs_conservative_oracle\":%u,"
        "\"control_conservative_image_vs_std_oracle\":%u,\"control_comparator_selftest\":true,\"triangles_reoriented_front\":%u,"
        "\"observed_backface_inner_mismatches\":%s,\"observed_backface_inner_first\":%s",
        tier,o.uncertainty,cf_margin,o.std_cover,o.outer_must,o.outer_may,may_covered,inner?std::to_string(o.inner_must).c_str():"null",
        std_diff.count,cons_diff.count,inner?std::to_string(inner_diff.count).c_str():"null",inner_set,o.differ,std_vs_cons.count,cons_vs_std.count,
        o.reoriented,inner?std::to_string(c.backface.count).c_str():"null",cf_json_text(c.backface.first()).c_str());
    out.json=t;
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Subtest 3: indirect DispatchRays

inline CfOutcome cf_dxr_indirect(Session& s,const CfFeatures& f){
    CfOutcome out;out.name="dxr-indirect";
    if(f.rt_tier<D3D12_RAYTRACING_TIER_1_1){out.status="SKIP";out.hr=DXGI_ERROR_UNSUPPORTED;out.reason="RaytracingTier="+cf_rt_text(f.rt_tier)+"_below_1.1";return out;}
    const CfDxrOracle o=cf_dxr_oracle();
    {std::vector<UINT32> image(cf_pixels);for(UINT i=0;i<cf_pixels;++i)image[i]=o.words[i%cf_sections][i%cf_section_words]^i;
     const bool design=o.margin && o.hits && o.misses && o.count_discriminating==32 && cf_comparator_selftest(image);
     char label[128]{};sprintf_s(label,"DXR oracle %u hits %u misses, margin %s, count-discriminating words %u",o.hits,o.misses,o.margin?"kept":"violated",o.count_discriminating);
     s.event("after",label,design?S_OK:E_UNEXPECTED);if(!design){cf_fail(out,E_UNEXPECTED,"oracle-design");return out;}}
    CfKeep keep;HRESULT hr=S_OK;
    ComPtr<ID3D12Device5> device5;
    if(FAILED(hr=s.api("QueryInterface ID3D12Device5",[&]{return s.device.As(&device5);}))){cf_fail(out,hr,"device5");return out;}
    keep.hold(device5);
    constexpr UINT64 output_bytes=UINT64{cf_sections}*cf_section_words*4,upload_bytes=4096,instance_offset=256,prefill_offset=1024;
    constexpr UINT64 args_bytes=2*sizeof(D3D12_DISPATCH_RAYS_DESC),args_readback=output_bytes,counts_readback=output_bytes+256,readback_bytes=4096;
    constexpr UINT64 table_bytes=256,record=D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    static_assert(prefill_offset+output_bytes<=upload_bytes && counts_readback+8<=readback_bytes && args_readback+args_bytes<=counts_readback);
    ComPtr<ID3D12Resource> upload,table,blas,tlas,scratch,output,args,counts,readback;
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_UPLOAD,upload_bytes,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,upload,"UPLOAD"))){cf_fail(out,hr,"upload");return out;}
    keep.hold(upload);
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_UPLOAD,table_bytes,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_FLAG_NONE,table,"shader table UPLOAD"))){cf_fail(out,hr,"table");return out;}
    keep.hold(table);
    const D3D12_GPU_VIRTUAL_ADDRESS upload_va=upload->GetGPUVirtualAddress(),table_va=table->GetGPUVirtualAddress();
    if(!upload_va || !table_va || table_va%D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT){cf_fail(out,E_FAIL,"table-address");return out;}

    D3D12_RAYTRACING_GEOMETRY_DESC geometry{};geometry.Type=D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometry.Flags=D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;geometry.Triangles.IndexFormat=DXGI_FORMAT_UNKNOWN;
    geometry.Triangles.VertexFormat=DXGI_FORMAT_R32G32B32_FLOAT;geometry.Triangles.VertexCount=3;geometry.Triangles.VertexBuffer={upload_va,sizeof(cf_ray_triangle[0])};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bottom{};bottom.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    bottom.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;bottom.NumDescs=1;bottom.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;bottom.pGeometryDescs=&geometry;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};top.Type=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    top.Flags=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;top.NumDescs=1;top.DescsLayout=D3D12_ELEMENTS_LAYOUT_ARRAY;top.InstanceDescs=upload_va+instance_offset;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bottom_info{},top_info{};
    s.event("before","GetRaytracingAccelerationStructurePrebuildInfo");
    device5->GetRaytracingAccelerationStructurePrebuildInfo(&bottom,&bottom_info);device5->GetRaytracingAccelerationStructurePrebuildInfo(&top,&top_info);
    s.event("after","GetRaytracingAccelerationStructurePrebuildInfo");
    const auto sane=[](UINT64 bytes){return bytes && bytes<=(64ull<<20);};
    if(!sane(bottom_info.ResultDataMaxSizeInBytes) || !sane(bottom_info.ScratchDataSizeInBytes) || !sane(top_info.ResultDataMaxSizeInBytes) || !sane(top_info.ScratchDataSizeInBytes)){
        cf_fail(out,E_FAIL,"prebuild-sizes");return out;}
    const D3D12_RESOURCE_FLAGS uav_flag=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_DEFAULT,bottom_info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,uav_flag,blas,"bottom level"))){cf_fail(out,hr,"blas");return out;}
    keep.hold(blas);
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_DEFAULT,top_info.ResultDataMaxSizeInBytes,D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,uav_flag,tlas,"top level"))){cf_fail(out,hr,"tlas");return out;}
    keep.hold(tlas);
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_DEFAULT,(std::max)(bottom_info.ScratchDataSizeInBytes,top_info.ScratchDataSizeInBytes),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,uav_flag,scratch,"scratch"))){cf_fail(out,hr,"scratch");return out;}
    keep.hold(scratch);
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_DEFAULT,output_bytes,D3D12_RESOURCE_STATE_COPY_DEST,uav_flag,output,"OUTPUT"))){cf_fail(out,hr,"output");return out;}
    keep.hold(output);
    // Arguments and counts: DEFAULT memory the CPU cannot map, written only by the compute shader.
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_DEFAULT,256,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,uav_flag,args,"indirect arguments DEFAULT"))){cf_fail(out,hr,"args");return out;}
    keep.hold(args);
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_DEFAULT,256,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,uav_flag,counts,"count buffer DEFAULT"))){cf_fail(out,hr,"counts");return out;}
    keep.hold(counts);
    if(FAILED(hr=cf_buffer(s,D3D12_HEAP_TYPE_READBACK,readback_bytes,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE,readback,"READBACK"))){cf_fail(out,hr,"readback");return out;}
    keep.hold(readback);
    const D3D12_GPU_VIRTUAL_ADDRESS blas_va=blas->GetGPUVirtualAddress(),tlas_va=tlas->GetGPUVirtualAddress(),scratch_va=scratch->GetGPUVirtualAddress(),
        output_va=output->GetGPUVirtualAddress(),args_va=args->GetGPUVirtualAddress(),counts_va=counts->GetGPUVirtualAddress();
    constexpr UINT64 as_align=D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
    if(!blas_va || !tlas_va || !scratch_va || !output_va || !args_va || !counts_va || blas_va%as_align || tlas_va%as_align || scratch_va%as_align){cf_fail(out,E_FAIL,"addresses");return out;}

    {   // UPLOAD: triangle at 0, instance at 256, output prefill at 1024.
        std::vector<unsigned char> bytes(static_cast<size_t>(upload_bytes),0);
        std::memcpy(bytes.data(),cf_ray_triangle,sizeof(cf_ray_triangle));
        D3D12_RAYTRACING_INSTANCE_DESC instance{};instance.Transform[0][0]=instance.Transform[1][1]=instance.Transform[2][2]=1.0f;
        instance.InstanceMask=0xFF;instance.AccelerationStructure=blas_va;
        std::memcpy(bytes.data()+instance_offset,&instance,sizeof(instance));
        for(UINT64 i=0;i<output_bytes/4;++i)std::memcpy(bytes.data()+prefill_offset+i*4,&cf_prefill,4);
        if(FAILED(hr=cf_write(s,upload.Get(),0,bytes.data(),bytes.size(),"UPLOAD"))){cf_fail(out,hr,"map");return out;}
    }
    unsigned char expected_args[2*sizeof(D3D12_DISPATCH_RAYS_DESC)];cf_dxr_records(table_va,expected_args);
    {   // READBACK starts at the complement of every expectation.
        std::vector<UINT32> prefill(static_cast<size_t>(readback_bytes/4),0x5A5A5A5Au);
        for(UINT sct=0;sct<cf_sections;++sct)for(UINT i=0;i<cf_section_words;++i)prefill[sct*cf_section_words+i]=~o.words[sct][i];
        for(UINT64 i=0;i<args_bytes/4;++i){UINT32 w=0;std::memcpy(&w,expected_args+i*4,4);prefill[static_cast<size_t>(args_readback/4+i)]=~w;}
        prefill[static_cast<size_t>(counts_readback/4)]=~1u;prefill[static_cast<size_t>(counts_readback/4+1)]=~2u;
        if(FAILED(hr=cf_prefill_readback(s,readback.Get(),prefill))){cf_fail(out,hr,"prefill");return out;}
    }

    // Root signatures: ray pipeline (t0 top level, u0 output, both root descriptors); argument writer (u0 args, u1 counts,
    // four root constants at b0).
    ComPtr<ID3D12RootSignature> rt_root,cs_root;
    {D3D12_ROOT_PARAMETER p[2]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
     const D3D12_ROOT_SIGNATURE_DESC d{2,p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
     if(FAILED(hr=cf_root(s,d,rt_root,"ray pipeline"))){cf_fail(out,hr,"root-signature");return out;}}
    keep.hold(rt_root);
    {D3D12_ROOT_PARAMETER p[3]{};p[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[0].Descriptor={0,0};
     p[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;p[1].Descriptor={1,0};
     p[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;p[2].Constants={0,0,4};
     const D3D12_ROOT_SIGNATURE_DESC d{3,p,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
     if(FAILED(hr=cf_root(s,d,cs_root,"argument writer"))){cf_fail(out,hr,"root-signature");return out;}}
    keep.hold(cs_root);
    ComPtr<ID3D12PipelineState> cs_pso;
    {D3D12_COMPUTE_PIPELINE_STATE_DESC d{};d.pRootSignature=cs_root.Get();d.CS={g_dxr_args_cs,sizeof(g_dxr_args_cs)};
     if(FAILED(hr=s.api("CreateComputePipelineState argument writer",[&]{return s.device->CreateComputePipelineState(&d,IID_PPV_ARGS(&cs_pso));}))){cf_fail(out,hr,"pso-cs");return out;}}
    keep.hold(cs_pso);
    ComPtr<ID3D12StateObject> state;
    {D3D12_EXPORT_DESC exports[4]{{L"raygen_a",nullptr,D3D12_EXPORT_FLAG_NONE},{L"raygen_b",nullptr,D3D12_EXPORT_FLAG_NONE},
         {L"miss",nullptr,D3D12_EXPORT_FLAG_NONE},{L"closest",nullptr,D3D12_EXPORT_FLAG_NONE}};
     D3D12_DXIL_LIBRARY_DESC library{{g_dxr_lib,sizeof(g_dxr_lib)},4,exports};
     D3D12_HIT_GROUP_DESC group{L"group",D3D12_HIT_GROUP_TYPE_TRIANGLES,nullptr,L"closest",nullptr};
     D3D12_RAYTRACING_SHADER_CONFIG shader_config{4,8};D3D12_RAYTRACING_PIPELINE_CONFIG pipeline_config{1};
     D3D12_GLOBAL_ROOT_SIGNATURE global{rt_root.Get()};
     const D3D12_STATE_SUBOBJECT subobjects[5]{{D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY,&library},{D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP,&group},
         {D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG,&shader_config},{D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG,&pipeline_config},
         {D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE,&global}};
     const D3D12_STATE_OBJECT_DESC desc{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE,5,subobjects};
     if(FAILED(hr=s.api("CreateStateObject RAYTRACING_PIPELINE lib_6_3 two raygen",[&]{return device5->CreateStateObject(&desc,IID_PPV_ARGS(&state));}))){cf_fail(out,hr,"state-object");return out;}}
    keep.hold(state);
    {ComPtr<ID3D12StateObjectProperties> properties;
     if(FAILED(hr=s.api("QueryInterface ID3D12StateObjectProperties",[&]{return state.As(&properties);}))){cf_fail(out,hr,"state-properties");return out;}
     const wchar_t* const names[4]{L"raygen_a",L"raygen_b",L"miss",L"group"};unsigned char bytes[table_bytes]{};
     for(int i=0;i<4;++i){
         const void* id=properties->GetShaderIdentifier(names[i]);
         if(!id){cf_fail(out,E_FAIL,"shader-identifier");return out;}
         std::memcpy(bytes+i*D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT,id,record);
     }
     s.event("after","Shader identifiers raygen_a raygen_b miss group");
     if(FAILED(hr=cf_write(s,table.Get(),0,bytes,sizeof(bytes),"shader table"))){cf_fail(out,hr,"map");return out;}}
    ComPtr<ID3D12CommandSignature> signature;HRESULT signature_hr=S_OK;
    {D3D12_INDIRECT_ARGUMENT_DESC argument{};argument.Type=D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS;
     const D3D12_COMMAND_SIGNATURE_DESC d{sizeof(D3D12_DISPATCH_RAYS_DESC),1,&argument,0};
     // A refusal is a result, not the end of the subtest: the direct DispatchRays still runs as the positive control,
     // the four indirect sections stay at the prefill and fail, and the reason names the refused call.
     signature_hr=s.api("CreateCommandSignature DISPATCH_RAYS stride 104",[&]{return s.device->CreateCommandSignature(&d,nullptr,IID_PPV_ARGS(&signature));});
     if(FAILED(signature_hr) || !signature){signature.Reset();if(SUCCEEDED(signature_hr))signature_hr=E_POINTER;}
#ifdef CONFORMANCE_TEST_REFUSE_SIGNATURE
     signature.Reset();signature_hr=E_NOTIMPL;s.event("after","Test build: command signature refusal simulated",signature_hr);
#endif
    }
    keep.hold(signature);

    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    if(FAILED(hr=cf_list(s,keep,allocator,list,fence))){cf_fail(out,hr,"command-list");return out;}
    ComPtr<ID3D12GraphicsCommandList4> list4;
    if(FAILED(hr=s.api("QueryInterface ID3D12GraphicsCommandList4",[&]{return list.As(&list4);}))){cf_fail(out,hr,"list4");return out;}
    keep.hold(list4);
    ID3D12GraphicsCommandList4* l=list4.Get();
    l->CopyBufferRegion(output.Get(),0,upload.Get(),prefill_offset,output_bytes);
    {const D3D12_RESOURCE_BARRIER b=cf_transition(output.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);l->ResourceBarrier(1,&b);}
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build{};build.DestAccelerationStructureData=blas_va;build.Inputs=bottom;build.ScratchAccelerationStructureData=scratch_va;
    l->BuildRaytracingAccelerationStructure(&build,0,nullptr);
    D3D12_RESOURCE_BARRIER uav{};uav.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;l->ResourceBarrier(1,&uav);
    build.DestAccelerationStructureData=tlas_va;build.Inputs=top;
    l->BuildRaytracingAccelerationStructure(&build,0,nullptr);l->ResourceBarrier(1,&uav);
    // The arguments come from the GPU: one compute thread writes both records and both count words.
    l->SetComputeRootSignature(cs_root.Get());l->SetPipelineState(cs_pso.Get());
    l->SetComputeRootUnorderedAccessView(0,args_va);l->SetComputeRootUnorderedAccessView(1,counts_va);
    const UINT32 constants[4]{static_cast<UINT32>(table_va),static_cast<UINT32>(table_va>>32),static_cast<UINT32>(record),0};
    l->SetComputeRoot32BitConstants(2,4,constants,0);l->Dispatch(1,1,1);
    {const D3D12_RESOURCE_BARRIER b[2]{cf_transition(args.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT),
        cf_transition(counts.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT)};l->ResourceBarrier(2,b);}
    l->SetComputeRootSignature(rt_root.Get());l->SetPipelineState1(state.Get());l->SetComputeRootShaderResourceView(0,tlas_va);
    D3D12_DISPATCH_RAYS_DESC direct;std::memcpy(&direct,expected_args,sizeof(direct));
    const UINT64 section_bytes=UINT64{cf_section_words}*4;
    l->SetComputeRootUnorderedAccessView(1,output_va+0*section_bytes);l->DispatchRays(&direct);
    if(signature){
        l->SetComputeRootUnorderedAccessView(1,output_va+1*section_bytes);l->ExecuteIndirect(signature.Get(),1,args.Get(),0,nullptr,0);
        l->SetComputeRootUnorderedAccessView(1,output_va+2*section_bytes);l->ExecuteIndirect(signature.Get(),2,args.Get(),0,counts.Get(),0);
        l->SetComputeRootUnorderedAccessView(1,output_va+3*section_bytes);l->ExecuteIndirect(signature.Get(),2,args.Get(),0,counts.Get(),4);
        l->SetComputeRootUnorderedAccessView(1,output_va+4*section_bytes);l->ExecuteIndirect(signature.Get(),2,args.Get(),0,nullptr,0);
    }
    {const D3D12_RESOURCE_BARRIER b[3]{cf_transition(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE),
        cf_transition(args.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_COPY_SOURCE),
        cf_transition(counts.Get(),D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,D3D12_RESOURCE_STATE_COPY_SOURCE)};l->ResourceBarrier(3,b);}
    l->CopyBufferRegion(readback.Get(),0,output.Get(),0,output_bytes);
    l->CopyBufferRegion(readback.Get(),args_readback,args.Get(),0,args_bytes);
    l->CopyBufferRegion(readback.Get(),counts_readback,counts.Get(),0,8);
    s.event("after",signature?"DXR list recorded: builds, argument writer, DispatchRays, ExecuteIndirect x4, readback copies"
                              :"DXR list recorded without ExecuteIndirect (command signature refused): builds, argument writer, DispatchRays, readback copies");
    if(FAILED(hr=cf_execute(s,keep,list.Get(),fence.Get()))){cf_fail(out,hr,"execute");return out;}
    std::vector<UINT32> got(static_cast<size_t>(readback_bytes/4));
    if(FAILED(hr=cf_read(s,readback.Get(),0,got.data(),got.size()*4,"READBACK"))){cf_fail(out,hr,"readback-map");return out;}
    cf_dump(s,L"dxr.bin",got);
    const CfDxrCompare c=cf_dxr_compare(o,got.data(),got.data()+args_readback/4,got.data()+counts_readback/4,expected_args);
    const CfDiff &args_diff=c.args,&count_control=c.count_control;const CfDiff (&section)[cf_sections]=c.section;
    std::string sections_json,sections_detail;
    for(UINT sct=0;sct<cf_sections;++sct){
        char t[256]{};
        sprintf_s(t,"%s{\"variant\":\"%s\",\"mismatches\":%u,\"checked\":%u,\"first\":%s}",sct?",":"",cf_section_name(sct),section[sct].count,section[sct].checked,
            cf_json_text(section[sct].first()).c_str());sections_json+=t;
        sprintf_s(t,"%s%s:%u/%u",sct?",":"",cf_section_name(sct),section[sct].count,section[sct].checked);sections_detail+=t;
    }
    out.mismatches=args_diff.count;out.checked=args_diff.checked;out.first=args_diff.first();
    for(UINT sct=0;sct<cf_sections;++sct){out.mismatches+=section[sct].count;out.checked+=section[sct].checked;if(out.first=="none")out.first=section[sct].first();}
    out.status=out.mismatches?"FAIL":"PASS";out.hr=out.mismatches?E_FAIL:S_OK;
    if(!signature){char r[64]{};sprintf_s(r,"CreateCommandSignature-DISPATCH_RAYS-refused-hr-%08lx",static_cast<unsigned long>(signature_hr));
        out.reason=r;out.status="FAIL";out.hr=signature_hr;}
    char t[640]{};
    sprintf_s(t,"variants=%s gpu_args=%u/%u hits=%u misses=%u control_count1_vs_count2_oracle=%u/%u",sections_detail.c_str(),args_diff.count,args_diff.checked,
        o.hits,o.misses,count_control.count,count_control.checked);
    out.detail=t;
    sprintf_s(t,"\"command_signature_hr\":\"%08lx\",\"raytracing_tier\":\"%s\",\"hits\":%u,\"misses\":%u,\"gpu_written_args_mismatches\":%u,\"gpu_written_args_first\":%s,"
        "\"control_count_discriminating_words\":%u,\"control_count1_image_vs_count2_oracle\":%u,\"control_comparator_selftest\":true,\"variants\":[",
        static_cast<unsigned long>(signature_hr),cf_rt_text(f.rt_tier).c_str(),o.hits,o.misses,args_diff.count,cf_json_text(args_diff.first()).c_str(),o.count_discriminating,count_control.count);
    out.json=t+sections_json+"]";
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Driver

inline CfFeatures cf_query_features(Session& s){
    CfFeatures f;char label[128]{};
    D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
    f.options_hr=s.api("CheckFeatureSupport OPTIONS",[&]{return s.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof(options));});
    if(SUCCEEDED(f.options_hr)){f.rovs=options.ROVsSupported;f.cr_tier=static_cast<UINT>(options.ConservativeRasterizationTier);}
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
    f.options5_hr=s.api("CheckFeatureSupport OPTIONS5",[&]{return s.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5,&options5,sizeof(options5));});
    if(SUCCEEDED(f.options5_hr))f.rt_tier=static_cast<UINT>(options5.RaytracingTier);
    const D3D_FEATURE_LEVEL requested[]{D3D_FEATURE_LEVEL_12_2,D3D_FEATURE_LEVEL_12_1,D3D_FEATURE_LEVEL_12_0,D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0};
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels{};levels.NumFeatureLevels=5;levels.pFeatureLevelsRequested=requested;
    f.levels_hr=s.api("CheckFeatureSupport FEATURE_LEVELS",[&]{return s.device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS,&levels,sizeof(levels));});
    if(SUCCEEDED(f.levels_hr))f.max_level=static_cast<UINT>(levels.MaxSupportedFeatureLevel);
    for(UINT model=0x69;model>=0x60;--model){
        D3D12_FEATURE_DATA_SHADER_MODEL query{static_cast<D3D_SHADER_MODEL>(model)};
        f.model_hr=s.device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&query,sizeof(query));
        if(SUCCEEDED(f.model_hr)){f.model=static_cast<UINT>(query.HighestShaderModel);break;}
    }
    sprintf_s(label,"Reported ROVsSupported %d",f.rovs?1:0);s.event("after",label,f.options_hr);
    sprintf_s(label,"Reported ConservativeRasterizationTier %u",f.cr_tier);s.event("after",label,f.options_hr);
    sprintf_s(label,"Reported RaytracingTier %s",cf_rt_text(f.rt_tier).c_str());s.event("after",label,f.options5_hr);
    sprintf_s(label,"Reported MaxSupportedFeatureLevel %s",cf_level_text(f.max_level).c_str());s.event("after",label,f.levels_hr);
    sprintf_s(label,"Reported HighestShaderModel %s",cf_model_text(f.model).c_str());s.event("after",label,f.model_hr);
    return f;
}
inline std::string cf_features_line(const CfFeatures& f,const char* device_level){
    char t[256]{};
    sprintf_s(t,"FEATURES ROVsSupported=%d ConservativeRasterizationTier=%u RaytracingTier=%s MaxSupportedFeatureLevel=%s HighestShaderModel=%s device=%s",
        f.rovs?1:0,f.cr_tier,cf_rt_text(f.rt_tier).c_str(),cf_level_text(f.max_level).c_str(),cf_model_text(f.model).c_str(),device_level);
    return t;
}
// Driver modules in this process (path only): the module witness of which UMD answered.
inline std::string cf_modules_json(){
    HMODULE modules[1024]{};DWORD needed=0;std::string json="{";bool first=true;
    if(!EnumProcessModules(GetCurrentProcess(),modules,sizeof(modules),&needed))return "{}";
    const DWORD count=(std::min)(static_cast<DWORD>(needed/sizeof(HMODULE)),static_cast<DWORD>(1024));
    for(DWORD i=0;i<count;++i){
        char path[MAX_PATH]{};if(!GetModuleFileNameA(modules[i],path,MAX_PATH))continue;
        std::string lower=path;for(char& c:lower)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
        const size_t slash=lower.find_last_of('\\');const std::string name=slash==std::string::npos?lower:lower.substr(slash+1);
        static const char* const keys[]{"amdgpu_wddm","d3d12","d3d10warp","dxgi.dll","nvwgf2um","nvldumd","vulkan-1"};
        bool keep=false;for(const char* k:keys)if(name.find(k)!=std::string::npos)keep=true;
        if(!keep)continue;
        json+=(first?"":",")+cf_json_text(name)+":"+cf_json_text(path);first=false;
    }
    return json+"}";
}
struct CfReport {
    CfFeatures features;std::vector<CfOutcome> outcomes;UINT32 seed{};std::string adapter,modules{"{}"},overall{"ERROR"},device_level;HRESULT hr{E_FAIL};
    std::string to_json(const std::string& extra="")const{
        char t[512]{};
        sprintf_s(t,"{\"schema\":1,\"client\":\"amdgpu_wddm_conformance\",\"runtime\":\"system32/d3d12.dll\",\"overall\":\"%s\",\"hr\":\"%08lx\",\"seed\":\"%08x\","
            "\"device_feature_level\":\"%s\",\"features\":{\"ROVsSupported\":%d,\"ConservativeRasterizationTier\":%u,\"RaytracingTier\":\"%s\","
            "\"MaxSupportedFeatureLevel\":\"%s\",\"HighestShaderModel\":\"%s\",\"line\":",overall.c_str(),static_cast<unsigned long>(hr),seed,device_level.c_str(),
            features.rovs?1:0,features.cr_tier,cf_rt_text(features.rt_tier).c_str(),cf_level_text(features.max_level).c_str(),cf_model_text(features.model).c_str());
        std::string json=t+cf_json_text(cf_features_line(features,device_level.c_str()))+"},\"adapter\":"+cf_json_text(adapter)+",\"modules\":"+modules+",\"subtests\":[";
        for(size_t i=0;i<outcomes.size();++i)json+=(i?",":"")+outcomes[i].to_json();
        return json+"]"+(extra.empty()?"":","+extra)+"}\n";
    }
};
inline std::string cf_adapter_text(IDXGIAdapter1* adapter){
    DXGI_ADAPTER_DESC1 d{};if(!adapter || FAILED(adapter->GetDesc1(&d)))return "unknown";
    char name[128]{};WideCharToMultiByte(CP_UTF8,0,d.Description,-1,name,sizeof(name)-1,nullptr,nullptr);
    char t[256]{};sprintf_s(t,"VEN_%04X DEV_%04X %s%s",d.VendorId,d.DeviceId,name,(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)?" (software)":"");return t;
}
// Runs the selected subtests (mask: 1 rov, 2 conservative, 4 dxr-indirect) and fills the report; the HRESULT is S_OK only
// when every selected subtest passed, DXGI_ERROR_UNSUPPORTED when none failed but one was skipped, else the failure.
inline HRESULT conformance_execute(Session& s,CfReport& r,unsigned mask=7){
    if(!s.device || !s.queue || s.pending || s.copy_success)return E_UNEXPECTED;
    unsigned char random[4]{};
    if(BCryptGenRandom(nullptr,random,sizeof(random),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return E_FAIL;
    std::memcpy(&r.seed,random,4);
    {char label[48]{};sprintf_s(label,"Pattern seed %08x",r.seed);s.event("after",label);}
    r.adapter=cf_adapter_text(s.adapter.Get());
    // The minimum level the device was created with (build define, as in Session::create_device).
#ifdef INTERACTIVE_FEATURE_LEVEL_12_1
    r.device_level="12_1";
#else
    r.device_level="11_0";
#endif
    r.features=cf_query_features(s);
    const std::string features=cf_features_line(r.features,r.device_level.c_str());
    std::puts(features.c_str());std::fflush(stdout);s.event("after",features.c_str());
    const char* const names[3]{"rov","conservative","dxr-indirect"};
    for(unsigned k=0;k<3;++k){
        if(!(mask&(1u<<k)))continue;
        CfOutcome o;o.name=names[k];
        const HRESULT removed=s.device->GetDeviceRemovedReason();
        if(s.pending){o.reason="previous-submission-not-retired";o.hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
        else if(FAILED(removed)){o.reason="device-removed";o.hr=removed;}
        else if(GetTickCount64()+8000>s.deadline){o.reason="deadline";o.hr=HRESULT_FROM_WIN32(WAIT_TIMEOUT);}
        else if(s.abort_requested()){o.reason="abort";o.hr=HRESULT_FROM_WIN32(ERROR_CANCELLED);}
        else{
            char label[64]{};sprintf_s(label,"Subtest %s begin",names[k]);s.event("before",label);
            try{o=k==0?cf_rov(s,r.features,r.seed):k==1?cf_conservative(s,r.features):cf_dxr_indirect(s,r.features);}
            catch(const std::exception&){o.status="ERROR";o.reason="exception";o.hr=E_FAIL;}
        }
        r.outcomes.push_back(o);
        const std::string line=o.line();std::puts(line.c_str());std::fflush(stdout);
        char label[160]{};sprintf_s(label,"Compare conformance %s %s mismatches %u checked %u",o.name.c_str(),o.status.c_str(),o.mismatches,o.checked);
        s.event("after",label,o.hr);
    }
    {const HRESULT removed=s.device->GetDeviceRemovedReason();s.event("after","GetDeviceRemovedReason after subtests",removed);}
    r.modules=cf_modules_json();
    bool failed=false,skipped=false;HRESULT first_failure=S_OK;
    for(const auto& o:r.outcomes){
        if(o.status=="SKIP")skipped=true;
        else if(o.status!="PASS"){failed=true;if(first_failure==S_OK)first_failure=FAILED(o.hr)?o.hr:E_FAIL;}
    }
    if(r.outcomes.empty())failed=true,first_failure=E_INVALIDARG;
    r.overall=failed?"FAIL":skipped?"SKIP":"PASS";
    r.hr=failed?first_failure:skipped?DXGI_ERROR_UNSUPPORTED:S_OK;
    char t[64]{};sprintf_s(t,"CONFORMANCE overall %s hr=%08lx",r.overall.c_str(),static_cast<unsigned long>(r.hr));
    std::puts(t);std::fflush(stdout);s.event("after",t,r.hr);
    return r.hr;
}
// The interactive "copy" verb: all three subtests, conformance.json in the session directory, copy_success = all pass.
inline HRESULT conformance_run(Session& s){
    CfReport r;HRESULT hr=conformance_execute(s,r);
    if(hr==E_UNEXPECTED && r.outcomes.empty())return hr;
    if(!publish(s.root/L"conformance.json",r.to_json())){s.event("after","conformance.json not written",E_FAIL);if(SUCCEEDED(hr))hr=E_FAIL;}
    s.copy_success=hr==S_OK;
    return hr;
}
