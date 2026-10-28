// SPDX-License-Identifier: MIT
// engine-ddi: the adapter caps path of GetCaps. query_adapter_caps asks the engine once (QueryAdapterCaps, engine
// ABI 1.2, V11) and keeps its answers; build_caps answers each GetCaps from them. Where each value comes from is
// the table in INTEGRATION.md ("GetCaps"): a D3D12_FEATURE_* answer of the engine wherever a 1:1 field exists,
// otherwise a documented constant; for 1002 the shell's memory architecture policy on top
// (set_memory_architecture_policy). Layout references are WDK 10.0.26100 um\d3d12umddi.h ("H:line").
#include "internal.h"
#include "tight-alignment.h"
#include <cstring>
#include <cwchar>
#include <iterator>

namespace engine_ddi {

// ---- Payload layouts ------------------------------------------------------------------------------------------------
static_assert(sizeof(D3D12DDIARG_GETCAPS) == (sizeof(void*) == 8 ? 32 : 16), "H:2611-2617, x64 and x86");
static_assert(sizeof(D3D12DDI_3DPIPELINESUPPORT1_DATA_0081) == 8, "1074: H:10415-10420");
static_assert(sizeof(D3D12DDI_3DPIPELINELEVEL) == 4, "1007: the level itself, H:2922-2933");
static_assert(sizeof(D3D12DDI_D3D12_SHADER_MODELS_DATA_0011) == 2 * sizeof(void*), "1012: H:3502-3507, two pointers");
static_assert(sizeof(D3D12DDI_D3D12_OPTIONS_DATA_0089) == 124, "1006: H:11078-11112");
static_assert(sizeof(D3D12DDI_SHADER_CAPS_0084) == 64, "1004: H:10515-10534");
static_assert(sizeof(D3D12DDI_ARCHITECTURE_INFO_DATA) == 4, "1005: H:2916-2920");
static_assert(sizeof(D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041) == 20, "1002: H:6806-6814");
static_assert(sizeof(D3D12DDI_GPUVA_CAPS_0004) == 4, "1009: H:250-257");
static_assert(sizeof(D3D12DDI_TEXTURE_LAYOUT_CAPS_0026) == 20, "1060: H:5525-5536");
static_assert(sizeof(D3D12DDI_ROW_MAJOR_LAYOUT_CAPS) == 20, "1003: H:280-292, SubCaps[2] of 8 bytes and Flags");
static_assert(sizeof(D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_DATA_0030) == 8, "1057: H:13697-13701");
static_assert(sizeof(BOOL) == 4, "1069 and 1071: pData = BOOL, H:128, H:131");
static_assert(sizeof(D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023) == 4, "1062: H:5140-5143");
static_assert(sizeof(D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050) == 4, "1067: H:7004-7008");
static_assert(sizeof(D3D12DDI_OPTIONS_DATA_0090) == 4, "1077: H:11127-11131");
static_assert(sizeof(D3D12DDI_OPTIONS_DATA_0091) == 16, "1078: H:11143-11150");
static_assert(sizeof(D3D12DDI_OPTIONS_DATA_0093) == 8, "1079: H:11254-11259");
static_assert(sizeof(D3D12DDI_SHADER_MODEL_6_8_OPTIONS_0110) == 8, "1091: H:13671-13679");

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
// Tier 1 is the only mesh shader tier at 0092 (H:9353-9357), and the DDI and API values are equal.
static_assert(D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED == static_cast<int>(D3D12_MESH_SHADER_TIER_NOT_SUPPORTED) &&
              D3D12DDI_MESH_SHADER_TIER_1 == static_cast<int>(D3D12_MESH_SHADER_TIER_1), "mesh shader tier");
// The view instancing tiers at 0092 are NOT_SUPPORTED to 3 (H:6370-6376), the same four values as the API's.
static_assert(D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED ==
                  static_cast<int>(D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED) &&
              D3D12DDI_VIEW_INSTANCING_TIER_1 == static_cast<int>(D3D12_VIEW_INSTANCING_TIER_1) &&
              D3D12DDI_VIEW_INSTANCING_TIER_2 == static_cast<int>(D3D12_VIEW_INSTANCING_TIER_2) &&
              D3D12DDI_VIEW_INSTANCING_TIER_3 == static_cast<int>(D3D12_VIEW_INSTANCING_TIER_3),
              "view instancing tier");
// The render pass tiers at 0092 are NOT_SUPPORTED, 1 and 2 (H:7645-7652); the API's three values are equal to
// them. The DDI's NOT_SUPPORTED and the API's TIER_0 are the same thing: the runtime emulates render passes.
static_assert(D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED == static_cast<int>(D3D12_RENDER_PASS_TIER_0) &&
                  D3D12DDI_RENDER_PASS_TIER_1 == static_cast<int>(D3D12_RENDER_PASS_TIER_1) &&
                  D3D12DDI_RENDER_PASS_TIER_2 == static_cast<int>(D3D12_RENDER_PASS_TIER_2),
              "render pass tier");
// The engine answers which list types take an immediate write as a D3D12_COMMAND_LIST_SUPPORT_FLAGS mask, one bit
// per D3D12_COMMAND_LIST_TYPE; the DDI field is a queue-flag mask (H:11091, values H:1435-1445), so the two are
// mapped bit by bit in write_buffer_immediate_queues below.
static_assert(D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT == (1 << D3D12_COMMAND_LIST_TYPE_DIRECT) &&
                  D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE == (1 << D3D12_COMMAND_LIST_TYPE_COMPUTE) &&
                  D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY == (1 << D3D12_COMMAND_LIST_TYPE_COPY),
              "command list support flags");

class AdapterCaps {
public:
    D3D12_FEATURE_DATA_FEATURE_LEVELS levels;
    D3D12_FEATURE_DATA_SHADER_MODEL shader_model;
    D3D12_FEATURE_DATA_ARCHITECTURE1 architecture;
    D3D12_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT gpu_va;
    D3D12_FEATURE_DATA_D3D12_OPTIONS options;
    D3D12_FEATURE_DATA_D3D12_OPTIONS1 options1;
    D3D12_FEATURE_DATA_D3D12_OPTIONS2 options2;   // DepthBoundsTestSupported (ProgrammableSamplePositionsTier: E)
    D3D12_FEATURE_DATA_D3D12_OPTIONS3 options3;
    BC250_VKD3D_VIEW_INSTANCING_CONTRACT view_contract;
    D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4;
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5;
    D3D12_FEATURE_DATA_D3D12_OPTIONS7 options7;   // MeshShaderTier, SamplerFeedbackTier
    D3D12_FEATURE_DATA_D3D12_OPTIONS9 options9;
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11;
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 options12;
    D3D12_FEATURE_DATA_D3D12_OPTIONS13 options13;
    D3D12_FEATURE_DATA_D3D12_OPTIONS14 options14;   // logged only: 1079 reports both FALSE (caps.cpp)
#if defined(D3D12DDI_BUILD_VERSION_0119)
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 options19;
    tight::Support tight_alignment{};
    bool options19_answered{};
#endif
    D3D12_FEATURE_DATA_D3D12_OPTIONS21 options21;   // 1091
    D3D12_FEATURE_DATA_SERIALIZATION serialization;
    D3D12_FEATURE_DATA_SHADER_CACHE shader_cache;   // AUTOMATIC_DISK_CACHE: DriverManagedShaderCachePresent
    MemoryArchitecturePolicy memory_policy;     // all Default until set_memory_architecture_policy
    bool partial_graphics_programs{};
    bool report_raytracing_tier{true};          // the driver's default; set_raytracing_tier_reporting takes it back
    // Tier 1.2 and shader model 6.9: off in the library, set_raytracing_tier12_reporting turns them on (the shell
    // does; only a lab-flavour shell has an off switch, the experiment raytracing-tier12-off). Off, the answers are
    // 6_8 and 1_1 at most.
    bool report_raytracing_tier12{false};
    D3D_SHADER_MODEL shader_model_ceiling{D3D_SHADER_MODEL_6_9};   // set_shader_model_ceiling lowers it
};

namespace {
constexpr uint32_t kAbi12 = (1u << 16) | 2u;
// The payload layouts build_caps writes: 0093 adds type 1079 and changes no 0092 layout (H:11247-11259); 0094 adds
// no caps type and changes only the L68 barrier argument (H:11262-11301).
constexpr uint32_t kDdiVersionMin = 92;                  // D3D12DDI_BUILD_VERSION_0092, H:11156
#if defined(D3D12DDI_BUILD_VERSION_0119)
// 0120 (ddi-ladder.h, no public header defines it) changes no caps payload of 0119: the newest build that the
// ladder knows is the newest layout build_caps takes.
constexpr uint32_t kDdiVersionMax = native12::kDdiMax;
static_assert(kDdiVersionMax == 122, "both flavours implement the 0122 contract");
#else
constexpr uint32_t kDdiVersionMax = 94;                  // D3D12DDI_BUILD_VERSION_0094, H:11265
#endif
// The lowest negotiated build version at which 1012 lists 6_9 and 1006 answers tier 1_2. A runtime that negotiates
// less does not know the tier: the 22621 system core knows build versions up to 0092 only, copies the 1006 tier to
// the application, and refuses ALLOW_OPACITY_MICROMAPS in every RAYTRACING_PIPELINE_CONFIG1 (INTEGRATION.md,
// "Raytracing tier 1.2"). The 26100 system core and the Agility SDK 1.619 core negotiate 0094 and accept the flag.
constexpr uint32_t kTier12DdiVersion = 94;
static_assert(kTier12DdiVersion >= kDdiVersionMin && kTier12DdiVersion <= kDdiVersionMax, "a version build_caps takes");

constexpr D3D_FEATURE_LEVEL kLevels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                         D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};

// The release value of every shader model from 5_1 to 6_8 (H:3478-3500). The suffix of a value names the DDI build
// that defined it, not a version the driver must negotiate: the runtimes read in INTEGRATION.md "Shader model"
// (M840) take the highest release value of the 1012 list that they know, whatever build the driver negotiated.
// The 22621 system runtime knows release values up to 6_6 and ignores 6_7 and 6_8; runtime 26100 and the Agility
// SDK cores 1.615 and 1.619 map both. The engine is asked with 6_9 as the highest model of interest; its answer is
// what dxil-spirv implements on this adapter (vkd3d-proton device.c, d3d12_device_caps_init_shader_model).
struct ShaderModel { D3D_SHADER_MODEL api; D3D12DDI_SHADER_MODEL ddi; };
constexpr ShaderModel kShaderModels[] = {
    {D3D_SHADER_MODEL_5_1, D3D12DDI_SHADER_MODEL_5_1_RELEASE_0011}, {D3D_SHADER_MODEL_6_0, D3D12DDI_SHADER_MODEL_6_0_RELEASE_0011},
    {D3D_SHADER_MODEL_6_1, D3D12DDI_SHADER_MODEL_6_1_RELEASE_0033}, {D3D_SHADER_MODEL_6_2, D3D12DDI_SHADER_MODEL_6_2_RELEASE_0042},
    {D3D_SHADER_MODEL_6_3, D3D12DDI_SHADER_MODEL_6_3_RELEASE_0054}, {D3D_SHADER_MODEL_6_4, D3D12DDI_SHADER_MODEL_6_4_RELEASE_0062},
    {D3D_SHADER_MODEL_6_5, D3D12DDI_SHADER_MODEL_6_5_RELEASE_0071}, {D3D_SHADER_MODEL_6_6, D3D12DDI_SHADER_MODEL_6_6_RELEASE_0082},
    {D3D_SHADER_MODEL_6_7, D3D12DDI_SHADER_MODEL_6_7_RELEASE_0093}, {D3D_SHADER_MODEL_6_8, D3D12DDI_SHADER_MODEL_6_8_RELEASE_0108},
    // The release value 6_9_RELEASE_0118 (raytracing-tier12.h), which the 26100 header does not have. The Agility
    // 1.619 core maps it only from a driver that negotiated 0120 or later (INTEGRATION.md, "Raytracing tier 1.2");
    // listed_shader_model lists it only with the tier 1.2 switch on and at 0094.
    {D3D_SHADER_MODEL_6_9, static_cast<D3D12DDI_SHADER_MODEL>(rt12::kDdiShaderModel)},
};
constexpr D3D_SHADER_MODEL kHighestShaderModel = D3D_SHADER_MODEL_6_9;
static_assert(rt12::kShaderModel == D3D_SHADER_MODEL_6_9, "shader model 6.9");
static_assert(kShaderModels[std::size(kShaderModels) - 1].api == kHighestShaderModel, "the table ends at the ceiling");

bool known_shader_model(D3D_SHADER_MODEL m) noexcept {
    for (const ShaderModel& s : kShaderModels)
        if (s.api == m) return true;
    return false;
}

// The shell's own floor under a 6_9 answer: the optional features that every shader model 6.9 program may use
// without a check of its own (native 16-bit operations for the long vector types, 64-bit integers, wave
// operations). The engine gives 6_9 for long vectors, maintenance9 and SER (vkd3d-proton device.c,
// d3d12_device_caps_init_shader_model); this rule adds the three OPTIONS answers it reads elsewhere.
bool sm69_floor(const AdapterCaps& c) noexcept {
    return c.options1.WaveOps && c.options1.Int64ShaderOps && c.options4.Native16BitShaderOpsSupported;
}

// What 1012 and 1091 report: the engine's model, at most the shell's ceiling (6_9 unless lowered). 6_9 needs the
// tier 1.2 switch and sm69_floor; else the answer is 6_8 at most.
D3D_SHADER_MODEL reported_shader_model(const AdapterCaps& c) noexcept {
    D3D_SHADER_MODEL m = c.shader_model.HighestShaderModel < c.shader_model_ceiling ? c.shader_model.HighestShaderModel
                                                                                    : c.shader_model_ceiling;
    if (m >= D3D_SHADER_MODEL_6_9 && !(c.report_raytracing_tier12 && sm69_floor(c))) m = D3D_SHADER_MODEL_6_8;
    return m;
}

// What 1012 lists at the negotiated build version: reported_shader_model, and 6_8 at most below kTier12DdiVersion.
D3D_SHADER_MODEL listed_shader_model(const AdapterCaps& c, uint32_t ddi_version) noexcept {
    const D3D_SHADER_MODEL m = reported_shader_model(c);
    return m >= D3D_SHADER_MODEL_6_9 && ddi_version < kTier12DdiVersion ? D3D_SHADER_MODEL_6_8 : m;
}

// Raytracing tier 1.2: opacity micromaps and SER (DirectX-Specs Raytracing.md, tier 1.2). All of these:
//   - the switch (set_raytracing_tier12_reporting) and the general raytracing switch;
//   - the engine's own tier 1_2 (VK_EXT_opacity_micromap and SER, d3d12_device_determine_ray_tracing_tier);
//   - shader model 6_9 listed, because SER is a shader model 6.9 feature, which needs a negotiated build version
//     of kTier12DdiVersion or higher (listed_shader_model).
// The runtimes copy the 1006 tier to the application at every version, and ask type 1095 only at 0118 or higher
// (INTEGRATION.md, "Raytracing tier 1.2"). The acceleration structure and state object slots have a switch of
// their own and read the engine device's own tier (context.cpp, set_raytracing_tier12_commands).
bool tier12_reported(const AdapterCaps& c, uint32_t ddi_version) noexcept {
    return c.report_raytracing_tier && c.report_raytracing_tier12 &&
           static_cast<UINT>(c.options5.RaytracingTier) >= rt12::kTier &&
           listed_shader_model(c, ddi_version) >= D3D_SHADER_MODEL_6_9;
}

// Whether 1006 and 1004 report mesh shaders. Two conditions, both of them the engine's own answer:
//   - the engine's mesh shader tier, which is tier 1 exactly when its Vulkan device has VK_EXT_mesh_shader with
//     both meshShader and taskShader (vkd3d-proton device.c, d3d12_device_determine_mesh_shader_tier); an engine
//     that does not answer OPTIONS7 leaves the zeroed structure, which is NOT_SUPPORTED;
//   - a reported shader model of 6_5 or higher, because a mesh or amplification program is a DXIL program of
//     shader model 6.5 (DirectX-Specs MeshShader.md). Below that no application can make one, so the tier would
//     name a path that cannot be taken.
bool mesh_shaders(const AdapterCaps& c) noexcept {
    return c.options7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1 &&
           reported_shader_model(c) >= D3D_SHADER_MODEL_6_5;
}

// Feature level 12_2 needs raytracing tier 1.1, mesh shaders, VRS tier 2 and sampler feedback (DirectX-Specs
// D3D12_FeatureLevel12_2.md). build_caps reports no VRS and no sampler feedback while their DDI slots are
// fail-safes (SLOTS.md), so its ceiling is 12_1 whatever the mesh shader tier says; 3DPIPELINESUPPORT has the
// same ceiling by contract (H:10368-10373).
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

// 1060 SupportsRowMajorTexture. The pinned engine creates no texture of layout
// ROW_MAJOR (vkd3d-proton fork libs/vkd3d/resource.c, vkd3d_get_image_create_info refuses it with E_NOTIMPL) and
// reports CrossAdapterRowMajorTextureSupported FALSE (libs/vkd3d/device.c, d3d12_device_caps_init_feature_options).
constexpr BOOL kRowMajorTexture = FALSE;

// 1003 SubCaps[0]: row-major data in buffers, every element size, alignment 1 (cosumd12's values).
constexpr UINT16 kRowMajorMaxElementSize = 0xFFFF;
constexpr UINT16 kRowMajorAlignment = 1;

// 1059: the queue types engine-ddi creates (queue.cpp), in the form of cosumd12's answer.
constexpr D3D12DDI_COMMAND_QUEUE_FLAGS kEngineQueueFlags = static_cast<D3D12DDI_COMMAND_QUEUE_FLAGS>(
    D3D12DDI_COMMAND_QUEUE_FLAG_3D | D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE | D3D12DDI_COMMAND_QUEUE_FLAG_COPY);

// 1060 DeviceDependentSwizzleCount: the valid indices of 1061 are 0 through this count - 1 (H:4752-4755).
constexpr UINT kDeviceDependentSwizzleCount = 0;

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
    case D3D12_FEATURE_D3D12_OPTIONS2: return "D3D12_OPTIONS2";
    case D3D12_FEATURE_D3D12_OPTIONS3: return "D3D12_OPTIONS3";
    case D3D12_FEATURE_D3D12_OPTIONS4: return "D3D12_OPTIONS4";
    case D3D12_FEATURE_D3D12_OPTIONS5: return "D3D12_OPTIONS5";
    case D3D12_FEATURE_D3D12_OPTIONS7: return "D3D12_OPTIONS7";
    case D3D12_FEATURE_D3D12_OPTIONS9: return "D3D12_OPTIONS9";
    case D3D12_FEATURE_D3D12_OPTIONS11: return "D3D12_OPTIONS11";
    case D3D12_FEATURE_D3D12_OPTIONS12: return "D3D12_OPTIONS12";
    case D3D12_FEATURE_D3D12_OPTIONS13: return "D3D12_OPTIONS13";
    case D3D12_FEATURE_D3D12_OPTIONS14: return "D3D12_OPTIONS14";
    case D3D12_FEATURE_D3D12_OPTIONS21: return "D3D12_OPTIONS21";
    case D3D12_FEATURE_SERIALIZATION: return "SERIALIZATION";
    case D3D12_FEATURE_SHADER_CACHE: return "SHADER_CACHE";
    default: return "?";
    }
}

