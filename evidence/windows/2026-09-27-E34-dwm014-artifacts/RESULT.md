# M576: DWM014 still has dynamic visual corruption

Candidate039 UMD50491FA3/ICD3508416F, including the M575 shader-lifetime fix,
runs GPU DWM952 for185.736 measured seconds. The owner explicitly confirms
that artifacts persist. The primary capture dynamic-26.bmp independently shows
large stretched window contents, cyan regions and black blocks. The final GDI
capture screen.png shows a wide cyan strip and triangle. Visual acceptance FAILS.

ETW records3872 matching DWM DMA start/stop pairs, matching submission and
completion IDs, no preempted pairs and zero lost events/buffers. This proves
attributable completed GPU work, not correct pixels. Static red/overlap ROIs
match8000 expected pixels in the baseline and final primary, but all8000
positions fail in the final GDI capture. The captures are not simultaneous.
The successful runner result cannot override the visual failure.

dynamic-color-analysis.json counts cyan outside the control's entire motion
envelope [600,120,1016,362). Most captures have seven unrelated cyan background
pixels there; dynamic-26.bmp has167179 and screen.png has51070. These are
diagnostic counts, not a complete visual oracle. Original screenshots, primary
images, ETL, decoded events and full diagnostics stay private; hashes identify
them. Only derived findings and measurements are published.

The partial map audit is retained with incomplete-line counts. Periodic explicit
primary/GDI captures are diagnostic readbacks; this run is not a new complete
proof excluding all CPU frame copies. No performance claim is made.

Baselines UMD8279AC7F/registered ICD93B1D1FD were restored and hash-checked;
CPU DWM4400 replaced GPU952. Main, composition and watchdog tasks were stopped
and unregistered. No candidate promotion. BD-043 and G0 remain open.
Parent revision a7e5872. The next isolation should cover changing viewport,
input-layout/multiple vertex streams and buffer subrange reuse, not repeat the
already-passing fixed-state offscreen controls as if they covered DWM.
