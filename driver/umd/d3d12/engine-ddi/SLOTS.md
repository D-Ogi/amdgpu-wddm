# engine-ddi slot ownership (boundary r3)

Who fills each DDI 0092 slot. `engine-ddi` slots are filled by `fill_device_core` / `fill_command_list`; `shell` slots
by the native12 shell. A mixed slot names the owner and the boundary call it makes. Phase: P0 first pixel, P1 FL 11_x
rendering, P2 FL 12_0/12_1 (tiled, conservative raster, ROV), P3 DXR, P4 other (fail-safe until needed). Until a slot
is implemented, its entry is the fail-safe of `engine-ddi.h`; no slot is left NULL once the table is published.

A creation slot's fail-safe answers `E_OUTOFMEMORY`, not `E_NOTIMPL`. The runtime holds every creation function of
a user-mode display driver in its AllowOutOfMemory category. Any other status is a critical driver failure, so the
runtime removes the device and reports `DXGI_ERROR_DRIVER_INTERNAL_ERROR` over the real reason, which BD-075
measured on `pfnOpenHeapAndResource`. The creation slots D86, D88, D90, D102 and D117 answer that way, and the
implemented three of them clamp their own refusals through the same `admitted_create_failure`, with no exception:
`D3D12_ERROR_DRIVER_VERSION_MISMATCH` and `D3D12_ERROR_ADAPTER_NOT_FOUND`, which the API defines for a blob this
driver can no longer read, become `E_OUTOFMEMORY` out of D86 like every other status, because no document says
this slot admits them and a stale blob is the ordinary case after a driver update. The application's answer to
either is the same: create a fresh empty library. A lab trial of the runtime's reaction is what would lift that
clamp (`pipelines.cpp`, above `create_pipeline_library`). D89 `pfnCalcSerializedLibrarySize` is a
size query that returns a `SIZE_T` and keeps the plain fail-safe. Its implementation answers 0 and reports nothing,
because a status out of a query is the same BD-075 signature. D91 and D114 return nothing at all, so each answers
its output and leaves the device alone. D83 `pfnRetrieveShaderComment` has an `HRESULT` and uses it only for a
caller's mistake.

The phase of a slot says which milestone made it necessary, not whether it is filled. These slots at P4 carry an
implementation today: D64, D65, D83, D85, D86, D87, D88, D89, D90, D91, D114, D118, D119, L49, L53, L54, L55, L57,
L65, L66 and L69. L52 is filled as well, and
1006 reports `DepthBoundsTestSupported` from the engine's answer. `ViewInstancingTier` is 2 only when the
engine reports full-domain contract revision 1 and OPTIONS3 tier 2 or 3. Older engines report 0 (INTEGRATION.md).
L56 `pfnWriteBufferImmediate` is filled in both
command list tables, and 1006 reports `WriteBufferImmediateQueueFlags` from the engine's own list-type answer. L68
`pfnBarrier` is filled in both tables as well, at P1, and 1006 reports `EnhancedBarriersSupported` from the
engine's own OPTIONS12 answer.

L68 `pfnBarrier` takes one flat array of tagged barriers, while `ID3D12GraphicsCommandList7::Barrier` takes groups
of one type each. The slot therefore walks the array once to count the barriers of each type and the number of
maximal runs of one type, translates each barrier into the array of its own type, and builds one group per run.
Order is a contract, not a convenience: D3D12 defines barrier order by API-call order, then barrier-group index,
then barrier-array index, and two barriers on the same memory must complete in that order, so the runs keep the
DDI array's order and nothing is coalesced across a change of type. A call with more runs than one engine `Barrier`
takes is split into consecutive calls of at most 64 groups, which keeps that order as well. Two DDI shapes have no
API form: the `LEGACY_*` barrier layouts become the queue-agnostic API layout of the same access, and a
`D3D12DDI_BARRIER_TYPE_RANGED` barrier becomes a global barrier of the same scopes, which is the conservative
reading of a range. Neither is refused, because reporting `EnhancedBarriersSupported` makes the runtime route a
legacy `ResourceBarrier` call through this slot too, and a refusal there would cost the device on the first one.
At DDI 0094 the slot takes `D3D12DDIARG_BARRIER_0094`, whose ranged member has a flag (NONE, ATOMIC_COPY,
ACQUIRE, RELEASE) but no scopes, and whose array stride differs. The shell sets the entry of the build the runtime
negotiated (`select_barrier`); a 0094 ranged barrier becomes a global barrier `SYNC_ALL`, `ACCESS_COMMON` before
and after. That over-synchronises a range and does not make atomic copy supported.

