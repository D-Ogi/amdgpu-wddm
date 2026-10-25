# D3D12 DDI version negotiation through 0119

The shell offers each supported DDI version in descending order. Each version selects typed device and command-list tables.
Negotiation does not enable shader or hardware features. The engine caps query and the implemented translation paths determine those features.

The contract source is WDK 10.0.28000.2526, `um/d3d12umddi.h` from include version 10.0.28000.0.
`H` references below give line numbers in that header.
The [Microsoft DDI reference](https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d12umddi/) describes these interfaces.
The header governs their exact C declarations.

## Build and table selection

The default WDK 26100 build still offers versions 0092 through 0094.
The explicit `-DdiInclude` argument selects the WDK 28000 UM headers for the shell and engine-ddi builds.
Those headers enable the layouts through 0119 and the build 0120, which uses the 0119 layouts.
No public header defines 0120. A static analysis of D3D12Core 1.619.6 and 1.721.3 found no table, slot or caps payload change at 0120.
At 0120 the runtime stops its 6_8 clamp of the shader model answer (caps type 1012).
Both flavours offer the same versions: 0120 down to 0092 with the WDK 28000 header, 0094 down to 0092 without it.
The build records the selected header path and SHA256.
The ladder build also requires `-ApiInclude` with Agility 1.618 or newer API headers for tight alignment.
Agility 1.619.6 is the tested header input. The build records its path and SHA256 separately from the DDI header.
No automatic header upgrade changes the supported versions.

The engine test build accepts an explicit `-EngineAbiPin` file for candidate validation.
The default remains the tracked engine pin. Both paths enforce the header and flavour-specific DLL hashes.
The build records the pin path and SHA256 in its selection report and each run summary.
It rejects a pin that changes during the build. A candidate pin does not promote an engine patch.
Promotion still requires the engine source revision, patch provenance and matching package artifacts.

In this table, Core means `D3D12DDI_DEVICE_FUNCS_CORE_*` and List means `D3D12DDI_COMMAND_LIST_FUNCS_3D_*`.
Each suffix selects the complete C structure, including all unchanged members.

| Negotiated version | Core suffix | List suffix | Core declaration | List declaration |
| --- | --- | --- | --- | --- |
| 0092-0093 | 0088 | 0092 | Existing layout | Existing layout |
| 0094 | 0088 | 0094 | Existing layout | H11457 |
| 0095 | 0095 | 0095 | H11572 | H11740 |
| 0096-0098 | 0096 | 0095 | H11865 | H11740 |
| 0099 | 0099 | 0099 | H12138 | H12312 |
| 0100-0101 | 0100 | 0099 | H12446 | H12312 |
| 0102-0107 | 0102 | 0099 | H12670 | H12312 |
| 0108 | 0108 | 0108 | H13288 | H13458 |
| 0109-0110 | 0109 | 0108 | H13606 | H13458 |
| 0111 | 0111 | 0108 | H13923 | H13458 |
| 0112 | 0112 | 0108 | H14125 | H13458 |
| 0113 | 0113 | 0108 | H14375 | H13458 |
| 0114-0115 | 0113 | 0114 | H14375 | H14635 |
| 0116-0118 | 0116 | 0114 | H14752 | H14635 |
| 0119 | 0119 | 0114 | H15055 | H14635 |
| 0120 | 0119 | 0114 | H15055 | H14635 |

[`ddi-ladder.h`](../../driver/umd/d3d12/ddi-ladder.h) contains the typed member lists and the version selection.
The compiler checks each member assignment. Size assertions detect table growth.
Each list layout and table number has separate immutable callback storage.
A later attempt to publish different callbacks fails without changing the published table.
The code never treats a smaller table as a larger table.

## Device callbacks

[`ddi-modern-core.h`](../../driver/umd/d3d12/engine-ddi/ddi-modern-core.h) selects typed engine-ddi entries for modern descriptions.
The production entries preserve fields that the old layouts cannot represent.
Conservative conversion remains available when an adapter has no modern entry. It is not the production feature path.
Unchanged callbacks retain their existing implementation.

