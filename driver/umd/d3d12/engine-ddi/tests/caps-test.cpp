// SPDX-License-Identifier: MIT
// GetCaps host test, linked against the native engine-ddi.lib (no harness macro), the way the shell links it.
//
//   caps-test.exe                         stub engine: QueryAdapterCaps answers from the fixed tables below
//   caps-test.exe --engine <dll> [--adapter <substring>]
//                                         the real engine DLL (ABI 1.2) on this PC's GPU, INLINE create info
//
// The stub's answers are test inputs, not measurements of any GPU. The test checks the calls the runtime made in
// M768 with their exact sizes (1074 with 8 bytes, 1007 with 4) and the mapping of every other type build_caps
// answers (INTEGRATION.md, "GetCaps").
#include "engine-ddi.h"
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
int failures = 0;
void check(bool ok, const char* format, ...) {
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", text);
    failures += ok ? 0 : 1;
}

constexpr uint32_t kDdi = D3D12DDI_BUILD_VERSION_0092;
constexpr uint32_t kAbi12 = (1u << 16) | 2u;

template <class T> HRESULT get(const engine_ddi::AdapterCaps* caps, D3D12DDICAPS_TYPE type, T& data,
                               void* info = nullptr, UINT size = sizeof(T)) {
    D3D12DDIARG_GETCAPS request{type, info, &data, size};
    return engine_ddi::build_caps(caps, kDdi, &request);
}

bool untouched(const void* p, size_t n) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i)
        if (b[i] != 0xEE) return false;
    return true;
}

// ---- Stub engine ------------------------------------------------------------------------------------------------------
struct Answers {
    D3D_FEATURE_LEVEL max_level;
    D3D_SHADER_MODEL max_model;
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
    int unsupported;                            // a D3D12_FEATURE answered DXGI_ERROR_UNSUPPORTED; -1 for none
};

// An FL12_0 engine that also claims what engine-ddi must clamp (raytracing 1.1, enhanced barriers, mesh
// derivatives, VRS). Values are arbitrary but valid.
Answers fl12_0() {
    Answers a{};
    a.unsupported = -1;
    a.max_level = D3D_FEATURE_LEVEL_12_0;
    a.max_model = D3D_SHADER_MODEL_6_6;
    a.architecture = {0, FALSE, TRUE, FALSE, TRUE};
    a.gpu_va = {40, 44};
    D3D12_FEATURE_DATA_D3D12_OPTIONS& o = a.options;
    o.DoublePrecisionFloatShaderOps = TRUE;
    o.OutputMergerLogicOp = TRUE;
    o.MinPrecisionSupport = D3D12_SHADER_MIN_PRECISION_SUPPORT_16_BIT;
    o.TiledResourcesTier = D3D12_TILED_RESOURCES_TIER_2;
    o.ResourceBindingTier = D3D12_RESOURCE_BINDING_TIER_3;
    o.PSSpecifiedStencilRefSupported = TRUE;
    o.TypedUAVLoadAdditionalFormats = TRUE;
    o.ROVsSupported = FALSE;
    o.ConservativeRasterizationTier = D3D12_CONSERVATIVE_RASTERIZATION_TIER_1;
    o.MaxGPUVirtualAddressBitsPerResource = 40;
    o.StandardSwizzle64KBSupported = FALSE;
    o.CrossNodeSharingTier = D3D12_CROSS_NODE_SHARING_TIER_NOT_SUPPORTED;
    o.CrossAdapterRowMajorTextureSupported = FALSE;
    o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation = TRUE;
    o.ResourceHeapTier = D3D12_RESOURCE_HEAP_TIER_2;
    a.options1 = {TRUE, 32, 64, 2560, TRUE, TRUE};
    a.options3.CopyQueueTimestampQueriesSupported = TRUE;
    a.options3.BarycentricsSupported = FALSE;
    a.options4.MSAA64KBAlignedTextureSupported = TRUE;
    a.options4.Native16BitShaderOpsSupported = TRUE;
    a.options5.SRVOnlyTiledResourceTier3 = FALSE;
    a.options5.RenderPassesTier = D3D12_RENDER_PASS_TIER_0;
    a.options5.RaytracingTier = D3D12_RAYTRACING_TIER_1_1;
    a.options9.AtomicInt64OnTypedResourceSupported = TRUE;
    a.options9.AtomicInt64OnGroupSharedSupported = TRUE;
    a.options9.DerivativesInMeshAndAmplificationShadersSupported = TRUE;
    a.options11.AtomicInt64OnDescriptorHeapResourceSupported = TRUE;
    a.options12.EnhancedBarriersSupported = TRUE;
    a.options12.RelaxedFormatCastingSupported = TRUE;
    a.options13 = {TRUE, TRUE, FALSE, TRUE, TRUE, TRUE};
    a.serialization.HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_10;
    return a;
}

