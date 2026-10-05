# KMD147 / cached shared-resource UMD: offline ETW comparison

The captured Winlogon workload no longer shows the former 250 ms render / 750 ms present pauses, but it still misses a 60 Hz frame budget. Its immediate measured bottleneck is synchronous per-entrypoint UMD logging on the DWM main thread. A separate 59.709 ms present spends about 49 sampled milliseconds copying on the CPU before submitting its kernel flip. This is not evidence that the cache change fixed animation or S4 stutter.

## Capture and method

- Input files were read only after the coordinator confirmed completed transfer: graphics.etl 124780544 bytes; input.etl 1703936 bytes. No lab action or source edit was performed for this analysis.
- Coordinator identifies KMD147 and the new UMD; ETW independently identifies DWM2004 and its UMD image base 0x00007ffb5f9c0000, image size 0x0420d000. Matching local DLL SHA256 is E0ACCCB5591CD2B9C8DC78B83BF1C78DF4F7ECB001D9FA77FE1C323BC48E91CB; PDB E8688F4E1DEBB6004790239071FC2208E01491405306A21C429EDE9D941EA7AC. Artifact identity on the lab is the coordinator's separate deployment witness.
- Existing etl-anchor.exe and analyze-direct.py/analyze-full-dwm.py were reused unchanged. Raw QPC events are matched to converted FILETIME events by provider/event/process/thread/payload identity. Both traces use QPC frequency 10000000; exported header start times are checked. SHA256SUMS records inputs and analyzers.
- Active graphics window is [1439774,4439774) microseconds relative to its trace; active input window [1645292.4,4645292.4). Both are the helper's exact three seconds, not an inferred wall-clock window.
- Both input and graphics report Events Lost=0, Buffers lost=0; both native anchors report lost_events=0. Graphics trace header length is 30 seconds; input 35 seconds. All observed DWM render events in this graphics trace belong to DWM2004.
- Important non-equivalence: new helper ran on WinSta0/Winlogon, while cold-02 and after-dwm-01 used WinSta0/Default. This scene and process/resource population changed. Therefore the comparisons below are descriptive, not an isolated cache-policy speedup claim. The coordinator's matched v1/v2 Lock2 probe is separate evidence.

## Three-second active interval

All durations and gaps below are milliseconds; p95 uses the existing analyzer's sorted floor(n*0.95) element. Start/stop durations require both ends within the window.

| Metric | cold-02 KMD146 | after-dwm-01 KMD146 | cache147-01 |
|---|---:|---:|---:|
| Logical movements matched / sampled | 188/188 | 163/163 | 187/187 |
| Logical observation gap p95 / max | 16.342 / 19.448 | 17.844 / 162.664 | 19.725 / 23.273 |
| SetCursorPos-to-observe median / max | 4.112 / 7.583 | 5.168 / 8.242 | 7.012 / 10.891 |
| Win32k QueueInputMessage / RetrieveInputMessage | 180/180 | 133/133 | 97/97 |
| DWM render starts / complete pairs | 180/180 | 135/135 | 145/144 |
| DWM render duration median / p95 / max | 0.808 / 1.041 / 3.153 | 1.710 / 1.930 / 255.440 | 23.849 / 29.235 / 48.520 |
| DWM render-start gap median / max | 16.676 / 16.823 | 16.677 / 258.057 | 29.433 / 84.759 |
| DWM present starts / complete pairs | 5/5 | 126/126 | 84/83 |
| DWM present duration median / p95 / max | 3.924 / 5.189 / 5.189 | 1.580 / 12.263 / 69.424 | 5.531 / 9.676 / 59.709 |
| DWM present-start gap median / max | 500.753 / 984.011 | 16.726 / 270.266 | 33.590 / 84.456 |
| Hardware vsync events / largest gap | 180 / 16.796 | 180 / 16.788 | 177 / 33.319 |
| DWM SCHEDULE_GLITCH events | see raw baseline | 8 | 83 |

The new scene presents about 28 times/second, with 83 glitch events. Cold-02's sparse presents reflect a largely unchanged scene; its many renders and few presents must not be presented as a low frame-rate regression. Likewise the logical cursor samples do not prove physically displayed cursor positions. Win32k message payloads are not independently established as one WM_MOUSEMOVE per helper coordinate.

## Whole graphics trace

| Duration metric | cold-02 | after-dwm-01 | cache147-01 |
|---|---:|---:|---:|
| Present pairs | 32 | 207 | 87 |
| Present median / p95 / max ms | 6.335 / 10.738 / 11.920 | 1.687 / 496.309 / 752.578 | 5.555 / 9.676 / 59.709 |
| Render p95 / max ms | 1.764 / 5.534 | 249.310 / 259.531 | 28.809 / 48.520 |

