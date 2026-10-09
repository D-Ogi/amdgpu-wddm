// SPDX-License-Identifier: MIT
#pragma once
#include "runtime-bridge.h"
#include <atomic>
#include <cstdio>
#include <cstring>
namespace bc250::umd {
// A capability snapshot must come from the bound engine/ICD, at the adapter's
// maximum offered level. Querying an FL10 device directly would hide FL11 caps.
// options2/options3 are the FL12 capabilities; below FL12_0 they stay zero
// (neither read from the engine nor advertised).
struct AdapterCaps {
    D3D_FEATURE_LEVEL maximum{};
    D3D11_FEATURE_DATA_DOUBLES doubles{};
    D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS compute{};
    D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
    D3D11_FEATURE_DATA_ARCHITECTURE_INFO architecture{};
    D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT precision{};
    D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2{};
    D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3{};
};
// Bit n stands for D3D11DDI_3DPIPELINELEVEL n; FL9 (bits 4-6) is not offered.
inline UINT adapter_pipeline_mask(D3D_FEATURE_LEVEL level) {
    switch(level) {
    case D3D_FEATURE_LEVEL_10_0:return 1;
    case D3D_FEATURE_LEVEL_10_1:return 3;
    case D3D_FEATURE_LEVEL_11_0:return 7;
    case D3D_FEATURE_LEVEL_11_1:return 15;
    case D3D_FEATURE_LEVEL_12_0:return 15|(1u<<D3DWDDM2_0DDI_3DPIPELINELEVEL_12_0);
    case D3D_FEATURE_LEVEL_12_1:return 15|(1u<<D3DWDDM2_0DDI_3DPIPELINELEVEL_12_0)|(1u<<D3DWDDM2_0DDI_3DPIPELINELEVEL_12_1);
    default:return 0;
    }
}
inline bool fl12_caps_empty(const AdapterCaps &caps) {
    const D3D11_FEATURE_DATA_D3D11_OPTIONS2 none2{};const D3D11_FEATURE_DATA_D3D11_OPTIONS3 none3{};
    return !std::memcmp(&caps.options2,&none2,sizeof(none2)) && !std::memcmp(&caps.options3,&none3,sizeof(none3));
}
inline bool valid_adapter_caps(const AdapterCaps &caps) {
    if (!adapter_pipeline_mask(caps.maximum)) return false;
    if (caps.maximum>=D3D_FEATURE_LEVEL_11_0 &&
        !caps.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x) return false;
    if (caps.maximum>=D3D_FEATURE_LEVEL_11_1 && !caps.options.OutputMergerLogicOp) return false;
    const auto &o=caps.options2;
    if (caps.maximum<D3D_FEATURE_LEVEL_12_0) {
        if (!fl12_caps_empty(caps)) return false;
    } else {
        // D3D11.3 FL12_0: tiled resources tier 2 and typed UAV loads; FL12_1 adds
        // conservative rasterization and ROVs. Standard swizzle and UMA are not offered.
        if (o.TiledResourcesTier<D3D11_TILED_RESOURCES_TIER_2 || o.TiledResourcesTier>D3D11_TILED_RESOURCES_TIER_3 ||
            !o.TypedUAVLoadAdditionalFormats || o.ConservativeRasterizationTier>D3D11_CONSERVATIVE_RASTERIZATION_TIER_3 ||
            o.StandardSwizzle || o.UnifiedMemoryArchitecture) return false;
        if (caps.maximum==D3D_FEATURE_LEVEL_12_1 &&
            (o.ConservativeRasterizationTier<D3D11_CONSERVATIVE_RASTERIZATION_TIER_1 || !o.ROVsSupported)) return false;
    }
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
    if (maximum>=D3D_FEATURE_LEVEL_12_0) {
        hr=read(D3D11_FEATURE_D3D11_OPTIONS2,caps.options2);if(FAILED(hr))return hr;
        hr=read(D3D11_FEATURE_D3D11_OPTIONS3,caps.options3);if(FAILED(hr))return hr;
        // Properties the shell does not offer, whatever the engine says.
        caps.options2.StandardSwizzle=FALSE;caps.options2.UnifiedMemoryArchitecture=FALSE;
    }
    if (!valid_adapter_caps(caps)) return E_FAIL;
    out=caps;return S_OK;
}
// Optional capabilities may be conservatively omitted, but every advertised
// bit must hold on the engine. Architecture is a property, not an optional bit.
inline bool adapter_caps_supported(const AdapterCaps &advertised,const AdapterCaps &actual) {
    if (!valid_adapter_caps(advertised) || !valid_adapter_caps(actual) || actual.maximum<advertised.maximum) return false;
    const auto &a=advertised.options2,&e=actual.options2;
    return (!advertised.doubles.DoublePrecisionFloatShaderOps || actual.doubles.DoublePrecisionFloatShaderOps) &&
        (!advertised.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x ||
          actual.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x) &&
        (!advertised.options.OutputMergerLogicOp || actual.options.OutputMergerLogicOp) &&
        (bool(advertised.architecture.TileBasedDeferredRenderer)==bool(actual.architecture.TileBasedDeferredRenderer)) &&
        !(advertised.precision.PixelShaderMinPrecision&~actual.precision.PixelShaderMinPrecision) &&
        !(advertised.precision.AllOtherShaderStagesMinPrecision&~actual.precision.AllOtherShaderStagesMinPrecision) &&
        (!a.PSSpecifiedStencilRefSupported || e.PSSpecifiedStencilRefSupported) &&
        (!a.TypedUAVLoadAdditionalFormats || e.TypedUAVLoadAdditionalFormats) &&
        (!a.ROVsSupported || e.ROVsSupported) &&
        a.ConservativeRasterizationTier<=e.ConservativeRasterizationTier && a.TiledResourcesTier<=e.TiledResourcesTier &&
        (!advertised.options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer ||
          actual.options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
}
template<typename Query> HRESULT verify_adapter_caps(const AdapterCaps &advertised,Query &&query) {
    if (!valid_adapter_caps(advertised)) return E_INVALIDARG;
    AdapterCaps actual{};
    HRESULT hr=read_adapter_caps(advertised.maximum,query,actual);
    if (FAILED(hr)) return hr;
    return adapter_caps_supported(advertised,actual) ? S_OK : DXGI_ERROR_UNSUPPORTED;
}
// The adapter without FL12: the sparse policy is off, or the query failed.
inline AdapterCaps without_fl12(AdapterCaps caps) {
    if (caps.maximum>D3D_FEATURE_LEVEL_11_1) caps.maximum=D3D_FEATURE_LEVEL_11_1;
    caps.options2={};caps.options3={};
    return caps;
}
// Bits of the address space one resource may span (D3DWDDM2_0DDICAPS_GPUVA_CAPS). DXVK answers 32/40 as
// "not accurate"; 40 is within the KMD's 48-bit GPU virtual address space (BC250_WDDM_VA_BITS).
inline constexpr UINT adapter_gpuva_bits_per_resource=40;
template<typename T> HRESULT write_adapter_cap(const D3D10_2DDIARG_GETCAPS &args,const T &value) {
    if (!args.pData || args.DataSize!=sizeof(T)) return E_INVALIDARG;
    std::memcpy(args.pData,&value,sizeof(value));return S_OK;
}
inline HRESULT get_adapter_caps(const AdapterCaps &caps,const D3D10_2DDIARG_GETCAPS &args) {
    if (!valid_adapter_caps(caps)) return E_FAIL;
    const bool fl12=caps.maximum>=D3D_FEATURE_LEVEL_12_0;
    const auto &o=caps.options2;
    switch(args.Type) {
    case D3D11DDICAPS_THREADING:
        // Engine E2 requires runtime-domain serialization; no command lists.
        return write_adapter_cap(args,D3D11DDI_THREADING_CAPS{0});
    case D3D11DDICAPS_SHADER: {
        UINT bits=0;
        if(caps.doubles.DoublePrecisionFloatShaderOps)bits|=D3D11DDICAPS_SHADER_DOUBLES;
        if(caps.compute.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x)
            bits|=D3D11DDICAPS_SHADER_COMPUTE_PLUS_RAW_AND_STRUCTURED_BUFFERS_IN_SHADER_4_X;
        // The runtime derives OPTIONS2 stencil ref, typed UAV loads and ROVs from these bits.
        if(o.PSSpecifiedStencilRefSupported)bits|=D3D11DDICAPS_SHADER_SPECIFIED_STENCIL_REF;
        if(o.TypedUAVLoadAdditionalFormats)bits|=D3D11DDICAPS_SHADER_TYPED_UAV_LOAD_ADDITIONAL_FORMATS;
        if(o.ROVsSupported)bits|=D3D11DDICAPS_SHADER_ROVS;
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
    default:break;
    }
    // WDDM 1.3/2.0 queries: answered only by an FL12 adapter, whose runtime uses the WDDM 2.0 DDI.
    if (fl12) switch(args.Type) {
    case D3DWDDM1_3DDICAPS_D3D11_OPTIONS1: {
        // Flags are cumulative: tier n supports every lower tier.
        UINT tiers=0;
        if(o.TiledResourcesTier>=D3D11_TILED_RESOURCES_TIER_1)tiers|=D3DWDDM1_3DDI_TILED_RESOURCES_TIER_1_SUPPORTED;
        if(o.TiledResourcesTier>=D3D11_TILED_RESOURCES_TIER_2)tiers|=D3DWDDM1_3DDI_TILED_RESOURCES_TIER_2_SUPPORTED;
        if(o.TiledResourcesTier>=D3D11_TILED_RESOURCES_TIER_3)tiers|=D3DWDDM2_0DDI_TILED_RESOURCES_TIER_3_SUPPORTED;
        return write_adapter_cap(args,D3DWDDM1_3DDI_D3D11_OPTIONS_DATA1{tiers});
    }
    case D3DWDDM1_3DDICAPS_MARKER:
        return write_adapter_cap(args,UINT(D3DWDDM1_3DDI_MARKER_TYPE_NONE));
    case D3DWDDM2_0DDICAPS_D3D11_OPTIONS2: {
        static_assert(UINT(D3D11_CONSERVATIVE_RASTERIZATION_TIER_3)==UINT(D3DWDDM2_0DDI_CONSERVATIVE_RASTERIZATION_TIER_3));
        const auto tier=static_cast<D3DWDDM2_0DDI_CONSERVATIVE_RASTERIZATION_TIER>(o.ConservativeRasterizationTier);
        // The legacy layout carries an ASTC profile word after the tier: no ASTC.
        if (args.DataSize==sizeof(D3DWDDM2_0DDI_D3D11_OPTIONS2_DATA_LEGACY_ASTC))
            return write_adapter_cap(args,D3DWDDM2_0DDI_D3D11_OPTIONS2_DATA_LEGACY_ASTC{tier,0});
        return write_adapter_cap(args,D3DWDDM2_0DDI_D3D11_OPTIONS2_DATA{tier});
    }
    case D3DWDDM2_0DDICAPS_MEMORY_ARCHITECTURE:
        // DXVK reports no UMA; resources are not CPU-addressable in place.
        return write_adapter_cap(args,D3DWDDM2_0DDI_MEMORY_ARCHITECTURE_CAPS{FALSE,FALSE});
    case D3DWDDM2_0DDICAPS_TEXTURE_LAYOUT:
        // No device-dependent layouts and no 64 KB standard swizzle (StandardSwizzle FALSE).
        return write_adapter_cap(args,D3DWDDM2_0DDI_TEXTURE_LAYOUT_CAPS{0,0,FALSE});
    case D3DWDDM2_0DDICAPS_D3D11_OPTIONS3:
        return write_adapter_cap(args,D3DWDDM2_0DDI_D3D11_OPTIONS3_DATA{
            caps.options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer});
    case D3DWDDM2_0DDICAPS_GPUVA_CAPS:
        return write_adapter_cap(args,D3DWDDM2_0DDI_GPUVA_CAPS_DATA{adapter_gpuva_bits_per_resource});
    // BD-099: the two queries of the WDDM 2.2 interface. Both say "nothing of this is offered", which is what
    // the engine and this shell do. The runtime asks them before it picks an interface, so the answer does not
    // depend on the interface it picks.
    case D3DWDDM2_2DDICAPS_SHADERCACHE:
        // The engine keeps its own pipeline cache (DXVK). No session of the runtime's cache is asked for, so the
        // four shader-cache entries of the WDDM 2.2 table are never called (ddi-wddm22.cpp).
        return write_adapter_cap(args,D3DWDDM2_2DDICAPS_SHADERCACHE_DATA{FALSE});
    case D3DWDDM2_2DDICAPS_TEXTURE_LAYOUT:
        // As the deprecated WDDM 2.0 answer above: no device-dependent layout or swizzle, no 64 KB standard
        // swizzle, and no indexable swizzle patterns. The counts are zero, so no pattern is ever asked for.
        return write_adapter_cap(args,D3DWDDM2_2DDI_TEXTURE_LAYOUT_CAPS{0,0,FALSE,FALSE});
    default:break;
    }
    // No invented layout or capability for an unknown query. Say once which one it was.
    static std::atomic_bool logged{false};
    if(!logged.exchange(true)) {
        char text[96];std::snprintf(text,sizeof(text),"M14: GetCaps type %u not answered\n",unsigned(args.Type));
        OutputDebugStringA(text);
    }
    return E_NOTIMPL;
}
}
