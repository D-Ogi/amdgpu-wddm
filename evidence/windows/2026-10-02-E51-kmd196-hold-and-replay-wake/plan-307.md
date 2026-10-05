# Trial 307: deferred replay ON with adapter119 at HIGH + RT (Witcher 3, Steam start, uncapped): promotion check
Candidate shell adapter119 5A1B7BAF (m15/replay-wake e9f5e701), engine D79FEC49 and ICD F9DCB33B registered; profile
with deferred-replay for this session (restored after). Same room and sequence. Reference: 301 (registered, replay
off: 9.4/s, 98 % GPU busy).
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ADAPTER=adapter119 run-m157.sh 307 high-rt 600`
- Question: is the RT path correct with replay on (DispatchRays, BuildRaytracingAccelerationStructure and
  SetPipelineStackSize go through the ring; SetPipelineStackSize drains every ring)?
- Expected: world with RT reflections as in 301; 9.4/s within noise (GPU-bound); no removal.
- Refutation: removal, fault or 0x116; wrong, black or flickering geometry; RT effects missing.
- Bounds: interactive game session about 6 min, at most 1200 s; plug every 15 s; 87 C cap (301 ended at 85.1 C: quit
  at once above 86 C); 300 W PSU.
