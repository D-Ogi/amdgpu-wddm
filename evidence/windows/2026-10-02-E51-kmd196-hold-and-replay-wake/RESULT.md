# E51: where the GPU was idle - the kernel driver's held submission, and the shell's deferred-replay wake

Date: 2026-10-02. Unit A, Windows 11 Pro build 22631. The Witcher 3 next-generation DirectX 12 edition
(build 25646871, 5.0.0.1044392), started through the Steam client, at native 1920x1080 with FXAA, no upscaler,
frame generation off and dynamic resolution off. Desktop composed on the GPU in every session
(`prestate.tsv`, DWM modules column: the router `674AD261`, the Zink desktop UMD `18BFC610` and the hosted RADV
`66FE8F31`).

Two changes are measured here, each against its own reference:

1. **The kernel driver.** On 0.7.193.1 a submission that arrived while another process's job held VMID 1's root
   was refused and retried on a relative `KeDelayExecutionThread(1 ms)`. 0.7.196.1 replaces that with a bounded
   500 us spin and then a wait on the graphics retirement event. Trials 313-316 ran the earlier driver, 317, 318
   and 320 the later one, with the rest of the stack held fixed within each preset pair.
2. **The D3D12 shell.** Two commits on `m15/replay-wake`: `2322765d` wakes a sleeping deferred-replay worker once
   8 KiB are pending instead of on the first publish (shell `406FD683`, trial 305), and `e9f5e701` drains no ring
   at `ResetCommandPool` while no list of that pool is open (shell `5A1B7BAF`, trials 306 and 307). `5A1B7BAF` is
   the shell registered on unit A since 2026-10-02T17:13Z.

## Method

- **Rate and frame interval** come from the GPU ETW session over a 40 s window taken during a walk through the
  game world, 130 s or more after the game appeared, with the trial's own `etw-notes-NNN.txt` recording when the
  window opened and closed. `gpu-NNN-summary.txt` is that session's summary, unedited. The game's rate is the
  `Presents by process` line of its own process id; `supervisor-NNN.json` gives that id under `game.pid`. The
  other process at a similar rate in the same list is the compositor.
- **Where the GPU's time went** comes from a second, independent instrument: `tools/win/gpu-timeline`, which
  point-samples `GRBM_STATUS` and the command-processor status registers about 930 times a second for 60 s and
  classifies each sample as idle, pipeline (and whether the command processor is fed or draining) or command
  processor alone. `timeline-NNN.txt` is the analyser's output, regenerated for this directory from the stored
  GTL1 file of each run; `sources.tsv` names that file and its SHA256. The per-frame lines in it are computed
  from the rate in the header of each file, which is the rate the ETW session measured.
- **The kernel driver's own view of a hold** comes from its guard log, streamed during the session.
  `kmd-holds-NNN.txt` keeps every line of that log mentioning a held submission, including the summary line and
  the nine-bucket histogram. 0.7.193.1 has no such counters, which `kmd-holds-314.txt` records as an empty file
  rather than leaving to inference.
- **Closure.** Each trial ran under the lab's native-caps kit: it captured the registration and the machine state
  (`prestate.tsv` is read from that capture), swapped candidate DLLs into the registered directory where the plan
  asked for it, ran the session under a bounded timer, then restored and verified the registered DLLs.
  `supervisor-NNN.json` holds the closure, the SHA256 of the triplet that was loaded, and the module list the
  game process actually had mapped. `functional-restored` means the session's checks passed, the process tree
  closed and the baseline came back.
- **What the plan asked before the trial ran** is in `plan-NNN.md`: question, expectation and refutation
  condition, written before the session. Several expectations were not met, and the files say so.
- Each `ocr-NNN-*.json` is the text the lab's own OCR read from one of the operator's screenshots: it places the
  session in the game rather than at a menu. The screenshots themselves stay in the workspace, because they show
  the lab desktop.
- Removed from this directory on purpose: the `saves` block of each trial's closure record, which inventories the
  owner's save games. The development machine's own drive path, which the plans and the timeline outputs printed,
  is written `<BC250_ROOT>` here, as the rest of the repository writes it. Nothing else was taken out of or
  changed in the published files.

