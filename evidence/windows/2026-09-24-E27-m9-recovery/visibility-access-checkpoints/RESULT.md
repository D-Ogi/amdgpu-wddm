# M395 - Persist existing visibility accesses before observer MMIO

Candidate0.7.123.1 adds a sequence-local TraceBootstrapTlb flag, reset by
SequenceBegin, set only for M393's PASSIVE-safe unpublished RLC stage and cleared
immediately after the stage call. It changes no shim/register sequence.

ObserveRetirementTlb keeps the existing message and RLC state reads. In the
selected bootstrap scope it persists that message BEFORE those reads, then
persists an observer-return marker AFTER them. Existing shim callbacks occur
after request write, after GC10.1 dummy request read and after ACK polling.
This can distinguish a completed original invalidation access from a hang in
the observer's own RLC MMIO. No extra register read or per-poll snapshot is added.

GpuMemCompleteGfxBootstrap also persists GFX return/MMHUB entry, MMHUB return,
publication and final RLC-state observer return. Required flushes and failure
checks are unchanged. There is no skip-invalidations workaround.

Actual-source bootstrap harness:228checks0fail, both traced/untraced and prior
four success/failure scenarios. New snapshot-count checks distinguish no trace,
early RLC failure, failed visibility and successful publication. Existing shim
MMIO and observed/unobserved replay code are unchanged. WDK build and package
verification pass:25checks0errors0warnings13notes. Initial build log is retained;
final build adds explicit flag reset in SequenceBegin.
Final SYS SHA256:5BD2B358B9A6E016EFB0B911F9511AA4DA7CA4E1E82DE22023512187ADCEC823.
Workspace package: scratch/build/bc250kmd-07123/package-umd.

Not installed or run on the GPU. Lab remains recovered Windows122 from M394.
Next first-load control must witness all selected messages, then one changed
warm trial narrows the M394 visibility interval. Timing effects of synchronous
snapshots remain a diagnostic limitation. Full M9 remains open.
