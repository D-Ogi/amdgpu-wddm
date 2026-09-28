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
| DXGI 1.2 `Blt1` with a source rectangle (ABI 1.1) | `IBc250DxvkDevice1::Blt1` |
| Runtime allocations with a tiling the shell chose, such as LINEAR (ABI 1.2) | `IBc250DxvkDevice2::CreateTexture2DFromImage2` |
| Adapter-level `GetCaps` answers must hold at every offered level; the device answers only at its own (ABI 1.3) | `IBc250DxvkDevice3::CheckFeatureSupportAtLevel` |

## The ABI

The single copy of the contract is `src/ddi/bc250_dxvk_engine.h` in the DXVK branch; the shell includes it from
the DXVK checkout it builds against. It is WDK-free: DXVK's `util_gdi.h` declares extern-C D3DKMT prototypes
that collide with the WDK's in one translation unit, so the header uses only `windows.h`, `d3d11_4.h` and
`vulkan_core.h`. Structures that mirror WDK structures keep their layout, and the shell checks that with
`static_assert`.

The DLL exports one function, `Bc250DxvkEngineGetFuncs(abiVersion, funcs)`. A major mismatch returns
`E_NOINTERFACE`. Minor versions only add, and what they add sits behind a new interface or function, so an older
engine answers `E_NOINTERFACE` instead of doing the wrong thing. ABI 1.3 is in force: 1.0 plus
`IBc250DxvkDevice1` (`Blt1`), `IBc250DxvkDevice2` (`CreateTexture2DFromImage2`) and `IBc250DxvkDevice3`
(`CheckFeatureSupportAtLevel`), which the engine device answers to `QueryInterface`. The rules E1-E6 are written
in the header and are not repeated here:
- E1 the shell owns VkInstance and VkDevice;
- E2 threads;
- E3 submission;
- E4 one COM reference per DDI object;
- E5 runtime allocations;
- E6 errors.

### Threads (E2)

The runtime enters a device from one thread at a time (the shell does not report
`D3D11DDICAPS_FREETHREADED`), and runtime callbacks are valid only inside a DDI entry. Hosted RADV reaches
runtime callbacks from ordinary Vulkan calls (allocation, residency, submission). So in ABI 1 the engine makes
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
geometry shader. On the branch it emits every vertex of its input primitive, and linking sets its input and
output primitive together, so strips stream out as lists, primitives that do not fit are dropped whole, and a
rasterized stream draws lines and triangles (dxbc-spirv `IoMap::emitGsPassthrough`,
`LowerIoPass::changeGsPassthroughPrimitiveType`). Upstream emitted one point per primitive.

Upstream `D3D11Device::ComputeShaderKey` read the stream-output digest after `finalize()` had reset the hasher,
so every shader key carried the same constant there. Shaders with equal bytecode then shared one module whatever
their declaration, strides or rasterized stream. For the DDI this is the common case, because every stream-output
shader of one signature arrives as the same container. The branch fixes it.

The runtime's `D3D11_1DDIARG_SIGNATURE_ENTRY` carries no stream, so the shell passes Stream 0 for every entry.
When all entries of a gs_5_0 output signature say 0, the engine reads the streams from the program itself: each
entry takes the first `dcl_stream` block whose output declaration covers its register and components and has not
claimed them yet. fxc reuses registers across streams, so the register alone is not enough. A nonzero Stream is
kept. On the way, dxbc-spirv turned out to emit no `Stream` decoration for outputs of streams 1-3, stream output
included, because the geometry stream mask was set only after the output declarations; streams 1-3 then captured
stream 0's values. Per-application DXVK has the same bug; the submodule branch fixes it (commit 253c08c).

### Predication

Upstream DXVK's `SetPredication` records the predicate and ignores it. The branch honours it on the immediate
context, the only context the DDI drives while the shell does not report command lists. Every operation in
D3D11 spec 20.2 checks the predicate:
- Draw* and Dispatch*;
- the clears, ClearView included;
- the copies, CopyStructureCount and UpdateSubresource;
- GenerateMips and ResolveSubresource.

The predicate is evaluated on the CPU. A result that is already available is used. Otherwise the engine submits
and waits for the GPU, except for a predication hint, which may proceed. The operation is skipped when the result
equals the predicate value.

`CreatePredicate` accepts the stream-output overflow types too. The any-stream overflow predicate reads all four
streams; upstream read stream 0 only.

