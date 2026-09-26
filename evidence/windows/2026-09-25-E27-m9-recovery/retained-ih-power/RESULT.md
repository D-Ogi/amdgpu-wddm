# M461 - Retained interrupt-ring hardware boundary

2026-09-25. Source/host only; no deployment or lab power transition.

IhHaltRetained separates synchronized ISR admission closure, DPC joining and
checked hardware disable from ring teardown/backing release. Existing Fini
uses this halt boundary then deliberately tears down. Failed sequence/disable
cannot authorize release. IhSetPowerRetained serializes through GartLock and
preserves the AMD owner, ring, writeback and GTT mapping identities.

Resume requires the retained suspended state and restored GART. It calls the
existing source-derived hardware prepare with delivery disabled first; only then
does it zero retained CPU writeback/read pointers and publish a fresh DPC copy
through the existing synchronized enable path. Pre-sleep quiet state is not
proof that firmware left hardware disabled. Failure keeps the backing and
requires successful halt before any future release. Normal stop remains distinct.

Actual-source host tests83/0. Production halt, Fini, retained core and wrapper
are extracted; kernel/synchronization and AMD register operations are mocks.
The tests cover identity retention, poisoned writeback pointers, suspend/restore,
explicit later teardown, disable refusal, required GART, partial prepare/enable
failure, retry after a successful halt, backend restoration and IRQL admission.
They do not prove actual ISR/DPC scheduling or hardware register behavior.
Mutation retaining stale WPTR:83 checks,4 failures. Mutation releasing backing
on suspend:85 checks,3 failures. Raw logs and mutated source retained.
Full WDK build/sign passes. Undeployed development SYS SHA256:
C50A55839FEF8AFE75F29AF5DF49943159B33D726B7F58C2A85F5D8ACE9258FA.
Build scratch/build/m461-retained-ih-final still carries version145; installed
M458 SYS ED7B2E07 remains unchanged. Intermediate builds are not this result.

PROVENANCE: existing AMD IH shim and local navi10_ih.c reference at Linux v6.18,
7d0a66e4bb9081d75c82ec4957c50034cb0ea449; repository GPL-2.0, reference read only.
Upstream navi10_ih_suspend/resume call hw_fini/hw_init without software teardown.
No upstream code or register constants were copied by this change. Hardware
operations reuse existing bc250_ih_hw_prepare/enable/fini and synchronization.

Not wired to SetPowerState. Coordinator must first quiesce OS/private producers,
then stop IH before losing translation, restore the same GART/pinned backing
before IH resume, and coordinate firmware, GFX/SDMA, display and health state.
IhIsActive deliberately still sees retained SetUp and blocks destructive GART
operations. A new retained GART path is needed; bypassing that protection in the
old destructive initializer is not the intended integration. Full M9 remains open.
