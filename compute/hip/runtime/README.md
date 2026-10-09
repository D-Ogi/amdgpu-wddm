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

38 exported names, which [`amdhip64.def`](amdhip64.def) lists. A plain HIP vector addition
links against exactly that set.

| File | What it holds |
|---|---|
| `hip_module.cpp` | the registration interface, the fat binary wrapper, the lazy module load |
| `hip_launch.cpp` | the call configuration, `hipLaunchKernel`, the kernel argument pool |
| `hip_memory.cpp` | the allocation table and the memory entry points |
| `hip_stream.cpp` | software streams over one hardware queue |
| `hip_event.cpp` | events over the stream fence values |
| `hip_device.cpp` | the process state, the lazy device, the device properties |
| `hip_error.cpp` | the per-thread error state and the status translation |
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

1. `amdhip64.def` holds exactly 38 names, and every one of them is declared in
   `hip_runtime.h`.
2. Every undefined symbol of the runtime objects that belongs to our own stack is declared in
   `bc250hsa.h`. A new call into layer 1 that the contract does not carry is a build failure.
3. The built DLL exports exactly the names of `amdhip64.def`, no more and no fewer.
4. `test_hip_mock.exe` passes: registration, argument packing, stream order across an event
   wait, event timing, no allocation leak on a second run, the fixed properties of the design,
   the memory entry points with an explicit kind and with `hipMemcpyDefault`, the null stream,
   the per-thread error state, and a second fat binary in one process.
5. A real HIP program, compiled by clang against `hip_runtime.h` and linked against
   `amdhip64.lib`, imports `amdhip64.dll`, runs against the mock build, and records the
   dispatches that its two `<<<>>>` calls asked for, with the measured grid, block and kernel
   argument size. Every wait of that run carries the bound that `--wait-total` gave it.
6. With `-Rebuild`: the code object fixture of the host test, built again from its own source,
   and the host test run against the result. The fixture is not reproducible byte for byte,
   because clang writes a unique `__hip_cuid_*` symbol into every compilation
   (`tests/data/PROVENANCE-runtime.txt`).

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
the three worker threads finished 63 operations inside a 204 ms wait of thread 0, and four
threads of four waits each took 840 ms where a serialised runtime takes 3240 ms. The control
build measured 0 operations inside the wait and 3044 ms for the same work.

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
- `__hipRegisterManagedVar` reports a missing capability. Managed memory needs page migration.
- A kernel that asks for a host call buffer (device-side `printf`) is refused by name. The
  counter of layer 1 answers kill criterion K4 of the route document.
- Step 3 adds 14 more names for llama.cpp, five of them honest stubs, and the device-side math,
  atomic and shuffle sets in the header.
