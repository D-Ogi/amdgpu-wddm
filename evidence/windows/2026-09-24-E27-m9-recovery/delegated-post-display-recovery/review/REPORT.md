# BD-003 / BD-011 bounded display recovery (not deployed)

Source changes are after candidate137 frozen in `scratch/m9/display137-build-source`.
No lab access, forced bugcheck, deployment, version change, shared backlog/state/evidence edit or commit.

Implementation:
- `DcnRestorePostDisplay` reuses the captured firmware address/pitch and existing quiet flip sequence. It verifies a cleared FLIP_PENDING, physical EARLIEST_INUSE equality and firmware pitch before publishing success. Observation uses physical addresses so it remains valid independently of the VRAM allocation gate. Bounded high-low-high reads prevent a torn address.
- A settled correct framebuffer needs no writes. A flip is followed by at most 50 ms explicit polling stalls, using the existing 100 us step; lock ACK still has the existing 10 us bound. These are explicit stall budgets, not measured wall-clock limits.
- Bugcheck SystemDisplayEnable restores the framebuffer actually addressed by the already-mapped CPU view before allowing SystemDisplayWrite. It covers a crash after hardware programming but before DcnDiverged publication by checking the captured firmware state, not only that flag. Failed restore leaves dimensions zero and CPU writes disabled.
- The crash path uses nonpaged state and code, checked register access, interlocked counters and KeStallExecutionProcessor. It allocates nothing, takes no software lock, logs nothing, starts no worker and waits on no event/scheduler. It does not call the ordinary stop path.
- SystemDisplayWrite retains CPU copying, clips rows to the target and columns to source stride, and refuses writes unless SystemDisplayEnable completed. No cache attribute or new mapping change.
- Ordinary stop keeps the M441 native-SMU join first. WddmStop disables/joins DPCs, restores POST, records status, then retires VidMm state and WDDM objects. Display-only mode without a WDDM object restores in pnp.c before other teardown. There is one restore attempt, with no automatic retry hidden at the end.
- StopDeviceAndReleasePostDisplayOwnership returns restoration failure and zero output metadata if restoration failed; ordinary StopDevice retains its completion status. Successful metadata names our actual single active target. Local MS explicitly says a failed release callback is followed by ordinary StopDevice.

Material limits:
- This fixes the framebuffer mismatch; it does NOT implement the full SystemDisplayEnable requirement to cancel all GPU work or reset to idle at bugcheck. No full reset is added or claimed. The chosen target is the existing always-connected inherited single output; no hotplug, modeset, cursor or gamma implementation is added.
- A restore failure does not quarantine all OS display allocations or guarantee a visible screen: normal stop teardown still proceeds. The release callback now honestly refuses to hand Basic Display stale metadata.
- Physical bugcheck recovery, Basic Display handover and real latch timing remain untested. Host MMIO controls are not a hardware recovery claim.

Validation:
- `restore.log`: 554 checks, 0 failures, actual DCN write sequence/physical read/check/restore and actual SystemDisplayEnable/Write extracted. Positive delayed latch, correct pitch, CPU image copy, row/stride clipping, idempotent restore, mid-flip publication, unchanged basic-display path. A deliberately unlatched surface returns timeout without allowing CPU output.
- `stop-v2.log`: 70 checks, 0 failures, actual WddmStop, ordinary StopDevice and release DDI extracted. Verifies SMU join, DPC join, restore before VidMm/object retirement, failure propagation with zero metadata, ordinary-stop status and no-WDDM fallback.
- `observation-regression.log`: 6726 checks, 0 failures; M443 observation behavior survives factoring physical scanout reads from card-address conversion.
- Negative actual-source controls: skipped bugcheck restore gives 10 failures; omitted INUSE check 6; unconditional handover success 2; restore after VidMmStop 6. Corresponding logs `no-bugcheck-restore.log`, `no-inuse-check.log`, `false-handover-success.log`, `late-restore.log`.
- `build-final.log`: full WDK build passes. Output `scratch/build/bd003-011-final/package`, DEV SYS SHA256 `69B905A3BA97E4F7BC29E5A41000F9B3A1851549B20B2B82349EB634756E5202`. This is a combined working-tree development artifact with shared137 metadata (0.7.137.0 base package), never deployed; peer firmware metadata work can be included in the build and full wddm.c snapshot. `changes.patch` includes only this task's WddmStop delta for that shared file.

References:
- Local `ref/ddi-display/dispmprt.md:9508-9532`: keep the active mode, visible target and CPU-accessible framebuffer; return current settings.
- Same file `:9546`: successful release replaces StopDevice, failed release is followed by StopDevice.
- Same file `:9702-9727`: bugcheck enable requirements, CPU writes to the current screen, nonpaged/any-IRQL callback, kernel services may be unavailable.
- Same file `:9822-9843`: CPU source copy to the current framebuffer, nonpaged/any-IRQL callback.
- `ref/windows-driver-docs/windows-driver-docs-pr/display/threading-and-synchronization-third-level.md`: StopDevice exclusion and idle/evicted assumptions (local staging110f60ea).
- AMD DCN MIT provenance is unchanged: Linux source `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`, hubp1_is_flip_pending and optc1_lock, named original DCN2.0.1 register fields. No new register or write permission was introduced.

Suggested append-only backlog comments:
- BD-003: source now restores and verifies the permanently mapped POST framebuffer before enabling bugcheck CPU writes. Actual-source 554 checks pass, removed-restore mutation fails10, full WDK build passes. No physical bugcheck trial and no complete GPU cancellation/reset implementation; do not call this full bugcheck recovery acceptance. Suggested status FIXED for the reported framebuffer mismatch, or IN-PROGRESS if tracking the full callback contract here.
- BD-011: source now restores after DPC join but before VidMm/object release, checks actual INUSE/pitch/pending state, and propagates the result from release while ordinary stop remains independent. Actual stop controls70 pass, false-success and late-restore mutations fail2/6. Suggested FIXED (source); physical healthy handover still requires the next candidate.

Exact changed files are listed in `source-sha256.json`; final source snapshot is `source/`.
Main owns evidence, versioning, public documentation updates and hardware acceptance.
