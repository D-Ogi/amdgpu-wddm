// SPDX-License-Identifier: MIT
// GetCaps host test, linked against the native engine-ddi.lib (no harness macro), the way the shell links it.
//
//   caps-test.exe                         stub engine: QueryAdapterCaps answers from the fixed tables below
//   caps-test.exe --engine <dll> [--adapter <substring>]
//                                         the real engine DLL (ABI 1.2) on this PC's GPU, INLINE create info
//
// The stub's answers are test inputs, not measurements of any GPU. The test checks the calls the runtime made in
// M768 with their exact sizes (1074 with 8 bytes, 1007 with 4) and the mapping of every other type build_caps
// answers (INTEGRATION.md, "GetCaps"), and the shell's memory architecture policy of 1002 (INTEGRATION.md,
// "Memory architecture policy").
#include "engine-ddi.h"
#include <dxgi1_4.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <io.h>
#include <iterator>

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

// The shell asks with the newest layout it offers (adapter-caps.cpp); 0092 is checked separately.
constexpr uint32_t kDdi = D3D12DDI_BUILD_VERSION_0094;
constexpr uint32_t kAbi12 = (1u << 16) | 2u;
constexpr uint32_t kAbi = BC250_VKD3D_ENGINE_ABI_VERSION;   // what the shell asks the engine for

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
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3;
    BC250_VKD3D_VIEW_INSTANCING_CONTRACT view_contract;
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7;
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9;
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12;
    D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13;
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14;
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21;
    D3D12_FEATURE_DATA_SERIALIZATION serialization;
    UINT raw_model;                             // non-zero: SHADER_MODEL answers this whatever was asked
    int unsupported;                            // a D3D12_FEATURE answered DXGI_ERROR_UNSUPPORTED; -1 for none
};

// An FL12_0 engine that also claims what engine-ddi must clamp (mesh derivatives, VRS) and what it maps through
// to a cap of its own (raytracing 1.1, enhanced barriers, relaxed format casting). Values are arbitrary but
// valid. Its OPTIONS7 stays zeroed: a Vulkan device without
// VK_EXT_mesh_shader, so the mesh shader tier and the mesh derivatives of OPTIONS9 are both clamped off. Its
// OPTIONS2 DepthBoundsTestSupported and its OPTIONS3 ViewInstancingTier stay at the floor as well, so the 1006
// checks below read what an engine without either feature answers; test_depth_bounds and test_view_instancing
// raise each of them in turn.
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
D3D_SHADER_MODEL g_model_asked = static_cast<D3D_SHADER_MODEL>(0);

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
        if (q.Feature == BC250_VKD3D_FEATURE_VIEW_INSTANCING_CONTRACT) {
            if (a.view_contract.Revision) answer(q, a.view_contract);
            continue;
        }
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
            g_model_asked = d->HighestShaderModel;
            if (d->HighestShaderModel > a.max_model) d->HighestShaderModel = a.max_model;
            if (a.raw_model) d->HighestShaderModel = static_cast<D3D_SHADER_MODEL>(a.raw_model);
            q.Result = S_OK;
            break;
        }
        case D3D12_FEATURE_ARCHITECTURE1: answer(q, a.architecture); break;
        case D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT: answer(q, a.gpu_va); break;
        case D3D12_FEATURE_D3D12_OPTIONS: answer(q, a.options); break;
        case D3D12_FEATURE_D3D12_OPTIONS1: answer(q, a.options1); break;
        case D3D12_FEATURE_D3D12_OPTIONS2: answer(q, a.options2); break;
        case D3D12_FEATURE_D3D12_OPTIONS3: answer(q, a.options3); break;
        case D3D12_FEATURE_D3D12_OPTIONS4: answer(q, a.options4); break;
        case D3D12_FEATURE_D3D12_OPTIONS5: answer(q, a.options5); break;
        case D3D12_FEATURE_D3D12_OPTIONS7: answer(q, a.options7); break;
        case D3D12_FEATURE_D3D12_OPTIONS9: answer(q, a.options9); break;
        case D3D12_FEATURE_D3D12_OPTIONS11: answer(q, a.options11); break;
        case D3D12_FEATURE_D3D12_OPTIONS12: answer(q, a.options12); break;
        case D3D12_FEATURE_D3D12_OPTIONS13: answer(q, a.options13); break;
        case D3D12_FEATURE_D3D12_OPTIONS14: answer(q, a.options14); break;
        case D3D12_FEATURE_D3D12_OPTIONS21: answer(q, a.options21); break;
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

// 1003 TEXTURE_LAYOUT_SETS (20 bytes, pInfo {D3D12DDI_TL_ROW_MAJOR, unit}) for every functional unit, and 1061
// SWIZZLE_PATTERN. 1003 states the row-major data of buffers on every unit: entry 0 covers every element size with
// alignment 1, entry 1 is unused. A device-dependent swizzle count of 0 in 1060 means no swizzle pattern index.
void check_layout_sets(const engine_ddi::AdapterCaps* caps, const char* what) {
    D3D12DDI_TEXTURE_LAYOUT_CAPS_0026 layout;
    std::memset(&layout, 0xEE, sizeof(layout));
    const HRESULT hr60 = get(caps, D3D12DDICAPS_TYPE_0022_TEXTURE_LAYOUT, layout);
    check(hr60 == S_OK && !layout.SupportsRowMajorTexture && layout.DeviceDependentSwizzleCount == 0,
          "%s: 1060 reports no row-major texture and no device-dependent swizzle pattern", what);
    D3D12DDI_ROW_MAJOR_LAYOUT_CAPS expected{};
    expected.SubCaps[0].MaxElementSize = 0xFFFF;
    expected.SubCaps[0].BaseOffsetAlignment = 1;
    expected.SubCaps[0].PitchAlignment = 1;
    expected.SubCaps[0].DepthPitchAlignment = 1;
    const D3D12DDI_FUNCTIONAL_UNIT units[] = {D3D12DDI_FUNCUNIT_COMBINED, D3D12DDI_FUNCUNIT_COPY_SRC,
                                              D3D12DDI_FUNCUNIT_COPY_DST};
    for (const D3D12DDI_FUNCTIONAL_UNIT unit : units) {
        UINT key[2] = {D3D12DDI_TL_ROW_MAJOR, static_cast<UINT>(unit)};
        D3D12DDI_ROW_MAJOR_LAYOUT_CAPS sets;
        std::memset(&sets, 0xEE, sizeof(sets));
        const HRESULT hr = get(caps, D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS, sets, key, 20);
        check(hr == S_OK && !std::memcmp(&sets, &expected, sizeof(expected)) && key[0] == D3D12DDI_TL_ROW_MAJOR &&
                  key[1] == static_cast<UINT>(unit),
              "%s: 1003 TEXTURE_LAYOUT_SETS, DataSize 20, pInfo {ROW_MAJOR, unit %d}: SubCaps[0] 0xFFFF with "
              "alignments 1, SubCaps[1] zero, Flags NONE (hr %08lx, SubCaps[0].MaxElementSize %u)",
              what, static_cast<int>(unit), static_cast<unsigned long>(hr),
              static_cast<unsigned>(sets.SubCaps[0].MaxElementSize));
    }
    D3D12DDI_ROW_MAJOR_LAYOUT_CAPS sets;
    std::memset(&sets, 0xEE, sizeof(sets));
    HRESULT hr = get(caps, D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS, sets);
    check(hr == E_INVALIDARG && untouched(&sets, sizeof(sets)), "%s: 1003 with pInfo NULL: E_INVALIDARG, nothing written",
          what);
    UINT index = 0;
    D3D12DDI_SWIZZLE_PATTERN_DESC_0022 pattern;
    std::memset(&pattern, 0xEE, sizeof(pattern));
    hr = get(caps, D3D12DDICAPS_TYPE_0022_SWIZZLE_PATTERN, pattern, &index);
    check(hr == E_INVALIDARG && untouched(&pattern, sizeof(pattern)) && layout.DeviceDependentSwizzleCount == 0,
          "%s: 1061 SWIZZLE_PATTERN, %zu bytes, index 0: E_INVALIDARG, nothing written, as 1060 counts 0 patterns",
          what, sizeof(pattern));
}

