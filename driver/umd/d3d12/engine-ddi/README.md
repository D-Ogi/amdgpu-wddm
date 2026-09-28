# engine-ddi: D3D12 DDI slots on the vkd3d-proton engine

Status: boundary r2 (2026-09-28), r1 revised after review. No slot code exists yet. The directory is not part of
the shell build: the gate below runs from its own script.

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
  - in RuntimeBacked, the Vulkan memory type (one of `memory_type_bits`), size and alignment of the import.
- Aliasing follows D3D12, and aliasing barriers go to the engine unchanged.
- The first RuntimeBacked slice is dedicated allocations only, which covers Present back buffers. Heap-only
  heaps and placed resources on runtime memory return E_NOTIMPL until the hosted same-storage/VA import is
  validated.

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
- The native intake builds no container. The DDI payload is the bare program (`_In_reads_(pShaderCode[1])`)
  with register-only signatures.
- engine-ddi copies it into private storage, logs `pShaderCode[0..3]`, the length and the signature entry
  counts, and reports E_NOTIMPL.
- A harness-only path (`AMDGPU_WDDM_ENGINE_DDI_HARNESS`) accepts a complete DXBC or DXIL container, so that
  pipelines and dispatches can be tested offline. It proves nothing about the runtime payload.

**Engine parts of shell-owned mixed slots.**
- `create_engine_queue` and `destroy_engine_queue`: the shell creates the context first, and the engine binds its
  VkQueue to it through the engine's BindQueue service. engine-ddi adds a retirement fence per queue.
- `execute_command_lists`: synchronous; everything is on the bound context on return.
- `resource_allocation`: gives Present the runtime allocation behind a committed back buffer.

**Caps.**
- `collect_caps` takes `EngineCaps`, an adapter-level result of the engine's own policy. The declared source is
  the planned engine ABI 1.2 QueryAdapterCaps: a physical-device query with no VkDevice, queues or GPU storage.
- There is no temporary engine device in the native driver. The harness may fill `EngineCaps` from a real
  engine device's CheckFeatureSupport, under the harness macro only.
- `build_caps` writes only exact, validated payload sizes at or below the negotiated version, with no prefix
  copies.

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

## Gate

`engine-ddi-header-test.cpp` is built with the shell's flags:
- `/std:c++20 /W4 /WX`;
- SDK `d3d12.h` together with WDK `d3d12umddi.h`;
- the Vulkan headers and the engine ABI header, the latter exported byte for byte from the vkd3d-proton fork.

It checks that the header is self-contained and that the boundary revision is 2. It also checks:
- the record tags are unique;
- the table sizes are 976 and 560;
- the boundary structs have their pinned layouts;
- the r2 signatures of `free_memory`, `collect_caps` and `pfnCreateHeapAndResource`.
