# HIP device contract and checks

These headers implement a subset of HIP for gfx1013. The version identifies the compatibility target, not complete HIP support.
Compiler checks establish header behavior. They do not establish a completed lab validation or release acceptance.

PROVENANCE: ROCm/clr, MIT, tag `rocm-6.2.0`, commit `dd7f9576620569fc8847cef767efcc1f4bab9d1a`.

The audit uses the [ROCm device headers](https://github.com/ROCm/clr/tree/dd7f9576620569fc8847cef767efcc1f4bab9d1a/hipamd/include/hip/amd_detail).
The ABI reference is LLVM 23.1.2 [AMDGPUUsage](https://llvm.org/docs/AMDGPUUsage.html), with its GFX10 memory model.
The scope covers the implemented mappings in `hip_runtime.h`, the numeric headers, vector types and cooperative groups.

## Launch geometry

`threadIdx` identifies a work-item inside its block. `blockIdx` identifies the block inside the grid.
`blockDim` counts work-items per block. `gridDim` counts blocks per grid axis.

The AQL dispatch packet stores each grid extent in work-items.
The header divides that extent by the corresponding workgroup size to implement `gridDim`.
Using the raw AQL extent as `gridDim` produces an incorrect stride in kernels that index several dimensions.

HIP launches here contain uniform, complete blocks. The runtime does not support an AQL launch with a smaller physical last group.
An application can have a partial logical tail. Threads outside that data extent still exist and must check their bounds.
Their `blockDim` and `gridDim` do not change.

`warpSize` comes from the compiler's wave size. The device controls require wave32 and check that value in the compiled metadata.

## Corrections covered by the controls

| Area | Contract |
|---|---|
| Byte permutation | Each selector nibble selects one input byte. |
| Half extrema and ReLU | Ordinary extrema suppress one NaN. Named NaN variants and ReLU preserve NaNs. Signed zero has explicit checks. |
| Double conversion | Binary64 rounds directly to binary16 or bfloat16. An intermediate float must not introduce double rounding. |
| Packed bfloat | `__hip_bfloat162` and its raw type have 4-byte alignment. |
| Atomics | Ordinary operations use agent scope. System variants use system scope. Floating-point exchange compiles and retains atomic behavior. |
| Warp operations | Ballots honor the explicit mask. Shuffle widths must be powers of two within the wave size. |
| Cooperative geometry | Rank and size include all three dimensions. A grid-wide barrier remains unsupported. |

System scope alone does not make an allocation eligible for CPU/GPU system atomics.
The memory type, hardware and synchronization contract must also support that access.

## Rebuild requirement

Rebuild every consumer that embeds device code after a header change.
This includes `ggml-hip.dll`, `bc250hipblas.dll`, `vadd.exe`, `hipbench.exe` and the device controls.
Rebuild the host and device halves together. The packed bfloat alignment change affects their shared ABI.
A runtime DLL replacement cannot correct instructions inside an existing code object.

Current code-object metadata does not identify the revision of these headers.
The loader cannot reliably distinguish the old mapping from the corrected mapping.
Keep compiler, source and header hashes with each artifact. Do not admit unknown old objects as corrected artifacts.

## Unsupported operations

- The runtime refuses cooperative launch. `grid_group::is_valid()` returns false, and reaching `grid_group::sync()` traps.
- Device `printf` requires a hostcall service that this backend does not implement. A reachable call fails device linking.
- `__nanosleep` has no supported timing contract here. A reachable call produces a compiler error.
- An unused helper can contain either unsupported call without preventing unrelated kernels from linking.

The second `__launch_bounds__` argument remains an unused occupancy hint. The maximum block size still applies.

## Build and check commands

Run the commands from the repository root.
Set `$workspace` to the workspace directory. Set `$hipLib` to the built runtime's `amdhip64.lib`.
Keep temporary files and output on the workspace drive.

```powershell
$out = Join-Path $workspace 'scratch/hip-device-checks'
New-Item -ItemType Directory -Force $out | Out-Null
$env:TEMP = $out
$env:TMP = $out
$clang = Join-Path $workspace 'toolchain/llvm-amdgpu-22.1.8/mingw64/bin/clang.exe'
python -B compute/hip/tests/host/test_builtin_values.py --clang $clang `
  --include compute/hip/include --out "$out/compiler"
pwsh compute/hip/build-geometry.ps1 -Root $workspace `
  -Out "$out/geometry" -ImportLibrary $hipLib
pwsh compute/hip/build-device-control.ps1 -Root $workspace `
  -Out "$out/builtins" -ImportLibrary $hipLib
pwsh compute/hip/build-rebuilt-controls.ps1 -Root $workspace `
  -Out "$out/rebuilt" -ImportLibrary $hipLib
```

The compiler gate checks constant results, atomic IR and unsupported-call diagnostics.
The geometry builder runs its CPU oracle controls. The rebuild script runs the HIPBLAS CPU reference test.
These commands never execute a GPU client on the build host.
The builders record input hashes and reject inputs that change during compilation.

`geometry.exe` checks 1D, 2D and 3D launches, warp size and logical tails.
Its output addresses use independent launch bounds, so incorrect dimensions cannot select an unbounded output address.
`builtins.exe` checks live inputs, two waves, shuffles, ballots, block collectives, atomics and numeric conversions.
Run both only within a bounded GPU test session. Keep their output and the driver health result.

## Old-header negative controls

Export `compute/hip/include` from revision `1186a757` to a separate scratch directory named by `$oldInclude`.
Keep the repository headers unchanged.

```powershell
python -B compute/hip/tests/host/test_builtin_values.py --clang $clang `
  --include $oldInclude --out "$out/old-compiler" --negative
pwsh compute/hip/build-geometry.ps1 -Root $workspace `
  -Out "$out/old-geometry" -ImportLibrary $hipLib -IncludeRoot $oldInclude
```

The compiler control must detect the old contract failure. Its current old-header witness rejects floating-point `atomicExch`.
The old geometry executable must report dimension mismatches when the bounded lab control runs it.
The corrected executable must report no mismatches and intact canaries under the same launch conditions.
Successful compilation alone does not establish either device result.
