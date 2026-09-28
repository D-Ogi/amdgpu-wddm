# engine-ddi: D3D12 DDI slots on the vkd3d-proton engine

Status: boundary r3 (2026-09-28): r2 plus the adapter caps path on engine ABI 1.2. The static library
`engine-ddi.lib` builds with `tools/build/build-engine-ddi.ps1`, and [INTEGRATION.md](INTEGRATION.md) says what
the shell calls and when. The engine ABI header is included by path from the pinned vkd3d-proton fork checkout
([engine-abi.json](engine-abi.json)).

## What it is

This directory translates D3D12 DDI 0092 slots into calls on the engine's ordinary `ID3D12Device`,
`ID3D12GraphicsCommandList` and `ID3D12CommandQueue` objects. The engine is `amdgpu_wddm_vkd3d.dll`; see
[the engine design note](../../../../docs/design/d3d12-ddi-engine.md).

The work is split between engine-ddi and the native12 shell in this directory's parent:
- **engine-ddi** handles resources, descriptors, root signatures, shaders and pipelines, command pools and lists,
  queries, state objects, and the caps payload builders.
- **The shell** keeps the adapter, device state, FillDDITable composition, queues and their WDDM contexts, fences,
  allocation and residency callbacks, the DXGI table, present and registration.

[SLOTS.md](SLOTS.md) lists the owner and phase of every slot: 175 engine-ddi, 28 shell, including the shell's
override of `pfnPresent`.

## The boundary, in `engine-ddi.h`

**DeviceContext.**
- The shell creates one in CreateDevice, after the engine device exists, and destroys it in DestroyDevice.
- engine-ddi finds it from a `D3D12DDI_HDEVICE` only through the shell's `ResolveDevice` hook. It never reads
  `native12::Device`.
- `destroy_device_context` with live objects returns S_FALSE and their count, and leaves the context and its
  engine reference intact, because those records still point to it. There is no abandon operation; terminal
  device loss would need a separate reviewed addition that says how every record stops using the context.

**ShellHooks.**
- Device and list error reporting, and the device-loss query. Sticky-loss policy stays with the shell.
- Command-list table binding: the shell keeps the `hRTTable` values and calls `pfnSetCommandListDDITableCb`.
- Heap memory: `allocate_memory`, and `free_memory`, which returns an HRESULT.
- Hooks run only on the thread of a DDI call into the device, during that call, and never re-enter engine-ddi.
  Two DDI threads may call them at once; engine-ddi holds no lock of its own while it calls one.

**MemoryMode.**
- `RuntimeBacked` is the only native mode. Every heap's memory is a runtime allocation that hosted RADV has
  imported into the engine's VkDevice. A failed allocation fails the create. There is no fallback.
- `EnginePrivateTest` exists for the offline harness only. It is refused unless the implementation is compiled
  with `AMDGPU_WDDM_ENGINE_DDI_HARNESS`.

**Memory domains.**
- *Heap memory*: one `ImportedMemory` record per heap, shell-owned from the moment `allocate_memory` succeeds.
  `MemoryRequest` carries the runtime owner the DDI supplied: the fourth argument of `pfnCreateHeapAndResource`
  (a `D3D12DDI_HRTRESOURCE`), the heap's for a heap and the pair's for a committed resource. engine-ddi never
  invents one.
- `ImportedMemory::gpu_va` is a completed, validated mapping, filled by the shell only after
  MapGpuVirtualAddress and residency have completed. 0 means no VA, and any use that needs one fails with
  E_INVALIDARG.
- *Internal memory*: what the engine's VkDevice allocates for itself (descriptor heap backing, rings, scratch,
  query pools, engine-ddi's retirement fences). Device-owned lifetime and residency through the device's
  callbacks; never an `ImportedMemory` record and never a standalone KMT allocation. engine-ddi does not
  allocate it.

**Heaps and resources.**
- A heap-only create makes one allocation.
- A committed resource is a dedicated allocation, a heap made from that memory, and a resource placed at 0.
- A placed resource uses the existing heap's memory, holds a reference on it, and never allocates.
- Placement is checked before the engine is called, with E_INVALIDARG on any mismatch:
  - the D3D12 placement alignment and heap bounds;
  - the heap's allowed resource categories;
  - in RuntimeBacked, the size and alignment of the import. The engine's `CreateHeapFromMemory` (ABI 1.2 V10)
    checks the Vulkan memory type and refuses one it would not pick for the heap.
- Aliasing follows D3D12, and aliasing barriers go to the engine unchanged.
- In RuntimeBacked every heap is an engine heap made by `CreateHeapFromMemory` over the shell's `ImportedMemory`,
  for all three shapes; MapHeap and UnmapHeap go to the engine's V10 MapHeap and UnmapHeap. The offline harness
  round-trips a committed and a placed buffer on imported memory word for word (INTEGRATION.md). Whether the
  runtime's own heap shape matches the harness's is still a lab question.

**Release sequence of heap memory.** It runs once, when the last user of a heap has been destroyed.
1. Retirement. A final COM Release does not prove that the GPU has finished. engine-ddi keeps ownership in a
   deferred-release record until every engine queue of the device has completed the work submitted before the
   destroy. It reads that from its own fence on each engine queue, signalled after every
   `execute_command_lists`. If a queue is destroyed first, its part retires only if its fence reached the
   recorded value; otherwise the record stays pending and counts as live.
2. The engine's final Release of its heap. The engine never frees borrowed memory.
3. `free_memory`: the shell releases its Vulkan import. engine-ddi calls it exactly once and never retries. A
   failure is reported through `report_device_error`, and the shell keeps its record, handle and cookie.
