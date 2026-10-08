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
#define BC250_ESCAPE_RUN_HWMON 27u              // Super I/O hardware monitor: fan speed, duty read-back, its own temperatures
#define BC250_ESCAPE_RUN_DPM_CURVE 28u          // the operator's GPU V/F curve and its trial: read, set, keep, cancel, reset
#define BC250_ESCAPE_RUN_CPU 29u                // CPU clock limit, undervolt, temperature cap, readbacks, core mask
#define BC250_ESCAPE_RUN_FAN 30u                // case fan control: read, board, curve, fixed duty under a lease, renew
#define BC250_ESCAPE_RUN_DPAUDIO 31u            // DP audio: step 0 observation, steps 1 and 2 state (BC250_ESCAPE_DPAUDIO below)
#define BC250_KMD_VERSION 0x000700D8u       // revision 216 (INF 0.7.216.1, on 215.1): DP audio step 2, the
                                            // stream half (dpaudio.c: wall DTO, AFMT, DP_SEC). One escape
                                            // grows, and that is why this word moves: RUN_DPAUDIO ABI 2,
                                            // 480 bytes, the unchanged ABI 1 layout (408 bytes) followed by
                                            // the stream record. ABI 1 answers as before, and the RUN_DPAUDIO
                                            // number does not change. EnableDpAudioStream is 1 by default;
                                            // 0 is its bisect switch. The endpoint now stays hidden unless
                                            // the stream runs, so 0 gives no audio endpoint at all.
                                            //
                                            // Revision 215 (INF 0.7.215.1, on 214.1): the b21 train driver.
                                            // Two escapes are new, and that is why this word moves:
                                            // BC250_ESCAPE_RUN_FAN (30, fan.c, docs/design/fan.md Part B)
                                            // and BC250_ESCAPE_RUN_DPAUDIO (31, dpaudio.c, steps 0 and 1).
                                            // No older escape changes its layout. EnableFanControl,
                                            // EnableDpAudio and EnableDpAudioEndpoint are 1 by default; 0
                                            // is each one's bisect switch and gives the 214.1 behaviour.
                                            //
                                            // The same revision also reads the SMU metrics table
                                            // (smu_metrics.c, docs/design/dpm.md "Power reading") and
                                            // adds RUN_DPM ABI 3: BC250_ESCAPE_DPM_EX, 248 bytes, the
                                            // unchanged ABI 2 layout followed by BC250_DPM_METRICS (the
                                            // package power, the two rails, the table's own clock and
                                            // temperatures). ABI 1 and ABI 2 answer as before, and the
                                            // RUN_DPM number does not change. EnableSmuMetrics is 1 by
                                            // default; 0 is its bisect switch and sends no metrics
                                            // message at all. Not released before this addition, so the
                                            // word stays 0x000700D7.
                                            //
                                            // Revision 214 (INF 0.7.214.1, on 213.1): the VMID pool
                                            // (kmd/vmid-pool, gfx.c, vmid_pool.h,
                                            // docs/design/gfx-submit-root-serialization.md). Each page-table
                                            // root gets a VMID of its own from VMIDs 1 and 3..15, so a job of
                                            // another process no longer waits for the ring to drain. No escape
                                            // grows or changes its layout. One field changes its meaning, and
                                            // that is why this word moves: Valid of a BC250_PJ_GFX_SUBMIT
                                            // journal record is the VMID of the IB from this revision, and 0 in
                                            // the records of earlier drivers, whose WDDM jobs all ran at VMID 1.
                                            // EnableVmidPool 1 is the default (INF and release installer), 0 is
                                            // its bisect switch, and a start with it at 0 behaves as 0.7.213.1.
                                            //
                                            // Revision 213 (INF 0.7.213.1, on 208.1): the b20 train driver after
                                            // the respin. Seven revisions were written apart on seven branches,
                                            // each taking the next free number for itself: 209 (the DirectFlip
                                            // handshake), 208 again (the fan reader), 210 twice (the ring-gap
                                            // instrument with the notify-DPC pairing, and the Tuner), and two
                                            // more for the soft thermal zone and the softened flag words. None
                                            // of them was deployed. The train carries all of them in one driver
                                            // and takes 213, above every number any of them claimed, so that no
                                            // claimed revision is reused and Windows cannot tie this driver with
                                            // one of them. The first build of this train (0.7.212.1, escape ABI
                                            // 0x000700D4) was never installed either.
                                            //
                                            // Three new escape numbers against 0.7.208.1: 27 RUN_HWMON (reply
                                            // 216 bytes, ABI 1), 28 RUN_DPM_CURVE (360 bytes, ABI 1) and 29
                                            // RUN_CPU (296 bytes, ABI 1).
                                            //
                                            // Two changes to an older escape, both of them additive, and both
                                            // the reason this constant is 0x000700D5 and not 0x000700D4:
                                            //
                                            //   - RUN_DPM_TUNE grows to ABI 3, 184 bytes (the soft thermal
                                            //     zone). ABI 1 (120 bytes) and ABI 2 (152 bytes) keep their
                                            //     layout and their meaning, so every reader of 0.7.208.1 still
                                            //     parses what it read, and a THERMAL at ABI 1 or ABI 2 leaves
                                            //     the zone's own values alone. A driver before 0.7.213 refuses
                                            //     the 184-byte escape, which is how a tool finds the step down.
                                            //   - START_HEALTH CONFIRM and RUN_CPU KEEP admit
                                            //     NoAdapterSynchronization: neither sends a mailbox message or
                                            //     reads a BAR, and CONFIRM is retried every 5.5 s at an
                                            //     administrator logon, where idling the GPU scheduler is the
                                            //     wrong price. For one release both also admit the
                                            //     HardwareAccess word they asked for up to 0.7.212, so an older
                                            //     CLI, DLL or overlay still works against this driver.
                                            //
                                            // Every finished addition is ON in the release, with one switch each
                                            // to bisect a regression (the release-train policy, owner
                                            // 2026-10-05): EnableDirectFlipHandshake 1, EnableHwmon 1,
                                            // NotifyDpcInReport 1 and the soft thermal zone on when
                                            // DpmThermalZone is absent. What stays absent is what an operator
                                            // sets, not a feature: HwmonBasePort, HwmonExpectId,
                                            // HwmonDutyProven, HotSubmitLog, CpuTune and the rest of the Tuner's
                                            // own values. A start with the whole switch set closed behaves as
                                            // 0.7.208.1 did.
                                            //
                                            // The revisions as they were written, newest first:
                                            //
                                            // the soft thermal zone (written on dpm/thermal-soft-zone and merged
                                            // into this lineage by train b20, BD-087 and C56): the governor steps
                                            // the clock cap down inside a zone below the 87 C hot cap instead of
                                            // waiting for the cap itself, holds a step at a start and across a
                                            // stall, and judges its thresholds with a lead measured over a fixed
                                            // slope window. RUN_DPM_TUNE ABI 3 carries the zone's threshold, its
                                            // step and its lead, each with its default, and the slope window out.
                                            // The zone is on when DpmThermalZone is absent; DpmThermalZone 0 runs
                                            // the 0.7.212 thermal rules for a whole start, which is this wagon's
                                            // bisect switch. BC250_DPM_TUNE_HOT_MC publishes the 87 C both deltas
                                            // count down from, and driver/kmd/dpm.c holds a C_ASSERT against the
                                            // shim's own BC250_DPM_HOT_MC, so moving the hot limit breaks the
                                            // build instead of a printed threshold.
                                            //
                                            // the softened flag words (written on kmd/confirm-soft and merged
                                            // into this lineage by train b20, C57 and K195): START_HEALTH CONFIRM
                                            // and RUN_CPU KEEP take NoAdapterSynchronization, as described above.
                                            // Nothing else about either operation changed: both still need an
                                            // administrator, KEEP still writes the Parameters key and ends the
                                            // trial, and CONFIRM still names the generation and the visibility
                                            // epoch its client observed.
                                            //
                                            // revision 209 (INF 0.7.209.1, on 208.1): M15.14 increment 2, the
                                            // DirectFlip handshake's kernel half. One derivation of the type-0
                                            // placement (WddmGdiRecordPolicy) that CreateAllocation uses and the
                                            // compositor's user-mode driver asks, with WddmGdiRecordScannable as
                                            // the user-mode question - that one refuses every record it could not
                                            // read, because the placement derivation is fail-safe for the kernel
                                            // driver and fail-open for the shell. A second optional
                                            // UMDRIVERPRIVATE trailer (bc250_scanout_caps.h, offset 1496, 24
                                            // bytes) publishes whether this start will admit a client scan-out
                                            // flip and at what geometry; it is written only when the new REG_DWORD
                                            // EnableDirectFlipHandshake is 1 AND every start-latched fact the flip
                                            // path needs is true, so a start with it off is byte for byte 208 at
                                            // every buffer size and a closed kernel path can never leave the shell
                                            // agreeing to a flip this driver would refuse. Three new witnesses in
                                            // the summary: type-0 creates by the resource record they arrived with
                                            // (a standard primary carries none), the OS's own
                                            // DXGK_SETVIDPNSOURCEADDRESS_FLAGS per bit, and the Presents carrying
                                            // RedirectedFlip. No escape struct, no journal record layout and no
                                            // counter of the escape interface changed; the constant moves because
                                            // packagecheck VRS010 matches it against the INF revision and because
                                            // Windows keeps the driver it has when the version ties.
                                            //
                                            // the fan reader (written as revision 208 on fan/read-nct6686 and
                                            // merged into this lineage by train b20): the board's own hardware
                                            // monitor becomes readable. The ASRock BC-250 carries a Nuvoton
                                            // NCT6686D Super I/O. Its embedded controller turns the case fan
                                            // from the BIOS "Fan Setting" curve, and until this revision
                                            // nothing in Windows could read it, so the control application
                                            // said "The driver cannot read it yet".
                                            //
                                            // The reader is read-only and gated. EnableHwmon 1 is the INF
                                            // default from 0.7.213; EnableHwmon 0 means no port access
                                            // happens at all and is the bisect switch of this wagon. With
                                            // the gate open the start proves the chip from the EC window alone
                                            // (firmware version, build date, customer ID, the monitor's own
                                            // run bit and the two present masks) and refuses to read a
                                            // window that does not answer like this chip. The governor
                                            // thread then samples once a second, publishes one snapshot
                                            // under a spin lock, and RUN_HWMON hands that snapshot out.
                                            // The escape reads no port, so it keeps
                                            // NoAdapterSynchronization alone and never stalls a frame.
                                            //
                                            // What is read: every present tachometer (raw RPM), every
                                            // present duty output (read-back only), the fan mode mask, the
                                            // fan engine status and the monitor's own temperature channels,
                                            // which on unit A are the APU over SB-TSI and two board
                                            // thermistors. Nothing is ever written: there is no duty write,
                                            // no mode write, no limit write and no HWM_CFG write, and the
                                            // allowlist in driver/shim/bc250_hwmon.c refuses every write by
                                            // rule with a host test behind it. The Super I/O configuration
                                            // ports 0x2E/0x2F are never touched either, because the DSDT
                                            // drives that pair under an ACPI mutex this driver cannot take.
                                            //
                                            // The reading changes no decision in the driver: DPM thresholds,
                                            // the thermal cap, the ramp and the idle point are unchanged,
                                            // and bc250_dpm_step gains no fan input. The BIOS still owns
                                            // the fan. No other escape structure and no other ABI changed.
                                            //
                                            // the ring-gap histogram and the paired notify DPC (written as
                                            // revision 210 on c48/notifydpc-in-report and merged into this
                                            // lineage by train b20): the ring's own idle-gap histogram, and
                                            // the completion report pairs its own DPC-level notification.
                                            //
                                            // Offline analysis of RotTR sessions 418-420 found the 3D ring idle
                                            // a median 8 ms, 418 to 605 times per 105 s, with a dispatchable
                                            // packet already queued, the gap ending within 300 us of a display
                                            // VSync: 0.45 to 0.67 ms of every frame, 59 to 64 % of all ring
                                            // idle. Reading it took a 3 GB event-trace dump per session. This
                                            // revision measures a near relative of that quantity in the driver
                                            // that owns the ring (ring_gap.h, two counter reads per packet
                                            // boundary, four summary lines per node) - a superset, because the
                                            // driver cannot see a packet dxgkrnl holds queued, so only the
                                            // VSync-ended filter separates the class from ordinary starved
                                            // idle. "The GPU must not wait" now has a number in every
                                            // workload, game or client, from the log summary alone.
                                            //
                                            // It also changes one thing, behind NotifyDpcInReport (default 1,
                                            // as it was written on c48/notifydpc-in-report):
                                            // the report pass calls DxgkCbNotifyDpc itself, and the
                                            // DxgkCbQueueDpc whose only job was to bring that call about is
                                            // then not made - one dxgkrnl device DPC per completion fewer.
                                            // Until now the DPC-level notification a completion report owes
                                            // arrived only with that next dxgkrnl DPC: a 12 to 20 us hop at the
                                            // 187 completions a second session 418 retired, about 0.03 ms of a
                                            // 70 Hz frame. It is NOT what holds the ring for 8 ms; C48 died on
                                            // its own clause 1 and notify_pairing.h carries the evidence. The
                                            // DDI asks for the pairing (d3dkmddi.md:2392); notify_pairing.h is
                                            // the model of both shapes and the host test drives it. The value 0
                                            // restores 0.7.208.1 exactly, which is how the lab prices it.
                                            //
                                            // Third, HotSubmitLog (default 0) takes the three per-submit guard
                                            // log lines of SubmitIbLocked out of the hot path: they wrote 3871
                                            // lines a second inside GartLock and 75 % of the ring was
                                            // overwritten before anything read it. The count of what was left
                                            // out is in the summary, so a quiet log is not read as a quiet
                                            // ring. No escape struct, no journal record layout and no counter
                                            // of the escape ABI changed, so bc250kmd_cli stays compatible; the
                                            // constant moves because packagecheck VRS010 matches it against
                                            // the INF revision, and because Windows keeps the driver it has
                                            // when the version ties. 209 is taken by the independent-flip
                                            // candidate, so this one is 210.
                                            //
                                            // the Tuner (written as revision 210 on tuner/vf-cpu and merged into
                                            // this lineage by train b20): the GPU V/F curve with a kernel-owned
                                            // trial that reverts, the CPU surface on the firmware's queue 3 (a
                                            // clock limit, an undervolt and a temperature cap, read-gated and
                                            // staged), and the 8-core unlock through the AMD-named queue 0
                                            // message. docs/design/tuner.md and ADR 0020 are the design. Two new
                                            // escapes, 28 RUN_DPM_CURVE (360 bytes) and 29 RUN_CPU (296 bytes);
                                            // 27 is the fan reader's, which is in this driver as well.
                                            //
                                            // revision 208 (INF 0.7.208.1, on 207.1): five summary lines of the
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
// Both operations take NoAdapterSynchronization=1 and every other flag zero (0.7.213):
// CONFIRM touches only this snapshot and the registry, and an administrator logon
// retries it every 5.5 s, which must never suspend the GPU scheduler. For one release
// CONFIRM also admits the HardwareAccess=1 word it asked for up to 0.7.212, so an
// older CLI, DLL or overlay still confirms a start.
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
#define BC250_DPM_FLAG_CURVE 2048u           // CurrentMv comes from an operator's V/F curve, not the table (0.7.210)
#define BC250_DPM_FLAG_CURVE_TRIAL 4096u     // a curve trial runs; RUN_DPM_CURVE says for how much longer (0.7.210)
// 16384 and 32768 (0.7.216.7), in every ABI: no field moves, so BC250_KMD_VERSION stays. 8192 is ABI 3's, below.
#define BC250_DPM_FLAG_JOINT 16384u          // the joint power arm runs in this start (DpmJointGovernor 1)
#define BC250_DPM_FLAG_JOINT_CAP 32768u      // the arm's CPU clock limit is in the chip now (the driver log says which)
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

