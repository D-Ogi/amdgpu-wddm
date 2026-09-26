# Local0769 ready-path FLUSH_TLB integration

No hardware access, deployment or reset. Candidate package0.7.69.1:
scratch/build/bc250kmd-0769/package-umd.
SYS SHA2563295E344093A2D8E98D12D59BBA1D304A807730665A5BC946B54249CF93F527D.

BuildPagingBuffer now recognizes FLUSH_TLB and uses GfxPagingBuildFlush with
BC250_WDDM_VMID1. It writes the resulting15DWORDs into the existing OS-owned
private record and DMA buffer, advances both pointers/sizes, and counts flushes
separately from moved bytes. Command budget includes earlier operations, live
ring reservation, outer completion fence and alignment. It does not interpret
the flush union as a TransferVirtual/FillVirtual payload.

The full GFXHUB VMID1 address space is invalidated rather than only the requested
root/range. All current WDDM app submissions use VMID1; each root assignment also
flushes. This conservative over-invalidation avoids capturing a process binding
that can change while the paging buffer is queued. VMID0 GART is unaffected by
this operation. Changing the single-VMID architecture requires revisiting this.
Existing submit copies packets into SDMA before its outer completion fence.
Actual cross-engine/OS ordering and ACK behavior require hardware verification;
this is not proof of page-table publication correctness.

Extraction harness links actual GfxPagingBuildFlush plus real packet/budget
helpers; locks are balanced-count mocks, not concurrent kernel verification.
2560checks pass, including prior routing controls, ready VMID1 vsGART, all4-byte
DMA offsets0..4096, insufficient space without partial publication, absent/not-ready
engine and invalid arguments. Full private-record regression passes. Full WDK
build/sign passes; post-build edits only comments/indentation. The entire WDDM
DDI dispatcher/publication path has compile/source review, not dynamic coverage.

IMPORTANT OPEN: before GPU readiness, helper returns DEVICE_NOT_READY and the
outer builder still folds unsupported/error outcomes to empty SUCCESS. That
inherited behavior is NOT fixed by this revision. Other unsupported operations,
header-failure status policy and immediate CPU GPU_PHYSICAL PTE writes remain
under audit. Do not deploy as a completed paging implementation.
