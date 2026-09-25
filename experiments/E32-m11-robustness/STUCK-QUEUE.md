# M11 deliberate stuck-queue procedure

Status: host preparation only; NOT executed on unit A.

After the complete 24-hour mixed-load record is preserved, run a single E14
fill_g1 dispatch using stuck.comp compiled as a separate fill.spv. Only invocation0
runs: it atomically writes0 and waits for an atomic read to become nonzero.
There is no writer that can release it. The buffer and descriptor layout match
the existing E14 fill probe. No register writes or firmware changes are involved.

Before injection: verify baseline hashes, boot, native1000MHz/VID116, temperature,
STOP clear, working E14 fill positive control, recovery plug identity/relay and
independent SSH/controller logging. Preserve event/dump and health baselines.
Announce the intentional hang through the overlay.

Launch once using the normal interactive token and final ICD. Record loader
witness and native submission diagnostics. Observe at most30seconds externally.
Do not confuse killing the process with successful GPU recovery. Record whether
Windows reports a TDR, process device loss, bugcheck, or an unrecovered hang.
If OS still responds, collect events/health and preserve logs before recovery.
If unreachable, use the already-authorized smart plug after the observation
deadline, and record both relay transitions. No repeated injections as an
automatic retry. The acceptance criterion is the measured, documented outcome,
not a successful hardware reset.

Recover the same Windows profile and run E14 hashes, both model references and
M10 presentation once to document the post-experiment state. This is separate
from the preceding no-TDR soak, whose 24-hour interval must remain intact.

Source provenance: original test shader; E14 harness and pipeline layout from
this repository. Compiler and SPIR-V hashes recorded locally in scratch/m11/stuck.


## Prepared tools
stuck-worker.ps1 supports Positive and Stuck stages. Both use the same pinned
E14 executable and ICD, select fill_g1, and run once. The stuck shader is kept
in a separate directory; never overwrite the canonical M8 SPIR-V. Positive must
return the CPU/Linux hash before Stuck can start. The worker has a30second
process deadline; killing that process does not establish GPU recovery.

stuck_observe.py requires a collected soak that passes audit.py, a new local
observation directory, working plug readback, no active M11 task and an existing
prepared BC250-M11-stuck-01-Stuck task under the normal interactive token.
It checks STOP and operating-point/temperature before launch. It writes one
arming marker, attempts Start-ScheduledTask only once, and records independent
host observations for30seconds. A missing launch acknowledgment is ambiguous
and is never retried. No power transition is automated by this script.

Lab setup remains deliberately deferred until the soak ends: create a new
C:/BC250/m11/stuck-01 directory, copy the worker and the compiled shader into
its shader subdirectory, register separate normal-token Positive and Stuck
tasks, and run only Positive first. Preserve its output and hashes. Invoke
stuck_observe.py only after this control passes and the complete soak evidence
is local. Inspect its records and the lab immediately at the deadline; use the
standing authorized recovery procedure if necessary. Do not leave an unresolved
GPU hang unattended.

Host syntax/help checks are complete. No stuck worker or shader has been uploaded
or executed. Runtime proof of the observer and the actual outcome remain open.
