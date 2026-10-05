# Trial 318: KMD 0.7.196.1 (C19 fix) at HIGH, Witcher 3 uncapped, with the GPU timeline and the ETW alignment
KMD 0.7.196.1 registered 20:05Z (kmd196-deploy001, commit 28b89a4c: a held submission waits on the gfx retirement
event after a bounded 500 us spin, not on 1 ms timer ticks; held-time counters in the guard log). Candidates as 314
(swapped in place, restored after): shell adapter123 A27C9617, engine D4057452 and ICD 102D77EC from
scratch\m15\dp11-freeze; no statistics knob (ICD cfg cleared).
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter123
M157_ENGINE=<BC250_ROOT>/scratch/m15/dp11-freeze/engine/amdgpu_wddm_vkd3d.dll
M157_ICD=<BC250_ROOT>/scratch/m15/dp11-freeze/icd/amdgpu_wddm_radv.dll SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 318 high 600`; world confirmed on a shot, then the GPU timeline (60 s) and a 64 s pan.
- Question (C19 at HIGH): class C/held was 4.23 ms/frame at HIGH (314, K135); 317 cut it at LOW from 3.14 to 0.24.
- Expected: rate 31.3 -> about 35/s (+13 %, the K135 upper bound); idle per frame 5.8 -> about 2 ms; held per hold
  about 0.2 ms, spin-only.
- Refutation: rate below 33/s with C/held gone (the idle moved to another class), or a fault, TDR or removal.
- Bounds: interactive game session about 7 min, at most 1200 s; plug every 15 s; 87 C cap (314/316 reached 86.1-86.2 C
  at the end of the pan: quit at once at 87); 300 W PSU.
