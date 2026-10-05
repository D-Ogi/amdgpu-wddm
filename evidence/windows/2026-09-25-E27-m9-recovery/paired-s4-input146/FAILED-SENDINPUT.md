# Cold02 input control: SendInput did not change observed cursor coordinates

2026-09-25. Analysis only, no helper edits or lab actions. This is not a valid
moving-cursor positive control and must not be compared with S4 as though it were.

- 488 measurements; every GetCursorPos is(960,600), despite 61 distinct intended
coordinates. The 10 match flags occur exactly at trajectory indices30,90,...570,
where intendedX crosses960. They are stationary-coordinate coincidences, not
successful motion or measured delivery latency. Helper exit0 only reports that
its API calls/checks completed.
- Input ETL contains 489 WakeMIT events from helperPID5364:488 moves plus the final
restore. They are followed by ProcessQueuedMouseEvents on DWM PID704/TID1928.
- Strictly within the intended10s QPC interval: 487 WakeMIT,487 mouse-processing
starts and 487 stops. The last move falls just beyond the nominal10s edge due to
helper scheduling/check overhead and is excluded, as is restoration.
- WakeMIT-to-next mouse-processing start:54-132us, median78us,p95 92us. The mouse
handler takes11-144us, median13us,p95 24us. These event boundaries identify fast
queue handling, not proof that the coordinates were applied or displayed.
- No QueueInputMessage or RetrieveInputMessage events occur within the strict
active interval. The five of each in the whole ETL occur outside it; their
message fields are0. No successful WM_MOUSEMOVE delivery is established.
- HIDCLASS1 captured device rundown information, not an independent per-movement
input stream. Synthetic input is not a physical USB-mouse measurement.

## Clock correlation and parser

`../analyze-cursor-input.py` reads cursor.csv and xperf input-events.csv. Because
UTC in the helper header is emitted after qpc_start, it is not treated as an exact
anchor. Instead all488 ordered same-PID WakeMIT events are tested against the
corresponding SendInput begin/end QPC brackets. A single offset fits every pair:
13953701290..13953702490 QPC ticks, a120us-wide interval at10MHz. CSV export time
rounding gets a1us allowance. This is an explicitly validated pairing model, not
a raw-QPC ETL decoder or a claim that every Win32k WakeMIT is universally emitted
inside SendInput. Mismatched event counts or an empty offset intersection fail.

Using the midpoint, the strict active window is input-ETL relative
1.5528638..11.5528638 seconds, with +/-60us model uncertainty. The parser's event
counts are restricted to this window; setup/rundown and restore are excluded.
Observed sample spacing is typically21.844ms (p95 22.093ms), not a measured16ms.
Thread.Sleep scheduling and repeated non-match polling extend nominal tick time.

Files: `cursor-input-analysis.json`, `cursor-input-analysis.txt`,
`input-events.csv`, `input-stats.txt`, and original `movement/cursor.csv`.
Parser tests in `../test-cursor-input.py`:3 PASS, covering stationary-coordinate
false success, incompatible timestamp brackets and missing events.

## Interpretation / next control

The input reaches a Win32k mouse-handling path promptly, but logical cursor
position remains fixed. This separates the failure from a general inability
to enter the helper's session or a half-second delay before that handler.
Recorded identity is console session1, WinSta0, input/attached desktop Default.
Names plus mouse-handler activity do not identify the internal reason the
coordinates were not applied. There is no evidence here to blame DWM rendering,
GPU completion, or S4: this was a cold control and movement itself failed.

The root is preparing a separate SetCursorPos control on the same attached
input desktop. Its independent coordinate-changing path can establish whether
logical positioning is possible, while preserving the original SendInput binary
and failed run. Success there would narrow this to the synthetic-input path,
not prove physical mouse delivery or physical scanout. No graphics ETL dump or
DWM latency conclusion was made for this failed movement control, per root scope.