// RUN_DPM ABI 3 (0.7.215): the SMU metrics table (driver/kmd/smu_metrics.c, driver/shim/include/bc250_smu_metrics.h,
// docs/design/dpm.md "Power reading"). BC250_ESCAPE_DPM_EX is the ABI 2 structure above, unchanged, with AbiVersion 3,
// followed by BC250_DPM_METRICS. Everything in the tail is out, and like the rest of RUN_DPM it is the governor
// thread's published copy: the escape sends no SMU message. The driver takes all three sizes, each with its own
// AbiVersion; a driver before 0.7.215 refuses 248 bytes with STATUS_INVALID_PARAMETER, and a tool then asks with
// ABI 2 or ABI 1. BC250_DPM_FLAG_POWER is set in an ABI 3 answer alone, when MetricsState is OK and the table is at
// most three seconds old. The power figures are the SMU's own: SocketPowerMw is the whole package, processor and
// graphics together, and neither the board nor the fan is in it.
#define BC250_DPM_ABI_3 3u
#define BC250_DPM_ABI2_SIZE 192u             // the ABI 2 prefix of BC250_ESCAPE_DPM_EX, which is BC250_ESCAPE_DPM
#define BC250_DPM_ABI3_SIZE 248u
#define BC250_DPM_FLAG_POWER 8192u           // ABI 3: the BC250_DPM_METRICS values come from a fresh table (0.7.215)
#define BC250_DPM_METRICS_OFF 0u             // EnableSmuMetrics 0: no metrics message is sent
#define BC250_DPM_METRICS_WAITING 1u         // on; no table accepted yet in this start
#define BC250_DPM_METRICS_OK 2u              // the values are from the last accepted table (MetricsAgeMs old)
#define BC250_DPM_METRICS_REFUSED 3u         // the firmware refused or did not answer: no reading until the driver loads again
#define BC250_DPM_METRICS_NO_TABLE 4u        // no table page (VRAM closed, mapping failed) or no native SMU owner
#define BC250_DPM_METRICS_BAD_TABLE 5u       // three tables in a row failed the checks: stopped for this start
typedef struct _BC250_DPM_METRICS {
    unsigned long MetricsState;             // BC250_DPM_METRICS_*
    unsigned long MetricsAgeMs;             // since the last accepted table; 0 when there is none
    unsigned long MetricsReads;             // tables accepted in this start
    unsigned long MetricsFailures;          // reads in this start that ended without a table
    unsigned long SocketPowerMw;            // Current.CurrentSocketPower: the package, mW
    unsigned long SocketPowerAvgMw;         // Average.CurrentSocketPower, mW
    unsigned long GfxPowerMw, SocPowerMw;   // Current.Power[1] (VDDCR_GFX) and Power[0] (VDDCR_VDD), mW
    unsigned long GfxMv, SocMv;             // Current.Voltage[1] and Voltage[0], mV
    unsigned long GfxMHz;                   // Current.GfxclkFrequency: the table's own GPU clock
    unsigned long GfxTemperatureCc;         // Current.GfxTemperature, centi-Celsius
    unsigned long SocTemperatureCc;         // Current.SocTemperature, centi-Celsius
    unsigned long ThrottlerStatus;          // Current.ThrottlerStatus, the firmware's bits as they are
} BC250_DPM_METRICS; // 56 bytes
typedef struct _BC250_ESCAPE_DPM_EX {
    BC250_ESCAPE_DPM Dpm;                   // the ABI 2 layout; Dpm.AbiVersion is BC250_DPM_ABI_3
    BC250_DPM_METRICS Metrics;
} BC250_ESCAPE_DPM_EX; // 248 bytes on Windows, ABI 3

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
// ABI 3 (0.7.213, BD-087) appends the soft thermal zone to the same THERMAL operation: the zone's threshold below HOT
// (0: the zone off), its step down and the lead its thresholds are judged with, each with its default, plus the fixed
// slope window the lead measures the die's rise over. The zone is ON by default, so a READ of a healthy start shows it;
// the one switch that turns it off for a whole start is the registry value DpmThermalZone (driver/kmd/dpm.c), and a
// RESET here goes back to the shim's defaults and therefore turns it on again. The driver takes all three sizes:
// AbiVersion 1 with the first 120 bytes, AbiVersion 2 with the first 152 (both layouts unchanged), AbiVersion 3 with all
// 184. An ABI 1 or ABI 2 THERMAL keeps the stored zone values, so an older tool cannot switch the zone off by writing
// fields it does not know; a RESET resets everything, as it always did. A driver before 0.7.213 fails the 184-byte
// escape itself with STATUS_INVALID_PARAMETER: a tool asks with ABI 3 and steps down to 2 and then to 1 on that answer.
#define BC250_DPM_TUNE_ABI 3u
#define BC250_DPM_TUNE_ABI_2 2u
#define BC250_DPM_TUNE_ABI_1 1u
// The reading the two deltas of this escape are counted down from: BC250_DPM_HOT_MC of
// driver/shim/include/bc250_dpm.h, 87 C, fixed since 0.7.195. Both the soft release and the soft zone travel as a
// delta below it, so a tool has to know it to print the threshold in force, and a user-mode tool does not include the
// shim's kernel header. The driver's own C_ASSERT (driver/kmd/dpm.c) keeps the two equal, so moving the hot limit
// breaks the driver build instead of a printed threshold (0.7.213 safety review, finding 8).
#define BC250_DPM_TUNE_HOT_MC 87000l
#define BC250_DPM_TUNE_ABI1_SIZE 120u        // the ABI 1 prefix of BC250_ESCAPE_DPM_TUNE
#define BC250_DPM_TUNE_ABI2_SIZE 152u        // the ABI 2 prefix
#define BC250_DPM_TUNE_OP_READ 0u
#define BC250_DPM_TUNE_OP_THRESHOLDS 1u      // in: UpPermille, TargetPermille, DownPermille, DownHoldMs
#define BC250_DPM_TUNE_OP_FLOOR 2u           // in: FloorMHz
#define BC250_DPM_TUNE_OP_RESET 3u           // thresholds, floor and (every ABI alike) thermal timing and zone to defaults
#define BC250_DPM_TUNE_OP_THERMAL 4u         // ABI 2: in HotStepMs, SoftReleaseDeltaMc, SoftReleaseStepMs; ABI 3 also in
                                             // ZoneDeltaMc, ZoneStepMs, ZoneLeadMs
