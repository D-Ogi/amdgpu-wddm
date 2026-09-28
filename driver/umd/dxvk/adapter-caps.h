// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include <cstring>
namespace bc250::umd {
// A capability snapshot must come from the bound engine/ICD, at the adapter's
// maximum offered level. Querying an FL10 device directly would hide FL11 caps.
struct AdapterCaps {
    D3D_FEATURE_LEVEL maximum{};
    D3D11_FEATURE_DATA_DOUBLES doubles{};
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS compute{};
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    D3D11_FEATURE_DATA_ARCHITECTURE_INFO architecture{};
    D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT precision{};
};
inline UINT adapter_pipeline_mask(D3D_FEATURE_LEVEL level) {
    switch(level) {
    case D3D_FEATURE_LEVEL_10_0:return 1;
    case D3D_FEATURE_LEVEL_10_1:return 3;
    case D3D_FEATURE_LEVEL_11_0:return 7;
    case D3D_FEATURE_LEVEL_11_1:return 15;
    default:return 0;
    }
}
inline bool valid_adapter_caps(const AdapterCaps &caps) {
    if (!adapter_pipeline_mask(caps.maximum)) return false;
    if (caps.maximum>=D3D_FEATURE_LEVEL_11_0 &&
        !caps.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x) return false;
    if (caps.maximum==D3D_FEATURE_LEVEL_11_1 && !caps.options.OutputMergerLogicOp) return false;
    const UINT known=D3D11_SHADER_MIN_PRECISION_10_BIT|D3D11_SHADER_MIN_PRECISION_16_BIT;
    return !((caps.precision.PixelShaderMinPrecision|caps.precision.AllOtherShaderStagesMinPrecision)&~known);
}
template<typename Query> HRESULT read_adapter_caps(D3D_FEATURE_LEVEL maximum,Query &&query,AdapterCaps &out) {
    if (!adapter_pipeline_mask(maximum)) return E_INVALIDARG;
    AdapterCaps caps{};caps.maximum=maximum;
    auto read=[&](D3D11_FEATURE feature,auto &data) {return query(maximum,feature,&data,UINT(sizeof(data)));};
    HRESULT hr=read(D3D11_FEATURE_DOUBLES,caps.doubles);if(FAILED(hr))return hr;
    hr=read(D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,caps.compute);if(FAILED(hr))return hr;
    hr=read(D3D11_FEATURE_D3D11_OPTIONS,caps.options);if(FAILED(hr))return hr;
    hr=read(D3D11_FEATURE_ARCHITECTURE_INFO,caps.architecture);if(FAILED(hr))return hr;
    hr=read(D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT,caps.precision);if(FAILED(hr))return hr;
    if (!valid_adapter_caps(caps)) return E_FAIL;
    out=caps;return S_OK;
}
template<typename T> HRESULT write_adapter_cap(const D3D10_2DDIARG_GETCAPS &args,const T &value) {
    if (!args.pData || args.DataSize!=sizeof(T)) return E_INVALIDARG;
    std::memcpy(args.pData,&value,sizeof(value));return S_OK;
}
inline HRESULT get_adapter_caps(const AdapterCaps &caps,const D3D10_2DDIARG_GETCAPS &args) {
    if (!valid_adapter_caps(caps)) return E_FAIL;
    switch(args.Type) {
    case D3D11DDICAPS_THREADING:
        // Engine E2 requires runtime-domain serialization; no command lists.
        return write_adapter_cap(args,D3D11DDI_THREADING_CAPS{0});
    case D3D11DDICAPS_SHADER: {
        UINT bits=0;
        if(caps.doubles.DoublePrecisionFloatShaderOps)bits|=D3D11DDICAPS_SHADER_DOUBLES;
        if(caps.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
            bits|=D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
        return write_adapter_cap(args,D3D11DDI_SHADER_CAPS{bits});
    }
    case D3D11DDICAPS_3DPIPELINESUPPORT:
        return write_adapter_cap(args,D3D11DDI_3DPIPELINESUPPORT_CAPS{adapter_pipeline_mask(caps.maximum)});
    case D3D11_1DDICAPS_D3D11_OPTIONS:
        return write_adapter_cap(args,D3D11_1DDI_D3D11_OPTIONS_DATA{caps.options.OutputMergerLogicOp,FALSE});
    case D3D11_1DDICAPS_ARCHITECTURE_INFO:
        return write_adapter_cap(args,D3D11_1DDI_ARCHITECTURE_INFO_DATA{caps.architecture.TileBasedDeferredRenderer});
    case D3D11_1DDICAPS_SHADER_MIN_PRECISION_SUPPORT:
        static_assert(UINT(D3D11_SHADER_MIN_PRECISION_10_BIT)==UINT(D3D11_DDI_SHADER_MIN_PRECISION_10_BIT));
        static_assert(UINT(D3D11_SHADER_MIN_PRECISION_16_BIT)==UINT(D3D11_DDI_SHADER_MIN_PRECISION_16_BIT));
        return write_adapter_cap(args,D3D11_DDI_SHADER_MIN_PRECISION_SUPPORT_DATA{
            caps.precision.PixelShaderMinPrecision,caps.precision.AllOtherShaderStagesMinPrecision});
    default:return E_NOTIMPL; // No invented layout or capability for an unknown query.
    }
}
}