// 1057 PROTECTED_RESOURCE_SESSION_SUPPORT (8 bytes, NodeIndex 0), 1069 EXECUTECOMMANDLISTS_PARALLELISM and 1071
// SUPPORT_BATCHED_MARKERS (a BOOL each): the documented "none" answers.
void check_none_types(const engine_ddi::AdapterCaps* caps, const char* what) {
    D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_DATA_0030 session;
    std::memset(&session, 0xEE, sizeof(session));
    session.NodeIndex = 0;
    const HRESULT hr57 = get(caps, D3D12DDICAPS_TYPE_0030_PROTECTED_RESOURCE_SESSION_SUPPORT, session);
    BOOL parallel;
    std::memset(&parallel, 0xEE, sizeof(parallel));
    const HRESULT hr69 = get(caps, D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM, parallel);
    BOOL markers;
    std::memset(&markers, 0xEE, sizeof(markers));
    const HRESULT hr71 = get(caps, D3D12DDICAPS_TYPE_0073_SUPPORT_BATCHED_MARKERS, markers);
    check(hr57 == S_OK && session.NodeIndex == 0 &&
              session.Support == D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_0030_NONE && hr69 == S_OK &&
              parallel == FALSE && hr71 == S_OK && markers == FALSE,
          "%s: 1057 (%zu bytes, node 0) Support NONE; 1069 and 1071 (%zu bytes) FALSE", what, sizeof(session),
          sizeof(BOOL));
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
    check(g_queries == 19, "one batch of 19 features including the optional view contract (%u)", g_queries);
    check(g_model_asked == D3D_SHADER_MODEL_6_9, "SHADER_MODEL is asked with 6_9 as the highest model (%x)",
          static_cast<unsigned>(g_model_asked));

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
    UINT node0 = 0;
    r = {D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES, &node0, raw, sizeof(D3D12DDI_COMMAND_QUEUE_FLAGS)};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    D3D12DDI_COMMAND_QUEUE_FLAGS flags;
    std::memcpy(&flags, raw, sizeof(flags));
    check(hr == S_OK && flags == (D3D12DDI_COMMAND_QUEUE_FLAG_3D | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE |
                                  D3D12DDI_COMMAND_QUEUE_FLAG_COPY) &&
              untouched(raw + sizeof(flags), sizeof(raw) - sizeof(flags)) && node0 == 0,
          "1059 CPU_PAGE_TABLE_FALSE_POSITIVES, node 0, 4 bytes: 3D, COMPUTE and COPY (hr %08lx, flags 0x%x)",
          static_cast<unsigned long>(hr), static_cast<unsigned>(flags));
    std::memset(raw, 0xEE, sizeof(raw));
    UINT node1 = 1;
    r = {D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES, &node1, raw, sizeof(D3D12DDI_COMMAND_QUEUE_FLAGS)};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    const bool node_refused = hr == E_INVALIDARG && untouched(raw, sizeof(raw));
    r = {D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES, nullptr, raw, sizeof(D3D12DDI_COMMAND_QUEUE_FLAGS)};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    const bool null_refused = hr == E_INVALIDARG && untouched(raw, sizeof(raw));
    r = {D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES, &node0, raw, 8};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    check(node_refused && null_refused && hr == E_INVALIDARG && untouched(raw, sizeof(raw)),
          "1059 with node 1, with pInfo NULL and with 8 bytes: E_INVALIDARG, nothing written");
    BOOL compute_only = FALSE;
    r = {D3D12DDICAPS_TYPE_0033_ADAPTER_COMPUTE_ONLY, nullptr, raw, sizeof(compute_only)};
    hr = engine_ddi::build_caps(caps, kDdi, &r);
    check(hr == E_NOTIMPL && untouched(raw, sizeof(raw)), "unanswered type 1066: E_NOTIMPL, nothing written (hr %08lx)",
          static_cast<unsigned long>(hr));
    r = {D3D12DDICAPS_TYPE_3DPIPELINESUPPORT, nullptr, raw, 4};
    hr = engine_ddi::build_caps(caps, 91, &r);
    const bool refused91 = hr == E_INVALIDARG && untouched(raw, sizeof(raw));
    hr = engine_ddi::build_caps(caps, 95, &r);
    check(refused91 && hr == E_INVALIDARG && untouched(raw, sizeof(raw)),
          "a ddi_version outside 92 to 94 (91, 95): E_INVALIDARG");
    hr = engine_ddi::build_caps(caps, D3D12DDI_BUILD_VERSION_0093, &r);
    D3D12DDI_3DPIPELINELEVEL l93;
    std::memcpy(&l93, raw, sizeof(l93));
    check(hr == S_OK && l93 == D3D12DDI_3DPIPELINELEVEL_12_0 && untouched(raw + 4, sizeof(raw) - 4),
          "ddi_version 93: the same layouts are answered (1007 12_0)");
    std::memset(raw, 0xEE, sizeof(raw));
    hr = engine_ddi::build_caps(caps, D3D12DDI_BUILD_VERSION_0092, &r);
    D3D12DDI_3DPIPELINELEVEL l92;
    std::memcpy(&l92, raw, sizeof(l92));
    check(hr == S_OK && l92 == D3D12DDI_3DPIPELINELEVEL_12_0 && untouched(raw + 4, sizeof(raw) - 4),
          "ddi_version 92: the 0092 layouts are still answered (1007 12_0)");

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
          "1012 SHADER_MODELS, 16 bytes, engine at 6_6: 8 models, 5_1 to 6_6 (count %u)", n);

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
    check(o.RaytracingTier == D3D12DDI_RAYTRACING_TIER_1_1 && o.EnhancedBarriersSupported &&
              o.MeshShaderTier == D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED &&
              o.VariableShadingRateTier == D3D12DDI_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED &&
              o.SamplerFeedbackTier == D3D12DDI_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED &&
              o.RenderPassTier == D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED && !o.DepthBoundsTestSupported &&
              o.WriteBufferImmediateQueueFlags == D3D12DDI_COMMAND_QUEUE_FLAG_NONE && !o.DriverManagedShaderCachePresent,
          "1006: the engine's raytracing 1.1 and its enhanced barriers are reported, the fields whose slots are "
          "still fail-safes are not, an engine at render pass tier 0 and with no immediate-write list type gets "
          "neither, and an engine without VK_EXT_mesh_shader gets no mesh shader tier");
    check(o.ViewInstancingTier == D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED &&
              o.ProgrammableSamplePositionsTier == D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED,
          "1006: an engine that reports no depth bounds test and no view instancing gets neither, and sample "
          "positions stay off whatever the engine says (tier %d)", static_cast<int>(o.ViewInstancingTier));

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
    check_layout_sets(caps, "FL12_0 engine");
    check_none_types(caps, "FL12_0 engine");

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

// ---- Memory architecture policy (1002) ------------------------------------------------------------------------------
using engine_ddi::MemoryArchitecturePolicy;
using engine_ddi::PolicyBool;
using Mem = D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041;

MemoryArchitecturePolicy policy() {
    MemoryArchitecturePolicy p{};
    p.size = sizeof(p);
    return p;
}

HRESULT get_1002(const engine_ddi::AdapterCaps* caps, Mem& m) {
    UINT node = 0;
    std::memset(&m, 0xEE, sizeof(m));
    return get(caps, D3D12DDICAPS_TYPE_MEMORY_ARCHITECTURE, m, &node);
}

bool same(const Mem& a, const Mem& b) { return !std::memcmp(&a, &b, sizeof(Mem)); }

// The 1002 answer and the other types around it, byte for byte, before and after policies.
struct Snapshot {
    Mem mem;
    D3D12DDI_ARCHITECTURE_INFO_DATA arch;
    D3D12DDI_D3D12_OPTIONS_DATA_0089 options;
};
bool snapshot(const engine_ddi::AdapterCaps* caps, Snapshot& s) {
    std::memset(&s, 0xEE, sizeof(s));
    return get_1002(caps, s.mem) == S_OK && get(caps, D3D12DDICAPS_TYPE_ARCHITECTURE_INFO, s.arch) == S_OK &&
           get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, s.options) == S_OK;
}
bool others_same(const Snapshot& a, const Snapshot& b) {
    return !std::memcmp(&a.arch, &b.arch, sizeof(a.arch)) && !std::memcmp(&a.options, &b.options, sizeof(a.options));
}

// Sets p, then reads 1002: S_OK and the answer, or the refusal and the answer that stayed.
HRESULT apply(engine_ddi::AdapterCaps* caps, const MemoryArchitecturePolicy& p, Mem& m) {
    const HRESULT hr = engine_ddi::set_memory_architecture_policy(caps, &p);
    get_1002(caps, m);
    return hr;
}

// The raytracing tier: reported by default, the engine's 1_1 as 1_1, never more than the engine says, and
// NOT_SUPPORTED once reporting is taken back. Nothing else of 1006 changes either way.
void test_raytracing_tier_reporting(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    const auto tier_with = [&](D3D12_RAYTRACING_TIER engine, const char* what, D3D12DDI_RAYTRACING_TIER expected) {
        Answers a = fl12_0();
        a.options5.RaytracingTier = engine;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, what);
        if (!caps) return;
        D3D12DDI_D3D12_OPTIONS_DATA_0089 base{}, off{}, on{};
        HRESULT hr = get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, base);
        const HRESULT hr_off = engine_ddi::set_raytracing_tier_reporting(caps, false);
        if (hr == S_OK) hr = get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, off);
        const HRESULT hr_on = engine_ddi::set_raytracing_tier_reporting(caps, true);
        if (hr == S_OK) hr = get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, on);
        D3D12DDI_D3D12_OPTIONS_DATA_0089 rest = base;
        rest.RaytracingTier = D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED;
        check(hr == S_OK && hr_on == S_OK && hr_off == S_OK && base.RaytracingTier == expected &&
                  off.RaytracingTier == D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED &&
                  !std::memcmp(&rest, &off, sizeof(off)) && !std::memcmp(&on, &base, sizeof(base)),
              "raytracing tier, %s: 1006 reports %d by default, NOT_SUPPORTED once taken back with the rest of "
              "1006 unchanged, and %d again after",
              what, static_cast<int>(expected), static_cast<int>(expected));
        engine_ddi::free_adapter_caps(caps);
    };
    tier_with(D3D12_RAYTRACING_TIER_1_1, "engine at 1_1", D3D12DDI_RAYTRACING_TIER_1_1);
    tier_with(static_cast<D3D12_RAYTRACING_TIER>(12), "engine at 1_2", D3D12DDI_RAYTRACING_TIER_1_1);
    tier_with(D3D12_RAYTRACING_TIER_1_0, "engine at 1_0", D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED);
    tier_with(D3D12_RAYTRACING_TIER_NOT_SUPPORTED, "engine without", D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED);
    check(engine_ddi::set_raytracing_tier_reporting(nullptr, true) == E_INVALIDARG,
          "raytracing tier reporting refused: null caps");
}

