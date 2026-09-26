# M396 - Warm failure follows the invalidate request, inside the observer interval

Unit A, 2026-09-24. Installed 0.7.123.1, SYS
5BD2B358B9A6E016EFB0B911F9511AA4DA7CA4E1E82DE22023512187ADCEC823.
First start passes all request/read/ACK observer, MMHUB and publication boundaries.
The 64 KiB residency probe passes three eviction/restoration cycles with GPU
readback. Stop completes, including quiescence and retirement, in the same boot.
One warm start loses SSH after PnP enable at 10:59:37 local. No unchanged retry.

Latest persistent snapshot ring-20260924-085938-124.log ends at 0.304 s with
GFXHUB after-invalidate-request, value 0x00F80001, sequence fault 0.
It includes after-rlc-resume at 0.300 s. The request write returned and its
observer persisted the sample. No observer-return checkpoint follows.
The remaining interval contains return from GuardLogKeep, GfxTraceRlcState
(two diagnostic reads: RLC_CNTL then GRBM_STATUS2), and the next log/persist.
This does not identify an exact stalled instruction or prove the observer
caused the hang. It does rule out failure before the request write returned.

One recorded AC cycle with both configured OS SSH endpoint sets unavailable
restores Windows boot 11:01:13 local, version 123, display-only stage 61,
CLI success, FullWddm 0, UnconfirmedStarts 2. Other execution/diagnostic gates
remain enabled. User confirms desktop. No reset after recovery inspection.
Interrupted local collection had completed both output files. Interface/PCI
instance identities are redacted in copied logs; power receipt has no secrets.

Next remove unrelated diagnostic MMIO from the in-flight invalidate callback,
retaining request, required dummy read, ACK poll, MMHUB and post-flush tracing.
Test first-load content before a single changed warm trial. Full M9 remains open.
