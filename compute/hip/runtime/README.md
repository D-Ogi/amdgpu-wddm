# amdhip64.dll: layer 2 of the BC-250 HIP runtime

Milestone M16, gate G5, route B. The design is [`docs/design/m16-hip-route-b.md`](../../../docs/design/m16-hip-route-b.md),
sections 4 and 5. This directory holds the HIP application binary interface: registration,
launch, memory, streams, events, errors and device properties. It calls
[`compute/hip/include/bc250hsa.h`](../include/bc250hsa.h) and nothing else of the submission
path, so it builds and its tests run on a machine with no BC-250 adapter.

## What it is

An application compiled by clang for HIP asks for `amdhip64.dll`. Clang's module constructor
calls `__hipRegisterFatBinary` and `__hipRegisterFunction` before `main()`, and the
`kernel<<<grid, block, shared, stream>>>` syntax becomes `__hipPushCallConfiguration` and a
call of `hipLaunchKernel`. This DLL answers those calls. No AMD user-mode component is
involved, and no part of this work uses PAL (owner decision D015).

55 exported names, which [`amdhip64.def`](amdhip64.def) lists: the 38 of step 2, against which a
plain HIP vector addition links, the 15 that llama.cpp's ggml-hip backend adds in step 3 (design
section 4.9), and two of ours that are not HIP (`bc250hipGetCounters` and
`bc250hipResetCounters`, see Measurement below).

| File | What it holds |
|---|---|
| `hip_module.cpp` | the registration interface, the fat binary wrapper, the lazy module load |
| `hip_launch.cpp` | the call configuration, `hipLaunchKernel`, the kernel argument pool |
| `hip_memory.cpp` | the allocation table and the memory entry points |
| `hip_stream.cpp` | software streams over one hardware queue |
| `hip_event.cpp` | events over the stream fence values |
| `hip_device.cpp` | the process state, the lazy device, the device properties |
| `hip_error.cpp` | the per-thread error state and the status translation |
| `hip_perf.cpp` | the two counter calls, which are ours and not HIP |
| `dllmain.cpp` | the entry point, which does nothing on purpose |

## How to build it

```
pwsh compute\hip\build-runtime.ps1
pwsh compute\hip\build-runtime.ps1 -Bc250hsaLib <path of bc250hsa.lib>
```

The product code is built with MSVC from the installed Visual Studio toolset, with the headers
and import libraries of the SDK NuGet packages under `-Kits`. The portable AMDGPU clang builds
the device-side artifacts only: the import library through `llvm-dlltool`, and the HIP sample.
The script says so and goes on when that clang is absent. Nothing is installed and nothing is
written to drive C:.

`-Bc250hsaLib` names the static library of layer 1 (branch `m16/hip-dispatch`). Without it the
script builds everything else, including the mock build of the same DLL, and says that the
product DLL needs that library.

## What the build gates

1. `amdhip64.def` holds exactly 55 names, and every one of them is declared in
   `hip_runtime.h`.
2. Every undefined symbol of the runtime objects that belongs to our own stack is declared in
   `bc250hsa.h`. A new call into layer 1 that the contract does not carry is a build failure.
3. The built DLL exports exactly the names of `amdhip64.def`, no more and no fewer.
4. `test_hip_mock.exe` passes: registration, argument packing, stream order across an event
   wait, event timing, no allocation leak on a second run, the fixed properties of the design,
   the memory entry points with an explicit kind and with `hipMemcpyDefault`, the null stream,
   the per-thread error state, a second fat binary in one process, and the 15 step-3 entry
   points: 232 checks in all, measured by this build.
5. A real HIP program, compiled by clang against `hip_runtime.h` and linked against
   `amdhip64.lib`, imports `amdhip64.dll`, runs against the mock build, and records the
   dispatches that its two `<<<>>>` calls asked for, with the measured grid, block and kernel
   argument size. Every wait of that run carries the bound that `--wait-total` gave it.
6. With `-Rebuild`: the code object fixture of the host test, built again from its own source,
   and the host test run against the result. The fixture is not reproducible byte for byte,
   because clang writes a unique `__hip_cuid_*` symbol into every compilation
   (`tests/data/PROVENANCE-runtime.txt`).
7. `test_hip_batch.exe` passes, and `test_hip_mock.exe` and `test_hip_threads.exe` pass a
   second time with `BC250_HIP_BATCH=1` and `BC250_HIP_BARRIER=light`. Those are the defaults
   of this build, and the second pass stays because it names them.
