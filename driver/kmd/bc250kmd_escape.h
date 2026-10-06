// Private escape data shared between bc250kmd and lab tools (D3DKMTEscape, D3DKMT_ESCAPE_DRIVERPRIVATE).
// dxgkrnl does route them to a display-only driver (facts M29), so this is the control channel for the
// bring-up experiments (ADR 0007). Register commands work only for an administrator, only while the registry
// gates of mmio.c are open, and only on offsets of the generated tables.
#pragma once

#define BC250_ESCAPE_MAGIC 0x30353242u      // "B250"
#define BC250_ESCAPE_GET_INFO 1u
#define BC250_ESCAPE_READ_REG 2u            // in: RegOffset (BAR5 byte offset); out: RegValue
#define BC250_ESCAPE_WRITE_REG 3u           // in: RegOffset, RegValue; out: RegValue read back after the write
#define BC250_ESCAPE_GET_MEMORY 4u          // BC250_ESCAPE_MEMORY out: where the framebuffer, BAR0 and the VRAM carve-out are
#define BC250_ESCAPE_VRAM_READ 5u           // BC250_ESCAPE_MEMORY in: Path, Offset; out: Value
#define BC250_ESCAPE_VRAM_WRITE 6u          // BC250_ESCAPE_MEMORY in: Path, Offset, Value; out: Value read back
#define BC250_ESCAPE_RUN_GART 7u            // BC250_ESCAPE_GART in: Op; out: what the sequence wrote (or would write)
#define BC250_ESCAPE_RUN_PSP 8u             // BC250_ESCAPE_PSP in: Op; out: register writes and PSP commands
#define BC250_ESCAPE_RUN_GFX 9u             // BC250_ESCAPE_GFX in: Op, LastStage; out: stages, register and doorbell writes
#define BC250_ESCAPE_RUN_IH 10u             // BC250_ESCAPE_IH in: Op; out: interrupt counts, vectors seen, register writes
#define BC250_ESCAPE_RUN_FENCE 11u          // BC250_ESCAPE_FENCE in: Ring, Count, Interrupt; out: fences completed, timing
#define BC250_ESCAPE_GET_LOG 12u            // BC250_ESCAPE_LOG in: From; out: the driver's log ring from that sequence on
#define BC250_ESCAPE_LOG_SUMMARY 13u        // BC250_ESCAPE_LOG: wddm.c writes its counter tables into the ring, then as GET_LOG
#define BC250_ESCAPE_RUN_DCN 14u            // BC250_ESCAPE_DCN: a read-only dump of the DCN registers (ADR 0011 point 3)
#define BC250_ESCAPE_RUN_DCNFLIP 15u        // BC250_ESCAPE_DCNFLIP in: Physical, Fill, FillColor, Restore; out: the
                                            // whole flip sequence, decoded (ADR 0011 point 3 step 2)
#define BC250_ESCAPE_RUN_SDMACOPY 16u       // BC250_ESCAPE_SDMACOPY in: Bytes; out: the SDMA copy/fill positive
                                            // control (ADR 0013), read back and compared by the CPU
#define BC250_ESCAPE_RUN_FBDUMP 17u         // BC250_ESCAPE_FBDUMP in: Hubp, FirstRow, RowCount; out: a read-only
                                            // band of the scanned-out surface's pixels, for bc250kmd_cli fbdump
#define BC250_ESCAPE_RUN_SDMAIB 18u         // BC250_ESCAPE_SDMACOPY payload, VMID0 indirect fill/copy control
#define BC250_ESCAPE_RUN_CLOCK 19u             // typed SMU telemetry or complete operating-point transaction
#define BC250_ESCAPE_OBSERVE_DCN 20u       // named, read-only scanout and timing observations
#define BC250_ESCAPE_RUN_START_HEALTH 21u      // cached start/presentation witness and checked confirmation
#define BC250_ESCAPE_RUN_CU_MODE 22u            // CU mode snapshot (24 or 40 CUs) and boot-guard confirmation
#define BC250_ESCAPE_RUN_DPM 23u                // DPM governor telemetry and boot-guard confirmation
#define BC250_ESCAPE_GET_PAGING_JOURNAL 24u     // BC250_ESCAPE_PAGING_JOURNAL in: From; out: the paging journal from that
                                                // record on (page table updates, fills, transfers, flushes, destroys)
#define BC250_ESCAPE_RUN_INTEROP 25u            // GPU DWM interop switches: requested, effective, reason, session marker
#define BC250_ESCAPE_RUN_DPM_TUNE 26u           // DPM governor thresholds, floor, thermal timing: read, set, reset (not persisted)
#define BC250_KMD_VERSION 0x000700D0u       // revision 208 (INF 0.7.208.1, on 207.1): five summary lines of the
                                            // guard log that did not fit a log line are two lines each.
                                            // BC250_LOG_TEXT is 160 bytes and RtlStringCchVPrintfA truncates
                                            // without a word, so 0.7.207.1 printed "wddm summary: scan-out ...
                                            // format/geometry/pitch/size/segment/alignment/gated 0/0/" and lost
                                            // every scan-out refusal count (BD-070). The scan-out summary, the
                                            // VidPn flip summary, the blit destination summary, the object
                                            // created/destroyed summary and the vidmm PTE encoding summary now
                                            // write two lines each, every one of them inside the line. For the
                                            // scan-out, the VidPn flip and the blit destination summaries the
                                            // first line keeps word for word the text its parser reads, so the
                                            // scan-out trial and the overlay read what they always read. The
                                            // other two pairs needed their reader changed: runcompare takes the
                                            // object pair as a block of two lines, and monfence's run-lab.ps1
                                            // keys the vidmm counters by level and segment. No escape struct, no
                                            // journal record layout, no counter
                                            // and no gate changed, and BC250_LOG_TEXT itself is untouched: the
                                            // constant moves because packagecheck VRS010 matches it against the
                                            // INF revision, and because 0.7.207.1 is installed on the lab and
                                            // Windows keeps the driver it has when the version ties. The new
                                            // gate guardlog-width (tools/quality/quick.ps1) measures every
                                            // GuardLog format against the 159 characters a line holds.
                                            //
                                            // revision 207 (INF 0.7.207.1, on 205.1): the b18 train driver. It
                                            // carries three changes that were written apart as 0.7.195.1,
                                            // 0.7.206.1 and 0.7.206.2. None of those three revisions was
                                            // ever deployed. The train keeps all three and takes the next
                                            // number.
                                            //
                                            // (a) M15.14, the first revision in which an application's own
                                            // swap-chain buffer can be scanned out. SetVidPnSourceAddress no
                                            // longer refuses every UMD allocation outright; the rule is
                                            // scanout_admit.h (format, POST geometry, pitch, size,
                                            // 4 KiB address, residency in the DirectFlip segment),
                                            // host-tested through each refusal, and dcn.c's
                                            // AddressAllowed is unchanged behind it. A surface asks for
                                            // scan-out through BC2A v3 (UMD_BLOB_A_SCANOUT and four
                                            // appended geometry words, inside the existing 192-byte
                                            // wire size) or through the E26R access bit SCANOUT, which
                                            // also places the allocation in the local segment. The new
                                            // counters ride the existing LOG_SUMMARY ring, so no
                                            // escape struct and no escape ABI changed for this change.
                                            // The admitted buffer is recorded before the plane is
                                            // programmed, so that DestroyAllocation restores the
                                            // firmware surface for the buffer the plane really holds.
                                            // EnableScanoutAdmit 0 (absent = on, the INF writes no
                                            // value) refuses every requesting candidate with the
                                            // status "gated" and leaves this start as 0.7.205.1 was.
                                            //
                                            // (b) An idle GPU runs at
                                            // 500 MHz (owner decision 2026-10-05: "jak lab nie pracuje,
                                            // to ustawiaj mu zegar gpu na 500 MHz" - when the lab does
                                            // not work, set its GPU clock to 500 MHz). The clock table
                                            // reaches down to 500 MHz at the lab floor's 820 mV /
                                            // VID 116: 16 levels, index 0 = 500 MHz (the idle point),
                                            // index 3 = 800 MHz (BC250_DPM_THERMAL_FLOOR_LEVEL),
                                            // index 5 = 1000 MHz (BC250_DPM_FLOOR_LEVEL). 700 and
                                            // 600 MHz keep the 100 MHz grid whole and no rule selects
                                            // them. The governor holds the idle point after
                                            // DpmIdleHoldMs (3000) with the mean busy share of the
                                            // graphics engine and the paging node under
                                            // DpmIdleBusyPermille (2) and no work on the GFX ring, and
                                            // leaves it for the lab floor at the first tick with work
                                            // again; DpmIdleMHz 0 turns the whole state off. A thermal
                                            // limit, a runtime floor, SetStablePowerState, a missing
                                            // sensor and a hot part all keep the state out, and a start
                                            // that does not govern never configures it. A refused
                                            // idle point falls back to 800 MHz, then off, like the
                                            // sub-floor, and does not count towards the SMU give-up
                                            // limit. RUN_DPM is ABI 2 (192 bytes) and appends the
                                            // state's setting and counters; the driver still takes the
                                            // 160-byte ABI 1 request. RUN_DPM_TUNE is unchanged
                                            // (ABI 2, 152 bytes), and a runtime tune floor still has
                                            // to be 1000 MHz or more.
                                            //
                                            // (c) Composed A8 surfaces (M14.1). The shared table's A8 row
                                            // (D3DDDIFMT_A8) carries COMPOSED at 1 byte a pixel and
                                            // DcnLinearSurfaceBytes takes 1-byte pixels, so a type-0 LB7A A8
                                            // surface (the shared atlases DirectComposition creates, Task
                                            // Manager's 32x32 A8 render target) is created and opened. GDI
                                            // types keep their own set; Present Blt and scan-out still refuse
                                            // A8. No escape struct or journal layout changed for this change
                                            // either. The change was written as revision 195 on 0.7.194.1 and
                                            // was never deployed, so it rides the train too.
                                            //
                                            // revision 205 (INF 0.7.205.1, on 204): the clock table gains
                                            // two thermal-only points below the lab floor, 900 and
                                            // 800 MHz, both at the floor's 820 mV / VID 116 (owner
                                            // decision 2026-10-05: a clock under 1000 MHz is allowed
                                            // when Tctl reaches 87 C). 13 levels, index 0 = 800 MHz,
                                            // BC250_DPM_FLOOR_LEVEL = 2 = 1000 MHz. The load still
                                            // never asks below 1000 MHz; only the thermal cap goes
                                            // lower (one level per hot step from 87 C, 800 MHz at once
                                            // at 90 C). A missing sensor, SetStablePowerState, the
                                            // fixed mode, stop, power down and giving up stay at
                                            // 1000 MHz. The power tables publish no level below
                                            // 1000 MHz (facts M47) although unit A's firmware accepts
                                            // 800 and 900 MHz (facts M785), so a refused sub-floor
                                            // transition is logged ("sub-floor refused"),
                                            // does not count towards the SMU
                                            // give-up limit, and stops the cap at 1000 MHz for the
                                            // rest of that start. Every escape carries MHz, not a
                                            // level index: no struct and no ABI changed (RUN_DPM
                                            // ABI 1, RUN_DPM_TUNE ABI 2). A runtime tune floor still
                                            // has to be 1000 MHz or more.
                                            // 204 (INF 0.7.204.1, on 203): the DPM warm zone
                                            // (no raise) starts at 87 C, the hot limit, instead of
                                            // 85 C (owner, 2026-10-04). The thermal ramp's interval
                                            // runs from 1 s at 70 C to 4 s at 87 C. At 87 C and above
                                            // the hot cap lowers one level per hot step and the warm
                                            // rule refuses every raise. No escape struct changed.
                                            // 203 (INF 0.7.203.1, on 202): the DPM governor's
                                            // thermal ramp (BC250_DPM_RAMP_KNEE_MC, session 367). From
                                            // 70 C up to the 85 C warm zone a raise goes one level at
                                            // most, at least 1 s (70 C) to 4 s (85 C) after the last
                                            // raise; lowering and the thermal limits are unchanged. The
                                            // dpm log lines count the cut or held raises ("ramp N"),
                                            // throttle 9 is thermal-ramp. No escape struct changed.
                                            // 202 (INF 0.7.202.1, on 201): BD-065 diagnostics.
                                            // A GPU Present whose allocation snapshot is refused is
                                            // counted by the first failing check (owner, unbound,
                                            // BC2A, format, ...) and the first 16 are logged with both
                                            // descriptors. Behaviour and status codes unchanged. No
                                            // escape struct changed.
                                            // 201 (INF 0.7.201.1, on 200): the full table offers
                                            // VidPN source modes in A8B8G8R8, A2B10G10R10 and
                                            // A16B16G16R16F beside A8R8G8B8 (display_modes.h), so DXGI
                                            // can list modes for those formats (3DMark, session
                                            // native-caps349); the scan-out stays 8-bit. Registry value
                                            // OfferComposedSourceModes 0 turns them off. No escape
                                            // struct changed.
                                            // 200 (INF 0.7.200.1, on 199): the DPM governor
                                            // refuses a raise of clock or voltage from 85 C up to the
                                            // 87 C hot limit (BC250_DPM_WARM_MC, session 344); the dpm
                                            // log lines count the refused steps ("warm N"), throttle 8
                                            // is thermal-warm. No escape struct changed.
                                            // 199 (INF 0.7.199.1, on 198): the standard-allocation
                                            // size query leaves the public Pitch alone, the fill publishes it
                                            // (d3dkmddi.md:32953); the wddm summary counts standard
                                            // allocation requests and answers by kind and GDI type, LB7A
                                            // create (created/refused/rolled back) and open outcomes, and
                                            // CreateAllocation calls by final outcome (BD-060, gdi_admission.h).
                                            // No escape struct changed.
                                            // 198 (INF 0.7.198.2, on 197): a system sleep or shutdown
                                            // ends the GPU DWM interop session, so a clean restart no
                                            // longer reads as a dead boot (BD-059): \Callback\PowerState
                                            // and the adapter's D3 for a system action unmark, S0 marks
                                            // again; RUN_INTEROP ends 3 system-power and 4 adapter-d3,
                                            // flags POWER_CALLBACK and DOWN. No struct changed.
                                            // 197: the DPM thermal cap's re-entry steps a hot step after the
                                            // last cap change, not at every crossing of 87 C, and RUN_DPM_TUNE
                                            // ABI 2 (152 bytes, ABI 1 still taken) sets the hot step and an
                                            // optional soft release below 87 C at run time (BD-055).
                                            // CalibrateGpuClock answers the SMUIO TSC at 100 MHz
                                            // (the clock of the GPU's own timestamps) instead of
                                            // the QPC at 10 MHz (BD-056).
                                            // 196: a held UMD or Present submission waits on the gfx
                                            // retirement event instead of sleeping 1 ms at a time, and the
                                            // guard log reports the held time in microseconds from QPC with
                                            // cumulative counters in the wddm profile summary (wddm.c
                                            // WddmHoldBegin/Wait/Report, gfx.c GfxRetireSignal). No escape
                                            // struct and no journal record layout changed; the constant moves
                                            // because packagecheck VRS010 matches it against the INF revision.
                                            // 193: hang instrumentation for the 147/208/209/245 VM-fault
                                            // class - UTCL2 faults logged once a second with the latched GCVM
                                            // status, a CP/GRBM/GCVM snapshot at each HARDWARE FENCE TIMEOUT,
                                            // and process/thread/context identity in the paging journal
                                            // (ih_fault.h, paging_identity.h, journal version 2). No escape
                                            // struct and no journal record layout changed; the constant moves
                                            // because packagecheck VRS010 matches it against the INF revision.
                                            // 192: O(1) object index and allocation serials instead of
                                            // adapter-list scans on the close, bind and Present paths
                                            // (object_index.h, wddm.c); 191: FP16 (A16B16G16R16F) swap-chain
                                            // buffers admitted as composed LB7A surfaces at 8 bytes a pixel
                                            // (surface_format.h, gdi_private.h); 190: DDI interface version 0xE003
                                            // (WDDM 2.9; 3.x makes VidMm refuse the VRAM-only CpuVisible CDD
                                            // shadow); 189: target modes name their wire format (8-bit RGB),
                                            // which interface 2.2+ needs (display.c OfferTargetMode); 185: DPM
                                            // governor thresholds and a runtime clock floor (dpm.c)

