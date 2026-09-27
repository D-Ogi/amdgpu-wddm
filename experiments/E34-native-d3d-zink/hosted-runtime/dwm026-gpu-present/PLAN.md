# DWM026 draft: GPU Present trial with verified CPU baseline

Not launched. Final build, manifest and target preflight required.
The intended experiment is the engine-copy producer/consumer on the observed
non-UMD1920x1200 pair. The025 trace has17 written-primary references on the same CDD device, plus
source MakeResident1 and live VA pairs. The OS residency rule for written-primary
references supports a bounded diagnostic; retention through GPU completion must
be measured rather than inferred from025 CPU copies. Interop/GPU Present both1
during this trial, both0 on rollback. The BGP1 branch cannot fall back to E26P.
KMD161's first hardware submissions log VMID/root/IB/fence; the bounded startup
collector preserves these. Hardware completion uses GfxFenceArrived; failure
closes the path and leaves outstanding fences pending, never fakes completion.

The runner replaces the fixed2-second delay with wait-composition-baseline.ps1.
After actual WM_PAINT evidence, retain numbered captures and require all8000
known static pixels to match before any DLL replacement or adapter transition.
Missing readiness or a30-second retry deadline fails without GPU transition.
Individual fbdump child waits are bounded to5 seconds and retain process identity.
The loop can exceed30 seconds by the last admitted capture/check duration.
Copy the two shared helper scripts beside the runner and include their hashes
in the final manifest before staging. Verify targetPS5 parsing and replay controls.

The independent300-second rollback and producer270-second lifetime are retained.
Keep the measured GPU interval180 seconds. Recheck the total time budget after
all final instrumentation is known. Preserve failed captures; a later successful
capture does not erase the failure history or excuse another invalid baseline.

Trial renaming uses exact identifiers. DWM025's script used ETW buffer size1025
because an earlier broad024->025 replacement also changed1024. This draft
restores1024;025 reported zero lost events, so its recorded trace is not invalidated.
Do not alter the completed025 source/manifest or immutable evidence.

Runtime pass requires nonzero BGP1 records/submissions, actual hardware fences,
matching node/context/root/IB witnesses and loss-free ETW completion. A healthy
DWM or zero CPU blits alone cannot substitute. Report any rotation/refusal/error
and all image failures. The first16 detailed records do not constitute a full
lifetime trace. Terminate/rollback on device loss or failed startup; never rerun
because an observation times out. Retain original process identities.
