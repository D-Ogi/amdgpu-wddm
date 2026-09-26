M306 - Captured unequal-offset emission and owned DDI route
Base revision bed764da5192be7132d646be0e6331c1edeb30fd plus working tree modifications; snapshots included.

GfxPagingEmitCapturedLinear emits from retained physical slices under engine lifetime lock with scratch/VRAM origin validation. Same-page overlapping system copies use staging inside mapped transaction; local overlaps use existing staged SDMA bytes helper. Slice and proposed progress become visible only after successful construction.

WddmBuildCapturedVirtualTransfer no longer sends unequal offsets to the legacy per-slice resolver. Capture proves monotone ordering, retains addresses, and advances traversal progress only after exact DMA/private publication with applicable local-table logical commit. Completion frees the context-owned capture. Other dependency shapes still return internal NOT_SUPPORTED: general interval graphs and OS-facing restricted-status policy remain open. Matching-offset single-page transfers retain legacy helper; page-crossing matching-offset ranges now use captured graph.

M303 actual builder regression updated to invoke the wrapper selected by the real dispatcher. System disjoint and physically aliased8192byte destination+1 transfers use256DWORD ring, require multiple callbacks, disable translation after first callback, independently replay83/100DWORD packets and compare original-byte snapshot. Exact private coverage, total moved bytes, complete token and capture release verified. Focused54checks PASS; routing31080PASS. Existing direct legacy helper remains a known-limited internal implementation; it is no longer selected for this unequal-offset production route. These tests do not newly cover local/mixed unequal packets or self-table linear commits.

WDK26100build PASS, SYS952F05B7142D18A4E526622F160F9B8477380EF20E5AD42106D45F567D7FF556, not deployed. No live OS/GPU acceptance or throughput claim. No lab access/mutation this turn; sshd response still pending.
