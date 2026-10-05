# Trial 317: KMD 0.7.196.1 (C19 fix) at LOW, Witcher 3 uncapped, with the GPU timeline and the ETW alignment
KMD 0.7.196.1 registered 20:05Z (kmd196-deploy001, commit 28b89a4c: a held submission waits on the gfx retirement
event after a bounded 500 us spin, not on 1 ms timer ticks; held-time counters in the guard log). Candidates as 314
(swapped in place, restored after): shell adapter123 A27C9617, engine D4057452 and ICD 102D77EC from
scratch\m15\dp11-freeze; no statistics knob (ICD cfg cleared).
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter123
M157_ENGINE=<BC250_ROOT>/scratch/m15/dp11-freeze/engine/amdgpu_wddm_vkd3d.dll
M157_ICD=<BC250_ROOT>/scratch/m15/dp11-freeze/icd/amdgpu_wddm_radv.dll SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 317 low 600`; world confirmed on a shot, then the GPU timeline (60 s) and a 64 s pan.
- Question (C19 kill test): does the KMD hold behind DWM's job disappear? Class C/held per frame (align.py) was 3.14
  ms at LOW (313, K134); the guard log's held time per hold was 4.7 ms.
- Expected: held time per hold below 1 ms (spin-only share high, timeout wakes near 0); GFX idle per frame down from
  5.9 ms by about 2.5 ms; rate above 313/315 (54.8/54.3 per s) by about 10 %, if the CPU keeps up (class A).
- Refutation: held time per hold above 2 ms, idle per frame unchanged, rate unchanged; or a fault, a TDR, a removal,
  timeout wakes as a large share (lost end-of-pipe interrupts).
- Bounds: interactive game session about 7 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU.