| First layout | Changed callback group | Translation or refusal |
| --- | --- | --- |
| 0095 | Depth/stencil size and creation | Retain independent front/back masks in DEPTH_STENCIL_DESC2. Pipeline streams use DEPTH_STENCIL2. |
| 0096 | Sampler creation | Call engine CreateSampler2 with the border-color union and sampler flags intact, including non-normalized coordinates. |
| 0099 | Rasterizer size and creation | Retain FLOAT depth bias in RASTERIZER_DESC2 without an INT conversion. |
| 0099 | Pipeline size and creation | Copy named fields and preserve dynamic depth-bias and strip-cut flags. |
| 0100 | Root-signature size and creation | Serialize version 1.2 directly, including all static-sampler fields, border-color enums and flags. |
| 0102 | Rasterizer size and creation | Preserve the line-rasterization mode, including narrow quadrilaterals, through RASTERIZER2. |
| 0108 | Program identifier and graph memory requirements | Return registry identifiers for generic programs. Unknown program names return zero. Work-graph memory requirements remain zero. |
| 0109 | Heap/resource size, creation and allocation information | Preserve existing fields and castable formats. Refuse nonzero CreateAtVirtualAddress. |
| 0111 | Heap/resource callbacks and texture layouts | Translate tight alignment through resource sizing and creation when the engine supports tier 1. Refuse GUID layouts and return zero available GUID layouts. |
| 0112 | Fence size and creation | Convert monitored fences. Refuse native fences. |
| 0113 | Application-specific state, blob status and blob size | Return E_NOTIMPL, NOT_SPECIFIED and zero respectively. |
| 0116 | Fence size and creation | Convert monitored fences. Refuse native and opened-native fences. |
| 0119 | Pipeline size and creation | Convert named pipeline fields. Existing shader objects and asynchronous plans own their bytecode. |

Creation callbacks log the internal refusal reason.
The AllowOutOfMemory contract maps unsupported creation requests to E_OUTOFMEMORY through the existing admission policy.
Void creation callbacks report an error through the device callback.
Size callbacks report the error and return zero.
An unsupported resource allocation query returns `ResourceDataSize = UINT64_MAX` with placement alignment.
It does not call SetErrorCb for a valid but unsupported resource description.

## Command-list callbacks

[`ddi-modern-list.h`](../../driver/umd/d3d12/engine-ddi/ddi-modern-list.h) accepts typed engine entries for the five new commands below.
A missing entry records a list error. It never becomes a successful no-op.
The shell keeps that error for the runtime to observe when the list closes.

| First layout | Added command | Current behavior |
| --- | --- | --- |
| 0095 | OmSetFrontAndBackStencilRef | Record engine ID3D12GraphicsCommandList8::OMSetFrontAndBackStencilRef. |
| 0099 | RSSetDepthBias | Record engine ID3D12GraphicsCommandList9::RSSetDepthBias with FLOAT arguments. |
| 0099 | IASetIndexBufferStripCutValue | Record engine ID3D12GraphicsCommandList9::IASetIndexBufferStripCutValue. |
| 0108 | SetProgram | Resolve a same-device generic-program identifier and record its engine PSO binding. Other program types report E_NOTIMPL. |
| 0108 | DispatchGraph | No engine-ddi entry is installed. Record E_NOTIMPL. |

The graphics commands retain the queried engine interface until replay executes them.
An unavailable engine interface reports a list error.
Generic identifiers are registry keys. The code does not dereference an incoming identifier as a pointer.
Unknown, stale and cross-device identifiers report E_INVALIDARG.

Layout 0114 changes 39 signatures to API handles without adding slots.
Typed wrappers use the WDK helpers to obtain driver handles.
The wrappers preserve null pipeline, root-signature and state-object handles.
They reject a null command-list handle.

The changed groups cover draw and dispatch, topology, viewport and scissor state, blend factors, and stencil references.
They also cover pipeline state, compute and graphics root signatures, descriptor tables, constants, and root buffer views.
The remaining groups cover index and vertex buffers, stream output, render targets, depth bounds, sample positions, and view masks.
Ray dispatch, shading rate, mesh dispatch, and the five commands above complete the changed groups.
All unchanged arguments retain their original types.