#define BC250_DPM_TUNE_FLAG_GOVERNING 1u     // a DPM start's governor thread runs and has not given up: writes are taken
#define BC250_DPM_TUNE_FLAG_THRESHOLDS 2u    // the thresholds were set at run time (else the defaults)
#define BC250_DPM_TUNE_FLAG_FLOOR 4u         // a runtime floor is set (else none)
#define BC250_DPM_TUNE_FLAG_APPLIED 8u       // the governor thread runs with the values below (Applied == Serial)
#define BC250_DPM_TUNE_FLAG_THERMAL 16u      // ABI 2: the thermal timing was set at run time (else the defaults)
#define BC250_DPM_TUNE_FLAG_ZONE 32u         // ABI 3: the soft zone's values are not the defaults
#define BC250_DPM_TUNE_FLAG_ZONE_OFF 64u     // ABI 3: the soft zone is off for this start (ZoneDeltaMc 0)
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
    // ABI 3 from here (BC250_DPM_TUNE_ABI2_SIZE bytes above).
    unsigned long ZoneDeltaMc, ZoneStepMs, ZoneLeadMs;                               // in: THERMAL; out: in force
    unsigned long DefaultZoneDeltaMc, DefaultZoneStepMs, DefaultZoneLeadMs;          // out
    unsigned long ZoneSlopeMs;              // out: the window the lead's slope is measured over (fixed, not tunable)
    unsigned long Reserved3;                // zero in, zero out
} BC250_ESCAPE_DPM_TUNE; // 184 bytes on Windows, ABI 3 (the first 152 are ABI 2, the first 120 ABI 1)

