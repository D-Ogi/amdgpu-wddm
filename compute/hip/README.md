# compute/hip - HIP for gfx1013 on this driver

Milestone M16, gate G5, route B of `docs/design/m16-hip-route-b.md`: our own thin HIP runtime on our
own kernel driver. No ROCm runtime, no AMD user-mode component, and no PAL.

The work has two layers. **This directory holds layer 1**, which is the submission layer and the
step-1 tool. Layer 2 (`runtime/`, `amdhip64.dll`) uses layer 1 and nothing else of the submission
path, and comes on its own branch.

## What is here

| Path | What it is |
|---|---|
| `include/bc250hsa.h` | the frozen interface of layer 1. Section 3.9 of the design holds the same text, and `build.ps1 -CheckDoc` compares them byte for byte |
| `include/hip/hip_runtime.h` | the minimal HIP header of section 4.4. Layer 2 owns it, on branch `m16/hip-runtime`, and layer 1 does not include it |
| `include/hip/hip_version.h`, `hip_vector_types.h`, `hip_fp16.h`, `hip_bf16.h`, `hip_cooperative_groups.h` | the device-side header set that llama.cpp's `ggml-hip` backend includes. Section 4.12 of the design says what each one answers and which failed build asked for it |
| `include/hipblas/hipblas.h` | the ten BLAS entry points the backend links against. The route and the missing implementation are in `compute/hipblas/README.md` |
| `cmake/` | our own `find_package` answer for `hip`, `hipblas` and `rocblas`, with `hip::host` and `hip::device` |
| `tools/make-rocm-root.py` | assembles the include tree, the CMake packages, the import library and the device library bitcode into one ROCm-shaped root, which `--rocm-path` and `CMAKE_PREFIX_PATH` then name |
| `bc250hsa/` | the library. The table below gives its file-by-file shape |
| `tests/host/` | five tests that need no GPU and no BC-250 adapter |
| `tests/data/` | the committed code object, fat binary, metadata dumps and the golden PM4 stream, with `PROVENANCE.txt` |
| `tools/hipprobe.c` | the step-1 tool: three kernels, every result checked on the processor, one JSON line each |
| `tools/run-lab.ps1` | the lab side of the step-1 trial: the six bounded runs of section 6.2, one record, the seven pass criteria |
| `build.ps1` | builds the library, the tests and the tool, and runs every gate that runs here |

## The shape of the library

```
bc250hsa.h                 the only header a caller needs
  status.c                 status names, the log hook, fourteen process counters
  co_msgpack.c             a MessagePack reader that refuses what it does not know
  co_metadata.c            NT_AMDGPU_METADATA to bc250hsa_kernel; the 64-byte descriptor
  co_loader.c              the clang offload bundle, then the ELF code object
  kernarg.c                the kernel argument buffer, from the metadata list only
  pm4_dispatch.c           the 19 packets of one gfx1013 compute dispatch, and the batch of several
  kmt_device.c             the adapter, the device, the node-0 context, the fence
  kmt_memory.c             allocate, map a GPU address, make resident, wait for the paging fence
  submit.c                 the command ring, D3DKMTSubmitCommand, the batch, the sliced bounded wait
```

The first six files touch no operating system call, so every host test links them and runs anywhere.
The last three are the Windows half. The split is why `tests/host/` needs no adapter.

Three rules the library keeps, because each one is a measured trap:

1. **The packer never computes an offset.** It walks the `.args` list of the metadata. In a measured
   kernel two hidden fields meet at one byte boundary, and a kernel gets an implicit argument block
   only when it reads the implicit argument pointer, so no fixed structure can describe it.
2. **The host writes `COMPUTE_PGM_RSRC2.LDS_SIZE`.** The command processor normally takes that field
   from the AQL packet. A PM4 dispatch has no AQL packet, and the kernel descriptor holds 0 even for
   a kernel with 1024 bytes of local memory (measured on `reduce256`).