No 250-750 ms completed render/present is present in the new trace. The longest render and present both occur within the active helper interval, so they are not solely trace-stop/rundown costs. Later present-start gaps reach 12.565 seconds and render-start gaps 1.502 seconds, but wait-for-work reaches 1.487 seconds and the later scene mostly does not present. These gaps alone are not a continuously rendering stall. Whole-trace maxima include the remaining capture/rundown interval and are not a controlled benchmark.

## Sampled CPU callstacks: logging is now prominent

`analyze-sample-stacks.py` joins only SampledProfile events for DWM2004 to Stack rows with exactly the same timestamp and TID. This is stronger than counting every Stack event, which also includes syscall/context-switch stacks. `sample-stacks.json` keeps the matched rows; `symbols.txt` resolves the exact captured UMD addresses against its matching PDB. The general `cpu-excerpt.json` includes all event-stack rows only as context, not as a CPU-time denominator.

In the active three seconds there are 3195 DWM sampled-profile events, of which 2855 belong to main thread4336. At least 1675 samples have one of the four directly resolved DebugPrintf frames at offsets 0xBCBE5,0xBCC19,0xBCC22,0xBCB4C. Including the resolved 0xBCB3E frame gives 1687/3195 samples (52.8% of all DWM samples, all on main4336); the exact deduplicated counts are in logging-samples.json. This differs from the previous 12 heavily occupied raster-worker threads.

| UMD RVA | PDB symbol and source | Active samples containing frame |
|---|---|---:|
| 0xBCBE5 | DebugPrintf+0x115, Debug.cpp:41, CreateFileA continuation | 1048 |
| 0xBCC19 | DebugPrintf+0x149, Debug.cpp:46, WriteFile continuation | 366 |
| 0xBCC22 | DebugPrintf+0x152, Debug.cpp:49, CloseHandle path | 170 |
| 0xBCB4C | DebugPrintf+0x7c, Debug.cpp:38, OutputDebugString continuation | 91 |

The longest render [2729119,2777639] us lasts 48.520 ms. It contains 49 DWM CPU samples, 47 on main4336; 33 have one of the five resolved DebugPrintf frames. The main thread visits Ntfs/FLTMGR stacks through logging. The source at `scratch/mesa-main-20260924/src/gallium/frontends/d3d10umd/Debug.cpp:27` calls OutputDebugStringA for every message and opens/writes/closes a log file for the first10000 messages (and later selected Perf/SetError lines). `Debug.h:63` enables LOG_ENTRYPOINT unconditionally. Caller examples resolve to ResourceMap and PsSetShaderResources. Source reading and the sampled callstacks agree.

This supports making verbose entrypoint logging opt-in and leaving low-rate identity/failure/performance diagnostics. A matched repeat on the same desktop after that change is needed to quantify the actual gain; do not infer that all remaining delay is logging.

## Remaining CPU copy during present

Longest present: [1482698,1542407] us, 59.709 ms, main4336. Of 62 DWM CPU samples in the interval, 59 are main4336 and 49 have this chain:

`memcpy -> util_copy_rect (u_format.c:92) -> util_resource_copy_region (u_surface.c:346) -> lp_resource_copy (lp_surface.c:114) -> ResourceCopyRegion (Resource.cpp:834)`.

The sampled memcpy instructions resolve at RVA0x2B45DBE/0x2B45DCE/0x2B45DA3. The first kernel Flip is at1537576 us, 54.878 ms after present entry. QueuePacket start1537652, stop1537884 (0.232 ms); MMIOFlip1537817. Most of this interval therefore precedes the kernel GPU queue, unlike a stalled queued GPU operation.

ETW stacks establish CPU copy cost but not source/destination pointers, primary/shared role, actual mapping cache attributes or copy dimensions. It would be premature to label this particular memcpy a WC read or to add Cached to a primary. The previous shade_quads worker hotspot does not dominate this captured scene. First remove proven logging overhead, then correlate remaining copy cost with actual resource-role/cache observations if still material.

## Artifacts and limits

- direct-analysis.json/txt and full-dwm-analysis.json/txt: unchanged existing analyzer outputs.
- graphics/input-anchor.json; graphics/input-events.csv; both dump logs.
- analyze-cpu.py, cpu-excerpt.json/txt; analyze-sample-stacks.py, sample-stacks.json/txt; logging-samples.json; symbols.txt; validation.txt; SHA256SUMS.
- No loss reported is not proof every potential provider/stack category was enabled. The sampled stacks are statistical evidence, not exact accounting of every millisecond or full causal proof for later owner-observed animations.
- No new S4 transition is contained in this analysis. No assertion of S4 acceptance, physical cursor smoothness, animation correctness or M9 completion follows.

