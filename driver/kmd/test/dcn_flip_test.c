// Actual-source host test. MMIO model delays lock acknowledgement independently
// of the request and latches HIGH on LOW. No claim about physical DCN timing.
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "plane_format.h"
#include "dcn_translate.h"
#include "dcn_scale.h"           /* BD-223: BC250_SCALER_PLAN */
typedef unsigned long ULONG;
typedef uint64_t ULONGLONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
typedef long LONG;
typedef int64_t LONGLONG;
typedef int64_t LONG64;
typedef unsigned char UCHAR, *PUCHAR;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct { ULONGLONG Physical; ULONG Pitch; LONGLONG ProgramEndQpc; BOOLEAN Retired; } BC250_DCN_WRITER_BRIDGE;
#define BC250_DCN_WRITER_RETIRE_US 30000u
#define PASSIVE_LEVEL 0
/* BD-195: DcnLinkSurfaceAdopt's values (d3dukmdt.h, bc250kmd.h). */
#define D3DDDIFMT_A8R8G8B8 21
#define BC250_CHILD_UID 0x250001
typedef struct { NTSTATUS Status; ULONG InterruptBefore, InterruptCleared, PendingBefore, PendingAfter; BOOLEAN Fresh; } BC250_DCN_WITNESS_ARM;
typedef struct { ULONG Interrupt, Low1, High1, High2, Low2; ULONGLONG Earliest1, Earliest2; BOOLEAN Event; } BC250_DCN_WITNESS_READ;
#define FIELD_OFFSET(type, field) offsetof(type, field)
/* BD-170: bc250kmd.h's immediate-flip record, bounds and outcomes, as DcnImmediateObserve uses them. */
#define BC250_DCN_IMMEDIATE_RECORDS 8u
#define BC250_DCN_IMMEDIATE_READS 3u
#define BC250_DCN_IMMEDIATE_OBSERVE_US 50u
#define BC250_DCN_IMMEDIATE_DONE 1u
#define BC250_DCN_IMMEDIATE_DONE_LATE 2u
#define BC250_DCN_IMMEDIATE_PENDING 3u
#define BC250_DCN_IMMEDIATE_PRIMARY_OTHER 4u
#define BC250_DCN_IMMEDIATE_READ_FAILED 5u
#define BC250_DCN_IMMEDIATE_SUPERSEDED 6u
#define BC250_DCN_IMMEDIATE_OUTCOMES 7u
typedef struct {
    volatile LONG Version; LONG Sequence; ULONG Generation; ULONG Outcome; ULONG Flags;
    ULONGLONG CardAddress; ULONGLONG Physical; ULONGLONG Observed; LONG ReadStatus; ULONG Samples;
    ULONG Frame; ULONG PositionBefore; ULONG PositionAfter; ULONG Control;
    LONG ProgramUs; LONG FirstUs; LONG ClearUs; LONG DoneUs; LONG EndUs;
} BC250_DCN_IMMEDIATE_RECORD;
/* Modeset.PipeChanged: a committed scaled display mode (modeset.c); the escape refuses while it is set.
   M15.14: the plane format state DcnCaptureFirmwareFormat fills and DcnPlaneFormatRegisters reads. */
typedef struct { volatile long DcnLockTimeouts; int VidPnFlipEnabled;
    struct { BOOLEAN PipeChanged, InheritedFixed; ULONG InheritedSupport; } Modeset;
    void *Mmio;
    BOOLEAN DcnWriteEnabled, VramEnabled, DcnFirmwareKnown, DcnDiverged;
    ULONG SourceWidth, SourceHeight;
    struct { ULONG Width, Height, Pitch, ColorFormat, TargetId; LARGE_INTEGER PhysicAddress; } Post, RuntimePost;
    void *Framebuffer, *RuntimeFramebuffer;
    ULONGLONG RuntimeFramebufferLength;
    LARGE_INTEGER RuntimeFramebufferPhysical;
    volatile LONG RuntimeDisplayChanged, RuntimePostValid;
    LARGE_INTEGER VramPhysical;
    ULONGLONG VramMcBase, VramLength, FramebufferLength, DcnFirmwareAddress, DcnCurrentAddress;
    ULONG DcnFirmwarePitch, DcnCurrentPitch;
    volatile LONG DcnFormatRefused, DcnFlipRefused, DcnFormatChanges, DcnFlipsHardware, DcnSurfaceSequence;
    ULONG DcnFirmwareSurfaceConfig, DcnFirmwareHubpretControl, DcnFirmwareCnvcFormat;
    BOOLEAN DcnPlaneFormats; ULONG DcnPlaneFormat;
    BOOLEAN DcnPipeConnector; ULONG DcnPipeDetPlane1;   /* BD-197: the connector commit's DET plane-1 base */
    volatile LONG DcnImmediateFlips, DcnImmediateSameFrame;
    volatile LONG DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_OUTCOMES];
    volatile LONG DcnImmediateDoneMaxUs, DcnImmediateObserveMaxUs, DcnImmediateOverruns;
    volatile LONG64 DcnImmediateDoneSumUs;
    volatile LONG DcnImmediateLastFrame, DcnImmediateRecordNext;
    LONGLONG DcnQpcFrequency;
    BC250_DCN_IMMEDIATE_RECORD DcnImmediateRecords[BC250_DCN_IMMEDIATE_RECORDS]; } BC250_DEVICE;
