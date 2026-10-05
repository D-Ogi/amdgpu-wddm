# CPU access lifetime for fences and paging, local0764

2026-09-23. Base HEAD bed764d plus captured worktree changes. No lab access or deployment.
Device-level GfxAccessLock serializes admission and reference counting at up to
DISPATCH_LEVEL. The first reference clears a notification event; the last release
signals it. Close prevents new references then waits for active CPU users. Object
lookup occurs within admission, before a lifecycle writer can detach/free the object.
The device itself remains owned by the active DDI/DPC/StopDevice invocation.

References wrap GFX and paging ready/busy/gate/failure/fence accessors and the complete
GfxSubmitPaging call. The inner paging ready query avoids recursive admission, so a
writer closing admission during an already admitted submission cannot invalidate
its internal ready check. No reference holder waits for GPU progress or takes
GartLock. The spin lock is not held while parsing/copying packets or reading fences.
Fini closes before hardware halt, TearDown also closes for PLAN, and Stop closes
before detaching Gfx. Start and successful RUN through interrupt stage reopen admission.
GartLock continues to serialize GFX submission and lifecycle writers; the0763 push
lock continues to protect PASSIVE_LEVEL builders.

The extracted actual gate and both fence callbacks pass a Windows host concurrency
test using SRW locks/events to model kernel primitives. Two blocked readers prevent
close from completing for300ms; late readers are refused without touching a fence.
After release, both original readers finish and close can reclaim. Event reuse,
repeated close, reopening, NULL object and zero sequence are covered. Removing the
wait in generated code fails the test. This is a mutation control, not an execution
of the entire original driver. Real kernel IRQL/interruption behavior is untested.
The full paging-submit worker is compile-checked, not dynamically run by this harness.
Existing builder-lifetime and private-buffer regressions pass.

Full KMD build/sign passes, package scratch/build/bc250kmd-0764/package-umd,0.7.64.1.
SYS SHA256 E0A74C95B2B9815EA431B7A4E3C8E0AA69C923AEAE697266075E2ABC015C52D4.
Not deployed. CPU reference drain does NOT prove GPU DMA has finished or engines
have reset. Outstanding hardware backing pages still require halt/recovery policy.
WddmStop currently precedes IhStop: WDDM object/ISR/DPC lifetime needs a separate
review, including the placement of Device->Wddm detach relative to flush. System
mapping, refusal/error semantics, runtime profiling and1GiB paging remain open.
No redactions.