#define BC250_ESCAPE_STATUS_DONE 0u
#define BC250_ESCAPE_STATUS_UNKNOWN_COMMAND 1u
#define BC250_ESCAPE_STATUS_REFUSED 2u      // NtStatus says why: gate closed, offset not in the table
#define BC250_ESCAPE_STATUS_NOT_ADMIN 3u

#define BC250_ESCAPE_FLAG_MMIO_MAPPED 1u
#define BC250_ESCAPE_FLAG_MMIO_WRITE 2u
#define BC250_ESCAPE_FLAG_VRAM 4u           // the EnableVram gate was open at start and the carve-out was identified
#define BC250_ESCAPE_FLAG_VRAM_WRITE 8u
#define BC250_ESCAPE_FLAG_GART 16u          // the EnableGart gate was open at start
#define BC250_ESCAPE_FLAG_PSP 32u           // the EnablePsp gate was open at start
#define BC250_ESCAPE_FLAG_GFX 64u           // the EnableGfx gate was open at start
#define BC250_ESCAPE_FLAG_IH 128u           // the EnableIh gate was open at start
#define BC250_ESCAPE_FLAG_FULL_WDDM 256u    // the EnableFullWddm gate was open in DriverEntry: the full table is live
#define BC250_ESCAPE_FLAG_DCN_WRITE 512u    // the EnableDcnWrite gate was open at start (needs EnableMmio too)

// The two independent ways to the same VRAM byte (vram.c).
#define BC250_VRAM_PATH_PHYSICAL 0u         // system physical address of the carve-out: GCMC_VM_FB_OFFSET << 24
#define BC250_VRAM_PATH_BAR0 1u             // the PCI aperture, which shows the first BAR0-length bytes of VRAM

typedef struct _BC250_ESCAPE {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long LastStage;                // out: BC250_STAGE
    unsigned long Width, Height, Pitch, ColorFormat;    // out: the firmware mode the driver runs on
    unsigned long Presents;                 // out: presents since start
    unsigned long RegOffset, RegValue;      // register commands
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    // out, GET_INFO only (0.7.22, the overlay's live panel): Reserved[0] is BC250_WDDM.Blits, Reserved[1] is
    // .Flips (wddm.c's WddmCounters) - both 0 outside FullWddm, and both 0 for every other command, same as
    // before this comment. Reusing the two words already here instead of growing the struct is why this did
    // not need BC250_KMD_VERSION to move.
    unsigned long Reserved[2];
} BC250_ESCAPE;

// Fixed-width ABI shared by the native CLI/DLL and monitor. No raw message IDs.
// READ: HardwareAccess=0, NoAdapterSynchronization=1; the embedded SMU owner
// serializes transactions and joins stop before BAR unmap. SET: HardwareAccess=1,
// NoAdapterSynchronization=0 for Level Two synchronization; administrator only.
#define BC250_CLOCK_ABI 1u
#define BC250_CLOCK_OP_READ 0u
#define BC250_CLOCK_OP_SET 1u
typedef struct _BC250_ESCAPE_CLOCK {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, RequestedMHz, RequestedMv;
    unsigned long ObservedMHz, ObservedVid;
    long TemperatureMc;
    unsigned long InitialMHz, InitialVid, ExpectedVid, VoltageStaged, Ready;
    unsigned long Reserved[3];
} BC250_ESCAPE_CLOCK;

// Adapter-owned software snapshot; READ must not idle GPU scheduling or read BARs.
// CONFIRM names the exact generation and visibility epoch observed by the client.
#define BC250_START_HEALTH_ABI 1u
#define BC250_START_HEALTH_READ 0u
#define BC250_START_HEALTH_CONFIRM 1u
#define BC250_START_HEALTH_FULL 1u
#define BC250_START_HEALTH_READY 2u
#define BC250_START_HEALTH_VISIBLE 4u
#define BC250_START_HEALTH_CONFIRMED 8u
#define BC250_START_HEALTH_REQUIRED 7u
#define BC250_START_HEALTH_MIN_MS 60000ull
#define BC250_START_HEALTH_FRESH_MS 15000ull
typedef struct _BC250_ESCAPE_START_HEALTH {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long long Generation, Epoch, Completed, LastCompletionAgeMs, ReadyAgeMs;
    unsigned long long ExpectedGeneration, ExpectedEpoch;
    unsigned long Reserved[2];
} BC250_ESCAPE_START_HEALTH; // 96 bytes on Windows, ABI 1