static void TestGuardLog(const char *format, ...);
#define GuardLog TestGuardLog
static ULONG DisplaySourceWidth(const BC250_DEVICE *d) { return d->SourceWidth; }
static ULONG DisplaySourceHeight(const BC250_DEVICE *d) { return d->SourceHeight; }
static long InterlockedIncrement(volatile long *value) { return ++*value; }
static long InterlockedExchange(volatile long *p, long v) { long old = *p; *p = v; return old; }
static long InterlockedCompareExchange(volatile long *p, long v, long c) { long old = *p; if (old == c) *p = v; return old; }
static LONG64 InterlockedExchangeAdd64(volatile LONG64 *p, LONG64 v) { LONG64 old = *p; *p += v; return old; }
static void TestCopy(void *to, const void *from, size_t bytes);
#define RtlCopyMemory(to, from, bytes) TestCopy((to), (from), (bytes))
#define TRUE 1
#define FALSE 0
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011u)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184u)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBu)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022u)
#define STATUS_DEVICE_CONFIGURATION_ERROR ((NTSTATUS)0xC0000182u)
#define BC250_KMD_VERSION 1
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef struct {
    unsigned long Version, NtStatus, InUseBefore, InUseAfter, FrameCountBefore, FrameCountAfter;
    unsigned long FlipPendingCleared, WaitUs, DchubpCntl, Underflow, Restore, Fill, FillColor;
    ULONGLONG FirmwareAddress, AddressBefore, AddressAfter, Physical;
    char Reason[128];
} BC250_ESCAPE_DCNFLIP;
static unsigned escape_calls;
static void Refuse(BC250_ESCAPE_DCNFLIP *data, NTSTATUS status, const char *why)
{ data->NtStatus=(unsigned long)status; data->Reason[0]=why[0]; }
static void DcnFlipCore(BC250_DEVICE *d, ULONGLONG address, BOOLEAN restore, ULONG fill, ULONG color, BC250_ESCAPE_DCNFLIP *out)
{ (void)d;(void)address;(void)restore;(void)fill;(void)color;(void)out;escape_calls++; }
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define NT_SUCCESS(s) ((s) >= 0)
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static unsigned protected_call, protected_logs;
static void TestGuardLog(const char *format, ...)
{
    (void)format;
    if (protected_call) protected_logs++;
    CHECK(!protected_call);
}
static struct {
    unsigned ack_after, stalls, reads, writes, address_writes, triggers, format_writes, capture_reads;
    int locked, acknowledged, quiet;
    ULONG flip, surface, pitch, high;
    ULONG config, crossbar, cnvc, control, mpcc, lut;   /* M15.14: the plane format registers */
    ULONGLONG latched;
    /* BD-170, the observation after an immediate flip: QPC at 10 MHz, moved 1 us by each stall; the pending bit
       stays set for pending_reads FLIP_CONTROL reads (~0u: for ever); frame and position as given; the
       PRIMARY_SURFACE_ADDRESS pair names primary_old for primary_lag samples (~0u: for ever) and primary after them,
       its high word carries high_noise outside the field, the first primary_fails high-word reads fail and the first
       low_fails low-word reads fail; a frozen counter and a failing FLIP_CONTROL read for the bound tests; generation
       is the primary generation, moved by another writer at observe read number supersede_at (0: never); each
       FLIP_CONTROL read moves the QPC by read_cost ticks (the time a sample takes). */
    int observing, frozen, flip_read_fails, high_read;
    unsigned pending_reads, observe_reads, observe_stalls, primary_lag, primary_fails, low_fails, primary_reads;
    unsigned supersede_at, read_cost, stall_cost, earliest_lag, earliest_reads;
    int witness, witness_clear_sticky;
    unsigned witness_reads, witness_writes, witness_fail_at, witness_pending_reads;
    ULONG witness_before, witness_after, witness_interrupt;
    int capturing, bridge, retiring, irql;
    unsigned retire_stalls, retire_samples, bridge_read_cost;
    LONGLONG earliest_ready_qpc, delay_cost;
    LONGLONG qpc;
    ULONG frame, position, high_noise;
    ULONGLONG primary, primary_old, sample, earliest_old;
    volatile LONG generation;
} model;
static int KeGetCurrentIrql(void) { return model.irql; }
static unsigned slot_copies;
static void TestCopy(void *to, const void *from, size_t bytes)
{
    /* The record's sequence lock: the one copy into a slot happens while the slot's Version is odd. */
    const BC250_DCN_IMMEDIATE_RECORD *slot =
        (const BC250_DCN_IMMEDIATE_RECORD *)((const unsigned char *)to - offsetof(BC250_DCN_IMMEDIATE_RECORD, Sequence));
    CHECK((slot->Version & 1) == 1);
    CHECK(bytes == sizeof(*slot) - offsetof(BC250_DCN_IMMEDIATE_RECORD, Sequence));
    slot_copies++;
    memcpy(to, from, bytes);
}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *frequency)
{
    LARGE_INTEGER now;
    /* The observation takes the frequency MmioStart cached (Device->DcnQpcFrequency), not one per call. */
    if (model.observing) CHECK(frequency == NULL);
    if (frequency) frequency->QuadPart = 10000000;
    now.QuadPart = model.qpc;
    return now;
}
static NTSTATUS MmioDcnRead(const BC250_DEVICE *d, ULONG reg, ULONG *value)
{
    (void)d;
    if (model.witness) {
        model.witness_reads++;
        if (model.witness_fail_at == model.witness_reads) return STATUS_IO_TIMEOUT;
        if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL) {
            *value = model.witness_pending_reads++ ? model.witness_after : model.witness_before;
        } else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT) {
            *value = model.witness_interrupt;
        } else { CHECK(0); return STATUS_INVALID_PARAMETER; }
        return STATUS_SUCCESS;
    }
    if (model.bridge && !model.locked && !model.observing) {
        if (!model.frozen && reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL)
            model.qpc += model.bridge_read_cost;
        switch (reg) {
        case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL: *value=model.flip; break;
        case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE: *value=(ULONG)model.latched; break;
        case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH: *value=(ULONG)(model.latched >> 32); break;
        case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS: *value=(ULONG)model.latched; break;
        case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH: *value=(ULONG)(model.latched >> 32); break;
        case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH: *value=model.pitch; break;
        case BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG: *value=model.config; break;
        case BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL: *value=model.crossbar; break;
        case BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT: *value=model.cnvc; break;
        default: CHECK(0); return STATUS_INVALID_PARAMETER;
        }
        return STATUS_SUCCESS;
    }
    if (model.capturing && !model.locked) {
        if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS) *value = (ULONG)model.latched;
        else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) *value = (ULONG)(model.latched >> 32);
        else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH) *value = model.pitch;
        else { CHECK(0); return STATUS_INVALID_PARAMETER; }
        return STATUS_SUCCESS;
    }
    /* DcnCaptureFirmwareFormat's reads: once per start, outside any flip. */
    if (reg == BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG || reg == BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL ||
        reg == BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT || reg == BC250_REG_DMU_CNVC_CFG0_FORMAT_CONTROL ||
        reg == BC250_REG_DMU_MPCC0_MPCC_CONTROL || reg == BC250_REG_DMU_CNVC_CFG0_ALPHA_2BIT_LUT) {
        CHECK(!model.locked);
        model.capture_reads++;
        *value = reg == BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG ? model.config :
            reg == BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL ? model.crossbar :
            reg == BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT ? model.cnvc :
            reg == BC250_REG_DMU_CNVC_CFG0_FORMAT_CONTROL ? model.control :
            reg == BC250_REG_DMU_MPCC0_MPCC_CONTROL ? model.mpcc : model.lut;
        return STATUS_SUCCESS;
    }
    if (model.observing) {
        /* The observation reads with no lock held and writes nothing. */
        CHECK(!model.locked);
        model.observe_reads++;
        if (model.supersede_at && model.observe_reads == model.supersede_at) model.generation += 2;
        if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL) {
            if (model.retiring) { CHECK(!protected_call && model.irql == PASSIVE_LEVEL); model.retire_samples++; }
            if (!model.frozen) model.qpc += model.read_cost;
            if (model.flip_read_fails) return STATUS_IO_TIMEOUT;
            *value = model.flip | (model.pending_reads ? HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK : 0);
            if (model.pending_reads && model.pending_reads != ~0u) model.pending_reads--;
        } else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) {
            /* One sample is one pair, high word first. */
            CHECK(!model.high_read);
            model.primary_reads++;
            if (model.primary_fails) { if (model.primary_fails != ~0u) model.primary_fails--; return STATUS_DEVICE_NOT_READY; }
            model.sample = model.primary_lag ? model.primary_old : model.primary;
            if (model.primary_lag && model.primary_lag != ~0u) model.primary_lag--;
            model.high_read = 1;
            *value = (ULONG)(model.sample >> 32) | model.high_noise;
        } else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS) {
            CHECK(model.high_read);
            model.high_read = 0;
            if (model.low_fails) { if (model.low_fails != ~0u) model.low_fails--; return STATUS_IO_TIMEOUT; }
            *value = (ULONG)model.sample;
        } else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH) {
            *value = (ULONG)(((model.earliest_lag || model.qpc < model.earliest_ready_qpc) ? model.earliest_old : model.latched) >> 32);
        } else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE) {
            *value = (ULONG)((model.earliest_lag || model.qpc < model.earliest_ready_qpc) ? model.earliest_old : model.latched);
            model.earliest_reads++;
            if (model.earliest_lag && model.earliest_lag != ~0u) model.earliest_lag--;
        } else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH) *value=model.pitch;
        else if (reg == BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT) *value = model.frame;
        else { CHECK(reg == BC250_REG_DMU_OTG0_OTG_STATUS_POSITION); *value = model.position; }
        return STATUS_SUCCESS;
    }
    if (reg == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) {
        CHECK(model.locked);
        model.reads++;
        model.acknowledged = model.stalls >= model.ack_after;
        *value = 1 | (model.acknowledged ? OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK : 0);
    } else {
        CHECK(reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL ||
              reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL ||
              reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH);
        CHECK(model.locked && model.acknowledged);
        *value = reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL ? model.flip :
            reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL ? model.surface : model.pitch;
    }
    return STATUS_SUCCESS;
}
static void KeStallExecutionProcessor(ULONG us)
{
    if (model.retiring) {
        CHECK(!protected_call && model.irql == PASSIVE_LEVEL && !model.locked);
        CHECK(us != 0 && us <= 50);
        model.retire_stalls++;
        if (!model.frozen) model.qpc += model.delay_cost ? model.delay_cost : (LONGLONG)us * 10;
        return;
    }
    CHECK(us == 1);
    if (model.observing) {
        CHECK(!model.locked);
        model.observe_stalls++;
        if (!model.frozen) model.qpc += model.stall_cost ? model.stall_cost : 10;
        return;
    }
    CHECK(model.locked && !model.acknowledged);
    model.stalls += us;
}
static NTSTATUS MmioDcnWriteEx(const BC250_DEVICE *d, ULONG reg, ULONG value, BOOLEAN quiet)
{
    (void)d;
    if (model.witness) {
        const ULONG events = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK |
                             HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_OCCURRED_MASK;
        const ULONG clears = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_CLEAR_MASK |
                             HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_CLEAR_MASK;
        const ULONG enables = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_MASK_MASK |
                              HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_TYPE_MASK |
                              HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_INT_MASK_MASK |
                              HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_AWAY_INT_TYPE_MASK;
        CHECK(reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT && quiet);
        CHECK((value & enables) == (model.witness_interrupt & enables));
        CHECK(!(value & ~(enables | clears)));
        model.witness_writes++;
        model.witness_interrupt = (model.witness_interrupt & events) | value;
        if (!model.witness_clear_sticky) model.witness_interrupt &= ~(events | clears);
        return STATUS_SUCCESS;
    }
    CHECK(quiet == model.quiet);
    CHECK(!model.observing);
    model.writes++;
    if (reg == BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK) {
        CHECK(value == 0 || value == 1);
        model.locked = value != 0;
    } else if (reg == BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG) {
        CHECK(!model.locked);
        CHECK(model.address_writes == 2);
        CHECK(value == 1);
        model.triggers++;
        if (model.bridge) { model.observing=1; model.primary=model.latched; }
    } else {
        // Catch address/control publication before the hardware acknowledged the lock.
        CHECK(model.locked && model.acknowledged);
        if (reg == BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG || reg == BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL ||
            reg == BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT) {
            /* M15.14: the format latches with the address, so it is written before either address half. */
            CHECK(model.address_writes == 0);
            model.format_writes++;
            if (reg == BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG) model.config = value;
            else if (reg == BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL) model.crossbar = value;
            else model.cnvc = value;
        }
        else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL) model.flip = value;
        else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL) model.surface = value;
        else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH) model.pitch = value;
        else if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH) {
            CHECK(model.address_writes == 0);
            model.high = value;
            model.address_writes++;
        } else {
            CHECK(reg == BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS);
            CHECK(model.address_writes == 1);
            model.latched = ((ULONGLONG)model.high << 32) | value;
            model.address_writes++;
        }
    }
    return STATUS_SUCCESS;
}
/* BD-223: the pipe's shape. DcnPlaneShapeLocked runs inside the acknowledged lock, before either address half,
   and once per write sequence; DcnPlaneShapeAdmit answers as shape_model.admit says. dcn_scale_test.c tests the
   register writes of the shape itself (Bc250ScalerProgramLocked). */
static struct { unsigned calls, shaped, admits; int admit; NTSTATUS status; const BC250_SCALER_PLAN *last; } shape_model;
static NTSTATUS DcnPlaneShapeLocked(BC250_DEVICE *d, const BC250_SCALER_PLAN *shape)
{
    (void)d;
    CHECK(model.locked && model.acknowledged && model.address_writes == 0);
    shape_model.calls++;
    shape_model.last = shape;
    if (shape) shape_model.shaped++;
    return shape_model.status;
}
static BOOLEAN DcnPlaneShapeAdmit(BC250_DEVICE *d, const BC250_SCALER_PLAN *shape)
{
    (void)d; (void)shape;
    CHECK(!model.locked && model.writes == 0);
    shape_model.admits++;
    return shape_model.admit;
}
/* ACTUAL_SOURCE */
/* BD-170: the flip type. Every bit of DCSURF_FLIP_CONTROL is kept, the stereo-sync fields included, and only
   SURFACE_FLIP_TYPE follows the request: 1 for an immediate flip, 0 for a vsync flip (the value 0.7.216.64 wrote).
   An unacknowledged lock writes no flip type at all. */
