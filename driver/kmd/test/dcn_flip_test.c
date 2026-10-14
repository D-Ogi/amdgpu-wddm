// Actual-source host test. MMIO model delays lock acknowledgement independently
// of the request and latches HIGH on LOW. No claim about physical DCN timing.
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "plane_format.h"
typedef unsigned long ULONG;
typedef uint64_t ULONGLONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
typedef long LONG;
typedef int64_t LONGLONG;
typedef int64_t LONG64;
typedef unsigned char UCHAR, *PUCHAR;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
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
typedef struct { volatile long DcnLockTimeouts; int VidPnFlipEnabled; struct { BOOLEAN PipeChanged; } Modeset;
    void *Mmio;
    ULONG DcnFirmwareSurfaceConfig, DcnFirmwareHubpretControl, DcnFirmwareCnvcFormat;
    BOOLEAN DcnPlaneFormats; ULONG DcnPlaneFormat;
    volatile LONG DcnImmediateFlips, DcnImmediateSameFrame;
    volatile LONG DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_OUTCOMES];
    volatile LONG DcnImmediateDoneMaxUs, DcnImmediateObserveMaxUs, DcnImmediateOverruns;
    volatile LONG64 DcnImmediateDoneSumUs;
    volatile LONG DcnImmediateLastFrame, DcnImmediateRecordNext;
    LONGLONG DcnQpcFrequency;
    BC250_DCN_IMMEDIATE_RECORD DcnImmediateRecords[BC250_DCN_IMMEDIATE_RECORDS]; } BC250_DEVICE;
#define GuardLog(...) ((void)0)
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
    unsigned supersede_at, read_cost;
    LONGLONG qpc;
    ULONG frame, position, high_noise;
    ULONGLONG primary, primary_old, sample;
    volatile LONG generation;
} model;
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
        } else if (reg == BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT) *value = model.frame;
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
    CHECK(us == 1);
    if (model.observing) {
        CHECK(!model.locked);
        model.observe_stalls++;
        if (!model.frozen) model.qpc += 10;
        return;
    }
    CHECK(model.locked && !model.acknowledged);
    model.stalls += us;
}
static NTSTATUS MmioDcnWriteEx(const BC250_DEVICE *d, ULONG reg, ULONG value, BOOLEAN quiet)
{
    (void)d;
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
                CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, immediate, TRUE) == STATUS_SUCCESS);
                CHECK(model.flip == (immediate ? (original | HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK) :
                                                 (original & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK)));
                CHECK(model.latched == target && model.triggers == 1 && !model.locked && model.writes == 8);
            }
    memset(&model, 0, sizeof(model));
    model.ack_after = 11; model.flip = 0; model.quiet = 1;
    CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, TRUE, TRUE) == STATUS_IO_TIMEOUT);
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
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && r->Samples == BC250_DCN_IMMEDIATE_OBSERVE_US + 1);
    CHECK(r->Observed == 0xB000 && r->ClearUs == 0 && r->DoneUs == -1 && r->EndUs == 50 && r->ReadStatus == 0);
    CHECK(d.DcnImmediateSameFrame == 1 && d.DcnImmediateDoneMaxUs == 2 && d.DcnImmediateObserveMaxUs == 50);

    /* Never clears, with the address register on the request all the time: PENDING at the bound. The address alone
       is never P'. */
    ObserveReset();
    model.qpc = 4000; model.pending_reads = ~0u; model.primary = 0xD000;
    CHECK(DcnImmediateObserve(&d, 0xD000, 0xD000, 4000, 4000, &model.generation, 4, 0x2u) == BC250_DCN_IMMEDIATE_PENDING);
    outcomes[BC250_DCN_IMMEDIATE_PENDING]++;
    r = &d.DcnImmediateRecords[3];
    CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && model.primary_reads == BC250_DCN_IMMEDIATE_OBSERVE_US + 1);
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
        CHECK(model.observe_stalls == BC250_DCN_IMMEDIATE_OBSERVE_US && r->Samples == BC250_DCN_IMMEDIATE_OBSERVE_US + 1);
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
       bound held at the sample start would take it. P' at a sample that started inside the bound and ended past it is
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
                CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, FALSE, quiet) == STATUS_SUCCESS);
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
        CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, FALSE, quiet) == STATUS_IO_TIMEOUT);
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
            CHECK(DcnFlipWriteSequence(&device,target,pitches[i],4,NULL,FALSE,FALSE)==STATUS_SUCCESS);
            CHECK(model.pitch==((0xA5A5FFFFul&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)|(pitches[i]/4-1)));
        }
    }
    /* M15.14: the pitch field counts pixels of the plane's format, never 4 bytes by assumption. */
    {
        memset(&model,0,sizeof(model));model.pitch=0xA5A5FFFFul;
        CHECK(DcnFlipWriteSequence(&device,target,15360,8,NULL,FALSE,FALSE)==STATUS_SUCCESS);
        CHECK((model.pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)==1919);
        memset(&model,0,sizeof(model));
        CHECK(DcnFlipWriteSequence(&device,target,7682,4,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,7684,8,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,7680,0,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,0,4,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,
            (HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK+2ul)*4ul,4,NULL,FALSE,FALSE)==STATUS_INVALID_PARAMETER);
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
                CHECK(DcnFlipWriteSequence(&device, target, 7680, Bc250PlaneEncoding(f)->BytesPerPixel, &regs, FALSE, TRUE) ==
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
        CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, &regs, FALSE, TRUE) == STATUS_IO_TIMEOUT);
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
    printf("dcn flip: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
