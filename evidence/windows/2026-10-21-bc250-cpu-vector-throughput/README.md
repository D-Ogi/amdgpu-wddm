# BC-250 CPU: instruction set and vector throughput (2026-10-21)

Question: which instructions the BC-250 CPU has, and whether its 256-bit (AVX/AVX2) units run at full width. A
second question came from the GDI route of the Vulkan WSI (BD-197): what its per-byte RGBA8 to BGRA8 row loop costs
on this CPU.

Tool: [`tools/win/cpubench`](../../../tools/win/cpubench/README.md). Unit A, Windows 22631, tester.31v3 installed,
thread pinned to logical CPU 2 at the highest thread priority. The lab desktop was idle: no game or trial ran at
the same time, and `run.ps1` lists none.

## Files

| file | what |
|---|---|
| `lab-throughput.txt` | unit A, lab clock 2026-10-21 01:56:22Z, the three builds, 15 repeats each |
| `lab-cpuid.txt` | unit A, lab clock 2026-10-21 02:14:16Z, `cpubench-clang.exe cpuid` of the committed source |
| `dev-pc-control.txt` | the positive control: the same clang build (E6639816) on an Intel Core i7-13700KF |
| `disasm-fma256-excerpt.txt` | `dumpbin /disasm` of the FMA loop in the MSVC and the clang build |
| `cpubench-as-measured.c`, `kernels-as-measured.inc` | the sources of the three builds in `lab-throughput.txt` |
| `SHA256SUMS.txt` | the hashes of the files above |

The committed `tools/win/cpubench/cpubench.c` adds the psABI level line and the `cpuid` argument to
`cpubench-as-measured.c`. The kernels and `kernels.inc` are unchanged.

The builds of `lab-throughput.txt`:

| build | compiler and flags | sha256 |
|---|---|---|
| `cpubench.exe` | MSVC 19.44 `/O2 /W3 /MT` | F521259E97707065BABB37CCCD5CB8964EA6BA45377B7FE8E80974F00EDB02FD |
| `cpubench-avx2.exe` | MSVC 19.44 `/O2 /W3 /MT /arch:AVX2` | 43AD563A34DBE088428E418A1F1E1BD5349BE5E8DA2A81FBC50AB49A6C196AC5 |
| `cpubench-clang.exe` | clang-cl 23.1.2 `/O2 /MT -mavx2 -mfma` | E6639816960C743E3AF841F799A20D308BBAAFB21260FCBEAA5465DDBDA4FA29 |

## Instruction set

The CPUID leaves 1, 7 and 0x80000001 and XCR0 give:

- Present: SSE2, SSE3, SSSE3, SSE4.1, SSE4.2, SSE4a, POPCNT, LZCNT, CMPXCHG16B, LAHF/SAHF, MOVBE, XSAVE, AVX,
  FMA3, F16C, AVX2, BMI1, BMI2, ADX, AES-NI, PCLMULQDQ, SHA, RDRAND, RDSEED, CLFLUSHOPT, CLWB, RDPID. XCR0 is
  0x7, so the OS saves the YMM state.
- Absent: AVX-512 (all of it), VAES, VPCLMULQDQ, FSRM, FMA4, XOP.
- psABI level: x86-64-v3 (every v3 bit present), not v4.
- Leaf 1 EAX 0x00840F70: family 17h, model 47h, a Zen 2 core. Windows names it "AMD BC-250", 6 cores, 12 logical
  processors.

The Intel control CPU is also x86-64-v3, but has no SSE4a. A binary built for `-march=znver2` can therefore use an
instruction that an Intel CPU of the same level does not have. `-march=x86-64-v3` with `-mtune=znver2` does not.

## Vector throughput

Each `reg` line is ten independent dependency chains of one instruction, in ns per instruction. The `lat` line is
one chain. Only the clang build gives valid `reg` and `lat` lines. MSVC keeps the ten 256-bit FMA chains on the
stack, so each FMA waits for a load and a store (`disasm-fma256-excerpt.txt`: `vfmadd231ps ymm0,ymm2,ymmword ptr
[rbp+40h]` followed by `vmovups ymmword ptr [rbp+40h],ymm0`). Clang keeps all ten in registers. An empty `asm`
barrier on each chain prevents clang from narrowing a chain to its lane 0 or merging two constant shuffles
(`KEEP` in `kernels.inc`).

