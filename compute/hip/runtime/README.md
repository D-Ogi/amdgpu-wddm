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
   wait, no allocation leak on a second run, and the fixed properties of the design.
5. A real HIP program, compiled by clang against `hip_runtime.h` and linked against
   `amdhip64.lib`, imports `amdhip64.dll`, runs against the mock build, and records the
   dispatches that its two `<<<>>>` calls asked for, with the measured grid, block and kernel
   argument size.

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
- `hipEventElapsedTime` is the difference of two host timestamps, each taken when its fence
  value retired. A GPU timestamp through a second `RELEASE_MEM` is better and needs one cost
  measurement.
- `__hipRegisterManagedVar` reports a missing capability. Managed memory needs page migration.
- A kernel that asks for a host call buffer (device-side `printf`) is refused by name. The
  counter of layer 1 answers kill criterion K4 of the route document.
- Step 3 adds 14 more names for llama.cpp, five of them honest stubs, and the device-side math,
  atomic and shuffle sets in the header.
