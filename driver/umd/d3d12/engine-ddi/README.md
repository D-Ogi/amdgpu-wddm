# engine-ddi: D3D12 DDI slots on the vkd3d-proton engine

Status: boundary draft r1 (2026-09-28), for review before any slot is implemented. No slot code exists yet. The
directory is not part of the shell build: the gate below runs from its own script until the boundary is agreed.

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
- The shell creates one in CreateDevice, after the engine device exists, and destroys it after the last object.
- engine-ddi finds it from a `D3D12DDI_HDEVICE` only through the shell's `ResolveDevice` hook. It never reads
  `native12::Device`.

**ShellHooks.**
- Device and list error reporting, and the device-loss query. Sticky-loss policy stays with the shell.
- Command-list table binding: the shell keeps the `hRTTable` values and calls `pfnSetCommandListDDITableCb`.
- Runtime-backed memory: `allocate_memory` and `free_memory`.
- Hooks run only on the DDI call's thread and never re-enter engine-ddi.

**MemoryMode.**
- `RuntimeBacked` is the only native mode. Every heap and every committed or placed resource's memory is a
  runtime allocation that hosted RADV has imported into the engine's VkDevice.
- A failed allocation fails the create. There is no fallback.
- `EnginePrivateTest` exists for the offline harness only. It is refused unless the implementation is compiled
  with `AMDGPU_WDDM_ENGINE_DDI_HARNESS`.

**Engine parts of shell-owned mixed slots.**
- `create_engine_queue` and `destroy_engine_queue`: the shell creates the context first, and the engine binds its
  VkQueue to it through the engine's BindQueue service.
- `execute_command_lists`: synchronous; everything is on the bound context on return.
- `resource_allocation`: gives Present the runtime allocation behind a back buffer.

**Caps.** A snapshot is collected from one engine device of the selected adapter. `build_caps` then writes only
exact, validated payload sizes at or below the negotiated version, with no prefix copies.

**Records.**
- Every object's runtime-owned storage starts with a 24-byte header: a type tag, flags, one engine reference and
  the creating device.
- Destroy releases the engine reference and poisons the tag. Use through another device is refused.

## Open questions for review

1. **Caps before any device.** GetCaps arrives before CreateDevice. The choices are:
   - a temporary engine device per adapter: about 0.3 s on the development PC;
   - a persistent adapter-level engine device;
   - a new engine entry that answers caps from the physical device.
2. **Importing runtime allocations.** The engine side needs engine ABI 1.2 `CreateHeapFromMemory`: an
   `ID3D12Heap` over a VkDeviceMemory it does not own. Committed resources then become dedicated heap plus placed
   resource. The hosted RADV side is the shell's: a runtime allocation imported into the engine's VkDevice, which
   the M14 image path may already do. Is that path reusable here?
3. **Engine-internal memory.** Descriptor heaps, upload rings and scratch are allocated through hosted RADV
   without an hRTResource. Is that acceptable for residency accounting in the first native slice?
4. **Shaders.** Is the DDI shader code a DXIL container or a bare program? The inventory infers bare program
   tokens with register-only signatures. The first native run should log the first DWORDs.

## Gate

`scratch\m15\ddi-translate\build-header-test.ps1`, a local script until the shell build takes this directory,
builds `engine-ddi-header-test.cpp` with the shell's flags:
- `/std:c++20 /W4 /WX`;
- SDK `d3d12.h` together with WDK `d3d12umddi.h`;
- the Vulkan headers and the engine ABI header from the vkd3d-proton checkout.

It checks that the header is self-contained, that the record tags are unique, and the table sizes 976 and 560.