static void FlipTypes(void)
{
    BC250_DEVICE device = {0};
    const ULONGLONG target = UINT64_C(0x123456789ABC000);
    unsigned bit, delay;
    int immediate;
    for (immediate = 0; immediate <= 1; immediate++)
        for (delay = 0; delay <= 10; delay += 5)
            for (bit = 0; bit < 32; bit++) {
                const ULONG original = 1ul << bit;
                memset(&model, 0, sizeof(model));
                model.ack_after = delay; model.flip = original; model.quiet = 1;
                CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, NULL, immediate, TRUE) == STATUS_SUCCESS);
                CHECK(model.flip == (immediate ? (original | HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK) :
                                                 (original & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK)));
                CHECK(model.latched == target && model.triggers == 1 && !model.locked && model.writes == 8);
            }
    memset(&model, 0, sizeof(model));
    model.ack_after = 11; model.flip = 0; model.quiet = 1;
    CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, NULL, TRUE, TRUE) == STATUS_IO_TIMEOUT);
    CHECK(model.flip == 0 && model.address_writes == 0 && !model.locked);
}
/* BD-170 r6: DcnImmediateObserve. P' = SURFACE_FLIP_PENDING first observed clear and DCSURF_PRIMARY_SURFACE_ADDRESS,
   read high word first and masked to its fields, equal to the physical request in the same widths, at one sample,
   with the primary generation the same at the start and at the end. The bound is 50 us of the QPC counted from the
   end of the programming, and 50 stalls; the outcome says what ended it; the last eight records are kept, each
   written whole under its sequence lock; no write, no lock, no wait for a refresh. QPC is 10 MHz here, so 10 ticks
   are 1 us. */
