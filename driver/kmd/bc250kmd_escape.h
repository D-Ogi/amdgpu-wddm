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
#define BC250_KMD_VERSION 0x000700A8u       // revision 168: rearm MSI after empty IH consumption

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
// bc250kmd_cli does not send it (its `log summary` prints the whole ring, which is what an evidence file wants);
// it is here for a caller that polls.
//
// BC250_ESCAPE_LOG_SUMMARY is refused (REFUSED, STATUS_INVALID_DEVICE_REQUEST) unless D3DKMT_ESCAPE.Flags has
// HardwareAccess set and NoAdapterSynchronization clear: the summary reads state that a device stop frees, and
// that flag is what makes dxgkrnl serialize the two.
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