// The three answers the mesh shader tier decides, read in one go: 1006's tier and full range render target array
// index, 1004's mesh derivatives, and the driver maximum feature level, which the tier must not move.
struct MeshAnswer {
    HRESULT hr;
    D3D12DDI_MESH_SHADER_TIER tier;
    BOOL full_range;
    BOOL culled_primitives;
    BOOL derivatives;
    D3D12DDI_3DPIPELINELEVEL level;
    D3D12DDI_D3D12_OPTIONS_DATA_0089 options;
};
MeshAnswer mesh_answer(const engine_ddi::AdapterCaps* caps) {
    MeshAnswer m;
    std::memset(&m, 0xEE, sizeof(m));
    D3D12DDI_SHADER_CAPS_0084 s;
    std::memset(&s, 0xEE, sizeof(s));
    m.hr = get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, m.options);
    const HRESULT hr_shader = get(caps, D3D12DDICAPS_TYPE_SHADER, s);
    HRESULT hr_level = E_FAIL;
    m.level = level_1074(caps, D3D12DDI_3DPIPELINELEVEL_12_2, hr_level);
    if (m.hr == S_OK) m.hr = hr_shader;
    if (m.hr == S_OK) m.hr = hr_level;
    m.tier = m.options.MeshShaderTier;
    m.full_range = m.options.MeshShaderSupportsFullRangeRenderTargetArrayIndex;
    m.culled_primitives = m.options.MSPrimitivesPipelineStatisticIncludesCulledPrimitives;
    m.derivatives = s.DerivativesInMeshAndAmplificationShaders;
    return m;
}

// The same answer with the three mesh fields back at the floor: everything else of 1006 must be what an engine
// without mesh shaders answers.
bool rest_of_1006_unchanged(const MeshAnswer& m, const MeshAnswer& floor) {
    D3D12DDI_D3D12_OPTIONS_DATA_0089 rest = m.options;
    rest.MeshShaderTier = D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED;
    rest.MeshShaderSupportsFullRangeRenderTargetArrayIndex = FALSE;
    return !std::memcmp(&rest, &floor.options, sizeof(rest));
}

// The mesh shader tier: 1006 reports tier 1 exactly when the engine's OPTIONS7 answers tier 1 or higher and the
// reported shader model reaches 6_5, with 1006's full range render target array index and 1004's mesh derivatives
// behind the same condition. A higher tier of a later engine is reported as 1 (the only tier at 0092), an
// unanswered OPTIONS7 is NOT_SUPPORTED, and the tier moves neither the rest of 1006 nor the feature level.
void test_mesh_shader_tier(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    const auto engine_with = [](D3D12_MESH_SHADER_TIER tier) {
        Answers a = fl12_0();                 // OPTIONS9 derivatives are already TRUE there
        a.options7.MeshShaderTier = tier;
        a.options9.MeshShaderSupportsFullRangeRenderTargetArrayIndex = TRUE;
        return a;
    };

    // The floor: an engine whose Vulkan device has no VK_EXT_mesh_shader. The rest of this test compares with it.
    MeshAnswer floor{};
    {
        engine_ddi::AdapterCaps* caps =
            query_stub(engine_with(D3D12_MESH_SHADER_TIER_NOT_SUPPORTED), info, "engine without mesh shaders");
        if (!caps) return;
        floor = mesh_answer(caps);
        check(floor.hr == S_OK && floor.tier == D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED && !floor.full_range &&
                  !floor.culled_primitives && !floor.derivatives && floor.level == D3D12DDI_3DPIPELINELEVEL_12_0,
              "mesh shaders, engine at NOT_SUPPORTED: 1006 reports no tier and no full range render target array "
              "index, 1004 no derivatives, 1074 still 12_0 (tier %d)", static_cast<int>(floor.tier));
        engine_ddi::free_adapter_caps(caps);
    }

    // The engine's tier 1, and a later engine's higher tier, both reported as tier 1.
    const auto reports_tier_1 = [&](D3D12_MESH_SHADER_TIER engine, const char* what) {
        engine_ddi::AdapterCaps* caps = query_stub(engine_with(engine), info, what);
        if (!caps) return;
        const MeshAnswer m = mesh_answer(caps);
        check(m.hr == S_OK && m.tier == D3D12DDI_MESH_SHADER_TIER_1 && m.full_range && !m.culled_primitives &&
                  m.derivatives && m.level == D3D12DDI_3DPIPELINELEVEL_12_0 && rest_of_1006_unchanged(m, floor),
              "mesh shaders, %s: 1006 reports tier 1 with the full range render target array index of OPTIONS9 and "
              "nothing else of 1006 moved, 1004 reports the derivatives of OPTIONS9, 1074 stays 12_0 (tier %d)",
              what, static_cast<int>(m.tier));
        engine_ddi::free_adapter_caps(caps);
    };
    reports_tier_1(D3D12_MESH_SHADER_TIER_1, "engine at tier 1");
    reports_tier_1(static_cast<D3D12_MESH_SHADER_TIER>(20), "engine at a tier above 1");

    // An engine that does not answer OPTIONS7 at all: the zeroed structure, which is NOT_SUPPORTED.
    {
        Answers a = engine_with(D3D12_MESH_SHADER_TIER_1);
        a.unsupported = D3D12_FEATURE_D3D12_OPTIONS7;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine without an OPTIONS7 answer");
        if (!caps) return;
        const MeshAnswer m = mesh_answer(caps);
        check(m.hr == S_OK && m.tier == D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED && !m.full_range && !m.derivatives &&
                  rest_of_1006_unchanged(m, floor),
              "mesh shaders, engine without an OPTIONS7 answer: no tier, no full range render target array index, "
              "no derivatives (tier %d)", static_cast<int>(m.tier));
        engine_ddi::free_adapter_caps(caps);
    }

    // A mesh or an amplification program is a DXIL program of shader model 6.5, so a reported model below 6_5
    // takes the tier away whatever the engine answers, and restoring the model brings it back.
    {
        engine_ddi::AdapterCaps* caps =
            query_stub(engine_with(D3D12_MESH_SHADER_TIER_1), info, "engine at tier 1, shader model ceiling");
        if (!caps) return;
        const HRESULT h64 = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_4);
        const MeshAnswer below = mesh_answer(caps);
        const HRESULT h65 = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_5);
        const MeshAnswer at = mesh_answer(caps);
        check(h64 == S_OK && h65 == S_OK && below.hr == S_OK && at.hr == S_OK &&
                  below.tier == D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED && !below.full_range && !below.derivatives &&
                  rest_of_1006_unchanged(below, floor) && at.tier == D3D12DDI_MESH_SHADER_TIER_1 && at.full_range &&
                  at.derivatives,
              "mesh shaders, ceiling 6_4 on an engine at tier 1: no tier, no full range render target array index "
              "and no derivatives; a ceiling of 6_5 reports tier 1 again (tiers %d then %d)",
              static_cast<int>(below.tier), static_cast<int>(at.tier));
        engine_ddi::free_adapter_caps(caps);
    }
}

// The two 1006 answers the engine decides for the depth bounds test (L52) and view instancing (L57), read in one
// go, with the sample position tier and the driver maximum feature level beside them: sample positions must stay
// off whatever the engine answers (its own entry is a stub), and neither feature may move the level, because 12_2
// also needs VRS tier 2 and sampler feedback.
struct PipelineCapAnswer {
    HRESULT hr;
    BOOL depth_bounds;
    D3D12DDI_VIEW_INSTANCING_TIER view_instancing;
    D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER sample_positions;
    D3D12DDI_3DPIPELINELEVEL level;
    D3D12DDI_D3D12_OPTIONS_DATA_0089 options;
};
PipelineCapAnswer pipeline_cap_answer(const engine_ddi::AdapterCaps* caps) {
    PipelineCapAnswer p;
    std::memset(&p, 0xEE, sizeof(p));
    p.hr = get(caps, D3D12DDICAPS_TYPE_D3D12_OPTIONS, p.options);
    HRESULT hr_level = E_FAIL;
    p.level = level_1074(caps, D3D12DDI_3DPIPELINELEVEL_12_2, hr_level);
    if (p.hr == S_OK) p.hr = hr_level;
    p.depth_bounds = p.options.DepthBoundsTestSupported;
    p.view_instancing = p.options.ViewInstancingTier;
    p.sample_positions = p.options.ProgrammableSamplePositionsTier;
    return p;
}
// The same answer with the two fields back at the floor: everything else of 1006 must be what an engine with
// neither feature answers.
bool rest_of_1006_same(const PipelineCapAnswer& p, const PipelineCapAnswer& floor) {
    D3D12DDI_D3D12_OPTIONS_DATA_0089 rest = p.options;
    rest.DepthBoundsTestSupported = FALSE;
    rest.ViewInstancingTier = D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
    return !std::memcmp(&rest, &floor.options, sizeof(rest));
}

