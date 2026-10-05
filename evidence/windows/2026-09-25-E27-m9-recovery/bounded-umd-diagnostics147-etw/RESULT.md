# Quiet UMD147: matched Winlogon cursor ETW

The same three-second Winlogon cursor workload improves from 84 to176 present starts after verbose UMD logging is disabled. Render median falls from23.849 to1.919ms; no SCHEDULE_GLITCH events occur. Whole-trace render/present maxima are3.689/4.028ms, compared with48.520/59.709ms before. The prior long synchronous logging stacks and long CPU memcpy interval do not recur in this capture.

## Identity and method

- Coordinator confirmed completed transfer before analysis: graphics.etl106954752bytes, input.etl1703936bytes. This task performed offline reads/analysis only, no lab or production edits.
- Both this run and cache147-01 use the same SetCursorPos-direct trajectory, duration3000ms, WinSta0/Winlogon and original960:600 position. Helper PID3104 replaces7552. This is a closer scene match than the earlier comparison with Default desktop.
- Coordinator reports unchanged KMD147 and only Debug.cpp changed in UMD. DWM was restarted, so process/resource ages are not identical; ETW identifies all216 render pairs as DWM3636, previously DWM2004. It records new UMD base0x00007ffb5f9c0000, image size0x0420d000, timestamp0x6ab5e828. LogonUI's loaded identity is the coordinator's separate witness.
- Matching local DLL SHA256 FF864DB8CBD6512DDD5538941A6332A9F85AF0D6EA287A5C3FC4A2C2C771E6D5; PDB A9A364A9FC625EEAEEBCCAB9952D4BC57128051124ABE2BF68EE151603E8A385. SHA256SUMS records ETL/helper/analyzer/anchor/DLL/PDB identities.
- Reused existing etl-anchor.exe and analyze-direct.py/analyze-full-dwm.py unchanged. QPC frequency10000000, raw and converted matching-event anchors, checked trace-header start times. Exact active windows are graphics[1725306,4725306)us, input[1932756.3,4932756.3)us.
- Both ETL headers report Events Lost=0 and Buffers lost=0; native anchors also report lost_events=0. Graphics header duration30s; input35s. No loss is not proof every possible event/stack provider was enabled.

## Matched active three seconds

Times are milliseconds; median andp95 follow the unchanged analyzers. Duration pairs require both endpoints inside the active window.

| Metric | cache147-01 verbose | quiet147-01 |
|---|---:|---:|
| Logical movements matched/sampled |187/187|188/188|
| Logical observation gap p95/max |19.725/23.273|17.783/21.106|
| SetCursorPos-to-observation median/max |7.012/10.891|7.211/8.342|
| Win32k QueueInputMessage/RetrieveInputMessage |97/97|177/177|
| Render starts/complete pairs |145/144|179/179|
| Render duration median/p95/max |23.849/29.235/48.520|1.919/2.206/3.689|
| Render-start gap median/max |29.433/84.759|16.678/17.064|
| Present starts/complete pairs |84/83|176/176|
| Present duration median/p95/max |5.531/9.676/59.709|1.968/2.386/4.028|
| Present-start gap median/p95/max |33.590/48.618/84.456|16.726/17.261/33.429|
| Hardware vsync events/largest gap |177/33.319|180/16.873|
| SCHEDULE_GLITCH events |83|0|

This is about58.7 present starts per second, with a largest present-start gap33.429ms. Render cadence itself stays within17.064ms. Logical GetCursorPos agreement is not a physical-pixel measurement; no one-to-one mapping from the Win32k event counts to physical cursor frames is asserted.

## Whole trace

| Metric | verbose | quiet |
|---|---:|---:|
| Render complete pairs |186|216|
| Render p95/max ms |28.809/48.520|2.202/3.689|
| Present complete pairs |87|179|
| Present median/p95/max ms |5.555/9.676/59.709|1.958/2.393/4.028|
| SCHEDULE_GLITCH events |87|0|

No completed render/present longer than4.028ms appears. The largest whole-trace present-start gap13.978s and render-start gap1.501s occur while the later mostly unchanged scene waits for work (WFW maximum1.497s); those are not evidence of a continuously rendering hang. Whole-trace statistics include the remaining capture/rundown interval. Both longest measured render and present occur within the active three seconds.

## Exact sampled stacks and remaining work

`analyze-sample-stacks.py` joins DWM3636 SampledProfile events with Stack rows at exactly matching timestamp/TID. The unchanged general CPU excerpt also includes nonsampled event stacks, but those are not used as a CPU-time denominator. `symbols.txt` resolves the captured UMD RVAs using the matching new PDB.

- Active DWM sampled events:1267 versus3195 previously. Main thread4224 accounts for648, compared with2855 samples on previous main4336. These are sample counts, not exact wall-time accounting.
- New DebugPrintf resolved frames remain in36/1267 samples (2.84%): RVA0xBCCF5 at Debug.cpp:53 CreateFileA continuation24samples; RVA0xBCD29 at Debug.cpp:58 WriteFile continuation12samples. Their caller is _Present+0x3cb, DxgiFns.cpp:213, the deliberately retained BC250 Perf diagnostic. Previously five resolved DebugPrintf frames appeared in1687/3195 samples (52.8%). This comparison uses exact PDB-resolved frame addresses; it is not a claim that no other sample could contain DebugPrintf.
- Retained Perf logging is emitted for the first120 presents and every60th thereafter. Therefore some low-volume logging remains by design; the new trace has no indication that it prevents the measured cadence.
- Worker stacks now include `cnd_wait -> util_barrier_wait -> thread_function` (threads_win32.c:190, u_thread.c:246, lp_rast.c:1220), rather than the previous long shade_quads CPU loop. Worker sample counts are distributed across many short intervals; no long busy rendering loop is established here.
- Longest render[3460830,3464519]us lasts3.689ms, with only3 matched DWM sampled events, all main4224 and no matched UMD frame. This sample density is insufficient for finer causal attribution.
- Longest present[1744689,1748717]us lasts4.028ms,6 sampled DWM events,3 main4224. One contains `memcpy -> util_copy_rect -> util_resource_copy_region -> lp_resource_copy -> ResourceCopyRegion`. It resolves memcpy RVA0x2B45DFE, util_copy_rect0x2AC85E, util_resource_copy_region0xD03A0, lp_resource_copy0x3374AF, ResourceCopyRegion0xB26B9. The copy still exists but is not a long dominant stall in this interval.
- First kernel Flip is1747475us,2.786ms after present entry; QueuePacket1747549..1747711us (0.162ms), MMIOFlip1747672us. No queued GPU delay resembling the former59ms interval is present.

The results support keeping verbose per-entrypoint logging opt-in. They do not justify a primary-shadow redesign on the basis of the former memcpy sample alone. A separate controlled animation workload and S4 transition are still needed to assess the owner's wider symptom. Resource pointer/cache roles cannot be inferred from these ETW callstacks.

## Scope and artifacts

Outputs: direct-analysis.json/txt, full-dwm-analysis.json/txt, graphics/input-anchor.json, graphics/input-events.csv, CPU/sample stack excerpts and scripts, logging-samples.json, symbols.txt, validation.txt, SHA256SUMS. Baseline is ../cache147-01/RESULT.md and its raw outputs.

The coordinator separately reports shared/pixel controls PASS and both DWM/LogonUI loading the quiet DLL. Those checks were not rerun here. This report does not claim physical smoothness for every scene, automatic startup confirmation, S4 acceptance, or completion of M9. The idle presentation/freshness condition is outside this ETW conclusion.
