# Paging DMA-boundary preemption source fix

2026-09-25. Source/host scope only; no lab, driver version or deployment change.

## Contract and implementation

Microsoft local conceptual snapshot staging 110f60eaf2ac5836e644d320c1e92c1011f2af5e,
ref/windows-driver-docs/windows-driver-docs-pr/display/gpu-preemption.md:63:
VidSch resubmits preempted paging commands with the same fence IDs, before other
commands on that engine. DDI snapshot 7515063cea4c9e98db6a92986c5b4ddb0463fd16:
ns-d3dkmddi-_dxgk_submitcommandflags.md:101 documents the WDDM2 Resubmission flag;
ns-d3dkmddi-_dxgkargcb_notify_interrupt_data.md:80-90 defines LastCompletedFenceId
and the preempted range. These are local public Microsoft sources, not assumptions
that a driver can resume its own paused FIFO.

Changes in driver/kmd/wddm.c:
- WddmGpuFencePaging stops starting the next paging job while preemption is pending.
  It still observes and retires the actually executing buffer's real fence.
- Report DPC waits for that hardware buffer, active admission and pending completion
  publication. Queued-but-unexecuted jobs no longer block reaching the boundary.
- WddmReleasePreemptedPagingLocked clears only borrowed job metadata under the queue
  lock before DMA_PREEMPTED. It leaves command bytes and OS storage intact.
  No completion is synthesized for detached jobs.
- Scheduler replay passes through the existing parser/admission path, reusing the
  original fence IDs. There is no autonomous queue restart or duplicate execution.
- A faulted paging engine with unexecuted queued jobs keeps ownership for recovery;
  a late real hardware fence cannot certify a replay path whose admission is closed.
  Existing empty-queue late-completion behavior remains unchanged.

No new header fields, allocation, timer, polling, or hardware register operations.

## Actual-source validation

New experiments/E27-m9-inference/generate-paging-preemption-test.py composes the
existing actual queue harness with extracted actual PreemptFence and ReportDpc.
Companion paging-preemption-test.c supplies scheduler callbacks and assertions.

- /W4 /WX build and 1074 checks, 0 failures: normal queue controls plus active-DMA
  stop, completion-before-preemption, exact last-completed fence, unchanged native
  and direct payload, same-fence scheduler replay inside notification callback,
  exactly-once execution, no stranded FIFO/allocation, admission racing preemption,
  and faulted-engine tail retention after a late real fence.
- Old-dispatch mutation removes only the pending-preemption gate: 643 checks,
  1 expected failure, proving the old source dispatches the next buffer.
- Existing paging queue actual-source suite: 547 checks, 0 failures. External
  builder-import fixtures were not set for this standalone run.
- Existing report controls pass: publication ordering/race, active submit, two nodes,
  rejected packets, watchdog/late fence, high initial IDs and wrap. Its generator
  now extracts exact function bodies instead of swallowing unrelated display code;
  its host model adds only the queue type/tail used by the new helper.

positive.log, negative-old-drain.log, existing-preemption.log and generated
positive.c/negative.c retain the exercised scopes. Run build.cmd normally or with
--old-drain. SOURCE_SHA256SUMS identifies copied source, including existing unrelated
working-tree changes in the shared wddm.c; only the functions above changed here.

## Remaining acceptance

Root owns WDK package build and hardware acceptance. This is not real VidSch
preemption evidence, hardware mid-buffer preemption, engine reset, or M9 completion.
A lab control must show actual preempt notification followed by scheduler replay,
correct full contents and matched submission/completion fences. No driver-controlled
relaunch should occur between the boundary and scheduler replay.
