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
