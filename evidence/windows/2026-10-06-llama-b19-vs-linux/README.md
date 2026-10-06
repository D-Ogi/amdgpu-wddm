# llama.cpp on release b19 against the Linux reference at equal clocks

Date: 2026-10-06, unit A. Four `llama-bench` runs between 18:32Z and 18:45Z on the installed release,
two at the release default clock ceiling and two with the ceiling set to 1000 MHz, which is the clock of
the Linux reference. The question is the old one of M9 and M12: at the same shader clock, how does the
Windows stack compare with `amdgpu` plus RADV on this unit. The answer here is that the Windows stack is
faster, by 62 % on prompt processing and 30 to 31 % on generation at 1000 MHz.

Every run stayed far inside the three-minute bound. The whole set took 13 minutes, including one restart
for the clock change and one to put the ceiling back.

## The stack under test

| Part | Identity |
|---|---|
| Release | 0.7.208.100-tester.13 (train b19), installed at `C:\Program Files\amdgpu-wddm`. The PnP driver version reads `0.7.208.100` |
| Kernel driver | `bc250kmd` 0.7.208.1, escape ABI `0x000700D0` (the `dpm` line of `bc250kmd_cli.exe`) |
| Vulkan ICD | the system ICD `C:\Program Files\amdgpu-wddm\vulkan\vulkan_radeon.dll`, SHA-256 prefix `CF3948D6`. That is the Mesa fork at `2732f9c8` on `amdgpu-wddm/gdi-immediate-quiet`, Mesa main 26.3-devel with the WDDM winsys (`tools/release/release-sources.json`, key `vulkan_icd`) |
| Benchmark | `C:\BC250\m9\llama\llama-bench.exe`, build `3b3da01dc` (9564), which is `3b3da01dc21dc68e958efb898ab739c65ed08ca2`. The Linux reference prints the same build string |
| Models | `stories15M-q4_0.gguf` and `tinyllama-1.1b-chat-v1.0.Q4_0.gguf` under `C:\BC250\m9\models` |
| Arguments | `-ngl 99 -p 512 -n 128 -r 3 -t 6 -o json -v`, with `MESA_SHADER_CACHE_DISABLE=true` and `VK_LOADER_DEBUG=driver` |
| Start method | a scheduled task in the interactive session, started through `conhost.exe --headless`, with `NUL` as stdin |
| Clock control | the KMD governor (DPM) stayed on. The ceiling came from `DpmMaxMHz` under the driver service's `Parameters` key |
| Other load | no game and no other trial. The trial paused the overlay summary poll (`overlay_summary_paused=True`), because that poll costs frames (see `reference_overlay_summary_hitch`) |

The model files are the ones whose hashes the 2026-09-24 runs recorded:
`66967FBECE6DBE97886593FDBB73589584927E29119EC31F08090732D1861739` for `stories15M-q4_0.gguf` and
`DA3087FB14AEDE55FDE6EB81A0E55E886810E43509EC82ECDC7AA5D62A03B556` for
`tinyllama-1.1b-chat-v1.0.Q4_0.gguf` (`../2026-09-24-E27-m9-recovery/bench07105/run.log`). This run did not
hash them again, so the same path and the same byte counts in the `.err` files are all that ties them to
those hashes.

Each `.err` file carries the loader witness, one line per device: the Vulkan loader found
`C:\Program Files\amdgpu-wddm\vulkan\radeon_icd.json` in `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` and loaded
the system `vulkan_radeon.dll` from it. No per-process ICD sat next to the benchmark. RADV reported
`AMD BC-250 (RADV GFX1013)` and the backend column of the JSON reads `Vulkan`.

## What ran, in order

`start_utc` and `end_utc` in the console outputs are UTC. The `boot=` line and the sampler timestamps are
local time, which is UTC+2 on this machine.

| UTC | Step | Artifacts |
|---|---|---|
| 18:32:02 to 18:32:13 | run 1, ceiling 1500 (`cap 1500 max 1500`) | `run1.out`, `runs/llama-b19-20261006T183202Z/` |
| 18:33:10 to 18:33:36 | run 2, ceiling 1500 | `run2.out`, `runs/llama-b19-20261006T183310Z/` |
| about 18:40 | `setmax.ps1 -Mhz 1000` wrote `DpmMaxMHz` and restarted the machine | - |
| 18:41:42 | the lab came back (`boot=2026-10-06T20:41:42` local) | - |
| 18:43:54 to 18:44:21 | run 3, ceiling 1000 (`cap 1000 max 1000`) | `run3-1000.out`, `runs/llama-b19-20261006T184354Z/` |
| 18:44:32 to 18:44:57 | run 4, ceiling 1000 | `run4-1000.out`, `runs/llama-b19-20261006T184432Z/` |
| after run 4 | `setmax.ps1 -Mhz 1500` put the release default back, with one more restart | - |

