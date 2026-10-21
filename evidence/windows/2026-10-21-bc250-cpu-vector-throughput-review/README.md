# BC-250 CPU throughput: review and follow-up run (2026-10-21)

This directory reviews the [CPU throughput evidence](../2026-10-21-bc250-cpu-vector-throughput/README.md) of the same
day (facts M906 to M908) and holds one follow-up run on unit A. That directory is immutable, so its corrections are
here. The run measures the floating-point pipes of one core, the core clock, the latencies, SMT and the memory path
of a frame copy. It gives the new fact [M910](../../../docs/facts/hardware.md#m910). M907 and M908 are corrected in
place.

## Files

| file | what |
|---|---|
| `lab-run-q44.txt` | unit A, 2026-10-21: the output of `run-q44.ps1`, below a header |
| `summary.txt` | the output of `python summarize.py lab-run-q44.txt`: cycles, rates and the frame lines |
| `q44bench.c`, `gen.py`, `mesa_gdi.cpp` | the source of the two `q44bench` builds |
| `build.ps1` | the build of `q44bench-clang.exe` and `q44bench-msvc.exe` from the files above |
| `run-q44.ps1` | the script that ran on unit A |
| `summarize.py`, `loops.py` | the reader of the run, and the loop finder for the listings below |
| `disasm-msvc-cpubench-fma-loops.txt` | the `fma`, `mul256` and `lat` loops of the two MSVC builds of the earlier run |
| `disasm-clang-keep-sweep.txt` | the C chain sweep of `q44bench-clang.exe`: spills, and the FMA(a, 1, c) fold |
| `disasm-cpubench-row-loops.txt` | the row copies of the three builds of the earlier run |
| `disasm-mesa-gdi-copy.txt` | the RGBA8 copy of the GDI present of Mesa: the source, the shipped ICD, the replica |
| `vcruntime-memcpy-paths.txt` | the path of the static C runtime `memcpy` for each size |
| `SHA256SUMS.txt` | the SHA-256 of each other file in this directory, except this README |

The explicit-assembly kernels are in this directory, not in `tools/win/cpubench`. `gen.py` writes all 100 of them
into `kernels_gen.inc` (SHA-256 `86A588AC613401E743198C9B41D2C699EAC64315E285E18996BAADDCCCEFEF8D`). `build.ps1`
runs `gen.py` and then both compilers:

    pwsh -File build.ps1 -Kits <toolchain>\nuget -Llvm <LLVM 23.1.4 directory> -Out <build directory>

A rebuild on the development PC on 2026-10-21 gave the same disassembly as the two measured binaries. The files
differ only in their link time stamps. The copies of `q44bench.c` and `run-q44.ps1` here differ from the files as
run only in their header comments, and `run-q44.ps1` also takes `-Dir` as a required parameter.

## The run

Unit A, Windows 22631, tester.31v3 installed, the thread pinned to logical CPU 2 at the highest thread priority. The
run took 04:47:57 to 04:48:31 UTC by the clock of the development PC. The lab clock was about 48 s ahead. The
script stops when a game or a GPU trial runs, and it lists the process names that it finds. None ran. The CPU was
0 % busy before the run and 1 % busy after it.

| binary | compiler and flags | SHA-256 |
|---|---|---|
| `q44bench-clang.exe` | clang-cl 23.1.4 `/O2 /MT -mavx2 -mfma`, with the explicit-assembly kernels | 4771DF7F476B6C67D74672035AE315034DABE705273B891063C41ED218500F05 |
| `q44bench-msvc.exe` | MSVC 19.44 `/O2 /MT`, no `/arch`, the C parts only | 00F5D33663FFDCDF24ACBA0DE4BBCB323DDAD104A09848A16E6BDBEC20E4F3FA |
| `cpubench-clang-E663.exe`, three runs | the `cpubench-clang.exe` of the earlier run | E6639816960C743E3AF841F799A20D308BBAAFB21260FCBEAA5465DDBDA4FA29 |

The cycles in `summary.txt` are nanoseconds times the clock of a chain of 100 dependent 64-bit adds, at one cycle
for each add. The median of that clock read 3.489 to 3.490 GHz at the start, in the middle, before the SMT part and
at the end. `summary.txt` uses 3.493 GHz, the mean of the fastest repeat of each of the four readings. At that
clock a chain of `imul` takes 3.003 cycles and a chain of FADD 3.002 cycles. AMD documents 3 cycles for both on Zen
2, so these two values are the positive control of the clock. The TSC runs at 3.194 GHz, the nominal clock that
Windows reports.

The three `cpubench` runs repeat the earlier `reg` and `lat` lines within 0.1 %: 0.2865 to 0.2867 ns for FMA, FADD
and FMUL, 0.1432 to 0.1433 ns for `pshufb` and 1.4326 to 1.4327 ns for the FMA latency. Their frame lines repeat
the earlier run within 1 %.

## Corrections to the earlier README

The instruction set, the time of each instruction and the row copy times of the earlier README are correct. These
sentences need a correction:

| line | earlier text | correction |
|---|---|---|
| 51 | "Only the clang build gives valid `reg` and `lat` lines." | The MSVC `fma` lines are not valid, because MSVC keeps those chains on the stack. Its add, mul, shuffle and `lat` lines agree with the clang build within 0.1 % (0.2866, 0.1433 and 1.4330 ns). The listings show `mul256` (the `/arch:AVX2` build) and `lat` (both builds) in registers. |
| 70 | "The core does not split them into two halves." | A 256-bit FMA and a 256-bit FADD issue in the same cycle, two per cycle. Each 256-bit operation thus takes one 256-bit pipe for one cycle. |
| 73-74 | "FADD and FMUL have a latency of 3 cycles there, so 6 chains would be sufficient." | Correct. The latency is now measured: 3.00 cycles. |
| 78-79 | "Third-party die analysis of the PS5's Zen 2, a part of the same family, reports a reduced floating-point unit." | Chips and Cheese measured a BC-250 itself, under Ubuntu Linux, with the clocks capped at 3.5 GHz, on three cores of one CCX. It reports that FP3 is removed and that FP2 handles only stores. |
| 95-96 | "so it is limited by the instructions and not by memory." | It is limited by the core and not by memory. Four byte stores per pixel, at the one store per cycle that AMD documents for Zen 2, give about 4 cycles per pixel. The measured loop takes 4.2 cycles per pixel. |
| 104 | "one throughput run with 15 repeats per line." | One throughput run. The `frame` and `l1` lines are the best of 15 repeats. Each `reg` and `lat` line is one timed call of 50,000,000 iterations. |
| 106-107 | "The clock is estimated from the documented FMA latency." | This run measures the clock: 3.49 GHz. At that clock the FMA latency is 5.003 cycles. |
| 108 | "Whether one FMA and one FADD issue in the same cycle (two pipes shared) was not measured." | They issue in the same cycle, two per cycle. |

The source of line 78 is "The Nerfed FPU in PS5's Zen 2 Cores", Chips and Cheese, 2024-03-20,
<https://chipsandcheese.com/p/the-nerfed-fpu-in-ps5s-zen-2-cores>.

M908 cites [E43](../2026-09-27-E43-witcher3-dx12-present-log/RESULT.md) for the copy of The Witcher 3. E43 names its
format 37 `VK_FORMAT_B8G8R8A8_UNORM`. The Vulkan format 37 is `VK_FORMAT_R8G8B8A8_UNORM`, so E43 measured the byte
swap branch. The source of its ICD, fork commit `0f9811a5`, has the same loop as Mesa `042227dc`.

## The floating-point unit

Each kernel is a loop of explicit assembly with 12 independent chains. In a `mix_` kernel, the even chains run one
instruction and the odd chains run the other. The rates are instructions per cycle from the fastest of 5 repeats,
and the 5 repeats of each kernel below spread by 0.2 % or less.

| kernel | 128-bit | 256-bit |
|---|---:|---:|
| FMA, FADD or FMUL alone | 1.00 | 1.00 |
| FMA and FMUL | 1.00 | 1.00 |
| FADD and FMA | 2.00 | 2.00 |
| FADD and FMUL | 2.00 | 2.00 |
| FADD and `vfmadd213ps` with a multiplier of 1.0 | 2.00 | 2.00 |
| `vpshufb` alone, `vpaddd` alone, or the two together | 2.00 | 2.00 |
| FMA, FADD or FMUL with `vpshufb` | 2.00 | 2.00 |
| FMA or FADD with `vpaddd` | 2.00 | 2.00 |

FMA and FMUL share one pipe, and FADD uses the other. Both pipes take `vpshufb` and `vpaddd`. The 256-bit forms
run at the rate of the 128-bit forms, also in the pairs, so each pipe is 256 bits wide. The FMA with a multiplier of
1.0 gives the same bits as FADD after 100,001 iterations (the `q24` line). It still issues on the FMA pipe, so the
pipe follows the instruction and not the arithmetic.

A rotating mask (`rot`) checks the `vpshufb` kernels. With the swap mask of the earlier run, an even number of
shuffles gives the start value again. With the rotating mask, the output equals the expected value after 1000 to
1003 iterations (the `q27` lines), at the same rate.

The chain sweep gives the latencies at both widths: FMA 5.003 cycles, FADD 3.002 and FMUL 3.001. One instruction
per cycle comes with 5 chains for FMA and with 4 chains for FADD and FMUL. The sweep has no 3-chain point. The other
latencies, in cycles, with the rate at 12 chains in instructions per cycle:

| instruction | latency | rate |
|---|---:|---:|
| `vpshufb`, `vpaddd` | 1 | 2.00 |
| `vpmulld` | 4 | 0.50 |
| `vpermd` | 8 | 0.50 |
| `vpermq` | 6 | 0.78 |
| `vperm2i128` | 3 | 1.00 |
| `imul r64` | 3 | - |

Independent scalar adds in 8 registers issue 3.69 per cycle, without the loop instructions. A chain of 48
dependent adds with 4 independent 256-bit FMAs between them still takes 1.001 cycles per add. The FMAs thus do
not lower the clock.

## The FMA pattern of MSVC

MSVC 19.44 keeps all ten 256-bit FMA chains of `kernels.inc` on the stack, and two of the ten 128-bit chains
(`disasm-msvc-cpubench-fma-loops.txt`). Each such FMA reads its accumulator from memory, and a store writes the
result back. The `mem_` kernels run this pattern in explicit assembly. The values are the fastest of 5 repeats,
and the median of the 256-bit kernel with ten chains through memory is 1.44.

| kernel | cycles per FMA |
|---|---:|
| one chain through memory, 128-bit | 13.01 |
| one chain through memory, 256-bit | 14.01 |
| ten chains through memory, 256-bit, the MSVC `fma256` loop | 1.40 |
| two chains through memory and eight in registers, 128-bit, the MSVC `fma128` loop | 1.30 |
| ten FMAs from memory, each with a store to a slot that no FMA reads | 1.00 |

An FMA whose accumulator goes through memory thus takes 13 cycles per step at 128 bits and 14 cycles at 256 bits.
The MSVC `fma128` and `fma256` lines of the earlier run, 0.3726 and 0.4013 ns, are 1.30 and 1.40 cycles. The MSVC
build of the C sweep in `q44bench.c` runs at 0.2864 to 0.2866 ns per FMA with 6 to 12 chains, the rate in registers.

The `KEEP` barrier of `kernels.inc` does not prevent spills. In the clang build of the same sweep, the third inlined
copy keeps one of 10 or two of 12 accumulators on the stack. Clang also turns `_mm256_fmadd_ps(x, set1(1.0f), c)` into
`vaddps` (`disasm-clang-keep-sweep.txt`). The comment of `kernels.inc` now says so.

## SMT

Total instructions per nanosecond of both threads. Logical CPUs 2 and 3 are one core (the `topology` lines, mask
0xc), and logical CPU 4 is on another core.

| kernel | one thread | two threads, one core | two threads, two cores |
|---|---:|---:|---:|
| 256-bit FMA, one chain | 0.698 | 1.396 | 1.396 |
| 256-bit FMA, 12 chains | 3.490 | 3.494 | 6.982 |
| 256-bit FADD, 12 chains | 3.489 | 3.493 | 6.979 |
| scalar add, 8 registers | 12.88 | 12.89 | 25.76 |

Two threads on one core share its pipes. A thread that waits on the latency of a chain leaves the pipes to the
other thread.

## The memory path of a frame copy

One 1920x1200 frame of 4-byte pixels (9,216,000 bytes), the best of 15 repeats. The kernels of
`q44bench-clang.exe` go over the whole frame in explicit assembly.

| kernel | ms per frame | GB/s of the frame |
|---|---:|---:|
| read only, 16-byte loads | 0.541 | 17.0 |
| write only, 16-byte stores | 0.566 | 16.3 |
| write only, non-temporal 32-byte stores | 0.241 | 38.3 |
| copy, 16-byte loads and stores | 0.835 | 11.0 |
| copy, 32-byte loads and stores | 0.953 | 9.7 |
| copy, 32-byte, with 7 ALU instructions between the load and the store | 0.898 | 10.3 |
| copy with `pshufb`, 16 bytes | 0.871 | 10.6 |
| copy with `vpshufb`, 32 bytes | 0.996 | 9.3 |
| copy, non-temporal 32-byte stores | 0.619 | 14.9 |
| `memcpy` of the whole frame in one call | 0.624 | 14.8 |
| `rep movsb` of the whole frame | 1.296 | 7.1 |
| `memcpy` for each 7,680-byte row, the `memcpy` line of `cpubench` | 1.058-1.067 | 8.7 |

A plain 32-byte copy is slower than a plain 16-byte copy. The AVX2 penalty of the row copies is thus in the memory
path and not in the shuffle. Its mechanism is not established. The C runtime `memcpy` uses non-temporal stores above
1.5 MiB, so one call for the frame takes that path. A call for each row takes its temporal 256-byte loop
(`vcruntime-memcpy-paths.txt`).

## The GDI copy of Mesa

`mesa_gdi.cpp` is a replica of the RGBA8 loop of `wsi_common_win32.cpp` at Mesa `042227dc`. It writes through a
`uint8_t *` and reads the width through the chain pointer for each pixel, as the shipped ICD does. A second version
reads the width once per row.

| build | width for each pixel, as shipped | width once per row |
|---|---:|---:|
| MSVC 19.44 `/O2` | 2.894 ms | 2.763 ms |
| clang-cl 23.1.4 `/O2 -mavx2` | 3.014 ms | 1.066 ms |

The shipped `vulkan_radeon.dll` (`4B307F93`, the system Vulkan ICD of tester.31v3) runs 4 byte loads, 4 byte
stores and one load of the width for each pixel (`disasm-mesa-gdi-copy.txt`). An MSVC build of the same source with
`/arch:AVX2` and LTO has the same loop. Clang-cl vectorizes only the version that reads the width once, and only
with AVX2. That the byte stores force the reload, because they may alias the chain, is an inference from the C rules
and the listings.

The present log measured the same loop in a present. For vkpresent with an RGBA8 chain at 1920x1200, the median
copy took 2.898 ms ([BD-197 block E](../2026-10-21-bd197-vk-dxgi-default/README.md)). The SSSE3 copy of Mesa
`9c3b6660`, on the BD-197 branch, took 0.828 ms. For The Witcher 3 DX12 in E43, the median copy took 3.232 ms and
the fastest 2.889 ms.

## Open observation

This is an observation, not a fact. With the destination 2048 bytes further into its page than the source, two
16-byte copy loops take 8-13 % less time: `asm_copy_x4` 0.777 against 0.896 ms, and `asm_shuf_x1` 0.800 against
0.871 ms. The page offsets of the DIB section and of the blit buffer in a present are not known.

## Limits

- One unit and one run, with 5 repeats for each kernel and 15 for each frame line. Over the 103 kernel lines, the
  median spread of the repeats is 0.08 %. Outside the memory chain kernels it is 1.2 % at most. The two
  `mem_chain10` kernels spread by 3.7 and 10.7 %, and the frame lines by a median of 3.2 % and 9.5 % at most.
- The cycles assume one cycle for each dependent add. The `imul` and FADD chains agree with that clock.
- Not measured: a copy over two cores, and the cause of the slower 32-byte copy.
