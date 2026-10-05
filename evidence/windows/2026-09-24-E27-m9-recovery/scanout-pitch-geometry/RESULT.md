# M436: primary pitch and inherited scanout geometry

2026-09-24, source and host validation only. Addresses and masks are generated
from named AMD DCN 2.0.1 registers. No lab command, deployment or OS restart.
Workspace backlog items BD-002 and BD-017; no new defect IDs.

## Implementation

Shared primary allocations use a 256-byte (64-pixel) aligned row pitch and
reserve pitch times height, including padding. Shadow/staging/GDI linear
surfaces keep their own pitch. SetVidPnSourceAddress reads private geometry
from a non-NULL allocation handle, checks the current inherited mode and extent,
and programs its pitch with its address. NULL handles preserve the current
properties (initially POST). A same-address pitch change reaches hardware;
failed programming preserves published metadata and permits a retry.

DCSURF_SURFACE_PITCH is admitted through gen_regs.py. The flip writes its PITCH
field as pixels minus one under the existing acknowledged OTG lock, preserving
META_PITCH and unrelated bits. Firmware address and hardware pitch are captured
before the first flip and checked against POST pitch and mapped extent. Restore
uses that saved firmware pitch. No changes to the manual-trigger decision.

AddressAllowed, diagnostic FillSurface and DcnScanoutMapping use POST width and
height with the selected pitch. Mapping covers every padded row and remaps when
pitch changes even at the same address. A generation-checked snapshot prevents
mixing address and pitch across publication. The firmware seed copies visible
pixels row by row between distinct source/destination pitches; source mapping
uses the firmware extent, avoiding truncation when the destination is narrower.
Padding is not copied. Remapping invalidates the seed marker.

## Controls and results

Production function bodies are extracted into host fixtures; MMIO, allocation
mapping and OS callbacks are simulated. Tests compile the real geometry helpers.

- MMIO sequence: 35374 checks, zero failures. Existing delayed-ACK, timeout,
  control-field preservation and escape ownership controls retained. New pitch
  encoding and META_PITCH preservation controls pass.
- DDI publication: 109 checks, zero failures. Includes different-pitch allocation
  under an unchanged address, refusal/retry, NULL-handle preservation, undersized
  allocation, nested writer and vsync generation controls.
- Geometry: 6892 checks, zero failures. Actual allocation DDI, firmware capture,
  address bounds, fill, mapping and row-copy functions. Widths 1366/1440/1680/
  1920/2560 with heights 768/900/1050/1200/1440. Canary bytes and row padding stay
  untouched; all visible rows match, including the last one. Mapping is reused
  when unchanged and rebuilt for a same-address pitch change. 2560x1440 exceeds
  the old fixed 1920x1200 extent.
- Deliberate regressions are detected: fixed hardware pitch gives 3 failures;
  ignoring same-address pitch changes gives 8; unaligned primaries give 3.
- Full WDK build/link/catalog/sign passes. Development SYS SHA256 `c993f187efe4ce84de90fbcda20ff80e1401a695aa6a5b72046a1a8827213031`.
  Artifact `scratch/build/scanout-geometry-dev-v2/package/bc250kmd.sys` retains
  version134 metadata but was NEVER installed. The installed M432 binary differs.
  The earlier development build log is retained; v2 is the final source here.

## Contracts and limits

Local Microsoft DDI collection `ref/ddi-display/d3dkmddi.md`, field hAllocation
of DXGKARG_SETVIDPNSOURCEADDRESS, requires private allocation properties such as
pitch to be programmed. Original WDK/SDK version is 10.0.26100. The NULL-handle
preservation rule is our adaptation of that contract.
Linux v6.18 `drivers/gpu/drm/amd/display/dc/hubp/dcn20/dcn20_hubp.c`,
hubp2_program_size, supplies pixel-minus-one encoding; register fields come from
AMD dcn_2_0_1 headers. No GPL implementation was copied.

The driver still admits the inherited POST mode; this does not implement new
hardware modesets or prove any of these widths on unit A. Host capture/sequence
controls are not a measured PnP display handover. Allocation lifetime throughout
a concurrent CPU present, earliest-in-use completion, bugcheck visibility,
restore failure reporting and timing remain separate open work. Snapshot
consistency does not establish CPU/GPU coherence or buffer residency/lifetime.
No 12 GiB residency or overall M9 acceptance is claimed. STATE.md stays M432.
