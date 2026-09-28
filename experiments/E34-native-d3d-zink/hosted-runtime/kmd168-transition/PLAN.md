# KMD168 bounded transition - preparation only, not runnable yet

Candidate source6fc639e / final005 SYS130A9D20, exact166 rollback SYSAA77E8B3.
CPU UMD8279 and registered ICDCF39 remain the baseline. No hosted DWM workload
belongs in the driver transition. A later combined GPU trial has its own180s bound.

Implemented: pure watchdog decision and monotonic child-budget helpers, tested
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