Modern layouts retain the typed wrappers instead of enabling the direct-entry optimization.
A future optimization requires measurements and equivalent behavior on the BC-250.
The new layouts alone do not establish a performance gain.

## Caps payloads

The existing OPTIONS_0089 payload at caps type 1006 does not grow.
New options use separate caps types. Each reply checks the exact payload size before writing.
New replies reject a DDI version below their introduction.
The existing SM6.8 options reply retains its cross-version behavior.

| Caps type | Payload suffix and declaration | Answer |
| --- | --- | --- |
| 1080 | OPTIONS_DATA_0098, H12059 | L1MemoryFullyCpuAccessible FALSE. The shell has no complete GPU-upload heap contract. |
| 1081 | OPTIONS_DATA_0101, H12619 | ComputeOnlyCustomHeapSupported FALSE. This is not an FL1_0 compute-only adapter. |
| 1082 | OPTIONS_DATA_0102, H12632 | Check the mandatory minima against engine API OPTIONS19, then copy its sample mask and descriptor limits. Failure leaves the payload unchanged. |
| 1013 | OPTIONS_DATA_0103, H12840 | WorkGraphsTier NOT_SUPPORTED. Engine OPTIONS21 cannot enable an absent state-object and dispatch path. |
| 1075 | WAVE_MMA_DATA_0103, H12868 | Unsupported. Preserve input data type and dimensions. Clear output support, sizes and accumulator flags. |
| 1084 | FEATURE_DATA_PREDICATION_106, H12900 | Optional FL1_0 feature FALSE. Existing FL12 predication remains separate. |
| 1085 | FEATURE_DATA_PLACED_RESOURCE_SUPPORT_INFO_106, H12905 | Optional FL1_0 feature FALSE. Preserve placement inputs. |
| 1086 | FEATURE_DATA_HARDWARE_COPY_106, H12916 | Optional FL1_0 feature FALSE. |
| 1087 | OPTIONS_DATA_0109, H13563 | RecreateAtTier NOT_SUPPORTED. |
| 1088 | OPTIONS_DATA_0110, H13823 | ExecuteIndirectTier follows engine OPTIONS21 for tier 1.1. Otherwise report tier 1.0. |
| 1089 | TIGHT_ALIGNMENT_TIER_DATA_0111, H13854 | Engine tight alignment tier 1. Unsupported or unknown engine tiers map to NOT_SUPPORTED. |
| 1090 | GUID_TEXTURE_LAYOUT_TIER_DATA_0111, H13893 | NOT_SUPPORTED. |
| 1091 | SHADER_MODEL_6_8_OPTIONS_0110, H13830 | Existing OPTIONS21 mapping for gradient/bias sampling and extended command information. Require reported SM6.8. |
| 1092 | GUID_TEXTURE_LAYOUT_GUID_SUPPORT_DATA_0111, H13898 | GuidSupported FALSE. Preserve the resource pointer and access mask. |
| 1093 | NATIVE_FENCE_SUPPORT_DATA_0112, H14100 | NativeGpuFenceSupported FALSE. |
| 1094 | FEATURE_DATA_APPLICATION_SPECIFIC_DRIVER_STATE_113, H14351 | Supported FALSE. |
| 1095 | OPTIONS_DATA_0118, H14995 | ShaderExecutionReorderingActuallyReorders FALSE. The current engine caps contract has no SER answer. |

ExecuteIndirect tier 1.1 uses the existing incrementing-constant translation in `commands.cpp`.
Both API and DDI enums use values 10 and 11.
Work graph tiers instead use 0, 1 and 10 (H10690).
Caps type 1095 contains a BOOL, not a SER tier.

OPTIONS_0102 requires sampler heaps of at least 4000 descriptors, or 2048 with static samplers, and at least 1000000 view descriptors.
At feature level 11_1 or later, the no-output sample mask must contain counts 1, 4, 8 and 16 (`0x1d`).
An unanswered engine query or a missing minimum returns E_NOTIMPL. The normal reply never raises an engine limit to satisfy the check.
This refusal prevents an unsupported payload from being reported. It does not establish that the runtime accepts the negotiated DDI version.

