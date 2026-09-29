# Linking engine-ddi into the native12 shell

What the shell (namespace `native12`, the rest of `driver/umd/d3d12`) builds, calls and when. The boundary itself
is `engine-ddi.h`, revision 3.

## Build and dependencies

- Recipe: `tools/build/build-engine-ddi.ps1 -NativeOnly`. It produces `engine-ddi.lib` (static, `/MT`, `/analyze`
  clean under `/WX`, no harness symbol) and runs the host tests. The output is
  `<workspace>\scratch\build\d3d12-engine-ddi\engine-ddi.lib`.
- Compile the shell's translation units that include `engine-ddi.h` with the same include directories: the WDK
  `um` and `shared` directories, the engine ABI header's directory and the Vulkan headers below.
- Engine ABI header, included by path and not vendored into this repository:
  - file `libs/ddi/bc250_vkd3d_engine.h` of the project's vkd3d-proton fork, branch
    `amdgpu-wddm/ddi-engine-1.3`, commit `66c98e7246c024964e97b05575de0cacdfefdee7`;
  - revision r5-draft, ABI 1.3, NOT FROZEN in the header's own words; r4 added the instance mode (V12,
    `CreateInfo.InstanceMode` at offset 40, CreateInfo 48 bytes on x64), r5 adds linear images (V13,
    `QueryLinearImage` and `CreateLinearPlacedResource`, function table 80 bytes on x64);
  - SHA-256 `9F77137FA26FBD1BA840BCB25B2B8BA998720E76A893B68327882085A9672AED`;
  - default checkout `<workspace>\scratch\m15\vkd3d-1.3-src` (`-EngineSource`, `source_checkout` in the pin).
    The shell's build must include the same header: `tools/build/build-umd-d3d12.ps1` defaults to the r3
    checkout `scratch\m15\vkd3d-1.2-src` until it moves.
- Vulkan headers: `khronos/Vulkan-Headers/include` of the same checkout, submodule commit
  `ee2ec5fd83dafce291024683b50dc89219333076`.
- Engine DLL: `amdgpu_wddm_vkd3d.dll` built by `tools/build/build-vkd3d.ps1` (config `ddi-engine`) from that
  commit with no local changes, SHA-256 `D8BB19C34D33C1F4D25E96C56CFD924B533A010A7AD6596B692F08ED8DCA0317`, in
  `<workspace>\scratch\m15\engine-1.3-r5-66c98e72` (`engine_dll_dir` in the pin). This engine refuses a command
  signature that changes state on a device without device generated commands (E_NOTIMPL at the create)
  instead of creating one that executes nothing.
- [engine-abi.json](engine-abi.json) holds these pins. The build script refuses a header, and before a run an
  engine DLL, whose SHA-256 differs. A new header revision means a new pin, a rebuild of both sides, and a new
  engine DLL.

## Adapter: GetCaps

The lab run M768 showed this order: OpenAdapter12, GetCaps 1074 (DataSize 8, pInfo NULL), GetCaps 1007
(DataSize 4), GetSupportedVersions with capacities 0 then 1, CloseAdapter. GetCaps comes before version
negotiation and before any device.

```cpp
HRESULT engine_ddi::query_adapter_caps(const BC250_VKD3D_ENGINE_FUNCS* funcs,
                                       const BC250_VKD3D_DEVICE_CREATE_INFO* info,
                                       engine_ddi::AdapterCaps** out) noexcept;
void engine_ddi::free_adapter_caps(engine_ddi::AdapterCaps* caps) noexcept;
HRESULT engine_ddi::build_caps(const engine_ddi::AdapterCaps* caps, uint32_t ddi_version,
                               const D3D12DDIARG_GETCAPS* request) noexcept;
HRESULT engine_ddi::set_memory_architecture_policy(engine_ddi::AdapterCaps* caps,
                                                   const engine_ddi::MemoryArchitecturePolicy* policy) noexcept;
```

1. OpenAdapter12, or at the latest the first GetCaps:
   - load the engine DLL and call `Bc250Vkd3dEngineGetFuncs(BC250_VKD3D_ENGINE_ABI_VERSION, &funcs)` with
     `funcs.Size = sizeof(funcs)`. The argument is `BC250_VKD3D_ENGINE_ABI_VERSION` (1.3): the engine leaves the
     entries of a later minor NULL for a shell that asks for an earlier one, and `query_adapter_caps` and
     `create_device_context` then return E_INVALIDARG;
   - fill the `BC250_VKD3D_DEVICE_CREATE_INFO` that CreateDevice will receive: `Size`
     `sizeof(BC250_VKD3D_DEVICE_CREATE_INFO)` (48 since the r4 header), `AbiVersion` 1.3 (`0x00010003`), hosted
     RADV's `GetInstanceProcAddr`, the `AdapterLuid` hosted RADV reports for this adapter (V2),
     `MinimumFeatureLevel` `D3D_FEATURE_LEVEL_11_0`, `QueueMode` INLINE, the shell's `Services` and `InstanceMode`
     `BC250_VKD3D_INSTANCE_MODE_PRIVATE` (V12: hosted RADV binds its runtime identity to the VkInstance, so no
     device or query may share one; an r3 engine would ignore the field, which the pin rules out). The engine
     applies the INLINE admission to it (three graphics-family VkQueues in ABI 1.2), and its answers are those of a
     device made from this info (V11), so the same info must go to CreateDevice;
   - call `query_adapter_caps` once and keep the result with the adapter. One call is one QueryAdapterCaps batch
     and one VkInstance; no VkDevice is created and no Service is called. On failure there is nothing to answer
     GetCaps from: fail OpenAdapter12 (or that GetCaps) with the returned HRESULT;
   - optionally, before the first GetCaps answers from it, `set_memory_architecture_policy(caps, &policy)`: the
     shell's answers for 1002 ("Memory architecture policy" below). Without the call 1002 is as in the table.
2. `pfnGetCaps`: `return engine_ddi::build_caps(caps, D3D12DDI_BUILD_VERSION_0092, pCaps);`. build_caps only reads
   `caps` and may run on any thread.
3. `pfnGetSupportedVersions`: unchanged. It keeps reporting the single entry `D3D12DDI_SUPPORTED_0092`. Because the
   first GetCaps calls come before this negotiation, `ddi_version` is the constant 92, not a negotiated value;
   build_caps writes the 0092 layouts and refuses any other `ddi_version`.
4. `pfnCloseAdapter`: `free_adapter_caps(caps)`.

A caps test on the development PC's GPU with the pinned DLL (`caps-test.exe --engine`) answered 1074 and 1007 with
12_1, from an engine at FL12_2. On unit A, M769 read the engine's own answers through QueryAdapterCaps: FL11_1,
tiled resources tier 0, binding tier 3, raytracing tier 1.1. With this mapping 1074 and 1007 would report 11_1 and
1006 no raytracing; that is derived, not yet run through build_caps on unit A.

### GetCaps mapping

Sources are the engine's QueryAdapterCaps answers (D3D12_FEATURE_*, SDK `d3d12.h` 10.0.26100) or a constant.
Layout references are WDK 10.0.26100 `um/d3d12umddi.h` ("H:"); DDI-ref and the other source names are those of
"Memory architecture policy" below; cosumd12 is Microsoft's compute-only sample driver,
`ref\graphics-driver-samples\compute-only-sample\cosumd12` (@de4a2161), cited for shape only. A wrong DataSize is
E_INVALIDARG with nothing written. An unanswered type returns E_NOTIMPL and logs type, size and whether pInfo is set.

