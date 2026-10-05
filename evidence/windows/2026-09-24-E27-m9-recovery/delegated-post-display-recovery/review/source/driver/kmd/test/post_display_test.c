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
/* ACTUAL_SOURCE */
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
