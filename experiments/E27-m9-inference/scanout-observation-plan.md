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

Baseline result: M444 on retained136 returns 1024 successful calls, all blank and
line0. Corrected-candidate acceptance remains pending. Candidate138 is frozen
and builds successfully with the M445/M446/M447 additions; no deployment yet.
During the same trial, inspect runtime firmware caps and normal paging's PTE
observation buckets. The latter count attempts, not residency or retirement.
Exact deployed state lives in the workspace STATE.md.


Candidate139 trial combines the reviewed third display batch and ABI1 named
observation with M442-M447. Candidate137/138 were built but never installed.
Perform one full136 -> full139 PnP transition, preserving native clock ownership,
Mesa modules and a136 rollback package. No OS restart or forced bugcheck.

The named observation escape uses HardwareAccess=1 and no other flags, requiring
administrator and Level Two adapter exclusion. It performs no register writes
but idles GPU scheduling. Record 16 raw observations around 1024 normal scanline
queries, QPC brackets and all validity/status fields. This is a correctness
instrument, not an unperturbed performance measurement. Check stable scanout
sequence/pending/actual-address equality; two identical 11-word timing tuples;
actual decoder replay; frame-counter interval against the derived rational rate;
and QueryDisplayConfig's advertised target timing. Hardware latching can still
move inside an individual observation; do not call it an atomic snapshot.

Query runtime firmware metadata through real D3DKMTQueryAdapterInfo, compare it
with exact prepared-image metadata and the known SMU reply, then run existing
D3D shared/pixel/60-present,64MiB eviction/full-GPU-readback,8shader and twoE14-model
controls. Preserve PTE observation summaries and exact candidate/module hashes.
Visibility hide/show and actual stop of139 are not automatically proved by its
first successful startup. Avoid monitor power/sleep until full resume exists.