// 1006 DepthBoundsTestSupported is the engine's OPTIONS2 answer and nothing else: TRUE when the engine says so,
// FALSE when it says so, FALSE when it does not answer OPTIONS2 at all, and in no case does it move the rest of
// 1006 or the feature level.
void test_depth_bounds(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    PipelineCapAnswer floor{};
    {
        engine_ddi::AdapterCaps* caps = query_stub(fl12_0(), info, "engine without a depth bounds test");
        if (!caps) return;
        floor = pipeline_cap_answer(caps);
        check(floor.hr == S_OK && !floor.depth_bounds &&
                  floor.sample_positions == D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED &&
                  floor.level == D3D12DDI_3DPIPELINELEVEL_12_0,
              "depth bounds, engine at FALSE: 1006 reports FALSE, 1074 still 12_0 (hr %08lx)",
              static_cast<unsigned long>(floor.hr));
        engine_ddi::free_adapter_caps(caps);
    }
    {
        Answers a = fl12_0();
        a.options2.DepthBoundsTestSupported = TRUE;
        // The engine's sample position tier is raised at the same time, to show that engine-ddi reports the one
        // field of OPTIONS2 it has implemented and not the other.
        a.options2.ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_2;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine with a depth bounds test");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && p.depth_bounds &&
                  p.sample_positions == D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED &&
                  p.level == D3D12DDI_3DPIPELINELEVEL_12_0 && rest_of_1006_same(p, floor),
              "depth bounds, engine at TRUE: 1006 reports TRUE and nothing else of 1006 moved, the engine's "
              "sample position tier 2 is still not reported, 1074 stays 12_0");
        engine_ddi::free_adapter_caps(caps);
    }
    {
        Answers a = fl12_0();
        a.options2.DepthBoundsTestSupported = TRUE;
        a.unsupported = D3D12_FEATURE_D3D12_OPTIONS2;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine without an OPTIONS2 answer");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && !p.depth_bounds && rest_of_1006_same(p, floor),
              "depth bounds, engine without an OPTIONS2 answer: the zeroed structure is FALSE");
        engine_ddi::free_adapter_caps(caps);
    }
}

// Legacy partial multiview is never enough. The full-domain contract enables only Tier 2.
void test_view_instancing(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    PipelineCapAnswer floor{};
    {
        engine_ddi::AdapterCaps* caps = query_stub(fl12_0(), info, "engine without view instancing");
        if (!caps) return;
        floor = pipeline_cap_answer(caps);
        check(floor.hr == S_OK && floor.view_instancing == D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED &&
                  floor.level == D3D12DDI_3DPIPELINELEVEL_12_0,
              "view instancing, engine at NOT_SUPPORTED: 1006 reports no tier, 1074 still 12_0 (tier %d)",
              static_cast<int>(floor.view_instancing));
        engine_ddi::free_adapter_caps(caps);
    }
    const auto reports = [&](D3D12_VIEW_INSTANCING_TIER engine, D3D12DDI_VIEW_INSTANCING_TIER want,
                             const char* what, UINT revision = 0, UINT contract_tier = 0) {
        Answers a = fl12_0();
        a.options3.ViewInstancingTier = engine;
        a.view_contract = {revision, contract_tier};
        engine_ddi::AdapterCaps* caps = query_stub(a, info, what);
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && p.view_instancing == want && p.level == D3D12DDI_3DPIPELINELEVEL_12_0 &&
                  rest_of_1006_same(p, floor),
              "view instancing, %s: 1006 reports tier %d and nothing else of 1006 moved, 1074 stays 12_0 (tier %d)",
              what, static_cast<int>(want), static_cast<int>(p.view_instancing));
        engine_ddi::free_adapter_caps(caps);
    };
    reports(D3D12_VIEW_INSTANCING_TIER_1, D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED, "engine at tier 1");
    reports(D3D12_VIEW_INSTANCING_TIER_2, D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED, "engine at tier 2");
    reports(D3D12_VIEW_INSTANCING_TIER_3, D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED, "engine at tier 3");
    reports(static_cast<D3D12_VIEW_INSTANCING_TIER>(9), D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED,
            "engine at a tier above 3");
    reports(D3D12_VIEW_INSTANCING_TIER_2, D3D12DDI_VIEW_INSTANCING_TIER_2, "full-domain tier 2", 1, 2);
    reports(D3D12_VIEW_INSTANCING_TIER_3, D3D12DDI_VIEW_INSTANCING_TIER_2, "full-domain tier capped at 2", 1, 2);
    reports(D3D12_VIEW_INSTANCING_TIER_1, D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED, "insufficient adapter tier", 1, 2);
    reports(D3D12_VIEW_INSTANCING_TIER_2, D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED, "unknown contract revision", 2, 2);
    reports(D3D12_VIEW_INSTANCING_TIER_2, D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED, "unknown contract tier", 1, 9);
    {
        Answers a = fl12_0();
        a.options3.ViewInstancingTier = D3D12_VIEW_INSTANCING_TIER_2;
        a.unsupported = D3D12_FEATURE_D3D12_OPTIONS3;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine without an OPTIONS3 answer");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && p.view_instancing == D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED,
              "view instancing, engine without an OPTIONS3 answer: the zeroed structure is NOT_SUPPORTED (tier %d)",
              static_cast<int>(p.view_instancing));
        engine_ddi::free_adapter_caps(caps);
    }
}

// 1006 with one field of p back at the floor: everything else of 1006 must be what the floor engine answered.
bool rest_of_1006_same_but(const D3D12DDI_D3D12_OPTIONS_DATA_0089& p, const D3D12DDI_D3D12_OPTIONS_DATA_0089& floor,
                           bool render_pass, bool write_buffer_immediate, bool enhanced_barriers = false) {
    D3D12DDI_D3D12_OPTIONS_DATA_0089 rest = p;
    if (render_pass) rest.RenderPassTier = D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED;
    if (write_buffer_immediate) rest.WriteBufferImmediateQueueFlags = D3D12DDI_COMMAND_QUEUE_FLAG_NONE;
    if (enhanced_barriers) rest.EnhancedBarriersSupported = FALSE;
    return !std::memcmp(&rest, &floor, sizeof(rest));
}

// 1006 RenderPassTier is the engine's own OPTIONS5 answer, clamped to tier 2, the last tier the DDI defines at
// 0092, and render_pass_supported is that same answer: the shell fills the render pass table from it, so the cap
// and the table cannot disagree. The pinned engine answers tier 0, so nothing is reported today although both
// slots are filled (commands.cpp, begin_render_pass and end_render_pass).
void test_render_pass(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    check(!engine_ddi::render_pass_supported(nullptr), "render pass: null caps publish no table");
    D3D12DDI_D3D12_OPTIONS_DATA_0089 floor{};
    {
        engine_ddi::AdapterCaps* caps = query_stub(fl12_0(), info, "engine at render pass tier 0");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        floor = p.options;
        check(p.hr == S_OK && p.options.RenderPassTier == D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED &&
                  !engine_ddi::render_pass_supported(caps) && p.level == D3D12DDI_3DPIPELINELEVEL_12_0,
              "render pass, engine at tier 0: 1006 reports NOT_SUPPORTED, no table is published, 1074 still 12_0");
        engine_ddi::free_adapter_caps(caps);
    }
    const auto reports = [&](D3D12_RENDER_PASS_TIER engine, D3D12DDI_RENDER_PASS_TIER want, const char* what) {
        Answers a = fl12_0();
        a.options5.RenderPassesTier = engine;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, what);
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && p.options.RenderPassTier == want && engine_ddi::render_pass_supported(caps) &&
                  p.level == D3D12DDI_3DPIPELINELEVEL_12_0 &&
                  rest_of_1006_same_but(p.options, floor, true, false),
              "render pass, %s: 1006 reports tier %d, the table is published, nothing else of 1006 moved, 1074 "
              "stays 12_0 (tier %d)", what, static_cast<int>(want), static_cast<int>(p.options.RenderPassTier));
        engine_ddi::free_adapter_caps(caps);
    };
    reports(D3D12_RENDER_PASS_TIER_1, D3D12DDI_RENDER_PASS_TIER_1, "engine at tier 1");
    reports(D3D12_RENDER_PASS_TIER_2, D3D12DDI_RENDER_PASS_TIER_2, "engine at tier 2");
    reports(static_cast<D3D12_RENDER_PASS_TIER>(7), D3D12DDI_RENDER_PASS_TIER_2, "engine at a tier above 2");
    {
        Answers a = fl12_0();
        a.options5.RenderPassesTier = D3D12_RENDER_PASS_TIER_2;
        a.unsupported = D3D12_FEATURE_D3D12_OPTIONS5;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine without an OPTIONS5 answer");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && p.options.RenderPassTier == D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED &&
                  !engine_ddi::render_pass_supported(caps),
              "render pass, engine without an OPTIONS5 answer: the zeroed structure is NOT_SUPPORTED and no table "
              "is published (tier %d)", static_cast<int>(p.options.RenderPassTier));
        engine_ddi::free_adapter_caps(caps);
    }
}

