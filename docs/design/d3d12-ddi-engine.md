# Design note: the vkd3d-proton engine behind a native D3D12 UMD (M15)

Date: 2026-09-28. Status: engine ABI 1.0 implemented, tested offline on the development PC and run standalone on
unit A (T1, fact M757). The native D3D12 shell is being built; no DDI device runs on the engine yet. Scope: the engine `amdgpu_wddm_vkd3d.dll`, its ABI and its known gaps.
Build recipe: [build.md](../build.md#vkd3d-proton). Direction: ADR 0017 item 5 (vkd3d-proton is the default
engine; FL 12_0 Must, 12_1 Should; a bounded spike first), item 6 (vkd3d-proton stays a separate LGPL DLL) and
item 7 (no numeric bound for M15 before its first measurement).

## Position in the stack

```
application -> d3d12.dll / d3d12core.dll / dxgi.dll (Microsoft runtime)
            -> shell: OpenAdapter12, D3D12 DDI tables and handles, runtime callbacks, contexts, residency,
               present; provides hosted RADV's Vulkan entry point
            -> engine amdgpu_wddm_vkd3d.dll: vkd3d-proton's D3D12 device, on its own VkInstance and VkDevice
            -> hosted RADV (the hosted ICD) -> runtime callbacks -> dxgkrnl -> KMD
```

The engine is built from the vkd3d-proton branch `amdgpu-wddm/ddi-engine`. It is upstream vkd3d-proton plus the
`libs/ddi/` directory (MIT) and two small changes in `libs/vkd3d/`:
- the embedder's configuration defaults (see Files, configuration and logging);
- the optional device extensions that extend `VK_KHR_swapchain` are enabled only together with it. `d3d12core`
  always requires the swapchain extension. The engine enables no WSI, so its device create info was invalid,
  as the validation layer reported (`VUID-vkCreateDevice-ppEnabledExtensionNames-01387`).

The branch is published at <https://github.com/D-Ogi/vkd3d-proton/tree/amdgpu-wddm/ddi-engine>; `recipe.json` of
each build names the commit.

## Where the boundary sits

The DXVK engine imports the shell's VkDevice (its rule E1). The D3D12 engine instead creates its own instance
and device through `vkd3d_create_instance` and `vkd3d_create_device`, the public path that `d3d12core.dll`
takes. The shell passes only its `vkGetInstanceProcAddr`.

vkd3d-proton decides which extensions and features its device needs, and builds its queue families from them.
An imported device would have to replicate that negotiation in the shell and keep it in step with every
upstream change. The public path costs nothing of the kind. What the hosted contract adds to instance or device
creation, the shell's entry point can still chain into the `pNext` lists it sees.

The engine exposes vkd3d-proton's ordinary `ID3D12Device` objects. The D3D12 DDI is close to the API: handles,
descriptor heaps, command lists and root signatures map almost one to one onto the device's interfaces. So the
shell translates arguments and does not re-implement state. Whether the cost of that extra call layer matters
is part of the first M15 measurement.

## The ABI

The single copy of the contract is `libs/ddi/bc250_vkd3d_engine.h` in the vkd3d-proton branch; the shell
includes it from the checkout it builds against. It needs only `windows.h` and `vulkan_core.h`, so both the
engine's translation units (vkd3d-proton's own D3D12 headers) and the shell's (SDK `d3d12.h` plus
`d3d12umddi.h`) can include it.

The DLL exports one function, `Bc250Vkd3dEngineGetFuncs(abiVersion, funcs)`, which fills `CreateDevice`. Before
branch commit a582668d the DLL was `bc250vkd3d.dll`; the header file name and the code identifiers keep their
BC250 names.

- **Versioning.** The version the shell passes is the version it *requires*: the lowest minor whose additions
  it calls. It is not the version of the header it compiled against. A newer header therefore does not force a
  newer engine; the DXVK shell learned that the hard way. A major mismatch, or a minor above the engine's,
  returns `E_NOINTERFACE`.
- **Rules.** The rules V1-V6 are written in the header:
  - V1: the engine creates its own Vulkan objects through the shell's entry point and loads no Vulkan DLL;
  - V2: the adapter is the physical device whose `deviceLUID` equals the adapter's LUID, and devices are
    independent;
  - V3: threads and submission (below);
  - V4: vkd3d-proton COM objects, and the final `Release` returns 0;
  - V5: HRESULTs only;
  - V6: no files.

### Threads and submission (V3)

vkd3d-proton submits from one submission thread per queue and waits for fences on worker threads. Neither fits
a DDI device:
- The runtime expects the work of `ExecuteCommandLists` to have been submitted to that queue's context when the
  call returns.
- The runtime callbacks that hosted RADV uses are bound to the thread inside the DDI entry.

ABI 1.0 is therefore for capability queries and engine bring-up off the runtime. A later minor adds a
synchronous, context-bound queue mode: each D3D12 queue gets one runtime context and one VkQueue, and
`ExecuteCommandLists` submits on the caller's thread.

The fence contract that mode must follow is not settled.
- The D3D12 DDI gives `pfnCreateFence` only GPU virtual address placements, and the queue-level
  `pfnSignalFence` and `pfnWaitForFence` serve linked adapters.
- That suggests the runtime itself signals and waits on the monitored fences of the contexts the UMD creates.
  If so, nothing needs to be imported into vkd3d-proton.
