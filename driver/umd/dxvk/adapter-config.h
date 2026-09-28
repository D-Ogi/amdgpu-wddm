// SPDX-License-Identifier: MIT
#pragma once
#include "ddi-adapter.h"
namespace bc250::umd {
// Little-endian fixed-width deployment record beside bc250d3d11.dll.
struct AdapterConfigRecord {
    UINT32 magic,version,size,reserved;
    UINT32 maximum,doubles,compute,logic_op,tile_based,pixel_precision,other_precision;
    unsigned char engine_sha256[32],icd_sha256[32];
};
static_assert(sizeof(AdapterConfigRecord)==108);
inline constexpr UINT32 adapter_config_magic=0x4334314d; // M14C
inline HRESULT decode_adapter_config(const AdapterConfigRecord &r,AdapterConfiguration &out) {
    if(r.magic!=adapter_config_magic || r.version!=1 || r.size!=sizeof(r) || r.reserved ||
       r.doubles>1 || r.compute>1 || r.logic_op>1 || r.tile_based>1) return E_INVALIDARG;
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
    if(!valid_adapter_caps(caps))return E_INVALIDARG;
    out.caps=caps;
    std::memcpy(out.engine_sha256,r.engine_sha256,32);std::memcpy(out.icd_sha256,r.icd_sha256,32);
    return S_OK;
}
}
