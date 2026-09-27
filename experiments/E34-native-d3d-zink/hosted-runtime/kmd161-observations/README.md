# KMD161 retained Present observation candidate

Hypothesis: the first CDD Blt descriptor pairs and the interop capabilities
returned to Windows can be recovered after the normal log ring rolls over.
M644 lacked this evidence; a registry latch is not a returned-caps witness.

Build procedure: derive an isolated worktree from exact160/a1f3bf9, apply only
b9a0276's wddm.c change, bump INF to0.7.161.0 and escape to0x000700A1.
Audit the three changed files, commit that source, then run driver/kmd/build.ps1
with local WDK packages, the retained157 UMD stub and QualityWorkspace enabled.
Retain source manifest, 13 quick gates, full compile/link, stack and signing
receipts. Compare both signed SYS copies. This stage does not change the lab.

Planned runtime procedure: retain exact160 rollback, preflight health/STOP and
clocks, install the diagnostic candidate in-session with streamed logs. First
check CPU controls and retained observations with interop/GPU Present both0.
Only then repeat the bounded interop1/GPU Present0 capture, at most180 seconds,
with independent rollback. The runtime scripts and exact hashes must be
reviewable before launch; this plan alone is not a completed runtime test.

Expected: on-demand summaries contain the published first16 Blt records,
including explicit snapshot refusal or both validated descriptors. Returned
interop counters distinguish actual caps replies from the configured gate.
After adapter restart, retained records belong only to the new start.
Unexpected: missing/inconsistent observations or a regression requires retaining
receipts and rollback, not enabling BGP1 based on a quiet desktop.

No new residency, allocation admission or CPU fallback is introduced. A valid
pair snapshot still does not prove submitting-device residency or retirement.
Full G0 requires pixel correctness, DWM GPU ownership and no CPU frame copies.

Result: pending build/runtime evidence.