Both models ran inside each run, `stories15M` first and TinyLlama second. The GPU work of one run takes
about 5 s in total, because each test is 512 prompt tokens or 128 generated tokens, three times.

## The clock while the work ran

The runs sampled the governor once a second with `bc250kmd_cli.exe dpm 25 1000`, which writes
`runs/<stamp>/dpm.txt`. The sampler of run 1 wrote nothing (`dpm.txt` is empty and the console says
`dpm_samples=0`), so run 1 has no clock witness beyond the single `dpm 1` line in `run1.out`, which reads
`cap 1500 max 1500`.

| Run | Ceiling | Samples | Clock and voltage while busy | Idle | Tctl max |
|---|---|---|---|---|---|
| 183202Z | 1500 | 0 | not sampled | - | not sampled |
| 183310Z | 1500 | 25 | 1500 MHz 919 mV at 100.0 % and 86.7 % busy, 1000 MHz 820 mV at the 46.7 % ramp sample | 500 MHz 820 mV | 70.6 C |
| 184354Z | 1000 | 25 | 1000 MHz 820 mV at 100.0, 100.0, 90.3, 86.7 and 40.0 % busy | 500 MHz 820 mV, also at 10.0 % busy | 68.1 C |
| 184432Z | 1000 | 25 | 1000 MHz 820 mV at 100.0, 86.7, 83.9 and 63.3 % busy | 500 MHz 820 mV, also at 3.3 % busy | 68.5 C |

The 1000 MHz leg is the clean comparison. The ceiling holds the clock at `cap 1000 max 1000`, and every
sample at 40 % busy or more reads 1000 MHz. The governor still drops to its 500 MHz idle state while busy
reads 3 to 10 %: that happens at the `stories15M` tests, whose GPU work is a few milliseconds each
(samples 20:43:57, 20:44:33 and 20:44:34 local), and twice in the tail of run 4 (20:44:51 and 20:44:52).
Every
TinyLlama test of this leg (`test_time` 18:44:00Z, 18:44:02Z, 18:44:36Z and 18:44:38Z) sits inside samples
that read 1000 MHz, so the headline numbers carry the clock they state. The 1500 MHz leg is not clean: the
governor ramps 500 to 1000 to 1500 MHz, and one busy sample of run 2 still sat at 1000 MHz. The 1500 MHz
figures are therefore a lower bound for a run held at 1500 MHz.

Temperature stayed between 65 and 70.6 C, far under the 87 C rule.

## Result

TinyLlama 1.1B Q4_0, the model the Linux reference used:

| Run | Ceiling | pp512 t/s | stddev | tg128 t/s | stddev |
|---|---|---|---|---|---|
| 183202Z | 1500 | 2659.69 | 4.39 | 260.02 | 6.23 |
| 183310Z | 1500 | 2651.31 | 5.91 | 270.82 | 1.34 |
| 184354Z | 1000 | 1813.08 | 3.38 | 202.99 | 0.57 |
| 184432Z | 1000 | 1812.60 | 5.03 | 202.09 | 0.96 |

Against Linux M52 on the same unit (`../../linux/2026-09-21-E14-vulkan-compute-reference/dpm/bench-1000.txt`
and `bench-default.txt`):

| Clock | Windows b19 pp512 | Linux pp512 | Windows lead | Windows tg128 | Linux tg128 | Windows lead |
|---|---|---|---|---|---|---|
| 1000 MHz | 1813.08 and 1812.60 | 1119.59 | +62 % and +62 % | 202.99 and 202.09 | 154.92 | +31 % and +30 % |
| 1500 MHz | 2659.69 and 2651.31 | 1657.11 | +61 % and +60 % | 260.02 and 270.82 | 219.38 | +19 % and +23 % |

Against the Windows numbers of 2026-09-24 (M415: KMD 127, a fixed 1000 MHz, a per-process RADV ICD):
1115.94 prompt and 114.56 generation. At the same 1000 MHz the release is 62 % faster on prompt and 76 to
77 % faster on generation. The generation gap of the M164 to M415 era is gone on this stack. In that era
Windows ran 25 to 37 % under the Linux generation rate of 154.92 t/s: M164 98.24, M166 103.06, M255 113.88,
M274 116.79, M321 115.06 and M415 114.56 t/s.

`stories15M` is in the files for completeness only:

