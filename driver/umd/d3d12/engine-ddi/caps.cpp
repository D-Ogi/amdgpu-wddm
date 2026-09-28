// SPDX-License-Identifier: MIT
// engine-ddi: the adapter caps path of GetCaps. query_adapter_caps asks the engine once (QueryAdapterCaps, engine
// ABI 1.2, V11) and keeps its answers; build_caps answers each GetCaps from them. Where each value comes from is
// the table in INTEGRATION.md ("GetCaps"): a D3D12_FEATURE_* answer of the engine wherever a 1:1 field exists,
// otherwise a documented constant. Layout references are WDK 10.0.26100 um\d3d12umddi.h ("H:line").
#include "internal.h"
#include <cstring>
#include <iterator>

namespace engine_ddi {

// ---- Payload layouts ------------------------------------------------------------------------------------------------
static_assert(sizeof(D3D12DDIARG_GETCAPS) == 32, "H:2611-2617");
static_assert(sizeof(D3D12DDI_3DPIPELINESUPPORT1_DATA_0081) == 8, "1074: H:10415-10420");
static_assert(sizeof(D3D12DDI_3DPIPELINELEVEL) == 4, "1007: the level itself, H:2922-2933");
static_assert(sizeof(D3D12DDI_D3D12_SHADER_MODELS_DATA_0011) == 16, "1012: H:3502-3507");
static_assert(sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0089) == 124, "1006: H:11078-11112");
static_assert(sizeof(D3D12DDI_SHADER_CAPS_0084) == 64, "1004: H:10515-10534");
static_assert(sizeof(D3D12DDI_ARCHITECTURE_INFO_DATA) == 4, "1005: H:2916-2920");
static_assert(sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041) == 20, "1002: H:6806-6814");
static_assert(sizeof(D3D12DDI_GPUVA_CAPS_0004) == 4, "1009: H:250-257");
static_assert(sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0026) == 20, "1060: H:5525-5536");
static_assert(sizeof(D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023) == 4, "1062: H:5140-5143");
static_assert(sizeof(D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050) == 4, "1067: H:7004-7008");
static_assert(sizeof(D3D12DDI_OPTIONS_DATA_0090) == 4, "1077: H:11127-11131");
static_assert(sizeof(D3D12DDI_OPTIONS_DATA_0091) == 16, "1078: H:11143-11150");

// ---- Enums passed through by value ------------------------------------------------------------------------------------
static_assert(D3D12DDI_RESOURCE_BINDING_TIER_1 == static_cast<int>(D3D12_RESOURCE_BINDING_TIER_1) &&
              D3D12DDI_RESOURCE_BINDING_TIER_3 == static_cast<int>(D3D12_RESOURCE_BINDING_TIER_3), "binding tier");
static_assert(D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED ==
                  static_cast<int>(D3D12_CONSERVATIVE_RASTERIZATION_TIER_NOT_SUPPORTED) &&
              D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER_3 == static_cast<int>(D3D12_CONSERVATIVE_RASTERIZATION_TIER_3),
              "conservative rasterization tier");
static_assert(D3D12DDI_TILED_RESOURCES_TIER_NOT_SUPPORTED == static_cast<int>(D3D12_TILED_RESOURCES_TIER_NOT_SUPPORTED) &&
              D3D12DDI_TILED_RESOURCES_TIER_3 == static_cast<int>(D3D12_TILED_RESOURCES_TIER_3), "tiled resources tier");
static_assert(D3D12DDI_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED == static_cast<int>(D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED) &&
              D3D12DDI_CROSS_NODE_SHARING_TIER_1_EMULATED == static_cast<int>(D3D12_CROSS_NODE_SHARING_TIER_1_EMULATED) &&
              D3D12DDI_CROSS_NODE_SHARING_TIER_0041_3 == static_cast<int>(D3D12_CROSS_NODE_SHARING_TIER_3),
              "cross node sharing tier");
static_assert(D3D12DDI_RESOURCE_HEAP_TIER_1 == static_cast<int>(D3D12_RESOURCE_HEAP_TIER_1) &&
              D3D12DDI_RESOURCE_HEAP_TIER_2 == static_cast<int>(D3D12_RESOURCE_HEAP_TIER_2), "resource heap tier");