3. **No wait is unbounded, and no wait is one long sleep.** A wait runs in slices and asks the
   operating system between two of them whether the device still runs. One long wait turned a healthy
   14.6-second wait behind another process's engine reset into a lost device (defect K225).

## Batching, and the barrier between dispatches

Build 1 sent one kernel dispatch as one indirect buffer and one `D3DKMTSubmitCommand`. Right, and
expensive: a kernel that runs for two microseconds paid for a call into the kernel driver, a ring
slot, a fence and a completion write. Section 8 of the design is the answer, and section 8.1 of
`bc250hsa.h` is its interface: consecutive dispatches go into one larger indirect buffer, and one
submission carries them all.

The whole mechanism is here in layer 1, because layer 1 owns the wait. `bc250hsa_wait` submits an
open buffer as soon as a caller asks for a fence value that was promised but not yet sent to the
device, so a program cannot look at work that is still waiting to be submitted. The other flush
points are the map, the unmap, the copy, the free, the code object load and the device closing: in
each one the caller is about to read bytes or addresses that open work owns.

| Knob | What it does | Default |
|---|---|---|
| `bc250hsa_batch_policy.enabled` | one buffer per batch instead of one per dispatch | 0, which is build 1 |
| `max_dispatches` | the dispatch cap of one buffer, at most 256 | 32 |
| `max_ib_dwords` | the dword cap | one ring slot, less the completion write |
| `max_hold_us` | how long an open buffer may hold a dispatch | 1000 |
| `light_barrier` | the level-0 and level-1 invalidate between two dispatches of one buffer, in place of the full acquire | 0 |

The library's own defaults stay the conservative ones, because the library has no policy of its
own (rule 6 of the header): a caller that says nothing gets the behaviour of build 1. The policy
of the product comes from `runtime/`, which reads `BC250_HIP_BATCH`, `BC250_HIP_BATCH_MAX`,
`BC250_HIP_BATCH_HOLD_US`, `BC250_HIP_BARRIER` and `BC250_HIP_PM4_STATE_CACHE` and sets it. Since
the lab session of 2026-10-09 (`evidence/m16/perf-2026-10-09`) those defaults are batching on, a
cap of 32 and the light barrier.

Inside one buffer, a dispatch writes only the compute state that differs from the one before it:
72 dwords a dispatch become 23 for the shape a real launch has. The first dispatch of every buffer
is complete, because another context's buffer runs between two of ours, and
`BC250HSA_DISPATCH_FULL_STATE` turns the whole mechanism off for a comparison. Design section 8.8
holds the rules. `test_pm4` section 2c states the dwords of each case and carries the negative
control that a field which did change is written.

`BC250HSA_ACQUIRE_GCR_CNTL_LIGHT` in `bc250hsa/pm4_regs.h` names the bits of the light barrier and
the Mesa file and line each one comes from, with the bits it drops stated: `GL2_INV`, `GL2_WB`,
`GLM_INV`, `GLM_WB`, `GLI_INV`. The instruction invalidate stays at the head of the buffer, because
a code object load is a flush point. `test_pm4` checks the dwords of both values, and the dropped
bits are its negative control.

The off-GPU cost itself is measured by `samples/hipbench.hip`, which reads the submission counters
through the two vendor calls of layer 2 and prints submissions per dispatch beside its timings. The
lab arms are in `scratch/m16-hip/lab/perf-README.md`, which is local.

## Build

```
pwsh compute\hip\build.ps1
pwsh compute\hip\build.ps1 -CheckDoc      # also compares the header with the design document
pwsh compute\hip\build.ps1 -Rebuild       # also rebuilds the test code objects with clang
pwsh compute\hip\build.ps1 -SkipTests
```

