# Sustained hosted DWM control

After M562's bounded composition proof and M565-M567's sharing/lifetime and
native graphics controls, run the current UMD23F5269C/ICD3508416F for 180
seconds under DWM. This remains a diagnostic deployment. It does not bypass
outstanding M13 sharing/residency or lifecycle acceptance.

Use a dedicated one-process DWM router, enabled only for initial selection.
The GDI-only control retains the red/half-alpha blue rectangles from M562 and
adds a cyan decorated window moving/resizing every half second. Compare static
interior pixels in CPU-baseline and GPU KMD-primary captures, and final screen
capture. Moving geometry stays outside those static regions. No client GPU
renderer can account for composition of these GDI surfaces.

Before mutation verify hashes, STOP, temperature and free storage. A separate
scheduled watchdog must be running before enabling GPU DWM. It restores the
baseline at 240 seconds, while the main runner restores in finally. Both use
the same restoration mutex. A replacement DWM cannot claim the GPU route.

Observe one DWM PID for 180 seconds; verify its hosted module identities and
log growth, STOP and thermal state, and window task health. Inspect audit/fence
progress after closing the writer; concurrent Get-Content was rejected by file
sharing in the retained DWM011 attempt.
Abort on restart, missing witness, failed capture, thermal limit or low storage.
Collect DxgKrnl to a sequential ETL without circular overwrite; event loss must
be checked before using the trace. Store raw ETL/screens privately. Capture
global KMD counters and readback images independently of the compositor.

Expected positive: correct composed pixels, continuing GPU completion/Present,
no unexpected DWM restart, assertion or device loss, and successful rollback.
Any failure is retained and investigated. A surviving process is insufficient.
The owner-requested three-minute duration alone does not prove 60 Hz cadence, Start-menu
correctness, all shared-resource cases or broad M13 lifecycle acceptance.

The owner shortened this diagnostic to three minutes on 2026-09-27; it is not
an execution of the longer historical M13 acceptance duration.