static void ObserveReset(void)
{
    memset(&model, 0, sizeof(model));
    model.observing = 1; model.generation = 4; model.frame = 0x1234; model.position = 0x00450000;
    model.flip = HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK;
}
static void ImmediateObserve(void)
{
    BC250_DEVICE d = {0};
    const BC250_DCN_IMMEDIATE_RECORD *r;
    LONG outcomes[BC250_DCN_IMMEDIATE_OUTCOMES] = {0};
    unsigned n;
    d.Mmio = &model;
    d.DcnQpcFrequency = 10000000;
    slot_copies = 0;

    /* P' at the third sample: the pending bit clears there, and the address register holds the request from the
       start (the programming wrote it). The programming took 0.3 us and the call follows it at once. The record keeps
       the card address of the DDI and the physical request. */
    ObserveReset();
    model.qpc = 1003; model.pending_reads = 2; model.primary = 0xA000;
    CHECK(DcnImmediateObserve(&d, 0x8000A000, 0xA000, 1000, 1003, &model.generation, 4, 0x86u) == BC250_DCN_IMMEDIATE_DONE);
    outcomes[BC250_DCN_IMMEDIATE_DONE]++;
    r = &d.DcnImmediateRecords[0];
    CHECK(model.observe_stalls == 2 && model.primary_reads == 3 && slot_copies == 1);
    CHECK(r->Version == 2 && r->Sequence == 1 && r->Generation == 4 && r->Outcome == BC250_DCN_IMMEDIATE_DONE);
    CHECK(r->Flags == 0x86u);   /* the request's flags as they came: FlipImmediate, FlipOnNextVSync, IndependentFlipExclusive */
    CHECK(r->CardAddress == 0x8000A000 && r->Physical == 0xA000 && r->Observed == 0xA000);
    CHECK(r->ReadStatus == 0 && r->Samples == 3);
    CHECK(r->Frame == 0x1234 && r->PositionBefore == 0x00450000 && r->PositionAfter == 0x00450000);
    CHECK(r->Control == (HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK |
                         HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK));
    CHECK(r->ProgramUs == 0 && r->FirstUs == 0 && r->ClearUs == 2 && r->DoneUs == 2 && r->EndUs == 2);
    CHECK(d.DcnImmediateDoneMaxUs == 2 && d.DcnImmediateDoneSumUs == 2 && d.DcnImmediateObserveMaxUs == 2);
    CHECK(d.DcnImmediateSameFrame == 0 && d.DcnImmediateOverruns == 0);

    /* The pending bit clear at once, the address register on another surface for two samples: not P' until the
       third. The same OTG frame as the flip before it. */
    ObserveReset();
    model.qpc = 2000; model.primary_old = 0xA000; model.primary = 0xB000; model.primary_lag = 2;
    CHECK(DcnImmediateObserve(&d, 0xB000, 0xB000, 2000, 2000, &model.generation, 4, 0x2u) == BC250_DCN_IMMEDIATE_DONE);
    outcomes[BC250_DCN_IMMEDIATE_DONE]++;
    r = &d.DcnImmediateRecords[1];
    CHECK(r->Sequence == 2 && r->ClearUs == 0 && r->DoneUs == 2 && r->Samples == 3 && r->Observed == 0xB000);
    CHECK(r->Flags == 0x2u);
    CHECK(model.observe_stalls == 2 && d.DcnImmediateSameFrame == 1 && d.DcnImmediateDoneSumUs == 4);

    /* The pending bit clear, the address register never on the request: PRIMARY_OTHER at the bound, never P'. The
       record keeps what the register held. */
    ObserveReset();
    model.qpc = 3000; model.frame = 0x1235; model.primary_old = 0xB000; model.primary = 0xC000; model.primary_lag = ~0u;
    CHECK(DcnImmediateObserve(&d, 0xC000, 0xC000, 3000, 3000, &model.generation, 4, 0x2u) ==
          BC250_DCN_IMMEDIATE_PRIMARY_OTHER);
    outcomes[BC250_DCN_IMMEDIATE_PRIMARY_OTHER]++;
    r = &d.DcnImmediateRecords[2];
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && r->Samples == BC250_DCN_IMMEDIATE_OBSERVE_US);
    CHECK(r->Observed == 0xB000 && r->ClearUs == 0 && r->DoneUs == -1 && r->EndUs == 50 && r->ReadStatus == 0);
    CHECK(d.DcnImmediateSameFrame == 1 && d.DcnImmediateDoneMaxUs == 2 && d.DcnImmediateObserveMaxUs == 50);

    /* Never clears, with the address register on the request all the time: PENDING at the bound. The address alone
       is never P'. */
    ObserveReset();
    model.qpc = 4000; model.pending_reads = ~0u; model.primary = 0xD000;
    CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 4000, 4000, &model.generation, 4, 0x2u) == BC250_DCN_IMMEDIATE_PENDING);
    outcomes[BC250_DCN_IMMEDIATE_PENDING]++;
    r = &d.DcnImmediateRecords[3];
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && model.primary_reads == BC250_DCN_IMMEDIATE_OBSERVE_US);
    CHECK(r->ClearUs == -1 && r->DoneUs == -1 && r->Observed == 0xD000 && r->EndUs == 50);

    /* The bound counts from the end of the programming: a 40 us programming leaves the observation its 50 us. */
    ObserveReset();
    model.qpc = 5400; model.pending_reads = ~0u;
    CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 5000, 5400, &model.generation, 4, 0x2u) == BC250_DCN_IMMEDIATE_PENDING);
    outcomes[BC250_DCN_IMMEDIATE_PENDING]++;
    r = &d.DcnImmediateRecords[4];
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && r->ProgramUs == 40 && r->FirstUs == 40);
    CHECK(r->EndUs == 90 && d.DcnImmediateObserveMaxUs == 90);

    /* A counter that does not move cannot stretch it: the step count ends it as well. */
    ObserveReset();
    model.qpc = 6000; model.pending_reads = ~0u; model.frozen = 1;
    CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 6000, 6000, &model.generation, 4, 0x2u) == BC250_DCN_IMMEDIATE_PENDING);
    outcomes[BC250_DCN_IMMEDIATE_PENDING]++;
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && d.DcnImmediateRecords[5].EndUs == 0);

    /* A FLIP_CONTROL that cannot be read is not taken as clear, and the address register is not read after it. */
    ObserveReset();
    model.qpc = 7000; model.flip_read_fails = 1; model.primary = 0xD000;
    CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 7000, 7000, &model.generation, 4, 0x2u) ==
          BC250_DCN_IMMEDIATE_READ_FAILED);
    outcomes[BC250_DCN_IMMEDIATE_READ_FAILED]++;
    r = &d.DcnImmediateRecords[6];
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && model.primary_reads == 0);
    CHECK(r->ReadStatus == STATUS_IO_TIMEOUT && r->Observed == 0 && r->Control == 0 && r->ClearUs == -1);

    /* An address register that cannot be read is not a match, with the pending bit clear. */
    ObserveReset();
    model.qpc = 8000; model.primary = 0xD000; model.primary_fails = ~0u;
    CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 8000, 8000, &model.generation, 4, 0x2u) ==
          BC250_DCN_IMMEDIATE_READ_FAILED);
    outcomes[BC250_DCN_IMMEDIATE_READ_FAILED]++;
    r = &d.DcnImmediateRecords[7];
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && r->ReadStatus == STATUS_DEVICE_NOT_READY);
    CHECK(r->Observed == 0 && r->ClearUs == 0 && r->DoneUs == -1);
    /* A failure that passes is not sticky: two failed reads, then the request. */
    ObserveReset();
    model.qpc = 9000; model.primary = 0xD000; model.primary_fails = 2;
    CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 9000, 9000, &model.generation, 4, 0x2u) == BC250_DCN_IMMEDIATE_DONE);
    outcomes[BC250_DCN_IMMEDIATE_DONE]++;
    r = &d.DcnImmediateRecords[8 % BC250_DCN_IMMEDIATE_RECORDS];
    CHECK(r->Sequence == 9 && r->Version == 4 && r->DoneUs == 2 && r->ReadStatus == 0);

    /* A first sample already past the bound (a preempted caller): P' all the same, as DONE_LATE, whose time is an
       upper bound only and stays out of the done maximum and sum. */
    ObserveReset();
    model.qpc = 10600; model.primary = 0xE000;
    CHECK(DcnImmediateObserve(&d, 0xE000, 0xE000, 10000, 10003, &model.generation, 4, 0x2u) ==
          BC250_DCN_IMMEDIATE_DONE_LATE);
    outcomes[BC250_DCN_IMMEDIATE_DONE_LATE]++;
    r = &d.DcnImmediateRecords[9 % BC250_DCN_IMMEDIATE_RECORDS];
    CHECK(r->FirstUs == 60 && r->DoneUs == 60 && model.observe_stalls == 0);
    CHECK(d.DcnImmediateDoneMaxUs == 2 && d.DcnImmediateDoneSumUs == 6 && d.DcnImmediateObserveMaxUs == 90);
    /* The same, not P': one sample and no wait. */
    ObserveReset();
    model.qpc = 11600; model.pending_reads = ~0u; model.primary = 0xE000;
    CHECK(DcnImmediateObserve(&d, 0xE000, 0xE000, 11000, 11003, &model.generation, 4, 0x2u) ==
          BC250_DCN_IMMEDIATE_PENDING);
    outcomes[BC250_DCN_IMMEDIATE_PENDING]++;
    CHECK(model.observe_stalls == 0 && d.DcnImmediateRecords[10 % BC250_DCN_IMMEDIATE_RECORDS].Samples == 1);
    /* Both ended past the bound and are counted; the observations that ended at 50 us are not. */
    CHECK(d.DcnImmediateOverruns == 2);

    /* Another writer moved the primary generation before the observation started: no read, SUPERSEDED. */
    ObserveReset();
    model.qpc = 12000; model.generation = 6; model.primary = 0xF000;
    CHECK(DcnImmediateObserve(&d, 0xF000, 0xF000, 12000, 12000, &model.generation, 4, 0x2u) ==
          BC250_DCN_IMMEDIATE_SUPERSEDED);
    outcomes[BC250_DCN_IMMEDIATE_SUPERSEDED]++;
    r = &d.DcnImmediateRecords[11 % BC250_DCN_IMMEDIATE_RECORDS];
    CHECK(model.observe_reads == 0 && model.primary_reads == 0 && r->Samples == 0 && r->Sequence == 12);
    /* Same frame as the observation before it: all but the first and the two around the frame change; this one,
       with no sample, does not take part. */
    CHECK(d.DcnImmediateSameFrame == 8);
    /* ... or during it: the predicate held, and is still not this request's witness. */
    ObserveReset();
    model.qpc = 13000; model.primary = 0xF000; model.supersede_at = 2;
    CHECK(DcnImmediateObserve(&d, 0xF000, 0xF000, 13000, 13000, &model.generation, 4, 0x2u) ==
          BC250_DCN_IMMEDIATE_SUPERSEDED);
    outcomes[BC250_DCN_IMMEDIATE_SUPERSEDED]++;
    r = &d.DcnImmediateRecords[12 % BC250_DCN_IMMEDIATE_RECORDS];
    CHECK(r->DoneUs == 0 && r->Samples == 1 && r->Generation == 4);

    /* Every outcome counted once per observation, and the ring keeps the last eight, each slot naming its own
       flip with an even Version (two increments per write). */
    for (n = 0; n < BC250_DCN_IMMEDIATE_OUTCOMES; n++) CHECK(d.DcnImmediateOutcomes[n] == outcomes[n]);
    CHECK(d.DcnImmediateRecordNext == 13 && slot_copies == 13);
    for (n = 0; n < BC250_DCN_IMMEDIATE_RECORDS; n++) {
        r = &d.DcnImmediateRecords[n];
        CHECK(r->Sequence >= 6 && (ULONG)(r->Sequence - 1) % BC250_DCN_IMMEDIATE_RECORDS == n);
        CHECK((r->Version & 1) == 0 && r->Version == 2 * (LONG)(1 + (r->Sequence - 1) / BC250_DCN_IMMEDIATE_RECORDS));
    }

    /* No BAR5: nothing read, and the observation still says so: READ_FAILED, recorded. */
    {
        BC250_DEVICE none = {0};
        ObserveReset();
        CHECK(DcnImmediateObserve(&none, 0xA000, 0xA000, 0, 0, &model.generation, 4, 0x2u) ==
              BC250_DCN_IMMEDIATE_READ_FAILED);
        CHECK(model.observe_reads == 0 && model.primary_reads == 0 && none.DcnImmediateRecordNext == 1);
        CHECK(none.DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_READ_FAILED] == 1 && none.DcnImmediateRecords[0].Samples == 0);
        CHECK(none.DcnImmediateRecords[0].ReadStatus == STATUS_DEVICE_NOT_READY);
    }
    /* No frequency cached (0): every time reads 0 us, and the step count alone still ends the observation. */
    {
        BC250_DEVICE nofrequency = {0};
        nofrequency.Mmio = &model;
        ObserveReset();
        model.qpc = 14000; model.pending_reads = ~0u;
        CHECK(DcnImmediateObserve(&nofrequency, 0xD000, 0xD000, 13000, 14000, &model.generation, 4, 0x2u) ==
              BC250_DCN_IMMEDIATE_PENDING);
        r = &nofrequency.DcnImmediateRecords[0];
        CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && r->Samples == BC250_DCN_IMMEDIATE_OBSERVE_US);
        CHECK(r->ProgramUs == 0 && r->FirstUs == 0 && r->EndUs == 0);
    }
    /* The widths. The request and the readback are compared in the fields' widths (the high field has 16 bits), so
       bits the register cannot hold never make a request unprovable, bits outside the field in a readback never make a
       match, and a difference inside the field is never P'. A low word that cannot be read is no match either. */
    {
        BC250_DEVICE widths = {0};
        widths.Mmio = &model; widths.DcnQpcFrequency = 10000000;
        ObserveReset();
        model.qpc = 20000; model.primary = UINT64_C(0x00001234000A0000); model.high_noise = 0xABCD0000ul;
        CHECK(DcnImmediateObserve(&widths, 0x1, UINT64_C(0xFFFF1234000A0000), 20000, 20000, &model.generation, 4, 0x2u) ==
              BC250_DCN_IMMEDIATE_DONE);
        r = &widths.DcnImmediateRecords[0];
        CHECK(r->Physical == UINT64_C(0x00001234000A0000) && r->Observed == UINT64_C(0x00001234000A0000));
        ObserveReset();
        model.qpc = 21000; model.primary = UINT64_C(0x00001235000A0000);
        CHECK(DcnImmediateObserve(&widths, 0x1, UINT64_C(0x00001234000A0000), 21000, 21000, &model.generation, 4, 0x2u) ==
              BC250_DCN_IMMEDIATE_PRIMARY_OTHER);
        ObserveReset();
        model.qpc = 22000; model.primary = 0xA000; model.low_fails = ~0u;
        CHECK(DcnImmediateObserve(&widths, 0xA000, 0xA000, 22000, 22000, &model.generation, 4, 0x2u) ==
              BC250_DCN_IMMEDIATE_READ_FAILED);
        r = &widths.DcnImmediateRecords[2];
        CHECK(r->ReadStatus == STATUS_IO_TIMEOUT && r->Observed == 0 && r->ClearUs == 0 && r->DoneUs == -1);
    }
    /* The bound is held after the witness reads. A sample takes 2.5 us here and a stall 1 us, so a sample starts every
       3.5 us: the fifteenth ends at 51.5 us and no sixteenth starts, although that one would start at 52.5 us and a
       bound held only at the sample start would stall again. P' at a sample that started inside the bound and ended past it is
       P', not DONE_LATE, and the overrun is counted. */
    {
        BC250_DEVICE late = {0};
        late.Mmio = &model; late.DcnQpcFrequency = 10000000;
        ObserveReset();
        model.qpc = 30000; model.pending_reads = ~0u; model.primary = 0xD000; model.read_cost = 25;
        CHECK(DcnImmediateObserve(&late, 0xD000, 0xD000, 30000, 30000, &model.generation, 4, 0x2u) ==
              BC250_DCN_IMMEDIATE_PENDING);
        r = &late.DcnImmediateRecords[0];
        CHECK(r->Samples == 15 && model.observe_stalls == 14 && r->FirstUs == 0 && r->EndUs == 51);
        CHECK(late.DcnImmediateOverruns == 1 && late.DcnImmediateObserveMaxUs == 51);
        ObserveReset();
        model.qpc = 40000; model.pending_reads = 14; model.primary = 0xD000; model.read_cost = 25;
        CHECK(DcnImmediateObserve(&late, 0xD000, 0xD000, 40000, 40000, &model.generation, 4, 0x2u) ==
              BC250_DCN_IMMEDIATE_DONE);
        r = &late.DcnImmediateRecords[1];
        CHECK(r->Samples == 15 && r->DoneUs == 49 && r->EndUs == 51 && late.DcnImmediateOverruns == 2);
        CHECK(late.DcnImmediateDoneMaxUs == 49);
    }
    model.observing = 0;
}
/* Review R3 regression: a 49 us failed read plus a 1 us stall used to admit another poll exactly at 50 us.
   A delayed stall covers interruption too. No second read means the outcome stays PENDING, not success. */
