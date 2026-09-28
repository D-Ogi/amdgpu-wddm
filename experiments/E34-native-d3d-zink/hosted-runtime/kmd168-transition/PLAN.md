# KMD168 bounded transition - preparation only, not runnable yet

Candidate source6fc639e / final005 SYS130A9D20, exact166 rollback SYSAA77E8B3.
CPU UMD8279 and registered ICDCF39 remain the baseline. No hosted DWM workload
belongs in the driver transition. A later combined GPU trial has its own180s bound.

Implemented: native suspended-start Job Object ownership, absolute-QPC child
runner, PowerShell5.1 argument encoding and watchdog/budget helpers, tested
under Windows PowerShell5.1. Candidate admission ends at110s, leaving70s for
restoration. A single QPC origin covers both phases. No child gets a fresh total
budget. At180s no further test operation is admitted; incomplete recovery is an
explicit failure requiring separate recovery, never successful closure.

Still required before launch:
- exact source/package manifests and fresh healthy166 preflight;
- durable original registration/parameters and mutation boundary before PnP;
- independent watchdog with task/process identities and bounded child ownership;
- installer and forced166 restoration using the existing verified newdev path;
- caller wrappers applying these budgets to every blocking command;
- restoration begins only after the original worker is proven terminal; an SSH
  observation timeout alone neither stops nor restarts a job;
- terminal restoration evidence verifies installed/loaded SYS and CPU UMD/ICD,
  independent health15, no unexpected OS restart, then task cleanup;
- failure-injection controls at pre-disable, disabled, installed and enabled phases.

Do not copy the old five-minute task and unbounded WaitForExit from166. A kernel
PnP hang cannot be made recoverable by terminating PowerShell; preserve evidence
and report recovery-required instead of racing another installer. Native GPU
content and combined desktop validation remain separate required controls.

## Bounded child implementation

`build-bounded-child.ps1` builds the native helper with /W4 /WX. It creates the
child suspended, assigns a kill-on-close job, then resumes. On root exit or timeout
it terminates remaining descendants and checks ActiveProcesses before emitting
job_empty. The last second of its absolute deadline is reserved for termination.
This bounds user-mode ownership, not a kernel PnP hang or firmware recovery.

`Invoke-KmdBoundedChild` uses explicit Windows argv encoding for PS5.1, a hidden
helper and the same absolute deadline. Lack of a complete receipt is failure,
never proof of restoration. Return0 requires root exit0 and empty job;124 is a
timeout,125 an infrastructure/closure failure,126 a child error. Output files are
create-new so reusing a run cannot overwrite receipts. Invoke with stage-specific
QPC deadline, capped by the single trial boundary; never reset it per child.

`test-bounded-child.ps1` passes under PS5.1: success with quotes/spaces/trailing
backslash, child error, timeout, orphan cleanup and timed-out descendant tree.
Local results: scratch/g0-hosted/bounded-child-tests-ps51-004/results.json.
Only the process primitive is complete; installer/watchdog integration is pending.
