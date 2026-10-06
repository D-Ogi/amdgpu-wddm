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
  instead of creating one that executes nothing. The runtime removes the device when pfnCreateCommandSignature
  fails with E_NOTIMPL (measured on the lab with a DISPATCH_RAYS argument: DXGI_ERROR_DEVICE_REMOVED, removed
  reason DXGI_ERROR_DRIVER_INTERNAL_ERROR), so engine-ddi translates every argument type of the DDI, mesh
  dispatch and the incrementing constant included, and answers an engine refusal or an unknown type with
  E_OUTOFMEMORY and a log line (commands.cpp); that the D3D12 runtime passes E_OUTOFMEMORY on without removing
  the device is an INFERENCE from the D3D10/11 rule (windows-driver-docs display `handling-errors.md`).
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
| 1006 | | RaytracingTier | the engine's 1_1 or higher as 1_1, else NOT_SUPPORTED | the acceleration structure, state object, pfnSetPipelineState1 and pfnDispatchRays slots reach the engine, collections and pfnAddToStateObject included ("Acceleration structures" and "Ray tracing state objects" below), and so does indirect ray dispatch (a DISPATCH_RAYS command signature), which the pinned engine traces up to the count (upstream traces the first record alone). Two gaps the tier promises: a collection imported with an export list answers E_NOTIMPL, and the runtime's state object description is not yet measured. `set_raytracing_tier_reporting(caps,false)` takes the answer back to NOT_SUPPORTED (the shell's experiment raytracing-tier-off) |
| 1006 | | VariableShadingRateTier, PerPrimitiveShadingRateSupportedWithViewportIndexing, AdditionalShadingRatesSupported, ShadingRateImageTileSize, VariableRateShadingSumCombinerSupported, MeshShaderPerPrimitiveShadingRateSupported | NOT_SUPPORTED, FALSE, 0 | pfnRSSetShadingRate and pfnRSSetShadingRateImage are fail-safes |
| 1006 | | MeshShaderTier, MeshShaderSupportsFullRangeRenderTargetArrayIndex, MSPrimitivesPipelineStatisticIncludesCulledPrimitives | NOT_SUPPORTED, FALSE, FALSE | pfnDispatchMesh and the mesh shader slots are fail-safes |
| 1006 | | SamplerFeedbackTier | NOT_SUPPORTED | pfnCreateSamplerFeedbackUnorderedAccessView is a fail-safe |
| 1006 | | EnhancedBarriersSupported | FALSE | pfnBarrier is a fail-safe |
| 1006 | | BackgroundProcessingSupported | FALSE | pfnSetBackgroundProcessingMode is the shell's slot; engine-ddi claims nothing for it |
| 1006 | | DriverManagedShaderCachePresent | FALSE | engine-ddi keeps no driver-managed shader cache |
| 1006 | | Deterministic64KBUndefinedSwizzle | FALSE | no engine answer |
| 1006 | | - | not in the DDI | `D3D12_FEATURE_DATA_D3D12_OPTIONS4::SharedResourceCompatibilityTier` and `D3D12_FEATURE_DATA_DISPLAYABLE::SharedResourceCompatibilityTier` have no DDI field: `SharedResourceCompatibilityTier` appears nowhere in `d3d12umddi.h`, and `D3D12DDI_D3D12_OPTIONS_DATA_0089` does not carry it. The D3D12 runtime synthesises the value from the WDDM/DDI level the driver reports (tier 1 "support is built into WDDM 2.4", `ref/sdk-api-docs` `ne-d3d12-d3d12_shared_resource_compatibility_tier.md`), so a driver that does not implement shared resources cannot withdraw the promise, and the enum has no "unsupported" value to report. On unit A the runtime answers tier 2 while every shared create and open fails (BD-075). What the driver can do honestly is refuse those calls without taking the device with it; `docs/d3d12-shared-resources.md` holds the plan that would make the reported tier true |
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

## Failures a create or an open DDI may report

`windows-driver-docs` display `handling-errors.md` puts every creation function of a user-mode display driver in
the AllowOutOfMemory category: the runtime admits `E_OUTOFMEMORY` and `D3DDDIERR_DEVICEREMOVED` and treats any
other failure as critical, which costs the application its device and sets the removed reason to
`DXGI_ERROR_DRIVER_INTERNAL_ERROR`. Measured twice on this stack: `E_NOTIMPL` out of
`pfnCreateCommandSignature` (above) and, in all 13 shared-resource cells of `tools/win/capture-share`,
`pfnCreateHeapAndResource` and `pfnOpenHeapAndResource` of a shared resource (BD-075, hr `0x887A0005`, removed
reason `0x887A0020`). That the D3D12 runtime follows the same rule as D3D10/11 stays an INFERENCE from that
document plus those two measurements.

`engine_ddi::admitted_create_failure` (engine-ddi.h) is therefore the last step of this module's
`pfnCreateHeapAndResource` and `pfnOpenHeapAndResource`: whatever the refusal decided inside the driver, the slot
reports `E_OUTOFMEMORY`, and the `log_refusal` line of the same call carries the real HRESULT, the heap and
resource description and, for a refusal the shell took, the admission check that took it (`heap-import.h`,
`ImportReport::refusal`). A lost device is the one other admitted answer, and it leaves under the name the runtime
admits: the clamp turns `DXGI_ERROR_DEVICE_REMOVED`, `_RESET` and `_HUNG` - the names the API shows the
application, which this module uses for a lost context and for `VK_ERROR_DEVICE_LOST` - into
`D3DDDIERR_DEVICEREMOVED`.

Two more layers sit above this one and refuse on their own, so each clamps its own refusals:

- the shell's wrapper of `pfnCreateHeapAndResource` (`native-tables.cpp`), because the owner scope around that
  slot can refuse as well;
- the DDI thunk (`ddi-entry.h`), which is what the runtime actually calls. It answers `E_INVALIDARG` for a device
  handle that resolves to nothing, `E_UNEXPECTED` for a scope it could not enter (engine teardown, a nested entry)
  or a missing original, and `E_FAIL` for an exception that is not `bad_alloc` - all of them refusals of a create
  DDI as far as the runtime can tell. `CoreBinding::allow_out_of_memory` names the two slots of this category, and
  `native12::ddi_admitted_create_failure` is the same rule; `native-tables.cpp` `static_assert`s that the two
  copies answer alike.

Other create slots are not covered yet; the shared-resource plan in `docs/d3d12-shared-resources.md` names the
audit.

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
  registered too. Optionally, right after `create_device_context` and before any queue, `set_retire_policy`: the
  retire hand-off, which leaves the release sequence of submission calls to the resource DDIs within a backlog and
  an age bound (engine-ddi.h). The shell sets it only under the diagnostic experiment `retire-handoff`.
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
| CheckFormatSupport | the engine's FORMAT_SUPPORT, mapped bit by bit; 0 when the engine refuses the format, except NOT_SUPPORTED for R10G10B10_XR_BIAS_A2_UNORM without an engine 2D texture (the runtime offers that format as a display format over a 0, 266); a packed video format engine-ddi stores gets its view format's answer within the bits the format list allows it ("Packed video formats" below) | engine answer |
| CheckMultisampleQualityLevels, Flags NONE | the engine's MULTISAMPLE_QUALITY_LEVELS; 0 when the engine refuses | engine answer |
| CheckMultisampleQualityLevels, Flags TILED_RESOURCE | the engine's MULTISAMPLE_QUALITY_LEVELS with the TILED_RESOURCE flag; 0 when the engine refuses | engine answer |
| GetDescriptorSizeInBytes | the engine's descriptor increment | engine answer |
| CheckResourceAllocationInfo, CheckExistingResourceAllocationInfo | the engine's GetResourceAllocationInfo for the description (for a stored packed video format, the description of its storage); no additional data. A description CheckResourceAllocationInfo cannot size gets ResourceDataSize UINT64_MAX, the API's error answer, with the default alignment for its sample count. Measured on unit A (native-caps284, 285, 64 x 64 YUY2 and R8G8_B8G8_UNORM textures): with E_INVALIDARG reported instead, the runtime removed the device at GetResourceAllocationInfo (DXGI_ERROR_DRIVER_INTERNAL_ERROR, SizeInBytes 0); with UINT64_MAX it answered SizeInBytes 0xFFFFFFFFFFFF0000 (UINT64_MAX aligned down to 64 KiB, no wrap), CreateCommittedResource E_OUTOFMEMORY, CreatePlacedResource E_INVALIDARG, and the device stayed | engine answer |
| EnumerateMetaCommands | count 0, S_OK | exact: engine-ddi has no meta commands |
| CheckDriverMatchingIdentifier | UNRECOGNIZED | exact: engine-ddi serializes nothing |
| ImplicitShaderCacheControl | no-op | exact: 1006 D3D12_OPTIONS reports DriverManagedShaderCachePresent FALSE |

Every other engine-ddi slot is implemented (SLOTS.md) or a fail-safe. A void fail-safe whose non-const pointers
are all `_Out_` zeroes them before it reports E_NOTIMPL, and each of these is a placeholder: returns 0 (the
E_NOTIMPL report is the difference from the table above): CheckSubresourceInfo,
GetMetaCommandRequiredParameterInfo. The first lab log of
D3D12CreateDevice on the native path (the engine-ddi log line "fail-safe slot D+0x... called") settles the list.

### Packed video formats

FL11_1 requires 2D textures of AYUV, Y410, Y416, YUY2, Y210 and Y216, and the runtime adds that support whatever
CheckFormatSupport answers (284: YUY2 answered 0 came back with TEXTURE2D and SHADER_SAMPLE). The engine has no image
of any of them. engine-ddi stores each as the typeless format of its element (internal.h, `StoredFormat`): AYUV and
YUY2 as R8G8B8A8_TYPELESS, Y410 as R10G10B10A2_TYPELESS, Y416, Y210 and Y216 as R16G16B16A16_TYPELESS. A 4:2:2 element
(YUY2, Y210, Y216) holds two pixels: the engine's width is half the resource's. The DXGI_FORMAT reference names the
view formats of each; they are the storage's family, plus R32_UINT for a UAV of a 4-byte element, which the engine
adds to a 4-byte typeless image with ALLOW_UNORDERED_ACCESS.

- Sizing and creation: one translation of the description serves CheckResourceAllocationInfo and
  CreateHeapAndResource, so the size answered is the size created. Only a single-sample 2D texture is stored; a
  4:2:2 texture also needs an even width and exactly one mip level (its mip widths in pixels and in elements part
  ways). Any other description of these formats gets UINT64_MAX and E_INVALIDARG.
- Views: an SRV, UAV or RTV format passes as given, except that a view naming the video format, or no format, of a
  stored resource gets the format's default view: R8G8B8A8_UNORM, R10G10B10A2_UNORM or R16G16B16A16_UNORM.
- Copies: CopyTextureRegion makes a footprint of a video format one of its storage, as many elements wide as its
  pixels fill, and divides a 4:2:2 copy's destination x and source box by two, the box's right edge rounded up.
  CopyResource copies storage to storage.
- Support: CheckFormatSupport answers the default view format's engine answer within the bits the format list allows
  the video format (sample, gather and typed UAV writes; AYUV also render target and blend).
  CheckMultisampleQualityLevels answers the view format's levels at one sample, none above.
- Harness (`tests/test-stored-formats.cpp`): each format sized as its storage; three 8 x 4 textures of each, one
  filled from a footprint, one cleared through a UAV naming the video format, one through the UINT view (R32_UINT for
  a 4-byte element); copies with a box and an offset in pixels, from a footprint and between textures; all read back
  byte for byte. Without the copy translation the 4:2:2 rows differ; without the view translation the engine finds no
  view format ("Failed to find format") and the harness dies. Not yet run on unit A.

Known gaps, refused (UINT64_MAX, E_INVALIDARG) while the runtime still offers their FL11_1 support:

- R8G8_B8G8_UNORM and G8R8_G8B8_UNORM: a sampled view reconstructs the shared channel per pixel, which no store as a
  typeless element gives.
- NV11: planar, two planes.
- A mip chain of a 4:2:2 format. YUY2's legacy R8G8_B8G8_UNORM view at twice the width reaches the engine as given and
  is refused there.
- A reserved (tiled) texture of a stored format is created, with the storage's tile shape and coordinates; untested.

### Heap memory: allocate_memory and free_memory

pfnCreateHeapAndResource with a heap description (committed, or a heap alone) makes one `allocate_memory` call;
a placed resource (resource description only, `ReuseBufferGPUVA` naming a resource of the heap) makes none.
engine-ddi then calls the engine's `CreateHeapFromMemory` (ABI 1.2 V10) over the memory, and places a committed
resource at offset 0. MapHeap and UnmapHeap go to the engine's MapHeap and UnmapHeap: the CPU address of heap
offset 0, a placed buffer at that address plus its offset.

The linear primary (boundary r4, engine ABI 1.3 V13). A committed texture on a heap with
`D3D12DDI_HEAP_FLAG_PRIMARY` is what a reader outside the engine opens and reads by row pitch. When its
description is one the surface exists for (2D, one mip, one layer, one sample, a format the surface format table
`driver/contract/amdgpu_wddm_surface_format.h` enables for composition - today B8G8R8A8_UNORM, R8G8B8A8_UNORM,
R10G10B10A2_UNORM and R16G16B16A16_FLOAT - at most 8192 on an edge, heap without CPU access, castable formats none beyond the format and
its sRGB sibling where the table names one), engine-ddi:

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
256x256 B8G8R8A8_UNORM, 127x79 R8G8B8A8_UNORM, 200x120 R10G10B10A2_UNORM and 136x72 R16G16B16A16_FLOAT (scRGB
values 2.0, -0.25, 0.5, 1.0, which a clamping or 4-byte surface would not keep) primaries are created, cleared
through a render target view,
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

### Acceleration structures

D108 GetRaytracingAccelerationStructurePrebuildInfo, L60 BuildRaytracingAccelerationStructure, L61
EmitRaytracingAccelerationStructurePostbuildInfo and L62 CopyRaytracingAccelerationStructure are engine-ddi slots
on the engine's `ID3D12Device5` and `ID3D12GraphicsCommandList4` (commands.cpp); the list slots are in both tables.
The DDI 0054 structures have the API's members at the API's offsets (static_asserts in commands.cpp against
d3d12umddi.h and d3d12.h of 10.0.26100), so the inputs, the geometry descriptions, the prebuild answer and the
postbuild descriptions reach the engine in place; the build, emit and copy arguments are unpacked into the API's
parameters. Nothing of the shell is involved.

Refused, reported once on the list (D108: on the device, with the answer zeroed) and not passed on:
- E_INVALIDARG: a list that is not recording or not DIRECT or COMPUTE (a bundle, a COPY list), a null argument, an
  unknown structure type, element layout, geometry type, postbuild type or copy mode, a geometry or source array
  missing for a nonzero count.
- E_NOTIMPL: the tools visualization postbuild type, and the visualization, serialize and deserialize copy modes.
  The engine copies by clone and compaction only and writes a zero or nothing for the others
  (`libs/vkd3d/acceleration_structure.c`); engine-ddi serializes nothing.

GPU addresses are the application's, as in every other slot. RaytracingTier stays NOT_SUPPORTED (the 1006 row
above); that the runtime passes these arguments as the harness does is INFERENCE.