The candidate engine derives no-output counts from Vulkan support and checks attachment-free pipeline creation against that mask.
Its larger sampler array uses a descriptor-buffer route with layout and capacity checks.
Acceptance requires actual use of the highest advertised descriptor while static samplers coexist.
Neither a larger cap value nor successful allocation alone proves that behavior.
The tracked engine pin has not been replaced by these candidate changes.

`AMDGPU_WDDM_LAB_DDI0102_LIMITS` exists only in the lab flavour.
Its `control`, `samples`, `samplers` and `both` arms intentionally bypass the minimum check to isolate runtime creation failures.
The log labels their replies as synthetic. They are diagnostic inputs, not feature support or release evidence.
Release builds contain no such override. Normal validation must run with the variable unset.

The SM6.9 release enum appears at H3510 with value `0x00060095`.
The shell still queries and reports at most SM6.8.
Adding an enum does not prove compiler support.
Mesh support still requires the engine mesh tier and the existing shader-model condition.
This change does not raise mesh support, WorkGraphsTier, SER support or the shader-model ceiling.

## State subobjects

The STATE_SUBOBJECT_0054 envelope stays `{Type, pDesc}` (H7780).
The enum at H7740 adds the following payload choices.

| Values | Types and payloads |
| --- | --- |
| 13 | WORK_GRAPH uses WORK_GRAPH_DESC_0108 at H13153. |
| 14 | STREAM_OUTPUT uses STREAM_OUTPUT_DESC_0108 at H13276. |
| 15-18 | BLEND, SAMPLE_MASK, RASTERIZER_0102 and DEPTH_STENCIL_0095. |
| 19-24 | INPUT_LAYOUT, IB_STRIP_CUT_VALUE, PRIMITIVE_TOPOLOGY, RENDER_TARGET_FORMATS, DEPTH_STENCIL_FORMAT and SAMPLE_DESC. |
| 26, 28 | FLAGS and VIEW_INSTANCING. Values 25 and 27 have no declared subobject. |
| 29 | GENERIC_PROGRAM uses GENERIC_PROGRAM_DESC_0108 at H13267. It refers to exports and other subobjects. |
| 30-31 | GLOBAL_SERIALIZED_ROOT_SIGNATURE and LOCAL_SERIALIZED_ROOT_SIGNATURE use the structures at H21418 and H21423. |

Serialized root-signature subobjects contain CREATE_ROOT_SIGNATURE_0100 descriptions.
Their names do not make `pDesc` a pointer to raw serialized bytes.
[`generic-programs.cpp`](../../driver/umd/d3d12/engine-ddi/generic-programs.cpp) translates generic executable objects into named engine PSOs.
Non-library DXIL shaders enter through owned containers and export names.
The translator supports generic shader collections, filtered or renamed imports, and AddToStateObject.
Pure DXR library and hit-group collections continue through the existing raytracing translator.
Unsupported mixtures fail creation rather than entering the wrong translator.

Each generic program selects graphics state subobjects from its own creation description.
An addition inherits shader exports and compiled programs, but must supply its own graphics state definitions.
Both the parent and the addition must opt in to additions.
Shared ownership keeps imported shader bytes, root associations and program PSOs alive after source collection or parent destruction.
SetProgram retains its PSO independently until the recorded bind executes.

Global root handles and global serialized-root descriptions are supported.
Runtime-resolved associations select the root signature. Different engine interface pointers do not imply different root definitions.
Collections may retain unresolved summary flags. Executable summaries must be resolved.
Local root-signature subobjects and work-graph subobjects are not implemented by this generic translator.
WorkGraphsTier remains NOT_SUPPORTED, and DispatchGraph remains a reported error.
Generic-program support does not advertise work-graph support.

The tracked generic signature gate compares both raw-DXIL and whole-container intake against original DXC containers.
It covers packed signatures, 16-bit types, tessellation domains, geometry streams, clip/cull, coverage and depth semantics.
Those comparisons establish reconstruction behavior only. They do not establish every graphics stage on the BC-250.

## Bytecode lifetime at 0119

