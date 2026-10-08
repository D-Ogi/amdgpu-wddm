# The Witcher 3 HIGH with RT: frame time per RT effect and one RADV knob (2026-10-07/08)

Unit A, Windows 11 Pro, 40 CU, desktop on the GPU route. Sessions 469 to 476, 2026-10-07 23:36Z to 2026-10-08
01:36Z. Every number in this file comes from the files next to it. Fact: M837.

## Question

The Witcher 3 (next-gen, D3D12) at HIGH with RT is the owner's acceptance target. Where does its frame time go,
per RT effect? Which lever gives the most frames: an RT effect, or a RADV RT knob?

## Stack

The lab ran one hand deviation for the whole series. Nothing changed between sessions (`identity-pre.txt`,
`identity-after-*.txt`).

| Part | Identity |
|---|---|
| Kernel driver | 0.7.216.20, `bc250kmd.sys` SHA-256 `7580A8F7...`, DPM ceiling 1500 MHz with the thermal soft zone |
| Desktop route | router `93F707BB...`, hosted zink, ICD `CD360941...` |
| D3D12 shell | `BBB5803E...` (adapter132, the deployed shell) |
| D3D12 engine | `348117F1...` (vkd3d-proton fork) |
| D3D12 ICD | `822134D0...` (RADV fork) |

The supervisor's lab baseline (`lab-baseline.json`) was re-pinned to this deviation before the series
(`scripts/handdev-kmd20-baseline.py`).

## Game settings

- Preset HIGH, then only the `[Rendering/RT]` keys of the arm (`scripts/w3rt-*.txt`, written by
  `scripts/make-presets.py`). RT global illumination (GI) is on whenever RT is on. Path tracing is off.
- Native 1920x1080, exclusive fullscreen (`FullScreenMode=2`), FXAA, no upscaler, no frame generation, no dynamic
  resolution. VSync off, LimitFPS 240.
- Direct start, no experiment (the shell defaults, as a Steam start of this image without an application profile).

## Method

1. **One session per arm (469 to 473).** `scripts/run-arm.sh` starts the game, loads the save in the Kaer Morhen
   room and sends the fixed input of `drive-still.sh`: one camera turn (`look:40:0:10:50`), then no input. The
   camera turn opens window B: 90 s of DxgKrnl present events, GPU only. A shot at +50 s and at +100 s, then quit.
2. **One session with in-game changes (474).** The owner asked for about one minute per setting, changed in the
   game. The operator changed the RT settings in the game's Graphics page (`scripts/rt-sweep.sh`, every menu level
   confirmed by OCR, `sweep-474.log`). One window B of 600 s covers the session. Four marks cut it into 50 s windows
   (`mark measure X start|end` in `game-log-474.txt`, cut by `etw-present-windows.py` into `split-474.txt`). Each
   window starts 8 s after the return to the world. The camera did not move.
3. **One session with a RADV knob (476).** As step 1, all four RT effects, with `RADV_PERFTEST=cswave32` in the
   game's environment (the lab marker `C:\BC250\tools\radv-perftest.txt`, written after the push and removed after
   the session).

Per window:

- the game's present rate, interval median, p95, p99 and max (DxgKrnl)
- clock, voltage, Tctl, GRBM busy and SMU power (the 1 s KMD DPM sampler)
- 3D % (Windows GPU engine counters)
- wall watts (the smart plug, about every 14 s)

The overlay's summary poll was paused (`graphics-summary.pause`) for every session.

## Clock normalisation

The KMD thermal soft zone held the clock between 1100 and 1500 MHz at Tctl 78 to 85 C. The same setting therefore
ran at different clocks in different windows. The table gives two normalised rates:

- **Frames/s per GHz**: rate / mean clock. This assumes that the rate follows the clock 1:1.
- **Frames/s at 1.5 GHz**: rate x (1.5 / GHz)^0.75. Inside the 474 windows the governor moved the clock between
  5 s slices. A linear fit of rate against clock per window (`clock-scaling-474.txt`, `scripts/clock-scaling.py`)
  gives an elasticity of 0.74 to 0.77, not 1. A part of the frame does not follow the shader clock.

## Results

