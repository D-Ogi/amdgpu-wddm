// Actual-source host test. MMIO model delays lock acknowledgement independently
// of the request and latches HIGH on LOW. No claim about physical DCN timing.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "plane_format.h"
typedef unsigned long ULONG;
typedef uint64_t ULONGLONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
/* M15.14: the plane format state DcnCaptureFirmwareFormat fills and DcnPlaneFormatRegisters reads. */
typedef struct { volatile long DcnLockTimeouts; int VidPnFlipEnabled; void *Mmio;
    ULONG DcnFirmwareSurfaceConfig, DcnFirmwareHubpretControl, DcnFirmwareCnvcFormat;
    BOOLEAN DcnPlaneFormats; ULONG DcnPlaneFormat; } BC250_DEVICE;
#define GuardLog(...) ((void)0)
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
    unsigned ack_after, stalls, reads, writes, address_writes, triggers, format_writes, capture_reads;
    int locked, acknowledged, quiet;
    ULONG flip, surface, pitch, high;
    ULONG config, crossbar, cnvc, control, mpcc, lut;   /* M15.14: the plane format registers */
    ULONGLONG latched;
} model;
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
                CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, quiet) == STATUS_SUCCESS);
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
        CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, NULL, quiet) == STATUS_IO_TIMEOUT);
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
            CHECK(DcnFlipWriteSequence(&device,target,pitches[i],4,NULL,FALSE)==STATUS_SUCCESS);
            CHECK(model.pitch==((0xA5A5FFFFul&~HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)|(pitches[i]/4-1)));
        }
    }
    /* M15.14: the pitch field counts pixels of the plane's format, never 4 bytes by assumption. */
    {
        memset(&model,0,sizeof(model));model.pitch=0xA5A5FFFFul;
        CHECK(DcnFlipWriteSequence(&device,target,15360,8,NULL,FALSE)==STATUS_SUCCESS);
        CHECK((model.pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)==1919);
        memset(&model,0,sizeof(model));
        CHECK(DcnFlipWriteSequence(&device,target,7682,4,NULL,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,7684,8,NULL,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,7680,0,NULL,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,0,4,NULL,FALSE)==STATUS_INVALID_PARAMETER);
        CHECK(DcnFlipWriteSequence(&device,target,
            (HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK+2ul)*4ul,4,NULL,FALSE)==STATUS_INVALID_PARAMETER);
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
                CHECK(DcnFlipWriteSequence(&device, target, 7680, Bc250PlaneEncoding(f)->BytesPerPixel, &regs, TRUE) ==
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
        CHECK(DcnFlipWriteSequence(&device, target, 7680, 4, &regs, TRUE) == STATUS_IO_TIMEOUT);
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
        DcnFlipEscape(&device,&data);
        CHECK(escape_calls==1 && data.NtStatus==0);
    }
    printf("dcn flip: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
