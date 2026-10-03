# Trial 300: CPU profile of uncapped Witcher 3 LOW on draw-path2 artifact5 (arm B2)
Candidates (swapped in place, restored after): shell adapter117 DCE50410 (unchanged from 298), engine D79FEC49
(vkd3d amdgpu-wddm/draw-path c3710ac1 on bed41016: 298's two plus out-of-line texture copy flush 847c1283, inline DGC
batch check c3bc88aa, set of an unchanged VB/IB view skips the VA lookup 62bd9d31, query pool resets only of handed-out
queries dfd5b8ad, VB/IB bind by device address c3710ac1), ICD F9DCB33B (mesa-wddm amdgpu-wddm/draw-path ae98c795 on
89ef0dc8: 298's BO cache plus meta begin only for image barriers a8ce5831, BC250_DRAW_STATS counters (off unless set)
96a2803c/7cfc5b5b, meta begin in a rendering begin only when it runs one a97de225, early return from an end of
rendering that resolves nothing 54d05f4f, copy/fill pipelines kept in the command buffer bfcbcd8c, vertex buffer
descriptors reused across pipeline binds ae98c795). Frozen copy scratch\m15\dp5-freeze (SHA256SUMS verified). Host:
engine-ddi harness 437 ok plain/replay/VVL (run-dp2e), ICD queue tests 0 failures (queue-tests-ae98c795). Method of
298/299; A = 299 (7.56 ms/frame), B1 = 298 (7.10).
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter117
M157_ENGINE=<BC250_ROOT>/scratch/m15/dp5-freeze/engine/amdgpu_wddm_vkd3d.dll
M157_ICD=<BC250_ROOT>/scratch/m15/dp5-freeze/icd/amdgpu_wddm_radv.dll SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 300 low 600`
- Question: how far do the artifact5 changes take our main-thread DDIs below 298 (7.10), in particular IASet* (0.48
  in 298) and the render-pass part of DrawIndexed (begin_render_pass 1.21 inclusive in 291)?
- Expected: our DDIs 6.3-6.8 ms/frame, IASet* at or below 299's 0.39, rate 51-53/s; image identical.
- Refutation: our DDIs within +-0.15 ms of 298; broken geometry (a wrong VB/IB address or a reused stale vertex
  descriptor); fault or removal.
- Bounds: interactive game session about 6 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU.