// 1006 WriteBufferImmediateQueueFlags: the engine's OPTIONS3 answer, a mask of D3D12_COMMAND_LIST_TYPE bits,
// mapped to the DDI's queue flags. The three list types that have a queue of their own map one by one; the
// engine's BUNDLE bit and its video bits have no queue flag to go to. L56 is filled in both command list tables
// (commands.cpp), so every flag reported has a slot behind it.
void test_write_buffer_immediate(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    D3D12DDI_D3D12_OPTIONS_DATA_0089 floor{};
    {
        engine_ddi::AdapterCaps* caps = query_stub(fl12_0(), info, "engine with no immediate-write list type");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        floor = p.options;
        check(p.hr == S_OK && p.options.WriteBufferImmediateQueueFlags == D3D12DDI_COMMAND_QUEUE_FLAG_NONE,
              "write buffer immediate, engine at no list type: 1006 reports no queue flag");
        engine_ddi::free_adapter_caps(caps);
    }
    const auto reports = [&](UINT engine, UINT want, const char* what) {
        Answers a = fl12_0();
        a.options3.WriteBufferImmediateSupportFlags = static_cast<D3D12_COMMAND_LIST_SUPPORT_FLAGS>(engine);
        engine_ddi::AdapterCaps* caps = query_stub(a, info, what);
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && static_cast<UINT>(p.options.WriteBufferImmediateQueueFlags) == want &&
                  rest_of_1006_same_but(p.options, floor, false, true),
              "write buffer immediate, %s: 1006 reports queue flags 0x%x and nothing else of 1006 moved (0x%x)",
              what, want, static_cast<unsigned>(p.options.WriteBufferImmediateQueueFlags));
        engine_ddi::free_adapter_caps(caps);
    };
    // The pinned engine's own answer (device.c, d3d12_device_caps_init_feature_options3).
    reports(D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT | D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE |
                D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY | D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE,
            D3D12DDI_COMMAND_QUEUE_FLAG_3D | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE | D3D12DDI_COMMAND_QUEUE_FLAG_COPY,
            "engine at direct, compute, copy and bundle");
    reports(D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT, D3D12DDI_COMMAND_QUEUE_FLAG_3D, "engine at direct alone");
    reports(D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE | D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY,
            D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE | D3D12DDI_COMMAND_QUEUE_FLAG_COPY,
            "engine at compute and copy");
    reports(D3D12_COMMAND_LIST_SUPPORT_FLAG_BUNDLE, D3D12DDI_COMMAND_QUEUE_FLAG_NONE, "engine at bundle alone");
    reports(D3D12_COMMAND_LIST_SUPPORT_FLAG_VIDEO_DECODE | D3D12_COMMAND_LIST_SUPPORT_FLAG_VIDEO_PROCESS |
                D3D12_COMMAND_LIST_SUPPORT_FLAG_VIDEO_ENCODE,
            D3D12DDI_COMMAND_QUEUE_FLAG_NONE, "engine at the video list types alone");
    {
        Answers a = fl12_0();
        a.options3.WriteBufferImmediateSupportFlags = D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT;
        a.unsupported = D3D12_FEATURE_D3D12_OPTIONS3;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine without an OPTIONS3 answer");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && p.options.WriteBufferImmediateQueueFlags == D3D12DDI_COMMAND_QUEUE_FLAG_NONE,
              "write buffer immediate, engine without an OPTIONS3 answer: the zeroed structure is no queue flag "
              "(0x%x)", static_cast<unsigned>(p.options.WriteBufferImmediateQueueFlags));
        engine_ddi::free_adapter_caps(caps);
    }
}

// 1006 EnhancedBarriersSupported is the engine's OPTIONS12 answer and nothing else: FALSE when the engine says so,
// TRUE when it says so, FALSE when it does not answer OPTIONS12 at all, and in no case does it move the rest of
// 1006 or the feature level. L68 pfnBarrier is filled in both command list tables (resources.cpp), so the report
// has a slot behind it. The other field of OPTIONS12 engine-ddi maps, RelaxedFormatCastingSupported in 1077, is
// read at every step: it stays TRUE while enhanced barriers is cleared on its own, so the two fields are shown to
// be read independently, and it falls with enhanced barriers only when the whole structure goes unanswered.
void test_enhanced_barriers(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    // The floor engine of this test is the shared one with enhanced barriers cleared, so the two cases below can
    // be compared field by field against an otherwise identical answer.
    const auto relaxed_casting = [](const engine_ddi::AdapterCaps* caps) {
        D3D12DDI_OPTIONS_DATA_0090 o90;
        std::memset(&o90, 0xEE, sizeof(o90));
        return get(caps, D3D12DDICAPS_TYPE_OPTIONS_0090, o90) == S_OK && o90.RelaxedFormatCastingSupported;
    };
    D3D12DDI_D3D12_OPTIONS_DATA_0089 floor{};
    {
        Answers a = fl12_0();
        a.options12.EnhancedBarriersSupported = FALSE;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine without enhanced barriers");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        floor = p.options;
        check(p.hr == S_OK && !p.options.EnhancedBarriersSupported && relaxed_casting(caps) &&
                  p.level == D3D12DDI_3DPIPELINELEVEL_12_0,
              "enhanced barriers, engine at FALSE: 1006 reports FALSE, 1077 still reports relaxed format casting, "
              "1074 still 12_0 (hr %08lx)", static_cast<unsigned long>(p.hr));
        engine_ddi::free_adapter_caps(caps);
    }
    {
        engine_ddi::AdapterCaps* caps = query_stub(fl12_0(), info, "engine with enhanced barriers");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && p.options.EnhancedBarriersSupported && relaxed_casting(caps) &&
                  p.level == D3D12DDI_3DPIPELINELEVEL_12_0 &&
                  rest_of_1006_same_but(p.options, floor, false, false, true),
              "enhanced barriers, engine at TRUE: 1006 reports TRUE, 1077 still reports relaxed format casting, "
              "nothing else of 1006 moved, 1074 stays 12_0");
        engine_ddi::free_adapter_caps(caps);
    }
    {
        Answers a = fl12_0();
        a.unsupported = D3D12_FEATURE_D3D12_OPTIONS12;
        engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine without an OPTIONS12 answer");
        if (!caps) return;
        const PipelineCapAnswer p = pipeline_cap_answer(caps);
        check(p.hr == S_OK && !p.options.EnhancedBarriersSupported && !relaxed_casting(caps) &&
                  rest_of_1006_same_but(p.options, floor, false, false, false),
              "enhanced barriers, engine without an OPTIONS12 answer: the zeroed structure is FALSE in 1006 and in "
              "1077, and the rest of 1006 is the floor engine's answer");
        engine_ddi::free_adapter_caps(caps);
    }
}

// 1012 as a list, read into a buffer of 12 entries: S_OK, the count, and the entries in order.
struct ModelList { HRESULT hr; UINT count; D3D12DDI_SHADER_MODEL models[12]; };
ModelList model_list(const engine_ddi::AdapterCaps* caps, uint32_t ddi = kDdi) {
    ModelList l;
    std::memset(&l, 0xEE, sizeof(l));
    l.count = 12;
    D3D12DDI_D3D12_SHADER_MODELS_DATA_0011 d{&l.count, l.models};
    D3D12DDIARG_GETCAPS request{D3D12DDICAPS_TYPE_0011_SHADER_MODELS, nullptr, &d, sizeof(d)};
    l.hr = engine_ddi::build_caps(caps, ddi, &request);
    return l;
}

// The list from 5_1 up to `highest`, every release value in order, and nothing written past it.
bool list_is(const ModelList& l, D3D12DDI_SHADER_MODEL highest) {
    static const D3D12DDI_SHADER_MODEL all[] = {
        D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011, D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011, D3D12DDI_SHADER_MODEL_6_1_RELEASE_0033,
        D3D12DDI_SHADER_MODEL_6_2_RELEASE_0042, D3D12DDI_SHADER_MODEL_6_3_RELEASE_0054, D3D12DDI_SHADER_MODEL_6_4_RELEASE_0062,
        D3D12DDI_SHADER_MODEL_6_5_RELEASE_0071, D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082, D3D12DDI_SHADER_MODEL_6_7_RELEASE_0093,
        D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108, static_cast<D3D12DDI_SHADER_MODEL>(engine_ddi::rt12::kDdiShaderModel)};
    UINT n = 0;
    while (n < std::size(all) && all[n] <= highest) ++n;
    if (l.hr != S_OK || l.count != n) return false;
    for (UINT i = 0; i < n; ++i)
        if (l.models[i] != all[i]) return false;
    return untouched(l.models + n, sizeof(l.models) - n * sizeof(l.models[0]));
}

// 1091 SHADER_MODEL_6_8_OPTIONS_0110 (8 bytes): S_OK and both fields, packed as bits 0 and 1; -1 on a failure.
int options_1091(const engine_ddi::AdapterCaps* caps) {
    D3D12DDI_SHADER_MODEL_6_8_OPTIONS_0110 d;
    std::memset(&d, 0xEE, sizeof(d));
    if (get(caps, D3D12DDICAPS_TYPE_SHADER_MODEL_6_8_OPTIONS_0110, d) != S_OK) return -1;
    if ((d.SampleCmpGradientAndBiasSupported != 0 && d.SampleCmpGradientAndBiasSupported != 1) ||
        (d.ExtendedCommandInfoSupported != 0 && d.ExtendedCommandInfoSupported != 1))
        return -1;
    return d.SampleCmpGradientAndBiasSupported | (d.ExtendedCommandInfoSupported << 1);
}

// 1079 OPTIONS_0093 (8 bytes): S_OK and both fields, packed as bits 0 and 1; -1 on a failure.
int options_1079(const engine_ddi::AdapterCaps* caps) {
    D3D12DDI_OPTIONS_DATA_0093 d;
    std::memset(&d, 0xEE, sizeof(d));
    if (get(caps, D3D12DDICAPS_TYPE_OPTIONS_0093, d) != S_OK) return -1;
    if ((d.AdvancedTextureOpsSupported != 0 && d.AdvancedTextureOpsSupported != 1) ||
        (d.WriteableMSAATexturesSupported != 0 && d.WriteableMSAATexturesSupported != 1))
        return -1;
    return d.AdvancedTextureOpsSupported | (d.WriteableMSAATexturesSupported << 1);
}

