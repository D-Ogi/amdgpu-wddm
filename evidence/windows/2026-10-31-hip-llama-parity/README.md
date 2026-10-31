# HIP llama.cpp on unit A against the published bc250-rocm figures (tester.35)

Date: 2026-10-30 and 2026-10-31, unit A, Windows 11 Pro 22631. The questions:

1. Does llama.cpp on the HIP route of this driver reach the published Linux ROCm figures of bc250-rocm
   (https://github.com/akandr/bc250-rocm, commit 9a2abb17) for pp512 and tg64 of the three models below, at 1500 MHz
   and 40 CU, without a regression of perplexity or of the llama.cpp backend tests?
2. How much of the prompt rate comes from the large VRAM pages of the kernel driver (`LargePages` 1)?

Their figures are measurements on their own board and Linux setup (Fedora 44, ROCm 7.1.1, a GPU clock capped at
1500 MHz, the median of 18 samples). For this driver they are targets, not facts about unit A.

## Settings

| Item | Value |
|---|---|
| Board settings | 40 CU (`CuMode` 40). DPM ceiling 1500 MHz (`DpmMaxMHz` 1500), DPM floor pinned at 1500 MHz (`lab-clock-pin.ps1`). 12 GiB carve-out. `LargePages` 1 where a section does not name another value |
| Models | `qwen2.5-1.5b-instruct-q4_k_m.gguf` (1.04 GiB), `Qwen3-8B-Q8_0.gguf` (8.11 GiB), `Qwen3.6-35B-A3B-UD-IQ2_M.gguf` (10.72 GiB) |
| Protocol | `llama-bench -fa 1 -ngl 99 -r 3`, pp512 (`-p 512 -n 0`) and tg64 (`-p 0 -n 64`) in separate invocations, as their `campaign_split_f44.sh`. Their `-mmp 0` is not a switch of this llama-bench (`-lm` replaced it). The load mode stayed at its default |
| Arm bound | Each arm inside 170 s (`lab-arm.ps1`, `lab-arm-inst.ps1`). Before each arm: Tctl below 68 C (one read every 20 s, at most 240 s) and the STOP flag. A stop above 80 C |
| Clock | 1500 MHz at 100 VID before and after every arm (`bc250kmd_cli clock read`) |

## The stack under test

| Part | Identity (SHA-256) |
|---|---|
| `ggml-hip.dll` "a1g": llama.cpp b86d2f07 with the 16 patches of `compute/hip/patches/llama-patches.json`, built at hip/llama-asset-t35 f623a743. It is the build of the llama asset of tester.35 | `1607928BFB3686FBF6658F4DD5250B198FB8DBD9A38D00102C95374845B7519C` |
| `ggml-hip.dll` "base": the same commit with only the four gfx1013 build patches | `4D878441354C92C0369207A573C7CD25CF37F27DC74B2585651B718BFDC7675A` |
| `amdhip64.dll` "set35r": the release flavour of the tester.35 release set | `98630A90FAB473434BE7F2A4FA285B5BEEC9570DCA126966233CD6FB7BFEAEC8` |
| `amdhip64.dll` of the installed tester.35 (`<InstallRoot>\tools\hip`) | `8E7D9FD13BF0B85E08DDA26704AC6D2A9A271AA4E8DCF10F05AA057C0FCB34FB` |

The installed `amdhip64.dll` is "set35r" with the embedded test signature. No other byte differs.

## H0: the release runtime and the asset build before the package (tester.34 kernel driver)

2026-10-31 05:38-06:05Z, the tester.34 release installed (kernel driver 40.7.216.106) with `LargePages` 1. Each tool
ran from `C:\BC250\tmp\t35hip\run-a1g-set35r` (`lab-arm.ps1`, `series-final.ps1`).

The bench arms of `series-final.log` stopped at the switch `-mmp 0`: exit 1, and llama-bench printed its usage.
`series-finalb.log` is the same script without that switch, with the parts `bench` only. Two invocations of each row:

| Model | pp512 | bc250-rocm pp512 | tg64 | bc250-rocm tg64 |
|---|---:|---:|---:|---:|
| qwen2.5-1.5B-Instruct Q4_K_M | 2149.91, 2139.33 | 1798.6 | 198.29, 198.54 | 196.7 |
| Qwen3-8B Q8_0 | 578.02, 577.25 | 409.4 | 39.97, 39.94 | 38.5 |
| Qwen3.6-35B-A3B UD-IQ2_M | 662.65, 663.37 | 588.8 | 74.44, 74.52 | 71.2 |

`llama-perplexity -c 512 --chunks 16 -fa on -ngl 99` on the first part of the wikitext-2 test set. "a1g" is in
`series-final.log`, "base" in `series-baseppl.log` (`series-final.ps1 -Llama base -Parts ppl`):

| Model | a1g | base | +/- of a1g |
|---|---:|---:|---:|
| qwen2.5-1.5B-Instruct Q4_K_M | 12.4067 | 12.4996 | 0.53 |
| Qwen3-8B Q8_0 | 11.4426 | 11.4414 | 0.56 |
| Qwen3.6-35B-A3B UD-IQ2_M | 7.8359 | 7.8603 | 0.31 |

`test-backend-ops -b ROCm0` (`series-ops.ps1`, one group in each arm, `series-final.log`):

| Group | Filter | Tests passed |
|---|---|---:|
| mmid | `-o MUL_MAT_ID` | 931 of 931 |
| mmq8 | `-o MUL_MAT -p type_a=q8_0` | 113 of 113 |
| mmq4k | `-o MUL_MAT -p type_a=q4_K` | 120 of 120 |
| mmiq2 | `-o MUL_MAT -p type_a=iq2_` | 179 of 179 |
| mmf16 | `-o MUL_MAT -p type_a=f16` | 361 of 361 |
| fa128 | `-o FLASH_ATTN_EXT -p hsk=128,` | 555 of 555 |
| fa256 | `-o FLASH_ATTN_EXT -p hsk=256,` | 149 of 149 |
| misc | `-o RMS_NORM,L2_NORM,ROPE,...` (20 operations, the list of `series-ops.ps1`) | 2486 of 2486 |

4894 of 4894 tests passed. `llama-completion` with the chat template of Qwen3-8B Q8_0 (`--jinja`) answered "Paris" to
"What is the capital of France? Answer with one word.".

## A2: the installed tester.35

2026-10-31 10:00-10:08Z, the tester.35 release package (zip `380F84B3`) installed over tester.34 in place, with the
same settings. The tools ran from the installation: `llama-bench` and `llama-completion` of the installed llama.cpp
component, with the runtime that Windows finds through the machine PATH (`<InstallRoot>\tools\hip`).
`lab-arm-inst.ps1` prints the first `amdhip64.dll` of the search order. The installed `ggml-hip.dll` is "a1g".
The perplexity arm used the `llama-perplexity` of "a1g" on a copy of the installed runtime folder (`prep-inst35.ps1`),
because the llama asset does not carry that tool. Two invocations of each row (`series-a2.log`, `series-a2.ps1`):

| Model | pp512 | bc250-rocm pp512 | tg64 | bc250-rocm tg64 |
|---|---:|---:|---:|---:|
| qwen2.5-1.5B-Instruct Q4_K_M | 2157.75, 2159.37 | 1798.6 | 197.92, 197.02 | 196.7 |
| Qwen3-8B Q8_0 | 578.09, 577.88 | 409.4 | 39.97, 39.78 | 38.5 |
| Qwen3.6-35B-A3B UD-IQ2_M | 662.50, 662.84 | 588.8 | 74.52, 74.48 | 71.2 |

Qwen3-8B Q8_0 perplexity 11.4426 +/- 0.55798, the value of H0. The chat answer with the template: "Paris".

## LP: large VRAM pages against the same layout with 4 KB mappings

2026-10-30 21:12-21:20Z, the tester.34 release installed, the settings of the table above. `LargePages` 2 places
VRAM allocations as `LargePages` 1 does (the VRAM segment base on a 1 GiB boundary, an allocation of 2 MiB or more on
a 2 MiB offset), but maps them with 4 KB pages (`driver/kmd/wddm.c`). The pair therefore separates the effect of the
page size from the effect of the layout. The driver reads the value when it starts (`lab-largepages.ps1` sets it),
so a restart separates the two series. One `llama-bench -p 512 -n 64 -fa 1 -ngl 99 -r 2` arm for each model
(`series-lp.ps1` with the defaults of `sweep-env.ps1`), llama build "bload" and runtime "slab" of the lab folders
(`series-lp1.log`, `series-lp2.log`):

| Model | pp512, `LargePages` 1 | pp512, `LargePages` 2 | tg64, `LargePages` 1 | tg64, `LargePages` 2 |
|---|---:|---:|---:|---:|
| Qwen3-8B Q8_0 | 572.12 | 252.37 | 39.52 | 36.15 |
| qwen2.5-1.5B-Instruct Q4_K_M | 2145.88 | 1865.51 | 189.72 | 182.92 |
| Qwen3.6-35B-A3B UD-IQ2_M | 662.29 | 652.79 | 73.14 | 69.13 |

The 2 MiB pages raise the 8B prompt rate 2.27 times. The perplexity under both values is in the reference runs of
`../2026-10-31-hip-wait-spin/` (`ppl-reference-lp1.log`, `ppl-reference-lp2.log`).

## Result

On the installed tester.35 at 1500 MHz and 40 CU, the pp512 and tg64 rates of all three models are higher than the
published bc250-rocm figures, and each mean is within 1 % of the mean of H0. The perplexity of "a1g" is that
of the build with only the gfx1013 build patches within the +/- of each value, and no backend test fails.

## Limits

- No comparison on the same board: the bc250-rocm figures come from their board. No Linux run of unit A is part of
  this record.
- Their faster variants: `GGML_CUDA_GRAPH_OPT=1` on ROCm gives them tg64 213.1 for qwen2.5-1.5B and their Vulkan build
  212.3. This route gives 197.0 to 198.5.
- Power and the default clock: the driver default lets the clock go up to 2000 MHz. This record does not measure it.
- Two invocations of each bench row, `-r 3` each, and the spread that `llama-bench` prints.
- LP: one arm for each value, no file hashes. `sweep-env.ps1` hides the output lines of `lab-arm.ps1`, and that
  script changed after this series. This directory holds the later version. The "bload" and "slab" folders are not
  the files of H0 or A2.

## Files

- Logs: `series-final.log` (H0 perplexity, backend tests and chat), `series-finalb.log` (H0 bench),
  `series-baseppl.log` (H0 perplexity of "base"), `series-a2.log` (A2), `series-lp1.log` and `series-lp2.log` (LP).
  In the bench rows, the letter A and a replacement character (U+FFFD) stand in place of the plus-minus sign that
  `llama-bench` prints: a code page conversion of the lab output lost it.
- Scripts: `lab-arm.ps1` runs one bounded arm on unit A from a run folder, `lab-arm-inst.ps1` one arm on the installed
  files. `series-final.ps1`, `series-a2.ps1`, `series-ops.ps1` and `series-lp.ps1` run the series. `prep-inst35.ps1`
  copies the installed runtime folder, `lab-clock-pin.ps1` pins the DPM floor, `lab-largepages.ps1` sets the
  `LargePages` value of the next start and `lab-ops-sum.ps1` prints the test counts. `sweep-env.ps1` is in
  `../2026-10-31-hip-wait-spin/`.