Every `D3D12DDICAPS_TYPE` value (H:94-150) that exists at 0092. The version is the suffix of the name; a value
without one predates the suffixes, except 1069, which H:127-130 lists between 1068 (0061) and 1070 (0073). 1008 and
1011 are not defined. Left out because they are later than 0092: 1013 (0103), 1075 (0103), 1079 (0093), 1080 (0098),
1081 (0101), 1082 (0102), 1084-1086 (106), 1087 (0109), 1088 (0110), 1091 (0110); 1083 is reserved (H:142).

| Type | Name | Status | Reason |
|---|---|---|---|
| 1000 | TEXTURE_LAYOUT | E_NOTIMPL | deprecated by 1060 (H:96, H:165-170) |
| 1001 | SWIZZLE_PATTERN | E_NOTIMPL | deprecated by 1061 (H:97, H:206-215) |
| 1002 | MEMORY_ARCHITECTURE | answered | table below |
| 1003 | TEXTURE_LAYOUT_SETS | answered | table below |
| 1004 | SHADER | answered | table below |
| 1005 | ARCHITECTURE_INFO | answered | table below |
| 1006 | D3D12_OPTIONS | answered | table below |
| 1007 | 3DPIPELINESUPPORT | answered | table below |
| 1009 | GPUVA_CAPS | answered | table below |
| 1010 | TEXTURE_LAYOUT1 | E_NOTIMPL | deprecated by 1060 (H:106, H:184-188) |
| 1012 | 0011_SHADER_MODELS | answered | table below |
| 1057 | 0030_PROTECTED_RESOURCE_SESSION_SUPPORT | answered | table below |
| 1058 | 0030_CRYPTO_SESSION_SUPPORT | E_NOTIMPL | deprecated, moved to the video caps (H:112) |
| 1059 | 0022_CPU_PAGE_TABLE_FALSE_POSITIVES | answered | "1059 value" below |
| 1060 | 0022_TEXTURE_LAYOUT | answered | table below |
| 1061 | 0022_SWIZZLE_PATTERN | answered: E_INVALIDARG | table below |
| 1062 | 0023_UMD_BASED_COMMAND_QUEUE_PRIORITY | answered | table below |
| 1063, 1064, 1065 | 0030_CONTENT_PROTECTION_SYSTEM_COUNT, _SUPPORT, 0030_CRYPTO_SESSION_TRANSFORM_SUPPORT | E_NOTIMPL | deprecated, moved to the video caps (H:120-122) |
| 1066 | 0033_ADAPTER_COMPUTE_ONLY | E_NOTIMPL | H names no payload for it and DDI-ref says only "Adapter compute only"; this is a 3D adapter, whose level 1007 and 1074 report (cosumd12 states compute-only through 1007 `1_0_CORE` alone) |
| 1067 | 0050_HARDWARE_SCHEDULING_CAPS | answered | table below |
| 1068 | QUERY_META_COMMAND_CAPS_0061 | E_NOTIMPL | the payload names a CommandId (H:17859-17866) from EnumerateMetaCommands, which reports none ("Query slots around CreateDevice" below) |
| 1069 | EXECUTECOMMANDLISTS_PARALLELISM | answered | table below |
| 1070 | SAMPLER_FEEDBACK_0073 | E_NOTIMPL | a per-resource query of the feedback map size (H:9428-9437); 1006 reports SamplerFeedbackTier NOT_SUPPORTED |
| 1071 | 0073_SUPPORT_BATCHED_MARKERS | answered | table below |
| 1072, 1073 | 0074_PROTECTED_RESOURCE_SESSION_TYPE_COUNT, _TYPES | E_NOTIMPL | asked only when 1057 reports SUPPORTED (DDI-ref ne-d3d12umddi-d3d12ddicaps_type.md:159, :163); 1057 reports NONE |
| 1074 | 0081_3DPIPELINESUPPORT1 | answered | table below |
| 1077 | OPTIONS_0090 | answered | table below |
| 1078 | OPTIONS_0091 | answered | table below |

Answered: 19 of the 31 values at 0092, 1061 as a refusal by contract. The 12 left E_NOTIMPL are the deprecated
1000, 1001, 1010, 1058, 1063, 1064 and 1065, the undocumented 1066, and 1068, 1070, 1072 and 1073, which
the runtime reaches only through a capability engine-ddi reports as absent.

1059 value. `*pInfo` is a NodeIndex and pData a `D3D12DDI_COMMAND_QUEUE_FLAGS` of 4 bytes (H:1431-1434). Neither H
nor DDI-ref says what a set flag means. cosumd12, a compute-only driver, answers COMPUTE, the queue type it has,
under the comment "TODO: What is this?" (CosUmd12Adapter.cpp:369-376). engine-ddi answers in the same form with
the queue types it creates: 3D, COMPUTE and COPY. That the answer names the driver's queue types is INFERENCE
from that one sample. Node 0 only; another node, a NULL pInfo or another DataSize is E_INVALIDARG with nothing
written.

L below is min(level of FEATURE_LEVELS.MaxSupportedFeatureLevel, 12_1), asked for {11_0, 11_1, 12_0, 12_1, 12_2}.
The engine-ddi ceiling is 12_1 because 12_2 needs raytracing 1.1, mesh shaders, VRS tier 2 and sampler feedback,
which this revision reports as unsupported (fail-safe slots in SLOTS.md).