// The shader model: the engine's answer to SHADER_MODEL asked with 6_8, the 1012 list up to it, 1091 from OPTIONS21
// at 6_8 only, the ceiling of the experiments shader-model-68-off and shader-model-67-off, and the refusals.
void test_shader_model(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    const auto engine_at = [](D3D_SHADER_MODEL model) {
        Answers a = fl12_0();
        a.max_model = model;
        a.options14.AdvancedTextureOpsSupported = TRUE;
        a.options14.WriteableMSAATexturesSupported = TRUE;
        a.options21.SampleCmpGradientAndBiasSupported = TRUE;
        a.options21.ExtendedCommandInfoSupported = TRUE;
        return a;
    };
    struct Case {
        D3D_SHADER_MODEL engine; D3D12DDI_SHADER_MODEL highest; int expected_1091; int expected_1079; const char* what;
    };
    const Case cases[] = {
        {D3D_SHADER_MODEL_6_5, D3D12DDI_SHADER_MODEL_6_5_RELEASE_0071, 0, 0, "engine at 6_5"},
        {D3D_SHADER_MODEL_6_6, D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082, 0, 0, "engine at 6_6"},
        // 1079: both FALSE even with both OPTIONS14 fields TRUE (integer border colours need DDI 0096).
        {D3D_SHADER_MODEL_6_7, D3D12DDI_SHADER_MODEL_6_7_RELEASE_0093, 0, 0, "engine at 6_7"},
        {D3D_SHADER_MODEL_6_8, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108, 3, 0, "engine at 6_8"},
        {D3D_SHADER_MODEL_6_9, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108, 3, 0, "engine at 6_9, tier 1.2 switch off"},
    };
    for (const Case& k : cases) {
        const Answers a = engine_at(k.engine);
        engine_ddi::AdapterCaps* caps = query_stub(a, info, k.what);
        if (!caps) continue;
        const ModelList l = model_list(caps);
        const int o = options_1091(caps);
        const int o79 = options_1079(caps);
        check(list_is(l, k.highest) && o == k.expected_1091 && o79 == k.expected_1079,
              "shader model, %s: 1012 lists 5_1 to %x (count %u), 1091 SampleCmpGradientAndBias %d, "
              "ExtendedCommandInfo %d, 1079 AdvancedTextureOps %d, WriteableMSAATextures %d",
              k.what, static_cast<unsigned>(k.highest), l.count, o & 1, (o >> 1) & 1, o79 & 1, (o79 >> 1) & 1);
        engine_ddi::free_adapter_caps(caps);
    }

    // 1091 with the engine at 6_8: each field from OPTIONS21 alone, and FALSE when OPTIONS21 is unanswered.
    Answers a = engine_at(D3D_SHADER_MODEL_6_8);
    a.options21.SampleCmpGradientAndBiasSupported = FALSE;
    engine_ddi::AdapterCaps* caps = query_stub(a, info, "engine at 6_8 without SampleCmpGradientAndBias");
    if (caps) {
        check(options_1091(caps) == 2, "1091: SampleCmpGradientAndBias FALSE as the engine says, ExtendedCommandInfo TRUE");
        engine_ddi::free_adapter_caps(caps);
    }
    a = engine_at(D3D_SHADER_MODEL_6_8);
    a.unsupported = D3D12_FEATURE_D3D12_OPTIONS21;
    caps = query_stub(a, info, "engine at 6_8 without OPTIONS21");
    if (caps) {
        check(list_is(model_list(caps), D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108) && options_1091(caps) == 0,
              "1091: an unanswered OPTIONS21 reports both FALSE; 1012 still lists 6_8");
        engine_ddi::free_adapter_caps(caps);
    }

    // 1079 with OPTIONS14 unanswered: also both FALSE, S_OK.
    a = engine_at(D3D_SHADER_MODEL_6_7);
    a.unsupported = D3D12_FEATURE_D3D12_OPTIONS14;
    caps = query_stub(a, info, "engine at 6_7 without OPTIONS14");
    if (caps) {
        check(options_1079(caps) == 0, "1079: an unanswered OPTIONS14 reports both FALSE");
        engine_ddi::free_adapter_caps(caps);
    }

    // 1079 exists from 0093 on: exact 8 bytes, refused at 0092, nothing written on a refusal.
    caps = query_stub(engine_at(D3D_SHADER_MODEL_6_8), info, "engine at 6_8, 1079 payload");
    if (caps) {
        uint8_t raw[16];
        std::memset(raw, 0xEE, sizeof(raw));
        D3D12DDIARG_GETCAPS r{D3D12DDICAPS_TYPE_OPTIONS_0093, nullptr, raw, 4};
        const HRESULT hr4 = engine_ddi::build_caps(caps, kDdi, &r);
        r.DataSize = 12;
        const HRESULT hr12 = engine_ddi::build_caps(caps, kDdi, &r);
        r.DataSize = 8;
        const HRESULT hr92 = engine_ddi::build_caps(caps, D3D12DDI_BUILD_VERSION_0092, &r);
        check(hr4 == E_INVALIDARG && hr12 == E_INVALIDARG && hr92 == E_INVALIDARG && untouched(raw, sizeof(raw)),
              "1079 with 4 and 12 bytes, and at ddi_version 92: E_INVALIDARG, nothing written");
        const HRESULT hr93 = engine_ddi::build_caps(caps, D3D12DDI_BUILD_VERSION_0093, &r);
        D3D12DDI_OPTIONS_DATA_0093 d93;
        std::memcpy(&d93, raw, sizeof(d93));
        check(hr93 == S_OK && !d93.AdvancedTextureOpsSupported && !d93.WriteableMSAATexturesSupported,
              "1079 at ddi_version 93: answered, both FALSE (%08lx)", static_cast<unsigned long>(hr93));
        engine_ddi::free_adapter_caps(caps);
    }

    // The payload size is exact: 4 and 12 bytes are refused with nothing written.
    caps = query_stub(engine_at(D3D_SHADER_MODEL_6_8), info, "engine at 6_8, payload sizes");
    if (caps) {
        uint8_t raw[16];
        std::memset(raw, 0xEE, sizeof(raw));
        D3D12DDIARG_GETCAPS r{D3D12DDICAPS_TYPE_SHADER_MODEL_6_8_OPTIONS_0110, nullptr, raw, 4};
        const HRESULT hr4 = engine_ddi::build_caps(caps, kDdi, &r);
        r.DataSize = 12;
        const HRESULT hr12 = engine_ddi::build_caps(caps, kDdi, &r);
        check(hr4 == E_INVALIDARG && hr12 == E_INVALIDARG && untouched(raw, sizeof(raw)),
              "1091 with 4 and 12 bytes: E_INVALIDARG, nothing written");

        // The ceiling: 6_7 (shader-model-68-off), 6_6 (shader-model-67-off, the answer before 2026-10-07), back to 6_8.
        const HRESULT h67 = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_7);
        const bool at67 = list_is(model_list(caps), D3D12DDI_SHADER_MODEL_6_7_RELEASE_0093) && options_1091(caps) == 0;
        const HRESULT h66 = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_6);
        const bool at66 = list_is(model_list(caps), D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082) && options_1091(caps) == 0 &&
                          options_1079(caps) == 0;
        const HRESULT h68 = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_8);
        const bool at68 = list_is(model_list(caps), D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108) && options_1091(caps) == 3;
        check(h67 == S_OK && at67 && h66 == S_OK && at66 && h68 == S_OK && at68,
              "ceiling: 6_7 lists to 6_7 with 1091 FALSE, 6_6 lists to 6_6 (the answer before), 6_8 restores 6_8");

        // Refused ceilings keep the one held before (6_8): a model between rows, one above the table, a null caps.
        const HRESULT bad52 = engine_ddi::set_shader_model_ceiling(caps, static_cast<D3D_SHADER_MODEL>(0x52));
        const HRESULT h69 = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_9);
        const bool at69 = list_is(model_list(caps), D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108);
        check(h69 == S_OK && at69, "ceiling 6_9 accepted; with the tier 1.2 switch off the list still ends at 6_8");
        engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_8);
        const HRESULT bad6a = engine_ddi::set_shader_model_ceiling(caps, static_cast<D3D_SHADER_MODEL>(0x6A));
        const HRESULT bad0 = engine_ddi::set_shader_model_ceiling(caps, static_cast<D3D_SHADER_MODEL>(0));
        const HRESULT null_caps = engine_ddi::set_shader_model_ceiling(nullptr, D3D_SHADER_MODEL_6_6);
        check(bad52 == E_INVALIDARG && bad6a == E_INVALIDARG && bad0 == E_INVALIDARG && null_caps == E_INVALIDARG &&
                  list_is(model_list(caps), D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108),
              "ceiling refused: 0x52, 0x6A, 0 and a null caps; the ceiling held before stays");
        engine_ddi::free_adapter_caps(caps);
    }

    // A ceiling never raises the engine's answer.
    caps = query_stub(engine_at(D3D_SHADER_MODEL_6_6), info, "engine at 6_6, ceiling 6_8");
    if (caps) {
        const HRESULT h = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_8);
        check(h == S_OK && list_is(model_list(caps), D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082) && options_1091(caps) == 0,
              "ceiling 6_8 on an engine at 6_6: 1012 still ends at 6_6, 1091 FALSE");
        engine_ddi::free_adapter_caps(caps);
    }

    // An engine answer that is not an answer to the request: above 6_9, below 5_1, or between two models.
    const UINT bad_answers[] = {0x50, 0x52, 0x6A};
    for (const UINT bad : bad_answers) {
        Answers b = engine_at(D3D_SHADER_MODEL_6_8);
        b.raw_model = bad;
        g_answers = &b;
        const BC250_VKD3D_ENGINE_FUNCS funcs = stub_funcs();
        engine_ddi::AdapterCaps* out = reinterpret_cast<engine_ddi::AdapterCaps*>(uintptr_t{1});
        const HRESULT hr = engine_ddi::query_adapter_caps(&funcs, &info, &out);
        check(hr == E_UNEXPECTED && !out, "an engine answering shader model %x is refused (hr %08lx)", bad,
              static_cast<unsigned long>(hr));
    }
}