D76 `pfnCheckSubresourceInfo` answers the two shapes whose memory layout is known here, a buffer and the linear
primary, from the description the resource was created with and the row pitch of its linear surface. Any other
image gets the zeroed answer and a log line, with no error code at all: the DDI reference allows this void query
none, and an `E_NOTIMPL` from it would be the BD-075 signature again. The engine creates no `ROW_MAJOR` image and
no swizzle of a 64 KiB image is described anywhere here, so that third case is unreachable through the heaps this
driver creates.

D85 to D90 carry an `ID3D12PipelineLibrary` as the engine's own, and two things had to be decided for that. The
DDI names a pipeline inside a library by a `UINT` index, while the engine's interface names one by a string, so
this module writes the index as eight upper-case hexadecimal digits: defined for every index, different for every
index, and the same mapping on both sides of a serialize and a reload. That the index itself survives a reload is
an inference, because the DDI gives the runtime no other key for a pipeline it added. The blob is the engine's own
shape, with its table of contents and its pipeline-cache check, because `pfnCreatePipelineLibrary` hands
`pInitialData` to the engine unchanged and `pfnSerializeLibrary` writes what the engine's `Serialize` writes. What
the DDI needs is only that this driver can read what this driver wrote. `pfnSerializeLibrary` carries no buffer
size of its own, so the library remembers what D89 answered to each calling thread, a store forgets every one of
them, and a serialize with no size of its own thread behind it is refused rather than guessed. The size is held
per thread and not per library because `ID3D12PipelineLibrary` is free-threaded: one value per library would let
one thread's serialize be handed a size another thread's query had made larger. A `LibraryReference` in a create payload turns
`pfnCreatePipelineState` into a load out of that library under the index's name, which is where a library saves an
application its compile. None of this is a reported capability: the 0092 capability structures carry no pipeline
library field, and `DriverManagedShaderCachePresent` stays FALSE, because an application's library is the
application's cache and not a cache of this driver's that D121 could clear.

The render pass table (`D3D12DDI_TABLE_TYPE_0043_RENDER_PASS`, 20) is not in the slot tables below: it is a table
of its own with two slots at 0053, `pfnBeginRenderPass` and `pfnEndRenderPass`. Both are filled, onto the engine's
`ID3D12GraphicsCommandList4`, and `fill_render_pass` publishes them. The shell fills the table only while 1006
reports a render pass tier, which is the engine's own `OPTIONS5` answer. The pinned engine answers tier 0 with the
comment "Currently not supported" although it implements both calls, so nothing is reported today and the runtime
keeps emulating render passes. **Open:** render pass is also an extended feature
(`D3D12DDI_FEATURE_0043_RENDER_PASS`, versions 1 at 0043 and 2 at 0053), and the shell's `extended_features` still
advertises none. Which of the two routes the runtime takes to ask for the table is unmeasured, so the
advertisement belongs with the first engine that answers a tier.

