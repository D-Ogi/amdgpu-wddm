# Trial 305: deferred replay ON with the wake threshold (adapter118), CPU profile of uncapped Witcher 3 LOW
Candidate shell adapter118 406FD683 (bc250-win m15/replay-wake 2322765d on f33f9754: a sleeping replay worker is
woken once 8 KiB are pending, not by the first publish); engine D79FEC49 and ICD F9DCB33B registered. Profile with
deferred-replay for this session (set-profile.ps1 before, restored after). Same method as 304 (A = 304: replay on,
registered shell; reference 300: replay off). Host: engine-ddi harness plain / --deferred-replay / VVL with the
harness built from the same tree (scratch\m15\replay-wake\harness).
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter118 SESSION_ETW_ARGS="-Seconds 40
-LatestB 1100 -WorldSeconds 40" run-m157.sh 305 low 600`
- Question: does the threshold remove the 1.16 ms/frame of replay_wake system calls on the main thread, and what do
  the drains at Close and ResetCommandPool cost when the worker starts later?
- Expected: main-thread kernel time under replay_wake below 0.1 ms/frame; main thread on-CPU about 1 ms/frame lower
  than 304; Close/pool drain waits up by at most 0.3 ms/frame; rate 54-56/s; image identical.
- Refutation: rate within +-1 % of 304 with the wakes gone (the drain waits ate it); wrong image; fault or removal.
- Bounds: interactive game session about 6 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU.
