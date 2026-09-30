// SPDX-License-Identifier: MIT
#include "adapter-caps.h"
#include <cstdio>
#include <cstdlib>
using namespace bc250::umd;
#define CHECK(x) do { if(!(x)) { std::printf("FAIL line %d\n",__LINE__);std::abort(); } } while(0)
int main() {
    AdapterCaps good{}; good.maximum=D3D_FEATURE_LEVEL_11_1;
    good.doubles.DoublePrecisionFloatShaderOps=TRUE;
    good.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=TRUE;
    good.options.OutputMergerLogicOp=TRUE;
    good.precision.PixelShaderMinPrecision=D3D11_SHADER_MIN_PRECISION_10_BIT;
    good.precision.AllOtherShaderStagesMinPrecision=D3D11_SHADER_MIN_PRECISION_16_BIT;
    CHECK(valid_adapter_caps(good));
    unsigned calls=0,failAt=0;
    auto query=[&](D3D_FEATURE_LEVEL level,D3D11_FEATURE type,void *data,UINT size) -> HRESULT {
        CHECK(level==D3D_FEATURE_LEVEL_11_1);
        if(++calls==failAt)return E_OUTOFMEMORY;
        switch(type) {
#define COPY(feature,member) case feature: CHECK(size==sizeof(good.member));std::memcpy(data,&good.member,size);break
        COPY(D3D11_FEATURE_DOUBLES,doubles);
        COPY(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,compute);
        COPY(D3D11_FEATURE_D3D11_OPTIONS,options);
        COPY(D3D11_FEATURE_ARCHITECTURE_INFO,architecture);
        COPY(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT,precision);
#undef COPY
        default:std::abort();
        }
        return S_OK;
    };
    AdapterCaps result{};
    for(failAt=1;failAt<=5;++failAt) {
        calls=0;CHECK(read_adapter_caps(good.maximum,query,result)==E_OUTOFMEMORY);
        CHECK(calls==failAt && result.maximum==0);
    }
    failAt=0;calls=0;
    CHECK(read_adapter_caps(good.maximum,query,result)==S_OK && calls==5 && result.maximum==good.maximum);
    const D3D_FEATURE_LEVEL unsupported[]={D3D_FEATURE_LEVEL_9_1,D3D_FEATURE_LEVEL_12_2};
    for(auto level:unsupported) {
        calls=0;CHECK(read_adapter_caps(level,query,result)==E_INVALIDARG && !calls && result.maximum==good.maximum);
    }
    auto bad=good;bad.options.OutputMergerLogicOp=FALSE;CHECK(!valid_adapter_caps(bad));
    bad=good;bad.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=FALSE;CHECK(!valid_adapter_caps(bad));
    bad=good;bad.precision.PixelShaderMinPrecision=4;CHECK(!valid_adapter_caps(bad));
    const D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_10_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_11_1};
    for(unsigned i=0;i<4;++i) {
        auto caps=good;caps.maximum=levels[i];
        D3D11DDI_3DPIPELINESUPPORT_CAPS pipeline{};
        D3D10_2DDIARG_GETCAPS args{};args.Type=D3D11DDICAPS_3DPIPELINESUPPORT;
        args.pData=&pipeline;args.DataSize=sizeof(pipeline);
        CHECK(get_adapter_caps(caps,args)==S_OK && pipeline.Caps==((1u<<(i+1))-1));
    }
    good.options.OutputMergerLogicOp=FALSE;calls=0;
    const auto before=result;
    CHECK(read_adapter_caps(good.maximum,query,result)==E_FAIL && calls==5 &&
          !std::memcmp(&before,&result,sizeof(result)));
    good.options.OutputMergerLogicOp=TRUE;
    CHECK(adapter_caps_supported(good,good));
    auto advertised=good;advertised.doubles.DoublePrecisionFloatShaderOps=FALSE;
    advertised.precision.PixelShaderMinPrecision=0;
    CHECK(adapter_caps_supported(advertised,good));
    auto actual=good;actual.doubles.DoublePrecisionFloatShaderOps=FALSE;
    CHECK(!adapter_caps_supported(good,actual));
    actual=good;actual.precision.AllOtherShaderStagesMinPrecision=0;
    CHECK(!adapter_caps_supported(good,actual));
    actual=good;actual.architecture.TileBasedDeferredRenderer=TRUE;
    CHECK(!adapter_caps_supported(good,actual));
    calls=0;CHECK(verify_adapter_caps(good,query)==S_OK && calls==5);
    advertised=good;advertised.architecture.TileBasedDeferredRenderer=TRUE;
    calls=0;CHECK(verify_adapter_caps(advertised,query)==DXGI_ERROR_UNSUPPORTED && calls==5);
    failAt=3;calls=0;CHECK(verify_adapter_caps(good,query)==E_OUTOFMEMORY && calls==3);failAt=0;
    const D3D10_2DDICAPS_TYPE types[]={D3D11DDICAPS_THREADING,D3D11DDICAPS_SHADER,D3D11DDICAPS_3DPIPELINESUPPORT,
        D3D11_1DDICAPS_D3D11_OPTIONS,D3D11_1DDICAPS_ARCHITECTURE_INFO,D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT};
    const UINT sizes[]={4,4,4,8,4,8};
    const UINT expected[][2]={{0,0},{3,0},{15,0},{TRUE,FALSE},{FALSE,0},{1,2}};
    for(unsigned i=0;i<6;++i) {
        UINT words[3]={99,99,99};D3D10_2DDIARG_GETCAPS args{};args.Type=types[i];args.pData=words;
        for(UINT size=0;size<12;++size) if(size!=sizes[i]) {
            args.DataSize=size;CHECK(get_adapter_caps(good,args)==E_INVALIDARG);
            CHECK(words[0]==99 && words[1]==99 && words[2]==99);
        }
        args.DataSize=sizes[i];CHECK(get_adapter_caps(good,args)==S_OK);
        CHECK(words[0]==expected[i][0] && words[2]==99);
        CHECK(words[1]==(sizes[i]==8 ? expected[i][1] : 99));
        args.pData=nullptr;CHECK(get_adapter_caps(good,args)==E_INVALIDARG);
    }
    UINT marker=99;D3D10_2DDIARG_GETCAPS unknown{};unknown.Type=static_cast<D3D10_2DDICAPS_TYPE>(0);
    unknown.pData=&marker;unknown.DataSize=4;
    CHECK(get_adapter_caps(good,unknown)==E_NOTIMPL && marker==99);
    CHECK(get_adapter_caps(bad,unknown)==E_FAIL && marker==99);
    // WDDM 1.3/2.0 queries are an FL12 adapter's alone.
    unknown.Type=D3DWDDM1_3DDICAPS_D3D11_OPTIONS1;CHECK(get_adapter_caps(good,unknown)==E_NOTIMPL && marker==99);

    // FL12: tiled tier 2 and typed UAV loads for 12_0, conservative rasterization and ROVs for 12_1.
    AdapterCaps fl12=good;fl12.maximum=D3D_FEATURE_LEVEL_12_1;
    fl12.options2.TiledResourcesTier=D3D11_TILED_RESOURCES_TIER_3;fl12.options2.TypedUAVLoadAdditionalFormats=TRUE;
    fl12.options2.ROVsSupported=TRUE;fl12.options2.PSSpecifiedStencilRefSupported=TRUE;
    fl12.options2.ConservativeRasterizationTier=D3D11_CONSERVATIVE_RASTERIZATION_TIER_2;
    fl12.options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer=TRUE;
    CHECK(valid_adapter_caps(fl12));
    auto broken=fl12;broken.options2.TiledResourcesTier=D3D11_TILED_RESOURCES_TIER_1;CHECK(!valid_adapter_caps(broken));
    broken=fl12;broken.options2.TypedUAVLoadAdditionalFormats=FALSE;CHECK(!valid_adapter_caps(broken));
    broken=fl12;broken.options2.ROVsSupported=FALSE;CHECK(!valid_adapter_caps(broken));
    broken.maximum=D3D_FEATURE_LEVEL_12_0;CHECK(valid_adapter_caps(broken));
    broken=fl12;broken.options2.ConservativeRasterizationTier=D3D11_CONSERVATIVE_RASTERIZATION_NOT_SUPPORTED;
    CHECK(!valid_adapter_caps(broken));
    broken=fl12;broken.options2.UnifiedMemoryArchitecture=TRUE;CHECK(!valid_adapter_caps(broken));
    broken=good;broken.options2.TiledResourcesTier=D3D11_TILED_RESOURCES_TIER_2;CHECK(!valid_adapter_caps(broken));
    const auto clamped=without_fl12(fl12);
    CHECK(clamped.maximum==D3D_FEATURE_LEVEL_11_1 && fl12_caps_empty(clamped) && valid_adapter_caps(clamped));
    unsigned fl12Calls=0;D3D11_FEATURE_DATA_D3D11_OPTIONS2 engineOptions2=fl12.options2;
    auto query12=[&](D3D_FEATURE_LEVEL level,D3D11_FEATURE type,void *data,UINT size) -> HRESULT {
        CHECK(level==D3D_FEATURE_LEVEL_12_1);++fl12Calls;
        switch(type) {
#define COPY(feature,value) case feature: CHECK(size==sizeof(value));std::memcpy(data,&value,size);break
        COPY(D3D11_FEATURE_DOUBLES,fl12.doubles);
        COPY(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,fl12.compute);
        COPY(D3D11_FEATURE_D3D11_OPTIONS,fl12.options);
        COPY(D3D11_FEATURE_ARCHITECTURE_INFO,fl12.architecture);
        COPY(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT,fl12.precision);
        COPY(D3D11_FEATURE_D3D11_OPTIONS2,engineOptions2);
        COPY(D3D11_FEATURE_D3D11_OPTIONS3,fl12.options3);
#undef COPY
        default:std::abort();
        }
        return S_OK;
    };
    CHECK(verify_adapter_caps(fl12,query12)==S_OK && fl12Calls==7);
    auto weaker=fl12;weaker.options2.TiledResourcesTier=D3D11_TILED_RESOURCES_TIER_2;
    weaker.options2.ConservativeRasterizationTier=D3D11_CONSERVATIVE_RASTERIZATION_TIER_1;
    CHECK(verify_adapter_caps(weaker,query12)==S_OK);
    engineOptions2.ConservativeRasterizationTier=D3D11_CONSERVATIVE_RASTERIZATION_TIER_1;
    CHECK(verify_adapter_caps(fl12,query12)==DXGI_ERROR_UNSUPPORTED);
    engineOptions2=fl12.options2;engineOptions2.UnifiedMemoryArchitecture=TRUE;engineOptions2.StandardSwizzle=TRUE;
    CHECK(verify_adapter_caps(fl12,query12)==S_OK); // properties the shell does not offer
    struct Expect { D3D10_2DDICAPS_TYPE type; UINT size; UINT first,second; };
    const Expect answers[]={
        {D3D11DDICAPS_3DPIPELINESUPPORT,4,0x18F,0},
        {D3D11DDICAPS_SHADER,4,0x73,0},
        {D3DWDDM1_3DDICAPS_D3D11_OPTIONS1,4,7,0},
        {D3DWDDM1_3DDICAPS_MARKER,4,0,0},
        {D3DWDDM2_0DDICAPS_D3D11_OPTIONS2,4,2,0},
        {D3DWDDM2_0DDICAPS_D3D11_OPTIONS2,8,2,0},
        {D3DWDDM2_0DDICAPS_MEMORY_ARCHITECTURE,8,FALSE,FALSE},
        {D3DWDDM2_0DDICAPS_TEXTURE_LAYOUT,12,0,0},
        {D3DWDDM2_0DDICAPS_D3D11_OPTIONS3,4,TRUE,0},
        {D3DWDDM2_0DDICAPS_GPUVA_CAPS,4,40,0}};
    for(const auto &a:answers) {
        UINT words[4]={99,99,99,99};D3D10_2DDIARG_GETCAPS args{};args.Type=a.type;args.pData=words;args.DataSize=a.size;
        CHECK(get_adapter_caps(fl12,args)==S_OK && words[0]==a.first && words[3]==99);
        if(a.size>=8)CHECK(words[1]==a.second);
        args.DataSize=a.size+1;CHECK(get_adapter_caps(fl12,args)==E_INVALIDARG);
    }
    D3D11DDI_3DPIPELINESUPPORT_CAPS pipeline12{};D3D10_2DDIARG_GETCAPS pipelineArgs{};
    pipelineArgs.Type=D3D11DDICAPS_3DPIPELINESUPPORT;pipelineArgs.pData=&pipeline12;pipelineArgs.DataSize=sizeof(pipeline12);
    auto fl12_0=fl12;fl12_0.maximum=D3D_FEATURE_LEVEL_12_0;
    CHECK(get_adapter_caps(fl12_0,pipelineArgs)==S_OK && pipeline12.Caps==0x8F);
    unknown.Type=D3DWDDM2_2DDICAPS_SHADERCACHE;CHECK(get_adapter_caps(fl12,unknown)==E_NOTIMPL && marker==99);
    std::puts("PASS adapter caps level, failure atomicity, size and field mapping controls, FL12 caps and WDDM 2.0 queries");
}
