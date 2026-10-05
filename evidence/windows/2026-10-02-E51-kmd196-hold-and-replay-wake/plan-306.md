# Trial 306: deferred replay ON, wake threshold plus no pool drain (adapter119), CPU profile of uncapped Witcher 3 LOW
Candidate shell adapter119 5A1B7BAF (bc250-win m15/replay-wake e9f5e701: adapter118 + ResetCommandPool drains no ring
while no list of that pool is open); engine D79FEC49 and ICD F9DCB33B registered. Profile with deferred-replay for this
session (set-profile.ps1 before, restored after). Same method as 305 (A = 305). Host: engine-ddi harness 426 ok plain /
--deferred-replay / VVL built from the same tree.
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter119 SESSION_ETW_ARGS="-Seconds 40
-LatestB 1100 -WorldSeconds 40" run-m157.sh 306 low 600`
- Question: does C15 hold: ResetCommandPool's main-thread cost (0.54 ms/frame inclusive in 305, 0.33 of it waiting)
  drops to the allocator reset itself, and the frame rate follows?
- Expected: ResetCommandPool inclusive below 0.2 ms/frame, no wait_until under it; rate 54.5-55.5/s; image identical.
- Refutation: ResetCommandPool still waits (an open list of the pool at reset: then the count is wrong or the game
  resets with a list open); rate within +-1 % of 305; wrong image; fault or removal.
- Bounds: interactive game session about 6 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU.