## 1. The kernel driver: a held submission waiting on a clock tick (0.7.193.1)

`driver/kmd/wddm.c` carries the measurement that led to the change, taken on trials 313 and 314 with the GPU
timeline and dxgkrnl's ETW aligned to the microsecond:

- the compositor's job is 0.29 ms long (p10-p90 0.26-0.31);
- dxgkrnl's node-0 worker calls `SubmitCommand` for the game's next packet 0.14 ms after that job starts, about
  0.13 ms **before** the job's completion interrupt, so the refusal happens with roughly 0.13 ms left to wait;
- the `KeDelayExecutionThread(1 ms)` that followed the refusal lasted p50 4.2 ms and p90 5.6 ms in the HIGH
  session and p50 2.3 ms and p90 14.2 ms in the LOW one. A relative sleep expires on a clock tick, so the
  "1 ms" was a tick;
- readied-to-running was 0.04-0.06 ms, so no thread was starved of CPU: the timer fired late;
- cost: about 4.7 ms per handover from the compositor to the game, 0.6-1.0 holds per frame, 2.9-4.0 ms of
  graphics idle per frame.

The timeline agrees from the other side. On 0.7.193.1 the graphics pipe was idle 5.70-5.92 ms per frame at both
presets, a cost that did not move when the rendering work per frame doubled:

| trial | preset | kernel driver | frames/s | GPU idle | idle ms/frame | pipeline ms/frame |
|---|---|---|---|---|---|---|
| 313 | low | 0.7.193.1 | 54.6 | 32.3 % | 5.92 | 11.88 |
| 315 | low | 0.7.193.1 | 54.3 | 32.0 % | 5.89 | 12.01 |
| 314 | high | 0.7.193.1 | 31.3 | 18.3 % | 5.83 | 25.54 |
| 316 | high | 0.7.193.1 | 31.3 | 17.9 % | 5.70 | 25.64 |

## 2. The kernel driver: 0.7.196.1, a 500 us spin and then the retirement event

`BC250_WDDM_HOLD_SPIN_US` is 500, in steps of 20 us, so 25 retries fill the budget; phase 2 waits on the
graphics retirement event with a 1 ms fallback timeout for a lost end-of-pipe interrupt. The spin runs at
APC_LEVEL or below and reads the fence page directly on every step, so it does not depend on the fence DPC being
scheduled. No `KeDelayExecutionThread` remains in either phase.

| trial | preset | kernel driver | frames/s | GPU idle | idle ms/frame | pipeline ms/frame |
|---|---|---|---|---|---|---|
| 317 | low | 0.7.196.1 | 59.6 | 23.7 % | 3.98 | 12.32 |
| 318 | high | 0.7.196.1 | 36.8 | 4.3 % | 1.18 | 25.46 |
| 320 | high with RT | 0.7.196.1 | 10.1 | 3.0 % | 2.93 | 95.18 |

The pipeline time per frame is the control: 11.88 and 12.01 ms before against 12.32 ms after at low, 25.54 and
25.64 ms before against 25.46 ms after at high. The rendering work did not change. What changed is idle time:

- **high**, the clean pair (314 and 318 ran the same shell `A27C9617`, engine `D4057452` and ICD `102D77EC`;
  only the kernel driver differs): 31.3 -> 36.8 frames/s, +17.6 %. Idle fell 5.83 -> 1.18 ms per frame, that is
  -4.65 ms, against a frame time that fell 31.95 -> 27.17 ms, that is -4.78 ms. The gain is the idle time.
  Graphics idle fell from 18.3 % to 4.3 % of the window.
- **low**: 54.3-54.8 -> 59.6 frames/s, about +9 %. Idle fell 5.89 -> 3.98 ms per frame (-1.91) against a frame
  time that fell 18.42 -> 16.78 ms (-1.64). **This is not a clean A/B**: 317 ran the shell, engine and ICD of
  313-316's artifact12 set, and no pre-196 low session ran exactly that set, so the references are 313 (another
  shell) and 315 (another engine and ICD). The low rate is also measured with the ETW kernel session running,
  which later measurement priced at about 1.5 ms per frame on this preset.
