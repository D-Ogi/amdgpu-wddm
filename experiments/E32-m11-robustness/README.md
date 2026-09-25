# E32: M11 mixed-load robustness

Status: preparation, 2026-09-25. No M11 acceptance yet.

## Requirements
Run 24 continuous hours on unit A, rotating E14's eight compute tests, both
E14 inference models and M10 presentation. Require exact reference hashes/text
throughout and in the final cycle, no TDR, bugcheck or new live kernel report,
no leak in the deployed driver's pool tags, and temperature below 85 C.
Run one deliberately stuck queue separately and document the observed outcome.

## Fixed baseline and procedure
KMD147 SYS5FCB554A, desktop UMD 8279AC7F (surface padding and atomic resource closure), M10 ICD9C40083C.
Use a normal interactive worker so Vulkan loader environment selection works.
An independent privileged scheduled monitor samples native KMD clock/temperature,
health generation/epoch, Windows events and live-kernel-dump metadata. Each
completed cycle waits at a quiescent checkpoint for PoolMon. Store all project-tag
rows and allocation/free/bytes counts; missing transient tags are zero, not omitted.
The monitor must acknowledge each checkpoint before the worker continues.
Requests and acknowledgments are empty, uniquely numbered files created once
per cycle. Readers test existence and never open an ACK for text. This avoids
the sharing collision that stopped soak-01 at cycle88; no old interval is
carried forward after this harness correction.

First run a bounded two-cycle control. Then freeze scripts, binaries, shaders,
models and references by SHA256 and start a new 24-hour ledger. The test continues
on the lab across SSH interruptions; status files alone do not establish liveness.
Poll the actual worker/monitor processes and scheduled tasks.

Timeout, STOP, temperature >=85 C, changed boot/binary/generation, content mismatch,
TDR or new live-kernel report closes the run immediately. Preserve failed runs.
No automatic resume/restart can turn multiple shorter runs into 24 hours.
Inspect pool trends and final settled samples; any unexplained retained growth
prevents acceptance. Warm-up allocation changes must be explained, not ignored.

The stuck-queue experiment follows the preserved soak run, with independent logs
and smart-plug recovery if required. It is not included in the no-TDR soak interval.
Injection mechanism and expected behavior must be specified before execution.

## References
Roadmap M11; E14 reference hashes/text; M476 presentation.
PoolMon snapshot syntax: local Microsoft devtest/poolmon-startup-command.md.
Deployed pool tags are taken from scratch/m9/combined147-build-source, not the
newer dirty KMD source. No live KDNET or firmware changes.

## Preparation result: M477
The initial controls exposed shared-resource retention in the desktop UMD.
Apply mesa-resource-close.patch after E26 and M474. The two-cycle regression
returns every measured project tag to its baseline while pixels, compute hashes,
model text and presentation pass. See ../../evidence/windows/2026-09-25-E32-resource-close/RESULT.md.
This does not replace either remaining M11 acceptance requirement.


## Final collection and acceptance audit
After both tasks are terminal, run collect.py with the existing run identifier
and a new workspace destination. It refuses a live run. final-readback.ps1
independently reads Windows event logs with positive read controls, compares dump
metadata and boot identity, rehashes every input and checks the loaded desktop
UMD. It does not restart the OS, driver or tests.

Run audit.py on the collected directory. It reads every compute result and model
output against the canonical Linux references, verifies loader witnesses and raw
output hashes, checks the complete cycle/pool/temperature ledger, and requires
24 monotonic hours plus the independent final readback. A first-checkpoint rise
is accepted only if subsequent checkpoints do not grow and every final tag
returns exactly to its initial count and bytes. Any unexplained change requires
investigation; never relax a failed check merely to obtain PASS.

The audit deliberately covers only the soak. M11 additionally requires the
separate one-shot stuck queue and a manual requirement-by-requirement review.
Measured old/fixed controls and a self-consistent corrupted-output mutation are
covered by test_audit.py. The short passing control must fail the 24-hour gate.