H15008-H15010 transfers responsibility for retained bytecode from the runtime to the driver.
This applies to PSOs and state objects.
The engine source review below uses revision `f5fda15126faa9cbb8392035b965d4b7d968ae45` as its baseline.
[`engine-abi.json`](../../driver/umd/d3d12/engine-ddi/engine-abi.json) pins that revision.
Engine line numbers refer to that baseline, before the separate repair. Shell entries identify current ownership mechanisms without historical line numbers.

| Retention path | Ownership evidence | Limit of the finding |
| --- | --- | --- |
| Shader creation | `pipelines.cpp` stores BuildContainer output in the shader object. | The shader record owns the container until destruction. |
| Asynchronous PSO and retry | `PsoPlan` owns stage vectors. The asynchronous job takes the plan and retains it for retries. | Job destruction releases the plan. No retained stage pointer refers to caller bytecode. |
| All pipeline stages | `pipelines.cpp` builds compute, mesh and graphics plans from owned shader containers. | Plan builders catch allocation exceptions and return E_OUTOFMEMORY. |
| Draw-time variants | Engine `state.c:5958` initially borrows shader bytes. Finalization at lines 6324-6341 copies stages that still need bytecode for late compilation. | Allocation failure returns E_OUTOFMEMORY. Stages with SPIR-V or shader modules need no extra bytecode copy. |
| Late shader compilation | Engine `state.c:3480-3482` consumes cached bytecode. Lines 3519-3523 free owned copies after module creation. | Variant construction at lines 7063-7075 uses module or SPIR-V state under the pipeline lock. |
| Shader cache | Engine `cache.c:2756-2765` hashes caller bytecode during creation. Lines 914-923 allocate and fill serialized SPIR-V storage. | Cache entries retain hashes, compiled state and serialized data. They do not retain the shader-input pointer for later hashing. |
| Deferred cache writes | Engine `cache.c:2901-2906` retains the pipeline object before queuing work. Lines 2877-2897 serialize synchronously in threadless mode. | This keeps the pipeline's owned compiled data alive. It does not require another application-bytecode copy. |
| DXR state-object creation | `state-objects.cpp` builds owned library containers and invokes engine creation synchronously. | Export arrays remain borrowed during that call. The engine must copy anything it retains beyond the call. |
| Generic collections and additions | `generic-programs.cpp` owns reconstructed containers and export names. Imports and additions share shader, root and program ownership. | Caller bytes may be released after creation. Local graphics-state lookup does not retain a parent caller-array map. |
| Generic program binding | The identifier registry holds weak program references. SetProgram takes a strong lookup and a separate PSO COM reference for replay. | Destroyed identifiers are refused after the final program owner disappears. Normal reset and destruction drain pending replay. |
| Deferred collections | Engine `raytracing_pipeline.c:603-636` copies hit groups, library descriptors, bytecode and association names. Cleanup at lines 545-577 frees those copies. | The pinned implementation does not check each allocation. Its normal ownership path does not establish safe allocation failure. |
| Parsed DXIL subobjects | Engine `libs/vkd3d-shader/dxil.c:2227-2289` copies names, root-signature payloads and association export arrays. | Root-payload and association allocations lack checks in the pinned source. |
| Compiled collection imports | Engine `raytracing_pipeline.c:2785-2792` retains collection references. Lines 2806-2820 transfer exports and entry points into the object. | Child lifetime does not depend on application bytecode. |
| AddToStateObject | Engine `raytracing_pipeline.c:2907` uses the same create path with a parent. Line 1311 imports that parent as a collection. | Normal compiled-parent additions share the retained-collection path. |
| Deferred collection imports | Engine `raytracing_pipeline.c:735` borrows existing engine-owned library descriptors. Lines 848-850 mark imported subobject payloads as borrowed. | The pinned source omits a parent reference. An extracted regression demonstrates freed payloads after parent release. |

The pinned defer function dereferences unchecked allocations at lines 603-604 and 624-627.
Its caller at lines 2842-2846 has no copy-failure result and replaces the previous HRESULT with private-store initialization.
The initial shallow copy also shares pointer arrays with the input.
Adding isolated NULL checks cannot establish safe cleanup after partial copying.

