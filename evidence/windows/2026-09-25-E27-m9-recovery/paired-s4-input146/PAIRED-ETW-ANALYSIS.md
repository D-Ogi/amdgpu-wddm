# Paired direct-position traces: cold, post-S4, and after DWM restart

2026-09-25. Offline analysis only. No driver/helper modifications or lab actions
by the analyst. Raw data remains outside the public repository.

## Finding

The measured post-S4 stall is CPU llvmpipe work before the flip enters dxgkrnl's
queue. A 757.086 ms DWM Present contains almost continuous execution on 12 renderer
workers; the flip itself enters the queue only near the end and completes its
queue packet in 66 us. This example does not support a 500 ms KMD blit or GPU-fence
wait. DWM restart does not eliminate the expensive operations: the new process
still has approximately 250 ms rendering and up to752.578ms Present intervals.

This identifies a bottleneck location, not its low-level cause. Cache/PAT/MTRR,
CPU throttling, shader variants, resource layout, or memory-bandwidth explanations
remain hypotheses. No claim is made that this trace proves which one changed.

The root reports the same boot 03:57:55 and unchanged 146; DWM 704 spans the cold and
post-S4 runs. DWM was restarted alone at 04:42:44 to 6076, without a GPU reset.
The owner reports regular cursor and sign-in-animation stutter after S4, with
only a small improvement after DWM restart. Those observations are independent
of the ETW counters and are not replaced by them.

## Exactly three seconds of direct cursor positioning

All three helpers attach to session 1/WinSta0/Default and use the same direct
SetCursorPos variant. Cold/post start at 960,600; after-DWM starts at 848,468, so
that run's screen content/trajectory origin is not exactly identical.

| Measure | Cold02 | Post-S4 | After DWM restart |
|---|---:|---:|---:|
| Samples / coordinate matches |188/188|188/188|163/163|
| Coordinate changes |187|187|162|
| Distinct coordinates |61|61|61|
| Maximum observation gap, ms |19.448|17.925|162.664|
| DWM render pairs |180|179|135|
| Maximum render duration, ms |3.153|1.170|255.440|
| Maximum render-start gap, ms |16.823|16.905|258.057|
| SCHEDULE_PRESENT pairs |5|0|126|
| Maximum Present duration, ms |5.189|not observed|69.424|
| Maximum Present-start gap, ms |984.011|not observed|270.266|
| VSyncDPC events |180|180|180|
| Maximum vsync gap, ms |16.796|16.788|16.788|
| Win32k Queue/RetrieveInputMessage pairs |180|179|133|

The initial post-S4 three-second stimulus changes logical positions and triggers
DWM work every refresh, but all 179 render stops have no-present-needed result;
there are no full Present calls to time. This is why that short window alone
misses the expensive frame path. After the DWM restart, expensive rendering is
also directly present inside the controlled three-second window: render at
relative 0.981725..1.237165s takes 255.440 ms. This example is not WPR stop/rundown.

GetCursorPos confirms logical coordinates, not displayed pointer pixels. The
Win32k Queue/RetrieveInputMessage fields decode message 0, so their counts are not
claimed as verified WM_MOUSEMOVE coordinate payloads. Regular vsync does not
prove smooth visible cursor updates. The driver exposes a software cursor.

## Wider traces: expensive frame path

These figures include the recorded period after motion, including WPR stop and
rundown. They describe observed call costs; they are not a controlled idle
benchmark or an unbiased application-frame-rate comparison. The capture script
waits one second after helper completion before starting WPR stop.

| Full-trace cost, ms | Cold02 | Post-S4 | After DWM restart |
|---|---:|---:|---:|
| Present count |32|42|207|
| Present median |6.335|493.615|1.687|
| Present P95 |10.738|740.615|496.309|
| Present maximum |11.920|757.086|752.578|
| Render P95 |1.764|251.463|249.310|
| Render maximum |5.534|292.808|259.531|

Post-S4 has 252 render-stop events: 210 no-present-needed and 42 present-needed,
followed by 42 Present calls. Vsync remains regular throughout the selected GPU
notification stream, maximum 16.838 ms. There is no missing-vsync-sized hole
explaining the CPU-render stalls.

