# Trial 304: CPU profile of uncapped Witcher 3 LOW with deferred replay ON on the registered triplet (C13, row 1c)
Registered triplet DCE50410 / D79FEC49 / F9DCB33B (302), nothing swapped; the application profile gets deferred-replay
added for this session (set-profile.ps1 before, the registered list restored after). Same method as 300 (A = 300,
replay off): Steam start, start room, sequence ab-c13-seq.txt, PerfView CPU window B 40 s from the world trigger.
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 304 low 600`
Profile before: present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff,deferred-replay;
after: the same without deferred-replay.
- Question: with replay on, how much of our 6.95 ms/frame leaves the main thread, and where does the main thread
  wait (drain kinds: close, destroy, pool, space, slots) and for how long per frame? K120 (288/289) saw the median
  shorten 7 % and hitches over 50 ms triple; the conjecture names drain-all on destroys/pool resets and worker wake
  latency.
- Expected: main thread on-CPU per frame below 300's 20.2 ms (our DDIs' main-thread share down to the encode cost,
  1-2 ms), off-CPU waits at Close visible; rate above 51.5/s; more intervals over 50 ms than 300 (0).
- Refutation: main-thread DDI share within 0.5 ms of 300 (the replay does not move work); wrong image; fault.
- Bounds: interactive game session about 6 min, at most 1200 s; plug every 15 s; 87 C cap; 300 W PSU.
