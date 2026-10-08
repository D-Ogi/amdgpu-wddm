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
| `bc250hsa/` | the library. The table below gives its file-by-file shape |
| `tests/host/` | five tests that need no GPU and no BC-250 adapter |
| `tests/data/` | the committed code object, fat binary, metadata dumps and the golden PM4 stream, with `PROVENANCE.txt` |
| `tools/hipprobe.c` | the step-1 tool: three kernels, every result checked on the processor, one JSON line each |
| `tools/run-lab.ps1` | the lab side of the step-1 trial: the six bounded runs of section 6.2, one record, the seven pass criteria |
| `build.ps1` | builds the library, the tests and the tool, and runs every gate that runs here |

## The shape of the library

```
bc250hsa.h                 the only header a caller needs
  status.c                 status names, the log hook, twelve process counters
  co_msgpack.c             a MessagePack reader that refuses what it does not know
  co_metadata.c            NT_AMDGPU_METADATA to bc250hsa_kernel; the 64-byte descriptor
  co_loader.c              the clang offload bundle, then the ELF code object
  kernarg.c                the kernel argument buffer, from the metadata list only
  pm4_dispatch.c           the 19 packets of one gfx1013 compute dispatch
  kmt_device.c             the adapter, the device, the node-0 context, the fence
  kmt_memory.c             allocate, map a GPU address, make resident, wait for the paging fence
  submit.c                 the command ring, D3DKMTSubmitCommand, the sliced bounded wait
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
that the submission is still in flight, and then frees nothing and closes nothing, because the command
processor may still be reading.

Exit codes: 0 all right, 1 usage, 2 host failure, 3 timeout, 4 device lost, 5 result mismatch, 6 the
adapter did not open.

On the lab, `tools/run-lab.ps1` performs the six runs of section 6.2 of the design in one session,
under a 150-second bound, and writes `result.json`, the driver log tail and the clock readings beside
it. The record ends with the seven pass criteria of section 6.3, each one `pass`, `FAIL` or
`unknown`, so a reader does not have to judge. It refuses to start at or above 87 C, and after a
timeout or a lost device it stops the session and submits nothing again. The kit that drives it, the
push list and the evidence to pull are in `scratch/m16-hip/lab/` of the workspace, which stays
outside this repository because it names lab paths.

"Mierz siły na zamiary" - measure your strength against your plans. Layer 1 is the strength. The
plans are in section 7 of the design, which lists the nine questions only the lab can answer.
