// Actual-source restore plus CPU writes. Does not trigger a bugcheck or prove
// hardware idle/reset; models the already-running single DCN pipe only.
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "plane_format.h"
#include "display_io.h"
#include "dcn_translate.h"
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
typedef struct { long long QuadPart; } PHYSICAL_ADDRESS;
typedef struct { UINT Width,Height,Pitch; D3DDDIFORMAT ColorFormat; PHYSICAL_ADDRESS PhysicAddress; } DXGK_DISPLAY_INFORMATION;
typedef struct _BC250_DEVICE {
    BOOLEAN VramEnabled;PHYSICAL_ADDRESS VramPhysical;ULONGLONG VramLength,VramMcBase;
    void *Mmio,*Framebuffer;
    SIZE_T FramebufferLength;
    DXGK_DISPLAY_INFORMATION Post,RuntimePost;
    UCHAR* RuntimeFramebuffer;
    SIZE_T RuntimeFramebufferLength;
    PHYSICAL_ADDRESS RuntimeFramebufferPhysical;
    volatile long RuntimeDisplayChanged,RuntimePostValid,RuntimePlaneBlank;
    BOOLEAN DcnWriteEnabled,DcnFirmwareKnown,DcnDiverged,SystemDisplayReady,DcnBlanked;
    ULONG DcnFirmwarePitch,DcnCurrentPitch;
    ULONGLONG DcnFirmwareAddress,DcnCurrentAddress;
    volatile long DcnLockTimeouts;
    /* M15.14: the plane format state (dcn.c DcnCaptureFirmwareFormat). */
    ULONG DcnFirmwareSurfaceConfig,DcnFirmwareHubpretControl,DcnFirmwareCnvcFormat;
    BOOLEAN DcnPlaneFormats;
    ULONG DcnPlaneFormat;
} BC250_DEVICE;
#include "connector_surface.h"
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
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RtlCopyMemory(d,s,n) memcpy(d,s,n)
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static struct {
    ULONG pitch,flip,surface,high,blank,dbuf,hubp;
    unsigned blank_reads,unstable_blank;
    ULONG config,crossbar,cnvc;unsigned format_reads,format_writes;   /* M15.14: the plane format registers */
    ULONGLONG requested,scanned;
    unsigned elapsed,latch_delay,writes;
    int locked,triggered,never_latch;
} model;
static long InterlockedIncrement(volatile long *v){return ++*v;}
static long InterlockedCompareExchange(volatile long* v,long n,long old){long r=*v;if(r==old)*v=n;return r;}
static long InterlockedExchange(volatile long* v,long n){long r=*v;*v=n;return r;}
static NTSTATUS MmioDcnRead(const BC250_DEVICE*d,ULONG reg,ULONG*out)
{
    if(!d->Mmio)return STATUS_DEVICE_NOT_READY;
    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL:
        *out=model.blank;
        if(model.unstable_blank && ++model.blank_reads==2)*out^=OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        break;
    case BC250_REG_DMU_HUBP0_DCHUBP_CNTL:*out=model.hubp;break;
    case BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL:*out=model.dbuf;break;
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:*out=model.locked?OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK:0;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL:*out=model.flip;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL:*out=model.surface;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH:*out=model.pitch;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE:*out=(ULONG)model.scanned;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH:*out=(ULONG)(model.scanned>>32);break;
    /* M15.14: read only when the start captured the firmware's format (d->DcnPlaneFormats). */
    case BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG:CHECK(d->DcnPlaneFormats);model.format_reads++;*out=model.config;break;
    case BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL:CHECK(d->DcnPlaneFormats);model.format_reads++;*out=model.crossbar;break;
    case BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT:CHECK(d->DcnPlaneFormats);model.format_reads++;*out=model.cnvc;break;
    default:CHECK(0);return STATUS_INVALID_PARAMETER;
    }return STATUS_SUCCESS;
}
static NTSTATUS MmioDcnWriteEx(const BC250_DEVICE*d,ULONG reg,ULONG value,BOOLEAN quiet)
{
    CHECK(quiet); // No logging/kernel-lock path is reachable from bugcheck.
    if(!d->Mmio || !d->DcnWriteEnabled)return STATUS_DEVICE_NOT_READY;
    model.writes++;
    switch(reg){
    case BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL:
        model.blank=value&~OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        if(value&OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK)model.blank|=OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        break;
    case BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL:model.dbuf=value;break;
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:model.locked=value!=0;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL:model.flip=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL:model.surface=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_PITCH:model.pitch=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH:model.high=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS:
        model.requested=((ULONGLONG)model.high<<32)|value;model.flip|=HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;break;
    case BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG:model.triggered=1;CHECK(!model.locked);break;
    /* M15.14: written inside the lock, and only when the start captured the firmware's format. */
    case BC250_REG_DMU_HUBP0_DCSURF_SURFACE_CONFIG:CHECK(model.locked && d->DcnPlaneFormats);model.format_writes++;model.config=value;break;
    case BC250_REG_DMU_HUBPRET0_HUBPRET_CONTROL:CHECK(model.locked && d->DcnPlaneFormats);model.format_writes++;model.crossbar=value;break;
    case BC250_REG_DMU_CNVC_CFG0_CNVC_SURFACE_PIXEL_FORMAT:CHECK(model.locked && d->DcnPlaneFormats);model.format_writes++;model.cnvc=value;break;
    default:CHECK(0);return STATUS_INVALID_PARAMETER;
    }return STATUS_SUCCESS;
}
/* Display modes (modeset.c): after the firmware surface is back, the bugcheck screen gets the native viewport.
 * Counted here; the modeset host suite tests what it writes. It must run only after a successful restore. */
