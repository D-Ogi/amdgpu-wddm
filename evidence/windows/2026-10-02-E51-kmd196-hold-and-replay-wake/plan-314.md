# Trial 314: artifact12 triplet at HIGH with the GPU timeline, Witcher 3 uncapped
Candidates (swapped in place, restored after): shell adapter123 A27C9617 (bc250-win m15/entry-fast2 f638ee3d on
525fa8bd: Close as the list's last replay entry, the worker yields 1 ms before it sleeps, RecordingScope ctor inline;
host 19/19 shell tests, replay-test, engine-ddi harness 466 ok plain/replay/VVL), engine D4057452 and ICD 102D77EC
from scratch\m15\dp11-freeze (as 313; vkd3d engine_test THREADED 212 / inline 278 / hang 36 ok on the host).
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter123
M157_ENGINE=<BC250_ROOT>/scratch/m15/dp11-freeze/engine/amdgpu_wddm_vkd3d.dll
M157_ICD=<BC250_ROOT>/scratch/m15/dp11-freeze/icd/amdgpu_wddm_radv.dll SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 314 high 600`. World confirmed on a shot, then `gtl-host.py run w3-high-314 --seconds 60
--detach` and at once a 64 s pan (window B starts at the first movement, so B and the timeline overlap).
- Question (K130 at HIGH): does the GFX also idle when each frame carries more GPU work, and how much of its busy
  time is barrier drain? If the idle per frame stays near LOW's 5.9 ms, it is a per-frame latency of the chain (C18);
  if it shrinks in proportion, it is the CPU side keeping up worse at LOW.
- Expected: image correct at HIGH (held read-only transitions, Close entries); rate at or above the last HIGH runs
  on older stacks (about 31/s); GFX idle below LOW's 32 % (more GPU work per frame).
- Refutation: wrong image or fault, TDR, removal, a "CloseCommandList: the engine Close failed" line.
- Bounds: interactive game session about 7 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU (stop raising
  load near 270 W wall). gtl at most 60 s, only after the world is confirmed; never across a KMD swap.