static void ObserveAdmissionBoundary(void)
{
    const unsigned stalls[] = {10, 600};
    unsigned n;
    for (n = 0; n < sizeof(stalls) / sizeof(stalls[0]); n++) {
        BC250_DEVICE d = {0};
        const BC250_DCN_IMMEDIATE_RECORD *r;
        d.Mmio = &model; d.DcnQpcFrequency = 10000000;
        ObserveReset();
        model.qpc = 10000; model.primary = 0xD000; model.pending_reads = 1;
        model.read_cost = 490; model.stall_cost = stalls[n];
        CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 10000, 10000, &model.generation, 4, 2) ==
              BC250_DCN_IMMEDIATE_PENDING);
        r = &d.DcnImmediateRecords[0];
        CHECK(r->Samples == 1 && model.primary_reads == 1 && model.observe_stalls == 1);
        CHECK(r->FirstUs == 0 && r->DoneUs == -1 && r->EndUs == (LONG)((490 + stalls[n]) / 10));
        CHECK(d.DcnImmediateOverruns == (stalls[n] > 10 ? 1 : 0));
    }
    model.observing = 0;
}
static void SourceDevice(BC250_DEVICE *d)
{
    memset(d, 0, sizeof(*d)); memset(&model, 0, sizeof(model));
    d->Mmio = &model; d->VidPnFlipEnabled = TRUE; d->DcnWriteEnabled = TRUE;
    d->VramEnabled = TRUE; d->VramMcBase = 0x100000; d->VramPhysical.QuadPart = 0x200000;
    d->VramLength = 0x400000;
    d->SourceWidth = d->Post.Width = d->RuntimePost.Width = 64;
    d->SourceHeight = d->Post.Height = d->RuntimePost.Height = 16;
    d->DcnFirmwarePitch = d->Post.Pitch = d->RuntimePost.Pitch = 256;
    d->DcnFirmwareAddress = (ULONGLONG)d->VramPhysical.QuadPart;
    d->Post.PhysicAddress.QuadPart = (LONGLONG)d->DcnFirmwareAddress;
    d->DcnFirmwareKnown = TRUE; d->FramebufferLength = 4096;
    d->DcnPlaneFormats = TRUE; d->DcnPlaneFormat = BC250_PLANE_FORMAT_ARGB8888;
    model.quiet = TRUE; model.pitch = 63; model.latched = d->DcnFirmwareAddress;
}
/* No stub for DcnFlipSourceAddress or its programming/validation callees. Its success, format change,
   first inherited flip and refusal paths all execute while the logging assertion represents L. */
static void SourceAddressNoLog(void)
{
    BC250_DEVICE d;
    BOOLEAN wrote;
    ULONGLONG physical;
    unsigned mode;
    protected_logs = 0; protected_call = 1;
    for (mode = 0; mode < 3; mode++) {
        SourceDevice(&d);
        if (mode == 1) { d.Modeset.InheritedFixed = TRUE; d.Modeset.InheritedSupport = 1; }
        if (mode == 2) { d.DcnFirmwareKnown = FALSE; model.capturing = 1; }
        CHECK(DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 4096, TRUE,
                                   &physical, &wrote) == STATUS_SUCCESS);
        CHECK(wrote && physical == 0x201000 && model.latched == physical && !model.locked);
        CHECK(d.DcnFlipsHardware == 1 && d.DcnSurfaceSequence == 2 && d.DcnImmediateFlips == 1);
    }
    SourceDevice(&d);
    CHECK(DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ABGR8888, 4096, FALSE,
                               &physical, &wrote) == STATUS_SUCCESS);
    CHECK(wrote && d.DcnFormatChanges == 1 && model.format_writes == 3);
    SourceDevice(&d); d.DcnPlaneFormats = FALSE;
    CHECK(DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ABGR8888, 4096, TRUE,
                               &physical, &wrote) == STATUS_NOT_SUPPORTED);
    CHECK(!wrote && d.DcnFormatRefused == 1 && model.writes == 0);
    SourceDevice(&d);
    CHECK(DcnFlipSourceAddress(&d, 0x0, 256, BC250_PLANE_FORMAT_ARGB8888, 4096, TRUE,
                               &physical, &wrote) == STATUS_ACCESS_DENIED);
    CHECK(!wrote && d.DcnFlipRefused == 1 && model.writes == 0);
    SourceDevice(&d); model.ack_after = 11;
    CHECK(DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 4096, TRUE,
                               &physical, &wrote) == STATUS_IO_TIMEOUT);
    CHECK(wrote && d.DcnFlipRefused == 1 && d.DcnSurfaceSequence == 2 && !model.locked);
    CHECK(model.address_writes == 0);
    CHECK(protected_logs == 0);
    protected_call = 0;
}
/* BD-197: the trailer's PLANE_FORMATS answer (DcnPlaneFormatsOpen) is the flip path's, in every state of the four
   facts it reads: never a promise of a format the flip refuses, never a refusal of one the flip programs. The
   firmware's format flips in every state but a runtime mode without a valid surface (bit 8), where nothing flips,
   so a refusal of the other format is about the format alone. Step 2: a runtime mode (bit 4) is the connector
   commit's own pipe and takes every format, the fixed inherited pipe (bit 2) under it included. */