A separate engine patch makes the bytecode and descriptor copies transactional.
Its second revision retains imported deferred collections until the final data cleanup.
This preserves borrowed root-signature and association payloads across further deferrals without another payload copy.
The patch also checks DXIL subobject and import-name allocations and propagates failures through the callers.
Promotion with 0119 requires review, full compilation and regression results for the exact repaired engine artifact.
The currently pinned engine does not include that repair.

The extracted regression runs the production import, defer and cleanup functions with tracked allocation and reference mocks.
The first revision leaves root and association payload pointers outside live allocations after parent release.
The second revision passes two successive import/defer generations after each caller releases its parent reference.
It also passes allocation-failure sweeps for copying, import names and reference cleanup.
These checks establish those ownership paths within the extracted harness. They do not exercise GPU compilation or the full DXIL parser.

The source review therefore does not establish complete 0119 acceptance.
Required tests overwrite and free caller bytecode after creation, then compare asynchronous completion and draw-time variants against controls.
They must also cover collections, AddToStateObject, embedded DXIL subobjects and each copy-allocation failure.
The full engine regression must release the original deferred collection before the next link.
These tests must distinguish the current engine from any repaired build.
No successful runtime 0119 lab acceptance is claimed here.

## Validation boundary

The FillDDITable enum is at H2501-H2525. Publication covers these table types:

| Type | Publication |
| --- | --- |
| 0, 1, 2 | Core, command list and command queue. The first two use the negotiated layout. |
| 8 | EXTENDED_FEATURES_0021, four typed slots, no advertised features. |
| 20 | RENDER_PASS_0053, subject to the adapter render-pass capability. |
| 27 | EXTENDED_FEATURES_0096 at build 0096 or later. The upper feature bound applies to an empty set. |

Type 27 adds HighestAllowedFeatureNumber to the first callback (H12029-H12039).
Enumeration returns S_OK and count zero without writing any feature element.
Unknown feature versions return E_NOTIMPL and count zero without writing version elements.
The enable and callback-install functions retain their 0021 refusal behavior.
The old 0021 table remains available with its own signature.

All other named table types return E_NOTIMPL: 3, 4, 7, 10-17, 19, 21-26 and 30.
This is a stated implementation boundary, not proof that a runtime tolerates each refusal.
Types 10, 16, 23 and 25 describe runtime callbacks, not driver function tables.
Type 30 depends on the shader-cache feature version: 0114 has three slots and 0118 has one.
Neither feature version is advertised. Type 26 is the state-object experiment table, separate from the core 0108 path.
The header does not identify an unambiguous current payload for types 3, 7 and 19.

The ordered host regression covers the observed 0119 request prefix: 0, 1 number 0, 1 number 1, 2, 27.
It requires S_OK at every step and checks each exact layout size.
The runtime trace ended at the first type-27 refusal. A further lab trace must establish any later required tables.

Host tests cover typed negotiation, payload bounds, unsupported-feature errors, API-handle conversion, and callback storage identity.
Content tests cover independent stencil state, fractional and dynamic bias, strip-cut changes, narrow lines, format 191 and modern sampler paths.
Generic tests compare two program outputs with ordinary PSOs after caller-bytecode release, collection destruction and parent destruction.
They also check invalid identifier errors and object cleanup.
The no-output and descriptor tests exercise sample counts and descriptor access, rather than only checking reported caps.
The generic signature gate runs without a GPU as part of the standard engine build, including NativeOnly.
The build matrix covers x64 release, x64 lab and x86 release.
The engine gate must also run both pinned engine flavours.
Passing those checks does not replace a lab run with the inbox runtime and the selected Agility runtime.
Host rendering on an RTX 4090 does not establish BC-250 behavior.
No completed full candidate gate or successful runtime 0119 lab acceptance is claimed here.
Target acceptance still requires the mandatory no-output counts and point-sampling boundary behavior on the target backend.

Optional readback artifacts preserve the test outputs for comparison across adapters.
RGBA targets produce images from actual rendered pixels. Integer and float buffers remain numeric data with separate difference maps.
Exact byte agreement is a differential witness and does not replace each test's expected-value assertions.
These artifacts are test outputs, not screenshots of a game.