8. `hipbench.exe`, against the mock DLL, measures under 0.1 submissions per launch with no
   environment at all (the default batches) and exactly 1.000 with `--batch 0 --barrier full`,
   which is build 1. The number comes from the counters of the DLL itself, so this also gates
   the two counter calls, and the second arm gates the switches.

## Batching, and the switches

Section 8 of the design is the off-GPU cost of a launch, and the mechanism is in layer 1
(`compute/hip/README.md` has the shape of it). This runtime needed two insertions for it:
`hip_device.cpp` reads five environment variables once, at its first call, and gives layer 1 a
policy. `hipEventRecord` submits an open buffer before it stamps the event, so an event covers
the work the stream had asked for. Every other path is right without a change, because
`bc250hsa_wait` submits an open buffer itself when a caller asks for a value the device has not
been given.

| Variable | Values | Default |
|---|---|---|
| `BC250_HIP_BATCH` | `0`, `1` | `1`, several launches in one indirect buffer |
| `BC250_HIP_BATCH_MAX` | 1 to 256 dispatches per buffer | 32 |
| `BC250_HIP_BATCH_HOLD_US` | microseconds a buffer may hold a dispatch | 1000 |
| `BC250_HIP_BARRIER` | `full`, `light` | `light` |
| `BC250_HIP_PM4_STATE_CACHE` | `0`, `1` | `1`, write only the state that changed |
| `BC250_HIP_GPU_COPY` | `0`, `1` | `1`, the copy engine moves device memory to the host |
| `BC250_HIP_GPU_COPY_MIN` | bytes, digits only, up to 4294967295. 0 puts every copy on the GPU route. Any other text keeps the default and says so | 4096, below which the host processor is cheaper |
| `BC250_HIP_GPU_FILL` | `0`, `1` | `0`, `hipMemset` of a device allocation stays a CPU write |

The last three are the device-to-host copy route. A `hipMalloc` buffer is VRAM behind a mapping
that is not cached, so a CPU read of it runs at 0.21 GB/s against 15.6 GB/s the other way, and the
logits of one generated token are a 1 MiB read of exactly that kind. With the route on, a
`DMA_DATA` packet moves those bytes instead, either straight into the destination when the
application asked `hipHostMalloc` for it or through a staging ring when it did not.
`BC250_HIP_GPU_COPY=0` is the control arm, and the threshold exists because one submission and one
fence cost about 14.2 us, which is more than the host processor needs for a page.

`BC250_HIP_GPU_FILL` is off by default for a reason of the same kind: a CPU write of that mapping is
15.6 GB/s, so sending a fill of a mapped allocation to the copy engine could make it slower. A
`hipMemset` of an allocation with no host mapping always takes the engine, because nothing else can
reach it. On this part `hipMalloc` makes no such allocation, because it falls back to host-visible
memory when the mapping fails, so the switch is the only way the GPU fill runs here. The switch
covers device allocations. A `hipMemset` of a `hipHostMalloc` pointer stays a host-processor write,
because the program holds that pointer and reads the memory itself.

A copy or fill that the copy engine has accepted is never done again by the host processor.
Layer 1 reports how far each call got (`bc250hsa_op_receipt`, `bc250hsa.h` section 5.1): nothing
submitted, submitted with a fence value, or retired. Only a refusal before the submission, on a
device that is not lost, falls back to the host processor. A copy that was submitted and then did
not finish, timed out or lost the device fails the HIP call, because the engine may still write
the destination. The startup coherence control is required by the destination allocation and not
by the kind the caller names: a device-to-device copy into memory the host processor can read,
such as the device address of a `hipHostMalloc` buffer, is gated as a device-to-host copy is. On
this part every `hipMalloc` buffer has a host mapping, so until the control passes those copies
also take the host processor.

The first two defaults were the conservative value until the lab said otherwise. It did, on
2026-10-09: `evidence/m16/perf-2026-10-09` and facts M853 to M857. Batching with the light
barrier is exact over two chains of 1000 dependent kernels, 2.2 times faster on the launch line
and 3.8 times faster on the chain line, and 0.032 submissions per dispatch against 1.000. One
line is slower, a 4 KB device-to-host copy by about 9.5 us, which the second launch after it
repays. Section 8.6a of the design has the whole of that and the arm that will settle its cause.

`BC250_HIP_BATCH=0` with `BC250_HIP_BARRIER=full` is exactly build 1. A value the backend
refuses gets one line on the standard error stream and the device's own default, which is one
submission per launch, never a failed `hipInit`.