// The board's hardware monitor (driver/kmd/hwmon.c, driver/shim/bc250_hwmon.c, docs/design/fan.md). The
// ASRock BC-250 carries a Nuvoton NCT6686D Super I/O. Its embedded controller turns the case fan from the
// BIOS "Fan Setting" curve; this escape only reports what the chip says. Adapter-owned software snapshot,
// published by the governor thread once a second: the escape reads no port and sends no message, so it takes
// NoAdapterSynchronization=1 and every other D3DDDI_ESCAPEFLAGS bit zero, exactly as RUN_DPM. READ is the
// only operation and is open to every caller; there is no write operation here. The one writer of the chip is
// the fan control, with its own escape (RUN_FAN, below).
//
// Rpm[i] is raw RPM of tachometer i; 0 means either a channel that does not turn or a value this driver
// refused. The two are told apart by RpmValidMask, which carries one bit per tachometer whose value THIS
// sample accepted: a channel that is present (FanPresentMask) with its valid bit clear was refused, and
// Refusals counts every refused value of the start. DutyPermille[i] is the duty
// READ-BACK of output i, 0..1000. Until DUTY_PROVEN is set the duty is a number the chip reports and not a
// proven description of the fan that turns: at E01 all five duty channels read 245 of 255 (96 %) while the
// fan turned at 1589 RPM, which is about half of this board's measured full-duty speed, and those two do not
// belong to the same fan. One lab trial settles it, and HwmonDutyProven then opens the flag.
// TemperatureMc[i] is the monitor's own channel i with its source code in TemperatureSource[i]: 0x46 is the
// APU over SB-TSI (an independent second reading of the temperature the SMU reports as Tctl), 0x08 and 0x09
// are board thermistors. A source of 0 means the channel carries nothing in this sample, which covers a
// channel the map does not hold and a value the driver refused: a reader must then show "no reading" for it
// and never a temperature. ModeMask is 0xA00 and Engine is 0xCF8, both AS THE START READ THEM and not per
// sample. M803 measured both on unit A (0xE0 and 0x60 at rest); the reader decides nothing on them, and the fan
// control reads its own live values inside its handshake (RUN_FAN). AgeMs counts from the last accepted sample at the time of the escape, so a stopped
// sampler shows its age growing. Reason is enum bc250_hwmon_reason (driver/shim/include/bc250_hwmon.h) and
// says why VALID is clear.
#define BC250_HWMON_ABI 1u
#define BC250_HWMON_OP_READ 0u
#define BC250_HWMON_FAN_SLOTS 8u             // tachometer and duty slots on the wire
#define BC250_HWMON_TEMP_SLOTS 4u            // temperature channels on the wire
#define BC250_HWMON_FLAG_VALID 1u            // the identity passed and the reader is online
#define BC250_HWMON_FLAG_MONITORING 2u        // HWM_CFG bit 7 was set at start: the firmware monitors
#define BC250_HWMON_FLAG_FRESH 4u            // AgeMs is inside the freshness window (three missed samples)
#define BC250_HWMON_FLAG_GATED 8u            // EnableHwmon is 0: no port access ever happened
#define BC250_HWMON_FLAG_ID_PINNED 16u       // HwmonExpectId was set and matched
#define BC250_HWMON_FLAG_DUTY_PROVEN 32u     // HwmonDutyProven is 1: a lab trial proved the duty read-back
// STOPPED: every present tachometer answered, every one of them reads 0, and a duty output is not 0, in
// BC250_HWMON_STOPPED_SAMPLES samples in a row (driver/kmd/hwmon.h). Three conditions and a repeat, because this flag is shown in
// red and is what the owner reacts to: one refused tachometer reading is not a stopped fan, and the driver
// must not send anybody to the case over a collision on a window that has no arbiter.
#define BC250_HWMON_FLAG_STOPPED 64u         // the duty is not 0 and no present tachometer turns (see above)
// Monitor source codes, so that a tool can name a channel without the shim header. Identical definitions live in
// driver/shim/include/bc250_hwmon.h, which the driver includes beside this file; the guard keeps that legal and
// the values are the ones measured on unit A through the Linux labels (E01 sensors-all.txt).
#ifndef BC250_HWMON_SOURCE_APU
#define BC250_HWMON_SOURCE_APU 0x46u         // AMD TSI at SMBus 0x98: the APU die, 83.0 C at E01
#endif
#ifndef BC250_HWMON_SOURCE_THERMISTOR14
#define BC250_HWMON_SOURCE_THERMISTOR14 0x08u
#endif
#ifndef BC250_HWMON_SOURCE_THERMISTOR15
#define BC250_HWMON_SOURCE_THERMISTOR15 0x09u
#endif
typedef struct _BC250_ESCAPE_HWMON {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long BasePort;                 // the EC window in use, 0 when the reader is offline
    unsigned long CustomerId;               // EC 0x602
    unsigned long EcVersion;                // high << 8 | low
    unsigned long EcBuild;                  // year << 16 | month << 8 | day
    unsigned long FanPresentMask;           // bit i: tachometer i exists
    unsigned long DutyPresentMask;          // bit i: duty output i exists
    unsigned long ModeMask;                 // 0xA00 as the start read it (M803: 0xE0 at rest)
    unsigned long Rpm[BC250_HWMON_FAN_SLOTS];
    unsigned long DutyPermille[BC250_HWMON_FAN_SLOTS];
    long TemperatureMc[BC250_HWMON_TEMP_SLOTS];
    unsigned long TemperatureSource[BC250_HWMON_TEMP_SLOTS];
    unsigned long AgeMs;                    // since the last accepted sample
    unsigned long long Samples, Errors, Retries;
    unsigned long long Generation;          // start-health generation of the start this describes
    unsigned long Reason;                   // enum bc250_hwmon_reason when VALID is clear
    unsigned long Engine;                   // 0xCF8 as the start read it (M803: 0x60 at rest); reported and logged only
    unsigned long RpmValidMask;             // bit i: Rpm[i] is a value this sample accepted, not a refusal
    unsigned long DutyValidMask;            // bit i: DutyPermille[i] is a value this sample accepted
    unsigned long long Refusals;            // values refused since the start, over every register
} BC250_ESCAPE_HWMON; // 216 bytes on Windows, ABI 1
// The operator's GPU V/F curve and its trial (0.7.213.1; driver/kmd/dpm.c, driver/shim/bc250_dpm.c,
// docs/design/tuner.md, ADR 0020). The clock grid cannot move, so a curve is BC250_DPM_CURVE_POINTS voltages, one
// per level from FirstMHz (1000) up in StepMHz (100) steps to the table's ceiling (2000). Software state only: the
// escape stores a candidate under the DPM state's locks and the governor thread applies it at its next tick
// (25 ms) through the same checked transaction every other level change uses. So every operation takes
// NoAdapterSynchronization=1 and every other D3DDDI_ESCAPEFLAGS bit zero, as RUN_DPM_TUNE, and no operation
// idles the GPU or stalls a running game (BD-054).
//
// READ is open to every caller. SET, KEEP, CANCEL and RESET need an administrator and ExpectedGeneration equal to
// the Generation a READ of this start returned (STATUS_RETRY otherwise). SET, KEEP and CANCEL also need a running
// DPM start (STATUS_INVALID_DEVICE_STATE: a fixed-lab start has no governor tick, so nothing could revert a
// candidate; such a start sits at 1000 MHz, where the curve is the floor's 820 mV anyway).
//
// SET is a trial, not a setting: the candidate becomes active, nothing is written to disk, and the governor
// thread puts the stored curve back when TrialMs passes without a KEEP. The revert is the kernel's own act, so a
// killed tool, a hung tool, a lost remote session and a bugcheck all end at the stored curve. KEEP inside the
// window persists the candidate (11 named REG_DWORDs plus the boot guard's two marks) and clears the trial.
// CANCEL reverts at once. RESET puts the table's own line back and deletes the stored values.
//
// A refused candidate leaves everything as it was and names the rule in Error (enum bc250_clock_curve_error,
// driver/shim/include/bc250_clock.h) and the level in ErrorLevel: a value outside 820..1000 mV, more than
// BC250_CURVE_UNDERVOLT_MV under the table's line, a voltage that falls as the clock rises, or a first point that
// is not the lab floor's 820 mV. FloorMv carries the lowest voltage admitted at each level, so a window can draw
// the band it may not enter without knowing the rule.
#define BC250_DPM_CURVE_ABI 1u
#define BC250_DPM_CURVE_POINTS 11u           // levels 5..15 of the clock table: 1000..2000 MHz
#define BC250_DPM_CURVE_OP_READ 0u
#define BC250_DPM_CURVE_OP_SET 1u            // in: CandidateMv, TrialMs
#define BC250_DPM_CURVE_OP_KEEP 2u           // the candidate on trial becomes the stored curve
#define BC250_DPM_CURVE_OP_CANCEL 3u         // the stored curve comes back now
#define BC250_DPM_CURVE_OP_RESET 4u          // the table's own line, stored
#define BC250_DPM_CURVE_FLAG_VALID 1u        // the governor state was read (a full WDDM start)
#define BC250_DPM_CURVE_FLAG_ON_TRIAL 2u     // a candidate runs; TrialRemainingMs says for how much longer
#define BC250_DPM_CURVE_FLAG_STORED 4u       // the stored curve is not the table's line (registry values exist)
#define BC250_DPM_CURVE_FLAG_PENDING 8u      // DpmCurvePending is on disk: a start with this curve was not confirmed
#define BC250_DPM_CURVE_FLAG_CONFIRMED 16u   // DpmCurveConfirmed matches the stored curve
#define BC250_DPM_CURVE_FLAG_DEFAULT 32u     // the active curve is the table's own line
#define BC250_DPM_CURVE_FLAG_GOVERNING 64u   // a DPM start's governor runs and has not given up: writes are taken
#define BC250_DPM_CURVE_FLAG_APPLIED 128u    // the governor has applied the active curve (Applied == Serial)
typedef struct _BC250_ESCAPE_DPM_CURVE {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long TrialMs;                  // in: SET (10000..180000, 0 = the driver's 25000); out: in force
    unsigned long TrialRemainingMs;         // out: 0 outside a trial
    unsigned long Serial, Applied;          // out: changes of the active curve, and what the governor runs
    unsigned long Error, ErrorLevel;        // out: why a candidate was refused, and at which table level
    unsigned long FirstMHz, StepMHz, Points;    // out: the grid the five vectors below describe
    unsigned long CandidateMv[BC250_DPM_CURVE_POINTS];  // in: SET; out: the candidate on trial, else zeros
    unsigned long ActiveMv[BC250_DPM_CURVE_POINTS];     // out: what the governor applies now
    unsigned long StoredMv[BC250_DPM_CURVE_POINTS];     // out: what the registry holds (the revert target)
    unsigned long DefaultMv[BC250_DPM_CURVE_POINTS];    // out: the table's own line
    unsigned long FloorMv[BC250_DPM_CURVE_POINTS];      // out: the lowest voltage admitted at each level
    unsigned long Level, LevelMHz, LevelMv;     // out: where the governor is now, and the curve's voltage there
    unsigned long ObservedMHz, ObservedVid;     // out: the SMU's last readback
    long TemperatureMc;                         // out
    unsigned long CeilingMHz, Mode;             // out: this start's DpmMaxMHz and BC250_DPM_MODE_*
    unsigned long Sets, Keeps, Cancels, Reverts;    // out: the trial's own counters
    unsigned long long Generation;          // out: start-health generation of the start this describes
    unsigned long long ExpectedGeneration;  // in: SET, KEEP, CANCEL, RESET
    unsigned long Reserved[2];              // zero in, zero out
} BC250_ESCAPE_DPM_CURVE; // 360 bytes on Windows, ABI 1