// 1006 RenderPassTier: the engine's own OPTIONS5 answer (vkd3d-proton device.c,
// d3d12_device_caps_init_feature_options5), clamped to tier 2, the last tier the DDI defines at 0092. The pinned
// engine answers tier 0 with the comment "Currently not supported" although it implements both calls, so nothing
// is reported today; an engine that answers a tier turns the feature on by itself, because the two slots
// (commands.cpp, begin_render_pass and end_render_pass) and the table (fill_render_pass) are in place and the
// shell publishes them from this same answer (render_pass_supported). An engine that does not answer OPTIONS5
// leaves the zeroed structure, which is NOT_SUPPORTED.
D3D12DDI_RENDER_PASS_TIER render_pass_tier(const AdapterCaps& c) noexcept {
    return static_cast<D3D12DDI_RENDER_PASS_TIER>(c.options5.RenderPassesTier > D3D12_RENDER_PASS_TIER_2
                                                      ? D3D12_RENDER_PASS_TIER_2
                                                      : c.options5.RenderPassesTier);
}

// 1006 WriteBufferImmediateQueueFlags: the queue types whose command lists take pfnWriteBufferImmediate (L56,
// commands.cpp). The engine's OPTIONS3 answer is a list-type mask (vkd3d-proton device.c,
// d3d12_device_caps_init_feature_options3: DIRECT, COMPUTE, COPY and BUNDLE, with no condition); the DDI field is
// a queue-flag mask, so the three list types that have a queue of their own are mapped one by one and the engine's
// BUNDLE bit has no queue flag to go to. The result is narrowed to the queues engine-ddi creates
// (kEngineQueueFlags, queue.cpp), and the slot sits in both command list tables, so every queue flag reported has
// a filled slot behind it. An engine that does not answer OPTIONS3 leaves the zeroed structure, which is NONE.
D3D12DDI_COMMAND_QUEUE_FLAGS write_buffer_immediate_queues(const AdapterCaps& c) noexcept {
    const auto engine = static_cast<UINT>(c.options3.WriteBufferImmediateSupportFlags);
    UINT flags = 0;
    if (engine & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT) flags |= D3D12DDI_COMMAND_QUEUE_FLAG_3D;
    if (engine & D3D12_COMMAND_LIST_SUPPORT_FLAG_COMPUTE) flags |= D3D12DDI_COMMAND_QUEUE_FLAG_COMPUTE;
    if (engine & D3D12_COMMAND_LIST_SUPPORT_FLAG_COPY) flags |= D3D12DDI_COMMAND_QUEUE_FLAG_COPY;
    return static_cast<D3D12DDI_COMMAND_QUEUE_FLAGS>(flags & static_cast<UINT>(kEngineQueueFlags));
}

