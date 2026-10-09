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
`bc250hsa_log` line of layer 1 arrives on the same stream. A file path is appended to, so several
runs of one trial keep their order, and a path that cannot be opened gets one line on the error
stream instead of silence. Each line carries the process and the thread identifier. The log holds
call names, kernel names and status names, and no application data.

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
chain in the shape of a decode step, host copies at 4 KB, 1 MiB and 64 MiB, the cost of
synchronising a retired stream, an event round, and submissions per launch beside each one. The
The mock DLL proves the harness. The numbers that mean anything come from the lab arms in
`scratch/m16-hip/lab/perf-README.md`, which is local.

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
- `hipMemset` fills through the host mapping. A fill kernel and the copy engine are later work.
- A device-to-device copy needs a host mapping on both sides.
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