4. The runtime deallocation, by the shell, on the same thread.

Steps 2 to 4 run only on the thread of a DDI call into the owning device while its runtime device exists:
- the call that makes the last destroy, if the work has already retired;
- otherwise the first later call that observes retirement: `execute_command_lists`, `pfnCreateHeapAndResource`,
  `pfnDestroyHeapAndResource`, `destroy_engine_queue` or `destroy_device_context`.

They never run from an engine thread or callback: INLINE mode has no engine threads, and engine-ddi starts none.

**Shaders.**
- The DDI payload is the bare program (DXBC tokens, or the DXIL part) with its length in DWORD 1, and
  register-only signatures. That the buffer holds exactly that many DWORDs is an inference from the SAL
  annotation `_In_reads_(pShaderCode[1])`; no runtime payload has been measured.
- Every create-shader slot rebuilds the container the engine compiles with
  [shader-container](shader-container/README.md) `BuildContainer`, which reads no further than that length:
  hull and domain programs with the Tessellation signatures, the others with Standard. The container stays in
  engine-ddi's own allocation until DestroyShader. A failure is logged and reported through
  `report_device_error`: E_NOTIMPL for a program the reconstruction cannot represent, E_INVALIDARG for a broken
  payload, E_OUTOFMEMORY. Mesh and amplification programs are E_NOTIMPL.
- CreatePipelineState gives the engine the container bytes, names input elements from the vertex program's
  rebuilt input signature (`InputLayoutSemantic`) and stream-output entries from the last stage before
  rasterization (`StreamOutputSemantic`). The element layout, blend, rasterizer and depth-stencil states keep
  their descriptions until then. A stream-output declaration with a gap is E_NOTIMPL while the pinned engine
  DLL crashes on one (INTEGRATION.md).
- The harness creates a DXIL compute program (dxc) and a DXBC vertex and pixel program (fxc) through these slots,
  from containers reduced to the DDI form, and checks the dispatch and the draw word for word. The reduction is
  the harness's model of the runtime, not a measurement.

**Engine parts of shell-owned mixed slots.**
- `create_engine_queue` and `destroy_engine_queue`: the shell creates the context first, and the engine binds its
  VkQueue to it through the engine's BindQueue service. engine-ddi adds a retirement fence per queue.
- `execute_command_lists`: synchronous; everything is on the bound context on return.
- `resource_allocation`: gives Present the runtime allocation behind a committed back buffer.

**Caps.**
- GetCaps comes before any device (lab run M768). `query_adapter_caps` asks the engine once through ABI 1.2
  QueryAdapterCaps (V11), with the create info the shell will pass to CreateDevice. The engine answers
  D3D12_FEATURE_* queries under its own device policy, with no VkDevice. `AdapterCaps` keeps those answers and
  nothing of engine-ddi's own.
- There is no temporary engine device in the native driver, and no harness-only caps source.
- `build_caps` maps each answered type from the engine's answers or a documented constant (the table in
  INTEGRATION.md). It writes only exact 0092 payload sizes, never a prefix. A wrong size is E_INVALIDARG and an
  unanswered type E_NOTIMPL, both logged with type and size.

**Records.**
- Every object's runtime-owned storage starts with a 24-byte header: a type tag, flags, one engine reference and
  the creating device.
- Destroy releases what the record holds and poisons the tag. Use through another device is refused.

## Review of r1 and what changed

Review 596 of r1 raised six points; r2 answers each:
1. Context teardown: S_FALSE keeps the context, and there is no abandon operation.
2. `free_memory`: returns HRESULT, is called exactly once, and follows the release sequence.
3. Placed resources and allocation: covered under heaps and resources.
4. Caps: come from an adapter query, not an engine device.
5. Internal memory: its own domain.
6. Shaders: no container synthesis in the native intake.

Two clarifications were added after the review:
- retirement before `free_memory`, on a DDI thread;
- the runtime owner in `MemoryRequest`, and `gpu_va` as a completed mapping.

r3 replaced the r2 caps declarations (`EngineCaps`, `collect_caps`) with `query_adapter_caps`,
`free_adapter_caps` and `build_caps` over the engine's QueryAdapterCaps answers.

Answer 6 no longer holds: the native intake now rebuilds containers with shader-container, whose offline control
rebuilt the fxc and dxc containers of eight cases byte for byte before it was wired in. The boundary did not
change; only the comment on shaders in `engine-ddi.h` did.

## Gate

`tools/build/build-engine-ddi.ps1 -NativeOnly` is the recipe for what the shell links; it needs no engine DLL
and no GPU. Without `-NativeOnly` it also builds the harness and runs against the pinned engine DLL.
1. The engine ABI header's SHA-256 must equal the pin in `engine-abi.json`.
2. `engine-ddi-header-test.cpp`, built with the shell's flags (`/std:c++20 /W4 /WX`, SDK `d3d12.h` with WDK
   `d3d12umddi.h`, the Vulkan headers and the engine ABI header). It checks that the header is self-contained,
   boundary revision 3, unique record tags, table sizes 976 and 560, the pinned struct layouts, the signatures of
   `free_memory`, the caps functions and `pfnCreateHeapAndResource`, and the ABI 1.2 header.
3. `engine-ddi.lib` compiled with `/analyze` under the same `/WX`, with no `harness_` symbol in it.
4. `tests/native-policy-test.cpp`: EnginePrivateTest refused, both tables filled.
5. `tests/caps-test.cpp` against a stub engine: GetCaps 1074 with 8 bytes and 1007 with 4 bytes, and every
   other answered type at its exact size. With `--engine` it runs `query_adapter_caps` on the real DLL.