void fill_options(const AdapterCaps& c, uint32_t ddi_version, D3D12DDI_D3D12_OPTIONS_DATA_0089& o) noexcept {
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
    // Depth bounds test: the engine's own OPTIONS2 answer, which is its Vulkan device's depthBounds feature
    // (vkd3d-proton device.c, d3d12_device_caps_init_feature_options2). Both halves of the feature are
    // implemented: pfnOMSetDepthBounds (L52, commands.cpp) and DepthBoundsTestEnable in the pipeline's
    // DEPTH_STENCIL1 subobject (pipelines.cpp). An engine that does not answer OPTIONS2 leaves the zeroed
    // structure, which is FALSE.
    o.DepthBoundsTestSupported = c.options2.DepthBoundsTestSupported;
    // Documented constants (INTEGRATION.md, "GetCaps"). Gated while the DDI slots behind them are fail-safes:
    //   ProgrammableSamplePositionsTier (pfnSetSamplePositions forwards, but the engine behind it is a stub),
    //   RaytracingTier (restricted collection imports
    //   refuse, indirect DispatchRays needs the
    //   fork's engine fix; the state object description is unmeasured, INTEGRATION.md "Ray tracing state objects"), the VRS fields
    //   (pfnRSSetShadingRate, pfnRSSetShadingRateImage), SamplerFeedbackTier
    //   (pfnCreateSamplerFeedbackUnorderedAccessView).
    //   BackgroundProcessingSupported: the engine answers FALSE itself and its SetBackgroundProcessingMode is a
    //   stub that returns E_NOTIMPL (vkd3d-proton device.c, d3d12_device_SetBackgroundProcessingMode and
    //   d3d12_device_caps_init_feature_options6), so there is no mode to apply; D114 is the shell's slot and
    //   accepts the call without applying one.
    //   Deterministic64KBUndefinedSwizzle: no engine answer exists; not claimed.
    // DriverManagedShaderCachePresent: the engine's per-user disk cache (%LOCALAPPDATA%\amdgpu-wddm\vkd3d, with the
    // binaries in hosted RADV's %LOCALAPPDATA%\mesa_shader_cache) is a cache of the driver's own, which D121
    // pfnImplicitShaderCacheControl disables, enables and clears (context.cpp, implicit_shader_cache_control). The
    // engine says whether this process has it: AUTOMATIC_DISK_CACHE in its D3D12_FEATURE_SHADER_CACHE answer, which it
    // gives only when the cache is configured (vkd3d-proton device.c). An engine without that answer reports no such
    // cache. The pipeline libraries of D85-D90 are not this cache: a library is the application's own.
    o.DriverManagedShaderCachePresent =
        (c.shader_cache.SupportFlags & D3D12_SHADER_CACHE_SUPPORT_AUTOMATIC_DISK_CACHE) ? TRUE : FALSE;
    o.RenderPassTier = render_pass_tier(c);
    // The engine's 1_1 or higher as 1_1, never more than the engine says, and NOT_SUPPORTED below that
    // (tier 1_0 demands acceleration structure copy modes the engine drops). set_raytracing_tier_reporting
    // takes the whole answer back to NOT_SUPPORTED. 1_2 only by tier12_reported, so 1_1 below kTier12DdiVersion.
    o.RaytracingTier = c.report_raytracing_tier && c.options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1
                           ? D3D12DDI_RAYTRACING_TIER_1_1
                           : D3D12DDI_RAYTRACING_TIER_NOT_SUPPORTED;
    if (tier12_reported(c, ddi_version)) o.RaytracingTier = static_cast<D3D12DDI_RAYTRACING_TIER>(rt12::kTier);
    o.VariableShadingRateTier = D3D12DDI_VARIABLE_SHADING_RATE_TIER_NOT_SUPPORTED;
    // Mesh shaders: the engine's own tier, which it derives from VK_EXT_mesh_shader and the two features it needs
    // (vkd3d-proton device.c, d3d12_device_determine_mesh_shader_tier: meshShader and taskShader both VK_TRUE, else
    // NOT_SUPPORTED). The only tier at 0092 is 1 (H:9353-9357), so a higher answer of a later engine is reported as
    // 1, and an engine that does not answer OPTIONS7 leaves the zeroed structure, which is NOT_SUPPORTED.
    // pfnDispatchMesh, the mesh and amplification shader slots and mesh pipelines are implemented
    // (pipelines.cpp, commands.cpp; SLOTS.md L67, D118-D120).
    o.MeshShaderTier = mesh_shaders(c) ? D3D12DDI_MESH_SHADER_TIER_1 : D3D12DDI_MESH_SHADER_TIER_NOT_SUPPORTED;
    // Both fields are only meaningful with mesh shaders. The first is the engine's OPTIONS9 answer; the second has
    // no engine answer at all (no D3D12_FEATURE field carries it) and stays FALSE.
    o.MeshShaderSupportsFullRangeRenderTargetArrayIndex =
        mesh_shaders(c) && c.options9.MeshShaderSupportsFullRangeRenderTargetArrayIndex;
    o.SamplerFeedbackTier = D3D12DDI_SAMPLER_FEEDBACK_TIER_NOT_SUPPORTED;
    o.ProgrammableSamplePositionsTier = D3D12DDI_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
    // Older engines report a tier despite rejecting valid repeated-layer PSOs.
    // Require the optional full-domain contract as well as this adapter's OPTIONS3.
    // The draw fallback implements Tier 2; it does not promise Tier 3's work sharing.
    o.ViewInstancingTier = c.view_contract.Revision == BC250_VKD3D_VIEW_INSTANCING_CONTRACT_REVISION &&
        c.view_contract.ViewInstancingTier == D3D12_VIEW_INSTANCING_TIER_2 &&
        c.options3.ViewInstancingTier >= D3D12_VIEW_INSTANCING_TIER_2 &&
        c.options3.ViewInstancingTier <= D3D12_VIEW_INSTANCING_TIER_3
        ? D3D12DDI_VIEW_INSTANCING_TIER_2 : D3D12DDI_VIEW_INSTANCING_TIER_NOT_SUPPORTED;
    o.WriteBufferImmediateQueueFlags = write_buffer_immediate_queues(c);
    // Enhanced barriers (H:11111): the engine's own OPTIONS12 answer (vkd3d-proton device.c,
    // d3d12_device_caps_init_feature_options12), which is unconditional in the pinned engine because its enhanced
    // barrier implementation needs no optional Vulkan feature. L68 pfnBarrier is filled in both command list
    // tables (resources.cpp) and the three resource-creation callbacks the feature also asks for
    // (CreateHeapAndResource, CalcPrivateHeapAndResourceSizes and CheckResourceAllocationInfo at 0088,
    // windows-driver-docs display\enhanced-barriers.md) are this module's slots with the initial barrier layout.
    // Reporting it also hands L68 the runtime's translation of legacy ResourceBarrier calls, which is why that
    // slot accepts every barrier type the DDI defines. An engine that does not answer OPTIONS12 leaves the zeroed
    // structure, which is FALSE.
    o.EnhancedBarriersSupported = c.options12.EnhancedBarriersSupported;
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
    // Derivatives in mesh and amplification programs: the engine's OPTIONS9 answer, and only with mesh shaders
    // reported (fill_options). The only wave MMA tier at 0092 is experimental (H:10436-10439), so that stays a
    // constant.
    s.DerivativesInMeshAndAmplificationShaders =
        mesh_shaders(c) && c.options9.DerivativesInMeshAndAmplificationShadersSupported;
    s.WaveMMATier = D3D12DDI_WAVE_MMA_TIER_NOT_SUPPORTED;
}