// CU mode (driver/kmd/cumode.c, docs/design/cu-mode.md). Adapter-owned software snapshot taken at the
// end of the start's GFX bring-up: no BAR access, so both operations take NoAdapterSynchronization=1
// and every other D3DDDI_ESCAPEFLAGS bit zero. READ is open to every caller. CONFIRM needs an
// administrator, the Generation a READ of this start returned, and a start-health READY adapter; it
// clears the pending mark of a 40 CU start, which a later start would otherwise treat as a crash.
// Registers are the values read back after the stage, per shader array (index se * 2 + sh).
// Reason is enum bc250_cu_reason (driver/shim/include/bc250_cu_mode.h).
#define BC250_CU_MODE_ABI 1u
#define BC250_CU_MODE_OP_READ 0u
#define BC250_CU_MODE_OP_CONFIRM 1u
#define BC250_CU_MODE_FLAG_VALID 1u          // the stage ran this start; the caps follow ActiveWgps
#define BC250_CU_MODE_FLAG_PENDING 2u        // 40 applied, not confirmed: a restart now falls back to 24
#define BC250_CU_MODE_FLAG_CONFIRMED 4u      // 40 applied and confirmed (this start or an earlier one)
#define BC250_CU_MODE_FLAG_STOCK_RECORD 8u   // stock came from this boot's record: an earlier start wrote
#define BC250_CU_MODE_FLAG_CONSISTENT 16u    // CC and SPI name the same WGPs on every shader array
#define BC250_CU_MODE_FLAG_WROTE 32u         // this start wrote CC/SPI (the stock of a cold boot needs none)
#define BC250_CU_MODE_SA_COUNT 4u
typedef struct _BC250_ESCAPE_CU_MODE {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long Requested;                // CuMode as read at start, 0 when absent
    unsigned long Applied;                  // 24 or 40, 0 when unknown (not run, restore failed)
    unsigned long Reason, ActiveCus, DisableMask, PciId;
    unsigned long RlcPgCntl, RlcAonWgpMask;
    unsigned long StockCc[BC250_CU_MODE_SA_COUNT], StockSpi[BC250_CU_MODE_SA_COUNT];
    unsigned long Cc[BC250_CU_MODE_SA_COUNT], User[BC250_CU_MODE_SA_COUNT], Spi[BC250_CU_MODE_SA_COUNT];
    unsigned long ActiveWgps[BC250_CU_MODE_SA_COUNT];
    unsigned long long Generation;          // start-health generation of the start this describes
    unsigned long long ExpectedGeneration;  // in, CONFIRM
    unsigned long Reserved[2];
} BC250_ESCAPE_CU_MODE; // 184 bytes on Windows, ABI 1

// DPM (driver/kmd/dpm.c, docs/design/dpm.md). Adapter-owned software snapshot the governor publishes
// every tick: no BAR access, no SMU message, so both operations take NoAdapterSynchronization=1 and
// every other D3DDDI_ESCAPEFLAGS bit zero. READ is open to every caller. CONFIRM needs an
// administrator, the Generation a READ of this start returned, and a start-health READY adapter; it
// clears the pending mark of a DPM start, which a later start would otherwise treat as a crash.
// Mode is BC250_DPM_MODE_*, Reason enum bc250_dpm_reason, Throttle enum bc250_dpm_throttle
// (driver/shim/include/bc250_dpm.h). CurrentMHz/CurrentMv are the level the governor committed;
// ObservedMHz/ObservedVid the SMU's readback, at most a second old (FLAG_CLOCK). Since 0.7.177 BusyPermille is
// the share of GRBM_STATUS.GUI_ACTIVE samples when FLAG_HW_BUSY is set, else the GFX ring's submit-to-fence share;
// SubmitBusyPermille is the latter always, SdmaBusyPermille the share of SDMA0 not-idle samples (the paging node).
// Both were Reserved (zero) in 0.7.175-176: a caller still sends them as zero, so the ABI stays 1.
// Throttle 8 (thermal-warm, a raise refused from 85 C, from 87 C since 0.7.204) is new in 0.7.200, throttle 9 (thermal-ramp, a raise cut to
// one level or held from 70 C) in 0.7.203; the layout and the ABI stay. A tool built before them shows the number
// it does not know as "?". Since 0.7.205 CurrentMHz, CapMHz, TargetMHz and ObservedMHz may read 900 or 800 MHz,
// the clock table's two thermal-only points below the lab floor; WantMHz (the load's demand) never does, and
// MaxMHz stays 1000..2000. Still no level index on the wire, so the layout and the ABI are unchanged.
// ABI 2 (0.7.207, the idle state) appends the state's setting and counters: IdleMHz (the point in force, 0 when
// the state is off for this start), IdleHoldMs and IdleBusyPermille (the window the GPU must be quiet for and
// the busy share it still admits), IdleEntries, IdleExits, IdleRefusals and IdleMs (time at the point).
// FLAG_IDLE says the clock is at the idle point now, and throttle 10 (idle) names it; both reach an ABI 1
// caller too, which shows the flag as a number it does not know. CurrentMHz, CapMHz, TargetMHz and ObservedMHz
// may now read 500 MHz. The driver takes both sizes: AbiVersion 1 with the first BC250_DPM_ABI1_SIZE bytes (the
// 0.7.205 layout, unchanged) and AbiVersion 2 with all 192. A size that is not its AbiVersion's is refused
// before any state is read. A driver before 0.7.207 fails the 192-byte escape itself with
// STATUS_INVALID_PARAMETER: a tool asks with ABI 2 and repeats with ABI 1 on that answer.
#define BC250_DPM_ABI 2u
#define BC250_DPM_ABI_1 1u
#define BC250_DPM_ABI1_SIZE 160u             // the ABI 1 prefix of BC250_ESCAPE_DPM
#define BC250_DPM_OP_READ 0u
#define BC250_DPM_OP_CONFIRM 1u
#define BC250_DPM_FLAG_RUNNING 1u            // the governor thread runs (fixed-lab too: it samples and logs)
#define BC250_DPM_FLAG_GOVERNING 2u          // mode DPM and not given up: it changes the clock
#define BC250_DPM_FLAG_PENDING 4u            // DPM, not confirmed: a restart now falls back to fixed-lab
#define BC250_DPM_FLAG_CONFIRMED 8u          // DPM, confirmed (this start or an earlier one)
#define BC250_DPM_FLAG_PAUSED 16u            // a power transition holds the governor
#define BC250_DPM_FLAG_STABLE 32u            // SetStablePowerState(TRUE): pinned to the floor
#define BC250_DPM_FLAG_SESSION 64u           // DpmSession is on disk: this start is above the floor now or was lately
#define BC250_DPM_FLAG_TEMPERATURE 128u      // TemperatureMc is this tick's reading
#define BC250_DPM_FLAG_CLOCK 256u            // ObservedMHz/ObservedVid read back within the last second
#define BC250_DPM_FLAG_HW_BUSY 512u          // BusyPermille and SdmaBusyPermille come from this tick's hardware samples
#define BC250_DPM_FLAG_IDLE 1024u            // the governor holds the idle point now (0.7.207)
typedef struct _BC250_ESCAPE_DPM {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long Mode, Requested, Reason, Throttle;
    unsigned long MaxMHz, CapMHz, TargetMHz, WantMHz;   // setting, thermal cap, last applied, load demand
    unsigned long CurrentMHz, CurrentMv, ObservedMHz, ObservedVid;
    long TemperatureMc;
    unsigned long BusyPermille, BusyAvgPermille;         // last tick, exponential average
    unsigned long Raises, Lowers, ThermalEvents, Errors, Resyncs;
    unsigned long long Ticks;
    unsigned long long BusyTime100ns;                    // GFX ring busy since the governor started
    unsigned long long UptimeMs;                         // since the governor started
    unsigned long long Generation;          // start-health generation of the start this describes
    unsigned long long ExpectedGeneration;  // in, CONFIRM
    unsigned long SubmitBusyPermille;       // out; in: zero
    unsigned long SdmaBusyPermille;         // out; in: zero
    // ABI 2 from here (BC250_DPM_ABI1_SIZE bytes above). All out.
    unsigned long IdleMHz;                  // the idle point in force, 0 when the state is off for this start
    unsigned long IdleHoldMs, IdleBusyPermille;      // DpmIdleHoldMs and DpmIdleBusyPermille in force
    unsigned long IdleEntries, IdleExits, IdleRefusals;
    unsigned long long IdleMs;              // time the governor held the idle point
} BC250_ESCAPE_DPM; // 192 bytes on Windows, ABI 2 (the first 160 are ABI 1)

