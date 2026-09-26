# Refused submission and reset reporting, local0767

2026-09-23. No lab access/deployment. Local Microsoft documentation revision
7515063cea4c9e98db6a92986c5b4ddb0463fd16: DXGK_INTERRUPT_DMA_FAULTED is reserved for
system use (enriched d3dkmddi.md:45981). ResetFromTimeout must stop all GPU memory
access before successful return; failure causes Windows bugcheck/restart. The
existing driver closed software gates, dropped pending fences and returned success
without halting hardware, so that success was unsupported.

WddmFailSubmission now closes a node after valid work cannot be dispatched, sets a
sticky refusal/fault state and discards deferred software completions. UMD fallback,
nonempty physical fallback and nonempty virtual fallback use it. Empty work and
successfully executed CPU Present retain legitimate software completion. Malformed
virtual Present now returns the allowed INVALID_PARAMETER. Rejected-fence retirement
and preemption cannot advance across refused work. Actual predecessor hardware fences
still complete. QueryEngineStatus reflects fault state instead of always responsive.

ResetFromTimeout now preserves pending fences and returns STATUS_UNSUCCESSFUL after
closing submit gates. This is an explicit FAILED recovery, not an implementation of
GPU reset. Windows may bugcheck/restart on this path. The candidate is local only.
A verified hardware halt/reset and successful recovery remain required for the goal.
No reserved DMA_FAULTED notification or fabricated completion is emitted.

Extracted helper/real fence/watchdog/reset code passes both-node tests: refusal emits
no completion, later software fences cannot retire it, a late real predecessor still
completes, and unsuccessful reset leaves both pending fences unchanged. Existing
watchdog controls and expanded report-DPC tests pass, including refusal blocking
preemption and rejected-fence bookkeeping. Full dispatch/DDI and QueryEngineStatus
behavior is compile/source-reviewed, not covered by this host harness.

Full KMD build/sign passes, scratch/build/bc250kmd-0767/package-umd,0.7.67.1.
SYS SHA25646BFD0E8A3005CD6661C6724C717AFDED17A60491695D6CE361A64B689521D5B.
BuildPagingBuffer still has unsupported/no-root/not-ready paths that return no work
with success and needs further correction within its restricted return contract.
Legacy physical ADL operations and OS page lifetime review remain open. No redactions.
