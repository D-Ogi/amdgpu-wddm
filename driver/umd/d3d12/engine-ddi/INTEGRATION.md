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
  (`MemoryMode::RuntimeBacked`, the shell's hooks, the engine's ABI 1.2 function table: it refuses one without
  CreateHeapFromMemory, MapHeap and UnmapHeap). Ordering: the context is created and registered, so that the
  shell's `ResolveDevice` returns it for this `D3D12DDI_HDEVICE`, before CreateDevice returns S_OK, and it stays
  registered and live until DestroyDevice. A failed `create_device_context` fails CreateDevice. Every engine-ddi
  slot finds its device only through `ResolveDevice`; a slot called while it returns null answers nothing (a query
  leaves its output zeroed or untouched) and reports no error, because there is no device to report to.
  DestroyDevice: `destroy_device_context`; S_FALSE with live objects keeps the context, and then it stays
  registered too.
- Queues (the queue table is the shell's): after creating the WDDM context, `create_engine_queue`;
  ExecuteCommandLists: `execute_command_lists`; DestroyCommandQueue: `destroy_engine_queue`.
- Present: `resource_allocation` for the back buffer's runtime allocation.

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
| CheckMultisampleQualityLevels, Flags TILED_RESOURCE | 0 | placeholder: returns 0 (until reserved resources land, P2) |
| GetDescriptorSizeInBytes | the engine's descriptor increment | engine answer |
| CheckResourceAllocationInfo, CheckExistingResourceAllocationInfo | the engine's GetResourceAllocationInfo for the description; no additional data | engine answer |
| EnumerateMetaCommands | count 0, S_OK | exact: engine-ddi has no meta commands |
| CheckDriverMatchingIdentifier | UNRECOGNIZED | exact: engine-ddi serializes nothing |
| ImplicitShaderCacheControl | no-op | exact: 1006 D3D12_OPTIONS reports DriverManagedShaderCachePresent FALSE |

Every other engine-ddi slot is implemented (SLOTS.md) or a fail-safe. A void fail-safe whose non-const pointers
are all `_Out_` zeroes them before it reports E_NOTIMPL, and each of these is a placeholder: returns 0 (the
E_NOTIMPL report is the difference from the table above): GetMipPacking, CheckSubresourceInfo,
GetRaytracingAccelerationStructurePrebuildInfo, GetMetaCommandRequiredParameterInfo. The first lab log of
D3D12CreateDevice on the native path (the engine-ddi log line "fail-safe slot D+0x... called") settles the list.

### Heap memory: allocate_memory and free_memory

pfnCreateHeapAndResource with a heap description (committed, or a heap alone) makes one `allocate_memory` call;
a placed resource (resource description only, `ReuseBufferGPUVA` naming a resource of the heap) makes none.
engine-ddi then calls the engine's `CreateHeapFromMemory` (ABI 1.2 V10) over the memory, and places a committed
resource at offset 0. MapHeap and UnmapHeap go to the engine's MapHeap and UnmapHeap: the CPU address of heap
offset 0, a placed buffer at that address plus its offset.

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

Development PC witness (harness round trip 5, `tests/test-runtime-backed.cpp`): a stub shell allocates the memory
on the engine's VkDevice as above, with stand-in allocation handles. A committed UPLOAD buffer of 128 KiB holds a
placed buffer at 64 KiB (GPU VA the heap's plus 64 KiB); the placed buffer, filled through MapHeap, is copied into
a committed DEFAULT and a committed READBACK buffer, and read back through MapHeap word for word. Three
allocations, three `free_memory` calls after their engine heaps, VVL with synchronization validation clean.
Whether hosted RADV's import of a runtime allocation behaves the same (same storage, `gpu_va`) is for the lab.

### Shaders and pipelines

The create-shader slots rebuild each program's container with shader-container `BuildContainer` (engine-ddi.h,
"Shaders"); nothing of the shell is involved. The shell's build links `shader-container.cpp` and
`dxil-metadata.cpp` through `engine-ddi.lib`, and must not compile them a second time.

Stream output. `StreamOutputSemantic` gives a gap in the declaration (RegisterIndex ~0u) as an entry with a NULL
SemanticName. The pinned engine DLL (4FFA7493, fork 7bfcd7f0) crashes on one; E6B8168E (branch
amdgpu-wddm/so-hole-fix, c5d9d85f) takes it. Until the pin moves to an engine that takes gaps
(`kEngineTakesStreamOutputGaps` in pipelines.cpp), CreatePipelineState refuses a declaration with a gap with
E_NOTIMPL. How the runtime encodes a gap in the DDI is not measured.

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

### Offline witness

The offline harness exercises the device path: copy, compute dispatch, a draw, the retirement sentinel and the
query slots above (EnginePrivateTest), and the RuntimeBacked heaps above, on the development PC with the pinned
engine DLL, also under VVL with synchronization validation. The dispatch and the draw use shaders created through
the DDI slots from containers reduced to the DDI form (a dxc cs_6_0 DXIL program; fxc vs_5_0 and ps_5_0 DXBC
programs), an element layout by register and DDI state objects. The reduction is the harness's model of the
runtime, not a measurement; the lab has yet to show the runtime's own payloads.
