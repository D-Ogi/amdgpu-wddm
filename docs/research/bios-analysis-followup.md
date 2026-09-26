# BIOS analysis follow-up

Reviewed 2026-09-24 against the current source, through facts M430-M435.
Workspace-only reports: firmware/bios/analysis/README.md; repeatable analysis
scripts now live in firmware/bios/analysis/scripts/. Do not depend on deleted
scratch/tmp/fwre/work or scratch/tmp/bios. No firmware code is imported here.

## Display review

| Item | Decision and acceptance remaining |
|---|---|
| D1 - update-lock acknowledgement | M430 adds bounded ACK wait; M431 propagates failure without publishing a new scanout address, permits a later identical-address retry, guards vsync snapshots and counts timeouts. M432 live control passes533 flips with0 ACK timeouts, correct D3D content and concurrent GPU workload correctness. Owner confirmed smooth image/movement after M432; timed long-duration observation remains open. |
| D2 - control fields | FLIP_CONTROL changes only SURFACE_FLIP_TYPE. M431 also preserves SURFACE_CONTROL except the two graphics TMZ fields. This does not enable DCC or protected surfaces. |
| D3 - manual trigger | Retain. The report's absence claim was scoped too narrowly: dcn201_tg_funcs selects optc2_program_manual_trigger; core/dc.c and dc_hw_sequencer.c call it after unlocking eligible address updates. Existing M87 hardware trace already observes it. No removal A/B is required to establish that the Linux path exists. |
| D4 - unused pipes | A modeset/full-display-ownership task. Preserve inherited working single-pipe state until that implementation owns all affected resources. |
| D5 - timings | Replace the nominal full-WDDM 60 Hz/zero-blanking report using measured timing and clock data, with proper rational units. 2080x1235 are totals, not active image dimensions. Do not assume the report's 59.95 Hz applies to every mode. Extend L8 for clock data. |
| D6 - geometry | One validated geometry must cover AddressAllowed, DcnScanoutMapping's mapped byte length, FillSurface, pitch/extent checks and mode reporting. Replacing only width/height constants leaves an undersized mapping. Compare post-display ownership with live registers before admitting a different mode. |
| D7 - reservations | Reviewer correction: donor vram_usagebyfirmware is zero, so historical GOP workspace alone does not justify a permanent post-ExitBootServices reservation. Verify live consumers on our own unit (L35); preserve disjoint active GART, PSP/TMR and ring ownership. |

Additional ownership work: diagnostic flip/restore now refuses while VidPn owns
OTG0; stop's quiesced restore remains separate. Synchronize
OTG_GLOBAL_SYNC_STATUS enable/disable read-modify-write with the actual display
ISR through the supported interrupt synchronization callback, not only a DPC
spin lock. Keep this as an explicit next task. Future modeset/ODM must select
MASTER_UPDATE_LOCK_SEL explicitly instead of relying on inherited OTG0 state.

Evidence: [M430](../../evidence/windows/2026-09-24-E27-m9-recovery/dcn-lock-ack-review/RESULT.md)
and [M431](../../evidence/windows/2026-09-24-E27-m9-recovery/vidpn-flip-publication/RESULT.md).
M432 installs a distinct134 candidate and passes the hardware controls in its
[evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07134-dcn-publication/RESULT.md).
The diagnostic trial also corrected the earlier review: full WDDM already had
a dispatcher guard refusing mutating escapes before DcnFlipEscape. The new inner
guard is additional protection and was not reached by that hardware trial.
M429 SMU ownership/backend/startup work
remains pending; these display changes do not close the M9 cold-start gate.

## 12 GiB residency

The supplied static Setup/AGESA analysis identifies a 12G option and a consuming
code path without a discovered clamp below it. This is a candidate configuration,
not proof of training, OS boot, stable GPU access or our requested residency.
Obtain the unit's own AmdSetup plus two matching read-only SPI dumps in L35.
Reuse the completed static analysis rather than repeating donor-image extraction.
A software dump is not a programmer-made recovery backup. Firmware writes remain
subject to the workspace's explicit consent and programmer-backup rule.

There is a second capacity gate: with the current WddmMemoryLayout reservations
and current framebuffer geometry, a 12 GiB carve-out projects to **11.58642578 GiB**
for the application segment (12440829952 bytes). Page tables, the firmware
framebuffer and the driver tail consume the rest. Other live local allocations
reduce the model's available share further. Reducing reserves needs a justified
layout; a pagefile or GTT budget is not extra local VRAM. Preserve the original
12 GiB goal separately from testing all usable bytes of a 12G carve-out.

Use the complete-set probe with all members retained, every-member residency
samples and distinct full GPU readbacks. M428 demonstrates 6 GiB; no larger
hardware capacity is claimed here. See the source projection in M430 evidence.

IOMMU needs DMA-remapping-aware driver ownership before any setting change.
Read actual clock/power limits through established interfaces (L8/L10); BIOS
mask decoding alone does not establish runtime limits or safe new clocks.

The [numbered driver review follow-up](driver-review-2026-09-24.md) tracks the additional display, memory and SMU findings, their corrections and remaining acceptance.
