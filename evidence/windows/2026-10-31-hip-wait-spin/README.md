# The spin of a HIP wait: llama.cpp decode tokens on unit A

Date: 2026-10-31, unit A, Windows 11 Pro 22631, the tester.34 release installed (kernel driver 40.7.216.106),
40 CU, the DPM ceiling at 1500 MHz, an 8 GiB carve-out, LargePages 1. Every arm ran a llama.cpp tool from
`C:\BC250\tmp\t35hip\run-<llama>-<runtime>` with flash attention on and `-ngl 99`, one arm at a time, each inside a
170 s bound (`lab-arm.ps1`). The tool is `llama-bench` where a section does not name another one. Before each arm,
`sweep-env.ps1` (or `series-ppl.ps1`, `series-ops.ps1`) checked the STOP flag and that Tctl was below 80 C. The
clock read 1500 MHz at 100 VID after every arm.

The question: where does a decode token of llama.cpp lose time on our HIP runtime, and does a short spin on the
fence mapping before the operating system's wait give that time back?

## The stack under test

| Part | Identity (SHA-256) |
|---|---|
| `ggml-hip.dll` "e57", the tester.35 asset candidate (4 GiB buffers) | `E57D3DBA6C2436B0683AD2D1F807757CAF749B8D6D633BE811804E84CB0DE04E` |
| `ggml-hip.dll` "ovl", the same patches with 1 GiB buffers | `017CB770` (prefix, `lab-arm.ps1` prints it) |
| `ggml-hip.dll` "a1g", the asset rebuilt at hip/llama-asset-t35 f623a743 (1 GiB buffers) | `1607928BFB3686FBF6658F4DD5250B198FB8DBD9A38D00102C95374845B7519C` |
| `llama-bench.exe` of "e57" | `65633817683A2E8E9ADB9442F6D99B3E2CA5F5CBA76DE62D89D344B4E6935CB8` |
| `amdhip64.dll` "t35l": main 35a5c7c2, lab flavour, no spin | `E129FBD760B117E10D7B499A91A0EF56E649777A61ACF38622904C6EB84AFD80` |
| `amdhip64.dll` "idl": the spin and a batch ramp, lab flavour | `A15D499623EFE0C3E8ABF6F5585311956F4099CC99F5604B71EE3F9680EBE9F9` |
| `amdhip64.dll` "idl2": the spin only, lab flavour | `8A77FC84363925ABBD24D3987FC3D142913F750C0B1BC2BFD2E2DD775E1F6BDF` |
| `bc250hipblas.dll` of "idl2" | `715F01B03508F139951FECA93AEC630C56A4328AF3A5746B306F7C84128D018D` |
| `amdhip64.dll` "idl3": the spin with its default of 20000 us and the budget lines of a failed GTT allocation, lab flavour | `82FA0DA84226BB8E6C890D3353F4456AA1D383791F5C3F6BDDE7322258EE2BF5` |
| `amdhip64.dll` "idr3": the same code, release flavour | `C470CE90965EB0FB364AD5C35FB4ACA819737E76B51351E706063E360E1B6DBB` |
| `bc250hipblas.dll` of "idl3" and of "idr3" | `8F77345B` and `06A03240` (prefixes) |

"idl3" and "idr3" are built from the code of this commit. One source comment in `runtime/hip_device.cpp` changed
after that build.

Models: `qwen2.5-1.5b-instruct-q4_k_m.gguf` (1.04 GiB) and `Qwen3-8B-Q8_0.gguf` (8.11 GiB), the files of the HIP
parity series.

## 1. The ring gaps of a token, without and with the spin

`ringgap-arm.ps1` reads the ring-gap counters of the kernel driver (`bc250kmd_cli log summary only`, cumulative
from the driver start) before and after one arm, and prints the difference for each node. Each arm was qwen2.5-1.5B
`-p 0 -n 64 -r 3`: 3 x 64 tokens and the one warm-up token of `llama-bench`, 193 tokens. The kernel driver opens a
gap when its DPC reads the fence of the last job and closes it at the next submission (`driver/kmd/ring_gap.h`).
The buckets are "up to" their upper edge.

Node 0, the compute ring (`ringgap-32.log` and `ringgap-spin.log`):

| Arm | tg64 | under 16 us | 16-32 | 32-64 | 64-128 | 128-512 | 512-2000 | 2000-8000 | 8000 and more |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| "e57" + "t35l", no spin | 190.27 | 1 | 39 | 91 | 64 | 185 | 23 | 2 | 12 |
| "a1g" + "idl2", spin 0 | 189.52 | 1 | 16 | 113 | 61 | 180 | 34 | 1 | 15 |
| "a1g" + "idl2", spin 20000 us | 198.49 | 104 | 49 | 41 | 1 | 196 | 13 | 1 | 12 |

