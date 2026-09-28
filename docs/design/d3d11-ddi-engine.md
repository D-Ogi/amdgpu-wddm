# Design note: the DXVK engine behind the system D3D10/11 UMD (M14)

Date: 2026-09-28. Status: engine implemented and tested offline; one standalone engine control on unit A (facts
M725); nothing measured through the Microsoft runtime yet. Scope: the engine `bc250dxvk.dll`, its ABI and its
known gaps. The shell (the DDI UMD in `driver/umd/dxvk/`) is described in that directory's README. Build recipe:
[build.md](../build.md#dxvk). Direction: ADR 0017 item 4 (DXVK is the engine), item 7 (5 % bound), item 8
(threading is a measurement).

## Position in the stack

```
application -> d3d11.dll / dxgi.dll (Microsoft runtime)
            -> shell: DDI tables, handles, runtime callbacks, hosted RADV bootstrap, VkInstance/VkDevice
            -> engine bc250dxvk.dll: DXVK's D3D11 device and immediate context on the imported VkDevice
            -> hosted RADV (the hosted ICD) -> runtime callbacks -> dxgkrnl -> KMD
```

The engine is built from the DXVK branch `amdgpu-wddm/ddi-engine`: upstream DXVK plus the `src/ddi/` directory
and small host-mode hooks in `src/dxvk/`, `src/d3d11/` and `src/util/log/`. `recipe.json` of each build names
the commit. The branch is not published yet.

## Where the boundary sits

ADR 0017 left open whether the shell drives DXVK at its `DxvkDevice`/`DxvkContext` layer or through its COM
`ID3D11Device` objects. The engine exposes the COM layer. Everything a D3D11 driver has to get right above
Vulkan already lives there and is exercised by every DXVK title: state tracking and validation, hazard and
resource tracking, `DISCARD`/`NO_OVERWRITE` renaming, staging, queries, deferred contexts, shader compilation
and the format tables. A shell on `DxvkContext` would have to rebuild all of that next to the DDI translation.
The cost is one virtual call per DDI entry on top of the shell's argument translation. Whether it matters is
part of the 5 % measurement, which has not been made.

What the D3D11 interfaces cannot express goes through `IBc250DxvkDevice`:

| Need | Method |
|---|---|
| Shaders arrive as DDI token streams plus DDI signatures, not DXBC containers | `CreateShader` |
| `CreateElementLayout` has register numbers and no VS bytecode | `CreateInputLayout`, `GetVertexFormat` |
| Primaries, back buffers and shared resources live in runtime allocations | `GetImageCreateInfo`, `CreateTexture2DFromImage` |
| DDI resource destruction and `ResourceIsStagingBusy` | `WaitForResourceIdle`, `IsResourceBusy` |
| DXGI present, `RotateResourceIdentities`, `Blt` | `SubmitForPresent`, `RotateResourceIdentities`, `Blt` |

## The ABI

The single copy of the contract is `src/ddi/bc250_dxvk_engine.h` in the DXVK branch; the shell includes it from
the DXVK checkout it builds against. It is WDK-free: DXVK's `util_gdi.h` declares extern-C D3DKMT prototypes
that collide with the WDK's in one translation unit, so the header uses only `windows.h`, `d3d11_4.h` and
`vulkan_core.h`. Structures that mirror WDK structures keep their layout, and the shell checks that with
`static_assert`.

The DLL exports one function, `Bc250DxvkEngineGetFuncs(abiVersion, funcs)`. A major mismatch returns
`E_NOINTERFACE`; minor versions only add. ABI 1.0 is in force. Its rules E1-E6 are written in the header and are
not repeated here:
- E1 the shell owns VkInstance and VkDevice;
- E2 threads;
- E3 submission;
- E4 one COM reference per DDI object;
- E5 runtime allocations;
- E6 errors.

### Threads (E2)

The runtime enters a device from one thread at a time (the shell does not report
`D3D11DDICAPS_FREETHREADED`), and runtime callbacks are valid only inside a DDI entry. Hosted RADV reaches
runtime callbacks from ordinary Vulkan calls (allocation, residency, submission). So in ABI 1.0 the engine makes
every Vulkan call on the thread inside the engine call ("inline mode"). A host option in `DxvkDevice` then runs
the work of DXVK's worker threads on the calling thread:
- the CS thread;
- submission and fence completion;
- pipeline workers;
- the descriptor worker.

The engine test hooks every Vulkan entry point it hands to the engine and checks the thread of each call.

The cost is CPU time on the application's thread, which per-application DXVK spreads over workers. The
largest part is the recording that DXVK's CS thread would do: about 1.5-1.9 times the per-draw time, measured
offline (see Gaps). Optimized pipeline compiles are kept off draws (see Gaps). A later minor version may add a
broker mode, in which workers hand runtime-reaching work back to a DDI entry; the engine will use it only when
the shell asks for it.

### Shaders from DDI token streams

`CreateShader` builds a DXBC container around the unchanged program, so DXVK's own compiler (dxbc-spirv) sees
the input it expects:
- signature chunks come from the DDI signatures;
- system values get their `SV_` names;
- every other element is named `BC250_R` with a semantic index that encodes the register and the first
  component, consistently across stages and stream output;
- the container hash is computed as DXBC requires.

Stream-output entries name output registers. The engine maps each one to the signature element that holds its
first component, and a gap entry (register `~0u`) becomes a D3D11 declaration entry without a semantic name. A
stream-output declaration without a geometry program (NULL, vertex or domain code) uses DXVK's pass-through
geometry shader.

### Runtime allocations and present (E5)

- **Image creation.** `GetImageCreateInfo` returns the `VkImageCreateInfo` that DXVK's `D3D11CommonTexture`
  would use for the description, including the format list that keeps compression on mutable-format images. The
  shell creates and binds the image on its runtime allocation; `CreateTexture2DFromImage` wraps it.
- **Present.** `SubmitForPresent` ends the engine frame and submits (E3); the shell then orders its present
  fence and calls `pfnPresentCb`.
- **Rotation.** `RotateResourceIdentities` moves image storage in command order through
  `DxvkContext::rotateImageStorage`. Rotated images are tracked as written again, because `invalidateImage`
  resets their tracking.
- **Blt.** `Blt` goes through `DxvkContext::blitImageView`, which stretches, converts formats and resolves.

### Logging and configuration in a host process

- **Log.** `BC250_DXVK_SHELL_SERVICES::Log` receives DXVK's log lines from the start of `CreateDevice` until the
  final `Release` returns. DXVK's logger is per process, so with several devices the oldest device's `Log` gets
  every line and no file is written. Device-less calls (`QueryDeviceRequirements`, `GetAdapterInfo`) log to the
  file in `DXVK_LOG_PATH`, or nowhere. The engine test sees about 300 info lines per device creation, most of
  them the adapter report; a shell that writes one file per process should filter level 3 and above by
  default.
- **Configuration.** Configuration behaves as in per-application DXVK, with one difference that matters: the
  host process is whatever loaded the system UMD, including DWM.
  - `DXVK_CONFIG_FILE` names a file; otherwise the engine reads `dxvk.conf` from the process's current
    directory.
  - `DXVK_CONFIG` adds options inline.
  - DXVK's built-in application profiles apply by executable name.
  - A stray `dxvk.conf` in an application's working directory therefore changes the system driver's behaviour
    for that process, exactly as it would change per-application DXVK. The 5 % comparison must run both paths
    with the same configuration.

## Costs measured offline

The engine test prints the wall time of the three adapter-level calls on the development PC (RTX 4090, not
unit A):

| Call | Time |
|---|---|
| `GetAdapterInfo` | about 17 ms |
| `QueryDeviceRequirements` | about 19 ms |
| `CreateDevice` | about 19 ms |

Each call imports its own `DxvkInstance`. That import is most of the cost: `GetAdapterInfo` does nothing else
and takes about as long. The shell's own `vkCreateDevice` is not included. These are not facts rows; unit A
numbers come from a lab run of the frozen test.

An instance cache keyed by `VkInstance` is not worth it at these costs. It would also risk reusing a stale
import when a destroyed handle's value comes back (ABA).

## Gaps

| Gap | State | Plan |
|---|---|---|
| Stream output without a geometry program on line and triangle lists | Upstream DXVK behaviour: the pass-through geometry shader (dxbc-spirv `IoMap::emitGsPassthrough`) emits vertex 0 of each primitive with point output, and linking patches only the input primitive type. Point lists are exact. A line or triangle list streams out one vertex per primitive, and a rasterized stream draws points. DXVK's own `CreateGeometryShaderWithStreamOutput` without code behaves the same. The engine test keeps a tripwire (exactly one exact record from a triangle list), so a fix shows up as a test change. | In dxbc-spirv, emit every input vertex and set the output topology and vertex count at link time, where DXVK patches the input type (`dxvk_shader_ir.cpp`). Not started. |
| Optimized pipelines in inline mode | Closed offline in engine commit 4c5fd822. With graphics pipeline libraries, DXVK draws with a fast-linked pipeline and compiles the optimized variant on a pipeline worker. Inline mode used to run that compile on the drawing thread, which then paid for both. The compile is now queued, and `SubmitForPresent` runs the queue after the frame's submission. It starts compiles for up to `dxvk.inlinePipelineBudget` microseconds (default 2000), so one long compile can exceed the budget. A budget of 0 keeps the fast-linked pipelines. The engine test classifies every graphics pipeline the engine creates. The previous engine fails its check, because its draw compiled the optimized pipeline itself. | Measure on unit A with a real title: warm-up frame times, and how long the queue takes to drain at the default budget. A client that never presents keeps fast-linked pipelines. |
| Inline execution on CPU-bound work | Measured offline (engine commit ea512f65, development PC, three runs each): `bc250dxvk_engine_test --bench` issues 1000 draws per frame, each with a constant buffer DISCARD and a texture switch, at a frame latency of 3. Inline mode took 346-357 us wall per frame. With DXVK's worker threads it took 184-233 us (switch `BC250DXVK_MEASURE_WORKER_THREADS=1`, which breaks E2 and exists only for this comparison). So the application thread pays roughly 1.5-1.9 times as much per draw inline. A GPU-bound title does not notice; a CPU-bound one can miss the 5 % bound by far. | Two steps. First, the ADR 0017 item 8 measurement on unit A: do the runtime callbacks that hosted RADV uses (allocation, residency, submission) work from a thread that is not inside a DDI entry? If they do, serialize them per device and allow DXVK's workers. If they do not, add a broker mode (a later ABI minor): workers queue the runtime-reaching work, and DDI entries and engine waits service the queue. |
| On-disk shader cache | Per-application DXVK keeps the dxbc-spirv results of each executable in `%LOCALAPPDATA%\dxvk` (or `DXVK_SHADER_CACHE_PATH`); its writer thread does file I/O only. The engine turns the cache off, to keep a writer thread and cache files out of every process that loads the system driver, DWM included. Every process start therefore translates its shaders again. | Decide with the load-time part of the 5 % comparison; the cache needs no Vulkan call, so E2 does not forbid it. |
| `Blt` with ROTATE90/270, or into a multisampled destination | `E_NOTIMPL` | Implement when a runtime path needs it. |
| 5 % bound against per-application DXVK | Not measured | Needs the shell's positive run through the system runtime. The per-application comparison build is the `per-app` recipe in build.md. |

## Validation

`bc250dxvk_engine_test.exe` is the offline positive control ([build.md](../build.md#dxvk)). It plays the shell:
it owns instance and device, feeds DDI-form shaders and runtime-style images, and checks the following:
- pixels;
- 500 sustained frames under a memory bound;
- storage rotation and Blt;
- stream output on point lists, plus the triangle-list tripwire;
- the thread of every Vulkan call (E2);
- no optimized pipeline compiled by a draw, and the deferred ones compiled by `SubmitForPresent`;
- the Log contract;
- final `Release` returning 0.

A build handed to the lab comes from a clean tree (`recipe.json` records no modified entries) and carries two
receipts from the development PC, one plain and one with the Khronos validation layer. Both must pass with no
failures and no validation messages. Facts M725 is the first such set on unit A, run standalone on RADV GFX1013
without the runtime.

Z pustego i Salomon nie naleje. (Even Solomon cannot pour from an empty jug.) Until the shell's first positive
run through the runtime, there is nothing to hold against the 5 % bound.
