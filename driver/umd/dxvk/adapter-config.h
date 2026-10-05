// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-adapter.h"
namespace bc250::umd {
// Little-endian fixed-width deployment record beside amdgpu_wddm_d3d11.dll.
// Version 1 (108 bytes) records FL10_0..FL11_1 adapters. Version 2 (132 bytes)
// adds the FL12 capabilities after other_precision; an FL12 maximum needs it.
struct AdapterConfigRecord {
    UINT32 magic,version,size,reserved;
    UINT32 maximum,doubles,compute,logic_op,tile_based,pixel_precision,other_precision;
    unsigned char engine_sha256[32],icd_sha256[32];
};
static_assert(sizeof(AdapterConfigRecord)==108);
struct AdapterConfigRecord2 {
    UINT32 magic,version,size,reserved;
    UINT32 maximum,doubles,compute,logic_op,tile_based,pixel_precision,other_precision;
    UINT32 tiled_tier,typed_uav_loads,rovs,stencil_ref,conservative_tier,vp_rt_index;
    unsigned char engine_sha256[32],icd_sha256[32];
};
static_assert(sizeof(AdapterConfigRecord2)==132);
inline constexpr UINT32 adapter_config_magic=0x4334314d; // M14C
inline HRESULT decode_adapter_config(const AdapterConfigRecord2 &r,AdapterConfiguration &out) {
    const bool v2=r.version==2;
    if(r.magic!=adapter_config_magic || (r.version!=1 && !v2) ||
       r.size!=(v2 ? sizeof(AdapterConfigRecord2) : sizeof(AdapterConfigRecord)) || r.reserved ||
       r.doubles>1 || r.compute>1 || r.logic_op>1 || r.tile_based>1) return E_INVALIDARG;
    if(v2 && (r.typed_uav_loads>1 || r.rovs>1 || r.stencil_ref>1 || r.vp_rt_index>1)) return E_INVALIDARG;
    bool engine=false,icd=false;
    for(unsigned i=0;i<32;++i){engine|=r.engine_sha256[i]!=0;icd|=r.icd_sha256[i]!=0;}
    if(!engine || !icd)return E_INVALIDARG;
    AdapterCaps caps{};caps.maximum=static_cast<D3D_FEATURE_LEVEL>(r.maximum);
    caps.doubles.DoublePrecisionFloatShaderOps=r.doubles;
    caps.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x=r.compute;
    caps.options.OutputMergerLogicOp=r.logic_op;
    caps.architecture.TileBasedDeferredRenderer=r.tile_based;
    caps.precision.PixelShaderMinPrecision=r.pixel_precision;
    caps.precision.AllOtherShaderStagesMinPrecision=r.other_precision;
    if(v2) {
        caps.options2.TiledResourcesTier=static_cast<D3D11_TILED_RESOURCES_TIER>(r.tiled_tier);
        caps.options2.TypedUAVLoadAdditionalFormats=r.typed_uav_loads;
        caps.options2.ROVsSupported=r.rovs;
        caps.options2.PSSpecifiedStencilRefSupported=r.stencil_ref;
        caps.options2.ConservativeRasterizationTier=static_cast<D3D11_CONSERVATIVE_RASTERIZATION_TIER>(r.conservative_tier);
        caps.options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer=r.vp_rt_index;
    }
    // valid_adapter_caps refuses FL12 caps below FL12_0 and an FL12 maximum without them (a v1 record).
    if(!valid_adapter_caps(caps))return E_INVALIDARG;
    out.caps=caps;
    std::memcpy(out.engine_sha256,r.engine_sha256,32);std::memcpy(out.icd_sha256,r.icd_sha256,32);
    return S_OK;
}
// Decodes the bytes of either version: the record's own size field selects the layout.
inline HRESULT decode_adapter_config(const void *data,size_t bytes,AdapterConfiguration &out) {
    AdapterConfigRecord2 r{};
    if(!data || (bytes!=sizeof(AdapterConfigRecord) && bytes!=sizeof(AdapterConfigRecord2)))return E_INVALIDARG;
    if(bytes==sizeof(AdapterConfigRecord)) {
        AdapterConfigRecord v1{};std::memcpy(&v1,data,sizeof(v1));
        std::memcpy(&r,&v1,offsetof(AdapterConfigRecord,engine_sha256));
        std::memcpy(r.engine_sha256,v1.engine_sha256,32);std::memcpy(r.icd_sha256,v1.icd_sha256,32);
    } else std::memcpy(&r,data,sizeof(r));
    if(r.size!=bytes)return E_INVALIDARG;
    return decode_adapter_config(r,out);
}
inline HRESULT decode_adapter_config(const AdapterConfigRecord &r,AdapterConfiguration &out) {
    return decode_adapter_config(&r,sizeof(r),out);
}
}