`BC250_HIP_PM4_STATE_CACHE=0` is the other control arm. By default a launch that follows another
one in the same indirect buffer writes only the compute state that changed, 23 dwords instead of
72 (design section 8.8). This variable makes every launch write the whole state, as build 1 did.

## The log, and why it exists

| Variable | Values | Default |
|---|---|---|
| `BC250_HIP_LOG` | `0` or absent, `1`, `stderr`, or a file path | off |
| `BC250_HIP_LOG_LEVEL` | `0` error, `1` warning, `2` information, `3` trace | `2` |

With it on, every refusal of this runtime names the call, the kernel and the reason, and every
`bc250hsa_log` line of layer 1 arrives on the same stream. The sink goes in where the device
opens, before the first call into layer 1, and not at the first refusal of this runtime: a sink
installed at the first refusal would miss the `bc250hsa_log` line that the same refusal had
already written, which is the one line that states which of the rules of `pm4_dispatch.c` spoke.
A file path is appended to, so several runs of one trial keep their order, and a path that cannot
be opened gets one line on the error stream instead of silence. Each line carries the process and
the thread identifier. The log holds call names, kernel names and status names, and no
application data.

The reason it exists is a lab session. On 2026-10-09 all three llama.cpp arms of M16 step 3B died
at `CUDA_CHECK(cudaGetLastError())` right after a `<<<>>>` call, with our own text for
`hipErrorNotSupported` and nothing else: layer 1 wrote its reason through `bc250hsa_log`, this
runtime installed no sink, and the refusal counters of layer 1 are not among the two counter
exports. The session had to end with "the next step is a build whose refusals say which call and
which kernel they refuse" (defect BD-110). With the switch the same run says, in one line:

```
amdhip64 [78528:109128] error hipLaunchKernel/dispatch_submit refuses kernel
  _ZL12rms_norm_f32ILi1024ELb1ELb0ELb0E...: not supported by this build (hipErrorNotSupported)
```

`hipLaunchKernel` reports through it at six points (the host stub, the code object load, the
kernel lookup, the kernel argument requirements, the packing and the submission), and so do
`hipHostRegister`, `hipMallocManaged`, `hipMemAdvise`, `hipStreamBeginCapture`,
`hipLaunchCooperativeKernel`, `hipFuncSetAttribute`, `__hipRegisterManagedVar`, the
device-to-device copy and the fill of an allocation with no host mapping.

## The AQL dispatch packet

A kernel that reads its own `blockDim` enables `ENABLE_SGPR_DISPATCH_PTR`:
`__builtin_amdgcn_workgroup_size_x`, which is what `blockDim.x` becomes, is a 16-bit load from
the AQL kernel dispatch packet and not an implicit kernel argument. A PM4 dispatch has no packet,
so layer 1 writes one at the end of the same kernel argument buffer this runtime takes from its
pool, and `hipLaunchKernel` passes its address in `bc250hsa_dispatch` (section 7.1 of
`bc250hsa.h`). One allocation, one lifetime: the packet retires with the dispatch that reads it.

MEASURED on the built `ggml-hip.dll` of llama.cpp: 1752 of its 7105 gfx1013 kernels enable the
bit, among them every `k_get_rows`, every `soft_max_f32` and half of the `k_bin_bcast` and
`mul_mat_q` sets. Without the packet layer 1 refused all 1752 by name, which is defect BD-110 and
the reason no model ran on 2026-10-09. The negative control is a build of this DLL with
`BC250_HIP_NO_DISPATCH_PACKET=1`, which `build-runtime.ps1` writes to
`mock-no-dispatch-packet\amdhip64.dll` and nothing else defines.

## Measurement

`bc250hipGetCounters` and `bc250hipResetCounters` are not HIP. They report what the submission
layer did, and above all how many times one kernel launch entered the kernel driver, which no
HIP entry point answers. `samples/hipbench.hip` is their named user: launch rate, a dependent
chain in the shape of a decode step, host copies at 4 KB, 64 KB, 1 MiB and 64 MiB, the cost of
synchronising a retired stream, an event round, and submissions per launch beside each one.
Before it times anything, `hipbench` checks the copy routes byte for byte (`--verify 1`, the
default): seven cases with a position-dependent source, a dirty destination and dirty bytes
around the range, among them the staging ring over more than four rounds, the 64 MiB copy whose
last 32 bytes ride in a second packet, unaligned offsets, device to device, and a pitched copy.
A destination that `hipHostMalloc` could not give is SKIP, which accepts nothing.
`--verify-fault omit-tail` and `--verify-fault corrupt-tail` are its positive control: every case
must then be WRONG and the exit status 1, and `build-runtime.ps1` runs both on the mock.
The mock DLL proves the harness. The numbers that mean anything come from the lab arms in
`scratch/m16-hip/lab/perf-README.md`, which is local.