const Answers* g_answers = nullptr;
const BC250_VKD3D_DEVICE_CREATE_INFO* g_info_seen = nullptr;
int g_calls = 0;
UINT32 g_queries = 0;

template <class T> void answer(BC250_VKD3D_FEATURE_QUERY& q, const T& value) {
    if (q.DataSize != sizeof(T)) {
        q.Result = E_INVALIDARG;
        return;
    }
    std::memcpy(q.pData, &value, sizeof(T));
    q.Result = S_OK;
}

HRESULT APIENTRY stub_query(const BC250_VKD3D_DEVICE_CREATE_INFO* info, UINT32 count, BC250_VKD3D_FEATURE_QUERY* queries) {
    ++g_calls;
    g_info_seen = info;
    g_queries = count;
    const Answers& a = *g_answers;
    for (UINT32 i = 0; i < count; ++i) {
        BC250_VKD3D_FEATURE_QUERY& q = queries[i];
        const auto feature = static_cast<D3D12_FEATURE>(q.Feature);
        q.Result = DXGI_ERROR_UNSUPPORTED;
        if (q.Reserved) {
            q.Result = E_INVALIDARG;
            continue;
        }
        if (static_cast<int>(feature) == a.unsupported) continue;
        switch (feature) {
        case D3D12_FEATURE_FEATURE_LEVELS: {
            if (q.DataSize != sizeof(D3D12_FEATURE_DATA_FEATURE_LEVELS)) { q.Result = E_INVALIDARG; break; }
            auto* d = static_cast<D3D12_FEATURE_DATA_FEATURE_LEVELS*>(q.pData);
            D3D_FEATURE_LEVEL best = static_cast<D3D_FEATURE_LEVEL>(0);
            for (UINT l = 0; l < d->NumFeatureLevels; ++l)
                if (d->pFeatureLevelsRequested[l] <= a.max_level && d->pFeatureLevelsRequested[l] > best)
                    best = d->pFeatureLevelsRequested[l];
            d->MaxSupportedFeatureLevel = best;
            q.Result = S_OK;
            break;
        }
        case D3D12_FEATURE_SHADER_MODEL: {
            if (q.DataSize != sizeof(D3D12_FEATURE_DATA_SHADER_MODEL)) { q.Result = E_INVALIDARG; break; }
            auto* d = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(q.pData);
            if (d->HighestShaderModel > a.max_model) d->HighestShaderModel = a.max_model;
            q.Result = S_OK;
            break;
        }
        case D3D12_FEATURE_ARCHITECTURE1: answer(q, a.architecture); break;
        case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: answer(q, a.gpu_va); break;
        case D3D12_FEATURE_D3D12_OPTIONS: answer(q, a.options); break;
        case D3D12_FEATURE_D3D12_OPTIONS1: answer(q, a.options1); break;
        case D3D12_FEATURE_D3D12_OPTIONS3: answer(q, a.options3); break;
        case D3D12_FEATURE_D3D12_OPTIONS4: answer(q, a.options4); break;
        case D3D12_FEATURE_D3D12_OPTIONS5: answer(q, a.options5); break;
        case D3D12_FEATURE_D3D12_OPTIONS9: answer(q, a.options9); break;
        case D3D12_FEATURE_D3D12_OPTIONS11: answer(q, a.options11); break;
        case D3D12_FEATURE_D3D12_OPTIONS12: answer(q, a.options12); break;
        case D3D12_FEATURE_D3D12_OPTIONS13: answer(q, a.options13); break;
        case D3D12_FEATURE_SERIALIZATION: answer(q, a.serialization); break;
        default: break;
        }
    }
    return S_OK;
}

