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
    `amdgpu-wddm/ddi-engine-1.2-wip`, commit `7bfcd7f09f078f5c312c34180fc9c51c5b8a0698`;
  - revision r3-draft, ABI 1.2, NOT FROZEN in the header's own words;
  - SHA-256 `768692FC98C5A267721AC1EE38C8CD49B6EE776D2BF562955FF292C30D472DC2`;
  - default checkout `<workspace>\scratch\m15\vkd3d-1.2-src` (`-EngineSource`), the directory
    `tools/build/build-umd-d3d12.ps1` already uses.
- Vulkan headers: `khronos/Vulkan-Headers/include` of the same checkout, submodule commit
  `ee2ec5fd83dafce291024683b50dc89219333076`.
- Engine DLL: `amdgpu_wddm_vkd3d.dll` built by `tools/build/build-vkd3d.ps1` (config `ddi-engine`) from that
  commit with no local changes, SHA-256 `4FFA7493DD818E3B3AB0BA3D988AFC05BDBCBC234726EFC52C54CA37890E833C`.
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
```

1. OpenAdapter12, or at the latest the first GetCaps:
   - load the engine DLL and call `Bc250Vkd3dEngineGetFuncs(BC250_VKD3D_ENGINE_ABI_VERSION, &funcs)` with
     `funcs.Size = sizeof(funcs)`. The argument must be 1.2: the engine leaves QueryAdapterCaps NULL for a shell
     that asks for 1.1, and `query_adapter_caps` then returns E_INVALIDARG;
   - fill the `BC250_VKD3D_DEVICE_CREATE_INFO` that CreateDevice will receive: `AbiVersion` 1.2, hosted RADV's
     `GetInstanceProcAddr`, the `AdapterLuid` hosted RADV reports for this adapter (V2), `MinimumFeatureLevel`
     `D3D_FEATURE_LEVEL_11_0`, `QueueMode` INLINE and the shell's `Services`. The engine applies the INLINE
     admission to it (three graphics-family VkQueues in ABI 1.2), and its answers are those of a device made from
     this info (V11), so the same info must go to CreateDevice;
   - call `query_adapter_caps` once and keep the result with the adapter. One call is one QueryAdapterCaps batch
     and one VkInstance; no VkDevice is created and no Service is called. On failure there is nothing to answer
     GetCaps from: fail OpenAdapter12 (or that GetCaps) with the returned HRESULT.
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
Layout references are WDK 10.0.26100 `um/d3d12umddi.h` ("H:"). Every type not listed returns E_NOTIMPL and logs
type and size; so do 1000, 1003, 1013, 1057, 1059 (its payload's meaning is not documented, H:1431-1434), 1066,
1069, 1070, 1071 and 1075. A wrong DataSize is E_INVALIDARG with nothing written.

L below is min(level of FEATURE_LEVELS.MaxSupportedFeatureLevel, 12_1), asked for {11_0, 11_1, 12_0, 12_1, 12_2}.
The engine-ddi ceiling is 12_1 because 12_2 needs raytracing 1.1, mesh shaders, VRS tier 2 and sampler feedback,
which this revision reports as unsupported (fail-safe slots in SLOTS.md).

| Type | Payload (bytes, H:) | Field | Value | Source or reason |
|---|---|---|---|---|
| 1074 3DPIPELINESUPPORT1 | `D3D12DDI_3DPIPELINESUPPORT1_DATA_0081`, 8, H:10415-10420 | MaximumDriverSupportedFeatureLevel | min(L, HighestRuntimeSupportedFeatureLevel) | FEATURE_LEVELS; "highest ... that does not exceed what the runtime understands", H:10377-10378 |
| 1007 3DPIPELINESUPPORT | `D3D12DDI_3DPIPELINELEVEL` itself, 4, H:2922-2933 | the level | L | FEATURE_LEVELS; never above 12_1, H:10373 |
| 1012 SHADER_MODELS | `D3D12DDI_D3D12_SHADER_MODELS_DATA_0011`, 16, H:3502-3507 | `*pNumShaderModelsSupported`, `pShaderModelsSupported` | every release model from 5_1 to min(M, 6_6); the count is always written, the array when non-NULL, E_INVALIDARG when its count is smaller | SHADER_MODEL asked with 6_6 gives M; 6_6 is the last release model at 0092 (6_7 is 0093, H:3478-3500) |
| 1006 D3D12_OPTIONS | `D3D12DDI_D3D12_OPTIONS_DATA_0089`, 124, H:11078-11112 | ResourceBindingTier, ConservativeRasterizationTier, CrossNodeSharingTier, ResourceHeapTier, OutputMergerLogicOp, VPAndRTArrayIndexFromAnyShaderFeedingRasterizerSupportedWithoutGSEmulation | same value | OPTIONS; DDI and API enums are equal (static_assert in caps.cpp) |
| 1006 | | TiledResourcesTier | OPTIONS tier, 4 reported as 3 | OPTIONS; no DDI tier 4 at 0092 (H:709-715). Gap: reserved resources return E_NOTIMPL and CopyTiles is a fail-safe until tiled resources land (P2), although FL12_0 implies tier 2 |
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
| 1002 MEMORY_ARCHITECTURE | `D3D12DDI_MEMORY_ARCHITECTURE_CAPS_0041`, 20, H:6806-6814; pInfo NULL or node 0 (H:152-155), else E_INVALIDARG | UMA, CacheCoherent | UMA, CacheCoherentUMA | ARCHITECTURE1 |
| 1002 | | HeapSerializationTier | 1 when the API tier is 10, else 0 | SERIALIZATION; DDI tiers 0 and 1, H:6793-6797 |
| 1002 | | IOCoherent, ResourceSerializationTier | FALSE, 0 | no engine answer |
| 1009 GPUVA_CAPS | `D3D12DDI_GPUVA_CAPS_0004`, 4, H:250-257; pInfo NULL or node 0 | MaxGPUVirtualAddressBitsPerResource | same value | GPU_VIRTUAL_ADDRESS_SUPPORT |
| 1060 TEXTURE_LAYOUT | `D3D12DDI_TEXTURE_LAYOUT_CAPS_0026`, 20, H:5525-5536; pInfo NULL, else E_INVALIDARG (no swizzle patterns) | Supports64KStandardSwizzle | StandardSwizzle64KBSupported | OPTIONS |
| 1060 | | DeviceDependentLayoutCount, DeviceDependentSwizzleCount, SupportsRowMajorTexture, IndexableSwizzlePatterns | 0, 0, FALSE, FALSE | no device-dependent layouts; no 1:1 engine answer for row-major textures |
| 1062 UMD_BASED_COMMAND_QUEUE_PRIORITY | `D3D12DDICAPS_UMD_BASED_COMMAND_QUEUE_PRIORITY_DATA_0023`, 4, H:5140-5143 | SupportedQueueFlagsForGlobalRealtimeQueues | NONE | no realtime queues |
| 1067 HARDWARE_SCHEDULING_CAPS | `D3D12DDICAPS_HARDWARE_SCHEDULING_CAPS_0050`, 4, H:7004-7008 | ComputeQueuesPer3DQueue | 0 | "0 means don't use scheduling groups", H:7007 |
| 1077 OPTIONS_0090 | `D3D12DDI_OPTIONS_DATA_0090`, 4, H:11127-11131 | RelaxedFormatCastingSupported | same value | OPTIONS12 |
| 1078 OPTIONS_0091 | `D3D12DDI_OPTIONS_DATA_0091`, 16, H:11143-11150 | the four fields | same values | OPTIONS13 |

FEATURE_LEVELS, SHADER_MODEL, ARCHITECTURE1, GPU_VIRTUAL_ADDRESS_SUPPORT, OPTIONS and OPTIONS1 are required:
`query_adapter_caps` fails with the engine's Result when one is unanswered. The others are optional: an
unanswered one is logged and its fields report no support.

## Device

- FillDDITable: `fill_device_core` and `fill_command_list` (table 0 compute, 1 graphics) first, then the shell's
  own slots over the 18 core slots engine-ddi leaves NULL, and the shell's `pfnPresent`.
- CreateDevice: the engine's `CreateDevice` with the adapter's create info, then `create_device_context`
  (`MemoryMode::RuntimeBacked`, the shell's hooks). DestroyDevice: `destroy_device_context`; S_FALSE with live
  objects keeps the context.
- Queues (the queue table is the shell's): after creating the WDDM context, `create_engine_queue`;
  ExecuteCommandLists: `execute_command_lists`; DestroyCommandQueue: `destroy_engine_queue`.
- Present: `resource_allocation` for the back buffer's runtime allocation.

### Query slots around CreateDevice

Which device slots the runtime calls during D3D12CreateDevice has not been observed: the list below is an
INFERENCE from the slot types (queries and controls the runtime can call before any object exists). Each gives a
defined answer and reports no error:

| Slot | Answer |
|---|---|
| CheckFormatSupport | the engine's FORMAT_SUPPORT, mapped bit by bit; 0 when the engine refuses the format |
| CheckMultisampleQualityLevels | the engine's MULTISAMPLE_QUALITY_LEVELS for Flags NONE; 0 when the engine refuses; 0 for TILED_RESOURCE (no reserved resource can be created) |
| GetDescriptorSizeInBytes | the engine's descriptor increment |
| CheckResourceAllocationInfo, CheckExistingResourceAllocationInfo | the engine's GetResourceAllocationInfo for the description; no additional data |
| EnumerateMetaCommands | count 0, S_OK |
| CheckDriverMatchingIdentifier | UNRECOGNIZED |
| ImplicitShaderCacheControl | no-op: no driver-managed shader cache is reported (1004) |

Every other engine-ddi slot is implemented (SLOTS.md) or a fail-safe. A void fail-safe whose non-const pointers
are all `_Out_` zeroes them before it reports E_NOTIMPL: GetMipPacking, CheckSubresourceInfo,
GetRaytracingAccelerationStructurePrebuildInfo, GetMetaCommandRequiredParameterInfo. The first lab log of
D3D12CreateDevice on the native path (the engine-ddi log line "fail-safe slot D+0x... called") settles the list.

Not ready on the device side: RuntimeBacked heap creation returns E_NOTIMPL after validating the shell's
allocation, because ABI 1.2 V10 (`CreateHeapFromMemory` over memory the shell allocates on the engine's
VkDevice) is not wired yet; native shader intake returns E_NOTIMPL (SLOTS.md). Only the offline harness
(EnginePrivateTest, engine-owned heaps) exercises the device path: copy, compute dispatch, the retirement
sentinel and the query slots above pass on the development PC with the pinned engine DLL, also under VVL with
synchronization validation.