// DPM runtime tuning (0.7.185.1; driver/kmd/dpm.c, docs/design/dpm.md "Runtime tuning"). The governor's four
// thresholds (struct bc250_dpm_tune) and a runtime floor, for A/B experiments on a running DPM start. Software state
// only: the escape stores the values under the DPM state's locks and the governor thread takes them at its next tick
// (25 ms); no BAR access, no SMU message from the escape. So every operation takes NoAdapterSynchronization=1 and every
// other D3DDDI_ESCAPEFLAGS bit zero, as RUN_DPM. READ is open to every caller. THRESHOLDS, FLOOR and RESET need an
// administrator and ExpectedGeneration equal to the Generation a READ of this start returned (STATUS_RETRY otherwise);
// THRESHOLDS and FLOOR also need a running DPM start (STATUS_INVALID_DEVICE_STATE: fixed-lab, governor stopped or gave
// up). Nothing is persisted: every device start begins with the defaults. A refused write leaves the values as they
// were and names the reason in Error (enum bc250_dpm_tune_error, driver/shim/include/bc250_dpm.h): ranges, the order
// down < target < up, invariant 1 (a one-step lowering never lands at or above up), invariant 2 (a raise never lands
// below down), the hold, the floor. FloorMHz in: a clock of the table from 1000 MHz up to the start's ceiling (MaxMHz),
// 0 or 1000 for no runtime floor; out: 0 when there is none. The thermal-only points below 1000 MHz (0.7.205) are the
// thermal cap's alone, so 800 and 900 are refused here with error 6 (floor). Every accepted change is logged in the driver log with its old and new
// values. Serial counts the changes since the driver loaded; Applied is the serial the governor thread runs with.
// The 160-byte RUN_DPM structure and BC250_DPM_ABI are unchanged.
// ABI 2 (0.7.197.1, BD-055) appends the thermal cap's timing: the hot step, the soft-release delta below HOT (0: off)
// and the soft-release step, with their defaults, and the THERMAL operation that sets them (error 7, thermal, when
// one is outside its range). The driver takes both sizes: AbiVersion 1 with the first 120 bytes (the ABI 1 layout,
// unchanged; its THRESHOLDS keeps the stored thermal timing, its RESET resets it too) and AbiVersion 2 with all 152.
// A size that does not match its AbiVersion is refused in the reply (NtStatus STATUS_INVALID_PARAMETER) before any
// state is read. A driver before 0.7.197 fails the 152-byte escape itself with STATUS_INVALID_PARAMETER: a tool asks
// with ABI 2 and repeats with ABI 1 on that answer.
#define BC250_DPM_TUNE_ABI 2u
#define BC250_DPM_TUNE_ABI_1 1u
#define BC250_DPM_TUNE_ABI1_SIZE 120u        // the ABI 1 prefix of BC250_ESCAPE_DPM_TUNE
#define BC250_DPM_TUNE_OP_READ 0u
#define BC250_DPM_TUNE_OP_THRESHOLDS 1u      // in: UpPermille, TargetPermille, DownPermille, DownHoldMs
#define BC250_DPM_TUNE_OP_FLOOR 2u           // in: FloorMHz
#define BC250_DPM_TUNE_OP_RESET 3u           // thresholds, floor and (ABI 2 and ABI 1 alike) thermal timing to defaults
#define BC250_DPM_TUNE_OP_THERMAL 4u         // ABI 2 only; in: HotStepMs, SoftReleaseDeltaMc, SoftReleaseStepMs
#define BC250_DPM_TUNE_FLAG_GOVERNING 1u     // a DPM start's governor thread runs and has not given up: writes are taken
#define BC250_DPM_TUNE_FLAG_THRESHOLDS 2u    // the thresholds were set at run time (else the defaults)
#define BC250_DPM_TUNE_FLAG_FLOOR 4u         // a runtime floor is set (else none)
#define BC250_DPM_TUNE_FLAG_APPLIED 8u       // the governor thread runs with the values below (Applied == Serial)
#define BC250_DPM_TUNE_FLAG_THERMAL 16u      // ABI 2: the thermal timing was set at run time (else the defaults)
typedef struct _BC250_ESCAPE_DPM_TUNE {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long UpPermille, TargetPermille, DownPermille, DownHoldMs;      // in: THRESHOLDS; out: in force
    unsigned long FloorMHz;                 // in: FLOOR; out: in force, 0 for none
    unsigned long Error;                    // out: enum bc250_dpm_tune_error of a refused write, else 0
    unsigned long MaxMHz;                   // out: the start's ceiling (DpmMaxMHz), the highest floor admitted
    unsigned long Mode;                     // out: BC250_DPM_MODE_* of this start
    unsigned long DefaultUpPermille, DefaultTargetPermille, DefaultDownPermille, DefaultDownHoldMs;    // out
    unsigned long Serial, Applied;          // out
    unsigned long long FloorTicks;          // out: governor ticks in which the floor lifted the clock above the load's want
    unsigned long long Generation;          // out: start-health generation of the start this describes
    unsigned long long ExpectedGeneration;  // in: THRESHOLDS, FLOOR, RESET
    unsigned long Reserved[2];              // zero in, zero out
    // ABI 2 from here (BC250_DPM_TUNE_ABI1_SIZE bytes above).
    unsigned long HotStepMs, SoftReleaseDeltaMc, SoftReleaseStepMs;                  // in: THERMAL; out: in force
    unsigned long DefaultHotStepMs, DefaultSoftReleaseDeltaMc, DefaultSoftReleaseStepMs;    // out
    unsigned long Reserved2[2];             // zero in, zero out
} BC250_ESCAPE_DPM_TUNE; // 152 bytes on Windows, ABI 2 (the first 120 are ABI 1)

// GPU DWM interop switches (driver/kmd/interop.c, docs/design/gpu-dwm-interop-switches.md). Adapter-owned
// software snapshot decided once per start (both switches are start-latched): no BAR access, so READ takes
// NoAdapterSynchronization=1 and every other D3DDDI_ESCAPEFLAGS bit zero, and is open to every caller. There is no
// write operation: the operator changes EnableGpuPresentBlit/EnableCddDwmInterop in the registry and restarts.
// Requested/Effective are BC250_INTEROP_SWITCH_* bits, Reason and ClosedReason enum bc250_interop_reason
// (driver/kmd/interop_policy.h). BlitSetting/CddSetting are the raw values read at start, 0 when the matching
// *_ABSENT or *_UNREADABLE flag is set. SessionBootId is the InteropSession value the start found (0 when none),
// BootId this boot's KUSER_SHARED_DATA.BootId. PreviousEnd is InteropLastEnd as the start found it, LastEnd this
// start's last unmark: BC250_INTEROP_END_*. Reserved: zero in, zero out.
#define BC250_INTEROP_ABI 1u
#define BC250_INTEROP_OP_READ 0u
#define BC250_INTEROP_SWITCH_BLIT 1u             // EnableGpuPresentBlit: a Blt present is one GPU copy
#define BC250_INTEROP_SWITCH_CDD 2u              // EnableCddDwmInterop: DRIVERCAPS DriverSupportsCddDwmInterop
#define BC250_INTEROP_FLAG_VALID 1u              // a full WDDM start decided; else Reason is not-run
#define BC250_INTEROP_FLAG_SESSION 2u            // InteropSession is on disk now: a device of this start uses the path
#define BC250_INTEROP_FLAG_UNCLEAN 4u            // the start found a marker of an earlier boot
#define BC250_INTEROP_FLAG_STALE 8u              // the start found a marker of this boot (a restart without unmark)
#define BC250_INTEROP_FLAG_CLOSED_BY_DRIVER 16u  // the switches are 0 because the driver wrote them so (ClosedReason)
#define BC250_INTEROP_FLAG_PERSISTED 32u         // this start wrote the durable close and it reached the disk
#define BC250_INTEROP_FLAG_PERSIST_FAILED 64u    // this start's durable close failed: the marker stays for the next one
#define BC250_INTEROP_FLAG_BLIT_ABSENT 128u      // EnableGpuPresentBlit absent: default 1
#define BC250_INTEROP_FLAG_CDD_ABSENT 256u       // EnableCddDwmInterop absent: default 1
#define BC250_INTEROP_FLAG_BLIT_UNREADABLE 512u  // not a REG_DWORD, or the read failed
#define BC250_INTEROP_FLAG_CDD_UNREADABLE 1024u
#define BC250_INTEROP_FLAG_POWER_CALLBACK 2048u  // the \Callback\PowerState registration is in place (0.7.198)
#define BC250_INTEROP_FLAG_DOWN 4096u            // a system power transition began and has not come back (0.7.198)
#define BC250_INTEROP_END_NONE 0u
#define BC250_INTEROP_END_STOP 1u                // the device stopped with the session marked
#define BC250_INTEROP_END_USERS 2u               // the last device that used the path was destroyed (DWM exit)
#define BC250_INTEROP_END_SYSTEM_POWER 3u        // a system sleep or shutdown began: \Callback\PowerState (0.7.198)
#define BC250_INTEROP_END_ADAPTER_D3 4u          // the adapter went to D3 for a system sleep or shutdown (0.7.198)
typedef struct _BC250_ESCAPE_INTEROP {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long Requested, Effective, Reason, ClosedReason;
    unsigned long BlitSetting, CddSetting;
    unsigned long BootId, SessionBootId;
    unsigned long Users, Marks, Unmarks, MarkFailures;   // devices using the path now; marker writes/deletes/failures
    unsigned long PreviousEnd, LastEnd;
    unsigned long long Generation;          // start-health generation of the start this describes
    unsigned long Reserved[2];
} BC250_ESCAPE_INTEROP; // 104 bytes on Windows, ABI 1

// Read-only diagnostics, not an atomic hardware snapshot. Require administrator,
// HardwareAccess=1 and every other D3DDDI_ESCAPEFLAGS bit zero: Level Two keeps
// BAR mapping alive, but idles GPU scheduling and therefore perturbs the workload.
// Sequence brackets software surface publication only; raster/flip latch can move.
// ValidMask bits follow register field order below (0..21). Timing bits 11..21
// are all set only when the entire shared timing tuple was read successfully.
#define BC250_DCN_OBSERVE_ABI 1u
#define BC250_DCN_OBSERVE_REG_COUNT 22u
#define BC250_DCN_OBSERVE_VALID_ALL ((1u << BC250_DCN_OBSERVE_REG_COUNT) - 1u)
#define BC250_DCN_OBSERVE_TIMING_MASK (BC250_DCN_OBSERVE_VALID_ALL & ~((1u << 11) - 1u))
typedef struct _BC250_ESCAPE_DCN_OBSERVE {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, RegisterCount, ValidMask;
    unsigned long PrimaryAddressLow, PrimaryAddressHigh;
    unsigned long EarliestInUseLow, EarliestInUseHigh;
    unsigned long FlipControl, SurfacePitch, OtgStatusPosition, OtgGlobalControl0;
    unsigned long OtgBlankControl, OtgDoubleBufferControl, OtgFrameCount;
    unsigned long TimingControl, TimingHTotal, TimingVTotal, TimingHBlank, TimingVBlank;
    unsigned long TimingPixelControl, TimingPhase, TimingModulo, TimingInterlace;
    unsigned long TimingVTotalControl, TimingReference;
    unsigned long SequenceBefore, SequenceAfter;
} BC250_ESCAPE_DCN_OBSERVE; // 128 bytes on Windows, ABI 1

// Memory commands. The first four fields are those of BC250_ESCAPE, so the driver can tell the two apart by
// Command after checking Magic.
typedef struct _BC250_ESCAPE_MEMORY {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Path;                     // in: BC250_VRAM_PATH_*
    unsigned long Value;                    // in/out: one 32-bit word of VRAM
    unsigned long long Offset;              // in: byte offset into VRAM, multiple of 4
    unsigned long long FramebufferPhysical; // out: where the firmware says its framebuffer is
    unsigned long long FramebufferLength;
    unsigned long long Bar0Physical, Bar0Length;
    unsigned long long VramPhysical, VramLength;    // out: from GCMC_VM_FB_OFFSET and FB_LOCATION_BASE/TOP
    unsigned long long VramMcBase;          // out: GPU physical (MC) address of VRAM byte 0
    unsigned long long TestOffset, TestLength;      // out: the only window VRAM_WRITE accepts
} BC250_ESCAPE_MEMORY;

// One register write of a sequence, as issued or as planned.
typedef struct _BC250_ESCAPE_WRITE {
    unsigned long Offset, Value;
} BC250_ESCAPE_WRITE;