static_assert(D3D12DDI_SHADER_MIN_PRECISION_10_BIT == static_cast<int>(D3D12_SHADER_MIN_PRECISION_SUPPORT_10_BIT) &&
              D3D12DDI_SHADER_MIN_PRECISION_16_BIT == static_cast<int>(D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT),
              "min precision");

class AdapterCaps {
public:
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels;
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model;
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture;
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gpu_va;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3;
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5;
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9;
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12;
    D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13;
    D3D12_FEATURE_DATA_SERIALIZATION serialization;
};

namespace {
constexpr uint32_t kAbi12 = (1u << 16) | 2u;
constexpr uint32_t kDdiVersion = 92;                     // D3D12DDI_BUILD_VERSION_0092, H:11156

constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                         D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};

// Shader models whose RELEASE value has a suffix at or below 0092 (H:3478-3500): 6_7_RELEASE is 0093, so a 0092
// driver lists at most 6_6. The engine is asked with 6_6 as the highest model of interest.
struct ShaderModel { D3D_SHADER_MODEL api; D3D12DDI_SHADER_MODEL ddi; };
constexpr ShaderModel kShaderModels[] = {
    {D3D_SHADER_MODEL_5_1, D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011}, {D3D_SHADER_MODEL_6_0, D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011},
    {D3D_SHADER_MODEL_6_1, D3D12DDI_SHADER_MODEL_6_1_RELEASE_0033}, {D3D_SHADER_MODEL_6_2, D3D12DDI_SHADER_MODEL_6_2_RELEASE_0042},
    {D3D_SHADER_MODEL_6_3, D3D12DDI_SHADER_MODEL_6_3_RELEASE_0054}, {D3D_SHADER_MODEL_6_4, D3D12DDI_SHADER_MODEL_6_4_RELEASE_0062},
    {D3D_SHADER_MODEL_6_5, D3D12DDI_SHADER_MODEL_6_5_RELEASE_0071}, {D3D_SHADER_MODEL_6_6, D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082},
};

// Feature level 12_2 needs raytracing tier 1.1, mesh shaders, VRS tier 2 and sampler feedback (DirectX-Specs
// D3D12_FeatureLevel12_2.md). build_caps reports none of them while their DDI slots are fail-safes (SLOTS.md), so
// its ceiling is 12_1; 3DPIPELINESUPPORT has the same ceiling by contract (H:10368-10373).
constexpr D3D12DDI_3DPIPELINELEVEL kLevelCeiling = D3D12DDI_3DPIPELINELEVEL_12_1;

D3D12DDI_3DPIPELINELEVEL pipeline_level(D3D_FEATURE_LEVEL level) noexcept {
    switch (level) {
    case D3D_FEATURE_LEVEL_11_0: return D3D12DDI_3DPIPELINELEVEL_11_0;
    case D3D_FEATURE_LEVEL_11_1: return D3D12DDI_3DPIPELINELEVEL_11_1;
    case D3D_FEATURE_LEVEL_12_0: return D3D12DDI_3DPIPELINELEVEL_12_0;
    case D3D_FEATURE_LEVEL_12_1: return D3D12DDI_3DPIPELINELEVEL_12_1;
    case D3D_FEATURE_LEVEL_12_2: return D3D12DDI_3DPIPELINELEVEL_12_2;
    default: return static_cast<D3D12DDI_3DPIPELINELEVEL>(0);
    }
}

D3D12DDI_3DPIPELINELEVEL lower(D3D12DDI_3DPIPELINELEVEL a, D3D12DDI_3DPIPELINELEVEL b) noexcept { return a < b ? a : b; }

D3D12DDI_3DPIPELINELEVEL driver_level(const AdapterCaps& c) noexcept {
    return lower(pipeline_level(c.levels.MaxSupportedFeatureLevel), kLevelCeiling);
}

// Node-indexed types: *pInfo is the node index (H:152-155, H:250-253). A null pInfo is taken as node 0
// (INFERENCE); this adapter has one node.
bool node_zero(const D3D12DDIARG_GETCAPS& r) noexcept { return !r.pInfo || *static_cast<const UINT*>(r.pInfo) == 0; }

