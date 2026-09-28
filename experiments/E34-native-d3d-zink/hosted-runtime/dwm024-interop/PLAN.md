# DWM024: bounded CDD interop shape capture on KMD160

Not launched. Exact KMD1608E676C81 and hosted UMD5C74BF98/ICD3508416F.
CPU126/GPU127 sharing controls passed (M642).
EnableCddDwmInterop1, EnableGpuPresentBlit0, identity probe1, via adapter restart.
The measured DWM window lasts180 seconds, with a300-second watchdog.
Restore first recovers baseline DLLs, then returns interop to0 via adapter restart
and restarts DWM. A pending marker makes partial adapter changes recoverable.
No OS reboot or clock ownership change. Check85C/STOP and exact baseline hashes.

Hypothesis: enabling interop produces CDD Blts involving the existing typed
LB7A DWM textures. Capture flags, allocation entries and startup identity logs.
Zero Blts is inconclusive. E26P system-memory refusals are a known limitation;
stale content does not qualify a successful transport. These CPU copies cannot
qualify G0. Runtime shapes determine GPU admission work; no BC2A extension is
assumed. Do not claim mapping lifetime from bounded logs.
Owner visual acceptance is separate and not inferred from captures.

Before launch: compile router/control, parse all scripts under target PS5,
verify staged hashes and exact159 rollback availability. The watchdog handles
library and adapter-setting rollback, but does not promise recovery from a hung
GPU; follow existing lab recovery rules if the OS stops responding.
