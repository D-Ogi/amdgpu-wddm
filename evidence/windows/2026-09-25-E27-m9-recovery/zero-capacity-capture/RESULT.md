# Zero-capacity capture admission fix

2026-09-25. Source/host/unsigned WDK build only. No driver version change, signing,
deployment or lab action. General capture-arena exhaustion remains open.

## Change

driver/kmd/wddm.c, WddmBuildCapturedVirtualTransfer: before acquiring a NEW capture,
return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER when DMA output size is zero or the
private buffer has no queued-direct output capacity (including a null pointer).
This is an actual output-capacity decision, not a translation of pool allocation
failure. No plan, token, arena span, output byte or progress is acquired/changed.

Existing active captures still follow their resume path. The terminal COMPLETE
token still succeeds as a no-op. The no-cross-page fast path remains unchanged.
Any pool/resource failure after nonzero-capacity admission retains its existing
honest result; no arbitrary resource cap or empty-success fallback was added.

Only this function changed in production. Tests in
experiments/E27-m9-inference/paging-route-test-suffix.c add four zero-capacity
controls, then retry through the existing complete alias-copy oracle. Old tests
that intentionally needed a pending capture now give insufficient NONZERO room;
zero room retaining a plan was implementation behavior, not the required contract.

## Verification

- Existing full actual-source KMD routing /W4 /WX suite:
  908083 checks, 0 failures (host.log).
  Covers exact byte-oracle alias copies, split batches, changed/poisoned VA lookup
  after capture, self-table copies, unequal offsets, active resume and interleaving.
  Four fresh-admission cases exercise zero DMA size, zero private size,
  private-header-only capacity and null private pointer, with pool failure enabled.
  All check untouched pointers, sizes, DMA/private bytes and capture bookkeeping.
- Negative control removes ONLY the new capacity guard from generated actual
  source. The retained graph suite returns exit1 with 48 failures
  (320871 checks; negative.log), beginning with zero-capacity ownership and
  output-preservation checks. Follow-on assertions also detect the unwanted plans.
- M465 actual queue/report preemption suite: 1074 checks, 0 failures
  (preemption.log), including truthful completion, scheduler same-fence replay,
  racing admission and faulted-tail retention.

Source is frozen under source/ with 481-file SHA256SUMS. Original build.ps1 is
preserved; build-unsigned.ps1 uses its compile/link/stack-check recipe and stops
before INF/catalog/certificate/signing. No signing process was invoked.

Full WDK /W4 /WX build succeeded from that snapshot:
- Output: P:/bc-250/scratch/build/paging-zero-capacity-dev/package/bc250kmd.sys
- SYS SHA256: 88988C7F3D45D15518B34B4900AA9ECC06179ECC9EA49D3CF1D5C2E3FA0FBDC6
- Authenticode status: NotSigned.
- Frozen wddm.c SHA256:
  EA0ED76FDE6C44481E473E9198669820186FCC3A7000D1CA7D2B80929B608E5F
- Stack check passed: 568 unwind functions, largest fixed frame3992 bytes in
  GfxPagingBuildUpdate; existing warnings recorded in wdk.log.

Reproduce ordinary routing build with driver/shim/test/run_paging.ps1 -KmdRouting.
run-negative.ps1 + omit-guard.py reproduce the deliberately failing host mutation.
preemption/build.cmd reproduces the focused scheduler/queue controls.

## Acceptance limit

This eliminates needless fresh-capture retention for zero-capacity requests.
It does not establish a bound on multiple unfinished nonempty captures, resolve
arbitrary aliases, prove a real Windows pressure sequence, or close M9.
No runtime performance or lab acceptance claim follows from this unsigned build.
