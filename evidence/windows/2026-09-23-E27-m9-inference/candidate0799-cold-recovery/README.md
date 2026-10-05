# Candidate 0.7.99.1 recovery, unit A, 2026-09-23

Owner reported persistent backlit black display after reboots, then desktop recovery after disconnecting AC power. This observation does not establish the hardware root cause.

Read-only Target.run capture is recovery.log; recovery.ps1 is the collector. No new GPU start, device restart or registry mutation was issued during collection. No private adapter instance identifier is included.

SSH returned; Windows boot timestamp is 2026-09-23T15:39:09. Persisted EnableFullWddm=0, LastStage=90, StageHistory="10 20 30 90", UnconfirmedStarts=2. Display device reports CM_PROB_FAILED_POST_START. Desktop recovery is not full-WDDM acceptance.

Last recovered snapshot of the failed full start, ring-20260923-131742-889.log, ends at "startup: entering GFX stage 6" after stage 5 returned success. Stage 6 is CP initialization in driver/kmd/gfx.c. No completion record was recovered. This localizes the last persisted boundary, not the failing instruction. Listed crash dumps predate this attempt. Old quiet ICD remains installed; new cache-intent ICD was not tested.
