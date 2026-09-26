# Driver review integration (2026-09-24)

The owner's workspace `DEFECTS.md` is the authoritative backlog for the review,
with stable BD identifiers and one status table. It is outside the public repo.
Update existing matching entries there; do not add issues found and immediately
fixed during implementation. Older comments remain unchanged. This document is
an implementation/evidence pointer, not a second defect-status index.

M435 addresses BD-006 (GC/NBIO/shim geometry agreement) and BD-010 (required POST
reservation) in source.72 actual-source checks for8/12/16GiB, two detected mutations
and full WDK build pass; no new deployment or12GiB hardware validation.
[Evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/vram-geometry-consistency/RESULT.md).

M431/M432 already cover BD-001 and BD-015 source corrections. The stated BD-012
full-WDDM diagnostic escape route is blocked by the existing dispatcher even
with VidPnFlipEnabled off. Native SMU migration spans BD-004/005/024/027; M441 later deploys it. M436 implements BD-002/017 across primary allocation, hardware pitch,
scanout bounds/mapping and CPU row copy. Host controls and full WDK build pass;
unit A validation remains open. [Evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/scanout-pitch-geometry/RESULT.md).
Review detail and comments belong in the workspace backlog.

This work retains the complete M9 DMA, startup, lifetime, recovery and performance
acceptance scope. See [current acceptance](m9-acceptance-status.md).

M441 deploys native SMU ownership with coordinated client retirement and passes
content/display controls. [Evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07136-native-smu/RESULT.md).
Delegated M442 covers RLC header validation and the local-PTE review;
M443 implements hardware raster/completion observation and partial pending-vblank
reporting. These source changes are excluded from deployed136.
[Review](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-rlc-pte-review/RESULT.md),
[display work and limits](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-display-observation/RESULT.md).

The next delegated batch is archived as M445 (BD-025 runtime firmware and BD-023
SMU operation policy), M446 (BD-003/011 POST display recovery), and M447
(BD-020 scoped overlap review, BD-021 observation counters, BD-022 cache review).
These are source/host results; the deployment baseline remains M441.
[Metadata](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-runtime-firmware/RESULT.md),
[display recovery](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-post-display-recovery/RESULT.md),
[memory review](../../evidence/windows/2026-09-24-E27-m9-recovery/delegated-memory-review/RESULT.md).
