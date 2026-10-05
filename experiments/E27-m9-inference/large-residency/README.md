# Large GPU residency and bounded NVMe backing

Owner request2026-09-24: test GPU residency covering75% of the physical16GiB,
therefore12GiB, and provide sensible page-file capacity. Allocation success or
full model offload does not meet the residency requirement.

Preflight on M426133: installed memory17179869184bytes, Windows-visible
8296042496bytes; VRAM carve-out0x200000000bytes, application segment0x1ED880000.
The present local segment cannot hold12GiB simultaneously. Do not substitute
75% of that smaller segment or a paged12GiB allocation as acceptance. Increasing
local carve-out or enabling sufficient concurrently resident shared memory needs
separate implementation/configuration and evidence. Firmware writes retain the
owner's explicit-consent rule; no firmware change is part of this plan.

Page-file preparation, before changes: automatic management, current7936MiB,
peak90MiB; current commit3965624320/16617541632bytes; C volume481573347328bytes
free out of536870907904. Set a fixed32768MiB page file on the lab NVMe C volume,
retaining original settings and active usage in a private lab record. This is a
capacity choice for12GiB data plus competing backing and OS use, not a bandwidth
optimization or additional GPU memory. Verify sufficient free disk before write.

The local driver docs describe paging-file-backed storage but not administrator
sizing/configuration. Consulted primary MS sources for that gap:
- https://learn.microsoft.com/en-us/troubleshoot/windows-client/performance/how-to-determine-the-appropriate-page-file-size-for-64-bit-versions-of-windows
- https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-pagefilesetting

WMI settings apply at system startup. Preserve M426 evidence, set the one-shot
full WDDM gate for the already installed133 and restart Windows once to activate
and verify the page file. This is an OS-boot startup test, not an AC cold-power
proof. Honor STOP/85C and check1000MHz/820mV after the startup task. Preserve the
M412D3D/M414RADV module selection and diagnostic USB Windows fallback.

After boot: verify page-file active size, commit limit, installed133 hash, loaded
full table, guard, clocks, temperature and desktop process/module. Repeat64MiB
positive GPU residency before increasing pressure. Record all outcomes. Use
existing recovery only when actual responsiveness requires it, never solely
because an observer timed out.

Large-test acceptance must distinguish dedicated/shared/nonresident states,
record memory budgets and paging while work executes, and validate full data by
GPU readback. Current probe caps size at1GiB and creates a same-sized pressure
allocation; extend its64-bit sizing and aggregate deadline deliberately before
larger trials. Hardware per-job watchdog limits stay unchanged.

## Follow-up plan: post-boot PnP control (2026-09-24)

The pagefile boot at18:23:50 activated32768MiB, but candidate133 selected
its display-only table after one-shot persistence returned0xC000014D in
DriverEntry. Preserve both pagefile32 logs before changing the device.
Hypothesis: the gate can be durably consumed in the running OS and the same
candidate can reach full GPU readiness through PnP. This does not prove an
OS-boot solution or establish which registry operation failed.

Use the unchanged installed0.7.133.1/SYS37A52F95... after confirming exact hash,
healthy display-only adapter, STOP clear, temperature below85C and1000MHz/820mV.
Disable/enable the identified display adapter once without reinstallation or OS
restart. Retain the M412 desktop DLL and display gates, arm the existing one-shot
full gate and SDMA startup controls. Capture startup log before ring wrap, prove
all controls and full-table selection, then close the control gates and confirm
the guard. Check unchanged OS boot, DWM module, active pagefile and clock.

Run the existing SHA-pinned64MiB positive GPU residency probe with fresh outputs
`boot64m` in a separate directory. Require native exit0, all four full GPU
readbacks, three residency cycles and hardware completion counters without
errors. On a failed transition, preserve its state before any recovery; do not
repeat an unchanged boot. No firmware or permanent full-mode policy change.

## Resident-set probe plan

Add a separate `--resident-only` mode, leaving the existing three-cycle control
available. Permit up to12GiB using64-bit sizes; scale only aggregate tool waits,
not KMD per-job watchdogs. This mode creates no same-sized pressure buffer and
does not evict the preserved allocation. Query its entire allocation residency
and both process memory budgets before each64MiB of GPU readback and at the end.
Require GPU-memory status1 for VRAM, shared status2 for GTT; never accept status2
as dedicated VRAM. Verify every word using the existing GPU copy/CPU oracle.
Queries are samples, not proof of uninterrupted residency between samples.

Validate the new mode first at64MiB on unchanged133, then6GiB within the current
8GiB carve-out if the control passes.6GiB is instrument/capacity progress only,
not the owner's12GiB acceptance. Preserve exact probe hash, all snapshots,
completion counters, pagefile/commit and clocks/temperature. A failed allocation
or budget query is a result to inspect, not grounds to drop the witness.
The local WDK26100 declarations and Microsoft descriptions for
QUERYALLOCATIONRESIDENCY, ALLOCATIONRESIDENCYSTATUS and QUERYVIDEOMEMORYINFO in
`ref/ddi-display/d3dkmthk.md` define the queried fields.

### Complete-set extension after the single6GiB attempt

The first v5 control passes64MiB, while CreateAllocation2 rejects a single6GiB
request with STATUS_INVALID_PARAMETER before any residency sample. Preserve
that failed attempt; its cause is not established. Extend resident-only mode to
retain up to12 members of at most1GiB each. Create and make all members resident
before filling/readback, query all handles together at every sample, and require
the sum in the requested class to equal the entire target. Use global word
indices to detect inter-member aliasing. Do not free or evict any member until
all reads finish. Repeat64MiB first, then the same total6GiB, with new artifacts.
This is a simultaneous working-set test, not sequential recycling of1GiB.
Local `display/residency-overview.md` describes the device residency list and
MakeResident/Evict reference counts; budget alone does not identify physical
placement. The single-allocation failure remains a separate open limitation.