"native via shader-container" marks the shader slots. The payload shape is known from the WDK header: a bare
program with its length in DWORD 1, plus register-only signature entries. That the buffer holds exactly that many
DWORDs is an inference from the SAL annotation `_In_reads_(pShaderCode[1])`, not a measurement. Each create
rebuilds the container the engine compiles with [shader-container](shader-container/README.md), within that
length; pipelines name input elements and stream-output entries from the rebuilt signatures, a gap in a
stream-output declaration as a NULL SemanticName, which the pinned r4 engine takes. A mesh and an amplification
program take the same route, with their signature entries named from the program's own DXIL metadata, because the
engine links a mesh pipeline's stages by semantic name. `pfnCreatePipelineState` then writes a subobject stream
for the engine's `ID3D12Device2::CreatePipelineState`. `pfnDispatchMesh` (L67) forwards to the engine's
`ID3D12GraphicsCommandList6`. None of it is reachable unless 1006 reports a mesh shader tier, which it does only
when the engine's own `D3D12_FEATURE_D3D12_OPTIONS7` answer does.

`pfnCreateHeapAndResource` (D60):
- A heap, or a committed resource (dedicated allocation, heap from it, resource placed at 0), makes exactly one
  `allocate_memory` call.
- A placed resource uses its heap's memory and never allocates.
- The runtime owner in the request is the call's `D3D12DDI_HRTRESOURCE` argument.

A reserved resource (D60 with neither a heap description nor a base resource) makes no `allocate_memory` call; its
tiles are bound to heaps through Q3 and Q4, whose slots are the shell's (the queue table) and whose engine parts are
`update_tile_mappings` and `copy_tile_mappings` (INTEGRATION.md, "Tiled resources").

`pfnDestroyHeapAndResource` (D61) hands heap memory to the release sequence of `engine-ddi.h`: `free_memory`
runs only after GPU retirement, on a DDI thread.

## Device core `D3D12DDI_DEVICE_FUNCS_CORE_0088` (122 slots)