Development PC witness (harness round trip 8, `tests/test-raytracing.cpp`, RuntimeBacked on the stub shell):
prebuild answers equal to the engine's own; a bottom level of one triangle, its clone, a top level of one instance
over each, and a cs_6_5 inline ray query per top level (the top level as a root SRV by address) whose 64 words equal
the hit pattern computed from the triangle; CURRENT_SIZE at the build and emitted after it, equal and within the
prebuild maximum; a build on a recording bundle and one on a recording COPY list are each one E_INVALIDARG on that
list (build controls; no DispatchRays runs in a bundle or COPY list in the harness). VVL with synchronization
validation clean. It is RuntimeBacked because the engine places a
structure from its address to the end of the VkBuffer behind it (`libs/vkd3d/va_map.c`): in EnginePrivateTest small
heaps are suballocated from one shared buffer, and the validation layer reports every structure as overlapping the
buffers after it.

### Ray tracing state objects

D105 CalcPrivateStateObjectSize, D106 CreateStateObject, D107 DestroyStateObject, D110 GetShaderIdentifier, D111
GetShaderStackSize, D112 GetPipelineStackSize and D113 SetPipelineStackSize are engine-ddi slots on the engine's
`ID3D12Device5::CreateStateObject` and `ID3D12StateObjectProperties` (state-objects.cpp); D115
CalcPrivateAddToStateObjectSize and D116 AddToStateObject are engine-ddi slots on the engine's
`ID3D12Device7::AddToStateObject`, queried at context creation (absent: E_NOTIMPL for growth only). L63
SetPipelineState1 and L64 DispatchRays go to the engine's `ID3D12GraphicsCommandList4` (commands.cpp, both tables;
the dispatch argument is the API's `D3D12_DISPATCH_RAYS_DESC`, size, offsets and alignment asserted). Indirect ray
dispatch is pfnExecuteIndirect with a command signature of a DISPATCH_RAYS argument (commands.cpp), created and
executed like the draw and dispatch signatures; its argument buffer holds the application's `D3D12_DISPATCH_RAYS_DESC`
records, which the engine reads as they are. vkd3d-proton upstream traces the first record alone and nothing with a
count buffer; the fork's fix (vkd3d-proton fork 4e572c10 and e1ce3e7e, on d0c089a1) unrolls one
`vkCmdTraceRaysIndirect2KHR` per record and, with a count buffer, first copies the records into scratch, those at or
past the count with zero dimensions over a zeroed, aligned table region. Refused:
an existing collection imported with an export list (E_NOTIMPL, temporarily, below) and work graphs (state object type
EXECUTABLE). The shell needs no new
resolver for D115 and D116: both carry the device handle first (d3d12umddi.h:9190-9191), which `entry-owner.h`
already resolves.