// The CPU surface (0.7.213.1; driver/kmd/cpu.c, driver/shim/bc250_cpu.c, docs/design/tuner.md, ADR 0020): a clock
// limit, an undervolt in firmware curve-scale steps, the firmware's own temperature cap, the readbacks of all
// three, and the core-enable mask. The transport is the firmware's queue 3, whose three mailbox registers are in
// the BAR5 aperture the driver already maps; the KMD is still the single SMU owner, with a second allowlist so
// that a GFX clock transaction can never send a CPU message and a CPU transaction can never send a clock message.
//
// Unlike every other escape here, most write operations DO send mailbox messages, so SET, CANCEL, RESET, CORES and
// SEARCH_* need HardwareAccess=1 (the Level Two exclusion) and an administrator, exactly as RUN_CLOCK's SET does.
// READ is adapter-owned software state and takes NoAdapterSynchronization alone; READBACK sends only getters and
// is a HardwareAccess operation as well. KEEP sends nothing - it writes the Parameters key and ends the trial - so
// from 0.7.213 it takes NoAdapterSynchronization as well, and still an administrator. For one release KEEP also
// admits the HardwareAccess word it asked for up to 0.7.212, so an older CLI or DLL keeps working.
//
// Three rules the driver enforces, and a caller should expect:
//   - No setter runs until this start's READBACK has answered once (FLAG_QUEUE3_PROVEN). Queue 3 has never been
//     spoken to on this part, so an inferred fact is measured before a write depends on it.
//   - No CPU message while the GPU is at or above BC250_CPU_GPU_BUSY_PERMILLE busy. At or above the lab's 87 C
//     no setter runs either, with two exceptions the driver names itself: a step that lowers the dissipation,
//     and the way back from a trial. The part must always be returnable to the settings it is known to run at,
//     and a trial left in the chip because the part was hot is the worse of the two states (0.7.211).
//     One message per BC250_CPU_MESSAGE_GAP_MS, and the owner lock is released between them so the
//     governor's 25 ms tick is never held for a whole sequence.
//   - A SET is a trial. The driver reverts it when TrialMs passes without a KEEP, and nothing reaches the
//     registry before a KEEP. A revert that the firmware refuses stays owed (FLAG_REVERT_OWED) and the driver
//     keeps trying; only then is a cold boot the last backstop, because nothing of this surface persists in
//     the chip.
//   - SEARCH_STEP and SET judge the sample the caller describes: WheaEvents, ChecksumErrors and Loaded come
//     from the caller, which is the only side that can count a machine check or know that it was loading the
//     part. Without Loaded the driver does not judge clock stretching at all, because an idle core sits a
//     gigahertz under any limit.
// CpuTune 0 (the release default) refuses every write with STATUS_INVALID_DEVICE_STATE: the CPU surface is opt-in
// per machine. Error is enum bc250_cpu_error; SearchFail is enum bc250_cpu_fail (driver/shim/include/bc250_cpu.h).
#define BC250_CPU_ABI 1u
#define BC250_CPU_CORE_SLOTS 8u
// What a caller must know to form a request, under names of this header alone: the two core masks the driver
// admits, the deepest step of the guided search and how many reasons Error can carry.
// driver/shim/include/bc250_cpu.h is the authority for all four, and driver/kmd/cpu.c asserts at compile time
// that these numbers still equal its own. A tool that prints a reason per Error value sizes its table on
// BC250_CPU_REQUEST_ERROR_COUNT and asserts the same way, so a new reason cannot reach a caller as "?".
#define BC250_CPU_REQUEST_MASK_STOCK 0x77u     // 6 of the 8 cores: the mask this part ships with
#define BC250_CPU_REQUEST_MASK_FULL 0xFFu      // all eight
#define BC250_CPU_REQUEST_SEARCH_STEPS 8u      // the deepest undervolt step the search tries
#define BC250_CPU_REQUEST_ERROR_COUNT 7u       // enum bc250_cpu_error, 0 to 6 (0.7.216.23 added NO_CEILING)
#define BC250_CPU_REQUEST_ERROR_NO_CEILING 6u  // the one reason a caller acts on: the clock control is refused
                                               // for this start, the undervolt and the cap still work (BD-094)
#define BC250_CPU_OP_READ 0u                 // software state only: what is applied, stored, recorded
#define BC250_CPU_OP_READBACK 1u             // send the getters of both queues; no setter
#define BC250_CPU_OP_SET 2u                  // in: Given, MaxMHz, UvSteps, TempC, TrialMs
#define BC250_CPU_OP_KEEP 3u
#define BC250_CPU_OP_CANCEL 4u
#define BC250_CPU_OP_RESET 5u                // the recorded baseline, and the stored values deleted
#define BC250_CPU_OP_CORES 6u                // in: CoreMask (119 or 255); applies at the next Windows restart
// The guided undervolt search ("find my setting"). The judgement is the shim's (bc250_cpu_search_next), the load is
// the caller's: BEGIN records the baseline and applies step 1 as a trial; then the caller loads the CPU for
// SearchLoadMs and calls STEP, which reads the chip, judges the step just loaded and either applies the next one or
// stops and puts the baseline back. SearchBest is then the deepest step that passed, and a person presses KEEP.
// A caller that stops calling loses nothing: the trial window of the step in force ends it.
#define BC250_CPU_OP_SEARCH_BEGIN 7u         // in: UvSteps (0 = BC250_CPU_SEARCH_MAX_STEPS), TrialMs
#define BC250_CPU_OP_SEARCH_STEP 8u          // out: SearchStep, SearchBest, SearchFail, SearchTested, Flags
#define BC250_CPU_GIVEN_MAX 1u               // Given bits: which of the three values a SET carries
#define BC250_CPU_GIVEN_UV 2u
#define BC250_CPU_GIVEN_TEMP 4u
#define BC250_CPU_FLAG_VALID 1u              // a full WDDM start with the SMU owner online
#define BC250_CPU_FLAG_ON_TRIAL 2u
#define BC250_CPU_FLAG_STORED 4u             // CpuMaxMHz, CpuUvSteps or CpuTempC exists
#define BC250_CPU_FLAG_PENDING 8u            // CpuPending is on disk: the last start with these values was not healthy
#define BC250_CPU_FLAG_CONFIRMED 16u
#define BC250_CPU_FLAG_QUEUE3_PROVEN 32u     // queue 3 answered a getter in this start: setters are admitted
#define BC250_CPU_FLAG_TUNE_ON 64u           // CpuTune 1
#define BC250_CPU_FLAG_SEARCHING 128u
#define BC250_CPU_FLAG_CORE_PENDING 256u     // CoreMaskPending: a restart now puts the stock mask back
#define BC250_CPU_FLAG_CORE_CONFIRMED 512u
#define BC250_CPU_FLAG_BUSY 1024u            // another CPU sequence runs: this request was refused, nothing changed
#define BC250_CPU_FLAG_REVERT_OWED 2048u     // a revert was refused and is retried: the chip still has the trial
#define BC250_CPU_FLAG_TEMP_VALID 4096u      // TemperatureMc was read; without this nothing judges the part cold
#define BC250_CPU_FLAG_BOOST_KNOWN 8192u     // the firmware's own boost ceiling answered in this start, so
                                             // BaselineMaxMHz can be given back and a clock limit is admitted.
                                             // Without it the clock control is refused (BC250_CPU_ERROR_NO_CEILING,
                                             // 0.7.216.23): a limit could only be given back as the P-state top,
                                             // under the boost (BD-094). No field moves, so the ABI stays
