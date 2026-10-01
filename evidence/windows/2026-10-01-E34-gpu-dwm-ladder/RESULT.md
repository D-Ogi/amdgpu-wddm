# GPU DWM promotion ladder T1-T7 and promotion (2026-10-01)

Unit A, KMD 0.7.182.1 (SYS 3CA0CF81), DPM ceiling 2000 MHz, 40 CU. Desktop route on the GPU: router 5BBEB783
registered in UserModeDriverName slots 2 and 3, hosted zink E6B944CF and ICD 66FE8F31 in DWM, interop switches 1.
CPU route: the same router with the CPU desktop UMD 4176D1DF. Each trial's result section is copied here as
`result-NNN.md`; raw logs stay in the local trial directories they name.

| Step | Trial | Result |
|---|---|---|
| T1 | kit attempt 001 | switches latched 0x303, CPU desktop exact, health flags 15 |
| T2 | kit attempt 002 | DWM composed on router + zink + ICD 66FE8F31 (rehearsal), restored |
| T3 | kit attempt 003 | router registered; DWM restarted onto the GPU route and back, flags 15 both |
| T4 | 212, 216 | 10-bit Present client under GPU DWM: frames 1 and 3 exact, frame 2 613 against 614 (BD-049) |
| T5 | 213, 215 | 8-bit Present client under GPU DWM: three frames exact, 0 kernel software blits |
| T6 | 217 | Witcher 3 HIGH 7 min on the GPU route: 35.5 frames/s (CPU route 214: 21.1) |
| T7 | 218 | the same again: 35.4 frames/s |

215 and 216 repeat T5 and T4 with clients rebuilt from main d1f17e6d (A8CC131E, D33810B3); 212/213 used clients
014/024.

## Game sessions (214 against 217/218)

Same engine (106D09E5), same game ICD (the use-after-free diagnostic build DC70A5E9), preset HIGH, 1920x1080
fullscreen, DRS and upscalers off, RT off by the preset, the same scripted route; only the desktop route differs.

| | 214 CPU route | 217 GPU route | 218 GPU route |
|---|---|---|---|
| window B rate (40 s, `window-B-NNN.txt`) | 21.1/s, median 48.1 ms | 35.5/s, median 26.7 ms | 35.4/s, median 27.9 ms |
| p99 frame interval | 75.2 ms | 62.4 ms | 52.4 ms |
| DPM (`dpm-summary-NNN.txt`) | 0 raises, 1000 MHz, busy 31-46 % | 120 raises, 2000 MHz at busy 69-77 % | 216 raises, 496 lowers |
| DxgKrnl DMA busy (`etw-gpu-B-summary-NNN.txt`) | 36.2 % of 52.8 s | 79.2 % of 49.5 s | 73.3 % of 50.3 s |
| kernel software blits | 0 | 0 | 0 |

The owner played 214 by hand alongside the route and saw no glitch; 214's scene therefore differs in detail.

## Task Manager's counters (`gpu-counters-NNN.txt`, `gpu-counters.ps1`)

One sample a second of `\GPU Engine(*)\Utilization Percentage`, `\GPU Adapter Memory(*)\Dedicated Usage` and
`Shared Usage` for the BC-250 LUID, `\Process(dwm)\% Processor Time` and `\Processor(_Total)`. In 217's world
window (01:28:40-01:32:20Z, 160 samples) the 3D engine averaged 61.7 % (maximum 94.7 %) attributed to the game's
process, dedicated memory reached 2820 MB, DWM used 8.6 % of one logical CPU and the machine 55.4 %. The CPU route
at idle had DWM at 21-49 % of one logical CPU (01:21Z). In the Present client trials 215/216 the 3D engine stayed
at or below 1.5 %, attributed to the GPU DWM.

## Promotion

`route.py 003 gpu` at 01:50:12Z (exit 0): DWM on the GPU route by default (DwmForceCpu 0), recorded as promotion,
not as M13.4 (no 30-minute run, owner decision of 2026-09-30).