CreateStateObject rebuilds the API description from the DDI's (d3d12umddi.h 10.0.26100; DirectX-Specs
Raytracing.md, "State object DDIs"), subobject by subobject:
- STATE_OBJECT_CONFIG, NODE_MASK (0 or 1), RAYTRACING_SHADER_CONFIG: copied; unknown flags are E_INVALIDARG.
- GLOBAL_ and LOCAL_ROOT_SIGNATURE: the handle's engine root signature; a null handle or another device's is
  E_INVALIDARG.
- DXIL_LIBRARY: `pDXILLibrary` has no size. Its length is the length the payload claims, checked for internal
  consistency: SizeInUint32 (DWORD 1) of a DXIL part, or the container size (bytes 24 to 27) when it starts with
  `DXBC`; above 16M DWORDs (64 MiB) it is refused. The internal sizes (part offsets and sizes, the program header, the
  bitcode offset and size) are validated against it; nothing in the DDI proves the backing extends that far, and
  readability up to it rests on the runtime. shader-container
  `BuildLibraryContainer` writes a container of that DXIL part alone (of a container, its DXIL part; RDAT and the
  rest dropped), which is what the engine's DXIL front end parses (dxil-spirv `parse_container`); program kind 6 is
  required. The export array is passed in place (layout asserted). The host gate is the harness's lib_6_3 fixture,
  whose capacity is known; the runtime's own library payload is recorded by the create's log line when first seen.
