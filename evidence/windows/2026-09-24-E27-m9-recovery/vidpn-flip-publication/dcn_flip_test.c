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
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define NT_SUCCESS(s) ((s) >= 0)
static unsigned checks, failures;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static struct {
    unsigned ack_after, stalls, reads, writes, address_writes, triggers;
    int locked, acknowledged, quiet;
    ULONG flip, surface, high;
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
              reg == BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL);
        CHECK(model.locked && model.acknowledged);
        *value = reg == BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL ? model.flip : model.surface;
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
                model.ack_after = delay; model.flip = original; model.surface = original; model.quiet = quiet;
                CHECK(DcnFlipWriteSequence(&device, target, quiet) == STATUS_SUCCESS);
                CHECK(model.stalls == delay && model.reads == delay + 1);
                CHECK(model.flip == (original & ~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_TYPE_MASK));
                CHECK(model.surface == (original & ~(HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_TMZ_MASK |
                    HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_META_SURFACE_TMZ_MASK)));
                CHECK(model.latched == target);
                CHECK(model.writes == 7 && model.triggers == 1 && !model.locked);
            }
        }
        memset(&model, 0, sizeof(model));
        model.ack_after = 11; model.quiet = quiet; device.DcnLockTimeouts = 0;
        CHECK(DcnFlipWriteSequence(&device, target, quiet) == STATUS_IO_TIMEOUT);
        CHECK(model.stalls == 10 && model.reads == 11);
        CHECK(!model.locked && model.address_writes == 0 && model.triggers == 0);
        CHECK(model.writes == 2);
        CHECK(device.DcnLockTimeouts == 1);
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