static void PlaneFormatsStates(BC250_DEVICE *d, unsigned state)
{
    SourceDevice(d);
    d->DcnPlaneFormats = (state & 1) ? FALSE : TRUE;
    if (state & 2) { d->Modeset.InheritedFixed = TRUE; d->Modeset.InheritedSupport = 1; }
    if (state & 4) { d->RuntimeDisplayChanged = 1; d->RuntimePostValid = (state & 8) ? 0 : 1; }
}
static void PlaneFormatsOpenAgrees(void)
{
    BC250_DEVICE d;
    BOOLEAN wrote, open, posted;
    ULONGLONG physical;
    NTSTATUS status;
    unsigned state, opened = 0;
    for (state = 0; state < 16; state++) {
        if ((state & 8) && !(state & 4)) continue;
        posted = !(state & 8);
        PlaneFormatsStates(&d, state);
        status = DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 4096, FALSE, &physical, &wrote);
        CHECK(posted ? (status == STATUS_SUCCESS && wrote) : (status == STATUS_NOT_SUPPORTED && !wrote));
        PlaneFormatsStates(&d, state);
        open = DcnPlaneFormatsOpen(&d);
        CHECK(open == (!(state & 1) && ((state & 4) ? posted : !(state & 2))));
        opened += open ? 1 : 0;
        status = DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ABGR8888, 4096, FALSE, &physical, &wrote);
        CHECK(open ? (status == STATUS_SUCCESS && wrote && d.DcnFormatChanges == 1) :
                     (status == STATUS_NOT_SUPPORTED && !wrote && model.writes == 0));
    }
    CHECK(opened == 3);   /* the free inherited pipe, and a posted runtime mode over either inherited pipe */
}
static void WitnessQualification(void)
{
    BC250_DEVICE d;
    BC250_DCN_WITNESS_ARM arm;
    unsigned before, after, failure;
    const ULONG pending = HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
    for (before = 0; before <= 1; before++) for (after = 0; after <= 1; after++) {
        SourceDevice(&d); model.witness = 1;
        model.witness_before = before ? pending : 0; model.witness_after = after ? pending : 0;
        model.witness_interrupt = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK |
                                  HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_INT_TYPE_MASK;
        DcnWitnessArm(&d, TRUE, &arm);
        CHECK(arm.Status == STATUS_SUCCESS && arm.Fresh == (!before && !after));
        CHECK(arm.PendingBefore == model.witness_before && arm.PendingAfter == model.witness_after);
        CHECK(model.witness_pending_reads == 2 && model.witness_writes == 1);
    }
    /* A failed read at every position, including either pending read, cannot qualify a fresh arm. */
    for (failure = 1; failure <= 4; failure++) {
        SourceDevice(&d); model.witness = 1; model.witness_fail_at = failure;
        DcnWitnessArm(&d, TRUE, &arm);
        CHECK(arm.Status == STATUS_IO_TIMEOUT && !arm.Fresh);
    }
    SourceDevice(&d); model.witness = 1; model.witness_clear_sticky = 1;
    model.witness_interrupt = HUBPREQ0_DCSURF_SURFACE_FLIP_INTERRUPT__SURFACE_FLIP_OCCURRED_MASK;
    DcnWitnessArm(&d, TRUE, &arm);
    CHECK(arm.Status == STATUS_SUCCESS && !arm.Fresh && model.witness_writes == 2);
    SourceDevice(&d); model.witness = 1;
    DcnWitnessArm(&d, FALSE, &arm);
    CHECK(arm.Status == STATUS_NOT_SUPPORTED && !arm.Fresh && model.witness_reads == 0 && model.witness_writes == 0);
}
static void BridgeDevice(BC250_DEVICE *d)
{
    SourceDevice(d);
    d->Framebuffer=&model;d->RuntimeFramebuffer=&model;d->RuntimeFramebufferLength=4096;
    d->RuntimeFramebufferPhysical.QuadPart=0x202000;d->DcnQpcFrequency=10000000;
    model.bridge=1;model.generation=3;model.latched=0x203000;model.earliest_old=0x203000;
    model.config=8;model.crossbar=0x00E40000;model.cnvc=8;
}
static NTSTATUS BridgeProgram(BC250_DEVICE *d, BOOLEAN *wrote, BC250_DCN_WRITER_BRIDGE *bridge)
{
    NTSTATUS status;
    protected_call=1;model.irql=2;
    status=DcnWriterBridge(d,&model.generation,3,wrote,bridge);
    CHECK(model.retire_stalls==0 && model.observe_stalls==0 && protected_logs==0);
    protected_call=0;model.irql=PASSIVE_LEVEL;
    return status;
}
static NTSTATUS BridgeRetire(BC250_DEVICE *d, const BC250_DCN_WRITER_BRIDGE *bridge)
{
    NTSTATUS status;
    model.retiring=1;
    status=DcnWriterBridgeRetire(d,&model.generation,3,bridge);
    model.retiring=0;
    return status;
}
static void WriterBridge(void)
{
    BC250_DEVICE d;
    BC250_DCN_WRITER_BRIDGE bridge;
    BOOLEAN wrote;
    unsigned n, reads;
    BridgeDevice(&d);model.pending_reads=2;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    CHECK(wrote && !bridge.Retired && bridge.Physical==0x202000 && bridge.Pitch==256);
    CHECK(model.latched==0x202000 && model.triggers==1 && model.observe_reads==0 && model.qpc==0);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_SUCCESS && model.retire_stalls==2 && model.qpc==1000);
    CHECK(model.generation==3 && d.DcnImmediateRecordNext==0);
    // An already-effective owned bridge works without a write, including a stopped OTG or a blank HUBP.
    for(n=0;n<6;n++) {
        BridgeDevice(&d);model.latched=n&1 ? d.DcnFirmwareAddress : 0x202000;
        CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
        CHECK(!wrote && bridge.Retired && bridge.Physical==model.latched && model.writes==0 && !model.observing);
        CHECK(BridgeRetire(&d,&bridge)==STATUS_SUCCESS && model.retire_stalls==0);
        CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && !wrote);
    }
    // Capacity and ownership are checked before any register write, not just membership in the VRAM aperture.
    BridgeDevice(&d);d.RuntimeFramebufferLength=4095;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_INVALID_PARAMETER);
    CHECK(!wrote && model.writes==0);
    // Even an already-latched reserved base is unsafe when its verified extent is too short.
    BridgeDevice(&d);d.RuntimeFramebufferLength=4095;model.latched=0x202000;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_INVALID_PARAMETER);
    CHECK(!wrote && model.writes==0);
    BridgeDevice(&d);d.RuntimeFramebuffer=NULL;d.Framebuffer=NULL;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_DEVICE_NOT_READY);
    CHECK(!wrote && model.writes==0);
    BridgeDevice(&d);d.RuntimeFramebufferPhysical.QuadPart=0x700000;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_INVALID_PARAMETER);
    CHECK(!wrote && model.writes==0);
    BridgeDevice(&d);d.VramMcBase=UINT64_MAX-4095;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_INVALID_PARAMETER);
    CHECK(!wrote && model.writes==0);
    BridgeDevice(&d);d.DcnQpcFrequency=0;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_DEVICE_NOT_READY && !wrote && model.writes==0);
    BridgeDevice(&d);model.generation=5;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_DEVICE_BUSY && !wrote && model.writes==0);
    // A slow pre-write snapshot refuses admission without a write, and does not begin a second snapshot.
    BridgeDevice(&d);model.bridge_read_cost=250;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_IO_TIMEOUT && !wrote && model.writes==0 && model.qpc==500);
    // A written bridge is not retirement. Pending and EARLIEST wait outside L at PASSIVE for at most 30 ms.
    BridgeDevice(&d);model.pending_reads=~0u;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && wrote);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT);
    CHECK(model.retire_stalls==600 && model.retire_samples==600 && model.qpc==300000);
    BridgeDevice(&d);model.latched=0x202000;model.pending_reads=~0u;
    model.flip=HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && wrote);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT);
    // r6 lab counterexample: pending is clear and PRIMARY is new, but EARLIEST still names the old application.
    // Programming succeeds; the writer cannot proceed while the old allocation remains eligible for scanout.
    BridgeDevice(&d);model.earliest_lag=~0u;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && wrote && model.earliest_reads==0);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT);
    CHECK(model.primary==0x202000 && model.earliest_old==0x203000 && model.earliest_reads>0);
    CHECK(model.retire_stalls==600 && model.qpc==300000);
    // One full frame is a normal positive control, not a 50 us fault. Reads before 16.7 ms still return A.
    BridgeDevice(&d);model.earliest_ready_qpc=167000;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && wrote && model.qpc==0);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_SUCCESS);
    CHECK(model.qpc==167000 && model.retire_stalls==334 && model.earliest_reads==670);
    // Q remains required as well as EARLIEST. An old primary cannot pass with an already-new effective address.
    BridgeDevice(&d);model.primary_old=0x203000;model.primary_lag=~0u;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && wrote);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT);
    // A failing MMIO path and a frozen clock both terminate. No repeated physical programming occurs.
    BridgeDevice(&d);model.flip_read_fails=1;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && wrote);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT && model.retire_samples==600 && model.triggers==1);
    BridgeDevice(&d);model.frozen=1;model.earliest_lag=~0u;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS && wrote);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT && model.retire_stalls==600 && model.qpc==0);
    // No MMIO starts at the deadline, even when delayed entry or preemption consumed the budget.
    BridgeDevice(&d);
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    model.qpc=bridge.ProgramEndQpc+300000;
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT && model.retire_samples==0);
    BridgeDevice(&d);model.earliest_lag=~0u;model.delay_cost=310000;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT && model.retire_stalls==1 && model.earliest_reads==2);
    // A sample that ends after 30 ms is not retirement, even if all its values match.
    BridgeDevice(&d);model.read_cost=300000;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT && model.retire_samples==2 && model.retire_stalls==0);
    BridgeDevice(&d);model.pending_reads=1;model.read_cost=299500;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_IO_TIMEOUT && model.retire_samples==1 && model.retire_stalls==1);
    CHECK(model.qpc==300000 && model.earliest_reads==0);
    // Generations are checked before and after the proof. A superseded writer cannot authorize destruction.
    BridgeDevice(&d);
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    model.generation=5;
    CHECK(BridgeRetire(&d,&bridge)==STATUS_DEVICE_BUSY && model.retire_samples==0);
    model.generation=3;model.supersede_at=1;
    CHECK(BridgeRetire(&d,&bridge)==STATUS_DEVICE_BUSY);
    // A misuse from DISPATCH_LEVEL refuses before the first read/stall, including the retired shortcut.
    BridgeDevice(&d);
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    reads=model.observe_reads;model.irql=2;
    CHECK(BridgeRetire(&d,&bridge)==STATUS_INVALID_DEVICE_STATE && model.observe_reads==reads && model.retire_stalls==0);
    bridge.Retired=TRUE;
    CHECK(BridgeRetire(&d,&bridge)==STATUS_INVALID_DEVICE_STATE && model.observe_reads==reads);
    CHECK(protected_logs==0);
    protected_call=0;
    model.bridge=0;model.observing=0;model.irql=0;
}
/* BD-195 (invariant I3): a link detach before the first Apply. RuntimePost is empty until a runtime mode is applied
 * (SourceDevice fills it, which is why WriterBridge did not see 31v2's refusal). */
static void WriterBridgeAfterDetach(void)
{
    BC250_DEVICE d;
    BC250_DCN_WRITER_BRIDGE bridge;
    BOOLEAN wrote;
    // The flag over an empty RuntimePost: the bridge refuses before any write, with a state status, not a size one.
    BridgeDevice(&d);memset(&d.RuntimePost,0,sizeof(d.RuntimePost));
    d.RuntimeDisplayChanged=1;model.latched=0x202000;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_INVALID_DEVICE_STATE);
    CHECK(!wrote && model.writes==0 && bridge.Physical==0);
    // The link writer adopts the reserved buffer before it sets the flag. The next Begin then finds its own bridge
    // current with HUBP blank and the OTG stopped: no write, retired at once, and RuntimePostValid stays 0.
    BridgeDevice(&d);memset(&d.RuntimePost,0,sizeof(d.RuntimePost));
    DcnLinkSurfaceAdopt(&d);
    CHECK(d.RuntimePost.Width==64 && d.RuntimePost.Height==16 && d.RuntimePost.Pitch==256);
    CHECK(d.RuntimePost.PhysicAddress.QuadPart==0x202000 && d.RuntimePost.ColorFormat==D3DDDIFMT_A8R8G8B8);
    CHECK(d.RuntimePost.TargetId==BC250_CHILD_UID && !d.RuntimePostValid && model.reads==0 && model.writes==0);
    d.RuntimeDisplayChanged=1;model.latched=0x202000;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_SUCCESS);
    CHECK(!wrote && bridge.Retired && bridge.Physical==0x202000 && bridge.Pitch==256 && model.writes==0);
    CHECK(BridgeRetire(&d,&bridge)==STATUS_SUCCESS && model.retire_stalls==0);
    // Adopted, but the plane still names another surface: no programming while the link is off (RuntimePostValid 0).
    BridgeDevice(&d);memset(&d.RuntimePost,0,sizeof(d.RuntimePost));
    DcnLinkSurfaceAdopt(&d);d.RuntimeDisplayChanged=1;
    CHECK(BridgeProgram(&d,&wrote,&bridge)==STATUS_NOT_SUPPORTED);
    CHECK(!wrote && model.writes==0);
    // An applied runtime mode is never replaced, and without a reserved buffer nothing is adopted.
    BridgeDevice(&d);d.RuntimeDisplayChanged=1;d.RuntimePost.Pitch=512;d.RuntimePost.PhysicAddress.QuadPart=0x204000;
    DcnLinkSurfaceAdopt(&d);
    CHECK(d.RuntimePost.Pitch==512 && d.RuntimePost.PhysicAddress.QuadPart==0x204000);
    BridgeDevice(&d);memset(&d.RuntimePost,0,sizeof(d.RuntimePost));d.RuntimeFramebuffer=NULL;
    DcnLinkSurfaceAdopt(&d);
    CHECK(d.RuntimePost.Pitch==0 && d.RuntimePost.PhysicAddress.QuadPart==0);
    CHECK(protected_logs==0);
    model.bridge=0;model.observing=0;model.irql=0;
}
/* BD-197 step 2: an RGBA8 flip on a runtime mode and the plane's way back to ARGB8888, by the desktop's next flip
   and by the next writer's bridge to the reserved buffer (a connector commit, disable, suspend or resume begins
   with it). Every format write keeps the pipe owner's other fields: the firmware's, with the commit's DET plane-1
   base once a commit programmed the pipe. The firmware values carry bits in every field the format leaves. */