- RAYTRACING_PIPELINE_CONFIG: read as `D3D12DDI_RAYTRACING_PIPELINE_CONFIG_0075` and passed as the API's
  RAYTRACING_PIPELINE_CONFIG1.
- HIT_GROUP: copied field by field, without SummaryFlags.
- SHADER_EXPORT_SUMMARY: not passed on. For each root signature or configuration the summary associates with an
  export, one SUBOBJECT_TO_EXPORTS_ASSOCIATION names every export associated with it. An association with a
  STATE_OBJECT_CONFIG or NODE_MASK adds nothing: they hold for the whole state object. The name that identifies a
  summary export is resolved once, never broadened: a name a library lists in its export array (an exposed alias,
  plain or mangled) or a hit group's name is kept; otherwise the unmangled name when no other summary export carries
  it; otherwise the mangled name when no other summary export carries it (an export with one name only goes by it,
  under the same uniqueness). The listed names of every library count, whatever the others do: the engine knows a
  listed export by its listed name alone, with a NULL mangled name (`libs/vkd3d-shader/dxil.c:2367-2407`), so a
  library listing a mangled name keeps it even beside a library with no export list. Anything else is E_INVALIDARG
  without an engine call: an unresolved or ambiguous identity, an export that is not among the listed ones when no
  library exports everything, or two summary exports resolving to the same name. The engine matches an association name against an export's
  mangled or plain name (`libs/vkd3d/state_object_common.c`, `vkd3d_export_equal`), and a plain name that overloads
  share would associate with all of them; overloads are told apart by the mangled name (Raytracing.md,
  D3D12_EXPORT_DESC; the summary node carries both names, "Shader export summary").