// 1002 as build_caps answers it: the engine's answers or constants, then the policy's explicit fields.
D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041 memory_architecture(const AdapterCaps& c,
                                                           const MemoryArchitecturePolicy& p) noexcept {
    D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041 m{};
    m.UMA = c.architecture.UMA;
    m.CacheCoherent = c.architecture.CacheCoherentUMA;
    m.IOCoherent = FALSE;                                   // no engine answer; not claimed without a policy
    m.HeapSerializationTier = c.serialization.HeapSerializationTier >= D3D12_HEAP_SERIALIZATION_TIER_10
                                  ? D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1
                                  : D3D12DDI_HEAP_SERIALIZATION_TIER_0041_0;
    m.ResourceSerializationTier = D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_0;   // no engine answer
    if (p.uma != PolicyBool::Default) m.UMA = p.uma == PolicyBool::True;
    if (p.cache_coherent != PolicyBool::Default) m.CacheCoherent = p.cache_coherent == PolicyBool::True;
    if (p.io_coherent != PolicyBool::Default) m.IOCoherent = p.io_coherent == PolicyBool::True;
    if (p.heap_serialization_tier.set)
        m.HeapSerializationTier = static_cast<D3D12DDI_HEAP_SERIALIZATION_TIER_0041>(p.heap_serialization_tier.value);
    if (p.resource_serialization_tier.set)
        m.ResourceSerializationTier =
            static_cast<D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041>(p.resource_serialization_tier.value);
    return m;
}

