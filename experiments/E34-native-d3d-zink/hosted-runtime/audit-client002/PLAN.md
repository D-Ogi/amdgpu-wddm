# Audit client002: bounded hosted flip and deliberate CPU-copy control

Prepared, not deployed or run. Prerequisite: close coordinated slot154 with the other investigator; no simultaneous driver/debugger changes.

Use the Mesa13e623af candidate49A44067, hosted paging-fence ICD C0CE5DCD and control67D231A4 from
aa0c7a8. Build the router with build.ps1 (/W4 /WX), then stage.py packages only
local files with exact hashes. A separate host Target script must transfer this
package to C:\BC250\m13\audit-client002, verify its manifest, update the overlay
and run launch.ps1. Do not run these scripts on the development PC.

The live baseline must be exact KMD164/SYS9B9B99D3, CPU UMD8279AC7F and registered
ICDCF3948D6. Preflight checks STOP, one CPU DWM, health confirmation with freshness,
1000MHz/VID116, temperature below85C and disabled legacy clock writer. Desktop
interop/Present registry gates must be off. No KMD, gate, registry, boot or DWM
restart change is part of this test.

launch.ps1 first arms a SYSTEM watchdog and verifies readiness, then launches an
interactive worker. Before mutation the worker validates stage hashes and the
watchdog PID/start identity, records preflight and copies/flushed verified CPU
UMD/ICD backups. DLL rename/install and child launch use the same mutex as rollback;
watchdog abort is checked under that lock. The temporary router selects only this
control's exact executable path plus its probe environment flag, within60 seconds
of the enable file. Every other process receives the immutable CPU UMD backup.

The registered ICD is temporarily replaced by the previously verified capability
ICD7A9970CA from C:\BC250\m13\shared-import001\vulkan_radeon.dll. Its hash is
checked before mutation and during copy. This preserves the previous small-client
configuration; it is not the hosted rendering ICD. Original files and durable
backups are retained for rollback. Restore refuses an unexpected current hash.

The child runs through a hidden cmd wrapper with native file redirection so stderr
exists and streams immediately to the file the audit control reads. The client
creates the visible320x240 window in the interactive session. The parent enforces
45 seconds, polls STOP/thermal state, and samples the actual UMD/ICD modules by
full path and hash. The control itself requires8 checkpoint ACKs and two exact
readbacks, with known GPU and deliberately CPU-copying intervals (see parent
runtime-audit-control.md). Readback intervals are excluded from GPU-only updates.

The worker's finally stops the child and restores both DLLs. Independent watchdog
starts its90-second clock before the interactive worker: on timeout it writes
abort, stops the worker task, kills the exact-path child and restores. It also
performs idempotent restoration after normal completion. Both receipts must be
terminal; cleanup.ps1 refuses running tasks, nonbaseline hashes or a live child.
The immutable fallback CPU DLL stays in the run directory because a process may
still have loaded the temporary router before restoration.

After normal restoration, closure checks unchanged DWM PID/start, OS boot and KMD
health generation/epoch as well as baseline hashes and thermals. Archive stdout,
stderr, module witness, preflight/closure, worker/watchdog receipts and logs with
Target. Verify exact map-event checkpoint intervals and the12 image-write positive
control before drawing any no-copy conclusion. A PASS printed by the app alone
is insufficient. No ETW/DWM GPU ownership or full G0 claim comes from this client.

Validation before staging: MSVC router/control builds, PowerShell AST parsing,
source review of timeout/restoration paths. Live fault-injection, successful lab
execution, module capture and CPU-copy detection remain unmeasured.

DDI acceptance: all12 deliberate frame-write maps must correlate to ResourceMap
scopes. The application writes those pointers itself; these calls must not emit
frontend copy_complete events. Require zero frontend-copy witnesses in the GPU
clear interval as well. UpdateSubresource/initial-data copy witnesses still need
a separate positive client arm before relying on them in DWM. No new lab result
is claimed by reusing the measured client executable.

Local preparation complete: router3A6B8759 builds /W4 /WX; all10 PowerShell
scripts parse under5.1. Package has13 hashed files, manifest
769F384565D42FD70C7534B61C2CA8ECF3AD1F76138CFDC89A8B0D80E26E0CA1.
No staging or execution on unit A. Existing client001 analyzer regression passes.