| Run | Ceiling | pp512 t/s | stddev | tg128 t/s | stddev |
|---|---|---|---|---|---|
| 183202Z | 1500 | 61504.58 | 14482.83 | 953.38 | 5.08 |
| 183310Z | 1500 | 49475.04 | 11107.94 | 885.94 | 5.47 |
| 184354Z | 1000 | 71297.45 | 1913.29 | 883.05 | 6.05 |
| 184432Z | 1000 | 45702.24 | 5294.22 | 952.91 | 6.52 |

That model has 15M parameters, so one test is a few milliseconds of GPU work and the numbers measure
dispatch and submission overhead plus noise. The prompt rate does not follow the clock at all (the highest
of the four is a 1000 MHz run), and the stddev reaches 24 % of the mean. Do not compare those rows with
anything. They are a load witness, nothing more.

## What this does not show

1. **Mesa versions differ.** Our ICD is a fork of Mesa main (26.3-devel) with the WDDM winsys. The Linux
   reference ran Alpine's Mesa 26.1.6. Three months of upstream RADV work sit between them, so this
   compares two stacks as they are, not two operating systems with one Mesa.
2. **The Linux reference is three weeks older** (2026-09-21) and ran on a different boot of a different OS.
   Nothing here repeats the Linux side.
3. **The backend list differs.** The Linux `llama-bench` registered a CPU BLAS backend as well (its backend
   column reads `BLAS,Vulkan`), the Windows one only Vulkan. With `-ngl 99` all layers are on the GPU in
   both, but the prompt path could still differ.
4. **n = 2 runs per clock**, three repetitions inside each. The spread between the two 1000 MHz runs is
   under 0.5 %, and between the two 1500 MHz runs it is 0.3 % on prompt and 4 % on generation.
5. **The voltages differ.** Linux ran 1000 MHz at 899 mV and 1500 MHz at 906 mV. Our governor uses 820 mV
   at 1000 MHz and 919 mV at 1500 MHz. That changes power, not throughput at a fixed clock, and this run
   measured no power at all. The smart plug was not sampled.
6. **No CU witness.** These runs printed no CU count. The lab was set back to 40 CU on 2026-10-05, and
   other runs of 2026-10-06 show `CuMode 40`, but runs 3 and 4 are after a restart and carry no such
   line. Treat the CU count of this set as unverified.
7. The 1500 MHz leg mixes clocks, as the section above states.

## Files

| Path | What it is |
|---|---|
| `bench-b19.ps1` | the lab-side script of one run: identity lines, the `dpm` sampler, the scheduled task, the two benchmarks, then the summary it prints |
| `setmax.ps1` | sets `DpmMaxMHz` and restarts the lab |
| `run.py` | pushes `bench-b19.ps1` through `tools/win/target.py` and prints its output |
| `run1.out`, `run2.out` | the console output of the two 1500 MHz runs. The result tables of run 1 are empty in this file, because the formatter ran before the benchmark flushed its JSON. The numbers are in `runs/llama-b19-20261006T183202Z/*.out` |
| `run3-1000.out`, `run4-1000.out` | the console output of the two 1000 MHz runs |
| `runs/<stamp>/stories15M.out`, `tinyllama.out` | `llama-bench` JSON, one object per test |
| `runs/<stamp>/stories15M.err`, `tinyllama.err` | the verbose log of each benchmark, with the Vulkan loader and RADV witness lines |
| `runs/<stamp>/dpm.txt` | the governor samples, one per second |
| `runs/<stamp>/run.cmd` | the command file the scheduled task ran |
| `SHA256SUMS.txt` | hashes of every file here |

No measurement file here is edited. A reader went through the `.err` files in full before the commit. They hold no
device UUID, no serial number, no address and no secret, so this directory redacts nothing.

This write-up itself changed once, before its first commit reached the public remote. A review found two errors in
it: the clock section claimed 1000 MHz for every busy sample, and two percentages rounded away from the measurement.
That change touched only this `README.md` and its line in `SHA256SUMS.txt`. The measurement files stay as the lab
wrote them.

## How to repeat

On the development PC, with the release installed on the lab and the models in place:

```
python P:\bc-250\scratch\llama-b19\run.py          # one run at the current ceiling
```

For the 1000 MHz leg, run `setmax.ps1 -Mhz 1000` on the lab through `target.py ps`, wait for the restart
with `python tools\win\target.py wait`, make the two runs, then `setmax.ps1 -Mhz 1500` and one more
restart. The script writes its artifacts to `C:\BC250\m9\llama-b19-<stamp>` on the lab. Pull that directory
to keep it. Check the `cap` and `max` fields of the `dpm` line in the console output before you trust a
clock, and read `dpm.txt` to check the clock while the work ran. An empty `dpm.txt`, as in run 1, means
there is no clock witness for that run.