DWM restart increases the proportion of short Present calls, lowering its median;
it does not remove the slow tail or 250 ms rendering. A low median after restart
therefore must not be reported as a repair.

## One 757 ms Present, with source attribution

Times are microseconds relative to post-S4 graphics ETL start:

1. DWM 704/TID2840 SCHEDULE_PRESENT starts4428887.
2. Main thread switches out at4428988, Waiting/WrAlertByThreadId.
3. Renderer workers 3380,3384,3388,3392,3396,3400,3404,3408,3412,3416,3420,3424
   each receive 746-756 CPU samples in the inspected~759 ms excerpt. The capture's
   approximately 1 ms sample period makes this roughly 9 aggregate CPU-seconds,
   near continuous work on 12 CPUs. The main thread has only 3 samples there.
4. DxgKrnl Flip first occurs5185574,756687us after Present entry.
5. QueuePacket start5185617, MMIOFlip5185660, QueuePacket stop5185683:66us.
6. SCHEDULE_PRESENT stops5185973:757086us total.

The known DLL sampled address 0x00007ffbc57fce34, with ETL image base
0x00007ffbc54a0000, is RVA 0x35ce34. Matching local DLL SHA256 is
D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D.
Its matching local PDB resolves this to `shade_quads+0x114`,
`scratch/mesa-main-20260924/src/gallium/drivers/llvmpipe/lp_rast_linear_fallback.c:112`.
Nearby sampled RVA 0x35ce08 is the same function. That wrapper calls the compiled
fragment shader for 4x4 blocks. Most samples are in anonymous JIT code called by
these workers, not a KMD copy loop. Local-symbol result is saved in
`post-s4-01/umd-symbols.txt`; lookup loads symbols only, never executes the DLL.

This supports a CPU rendering/flush bottleneck while Present waits for worker
completion. It does not identify an exact shader instruction or prove the named
wait routine inside Mesa: JIT unwind/symbol metadata is missing. The hottest JIT
addresses differ between cold and post-S4, so this is not a same-shader throughput
measurement. A memory or CPU-frequency benchmark is a useful next discriminator,
not a conclusion already established by these traces.

## Alignment, integrity, reproduction

`etl-anchor.exe` pairs an actual event's raw-QPC timestamp with its standard
ProcessTrace FILETIME conversion using GUID/ID/PID/TID/payload hash. Both ETLs
in each run report QPC 10 MHz, consistent with the helper. Analysis uses the helper's
[qpc_start,qpc_start+3s) interval, not the later UTC-header string. Integer xperf
microsecond export is the remaining coarse resolution; no millisecond-scale
alignment estimate is needed. The raw anchor was independently checked against
the earlier 488 SendInput/WakeMIT call brackets.

Active graphics-relative windows:

- Cold:598798.8..3598798.8us.
- Post-S4:672762.1..3672762.1us.
- After DWM restart:603897.9..3603897.9us.

All graphics headers show zero lost events/buffers; the input anchors report
zero lost events. Original files are unchanged. Per-run `direct-analysis.json`,
`full-dwm-analysis.json`, anchor JSON and active-event excerpts hold raw metrics.
`post-s4-01/long-present-excerpt.json` retains the inspected CPU/thread/queue data.
Tools are in `scratch/m9/input-control146/`: `analyze-direct.py`,
`analyze-full-dwm.py`, `analyze-long-present.py`, `SymbolLookup.cs`, `etl-anchor.c`.

Post-S4 hashes:

- graphics:8C985EE4A8F254FC7EBB235B12D201BEBF1CBAD5AE6F70EB3C542E928A8E28B9.
- input:6B864C3F5E856B4187E8A5250C932557E521B1D9B655949C7C5553FE9ECDE96C.

After-DWM hashes:

- graphics:1D06C7E2C7FA490871518CB25D45A299F53811B50BF9F8B4F0A62D30E312AB0C.
- input:033BCE1E6527D05E43D14DBD2779573B97A0E8F482D2CF7BB143001ED034130D.
