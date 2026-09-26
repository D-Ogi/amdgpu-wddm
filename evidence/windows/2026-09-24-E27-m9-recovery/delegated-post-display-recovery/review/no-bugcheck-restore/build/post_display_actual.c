// Actual-source restore plus CPU writes. Does not trigger a bugcheck or prove
// hardware idle/reset; models the already-running single DCN pipe only.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
typedef int32_t NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned UINT;
typedef uint64_t ULONGLONG;
typedef size_t SIZE_T;
typedef unsigned char UCHAR;
typedef int BOOLEAN;
typedef void* PVOID;
typedef unsigned D3DDDIFORMAT;
typedef unsigned D3DDDI_VIDEO_PRESENT_TARGET_ID;
typedef const void* PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS;
typedef struct {
    void *Mmio,*Framebuffer;
    SIZE_T FramebufferLength;
    struct { UINT Width,Height,Pitch; D3DDDIFORMAT ColorFormat; } Post;
    BOOLEAN DcnWriteEnabled,DcnFirmwareKnown,DcnDiverged,SystemDisplayReady;
    ULONG DcnFirmwarePitch,DcnCurrentPitch;
    ULONGLONG DcnFirmwareAddress,DcnCurrentAddress;
    volatile long DcnLockTimeouts;
} BC250_DEVICE;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011u)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBu)
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_CHILD_UID 0x250001
#define D3DDDI_ID_UNINITIALIZED 0xffffffffu
#define D3DDDIFMT_UNKNOWN 0
#define D3DDDIFMT_X8R8G8B8 22
#define D3DDDIFMT_A8R8G8B8 21
#define BC250_DCNFLIP_POLL_MAX_US 50000
#define BC250_DCNFLIP_POLL_STEP_US 100
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define min(a,b) ((a)<(b)?(a):(b))
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static struct {
    ULONG pitch,flip,surface,high;
    ULONGLONG requested,scanned;
    unsigned elapsed,latch_delay,writes;
    int locked,triggered,never_latch;
} model;
static long InterlockedIncrement(volatile long *v){return ++*v;}
static NTSTATUS MmioDcnRead(const BC250_DEVICE*d,ULONG reg,ULONG*out)
{
    if(!d->Mmio)return STATUS_DEVICE_NOT_READY;
    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:*out=model.locked?OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK:0;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL:*out=model.flip;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL:*out=model.surface;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH:*out=model.pitch;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE:*out=(ULONG)model.scanned;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH:*out=(ULONG)(model.scanned>>32);break;
    default:CHECK(0);return STATUS_INVALID_PARAMETER;
    }return STATUS_SUCCESS;
}
static NTSTATUS MmioDcnWriteEx(const BC250_DEVICE*d,ULONG reg,ULONG value,BOOLEAN quiet)
{
    CHECK(quiet); // No logging/kernel-lock path is reachable from bugcheck.
    if(!d->Mmio || !d->DcnWriteEnabled)return STATUS_DEVICE_NOT_READY;
    model.writes++;
    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:model.locked=value!=0;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL:model.flip=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL:model.surface=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH:model.pitch=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH:model.high=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS:
        model.requested=((ULONGLONG)model.high<<32)|value;model.flip|=HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;break;
    case BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG:model.triggered=1;CHECK(!model.locked);break;
    default:CHECK(0);return STATUS_INVALID_PARAMETER;
    }return STATUS_SUCCESS;
}
static void KeStallExecutionProcessor(ULONG us)
{
    CHECK(us==1 || us==100);model.elapsed+=us;
    if(model.triggered){
        // The pending bit can clear before the in-use address catches up.
        if(model.elapsed>=100)model.flip&=~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
        if(!model.never_latch && model.elapsed>=model.latch_delay)model.scanned=model.requested;
    }
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

static NTSTATUS DcnReadScanoutPhysical(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Physical)
{
    ULONG high, low, highAgain, attempt;
    ULONGLONG physical;
    NTSTATUS status;

    *Physical = 0;
    if (Device->Mmio == NULL) return STATUS_DEVICE_NOT_READY;
    // A latch between the two halves must not manufacture an address. Bounded
    // retries only: the caller can run at DPC and never waits for the next frame.
    for (attempt = 0; attempt < 3; ++attempt)
    {
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, &high);
        if (!NT_SUCCESS(status)) return status;
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE, &low);
        if (!NT_SUCCESS(status)) return status;
        status = MmioDcnRead(Device, BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH, &highAgain);
        if (!NT_SUCCESS(status)) return status;
        high &= HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
        highAgain &= HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH__SURFACE_EARLIEST_INUSE_ADDRESS_HIGH_MASK;
        if (high != highAgain) continue;
        physical = ((ULONGLONG)high << 32) | low;
        *Physical = physical;
        return STATUS_SUCCESS;
    }
    return STATUS_DEVICE_NOT_READY;
}

