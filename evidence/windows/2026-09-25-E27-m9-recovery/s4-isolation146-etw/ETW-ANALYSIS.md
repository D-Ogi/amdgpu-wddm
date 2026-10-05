# Post-S4 ETW analysis, KMD 0.7.146.1

2026-09-25. Offline analysis only. No driver changes or lab actions.

## Result

The trace identifies expensive CPU composition and long DWM wait-for-work
intervals, but does not establish the cause of the owner's periodic cursor stalls.
There is no captured mouse-movement oracle to prove that DWM was idle despite
pending movement. The trace does not support a lost-vsync or long-ISR/DPC theory
for this recorded interval. Regular vsync is not proof of smooth cursor updates.

The driver currently exposes a software cursor (root's source review: pointer
caps zero, SetPointerPosition no-op, shape unsupported). The relevant chain is
input -> DWM work/render -> present/flip, not an independent hardware cursor.

## Input and tools

- Source: `stutter.etl`, 124780544 bytes, SHA256
  `9827714421205A8805D5333A49B01F252C77C68F61ABA96F17FD59DC404A9F96`.
- ETL header: Windows build 22631, 12 logical processors; local interval
  03:50:36.135034 to 03:51:20.689436, 44.554402 seconds. The shell's later
  completion is not the ETL end. Lost buffers 0, lost events 0.
- WPR profiles: GeneralProfile, GPU, DesktopComposition. Script slept 20 seconds
  before stop; stopping/rundown extended the recorded interval. Figures for the
  first 20 trace seconds are reported separately to limit rundown contamination.
  This does not remove all WPR overhead or prove exact active-capture boundaries.
- Installed WPT xperf decoded the trace and Microsoft symbols resolved a 3 ms
  wait-entry excerpt. All outputs and symbol caches remain under P:\bc-250.
- No marked manual mouse interval, no cold trace with the same instrumentation.

## Concrete timing evidence

All values below are milliseconds. P50/P95 are sorted observed samples, not a
claim about an external population. Start/stop events pair by event task and TID.
DWM PID1548 main composition TID2864.

| Operation | First 20 s count | P50 | P95 | Maximum |
|---|---:|---:|---:|---:|
| DWM SCHEDULE_RENDER | 29 | 35.642 | 38.095 | 38.786 |
| DWM SCHEDULE_PRESENT | 27 | 1.568 | 1.983 | 4.204 |
| DWM SCHEDULE_PROCESS_FRAME | 29 | 37.415 | 39.838 | 40.272 |
| DWM SCHEDULE_WFVB | 29 | 0.098 | 10.999 | 15.334 |
| DWM SCHEDULE_WFW | 28 | 464.802 | 1354.230 | 1362.325 |
| Consecutive VSyncDPC interval | 1190 | 16.681 | 16.744 | 16.835 |

The full trace has 2433 VSyncDPC events, with 2432 consecutive intervals in
16.528-16.852 ms. There are no missing-frame-sized gaps in that notification
stream. The full-trace Present maximum is 24.387 ms at relative24.759093s,
inside a front-to-back copy interval, after the first20s selection.

Specific examples, timestamps relative to ETL start:

- Render7.438588-7.477374s takes38.786ms; whole process-frame
  7.438569-7.478841s takes40.272ms. This is a real multi-refresh composition
  duration, not a one-second GPU fence wait.
- WFW0.476030-1.838355s takes1362.325ms. Symbolized syscall entry at0.476040s
  is `ntoskrnl!NtWaitForMultipleObjects`; CSwitch at0.476042s puts TID2864 into
  Waiting/UserRequest and schedules Idle. Two immediate earlier polling calls
  returned STATUS_TIMEOUT and are not the long wait itself.
- WFW8.478989-9.833219s takes1354.230ms. These wait-for-work intervals must not
  be relabelled missed input without an input timestamp.

## CPU, interrupts, and renderer attribution

- Pairing captured ReadyThread with subsequent DWM switch-in gives maximum
  observed ready-to-run delay61us on TID2864 and9.625ms on a rendering worker.
  Thus this trace does not show a half-second ready-but-starved main compositor.
  Do not use the raw CSwitch WaitTime column for that conclusion: this dump
  contains saturated4294966 values. Explicit ReadyThread pairing was used.
- Maximum recorded DPC duration273us, USBPORT.SYS; maximum ISR313us,
  USBPORT.SYS. Maximum dxgkrnl ISR112us. These are individual event maxima,
  not merely low average CPU counters. They do not show half-second interrupt
  service blocking in the measured interval.
- DWM sampled CPU is concentrated in 12 workers TID3360..3404 in steps of4.
  Their ThreadStartImage identifies bc250d3d.dll. Most instruction addresses
  lie in anonymous code ranges, e.g. 0x000001cffa9f0417/426/436/440, and do not
  resolve to an image. This is consistent with the software renderer's JIT,
  not proof of a particular Mesa function or shader without JIT metadata.
- Sampled CPU totals report about17.5 CPU-seconds in DWM's unknown-code bucket
  across44.55 wall seconds and12 CPUs. This explains why a low whole-machine
  average can coexist with short, parallel CPU-render bursts. It does not
  establish that S4 changed renderer speed: a matched cold control is absent.

## Input oracle and limits

The trace contains Win32k message/paint and DComp events, but no named
Mouse/Pointer/Cursor/HID input stream. Decoded Win32k message fields contain
no WM_MOUSEMOVE(512/0x200) events. DispatchMessage has56 WM_PAINT(15) starts.
USBPORT interrupt activity is not evidence of mouse movement or its delivery.
The absence of these events is a capture limitation, not proof the owner did
not move the mouse, and not proof the input stack discarded it.

Therefore long WFW may be normal unchanged-desktop idle time, while ~35ms
composition is a measured cost. Neither alone explains the reported regular
half-second stalls. This trace cannot yet isolate input delivery, DWM wakeup,
software rendering, or cursor composition as the S4-specific regression.

## Smallest next test

Use the root's `NEXT-INPUT-CONTROL.md`: first prove a cold positive control with
actual input events and timestamps, then repeat the same short deterministic
cursor trajectory after S4 in the interactive desktop, with heavy summary
polling still paused. Record intended movement, observed GetCursorPos and
input-receipt timestamps, plus DWM render/present and vsync. An input-injection
success return or session0 movement alone is not a delivered-input oracle.
Keep the trajectory bounded and restore the original cursor position. Compare
input-to-DWM wake and input-to-presentation intervals; only then distinguish
waiting despite input from costly composition or downstream display latency.

## Reproduction and artifacts

`analyze-etw.ps1` produces the xperf outputs. `parse-etw.py` stores provider
headers/counts and selected records. `timing-etw.py` and `timing-first20.py`
produce interval statistics. `stacks-etw.py`, `inspect-stacks.py`, `input-etw.py`
provide scheduler/interrupt excerpts and input-field checks.

Key outputs: `etw-timespan.txt`, `etw-stats.txt`, `etw-dpcisr.txt`,
`etw-profile.txt`, `etw-events.csv`, `etw-timing-analysis.json`,
`etw-timing-first20.json`, `etw-cpu-extras.json`, `etw-input-analysis.json`,
`etw-narrow-symbols.csv`. Raw ETL and decoded paths stay outside the public repo.
The preliminary `dwm_ready_waits` field in timing JSON is the raw unreliable
WaitTime view; use `etw-cpu-extras.json` ReadyThread pairing for ready latency.
