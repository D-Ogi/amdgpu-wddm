# Trial 249: M15.7 The Witcher 3 HIGH at native 1080p on the registered triplet (adapter107), 7-min session

`run-m157.sh 249 high`: the registered triplet itself since 247 (shell adapter107 6D22D305, engine 15E3E24E, ICD
2A13235D; BC250_TRIAL_ACCEPTED=1, nothing swapped), experiments present-noprimary,present-cached,raytracing-tier,
recording-bind,retire-handoff, ICD cfg cleared, KMD 0.7.193.1 with DPM on, ceiling 2000 MHz, GPU DWM, preset HIGH
(RT off by the preset), AAMode 0/1, FG off, DRS off, native 1080p, LimitFPS 60. Interactive: load the save, walk the
Kaer Morhen interior route of 245/246, screenshots at 0.33 scale.

- Question: the M15.7 HIGH row on the current stack - frame rate and GPU share against 167 (adapter097, HIGH, 18.5
  fps, GPU busy 29.9 %), and whether the DPM governor raises the clock once the GPU carries more of the frame.
- Expected: functional-restored; in-world frames; no gfxhub fault, no TDR; window B well above 18.5 fps (LOW went
  23 -> 45 over the same period); GPU 3D share above LOW's 70 %; DPM raises above 1000 MHz for a larger share of
  samples than at LOW.
- Refutation: a fault, hang or 0x116; a wrong image; window B at or below 167's 18.5 fps; the governor pinned at
  1000 MHz while the 3D engine is near 100 % (a governor defect to file).
- Thermal: HTTP guard ends the game after two readings above 87 C (234 peaked at 86.0 C at HIGH-RT and 1000 MHz).

## Result
2026-10-01 12:17-12:27Z, registered triplet (adapter107 6D22D305 since 247), KMD 0.7.193.1, DPM 2000, GPU DWM:
**functional-restored** (bound stop at 361 s, tree closed, baseline restored). In the world from t~185 s (HIGH loads
slower; Kaer Morhen interior, correct image with HIGH shadows and lighting, shots 003-008 in
scratch\m15\control\native-caps249). No fault, no TDR: KMD node 0 126451 submitted / 126449 completed, 0 timeouts,
"no TDR"; GCVM fault latch 0 after the session; ICD log without invariant lines, 161 submits/s. **Window B (t=169 s, 40
s, walking): game 31.3/s, median 31.10 ms, p95 41.05, p99 60.84, max 133 ms, 28 frames > 50 ms, 2 > 100 ms** (167 on
adapter097: 18.5 fps, so +69 %). GPU counters (our LUID): **window B 3D mean 96.0 %, max 99.4 %**; world mean 71.2 %;
dedicated max 2720 MB; machine CPU 62 % in window B, DWM 9 % of one CPU. DPM: 2000 MHz from 12:21:34Z in the world
(busy 64-77 %), one 1900 MHz step by the thermal cap; Tctl peak 86.0 C (12:24:14Z), below the 87 C guard. Journal
(KMD 193) at HIGH: 207 records/s (190 gfx-submit), 0 lost; the follow loop managed 125 reads in 30 s (272 at LOW).

Reading: at HIGH the frame is now GPU-bound (3D engine 96 % at 2000 MHz in window B): for this preset the off-GPU goal
is reached and the remaining levers are GPU-side (shader cost, clock/thermal headroom, async compute). Operator
note: the first control call at Start Running + 26 s timed out in the ssh banner exchange (launch burst); after a 50 s
pause every call worked. A second timeout came when a journal-follow session and a control call opened at once:
one ssh session at a time.
