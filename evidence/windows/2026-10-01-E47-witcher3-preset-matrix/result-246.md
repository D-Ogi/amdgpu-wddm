# Trial 246: M15.7/M15.8 The Witcher 3 LOW at native 1080p on candidate shell adapter107 (release gate), 7-min session

`SESSION_ADAPTER=adapter107 run-m157.sh 246 low`: candidate shell adapter107 6D22D305 (bc250-win 2d410d5d on
m15/adapter107-release-gate = adapter106 + F1 two-phase retirement in engine-ddi, F2 shell-side progress gate for
released imports over the ICD's published progress, F3 bounded quarantine of released imports; all three on by
default), engine 15E3E24E and ICD 2A13235D as registered (only the shell is swapped and restored), experiments
present-noprimary,present-cached,raytracing-tier,recording-bind,retire-handoff (none of the three -off switches), ICD
cfg cleared, KMD as deployed (STATE.md) with DPM on, ceiling 2000 MHz, GPU DWM, AAMode 0/1, FG off, DRS off, native
1080p, LimitFPS 60. Interactive: load the save, walk in the world (route of 245), screenshots at 0.33 scale.

- Question: does the release gate remove 245's class (a job queued before a runtime-backed heap import was released
  reading its first page after the unmap retired; REPORT-245.md)? Second: the game-side price of KMD 192's object
  index (K88, K90) against 245's window B (48.8/s, median 19.78 ms on KMD 191).
- Expected: functional-restored; in-world frames; no gfxhub fault, no 0x116; window B at or above 245's 48.8/s;
  progress_gpu == submits, no invariant lines; no E_OUTOFMEMORY or VRAM growth from the quarantine.
- Refutation: a gfxhub fault or 0x116 again (same class: the gate let a release through too early, or a different
  class), a hang or E_OUTOFMEMORY (quarantine holds too much), a wrong image, DWM falling back to the CPU route.
- The shell's new trace lines go to the debugger only (trace mode 2); this session runs without cdb, so a failure is
  read from the KMD side (fault VA, journal) and dump; an A/B with the -off switches follows only if needed.
- Thermal: guard over the emergency channel ends the game after two readings above 87 C.

## Result
2026-10-01 11:59-12:08Z, KMD 0.7.193.1 (deployed 11:51Z, DPM on, ceiling 2000), GPU DWM, adapter107 6D22D305 as
candidate: **functional-restored** (bounded stop at 361 s, tree closed, baseline and settings restored, no survivors,
candidate not retained). In the world from t~110 s (Kaer Morhen interior, correct image, shots 002-007 in
scratch\m15\control\native-caps246). **No gfxhub fault, no 0x116, no TDR**: KMD summary node 0 71746 submitted /
71745 completed, 0 timeouts, 0 refused, "no TDR (ResetEngine ... never called)"; GCVM_L2_PROTECTION_FAULT_STATUS and
ADDR read 0 after the session (12:09:53Z, the latch is never written by 193). Window B (t=195 s, 40 s, walking
indoors): **game 45.3/s, median 21.55 ms, p95 28.73, p99 35.08, max 53.6 ms, 1 frame > 50 ms** (245: 48.8/s median
19.78, max 148; 225 on KMD 184: 45.4/s). ICD v3 periodic: submits = signal_calls (52011 at t=305 s), 257 submits/s,
deferred held 0 at every period after #2 (peak 16 / 768 KiB, longest hold 32 ms), no invariant lines. GPU counters
(gpuctr-246, our LUID): world 3D mean 69.9 %, max 98.9 %; window B 65.4 %; dedicated max 2045 MB, shared 537 MB; DWM
13 % of one CPU; machine CPU 58-70 %. DPM: one raise to 1800 MHz at busy 76.7 % (12:05:06Z), otherwise 1000 MHz at
busy 30-67 %; Tctl max 78.0 C. KMD 193 journal under the game (LAB-RESULT.md of work\193): 427 records/s, 273/s
gfx-submit, 0 lost at 250 ms polls.

Reading: the release gate held for one session with the 245 route and no new defect showed; one clean session
does not prove 245's class gone (it struck once), but the gate is correct by construction and costs nothing visible.
Window B is 7 % below 245 and equal to 225: within session-to-session spread, not a regression signal. New fact:
at LOW the GPU is now ~70 % busy at 1000 MHz (K1 measured 22.7 % at 23 fps on adapter097): the off-GPU work of the
last two days roughly doubled the frame rate and tripled the GPU share. Operator note: a first launcher started with
a shell `&` did not die with its tool shell; two launchers ran, the kit refused the second Stage (attempt exists),
the duplicate tree was killed at 12:02Z; its gpu-counters session wrote into the same file until then.
