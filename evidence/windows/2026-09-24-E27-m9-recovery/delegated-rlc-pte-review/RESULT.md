# M442 - Delegated RLC validation and local PTE review

2026-09-24. Main session reviewed both sub-agent results against the current
source and local references. No lab action was performed for these changes.
The concurrently deployed KMD136 was built before the RLC fix and excludes it.

## BD-026

The parser accepts only its implemented RLC v2.0 layout and a complete header.
The earlier description of reading v2.1 extension offsets was inaccurate;
accepting unsupported layouts while loading only RLC_G was the actual gap.
Real cyan_skillfish2 firmware remains accepted. Ten image loads and28 E03
mailbox accesses match; seven synthetic controls pass, and removing the gate
fails six rejection controls. User/WDK source compilation passes.

[Agent report](bd026/RESULT.md), [positive log](bd026/psp-replay.log),
[mutation log](bd026/mutation.log), source snapshots under bd026/source.
No proprietary firmware bytes are included. Only firmware metadata/hashes.
FIXED here means source plus host controls, not deployed or hardware verified.

## BD-021

No production change. The universal claim that Linux never sets SNOOPED for
VRAM is contradicted by its cached-VRAM branch. That branch is not proof of
BC-250 coherence: upstream VRAM caching depends on CPU-connected XGMI.
Current KMD requests cached backing only for GTT. Incoming local coherent PTEs
have not been observed, and MS does not authorize ignoring the requested bit.
Existing actual DXGK_PTE tests and WDK compilation pass with0 failures.

[Review, precise citations and next measurement](bd021/REVIEW.md),
[host log](bd021/pte-control.log). NEEDS-LAB: observe local/system requested
coherence flags in a normal paging trial before deciding whether code should change.
No extra restart or forced coherent local mapping is needed for that observation.

The source-only RLC snapshot is uncommitted. This evidence identifies it by
SHA256; no claim is made that current metadata version136 contains this fix.
