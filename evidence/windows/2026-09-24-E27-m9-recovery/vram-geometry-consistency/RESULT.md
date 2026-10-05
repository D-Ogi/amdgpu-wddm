# M435: VRAM geometry agreement and POST reservation

2026-09-24, source/host only. No lab action or deployment. Addresses come from
regcalc over AMD headers; the named NBIO MEMSIZE offset already exists in the
5355-entry read allow-list derived from unit A's Linux trace. No new MMIO address
was added to that allow-list.

VramStart now compares GC FB_LOCATION_BASE/TOP capacity with NBIO MEMSIZE before
publishing VramEnabled/VramLength. A mismatch leaves access disabled and returns
DEVICE_CONFIGURATION_ERROR. RunSetup reconciles the actual AMD shim output's
mc_vram_size, real_vram_size and vram_start with that established device geometry
before marking setup usable. This second check catches later differences too.

WddmMemoryLayout no longer interprets an unresolved POST framebuffer as an empty
reservation. It refuses a layout unless the framebuffer resolves through the
verified physical or BAR0 view with nonzero extent. BAR0 failure alone does not
reject a valid physical view. Existing GART CheckWindow already refuses unknown
POST identity, so this is not evidence of a previously observed corrupt segment.

## Validation

Actual VramStart, VramFramebufferOffset, WddmMemoryLayout and RunSetup bodies
are extracted by generate_vram_geometry_test.py and compiled by run_vram_geometry.ps1.
MMIO, OS ranges and AMD setup outputs are mocked; allocation/decision code is real.

- 8,12,16GiB agreeing geometries: VRAM, segment and GART setup pass.
- Differing and zero NBIO capacity: no VRAM publication or OS-memory query.
- All three shim geometry mismatches: setup refused.
- Missing BAR0 and unresolved POST: no application/table segments.
- Missing BAR0 but verified physical POST: layout succeeds; zero extent refuses.
- 72 checks, zero failures. Full WDK build/link/sign passes.
- Removing the capacity comparison yields15 failures.
- Removing required framebuffer resolution yields6 failures.
- Initial host adapter redundantly defined SAL macros; compiler /WX rejected it.
  Removed those test-only definitions; original log retained.

No memory training,12GiB hardware access, firmware change or runtime deployment
was performed. Host capacity acceptance is not GPU residency. Display pitch and
other numbered review findings remain tracked in the source snapshot.

Development SYS (never installed, still134 version metadata): SHA256 d464191a907c4781cf8bedc738ccd74137356d973d353b886c02d369831fea7e.
The lab remains on M432's distinct134 binary. Logs and snapshots contain no secrets.
