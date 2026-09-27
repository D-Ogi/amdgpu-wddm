# KMD153 diagnostic transition preparation

Status: prepared and syntax checked, not deployed or runtime validated.

Hypothesis: the bounded per-type diagnostic from M602 captures GDI allocation
requests during adapter start after the generic first-eight-call budget is used.
This is preparation for the engine Blt work, not CDD/DWM interop implementation.

Inputs: M602 source c3499f1b12dcc57902fdd522c3ad10498abc5620,
package-umd 0.7.153.1, SYS
0C0DA4E13FF110606265274B063D3B6034459A294FE2A552FD5806E0971B05EB.
Baseline: 0.7.152.1, SYS
7729EF3ED65E31F4B914E16026324AEE83E89002D1D113449E19188D3F91FA9D.
package-hashes.json records every file of both preserved packages. Candidate
hashes were checked against the immutable M602 build-summary.json.

Procedure:

1. Coordinate the lab slot and check STOP. Run preflight153.ps1 through target.py,
   preserve stdout/stderr privately and inspect all results. It queries versions,
   module hashes, temperature, clock, health, test processes, parameters, class
   registration and log without modifying driver or registry configuration.
2. Stage the candidate at C:\BC250\m12\candidate07153\package-umd and the
   preserved152 package as rollback152 beside it, with scripts and manifest.
   Verify every remote package file against the manifest before execution.
   Announce display interruption on the overlay and recheck preflight.
3. Run install153.ps1 as a durable remote job that can be inspected independently
   of SSH. This dispatcher is not prepared yet. The script preserves settings and
   their types, class registration and logs before disabling the adapter. It
   installs153, restores parameters and registration, then enables the adapter.
   An observation timeout does not establish that the job failed or stopped.
4. Collect early GDI records and compare type, phase and geometry to the local
   WDK10.0.26100 d3dkmdt.h. A missing record is inconclusive if the ring wrapped.
   Even retained startup coverage only describes this configuration, which does
   not advertise CDD/DWM interop.
5. Verify loaded version/hash, health, clock1000/820, CPU DWM module, paging and
   GPU controls. Only then run a bounded desktop trial of at most three minutes.
   No153 runtime validation has been performed yet.

Recovery is explicit, never automatic on an SSH timeout. The earlier151-to152
PnP transition hit TDR116. Preserve the transcript, boot identity and dump first.
If SSH responds, inspect the durable job and device state. rollback152.ps1 uses
UpdateDriverForPlugAndPlayDevicesW with INSTALLFLAG_FORCE (SDK10.0.26100
um/newdev.h) because pnputil alone does not force an older driver. It restores
saved parameters and registration without deleting driver-store packages. Reboot
requests and unexpected state stop for inspection. It requires installed153 and
the saved configuration: partial installation requires stage-specific recovery.
A hung lab follows the existing recovery procedure. No live KDNET.

Local validation: all three scripts pass the development PowerShell parser;
this is syntax validation, not execution of installation or rollback. The
read-only preflight completed at target UTC2026-09-27T04:33:09Z:152 hashes match,
CPU DWM4596, no test processes, only overlay/watchdog lab tasks,66.8C,native1000MHz,
legacy writer disabled. Raw receipt and full log remain private under
scratch/g0-hosted/kmd153-transition/preflight-20260927T043250Z. No new runtime
fact or promotion is claimed. Record the actual transition and regression in a
new immutable evidence set and facts.md entry.
