# Paging builder lifetime, local0763

2026-09-23. No lab access or deployment. Base HEAD bed764d with captured worktree
changes. Kernel primitives declared in local WDK10.0.26100 wdm.h (ExInitializePushLock,
ExAcquirePushLockShared/Exclusive and releases); device lock initialized at AddDevice.

Before: GfxPagingBuild loaded Device->Gfx before incrementing a counter. Teardown
cleared paging pointers before waiting and freed engine state even if the counter
remained positive after200ms. A late entrant could touch an already freed object.

After: the device owns GfxPagingLock. Builder takes it shared BEFORE loading Gfx,
keeps it for every engine-state access and releases through one cleanup path.
GfxEscape (including PLAN/RUN/FINI) and GfxStop take it exclusively BEFORE GartLock.
Stop detaches and frees Gfx while exclusive ownership remains held. The ad hoc
counter and bounded-free path are removed. Critical regions disable normal kernel
APCs without raising IRQL; page-table translation remains at PASSIVE_LEVEL. Builders
never take GartLock, so they can finish while a writer waits without that mutex held.
Multiple builders remain concurrent. Start publishes before DDI exposure as before.

Host test extracts actual builder and stop entry/exit. Windows SRW locks model the
kernel push locks; page resolution/packet production and hardware teardown are stubbed.
Two readers enter concurrently, then stop must not detach/free during a300ms hold.
Readers exit and stop completes; subsequent builder sees NULL. Not-ready/no-root
paths release the lock. Disabling the builder lock in the generated harness fails the
lifetime assertion (mutation control), enabled code passes. This is not a reproduction
of every instruction of the old code and not proof of kernel scheduling behavior.
Paging stream/private-record regressions and full kernel build/sign also pass.

Package scratch/build/bc250kmd-0763/package-umd,0.7.63.1, SYS SHA256
B6E4B84F63510B73D363ACFC77BA41A58C83698B1169DAE617480C97C5BBCD37.
Not deployed. This protects PASSIVE_LEVEL builders only. DISPATCH_LEVEL submission,
fence readers, timer/IH rundown and outstanding hardware DMA still require review;
a push lock cannot protect those paths. No GPU recovery claim. No redactions.