bool policy_bool_valid(PolicyBool b) noexcept {
    return b == PolicyBool::Default || b == PolicyBool::False || b == PolicyBool::True;
}

// set 0 with value 0, or set 1 with a value of the enum (H:6793-6797 ends at _1, H:6799-6804 at _2).
bool policy_tier_valid(const PolicyTier& t, uint32_t highest) noexcept {
    return t.set == 0 ? t.value == 0 : t.set == 1 && t.value <= highest;
}

template <class T> T* payload(const D3D12DDIARG_GETCAPS& r) noexcept {
    if (r.DataSize == sizeof(T) && r.pData) return static_cast<T*>(r.pData);
    log_line("GetCaps type %u: DataSize %u, pData %s; this revision answers only %zu bytes", static_cast<unsigned>(r.Type),
             r.DataSize, r.pData ? "set" : "null", sizeof(T));
    return nullptr;
}
#if defined(D3D12DDI_BUILD_VERSION_0119)
HRESULT options0102(const D3D12_FEATURE_DATA_D3D12_OPTIONS19& source, D3D_FEATURE_LEVEL level,
                    D3D12DDI_OPTIONS_DATA_0102* output) noexcept {
    D3D12DDI_OPTIONS_DATA_0102 value{};
    value.SupportedSampleCountsWithNoOutputs = source.SupportedSampleCountsWithNoOutputs;
    value.MaxSamplerDescriptorHeapSize = source.MaxSamplerDescriptorHeapSize;
    value.MaxSamplerDescriptorHeapSizeWithStaticSamplers = source.MaxSamplerDescriptorHeapSizeWithStaticSamplers;
    value.MaxViewDescriptorHeapSize = source.MaxViewDescriptorHeapSize;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    // Process-local, lab-only creation-gate probes. These values are deliberately synthetic.
    // Each arm bypasses minimum validation so the runtime, not this check, discriminates the fields.
    wchar_t arm[16]{};
    const DWORD length = GetEnvironmentVariableW(L"AMDGPU_WDDM_LAB_DDI0102_LIMITS", arm, 16);
    if (length) {
        if (length >= 16) return E_INVALIDARG;
        const bool samples = std::wcscmp(arm, L"samples") == 0 || std::wcscmp(arm, L"both") == 0;
        const bool samplers = std::wcscmp(arm, L"samplers") == 0 || std::wcscmp(arm, L"both") == 0;
        const bool control = std::wcscmp(arm, L"control") == 0;
        if (!samples && !samplers && !control) return E_INVALIDARG;
        if (samples) value.SupportedSampleCountsWithNoOutputs |= 0x1du;
        if (samplers && value.MaxSamplerDescriptorHeapSize < 4000u) value.MaxSamplerDescriptorHeapSize = 4000u;
        if (samples && samplers) {
            if (value.MaxSamplerDescriptorHeapSizeWithStaticSamplers < 2048u)
                value.MaxSamplerDescriptorHeapSizeWithStaticSamplers = 2048u;
            if (value.MaxViewDescriptorHeapSize < 1000000u) value.MaxViewDescriptorHeapSize = 1000000u;
        }
        log_line("LAB ONLY synthetic OPTIONS0102 probe samples=%u samplers=%u control=%u: source=%x/%u/%u/%u reported=%x/%u/%u/%u",
                 unsigned(samples), unsigned(samplers), unsigned(control), source.SupportedSampleCountsWithNoOutputs,
                 source.MaxSamplerDescriptorHeapSize, source.MaxSamplerDescriptorHeapSizeWithStaticSamplers,
                 source.MaxViewDescriptorHeapSize, value.SupportedSampleCountsWithNoOutputs,
                 value.MaxSamplerDescriptorHeapSize, value.MaxSamplerDescriptorHeapSizeWithStaticSamplers,
                 value.MaxViewDescriptorHeapSize);
        *output = value;
        return S_OK;
    }
#endif
    // DirectX-Specs VulkanOn12.md:909,1122-1128. These minima are implied by DDI0102,
    // not optional feature bits. Preserve the caller's payload when the backend cannot meet them.
    if ((level >= D3D_FEATURE_LEVEL_11_1 && (value.SupportedSampleCountsWithNoOutputs & 0x1du) != 0x1du) ||
        value.MaxSamplerDescriptorHeapSize < 4000u || value.MaxSamplerDescriptorHeapSizeWithStaticSamplers < 2048u ||
        value.MaxViewDescriptorHeapSize < 1000000u) {
        log_line("OPTIONS0102 refused: backend limits samples=%x sampler=%u static=%u view=%u, feature level=%x",
                 value.SupportedSampleCountsWithNoOutputs, value.MaxSamplerDescriptorHeapSize,
                 value.MaxSamplerDescriptorHeapSizeWithStaticSamplers, value.MaxViewDescriptorHeapSize, unsigned(level));
        return E_NOTIMPL;
    }
    *output = value;
    return S_OK;
}
#endif
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
    c->partial_graphics_programs = funcs->AbiVersion >= ((1u << 16) | 9u) &&
        funcs->CreatePartialGraphicsLibrary && funcs->AddRefPartialGraphicsLibrary &&
        funcs->ReleasePartialGraphicsLibrary && funcs->CreateLinkedGraphicsPipeline;
    c->levels.NumFeatureLevels = static_cast<UINT>(std::size(kLevels));
    c->levels.pFeatureLevelsRequested = kLevels;
    c->shader_model.HighestShaderModel = kHighestShaderModel;
    struct Entry { D3D12_FEATURE feature; void* data; UINT size; bool required; };
    const Entry entries[] = {
        {D3D12_FEATURE_FEATURE_LEVELS, &c->levels, sizeof(c->levels), true},
        {D3D12_FEATURE_SHADER_MODEL, &c->shader_model, sizeof(c->shader_model), true},
        {D3D12_FEATURE_ARCHITECTURE1, &c->architecture, sizeof(c->architecture), true},
        {D3D12_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT, &c->gpu_va, sizeof(c->gpu_va), true},
        {D3D12_FEATURE_D3D12_OPTIONS, &c->options, sizeof(c->options), true},
        {D3D12_FEATURE_D3D12_OPTIONS1, &c->options1, sizeof(c->options1), true},
        {D3D12_FEATURE_D3D12_OPTIONS2, &c->options2, sizeof(c->options2), false},
        {D3D12_FEATURE_D3D12_OPTIONS3, &c->options3, sizeof(c->options3), false},
        {static_cast<D3D12_FEATURE>(BC250_VKD3D_FEATURE_VIEW_INSTANCING_CONTRACT), &c->view_contract,
            sizeof(c->view_contract), false},
        {D3D12_FEATURE_D3D12_OPTIONS4, &c->options4, sizeof(c->options4), false},
        {D3D12_FEATURE_D3D12_OPTIONS5, &c->options5, sizeof(c->options5), false},
        {D3D12_FEATURE_D3D12_OPTIONS7, &c->options7, sizeof(c->options7), false},
        {D3D12_FEATURE_D3D12_OPTIONS9, &c->options9, sizeof(c->options9), false},
        {D3D12_FEATURE_D3D12_OPTIONS11, &c->options11, sizeof(c->options11), false},
        {D3D12_FEATURE_D3D12_OPTIONS12, &c->options12, sizeof(c->options12), false},
        {D3D12_FEATURE_D3D12_OPTIONS13, &c->options13, sizeof(c->options13), false},
        {D3D12_FEATURE_D3D12_OPTIONS14, &c->options14, sizeof(c->options14), false},
#if defined(D3D12DDI_BUILD_VERSION_0119)
        {D3D12_FEATURE_D3D12_OPTIONS19, &c->options19, sizeof(c->options19), false},
        {tight::feature, &c->tight_alignment, sizeof(c->tight_alignment), false},
#endif
        {D3D12_FEATURE_D3D12_OPTIONS21, &c->options21, sizeof(c->options21), false},
        {D3D12_FEATURE_SERIALIZATION, &c->serialization, sizeof(c->serialization), false},
        {D3D12_FEATURE_SHADER_CACHE, &c->shader_cache, sizeof(c->shader_cache), false},
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
#if defined(D3D12DDI_BUILD_VERSION_0119)
        if (entries[i].feature == D3D12_FEATURE_D3D12_OPTIONS19)
            c->options19_answered = SUCCEEDED(queries[i].Result);
#endif
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
    // The level list and model ceiling were engine-ddi's input; anything else is not an answer to them. A model
    // between two table rows (no such D3D_SHADER_MODEL exists today) is not an answer either.
    if (!pipeline_level(c->levels.MaxSupportedFeatureLevel) || c->shader_model.HighestShaderModel > kHighestShaderModel ||
        !known_shader_model(c->shader_model.HighestShaderModel)) {
        log_line("QueryAdapterCaps: feature level %x or shader model %x outside what was asked",
                 static_cast<unsigned>(c->levels.MaxSupportedFeatureLevel),
                 static_cast<unsigned>(c->shader_model.HighestShaderModel));
        delete c;
        return E_UNEXPECTED;
    }
    c->levels.pFeatureLevelsRequested = nullptr;          // not kept past the call
    // One line per adapter: the model and the optional features of 6_7 and 6_8 as the engine computed them. The
    // OPTIONS14 pair is not forwarded: 1079 reports both FALSE (build_caps).
    log_line("QueryAdapterCaps: shader model %x; AdvancedTextureOps %d, WriteableMSAATextures %d, "
             "SampleCmpGradientAndBias %d, ExtendedCommandInfo %d",
             static_cast<unsigned>(c->shader_model.HighestShaderModel), c->options14.AdvancedTextureOpsSupported,
             c->options14.WriteableMSAATexturesSupported, c->options21.SampleCmpGradientAndBiasSupported,
             c->options21.ExtendedCommandInfoSupported);
    *out = c;
    return S_OK;
}