| instruction | BC-250 128-bit | BC-250 256-bit | control 128-bit | control 256-bit |
|---|---:|---:|---:|---:|
| `vpshufb` | 0.1433 | 0.1433 | 0.0972 | 0.0961 |
| `vfmadd` ps | 0.2866 | 0.2866 | 0.0957 | 0.0962 |
| `vaddps` | 0.2865 | 0.2866 | 0.0969 | 0.0964 |
| `vmulps` | 0.2865 | 0.2866 | 0.0968 | 0.0969 |
| FMA latency, 256-bit | - | 1.4329 | - | 0.7722 |

How the clock is estimated: Zen 2 has an FMA latency of 5 cycles, so 1.4329 ns per FMA gives an estimated core clock
of 3.49 GHz during the run (Windows reports 3194 MHz as the nominal maximum). At that clock:

- `vpshufb` issues 2 per cycle, at 128 and 256 bits.
- FMA, FADD and FMUL each issue 1 per cycle, at 128 and 256 bits.
- The 256-bit forms run at the same rate as the 128-bit forms. The core does not split them into two halves.

The method can show 2 per cycle: on the control CPU the same binary gives 2 FMA, 2 FADD and 2 FMUL per cycle (0.096
ns at an estimated 5.2 GHz, from its 4-cycle FMA latency). Ten chains are enough for 2 per cycle on the BC-250 too:
FADD and FMUL have a latency of 3 cycles there, so 6 chains would be sufficient.

Comparison, not measured here: AMD documents four floating-point pipes for Zen 2 (two for FMA and FMUL, two for
FADD) in its Software Optimization Guide for AMD Family 17h Models 30h and Greater (publication 56305). That would
give 2 per cycle for each of the three operations. On this part each operation gets 1 per cycle. Third-party die
analysis of the PS5's Zen 2, a part of the same family, reports a reduced floating-point unit. The peak FP32 FMA rate
is therefore 16 FLOP per cycle per core, not 32.

## Row copy and memory

One 1920x1200 frame (9.2 MB) is copied row by row, best of 15 passes. `l1` repeats a 16 KiB block that stays in L1.
Every output is compared with the reference loop ("match" on every line).

| build | per-byte loop | 32-bit word swap | SSSE3 `pshufb` | AVX2 `vpshufb` | `memcpy` |
|---|---:|---:|---:|---:|---:|
| MSVC `/O2`, frame ms | 2.750 | 0.879 | 0.819 | 0.973 | 1.064 |
| MSVC `/O2 /arch:AVX2`, frame ms | 2.753 | 0.820 | 0.819 | 0.963 | 1.064 |
| clang-cl `/O2 -mavx2`, frame ms | 1.064 | 1.050 | 0.822 | 0.922 | 1.062 |
| MSVC `/O2 /arch:AVX2`, L1 GB/s | 3.36 | 31.73 | 27.58 | 54.40 | 105.43 |

- One thread copies at 8.7 GB/s (`memcpy`, frame). On the control CPU the same copy gives 30 GB/s.
- The per-byte loop of the MSVC builds runs at 3.35 GB/s, also in L1, so it is limited by the instructions and not
  by memory. It costs 1.93 ms per frame more than the SSSE3 copy.
- In the clang build the same C loop runs at the `memcpy` rate. That build is not the compiler of the shipped ICD,
  which is MSVC.
- Once a copy is vectorized, memory limits it. AVX2 is faster than SSSE3 in L1 (54 against 28-34 GB/s), but slower
  on the frame (0.96 against 0.82 ms). The reason is not established.

## Limits

- One unit, two runs for CPUID, one throughput run with 15 repeats per line. The `reg` lines agree with the
  MSVC builds' `add` and `mul` lines (0.2865-0.2868 ns), which keep their chains in registers.
- The clock is estimated from the documented FMA latency. A direct reading of the core clock during the run was
  not taken.
- Whether one FMA and one FADD issue in the same cycle (two pipes shared) was not measured.