| Trial | RT settings | RADV knob | Frames/s | Median ms | p95 ms | p99 ms | Clock MHz | Tctl C | Plug W | Frames/s per GHz | Frames/s at 1.5 GHz (e 0.75) | Frame ms at 1.5 GHz |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 469 | GI perf + reflections + shadows perf + AO (all four) | none | 12.8 | 78.1 | 85.4 | 92.0 | 1457 | 79.8-84.4 | 177.3 | 8.79 | 13.1 | 76.4 |
| 470 | GI perf only | none | 26.0 | 38.6 | 42.4 | 44.0 | 1367 | 81.1-84.2 | 180.5 | 19.02 | 27.9 | 35.9 |
| 471 | GI perf + reflections | none | 18.1 | 54.6 | 61.2 | 62.6 | 1452 | 78.2-84.6 | 179.8 | 12.47 | 18.5 | 53.9 |
| 472 | GI perf + shadows perf | none | 17.4 | 57.1 | 67.6 | 68.8 | 1335 | 81.8-85.0 | 179.0 | 13.03 | 19.0 | 52.7 |
| 473 | GI perf + AO | none | 21.6 | 45.9 | 51.0 | 53.4 | 1421 | 79.0-84.8 | 180.1 | 15.20 | 22.5 | 44.5 |
| 476 | GI perf + reflections + shadows perf + AO (all four) | cswave32 | 12.6 | 77.4 | 90.8 | 96.3 | 1416 | 77.8-85.0 | 175.8 | 8.90 | 13.2 | 76.0 |
| 474 A-ctl | all four (in-session control) | none | 13.2 | 75.7 | 81.0 | 86.5 | 1500 | 78.6-83.9 | 181.7 | 8.80 | 13.2 | 75.8 |
| 474 noAO | GI perf + reflections + shadows perf (= high-rt preset) | none | 12.5 | 81.5 | 84.9 | 86.2 | 1254 | 82.5-84.2 | 158.2 | 9.97 | 14.3 | 69.9 |
| 474 rt-off | RT off | none | 44.0 | 22.6 | 25.3 | 26.2 | 1104 | 82.2-84.6 | 158.7 | 39.86 | 55.4 | 18.1 |
| 474 gi-q | all four, GI Quality | none | 10.0 | 101.3 | 109.6 | 110.5 | 1218 | 82.0-84.2 | 151.3 | 8.21 | 11.7 | 85.5 |

- GPU busy is 98 to 100 % in every window (GRBM busy from the DPM sampler, DxgKrnl DMA union in 474). Every
  window is GPU-bound.
- Wall power 151 to 182 W mean, 193 W max. SMU power 84 to 98 W. Tctl max 85.0 C in the windows and 85.5 C in
  474 between two windows. No runner thermal stop.

## Cost of each RT effect

Frame time at 1.5 GHz (last column of the table). The effects on top of GI come from the sessions 470 to 473. The
in-session control A-ctl (75.8 ms) agrees with session 469 (76.4 ms) within 0.8 %.
So the sessions 469 to 473 and the 474 windows compare.

