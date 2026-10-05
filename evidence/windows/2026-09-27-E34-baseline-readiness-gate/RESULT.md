# CPU composition baseline readiness gate - M651

The fixed2-second launch delay in025 did not establish a valid CPU image.
New check-composition-baseline.ps1 compares all8000 red/overlap pixels to their
known colors. wait-composition-baseline.ps1 requires a running control task,
actual WM_PAINT counters and a passing image before publishing baseline-ready.
Captures/receipts use attempt numbers; failures remain visible. The loop has a
30-second deadline and each capture child a5-second wait bound. A final admitted
capture/check may finish beyond30 seconds. STOP or failure throws before the
caller's GPU transition. This does not establish whole-desktop correctness.

Replay against immutable real captures:024 baseline and final GPU image pass;
025 baseline fails8000/8000;025 final GPU static ROIs pass. Expected outcomes
asserted, input hashes retained. These tests cover the pixel decision, not
concurrent task timing or the remote capture orchestration. Host parser accepts
both helpers and all draft026 scripts. TargetPS5 parse and runtime readiness
validation remain required before launch.

The next runner draft calls readiness before ETW start, DLL replacement and
adapter mutation. It is explicitly unbuilt/unstaged, still has GPU Present0,
and must not be used as a BGP1 test until final gate/manifest/rollback review.
The final manifest must bind the helper copies placed beside the runner.
No lab change or new image measurement in this record.

The draft also fixes a trial-renaming mistake:025 used ETW buffer size1025,
from an overbroad024->025 replacement of1024.026 uses exact identifier changes
and restores1024.025 ETW reported zero events/buffers lost; its immutable
source, manifest and measurement are preserved unchanged.