static NTSTATUS RuntimeFlip(BC250_DEVICE *d, ULONGLONG card, ULONG pitch, ULONG format, ULONGLONG *physical,
    BOOLEAN *wrote)
{
    model.address_writes = 0;   /* the model counts one sequence's address halves */
    return DcnFlipSourceAddress(d, card, pitch, format, (ULONGLONG)pitch * 16, FALSE, physical, wrote);
}
static void RuntimeModeFormats(void)
{
    const ULONG fwConfig = 0x00000308ul, fwCrossbar = 0x5AE4100Ful, fwCnvc = 0x00000008ul;
    const ULONG det = HUBPRET0_HUBPRET_CONTROL__DET_BUF_PLANE1_BASE_ADDRESS_MASK;
    const ULONG formatBits = HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CB_B_MASK |
                             HUBPRET0_HUBPRET_CONTROL__CROSSBAR_SRC_CR_R_MASK;
    BC250_DEVICE d;
    BC250_DCN_WRITER_BRIDGE bridge;
    BOOLEAN wrote;
    ULONGLONG physical;
    unsigned connector;
    for (connector = 0; connector <= 1; connector++) {
        const ULONG ownerDet = connector ? 0x2A5ul : (fwCrossbar & det);
        BridgeDevice(&d); model.bridge = 0;
        d.DcnFirmwareSurfaceConfig = fwConfig; d.DcnFirmwareHubpretControl = fwCrossbar; d.DcnFirmwareCnvcFormat = fwCnvc;
        model.config = fwConfig; model.crossbar = fwCrossbar; model.cnvc = fwCnvc;
        d.Modeset.InheritedFixed = TRUE; d.Modeset.InheritedSupport = 1;   /* the commit replaced a scaled pipe */
        d.RuntimeDisplayChanged = 1; d.RuntimePostValid = 1;
        if (connector) {
            DcnPipeConnectorPlan(&d, ownerDet);
            CHECK(d.DcnPipeConnector && d.DcnPipeDetPlane1 == ownerDet && model.writes == 0);
        }
        CHECK(DcnPlaneFormatsOpen(&d));
        /* Only the runtime pitch: the commit's plan is for that surface. */
        CHECK(RuntimeFlip(&d, 0x101000, 512, BC250_PLANE_FORMAT_ABGR8888, &physical, &wrote) ==
              STATUS_NOT_SUPPORTED && !wrote && model.writes == 0);
        CHECK(RuntimeFlip(&d, 0x101000, 256, BC250_PLANE_FORMAT_ABGR8888, &physical, &wrote) ==
              STATUS_SUCCESS && wrote && physical == 0x201000 && model.latched == 0x201000);
        CHECK(model.format_writes == 3 && d.DcnFormatChanges == 1 && d.DcnPlaneFormat == BC250_PLANE_FORMAT_ABGR8888);
        CHECK(DcnPlaneFormatDecodeRegisters(model.config, model.crossbar, model.cnvc) == BC250_PLANE_FORMAT_ABGR8888);
        CHECK(model.config == fwConfig && model.cnvc == fwCnvc);
        CHECK((model.crossbar & det) == ownerDet && (model.crossbar & ~(det | formatBits)) == (fwCrossbar & ~(det | formatBits)));
        /* A run of RGBA8 frames writes the format once. */
        CHECK(RuntimeFlip(&d, 0x101000, 256, BC250_PLANE_FORMAT_ABGR8888, &physical, &wrote) ==
              STATUS_SUCCESS && wrote && model.format_writes == 3);
        /* The desktop's next BGRA8 flip writes ARGB8888 back, with the owner's DET. */
        CHECK(RuntimeFlip(&d, 0x104000, 256, BC250_PLANE_FORMAT_ARGB8888, &physical, &wrote) ==
              STATUS_SUCCESS && wrote && model.latched == 0x204000);
        CHECK(model.format_writes == 6 && d.DcnPlaneFormat == BC250_PLANE_FORMAT_ARGB8888);
        CHECK(model.config == fwConfig && model.cnvc == fwCnvc &&
              model.crossbar == ((fwCrossbar & ~det) | ownerDet));
        /* RGBA8 again, then the next writer's Begin: the bridge to the reserved buffer is an ARGB8888 flip. */
        CHECK(RuntimeFlip(&d, 0x101000, 256, BC250_PLANE_FORMAT_ABGR8888, &physical, &wrote) ==
              STATUS_SUCCESS && wrote && model.format_writes == 9);
        model.bridge = 1; model.writes = 0; model.address_writes = 0;
        CHECK(BridgeProgram(&d, &wrote, &bridge) == STATUS_SUCCESS);
        CHECK(wrote && bridge.Physical == 0x202000 && bridge.Pitch == 256 && model.latched == 0x202000);
        CHECK(model.format_writes == 12 && d.DcnPlaneFormat == BC250_PLANE_FORMAT_ARGB8888);
        CHECK(model.config == fwConfig && model.cnvc == fwCnvc &&
              model.crossbar == ((fwCrossbar & ~det) | ownerDet));
        CHECK(BridgeRetire(&d, &bridge) == STATUS_SUCCESS);
        /* The next Begin finds its bridge current: ARGB8888 at the reserved buffer, no write. */
        model.writes = 0; model.observing = 0;
        CHECK(BridgeProgram(&d, &wrote, &bridge) == STATUS_SUCCESS && !wrote && bridge.Retired && model.writes == 0);
        model.bridge = 0; model.observing = 0; model.irql = 0;
    }
    /* A runtime mode without a valid surface (a commit in progress, a detach) flips nothing and says so. */
    BridgeDevice(&d); model.bridge = 0;
    d.RuntimeDisplayChanged = 1; d.RuntimePostValid = 0;
    CHECK(!DcnPlaneFormatsOpen(&d));
    CHECK(RuntimeFlip(&d, 0x101000, 256, BC250_PLANE_FORMAT_ABGR8888, &physical, &wrote) ==
          STATUS_NOT_SUPPORTED && !wrote && model.writes == 0);
    CHECK(protected_logs == 0);
}
/* BD-223: a multiplane-overlay plane 0 of its own size. The surface checks use the shape's source size, the
   admission runs before any write, the shape reaches the write sequence inside the lock, and a refusal by either
   writes nothing. The full-size flip passes no shape. */