typedef struct _BC250_ESCAPE_CPU {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;
    unsigned long TrialMs, TrialRemainingMs, Serial, Error;
    unsigned long Given;                    // in: SET, BC250_CPU_GIVEN_* bits
    unsigned long MaxMHz, UvSteps, TempC;   // in: SET
    unsigned long AppliedMaxMHz, AppliedUvSteps, AppliedTempC;      // out: what the driver last sent (cached)
    unsigned long StoredMaxMHz, StoredUvSteps, StoredTempC;         // out: the registry's values, 0 when absent
    unsigned long BaselineMaxMHz, BaselineUvSteps, BaselineTempC;   // out: recorded before the first write
    unsigned long VoltageMv, GpuVoltageMv, CapC, Features;          // out: the queue 3 and queue 0 readbacks
    unsigned long CoreMHz[BC250_CPU_CORE_SLOTS];        // out: the effective clock per core, 0 for no answer
    unsigned long PstateMHz[BC250_CPU_CORE_SLOTS];      // out: the clock of each P-state
    unsigned long Cores, Threads;           // out: what Windows reports, which is all the mask's effect we can see
    unsigned long CoreMask, CoreMaskStored; // in: CORES; out: the setting in force and on disk
    unsigned long LastQueue, LastMessage, LastStatus, LastParameter;    // out: the support report only
    long TemperatureMc;                     // out
    unsigned long SearchStep, SearchBest, SearchFail, SearchTested;     // out
    unsigned long Reads, Writes, Refusals, Reverts;                    // out
    unsigned long RevertRetries, RevertFailures;     // out: an owed revert's attempts, and the refused ones
    unsigned long WheaEvents, ChecksumErrors;       // in: SET and SEARCH_STEP, from the caller's own counters
    unsigned long Loaded;                   // in: 1 while the caller loads the CPU over this sample
    unsigned long long Generation;          // out: start-health generation of the start this describes
    unsigned long long ExpectedGeneration;  // in: every write
    unsigned long Reserved[2];              // zero in, zero out
} BC250_ESCAPE_CPU; // 296 bytes on Windows, ABI 1

// The case fan control (driver/kmd/fan.c, driver/shim/bc250_fan.c, docs/design/fan.md Part B). The driver takes fan 1
// from the BIOS curve while it runs and gives it back to the chip's own automatic mode on every exit path (owner,
// 2026-10-06). Software state only: the escape reads the controller's published snapshot, and a write operation
// leaves a request that the governor thread applies at its next hardware-monitor step (at most one second later).
// No port is touched here, so every operation takes NoAdapterSynchronization=1 and every other D3DDDI_ESCAPEFLAGS bit
// zero, as RUN_HWMON and RUN_DPM_CURVE.
//
// READ is open to every caller. BOARD, CURVE, FIXED and RENEW need an administrator, ExpectedGeneration equal to the
// Generation a READ of this start returned (STATUS_RETRY otherwise), and a start whose fan control is enabled
// (STATUS_INVALID_DEVICE_STATE otherwise; Gate says why it is not).
//   BOARD  the chip's own curve (the BIOS "Fan Setting") runs the fan. Store=1 makes it the choice of every start.
//   CURVE  the driver's curve: Profile names a preset, or PROFILE_CUSTOM with Points, CurveC and CurvePct. LeaseMs 0
//          is durable (and Store=1 writes it to the registry); 5000..300000 is a trial that ends with the board.
//   FIXED  one duty, FixedPct 20..100, always under a lease of LeaseMs 5000..300000. Never stored.
//   RENEW  restarts the lease of a leased mode with LeaseMs.
// A refused request changes nothing and names the rule in Error (enum bc250_fan_error, driver/shim/include/bc250_fan.h).
//
// A new command and not a new revision of an old one, and no change of BC250_KMD_VERSION: the escape is additive, no
// existing layout moves, and a driver before this one answers BC250_ESCAPE_STATUS_UNKNOWN_COMMAND, which is how a tool
// finds it is talking to an older driver. The version constant is tied to the INF DriverVer (packagecheck VRS010), so
// a bump belongs to the release train that carries this escape, not to the escape itself.
#define BC250_FAN_ABI 1u
#define BC250_FAN_CURVE_SLOTS 8u
#define BC250_FAN_OP_READ 0u
#define BC250_FAN_OP_BOARD 1u
#define BC250_FAN_OP_CURVE 2u
#define BC250_FAN_OP_FIXED 3u
#define BC250_FAN_OP_RENEW 4u
#define BC250_FAN_FLAG_ENABLED 1u           // this start may drive the fan (EnableFanControl, the reader, the chip)
#define BC250_FAN_FLAG_CONTROLLING 2u       // the driver holds fan 1 now: its mode bit is set in the chip
#define BC250_FAN_FLAG_EMERGENCY 4u         // 100 %: the guard temperature reached 87 C
#define BC250_FAN_FLAG_LEASED 8u            // the mode in force ends when LeaseMs runs out (then the durable mode from before it)
#define BC250_FAN_FLAG_STORED 16u           // FanMode is on disk: StoredMode and StoredProfile are the registry's
#define BC250_FAN_FLAG_FAULT 32u            // the chip refused something: the board has the fan for this start
#define BC250_FAN_FLAG_GATED 64u            // EnableFanControl is 0: no write to the chip ever happens
#define BC250_FAN_FLAG_PAUSED 128u          // the adapter is out of D0: the board has the fan until D0
#define BC250_FAN_FLAG_RESTORE_SAVED 256u   // the board's own mode and target were recorded before the first change
#define BC250_FAN_FLAG_SUBSTITUTED 512u     // that record holds the rest values: our bit was already set when it was taken
#define BC250_FAN_FLAG_HELD_BACK 1024u      // a doubt gave the fan back; the driver takes it again after 30 s clean
// Why a start's fan control is not enabled.
#define BC250_FAN_GATE_OK 0u
#define BC250_FAN_GATE_SETTING 1u           // EnableFanControl is 0
#define BC250_FAN_GATE_READER 2u            // the hardware monitor is not online (EnableHwmon 0, or a refusal)
#define BC250_FAN_GATE_CHIP 3u              // the customer ID is neither pinned (HwmonExpectId) nor unit A's 0x162B
#define BC250_FAN_GATE_COUNT 4u
typedef struct _BC250_ESCAPE_FAN {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion;
    unsigned long Op;                       // in: BC250_FAN_OP_*
    unsigned long Flags;                    // out: BC250_FAN_FLAG_*
    unsigned long Mode;                     // in: CURVE/BOARD ignore it (the Op says); out: enum bc250_fan_mode in force
    unsigned long State;                    // out: enum bc250_fan_state
    unsigned long Reason;                   // out: enum bc250_fan_reason, the last handback
    unsigned long DoubtReason;              // out: enum bc250_fan_reason, 0 without doubt
    unsigned long Profile;                  // in: CURVE; out: enum bc250_fan_profile in force
    unsigned long Points;                   // in: CURVE with PROFILE_CUSTOM; out: points of the curve in force
    unsigned long CurveC[BC250_FAN_CURVE_SLOTS];    // in/out: degrees C, rising
    unsigned long CurvePct[BC250_FAN_CURVE_SLOTS];  // in/out: duty percent, never falling, 20..100
    unsigned long FixedPct;                 // in: FIXED; out: the fixed duty in force, 0 outside FIXED
    unsigned long LeaseMs;                  // in: CURVE, FIXED, RENEW; out: what is left of the lease, 0 if durable
    unsigned long Store;                    // in: BOARD, CURVE with LeaseMs 0: 1 writes the choice to the registry
    unsigned long TargetPct;                // out: what the curve, the fixed duty or the emergency asks for
    unsigned long AppliedPct;               // out: what the slope rule let through, 0 while the board has the fan
    unsigned long WrittenRaw;               // out: the duty target written last, 0..255
    unsigned long ReadbackRaw;              // out: the duty read-back of fan 1 in the last sample, 0..255
    long GuardMc;                           // out: max(Tctl, EC SB-TSI) of the last step
    unsigned long Rpm;                      // out: tachometer of fan 1 in the last sample
    unsigned long Channel;                  // out: the fan this control drives (1, the one that turns on unit A)
    unsigned long SavedMode, SavedTarget;   // out: the board's own values, the restore record
    unsigned long Error;                    // out: enum bc250_fan_error of this request
    unsigned long StoredMode, StoredProfile;    // out: the registry's choice, when STORED
    unsigned long Gate;                     // out: BC250_FAN_GATE_*
    unsigned long long Takeovers, Handbacks, Writes, Failures;     // out: since this device object was created
    unsigned long long Emergencies, Doubts, LeaseExpiries, WatchdogFires;
    unsigned long long Generation;          // out: start-health generation of the start this describes
    unsigned long long ExpectedGeneration;  // in: every write operation
    unsigned long Reserved[2];              // zero in, zero out
} BC250_ESCAPE_FAN; // 272 bytes on Windows, ABI 1

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
// `log summary only` does, for a caller that asks for one block on request. No shipped caller polls it: the
// overlay's graphics panel reads the ring with GET_LOG pages and sends this one only for the "graphics.summary"
// action of its table (BD-054, C55).
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
                                                // Count BC250_PJ_CTX_*, Valid the VMID the IB ran at
                                                // (from 0.7.214.1; 0 before, when it was always 1). Dma unused.
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