| Type | Payload (bytes, H:) | Field | Value | Source or reason |
|---|---|---|---|---|
| 1074 3DPIPELINESUPPORT1 | `D3D12DDI_3DPIPELINESUPPORT1_DATA_0081`, 8, H:10415-10420 | MaximumDriverSupportedFeatureLevel | min(L, HighestRuntimeSupportedFeatureLevel) | FEATURE_LEVELS; "highest ... that does not exceed what the runtime understands", H:10377-10378 |
| 1007 3DPIPELINESUPPORT | `D3D12DDI_3DPIPELINELEVEL` itself, 4, H:2922-2933 | the level | L | FEATURE_LEVELS; never above 12_1, H:10373 |
| 1012 SHADER_MODELS | `D3D12DDI_D3D12_SHADER_MODELS_DATA_0011`, 16, H:3502-3507 | `*pNumShaderModelsSupported`, `pShaderModelsSupported` | every release model from 5_1 to min(M, 6_6); the count is always written, the array when non-NULL, E_INVALIDARG when its count is smaller | SHADER_MODEL asked with 6_6 gives M; 6_6 is the last release model at 0092 (6_7 is 0093, H:3478-3500) |
| 1006 D3D12_OPTIONS | `D3D12DDI_D3D12_OPTIONS_DATA_0089`, 124, H:11078-11112 | ResourceBindingTier, ConservativeRasterizationTier, CrossNodeSharingTier, ResourceHeapTier, OutputMergerLogicOp, VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation | same value | OPTIONS; DDI and API enums are equal (static_assert in caps.cpp) |
| 1006 | | TiledResourcesTier | OPTIONS tier, 4 reported as 3 | OPTIONS; no DDI tier 4 at 0092 (H:709-715). Reserved resources, GetMipPacking, CopyTiles and the engine parts of Q3 and Q4 are implemented ("Tiled resources" below); the queue slots need the shell's hook |
| 1006 | | CopyQueueTimestampQueriesSupported, BarycentricsSupported | same value | OPTIONS3 |
| 1006 | | ReservedBufferPlacementSupported | MSAA64KBAlignedTextureSupported | OPTIONS4; "Actually just 64KB aligned MSAA support", H:11094 |
| 1006 | | SRVOnlyTiledResourceTier3 | same value | OPTIONS5 |
| 1006 | | DepthBoundsTestSupported | FALSE | pfnOMSetDepthBounds is a fail-safe |
| 1006 | | ProgrammableSamplePositionsTier | NOT_SUPPORTED | pfnSetSamplePositions is a fail-safe |
| 1006 | | WriteBufferImmediateQueueFlags | NONE | pfnWriteBufferImmediate is a fail-safe |
| 1006 | | ViewInstancingTier | NOT_SUPPORTED | pfnSetViewInstanceMask is a fail-safe |
| 1006 | | RenderPassTier | NOT_SUPPORTED | engine-ddi fills no render pass table; the runtime emulates render passes |
| 1006 | | RaytracingTier | NOT_SUPPORTED | state objects and pfnDispatchRays are fail-safes |
| 1006 | | VariableShadingRateTier, PerPrimitiveShadingRateSupportedWithViewportIndexing, AdditionalShadingRatesSupported, ShadingRateImageTileSize, VariableRateShadingSumCombinerSupported, MeshShaderPerPrimitiveShadingRateSupported | NOT_SUPPORTED, FALSE, 0 | pfnRSSetShadingRate and pfnRSSetShadingRateImage are fail-safes |
| 1006 | | MeshShaderTier, MeshShaderSupportsFullRangeRenderTargetArrayIndex, MSPrimitivesPipelineStatisticIncludesCulledPrimitives | NOT_SUPPORTED, FALSE, FALSE | pfnDispatchMesh and the mesh shader slots are fail-safes |
| 1006 | | SamplerFeedbackTier | NOT_SUPPORTED | pfnCreateSamplerFeedbackUnorderedAccessView is a fail-safe |
| 1006 | | EnhancedBarriersSupported | FALSE | pfnBarrier is a fail-safe |
| 1006 | | BackgroundProcessingSupported | FALSE | pfnSetBackgroundProcessingMode is the shell's slot; engine-ddi claims nothing for it |
| 1006 | | DriverManagedShaderCachePresent | FALSE | engine-ddi keeps no driver-managed shader cache |
| 1006 | | Deterministic64KBUndefinedSwizzle | FALSE | no engine answer |
| 1004 SHADER | `D3D12DDI_SHADER_CAPS_0084`, 64, H:10515-10534 | MinPrecision, DoubleOps, ShaderSpecifiedStencilRef, TypedUAVLoadAdditionalFormats, ROVs | MinPrecisionSupport, DoublePrecisionFloatShaderOps, PSSpecifiedStencilRefSupported, TypedUAVLoadAdditionalFormats, ROVsSupported | OPTIONS |
| 1004 | | WaveOps, WaveLaneCountMin, WaveLaneCountMax, TotalLaneCount, Int64Ops | same values (Int64Ops from Int64ShaderOps) | OPTIONS1 |
| 1004 | | Native16BitOps | Native16BitShaderOpsSupported | OPTIONS4 |
| 1004 | | AtomicInt64OnTypedResource, AtomicInt64OnGroupShared | the ...Supported fields | OPTIONS9 |
| 1004 | | AtomicInt64OnDescriptorHeapResource | AtomicInt64OnDescriptorHeapResourceSupported | OPTIONS11 |
| 1004 | | DerivativesInMeshAndAmplificationShaders | FALSE | mesh shaders are not reported (above) |
| 1004 | | WaveMMATier | NOT_SUPPORTED | the only other tier at 0092 is experimental, H:10436-10439 |
| 1005 ARCHITECTURE_INFO | `D3D12DDI_ARCHITECTURE_INFO_DATA`, 4, H:2916-2920 | TileBasedDeferredRenderer | TileBasedRenderer | ARCHITECTURE1 |
| 1002 MEMORY_ARCHITECTURE | `D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041`, 20, H:6806-6814; pInfo NULL or node 0 (H:152-155), else E_INVALIDARG | UMA, CacheCoherent | the policy's explicit value; with Default or no policy UMA, CacheCoherentUMA | ARCHITECTURE1, overridden by the shell's memory architecture policy (below) |
| 1002 | | HeapSerializationTier | the policy's explicit value; with Default or no policy 1 when the API tier is 10, else 0 | SERIALIZATION (DDI tiers 0 and 1, H:6793-6797), overridden by the policy |
| 1002 | | IOCoherent | the policy's explicit value; with Default or no policy FALSE | no engine answer: host and kernel-driver policy, which the shell's policy states |
| 1002 | | ResourceSerializationTier | the policy's explicit value; with Default or no policy 0 | no engine answer; overridden by the policy (tier 0: see "Open points" below) |
| 1009 GPUVA_CAPS | `D3D12DDI_GPUVA_CAPS_0004`, 4, H:250-257; pInfo NULL or node 0 | MaxGPUVirtualAddressBitsPerResource | same value | GPU_VIRTUAL_ADDRESS_SUPPORT |
| 1060 TEXTURE_LAYOUT | `D3D12DDI_TEXTURE_LAYOUT_CAPS_0026`, 20, H:5525-5536; pInfo NULL, else E_INVALIDARG (no swizzle patterns) | Supports64KStandardSwizzle | StandardSwizzle64KBSupported | OPTIONS |
| 1060 | | DeviceDependentLayoutCount, DeviceDependentSwizzleCount, SupportsRowMajorTexture, IndexableSwizzlePatterns | 0, 0, FALSE, FALSE | no device-dependent layouts; the pinned engine creates no ROW_MAJOR texture (vkd3d-proton fork `libs/vkd3d/resource.c`, `vkd3d_get_image_create_info` refuses the layout with E_NOTIMPL) and answers OPTIONS.CrossAdapterRowMajorTextureSupported FALSE (`libs/vkd3d/device.c`, `d3d12_device_caps_init_feature_options`) |
| 1003 TEXTURE_LAYOUT_SETS | `D3D12DDI_ROW_MAJOR_LAYOUT_CAPS`, 20, H:280-292 (`SubCaps[2]` of `D3D12DDI_ROW_MAJOR_LAYOUT_SUB_CAPS`, four UINT16 each: MaxElementSize, BaseOffsetAlignment, PitchAlignment, DepthPitchAlignment; then `D3D12DDI_ROW_MAJOR_LAYOUT_FLAGS` Flags, H:272-278); `*pInfo` is `UINT[2]` {`D3D12DDI_TL_ROW_MAJOR`, `D3D12DDI_FUNCTIONAL_UNIT`} (H:268-271) | the whole struct, for each of COMBINED, COPY_SRC, COPY_DST (H:259-266) | SubCaps[0] MaxElementSize 0xFFFF, BaseOffsetAlignment, PitchAlignment and DepthPitchAlignment 1; SubCaps[1] zero; Flags NONE; the same on every unit. pInfo NULL, a layout other than ROW_MAJOR (1, H:193) or a unit above COPY_DST: E_INVALIDARG | the values of cosumd12 for COMBINED (CosUmd12Adapter.cpp:345-367), independent of 1060 SupportsRowMajorTexture; "1003 values" below |
| 1061 0022_SWIZZLE_PATTERN | `D3D12DDI_SWIZZLE_PATTERN_DESC_0022`, 232, H:4752-4767; `*pInfo` an index 0 through DeviceDependentSwizzleCount - 1 | - | E_INVALIDARG for every request, nothing written | 1060 DeviceDependentSwizzleCount 0: no index exists (constant `kDeviceDependentSwizzleCount`, shared with 1060) |
| 1057 0030_PROTECTED_RESOURCE_SESSION_SUPPORT | `D3D12DDI_PROTECTED_RESOURCE_SESSION_SUPPORT_DATA_0030`, 8, H:13697-13701; NodeIndex (input) 0, else E_INVALIDARG | Support | NONE | engine-ddi refuses protected resource sessions: pfnSetProtectedResourceSession is a fail-safe and resource creation refuses a session handle (resources.cpp) |
| 1069 EXECUTECOMMANDLISTS_PARALLELISM | BOOL, 4, H:128 | the BOOL | FALSE | not claimed: ExecuteCommandLists is a slot of the shell's queue table |
| 1071 0073_SUPPORT_BATCHED_MARKERS | BOOL, 4, H:131 | the BOOL | FALSE | "Indicates whether UMD supports batched markers" (DDI-ref); pfnSetMarker accepts and drops the legacy marker |
| 1062 UMD_BASED_COMMAND_QUEUE_PRIORITY | `D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023`, 4, H:5140-5143 | SupportedQueueFlagsForGlobalRealtimeQueues | NONE | no realtime queues |
| 1067 HARDWARE_SCHEDULING_CAPS | `D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050`, 4, H:7004-7008 | ComputeQueuesPer3DQueue | 0 | "0 means don't use scheduling groups", H:7007 |
| 1077 OPTIONS_0090 | `D3D12DDI_OPTIONS_DATA_0090`, 4, H:11127-11131 | RelaxedFormatCastingSupported | same value | OPTIONS12 |
| 1078 OPTIONS_0091 | `D3D12DDI_OPTIONS_DATA_0091`, 16, H:11143-11150 | the four fields | same values | OPTIONS13 |

