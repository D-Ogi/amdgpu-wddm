# Hardware raster and scanout observation acceptance

Hypothesis: the M443 implementation reports changing hardware scanlines and
only completes flips when the observed scanout matches the queued surface.

Before changing the lab, run scanline_probe.c against retained full WDDM136.
It opens the sole active display adapter by QueryDisplayConfig's LUID, calls
D3DKMTGetScanLine1024 times with Sleep(1), records timestamps/status/raster values,
and closes the adapter. It performs no writes or modeset. Its exit status checks
API success only; the unchanged136 baseline may show the known stale-timer result.
Run in the lab's interactive session through a headless conhost scheduled task;
never launch it on the development desktop. Preserve source/build hashes and STOP.

After source review, use a distinct frozen package containing M443 plus any
reviewed second-batch lifecycle changes; do not repeatedly install intermediate
packages. The initially frozen137 build is an integration artifact, not a promise
of deployment. Retain native SMU ownership and current D3D/ICD identities.

For hardware acceptance, compare active line variation and blank samples with
OTG position/blank bounds from original AMD headers; validate EARLIEST_INUSE
against the settled programmed scanout address. A missing diagnostic route is
an instrumentation gap, not evidence of a register value. Then use the existing
shared-pixel/60-present and GPU-content controls, diagnostic old-buffer-report
counter and owner-visible pacing. Ambiguous deferred observations remain open.
No forced bugcheck, VRR, interlace or own modeset is part of this trial.

Result: pending. Exact deployed state lives in the workspace STATE.md.
