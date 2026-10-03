# Trial 315: barrier and flush statistics at LOW (draw-path2 artifact13), Witcher 3 uncapped, with the GPU timeline
Candidates (swapped in place, restored after): shell adapter123 A27C9617 (as 314), engine D8EC9A47 (vkd3d
amdgpu-wddm/draw-path 7356dbe3 = ebc14ce7 + barrier statistics) and ICD 58E64C7F (mesa-wddm amdgpu-wddm/draw-path
74578697 = 9bbd1c90 + flush/drain statistics in DRAW_STATS) from scratch\m15\dp13-freeze (SHA256SUMS verified).
Statistics are on for this session only: SESSION_ICD_CFG=drawstats writes BC250_DRAW_STATS=1 and
BC250_DEFERRED_SUMMARY_S=10 to C:\BC250\tmp\amdgpu_wddm_radv.cfg; the runner clears it after the log pull. Host:
ICD queue tests 33 pass + the known deferred_witness failure; engine-ddi harness 466 ok plain/replay/VVL-syncval;
engine_test threaded/inline/hang PASSED, output identical to ebc14ce7.
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter123 SESSION_ICD_CFG=drawstats
M157_ENGINE=<BC250_ROOT>/scratch/m15/dp13-freeze/engine/amdgpu_wddm_vkd3d.dll
M157_ICD=<BC250_ROOT>/scratch/m15/dp13-freeze/icd/amdgpu_wddm_radv.dll SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 315 low 600`; world confirmed on a shot, then the GPU timeline (60 s) and a 64 s pan.
- Question (row 1i): what drives the barrier drain (24.6 % of the time at LOW in 313): price draw-path2's over-flushes
  A (image-less write barrier = full GL2 write-back + invalidate), B (TRANSFER_WRITE in every UAV barrier = CB+DB
  flush + EOP wait), C (attachment-read dst on image-less barriers), D (forced barrier after a pending UAV clear),
  E (RT/DS -> SRV), F (query copy waits, per-submit preamble) as drains per frame by kind and cause.
- Expected: counters present in the ICD and engine logs every 10 s; image correct; rate lower than 313 by at most a
  few % (counting cost); the timeline's drain share as in 313 (about 24 %).
- Refutation: no counters (the cfg was not read), wrong image, fault or removal.
- Bounds: interactive game session about 7 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU.
