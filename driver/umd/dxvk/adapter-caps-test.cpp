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
    const D3D_FEATURE_LEVEL unsupported[]={D3D_FEATURE_LEVEL_9_1,D3D_FEATURE_LEVEL_12_0};
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
    std::puts("PASS adapter caps level, failure atomicity, size and field mapping controls");
}