// Raytracing tier 1.2 and shader model 6.9: off in the library, and with the switch on only when every condition
// holds (the engine at 6_9 and 1_2, the 6.9 floor of OPTIONS1 and OPTIONS4, raytracing reporting on, a negotiated
// build version of 0094 or higher). At 0092 and 0093 the answers are 6_8 and 1_1. The shell turns the switch on
// (adapter-caps.cpp).
void test_raytracing_tier12(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    constexpr auto kTier12 = static_cast<D3D12DDI_RAYTRACING_TIER>(engine_ddi::rt12::kTier);
    constexpr auto kSm69 = static_cast<D3D12DDI_SHADER_MODEL>(engine_ddi::rt12::kDdiShaderModel);
    struct Result { HRESULT hr; bool listed; D3D12DDI_RAYTRACING_TIER tier; };
    const auto read = [](engine_ddi::AdapterCaps* caps, uint32_t ddi, D3D12DDI_SHADER_MODEL highest) {
        D3D12DDI_D3D12_OPTIONS_DATA_0089 o{};
        D3D12DDIARG_GETCAPS r{D3D12DDICAPS_TYPE_D3D12_OPTIONS, nullptr, &o, sizeof(o)};
        const HRESULT hr = engine_ddi::build_caps(caps, ddi, &r);
        return Result{hr, list_is(model_list(caps, ddi), highest), o.RaytracingTier};
    };
    const auto engine_12 = [] {
        Answers a = fl12_0();
        a.max_model = D3D_SHADER_MODEL_6_9;
        a.options5.RaytracingTier = static_cast<D3D12_RAYTRACING_TIER>(engine_ddi::rt12::kTier);
        return a;
    };
    engine_ddi::AdapterCaps* caps = query_stub(engine_12(), info, "engine at 6_9 and 1_2");
    if (caps) {
        const Result off = read(caps, kDdi, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108);
        check(off.hr == S_OK && off.listed && off.tier == D3D12DDI_RAYTRACING_TIER_1_1,
              "tier 1.2 switch off (the library default): 1012 ends at 6_8, 1006 reports 1_1 (%d)",
              static_cast<int>(off.tier));
        const HRESULT on = engine_ddi::set_raytracing_tier12_reporting(caps, true);
        const Result at = read(caps, kDdi, kSm69);
        check(on == S_OK && at.hr == S_OK && at.listed && at.tier == kTier12,
              "tier 1.2 switch on, at 0094: 1012 ends at 6_9 (%x), 1006 reports 1_2 (%d)",
              static_cast<unsigned>(kSm69), static_cast<int>(at.tier));
        // A runtime that negotiates 0092 or 0093 does not know tier 1.2: the 22621 system core negotiates 0092 and
        // refuses opacity micromap pipelines.
        for (const uint32_t ddi : {uint32_t{D3D12DDI_BUILD_VERSION_0092}, uint32_t{D3D12DDI_BUILD_VERSION_0093}}) {
            const Result below = read(caps, ddi, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108);
            check(below.hr == S_OK && below.listed && below.tier == D3D12DDI_RAYTRACING_TIER_1_1,
                  "tier 1.2 switch on, at ddi_version %u: 1012 ends at 6_8, 1006 reports 1_1 (%d)",
                  static_cast<unsigned>(ddi), static_cast<int>(below.tier));
        }
        engine_ddi::set_raytracing_tier_reporting(caps, false);
        const Result rt_off = read(caps, kDdi, kSm69);
        engine_ddi::set_raytracing_tier_reporting(caps, true);
        check(rt_off.hr == S_OK && rt_off.tier == D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED,
              "tier 1.2 switch on, raytracing reporting off: 1006 reports NOT_SUPPORTED (%d)",
              static_cast<int>(rt_off.tier));
        const HRESULT h68 = engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_8);
        const Result ceiling = read(caps, kDdi, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108);
        check(h68 == S_OK && ceiling.listed && ceiling.tier == D3D12DDI_RAYTRACING_TIER_1_1,
              "tier 1.2 switch on, ceiling 6_8: 1012 ends at 6_8, 1006 reports 1_1 (%d)",
              static_cast<int>(ceiling.tier));
        engine_ddi::set_shader_model_ceiling(caps, D3D_SHADER_MODEL_6_9);
        const HRESULT back = engine_ddi::set_raytracing_tier12_reporting(caps, false);
        const Result again = read(caps, kDdi, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108);
        check(back == S_OK && again.listed && again.tier == D3D12DDI_RAYTRACING_TIER_1_1,
              "tier 1.2 switch off again: 6_8 and 1_1 (%d)", static_cast<int>(again.tier));
        engine_ddi::free_adapter_caps(caps);
    }
    // Each missing condition keeps 1_1; a missing 6.9 floor or an engine below 6_9 also keeps 6_8.
    struct Case { const char* what; void (*change)(Answers&); D3D12DDI_SHADER_MODEL highest; };
    const Case cases[] = {
        {"engine at 1_1", [](Answers& a) { a.options5.RaytracingTier = D3D12_RAYTRACING_TIER_1_1; }, kSm69},
        {"engine at 6_8", [](Answers& a) { a.max_model = D3D_SHADER_MODEL_6_8; },
         D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108},
        {"no WaveOps", [](Answers& a) { a.options1.WaveOps = FALSE; }, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108},
        {"no Int64ShaderOps", [](Answers& a) { a.options1.Int64ShaderOps = FALSE; },
         D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108},
        {"no Native16BitShaderOps", [](Answers& a) { a.options4.Native16BitShaderOpsSupported = FALSE; },
         D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108},
    };
    for (const Case& k : cases) {
        Answers a = engine_12();
        k.change(a);
        caps = query_stub(a, info, k.what);
        if (!caps) continue;
        const HRESULT on = engine_ddi::set_raytracing_tier12_reporting(caps, true);
        const Result r = read(caps, kDdi, k.highest);
        check(on == S_OK && r.hr == S_OK && r.listed && r.tier == D3D12DDI_RAYTRACING_TIER_1_1,
              "tier 1.2 switch on, %s: 1012 ends at %x, 1006 reports 1_1 (%d)", k.what,
              static_cast<unsigned>(k.highest), static_cast<int>(r.tier));
        engine_ddi::free_adapter_caps(caps);
    }
    check(engine_ddi::set_raytracing_tier12_reporting(nullptr, true) == E_INVALIDARG,
          "tier 1.2 switch refused: null caps");
}