const char* feature_name(D3D12_FEATURE f) noexcept {
    switch (f) {
    case D3D12_FEATURE_FEATURE_LEVELS: return "FEATURE_LEVELS";
    case D3D12_FEATURE_SHADER_MODEL: return "SHADER_MODEL";
    case D3D12_FEATURE_ARCHITECTURE1: return "ARCHITECTURE1";
    case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: return "GPU_VIRTUAL_ADDRESS_SUPPORT";
    case D3D12_FEATURE_D3D12_OPTIONS: return "D3D12_OPTIONS";
    case D3D12_FEATURE_D3D12_OPTIONS1: return "D3D12_OPTIONS1";
    case D3D12_FEATURE_D3D12_OPTIONS3: return "D3D12_OPTIONS3";
    case D3D12_FEATURE_D3D12_OPTIONS4: return "D3D12_OPTIONS4";
    case D3D12_FEATURE_D3D12_OPTIONS5: return "D3D12_OPTIONS5";
    case D3D12_FEATURE_D3D12_OPTIONS9: return "D3D12_OPTIONS9";
    case D3D12_FEATURE_D3D12_OPTIONS11: return "D3D12_OPTIONS11";
    case D3D12_FEATURE_D3D12_OPTIONS12: return "D3D12_OPTIONS12";
    case D3D12_FEATURE_D3D12_OPTIONS13: return "D3D12_OPTIONS13";
    case D3D12_FEATURE_SERIALIZATION: return "SERIALIZATION";
    default: return "?";
    }
}

void fill_options(const AdapterCaps& c, D3D12DDI_D3D12_OPTIONS_DATA_0089& o) noexcept {
    o = D3D12DDI_D3D12_OPTIONS_DATA_0089{};
    o.ResourceBindingTier = static_cast<D3D12DDI_RESOURCE_BINDING_TIER>(c.options.ResourceBindingTier);
    o.ConservativeRasterizationTier =
        static_cast<D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER>(c.options.ConservativeRasterizationTier);
    // D3D12_TILED_RESOURCES_TIER_4 has no DDI value at 0092 (H:11083 names the tier enum, which ends at 3).
    o.TiledResourcesTier = static_cast<D3D12DDI_TILED_RESOURCES_TIER>(
        c.options.TiledResourcesTier > D3D12_TILED_RESOURCES_TIER_3 ? D3D12_TILED_RESOURCES_TIER_3
                                                                    : c.options.TiledResourcesTier);
    o.CrossNodeSharingTier = static_cast<D3D12DDI_CROSS_NODE_SHARING_TIER>(c.options.CrossNodeSharingTier);
    o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation =
        c.options.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation;
    o.OutputMergerLogicOp = c.options.OutputMergerLogicOp;
    o.ResourceHeapTier = static_cast<D3D12DDI_RESOURCE_HEAP_TIER>(c.options.ResourceHeapTier);
    o.CopyQueueTimestampQueriesSupported = c.options3.CopyQueueTimestampQueriesSupported;
    o.BarycentricsSupported = c.options3.BarycentricsSupported;
    o.ReservedBufferPlacementSupported = c.options4.MSAA64KBAlignedTextureSupported;   // "just 64KB aligned MSAA", H:11094
    o.SRVOnlyTiledResourceTier3 = c.options5.SRVOnlyTiledResourceTier3;
    // Documented constants (INTEGRATION.md, "GetCaps"). Gated while the DDI slots behind them are fail-safes:
    //   DepthBoundsTestSupported (pfnOMSetDepthBounds), ProgrammableSamplePositionsTier (pfnSetSamplePositions),
    //   WriteBufferImmediateQueueFlags (pfnWriteBufferImmediate), ViewInstancingTier (pfnSetViewInstanceMask and
    //   graphics pipelines), RaytracingTier (state objects, pfnDispatchRays), the VRS fields (pfnRSSetShadingRate,
    //   pfnRSSetShadingRateImage), MeshShaderTier and the mesh fields (pfnDispatchMesh), SamplerFeedbackTier
    //   (pfnCreateSamplerFeedbackUnorderedAccessView), EnhancedBarriersSupported (pfnBarrier).
    //   RenderPassTier 0: engine-ddi fills no render pass table; the runtime then emulates render passes.
    //   BackgroundProcessingSupported: pfnSetBackgroundProcessingMode is the shell's slot.
    //   DriverManagedShaderCachePresent: engine-ddi keeps no driver-managed shader cache.
    //   Deterministic64KBUndefinedSwizzle: no engine answer exists; not claimed.
    o.RenderPassTier = D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED;
    o.RaytracingTier = D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED;
    o.VariableShadingRateTier = D3D12DDI_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
    o.MeshShaderTier = D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED;
    o.SamplerFeedbackTier = D3D12DDI_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
    o.ProgrammableSamplePositionsTier = D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
    o.ViewInstancingTier = D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
    o.WriteBufferImmediateQueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_NONE;
}