| Slot | Offset | Member | Owner | Phase |
|---|---|---|---|---|
| D0 | 0x000 | `pfnCheckFormatSupport` | engine-ddi | P0 |
| D1 | 0x008 | `pfnCheckMultisampleQualityLevels` | engine-ddi | P1 |
| D2 | 0x010 | `pfnGetMipPacking` | engine-ddi | P2 |
| D3 | 0x018 | `pfnCalcPrivateElementLayoutSize` | engine-ddi | P1 |
| D4 | 0x020 | `pfnCreateElementLayout` | engine-ddi | P1 |
| D5 | 0x028 | `pfnDestroyElementLayout` | engine-ddi | P1 |
| D6 | 0x030 | `pfnCalcPrivateBlendStateSize` | engine-ddi | P1 |
| D7 | 0x038 | `pfnCreateBlendState` | engine-ddi | P1 |
| D8 | 0x040 | `pfnDestroyBlendState` | engine-ddi | P1 |
| D9 | 0x048 | `pfnCalcPrivateDepthStencilStateSize` | engine-ddi | P1 |
| D10 | 0x050 | `pfnCreateDepthStencilState` | engine-ddi | P1 |
| D11 | 0x058 | `pfnDestroyDepthStencilState` | engine-ddi | P1 |
| D12 | 0x060 | `pfnCalcPrivateRasterizerStateSize` | engine-ddi | P1 |
| D13 | 0x068 | `pfnCreateRasterizerState` | engine-ddi | P1 |
| D14 | 0x070 | `pfnDestroyRasterizerState` | engine-ddi | P1 |
| D15 | 0x078 | `pfnCalcPrivateShaderSize` | engine-ddi; native via shader-container | P1 |
| D16 | 0x080 | `pfnCreateVertexShader` | engine-ddi; native via shader-container | P1 |
| D17 | 0x088 | `pfnCreatePixelShader` | engine-ddi; native via shader-container | P1 |
| D18 | 0x090 | `pfnCreateGeometryShader` | engine-ddi; native via shader-container | P1 |
| D19 | 0x098 | `pfnCreateComputeShader` | engine-ddi; native via shader-container | P1 |
| D20 | 0x0A0 | `pfnCalcPrivateGeometryShaderWithStreamOutput` | engine-ddi; native via shader-container | P1 |
| D21 | 0x0A8 | `pfnCreateGeometryShaderWithStreamOutput` | engine-ddi; native via shader-container | P1 |
| D22 | 0x0B0 | `pfnCalcPrivateTessellationShaderSize` | engine-ddi; native via shader-container | P1 |
| D23 | 0x0B8 | `pfnCreateHullShader` | engine-ddi; native via shader-container | P1 |
| D24 | 0x0C0 | `pfnCreateDomainShader` | engine-ddi; native via shader-container | P1 |
| D25 | 0x0C8 | `pfnDestroyShader` | engine-ddi | P1 |
| D26 | 0x0D0 | `pfnCalcPrivateCommandQueueSize` | shell; calls create_engine_queue | P0 |
| D27 | 0x0D8 | `pfnCreateCommandQueue` | shell; calls create_engine_queue | P0 |
| D28 | 0x0E0 | `pfnDestroyCommandQueue` | shell; calls destroy_engine_queue | P0 |
| D29 | 0x0E8 | `pfnCalcPrivateCommandPoolSize` | engine-ddi | P0 |
| D30 | 0x0F0 | `pfnCreateCommandPool` | engine-ddi | P0 |
| D31 | 0x0F8 | `pfnDestroyCommandPool` | engine-ddi | P0 |
| D32 | 0x100 | `pfnResetCommandPool` | engine-ddi | P0 |
| D33 | 0x108 | `pfnCalcPrivatePipelineStateSize` | engine-ddi | P1 |
| D34 | 0x110 | `pfnCreatePipelineState` | engine-ddi (compute, graphics, or a mesh pipeline when the mesh handle is set) | P1 |
| D35 | 0x118 | `pfnDestroyPipelineState` | engine-ddi | P1 |
| D36 | 0x120 | `pfnCalcPrivateCommandListSize` | engine-ddi | P0 |
| D37 | 0x128 | `pfnCreateCommandList` | engine-ddi; calls bind_list_table | P0 |
| D38 | 0x130 | `pfnDestroyCommandList` | engine-ddi | P0 |
| D39 | 0x138 | `pfnCalcPrivateFenceSize` | shell | P0 |
| D40 | 0x140 | `pfnCreateFence` | shell | P0 |
| D41 | 0x148 | `pfnDestroyFence` | shell | P0 |
| D42 | 0x150 | `pfnCalcPrivateDescriptorHeapSize` | engine-ddi | P0 |
| D43 | 0x158 | `pfnCreateDescriptorHeap` | engine-ddi | P0 |
| D44 | 0x160 | `pfnDestroyDescriptorHeap` | engine-ddi | P0 |
| D45 | 0x168 | `pfnGetDescriptorSizeInBytes` | engine-ddi | P0 |
| D46 | 0x170 | `pfnGetCPUDescriptorHandleForHeapStart` | engine-ddi | P0 |
| D47 | 0x178 | `pfnGetGPUDescriptorHandleForHeapStart` | engine-ddi | P1 |
| D48 | 0x180 | `pfnCreateShaderResourceView` | engine-ddi | P1 |
| D49 | 0x188 | `pfnCreateConstantBufferView` | engine-ddi | P1 |
| D50 | 0x190 | `pfnCreateSampler` | engine-ddi | P1 |
| D51 | 0x198 | `pfnCreateUnorderedAccessView` | engine-ddi | P1 |
| D52 | 0x1A0 | `pfnCreateRenderTargetView` | engine-ddi | P0 |
| D53 | 0x1A8 | `pfnCreateDepthStencilView` | engine-ddi | P1 |
| D54 | 0x1B0 | `pfnCalcPrivateRootSignatureSize` | engine-ddi | P1 |
| D55 | 0x1B8 | `pfnCreateRootSignature` | engine-ddi | P1 |
| D56 | 0x1C0 | `pfnDestroyRootSignature` | engine-ddi | P1 |
| D57 | 0x1C8 | `pfnMapHeap` | engine-ddi | P0 |
| D58 | 0x1D0 | `pfnUnmapHeap` | engine-ddi | P0 |
| D59 | 0x1D8 | `pfnCalcPrivateHeapAndResourceSizes` | engine-ddi | P0 |
| D60 | 0x1E0 | `pfnCreateHeapAndResource` | engine-ddi; heap or committed: one allocate_memory; placed: none | P0 |
| D61 | 0x1E8 | `pfnDestroyHeapAndResource` | engine-ddi; free_memory after retirement | P0 |
| D62 | 0x1F0 | `pfnMakeResident` | shell | P1 |
| D63 | 0x1F8 | `pfnEvict` | shell | P1 |
| D64 | 0x200 | `pfnCalcPrivateOpenedHeapAndResourceSizes` | engine-ddi; the records a create builds | P4 |
| D65 | 0x208 | `pfnOpenHeapAndResource` | engine-ddi; opens a shared linear surface, refuses every other shape with E_OUTOFMEMORY | P4 |
| D66 | 0x210 | `pfnCopyDescriptors` | engine-ddi | P1 |
| D67 | 0x218 | `pfnCopyDescriptorsSimple` | engine-ddi | P1 |
| D68 | 0x220 | `pfnCalcPrivateQueryHeapSize` | engine-ddi | P1 |
| D69 | 0x228 | `pfnCreateQueryHeap` | engine-ddi | P1 |
| D70 | 0x230 | `pfnDestroyQueryHeap` | engine-ddi | P1 |
| D71 | 0x238 | `pfnCalcPrivateCommandSignatureSize` | engine-ddi | P1 |
| D72 | 0x240 | `pfnCreateCommandSignature` | engine-ddi | P1 |
| D73 | 0x248 | `pfnDestroyCommandSignature` | engine-ddi | P1 |
| D74 | 0x250 | `pfnCheckResourceVirtualAddress` | engine-ddi | P0 |
| D75 | 0x258 | `pfnCheckResourceAllocationInfo` | engine-ddi | P0 |
| D76 | 0x260 | `pfnCheckSubresourceInfo` | engine-ddi | P1 |
| D77 | 0x268 | `pfnCheckExistingResourceAllocationInfo` | engine-ddi | P1 |
| D78 | 0x270 | `pfnOfferResources` | shell | P4 |
| D79 | 0x278 | `pfnReclaimResources` | shell | P4 |
| D80 | 0x280 | `pfnGetImplicitPhysicalAdapterMask` | shell | P0 |
| D81 | 0x288 | `pfnGetPresentPrivateDriverDataSize` | shell | P0 |
| D82 | 0x290 | `pfnQueryNodeMap` | shell | P0 |
| D83 | 0x298 | `pfnRetrieveShaderComment` | engine-ddi, the empty comment: one terminator, no status out of a query | P4 |
| D84 | 0x2A0 | `pfnCheckResourceAllocationHandle` | engine-ddi; from the RuntimeBacked record | P0 |
| D85 | 0x2A8 | `pfnCalcPrivatePipelineLibrarySize` | engine-ddi, the library record | P4 |
| D86 | 0x2B0 | `pfnCreatePipelineLibrary` | engine-ddi, the engine's `ID3D12PipelineLibrary1` over `pInitialData` | P4 |
| D87 | 0x2B8 | `pfnDestroyPipelineLibrary` | engine-ddi | P4 |
| D88 | 0x2C0 | `pfnAddPipelineStateToLibrary` | engine-ddi, `StorePipeline` under the index's name | P4 |
| D89 | 0x2C8 | `pfnCalcSerializedLibrarySize` | engine-ddi, the engine's size, remembered per thread as the blob's capacity | P4 |
| D90 | 0x2D0 | `pfnSerializeLibrary` | engine-ddi, `Serialize` into this thread's remembered capacity | P4 |
| D91 | 0x2D8 | `pfnGetDebugAllocationInfo` | shell, zero records of each kind, the device untouched | P4 |
| D92 | 0x2E0 | `pfnCalcPrivateCommandRecorderSize` | engine-ddi | P0 |
| D93 | 0x2E8 | `pfnCreateCommandRecorder` | engine-ddi | P0 |
| D94 | 0x2F0 | `pfnDestroyCommandRecorder` | engine-ddi | P0 |
| D95 | 0x2F8 | `pfnCommandRecorderSetCommandPoolAsTarget` | engine-ddi | P0 |
| D96 | 0x300 | `pfnCalcPrivateSchedulingGroupSize` | shell | P4 |
| D97 | 0x308 | `pfnCreateSchedulingGroup` | shell | P4 |
| D98 | 0x310 | `pfnDestroySchedulingGroup` | shell | P4 |
| D99 | 0x318 | `pfnEnumerateMetaCommands` | engine-ddi | P4 |
| D100 | 0x320 | `pfnEnumerateMetaCommandParameters` | engine-ddi | P4 |
| D101 | 0x328 | `pfnCalcPrivateMetaCommandSize` | engine-ddi | P4 |
| D102 | 0x330 | `pfnCreateMetaCommand` | engine-ddi | P4 |
| D103 | 0x338 | `pfnDestroyMetaCommand` | engine-ddi | P4 |
| D104 | 0x340 | `pfnGetMetaCommandRequiredParameterInfo` | engine-ddi | P4 |
| D105 | 0x348 | `pfnCalcPrivateStateObjectSize` | engine-ddi | P3 |
| D106 | 0x350 | `pfnCreateStateObject` | engine-ddi | P3 |
| D107 | 0x358 | `pfnDestroyStateObject` | engine-ddi | P3 |
| D108 | 0x360 | `pfnGetRaytracingAccelerationStructurePrebuildInfo` | engine-ddi | P3 |
| D109 | 0x368 | `pfnCheckDriverMatchingIdentifier` | engine-ddi | P3 |
| D110 | 0x370 | `pfnGetShaderIdentifier` | engine-ddi | P3 |
| D111 | 0x378 | `pfnGetShaderStackSize` | engine-ddi | P3 |
| D112 | 0x380 | `pfnGetPipelineStackSize` | engine-ddi | P3 |
| D113 | 0x388 | `pfnSetPipelineStackSize` | engine-ddi | P3 |
| D114 | 0x390 | `pfnSetBackgroundProcessingMode` | shell, accepted and dropped, no further measurements wanted | P4 |
| D115 | 0x398 | `pfnCalcPrivateAddToStateObjectSize` | engine-ddi | P3 |
| D116 | 0x3A0 | `pfnAddToStateObject` | engine-ddi; E_NOTIMPL without the engine's ID3D12Device7 | P3 |
| D117 | 0x3A8 | `pfnCreateSamplerFeedbackUnorderedAccessView` | engine-ddi | P4 |
| D118 | 0x3B0 | `pfnCreateAmplificationShader` | engine-ddi; native via shader-container (DXIL kind 14) | P4 |
| D119 | 0x3B8 | `pfnCreateMeshShader` | engine-ddi; native via shader-container (DXIL kind 13) | P4 |
| D120 | 0x3C0 | `pfnCalcPrivateMeshShaderSize` | engine-ddi | P4 |
| D121 | 0x3C8 | `pfnImplicitShaderCacheControl` | engine-ddi | P4 |