void test_policy_stub(const BC250_VKD3D_DEVICE_CREATE_INFO& info) {
    // An engine with heap serialization tier 0, so that every field can be overridden alone: UMA TRUE,
    // CacheCoherentUMA FALSE, heap tier 0 (DDI 0), and the constants IOCoherent FALSE, resource tier 0.
    Answers a = fl12_0();
    a.serialization.HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
    engine_ddi::AdapterCaps* caps = query_stub(a, info, "policy, heap tier 0 engine");
    if (!caps) return;
    Snapshot base;
    const bool base_ok = snapshot(caps, base);
    check(base_ok && base.mem.UMA == TRUE && base.mem.CacheCoherent == FALSE && base.mem.IOCoherent == FALSE &&
              base.mem.HeapSerializationTier == D3D12DDI_HEAP_SERIALIZATION_TIER_0041_0 &&
              base.mem.ResourceSerializationTier == D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_0,
          "policy: without a policy 1002 is the engine's answer and the constants (UMA 1, CacheCoherent 0, "
          "IOCoherent 0, tiers 0 and 0)");

    Mem m;
    MemoryArchitecturePolicy p = policy();
    HRESULT hr = apply(caps, p, m);
    Snapshot after;
    check(hr == S_OK && same(m, base.mem) && snapshot(caps, after) && others_same(base, after),
          "policy: an all-Default policy leaves 1002, 1005 and 1006 byte for byte as without a policy");

    // Each field alone; every other field keeps its answer.
    struct One { const char* what; void (*set)(MemoryArchitecturePolicy&); void (*expect)(Mem&); };
    const One ones[] = {
        {"UMA False", [](MemoryArchitecturePolicy& q) { q.uma = PolicyBool::False; }, [](Mem& e) { e.UMA = FALSE; }},
        {"CacheCoherent True", [](MemoryArchitecturePolicy& q) { q.cache_coherent = PolicyBool::True; },
         [](Mem& e) { e.CacheCoherent = TRUE; }},
        {"IOCoherent True", [](MemoryArchitecturePolicy& q) { q.io_coherent = PolicyBool::True; },
         [](Mem& e) { e.IOCoherent = TRUE; }},
        {"HeapSerializationTier 0 explicit (the engine's value)",[](MemoryArchitecturePolicy& q) { q.heap_serialization_tier = {1, 0}; },
         [](Mem& e) { e.HeapSerializationTier = D3D12DDI_HEAP_SERIALIZATION_TIER_0041_0; }},
        {"ResourceSerializationTier 1", [](MemoryArchitecturePolicy& q) { q.resource_serialization_tier = {1, 1}; },
         [](Mem& e) { e.ResourceSerializationTier = D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_1; }},
        {"ResourceSerializationTier 2", [](MemoryArchitecturePolicy& q) { q.resource_serialization_tier = {1, 2}; },
         [](Mem& e) { e.ResourceSerializationTier = D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2; }},
    };
    for (const One& one : ones) {
        p = policy();
        one.set(p);
        Mem expected = base.mem;
        one.expect(expected);
        hr = apply(caps, p, m);
        check(hr == S_OK && same(m, expected) && snapshot(caps, after) && others_same(base, after),
              "policy: %s alone changes that field of 1002 only (UMA %d, CacheCoherent %d, IOCoherent %d, tiers %d %d)",
              one.what, m.UMA, m.CacheCoherent, m.IOCoherent, static_cast<int>(m.HeapSerializationTier),
              static_cast<int>(m.ResourceSerializationTier));
    }

    // Every field explicit, the other way round from the engine where the rules allow it.
    p = policy();
    p.uma = PolicyBool::True;
    p.cache_coherent = PolicyBool::True;
    p.io_coherent = PolicyBool::True;
    p.heap_serialization_tier = {1, 1};
    p.resource_serialization_tier = {1, 2};
    hr = apply(caps, p, m);
    check(hr == S_OK && m.UMA == TRUE && m.CacheCoherent == TRUE && m.IOCoherent == TRUE &&
              m.HeapSerializationTier == D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1 &&
              m.ResourceSerializationTier == D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2,
          "policy: every field explicit (UMA, CacheCoherent, IOCoherent TRUE, tiers 1 and 2) is answered as set");
    const MemoryArchitecturePolicy all_explicit = p;
    const Mem kept = m;

    // Refusals keep the policy held before (all_explicit).
    struct Bad { const char* what; void (*set)(MemoryArchitecturePolicy&); };
    const Bad bads[] = {
        {"size 0", [](MemoryArchitecturePolicy& q) { q.size = 0; }},
        {"size + 4", [](MemoryArchitecturePolicy& q) { q.size = sizeof(q) + 4; }},
        {"UMA 3", [](MemoryArchitecturePolicy& q) { q.uma = static_cast<PolicyBool>(3); }},
        {"CacheCoherent 3", [](MemoryArchitecturePolicy& q) { q.cache_coherent = static_cast<PolicyBool>(3); }},
        {"IOCoherent 0xFFFFFFFF", [](MemoryArchitecturePolicy& q) { q.io_coherent = static_cast<PolicyBool>(~0u); }},
        {"heap tier set 2", [](MemoryArchitecturePolicy& q) { q.heap_serialization_tier = {2, 0}; }},
        {"heap tier Default with value 1", [](MemoryArchitecturePolicy& q) { q.heap_serialization_tier = {0, 1}; }},
        {"heap tier 2 (undefined at 0092)", [](MemoryArchitecturePolicy& q) { q.heap_serialization_tier = {1, 2}; }},
        {"resource tier 3 (undefined at 0092)",
         [](MemoryArchitecturePolicy& q) { q.resource_serialization_tier = {1, 3}; }},
        {"CacheCoherent True with UMA False",
         [](MemoryArchitecturePolicy& q) { q.uma = PolicyBool::False; q.cache_coherent = PolicyBool::True; }},
        {"heap tier 1 with resource tier Default (0)",
         [](MemoryArchitecturePolicy& q) { q.heap_serialization_tier = {1, 1}; }},
        {"heap tier 1 with resource tier 1",
         [](MemoryArchitecturePolicy& q) { q.heap_serialization_tier = {1, 1}; q.resource_serialization_tier = {1, 1}; }},
    };
    for (const Bad& bad : bads) {
        p = policy();
        bad.set(p);
        hr = apply(caps, p, m);
        check(hr == E_INVALIDARG && same(m, kept), "policy refused: %s; the policy held before stays (hr %08lx)",
              bad.what, static_cast<unsigned long>(hr));
    }
    check(engine_ddi::set_memory_architecture_policy(nullptr, &all_explicit) == E_INVALIDARG &&
              engine_ddi::set_memory_architecture_policy(caps, nullptr) == E_INVALIDARG && get_1002(caps, m) == S_OK &&
              same(m, kept),
          "policy refused: null caps or null policy");

    // Back to all Default: the answer without a policy again.
    hr = apply(caps, policy(), m);
    check(hr == S_OK && same(m, base.mem), "policy: an all-Default policy after others restores the engine's answer");
    engine_ddi::free_adapter_caps(caps);

    // The rules judge the resulting answer, Defaults resolved against the engine: an engine with cache-coherent UMA
    // refuses UMA False alone, and accepts IOCoherent alone.
    a = fl12_0();
    a.serialization.HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
    a.architecture.CacheCoherentUMA = TRUE;
    caps = query_stub(a, info, "policy, cache-coherent UMA engine");
    if (caps) {
        Mem before;
        get_1002(caps, before);
        p = policy();
        p.uma = PolicyBool::False;
        hr = apply(caps, p, m);
        const bool refused = hr == E_INVALIDARG && same(m, before);
        p = policy();
        p.io_coherent = PolicyBool::True;
        hr = apply(caps, p, m);
        Mem expected = before;
        expected.IOCoherent = TRUE;
        check(refused && hr == S_OK && same(m, expected),
              "policy: UMA False alone on a cache-coherent UMA engine is refused; IOCoherent True alone is accepted");
        engine_ddi::free_adapter_caps(caps);
    }

    // An engine without UMA: IOCoherent True alone is accepted (I/O coherence is not a UMA property).
    a = fl12_0();
    a.serialization.HeapSerializationTier = D3D12_HEAP_SERIALIZATION_TIER_0;
    a.architecture.UMA = FALSE;
    a.architecture.CacheCoherentUMA = FALSE;
    caps = query_stub(a, info, "policy, engine without UMA");
    if (caps) {
        Mem before;
        get_1002(caps, before);
        p = policy();
        p.io_coherent = PolicyBool::True;
        hr = apply(caps, p, m);
        Mem expected = before;
        expected.IOCoherent = TRUE;
        check(hr == S_OK && same(m, expected) && !m.UMA && !m.CacheCoherent,
              "policy: IOCoherent True alone with the engine's UMA FALSE is accepted and changes IOCoherent only");
        engine_ddi::free_adapter_caps(caps);
    }

    // The fl12_0 engine reports heap serialization tier 10 (DDI 1) with the constant resource tier 0. Without a policy
    // that stays as it was; a policy's resulting answer may not keep that pair, whatever field the policy sets.
    caps = query_stub(fl12_0(), info, "policy, heap tier 10 engine");
    if (caps) {
        Mem before;
        get_1002(caps, before);
        p = policy();
        p.io_coherent = PolicyBool::True;
        hr = apply(caps, p, m);
        const bool refused = hr == E_INVALIDARG && same(m, before);
        p.resource_serialization_tier = {1, 2};
        hr = apply(caps, p, m);
        check(before.HeapSerializationTier == D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1 && refused && hr == S_OK &&
                  m.IOCoherent && m.ResourceSerializationTier == D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2,
              "policy: on a heap tier 1 engine IOCoherent alone is refused (heap tier 1 without resource tier 2) and "
              "accepted with resource tier 2");
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
    HRESULT hr = get_funcs(kAbi, &funcs);
    check(hr == S_OK && funcs.AbiVersion >= kAbi && funcs.QueryAdapterCaps,
          "GetFuncs fills QueryAdapterCaps (hr %08lx, engine ABI %u.%u)", static_cast<unsigned long>(hr),
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
    info.InstanceMode = BC250_VKD3D_INSTANCE_MODE_PRIVATE;
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
    const ModelList l = model_list(caps);
    const int o1091 = options_1091(caps);
    check(l.hr == S_OK && l.count >= 1 && l.count <= 10 && o1091 >= 0,
          "1012 ends at %x (%u models); 1091 SampleCmpGradientAndBias %d, ExtendedCommandInfo %d",
          l.hr == S_OK && l.count >= 1 && l.count <= 10 ? static_cast<unsigned>(l.models[l.count - 1]) : 0u, l.count,
          o1091 & 1, (o1091 >> 1) & 1);
    check_layout_sets(caps, "engine");
    check_none_types(caps, "engine");
    engine_ddi::free_adapter_caps(caps);
    return 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    // engine-ddi's log sink is stderr here (log_line, AMDGPU_WDDM_LOG=stderr). The refusals this test provokes log
    // there by design; they are expected output, not failures, so they go to stdout in order with the ok/FAIL lines.
    // The verdict is the exit code and the PASSED/FAILED line.
    setvbuf(stdout, nullptr, _IONBF, 0);
    _dup2(_fileno(stdout), _fileno(stderr));
    SetEnvironmentVariableA("AMDGPU_WDDM_LOG", "stderr");
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
        info.InstanceMode = BC250_VKD3D_INSTANCE_MODE_PRIVATE;
        test_stub(info);
        test_policy_stub(info);
        test_raytracing_tier_reporting(info);
        test_mesh_shader_tier(info);
        test_depth_bounds(info);
        test_view_instancing(info);
        test_render_pass(info);
        test_write_buffer_immediate(info);
        test_enhanced_barriers(info);
        test_shader_model(info);
        test_raytracing_tier12(info);
    }
    std::printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