BC250_VKD3D_ENGINE_FUNCS stub_funcs() {
    BC250_VKD3D_ENGINE_FUNCS f{};
    f.Size = sizeof(f);
    f.AbiVersion = kAbi12;
    f.QueryAdapterCaps = stub_query;
    return f;
}

engine_ddi::AdapterCaps* query_stub(const Answers& a, const BC250_VKD3D_DEVICE_CREATE_INFO& info, const char* what) {
    g_answers = &a;
    g_calls = 0;
    const BC250_VKD3D_ENGINE_FUNCS funcs = stub_funcs();
    engine_ddi::AdapterCaps* caps = nullptr;
    const HRESULT hr = engine_ddi::query_adapter_caps(&funcs, &info, &caps);
    check(hr == S_OK && caps && g_calls == 1 && g_info_seen == &info,
          "%s: query_adapter_caps asks the engine once, with the shell's create info (hr %08lx, %d calls, %u queries)",
          what, static_cast<unsigned long>(hr), g_calls, g_queries);
    return hr == S_OK ? caps : nullptr;
}

D3D12DDI_3DPIPELINELEVEL level_1074(const engine_ddi::AdapterCaps* caps, D3D12DDI_3DPIPELINELEVEL highest, HRESULT& hr) {
    D3D12DDI_3DPIPELINESUPPORT1_DATA_0081 d;
    std::memset(&d, 0xEE, sizeof(d));
    d.HighestRuntimeSupportedFeatureLevel = highest;
    hr = get(caps, D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1, d);
    return d.HighestRuntimeSupportedFeatureLevel == highest ? d.MaximumDriverSupportedFeatureLevel
                                                             : static_cast<D3D12DDI_3DPIPELINELEVEL>(0);
}