Hosted RADV lists `VK_EXT_conditional_rendering`, which could evaluate predicates on the GPU instead of waiting.
Upstream removed its old implementation in 2020 as broken on several drivers. Nothing here moves to it without a
measurement showing the CPU wait matters.

### Runtime allocations and present (E5)

- **Image creation.** `GetImageCreateInfo` returns the `VkImageCreateInfo` that DXVK's `D3D11CommonTexture`
  would use for the description, including the format list that keeps compression on mutable-format images. The
  shell creates and binds the image on its runtime allocation; `CreateTexture2DFromImage` wraps it. An image
  with another tiling, such as the LINEAR surfaces the shell allocates, goes through `CreateTexture2DFromImage2`.
  It takes the shell's `VkImageCreateInfo`, checks it against `GetImageCreateInfo` and imports the image with
  that tiling. DXVK chooses format features and layouts by tiling, so a LINEAR image wrapped as OPTIMAL would be
  driven with features it may not have.
- **Present.** `SubmitForPresent` ends the engine frame and submits (E3); the shell then orders its present
  fence and calls `pfnPresentCb`.
- **Rotation.** `RotateResourceIdentities` moves image storage in command order through
  `DxvkContext::rotateImageStorage`. Rotated images are tracked as written again, because `invalidateImage`
  resets their tracking.
- **Blt.** `Blt` and `Blt1` go through `DxvkContext::blitImageView`, which stretches, converts formats and
  resolves. `Blt` takes the whole source subresource; `Blt1` takes a source rectangle, typically a dirty region.

### Feature answers for GetCaps

The runtime asks `GetCaps` at adapter level, before any device exists. A hosted physical device cannot exist
then: creating its winsys asks the runtime for a paging queue, and that callback belongs to a runtime device.
So the shell is to answer from its own table for the GPU, and the engine supplies the reference to check it
against. The shell's adapter entry, table and check are not implemented yet; the reference is.
- **The reference.** `IBc250DxvkDevice3::CheckFeatureSupportAtLevel` answers as `CheckFeatureSupport` would
  on an engine device created at another level. The device's own answers are not enough, because DXVK
  gates several of them on its level: doubles and typed UAV loads from 11_0, ROVs and the stencil
  reference from 11_1. A device created at 10_0 would contradict a correct table.
- **The check.** Inside `CreateDevice`, the shell compares its table with the answers at `MaxFeatureLevel`,
  whatever level the runtime chose for the device.
- **What the shell does not copy.** Some of DXVK's answers do not map to the caps types of the D3D11.1 DDI
  interface, the only one the shell lists:
  - tiled resources, conservative rasterization, viewport and render target array index from any stage,
    shared resource tier;
  - threading, which follows E2.
