# KMD169 transition rehearsal failed; exact166 recovered

Unit A, 2026-09-28. Runner79d877d5acf43b2386faddbe7a65537d80f8d4dd;
KMD169 source5985a4164fda6717a19c322f0ca3711a6bad7735, SYS AF715A56 prefix.
Stage004 manifest3BAF74E9A0CCEAB02CF4A280481383F9EDCD8748CAAEB37E6B7A4605E080898B.
CPU desktop registration throughout the planned166->169->166 rehearsal.
Candidate outer90s/inner87s, restoration170s, task180s. This was not a GPU DWM run.

The supervisor ended after69.6090579s with restore-unverified. Candidate install,
configuration and enable completed. Verify rejected UnconfirmedStarts=1 through
38 samples. The predicate required0 before checked health confirmation, which the
runner never performed. This is a procedure defect, not evidence of bad KMD169
rendering or a VSync stall.

Rollback reached exact166 installation, but the forced installer automatically
started the device; the expected disabled-state check failed. Configure was never
executed. Subsequent reads found exact166 SYS, device problem43, FullWddm0 and
INF-default bc250umd.dll registration, with UnconfirmedStarts1. Health returned
C000000E. These observations do not isolate the cause of the failed device start.

Recovery restored the saved CPU registration and attempted a DWM restart, which
was insufficient. After preserving receipts, saved driver configuration was
restored (FullWddm2, gates0) without resetting UnconfirmedStarts or diagnostic
stage history, followed by an orderly OS restart. No AC cycle.
New boot02:22:19.5Z: exact166, CPU UMD8279 loaded in DWM1580; health7. Checked
health confirmation for observed generation565448105/epoch5 returned15. One exact
candidate INF package was removed without force, and the terminal task removed.
Final preflight02:26Z passes,67.6C,no test workload. See recovered-summary.json.

Raw selected trial receipts are preserved here. Full local archive and recovery
logs remain in scratch/g0-hosted/kmd169-stage004-ops; private identifiers excluded
from the derived recovery summary. Remaining work: separate pre-confirmation
readiness from confirmed-state verification; handle forced installation's actual
device state before restoring configuration and starting the device. KMD169's
VSync fix remains unvalidated at runtime. G0 remains open.