// The GART command (gart.c). PLAN executes no write; ENABLE and RESTORE do.
#define BC250_GART_OP_PLAN 0u
#define BC250_GART_OP_ENABLE 1u
#define BC250_GART_OP_RESTORE 2u
#define BC250_GART_STATE_ENABLED 1u         // the sequence has been run and not restored
#define BC250_GART_STATE_SNAPSHOT 2u        // the firmware's register state is held for RESTORE
#define BC250_GART_MAX_WRITES 512

typedef struct _BC250_ESCAPE_GART {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_GART
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Op;                       // in: BC250_GART_OP_*
    long Result;                            // out: return code of the imported sequence (0, or -62 for a poll timeout)
    unsigned long FaultOffset;              // out: the first register the driver's table refused, 0 if none
    unsigned long State;                    // out: BC250_GART_STATE_*
    unsigned long WriteCount;               // out: writes the sequence issued; the first BC250_GART_MAX_WRITES are listed
    unsigned long Reserved;
    unsigned long long TablePhysical, TableMc, ScratchMc, DummyPhysical;
    BC250_ESCAPE_WRITE Writes[BC250_GART_MAX_WRITES];
} BC250_ESCAPE_GART;

// The PSP command (psp.c). PLAN executes no write and touches no VRAM; LOAD and UNLOAD do.
#define BC250_PSP_OP_PLAN 0u
#define BC250_PSP_OP_LOAD 1u
#define BC250_PSP_OP_UNLOAD 2u
#define BC250_PSP_STATE_RING 1u             // the PSP has been told about our ring
#define BC250_PSP_STATE_TMR 2u              // the PSP has been told about our TMR
#define BC250_PSP_STATE_GART 4u             // the GART sequence is enabled (LOAD needs it)
#define BC250_PSP_MAX_WRITES 32
#define BC250_PSP_MAX_COMMANDS 16

typedef struct _BC250_ESCAPE_PSP_COMMAND {
    unsigned long CommandId;                // GFX_CMD_ID_*: 5 SETUP_TMR, 6 LOAD_IP_FW
    unsigned long FirmwareType;             // GFX_FW_TYPE_*, 0 for SETUP_TMR
    unsigned long Size;                     // bytes of the image, or of the TMR
    long Result;                            // return code of the submission: 0, -62 no fence, -22 refused by the PSP
    unsigned long PspStatus;                // resp.status as the PSP wrote it
    unsigned long Microseconds;             // submission to fence
    unsigned long long McAddress;           // where the image (or the TMR) is
    unsigned long long TmrAddress;          // resp.fw_addr: where the PSP says it put the image
} BC250_ESCAPE_PSP_COMMAND;

typedef struct _BC250_ESCAPE_PSP {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_PSP
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Op;                       // in: BC250_PSP_OP_*
    long Result;                            // out: first failing return code of the sequence, 0 if none; with a file
                                            //      error (NtStatus), the index of the firmware file
    unsigned long FaultOffset;              // out: the first register the driver's table refused, 0 if none
    unsigned long State;                    // out: BC250_PSP_STATE_*
    unsigned long WriteCount;               // out: register writes issued (or planned)
    unsigned long CommandCount;             // out: commands of the sequence
    unsigned long CommandsDone;             // out: commands submitted, the failing one included
    unsigned long StagingUsed;              // out: bytes of the staging area in use
    unsigned long long RingMc, CommandMc, FenceMc, TmrMc, TmrPhysical, StagingMc;
    BC250_ESCAPE_WRITE Writes[BC250_PSP_MAX_WRITES];
    BC250_ESCAPE_PSP_COMMAND Commands[BC250_PSP_MAX_COMMANDS];
} BC250_ESCAPE_PSP;

// ---- BC250_ESCAPE_RUN_GFX (gfx.c): RLC, CP, KIQ, queues, ring tests, SDMA -------------------------------------------------
#define BC250_GFX_OP_PLAN 0u                // stages 1..LastStage against the real registers, no register or doorbell write
#define BC250_GFX_OP_RUN 1u                 // stages (next not yet done)..LastStage
#define BC250_GFX_OP_FINI 2u                // halt SDMA, CP and MEC, give the memory back
#define BC250_GFX_OP_STATE 3u               // nothing but the out fields
#define BC250_GFX_MAX_WRITES 1536
#define BC250_GFX_MAX_DOORBELLS 32
#define BC250_GFX_MAX_STAGES 16

typedef struct _BC250_ESCAPE_DOORBELL {
    unsigned long Index;                    // dword index into the doorbell BAR, amdgpu's doorbell index
    unsigned long AfterWrite;               // how many register writes of the sequence came before it
    unsigned long long Value;
} BC250_ESCAPE_DOORBELL;

typedef struct _BC250_ESCAPE_GFX_STAGE {
    unsigned long Stage;                    // BC250_GFX_STAGE_* (gfx.c names them in its table)
    long Result;                            // the shim function's return code
    unsigned long FirstWrite;               // index of the stage's first register write in Writes
    unsigned long Microseconds;
} BC250_ESCAPE_GFX_STAGE;

typedef struct _BC250_ESCAPE_GFX {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_GFX
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Op;                       // in: BC250_GFX_OP_*
    unsigned long LastStage;                // in: PLAN and RUN stop after this stage
    long Result;                            // out: first failing return code, 0 if none
    unsigned long FailedStage;              // out: the stage that returned it, 0 if none
    unsigned long FaultOffset;              // out: the first register the driver's table refused (0xD0000000 | index for a
                                            //      refused doorbell), 0 if none
    unsigned long StagesDone;               // out: the last stage that has been run on the hardware since the device started
    unsigned long WriteCount;               // out: register writes issued (or planned)
    unsigned long DoorbellCount;
    unsigned long StageCount;
    unsigned long VramBytes, GttBytes;      // out: GPU-visible memory held by the sequence
    BC250_ESCAPE_GFX_STAGE Stages[BC250_GFX_MAX_STAGES];
    BC250_ESCAPE_DOORBELL Doorbells[BC250_GFX_MAX_DOORBELLS];
    BC250_ESCAPE_WRITE Writes[BC250_GFX_MAX_WRITES];
} BC250_ESCAPE_GFX;

// ---- BC250_ESCAPE_RUN_IH (ih.c): the interrupt controller's ring, interrupt and DPC counts ---------------------------------
#define BC250_IH_OP_PLAN 0u                 // the init sequence against the real registers, no write
#define BC250_IH_OP_INIT 1u                 // ring set up and enabled
#define BC250_IH_OP_FINI 2u                 // ring disabled, memory given back
#define BC250_IH_OP_STATE 3u                // nothing but the out fields; answers with the gate closed as well
#define BC250_IH_MAX_WRITES 64
#define BC250_IH_MAX_KINDS 16
#define BC250_IH_MAX_LAST 32

// One interrupt vector as the DPC decoded it (amdgpu_iv_entry).
typedef struct _BC250_ESCAPE_IV {
    unsigned long ClientId, SourceId, RingId, VmId, VmIdSrc, Pasid;
    unsigned long SrcData[4];
    unsigned long long Timestamp;
} BC250_ESCAPE_IV;

typedef struct _BC250_ESCAPE_IV_KIND {
    unsigned long ClientId, SourceId, Count;
} BC250_ESCAPE_IV_KIND;

typedef struct _BC250_ESCAPE_IH {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_IH
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Op;                       // in: BC250_IH_OP_*
    long Result;                            // out: the shim's return code
    unsigned long FaultOffset;              // out: first refused register access of the sequence
    unsigned long WriteCount;               // out: register writes issued (or planned)
    unsigned long InterruptIsMessage;       // out: what Windows assigned: 1 a message (MSI), 0 the line
    unsigned long InterruptVector;
    unsigned long InterruptCount;           // out: calls of the interrupt routine since the device started, ours or not
    unsigned long LastMessageNumber;
    unsigned long Active;                   // out: the ring is enabled
    unsigned long OurInterrupts;            // out: calls taken as ours
    unsigned long DpcCount;
    unsigned long EntryCount;               // out: vectors consumed from the ring
    unsigned long OverflowCount;
    unsigned long Rptr, Wptr;
    // docs/design/vsync-interrupt-route.md: IH_STATUS, read best-effort (0 with EnableIh closed, like every other
    // field above). Rptr/Wptr only show an entry already written into the ring; these three are IH's own view of
    // whether anything is incoming or queued, from the other end of the same register.
    unsigned long Idle;                     // IH_STATUS.IDLE: the whole block (ring, input side, write-back) is idle
    unsigned long InputIdle;                // IH_STATUS.INPUT_IDLE: no client (any IP, DCE included) has anything
                                            // pending at IH's own input right now
    unsigned long BifInterruptLine;         // IH_STATUS.BIF_INTERRUPT_LINE: NBIO/BIF's own view of whether the
                                            // interrupt line toward the host is currently asserted
    unsigned long KindCount;
    unsigned long LastCount;
    BC250_ESCAPE_IV_KIND Kinds[BC250_IH_MAX_KINDS];     // how many of each (client, source) pair
    BC250_ESCAPE_IV Last[BC250_IH_MAX_LAST];            // the most recent vectors, oldest first
    BC250_ESCAPE_WRITE Writes[BC250_IH_MAX_WRITES];
} BC250_ESCAPE_IH;

// ---- BC250_ESCAPE_RUN_DCN (dcn.c): a read-only dump of the DCN 2.0.1 ("DMU") display controller's registers --------------
//
// ADR 0011 point 3: proves the offsets and the BAR5 mapping are right under Windows before that ADR's first write.
// HUBPREQ0..3, HUBP0..3, OTG0..1 and DCHUBBUB_CTRL_STATUS (gen_regs.py's DCN_REGISTERS, 75 registers), each a
// name and a value. No write of any kind, and no gate beyond EnableMmio (BAR5 mapped): the driver refuses with
// STATUS_DEVICE_NOT_READY, same as READ_REG, if the mapping is not there.
#define BC250_DCN_REG_COUNT 75              // kept equal to regs.generated.h's BC250_DCN_REG_INFO_COUNT; dcn.c
                                            // asserts it at compile time (gen_regs.py is the one place that counts)
#define BC250_DCN_NAME_LEN 48               // longest today is 44 characters (HUBPREQ0_..._ADDRESS_HIGH) plus the terminator