## Command list `D3D12DDI_COMMAND_LIST_FUNCS_3D_0092` (70 slots)

| Slot | Offset | Member | Owner | Phase |
|---|---|---|---|---|
| L0 | 0x000 | `pfnCloseCommandList` | engine-ddi | P0 |
| L1 | 0x008 | `pfnResetCommandList` | engine-ddi | P0 |
| L2 | 0x010 | `pfnDrawInstanced` | engine-ddi | P1 |
| L3 | 0x018 | `pfnDrawIndexedInstanced` | engine-ddi | P1 |
| L4 | 0x020 | `pfnDispatch` | engine-ddi | P1 |
| L5 | 0x028 | `pfnClearUnorderedAccessViewUint` | engine-ddi | P1 |
| L6 | 0x030 | `pfnClearUnorderedAccessViewFloat` | engine-ddi | P1 |
| L7 | 0x038 | `pfnClearRenderTargetView` | engine-ddi | P0 |
| L8 | 0x040 | `pfnClearDepthStencilView` | engine-ddi | P1 |
| L9 | 0x048 | `pfnDiscardResource` | engine-ddi | P1 |
| L10 | 0x050 | `pfnCopyTextureRegion` | engine-ddi | P0 |
| L11 | 0x058 | `pfnResourceCopy` | engine-ddi | P1 |
| L12 | 0x060 | `pfnCopyTiles` | engine-ddi | P2 |
| L13 | 0x068 | `pfnCopyBufferRegion` | engine-ddi | P0 |
| L14 | 0x070 | `pfnResourceResolveSubresource` | engine-ddi | P1 |
| L15 | 0x078 | `pfnExecuteBundle` | engine-ddi | P1 |
| L16 | 0x080 | `pfnExecuteIndirect` | engine-ddi | P1 |
| L17 | 0x088 | `pfnResourceBarrier` | engine-ddi | P0 |
| L18 | 0x090 | `pfnBlt` | engine-ddi | P1 |
| L19 | 0x098 | `pfnPresent` | shell overrides; calls resource_allocation | P0 |
| L20 | 0x0A0 | `pfnBeginQuery` | engine-ddi | P1 |
| L21 | 0x0A8 | `pfnEndQuery` | engine-ddi | P1 |
| L22 | 0x0B0 | `pfnResolveQueryData` | engine-ddi | P1 |
| L23 | 0x0B8 | `pfnSetPredication` | engine-ddi | P1 |
| L24 | 0x0C0 | `pfnIaSetTopology` | engine-ddi | P1 |
| L25 | 0x0C8 | `pfnRsSetViewports` | engine-ddi | P1 |
| L26 | 0x0D0 | `pfnRsSetScissorRects` | engine-ddi | P1 |
| L27 | 0x0D8 | `pfnOmSetBlendFactor` | engine-ddi | P1 |
| L28 | 0x0E0 | `pfnOmSetStencilRef` | engine-ddi | P1 |
| L29 | 0x0E8 | `pfnSetPipelineState` | engine-ddi | P1 |
| L30 | 0x0F0 | `pfnSetDescriptorHeaps` | engine-ddi | P1 |
| L31 | 0x0F8 | `pfnSetComputeRootSignature` | engine-ddi | P1 |
| L32 | 0x100 | `pfnSetGraphicsRootSignature` | engine-ddi | P1 |
| L33 | 0x108 | `pfnSetComputeRootDescriptorTable` | engine-ddi | P1 |
| L34 | 0x110 | `pfnSetGraphicsRootDescriptorTable` | engine-ddi | P1 |
| L35 | 0x118 | `pfnSetComputeRoot32BitConstant` | engine-ddi | P1 |
| L36 | 0x120 | `pfnSetGraphicsRoot32BitConstant` | engine-ddi | P1 |
| L37 | 0x128 | `pfnSetComputeRoot32BitConstants` | engine-ddi | P1 |
| L38 | 0x130 | `pfnSetGraphicsRoot32BitConstants` | engine-ddi | P1 |
| L39 | 0x138 | `pfnSetComputeRootConstantBufferView` | engine-ddi | P1 |
| L40 | 0x140 | `pfnSetGraphicsRootConstantBufferView` | engine-ddi | P1 |
| L41 | 0x148 | `pfnSetComputeRootShaderResourceView` | engine-ddi | P1 |
| L42 | 0x150 | `pfnSetGraphicsRootShaderResourceView` | engine-ddi | P1 |
| L43 | 0x158 | `pfnSetComputeRootUnorderedAccessView` | engine-ddi | P1 |
| L44 | 0x160 | `pfnSetGraphicsRootUnorderedAccessView` | engine-ddi | P1 |
| L45 | 0x168 | `pfnIASetIndexBuffer` | engine-ddi | P1 |
| L46 | 0x170 | `pfnIASetVertexBuffers` | engine-ddi | P1 |
| L47 | 0x178 | `pfnSOSetTargets` | engine-ddi | P1 |
| L48 | 0x180 | `pfnOMSetRenderTargets` | engine-ddi | P1 |
| L49 | 0x188 | `pfnSetMarker` | engine-ddi | P4 |
| L50 | 0x190 | `pfnClearRootArguments` | engine-ddi | P1 |
| L51 | 0x198 | `pfnAtomicCopyBufferRegion` | engine-ddi | P4 |
| L52 | 0x1A0 | `pfnOMSetDepthBounds` | engine-ddi | P2 |
| L53 | 0x1A8 | `pfnSetSamplePositions` | engine-ddi | P4 |
| L54 | 0x1B0 | `pfnResourceResolveSubresourceRegion` | engine-ddi | P4 |
| L55 | 0x1B8 | `pfnSetProtectedResourceSession` | engine-ddi | P4 |
| L56 | 0x1C0 | `pfnWriteBufferImmediate` | engine-ddi | P1 |
| L57 | 0x1C8 | `pfnSetViewInstanceMask` | engine-ddi | P4 |
| L58 | 0x1D0 | `pfnInitializeMetaCommand` | engine-ddi | P4 |
| L59 | 0x1D8 | `pfnExecuteMetaCommand` | engine-ddi | P4 |
| L60 | 0x1E0 | `pfnBuildRaytracingAccelerationStructure` | engine-ddi | P3 |
| L61 | 0x1E8 | `pfnEmitRaytracingAccelerationStructurePostbuildInfo` | engine-ddi | P3 |
| L62 | 0x1F0 | `pfnCopyRaytracingAccelerationStructure` | engine-ddi | P3 |
| L63 | 0x1F8 | `pfnSetPipelineState1` | engine-ddi | P3 |
| L64 | 0x200 | `pfnDispatchRays` | engine-ddi | P3 |
| L65 | 0x208 | `pfnRSSetShadingRate` | engine-ddi | P4 |
| L66 | 0x210 | `pfnRSSetShadingRateImage` | engine-ddi | P4 |
| L67 | 0x218 | `pfnDispatchMesh` | engine-ddi (graphics table only) | P4 |
| L68 | 0x220 | `pfnBarrier` | engine-ddi | P1 |
| L69 | 0x228 | `pfnOmSetAlphaBlendFactor` | engine-ddi | P4 |

