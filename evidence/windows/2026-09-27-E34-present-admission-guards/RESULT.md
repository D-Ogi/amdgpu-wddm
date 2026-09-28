# GPU Present admission and stale-state guards

M619, development-host checks only. Exact parent and source hashes are retained.

Both BGP1 producer and consumer reject BC2C UMD contexts until source and
destination residency on the submitting device has been established. Their
allocation-list sizing returns to the pre-gate policy. Ordinary BC2S GPU work is
unchanged. Non-UMD admission is still diagnostic and the gate remains off.

Every Present callback clears the private recognition word before any early
return or alternate path. The submit consumer never clears it, preserving legal
scheduler resubmission. Host tests check short/null buffers, old-record rejection,
unchanged trailing data, publication after clearing, and repeat matching.

Allocation destruction clears all matching BackingAllocation snapshots under the
adapter object-list lock before the object is removed and its pool freed. This
prevents future lookup from confusing a reused pool address with the old backing.
It does not establish OS handle resolution or concurrent DDI lifetime guarantees.
This destruction behavior has source review and compilation, not a runtime test.

All12 mandatory quick gates pass; actual WDDM/gfx_blt compilation passes. Logs
are copied unchanged, with no private identifiers removed. No new deployment,
full package build or GPU Present runtime validation is claimed.

Contract: local display/residency-overview.md at110f60ea explicitly states that
WDDM2 residency is controlled by the device list, not the Present allocation list.
Local WDK26100 DXGK_ALLOCATIONINFO.pPrivateDriverData is input; modifying it in
CreateAllocation to insert a backing identity is not an established alternative.
GetHandleData failed historically for CDD handles (M83). Runtime identity and
residency, interop admission and complete G0 acceptance remain open.