- **high with RT** (320) ran the registered triplet, shell `5A1B7BAF`, engine `D79FEC49`, ICD `F9DCB33B`: 10.1
  frames/s against 9.5 in trial 307 on 0.7.193.1 with the same triplet. That is +6 %, but trial 277 read 10.5
  frames/s on an earlier stack, so this preset's rate is not a clean monotone series and the +6 % is not load
  bearing. What is clean is the idle: 3.0 % of the window, and 95.18 of the 99.01 ms frame in the pipeline.

The kernel driver's own counters say the spin is doing all the work:

| trial | holds | of them resolved by the spin | mean held | worst held | spin steps | over 1 ms |
|---|---|---|---|---|---|---|
| 318 | 10177 | 10177 | 199 us | 618 us | 47151 | 0 |
| 320 | 17601 | 17601 | 200 us | 810 us | 80879 | 0 |

No hold reached the event wait and none reached the fallback timeout, in either session. The nine-bucket
histogram in `kmd-holds-318.txt` puts 85 holds under 100 us, 5039 in 100-200 us, 5048 in 200-500 us, 5 in
500-1000 us and none above. The event phase is insurance, not the mechanism.

The lesson, recorded because it cost two sessions to find: a relative `KeDelayExecutionThread` of "1 ms" on
Windows is a clock tick, not a millisecond, and no wait on a submission path may rest on one.

## 3. The D3D12 shell: the deferred-replay wake threshold and the pool drain

All four low sessions ran on kernel driver 0.7.193.1 with the engine `D79FEC49` and the ICD `F9DCB33B`, the
registered pair at the time, and differ only in the shell and in whether the application profile carried
`deferred-replay`:

| trial | shell | bc250-win | deferred replay | frames/s | GPU busy | interval median | p99 |
|---|---|---|---|---|---|---|---|
| 300 | `DCE50410` | `f33f9754` | off | 51.5 | 71.9 % | 18.66 ms | 32.76 ms |
| 304 | `DCE50410` | `f33f9754` | on | 52.4 | 80.9 % | 18.23 ms | 35.05 ms |
| 305 | `406FD683` | `2322765d` | on | 53.6 | 82.6 % | 17.76 ms | 34.85 ms |
| 306 | `5A1B7BAF` | `e9f5e701` | on | 54.8 | 92.0 % | 16.98 ms | 35.49 ms |

The ladder is monotone: +1.8 % for turning deferred replay on with the old shell, +2.3 % more for waking the
worker on 8 KiB of pending work instead of on the first publish, and +2.2 % more for not draining a ring at
`ResetCommandPool` while no list of that pool is open. 51.5 -> 54.8 frames/s is +6.4 % over the four steps, and
GPU busy over the window rose from 71.9 % to 92.0 %.

Trial 307 ran `5A1B7BAF` at the high-with-RT preset on the same kernel driver: 9.5 frames/s, interval median
106.78 ms, p99 126.38 ms, GPU busy 97.9 % of the window, with ray tracing on through the application profile.

Every one of 300, 304, 305, 306 and 307 closed `functional-restored` with the game still responding and stopped
by the operator inside the session bound, and with all three of our modules mapped in the game process
(`supervisor-NNN.json`, `game.modules`). The shell was promoted to the registered one on trial 308, and trial
309 is the check on the promoted artifact: feature level 12_1, tiled resources tier 3, three of three modules,
published in `2026-10-02-E50-native-d3d12-system-runtime` and cited by M780.

## What this does not show

- No session here is a conformance run, and the rates are one unit, one game, one 40 s window per trial.
- The low-preset rates carry the ETW kernel session's own cost, later priced at about 1.5 ms per frame; the
  numbers in this directory are therefore comparable with each other and not with later sessions measured on
  GPU-only windows.
- The low pair across the kernel driver change is not a clean A/B, as section 2 states.
- Image correctness was the operator's check on four screenshots per session; the screenshots stay in the
  workspace, and the OCR text published here only places each session in the game world.
- The timeline's class split rests on a point-sample assumption. Each `timeline-NNN.txt` prints how often the
  class changed between the first and the last `GRBM_STATUS` read of a sample, which is that assumption's error
  bar, and the batch-means standard error of every share.