## Command queue `D3D12DDI_COMMAND_QUEUE_FUNCS_CORE_0001` (7 slots)

| Slot | Offset | Member | Owner | Phase |
|---|---|---|---|---|
| Q0 | 0x000 | `pfnExecuteCommandLists` | shell; calls execute_command_lists | P0 |
| Q1 | 0x008 | `pfnUnused` | shell | P4 |
| Q2 | 0x010 | `pfnUnused2` | shell | P4 |
| Q3 | 0x018 | `pfnUpdateTileMappings` | shell; calls update_tile_mappings | P2 |
| Q4 | 0x020 | `pfnCopyTileMappings` | shell; calls copy_tile_mappings | P2 |
| Q5 | 0x028 | `pfnSignalFence` | shell | P0 |
| Q6 | 0x030 | `pfnWaitForFence` | shell | P0 |

## Extended features `D3D12DDI_EXTENDED_FEATURES_FUNCS_0021` (4 slots)

| Slot | Offset | Member | Owner | Phase |
|---|---|---|---|---|
| X0 | 0x000 | `pfnGetSupportedExtendedFeatures` | shell | P0 |
| X1 | 0x008 | `pfnGetSupportedExtendedFeatureVersions` | shell | P0 |
| X2 | 0x010 | `pfnEnableExtendedFeature` | shell | P0 |
| X3 | 0x018 | `pfnSetExtendedFeatureCallbacks` | shell | P0 |

