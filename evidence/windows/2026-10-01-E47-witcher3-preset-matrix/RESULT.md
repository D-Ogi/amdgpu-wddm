# E47: The Witcher 3 at low, high and high with RT on one accepted build (M15.7, M15.8)

Date: 2026-10-01, 11:59-12:39Z. Unit A, Windows 11 Pro build 22631. One build for all three sessions:

- Kernel driver 0.7.193.1 (SYS E3F75D80, ABI 0x000700C1; commit 0289c1a5 on kmd193-hang-witness), DPM on with the
  2000 MHz ceiling, thermal limits unchanged (cap steps from 87 C).
- D3D12 triplet: shell 6D22D305 (adapter107, bc250-win 2d410d5d on m15/adapter107-release-gate), engine 15E3E24E
  (vkd3d-proton fork e1ce3e7e), ICD 2A13235D. In 246 the shell was the candidate; 247 promoted it, and 249 and 250
  ran on the registered triplet with nothing swapped.
- Desktop composed on the GPU (router, hosted zink, ICD).
- Game settings: native 1920x1080 fullscreen, FXAA (no upscaler), frame generation off, dynamic resolution off,
  LimitFPS 60; presets low, high, and high with RT (EnableRT with the preset's RT effects).

Method: the native-caps kit's game session (`game-session.sh`, 7-minute bound, ended by the bound), steered
interactively along the Kaer Morhen interior route with the remote input channel. The kit's window B is a 40 s
DxgKrnl present-interval window opened by the operator's walk marker. Task Manager's GPU engine counters for the
adapter's LUID are sampled once a second (`gpu-counters-*.txt`). The HTTP temperature guard prints the KMD's DPM
reading about every 30 s (`dpm-guard-*.txt`). After session 250 the KMD log summary (`kmd-log-after-250.txt`) and
the CP/GRBM/GCVM registers (`gcvm-snapshot-after-250.txt`) were read with the kernel driver's CLI. The session
write-ups are `result-*.md`; journal rates are in `journal-rates.md`.

| session | preset | window B frames/s | median / p99 / max ms | 3D engine in window B | clock in the world | Tctl peak |
|---|---|---|---|---|---|---|
| 246 | low | 45.3 | 21.55 / 35.08 / 53.6 | 65.4 % | 1000 MHz, one raise to 1800 | 78.0 C |
| 249 | high | 31.3 | 31.10 / 60.84 / 133 | 96.0 % | 2000 MHz, one 1900 step by the thermal cap | 86.0 C |
| 250 | high + RT | 9.3 | 107.02 / 195.34 / 220 | 97.8 % | 2000 MHz | 86.5 C |

- Every session closed functional-restored. Every shot was correct, with RT reflections and lighting in 250. No
  streaming noise was seen.
- No bugcheck, no fence timeout, no TDR. After 250 the KMD summary reads node 0 158616 submitted, 0 timeouts,
  0 refused, "no TDR".
  - The gap between submitted and completed is not lost work. The completed counter counts fence interrupts that
    retired at least one job, so one interrupt can complete several submissions.
  - The GCVM protection fault status and address read 0 at 12:42:18Z. Driver 0.7.193.1 never clears that latch, so
    no VM fault occurred from its load at 11:51Z through 250.
- Reading: high and high with RT are GPU-bound (3D engine near 100 % at 2000 MHz). High with RT at 2000 MHz equals
  trial 223 (9.1 frames/s at an 1800 MHz thermal cap on driver 0.7.182.1), so that frame does not scale with the
  shader clock. At low the GPU is busy about 65-70 % at 1000 MHz.
- Caveat: in 246 a duplicate launcher ran its own GPU-counter sampler into `gpu-counters-246.txt` until 12:02Z
  (operator error, `result-246.md`). Window B (from 12:04:03Z) comes after that.

Facts: M775.
