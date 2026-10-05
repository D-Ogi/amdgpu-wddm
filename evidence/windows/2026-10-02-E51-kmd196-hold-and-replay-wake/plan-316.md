# Trial 316: barrier and flush statistics at HIGH (draw-path2 artifact13), Witcher 3 uncapped, with the GPU timeline
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
-WorldSeconds 40" run-m157.sh 316 high 600`; world confirmed on a shot, then the GPU timeline (60 s) and a 64 s pan.
- Question (row 1i, C20 at HIGH): are the drains per frame the same kinds as at LOW (K136: 146 image-less GL2
  WB+INV, 150 transfer-write CB+DB drains), or does HIGH add other kinds behind its 9.8 ms/frame drain (314)?
- Expected: counters every 10 s; image correct; rate about 31/s (314: 31.3); drain share about 30 % (314: 30.6).
- Refutation: no counters, wrong image, fault, TDR or removal.
- Bounds: interactive game session about 7 min, at most 1200 s; plug every 15 s; 87 C cap (314 reached 86.1 C: end
  at once after the pan); 300 W PSU. gtl at most 60 s, only after the world is confirmed.