void free_adapter_caps(AdapterCaps* caps) noexcept { delete caps; }

bool render_pass_supported(const AdapterCaps* caps) noexcept {
    return caps && render_pass_tier(*caps) != D3D12DDI_RENDER_PASS_TIER_NOT_SUPPORTED;
}

HRESULT set_raytracing_tier_reporting(AdapterCaps* caps, bool report) noexcept {
    if (!caps) return E_INVALIDARG;
    caps->report_raytracing_tier = report;
    log_line("raytracing tier reporting %s (engine tier %d)", report ? "on" : "off",
             static_cast<int>(caps->options5.RaytracingTier));
    return S_OK;
}

HRESULT set_raytracing_tier12_reporting(AdapterCaps* caps, bool report) noexcept {
    if (!caps) return E_INVALIDARG;
    caps->report_raytracing_tier12 = report;
    log_line("raytracing tier 1.2 reporting %s (engine tier %d, engine shader model %x, 6.9 floor %d): reported %x, "
             "tier 1.2 from build 94 %d", report ? "on" : "off", static_cast<int>(caps->options5.RaytracingTier),
             static_cast<unsigned>(caps->shader_model.HighestShaderModel), sm69_floor(*caps) ? 1 : 0,
             static_cast<unsigned>(reported_shader_model(*caps)), tier12_reported(*caps, kTier12DdiVersion) ? 1 : 0);
    return S_OK;
}

HRESULT set_shader_model_ceiling(AdapterCaps* caps, D3D_SHADER_MODEL ceiling) noexcept {
    if (!caps || !known_shader_model(ceiling)) {
        log_line("shader model ceiling %x refused%s", static_cast<unsigned>(ceiling), caps ? "" : ": null caps");
        return E_INVALIDARG;
    }
    caps->shader_model_ceiling = ceiling;
    log_line("shader model ceiling %x (engine %x): reported %x", static_cast<unsigned>(ceiling),
             static_cast<unsigned>(caps->shader_model.HighestShaderModel),
             static_cast<unsigned>(reported_shader_model(*caps)));
    return S_OK;
}

HRESULT set_memory_architecture_policy(AdapterCaps* caps, const MemoryArchitecturePolicy* policy) noexcept {
    if (!caps || !policy) return E_INVALIDARG;
    const MemoryArchitecturePolicy& p = *policy;
    if (p.size != sizeof(MemoryArchitecturePolicy) || !policy_bool_valid(p.uma) ||
        !policy_bool_valid(p.cache_coherent) || !policy_bool_valid(p.io_coherent) ||
        !policy_tier_valid(p.heap_serialization_tier, D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1) ||
        !policy_tier_valid(p.resource_serialization_tier, D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2)) {
        log_line("memory architecture policy refused: size %u, UMA %u, CacheCoherent %u, IOCoherent %u, "
                 "heap tier %u/%u, resource tier %u/%u (set/value)",
                 p.size, static_cast<unsigned>(p.uma), static_cast<unsigned>(p.cache_coherent),
                 static_cast<unsigned>(p.io_coherent), p.heap_serialization_tier.set, p.heap_serialization_tier.value,
                 p.resource_serialization_tier.set, p.resource_serialization_tier.value);
        return E_INVALIDARG;
    }
    // The answer build_caps would give, every Default resolved against the engine's answers; the contradictions
    // are those of INTEGRATION.md, "Memory architecture policy".
    const D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041 m = memory_architecture(*caps, p);
    const char* contradiction = nullptr;
    if (m.CacheCoherent && !m.UMA)
        contradiction = "CacheCoherent without UMA";
    else if (m.HeapSerializationTier == D3D12DDI_HEAP_SERIALIZATION_TIER_0041_1 &&
             m.ResourceSerializationTier != D3D12DDI_RESOURCE_SERIALIZATION_TIER_0041_2)
        contradiction = "heap serialization tier 1 without resource serialization tier 2";
    if (contradiction) {
        log_line("memory architecture policy refused: the resulting 1002 answer (UMA %d, CacheCoherent %d, "
                 "IOCoherent %d, heap tier %d, resource tier %d) has %s",
                 m.UMA, m.CacheCoherent, m.IOCoherent, static_cast<int>(m.HeapSerializationTier),
                 static_cast<int>(m.ResourceSerializationTier), contradiction);
        return E_INVALIDARG;
    }
    caps->memory_policy = p;
    return S_OK;
}

