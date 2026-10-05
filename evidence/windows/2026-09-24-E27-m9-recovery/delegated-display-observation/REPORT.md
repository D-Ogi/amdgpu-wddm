# BD-007 / BD-008 / BD-009 source follow-up (not deployed)

Changes are after frozen candidate 136. No lab, deployment, shared state/backlog, evidence or version edits.
Final source snapshot: `source/`; hashes: `source-sha256.json`; delta against the frozen candidate: `changes.patch`.

Behavior:
- BD-007: hardware GetScanLine reads OTG_STATUS_POSITION, V_TOTAL and V_BLANK_START_END. Blanking and active-relative line use the same counter sample; the old software-timer path is retained only when hardware flipping is disabled.
- BD-009: completion requires FLIP_PENDING clear AND EARLIEST_INUSE equal to the requested address. The high/low address observation uses bounded high-low-high reads, converts CPU physical back into the driver's card address and refuses an unavailable observation. The M431 generation check remains in place.
- BD-008: a stable deferred flip whose hardware-observed address is a distinct previous buffer now reports that buffer at vblank. It never substitutes a cached or fabricated address. Odd/changing publication generations, failed observations and EARLIEST_INUSE matching the requested address while FLIP_PENDING remains asserted still skip that sample. This is deliberately partial coverage of deferred cases, not a claim that all vblanks are reported.
- Added `DcnVsyncOldBufferReports` beside `DcnVsyncDeferred` in the summary. The latter now means completion-deferral attempts; their difference identifies samples not recovered by old-buffer reporting (not a precise lost-interrupt count: existing DPC coalescing remains).
- The private DCN read allow-list grows from 75 to 78 entries. The public 75-register DCN escape payload and write allow-list are unchanged.

Source basis:
- `ref/ddi-display/d3dkmddi.md:12801`: FlipOnVSyncMmIo notification reports the effective scan address.
- Same file `:37739`: CrtcVsync.PhysicalAddress is the displaying buffer.
- Same file `:7538`, `:32833`: current target raster position and vertical blank.
- Original DDI repository snapshot `7515063cea4c9e98db6a92986c5b4ddb0463fd16`; consolidated WDK/SDK 10.0.26100 declarations.
- PROVENANCE: Linux AMD display `dc/hubp/dcn10/dcn10_hubp.c` (MIT), `hubp1_is_flip_pending:751`; `dc/optc/dcn10/dcn10_optc.c` (MIT), `optc1_get_position:697`, `optc1_get_crtc_scanoutpos:1228`, vertical timing programming near `:223`. Local linux-src `7d0a66e4bb9081d75c82ec4957c50034cb0ea449`.
- Offsets from regcalc with original DCN 2.0.1 header: `registers-dmu-v2.log`. Two preceding lookup attempts selected the default GC header / a missing relative header path; neither supplied code offsets. gen_regs.py generated every actual constant.

Validation:
- Final publication: 121 checks, 0 failures (`publication-counter.log`). Actual DDI, completion helper and hardware-vsync callback extracted from source. Covers old-buffer reports, odd generation, completed generation crossing, fallback ABA generation, pending matching address, failed programming and pitch updates.
- DCN observation: 6,726 checks, 0 failures (`observation.log`). Actual helpers + GetScanLine DDI. Two complete raster frames, wrapping/nonwrapping blank, 8/12 GiB address geometry, address high-dword transition, mismatch and pending checks.
- Existing flip sequence: 35,374 checks, 0 failures (`flip.log`). Existing scanout geometry: 6,892 checks, 0 failures (`geometry.log`). These functions were unchanged after those runs.
- Negative controls: timer raster 4,199 failures; omitted EARLIEST comparison 2; dropped valid pending-vblank 4; removed completed-generation check 6; removed fallback-generation check 2. Logs `timer-scanline.log`, `no-earliest-v2.log`, `drop-deferred.log`, `no-generation.log`, `no-fallback-generation.log`. These precede addition of the diagnostic counter; they are not hardware fault injection. The first omitted-EARLIEST mutant failed /WX for its unused parameter and was corrected before the actual negative run.
- Full WDK build after counter: `build-v2.log`, output `scratch/build/bd007-009-v2/package`; SYS SHA256 `EE83AE2CB8E771BA092CD5CB6BB6C46709B52CE2F9859968F1CFAE8D4787A913`. Development working-tree artifact, NOT deployed. Metadata remains 136 and must not be confused with the frozen deployed candidate.

Suggested backlog consolidation:
- BD-007 -> FIXED (source), append: hardware scanline decoding now bypasses VSyncLast; actual-source raster controls and WDK build pass; physical observation/raster timing acceptance remains.
- BD-009 -> FIXED (source), append: pending-bit plus EARLIEST_INUSE match required, fail-closed observation and generation protection retained; positive controls and removed-check mutation pass/fail as expected; physical latch acceptance remains.
- BD-008 -> IN-PROGRESS, append: normal stable pending-old-buffer vblanks are reported; matching/pending, generation-race and unavailable observations remain intentionally deferred pending a fuller hardware-observation design. New counters separate successful old-buffer reports from completion deferrals; host checks pass. Do not call this every-vblank coverage.

Hardware acceptance for the next candidate:
1. Read-only positive control of EARLIEST_INUSE (compare programmed/current scanout in a settled desktop) and OTG counter/blank bounds before relying on new observations.
2. Sample D3DKMTGetScanLine through the real DDI during active desktop; observe changing active lines and actual blanking, with no timer-phase dependency.
3. Run the existing D3D shared/green/present control and compare actual image/cursor pacing, flip/completion counts, old-buffer reports and remaining deferrals, without pretending host MMIO simulation proves DCN timing.
4. Keep the fixed-refresh, one-pipe inherited mode limit explicit. No VRR/interlace/own-modeset acceptance is claimed.