| Step | Frame ms at 1.5 GHz | Added ms | Source |
|---|---|---|---|
| RT off | 18.1 | - | 474 rt-off |
| + RT GI Performance | 35.9 | +17.8 | 470 |
| GI + reflections | 53.9 | +18.0 | 471 against 470 |
| GI + shadows Performance | 52.7 | +16.8 | 472 against 470 |
| GI + AO | 44.5 | +8.6 | 473 against 470 |
| all four | 76.4 / 75.8 | | 469 / 474 A-ctl |
| all four, AO off (the game's high-rt preset) | 69.9 | AO = +5.9 | 474 noAO against A-ctl |
| all four, GI Quality | 85.5 | GI Quality = +9.7 | 474 gi-q against A-ctl |

- The single costs on top of GI add to 79.3 ms. The measured all-four frame is 76.4 ms. The effects share about
  3 ms of work.
- RT costs 76 % of the all-four frame (58 of 76 ms). GI, reflections and shadows cost about the same, 17 to 18 ms
  each. AO costs 6 to 9 ms.

## RADV knobs

One session, 476: all four effects with `RADV_PERFTEST=cswave32` (compute shaders in wave32 instead of wave64,
which includes the compute shaders that trace with ray queries). The game log records `radv_perftest cswave32`.

| | Frames/s | Clock MHz | Frames/s per GHz | Frames/s at 1.5 GHz | p95 ms |
|---|---|---|---|---|---|
| 476 cswave32 | 12.6 | 1416 | 8.90 | 13.2 | 90.8 |
| 474 A-ctl, no knob | 13.2 | 1500 | 8.80 | 13.2 | 81.0 |
| 469, no knob | 12.8 | 1457 | 8.79 | 13.1 | 85.4 |

- At 1.5 GHz cswave32 gives the same rate as the controls (13.2 against 13.2 and 13.1). Per GHz it is 1.1 % above
  A-ctl. With the 0.75 elasticity, the lower clock alone predicts 8.93 per GHz at 1416 MHz. The knob adds nothing
  that this series can measure.
- The clock of 476 was 5.6 % below A-ctl and 2.8 % below 469. The comparison therefore depends on the clock
  normalisation.
- The p95 frame time is 10 ms higher than A-ctl's. One session cannot separate this from session noise.
- Result: cswave32 is not a lever for this frame.

## Levers, from the all-four frame (13.1 frames/s at 1.5 GHz)

| Lever | Frames/s at 1.5 GHz | Gain | How it was measured |
|---|---|---|---|
| RT off | 55.4 | x4.2 | 474 rt-off |
| reflections off | about 17.1 | about +31 % | 76.4 - 18.0 ms, from 470/471 |
| shadows off | about 16.8 | about +28 % | 76.4 - 16.8 ms, from 470/472 |
| AO off (high-rt preset) | 14.3 | +8 % | 474 noAO against A-ctl (13.2) |
| GI Quality instead of Performance | 11.7 | -11 % | 474 gi-q against A-ctl (13.2) |
| RADV cswave32 | 13.2 | 0 % | 476 against A-ctl (13.2) |

The two "about" rows are sums of measured steps, not measured frames.

## Unusual events

- **468 did not run.** The owner's display mode changes moved the KMD epoch between run-m157's start-confirm step
  and the Capture witness (health flags 7, the confirmed bit missing). The start was confirmed again, and 469 ran the
  same arm.
- **Postflight refusal after every session.** The game's exclusive 1920x1080 does a real mode commit (stretched to
  the panel's 1920x1200). The in-trial KMD streams of 469 to 472 and 476 have these commits (`kmd-scan-*.txt`).
  The streams of 473 and 474 have no mode-set line, but their closures refused in the same way. The commit moves
  the KMD epoch. The supervisor's `Verify` step then refuses ("Confirmed CPU
  baseline required") and the closure reads recovery-unverified. After each session the runner confirmed the start
  again (flags 15), pulled the archive and read the stack identities (`identity-after-*.txt`). Nothing changed. The
  defect itself belongs to the lead's backlog.
- **472 has no GPU engine counter file.** The counter sampler of that session wrote nothing. GRBM busy from the
  DPM sampler (98.7 %) stands in.
- **475 (rtwave64) was refused before any GPU work.** The preflight's stale-marker gate (from trial 251) admits a
  RADV_PERFTEST marker only when it is newer than the attempt directory that the push creates. The runner wrote it
  before the push. `run-arm.sh` now writes it when run-slot prints "package flushed" (476).
- **Owner key presses at the intro.** In 474 the owner pressed Space three times at the intro trailers, and in 476
  once at about 01:28Z while the game waited at the intro. Both presses came before the menu. No measurement window
  contains them.
- **474 operator events.** One Esc did not open the pause menu, and the next Esc opened the LOAD GAME list.
  The operator left it with Esc and loaded nothing. One Down was lost on the way to the VIDEO page. The GAMEPLAY
  page opened, and nothing changed there.
- **474 has no second control window.** Window B (600 s) opened at the world arrival, not at the first camera turn.
  It ended before a fifth window fitted.
- **A second process presents with the game.** In every window B a process with pid 8004 (the same in every
  session of this boot) presents once per game frame. That process is `dwm.exe`. The game's exclusive fullscreen
  frames therefore go through DWM composition, not independent flip (M15.14).
- No GPU fault, TDR, removed device or bugcheck. The in-trial KMD streams (`kmd-scan-*.txt`) have no fault line.
  Every shot has the Kaer Morhen room with the RT effects of its arm (`shot-*.jpg`, `sweep-474-*.jpg`).

## Knobs that were not run

- **rtwave64** (RT pipelines in wave64). Not run. Trial 251 (2026-10-01) bugchecked 0x116 at the world load with
  this knob, and its dump analysis names the knob as the most likely cause. BD-074: a ray-query CTS case returns a
  wrong value under wave64. A measurement of this knob needs the BD-074 CTS cases first.
- **BVH build quality.** The ICD of this stack has no such knob for gfx10. The BVH builder (PLOC or LBVH) follows
  the application's build flags. `cswave32` (476) stands in as the second RADV knob.

## Next steps

1. The lever with the largest gain is the game's own RT reflections setting (about +31 %), then RT shadows (about
   +28 %). Neither is a driver change. For the driver, GI, reflections and shadows are three traced passes of
   17 to 18 ms each at 1.5 GHz. A profile of one of them (RGP-class timing per dispatch, or ETW per command
   buffer) is the next measurement. It tells whether BVH traversal, the ray-query compute shaders or the denoise
   passes take the time.
2. The rate follows the shader clock with an elasticity of about 0.75. The DPM ceiling of 1500 MHz and the thermal
   soft zone (1100 to 1500 MHz at Tctl 78 to 85 C) therefore cost frames directly. A higher ceiling with the same
   thermal headroom is the second lever.
3. rtwave64 needs the BD-074 CTS cases (`rt-k97-di1.txt`, `rt-k97-di.txt`) before any game session.
4. The DWM composition of the exclusive fullscreen game (pid 8004 above) belongs to M15.14. Independent flip
   would remove one composition per game frame.

## Files

- `plan-N.md`: the trial plans with results. `window-B-N.txt`: the runner's window B lines.
- `game-log-N.txt`, `game-result-N.txt` (with the RT keys that the game left), `settings-step-N.txt`.
- `dpm-window-B-N.txt`, `gpu-counters-window-B-N.txt`, `plug-N.log`, `kmd-scan-N.txt`, `identity-after-N.txt`.
- `results.json` (per session), `sweep-474.json` (per 474 window), `split-474.txt`, `clock-scaling-474.txt`,
  `sweep-474.log` (operator log with every OCR read).
- `scripts/`: the runners, presets and analysis scripts. `sha256.txt`: hashes of every file.