"a1g" is the rebuilt asset of section 5. The spin removes most gaps of 32 to 128 us and about 60 % of the gaps of
0.5 to 2 ms. 104 gaps are under 16 us: the host reads the fence before the DPC does and submits again.
The gap of 128 to 512 us that comes about once a token stays. The spin does not reach it, and this directory does not
tell what it holds. The gaps of 8 ms and more are the time between the arms of `llama-bench` and the model load.

## 2. The batch size is not the cause

`series-batch-e57.log`, qwen2.5-1.5B tg64 on "e57" and "t35l", by `BC250_HIP_BATCH_MAX`: 32 (the default) 190.18 and
189.39 (first and last arm), 64 189.16, 16 187.40, 128 188.85 tokens a second.

## 3. The spin, and a batch ramp that did not help

`series-ramp-spin.log`, qwen2.5-1.5B tg64 on "e57" and "idl". The ramp sent the first buffer after an idle GPU with
one dispatch, and each later buffer with four times more, up to the batch size. `BC250_HIP_BATCH_RAMP=0` turned it
off. The switch of the spin is `BC250_HIP_WAIT_SPIN_US`.

| Arm | tg64 tokens a second |
|---|---:|
| no ramp, no spin (first arm) | 190.22 |
| ramp, no spin | 186.77 |
| no ramp, spin 4000 us | **198.35** |
| ramp, spin 4000 us | 197.45 |
| ramp, spin 20000 us | 197.66 |
| ramp, spin 4000 us, batch 64 | 197.72 |
| ramp, spin 4000 us, batch 128 | 197.54 |
| no ramp, no spin (last arm) | 189.73 |

The ramp made each arm slower, so it is not in the runtime. The spin alone gave the most.

## 4. The spin bound, on the runtime without the ramp

`series-spin-idl2.log`, runtime "idl2", llama "e57":

| Arm | qwen2.5-1.5B tg64 | qwen2.5-1.5B pp512 |
|---|---:|---:|
| spin 0 (first arm) | 188.87 | 2150.06 |
| spin 4000 us | 198.03 | 2148.55 |
| spin 20000 us | 198.49 | |
| spin 1000 us | 190.02 | |
| spin 0 (last arm) | 188.89 | 2151.48 |

A spin of 1000 us ends before the work of a token does and gives almost nothing: the bound must cover the work of a
token. The prompt rate does not change.

Every Qwen3-8B arm of this file stopped at the model load: `ggml-hip.dll` "e57" divides the model into buffers of up
to 4 GiB, and its second buffer of 3797680128 bytes fit neither the rest of the carve-out nor the system memory budget
(`hip-8b-e57-alloc.log`: local budget 7482638336 bytes with 4244897792 used, non-local budget 3733582234 bytes, MakeResident
0xc0000017). "ovl" with 1 GiB buffers loads the same model (`diag-8b.txt`). The 8B arms below therefore use "ovl".

`series-spin-8b-ovl.log`, runtime "idl2", llama "ovl", Qwen3-8B Q8_0:

| Arm | tg64 | Arm | pp512 |
|---|---:|---|---:|
| spin 0 (first arm) | 39.18 | spin 0 (first arm) | 513.85 |
| spin 4000 us | 39.29 | spin 20000 us | 513.58 |
| spin 20000 us | **39.63** | spin 0 (last arm) | 513.87 |
| spin 50000 us | 39.58 | | |
| spin 0 (last arm) | 39.17 | | |

A token of the 8B model takes about 25 ms, so a spin of 4000 us ends before most of its work does.

## 5. The rebuilt asset

`series-a1g.log`, runtime "idl2", llama "a1g" (the asset builder with 1 GiB buffers, hip/llama-asset-t35 f623a743):

| Model | tg64 spin 0 (first) | tg64 spin 20000 us | tg64 spin 0 (last) | pp512 spin 20000 us |
|---|---:|---:|---:|---:|
| Qwen3-8B Q8_0 | 39.20 | 39.66 | 39.22 | 513.55 |
| qwen2.5-1.5B Q4_K_M | 188.94 | 198.06 | 189.43 | 2152.34 |

The 8B model loads at the 8 GiB carve-out with this build.

## 6. The release runtime with no switch

