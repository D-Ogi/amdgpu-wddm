# amdgpu_wddm_frameloop - a controlled D3D12 frame loop for the per-frame idle gap

A native D3D12 client that runs a game-shaped frame loop with a *calibrated* amount of GPU work per frame and
reports, per frame, where the time went on the CPU and on the GPU. It exists for one question: on the lab unit
a D3D12 game leaves the graphics pipe idle for about 5.9 ms per frame at both 54.8 and 31.3 fps, a fixed cost
that does not scale with the GPU work and therefore looks like latency in our frame loop rather than like
rendering. This client reproduces that loop with known work, so the gap can be measured and attributed without
a ten-minute game session.

The decisive output is the split of the idle time:

```
gpu idle per frame ms     the windows in which the graphics pipe had nothing running
  of it: awaiting submit  the part before the next work had even been submitted  -> a CPU-side cost
  of it: after submit     the part after it was submitted and the GPU still had not started it -> driver/KMD
```

On this development PC's RTX 4090 the second number is 0.115 ms per frame with 7 lists. If the lab shows
several milliseconds there, the cost is in our submission path; if it shows up under "awaiting submit", it is
the CPU half of the loop (fence wake, recording, Present).

## Where it lives

This directory holds the sources and the harness. Nothing built or measured is kept here: `src\build.ps1`
writes the client to `<BC250_ROOT>\scratch\m15\frameloop\build`, and `lab\run-frameloop.py` writes the
plan of a trial and the pulled results under `<BC250_ROOT>\scratch\m15\frameloop\lab`.
`BC250_FRAMELOOP_WORK` moves that work directory; `BC250_ROOT` is the workspace root, by default the parent
directory of this repository.

## What the loop does

Per frame, in order:

1. optional wait on the swap chain's frame-latency waitable object (`--frame-latency-waitable N`);
2. wait until frame `N - L` finished on the GPU: `SetEventOnCompletion` + `WaitForSingleObject` on that
   frame's fence, as a game does, never a `GetCompletedValue` poll that would hide the wake latency;
3. read back the retired frame's GPU timestamps;
4. optional CPU busy work (`--cpu-ms X`);
5. for each of `K` command lists (`--lists K`): reset it, record it, close it, and submit it in **its own**
   `ExecuteCommandLists` call, like the game's ~7 submissions per frame. Each list brackets itself with two
   timestamp queries. Lists `0..K-1` each run one calibrated compute dispatch; the last list additionally
   clears the back buffer, draws one fullscreen triangle that reads the buffer the dispatches wrote, and
   resolves the frame's queries;
6. `Signal` the frame's fence, then `Present`.

The draw reads what the compute wrote, so a presented frame cannot be produced without that frame's GPU work:
the timings describe one dependent pipeline, not two unrelated streams.

**Runtime**: `d3d12.dll` and `dxgi.dll` are loaded by full `System32` path with `LOAD_LIBRARY_SEARCH_SYSTEM32`,
and the exe links neither import library, so an application-local copy next to it cannot be picked up.

**Back buffer format**: `DXGI_SWAP_EFFECT_FLIP_DISCARD`, `DXGI_SCALING_STRETCH`, `DXGI_ALPHA_MODE_IGNORE`,
`MakeWindowAssociation(DXGI_MWA_NO_ALT_ENTER)` - the configuration known to work on our driver
(`bc250-win\tools\win\d3d12queue\interactive-present.h`). The format is named in exactly one constant,
`kBackBufferFormat` in `src\main.cpp`. Nothing else assumes 8 bits per channel: there is no byte-level readback
of a back buffer, the clear takes a `float4` and the pixel shader writes a `float4`, so moving to
`R10G10B10A2_UNORM` or an HDR format is that one constant plus the colour-space call.

## Calibration

`--gpu-ms T` is met in two stages.

1. **Isolated**: one dispatch at a time on an idle queue. A two-point fit (64 and 1024 iterations) gives the
   slope and the fixed intercept, then up to six refinement rounds.
2. **Sustained**: up to three short (0.5 s) passes of the *real* frame loop, each correcting the iteration
   count against the GPU busy time the loop actually produced. This matters on a GPU that boosts: on the 4090
   the isolated fit was 14 % low because the sustained load drops the clock. The lab boosts too since the KMD
   DPM shipped (load-driven clock, a 1500 MHz ceiling, a 500 MHz idle state and a thermal step under 1000 MHz),
   so these rounds do move there, and two arms run back to back are two operating points. Each run therefore
   records its own: `index.json` carries `point_before` and `point_after` per run (`clock read`: MHz, VID,
   temperature) and the set carries the `cu_record` from the driver's Parameters key. Read them before any
   comparison; K178 is the precedent, where every game rate of sessions 347-403 was silently a 24 CU number.

The dispatch shape stays fixed for a whole run (`--groups G`, 64 threads per group); only the iteration count
is calibrated. `calibration.gpu_busy_error_pct` in the JSON is the measured miss against what was asked, and
the client prints `OUTSIDE 10 %` when it misses by more than that.

**Floor.** `K` lists cannot produce less GPU work than `K` times the cost of one nearly empty dispatch
(`calibration.dispatch_floor_ms`, `frame_floor_ms`). Asking for less is refused by physics, not by the client:
it warns on stderr at startup and flags the error at the end. On WARP with 7 lists the floor is about 2 ms per
frame, which is why the `--gpu-ms 2 --lists 7` WARP row below misses by 17 %.

## Usage

