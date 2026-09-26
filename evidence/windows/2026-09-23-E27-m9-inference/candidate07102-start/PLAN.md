# Candidate0.7.102.1 startup

Hypothesis: finer CP1 checkpoints survive to disk and either localize the earlier07101 stop or document completion. One full-start attempt, no automatic retry/reboot.

Package SYS3B80D2EA3260CE053C11E888A274F909896E28D66D1CF6872B5213EDD5FFA760; packagecheck25passes,0errors,0warnings,14notes (known InfVerif1199 and absent commit binding). Includes M289-M310 paging capture changes and M316 checkpoints; cannot attribute any successful paging difference solely to instrumentation.

Procedure: STOP/overlay and temperature/clock preflight; push exact checked package via Target. install.ps1 closes all gates and validates installed version/hash/boot. Inspect success before start.ps1. Fresh STOP/overlay before ONE full start; stream output to exclusive host file. No OS reboot, DWM restart or speculative reset. If observation is lost, poll the original process and recover persisted logs when available; do not restart because observation expired.

Expected diagnostic evidence: CP1 wrapper checkpoints distinguish lock acquisition from shim KIQ preparation/programming; entering labels precede operations. A terminal result plus successful laterCP steps would establish startup in this one session only. Missing later files does not prove the exact failing instruction because persistence itself can fail; inspect KeepStatus. No M9 completion without live paging/cache/lifetime acceptance.