FEATURE_LEVELS, SHADER_MODEL, ARCHITECTURE1, GPU_VIRTUAL_ADDRESS_SUPPORT, OPTIONS and OPTIONS1 are required:
`query_adapter_caps` fails with the engine's Result when one is unanswered. The others are optional: an
unanswered one is logged and its fields report no support.

1003 values. Neither H nor DDI-ref ("Texture layout sets.") documents the fields of 1003, so the answer follows
the one implementation in the references and what the engine can honour.

- Buffers are created ROW_MAJOR "because row-major texture data can be located in them without creating a texture
  object" (API-ref ne-d3d12-d3d12_texture_layout.md:92). 1003 is keyed by that layout (H:268-271), so it is asked of
  a driver that has buffers, whether or not it has ROW_MAJOR textures. The engine has buffers and copies footprints
  between buffers and textures; an answer of zero capability was therefore wrong, whatever 1060 says.
- cosumd12 answers COMBINED with entry 0 MaxElementSize 0xFFFF and alignments 1, entry 1 zero, Flags NONE
  (CosUmd12Adapter.cpp:345-367). engine-ddi gives the same values. That an entry states the alignments for elements
  up to its MaxElementSize is INFERENCE from the field names.
- The engine honours alignment 1: it copies through Vulkan buffer-image copies, which require texel alignment only
  (Specs VulkanOn12.md:165). The API's own footprint alignments, `D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT` 512 and
  `D3D12_TEXTURE_DATA_PITCH_ALIGNMENT` 256 (Specs ResourceHeaps.md:467-482, H:18990-18991), are stricter and are
  relaxed only through 1078.
- cosumd12 refuses COPY_SRC and COPY_DST with E_NOTIMPL. engine-ddi answers them as COMBINED, since the engine has
  one copy path for every queue type.
- ROW_MAJOR textures stay unsupported (1060). If a runtime needs them, the way forward is ROW_MAJOR support in the
  engine, not another constant.

### Memory architecture policy

Whether GPU accesses to system memory are I/O coherent is decided by the host and the kernel driver (its PTE
cache bits and its mappings), not by the engine; the shell may also know better than the engine's memory-type scan
whether the adapter is UMA or cache coherent. `set_memory_architecture_policy` (engine-ddi.h) lets the shell state
each field of 1002 separately: `PolicyBool` Default, False or True for UMA, CacheCoherent and IOCoherent, and
`PolicyTier` {set 0, value 0} for Default or {set 1, value} for either serialization tier. `MemoryArchitecturePolicy`
is 32 bytes and starts with `size`; a policy with `size` set and every other byte zero is all Default. Only 1002
changes; every other type, and 1002 without a call or with an all-Default policy, stays byte for byte as it was.
Call it after `query_adapter_caps` and before the first GetCaps; it writes `caps`, so never concurrently with
`build_caps`. A later call replaces the whole policy. E_INVALIDARG keeps the policy held before: a null argument,
a `size` other than 32, a PolicyBool or `set` outside the values above, a Default tier with a non-zero value, a
tier the 0092 header does not define (heap tier above 1, H:6793-6797; resource tier above 2, H:6799-6804), or a
contradiction below.

Sources: H is `d3d12umddi.h` (WDK 10.0.26100); DDI-ref is `ref\windows-driver-docs-ddi` (@7515063c,
`wdk-ddi-src/content/d3d12umddi/`) and its merged copy `ref\ddi-display\d3d12umddi.md`; Specs is
`ref\DirectX-Specs\d3d` (@5a4139be); API-ref is `ref\sdk-api-docs\sdk-api-src\content\d3d12` (@a4fd3f7e); Guides
is `ref\windows-driver-docs\windows-driver-docs-pr\display` (@110f60ea); win32-docs is `ref\win32-docs\desktop-src`
(@e103fa4e). Engine sources are the vkd3d-proton fork at the commit engine-abi.json pins.

| Field | Meaning | Source |
|---|---|---|
| UMA | the GPU uses the same physical memory as the CPU; the runtime then requests no `_L1` pool | DDI-ref d3d12umddi.md:41637-41639; Specs ResourceHeaps.md:609, :915 |
| CacheCoherent | cache-coherent UMA, "a special type of GPU UMA design" with CPU and GPU caches integrated; the runtime then maps UPLOAD heaps write-back | DDI-ref d3d12umddi.md:41645-41647; Specs ResourceHeaps.md:611, D3D12GPUUploadHeaps.md:81-88; API-ref ns-d3d12-d3d12_feature_data_architecture1.md:74-77 (the API field is CacheCoherentUMA) |
| IOCoherent | GPU accesses to cacheable system memory are coherent with the CPU caches; without it the runtime invalidates and flushes CPU caches itself around CPU access | DDI-ref d3d12umddi.md:41641-41643; Specs ResourceHeaps.md:728, :1041, :1043; Guides allocation-usage-tracking.md:53-62; the kernel-side cap is `CacheCoherentMemorySupported`, ddi-display\d3dkmddi.md:22240-22242 |
| HeapSerializationTier | 0: no hardware support for heap serialization; 1: textures stay observable in their swizzle through overlapping resources (API TIER_10) | H:6793-6797; DDI-ref ne-d3d12umddi-d3d12ddi_heap_serialization_tier_0041.md:44, :48; API-ref ne-d3d12-d3d12_heap_serialization_tier.md |
| ResourceSerializationTier | 0: "reserved and cannot be used in current designs"; 1: stateless copies; 2: hardware support for heap serialization | H:6799-6804; DDI-ref ne-d3d12umddi-d3d12ddi_resource_serialization_tier_0041.md:44, :48, :52 |

