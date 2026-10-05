# CopyPageTableEntries publication and range-list integration, 0777

2026-09-23, host only. Source base bed764d plus ongoing uncommitted M9 changes.
No lab access, reboot, deployment, GPU execution or fresh health measurement.

Hypothesis: accepted copy ranges must update logical construction state in order;
capacity refusal must leave the unaccepted suffix unchanged and resume without
omitting or duplicating ranges. The test chains 40 dependent one-entry copies.

Implementation: VidMmCommitPagingCopy serializes metadata under CpuUpdateLock,
validates resolved physical entry identities, preserves unknown-source semantics
and skips unregistered application destinations. WddmPublishPagingRecordEx checks
DMA/private capacity and header before commit, then copies/advances accepted DMA.
WddmBuildPagingCopies uses range-index MultipassOffset, emits and publishes each
range before resolving the next, and accounts for cumulative command offsets.
The OS input DmaBufferWriteOffset is restored on every loop exit. The DDI dispatch
now routes COPY_PAGE_TABLE_ENTRIES here using the system context's recorded root.

Verification: run_paging.ps1 -KmdRouting yields8654 checks,0 failures. Tests exercise
actual range-list/publication/commit code and packet construction. Logical address
translation in these new list cases is mocked; existing real-walker tests remain.
A192-byte first buffer accepts one136-byte range, then returns insufficient.
Two further buffers finish40 dependent ranges under the live-ring budget. The
private-record parser validates contiguous command coverage. Rejected headers,
post-stop commit failures, bad identity alignment/bounds, unknown metadata, empty
lists, missing arrays and invalid progress are checked. No OS callback/concurrency
or real GPU cache/barrier claim follows from this field-level host model.

-OmitLogicalCommit deliberately skips both update and copy publication. It exits1
with588 failures of8654 checks; the actual code exits0. Initial mutation compile
failed on unused parameters; the final mutation explicitly marks them unused so
its failures are behavioral. Full WDK build/sign passes with candidate0777 SYS:
1CF718C53B1E001627AFBA4101C8D5BFF18CD8BE002C3CEB968F6AD09E35E4C3
Package: scratch/build/bc250kmd-0777/package-umd (not deployed).

Remaining: internal failure codes on this new path are propagated, never silently
converted to success, but arbitrary failure codes are still outside the restricted
BuildPagingBuffer return contract. Error policy, cache aliases, OS serialization,
GPU retirement/reset/reentry, physical ADL and real1GiB/performance acceptance
remain open. Full DDI dispatcher is source/WDK-compiled, not host-executed.