static void ShapedSourceAddress(void)
{
    BC250_DEVICE d;
    BC250_SCALER_PLAN shape;
    BOOLEAN wrote;
    ULONGLONG physical;
    protected_logs = 0; protected_call = 1;
    memset(&shape, 0, sizeof(shape));
    shape.SourceWidth = 64; shape.SourceHeight = 8; shape.TargetWidth = 64; shape.TargetHeight = 16;
    /* 64x8 at pitch 256 is 2048 bytes: an allocation of 2048 is enough for the plane, not for the source mode. */
    SourceDevice(&d); memset(&shape_model, 0, sizeof(shape_model)); shape_model.admit = TRUE;
    CHECK(DcnFlipSourceAddressShaped(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 2048, FALSE, &shape,
                                     &physical, &wrote) == STATUS_SUCCESS);
    CHECK(wrote && physical == 0x201000 && model.latched == physical && !model.locked);
    CHECK(shape_model.admits == 1 && shape_model.calls == 1 && shape_model.shaped == 1 && shape_model.last == &shape);
    SourceDevice(&d); memset(&shape_model, 0, sizeof(shape_model)); shape_model.admit = TRUE;
    CHECK(DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 2048, FALSE,
                               &physical, &wrote) == STATUS_INVALID_PARAMETER);
    CHECK(!wrote && model.writes == 0 && shape_model.calls == 0);
    SourceDevice(&d); memset(&shape_model, 0, sizeof(shape_model)); shape_model.admit = TRUE;
    CHECK(DcnFlipSourceAddress(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 4096, FALSE,
                               &physical, &wrote) == STATUS_SUCCESS);
    CHECK(wrote && shape_model.admits == 0 && shape_model.calls == 1 && shape_model.shaped == 0);
    /* The admission refuses: nothing written. */
    SourceDevice(&d); memset(&shape_model, 0, sizeof(shape_model)); shape_model.admit = FALSE;
    CHECK(DcnFlipSourceAddressShaped(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 2048, FALSE, &shape,
                                     &physical, &wrote) == STATUS_NOT_SUPPORTED);
    CHECK(!wrote && model.writes == 0 && shape_model.calls == 0);
    /* The pipe's pitch rule stays: a runtime mode at another pitch refuses the plane before the admission. */
    SourceDevice(&d); memset(&shape_model, 0, sizeof(shape_model)); shape_model.admit = TRUE;
    d.RuntimeDisplayChanged = 1; d.RuntimePostValid = 1;
    CHECK(DcnFlipSourceAddressShaped(&d, 0x101000, 512, BC250_PLANE_FORMAT_ARGB8888, 4096, FALSE, &shape,
                                     &physical, &wrote) == STATUS_NOT_SUPPORTED);
    CHECK(!wrote && model.writes == 0 && shape_model.admits == 0);
    /* The address of a plane must fit the carve-out for the plane's own size. */
    SourceDevice(&d); memset(&shape_model, 0, sizeof(shape_model)); shape_model.admit = TRUE;
    CHECK(DcnFlipSourceAddressShaped(&d, 0x0, 256, BC250_PLANE_FORMAT_ARGB8888, 2048, FALSE, &shape,
                                     &physical, &wrote) == STATUS_ACCESS_DENIED);
    CHECK(!wrote && model.writes == 0);
    /* The shape write fails inside the lock: unknown, so Wrote; no address, the lock released, no trigger. */
    SourceDevice(&d); memset(&shape_model, 0, sizeof(shape_model)); shape_model.admit = TRUE;
    shape_model.status = STATUS_IO_TIMEOUT;
    CHECK(DcnFlipSourceAddressShaped(&d, 0x101000, 256, BC250_PLANE_FORMAT_ARGB8888, 2048, FALSE, &shape,
                                     &physical, &wrote) == STATUS_IO_TIMEOUT);
    CHECK(wrote && model.address_writes == 0 && model.triggers == 0 && !model.locked && d.DcnFlipRefused == 1);
    CHECK(protected_logs == 0);
    protected_call = 0;
    memset(&shape_model, 0, sizeof(shape_model));
}
int main(void)
{
    BC250_DEVICE device = {0};
    unsigned delay, bit;
    int quiet;
    const ULONGLONG target = UINT64_C(0x123456789ABC000);
    for (quiet = 0; quiet <= 1; quiet++) {
        for (delay = 0; delay <= 10; delay++) {
            for (bit = 0; bit < 32; bit++) {
                ULONG original = (1ul << bit) | HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK;
                memset(&model, 0, sizeof(model));
                model.ack_after = delay; model.flip = original; model.surface = original; model.pitch = original; model.quiet = quiet;
                CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, NULL, FALSE, quiet) == STATUS_SUCCESS);
                CHECK(model.stalls == delay && model.reads == delay + 1);
                CHECK(model.flip == (original & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK));
                CHECK(model.surface == (original & ~(HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
                    HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_META_SURFACE_TMZ_MASK)));
                CHECK(model.pitch == ((original & ~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) | 1919));
                CHECK(model.latched == target);
                CHECK(model.writes == 8 && model.triggers == 1 && !model.locked && model.format_writes == 0);
            }
        }
        memset(&model, 0, sizeof(model));
        model.ack_after = 11; model.quiet = quiet; device.DcnLockTimeouts = 0;
        CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, NULL, FALSE, quiet) == STATUS_IO_TIMEOUT);
        CHECK(model.stalls == 10 && model.reads == 11);
        CHECK(!model.locked && model.address_writes == 0 && model.triggers == 0);
        CHECK(model.writes == 2);
        CHECK(device.DcnLockTimeouts == 1);
    }
    {
        const ULONG pitches[]={5632,5888,6912};
        unsigned i;
        for(i=0;i<3;i++){
            memset(&model,0,sizeof(model));model.pitch=0xA5A5FFFFul;
            CHECK(DcnFlipWriteSequence(&device,target,pitches[i],4,NULL,NULL,FALSE,FALSE)==STATUS_SUCCESS);
            CHECK(model.pitch==((0xA5A5FFFFul&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)|(pitches[i]/4-1)));
        }
    }
    /* M15.14: the pitch field counts pixels of the plane's format, never 4 bytes by assumption. */
    {
        memset(&model,0,sizeof(model));model.pitch=0xA5A5FFFFul;
        CHECK(DcnFlipWriteSequence(&device,target,15360,8,NULL,NULL,FALSE,FALSE)==STATUS_SUCCESS);
        CHECK((model.pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)==1919);
        memset(&model,0,sizeof(model));
        CHECK(DcnFlipWriteSequence(&device,target,7682,4,NULL,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,7684,8,NULL,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,7680,0,NULL,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,0,4,NULL,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,(HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK+2ul)*4ul,4,NULL,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(model.writes==0);
    }
    /* M15.14: the firmware's format, decoded at start, and every plane format composed from it. */
    {
        /* Unit A's firmware plane (E21/E22): format 8, crossbar CR_R 3 CB_B 2 Y_G 1 ALPHA 0. The other bits
         * are set to show that only the format fields move. */
        const ULONG fwConfig = 0x00000008ul | 0x00000300ul, fwCrossbar = 0x00E40000ul | 0x0000000Ful;
        const ULONG fwCnvc = 0x00000008ul;
        static const ULONG hubp[BC250_PLANE_FORMATS] = { 0, 8, 8, 10 }, cnvc[BC250_PLANE_FORMATS] = { 0, 8, 8, 10 };
        static const ULONG crossbar[BC250_PLANE_FORMATS] = { 0, 0x00E40000ul, 0x00B40000ul, 0x00B40000ul };
        BC250_DEVICE fmt = {0};
        BC250_PLANE_REGISTERS regs;
        ULONG f;
        fmt.Mmio = &model;
        memset(&model,0,sizeof(model));
        model.config = fwConfig; model.crossbar = fwCrossbar; model.cnvc = fwCnvc;
        CHECK(!DcnPlaneFormatRegisters(&fmt, BC250_PLANE_FORMAT_ARGB8888, &regs));   /* nothing captured yet */
        DcnCaptureFirmwareFormat(&fmt);
        CHECK(model.capture_reads == 6 && model.writes == 0);
        CHECK(fmt.DcnPlaneFormats && fmt.DcnPlaneFormat == BC250_PLANE_FORMAT_ARGB8888);
        CHECK(fmt.DcnFirmwareSurfaceConfig == fwConfig && fmt.DcnFirmwareHubpretControl == fwCrossbar &&
              fmt.DcnFirmwareCnvcFormat == fwCnvc);
        DcnCaptureFirmwareFormat(&fmt);                      /* once per start */
        CHECK(model.capture_reads == 6);
        /* ARGB8888 is the firmware's value itself, bit for bit: restoring is a flip to it. */
        CHECK(DcnPlaneFormatRegisters(&fmt, BC250_PLANE_FORMAT_ARGB8888, &regs));
        CHECK(regs.SurfaceConfig == fwConfig && regs.HubpretControl == fwCrossbar && regs.CnvcFormat == fwCnvc);
        CHECK(!DcnPlaneFormatRegisters(&fmt, BC250_PLANE_FORMAT_NONE, &regs));
        CHECK(!DcnPlaneFormatRegisters(&fmt, BC250_PLANE_FORMATS, &regs));
        for (f = BC250_PLANE_FORMAT_ARGB8888; f < BC250_PLANE_FORMATS; f++) {
            unsigned ack;
            for (ack = 0; ack <= 10; ack += 5) {
                memset(&model,0,sizeof(model));
                model.config = fwConfig; model.crossbar = fwCrossbar; model.cnvc = fwCnvc; model.ack_after = ack;
                model.quiet = 1;
                CHECK(DcnPlaneFormatRegisters(&fmt, f, &regs));
                CHECK(DcnFlipWriteSequence(&device, target, 7680, Bc250PlaneEncoding(f)->BytesPerPixel, &regs, NULL, FALSE, TRUE) ==
                      STATUS_SUCCESS);
                CHECK(model.format_writes == 3 && model.writes == 11 && model.triggers == 1 && !model.locked);
                CHECK(model.latched == target);
                CHECK((model.config & HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) == hubp[f]);
                CHECK((model.config & ~HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK) ==
                      (fwConfig & ~HUBP0_DCSURF_SURFACE_CONFIG__SURFACE_PIXEL_FORMAT_MASK));
                CHECK((model.crossbar & 0x00FF0000ul) == crossbar[f] && (model.crossbar & ~0x00FF0000ul) == 0x0Ful);
                CHECK(model.cnvc == cnvc[f]);
                CHECK(DcnPlaneFormatDecodeRegisters(model.config, model.crossbar, model.cnvc) == f);
            }
        }
        /* An unacknowledged lock writes no format either. */
        memset(&model,0,sizeof(model)); model.ack_after = 11; model.quiet = 1;
        CHECK(DcnPlaneFormatRegisters(&fmt, BC250_PLANE_FORMAT_ABGR2101010, &regs));
        CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, &regs, NULL, FALSE, TRUE) == STATUS_IO_TIMEOUT);
        CHECK(model.format_writes == 0 && model.writes == 2 && !model.locked);
        /* A firmware plane that is not ARGB8888 (ABGR8888, and format pairs no encoding has) keeps the start
         * at the firmware's format: no composition, so no flip may change it. */
        {
            static const ULONG bad[][3] = { { 8, 0x00B40000ul, 8 }, { 10, 0x00E40000ul, 8 }, { 8, 0x00E40000ul, 10 },
                                            { 24, 0x00E40000ul, 24 } };
            unsigned i;
            for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
                BC250_DEVICE other = {0};
                other.Mmio = &model;
                memset(&model,0,sizeof(model));
                model.config = bad[i][0]; model.crossbar = bad[i][1]; model.cnvc = bad[i][2];
                DcnCaptureFirmwareFormat(&other);
                CHECK(!other.DcnPlaneFormats && other.DcnPlaneFormat == BC250_PLANE_FORMAT_NONE);
                CHECK(!DcnPlaneFormatRegisters(&other, BC250_PLANE_FORMAT_ARGB8888, &regs));
                CHECK(!DcnPlaneFormatRegisters(&other, BC250_PLANE_FORMAT_ABGR8888, &regs));
                CHECK(model.writes == 0);
            }
        }
        /* No BAR5: no read at all. */
        {
            BC250_DEVICE none = {0};
            memset(&model,0,sizeof(model));
            DcnCaptureFirmwareFormat(&none);
            CHECK(!none.DcnPlaneFormats && model.capture_reads == 0);
        }
    }
    {
        BC250_ESCAPE_DCNFLIP data={0};
        unsigned writes=model.writes;
        device.VidPnFlipEnabled=1;
        DcnFlipEscape(&device,&data);
        CHECK(data.NtStatus==(unsigned long)STATUS_DEVICE_BUSY && escape_calls==0);
        data.Restore=1;
        DcnFlipEscape(&device,&data);
        CHECK(data.NtStatus==(unsigned long)STATUS_DEVICE_BUSY && escape_calls==0);
        CHECK(model.writes==writes);
        device.VidPnFlipEnabled=0;
        device.Modeset.PipeChanged=1;
        DcnFlipEscape(&device,&data);
        CHECK(data.NtStatus==(unsigned long)STATUS_INVALID_DEVICE_STATE && escape_calls==0);
        CHECK(model.writes==writes);
        device.Modeset.PipeChanged=0;
        data.NtStatus=0;
        DcnFlipEscape(&device,&data);
        CHECK(escape_calls==1 && data.NtStatus==0);
    }
    FlipTypes();
    ImmediateObserve();
    ObserveAdmissionBoundary();
    SourceAddressNoLog();
    PlaneFormatsOpenAgrees();
    ShapedSourceAddress();
    WitnessQualification();
    WriterBridge();
    WriterBridgeAfterDetach();
    RuntimeModeFormats();
    printf("dcn flip: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
