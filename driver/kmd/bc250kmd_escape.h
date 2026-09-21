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
#define BC250_KMD_VERSION 0x00070002u       // milestone 7 work, revision 2

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
    unsigned long Reserved[2];
} BC250_ESCAPE;

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
    unsigned long KindCount;
    unsigned long LastCount;
    BC250_ESCAPE_IV_KIND Kinds[BC250_IH_MAX_KINDS];     // how many of each (client, source) pair
    BC250_ESCAPE_IV Last[BC250_IH_MAX_LAST];            // the most recent vectors, oldest first
    BC250_ESCAPE_WRITE Writes[BC250_IH_MAX_WRITES];
} BC250_ESCAPE_IH;

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
#define BC250_FENCE_MAX_COUNT 1000u
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
typedef char BC250_ESCAPE_IV_SIZE_CHECK[(sizeof(BC250_ESCAPE_IV) == 48) ? 1 : -1];
typedef char BC250_ESCAPE_IH_SIZE_CHECK[(sizeof(BC250_ESCAPE_IH) == 2336) ? 1 : -1];
typedef char BC250_LOG_LINE_SIZE_CHECK[(sizeof(BC250_LOG_LINE) == 168) ? 1 : -1];
typedef char BC250_ESCAPE_LOG_SIZE_CHECK[(sizeof(BC250_ESCAPE_LOG) == 10812) ? 1 : -1];