void fill_shader(const AdapterCaps& c, D3D12DDI_SHADER_CAPS_0084& s) noexcept {
    s = D3D12DDI_SHADER_CAPS_0084{};
    s.MinPrecision = static_cast<D3D12DDI_SHADER_MIN_PRECISION>(c.options.MinPrecisionSupport);
    s.DoubleOps = c.options.DoublePrecisionFloatShaderOps;
    s.ShaderSpecifiedStencilRef = c.options.PSSpecifiedStencilRefSupported;
    s.TypedUAVLoadAdditionalFormats = c.options.TypedUAVLoadAdditionalFormats;
    s.ROVs = c.options.ROVsSupported;
    s.WaveOps = c.options1.WaveOps;
    s.WaveLaneCountMin = c.options1.WaveLaneCountMin;
    s.WaveLaneCountMax = c.options1.WaveLaneCountMax;
    s.TotalLaneCount = c.options1.TotalLaneCount;
    s.Int64Ops = c.options1.Int64ShaderOps;
    s.Native16BitOps = c.options4.Native16BitShaderOpsSupported;
    s.AtomicInt64OnTypedResource = c.options9.AtomicInt64OnTypedResourceSupported;
    s.AtomicInt64OnGroupShared = c.options9.AtomicInt64OnGroupSharedSupported;
    s.AtomicInt64OnDescriptorHeapResource = c.options11.AtomicInt64OnDescriptorHeapResourceSupported;
    // Constants: mesh and amplification shaders are gated (see fill_options); the only wave MMA tier at 0092 is
    // experimental (H:10436-10439).
    s.DerivativesInMeshAndAmplificationShaders = FALSE;
    s.WaveMMATier = D3D12DDI_WAVE_MMA_TIER_NOT_SUPPORTED;
}

template <class T> T* payload(const D3D12DDIARG_GETCAPS& r) noexcept {
    if (r.DataSize == sizeof(T) && r.pData) return static_cast<T*>(r.pData);
    log_line("GetCaps type %u: DataSize %u, pData %s; this revision answers only %zu bytes", static_cast<unsigned>(r.Type),
             r.DataSize, r.pData ? "set" : "null", sizeof(T));
    return nullptr;
}
} // namespace