typedef struct _BC250_ESCAPE_DCN_REG {
    char Name[BC250_DCN_NAME_LEN];          // the mm* name (dcn_2_0_1_offset.h), "mm" dropped
    unsigned long Offset;                   // BAR5 byte offset (tools/regcalc, ip DMU)
    unsigned long Value;
} BC250_ESCAPE_DCN_REG;

typedef struct _BC250_ESCAPE_DCN {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_DCN
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long RegCount;                 // out: entries in Regs[], BC250_DCN_REG_COUNT when Status is DONE
    unsigned long FaultOffset;              // out: should stay 0 - every offset in Regs[] is already on the
                                            //      driver's own generated table; nonzero says the two disagreed
    // Decoded summary, HUBP0 and OTG0 only: the pipe and timing generator the firmware is already scanning out
    // on (evidence/linux/2026-09-22-E21-linux-reference-4/dmupre.txt is the Linux reference to compare against).
    unsigned long long Hubp0Address;        // DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH<<32 | _ADDRESS
    unsigned long Hubp0Pitch;               // DCSURF_SURFACE_PITCH.PITCH field (dcn_2_0_1_sh_mask.h)
    unsigned long Hubp0Cntl;                // DCHUBP_CNTL, raw
    unsigned long Otg0Control;              // OTG_CONTROL, raw
    unsigned long Otg0MasterEnable;         // OTG_CONTROL.OTG_MASTER_EN field
    unsigned long Otg0HTotal, Otg0VTotal;
    unsigned long Otg0VblankIntEnabled;     // OTG_GLOBAL_SYNC_STATUS bit 12 (AMD's own name for this field in
                                            // dcn_2_0_1_sh_mask.h is VUPDATE_NO_LOCK_INT_EN, not "vblank")
    // docs/design/vsync-interrupt-route.md: whether the hardware event VUPDATE_NO_LOCK_INT_EN gates an
    // interrupt on ever actually latches, and whether anything holds the OTG update lock or the vupdate
    // keepout window open across it - the open question a lab run with gart/psp/gfx/ih up (E22 run 004, M98,
    // M99) needs these four for, which run 001/002's display-only dumps (M92, M94) never had a reason to read.
    unsigned long Otg0VupdateEventOccurred; // OTG_GLOBAL_SYNC_STATUS bit 14, VUPDATE_NO_LOCK_EVENT_OCCURRED:
                                            // latched by hardware, independent of whether _INT_EN is set; a 1
                                            // here with the ISR's InterruptCount still 0 would mean the event
                                            // fires but never reaches the IH ring/MSI, a 0 that it never fires.
    unsigned long Otg0VupdateIntStatus;     // OTG_GLOBAL_SYNC_STATUS bit 15, VUPDATE_NO_LOCK_INT_STATUS: never
                                            // read by amdgpu's own source (irq_service_dcn20.c's generic path
                                            // only ever writes this register), so its meaning here is empirical,
                                            // not textual - comparing M92 (INT_EN 0, this bit 0) against M103
                                            // (INT_EN 1, this bit 1) is the only cross-check on record, and it
                                            // moved with the enable bit, not with EVENT_OCCURRED (already 1 in
                                            // both): reads as EVENT_OCCURRED qualified by INT_EN, i.e. "this
                                            // occurrence is actually armed to raise the interrupt", one register,
                                            // same read as the two fields above, no extra MmioDcnRead call
                                            // (docs/design/vsync-interrupt-route.md section 12).
    unsigned long Otg0MasterUpdateLocked;   // OTG_MASTER_UPDATE_LOCK.OTG_MASTER_UPDATE_LOCK field:
                                            // DcnFlipWriteSequence always unlocks (dcn.c), even on its error
                                            // path; 1 here between flips would mean something left it locked
                                            // and the double-buffered registers are not taking latest values.
    unsigned long Hubp0FlipPending;         // DCSURF_FLIP_CONTROL.SURFACE_FLIP_PENDING field: M94 measured this
                                            // clearing within one frame under display-only gates; stuck at 1
                                            // here would mean the same latch does not clear once gart/psp/gfx/ih
                                            // are also up.
    unsigned long Otg0VupdateKeepoutEn;     // OTG_VUPDATE_KEEPOUT.OTG_MASTER_UPDATE_LOCK_VUPDATE_KEEPOUT_EN
                                            // field: the lock/unlock race-protection window (dcn20_optc.c's
                                            // triplebuffer lock/unlock, not an interrupt gate); should read 0
                                            // outside a flip.
    BC250_ESCAPE_DCN_REG Regs[BC250_DCN_REG_COUNT];
} BC250_ESCAPE_DCN;

// ---- BC250_ESCAPE_RUN_DCNFLIP (dcn.c): one gated display flip on HUBP0/OTG0 ------------------------------------------------
//
// ADR 0011 point 3 step 2, E22 step 2, the M87 sequence for HUBP0 only: OTG0_OTG_MASTER_UPDATE_LOCK = 1;
// HUBPREQ0_DCSURF_FLIP_CONTROL = 0; _SURFACE_CONTROL = 0; the new address; OTG0_OTG_MASTER_UPDATE_LOCK = 0;
// OTG0_OTG_TRIGA_MANUAL_TRIG = 1; then a poll of _FLIP_CONTROL bit 0x100 (SURFACE_FLIP_PENDING). Needs EnableMmio
// and EnableDcnWrite (BC250_ESCAPE_FLAG_DCN_WRITE); Fill needs EnableVramWrite as well. Not yet
// SetVidPnSourceAddress and not yet the interrupt - both come with the full table and the IH ring.
#define BC250_DCNFLIP_REASON_LEN 64

typedef struct _BC250_ESCAPE_DCNFLIP {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_DCNFLIP
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long long Physical;            // in: system physical address to flip HUBP0 to; ignored when Restore != 0
    unsigned long Fill;                     // in: nonzero = paint the target surface before the flip (needs EnableVramWrite,
                                            //     refused for the firmware's own address, ignored when Restore != 0)
    unsigned long FillColor;                // in: ARGB8888 fill colour
    unsigned long Restore;                  // in: nonzero = flip back to the firmware's own stored address
    char Reason[BC250_DCNFLIP_REASON_LEN];  // out: why, when Status is REFUSED; empty otherwise
    unsigned long long FirmwareAddress;     // out: the address stored as the firmware's own (device state, ADR 0011)
    unsigned long long AddressBefore, AddressAfter;     // out: HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS[_HIGH]
    unsigned long InUseBefore, InUseAfter;              // out: HUBPREQ0_DCSURF_SURFACE_INUSE
    unsigned long FrameCountBefore, FrameCountAfter;    // out: OTG0_OTG_STATUS_FRAME_COUNT
    unsigned long FlipPendingCleared;       // out: 1 when SURFACE_FLIP_PENDING (0x100) cleared inside the wait
    unsigned long WaitUs;                   // out: microseconds spent polling it
    unsigned long DchubpCntl;               // out: HUBP0_DCHUBP_CNTL, raw, read after the flip
    unsigned long Underflow;                // out: DchubpCntl's HUBP_UNDERFLOW_STATUS field (0x70000000), decoded
} BC250_ESCAPE_DCNFLIP;

// ---- BC250_ESCAPE_RUN_SDMACOPY (gfx.c): the SDMA copy/fill positive control (ADR 0013) --------------------------------------
//
// The one thing this escape asks: run one SDMA linear copy and one constant fill the way BuildPagingBuffer will
// once node 1 (ADR 0013) reaches the WDDM table, without going anywhere near that table, and prove it by reading
// the destination back with the CPU. Needs the same gates the SDMA ring test needs (EnableMmio, EnableVram,
// EnableGart, EnablePsp, EnableGfx, and a bring-up that has reached stage 7 - see BC250_ESCAPE_RUN_FENCE's
// BC250_FENCE_MODE_RING_TEST) plus EnableVramWrite: seeding the source and reading the destination back are both
// raw CPU writes/reads of VRAM, exactly what vram.c's EnableVramWrite gate already exists to gate elsewhere, not
// the bring-up's own buffers that the other gates already cover.
//
// Two 64 KiB VRAM scratch regions, allocated once from the same pool gpumem.c's bring-up buffers come from (the
// top of the carve-out, facts M31/M32) and reused across calls; refused if either one is not inside
// Device->VramPhysical/VramLength (gpumem.c's own allocator cannot hand out anything else, but this escape checks
// it again rather than trust that by construction - the same doubled check vram.c's Access() makes against
// MmGetPhysicalMemoryRanges()). Sequence: the source is seeded by the CPU with a counting pattern
// (BC250_SDMACOPY_PATTERN(offset)); bc250_sdma_copy_test() emits a constant-fill-then-linear-copy pair on SDMA0
// and the same fence bc250_sdma_ring_test() uses, polled the same way (no interrupt: this needs no more of the IH
// ring than the ring test does); the destination is then read back by the CPU and compared byte for byte with
// what the source was seeded with.
#define BC250_SDMACOPY_MAX_BYTES 0x10000u   // one scratch region, 64 KiB
#define BC250_SDMACOPY_DEFAULT_BYTES 4096u  // what Bytes == 0 asks for
#define BC250_SDMACOPY_PATTERN 0x000000A5u  // the SDMA fill's own value; every destination byte should read 0xA5

typedef struct _BC250_ESCAPE_SDMACOPY {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_SDMACOPY or BC250_ESCAPE_RUN_SDMAIB
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Bytes;                    // in: 1..BC250_SDMACOPY_MAX_BYTES, or 0 for BC250_SDMACOPY_DEFAULT_BYTES
    long Result;                            // out: the shim's return code, -62 if the fence never arrived
    unsigned long FaultOffset;
    unsigned long BytesCompared;            // out: bytes actually read back and compared
    unsigned long Matched;                  // out: 1 when every byte read back is what the source was seeded with
    unsigned long FirstMismatchOffset;      // out: byte offset of the first wrong byte, 0 when Matched
    unsigned long FirstMismatchGot, FirstMismatchWant;
    unsigned long LastSeq, LastValue;       // out: the fence value emitted and what the slot held when polling stopped
    unsigned long Microseconds;             // out: seed to read-back compared, all of it
    unsigned long Padding;                  // explicit, so the two 64-bit fields below start where they read
    unsigned long long SrcMc, DstMc;        // out: the two VRAM scratch regions' GPU (MC) addresses
} BC250_ESCAPE_SDMACOPY;