- EXISTING_COLLECTION: the handle must name a live COLLECTION of this device (else E_INVALIDARG); the API
  subobject carries its engine object with NumExports 0, and the importer's record takes one engine reference to it,
  released at the importer's destroy. An export list (NumExports != 0) is valid input and E_NOTIMPL for now, with a
  log line: the pinned engine indexes that list with the wrong loop variable for a deferred collection
  (`raytracing_pipeline.c:714-720`, `exports[i]` for `exports[j]`, in `d3d12_state_object_add_collection_deferred`);
  it waits for an engine revision with the fix and a restricted, reordered import control. For export identity an
  imported collection is a listing source: the names it exposed at its own create, which engine-ddi keeps as its own
  copy in the collection's record (resolved names, hit group names and what it imported), never names rebuilt from
  the summary.

Any other type: E_INVALIDARG, never passed through.

Every count read from the description is bounded: 65536 subobjects, library exports, summary nodes, summary exports in
all and associations per export, 1M associated names in all. A failed create returns its HRESULT, logs the reason
and leaves an inert record that DestroyStateObject accepts; nothing is reported through report_device_error. Each
create logs one line with the subobject types, the library form and length, the pipeline configuration and the
summary's counts, including how many export names were resolved by listed name, mangled and unmangled name.

Lifetime: what the translated description points at (the rebuilt description, the containers, the association name
arrays) must stay put until the engine's CreateStateObject returns; the engine deep-copies what it keeps
(`d3d12_state_object_pipeline_data_defer` and the parse data). engine-ddi keeps its copies until DestroyStateObject;
that is a conservative choice, not an engine requirement. Names and export arrays of the DDI description are used in
place: the runtime owns that description (Raytracing.md, "State object DDIs").

