# Trial 320: registered triplet at HIGH + RT on KMD 0.7.196.1 (Witcher 3, Steam start, uncapped): KMD regression check
Registered triplet (shell adapter119 5A1B7BAF, engine D79FEC49, ICD F9DCB33B; profile with deferred-replay), nothing
swapped; KMD 0.7.196.1 registered 20:05Z (spin-then-event wait for held submissions). Reference: 307 (KMD 193, same
triplet: 9.5/s, GPU busy 97.9 %). With the GPU timeline (60 s) during the pan.
`SESSION_VSYNC=false SESSION_LIMIT_FPS=240 SESSION_LAUNCH=steam SESSION_ETW_ARGS="-Seconds 40 -LatestB 1100
-WorldSeconds 40" run-m157.sh 320 high-rt 600`
- Question: is the RT path correct and fault-free on KMD 196 (RT runs are the 0x116 class: long jobs on the ring,
  holds behind DWM jobs of 100 ms frames), and does the RT rate move (GPU-bound, so about 9.5/s expected)?
- Expected: world with RT reflections as in 307; 9.5/s within noise; holds all spin-only or event, 0 timeouts.
- Refutation: removal, fault or 0x116; wrong, black or flickering geometry; timeout wakes (a missed retirement).
- Bounds: interactive game session about 6 min, at most 1200 s; plug every 15 s; 87 C cap (quit at once at 87);
  300 W PSU.