// ---- BC250_ESCAPE_RUN_FBDUMP (dcn.c): a read-only band of the scanned-out surface's pixels ----------------------------
//
// The owner's request (2026-09-22): a screenshot that also works under the full WDDM table, where mon.py's GDI
// capture shows black (facts M84) because CopyFromScreen reads the CDD's surfaces, not what the display controller
// scans out. This reads HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS[_HIGH] and _SURFACE_PITCH fresh every call (the
// same registers BC250_ESCAPE_RUN_DCN already dumps and BC250_ESCAPE_RUN_DCNFLIP already writes) and hands back
// one band of rows; bc250kmd_cli fbdump asks for BC250_FBDUMP_MAX_ROWS at a time until it has the whole surface,
// then writes one BMP. No register write of any kind, no gate beyond EnableMmio (BAR5 mapped) - the same
// condition BC250_ESCAPE_RUN_DCN already answers to.
//
// A present between two calls of the same dump is a torn frame in the tool's own BMP, not a driver bug: locking
// OTG0 for the whole dump would make this a write escape, which it deliberately is not.
#define BC250_FBDUMP_MAX_ROWS 64            // rows returned per call
#define BC250_FBDUMP_ROW_BYTES 8192u        // bytes held per row's slot in Pixels[]; the firmware mode's pitch is
                                            // 7680 (facts M14/M84), so this leaves headroom without the struct
                                            // running to the megabyte the whole surface (1920x1200x4) would take
                                            // in one call; a pitch wider than this is refused, not truncated
#define BC250_FBDUMP_REASON_LEN 64

typedef struct _BC250_ESCAPE_FBDUMP {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_FBDUMP
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Hubp;                     // in/out: which HUBP; only 0 is supported today, anything else refused
    unsigned long FirstRow;                 // in: 0-based row to start the band at
    unsigned long RowCount;                 // in: 1..BC250_FBDUMP_MAX_ROWS; out: rows actually returned (never less
                                            // than asked - a request that does not fit is refused, not shrunk)
    unsigned long Width, Height, Pitch, ColorFormat;    // out: the scanned-out surface's geometry; Pitch is read
                                            // fresh from HUBPREQ0_DCSURF_SURFACE_PITCH every call ((PITCH field +
                                            // 1) pixels, times 4 for A8R8G8B8), Width/Height/ColorFormat are the
                                            // firmware's own POST mode (display.c's Device->Post, facts M14)
    unsigned long Padding;                  // explicit, so Address (8 bytes) starts 8-aligned, like BC250_ESCAPE_SDMACOPY's
    unsigned long long Address;             // out: this band's own physical address (system physical): the
                                            // scanout base this call read from HUBPREQ0_DCSURF_PRIMARY_SURFACE_
                                            // ADDRESS[_HIGH]) plus FirstRow*Pitch, i.e. where Pixels[0] came from
    char Reason[BC250_FBDUMP_REASON_LEN];   // out: why, when Status is REFUSED; empty otherwise
    // out: RowCount rows of Pitch bytes each, packed contiguously (no padding between rows), row-major, the
    // surface's own FirstRow first. Bytes past RowCount*Pitch are left as the driver found them (not necessarily
    // zero) - the caller knows RowCount and Pitch and reads only that many.
    unsigned char Pixels[BC250_FBDUMP_MAX_ROWS * BC250_FBDUMP_ROW_BYTES];
} BC250_ESCAPE_FBDUMP;

// ---- BC250_ESCAPE_RUN_FENCE (gfx.c): fences on one ring, one after the other ----------------------------------------------
#define BC250_FENCE_RING_GFX 0u
#define BC250_FENCE_RING_COMPUTE0 1u        // 1..8: the eight compute rings
#define BC250_FENCE_RING_KIQ 9u
#define BC250_FENCE_RING_SDMA0 10u          // 10, 11: the two SDMA engines (their fence ends in a trap, not an end of pipe)
#define BC250_FENCE_MODE_VALUE 0u           // Interrupt: the control, the value and no interrupt
#define BC250_FENCE_MODE_INTERRUPT 1u
#define BC250_FENCE_MODE_RING_TEST 2u       // SDMA rings only: sdma_v5_0_ring_test_ring(), Count is ignored
#define BC250_FENCE_MODE_DISPATCH 3u        // compute rings only: libdrm's gfx10 memset dispatch (bc250_dispatch.h), Count is the
                                            // number of 64-thread workgroups, 1..16; one fence with the interrupt bit behind it
// The two indirect-buffer modes (ADR 0008 stage C). Gfx ring only, Count 1.
#define BC250_FENCE_MODE_IB 4u              // the driver builds the ring test as an IB in a GTT page of its own and submits it
                                            // through bc250_gfx_submit_ib() at VMID 0, then polls the fence as the modes above
                                            // do. Out: IbAddress, Dwords, IbFetched. Needs no gate beyond EnableGfx.
#define BC250_FENCE_MODE_IB_AT 5u           // the caller's IB: Vmid, RootPhysical, IbAddress, Dwords. Goes through GfxSubmitIb(),
                                            // i.e. the path wddm.c uses, so it needs EnableGpuSubmit and stage 8, takes the one
                                            // submission slot, and is polled with GfxFenceArrived() for BC250_SUBMIT_POLL_US.
#define BC250_FENCE_MAX_COUNT 1000u
#define BC250_FENCE_IB_MAX_DWORDS 0xFFFFFu  // the width of PACKET3_INDIRECT_BUFFER's IB_SIZE field (nvd.h); the driver
                                            // refuses the same bound through AMD's own macro, this one is so that a
                                            // tool need not send a request that cannot be encoded
#define BC250_DISPATCH_FILL 0x22222222u     // what the shader stores: libdrm's own value (shader_test_util.c:441-444)

typedef struct _BC250_ESCAPE_FENCE {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_RUN_FENCE
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Ring;                     // in: BC250_FENCE_RING_*
    unsigned long Count;                    // in: 1..BC250_FENCE_MAX_COUNT
    unsigned long Interrupt;                // in: BC250_FENCE_MODE_*
    long Result;                            // out: the shim's return code, -62 when a value did not arrive in time
    unsigned long FaultOffset;
    unsigned long Completed;                // out: fences whose value was read back
    unsigned long DoorbellCount;
    unsigned long LastSeq, LastValue;       // out: the last value emitted and what the slot held last
    unsigned long Microseconds;             // out: all of them
    unsigned long SlowestMicroseconds;      // out: the slowest single emit-to-value
    long DispatchCheck;                     // out, DISPATCH: bc250_gfx_dispatch_check(), 0 when every dword is as asked
    unsigned long DispatchBadOffset;        // out, DISPATCH: byte offset of the first wrong dword of the destination
    // The two IB modes. Appended, so every field above keeps the offset it had; the driver refuses a buffer shorter
    // than this structure, which is how an older tool is turned away rather than read past its end.
    unsigned long Vmid;                     // in, IB_AT: the VMID the CP fetches the IB through, 0..15 (0 = the GART aperture)
    unsigned long Dwords;                   // in, IB_AT: dwords of the IB, 1..0xFFFFF; out, IB: what the driver built
    unsigned long IbFetched;                // out, IB: 1 when the scratch register took the value the IB writes
    unsigned long Seq;                      // out: the fence sequence number the submission was given
    unsigned long Padding;                  // explicit, so the two 64-bit fields below start where they read
    unsigned long long RootPhysical;        // in, IB_AT: page directory root of that VMID; 0 = leave the VMID's root alone
    unsigned long long IbAddress;           // in, IB_AT: GPU address of the IB; out, IB: where the driver built it
} BC250_ESCAPE_FENCE;

// ---- BC250_ESCAPE_GET_LOG (guard.c): the driver's own log ring -------------------------------------------------------------
//
// The lab machine is headless over SSH: it has no kernel debugger and no DebugView, so the debug print stream
// GuardLog writes to reaches nobody. Since 0.7.1 every line also goes into a ring inside the driver image, and
// this escape reads it back by sequence number. `bc250kmd_cli log` pages through it.
//
// The ring keeps the first BC250_LOG_HEAD lines of a driver load for as long as the driver stays loaded - the
// order of the start-up is the evidence an experiment is run for - and wraps everything after them. Lines lost
// that way are counted, so a gap is always visible rather than silent.
#define BC250_LOG_TEXT 160                  // bytes of text per line, the terminator included
#define BC250_LOG_HEAD_LINES 256            // never overwritten while the driver stays loaded
#define BC250_LOG_RING_LINES 1024           // the whole ring: the head above plus a wrapping tail
#define BC250_LOG_MAX_LINES 64              // lines one escape returns

// As `From` with BC250_ESCAPE_LOG_SUMMARY: start at the first line this summary itself wrote, so that asking for
// a summary does not reprint the whole run. The driver answers the real number in SummaryFrom either way.
// `bc250kmd_cli log summary` does not send it (it prints the whole ring, which is what an evidence file wants);
// `log summary only` does, for a caller that polls (the overlay, BD-054).
//
// BC250_ESCAPE_LOG_SUMMARY is refused (REFUSED, STATUS_INVALID_DEVICE_REQUEST) unless D3DKMT_ESCAPE.Flags has
// HardwareAccess set and NoAdapterSynchronization clear: the summary reads state that a device stop frees, and
// that flag is what makes dxgkrnl serialize the two.
//
// BC250_ESCAPE_GET_LOG and BC250_ESCAPE_GET_PAGING_JOURNAL, from 0.7.184.1 on, are also answered with
// NoAdapterSynchronization alone (every other flag 0), in any power phase: they copy driver-image rings and touch
// no hardware, so dxgkrnl need not take the adapter lock for them (display.c, SoftwareReadEscape). Up to 0.7.183.1
// that combination is refused (REFUSED, STATUS_DEVICE_NOT_READY from the escape) and a reader must use
// HardwareAccess, which bc250kmd_cli falls back to by itself.
#define BC250_LOG_FROM_SUMMARY 0xFFFFFFFFu

typedef struct _BC250_LOG_LINE {
    unsigned long Sequence;                 // 0 for the first line of this driver load
    unsigned long Milliseconds;             // since GuardInit, i.e. since DriverEntry
    char Text[BC250_LOG_TEXT];
} BC250_LOG_LINE;

typedef struct _BC250_ESCAPE_LOG {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_GET_LOG or BC250_ESCAPE_LOG_SUMMARY
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long From;                     // in: the first sequence number wanted
    unsigned long Total;                    // out: lines logged since this driver load; sequence numbers run 0..Total-1
    unsigned long Lost;                     // out: lines the wrapping tail has overwritten
    unsigned long Above;                    // out: lines dropped because the caller was above DISPATCH_LEVEL
    unsigned long Returned;                 // out: lines in Lines[]
    unsigned long Next;                     // out: the sequence to ask for next; Returned 0 means the end
    unsigned long HeadLines, RingLines;     // out: the shape of the ring, so the tool need not assume it
    unsigned long SummaryFrom;              // out, LOG_SUMMARY only: the sequence its first line was given
    BC250_LOG_LINE Lines[BC250_LOG_MAX_LINES];
} BC250_ESCAPE_LOG;