void test_stub(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    // A 1.1 function table (the shell asked GetFuncs for 1.1): no QueryAdapterCaps, refused before any call.
    {
        BC250_VKD3D_ENGINE_FUNCS funcs = stub_funcs();
        funcs.AbiVersion = (1u << 16) | 1u;
        funcs.QueryAdapterCaps = nullptr;
        engine_ddi::AdapterCaps* caps = reinterpret_cast<engine_ddi::AdapterCaps*>(uintptr_t{1});
        check(engine_ddi::query_adapter_caps(&funcs, &info, &caps) == E_INVALIDARG && !caps,
              "an ABI 1.1 function table is refused (GetFuncs must be asked for 1.2)");
    }

    const Answers a12_0 = fl12_0();
    engine_ddi::AdapterCaps* caps = query_stub(a12_0, info, "FL12_0 engine");
    if (!caps) return;
    check(g_queries == 14, "one batch of 14 features (%u)", g_queries);

    // The two calls of M768, exact sizes.
    HRESULT hr = E_FAIL;
    D3D12DDI_3DPIPELINELEVEL level = level_1074(caps, D3D12DDI_3DPIPELINELEVEL_12_2, hr);
    check(hr == S_OK && level == D3D12DDI_3DPIPELINELEVEL_12_0,
          "1074 3DPIPELINESUPPORT1, DataSize 8, runtime highest 12_2: driver maximum 12_0 (hr %08lx, level %d)",
          static_cast<unsigned long>(hr), static_cast<int>(level));
    level = level_1074(caps, D3D12DDI_3DPIPELINELEVEL_11_1, hr);
    check(hr == S_OK && level == D3D12DDI_3DPIPELINELEVEL_11_1,
          "1074, runtime highest 11_1: the runtime's level bounds the answer (hr %08lx, level %d)",
          static_cast<unsigned long>(hr), static_cast<int>(level));
    D3D12DDI_3DPIPELINELEVEL l1007;
    std::memset(&l1007, 0xEE, sizeof(l1007));
    hr = get(caps, D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, l1007);
    check(hr == S_OK && l1007 == D3D12DDI_3DPIPELINELEVEL_12_0,
          "1007 3DPIPELINESUPPORT, DataSize 4: pData is the level itself, 12_0 (hr %08lx, level %d)",
          static_cast<unsigned long>(hr), static_cast<int>(l1007));

    // Exact sizes only: the other call's size is refused and nothing is written.
    uint8_t raw[16];
    std::memset(raw, 0xEE, sizeof(raw));
    D3D12DDIARG_GETCAPS r{D3D12DDICAPS_TYPE_0081_3DPIPELINESUPPORT1, nullptr, raw, 4};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    const bool r1074 = hr == E_INVALIDARG && untouched(raw, sizeof(raw));
    r = {D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, nullptr, raw, 8};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    check(r1074 && hr == E_INVALIDARG && untouched(raw, sizeof(raw)),
          "1074 with 4 bytes and 1007 with 8 bytes: E_INVALIDARG, nothing written");
    r = {D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM, nullptr, raw, sizeof(BOOL)};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    check(hr == E_NOTIMPL && untouched(raw, sizeof(raw)), "unanswered type 1069: E_NOTIMPL, nothing written (hr %08lx)",
          static_cast<unsigned long>(hr));
    r = {D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, nullptr, raw, 4};
    hr = engine_ddi::build_caps(caps, 91, &r);
    check(hr == E_INVALIDARG && untouched(raw, sizeof(raw)), "a ddi_version other than 92: E_INVALIDARG");

    // 1012 SHADER_MODELS: the count first, then the list.
    UINT n = 0;
    D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 sm{&n, nullptr};
    hr = get(caps, D3D12DDICAPS_TYPE_0011_SHADER_MODELS, sm);
    D3D12DDI_SHADER_MODEL models[8];
    std::memset(models, 0xEE, sizeof(models));
    UINT cap = 8;
    D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 list{&cap, models};
    const HRESULT hr_list = get(caps, D3D12DDICAPS_TYPE_0011_SHADER_MODELS, list);
    check(hr == S_OK && n == 8 && hr_list == S_OK && cap == 8 && models[0] == D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011 &&
              models[7] == D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082,
          "1012 SHADER_MODELS, 16 bytes: 8 models, 5_1 to 6_6 (count %u)", n);

    // 1006 D3D12_OPTIONS: pass-through tiers and the documented clamps.
    D3D12DDI_D3D12_OPTIONS_DATA_0089 o;
    std::memset(&o, 0xEE, sizeof(o));
    hr = get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, o);
    check(hr == S_OK && o.ResourceBindingTier == D3D12DDI_RESOURCE_BINDING_TIER_3 &&
              o.TiledResourcesTier == D3D12DDI_TILED_RESOURCES_TIER_2 &&
              o.ConservativeRasterizationTier == D3D12DDI_CONSERVATIVE_RASTERIZATION_TIER_1 &&
              o.ResourceHeapTier == D3D12DDI_RESOURCE_HEAP_TIER_2 && o.OutputMergerLogicOp &&
              o.VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation &&
              o.CopyQueueTimestampQueriesSupported && !o.BarycentricsSupported && o.ReservedBufferPlacementSupported,
          "1006 D3D12_OPTIONS, 124 bytes: tiers and flags from OPTIONS, OPTIONS3 and OPTIONS4");
    check(o.RaytracingTier == D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED && !o.EnhancedBarriersSupported &&
              o.MeshShaderTier == D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED &&
              o.VariableShadingRateTier == D3D12DDI_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED &&
              o.SamplerFeedbackTier == D3D12DDI_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED &&
              o.RenderPassTier == D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED && !o.DepthBoundsTestSupported &&
              o.WriteBufferImmediateQueueFlags == D3D12DDI_COMMAND_QUEUE_FLAG_NONE && !o.DriverManagedShaderCachePresent,
          "1006: raytracing 1.1 and enhanced barriers of the engine are not reported while their slots are fail-safes");

    D3D12DDI_SHADER_CAPS_0084 s;
    std::memset(&s, 0xEE, sizeof(s));
    hr = get(caps, D3D12DDICAPS_TYPE_SHADER, s);
    check(hr == S_OK && s.MinPrecision == D3D12DDI_SHADER_MIN_PRECISION_16_BIT && s.DoubleOps && s.WaveOps &&
              s.WaveLaneCountMin == 32 && s.WaveLaneCountMax == 64 && s.TotalLaneCount == 2560 && s.Int64Ops &&
              s.Native16BitOps && s.AtomicInt64OnTypedResource && s.AtomicInt64OnGroupShared &&
              s.AtomicInt64OnDescriptorHeapResource && !s.ROVs && !s.DerivativesInMeshAndAmplificationShaders &&
              s.WaveMMATier == D3D12DDI_WAVE_MMA_TIER_NOT_SUPPORTED,
          "1004 SHADER, 64 bytes: OPTIONS, OPTIONS1, OPTIONS4, OPTIONS9 and OPTIONS11");

    D3D12DDI_ARCHITECTURE_INFO_DATA arch;
    std::memset(&arch, 0xEE, sizeof(arch));
    const HRESULT hr_arch = get(caps, D3D12DDICAPS_TYPE_ARCHITECTURE_INFO, arch);
    UINT node = 0;
    D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041 mem;
    std::memset(&mem, 0xEE, sizeof(mem));
    hr = get(caps, D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE, mem, &node);
    check(hr_arch == S_OK && !arch.TileBasedDeferredRenderer && hr == S_OK && mem.UMA && !mem.CacheCoherent &&
              !mem.IOCoherent && mem.HeapSerializationTier == D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1 &&
              mem.ResourceSerializationTier == D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_0,
          "1005 ARCHITECTURE_INFO and 1002 MEMORY_ARCHITECTURE (node 0): ARCHITECTURE1 and SERIALIZATION");

    D3D12DDI_GPUVA_CAPS_0004 va;
    std::memset(&va, 0xEE, sizeof(va));
    hr = get(caps, D3D12DDICAPS_TYPE_GPUVA_CAPS, va, &node);
    check(hr == S_OK && va.MaxGPUVirtualAddressBitsPerResource == 40, "1009 GPUVA_CAPS: 40 bits per resource");

    D3D12DDI_TEXTURE_LAYOUT_CAPS_0026 layout;
    std::memset(&layout, 0xEE, sizeof(layout));
    hr = get(caps, D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT, layout);
    D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023 prio;
    std::memset(&prio, 0xEE, sizeof(prio));
    const HRESULT hr_prio = get(caps, D3D12DDICAPS_TYPE_0023_UMD_BASED_COMMAND_QUEUE_PRIORITY, prio);
    D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050 sched;
    std::memset(&sched, 0xEE, sizeof(sched));
    const HRESULT hr_sched = get(caps, D3D12DDICAPS_TYPE_0050_HARDWARE_SCHEDULING_CAPS, sched);
    check(hr == S_OK && !layout.DeviceDependentLayoutCount && !layout.DeviceDependentSwizzleCount &&
              !layout.Supports64KStandardSwizzle && !layout.SupportsRowMajorTexture && hr_prio == S_OK &&
              prio.SupportedQueueFlagsForGlobalRealtimeQueues == D3D12DDI_COMMAND_QUEUE_FLAG_NONE && hr_sched == S_OK &&
              sched.ComputeQueuesPer3DQueue == 0,
          "1060 TEXTURE_LAYOUT, 1062 queue priority and 1067 scheduling: documented constants");

    D3D12DDI_OPTIONS_DATA_0090 o90;
    std::memset(&o90, 0xEE, sizeof(o90));
    const HRESULT hr90 = get(caps, D3D12DDICAPS_TYPE_OPTIONS_0090, o90);
    D3D12DDI_OPTIONS_DATA_0091 o91;
    std::memset(&o91, 0xEE, sizeof(o91));
    hr = get(caps, D3D12DDICAPS_TYPE_OPTIONS_0091, o91);
    check(hr90 == S_OK && o90.RelaxedFormatCastingSupported && hr == S_OK &&
              o91.UnrestrictedBufferTextureCopyPitchSupported && o91.UnrestrictedVertexElementAlignmentSupported &&
              !o91.InvertedViewportHeightFlipsYSupported && o91.InvertedViewportDepthFlipsZSupported,
          "1077 OPTIONS_0090 and 1078 OPTIONS_0091: OPTIONS12 and OPTIONS13");
    engine_ddi::free_adapter_caps(caps);

    // Feature level policy: an FL12_2 engine is reported as 12_1, an FL11_0 engine as 11_0, in both types.
    Answers a12_2 = fl12_0();
    a12_2.max_level = D3D_FEATURE_LEVEL_12_2;
    caps = query_stub(a12_2, info, "FL12_2 engine");
    if (caps) {
        level = level_1074(caps, D3D12DDI_3DPIPELINELEVEL_12_2, hr);
        l1007 = static_cast<D3D12DDI_3DPIPELINELEVEL>(0);
        const HRESULT hr7 = get(caps, D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, l1007);
        check(hr == S_OK && level == D3D12DDI_3DPIPELINELEVEL_12_1 && hr7 == S_OK && l1007 == D3D12DDI_3DPIPELINELEVEL_12_1,
              "FL12_2 engine: 1074 and 1007 report 12_1, the engine-ddi ceiling (%d, %d)", static_cast<int>(level),
              static_cast<int>(l1007));
        engine_ddi::free_adapter_caps(caps);
    }
    Answers a11 = fl12_0();
    a11.max_level = D3D_FEATURE_LEVEL_11_0;
    a11.unsupported = D3D12_FEATURE_D3D12_OPTIONS13;
    caps = query_stub(a11, info, "FL11_0 engine without OPTIONS13");
    if (caps) {
        level = level_1074(caps, D3D12DDI_3DPIPELINELEVEL_12_2, hr);
        l1007 = static_cast<D3D12DDI_3DPIPELINELEVEL>(0);
        const HRESULT hr7 = get(caps, D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, l1007);
        std::memset(&o91, 0xEE, sizeof(o91));
        const HRESULT hr91 = get(caps, D3D12DDICAPS_TYPE_OPTIONS_0091, o91);
        check(hr == S_OK && level == D3D12DDI_3DPIPELINELEVEL_11_0 && hr7 == S_OK &&
                  l1007 == D3D12DDI_3DPIPELINELEVEL_11_0 && hr91 == S_OK &&
                  !o91.UnrestrictedBufferTextureCopyPitchSupported && !o91.InvertedViewportDepthFlipsZSupported,
              "FL11_0 engine: 1074 and 1007 report 11_0; an unanswered OPTIONS13 reports no support in 1078");
        engine_ddi::free_adapter_caps(caps);
    }
}