static NTSTATUS DcnCheckPostSurface(_In_ const BC250_DEVICE* Device)
{
    ULONG pending, pitch;
    ULONGLONG scanned;
    NTSTATUS status;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL,&pending);
    if (!NT_SUCCESS(status)) return status;
    if (pending&HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK) return STATUS_DEVICE_BUSY;
    status=DcnReadScanoutPhysical(Device,&scanned);
    if (!NT_SUCCESS(status)) return status;
    status=MmioDcnRead(Device,BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH,&pitch);
    if (!NT_SUCCESS(status)) return status;
    pitch=((pitch&HUBPREQ0_DCSURF_SURFACE_PITCH__PITCH_MASK)+1)*4;
    return scanned==Device->DcnFirmwareAddress && pitch==Device->DcnFirmwarePitch ? STATUS_SUCCESS : STATUS_DEVICE_BUSY;
}

NTSTATUS DcnRestorePostDisplay(_Inout_ BC250_DEVICE* Device)
{
    NTSTATUS status;
    ULONG waited=0;
    // Captured before any address write, so this also covers a bugcheck midway
    // through a flip before DcnDiverged/current-address publication completed.
    if (!Device->DcnFirmwareKnown)
        return Device->DcnDiverged ? STATUS_DEVICE_NOT_READY : STATUS_SUCCESS;
    if (!Device->Framebuffer || Device->DcnFirmwarePitch!=Device->Post.Pitch)
        return STATUS_DEVICE_NOT_READY;
    status=DcnCheckPostSurface(Device);
    if (!NT_SUCCESS(status))
    {
        status=DcnFlipWriteSequence(Device,Device->DcnFirmwareAddress,Device->DcnFirmwarePitch,TRUE);
        if (!NT_SUCCESS(status)) return status;
        for (;;)
        {
            status=DcnCheckPostSurface(Device);
            if (status!=STATUS_DEVICE_BUSY) break;
            if (waited>=BC250_DCNFLIP_POLL_MAX_US) return STATUS_IO_TIMEOUT;
            KeStallExecutionProcessor(BC250_DCNFLIP_POLL_STEP_US);
            waited+=BC250_DCNFLIP_POLL_STEP_US;
        }
        if (!NT_SUCCESS(status)) return status;
    }
    Device->DcnCurrentAddress=Device->DcnFirmwareAddress;
    Device->DcnCurrentPitch=Device->DcnFirmwarePitch;
    Device->DcnDiverged=FALSE;
    return STATUS_SUCCESS;
}

static BOOLEAN IsPostFormatSupported(D3DDDIFORMAT Format)
{
    return Format == D3DDDIFMT_X8R8G8B8 || Format == D3DDDIFMT_A8R8G8B8;
}

NTSTATUS Bc250SystemDisplayEnable(_In_ const PVOID MiniportDeviceContext, _In_ const D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                  _In_ const PDXGKARG_SYSTEM_DISPLAY_ENABLE_FLAGS Flags, _Out_ UINT* Width, _Out_ UINT* Height,
                                  _Out_ D3DDDIFORMAT* ColorFormat)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    NTSTATUS status;
    UNREFERENCED_PARAMETER(Flags);
    device->SystemDisplayReady=FALSE;
    *Width=0;*Height=0;*ColorFormat=D3DDDIFMT_UNKNOWN;
    // Single inherited output, advertised as always connected by QueryChildStatus.
    if (TargetId!=BC250_CHILD_UID && TargetId!=D3DDDI_ID_UNINITIALIZED) return STATUS_NOT_SUPPORTED;
    if (!device->Framebuffer || !IsPostFormatSupported(device->Post.ColorFormat) ||
        !device->Post.Width || !device->Post.Height ||
        (ULONGLONG)device->Post.Width*4>device->Post.Pitch ||
        (ULONGLONG)device->Post.Pitch*device->Post.Height>device->FramebufferLength)
        return STATUS_DEVICE_NOT_READY;
    status=STATUS_SUCCESS; /* negative: no scanout restoration */
    if (!NT_SUCCESS(status)) return status;
    *Width = device->Post.Width;
    *Height = device->Post.Height;
    *ColorFormat = device->Post.ColorFormat;
    device->SystemDisplayReady=TRUE;
    return STATUS_SUCCESS;
}