static unsigned modeset_restores;
static void ModesetRestoreQuiet(BC250_DEVICE*d){CHECK(!d->DcnDiverged);++modeset_restores;}
static int plane_is_blank;
static void KeStallExecutionProcessor(ULONG us)
{
    CHECK(us==1 || us==100);model.elapsed+=us;
    if(model.triggered && !plane_is_blank){
        // The pending bit can clear before the in-use address catches up.
        if(model.elapsed>=100)model.flip&=~HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
        if(!model.never_latch && model.elapsed>=model.latch_delay)model.scanned=model.requested;
    }
}
static long RuntimeRead(void* c,unsigned long o,unsigned long* v){return MmioDcnRead(c,o,v);}
static long RuntimeWrite(void* c,unsigned long o,unsigned long v){return MmioDcnWriteEx(c,o,v,TRUE);}
static void RuntimeStall(void* c,unsigned long us){(void)c;KeStallExecutionProcessor(us);}
static unsigned plane_blanks,plane_unblanks;
static long Bc250Dcn201PlaneBlank(BC250_DISP_IO* io,int blank)
{
    (void)io;
    plane_is_blank=blank;
    if(blank) ++plane_blanks;
    else { ++plane_unblanks; CHECK(model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK); }
    return STATUS_SUCCESS;
}
/* BD-170 r6: Desired as the writer's Resume hands it out (bc250kmd.h), and the two calls of wddm.c an unblank site
 * makes. The stub hands out the deposit a test sets, and records what the plane and the address registers held when
 * Resume ran (it must run with the plane blank and before the write sequence) and which deposit was programmed. */