Refused contradictions. They are judged on the resulting 1002 answer, every Default resolved against the engine's
answers, so a policy that sets one field can be refused because of an engine answer it keeps:

1. CacheCoherent TRUE with UMA FALSE. Specs ResourceHeaps.md:611: "`CacheCoherentUMA` is a special type of GPU UMA
   design"; the runtime's heap conversions (Specs D3D12GPUUploadHeaps.md:55, :70, :81) know CacheCoherentUMA only
   under UMA TRUE.
2. HeapSerializationTier 1 with ResourceSerializationTier other than 2. DDI-ref
   ne-d3d12umddi-d3d12ddi_heap_serialization_tier_0041.md:48: "The Driver verifier will ensure resource
   serialization tier 2 in addition to heap serialization tier 1."

Accepted, because no header or specification sentence calls them contradictory:

- IOCoherent TRUE with UMA FALSE. I/O coherence is stated for GPUs in general, discrete ones included: "On x86/x64
  today, all GPUs must support I/O coherency over PCIe" (Guides allocation-usage-tracking.md:55); Specs
  ResourceHeaps.md:728 and :1041 set no UMA condition, and the kernel cap is separate from any UMA notion.
- IOCoherent FALSE with CacheCoherent TRUE: the runtime then does the cache maintenance (ResourceHeaps.md:1041).
- An explicit ResourceSerializationTier 0: the header defines it (H:6801), and it is today's answer without a policy.

Open points, unchanged here: 1002 without a policy answers ResourceSerializationTier 0, which DDI-ref calls
reserved; and an engine at API heap serialization tier 10 would give heap tier 1 with resource tier 0 (rule 2).
The pinned engine answers heap serialization tier 0 unconditionally (vkd3d-proton fork `libs/vkd3d/device.c`,
D3D12_FEATURE_SERIALIZATION), so only the first applies. Which resource tier engine-ddi can claim is for the
engine owners to decide.