```
amdgpu_wddm_frameloop [options]
  --seconds S              run length, 1..60 (default 10)
  --gpu-ms T               GPU work per frame in ms, 0..100 (default 10; 0 = clear and draw only)
  --lists K                command lists (and ExecuteCommandLists calls) per frame, 1..64 (default 7)
  --latency L              frames in flight: wait for frame N-L, 1..8 (default 2)
  --buffers N              swap chain buffers, 2..4 (default 2)
  --present-interval N     Present sync interval, 0..4 (default 0)
  --tearing                request ALLOW_TEARING (only taken if the factory supports it; needs interval 0)
  --frame-latency-waitable N   waitable swap chain, SetMaximumFrameLatency(N), 1..16
  --cpu-ms X               CPU busy work per frame in ms, 0..100 (default 0)
  --groups G               thread groups per dispatch, 1..65536 (default 2048)
  --record-threads N       threads recording the K lists in parallel, 1..16 (default 1; needs --submit
                           sequential or batch, and is capped at --lists)
  --submit MODE            interleaved (default: record a list, submit it, one call each), sequential (record
                           all, then one call each), batch (record all, then all K in one call)
  --queues N               direct queues the frame's lists are spread over, 1 or 2 (default 1)
  --handoff MODE           with --queues 2: none (default, the second queue stays idle), per-list (list k on
                           queue k%2, each waiting on the fence the previous list signalled), per-frame (the
                           frame's lists on queue frame%2, one crossing a frame)
  --signal-per-list        Signal the handoff fence after every ExecuteCommandLists, not once per frame
  --size WxH               borderless window of this size instead of the whole output
  --warp | --adapter-luid HI:LO | --adapter-index N   adapter choice (default: first hardware adapter)
  --ts-hz HZ               the frequency the GPU timestamp counter really runs at, 1e6..1e10, used for every
                           conversion and for the --gpu-ms calibration instead of what GetTimestampFrequency
                           reports, with the clock origin fitted from bracketed dispatches
  --calibrate-every N      GetClockCalibration every N frames, 1..4096 (default 32)
  --warmup N               frames excluded from the distributions, 0..1000 (default 10)
  --raw-frames N           per-frame records written to the JSON, 0..131072 (default 20000)
  --frame-statistics       also call GetFrameStatistics after each Present, which is what the vblank grid
                           and the phase of every GPU gap are fitted from
  --vblank-phase-us US     a gap counts as vblank-aligned when it ends this close after a vblank,
                           10..5000 (default 300); the JSON also reports the uniform share of that window
  --icd-log PATH           read the winsys knobs and counters back from this ICD log (default: the path
                           BC250_DEFERRED_LOG names, else the ICD's own path for this process)
  --no-timestamps          run without GPU timestamp queries (CPU numbers only)
  --debug-layer            enable the D3D12 debug layer
  --out PATH               JSON output file (default: no file, stdout summary only)
  --selftest               CPU-only checks, no device
  --help
```

Default window: borderless (`WS_POPUP`) over the whole primary output of the chosen adapter, per-monitor DPI
aware so the client area matches the output's pixels and DXGI does not quietly stretch. `Esc` ends the run
early; so does closing the window.

Exit codes: `0` ran and self-consistent, `1` a failure (device removed, fence timeout, invalid timestamps,
output file not written), `2` usage, `3` no adapter or no device, `5` the watchdog terminated the process.

**Watchdog.** The client must never be left running on the lab. A thread asks the loop to stop at
`--seconds + 15` (the budget covers device creation and the calibration passes) and, if the process is still
alive five seconds later and has not begun writing output, writes `{"status":"WATCHDOG"}` to `--out` and calls
`TerminateProcess` with code 5.

## When the driver's clock is wrong: `--ts-hz`

A driver can report a timestamp frequency that is not the one the command processor writes. Ours does: unit A's
KMD answers `DxgkDdiCalibrateGpuClock` with `KeQueryPerformanceCounter` for both counters and the QPC frequency
as `GpuFrequency` (`driver/kmd/wddm.c`, `Bc250WddmCalibrateGpuClock`), while the hardware counter runs at
100 MHz. Trial `quick-20261002T225506Z` measured the consequence directly: ratio 10.0100 and 9.9708, so every
GPU-side value was 10x and the `--gpu-ms` calibration converged on a tenth of the work it was asked for
(12 ms asked, 1.217 ms delivered). Filed as **BD-056**, to be fixed in KMD 0.7.197 from `GOLDEN_TSC`.

`--ts-hz 100000000` is the interim measure, and it fixes two different things:

- **Scale** (durations). Every tick difference is divided by the given frequency, so list durations, gaps, idle
  and the calibration are real milliseconds again. This part is exact: it needs no knowledge of the origin.
- **Origin** (the cross-clock latencies `submit_to_gpu_start` and `gpu_end_to_wake*`). The driver's calibration
  point pairs a GPU tick with a CPU tick, and with the wrong frequency that pairing is useless (the lab's
  offset came out as 2.24e13 ticks, which is why those two columns read as +2240331332 ms and -2240331334 ms).
  The client therefore fits the origin itself, from physics: a dispatch cannot begin before the
  `ExecuteCommandLists` that submitted it, and cannot end after the CPU woke from the fence that it signalled.
  A short dedicated pass of 18 bracketed dispatches runs immediately before the loop, and the loop keeps
  refitting from its own frames, clamped to +-200 ppm of drift against the first fit.