`series-rel.log` (`series-rel.ps1`), llama "a1g". "idr3" is a release runtime: it reads no switch and spins for its
default of 20000 us. "idl3" is the same code in the lab flavour, with `BC250_HIP_WAIT_SPIN_US=0` as the control. An
arm with no switch has `T35_NOENV=1`, a variable that no part of the stack reads.

| Arm | qwen2.5-1.5B tg64 | Qwen3-8B tg64 |
|---|---:|---:|
| "idr3", no switch (first arm) | 198.31 | 39.64 |
| "idl3", `BC250_HIP_WAIT_SPIN_US=0` | 190.82 | 39.18 |
| "idl3", no switch | 198.34 | |
| "idr3", no switch (last arm) | 198.21 | 39.64 |

qwen2.5-1.5B pp512 on "idr3": 2150.16 tokens a second.

## 7. Perplexity and the backend tests on the release runtime

`series-spin-check.log` and `series-spin-check-15b.log` (`series-ppl.ps1`, `series-ops.ps1`), llama "a1g", runtime
"idr3" with its default spin. The 1.5B perplexity ran in a second call, because Tctl was above 80 C after the 8B arm.

`llama-perplexity` on the wikitext-2 test prefix (64730 bytes, `-c 512 --chunks 16`). The reference runs are from the
same day with runtime "socc" (amdhip64 CB3842F7, no spin) and llama build "bload" at a 12 GiB carve-out
(`ppl-reference-lp1.log`, `ppl-reference-lp2.log`). Their 8B arm under LargePages 1 is the failure of BD-277.

| Model | PPL with the spin | PPL of the reference runs |
|---|---|---|
| Qwen3-8B Q8_0 | 11.4426 +/- 0.55798 | 11.4426 +/- 0.55798 (LargePages 2) |
| qwen2.5-1.5B Q4_K_M | 12.4067 +/- 0.52907 | 12.4067 +/- 0.52907 (LargePages 1 and 2) |

`test-backend-ops -b ROCm0`, one group in each arm:

| Group | Filter | Tests passed |
|---|---|---:|
| MUL_MAT q8_0 | `-o MUL_MAT -p type_a=q8_0` | 113 of 113 |
| MUL_MAT q4_K | `-o MUL_MAT -p type_a=q4_K` | 120 of 120 |
| MUL_MAT f16 | `-o MUL_MAT -p type_a=f16` | 361 of 361 |
| FLASH_ATTN_EXT, head size 128 | `-o FLASH_ATTN_EXT -p hsk=128,` | 555 of 555 |
| 20 other operations | `-o RMS_NORM,L2_NORM,ROPE,...` (the list of `series-ops.ps1`) | 2486 of 2486 |

## Result

20000 us is the default of the runtime: it is the best of the measured bounds for both models (qwen2.5-1.5B tg64
188.87 to 198.49 or +5.1 %, Qwen3-8B tg64 39.18 to 39.63 or +1.1 %), and the prompt rate does not change. A release
runtime reads no switch (BD-167) and gives the same rates with its default (section 6). Its perplexity is that of the
runs without the spin, and no backend test fails (section 7). A lab runtime reads `BC250_HIP_WAIT_SPIN_US`. The spin
removes most of the short idle gaps of the compute ring (section 1). The gap of 128 to 512 us that comes about once
a token stays, and it is the next cost to find.

## Limits

- One boot, one series per question, `-r 3` for each arm, and the spread that `llama-bench` prints. The first and
  last arm of each series are the same arm, and they differ by 0.5 tokens a second or less.
- The spin keeps one processor core busy for up to the bound in each wait. The processor power was not measured.
  At 1500 MHz the GPU clock did not change, and it was not measured with the DPM ceiling at 2000 MHz.
- Sections 1 to 5 use lab runtimes, which read the switch. Sections 6 and 7 measure the release flavour with its default.
- The model files sit in the 8 GiB carve-out and, for the 8B model, partly in system memory. The parity series at the
  12 GiB carve-out is a different measurement.

## Files

- Logs: one for each series, named in the sections above. `hip-8b-e57-alloc.log` is the runtime log of the failed 8B
  load, and `diag-8b.txt` holds the console lines of the load check.
- Scripts: `lab-arm.ps1` runs one bounded arm on unit A. `sweep-env.ps1` runs one arm for each environment set,
  and `series-ovl.ps1`, `series-rel.ps1`, `series-ppl.ps1` and `series-ops.ps1` run the series. `ringgap-arm.ps1` and
  `lab-kmd-summary.ps1` read the ring-gap counters. `lab-rt-set.ps1` and `lab-llama-set.ps1` fill the run folders, and
  `lab-errtail.ps1` and `lab-ops-sum.ps1` print the output tails.
