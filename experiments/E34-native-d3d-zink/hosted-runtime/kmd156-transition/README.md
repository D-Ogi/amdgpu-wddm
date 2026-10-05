# KMD156 allocation identity diagnostic transition

Status: staged and syntax/hash checked, but withheld before execution. The
unstarted task was removed. The built probe must handle a nonzero release handle
even when acquired private data is NULL before any runtime test.

Preparation only. Source cf1aabb8121079cf86fca098214f1978f9624f5d, SYS
309182C1C9AA77314D4589EFE8C75ED7B7F79DC3FDE59A1A06CF9E205AF9ED55.
Baseline153 and exact rollback files are pinned in package-hashes.json.

Follow handle-identity-probe.md. Run preflight156.ps1 through target.py, preserve
its output, check STOP/temperature/health and absence of test workloads. Stage
all scripts and both packages, verify remote hashes and PowerShell5 syntax.
Announce the bounded display interruption through the lab overlay. Use
 dispatch156.ps1 Prepare/Start/Inspect/Cleanup for the durable SYSTEM task.

The installer retains exact pre-transition parameters and CPU UMD/ICD settings,
then explicitly keeps EnableGpuPresentBlit=0 and enables only
EnableHandleIdentityProbe=1. No interop cap is added. Early log capture runs at
most three minutes. Readiness uses bounded separate CLI processes; timeouts are
not evidence of a GPU or OS hang and require independent inspection.

After completion independently verify loaded version/hash, CPU DWM modules,
health/clock/temperature, boot continuity, worker/installer/collector termination.
Run bounded native allocation controls and preserve callback diagnostics before
log wrap. A successful build is not callback or GPU Present runtime acceptance.

Recovery is explicit, not automatic on observation timeout. rollback153.ps1
requires installed156; it force-selects the saved153 package, restores exact
saved parameters/registration and removes diagnostic parameters absent before
the transition. It waits for the restored interface. Partial installation,
reboot requests or unexpected state require stage-specific inspection. The
existing known-plug recovery rules apply only if the lab is hung/unreachable.
No live KDNET. No ordinary OS reboot is planned.
