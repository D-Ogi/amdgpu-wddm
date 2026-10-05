# Named DCN observation escape for BD-007/008/009 and BD-018 lab acceptance

Source complete; no lab action. Parent froze138 before this work and owns the following release freeze/version. This report is additional to REPORT.md, which records BD-013/014 fixes and BD-016/019 triage.

## Contract

Command BC250_ESCAPE_OBSERVE_DCN (20), ABI BC250_DCN_OBSERVE_ABI (1), exact payload BC250_ESCAPE_DCN_OBSERVE (128 bytes). The caller must be administrator, use D3DKMT_ESCAPE_DRIVERPRIVATE and set D3DDDI_ESCAPEFLAGS.Value exactly1: HardwareAccess=1, NoAdapterSynchronization=0, every other flag zero. Size and magic are checked by the real dispatcher; ABI/flags/admin are checked before any MMIO. Typed status is returned in Status/NtStatus; successful dispatch alone is not successful observation.

The local Microsoft D3DDDI_ESCAPEFLAGS contract explicitly assigns second-level synchronization to HardwareAccess. Level Two guarantees a single calling thread within the miniport, idle graphics hardware and no scheduler DMA. It therefore excludes adapter stop/MMIO teardown for this short direct reader. There is no independent MMIO lifetime join here, so zero flags or NoAdapterSynchronization would be inappropriate. This reader makes no register writes and no state transition; HardwareAccess is synchronization, not write permission. The synchronization perturbs active GPU scheduling. Do not report these observations as zero-overhead concurrent GPU telemetry.

Sources: ref/ddi-display/d3dukmdt.md, D3DDDI_ESCAPEFLAGS HardwareAccess field (line1012); ref/ddi-display/d3dkmddi.md, DxgkDdiEscape remarks (line7140); ref/windows-driver-docs/windows-driver-docs-pr/display/threading-and-synchronization-second-level.md. Conceptual snapshot110f60eaf2ac5836e644d320c1e92c1011f2af5e, consolidated declarations WDK/SDK10.0.26100.

## Data and limits

The fixed named register set has22 DWORDs. ValidMask bits0..21 follow structure order: primary low/high, earliest-in-use low/high, flip control, surface pitch, OTG position, global control0, blank control, doublebuffer control, frame count, then the11 timing inputs. Timing fields use peer's exact DisplayTimingSnapshot read helper: OTG control, H/Vtotal, H/Vblank, pixel control, DTO phase/modulo, interlace, Vtotal control, CLK reference. The timing mask is all-or-none because the shared helper returns at its first error. Individual first11 read failures clear only their corresponding validity bit; unread outputs remain zero and the first failing NTSTATUS is retained.

SequenceBefore/After bracket software surface publication (DcnSurfaceSequence). They do not freeze autonomous raster, hardware flip latch or frame count. Even equal sequences do not make paired low/high register reads or the entire tuple atomic. The probe must compare repeated stable samples, require valid fields, pendingclear and EARLIEST matching requested primary for positive latch controls; raw observations alone are not retirement notifications. Frame delta and QPC brackets can independently test the decoded refresh rate, subject to wrap and capture duration.

Read offsets are generated from original AMD headers. The only new named constant for this command is OTG0_OTG_GLOBAL_CONTROL0, already in the old public read allow-table. regcalc --ip DMU --reg-header third_party/linux-amdgpu/dcn_2_0_1_offset.h lookup mmOTG0_OTG_GLOBAL_CONTROL0 verified the name/address. No general raw-offset entry point, new write permission, modeset, firmware access or allocation was introduced. Public75-register dump ABI unchanged.

## Source delta

- driver/kmd/bc250kmd_escape.h: command20, fixed ABI/masks, documented synchronization and non-atomicity.
- driver/kmd/bc250kmd.h: DcnObserve declaration.
- driver/kmd/dcn.c: named read-only DcnObserve, uses shared display_timing_snapshot.h.
- driver/kmd/display.c: full-WDDM diagnostic whitelist and exact-size typed dispatch.
- driver/kmd/gen_regs.py and regs.generated.h: name existing read-only GLOBAL_CONTROL0 offset.
- driver/kmd/test/generate_dcn_observe_test.py, dcn_observe_test.c, run_dcn_observe.ps1: actual observer, actual timing helper and actual dispatcher prefix extracted for host verification.

Peer owns display_timing_snapshot.h; it is included in the snapshot for reproducibility, not claimed as this agent's new change. Other source files contain earlier batches and shared peer work.

## Validation

- observe.log:104 checks,0 failures. Verifies all22 named values against independent expected fields, ABI128, full validity, generation movement, original device state, real dispatch size/magic path, admin and exact flags, and partial-read validity. The MMIO model exposes read functions only.
- observe-unsync.log:31 failures when the production requirement is mutated to accept zero flags instead of HardwareAccess.
- observe-wrong-register.log:2 failures when EARLIEST is replaced with primary address.
- observe-stale-sequence.log:1 failure when the final generation is replaced with the initial generation.
- build-observe.log: full WDK build succeeded. Development SYS scratch/build/bd013-014-observe/package/bc250kmd.sys SHA25604E56D6A61F29855EC8AFB75B9E2C048BFDDB381A6FA9A891231BD5DA1F0EF73, shared138 metadata, never deployed.

Final10-file snapshot: observation-source/ and observation-source/sha256.json. Header in this snapshot still has138 metadata; parent owns freeze139. Production edits stopped before parent freeze. No shared STATE/DEFECTS/facts/evidence/version/commit changes.

No hardware acceptance is claimed. Actual blank/unblank, graphics interrupt synchronization, sampled scanout/raster, DTO/refresh agreement and restored POST ownership still need the appropriate lab controls. Do not force a bugcheck for this source review.