The compiler comes from the installed Visual Studio and the headers and import libraries from the SDK
NuGet packages under `toolchain\nuget`. No WDK or SDK installation is needed. The device artifacts of
`tests/data` need the portable AMDGPU clang, and only with `-Rebuild`. Compiler temporaries and build
output stay off drive C:.

The script is the gate. It runs, in this order:

1. `-CheckDoc`: `include/bc250hsa.h` equals the header block of the design document.
2. Restate-and-compare for the private driver blobs: the four magic values, the three structure sizes
   and the indirect buffer count, each against `driver/contract/`.
3. A parse of `tools/run-lab.ps1`, which runs under Windows PowerShell 5.1 on the lab.
4. `-Rebuild`: the two device artifacts, rebuilt and compared with the committed bytes.
5. The library, the five tests and `hipprobe.exe`, at `/W4 /WX /std:c11` with `/Brepro`, so the
   SHA-256 at the end names the input and not the hour of the build.
6. Every test, `hipprobe --selftest` and `hipprobe --help`.

The PM4 packet numbers, the register offsets and the event types are restated in
`bc250hsa/pm4_regs.h` and checked in C by `tests/host/test_pm4.c`, which includes the four vendored
Linux headers itself. A restated value that drifts fails the build (repository rules 1 and 2).

## The host tests

| Test | What it would catch |
|---|---|
| `test_loader` | a segment that is not copied, a `.bss` that is not zeroed, a relocation type accepted wrongly, a wrong descriptor address, a wrong ABI version check |
| `test_unbundle` | a reader that picks the host entry, keys an entry by its offset, misses the compressed magic, or splits a target identifier on a hyphen |
| `test_kernarg` | a fixed offset for a hidden field, the grid and the block the wrong way round, a hidden field left unwritten |
| `test_descriptor` | a field read from the wrong byte, a reserved field accepted non-zero, an unknown `KERNEL_CODE_PROPERTIES` bit accepted, a wrong `USER_SGPR` extraction |
| `test_pm4` | a changed packet order, register offset, shader-type bit, entry address shift, fence dword or padding, and every restated constant |

Each test takes the path of `tests/data` as its one argument and exits with the number of failed
checks.

## The step-1 trial

`tools/hipprobe.c` runs three kernels and checks every result on the processor:

| Kernel | Question it answers |
|---|---|
| `vadd` | does a clang-built code object run at all, with explicit arguments only |
| `reduce256` | is the computed `LDS_SIZE` right, and does a workgroup barrier work |
| `writeGridSize` | did the implicit argument block reach the kernel |

Every wait is at most ten seconds, and the tool never ends a timeout quietly: it prints the fence
values, the execution state and the dword count of the indirect buffer that is still in flight, says
that the submission is still in flight, and then frees nothing and closes nothing: it issues no unmap
and no destroy while the command processor may still be reading. It keeps nothing alive past its own
exit. The operating system reclaims the allocations, the addresses and the device when the process
ends, a moment later.

Exit codes: 0 all right, 1 usage, 2 host failure, 3 timeout, 4 device lost, 5 result mismatch, 6 the
adapter did not open.

On the lab, `tools/run-lab.ps1` performs the six runs of section 6.2 of the design in one session,
under a 150-second bound, and writes `result.json`, the driver log tail and the clock readings beside
it. The bound covers the whole session, from the first call of the release client to the last line of
the driver log tail: every single call is bounded by the time that is left, so the printed bound is
the real one. The record ends with the seven pass criteria of section 6.3, each one `pass`, `FAIL` or
`unknown`, so a reader does not have to judge. It refuses to run at or above 87 C and when it
cannot read Tctl at all, and after a timeout or a lost device it stops the session and submits
nothing again. The kit that drives it, the
push list and the evidence to pull are in `scratch/m16-hip/lab/` of the workspace, which stays
outside this repository because it names lab paths.

"Mierz siły na zamiary" - measure your strength against your plans. Layer 1 is the strength. The
plans are in section 7 of the design, which lists the nine questions only the lab can answer.