- **Record.** The engine test prints the record at `MaxFeatureLevel`. A unit A run of the frozen test gives
  the values for GFX1013.

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
| Stream output without a geometry program on adjacency and patch topologies | Points, lines and triangles, lists and strips, are closed (see Shaders from DDI token streams). The pass-through declares point input until linking and cannot drop adjacent vertices, so adjacency is not supported; patches need a domain program. | Implement if a runtime path needs it. |
| Predication on the GPU | Predicates are honoured by waiting on the CPU when a result is not yet available (see Predication). | Measure the wait on unit A with a title that predicates; move to `VK_EXT_conditional_rendering` only if it matters. |
| ClearView on buffer render target views | Closed in engine commit dd35ce7c. Upstream DXVK logged an error and cleared nothing. A typed buffer view is cleared in the buffer; otherwise the 1D proxy image is refreshed from the buffer, cleared and copied back. The engine test covers both paths. | None. |
| Optimized pipelines in inline mode | Closed offline in engine commit 4c5fd822. With graphics pipeline libraries, DXVK draws with a fast-linked pipeline and compiles the optimized variant on a pipeline worker. Inline mode used to run that compile on the drawing thread, which then paid for both. The compile is now queued, and `SubmitForPresent` runs the queue after the frame's submission. It starts compiles for up to `dxvk.inlinePipelineBudget` microseconds (default 2000), so one long compile can exceed the budget. A budget of 0 keeps the fast-linked pipelines. The engine test classifies every graphics pipeline the engine creates. The previous engine fails its check, because its draw compiled the optimized pipeline itself. | Measure on unit A with a real title: warm-up frame times, and how long the queue takes to drain at the default budget. A client that never presents keeps fast-linked pipelines. |
| Inline execution on CPU-bound work | Measured offline (engine commit ea512f65, development PC, three runs each): `bc250dxvk_engine_test --bench` issues 1000 draws per frame, each with a constant buffer DISCARD and a texture switch, at a frame latency of 3. Inline mode took 346-357 us wall per frame. With DXVK's worker threads it took 184-233 us (switch `BC250DXVK_MEASURE_WORKER_THREADS=1`, which breaks E2 and exists only for this comparison). So the application thread pays roughly 1.5-1.9 times as much per draw inline. A GPU-bound title does not notice; a CPU-bound one can miss the 5 % bound by far. | Two steps. First, the ADR 0017 item 8 measurement on unit A: do the runtime callbacks that hosted RADV uses (allocation, residency, submission) work from a thread that is not inside a DDI entry? If they do, serialize them per device and allow DXVK's workers. If they do not, add a broker mode (a later ABI minor): workers queue the runtime-reaching work, and DDI entries and engine waits service the queue. |
| On-disk shader cache | Per-application DXVK keeps the dxbc-spirv results of each executable in `%LOCALAPPDATA%\dxvk` (or `DXVK_SHADER_CACHE_PATH`); its writer thread does file I/O only. The engine turns the cache off, to keep a writer thread and cache files out of every process that loads the system driver, DWM included. Every process start therefore translates its shaders again. | Decide with the load-time part of the 5 % comparison; the cache needs no Vulkan call, so E2 does not forbid it. The d3d11bench protocol turns the cache off on both paths for the bound, and prices the decision with a separate per-application series that keeps it. |
| `Blt` and `Blt1` with ROTATE90/270 | `E_NOTIMPL`. Not reachable: the runtime asks for a rotation only from a driver that can return `DXGI_DDI_ERR_UNSUPPORTED` when it creates a primary, and the shell never does (dxgiddi `BltDXGI` and `Blt1DXGI` remarks). ROTATE180 is implemented anyway. | Implement if the shell ever refuses a primary. The docs define `Rotate` as a counter-clockwise turn of the source. |
| `Blt` and `Blt1` into a multisampled destination | `E_NOTIMPL` | Implement when a runtime path needs it. |
| Rendering into LINEAR runtime surfaces | The shell allocates back buffers and other runtime surfaces LINEAR, so a title draws straight into LINEAR images. Per-application DXVK draws into its own OPTIMAL back buffer and copies it once per frame to the presentable image. `bc250dxvk_engine_test --bench-tiling` times both on 1920x1080 RGBA8. On the development PC (RTX 4090, not unit A), clearing and 8 blended full-screen draws took 0.044 ms per frame into OPTIMAL and 0.119-0.133 ms into LINEAR (two runs). The OPTIMAL to LINEAR copy took 0.007 ms, and a 1:1 read took the same time from either tiling. | Run `--bench-tiling` on unit A. If drawing into LINEAR costs more than the copy there, the engine draws back buffers into an OPTIMAL image and copies it into the runtime surface at present, as per-application DXVK does. |
| 5 % bound against per-application DXVK | Not measured. The workload and the comparison exist: `tools/win/d3d11bench` (draws, fill and shader-creation scenes; `compare.py` gates each scene on the bound plus the run-to-run spread and on equal output checksums). Its README has the protocol. | Needs the shell's positive run through the system runtime. The per-application side is the `per-app` recipe in build.md, built from the same DXVK revision as the engine. |

## Validation

`bc250dxvk_engine_test.exe` is the offline positive control ([build.md](../build.md#dxvk)). It plays the shell:
it owns instance and device, feeds DDI-form shaders and runtime-style images, and checks the following:
- pixels;
- 500 sustained frames under a memory bound;
- storage rotation, Blt and Blt1 source rectangles;
- a LINEAR shell image: `CreateTexture2DFromImage2` checks, the tiling the engine uses, drawing and Blt1;
- ClearView on buffer render target views;
- stream output of points, lines and triangles, and a rasterized stream;
- stream output on a second stream of an fxc gs_5_0 program that reuses a stream 0 register, with the Stream 0
  signatures the shell passes, against the same program created through the D3D11 API, the path that
  per-application DXVK runs;
- occlusion and stream-output overflow predication;
- feature answers at other levels: at the device's own level they equal the device's, byte for byte;
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