Documented, not inferred (DirectX-Specs Raytracing.md 9490-9498, revision 5a4139be): the runtime hands the driver
subobjects defined in DXIL libraries as plain DDI subobjects (the driver does not find them in the library it gets),
and converts every association, default associations included, into an explicit list of associations for every
exported function.

The engine also treats every declared root signature and configuration as a default for all exports (engine
66c98e72: `libs/vkd3d/raytracing_pipeline.c:992-1030`, priority DECLARED_STATE_OBJECT 4 from :1316-1326; priorities
`libs/vkd3d/vkd3d_private.h:6502-6508`). The synthesized associations are explicit (EXPLICIT 6, :1251-1259) and take
precedence for every export the summary names. An absent local root association stays absent: when a
RAYTRACING_PIPELINE declares a local root signature and the summary associates none with some export, the
translation adds the device context's empty local root signature (a real engine root signature with
D3D12_ROOT_SIGNATURE_FLAG_LOCAL_ROOT_SIGNATURE and no parameters, created on first use, released with the context; a
failure is the create's HRESULT) and a SUBOBJECT_TO_EXPORTS_ASSOCIATION of it with no exports. That is an explicit
default (EXPLICIT_DEFAULT 5, :1257-1259): it outranks the declared defaults (`state_object_common.c:111-137` takes
the highest priority match) and yields to every explicit association, a hit group's included, which the engine takes
for the group's shaders only at a higher priority than theirs (`state_object_common.c:176-201`). Explicit empty
associations per export (6) would tie with a hit group's there and keep the empty one, so the default is the form
used. A COLLECTION is left as it is: there an absent association may be an unresolved dependency, not an absence.
The create's log line counts the exports concerned.

Inherited associations are not covered. How the runtime describes, in an importer's summary, the associations an
imported collection made is not measured; the harness's importer restates none of them, and an association with a
subobject outside the importer's own description stays refused (E_INVALIDARG) and logged, as before.

AddToStateObject translates the addition, a description valid on its own (Raytracing.md:3781), with the same
translation as a create, and calls the engine's `ID3D12Device7::AddToStateObject` with the parent's engine object;
the parent must be a live RAYTRACING_PIPELINE of this device and the addition a RAYTRACING_PIPELINE (else
E_INVALIDARG). The engine checks ALLOW_STATE_OBJECT_ADDITIONS on both (`raytracing_pipeline.c:2866-2884`) but no
name collision, so an addition's summary export or hit group named like an export the parent exposes is E_INVALIDARG
before the engine (Raytracing.md:3787). The child's record holds engine references only: one to the parent's
engine object, and its own copy of the names the parent exposed; nothing of the parent's record or translation,
because the runtime destroys a parent while its children live (Raytracing.md:9667-9669). Deviation from bare
forwarding: after a successful growth the bridge copies the parent's current pipeline stack size to the child. The
child starts with the parent's setting (Raytracing.md:3777), and the pinned engine gives it its computed default
instead (`raytracing_pipeline.c:2749-2752`, reached through `d3d12_rt_state_object_create` at :2846).

