// Actual-source host test. MMIO model delays lock acknowledgement independently
// of the request and latches HIGH on LOW. No claim about physical DCN timing.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
typedef unsigned long ULONG;
typedef uint64_t ULONGLONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
typedef struct { volatile long DcnLockTimeouts; int VidPnFlipEnabled; } BC250_DEVICE;
static long InterlockedIncrement(volatile long *value) { return ++*value; }
#define TRUE 1
#define FALSE 0
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011u)
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
    unsigned ack_after, stalls, reads, writes, address_writes, triggers;
    int locked, acknowledged, quiet;
    ULONG flip, surface, pitch, high;
    ULONGLONG latched;
} model;
static NTSTATUS MmioDcnRead(const BC250_DEVICE *d, ULONG reg, ULONG *value)
{
    (void)d;
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
    CHECK(model.locked && !model.acknowledged);
    model.stalls += us;
}
static NTSTATUS MmioDcnWriteEx(const BC250_DEVICE *d, ULONG reg, ULONG value, BOOLEAN quiet)
{
    (void)d;
    CHECK(quiet == model.quiet);
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
        if (reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL) model.flip = value;
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
static NTSTATUS DcnFlipWriteSequence(_Inout_ BC250_DEVICE* Device, ULONGLONG Target, ULONG Pitch, BOOLEAN Quiet)
{
    NTSTATUS status;
    ULONG value = 0, waited = 0;
    if (!Pitch || (Pitch&3ul) || ((Pitch/4-1)&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)) return STATUS_INVALID_PARAMETER;

    status = MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 1, Quiet);
    if (NT_SUCCESS(status))
    {
        for (;;)
        {
            status = MmioDcnRead(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, &value);
            if (!NT_SUCCESS(status) ||
                (value & OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK) != 0) break;
            if (waited == 10)
            {
                InterlockedIncrement(&Device->DcnLockTimeouts);
                status = STATUS_IO_TIMEOUT;
                break;
            }
            KeStallExecutionProcessor(1);
            waited++;
        }
    }
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL, &value);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,
        value & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK, Quiet);
    // AMD hubp2 also updates only the two graphics TMZ fields here. Our
    // unprotected surfaces clear those fields without erasing other controls.
    if (NT_SUCCESS(status)) status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL, &value);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL,
        value & ~(HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
                  HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_META_SURFACE_TMZ_MASK), Quiet);
    // AMD hubp2_program_size uses pixels minus one; preserve META_PITCH.
    if (NT_SUCCESS(status)) status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&value);
    if (NT_SUCCESS(status)) status=MmioDcnWriteEx(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,
        (value&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)|(Pitch/4-1),Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH, (ULONG)(Target >> 32), Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS, (ULONG)(Target & 0xFFFFFFFFull), Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0, Quiet);
    if (NT_SUCCESS(status)) status = MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG, 1, Quiet);
    if (!NT_SUCCESS(status))
    {
        // Release even after a missing acknowledgement. Never leave OTG0 locked
        // after a refused flip; no address or trigger write follows a timeout.
        (void)MmioDcnWriteEx(Device, BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK, 0, Quiet);
    }
    return status;
}

void DcnFlipEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_DCNFLIP* Data)
{
    Data->Version = BC250_KMD_VERSION;
    Data->NtStatus = 0;
    Data->FirmwareAddress = 0;
    Data->AddressBefore = 0;
    Data->AddressAfter = 0;
    Data->InUseBefore = 0;
    Data->InUseAfter = 0;
    Data->FrameCountBefore = 0;
    Data->FrameCountAfter = 0;
    Data->FlipPendingCleared = 0;
    Data->WaitUs = 0;
    Data->DchubpCntl = 0;
    Data->Underflow = 0;
    RtlZeroMemory(Data->Reason, sizeof(Data->Reason));

    // The WDDM DDI owns OTG0 while VidPn flips are enabled. Diagnostics must
    // not interleave another lock/address/unlock transaction with that owner.
    // DcnStop calls the core directly after WDDM has quiesced, so restoration
    // remains available to the normal stop path.
    if (Device->VidPnFlipEnabled)
    {
        Refuse(Data, STATUS_DEVICE_BUSY, "diagnostic flip refused while VidPn owns OTG0");
        return;
    }
    DcnFlipCore(Device, Data->Physical, Data->Restore != 0, Data->Fill, Data->FillColor, Data);
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
                CHECK(DcnFlipWriteSequence(&device, target, 7680, quiet) == STATUS_SUCCESS);
                CHECK(model.stalls == delay && model.reads == delay + 1);
                CHECK(model.flip == (original & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK));
                CHECK(model.surface == (original & ~(HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
                    HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_META_SURFACE_TMZ_MASK)));
                CHECK(model.pitch == ((original & ~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK) | 1919));
                CHECK(model.latched == target);
                CHECK(model.writes == 8 && model.triggers == 1 && !model.locked);
            }
        }
        memset(&model, 0, sizeof(model));
        model.ack_after = 11; model.quiet = quiet; device.DcnLockTimeouts = 0;
        CHECK(DcnFlipWriteSequence(&device, target, 7680, quiet) == STATUS_IO_TIMEOUT);
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
            CHECK(DcnFlipWriteSequence(&device,target,pitches[i],FALSE)==STATUS_SUCCESS);
            CHECK(model.pitch==((0xA5A5FFFFul&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)|(pitches[i]/4-1)));
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
        DcnFlipEscape(&device,&data);
        CHECK(escape_calls==1 && data.NtStatus==0);
    }
    printf("dcn flip: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