HRESULT build_caps(const AdapterCaps* caps, uint32_t ddi_version, const D3D12DDIARG_GETCAPS* request) noexcept {
    if (!caps || !request || ddi_version < kDdiVersionMin || ddi_version > kDdiVersionMax) return E_INVALIDARG;
    const AdapterCaps& c = *caps;
    const D3D12DDIARG_GETCAPS& r = *request;
    if (static_cast<UINT>(r.Type) == 1110) {
        // Agility 1.721.3-preview device-init query: linear algebra tier, one UINT32,
        // gated at DDI 0115. Local SCOPE-721 section 2.3 records the runtime contract;
        // WDK 28000.2526 has no named type. No later DDI structure is inferred here.
        static_assert(sizeof(UINT32) == 4);
        if (ddi_version < 115) return E_INVALIDARG;
        auto* d = payload<UINT32>(r);
        if (!d) return E_INVALIDARG;
        *d = 0; // No linear algebra tier is implemented.
        return S_OK;
    }
    // Agility 1.721.3-preview: one UINT tier, no input payload. The adapter
    // implementation and the pinned ABI 1.9 engine jointly implement this tier.
    if (static_cast<UINT>(r.Type) == 1099) {
        if (ddi_version < 122 || r.pInfo || !r.pData || r.DataSize != sizeof(UINT)) return E_INVALIDARG;
        *static_cast<UINT*>(r.pData) = c.partial_graphics_programs ? 10u : 0u;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        log_line("PGP_PROBE caps1099 tier%u production partial-program contract", c.partial_graphics_programs ? 10u : 0u);
#endif
        return S_OK;
    }
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
        const D3D_SHADER_MODEL highest = listed_shader_model(c, ddi_version);
        while (n < std::size(kShaderModels) && kShaderModels[n].api <= highest) ++n;
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
        fill_options(c, ddi_version, *d);
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
        *d = memory_architecture(c, c.memory_policy);
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
        t.DeviceDependentSwizzleCount = kDeviceDependentSwizzleCount;
        t.Supports64KStandardSwizzle = c.options.StandardSwizzle64KBSupported;
        t.SupportsRowMajorTexture = kRowMajorTexture;
        t.IndexableSwizzlePatterns = FALSE;
        *d = t;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_TEXTURE_LAYOUT_SETS: {
        // *pInfo is UINT[2] {D3D12DDI_TL_ROW_MAJOR, D3D12DDI_FUNCTIONAL_UNIT}, pData D3D12DDI_ROW_MAJOR_LAYOUT_CAPS
        // (H:268-271). ROW_MAJOR is the only layout the query names; the units are COMBINED, COPY_SRC and COPY_DST
        // (H:259-266). Buffers are ROW_MAJOR and carry the row-major data of footprint copies, which the engine
        // does support, so the answer is independent of 1060 SupportsRowMajorTexture. The values are those of
        // cosumd12 (CosUmd12Adapter.cpp:345-367): entry 0 covers every element size with alignment 1, entry 1 is
        // unused, Flags NONE. The engine copies through Vulkan buffer-image copies, which need texel alignment only,
        // and has one copy path, so the three units get the same answer. INTEGRATION.md, "1003 values".
        auto* d = payload<D3D12DDI_ROW_MAJOR_LAYOUT_CAPS>(r);
        if (!d) return E_INVALIDARG;
        const UINT* key = static_cast<const UINT*>(r.pInfo);
        if (!key || key[0] != D3D12DDI_TL_ROW_MAJOR || key[1] > D3D12DDI_FUNCUNIT_COPY_DST) {
            if (key)
                log_line("GetCaps TEXTURE_LAYOUT_SETS: layout %u, functional unit %u outside the contract", key[0], key[1]);
            else
                log_line("GetCaps TEXTURE_LAYOUT_SETS: pInfo NULL, the contract names UINT[2]");
            return E_INVALIDARG;
        }
        D3D12DDI_ROW_MAJOR_LAYOUT_CAPS t{};
        t.SubCaps[0].MaxElementSize = kRowMajorMaxElementSize;
        t.SubCaps[0].BaseOffsetAlignment = kRowMajorAlignment;
        t.SubCaps[0].PitchAlignment = kRowMajorAlignment;
        t.SubCaps[0].DepthPitchAlignment = kRowMajorAlignment;
        t.Flags = D3D12DDI_ROW_MAJOR_LAYOUT_FLAG_NONE;
        *d = t;
        log_line("GetCaps TEXTURE_LAYOUT_SETS: layout %u, functional unit %u: one entry, alignment %u", key[0], key[1],
                 static_cast<unsigned>(kRowMajorAlignment));
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0022_SWIZZLE_PATTERN:
        // *pInfo is an index below 1060's DeviceDependentSwizzleCount (H:4752-4755), which is 0: no index exists.
        static_assert(kDeviceDependentSwizzleCount == 0, "1061 answers no swizzle pattern");
        log_line("GetCaps SWIZZLE_PATTERN (DataSize %u, pInfo %s): 1060 reports no device-dependent swizzle pattern",
                 r.DataSize, r.pInfo ? "set" : "null");
        return E_INVALIDARG;
    case D3D12DDICAPS_TYPE_0022_CPU_PAGE_TABLE_FALSE_POSITIVES: {
        // *pInfo is a NodeIndex, pData a D3D12DDI_COMMAND_QUEUE_FLAGS (H:1431-1434). No source says what a set flag
        // means. cosumd12 answers with the queue type it has (COMPUTE, CosUmd12Adapter.cpp:369-376); engine-ddi
        // answers with the three it has. INFERENCE, INTEGRATION.md "1059 value".
        auto* d = payload<D3D12DDI_COMMAND_QUEUE_FLAGS>(r);
        if (!d) return E_INVALIDARG;
        const UINT* node = static_cast<const UINT*>(r.pInfo);
        if (!node || *node != 0) {
            if (node)
                log_line("GetCaps CPU_PAGE_TABLE_FALSE_POSITIVES: node %u, the adapter has node 0 only", *node);
            else
                log_line("GetCaps CPU_PAGE_TABLE_FALSE_POSITIVES: pInfo NULL, the contract names a NodeIndex");
            return E_INVALIDARG;
        }
        *d = kEngineQueueFlags;
        log_line("GetCaps CPU_PAGE_TABLE_FALSE_POSITIVES: node 0, flags 0x%x", static_cast<unsigned>(kEngineQueueFlags));
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0030_PROTECTED_RESOURCE_SESSION_SUPPORT: {
        // NodeIndex is an input inside the payload (H:13697-13701). engine-ddi refuses every protected resource
        // session (pfnSetProtectedResourceSession is a fail-safe, resources.cpp refuses a session handle).
        auto* d = payload<D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_DATA_0030>(r);
        if (!d || d->NodeIndex != 0) return E_INVALIDARG;
        d->Support = D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_FLAG_0030_NONE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_EXECUTECOMMANDLISTS_PARALLELISM: {
        auto* d = payload<BOOL>(r);                         // pData = BOOL (H:128)
        if (!d) return E_INVALIDARG;
        *d = FALSE;                                         // not claimed: the queue table is the shell's
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0073_SUPPORT_BATCHED_MARKERS: {
        auto* d = payload<BOOL>(r);                         // pData = BOOL (H:131)
        if (!d) return E_INVALIDARG;
        *d = FALSE;                                         // no marker batching; pfnSetMarker accepts and drops
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
    case D3D12DDICAPS_TYPE_OPTIONS_0093: {
        // Two optional features of shader model 6_7, both FALSE whatever the engine answers in OPTIONS14.
        // AdvancedTextureOps includes integer border colours, whose sampler layout comes only with DDI 0096
        // (H:11677-11706); this shell creates samplers from the 0092 D3D12DDI_SAMPLER_DESC (descriptors.cpp).
        // WriteableMSAATextures has no test of the shell's MSAA UAV path yet. The type exists from 0093 on; a
        // 0092 caller gets E_INVALIDARG like any unknown layout.
        if (ddi_version < 93) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0093>(r);
        if (!d) return E_INVALIDARG;
        d->AdvancedTextureOpsSupported = FALSE;
        d->WriteableMSAATexturesSupported = FALSE;
        return S_OK;
    }
#if defined(D3D12DDI_BUILD_VERSION_0119)
    // New payloads are exact WDK 10.0.28000 layouts. A higher negotiated DDI does not enable a feature whose
    // resource/state/command translation is absent. Keep those gates closed even if the engine supports it.
    case D3D12DDICAPS_TYPE_OPTIONS_0098: {
        if (ddi_version < 98) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0098>(r);
        if (!d) return E_INVALIDARG;
        d->L1MemoryFullyCpuAccessible = FALSE; // no shell GPU-upload heap contract
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0101: {
        if (ddi_version < 101) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0101>(r);
        if (!d) return E_INVALIDARG;
        d->ComputeOnlyCustomHeapSupported = FALSE; // this adapter is not an FL1_0 compute-only device
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0102: {
        if (ddi_version < 102) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0102>(r);
        if (!d) return E_INVALIDARG;
        // These are limits, not optional BOOLs. An unanswered engine query must not manufacture zero limits.
        if (!c.options19_answered) return E_NOTIMPL;
        return options0102(c.options19, c.levels.MaxSupportedFeatureLevel, d);
    }
    case D3D12DDICAPS_TYPE_OPTIONS1_0103: {
        if (ddi_version < 103) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0103>(r);
        if (!d) return E_INVALIDARG;
        // The engine's own answer (OPTIONS21, queried above): tier 1.0 only in a process where the engine has work
        // graphs on (vkd3d-proton device.c, d3d12_device_supports_workgraphs). The DDI work graph slots (CORE_0108,
        // COMMAND_LIST_3D_0108) exist at build version 0108 and later (work-graphs.cpp).
        d->WorkGraphsTier = ddi_version >= 108 && c.options21.WorkGraphsTier >= D3D12_WORK_GRAPHS_TIER_1_0
                                ? D3D12DDI_WORK_GRAPHS_TIER_1_0
                                : D3D12DDI_WORK_GRAPHS_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0103_WAVE_MMA: {
        if (ddi_version < 103) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_WAVE_MMA_DATA_0103>(r);
        if (!d) return E_INVALIDARG;
        // InputDataType, M and N are input fields. Preserve them when declining this experimental feature.
        d->Supported = FALSE;
        d->K = d->RequiredWaveLaneCountMin = d->RequiredWaveLaneCountMax = 0;
        d->AccumDataTypes = D3D12DDI_WAVE_MMA_ACCUM_DATATYPE_NONE;
        return S_OK;
    }
    case D3D12DDI_FEATURE_D3D12_PREDICATION_106: {
        if (ddi_version < 106) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_FEATURE_DATA_PREDICATION_106>(r);
        if (!d) return E_INVALIDARG;
        d->Supported = FALSE; // optional FL1_0 contract is not queried from this engine
        return S_OK;
    }
    case D3D12DDI_FEATURE_PLACED_RESOURCE_SUPPORT_INFO_106: {
        if (ddi_version < 106) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_FEATURE_DATA_PLACED_RESOURCE_SUPPORT_INFO_106>(r);
        if (!d) return E_INVALIDARG;
        d->Supported = FALSE; // preserve the format, pool, page properties and node-mask inputs
        return S_OK;
    }
    case D3D12DDI_FEATURE_HARDWARE_COPY_106: {
        if (ddi_version < 106) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_FEATURE_DATA_HARDWARE_COPY_106>(r);
        if (!d) return E_INVALIDARG;
        d->Supported = FALSE; // optional FL1_0 contract is not queried from this engine
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0109: {
        if (ddi_version < 109) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0109>(r);
        if (!d) return E_INVALIDARG;
        d->RecreateAtTier = D3D12DDI_RECREATE_AT_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0110: {
        if (ddi_version < 110) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0110>(r);
        if (!d) return E_INVALIDARG;
        // commands.cpp translates INCREMENTING_CONSTANT and the engine executes the signature.
        d->ExecuteIndirectTier = c.options21.ExecuteIndirectTier == D3D12_EXECUTE_INDIRECT_TIER_1_1
            ? D3D12DDI_EXECUTE_INDIRECT_TIER_1_1 : D3D12DDI_EXECUTE_INDIRECT_TIER_1_0;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_TIGHT_ALIGNMENT_TIER_0111: {
        if (ddi_version < 111) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_TIGHT_ALIGNMENT_TIER_DATA_0111>(r);
        if (!d) return E_INVALIDARG;
        d->SupportTier = c.tight_alignment.SupportTier == D3D12_TIGHT_ALIGNMENT_TIER_1 ? D3D12DDI_TIGHT_ALIGNMENT_TIER_1
                                                   : D3D12DDI_TIGHT_ALIGNMENT_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_GUID_TEXTURE_LAYOUT_TIER_0111: {
        if (ddi_version < 111) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_GUID_TEXTURE_LAYOUT_TIER_DATA_0111>(r);
        if (!d) return E_INVALIDARG;
        d->GuidTextureLayoutTier = D3D12DDI_GUID_TEXTURE_LAYOUT_TIER_NOT_SUPPORTED;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_GUID_TEXTURE_LAYOUT_GUID_SUPPORT_0111: {
        if (ddi_version < 111) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_GUID_TEXTURE_LAYOUT_GUID_SUPPORT_DATA_0111>(r);
        if (!d) return E_INVALIDARG;
        d->GuidSupported = FALSE; // pCreateResource and AllowedAccess are inputs
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_0112_NATIVE_FENCE_SUPPORT: {
        if (ddi_version < 112) return E_INVALIDARG;
        auto* d = payload<D3D12DDICAPS_NATIVE_FENCE_SUPPORT_DATA_0112>(r);
        if (!d) return E_INVALIDARG;
        d->NativeGpuFenceSupported = FALSE;
        return S_OK;
    }
    case D3D12DDI_APPLICATION_SPECIFIC_DRIVER_STATE_113: {
        if (ddi_version < 113) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_FEATURE_DATA_APPLICATION_SPECIFIC_DRIVER_STATE_113>(r);
        if (!d) return E_INVALIDARG;
        d->Supported = FALSE;
        return S_OK;
    }
    case D3D12DDICAPS_TYPE_OPTIONS_0118: {
        if (ddi_version < 118) return E_INVALIDARG;
        auto* d = payload<D3D12DDI_OPTIONS_DATA_0118>(r);
        if (!d) return E_INVALIDARG;
        // The current SDK/engine caps contract has no SER answer. SM6.9 is also absent from kShaderModels.
        d->ShaderExecutionReorderingActuallyReorders = FALSE;
        return S_OK;
    }
#endif
    case D3D12DDICAPS_TYPE_SHADER_MODEL_6_8_OPTIONS_0110: {
        // A 0110 type, asked by runtimes that map 6_8 whenever 1012 lists 6_8, with no DDI version check (runtime
        // 26100: QueryShaderModel66AndLater; M840). It is answered here although the shell negotiates 0092: an
        // unanswered type leaves both fields FALSE. Both are the engine's OPTIONS21 answers while 6_8 is reported,
        // FALSE otherwise ("only queried if the device supports >= shader model 6.8", H:13672).
        auto* d = payload<D3D12DDI_SHADER_MODEL_6_8_OPTIONS_0110>(r);
        if (!d) return E_INVALIDARG;
        const bool sm68 = reported_shader_model(c) >= D3D_SHADER_MODEL_6_8;
        d->SampleCmpGradientAndBiasSupported = sm68 && c.options21.SampleCmpGradientAndBiasSupported;
        d->ExtendedCommandInfoSupported = sm68 && c.options21.ExtendedCommandInfoSupported;
        return S_OK;
    }
    default:
        // Unknown contracts, including Agility 721 MLIR queries 1100/1102 (8 bytes),
        // fail without changing the caller's payload.
        log_line("GetCaps type %u (DataSize %u, pInfo %s): not answered in this revision", static_cast<unsigned>(r.Type),
                 r.DataSize, r.pInfo ? "set" : "null");
        return E_NOTIMPL;
    }
}

} // namespace engine_ddi