typedef struct {ULONGLONG CardAddress;ULONG Pitch;ULONG PlaneFormat;ULONGLONG Bytes;ULONG Sequence;} BC250_WDDM_DESIRED;
static struct {
    int has;BC250_WDDM_DESIRED desired;
    unsigned resumes,programmed;ULONG sequence;
    int blank_at_resume;ULONGLONG requested_at_resume;
} writer;
static BOOLEAN WddmWriterResume(BC250_DEVICE*d,BC250_WDDM_DESIRED*desired)
{
    (void)d;
    ++writer.resumes;writer.blank_at_resume=plane_is_blank;writer.requested_at_resume=model.requested;
    memset(desired,0,sizeof(*desired));
    if(!writer.has)return FALSE;
    *desired=writer.desired;return TRUE;
}
static void WddmWriterDepositProgrammed(BC250_DEVICE*d,ULONG sequence)
{
    (void)d;
    /* Only after the poll proved the deposit on the plane. */
    CHECK(model.scanned==model.requested && !(model.flip&HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK));
    ++writer.programmed;writer.sequence=sequence;
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
    CHECK(modeset_restores==1);
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
    CHECK(modeset_restores==2); // a failed restore leaves the scaler alone
    Bc250SystemDisplayWrite(&d,src,4,2,16,0,0);CHECK(fb[0]==0);
    init(&d,fb);d.DcnWriteEnabled=0;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_DEVICE_NOT_READY);
    CHECK(!d.SystemDisplayReady && model.writes==0);
    // A hidden source must become visible for bugcheck and POST handover.
    init(&d,fb);d.DcnBlanked=TRUE;
    model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && !d.DcnBlanked && !(model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
    CHECK(model.scanned==d.DcnFirmwareAddress);
    // Visibility can change before the first flip; that path must also unblank.
    init(&d,fb);d.DcnFirmwareKnown=FALSE;d.DcnDiverged=FALSE;d.DcnBlanked=TRUE;
    model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && !d.DcnBlanked && model.writes==1);
    // Hardware blank can survive while a new software context starts at false.
    init(&d,fb);d.DcnBlanked=FALSE;
    model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && !d.DcnBlanked && !(model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
    // Untouched basic-display path needs no MMIO or programming.
    init(&d,fb);d.DcnFirmwareKnown=0;d.DcnDiverged=0;d.Mmio=NULL;
    CHECK(Bc250SystemDisplayEnable(&d,D3DDDI_ID_UNINITIALIZED,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(d.SystemDisplayReady && model.writes==0);
    Bc250SystemDisplayWrite(&d,src,4,1,8,0,0);CHECK(fb[0]==1 && fb[1]==2 && fb[2]==0);
    // M15.14: a plane a client flip left in R8G8B8A8 or R10G10B10A2 goes back to the firmware's format with the
    // firmware's address, in the same locked update, and the bugcheck screen is B8G8R8A8 again.
    {
        static const ULONG cfg[3]={8,8,10},xbar[3]={0x00E40000ul,0x00B40000ul,0x00B40000ul},cnv[3]={8,8,10};
        unsigned i;
        for(i=0;i<3;i++){
            init(&d,fb);
            d.DcnPlaneFormats=TRUE;d.DcnPlaneFormat=i+1;
            d.DcnFirmwareSurfaceConfig=0x308;d.DcnFirmwareHubpretControl=0x00E4000Ful;d.DcnFirmwareCnvcFormat=8;
            model.config=0x300|cfg[i];model.crossbar=0x0000000Ful|xbar[i];model.cnvc=cnv[i];
            CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
            CHECK(d.SystemDisplayReady && model.scanned==d.DcnFirmwareAddress && !d.DcnDiverged);
            CHECK(model.config==0x308 && model.crossbar==0x00E4000Ful && model.cnvc==8 && model.format_writes==3);
            CHECK(d.DcnPlaneFormat==BC250_PLANE_FORMAT_ARGB8888);
        }
        // Only the format differs: the address and pitch are the firmware's already, and the check still sees it.
        init(&d,fb);
        d.DcnPlaneFormats=TRUE;d.DcnPlaneFormat=BC250_PLANE_FORMAT_ABGR8888;
        d.DcnFirmwareSurfaceConfig=8;d.DcnFirmwareHubpretControl=0x00E40000ul;d.DcnFirmwareCnvcFormat=8;
        model.scanned=d.DcnFirmwareAddress;model.pitch=7;model.config=8;model.crossbar=0x00B40000ul;model.cnvc=8;
        CHECK(DcnRestorePostDisplay(&d)==STATUS_SUCCESS);
        CHECK(model.crossbar==0x00E40000ul && model.format_writes==3 && d.DcnPlaneFormat==BC250_PLANE_FORMAT_ARGB8888);
        // Already the firmware's surface in the firmware's format: verified by reads, nothing written.
        writes=model.writes;
        CHECK(DcnRestorePostDisplay(&d)==STATUS_SUCCESS);CHECK(model.writes==writes && model.format_reads>=6);
    }
    // Changed geometry must never silently fall back to the old GOP mapping.
    init(&d,fb);d.RuntimeDisplayChanged=1;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_DEVICE_NOT_READY);
    CHECK(model.writes==0 && !d.SystemDisplayReady);
    d.SystemDisplayReady=TRUE;Bc250SystemDisplayWrite(&d,src,4,1,16,0,0);CHECK(fb[0]==0);
    // Runtime geometry remains distinct from immutable boot geometry.
    d.RuntimePost=d.Post;d.RuntimePost.Width=2;d.RuntimePost.Height=2;
    d.RuntimeFramebuffer=(UCHAR*)fb;d.RuntimeFramebufferLength=96;
    d.RuntimeFramebufferPhysical.QuadPart=0x490000000ll;
    d.RuntimePost.PhysicAddress=d.RuntimeFramebufferPhysical;d.RuntimePostValid=1;
    d.DcnPlaneFormats=TRUE;d.DcnFirmwareSurfaceConfig=8;
    d.DcnFirmwareHubpretControl=0x00E40000ul;d.DcnFirmwareCnvcFormat=8;
    writes=modeset_restores;
    CHECK(Bc250SystemDisplayEnable(&d,BC250_CHILD_UID,NULL,&width,&height,&fmt)==STATUS_SUCCESS);
    CHECK(width==2 && height==2 && model.scanned==0x490000000ull && d.Post.Width==4);
    CHECK(modeset_restores==writes && plane_blanks==1 && plane_unblanks==1 && !d.RuntimePlaneBlank);
    Bc250SystemDisplayWrite(&d,src,4,1,16,0,0);CHECK(fb[0]==1 && fb[1]==2 && fb[2]==0);
    // Ordinary allocation handoff preserves OTG visibility, but must release
    // HUBP so a later application flip can scan out without a cold unblank.
    // BD-170 r6: Resume runs once, with the plane blank and before the write sequence. A deposit is not programmed
    // here (the reserved buffer is the driver's own): the plane gets the reserved buffer, and End owes the deposit.
    model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    memset(&writer,0,sizeof(writer));writer.has=1;
    writer.desired.CardAddress=0x10000;writer.desired.Pitch=32;writer.desired.PlaneFormat=BC250_PLANE_FORMAT_ARGB8888;
    writer.desired.Bytes=96;writer.desired.Sequence=3;model.requested=0x123000;
    CHECK(DcnRuntimeSurface(&d,FALSE)==STATUS_SUCCESS);
    CHECK(!d.RuntimePlaneBlank && plane_unblanks==2 && (model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
    CHECK(writer.resumes==1 && writer.blank_at_resume && writer.requested_at_resume==0x123000);
    CHECK(writer.programmed==0 && model.scanned==0x490000000ull);
    memset(&writer,0,sizeof(writer));
    // Hardware visibility wins over a deliberately stale software cache.
    model.blank=0;d.DcnBlanked=TRUE;model.elapsed=0;model.scanned=0;
    CHECK(DcnRuntimeSurface(&d,FALSE)==STATUS_SUCCESS);
    CHECK(!d.DcnBlanked && !model.blank && !plane_is_blank);
    model.never_latch=1;model.elapsed=0;model.scanned=0;
    CHECK(DcnRuntimeSurface(&d,TRUE)==STATUS_IO_TIMEOUT);
    CHECK(d.RuntimePlaneBlank && plane_is_blank && plane_unblanks==4 && model.elapsed==50000);
    CHECK(d.DcnBlanked && (model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
    {
        BC250_LINK_SURFACE saved,refused;
        init(&d,fb);d.VramEnabled=TRUE;d.VramPhysical.QuadPart=0x400000000ll;d.VramLength=0x100000000ull;
        memset(&writer,0,sizeof(writer));
        d.DcnPlaneFormats=TRUE;d.DcnFirmwareSurfaceConfig=8;
        d.DcnFirmwareHubpretControl=0x00E40000;d.DcnFirmwareCnvcFormat=8;
        model.config=8;model.crossbar=0x00E40000;model.cnvc=8;model.pitch=7;
        model.scanned=0x480000000ull;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&saved)==STATUS_SUCCESS);
        CHECK(saved.Physical==model.scanned && saved.Physical!=d.DcnFirmwareAddress && saved.Pitch==32);
        CHECK(saved.Visible==1);
        CHECK(model.writes==0);
        model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&refused)==STATUS_DEVICE_BUSY && !refused.Physical);
        model.blank=0;model.unstable_blank=1;model.blank_reads=0;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&refused)==STATUS_DEVICE_BUSY && !refused.Physical);
        model.unstable_blank=0;model.hubp=HUBP0_DCHUBP_CNTL__HUBP_BLANK_EN_MASK;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&refused)==STATUS_SUCCESS && !refused.Visible);
        model.hubp=0;
        model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&refused)==STATUS_SUCCESS && !refused.Visible);
        model.blank=0;
        model.flip=HUBPREQ0_DCSURF_FLIP_CONTROL__SURFACE_FLIP_PENDING_MASK;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&refused)==STATUS_DEVICE_BUSY && refused.Physical==0);
        model.flip=0;model.scanned=0x500000000ull;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&refused)==STATUS_INVALID_PARAMETER && refused.Physical==0);
        model.scanned=0x480000000ull;model.config=0xff;
        CHECK(DcnLinkSurfaceCapture(&d,4,3,&refused)==STATUS_INVALID_PARAMETER);
        model.config=8;model.scanned=0x490000000ull;fb[0]=0x12345678;
        CHECK(DcnLinkSurfaceRestore(&d,&saved)==STATUS_SUCCESS);
        CHECK(model.scanned==saved.Physical && !model.blank && !plane_is_blank && fb[0]==0x12345678);
        saved.Visible=0;
        CHECK(DcnLinkSurfaceRestore(&d,&saved)==STATUS_SUCCESS);
        CHECK(d.DcnBlanked && plane_is_blank==0);
        model.never_latch=1;model.elapsed=0;model.scanned=0;
        saved.Visible=1;
        CHECK(DcnLinkSurfaceRestore(&d,&saved)==STATUS_IO_TIMEOUT);
        CHECK(d.DcnBlanked && plane_is_blank && fb[0]==0x12345678);
        CHECK(writer.resumes==3 && writer.programmed==0);
        // BD-170 r6: a deposit (a flip during the writer) replaces the saved surface, which the OS may have released:
        // programmed after Resume with the plane blank, proven on the plane by the same poll, then reported as
        // programmed with its sequence. The saved mode's size, Desired's pitch.
        model.never_latch=0;model.elapsed=0;model.scanned=0x480000000ull;
        memset(&writer,0,sizeof(writer));writer.has=1;
        writer.desired.CardAddress=0x90000000ull;writer.desired.Pitch=64;
        writer.desired.PlaneFormat=BC250_PLANE_FORMAT_ARGB8888;writer.desired.Bytes=64*3;writer.desired.Sequence=7;
        CHECK(DcnLinkSurfaceRestore(&d,&saved)==STATUS_SUCCESS);
        CHECK(writer.resumes==1 && writer.blank_at_resume && writer.requested_at_resume!=0x490000000ull);
        CHECK(model.requested==0x490000000ull && model.scanned==0x490000000ull && model.pitch==15);
        CHECK(writer.programmed==1 && writer.sequence==7);
        CHECK(d.DcnCurrentAddress==0x490000000ull && d.DcnCurrentPitch==64 && !d.DcnBlanked && !plane_is_blank);
        // A deposit that does not fit the saved mode refuses the restore before any write, with the plane blank:
        // another format, too few bytes, a card address outside the carve-out, and one not page aligned.
        {
            static const struct {ULONGLONG card;ULONG format;ULONGLONG bytes;} unfit[]={
                {0x90000000ull,BC250_PLANE_FORMAT_ABGR8888,64*3},
                {0x90000000ull,BC250_PLANE_FORMAT_ARGB8888,64*3-1},
                {0x100000000ull,BC250_PLANE_FORMAT_ARGB8888,64*3},
                {0x90000010ull,BC250_PLANE_FORMAT_ARGB8888,64*3}};
            unsigned k;
            for(k=0;k<sizeof(unfit)/sizeof(unfit[0]);k++){
                const ULONGLONG requested=model.requested;
                writer.desired.CardAddress=unfit[k].card;writer.desired.PlaneFormat=unfit[k].format;
                writer.desired.Bytes=unfit[k].bytes;writer.programmed=0;
                CHECK(DcnLinkSurfaceRestore(&d,&saved)==STATUS_DEVICE_NOT_READY);
                CHECK(model.requested==requested && plane_is_blank && d.DcnBlanked && writer.programmed==0);
            }
        }
        // No deposit: the saved surface, as before r6, and nothing reported as programmed.
        memset(&writer,0,sizeof(writer));model.elapsed=0;
        CHECK(DcnLinkSurfaceRestore(&d,&saved)==STATUS_SUCCESS);
        CHECK(model.scanned==saved.Physical && d.DcnCurrentAddress==saved.Physical && writer.resumes==1 && !writer.programmed);
    }
    printf("post display restore: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
