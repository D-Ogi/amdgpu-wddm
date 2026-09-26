# Local0761 watchdog correction

Source HEAD bed764d plus E26/E27 working changes. No lab access/deployment.
KMD0.7.61.1 signed SYS F72EF53327BB62150087938839B53B9E613771BA9A50FE81645170E4D3789804.

Both node watchdogs previously cleared hardware-pending state and recorded a successful
DMA completion without an arrived GPU fence. The new code marks a sticky per-node watchdog
fault, closes submission, discards deferred software retirement, and keeps actual hardware
work pending for the OS TDR mechanism. No new interrupt type is used: the driver advertises
WDDM2.0, so the newer GPU_ENGINE_TIMEOUT notification is not assumed available. Later software
completions cannot retire that node's timed-out work. An actual late fence still retires
only the submitted hardware job. Repeated timeout callbacks are idempotent.

Local MS contract: ref/ddi-display/d3dkmddi.md DMA_COMPLETED / SubmissionFenceId describes
completion of DMA work. Original docs commit7515063cea4c9e98db6a92986c5b4ddb0463fd16;
WDK26100 enriched declarations. A timeout is not evidence that such work completed.

The harness extracts actual software-completion, GPU-fence and watchdog functions from old
and new wddm.c. It observes completion records/reports/closed rings, preserves pending work,
checks subsequent software retirement is blocked, injects a genuine late fence, and includes
an on-time positive control for both nodes. Old code fails the no-fake-completion assertion;
new code passes /W4 /WX. Six actual report-DPC preemption scenarios also pass; KMD builds/signs.

Limits: this does not validate hardware TDR recovery, which remains incomplete. Submit-refusal
paths can still synthesize success before a watchdog fault; malformed virtual submission
handling and unsupported paging operations remain open. No deliberate hang was run on lab.