**The runtime's view and the engine's.** The runtime answers CheckFeatureSupport(ARCHITECTURE, ARCHITECTURE1) and
SERIALIZATION from 1002 (INFERENCE: 1002 is the only DDI source of these fields), and it turns an application's
heap type into a CPU page property and memory pool from the same view (Specs D3D12GPUUploadHeaps.md:55-90, API-ref
nf-d3d12-id3d12device-getcustomheapproperties(uint_d3d12_heap_type).md:79, :105, :131; "will not request `_L1` on
UMA designs", ResourceHeaps.md:915). DEFAULT stays NOT_AVAILABLE under every combination; UMA only moves its pool
from L1 to L0, and CacheCoherent turns UPLOAD from WRITE_COMBINE into WRITE_BACK. engine-ddi hands both values of
`D3D12DDIARG_CREATEHEAP_0001` to the engine as a CUSTOM heap (`heap_desc_of`, resources.cpp). The engine chooses
Vulkan memory from the CPU page property alone (`vkd3d_select_memory_flags`, `libs/vkd3d/memory.c`: WRITE_BACK
host-visible and host-cached, WRITE_COMBINE host-visible and host-coherent, NOT_AVAILABLE device-local), not from
its own UMA answer. That answer (`d3d12_device_is_uma`, `libs/vkd3d/device.c`: every memory type that is not lazily
allocated is host-visible) reaches this path in one place: `d3d12_device_validate_custom_heap_type`
(`libs/vkd3d/heap.c`) refuses pool L1 on a device it considers UMA. Per override:

- UMA TRUE over an engine answer FALSE: the runtime asks for L0 only; the engine takes every L0 heap. Correct.
  Applications may put more textures in CPU-visible CUSTOM heaps (win32-docs direct3d12/default-texture-mapping.md:30),
  which the engine accepts on any device; a performance question, not a correctness one.
- UMA FALSE over an engine answer TRUE: the runtime asks for DEFAULT heaps in L1 and the engine refuses every one
  with E_INVALIDARG. A loud failure, not silent corruption, but it breaks the device: not a usable combination
  without the engine change below.
- CacheCoherent TRUE over an engine answer FALSE: UPLOAD heaps arrive WRITE_BACK and get host-cached memory, the
  kind READBACK heaps get in every configuration already. Correct on the same terms as READBACK.
- IOCoherent, either way: the engine has no counterpart and needs none; the field changes only whether the runtime
  does CPU cache maintenance (ResourceHeaps.md:1041). TRUE is correct exactly when the kernel driver's mappings of
  system memory are cached and coherent for the GPU, which is the shell's premise for setting it.
- Serialization tiers: lowering a tier below the engine's is safe; raising one claims a behaviour the engine has not
  claimed, which the runtime and applications may rely on.
- The engine's own allocations (descriptor heaps, internal buffers) follow its own view; the runtime never sees them.

Engine ABI addition, needed only for UMA FALSE over an engine that considers itself UMA (not implemented): a host
memory architecture field in `BC250_VKD3D_DEVICE_CREATE_INFO` (ABI 1.3), for example `UINT32 HostMemoryArchitecture`
with a valid bit and a value bit each for UMA and cache-coherent UMA, that `d3d12_device_is_uma` returns in place
of its memory-type scan when valid. It would then govern the engine's ARCHITECTURE and ARCHITECTURE1 answers,
GetCustomHeapProperties and the L1 check of `d3d12_device_validate_custom_heap_type` alike. The shell fills it from
the same policy it gives `set_memory_architecture_policy`, in the one create info that goes to QueryAdapterCaps and
CreateDevice (V11). IOCoherent and the serialization tiers need no engine field. An engine-ddi-only alternative
without an ABI change would be to pass NOT_AVAILABLE L1 heaps to the engine as L0.

## Device

- FillDDITable: `fill_device_core` and `fill_command_list` (table 0 compute, 1 graphics) first, then the shell's
  own slots over the 18 core slots engine-ddi leaves NULL, and the shell's `pfnPresent`.
- CreateDevice: the engine's `CreateDevice` with the adapter's create info, then `create_device_context`
  (`MemoryMode::RuntimeBacked`, the shell's hooks, the engine's ABI 1.2 function table: it refuses one without
  CreateHeapFromMemory, MapHeap and UnmapHeap). Ordering: the context is created and registered, so that the
  shell's `ResolveDevice` returns it for this `D3D12DDI_HDEVICE`, before CreateDevice returns S_OK, and it stays
  registered and live until DestroyDevice. A failed `create_device_context` fails CreateDevice. Every engine-ddi
  slot finds its device only through `ResolveDevice`; a slot called while it returns null answers nothing (a query
  leaves its output zeroed or untouched) and reports no error, because there is no device to report to.
  DestroyDevice: `destroy_device_context`; S_FALSE with live objects keeps the context, and then it stays
  registered too.
- Queues (the queue table is the shell's): after creating the WDDM context, `create_engine_queue`;
  ExecuteCommandLists: `execute_command_lists`; DestroyCommandQueue: `destroy_engine_queue`, which returns
  `QueueClose::Retired` when every engine use of the queue is proven retired and `QueueClose::NotRetired`
  otherwise (removed device, a submission without a successful retirement signal, or a fence short of the last
  signal at the final Release), whatever the engine's GetDeviceRemovedReason reads. NotRetired also records
  retirement_lost for the device. Either way engine-ddi has freed the EngineQueue on return and keeps only the
  device's retirement bookkeeping; the shell must not pass the pointer again. The shell's WDDM context and tokens
  stay the shell's: after NotRetired the GPU may still use that context, so the shell keeps it, and it never
  calls back through the destroyed queue's HRTCOMMANDQUEUE.
- Command lists: a shell slot of the list table (its first argument is the `D3D12DDI_HCOMMANDLIST`, not the
  device) finds its device's shell with `command_list_shell(list)`: `ShellHooks::shell` of the context that created
  the list, or null for storage that holds no live engine-ddi list (before CreateCommandList constructs it, after
  DestroyCommandList; CloseCommandList and ResetCommandList do not end a list). It reads the record without a lock,
  so it is called from a slot of that list or while the shell knows the list is neither being created nor
  destroyed; the runtime serializes the calls of one list. The core slots start with the `D3D12DDI_HDEVICE` and need
  nothing of this kind.
- Present: `present_allocation` for the back buffer's runtime allocation; it admits a linear primary only.

### Query slots around CreateDevice

Which device slots the runtime calls during D3D12CreateDevice has not been observed: the list below is an
INFERENCE from the slot types (queries and controls the runtime can call before any object exists). Each reports
no error. "Engine answer" means the value comes from the engine for that query; "exact" means the value is the
complete answer for what engine-ddi supports; "placeholder: returns 0" marks a zero that stands for a query
engine-ddi does not resolve yet. A placeholder is not completed integration.

| Slot | Answer | Status |
|---|---|---|
| CheckFormatSupport | the engine's FORMAT_SUPPORT, mapped bit by bit; 0 when the engine refuses the format | engine answer |
| CheckMultisampleQualityLevels, Flags NONE | the engine's MULTISAMPLE_QUALITY_LEVELS; 0 when the engine refuses | engine answer |
| CheckMultisampleQualityLevels, Flags TILED_RESOURCE | the engine's MULTISAMPLE_QUALITY_LEVELS with the TILED_RESOURCE flag; 0 when the engine refuses | engine answer |
| GetDescriptorSizeInBytes | the engine's descriptor increment | engine answer |
| CheckResourceAllocationInfo, CheckExistingResourceAllocationInfo | the engine's GetResourceAllocationInfo for the description; no additional data | engine answer |
| EnumerateMetaCommands | count 0, S_OK | exact: engine-ddi has no meta commands |
| CheckDriverMatchingIdentifier | UNRECOGNIZED | exact: engine-ddi serializes nothing |
| ImplicitShaderCacheControl | no-op | exact: 1006 D3D12_OPTIONS reports DriverManagedShaderCachePresent FALSE |

Every other engine-ddi slot is implemented (SLOTS.md) or a fail-safe. A void fail-safe whose non-const pointers
are all `_Out_` zeroes them before it reports E_NOTIMPL, and each of these is a placeholder: returns 0 (the
E_NOTIMPL report is the difference from the table above): CheckSubresourceInfo,
GetRaytracingAccelerationStructurePrebuildInfo, GetMetaCommandRequiredParameterInfo. The first lab log of
D3D12CreateDevice on the native path (the engine-ddi log line "fail-safe slot D+0x... called") settles the list.

### Heap memory: allocate_memory and free_memory

pfnCreateHeapAndResource with a heap description (committed, or a heap alone) makes one `allocate_memory` call;
a placed resource (resource description only, `ReuseBufferGPUVA` naming a resource of the heap) makes none.
engine-ddi then calls the engine's `CreateHeapFromMemory` (ABI 1.2 V10) over the memory, and places a committed
resource at offset 0. MapHeap and UnmapHeap go to the engine's MapHeap and UnmapHeap: the CPU address of heap
offset 0, a placed buffer at that address plus its offset.

The linear primary (boundary r4, engine ABI 1.3 V13). A committed texture on a heap with
`D3D12DDI_HEAP_FLAG_PRIMARY` is what a reader outside the engine opens and reads by row pitch. When its
description is one the surface exists for (2D, one mip, one layer, one sample, B8G8R8A8_UNORM or
R8G8B8A8_UNORM, at most 8192 on an edge, heap without CPU access, castable formats none beyond the format and its
sRGB sibling), engine-ddi:

1. asks the engine what the linear image needs (`QueryLinearImage`), before any memory exists;
2. sizes the backing: the larger of the image's memory size and row pitch times the height rounded up to 4,
   rounded up to 4 KiB. A runtime heap smaller than that is E_INVALIDARG; a larger one, or one with an alignment,
   becomes the backing's size with the default heap alignment;
3. makes the one `allocate_memory` call with `kMemoryDedicated | kMemoryPrimary | kMemoryLinearSurface`,
   `byte_size` the backing's, `alignment` and `memory_type_bits` the image's, and the surface fields
   (`surface_row_pitch`, `surface_layout_size`). The shell describes the allocation to the kernel from these and
   from the resource description, and imports it with one of the named memory types;
4. creates the heap over the memory and the image at offset 0 (`CreateLinearPlacedResource`), and fails the
   create with E_FAIL if the bound image is not the one step 1 described.

A failure of step 1 fails the create: no other tiling takes the surface's place. A PRIMARY heap whose
description is none of the above is asked of the shell as before, with `kMemoryPrimary` alone, and the shell
decides. `pfnCheckResourceAllocationInfo` answers the backing's size and a 64 KiB alignment when the runtime
passes `D3D12DDI_RESOURCE_OPTIMIZATION_FLAG_PRIMARY` with such a description, and
`pfnCheckExistingResourceAllocationInfo` the same for a resource created this way. Only these two PRIMARY flags
select the surface; no description becomes linear by its shape.

Development PC witness (harness round trip 7, `tests/test-linear-primary.cpp`, RuntimeBacked on the stub shell):
256x256 B8G8R8A8_UNORM and 127x79 R8G8B8A8_UNORM primaries are created, cleared through a render target view,
copied to a READBACK buffer and compared texel by texel. What a reader of the memory itself sees is not
established by that: the copy goes through the image.

Heap size left to the resource. H and DDI-ref give `D3D12DDIARG_CREATEHEAP_0001::ByteSize` as "Size of the heap,
in bytes" (H:319-328) and define no special value. engine-ddi treats a ByteSize of UINT64_MAX as "no size given":

- with a resource description (committed), the heap gets the SizeInBytes of the engine's GetResourceAllocationInfo
  for that resource, the same size pfnCheckResourceAllocationInfo reported;
- without one (a heap alone), E_INVALIDARG: nothing says how large the heap is;
- a placed resource has no heap description and is not affected.

engine-ddi works on its own copy of the heap description. `MemoryRequest::heap` points at that copy, so
`request->heap->ByteSize` and `request->byte_size` are the same concrete size, and neither `allocate_memory` nor
the engine's `CreateHeapFromMemory` ever receives UINT64_MAX. A given ByteSize smaller than the resource needs is
E_INVALIDARG as before. The other heap fields, the flags included, pass through unchanged.

What `allocate_memory(shell, request, memory)` must deliver on S_OK, in the 48-byte `ImportedMemory`:

| Field | Value |
|---|---|
| `size` | `sizeof(ImportedMemory)` |
| `memory` | one whole `VkDeviceMemory` on the engine's VkDevice (`GetVulkanHandles`): the shell's import of the runtime allocation. Allocated with `VkMemoryAllocateFlagsInfo::flags` = `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` always; no `VkMemoryDedicatedAllocateInfo`, even for `kMemoryDedicated`; not mapped by the shell while engine-ddi holds it |
| `byte_size` | its `VkMemoryAllocateInfo::allocationSize`, at least `request->byte_size` |
| `memory_type_index` | a type the engine would pick for this heap (below) |
| `allocation` | the kernel allocation (`pfnAllocateCb_0022`), non-zero; `resource_allocation` and CheckResourceAllocationHandle return it |
| `gpu_va` | the GPU virtual address of the completed mapping, non-zero and a multiple of `request->alignment` |
| `reserved` | 0 |
| `cookie` | the shell's, passed back in `free_memory` |

The memory type. `request->memory_type_bits` is 0 in r3: engine-ddi cannot compute the engine's types. engine-ddi
turns the DDI heap into a `D3D12_HEAP_TYPE_CUSTOM` heap with the same CPU page property and memory pool, and the
engine accepts a type that its own choice for that heap would allow (vkd3d-proton `vkd3d_memory_type_supports_heap`:
the heap's domain and the buffer, sampled-image and render-target type masks of the categories the heap allows,
host-visible for a CPU-visible heap). The harness picks by the property flags vkd3d-proton selects for the CPU page
property: WRITE_BACK host-visible and host-cached, WRITE_COMBINE host-visible and host-coherent, NOT_AVAILABLE
device-local, among the types a buffer can use. A type the engine refuses fails CreateHeapFromMemory with
E_INVALIDARG; engine-ddi hands the memory back through `free_memory` and the create fails with that code.

Release: engine-ddi.h, "Release sequence of heap memory", is V10's safe order. After every engine queue has
retired the work submitted before the last destroy (the heap record and each resource placed in it), engine-ddi
releases the engine heap, then calls `free_memory` once, on a DDI thread. The shell then frees its Vulkan import
and deallocates the runtime allocation. The engine never frees, clears or zeroes the memory.

The memory of a linear primary is released inside the destroy that ends it: that destroy takes the snapshot
again, with engine-ddi's lock released in between, until the work has retired or 2000 ms have passed. Past the
bound it reports ERROR_TIMEOUT through `report_device_error` and records the release like any other; the
shell then gets `free_memory` from a later DDI call, where it must not name the runtime resource any more.
Harness witness: `tests/test-linear-primary.cpp` (a destroy that waits 40 ms and frees in the same call, a
destroy past a 30 ms bound, ordinary memory that does not wait).

Development PC witness (harness round trip 5, `tests/test-runtime-backed.cpp`): a stub shell allocates the memory
on the engine's VkDevice as above, with stand-in allocation handles. A committed UPLOAD buffer of 128 KiB holds a
placed buffer at 64 KiB (GPU VA the heap's plus 64 KiB); the placed buffer, filled through MapHeap, is copied into
a committed DEFAULT and a committed READBACK buffer, and read back through MapHeap word for word. Three
allocations, three `free_memory` calls after their engine heaps, VVL with synchronization validation clean.
Whether hosted RADV's import of a runtime allocation behaves the same (same storage, `gpu_va`) is for the lab.

### Residency: MakeResident and Evict

The slots (`pfnMakeResident_0001`, `pfnEvict2`) are the shell's; engine-ddi resolves their objects with
`object_allocation(context, object, &allocation)` (engine-ddi.h has the full contract):

| Object | Result | `allocation` |
|---|---|---|
| `D3D12DDI_HT_HEAP`, `D3D12DDI_HT_0012_RESOURCE` (committed or placed) | S_OK | the heap memory's kernel allocation; a placed resource gives its heap's |
| `D3D12DDI_HT_DESCRIPTOR_HEAP`, `D3D12DDI_HT_QUERY_HEAP` | S_FALSE | 0: engine-internal memory, always resident |
| anything else, another device's object, a destroyed one | E_INVALIDARG | 0 |

The shell looks up each object, drops duplicates (a placed resource and its heap give the same allocation) and
issues one `pfnMakeResidentCb` (or `pfnEvictCb`) batch on the runtime's paging queue; an empty batch returns fence 0
and WaitMask 0. The lookup takes no lock and reads only what is fixed at creation, so it may run concurrently with
other DDI calls; the runtime keeps the objects of the call alive. The `Handle` of each entry is taken to be the
object's `pDrvPrivate` (INFERENCE, not logged).

Creation does not make anything resident. With these DDIs present, the driver must not create allocations resident
(DirectX-Specs `d3d/ResourceHeaps.md`, residency section); `allocate_memory` therefore allocates and maps the GPU
virtual address only, and `gpu_va` needs the mapping, not residency. The API makes objects resident at creation,
so the runtime presumably calls `pfnMakeResident` right after CreateHeapAndResource (INFERENCE; the first native
log settles it).

### Shaders and pipelines

The create-shader slots rebuild each program's container with shader-container `BuildContainer` (engine-ddi.h,
"Shaders"); nothing of the shell is involved. The shell's build links `shader-container.cpp` and
`dxil-metadata.cpp` through `engine-ddi.lib`, and must not compile them a second time.

Stream output. `StreamOutputSemantic` gives a gap in the declaration (RegisterIndex ~0u) as an entry with a NULL
SemanticName, and CreatePipelineState passes it to the engine. The r4 pin carries the fix (0869138a keeps the NULL
as a gap, c5d9d85f matches entries by stream); the r3 engine (4FFA7493, fork 7bfcd7f0) crashed on one.
`kEngineTakesStreamOutputGaps` in pipelines.cpp is true for the pin; false would refuse a gap with E_NOTIMPL. How
the runtime encodes a gap in the DDI is not measured.

Inferences, none measured against the runtime: CreateGeometryShaderWithStreamOutput may come without a program
(stream output of the vertex or domain program, as in the D3D11 DDI); a null blend, rasterizer or depth-stencil
handle means the API default; FrontEnable and BackEnable FALSE leave that face's stencil untouched; ScissorEnable
is TRUE (D3D12 has no such flag).

### Committed render targets

engine-ddi makes a committed resource as a heap and a resource placed at 0 (above). vkd3d-proton gives a placed
render target or depth-stencil resource no initial layout transition (`libs/vkd3d/resource.c`, placed resource
creation, at 7bfcd7f0): D3D12 requires the application to initialize a placed one with a clear, a discard or a
copy, but not a committed one. Left alone, a committed render target drawn to before a clear starts in
`VK_IMAGE_LAYOUT_UNDEFINED` (VVL `VUID-vkCmdBeginRendering-pRenderingInfo-09592`).

engine-ddi therefore initializes them itself, with no engine option (queue.cpp):

- CreateHeapAndResource of a committed texture with ALLOW_RENDER_TARGET or ALLOW_DEPTH_STENCIL queues the
  resource. Buffers, placed resources and other textures are not queued.
- The next `execute_command_lists` of the device first records `DiscardResource(resource, NULL)` for everything
  queued since the last batch on one internal DIRECT list, and submits it before that call's lists. Nothing of the
  device reaches a resource on the GPU before an ExecuteCommandLists, so the batch is in time. vkd3d-proton does
  not check the D3D12 state for a discard: every subresource goes from `VK_IMAGE_LAYOUT_UNDEFINED` to the
  resource's layout.
- Queue. engine-ddi has no queue of its own: in INLINE mode a queue exists only through the shell's BindQueue with
  a cookie the shell knows, and admission counts three graphics VkQueues. The batch runs on the executing queue when
  that is DIRECT, otherwise on a live DIRECT queue of the device. An internal fence is signalled after it, and
  every engine queue waits for the last signalled value on the GPU before its next lists. Until the device has a
  DIRECT queue, the resources stay queued.
- A batch is ordinary work of its queue for the release sequence. A resource destroyed while a batch names it
  hands its engine resource to its heap memory, which releases it, before the heap, once that work has retired.
  No batch waits on the CPU: a list whose last batch has not completed is not reused, another one is made.
- If a batch fails, engine-ddi reports the error through `report_device_error` and makes no further batches.

After the discard the content of a committed render target or depth-stencil resource is undefined, not zero:
engine-ddi never writes a value into it. Whether the memory arrives zeroed is the kernel's and the shell's matter,
and it is not measured. The DDI heap flags (`D3D12DDI_HEAP_FLAGS`, d3d12umddi.h) carry no counterpart of the
API's `D3D12_HEAP_FLAG_CREATE_NOT_ZEROED`. The kernel allocation flags have `AllowNotZeroed` (in, WDDM 2.6: zero
pages are not required) and `Zeroed` (out: the allocation was fulfilled by zero pages); no code in `driver/` sets
`AllowNotZeroed` at this revision. The KMD implements the paging Fill operations (`driver/kmd/wddm.c`,
WddmBuildPhysicalFill and WddmBuildVirtualFill) and reports `ZeroInPteSupported`, through which VidMm can initialize
allocation contents.

Development PC witness: the harness draws into a fresh committed target with no clear, word for word, and VVL
reports nothing (`tests/test-shaders.cpp`).

### Tiled resources

A reserved resource is `pfnCreateHeapAndResource` with no heap description, no heap handle and no base resource
(`ReuseBufferGPUVA` naming none): the INFERENCE of engine-ddi.h, the only shape of that call left for
CreateReservedResource. engine-ddi creates it with the engine's CreateReservedResource1 (legacy initial state) or
CreateReservedResource2 (barrier layout, castable formats); it makes no `allocate_memory` call, has a GPU VA and
no allocation handle, and `object_allocation` answers S_FALSE for it. GetMipPacking (D2) returns the engine's
packed tail from GetResourceTiling, CopyTiles (L12) goes to the engine list. Both are engine-ddi slots.

Q3 and Q4 are slots of the shell's queue table. The shell hook (in `native-queue-ddi.cpp`, today
`native_update_tiles` and `native_copy_tiles`, which call `queue_failure`):