// The first escape struct with mixed 4 and 8 byte alignment (four bytes of padding before Last[]). The driver and the
// CLI must agree on it; a packing option on either side makes this a build failure instead of garbage vectors.
typedef char BC250_ESCAPE_FENCE_SIZE_CHECK[(sizeof(BC250_ESCAPE_FENCE) == 112) ? 1 : -1];
typedef char BC250_ESCAPE_IV_SIZE_CHECK[(sizeof(BC250_ESCAPE_IV) == 48) ? 1 : -1];
typedef char BC250_ESCAPE_IH_SIZE_CHECK[(sizeof(BC250_ESCAPE_IH) == 2344) ? 1 : -1];  // 2336 + 3 new unsigned long
                                                                                      // fields, minus the 4 bytes
                                                                                      // of padding they absorbed
                                                                                      // before Last[] (was 2336 +
                                                                                      // 12 - 4; vsync-interrupt-route.md)
typedef char BC250_LOG_LINE_SIZE_CHECK[(sizeof(BC250_LOG_LINE) == 168) ? 1 : -1];
typedef char BC250_ESCAPE_LOG_SIZE_CHECK[(sizeof(BC250_ESCAPE_LOG) == 10812) ? 1 : -1];
typedef char BC250_ESCAPE_DCN_REG_SIZE_CHECK[(sizeof(BC250_ESCAPE_DCN_REG) == 56) ? 1 : -1];
typedef char BC250_ESCAPE_DCN_SIZE_CHECK[(sizeof(BC250_ESCAPE_DCN) == 4288) ? 1 : -1];  // still 4288: Otg0VupdateIntStatus
                                                                                        // (+4 bytes) exactly consumes the 4
                                                                                        // bytes of trailing pad the struct
                                                                                        // already needed before Regs[75]
                                                                                        // to reach a multiple of 8 (Hubp0Address's
                                                                                        // alignment) - net size unchanged
typedef char BC250_ESCAPE_DCNFLIP_SIZE_CHECK[(sizeof(BC250_ESCAPE_DCNFLIP) == 168) ? 1 : -1];
typedef char BC250_ESCAPE_SDMACOPY_SIZE_CHECK[(sizeof(BC250_ESCAPE_SDMACOPY) == 88) ? 1 : -1];
typedef char BC250_ESCAPE_FBDUMP_SIZE_CHECK[(sizeof(BC250_ESCAPE_FBDUMP) == 524416) ? 1 : -1];

// ---- the paging journal (paging_journal.c, docs/design/paging-journal.md) ---------------------------------------
// One record per BuildPagingBuffer slice, TLB flush and DestroyAllocation. The kernel dump reader decodes the same
// layout from g_PagingJournal, so the record is plain fixed-width data with no pointers.
#define BC250_PJ_UPDATE_CPU 1u                  // UPDATE_PAGE_TABLE, CPU_VIRTUAL: written at once, no paging buffer
#define BC250_PJ_UPDATE_GPU 2u                  // UPDATE_PAGE_TABLE, GPU_PHYSICAL: built into the paging buffer at Dma
#define BC250_PJ_VIRTUAL_FILL 3u                // VIRTUAL_FILL: Va the destination, Offset the bytes moved
#define BC250_PJ_VIRTUAL_TRANSFER 4u            // VIRTUAL_TRANSFER: Va the source, Offset the bytes, Flags the direction
#define BC250_PJ_FLUSH_TLB 5u                   // FLUSH_TLB: only its position in the paging buffer
#define BC250_PJ_DESTROY_ALLOCATION 6u          // DestroyAllocation: Va the UMD's requested address, Offset the size
#define BC250_PJ_TRANSFER 7u                    // TRANSFER (physical): Offset the bytes moved
#define BC250_PJ_FILL 8u                        // FILL (physical): Offset the bytes moved
#define BC250_PJ_GFX_SUBMIT 9u                  // KMD193: one GFX IB reached the ring (gfx.c SubmitIbLocked).
                                                // Seq the GFX sequence, Fence the OS SubmissionFenceId, Va the
                                                // IB1 GPU address, Offset the context's root page table,
                                                // Allocation the KMD context object as a value, Level the
                                                // scheduler node, Index the process that created the context,
                                                // Count BC250_PJ_CTX_*. Valid and Dma unused.
#define BC250_PJ_FLAG_REPEAT 1u                 // UPDATE: DXGK_UPDATEPAGETABLEFLAGS.Repeat (one entry for the whole range)
#define BC250_PJ_FLAG_INITIAL 2u                // UPDATE: .InitialUpdate
#define BC250_PJ_FLAG_EVICTION 4u               // UPDATE: .NotifyEviction, VidMm evicts the allocation
#define BC250_PJ_FLAG_64KB 8u                   // UPDATE: .Use64KBPages (this driver refuses it)
#define BC250_PJ_FLAG_TO_SYSTEM 16u             // VIRTUAL_TRANSFER: local to system (paging out); else system to local
#define BC250_PJ_FLAG_UMD_ALLOCATION 32u        // DESTROY: a UMD allocation (Offset is its UmdBytes)
// KMD193 (0.7.193.1 and later), no layout change: the fields a kind left unused now carry the identity the
// 245 dump could not name (scratch game-recon bsod-245, REPORT-245.md item 3).
//   DESTROY  Level  the process that called DxgkDdiDestroyAllocation (the System worker for a VidMm-deferred
//                   destroy), Index its thread, Count the process that created the allocation, Valid the BC2A
//                   blob version (0 when it is not a UMD allocation), Dma the BC2A gem_flags.
//   UPDATE   with no hAllocation (every unmap), Allocation carries DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE
//                   .hProcess instead, and BC250_PJ_FLAG_PROCESS says so.
// Older drivers leave all of it zero, and g_PagingJournal.Version (BC250_PAGING_JOURNAL_VERSION) is 2 from
// this revision on, so a dump reader need not guess.
#define BC250_PJ_FLAG_PROCESS 64u               // UPDATE: Allocation holds hProcess, not an allocation handle
#define BC250_PJ_CTX_UMD 1u                     // GFX_SUBMIT, Count: the context arrived as a BC2C blob
#define BC250_PJ_CTX_SYSTEM 2u                  // GFX_SUBMIT, Count: DXGK_CREATECONTEXTFLAGS.SystemContext
// UPDATE, KMD183 (0.7.183.1 and later): bits 16-31 are a mask of the DXGK_PTE.Segment values the slice's valid
// entries carry: bit 16 + s for segment s < 15 (16 = system memory, 17 = segment 1, 18 = 2, 19 = 3), bit 31 for any
// segment from 15 up. All zero on older drivers and for a slice with no valid entry. Record size and layout unchanged.
#define BC250_PJ_FLAG_SEGMENT_SHIFT 16u
#define BC250_PJ_FLAG_SEGMENT_MASK 0xFFFF0000u
#define BC250_PJ_FLAG_SEGMENT(s) (((s) < 15u) ? (1u << (BC250_PJ_FLAG_SEGMENT_SHIFT + (s))) : 0x80000000u)

typedef struct _BC250_PAGING_JOURNAL_RECORD {
    unsigned long long Time;                // KeQueryInterruptTime() when recorded (100 ns since boot; the log's
                                            // Milliseconds are (Time - the log's start) / 10000)
    unsigned long long Va;                  // UPDATE: the GPU VA the slice's first entry maps; fills/transfers: see the kind
    unsigned long long Allocation;          // the driver's allocation handle (its BC250_WDDM_OBJECT), 0 when none
    unsigned long long Offset;              // UPDATE: AllocationOffsetInBytes; others: bytes (see the kind)
    unsigned long long Dma;                 // GPU path: DmaBufferGpuVirtualAddress + DmaBufferWriteOffset at the build; 0 = none
    unsigned long Kind;                     // BC250_PJ_*
    unsigned long Level;                    // UPDATE: page table level
    unsigned long Index;                    // UPDATE: StartIndex + slice start, entries into the table
    unsigned long Count;                    // UPDATE: entries in the slice
    unsigned long Valid;                    // UPDATE: entries of the slice with the Windows Valid bit; the rest zero theirs
    unsigned long Flags;                    // BC250_PJ_FLAG_*
    unsigned long Fence;                    // OS SubmissionFenceId of the paging buffer that carried it (0: CPU path, or
                                            // not submitted yet)
    unsigned long Seq;                      // the SDMA sequence GfxSubmitPaging gave that buffer (0: not yet)
} BC250_PAGING_JOURNAL_RECORD;
#define BC250_PAGING_JOURNAL_MAX 64u        // records one escape returns

typedef struct _BC250_ESCAPE_PAGING_JOURNAL {
    unsigned long Magic;                    // in: BC250_ESCAPE_MAGIC
    unsigned long Command;                  // in: BC250_ESCAPE_GET_PAGING_JOURNAL
    unsigned long Status;                   // out: BC250_ESCAPE_STATUS_*
    unsigned long Version;                  // out: BC250_KMD_VERSION
    unsigned long NtStatus;                 // out: the driver's reason when Status is REFUSED
    unsigned long Flags;                    // out: BC250_ESCAPE_FLAG_*
    unsigned long Returned;                 // out: records in Records[]
    unsigned long Capacity;                 // out: records the ring holds
    unsigned long long From;                // in: the first record index wanted
    unsigned long long Total;               // out: records written since this driver load; indices run 0..Total-1
    unsigned long long Next;                // out: the index to ask for next; Returned 0 means the end
    unsigned long long Lost;                // out: requested records the ring had already overwritten
    BC250_PAGING_JOURNAL_RECORD Records[BC250_PAGING_JOURNAL_MAX];
} BC250_ESCAPE_PAGING_JOURNAL;
typedef char BC250_PAGING_JOURNAL_RECORD_SIZE_CHECK[(sizeof(BC250_PAGING_JOURNAL_RECORD) == 72) ? 1 : -1];
typedef char BC250_ESCAPE_PAGING_JOURNAL_SIZE_CHECK[(sizeof(BC250_ESCAPE_PAGING_JOURNAL) == 4672) ? 1 : -1];