- This is a hypothesis. ADR 0017 item 5 names the opposite shape (the runtime's fence imported as a timeline
  semaphore). A bounded probe with a logging shell decides it before the minor is frozen.

### Descriptor handles

The UMD defines the CPU and GPU descriptor handle values and the increment, and the runtime carries them. The
shell can therefore return vkd3d-proton's own values. They are not plain addresses:
- On the development PC the CBV/SRV/UAV increment is 32.
- The CPU heap start carries tag bits in its low bits.
- The GPU heap start is a synthetic value (`0x100000000`), not a virtual address.

The shell must pass them through untouched: no alignment, no masking, no translation.

### Files, configuration and logging (V5, V6)

- **Disk cache.** vkd3d-proton by default keeps `vkd3d-proton.cache` in the process's working directory and
  runs a writer thread for it. A system driver is loaded into every D3D12 process, so the engine turns that
  cache off, as if `VKD3D_CONFIG` contained `pipeline_library_app_cache`. An application's own
  `ID3D12PipelineLibrary` keeps working.
- **How the default is set.** The branch adds `vkd3d_config_set_embedder_defaults()` to libvkd3d. The module
  that embeds libvkd3d calls it once, before its first instance; `d3d12core.dll` never calls it.
- **Configuration.** The process's `VKD3D_CONFIG` and the other vkd3d-proton variables still apply on top, for
  debugging. vkd3d-proton's built-in application workarounds apply by executable name, as per application.
  The M15 comparison must run both paths with the same configuration.
- **Log.** The log goes where vkd3d-proton's log goes: stderr, or the file in `VKD3D_LOG_FILE`. A shell log sink
  is a later minor.
- **C runtime.** The engine links the C runtime statically. The branch's meson refuses a dynamic CRT for it, so
  it needs no Visual C++ redistributable and shares no CRT state with the application.

## Costs measured offline

On the development PC (RTX 4090, not unit A), `CreateDevice` took 281-312 ms over six runs. That covers the
instance, the device and vkd3d-proton's device initialization. The DXVK engine's `CreateDevice` takes about
19 ms, but it excludes the shell's own `vkCreateDevice`, so the two numbers do not compare. These are not facts
rows; unit A numbers come from a lab run of the same test.

## Gaps

| Gap | State | Plan |
|---|---|---|
| Queue mode for a DDI device | ABI 1.0 has vkd3d-proton's own threads (V3). | ABI 1.1 after the fence probe: one runtime context and one VkQueue per D3D12 queue, synchronous `ExecuteCommandLists`, engine bookkeeping on its own completion signal. |
| Runtime allocations | Descriptor heaps and resources are engine allocations. | Import from runtime allocations in the same minor, the way the DXVK engine's E5 imports images. |
| Residency, offer and reclaim | Not implemented. | Same shape as the DXVK engine's gap; needs the hosted query from memory to kernel allocation. |
| FL 12_0 and 12_1 | vkd3d-proton reports 12_0 only with tiled resources tier 2 (`libs/vkd3d/device.c`, feature level selection), so it needs sparse binding from hosted RADV. 12_1 adds ROVs and conservative rasterization. | Sparse on by default in the hosted driver after its conformance run, then a caps run on unit A. |
| Disk shader cache | Off (V6). Every process start compiles its pipelines again. | Decide with the load-time part of the M15 measurement, as for the DXVK engine. |

## Validation

`amdgpu_wddm_vkd3d_engine_test.exe <amdgpu_wddm_vkd3d.dll> [adapter substring] [--icd <driver DLL>] [--fl <hex>]`
is the offline positive control. It plays the shell: it loads a Vulkan driver (the loader, or with `--icd` a
driver DLL directly through `vk_icdGetInstanceProcAddr`, as the shell loads hosted RADV) and passes its entry
point to the engine. It runs on any Vulkan 1.3 GPU, opens no window, writes no files and exits by itself; exit
code 0 means every check passed:
- the export's version rules;
- an unknown LUID refused with no device created;
- `CreateDevice` on the adapter's LUID, and `GetAdapterLuid` returning it;
- a second independent device;
- the capabilities the shell's `GetCaps` will mirror: feature level, the 12_0/12_1 tiers, shader model, ray
  tracing tier;
- a copy round trip UPLOAD -> DEFAULT -> READBACK on a direct queue, word for word;
- a compute dispatch: a DXIL `cs_6_0` program with an embedded root signature writes a raw UAV reached through
  a shader-visible descriptor table at `start + 3 * increment`, word for word;
- final `Release` returning 0.

On the development PC every check passes. The GPU is an RTX 4090 at FL 12_2, SM 6.8, ray tracing tier 1.1.
The engine's T1 on unit A (the frozen 1.0 package under its pre-rename file names, hosted RADV loaded directly)
passed every check as well: FL 11_1 with tiled resources tier 0, SM 6.8, ray tracing tier 1.1 reported, and a
descriptor handle increment of 64 where the development PC has 32 (M757). Handles and increments stay opaque:
nothing may assume a fixed increment.

A build handed to the lab follows the DXVK engine's rule:
- it comes from a clean tree: `recipe.json` records no modified or untracked entries;
- it carries two receipts from the development PC, one plain and one with the Khronos validation layer and
  synchronization validation. Both must pass with no validation messages and no vkd3d-proton error lines.

Nie od razu Kraków zbudowano. (Kraków was not built in a day.) Until the shell exists, the engine answers
questions about capabilities and nothing else.