```cpp
HRESULT engine_ddi::update_tile_mappings(EngineQueue* queue, D3D12DDI_HRESOURCE resource, UINT region_count,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* region_starts, const D3D12DDI_TILE_REGION_SIZE* region_sizes,
    D3D12DDI_HHEAP heap, UINT range_count, const D3D12DDI_TILE_RANGE_FLAGS* range_flags,
    const UINT* heap_range_starts, const UINT* range_tile_counts, D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept;
HRESULT engine_ddi::copy_tile_mappings(EngineQueue* queue, D3D12DDI_HRESOURCE dst,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* dst_start, D3D12DDI_HRESOURCE src,
    const D3D12DDI_TILED_RESOURCE_COORDINATE* src_start, const D3D12DDI_TILE_REGION_SIZE* size,
    D3D12DDI_TILE_MAPPING_FLAGS flags) noexcept;
```

1. Resolve the `D3D12DDI_HCOMMANDQUEUE` to its `QueueEngineSlot` and device as `native_execute` does.
2. Call through the registry the way `QueueEngineRegistry::execute` does: the slot goes from Live to Executing,
   the call gets `slot.engine` followed by the slot's arguments unchanged and in order (the header test pins both
   signatures against `PFND3D12DDI_UPDATETILEMAPPINGS` and `PFND3D12DDI_COPYTILEMAPPINGS`), then `check_health`,
   then back to Live. Two `QueueEngineOps` entries next to `execute` keep the registry's test seam.
