# M447: scoped GOP overlap review and PTE/cache observations

2026-09-24. Static review and host controls; no new hardware capture or deployment.

BD-020's alleged 128 KiB GOP overlap is rejected. The underlying static formula
uses 0x20 KiB, or 32 KiB; the PSP pages are outside that extent. Offline parsing
of existing own-unit debugfs and VFCT VBIOS artifacts finds zero reservation
fields. Linux allocates its fallback scratch in system memory and reserves no
firmware VRAM from this zero table. Main checked the formula, Linux routines and
current PSP layout. This does not assert absence of every firmware consumer.
[Review and bounded parser results](bd020/REVIEW.md). Proprietary images and
decompiled code remain outside this repository.

BD-021 adds aggregate PTE counters by level, segment and requested coherence,
including encoded snoop mismatches. Counts describe encoding attempts, not unique
pages, residency or retirement. No PTE/cache-policy change. Actual routing passes
908035 checks; frozen137 with corrected positive POST fixtures passes 907964.
The focused 71 checks pass and the wrong-domain mutation fails 19. Real WDK
translation-unit compilation passes. [Fixture comparison and source](bd021/OBSERVATION.md).

BD-022 remains open: the historical POST mapping conflict is corrected (2072
host checks pass; an always-NC regression fails 2049), but independent Windows
VidMm mapping attributes are not established. The old failed physical cache
query is not a usable cache-type observation. [Detailed review](bd022/REVIEW.md).

The observation source was frozen into candidate138. Both BD-021 and BD-022
require lab evidence or an architecture that eliminates the unresolved aliases.
No speculative blanket cache change was made. Reports and logs copied unaltered.
