# Direct-position cold02: logical-motion and DWM-work control

2026-09-25. Offline analysis only. Source helper and artifacts unchanged.

The cold control passes logical position change and refresh-paced DWM work.
It does not establish physical cursor smoothness or180 full-frame presents.
A matched S4 run can now compare the same direct-position stimulus, while
physical mouse delivery remains a separate question.

## Strict three-second result

Helper PID4660, console session1, WinSta0, input/attached desktop Default.
Transport is the root's separate SetCursorPos-direct helper, not SendInput.

- 188 samples,188 coordinate matches,187 observed changes,61 distinct positions.
- Median observed sample spacing16.0133ms; maximum19.4483ms. Call-to-observation
  interval median4.112ms, maximum7.583ms. That includes the helper's polling/check
  overhead; it is not a measurement of physical input-to-photon latency.
- 180 DWM SCHEDULE_RENDER start/stop pairs. Start-to-start median16.676ms,
  maximum16.823ms. Render duration median0.808ms, maximum3.153ms.
- 180 SCHEDULE_PROCESS_FRAME pairs, maximum6.867ms.
- 180 VSyncDPC events; maximum consecutive interval16.796ms.
- Only5 SCHEDULE_PRESENT start/stop pairs, duration median3.924ms, maximum5.189ms.
  Their maximum start-to-start gap is984.011ms, with other gaps around500ms.
  Do not substitute render counts for full present counts.
- 180 Win32k WakeMIT, ProcessQueuedMouseEvents starts/stops, QueueInputMessage and
  RetrieveInputMessage events. The input-message records belong to PID4916/TID4920,
  but their decoded message field is0. This is recorded input-path activity,
  not a verified WM_MOUSEMOVE payload carrying the requested coordinates.

The direct-path logical coordinate observations plus sustained render scheduling
are the positive control. They differ from failed SendInput cold02, where all488
observations stayed(960,600) despite rapid mouse-queue handling. No conclusion
about why that synthetic path had no observed coordinate effect is established.

## QPC alignment: no UTC-header guess

The direct path emits no WakeMIT from the helper PID, so the previous SendInput
bracket matcher correctly refuses it. A small offline native reader,
`../../input-control146/etl-anchor.c`, reads each ETL twice: once with
PROCESS_TRACE_MODE_RAW_TIMESTAMP and once with normal timestamp conversion.
It pairs the same first selected event by provider GUID, event ID, PID, TID and
payload hash. Both traces report clock type1 and frequency10000000, matching
the helper. Their lost-event counts are0.

The relation uses the paired raw QPC, converted FILETIME and ETL StartTime.
`analyze-direct.py` retains rational arithmetic until conversion to microseconds,
then checks the xperf header StartTime against the reader's header. It excludes
all events outside [qpc_start, qpc_start +3s), including restoration/setup/rundown.
The resulting active intervals are:

- Input ETL:670469.5..3670469.5us.
- Graphics ETL:598798.8..3598798.8us.

Precision is limited by QPC100ns and xperf's integer-microsecond export, not by
the later UTC string in cursor.csv. No sub-microsecond latency claim is needed.
As an independent positive check, the raw-reader clock offset for the earlier
SendInput input.etl is13953701501 QPC ticks, inside the120us-wide intersection
of all488 measured SendInput/WakeMIT brackets.

The API's raw/default conversion behavior is documented by Microsoft's
[EVENT_TRACE_LOGFILEW](https://learn.microsoft.com/en-us/windows/win32/api/evntrace/ns-evntrace-event_trace_logfilew).
The native reader compiled with MSVC /W4 /WX against local SDK10.0.26100.
It consumes files only and does not start an ETW session or a GUI.

## Artifacts and next comparison

- `graphics.etl` SHA256
  `8742B1A3164BEE2AD00040E359D04E7E56C03FF3F64675815EC3024D66B2CCD8`.
- `input.etl` SHA256
  `95024620E5B83A67B191061474F6E67BA3337690482D5780AAE48A6EA7952475`.
- `graphics-anchor.json`, `input-anchor.json`, `direct-analysis.json/txt`,
  `graphics-active-events.json`, `input-active-events.json`, decoded event CSVs.
- Reader/build/parser sources: `scratch/m9/input-control146/etl-anchor.c`,
  `build-anchor.ps1`, `analyze-direct.py`.

Compare a matched post-S4 run using the same helper, duration, desktop identity,
heavy-summary setting and trace keywords. First require real logical coordinate
changes again; then compare DWM work gaps and durations. Treat the sparse full
Present events as their own stream. Hardware scanout or the owner's observation
is still needed to decide whether the physical cursor was smooth.