3. Result. engine-ddi has already reported any failure through `report_device_error`. E_INVALIDARG is a refused
   call with nothing bound (unknown flags, a heap range beyond the heap, a resource that is not reserved); any
   other failure means the queue's retirement signal failed, which the shell treats like a failed
   `execute_command_lists`.

What engine-ddi does in the call: it takes the queue's submission lock, calls the engine queue's
UpdateTileMappings or CopyTileMappings (in INLINE mode the engine submits the sparse bind before returning),
signals the queue's retirement fence and processes retired releases. The heap is the engine heap of the heap
record; in RuntimeBacked mode that is CreateHeapFromMemory over the shell's import, so the tiles are bound to the
runtime allocation. Region bounds against the resource are the engine's check (it logs and drops a tile out of
range).

FL 12_0 needs tiled resources tier 2 from the engine. On unit A the engine reports tier 0 (M769) because hosted
RADV exposes no sparse binding without `RADV_EXPERIMENTAL=sparse` (M570): the slots work, but on unit A a
reserved resource fails in the engine until the engine reports a tier. Destroying a heap while a reserved
resource still maps tiles of it is the application's error in D3D12; engine-ddi does not track mappings.

Development PC witness (harness round trip 6, `tests/test-tiled.cpp`, RuntimeBacked on the stub shell): a
reserved buffer of 4 tiles and a reserved R32_UINT 256x256 texture mapped from a heap, written by copies and read
back word for word; one buffer tile remapped to a second heap (the buffer and both heaps checked);
CopyTileMappings into a second reserved buffer; CopyTiles from the texture to a linear buffer (each tile's texels
row by row) and on into a second reserved texture, read back as the first. VVL with synchronization validation
clean.

### Offline witness

The offline harness exercises the device path: copy, compute dispatch, a draw, the retirement sentinel and the
query slots above (EnginePrivateTest), and the RuntimeBacked heaps and tiled resources above, on the development PC with the pinned
engine DLL, also under VVL with synchronization validation. The dispatch and the draw use shaders created through
the DDI slots from containers reduced to the DDI form (a dxc cs_6_0 DXIL program; fxc vs_5_0 and ps_5_0 DXBC
programs), an element layout by register and DDI state objects. The reduction is the harness's model of the
runtime, not a measurement; the lab has yet to show the runtime's own payloads.