// BC250_ESCAPE_RUN_DPAUDIO (31): DisplayPort audio, steps 0, 1 and 2 (driver/kmd/dpaudio.c, dpaudio_seq.c).
//
// OBSERVE reads the step 0 registers now and returns them by slot (BC250_DPAUDIO_OBS_LIST): the codec root and
// function parameters, DC_PINSTRAPS, the DCCG audio DTOs, for both stream encoders the DIG/DP/AFMT/HPD state,
// for both Azalia endpoints a few configuration registers read through the endpoint's INDEX/DATA pair, and the DP
// reference clock counter that step 2 sets the DTO from. The
// INDEX write of such a read selects a configuration register and changes nothing else (dce_audio.c:73-84);
// nothing else is written. STATE returns only the software record of the last start, stop, resume and path
// power change, and touches no register. Both operations fill the STATE half.
//
// Flags: exactly HardwareAccess (Flags.Value 1), as OBSERVE_DCN: the reads need dxgkrnl's Level Two exclusion
// of stop and MMIO unmap, and STATE takes the same word so that the two cannot be told apart by a sampler. An
// operator's tool, never a poller's: no shipped component sends it on a schedule. Administrators only.
//
// Two sizes (0.7.216). ABI 2 is the whole structure: the ABI 1 layout, unchanged, followed by the record of the
// stream half (step 2). ABI 1 is its first BC250_DPAUDIO_ABI1_SIZE bytes, the 0.7.215 layout, and the driver
// writes nothing past them. A size that is not its AbiVersion's is refused. A driver before 0.7.216 fails the
// ABI 2 size itself with STATUS_INVALID_PARAMETER: a tool asks with ABI 2 and repeats with ABI 1 on that answer.
// Slots, reasons and steps only grow at the end, so an ABI 1 reader keeps the meaning of every number it knows.

#define BC250_DPAUDIO_ABI 2u
#define BC250_DPAUDIO_ABI_1 1u
#define BC250_DPAUDIO_ABI1_SIZE 408u         // the ABI 1 prefix of BC250_ESCAPE_DPAUDIO (dpaudio.c checks the offset)
#define BC250_DPAUDIO_OP_OBSERVE 1u
#define BC250_DPAUDIO_OP_STATE 2u

// Observation slots, in reply order. X(slot). The driver binds each slot to its register by name
// (dpaudio_seq.c g_ObsSlots, a designated initializer per slot); the CLI prints the slot names.
#define BC250_DPAUDIO_OBS_LIST(X) \
    X(CODEC_VENDOR_DEVICE) X(CODEC_REVISION) X(SUPPORTED_SIZE_RATES) X(STREAM_FORMATS) X(POWER_STATES) \
    X(DC_PINSTRAPS) X(DTO_SOURCE) X(DTO0_PHASE) X(DTO0_MODULE) X(DTO1_PHASE) X(DTO1_MODULE) \
    X(DIG0_FE_CNTL) X(DIG0_BE_CNTL) X(DP0_VID_STREAM_CNTL) X(DP0_SEC_CNTL) X(DP0_SEC_AUD_N) \
    X(DP0_SEC_AUD_M_READBACK) X(DP0_SEC_TIMESTAMP) X(DIG0_AFMT_CNTL) X(DIG0_AFMT_AUDIO_SRC_CONTROL) \
    X(DIG0_AFMT_AUDIO_PACKET_CONTROL) X(DIG0_AFMT_AUDIO_PACKET_CONTROL2) X(DIG0_AFMT_STATUS) X(HPD0_INT_STATUS) \
    X(DIG1_FE_CNTL) X(DIG1_BE_CNTL) X(DP1_VID_STREAM_CNTL) X(DP1_SEC_CNTL) X(DP1_SEC_AUD_N) \
    X(DP1_SEC_AUD_M_READBACK) X(DP1_SEC_TIMESTAMP) X(DIG1_AFMT_CNTL) X(DIG1_AFMT_AUDIO_SRC_CONTROL) \
    X(DIG1_AFMT_AUDIO_PACKET_CONTROL) X(DIG1_AFMT_AUDIO_PACKET_CONTROL2) X(DIG1_AFMT_STATUS) X(HPD1_INT_STATUS) \
    X(EP0_CONFIG_DEFAULT) X(EP0_HOT_PLUG_CONTROL) X(EP0_PIN_SENSE) X(EP0_UNSOLICITED_RESPONSE) \
    X(EP0_WIDGET_CONTROL) X(EP0_CHANNEL_SPEAKER) X(EP0_AUDIO_DESCRIPTOR0) X(EP0_SINK_INFO1) \
    X(EP1_CONFIG_DEFAULT) X(EP1_HOT_PLUG_CONTROL) X(EP1_PIN_SENSE) X(EP1_UNSOLICITED_RESPONSE) \
    X(EP1_WIDGET_CONTROL) X(EP1_CHANNEL_SPEAKER) X(EP1_AUDIO_DESCRIPTOR0) X(EP1_SINK_INFO1) \
    X(REFCLK_COUNT) X(DIG0_AFMT_INFOFRAME_CONTROL0) X(DIG0_AFMT_60958_0) \
    X(DIG1_AFMT_INFOFRAME_CONTROL0) X(DIG1_AFMT_60958_0)

#define BC250_DPAUDIO_OBS_ENUM(n) BC250_DPAUDIO_OBS_##n,
enum bc250_dpaudio_obs { BC250_DPAUDIO_OBS_LIST(BC250_DPAUDIO_OBS_ENUM) BC250_DPAUDIO_OBS_COUNT };
#undef BC250_DPAUDIO_OBS_ENUM
#define BC250_DPAUDIO_OBS_SLOTS 64u          // room in the reply; ValidMask has one bit per slot

// Why the last start did or did not enable the endpoint. X(name, text); the text is what the log and the CLI say.
#define BC250_DPAUDIO_REASON_LIST(X) \
    X(OK, "stream on and endpoint enabled") \
    X(NOT_STARTED, "no start since the driver loaded") \
    X(SWITCH_OFF, "EnableDpAudio is not 1") \
    X(ENDPOINT_SWITCH_OFF, "EnableDpAudioEndpoint is not 1") \
    X(NO_MMIO, "BAR5 not mapped (EnableMmio)") \
    X(READ_FAILED, "a precondition register could not be read") \
    X(CODEC_ID, "codec vendor/device is not 0x1002AA01") \
    X(STRAPS, "DC_PINSTRAPS_AUDIO is 0: audio not strapped on") \
    X(NO_STREAM, "no DP stream encoder has its video stream on") \
    X(TWO_STREAMS, "both DP stream encoders have their video stream on") \
    X(NOT_DP_SST, "no DIG back end in DP SST mode is fed by that stream encoder") \
    X(CONFIG_DEFAULT, "endpoint RESPONSE_CONFIGURATION_DEFAULT is not 0x185600F0") \
    X(WRITE_FAILED, "a write of the sequence failed; stream off and AUDIO_ENABLED cleared") \
    X(STOPPED, "stopped: stream off and AUDIO_ENABLED cleared") \
    X(PATH_OFF, "monitor path powered off: stream off and AUDIO_ENABLED cleared") \
    X(STREAM_SWITCH_OFF, "EnableDpAudioStream is not 1") \
    X(REFCLK, "DP reference clock count is outside 500 to 700 MHz") \
    X(STREAM_MISMATCH, "a stream register read back another value; stream off and AUDIO_ENABLED cleared")