The counters also carry durations, in ticks of `QueryPerformanceCounter` with its frequency
beside them: the time inside the waits (those the fence mapping answered and all others, each
with its longest single wait), the time inside `D3DKMTSubmitCommand`, and the time of each of
the five copy routes, from the entry of one copy to its return. `hipbench` prints them on a
`time:` line under each measurement. `bc250hsa.h` section 3 says what each total covers.

`bc250hipCounters` only grows at its end. A caller sets `struct_bytes` to the size of the
layout it was built with, 56, 160 or 256 bytes, and gets exactly the fields of that layout.
Nothing past them is written, and every other size is refused with `hipErrorInvalidValue`. A
program built against an older header, such as the BD-117 profile build of `llama-bench`, keeps
working.

## Threads

One lock holds the process state, and no thread holds it while it waits for the device. A wait
has three parts: read the fence value to wait for with the lock held, wait with the lock open,
take the lock again and update the state. `bc250hip::Guard` is that lock and the only way to
wait. Everything a call still needs after its wait is either held with a reference count (a
stream through `StreamRef`, an event through `EventRef`) or looked up again (the kernel of a host
stub, an allocation of the table), because another thread owns the state while the wait runs.
`hipStreamDestroy` and `hipEventDestroy` therefore take the handle's reference away and never
free an object that a waiting thread still holds.

Submissions stay serialised, because layer 1 has one hardware queue and its own device lock
(`bc250hsa.h`, rule 5). The rule above is about the waits, which are the long part.

