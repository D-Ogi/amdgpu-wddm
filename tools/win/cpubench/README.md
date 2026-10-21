# cpubench: CPUID and vector throughput of the CPU

One C file with the kernels in `kernels.inc`. It prints the CPUID feature bits, the psABI level (x86-64-v2, v3 or v4)
and the time of these operations:

- The RGBA8 to BGRA8 row copy of the Vulkan WSI's GDI route, in five versions: the per-byte loop of
  `wsi_common_win32.cpp`, a 32-bit word swap, SSSE3 `pshufb`, AVX2 `vpshufb` and `memcpy`. Each runs on one 1920x1200
  frame and on a 16 KiB block that stays in L1. Each output is compared with the per-byte loop's output.
- The throughput of 128-bit and 256-bit FMA, FADD, FMUL and `pshufb` (ten independent chains), and the latency of a
  256-bit FMA (one chain).

The thread is pinned to logical CPU 2 at the highest thread priority. Nothing is written to disk.

    pwsh -File build.ps1 -Kits <BC250_ROOT>\toolchain\nuget [-Llvm <LLVM directory>]
        -> <BC250_ROOT>\scratch\build\cpubench\cpubench.exe, cpubench-avx2.exe, cpubench-clang.exe
    cpubench.exe [repeats | cpuid]
    python tools/win/target.py ps tools/win/cpubench/run.ps1 -Dir <directory on the target>

`build.ps1` makes three builds of the same source: MSVC `/O2`, MSVC `/O2 /arch:AVX2` and clang-cl `/O2 -mavx2
-mfma`, so the compiler is the variable. It prints the compiler versions it used. **Only the clang build gives valid
`reg` and `lat` lines by construction.** MSVC 19.44 keeps the 256-bit FMA chains, and two of the 128-bit ones, on the
stack. Its `fma` lines thus measure a store-to-load chain and not the floating-point unit. Its other lines keep their
chains in registers in the measured builds, but nothing in the source makes sure of that. `cpuid` prints the CPUID
lines and the level, and exits without load.

Positive control: on an Intel Core i7-13700KF the clang build gives 2 FMA, 2 FADD and 2 FMUL per cycle, at 128 and
256 bits. Unit A's results are in
[`evidence/windows/2026-10-21-bc250-cpu-vector-throughput/`](../../../evidence/windows/2026-10-21-bc250-cpu-vector-throughput/README.md),
with the corrections and a follow-up run in
[`evidence/windows/2026-10-21-bc250-cpu-vector-throughput-review/`](../../../evidence/windows/2026-10-21-bc250-cpu-vector-throughput-review/README.md).
