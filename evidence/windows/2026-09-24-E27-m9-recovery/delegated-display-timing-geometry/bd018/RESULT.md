# BD-018: inherited mode signal timing

2026-09-24. Source/host implementation only; status NEEDS-LAB. No lab connections,
deployment, version changes, commits or shared backlog/state/evidence edits.

The real implementation and both users are in display.c, not wddm.c:
FillSignalInfo, OfferTargetMode, Bc250RecommendMonitorModes.

Changes:
- Full WDDM no longer manufactures 60 Hz, total=active and pixel=width*height*60.
  FillSignalInfo reads two complete identical timing tuples and decodes the inherited mode.
- Totals are the OTG fields plus one; active size must match the horizontal/vertical blank
  bounds and Windows POST dimensions. The supported path is fixed progressive OTG0 with
  enabled DP DTO, no interlace, DRR total selection, pixel add/drop or half-rate output.
- DCN201 reference frequency comes from CLK4_CLK2_CURRENT_CNT * 100 kHz. Pixel frequency
  is PHASE * reference / MODULO, rounded to the nearest integer Hz because PixelRate is
  integral. HSyncFreq=PixelRate/HTotal; VSyncFreq=PixelRate/(HTotal*VTotal), explicit rationals.
  No 600 MHz fallback when the reference counter is zero. No assumption PHASE alone is Hz.
- Read failures, changed snapshots, unsupported clock/mode state, or active geometry mismatch
  propagate through both mode-offer callers. Failed mode objects are released, not published.
- Display-only keeps its existing unspecified-frequency path and does not read MMIO.
- No timing register writes, new shared device cache or own modeset implementation.
- Shared display_timing_snapshot.h gives diagnostics the same ordered 11-register tuple.
  display_vblank is integrating this getter into its read-only observation escape20.

Contracts/references (local):
- ref/ddi-display/d3dkmdt.md:1614-1632: TotalSize entire signal, ActiveSize active pixels,
  HSyncFreq/VSyncFreq rational Hz, PixelRate pixel clock rate (WDK 10.0.26100 declarations).
- Microsoft conceptual threading-and-synchronization-third-level.md: StopDevice/StartDevice
  are exclusive Level Three. Mode callbacks run on the established adapter; the helper
  requires PASSIVE_LEVEL and a mapped BAR. RecommendMonitorModes is listed at Level Two.
  This is not callable as a free-standing background worker racing PnP teardown.
- Linux v6.18 7d0a66e4bb9081d75c82ec4957c50034cb0ea449, AMD MIT:
  display/dc/optc/dcn10/dcn10_optc.c:191-249 (total minus one, blank bounds), :1424 (active width);
  display/dc/clk_mgr/dcn201/dcn201_clk_mgr.c:200 (reference counter *100 kHz);
  display/dc/inc/hw/clk_mgr_internal.h:84/100 (CLK4 register instantiation);
  display/dc/dce/dce_clock_source.c:1192-1227 (DTO readback, ratio versus VBIOS PHASE=Hz agreement).
- Original dcn_2_0_1 masks/offsets and Cyan IP bases; clk_11_0_1_offset.h imported byte-for-byte
  from the same AMD MIT tree, one PROVENANCE line added. Addresses only through gen_regs/regcalc.
- Own-unit M86/E21 dmupre.txt proves HTotal=2080, VTotal=1235 and inherited OTG0.
  It does NOT include DTO/reference/blank/interlace controls used by this new decoder.

Exact ordered tuple, all read-only:
0 DMU.mmOTG0_OTG_CONTROL
1 DMU.mmOTG0_OTG_H_TOTAL
2 DMU.mmOTG0_OTG_V_TOTAL
3 DMU.mmOTG0_OTG_H_BLANK_START_END
4 DMU.mmOTG0_OTG_V_BLANK_START_END
5 DMU.mmOTG0_PIXEL_RATE_CNTL
6 DMU.mmDP_DTO0_PHASE
7 DMU.mmDP_DTO0_MODULO
8 DMU.mmOTG0_OTG_INTERLACE_CONTROL
9 DMU.mmOTG0_OTG_V_TOTAL_CONTROL
10 CLK.mmCLK4_0_CLK4_CLK2_CURRENT_CNT

Generation: gen_regs.py adds named/private DCN read entries and permits READ_REG observation of
new timing inputs/reference. Existing 75-register DCN escape payload is unchanged. It also adds
BLANK_CONTROL and DOUBLE_BUFFER_CONTROL named/read/write entries requested by display_vblank
for BD-013; those TWO WRITE additions belong to BD-013, not this timing implementation.

Validation:
- run_display_timing.ps1: 197 checks, zero failures (host-final.log).
  Actual helper and extracted actual FillSignalInfo; each mocked read checked against its
  exact generated register name/path. Synthetic fixtures include 2080x1235 at154MHz
  (59.950171286Hz), another geometry/reference ratio, and fractional DTO scaling.
  Also changed snapshot, zero reference, interlace/DRR, POST mismatch, read failure,
  wrong IRQL and display-only no-MMIO controls.
- AssumePhaseHz mutation: 197 checks, four failures (phase-mutation.log).
- Full WDK build succeeds (kmd-build.log); developer binary includes concurrent work and
  is NOT a release artifact. No deployment claim. Final subsequent change only splits the
  same snapshot helper into a shared header for diagnostics; host controls rerun afterward.
- Synthetic CLI replay example passes (replay-synthetic.log), not a lab measurement.
- Initial host harness SAL macro-redefinition failure retained in host.log; corrected final
  harness passes. git diff --check reports only CRLF normalization warnings.

Replay tool for the parent's captured tuple:
  scratch/build/bd018-final/timing_test.exe --snapshot WIDTH HEIGHT <11 raw values in order>
Input words accept 0x-prefixed hex or decimal. Prints the actual decoder's status, totals,
integer pixel Hz and rational H/V rates. This replays a single coherent tuple, so check the
observation's before/after tuple and validity first; replay alone does not prove coherence.
Actual decoder entry: DisplayDecodeTiming(values,width,height,signal), display_timing.h.
Getter: DisplayTimingSnapshot(device,values), display_timing_snapshot.h.

Required positive lab control before declaring FIXED/VERIFIED:
1. Read the complete tuple twice through escape20 alongside POST width/height; preserve raw
   values, status/validity and observation sequence. No clock/timing writes.
2. Verify current live reference nonzero, progressive/static enabled DP DTO, active bounds
   matching POST, and two identical tuples. Replay through the supplied actual decoder.
3. Cross-check inferred rate against an independent timed OTG frame counter interval or
   hardware-vsync count; retain a known-good desktop/content control after mode negotiation.
   A nominal59.95 label or EDID capability alone is not evidence of current pixel rate.
4. If the reference counter is zero, unstable or unsupported, investigate that source/config;
   do not restore a fabricated 600 MHz/60 Hz fallback. Alternate PHY clock, VRR/interlace,
   borders/scaling and own modeset are outside this supported path.

Proposed BD-018 append-only comment:
- 2026-09-24 Codex: FillSignalInfo and its two display.c callers now support reference-derived
  inherited timing: stable OTG totals/blank bounds plus DCN201 reference counter and DP DTO
  ratio; full WDDM refuses missing/unsupported observations instead of inventing nominal60Hz.
  Mode failure propagates with object cleanup.197 host checks pass; PHASE-as-Hz mutation fails4;
  WDK build passes. M86 proves totals but lacks the new clock tuple. NEEDS-LAB until paired raw
  capture, actual decoder replay, independent frame-rate observation and display control pass.