HRESULT query_adapter_caps(const BC250_VKD3D_ENGINE_FUNCS* funcs, const BC250_VKD3D_DEVICE_CREATE_INFO* info,
                           AdapterCaps** out) noexcept {
    if (!out) return E_INVALIDARG;
    *out = nullptr;
    if (!funcs || !info || funcs->Size < sizeof(BC250_VKD3D_ENGINE_FUNCS) || funcs->AbiVersion < kAbi12 ||
        !funcs->QueryAdapterCaps)
        return E_INVALIDARG;
    auto* c = make_new<AdapterCaps>();
    if (!c) return E_OUTOFMEMORY;
    c->levels.NumFeatureLevels = static_cast<UINT>(std::size(kLevels));
    c->levels.pFeatureLevelsRequested = kLevels;
    c->shader_model.HighestShaderModel = D3D_SHADER_MODEL_6_6;
    struct Entry { D3D12_FEATURE feature; void* data; UINT size; bool required; };
    const Entry entries[] = {
        {D3D12_FEATURE_FEATURE_LEVELS, &c->levels, sizeof(c->levels), true},
        {D3D12_FEATURE_SHADER_MODEL, &c->shader_model, sizeof(c->shader_model), true},
        {D3D12_FEATURE_ARCHITECTURE1, &c->architecture, sizeof(c->architecture), true},
        {D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &c->gpu_va, sizeof(c->gpu_va), true},
        {D3D12_FEATURE_D3D12_OPTIONS, &c->options, sizeof(c->options), true},
        {D3D12_FEATURE_D3D12_OPTIONS1, &c->options1, sizeof(c->options1), true},
        {D3D12_FEATURE_D3D12_OPTIONS3, &c->options3, sizeof(c->options3), false},
        {D3D12_FEATURE_D3D12_OPTIONS4, &c->options4, sizeof(c->options4), false},
        {D3D12_FEATURE_D3D12_OPTIONS5, &c->options5, sizeof(c->options5), false},
        {D3D12_FEATURE_D3D12_OPTIONS9, &c->options9, sizeof(c->options9), false},
        {D3D12_FEATURE_D3D12_OPTIONS11, &c->options11, sizeof(c->options11), false},
        {D3D12_FEATURE_D3D12_OPTIONS12, &c->options12, sizeof(c->options12), false},
        {D3D12_FEATURE_D3D12_OPTIONS13, &c->options13, sizeof(c->options13), false},
        {D3D12_FEATURE_SERIALIZATION, &c->serialization, sizeof(c->serialization), false},
    };
    constexpr size_t kCount = std::size(entries);
    BC250_VKD3D_FEATURE_QUERY queries[kCount]{};
    for (size_t i = 0; i < kCount; ++i)
        queries[i] = {static_cast<UINT32>(entries[i].feature), entries[i].size, entries[i].data, E_FAIL, 0};
    HRESULT hr = funcs->QueryAdapterCaps(info, static_cast<UINT32>(kCount), queries);
    if (FAILED(hr)) {
        log_line("QueryAdapterCaps failed: %08lx", static_cast<unsigned long>(hr));
        delete c;
        return hr;
    }
    for (size_t i = 0; i < kCount; ++i) {
        if (SUCCEEDED(queries[i].Result)) continue;
        log_line("QueryAdapterCaps: feature %u (%s) answered %08lx%s", static_cast<unsigned>(entries[i].feature),
                 feature_name(entries[i].feature), static_cast<unsigned long>(queries[i].Result),
                 entries[i].required ? "" : "; the caps fed by it report no support");
        if (entries[i].required) {
            hr = queries[i].Result;
            delete c;
            return hr;
        }
        std::memset(entries[i].data, 0, entries[i].size);   // the engine leaves *pData alone on failure (V11)
    }
    // The level list and model ceiling were engine-ddi's input; anything else is not an answer to them.
    if (!pipeline_level(c->levels.MaxSupportedFeatureLevel) || c->shader_model.HighestShaderModel < D3D_SHADER_MODEL_5_1 ||
        c->shader_model.HighestShaderModel > D3D_SHADER_MODEL_6_6) {
        log_line("QueryAdapterCaps: feature level %x or shader model %x outside what was asked",
                 static_cast<unsigned>(c->levels.MaxSupportedFeatureLevel),
                 static_cast<unsigned>(c->shader_model.HighestShaderModel));
        delete c;
        return E_UNEXPECTED;
    }
    c->levels.pFeatureLevelsRequested = nullptr;          // not kept past the call
    *out = c;
    return S_OK;
}

void free_adapter_caps(AdapterCaps* caps) noexcept { delete caps; }

