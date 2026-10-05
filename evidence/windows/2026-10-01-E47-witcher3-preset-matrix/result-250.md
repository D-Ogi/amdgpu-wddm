# Trial 250: M15.7 The Witcher 3 HIGH with RT at native 1080p on the registered triplet (adapter107), 7-min session

`run-m157.sh 250 high-rt`: the registered triplet since 247 (shell adapter107 6D22D305, engine 15E3E24E, ICD 2A13235D;
BC250_TRIAL_ACCEPTED=1, nothing swapped), experiments as 249, KMD 0.7.193.1 with DPM on, ceiling 2000 MHz, GPU DWM,
preset HIGH with RT on (EnableRT, RT GI/reflections/shadows/AO as the high-rt preset file sets them), AAMode 0/1, FG
off, DRS off, native 1080p, LimitFPS 60. Interactive: the Kaer Morhen interior route of 245/246/249.

- Question: the M15.7 HIGH+RT row on the current stack - frame rate, GPU share and thermal behaviour at the 2000 MHz
  ceiling, against 234 (HIGH-RT, ICD v3, 1000 MHz under the governor then, 99-107 submits/s, Tctl peak 86.0 C).
- Expected: functional-restored; RT image correct; no fault, no TDR; GPU 3D near 100 % (RT is GPU-bound, K67); DPM at
  2000 MHz until the thermal cap holds it near 87 C; frame rate above the earlier RT sessions.
- Refutation: a fault, hang or 0x116 (RT exercises BVH builds and the release gate under heavier paging); a wrong RT
  image; the guard stopping the game (two readings above 87 C: the RT thermal envelope at 2000 MHz is then the limit).
- Thermal: KMD cap drops at 87 C (one level at once, then every 500 ms), floor at 90 C; HTTP guard ends the game after
  two readings above 87 C.

## Result
2026-10-01 12:30-12:39Z, registered triplet (adapter107 6D22D305), KMD 0.7.193.1, DPM 2000, GPU DWM:
**functional-restored** (bound stop at 367 s, tree closed, baseline restored). RT image correct: reflections and RT
lighting in the Kaer Morhen interior, shots 003-008 in scratch\m15\control\native-caps250. No fault, no TDR: KMD
node 0 158616 submitted, 0 timeouts, 0 refused, "no TDR"; GCVM fault latch 0 at 12:42:18Z (snapshot-250.txt), ring
drained (RPTR 0x600 = WPTR & 0x7FF). The 754 gap between submitted and completed is the counter's meaning, not lost
work: HwCompleted counts fence interrupts that retired at least one job (wddm.c:945, wt193), and the gap stayed 754
over 15 s with the GPU idle. ICD log of the game (pid 11604): no invariant lines, 94-101 submits/s from t=157 s,
deferred destroy peak 16 BOs / 960 KiB, longest hold 263 ms, forced 0.
**Window B (t=203 s, 40 s, walking): game 9.3/s, median 107.0 ms, p95 146.8, p99 195.3, max 220 ms, 0 > 250 ms**;
DWM followed at 9.3/s. GPU counters (our LUID): **window B 3D mean 97.8 %, min 90.8 %**; world mean 97.6 %;
dedicated max 4096 MB; machine CPU 43 % in window B, DWM 2.9 % of one CPU. DPM: 2000 MHz / 1000 mV in window B (busy
93 and 63 % at the two guard readings), Tctl peak 86.5 C, below the 87 C guard (no stop, no thermal cap step seen).
Journal (KMD 193) at HIGH+RT: 142 records/s (101 gfx-submit), 0 lost.

Reading: the same frame rate as 223 (9.1/s at the 1800 MHz thermal-soft cap, KMD 182): +11 % clock bought +2 %.
At HIGH+RT the frame is GPU-bound (3D 98 %) and not clock-bound (K46 again): the time is in the RT work itself
(traversal, BVH memory traffic, RT shader cost), so the levers are on the GPU side, not off it. Operator: one ssh
session at a time from Start Running + 50 s; the launcher log has no banner timeout.