The fit's accuracy is the width of its bracket (`clock_fit.bracket_ms`, 0.076 ms on this machine) and it is
recorded with the result, not hidden: the GPU-side durations are exact, the two latency columns and the
`await`/`after` split carry that uncertainty. When the loop is Present-bound the submission-side bound is loose
by a whole frame and every refit hits the clamp; the client detects that and sets
`summary.consistency.cross_clock_usable` false, which means read the durations and the `cpu_ms` columns and
ignore the latencies. `src\tsfit-runs.ps1` is the host matrix that proves all of this: without the override,
with the right frequency (the fit then agrees with the host driver's own calibration to 0.1 ms), with a
deliberately wrong one (ratio 10.0005, `cross_clock_usable` false), and the Present-bound pair.

**Remove the override when KMD 0.7.197 lands.** The test is that the ratio stays at 1.00 without it.

## JSON schema

One object; `"schema": 4`. Every duration is in milliseconds unless the key says `ticks` or `qpc`.

| key | meaning |
| --- | --- |
| `command_line`, `config` | every option as it was applied after validation, including `ts_hz` (0 = the driver is believed) |
| `host` | `qpc_frequency`, `first_qpc`, `gpu_timestamp_frequency` (the one used), `gpu_timestamp_frequency_reported` (the driver's), `timestamp_frequency_overridden`, `dpi_awareness`, `clock_points`, `clock_drift_ppm` (GPU clock against QPC over the run; null when the origin was fitted) |
| `clock_fit` | `applied`, `samples`, `bracket_ms`, `origin_qpc`, `lower_qpc`, `upper_qpc`, `applied_from_clock_point`, `driver_point_offset_ms` (how far the driver's own point sits from the fit), `refits`, `refits_clamped`, `drift_allowance_ppm`, `all_time_bounds_crossed`, `driver_gpu_ticks`, `driver_cpu_ticks` |
| `timeline[]` | where the wall time went: `phase` (start, runtime, adapter, device, window, swap chain, pipeline, frames, recorders, calibration, sustained calibration, clock fit, loop, teardown), `ms`, `at_ms`. This is what the lab budgets are sized from. |
| `adapter` | description, vendor/device id, LUID, memory, `software`, `feature_level` (hex) |
| `swapchain` | `format`, `buffers`, `width`, `height`, `swap_effect`, `flags`, `present_flags`, `tearing_supported` |
| `calibration` | `groups`, `iterations`, `target_ms_per_dispatch`, `measured_ms_per_dispatch`, `slope_ms_per_iteration`, `intercept_ms`, `rounds[]` (each `kind` = `isolated` or `sustained`), `sustained_passes`, `dispatch_floor_ms`, `frame_floor_ms`, `gpu_busy_error_pct` |
| `summary.frames_submitted` / `frames_measured` | frames run, and the steady-state subset the distributions cover (warm-up and the drained tail excluded) |
| `summary.invalid_timestamp_frames` | frames whose resolved timestamps were zero or not monotonic; any of these fails the run |
| `summary.consistency` | `gpu_accounted_ms` (mean `busy` + mean `idle_per_frame`: one frame on the GPU timeline), `interval_ms`, `gpu_over_interval`, `timestamp_scale_ok` (see below), `negative_submit_latency`, `negative_wake_latency`, `negative_tolerance_ms` (0.01 ms plus the fit's bracket), `cross_clock_usable` |
| `summary.cpu_ms` | `frame_interval`, `fence_wait`, `waitable_wait`, `cpu_work`, `record` (wall time of the record phase), `record_cpu_total` (summed over the recording threads), `execute_total`, `signal`, `present`, `frame_cpu_total` |
| `summary.gpu_ms` | `busy` (sum of list durations), `span_first_to_last_list`, `intra_frame_gap_total`, `intra_frame_gap_each`, `inter_frame_gap`, `idle_per_frame`, `idle_awaiting_submission_per_frame`, `idle_after_submission_per_frame` |
| `summary.latency_ms` | `submit_to_gpu_start`, `gpu_end_to_wake`, `gpu_end_to_wake_blocking` |
| `gaps` | where each GPU gap ends against the display's refresh grid (needs `--frame-statistics`): `idle_ms` and `after_submission_ms` (the distributions of every gap and of its part after the submission), then for gaps `>= 1 ms` and `< 1 ms` a `count`, an `aligned` count and an aligned `share`, with `phase_window_us`, `uniform_share` (the share the same window would collect if nothing waited for a vblank), `phase_bracket_ms` and `aligned_share_usable` (below), `frame_statistics_calls` and `frame_statistics_failures`, and the nested `vblank` fit (`fitted`, `samples`, `hz`, `period_ms`, `origin_qpc`, `residual_us`) |
| `icd` | what the winsys reported in its own log over the same run: `log` (the file read), `header`, `submit_line` (the two lines kept verbatim), then the fields parsed out of them: `self_wait` (the arm of `BC250_SELF_WAIT`), `coalesce`, `wait_self_seen`, `wait_self_dropped`, `wait_self_busy`, `wait_objects`, `wait_dropped`, `wait_map_dropped`, `wait_calls`. Null when no log was found, so an arm whose counters did not move cannot be mistaken for one that ran. `wait_dropped` is the sum of both drops, so only `wait_map_dropped` compares with a log from before `BC250_SELF_WAIT`; a nonzero `wait_self_busy` means one queue's self-signal record had more than one user, and the arm is then not what it says. |
| `clock_calibration[]` | every `GetClockCalibration` point: `qpc`, `gpu_ticks`, `cpu_ticks`, `cost_ms` |
| `frames[]` | the raw records, one per line |
| `result` | `status` PASS/FAIL, `failure`, `failure_where`, `device_removed_reason`, `escape`, `wait_timed_out`, `stop_requested` |

Every distribution is `{count, min, p50, p90, p99, max, mean}`. Percentiles are nearest-rank
(`index = ceil(q*n) - 1`), never interpolated, so each reported value is a value that actually occurred.

A raw frame record (all values as measured, nothing reduced):

```json
{"i": 42, "begin": <qpc>, "wait": [<qpc>, <qpc>], "cpu_work_end": <qpc>,
 "record_ticks": <qpc delta>, "record_cpu_ticks": <qpc delta>, "execute_ticks": <qpc delta>, "signal": [<qpc>, <qpc>],
 "present": [<qpc>, <qpc>], "end": <qpc>, "present_hr": "00000000",
 "retired": 40, "drained": false, "ts_valid": true, "clock_point": 1,
 "lists": [[<exec_qpc>, <exec_done_qpc>, <gpu_begin_ticks>, <gpu_end_ticks>], ...]}
```

With `--frame-statistics` each record also carries `present_count`, `sync_qpc` and `sync_refresh` straight from
`DXGI_FRAME_STATISTICS`. The `(sync_refresh, sync_qpc)` pairs are the vblank grid: a least-squares line through
them gives the refresh period and an origin, and that turns any QPC into a phase inside the refresh interval.
The grid is reported in `gaps.vblank` and is believed only between 20 and 400 Hz and within a 1 ms worst
residual, so a mode change in the middle of a run disables the shares instead of inventing them.

`wait` is the wait **this** frame performed, which retires frame `retired`; so the wake latency of frame `M`
is `frames[M+L].wait[1]` against frame `M`'s last `gpu_end`. `clock_point` indexes `clock_calibration[]`:
GPU ticks are mapped onto the QPC timeline with that point, `qpc = cpu_ticks + (gpu - gpu_ticks) *
qpc_frequency / gpu_frequency`. With `qpc_frequency` and `first_qpc` printed, the records align directly with
an ETW trace or a register timeline taken over the same run.

### Reading the numbers

- **Check `summary.consistency.gpu_over_interval` first.** Per frame the list durations plus the gaps between
  them span exactly one frame period on the GPU timeline, so their sum must equal the frame interval measured
  on QPC, which no GPU tick touches. The ratio is 1.0002, 1.0000 and 0.9998 on the three host runs that carry
  it. A ratio far from one means `GetTimestampFrequency` and the counter the GPU actually writes disagree, and
  then every value in `gpu_ms` and `latency_ms` is scaled by that ratio: the `cpu_ms` block is still sound, the
  GPU block is not. Worth watching on a new driver stack - our own KMD answers `CalibrateGpuClock` with the
  CPU's performance counter (`driver/kmd/wddm.c`, `Bc250WddmCalibrateGpuClock`), which is not what the command
  processor writes into a timestamp query. The exit code does not change; the gate is in the result line
  (`ts_scale=`, `scale_ok=`) and in `summary.consistency`. That is exactly what happened on unit A: see
  `--ts-hz` above for the interim measure and what it does and does not fix.
- **Then check `summary.consistency.cross_clock_usable`.** False means the two latency columns and the
  `await`/`after` split cannot be read in this run, either because the scale is wrong or because the origin fit
  had no tight bound to work with. The durations (`busy`, the gaps, `idle_per_frame`) and the whole `cpu_ms`
  block are unaffected.
- `frame_interval` should be about `max(CPU path, gpu_busy + idle_per_frame)`. On the 4090, 12.06 ms interval
  against 11.99 busy + 0.12 idle; where the loop is Present-bound instead, 5.92 ms interval with
  `present` at 5.84 ms.
- `submit_to_gpu_start` is `ExecuteCommandLists` to that list actually starting on the GPU. With `L > 1` the
  queue is deliberately backlogged, so this is dominated by earlier frames still running (17 ms at
  `--latency 2 --gpu-ms 12`, 28 ms at `--latency 3`) and is **not** a measure of submission overhead on its
  own. `idle_after_submission_per_frame` is the number that isolates the driver's part.
- `gpu_end_to_wake` is only meaningful when the loop is GPU-bound. When it is Present-bound the fence has long
  completed before the wait is reached and the value is the CPU's lateness (11.7 ms at `--gpu-ms 0`), not the
  driver's. `gpu_end_to_wake_blocking` keeps only the waits that actually blocked.
- **An aligned share is read against `gaps.uniform_share`, never against a remembered per cent.** The share is
  the window over the refresh period (1.8 % for the default 300 us at 60 Hz), so only a share well above it
  says the gaps end at vblanks. `gaps.vblank.fitted` false means there is no usable grid and every share is
  null. So does `gaps.aligned_share_usable` false: the phase is taken on a GPU timestamp mapped into QPC, and
  a fitted origin has an uncertainty of its own (`gaps.phase_bracket_ms`, 0.065-0.102 ms over the 16 lab runs
  so far). Above half the one-sided phase window that uncertainty alone can move an aligned population out of
  the window, so the shares are withheld rather than reported as an instrument artefact.
- **`gaps.frame_statistics_failures` tells a failed call from a grid that did not fit.** A nonzero count means
  the driver refused `GetFrameStatistics`, which is a different fault from samples that no line fits.
- **`icd.self_wait` plus its counters are the witness of the arm that ran.** An `elide` run whose
  `wait_self_dropped` stayed at zero elided nothing, whatever the frame rate did; a `keep` run whose counter
  moved is not the control it claims to be; a `count` run must see matches and drop none; and any run with
  `wait_self_busy` above zero had a second user of the record. `run-frameloop.py` checks all of that itself and
  prints `COMPARISON WITHHELD` with the reason instead of a comparison. Session 322 is why: there the
  mechanism counters all moved, the kernel wait calls fell to 17, and the frame rate fell 1.7 % (K142).
- `--frame-latency-waitable N` moves the throttle from the fence wait to the waitable object: on the 4090 the
  fence wait drops from 11.7 ms to 0.006 ms at the same fps, and `gpu_end_to_wake_blocking` then has almost no
  samples. Useful as a control, not as the default.

## Host reference (this development PC)

RTX 4090, Windows 11 26200, borderless 1280x720, 6 s per run, latency 2 and 2 buffers unless stated. All runs
exited 0 with `invalid_timestamp_frames 0`. `idle` columns are per frame, p50.

| case | fps | asked GPU ms | GPU busy p50 | busy err % | interval p50 | idle | awaiting submit | after submit | inter-frame gap | submit p50 | wake p50 | wake p99 | present p50 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gpu-ms 0, 1 list | 169.0 | 0 | 0.01 | 0.0 | 5.92 | 5.91 | 5.89 | 0.01 | 5.91 | 0.04 | 11.75 | 12.03 | 5.84 |
| gpu-ms 0, 7 lists | 169.0 | 0 | 0.01 | 0.0 | 5.91 | 5.91 | 5.81 | 0.10 | 5.77 | 0.03 | 11.63 | 12.10 | 5.73 |
| gpu-ms 4, 7 lists | 169.0 | 4 | 3.98 | -0.2 | 5.92 | 1.93 | 1.83 | 0.10 | 1.85 | 1.72 | 7.71 | 7.85 | 5.75 |
| gpu-ms 12, 7 lists | 82.6 | 12 | 11.99 | -0.1 | 12.06 | 0.12 | 0.00 | 0.12 | 0.03 | 17.00 | 0.04 | 7.29 | 0.08 |
| gpu-ms 12, 1 list | 81.9 | 12 | 12.07 | +0.6 | 12.19 | 0.12 | 0.00 | 0.12 | 0.12 | 12.15 | 0.06 | 2.61 | 0.10 |
| gpu-ms 12, latency 1 | 79.5 | 12 | 12.20 | +1.7 | 12.44 | 0.24 | 0.13 | 0.10 | 0.15 | 5.21 | 0.04 | 5.30 | 0.09 |
| gpu-ms 12, latency 3, 3 buffers | 85.0 | 12 | 11.64 | -2.9 | 11.75 | 0.12 | 0.00 | 0.12 | 0.03 | 28.34 | 0.04 | 0.84 | 0.09 |
| gpu-ms 12, interval 1 | 80.0 | 12 | 11.54 | -3.6 | 11.76 | 0.12 | 0.00 | 0.12 | 0.03 | 17.37 | 0.04 | 0.13 | 0.11 |
| gpu-ms 12, waitable 2 | 82.7 | 12 | 11.98 | -0.2 | 12.07 | 0.12 | 0.00 | 0.12 | 0.03 | 17.01 | 0.08 | 0.15 | 0.10 |
| gpu-ms 25, 7 lists, 3440x1440 full output | 41.1 | 25 | 24.17 | -3.1 | 24.31 | 0.12 | 0.00 | 0.12 | 0.03 | 34.49 | 0.03 | 1.80 | 0.11 |

WARP ("Microsoft Basic Render Driver"), borderless 640x360, 8 s per run, GPU work scaled down because WARP is a
software rasterizer:

| case | fps | asked GPU ms | GPU busy p50 | busy err % | interval p50 | idle | awaiting submit | after submit | submit p50 | wake p50 | wake p99 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| gpu-ms 0, 1 list | 3195.2 | 0 | 0.26 | 0.0 | 0.31 | 0.04 | 0.00 | 0.04 | 0.33 | 0.02 | 0.15 |
| gpu-ms 0, 7 lists | 2120.2 | 0 | 0.26 | 0.0 | 0.37 | 0.11 | 0.00 | 0.11 | 0.40 | 0.02 | 0.23 |
| gpu-ms 2, 7 lists | 544.2 | 2 | 1.66 | **-16.6** | 1.83 | 0.17 | 0.00 | 0.17 | 2.44 | 0.02 | 0.03 |
| gpu-ms 6, 7 lists | 162.0 | 6 | 5.86 | +0.2 | 6.03 | 0.16 | 0.00 | 0.16 | 8.64 | 0.02 | 0.03 |
| gpu-ms 6, 1 list | 177.3 | 6 | 5.52 | -6.8 | 5.58 | 0.05 | 0.00 | 0.05 | 5.59 | 0.02 | 0.03 |
| gpu-ms 6, latency 1 | 153.9 | 6 | 6.15 | +5.1 | 6.34 | 0.18 | 0.05 | 0.13 | 2.57 | 0.01 | 0.03 |
| gpu-ms 6, latency 3, 3 buffers | 162.8 | 6 | 5.81 | -0.5 | 5.98 | 0.16 | 0.00 | 0.16 | 14.63 | 0.02 | 0.03 |
| gpu-ms 6, interval 1 | 100.0 | 6 | 5.80 | -2.0 | 8.34 | 2.60 | 0.00 | 2.60 | 11.46 | 0.02 | 0.03 |
| gpu-ms 6, waitable 2 | 159.5 | 6 | 5.93 | +1.9 | 6.08 | 0.14 | 0.00 | 0.14 | 8.75 | 0.04 | 0.07 |

The one bold row is the dispatch floor described above, not a defect; the client warned about it on stderr.

Reference reading of these two hosts: a healthy stack puts **essentially nothing** under
`idle_after_submission` - 0.115 ms per frame over 7 lists on the 4090 (0.016 ms per list boundary), 0.16 ms on
WARP. Whatever the lab shows above that, per frame, is ours.

Files: `runs\host-gpu-*.json`, `runs\host-warp-*.json`, and the two reduced tables
`runs\host-gpu-table.json`, `runs\host-warp-table.json`. `src\host-runs.ps1` reproduces the matrix
(`-Warp` for the software rows).

## Multithreaded recording and batched submission

The owner's rule is that multithreading is tested with our own clients before a game is trusted to exercise
it. Two independent axes:

- `--record-threads N`: N threads record the frame's K lists in parallel. Thread `t` takes lists
  `t, t+N, t+2N, ...` (round robin, because the last list carries the clear, the draw and the query resolve and
  is the most expensive to record). Each thread gets **its own command allocator per frame slot** - a command
  allocator can have only one list recording into it at a time - and each list is always reset from the same
  thread's allocator. The threads are created once and released per frame through an auto-reset event, so the
  fan-out costs a few microseconds, not a thread creation. `effective_record_threads` in the JSON is `N` capped
  at `--lists`.
- `--submit MODE`: `interleaved` (default) records a list and submits it before recording the next, which is
  what every number in the host reference above was taken with; `sequential` records all K and then submits
  each in its own call; `batch` records all K and submits them in one `ExecuteCommandLists`. Parallel recording
  needs `sequential` or `batch` - interleaving would serialise the recording again, so
  `--record-threads 2 --submit interleaved` is a usage error.

`cpu_ms.record` is what the frame paid (wall time of the record phase) and `cpu_ms.record_cpu_total` is what
the machine paid (summed over the threads). With one thread they are equal.

## Two queues and the handoff (the C48 arms)

A third axis, added for the ring-gap class the game sessions show (gaps of at least 4 ms whose end falls within
300 us after a vblank, sessions 418-420): make a cross-queue dependency explicit and see whether the client
produces the same class on its own.

- `--queues 2` creates a second direct queue. On its own it changes nothing else: every list still goes to
  queue 0, which is the control that separates "another queue exists" from "work crosses it".
- `--handoff per-list` sends list `k` to queue `k%2` and makes each submission wait on a fence the previous
  submission signalled, so a frame of 7 lists crosses six times (measured: 6.7 waits a frame, because the
  crossing before the last list is counted too). `per-frame` sends the frame's lists to queue `frame%2`, about
  one crossing a frame (measured 1.1).
- `--signal-per-list` keeps one queue and only adds a `Signal` after every `ExecuteCommandLists`: more
  scheduler-visible events, the same GPU work, no cross-queue dependency. It is the arm that separates "a
  signal a list" from "a wait a list".
- **The frame's last list never leaves queue 0.** It clears and draws the back buffer, and a flip-model back
  buffer may only be written by the queue its swap chain was created with: writing it from the second queue
  removed the device with `DXGI_ERROR_ACCESS_DENIED` as the removal reason (this PC's 4090, 2026-10-06, arm
  `e-2q-per-frame` of `src\c48-host-runs.ps1`). The handoff therefore always crosses back into queue 0 before
  that list, which is also the dependency the arm wants: the draw cannot start before the other queue's
  dispatches have finished. A handoff over `--submit batch`, over fewer than three lists (per-list) or two
  lists (per-frame), or without `--queues 2`, is a usage error rather than a mode that silently does nothing.

What the arms are read from: `queue_handoff` in the JSON (`fence_signals`, `fence_waits`, `queues_created`,
`last_queue`) is the arm's own witness - a handoff whose fence never moved did not run - and
`summary.gaps.count_ge_4ms`, `per_second_ge_4ms` and `aligned_share_ge_4ms` are the class. The lab set is
`c48-handoff` in `lab\sets.json` and the pre-registered rule is applied in one place only:
`<BC250_ROOT>\scratch\m15\etw\c48\c48repro.py` (`--selftest` checks the rule itself on synthetic arms). The
host matrix
`src\c48-host-runs.ps1` runs the five arms here and refuses one whose fences did not move.

On this 4090 the arms cost nothing measurable in frame rate (169.0, 168.8, 168.9, 168.6, 168.8 fps for base,
signal-per-list, 2q-none, 2q-per-list, 2q-per-frame), but `idle_after_submission` p50 rises from 0.106 ms to
0.840 ms a frame under `per-list` and 0.298 ms under `per-frame`: the crossing does add GPU-side idle after
submission even on a driver that has no such gap class. No vblank grid fits on this host (every
`GetFrameStatistics` sample is unusable in a windowed flip chain here), so the aligned shares - and therefore
the decision - can only come from the lab.

### Host results, 4090, 1280x720, 8 s, gpu-ms 12, latency 2

| case | threads | submit | fps | record wall | record CPU total | execute total | GPU idle/frame | interval |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 7 lists | 1 | interleaved | 82.5 | 0.16 | 0.16 | 0.16 | 0.12 | 12.10 |
| 7 lists | 1 | sequential | 82.7 | 0.14 | 0.14 | 0.15 | 0.12 | 12.08 |
| 7 lists | 1 | batch | 84.1 | 0.14 | 0.14 | **0.07** | 0.18 | 11.87 |
| 7 lists | 4 | sequential | 81.7 | 0.12 | 0.24 | 0.15 | 0.12 | 12.21 |
| 7 lists | 4 | batch | 84.4 | 0.18 | 0.45 | 0.12 | 0.27 | 11.82 |
| 32 lists | 1 | interleaved | 80.5 | 0.69 | 0.69 | 0.98 | 0.52 | 12.22 |
| 32 lists | 1 | sequential | 79.2 | 0.62 | 0.62 | 0.88 | 0.53 | 12.51 |
| 32 lists | 1 | batch | 72.6 | 0.50 | 0.50 | **0.15** | 0.38 | 13.57 |
| 32 lists | 4 | sequential | 80.8 | **0.27** | 0.67 | 0.79 | 0.51 | 12.29 |
| 32 lists | 4 | batch | 82.8 | 0.19 | 0.56 | 0.10 | 0.30 | 12.06 |
| 32 lists | 8 | sequential | 81.3 | **0.20** | 0.73 | 0.51 | 0.49 | 12.26 |
| 32 lists | 8 | batch | 84.4 | 0.21 | 0.48 | **0.07** | 0.27 | 11.82 |

All p50 ms, all runs exited 0 with zero invalid timestamps. What it says:

- **Parallel recording works and is nearly free.** At 32 lists the record phase drops from 0.62 ms to 0.27 ms
  (4 threads) and 0.20 ms (8 threads) - 3.1x - while the CPU total rises from 0.62 to 0.73 ms, 18 % more work
  for a third of the latency. At 7 lists there is nothing to win: 0.14 ms of recording split four ways is lost
  in the fan-out, and `record_cpu_total` rises to 0.24 ms for no wall-clock gain. Parallel recording is worth
  it from roughly 16 lists upward on this host.
- **One `ExecuteCommandLists` is much cheaper than K.** 32 separate calls cost 0.88 ms per frame, one call with
  32 lists costs 0.15 ms: 0.73 ms of CPU per frame for nothing. At 7 lists it is 0.15 vs 0.07 ms. This is the
  cheapest change available to a submission path and the one to check first on the lab.
- **The idle attribution shifts by construction in batch mode.** With one call, every list was already on the
  queue before the GPU started list 0, so all of a gap counts as `idle_after_submission` - the number cannot be
  compared across submit modes, only within one. Compare `idle_per_frame` across modes instead: at 32 lists it
  falls from 0.53 (sequential) to 0.27-0.30 ms (batch), because the per-submission boundaries are gone.

### Host results, WARP, 640x360, 8 s

| case | threads | submit | fps | record wall | record CPU total | execute total | GPU busy | idle/frame |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 7 lists, 6 ms | 1 | interleaved | 147.3 | 0.02 | 0.02 | 0.15 | 5.79 | 0.18 |
| 7 lists, 6 ms | 1 | batch | 176.8 | 0.02 | 0.02 | 0.14 | 5.38 | 0.20 |
| 7 lists, 6 ms | 4 | sequential | **114.1** | 0.04 | 0.05 | 0.17 | **7.35** | 0.21 |
| 7 lists, 6 ms | 4 | batch | 183.8 | 0.03 | 0.03 | 0.13 | 4.62 | 0.20 |
| 32 lists, 20 ms | 1 | sequential | 36.5 | 0.06 | 0.06 | 0.55 | 25.68 | 0.81 |
| 32 lists, 20 ms | 1 | batch | 52.1 | 0.06 | 0.06 | 0.63 | 17.93 | 0.99 |
| 32 lists, 20 ms | 4 | sequential | 49.3 | 0.04 | 0.07 | 0.50 | 19.16 | 0.83 |
| 32 lists, 20 ms | 8 | batch | 50.0 | 0.04 | 0.08 | 0.46 | 18.95 | 0.77 |

WARP's own rasterizer is a thread pool on the same CPUs, which produces the one result worth carrying to the
lab: at 7 lists, four recording threads pushed WARP's "GPU busy" from 6.46 to 7.35 ms and fps from 148 to 114.
**Recording threads can take CPU away from work that only looks like GPU work.** On the BC-250 that caution is
not academic while DWM composites on the CPU. WARP's calibration is also coarse and noisy here (errors from
-13 % to +43 % at iteration counts of 25-46, where WARP's own scheduling variance dominates); the matrix is a
correctness and shape check there, not a measurement.

Reproduce: `src\mt-runs.ps1` (`-Warp` for the software rows), outputs `runs\mt-gpu-*.json`,
`runs\mt-warp-*.json` and the two reduced tables.

Regression: `--record-threads 1 --submit interleaved` is the original code path untouched, and its row above
(interval 12.10, record 0.16, GPU busy 12.00, idle 0.12) matches the pre-multithreading run of the same
configuration (12.06 / 0.124 / 11.99 / 0.115) within this host's run-to-run noise.

## Running it on unit A

```
python tools/win/frameloop/lab/run-frameloop.py quick|mt-gpu|mt-present|gap|gap-queue|self-wait|c48-handoff [--trial NAME] [--dry-run]
```

An SSH session on the lab lands in session 0, where a process gets no desktop and can create no window, so a
client that opens a swap chain over the primary output cannot be started from there. `lab\frameloop-task.ps1`
does what the m8 and m13 harnesses did (`evidence/windows/2026-09-22-E25-m8-address32`,
`2026-09-24-E26-desktop-resume`): one **one-shot scheduled task per configuration**, principal the logged-on
console user, `LogonType Interactive`, started from the SSH session, polled until it leaves `Running`, then
unregistered. The action is a small `.cmd`, because a task action cannot redirect and the client's text summary
is worth keeping next to its JSON.

`lab\run-frameloop.py` is the local half: it checks the owner's STOP flag and writes a line on the overlay
(`mon.py`), pushes the exe and a plan file to `C:\BC250\frameloop`, runs the lab script in **one** SSH call (no
polling from here, so nothing provokes the lab's sshd penalties), pulls one archive back into
`lab\runs\<trial>\` and prints one row per configuration. Nothing is swapped: the client goes through the
registered triplet, which is the point of the measurement.

Bounds and refusals, all before anything starts: a set whose runs plus their expected overhead would exceed
170 s is refused here (one trial stays inside the lab's three minutes); the lab side refuses a wrong directory,
a client whose hash is not the plan's, an existing result directory, a competing `BC250|DWM|G0|WSI` task that is
running (the resident overlay and net watchdog are allowed by name), a machine with nobody logged on, and Tctl
at or above 87 C.

**Four timeouts, in this order**, which `lab\host-checks.ps1` enforces across the files that hold them: the
client's watchdog asks the loop to stop at `seconds + 15` and kills the process at `seconds + 20`, writing what
it has either way; Task Scheduler's `ExecutionTimeLimit` is `seconds + 25`; the poll gives up at `seconds + 30`.
The point of the order is that a run which goes wrong ends with the client's own evidence rather than with
`SCHED_S_TASK_TERMINATED` and nothing to read. The set's deadline is the lab's 170 s, and before each run the
lab side asks whether that run's **worst case** (`seconds + 30`) still fits; if not the run is recorded as
skipped rather than overrunning.

The sets are in `lab\sets.json` (`quick` about 53 s, `mt-gpu` and `mt-present` 103 s, `gap` 93 s, `gap-queue`
73 s, `self-wait` 69 s, `c48-handoff` 153 s, each with 170 s available), one `what` line per set saying what
it is for; `--dry-run` prints the plan and the commands and touches nothing.

A run may also carry **driver knobs of its own**, as an `env` object next to its `args`, and `self-wait` is the
set that uses it: the same configuration twice, once with `BC250_SELF_WAIT=keep` and once with `elide`, so the
two arms of the ICD's self-wait elision are one trial. Only `BC250_*` names are accepted, checked here and
again on the lab, and a value that carries a quote or a `%` is refused; the knobs become `set "NAME=VALUE"`
lines in that run's wrapper, so they reach the client's process alone and never the desktop's own ICD, which a
machine-wide cfg file would also move. Each run's knobs are written into its record. The lab side also points
`BC250_DEFERRED_LOG` at `<run>-icd.log` in the result directory, which is the file the client reads back for
its `icd` block, so the arm and its counters come home with the trial.

`lab\host-checks.ps1` is the gate for the runner itself, on this machine, with no lab involved: it generates the
`.cmd` wrapper through `lab\cmd-lines.ps1` and asserts it is exactly four lines of the exact expected shape
(plus one `set` line per knob, and nothing but our own knobs),
runs it through `cmd.exe` to check that the redirection, the log content and the exit code all arrive, refuses
the two shapes that have already gone wrong, checks that the lab script parses and still uses the generator,
and checks the four timeouts and the plan fields the two halves share. Both of those wrapper defects were found
the expensive way:

- An operator-precedence mistake (`'...' + $log + '"',` without parentheses) put the log path on a line of its
  own; `cmd` executed the path, **Notepad opened on the lab's console desktop**, the wrapper blocked on it and
  the task ran to its time limit. The client had already written its JSON, so the measurement survived, but the
  wall time of every run became the time limit.
- `echo exit %ERRORLEVEL%>> "log"` expands to `echo exit 0>> "log"`, and `cmd` reads the digit before `>>` as a
  **file handle**: that echoes `exit ` to the console and redirects stdin. The exit code never reached the log.
  The generated line therefore puts its redirection first: `>> "log" echo exit %ERRORLEVEL%`.

### First unit A trial, `quick-20261002T225506Z`

Client C6029B1A (before `--ts-hz`), KMD 0.7.196.1, desktop on GPU DWM, 1920x1200, Tctl 66.8 -> 69.0 C. Two runs
of three; `g0-l7` was skipped out of budget because both tasks ran to their time limit on the Notepad defect
above. Both runs exited PASS with `invalid_ts=0`, and both reported the scale mismatch, which is what the
consistency gate is for. Dividing the GPU columns by the measured 9.9708 gives the first real numbers from our
stack, next to the 4090 at the same shape (`--gpu-ms 12 --lists 7 --latency 2`):

| p50 ms | unit A (corrected) | 4090 |
| --- | --- | --- |
| GPU busy | 1.217 (12 asked: the calibration believed the 10x clock) | 12.373 |
| GPU idle per frame | 0.524 | 0.115 |
| of it: intra-frame gaps | 0.431 | 0.087 |
| **per list boundary** (6 of them) | **0.070** | **0.014** |
| of it: inter-frame gap | 0.089 | 0.029 |
| frame interval | 1.752 (481 fps) | 12.502 (80 fps) |
| CPU fence wait | 1.201 | 11.994 |
| CPU record wall (7 lists) | 0.172 | 0.162 |
| CPU execute total (7 ECLs) | 0.242 | 0.164 |
| CPU present | 0.086 | 0.148 |

The headline is the **per-list-boundary gap of 0.070 ms, five times the host's 0.014**, and that it is a real
measurement of our submission path rather than a game-session estimate. It is not yet the gap the game sees: the
calibration delivered a tenth of the GPU work, so this is the shape of a frame that is mostly submission. The
`--ts-hz 100000000` runs are the ones to compare against the game's classes.

## Build

```
powershell -ExecutionPolicy Bypass -File src\build.ps1
```

The exe goes to `<BC250_ROOT>\scratch\m15\frameloop\build` unless `-Out` says otherwise, so a build
never writes a binary into this repository.

`cl.exe` from the portable MSVC under `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` (found
with `vswhere`), headers and `dxc.exe` from `<BC250_ROOT>\toolchain\nuget` (SDK 10.0.26100.0), `/W4 /WX /O2 /MT
/std:c++17 /Brepro`, temporaries forced onto `P:`. The three shaders are precompiled to DXIL headers under
`src\gen\` at shader model 6.0; the exact dxc identity and command lines are recorded in `src\gen\dxc.txt`. The
build then runs three CPU-only gates - `--help` must exit 0, an invalid option must exit 2, `--selftest` must
exit 0 (41 checks: percentiles, the GPU-to-QPC mapping, the timestamp-scale gate, the `--ts-hz` boundaries,
every other option boundary, JSON escaping) - and keeps the previous exe under `build\retained\` by its hash.
The gates run with `$ErrorActionPreference` on `Continue` and are judged by their exit codes: a refused option
prints its usage to stderr, and PowerShell turns a native command's stderr into an error record, which under
`Stop` failed the build of a sound exe.

`/Brepro` on the compiler and the linker makes the exe a function of its sources: without it MSVC stamps the
time of the build into the image and two builds of the same code hash differently, so a SHA-256 in a trial
record would name a build rather than a revision. Two consecutive builds now produce the same hash.

Nothing links `d3d12.lib` or `dxgi.lib`; only `user32.lib` and `kernel32.lib`.

## Known limits

- **`submit_to_gpu_start` includes queue backlog.** By design the loop keeps `L` frames in flight, so this
  number is mostly "earlier frames were still running". Use `idle_after_submission_per_frame` for the driver's
  contribution.
- **`--latency 3` with the default 2 buffers does not give three frames in flight.** A flip-model chain with
  `N` buffers allows `N-1` outstanding Presents, so Present throttles first. The lab command for latency 3
  below therefore also passes `--buffers 3`.
- **GPU work floor**: `--gpu-ms` below `lists x dispatch_floor_ms` cannot be met (see Calibration).
- **Coarse calibration when the iteration count is small.** One iteration costs `slope_ms_per_iteration`; when
  the calibrated count falls to a few tens (WARP, or any slow adapter at a small per-dispatch target) a single
  step is a large fraction of the target and the calibration quantises. The knob is a *lower* `--groups`: fewer
  thread groups means a cheaper iteration, so more of them fit in the target and the steps get finer.
- **`idle_after_submission` is not comparable across `--submit` modes.** In batch mode the single submission
  precedes every list, so the whole of any gap is attributed after submission by construction. Compare
  `idle_per_frame` across modes and the split only within one mode.
- **Recording threads compete with CPU-side "GPU" work.** Measured on WARP (fps 148 -> 114 with four recording
  threads); the same applies wherever composition or the driver's own threads share the CPUs.
- **The clock mapping is piecewise**, one `GetClockCalibration` point every `--calibrate-every` frames
  (default 32), so a GPU timestamp is mapped with a point up to half a second old. Measured drift on the host
  was under 1 ppm; `host.clock_drift_ppm` reports it per run and `--calibrate-every 1` tightens it at the cost
  of one kernel call per frame.
- **Timestamp placement** is the runtime's: `EndQuery(TIMESTAMP)` is a bottom-of-pipe write on AMD, so a list's
  "start" timestamp is itself a pipelined event. A UAV barrier after each dispatch keeps consecutive dispatches
  from overlapping, which is what makes the per-list durations and the gaps between them separable; without it
  the GPU would overlap them and the gaps would be meaningless. That barrier is also a cost the numbers
  include.
- **The draw reads the compute output through an SRV**, not through a UAV in the pixel shader. The dependency
  is the same - the frame cannot be produced without the frame's dispatches - and the SRV path is the one
  already exercised on the lab.
- **`--gpu-ms 0` runs no dispatch at all** inside the loop (the "clear and draw only" case). The buffer is
  still defined: one priming dispatch at startup writes every slot.
- **No readback or pixel check.** This client measures time; it does not prove what reached the screen. The
  image is a moving noise pattern derived from the compute output, which is enough for an operator to see that
  frames are advancing, but a correctness witness is `d3d12queue`'s business.
- **WARP has no output**, so the window falls back to the primary monitor rectangle; WARP's own
  `submit_to_gpu_start` and `idle_after_submission` are a software scheduler's, useful only as an order of
  magnitude.
- **Seven lists are not a game.** A Witcher 3 frame carries about 160 barrier drains and some 5 waits per
  frame; this loop has K dispatches, one fence and one Present. What it measures is the *floor* of our chain's
  per-frame latency with a known amount of work, not the game's frame. A floor far below the game's gap says
  the gap is in the game's pattern (barriers, waits, descriptor work); a floor near it says the gap is in the
  loop itself and is reproducible without a game session.
- **The numbers assume the GPU timestamp and the reported frequency belong together.** They need not:
  `summary.consistency.gpu_over_interval` is the check, and on our own stack the KMD answers
  `CalibrateGpuClock` with the CPU's counter (BD-056). If the ratio is not about one, read `cpu_ms` only, or
  pass `--ts-hz` for the real frequency.
- **A fitted clock origin is a bracket, not a point.** With `--ts-hz` the client knows the scale exactly and the
  offset only to within `clock_fit.bracket_ms` (0.076 ms on this machine, one fence wake and one submission
  apart). Durations are unaffected; `submit_to_gpu_start`, `gpu_end_to_wake*` and the `await`/`after` split carry
  it, and the negative-value gate widens by that much
  (`summary.consistency.negative_tolerance_ms`). The fit also assumes the two clocks drift by less than 200 ppm
  (host: 46 ppm over 8 s); `clock_fit.refits_clamped` counts how often that clamp bound a refit, and when every
  refit is clamped the fit is declared unusable rather than quietly wrong.
- **`host.clock_drift_ppm` is null when the origin was fitted**: it is a measurement of the driver's calibration
  points against QPC, and with `--ts-hz` those points are not what the mapping uses.
