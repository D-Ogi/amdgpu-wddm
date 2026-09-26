# M397 - Candidate124 removes in-flight diagnostic MMIO

PROVENANCE: Linux v6.18 commit7d0a66e4bb9081d75c82ec4957c50034cb0ea449,
gmc_v10_0.c, AMD MIT. Source snapshot retained.

ObserveRetirementTlb now logs only supplied values. It no longer reads RLC_CNTL
or GRBM_STATUS2 between invalidate accesses. Both bootstrap and retirement use
this callback. Existing post-flush RLC observations remain. Required request,
dummy read, ACK polling, MMHUB flush and publication are unchanged.

Actual-source bootstrap harness models three callback phases:230checks0fail.
Restoring the old diagnostic call compiles and yields248checks18failures.
This establishes regression sensitivity for in-flush diagnostic reads, not
hardware recovery. WDK build passes; package25checks0errors0warnings13notes.
SYS13858D3FF1018C2F1F42636210B525F960DC1FF9C8A1FF66311B01CD8B81F351,
version0.7.124.1. No deployment or runtime acceptance yet.

Synchronous snapshots still perturb timing. M396 does not prove which observer
instruction stalled. Next run a first-load positive control, preserve evidence,
then one warm trial. Lab remains recovered123display-only; no lab actions here.