A copy or a fill on the copy engine follows the same rule, although its wait is inside layer 1.
The call pins the allocations it names, calls layer 1 with the lock open, and records the fence
value of the work when it takes the lock again. A `hipFree` of a pinned allocation, or of one
whose last copy has not retired, waits for that copy and its fence value first, so no memory is
freed under the engine. Layer 1 still has its own device lock, and every GPU copy holds it until
its work retired, a staged copy across all of its rounds (`bc250hsa.h` section 5.1). Another
thread's call into layer 1 on the same device waits for that copy. A launch or an event record
makes that call with the lock of this runtime held, so a third thread's call can then wait too. A
call that reads only the state of this runtime does not wait for the copy itself. A process that
calls the runtime from one thread never waits for this lock. Layer 1 counts every such wait in
`gpu_copy_lock_waits` and `gpu_copy_lock_wait_ticks`, and its `fence check: copy lock` lines say
them (`BC250_HIP_LOG=1`). A copy whose own wait fails keeps the device after the call returned:
until layer 1 sees it retire, a later launch or copy first waits for it under the wait bound and
fails with that wait's error if it still has not retired (`hipErrorLaunchTimeOut` for a launch
that timed out). Its `copy exclusion:` lines say which copy and which call. Until then no copy or
fill of this runtime moves a byte with the host processor either, on its configured route or as
the fallback after layer 1 refused the GPU route: such a copy could read the destination before
the late bytes land, or write over the source the device still reads. The call fails with the
copy's error and moves nothing. `cpu_copy_exclusion_refusals` counts these refusals, and a `copy
exclusion: refused ... on the host processor` line says the 1st, 2nd, 4th and later powers of
two. Once the fence reaches the copy, every route is as before. On a device that layer 1 marked
lost, every host-processor copy and fill is refused the same way, whatever the fence reads and
whether a copy is unretired or not, and fails with the loss. The program's own reads and writes
of the copy's ranges are outside the runtime: after a copy fails, the program must not touch its
ranges until a later `hipDeviceSynchronize` succeeds. A call that is refused or fails cancels no
asynchronous operation that is still outstanding: the failed copy and any other work the device
accepted may still complete. The error a call gets (`hipErrorLaunchTimeOut` for a wait that ran
out of its bound, `hipErrorUnknown` for a failed Windows call), the bounded wait for a copy and
its late retirement are the policy of this runtime, not a guarantee of portable HIP. This runtime
exports no three-dimensional copy and no two- or three-dimensional fill (`hipMemcpy3D`,
`hipMemset2D`, `hipMemset3D` and their asynchronous forms), so none of them is supported or
tested here. `test_hip_mock.exe` section 15 checks every host-processor path that is exported
against such a copy and against a lost device.
`test_hip_threads.exe` checks both halves, for the direct and the staged route: a query on
another thread finishes while a copy waits, and a `hipFree` raced against a copy that has not yet
submitted returns only after the copy.

The owner asks for multithreading to be measured with our own clients (2026-09-29).
[`tests/host/hip_threads_client.h`](../tests/host/hip_threads_client.h) is that client: one
thread waits for the device while the others launch and allocate, and it counts the work they
finish inside that wait. Two programs run it, and both are built and measured by
`build-runtime.ps1`:

| Program | Where |
|---|---|
| `test_hip_threads.exe` | the host test, against the mock backend. It also reads the live stream and event counts of the runtime, so a reference count that leaked or freed twice is visible |
| `test_hip_threads_control.exe` | the same test over a runtime compiled with `BC250_HIP_WAIT_UNDER_LOCK=1`. It is the negative control. With the lock held over the wait, no other thread may finish anything. The test also refuses to call itself a control unless the runtime it links really waits that way |
| `hipthreads.exe` | a real HIP program that clang compiles (`samples/threads.hip`), for the mock DLL here and the product DLL on the lab |

MEASURED on the development PC, 2026-10-09, with the mock backend holding each dispatch 200 ms:
the three worker threads finished 48 operations inside a 199 ms wait of thread 0, which is their
whole cap of 16 each, and four threads of four waits each took 812 ms where a serialised runtime
takes 3239 ms. The control build measured 0 operations inside the wait, and 2838 ms for the same
work.

## The mock build

`compute/hip/tests/host/hipmock_backend.c` implements `bc250hsa.h` over host memory. A dispatch
is recorded and the fence retires at once, so the mock runs no instruction: a HIP program
against the mock build gets `hipSuccess` from every call and zeros in its output buffer.
That is what makes a registration and packing test possible before layer 1 exists, and before
any lab trial.

The mock reads the code object for real, because the packing test needs the argument offsets of
the metadata. It is not the authority: the loader, the packer and the PM4 builder of layer 1
have their own tests against the same code objects.

## What this build does not do yet

- An asynchronous copy is synchronous. `hipMemcpyAsync` waits for the stream and copies at
  once, which is legal and slow.
- `hipMemset` fills through the host mapping. With `BC250_HIP_GPU_FILL=1`, or for an allocation
  with no host mapping, the copy engine fills it. A fill kernel is later work.
- A device-to-device copy takes the copy engine. With `BC250_HIP_GPU_COPY=0` it needs a host
  mapping on both sides.
- `hipEventElapsedTime` is the difference of two host timestamps, each taken at the moment its
  fence value retired: `hipEventRecord` stamps an event whose value has already retired, and
  every later wait stamps the events the device has passed. A GPU timestamp through a second
  `RELEASE_MEM` is better and needs one cost measurement.
- `hipMemcpyDefault` and `hipMemset` have to guess whether a pointer is device memory, because a
  GPU virtual address and a host pointer are numbers of the same size. A host mapping of this
  process wins over a numerically equal device address, and an explicit `hipMemcpyKind` is the
  caller's word, which the runtime does not argue with. When the GPU address window of layer 1
  overlaps the address space of the process, the runtime says so one time on the error stream.
- An asynchronous copy is still a synchronous one, so a copy on one stream waits for that
  stream's own work even when another stream could carry it.
- One wait can still happen with the process lock held, and it is not ours to move: a submission
  whose command ring is full waits for the oldest slot inside layer 1. It needs eight dispatches
  of one process in flight, and the fix belongs to layer 1 (design section 4.7).
- `__hipRegisterManagedVar` reports a missing capability. Managed memory needs page migration.
- A kernel that asks for a host call buffer (device-side `printf`) is refused by name. The
  counter of layer 1 answers kill criterion K4 of the route document.
- The device-side header set that llama.cpp's kernels need is not written. Design section 4.10
  measures it: about 100 names, which are the half and bfloat16 types, the vector types, the
  cross-lane functions, the atomics and the cached loads. The mathematics of those kernels is no
  longer work, because clang's own HIP math headers and the device library of section 4.6 carry
  all 48 names a probe asked for.
- There is no hipBLAS. The backend needs 11 entry points of `hipblas` and `rocblas`, and its
  CMake file requires all three packages. A shim over our own matrix multiply kernels is the
  open work, and `GGML_CUDA_FORCE_MMQ=ON` keeps the quantised multiplies out of it.