void Bc250SystemDisplayWrite(_In_ const PVOID MiniportDeviceContext, _In_ const PVOID Source, _In_ const UINT SourceWidth,
                             _In_ const UINT SourceHeight, _In_ const UINT SourceStride, _In_ const UINT PositionX,
                             _In_ const UINT PositionY)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;
    UINT y, width, height;

    if (!device->SystemDisplayReady || device->Framebuffer == NULL ||
        PositionX >= device->Post.Width || PositionY >= device->Post.Height) return;
    width = min(SourceWidth, device->Post.Width - PositionX);
    width = min(width, SourceStride / 4);
    height = min(SourceHeight, device->Post.Height - PositionY);
    for (y = 0; y < height; y++)
        RtlCopyMemory((UCHAR*)device->Framebuffer + (SIZE_T)(PositionY + y) * device->Post.Pitch + (SIZE_T)PositionX * 4,
                      (const UCHAR*)Source + (SIZE_T)y * SourceStride, (SIZE_T)width * 4);
}

static void init(BC250_DEVICE*d,ULONG*fb)
{
    memset(d,0,sizeof(*d));memset(&model,0,sizeof(model));memset(fb,0,96);
    d->Mmio=&model;d->Framebuffer=fb;d->FramebufferLength=96;
    d->Post.Width=4;d->Post.Height=3;d->Post.Pitch=32;d->Post.ColorFormat=D3DDDIFMT_X8R8G8B8;
    d->DcnWriteEnabled=1;d->DcnFirmwareKnown=1;d->DcnFirmwarePitch=32;
    d->DcnFirmwareAddress=0x470000000ull;d->DcnCurrentAddress=0x480000000ull;d->DcnDiverged=1;
    model.scanned=d->DcnCurrentAddress;model.pitch=15;model.latch_delay=300;
}
int main(void)
{
    BC250_DEVICE d;ULONG fb[24],src[8]={1,2,3,4,5,6,7,8};UINT width,height;D3DDDIFORMAT fmt;unsigned writes;
    init(&d,fb);
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && width==4 && height==3 && fmt==D3DDDIFMT_X8R8G8B8);
    CHECK(model.scanned==d.DcnFirmwareAddress && model.elapsed==300 && !d.DcnDiverged);
    CHECK(d.DcnCurrentAddress==d.DcnFirmwareAddress && d.DcnCurrentPitch==32);
    Bc250SystemDisplayWrite(&d,src,4,2,16,1,1);
    CHECK(fb[9]==1 && fb[10]==2 && fb[11]==3 && fb[17]==5 && fb[18]==6 && fb[19]==7);
    CHECK(fb[8]==0 && fb[12]==0 && fb[20]==0); // preserve pitch padding and clipped right edge
    writes=model.writes;CHECK(DcnRestorePostDisplay(&d)==STATUS_SUCCESS);CHECK(model.writes==writes);
    // Mid-publication crash: diverged flag has not been published, cached POST is valid.
    init(&d,fb);d.DcnDiverged=0;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(model.scanned==d.DcnFirmwareAddress && model.writes!=0);
    // Late in-use latch never arrives despite pending clear: do not claim restore.
    init(&d,fb);model.never_latch=1;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_IO_TIMEOUT);
    CHECK(!d.SystemDisplayReady && d.DcnDiverged && width==0 && height==0 && fmt==D3DDDIFMT_UNKNOWN);
    CHECK(model.elapsed==50000 && model.scanned!=d.DcnFirmwareAddress);
    Bc250SystemDisplayWrite(&d,src,4,2,16,0,0);CHECK(fb[0]==0);
    init(&d,fb);d.DcnWriteEnabled=0;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_DEVICE_NOT_READY);
    CHECK(!d.SystemDisplayReady && model.writes==0);
    // Untouched basic-display path needs no MMIO or programming.
    init(&d,fb);d.DcnFirmwareKnown=0;d.DcnDiverged=0;d.Mmio=NULL;
    CHECK(Bc250SystemDisplayEnable(&d,D3DDDI_ID_UNINITIALIZED,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && model.writes==0);
    Bc250SystemDisplayWrite(&d,src,4,1,8,0,0);CHECK(fb[0]==1 && fb[1]==2 && fb[2]==0);
    printf("post display restore: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