GetShaderIdentifier answers the engine's pointer (NULL for an unknown export), GetShaderStackSize UINT_MAX for an
unknown export, and a stack size above UINT_MAX is logged and answered as UINT_MAX (the DDI's type is 32 bits, the
engine's 64). SetPipelineState1 takes a live RAYTRACING_PIPELINE of the list's device; a collection, a failed
create's record, a null handle or another device's state object is E_INVALIDARG on the list. Both list slots record
in DIRECT and COMPUTE lists that are recording only, as the acceleration structure slots; the harness shows the
closed-list refusal for DispatchRays, and the bundle and COPY refusals only for a build (round trip 8).

The shell's entry thunk enters its device scope from a slot's first handle; for D110-D113 that is the state object,
and `state_object_shell` answers its owner from the record (the device's ShellHooks::shell for a live record and for
the inert record of a failed create, null for storage holding no record, such as after DestroyStateObject).

INFERENCE until a lab run logs a real description (the create's log line is the instrument): that the runtime hands
over the DDI form the harness builds, in particular that a 0092 driver receives RAYTRACING_PIPELINE_CONFIG as _0075
(the header names both layouts), that `pDXILLibrary` is a DXIL part with SizeInUint32 as for shaders (a whole
container is also accepted), and that the summary's subobject pointers point into `pSubobjects` (a pointer elsewhere
is matched by type and description).

Development PC witness (harness round trip 9, `tests/test-raytracing.cpp`, RuntimeBacked on the stub shell): a dxc
lib_6_3 library (`tests/fixture-raylib.hlsl`) with raygen, miss and closest, one triangles hit group with a local
root signature of one 32-bit constant, a global root signature SRV(t0) UAV(u0) and one 32-bit constant in b0 space2
(the value miss writes), in the harness's model of the DDI
form (DDI types, handles, a summary with per-export subobject pointers, no association subobject). Identifiers of
32 bytes, not all zero and different for raygen, miss and hit group; stack sizes answered; a shader table in an
UPLOAD buffer and DispatchRays 8x8 over the scene of round trip 8, every hit writing the local root constant and every
miss 2, the 64 words exact. The library as a whole container with no export list, plus a decoy local root signature
(closest's register one word later) associated with a second `closest` summary export that shares the plain name
and carries a different mangled name: the create resolves both by mangled name, and a second dispatch through its
own table writes the word closest's own local root signature selects, 64 words exact, not the next word the decoy's
would select. The decoy has no DXIL function behind it, so this shows the naming of the associations, not a
selection among real overloads. The description handed to the engine names closest's local root signature's
association by closest's mangled name alone and the decoy's by the decoy's, neither by the shared plain name. The same
decoy without a mangled name, or with both of closest's names, is
E_INVALIDARG, never a broadened association; a control build that passed plain names made the engine's create fail
(8007000e).

The descriptions are checked on the host: a harness-only observer (`harness_set_state_object_observer`, compiled only
with `AMDGPU_WDDM_ENGINE_DDI_HARNESS`, null by default) copies the API description immediately before the engine's
CreateStateObject. Mixed libraries (a second fixture, `tests/fixture-raylib-b.hlsl`, one miss shader `miss_far`): the
first library lists closest by its mangled name alone, without a rename, beside the second with no export list, in
both orders. The description handed to the engine has the libraries in that order, closest's listed mangled name alone
in its local root signature's association, and raygen, miss, that mangled name and `miss_far` in the global one; a
dispatch per order writes its own record constant, 64 words exact. A control build that dropped the listed names beside
the export-all library failed both the description check and the create (8007000e, the unmangled `closest`, which the
engine does not know the listed export by).

An absent local root association: closest's local root signature (the trap) holds its constant in b0 space1 and a
second one in b0 space4, a register no global parameter has; the summary associates no local root signature with
raygen and miss. The description handed to the engine declares the context's empty local root signature last with an
association of no export (the explicit default), the same object for every create of the context, and associates the
trap with `closest` alone; raygen and miss are in no local root association. A third dispatch with the empty default
in place writes the global 2 on all 52 misses and the record constant on the 12 hits, 64 words exact. The pixels do
not discriminate: raygen and miss read none of the trap's registers. A control build without the empty explicit
default failed the two description checks (no explicit default, one declared local root signature).

A collection-only executable: a COLLECTION of the library, then a pipeline importing it with EXISTING_COLLECTION of
all exports and no DXIL library. The description handed to the engine carries the collection's engine object with
NumExports 0, no library, no local root signature, and the global root signature's association naming raygen, miss
and closest, the names the collection exposed. The same import with an export list is E_NOTIMPL with no engine call.
The executable's record adds one public reference to the collection's engine object (2, then 3); the collection's DDI
object is destroyed before the executable is used, and a dispatch through the executable's table writes its own
constant on the 12 hits and 2 on the 52 misses, 64 words exact. Growth: a pipeline allowing additions, its
identifiers taken and its pipeline stack size set 4096 above its computed one (0 on this PC), grown by
`fixture-raylib-b` listing `miss_far`. The child's stack size read before any set is 4096, its raygen identifier is
the parent's, and the description handed to the engine is grown from the parent's engine object with one library
listing one export and the global association naming `miss_far` alone. The child's record adds one public reference
to the parent's engine object (2, then 3); the parent's DDI object is destroyed before the child is used, and a
dispatch through a table of the parent's raygen and hit group identifiers and `miss_far`'s writes the child's constant
on every hit and 3 on every miss, 64 words exact. Refused growth, one E_INVALIDARG each and no device error: from a
pipeline without ALLOW_STATE_OBJECT_ADDITIONS (the engine's check, one engine call) and an addition exporting `miss`
(engine-ddi's check, no engine call). Controls, each built into its own scratch directory and the source restored and
compared after: without the stack size copy the child read 0; without collection names as a listing source the
importer's create failed (E_INVALIDARG); without the collision check the engine accepted the colliding addition (S_OK);
without the held reference to the collection, or to the parent, the public count stayed 2. The dispatches passed in
the last two controls: the engine's own internal references keep an imported collection and a parent alive
(`raytracing_pipeline.c:2762-2769`), so engine-ddi's held references are for the lifetime contract, not the witness's
pixels.

Indirect ray dispatch: a command signature of one DISPATCH_RAYS argument, stride 128 (above the record's 104 bytes),
no root signature, S_OK. Two records over the first pipeline's table, 8x4 then 8x8, and the count words 1, 2, 0 and
3; six ExecuteIndirect variants (max 1; max 2 with count 1, 2, 0 or 3; max 2 without a count buffer), each into its
own prefilled result, first from an UPLOAD buffer with no INDIRECT_ARGUMENT transition before them in the list (the
engine may patch ahead of the list), then from a DEFAULT copy behind one (patched in the list). With the fork's fix
every record up to the smaller of count and maximum is traced: rows 0 to 3 for max 1 and count 1, none for count 0,
all 64 words for the rest, the other rows at the prefill, from both buffers. One record without a count buffer is
exact on every engine; the pinned engine and the registered 106D09E5 trace the first record alone and nothing with a
count buffer, exactly the pattern the round trip reports as one SKIP line. A first version of the fix wrote the
records past the count as all zeros: zero shader table addresses lost the device on the development PC's NVIDIA GPU
although nothing was to be launched (VK_ERROR_DEVICE_LOST in this round trip); the fix names a zeroed, aligned
scratch region for their tables instead.

Refusals: an unknown subobject type
(E_INVALIDARG), a ray tracing pipeline named as an existing collection (E_INVALIDARG), DispatchRays on a closed list and SetPipelineState1 with another
device context's state object (one E_INVALIDARG on the list). `state_object_shell` names the device's shell for a
live state object and for a refused create's inert record, and nothing once either is destroyed. VVL with
synchronization validation clean.

### Offline witness

The offline harness exercises the device path: copy, compute dispatch, a draw, the retirement sentinel and the
query slots above (EnginePrivateTest), and the RuntimeBacked heaps, tiled resources, acceleration structures and ray
tracing pipeline above, on the development PC with the pinned
engine DLL, also under VVL with synchronization validation. The dispatch and the draw use shaders created through
the DDI slots from containers reduced to the DDI form (a dxc cs_6_0 DXIL program and a cs_6_5 ray query; fxc
vs_5_0 and ps_5_0 DXBC programs), an element layout by register and DDI state objects; the ray tracing pipeline uses
a dxc lib_6_3 library's DXIL part in a DDI state object description. The reduction is the harness's model of the
runtime, not a measurement; the lab has yet to show the runtime's own payloads.
