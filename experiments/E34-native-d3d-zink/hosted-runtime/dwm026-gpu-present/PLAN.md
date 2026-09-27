# DWM026 draft: GPU Present trial with verified CPU baseline

Not built, staged or launched. The source is a draft, not a deployment artifact.
The intended experiment is the engine-copy producer/consumer on the observed
non-UMD1920x1200 pair. Residency contract review and gate configuration remain
pending; interop.ps1 currently retains the gate0 behavior from025. Do not launch
this draft as a GPU Present test or claim that its name enables engine work.

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