#define BC250_DPAUDIO_REASON_ENUM(n, t) BC250_DPAUDIO_REASON_##n,
enum bc250_dpaudio_reason { BC250_DPAUDIO_REASON_LIST(BC250_DPAUDIO_REASON_ENUM) BC250_DPAUDIO_REASON_COUNT };
#undef BC250_DPAUDIO_REASON_ENUM

#define BC250_DPAUDIO_STATE_IDLE 0u          // no start yet, or the start did not reach the decision
#define BC250_DPAUDIO_STATE_ENABLED 1u       // the stream is on and AUDIO_ENABLED is 1 on Endpoint, both by this driver
#define BC250_DPAUDIO_STATE_REFUSED 2u       // a precondition failed: nothing was written
#define BC250_DPAUDIO_STATE_FAILED 3u        // a write failed or read back wrong part way; the stop sequence ran
#define BC250_DPAUDIO_STATE_STOPPED 4u       // the stop path turned the stream off and cleared AUDIO_ENABLED
#define BC250_DPAUDIO_STATE_PATH_OFF 5u      // the monitor path is off: stream off, AUDIO_ENABLED 0 until it is back

#define BC250_DPAUDIO_NOTE_HPD_LOW 1u        // the chosen encoder's HPD sense read 0 (logged, not a refusal)
#define BC250_DPAUDIO_NOTE_INHERITED 2u      // AUDIO_ENABLED was already 1 before this start wrote anything
#define BC250_DPAUDIO_NOTE_REVISION 4u       // codec revision is not M819's 0x00100700 (logged, not a refusal)
#define BC250_DPAUDIO_NOTE_UNSOLICITED 8u    // the pin's UNSOLICITED_RESPONSE.ENABLE was set before the write (U3)

#define BC250_DPAUDIO_NO_SWITCH 0xFFFFFFFFu  // SwitchEnable/SwitchEndpoint/SwitchStream before any start read them

// ABI 2: the stream half (step 2) on the stream encoder Stream. StreamState says what this driver left there.
#define BC250_DPAUDIO_STREAM_OFF 0u          // not written by this start, or turned off by the stop sequence
#define BC250_DPAUDIO_STREAM_ON 1u           // the whole enable sequence ran and every read-back matched
#define BC250_DPAUDIO_STREAM_UNDONE 2u       // the enable sequence failed or read back wrong; the stop sequence ran

// The steps of the two stream sequences, for StreamStep: the first step that failed or read back wrong. X(name).
// The enable sequence in its order, then the stop sequence in its order (dpaudio_seq.c Bc250DpAudioStreamEnable,
// Bc250DpAudioStreamDisable). Append only: AFMT_MEM_POWER (0.7.216.4) runs first in the enable but takes the next
// number, so the numbers a tool already decodes keep their meaning.
#define BC250_DPAUDIO_STEP_LIST(X) \
    X(NONE) X(DTO_SELECT) X(DTO1_MODULE) X(DTO1_PHASE) X(DTO_512FBR) X(AFMT_CLOCK_ON) X(SRC_SELECT) \
    X(CHANNEL_ENABLE) X(AUD_N) X(TIMESTAMP) X(CS_UPDATE) X(LAYOUT_OVRD) X(INFO_UPDATE) X(CLOCK_ACCURACY) \
    X(SEC_ASP_ON) X(SEC_ATP_AIP_ON) X(SEC_STREAM_ON) X(SAMPLE_SEND_ON) \
    X(SAMPLE_SEND_OFF) X(SEC_STREAM_OFF) X(SEC_ATP_AIP_OFF) X(SEC_ASP_OFF) X(SEC_STREAM_KEEP) X(AFMT_CLOCK_OFF) \
    X(AFMT_MEM_POWER)
#define BC250_DPAUDIO_STEP_ENUM(n) BC250_DPAUDIO_STEP_##n,
enum bc250_dpaudio_step { BC250_DPAUDIO_STEP_LIST(BC250_DPAUDIO_STEP_ENUM) BC250_DPAUDIO_STEP_COUNT };
#undef BC250_DPAUDIO_STEP_ENUM

typedef struct _BC250_ESCAPE_DPAUDIO {
    unsigned long Magic, Command, Status, Version;
    unsigned long NtStatus, AbiVersion, Op, Flags;   // Flags: BC250_ESCAPE_FLAG_MMIO_MAPPED
    unsigned long long ValidMask;           // OBSERVE: bit i set when Regs[i] was read now
    unsigned long Regs[BC250_DPAUDIO_OBS_SLOTS];    // OBSERVE: slot order of BC250_DPAUDIO_OBS_LIST, 0 when not read
    // OBSERVE: the start's own decision (Bc250DpAudioDecide) over the registers above, as a start would take it
    // now. Nothing is written for it; the CLI prints it rather than repeating the logic.
    unsigned long ObsReason, ObsStream, ObsEndpoint, ObsNotes;
    // STATE: the software record (both operations).
    unsigned long State;                    // BC250_DPAUDIO_STATE_*
    unsigned long Reason;                   // enum bc250_dpaudio_reason
    unsigned long Notes;                    // BC250_DPAUDIO_NOTE_*
    unsigned long Endpoint, Stream;         // the chosen Azalia endpoint and DP stream encoder (valid from the decision)
    unsigned long SwitchEnable, SwitchEndpoint;      // EnableDpAudio, EnableDpAudioEndpoint as the last start read them
    unsigned long Starts, Resumes, Stops, Refusals, Failures, PathOn, PathOff;
    unsigned long IndirectReads, IndirectWrites, DirectWrites, AccessRefusals;  // through the checked accessors
    unsigned long CodecId;                  // VENDOR_AND_DEVICE_ID at the last decision
    unsigned long ConfigDefault;            // the chosen endpoint's RESPONSE_CONFIGURATION_DEFAULT at the last decision
    unsigned long HotPlugBefore, HotPlugAfter;      // the endpoint's HOT_PLUG_CONTROL before and after the last sequence
    unsigned long LastStatus;               // NTSTATUS of the last sequence (0 for none or success)
    unsigned long Abi1Pad;                  // 0. The ABI 1 record ended in 4 bytes of padding (ValidMask is 8-aligned)
    // ABI 2 from here (0.7.216): the stream half, step 2. 0 until a start reaches it.
    unsigned long SwitchStream;             // EnableDpAudioStream as the last start read it
    unsigned long StreamState;              // BC250_DPAUDIO_STREAM_*
    unsigned long StreamStep;               // enum bc250_dpaudio_step: the first step that failed or read back wrong
    unsigned long StreamStatus;             // NTSTATUS of that step (0 for none)
    unsigned long RefClockCount;            // CLK4_0_CLK4_CLK2_CURRENT_CNT at the last decision, 100 kHz units
    unsigned long DtoModule, DtoPhase;      // DCCG_AUDIO_DTO1_MODULE and _PHASE as the last enable read them back
    unsigned long MismatchOffset;           // BAR5 offset of the read-back that differed (0 for none)
    unsigned long MismatchExpected, MismatchActual; // the named bits written there, and the same bits read back
    unsigned long DtoSource, SecCntl, AfmtCntl;     // read back by the last stream sequence (enable or stop)
    unsigned long PacketControl, PacketControl2;    // DIGn_AFMT_AUDIO_PACKET_CONTROL and _CONTROL2, the same
    unsigned long StreamOn, StreamOff, StreamUndos; // enables that ran whole, stop sequences, undone enables
} BC250_ESCAPE_DPAUDIO; // 480 bytes on Windows, ABI 2 (ABI 1: the first 408)
typedef char BC250_ESCAPE_DPAUDIO_SIZE_CHECK[(sizeof(BC250_ESCAPE_DPAUDIO) == 480) ? 1 : -1];
typedef char BC250_DPAUDIO_OBS_FIT_CHECK[(BC250_DPAUDIO_OBS_COUNT <= BC250_DPAUDIO_OBS_SLOTS) ? 1 : -1];
