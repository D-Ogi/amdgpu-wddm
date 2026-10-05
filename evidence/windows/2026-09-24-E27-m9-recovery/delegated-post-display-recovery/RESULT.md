# M446: restore the framebuffer used for CPU display output

2026-09-24. Source and host controls only; lab remains candidate136.

BD-003 restores and verifies the already-mapped POST surface before enabling
bugcheck CPU writes. BD-011 restores after joining display DPCs and before VidMm
retirement, checks pending/actual scanout/pitch, and propagates release failure
with cleared output metadata. Ordinary stop keeps its existing completion policy.

554 restore, 70 lifecycle and 6726 observation checks pass. Four source mutations
fail 10, 6, 2 and 6 checks. Full WDK build passes. Main reviewed actual stop order,
quiet bounded restore, CPU-write gating and local Microsoft callback contracts.
[Agent report, exact source snapshot and logs](review/REPORT.md).

These source fixes do not implement full GPU cancellation/reset at bugcheck and
do not guarantee recovery when hardware refuses the restore. A failed release
still allows ordinary teardown. No forced bugcheck, hardware handover or deployment
was performed. Candidate138 contains these changes; it is not installed.
Copied test artifacts contain project source and logs, no proprietary firmware.