HRESULT build_caps(const AdapterCaps* caps, uint32_t ddi_version, const D3D12DDIARG_GETCAPS* request) noexcept {
    if (!caps || !request || ddi_version != kDdiVersion) return E_INVALIDARG;
    const AdapterCaps& c = *caps;
    const D3D12DDIARG_GETCAPS& r = *request;
    switch (r.Type) {
    case D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1: {
        auto* d = payload<D3D12DDI_3DPIPELINESUPPORT1_DATA_0081>(r);
        if (!d) return E_INVALIDARG;
        // "the highest feature level it supports that does not exceed what the runtime understands" (H:10377-10378).
        d->MaximumDriverSupportedFeatureLevel = lower(driver_level(c), d->HighestRuntimeSupportedFeatureLevel);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_3DPIPELINESUPPORT: {
        auto* d = payload<D3D12DDI_3DPIPELINELEVEL>(r);
        if (!d) return E_INVALIDARG;
        *d = lower(driver_level(c), D3D12DDI_3DPIPELINELEVEL_12_1);   // at most 12_1 (H:10373)
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0011_SHADER_MODELS: {
        auto* d = payload<D3D12DDI_D3D12_SHADER_MODELS_DATA_0011>(r);
        if (!d || !d->pNumShaderModelsSupported) return E_INVALIDARG;
        UINT n = 0;
        while (n < std::size(kShaderModels) && kShaderModels[n].api <= c.shader_model.HighestShaderModel) ++n;
        if (d->pShaderModelsSupported) {
            // _Field_size_opt_(*pNumShaderModelsSupported) (H:3506): the array holds that many entries.
            if (*d->pNumShaderModelsSupported < n) {
                log_line("GetCaps SHADER_MODELS: room for %u models, %u supported", *d->pNumShaderModelsSupported, n);
                return E_INVALIDARG;
            }
            for (UINT i = 0; i < n; ++i) d->pShaderModelsSupported[i] = kShaderModels[i].ddi;
        }
        *d->pNumShaderModelsSupported = n;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_D3D12_OPTIONS: {
        auto* d = payload<D3D12DDI_D3D12_OPTIONS_DATA_0089>(r);
        if (!d) return E_INVALIDARG;
        fill_options(c, *d);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_SHADER: {
        auto* d = payload<D3D12DDI_SHADER_CAPS_0084>(r);
        if (!d) return E_INVALIDARG;
        fill_shader(c, *d);
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_ARCHITECTURE_INFO: {
        auto* d = payload<D3D12DDI_ARCHITECTURE_INFO_DATA>(r);
        if (!d) return E_INVALIDARG;
        d->TileBasedDeferredRenderer = c.architecture.TileBasedRenderer;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE: {
        auto* d = payload<D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041>(r);
        if (!d || !node_zero(r)) return E_INVALIDARG;
        D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041 m{};
        m.UMA = c.architecture.UMA;
        m.CacheCoherent = c.architecture.CacheCoherentUMA;
        m.IOCoherent = FALSE;                               // no engine answer; not claimed
        m.HeapSerializationTier = c.serialization.HeapSerializationTier >= D3D12_HEAP_SERIALIZATION_TIER_10
                                      ? D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1
                                      : D3D12DDI_HEAP_SERIALIZATION_TIER_0041_0;
        m.ResourceSerializationTier = D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_0;   // no engine answer
        *d = m;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_GPUVA_CAPS: {
        auto* d = payload<D3D12DDI_GPUVA_CAPS_0004>(r);
        if (!d || !node_zero(r)) return E_INVALIDARG;
        d->MaxGPUVirtualAddressBitsPerResource = c.gpu_va.MaxGPUVirtualAddressBitsPerResource;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT: {
        // pInfo NULL: the caps; pInfo set: a swizzle pattern index, of which this driver has none (H:5525-5528).
        auto* d = payload<D3D12DDI_TEXTURE_LAYOUT_CAPS_0026>(r);
        if (!d || r.pInfo) return E_INVALIDARG;
        D3D12DDI_TEXTURE_LAYOUT_CAPS_0026 t{};
        t.DeviceDependentLayoutCount = 0;
        t.DeviceDependentSwizzleCount = 0;
        t.Supports64KStandardSwizzle = c.options.StandardSwizzle64KBSupported;
        t.SupportsRowMajorTexture = FALSE;                  // no 1:1 engine answer; not claimed
        t.IndexableSwizzlePatterns = FALSE;
        *d = t;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0023_UMD_BASED_COMMAND_QUEUE_PRIORITY: {
        auto* d = payload<D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023>(r);
        if (!d) return E_INVALIDARG;
        d->SupportedQueueFlagsForGlobalRealtimeQueues = D3D12DDI_COMMAND_QUEUE_FLAG_NONE;   // no realtime queues
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0050_HARDWARE_SCHEDULING_CAPS: {
        auto* d = payload<D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050>(r);
        if (!d) return E_INVALIDARG;
        d->ComputeQueuesPer3DQueue = 0;                     // "0 means don't use scheduling groups" (H:7007)
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0090: {
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0090>(r);
        if (!d) return E_INVALIDARG;
        d->RelaxedFormatCastingSupported = c.options12.RelaxedFormatCastingSupported;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0091: {
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0091>(r);
        if (!d) return E_INVALIDARG;
        d->UnrestrictedBufferTextureCopyPitchSupported = c.options13.UnrestrictedBufferTextureCopyPitchSupported;
        d->UnrestrictedVertexElementAlignmentSupported = c.options13.UnrestrictedVertexElementAlignmentSupported;
        d->InvertedViewportHeightFlipsYSupported = c.options13.InvertedViewportHeightFlipsYSupported;
        d->InvertedViewportDepthFlipsZSupported = c.options13.InvertedViewportDepthFlipsZSupported;
        return S_OK;
    }
    default:
        log_line("GetCaps type %u (DataSize %u, pInfo %s): not answered in this revision", static_cast<unsigned>(r.Type),
                 r.DataSize, r.pInfo ? "set" : "null");
        return E_NOTIMPL;
    }
}

} // namespace engine_ddi