// ---- Real engine --------------------------------------------------------------------------------------------------------
LONG g_services = 0;
HRESULT APIENTRY bind_queue(void*, void*, VkQueue) { InterlockedIncrement(&g_services); return E_FAIL; }
void APIENTRY unbind_queue(void*, void*, VkQueue) { InterlockedIncrement(&g_services); }

bool find_adapter(const wchar_t* filter, LUID& luid) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) return false;
    bool found = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; !found && factory->EnumAdapters1(i, &adapter) == S_OK; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
            (!filter || std::wcsstr(desc.Description, filter))) {
            std::printf("adapter: %ls\n", desc.Description);
            luid = desc.AdapterLuid;
            found = true;
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

int test_engine(const wchar_t* path, const wchar_t* adapter) {
    wchar_t full[MAX_PATH];
    HMODULE dll = nullptr;
    if (GetFullPathNameW(path, MAX_PATH, full, nullptr))
        dll = LoadLibraryExW(full, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE vulkan = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto gipa = vulkan ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(
                             reinterpret_cast<void*>(GetProcAddress(vulkan, "vkGetInstanceProcAddr")))
                       : nullptr;
    auto get_funcs = dll ? reinterpret_cast<PFN_BC250_VKD3D_ENGINE_GET_FUNCS>(
                               reinterpret_cast<void*>(GetProcAddress(dll, BC250_VKD3D_ENGINE_GET_FUNCS_NAME)))
                         : nullptr;
    LUID luid{};
    if (!get_funcs || !gipa || !find_adapter(adapter, luid)) {
        check(false, "setup: engine %s, export %s, vkGetInstanceProcAddr %s", dll ? "loaded" : "not loaded",
              get_funcs ? "found" : "missing", gipa ? "found" : "missing");
        return 1;
    }
    BC250_VKD3D_ENGINE_FUNCS funcs{};
    funcs.Size = sizeof(funcs);
    HRESULT hr = get_funcs(kAbi12, &funcs);
    check(hr == S_OK && funcs.AbiVersion >= kAbi12 && funcs.QueryAdapterCaps,
          "GetFuncs(1.2) fills QueryAdapterCaps (hr %08lx, engine ABI %u.%u)", static_cast<unsigned long>(hr),
          funcs.AbiVersion >> 16, funcs.AbiVersion & 0xFFFFu);
    if (hr != S_OK) return 1;

    // The create info the shell will pass to CreateDevice as well.
    BC250_VKD3D_SHELL_SERVICES services{sizeof(services), nullptr, bind_queue, unbind_queue};
    BC250_VKD3D_DEVICE_CREATE_INFO info{};
    info.Size = sizeof(info);
    info.AbiVersion = kAbi12;
    info.GetInstanceProcAddr = gipa;
    info.AdapterLuid = luid;
    info.MinimumFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    info.QueueMode = BC250_VKD3D_QUEUE_MODE_INLINE;
    info.Services = &services;
    engine_ddi::AdapterCaps* caps = nullptr;
    hr = engine_ddi::query_adapter_caps(&funcs, &info, &caps);
    check(hr == S_OK && caps && !g_services,
          "query_adapter_caps on the engine, INLINE create info: S_OK, no Services called (hr %08lx)",
          static_cast<unsigned long>(hr));
    if (!caps) return 1;

    HRESULT hr4 = E_FAIL;
    const D3D12DDI_3DPIPELINELEVEL level = level_1074(caps, D3D12DDI_3DPIPELINELEVEL_12_2, hr4);
    D3D12DDI_3DPIPELINELEVEL l1007 = static_cast<D3D12DDI_3DPIPELINELEVEL>(0);
    const HRESULT hr7 = get(caps, D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, l1007);
    check(hr4 == S_OK && hr7 == S_OK && level >= D3D12DDI_3DPIPELINELEVEL_11_0 &&
              level <= D3D12DDI_3DPIPELINELEVEL_12_1 && l1007 == level,
          "1074 (8 bytes, runtime highest 12_2) and 1007 (4 bytes) agree: level %d", static_cast<int>(level));
    UINT n = 0;
    D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 sm{&n, nullptr};
    hr = get(caps, D3D12DDICAPS_TYPE_0011_SHADER_MODELS, sm);
    D3D12DDI_D3D12_OPTIONS_DATA_0089 o{};
    const HRESULT hr_o = get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, o);
    D3D12DDI_SHADER_CAPS_0084 s{};
    const HRESULT hr_s = get(caps, D3D12DDICAPS_TYPE_SHADER, s);
    check(hr == S_OK && n >= 1 && hr_o == S_OK && hr_s == S_OK,
          "1012, 1006 and 1004 answered: %u shader models, binding tier %d, tiled tier %d, heap tier %d, waves %u-%u",
          n, static_cast<int>(o.ResourceBindingTier), static_cast<int>(o.TiledResourcesTier),
          static_cast<int>(o.ResourceHeapTier), s.WaveLaneCountMin, s.WaveLaneCountMax);
    engine_ddi::free_adapter_caps(caps);
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const wchar_t* engine = nullptr;
    const wchar_t* adapter = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (!std::wcscmp(argv[i], L"--engine") && i + 1 < argc) {
            engine = argv[++i];
        } else if (!std::wcscmp(argv[i], L"--adapter") && i + 1 < argc) {
            adapter = argv[++i];
        } else {
            std::printf("usage: caps-test [--engine <amdgpu_wddm_vkd3d.dll> [--adapter <substring>]]\n");
            return 2;
        }
    }
    if (engine) {
        test_engine(engine, adapter);
    } else {
        BC250_VKD3D_DEVICE_CREATE_INFO info{};
        info.Size = sizeof(info);
        info.AbiVersion = kAbi12;
        test_stub(info);
    }
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
