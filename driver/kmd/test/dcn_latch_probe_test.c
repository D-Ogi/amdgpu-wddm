// Actual probe with a deterministic device model. H1/H2 are hypotheses only.
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "dcn_latch_probe.h"
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
#include "flip_state.h"
typedef long NTSTATUS, LONG;
typedef unsigned long ULONG;
typedef long long LONGLONG;
typedef unsigned long long ULONGLONG;
typedef unsigned char BOOLEAN;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef struct _BC250_DEVICE {
    void *Mmio, *RuntimeFramebuffer;
    BOOLEAN VidPnFlipEnabled,VramEnabled,DcnWriteEnabled;
    LONG RetainedPowerPhase,RuntimeDisplayChanged;
    struct { ULONG Width,Height,Pitch; } Post,RuntimePost;
    struct { ULONG PipeChanged,InheritedFixed; } Modeset;
    LARGE_INTEGER VramPhysical,RuntimeFramebufferPhysical;
    ULONGLONG VramMcBase,VramLength,RuntimeFramebufferLength;
} BC250_DEVICE;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define MAXULONGLONG ULLONG_MAX
#define MAXLONGLONG LLONG_MAX
#define RtlZeroMemory(p,n) memset(p,0,n)
#define NT_SUCCESS(s) ((long)(s)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_INVALID_PARAMETER ((long)0xc000000d)
#define STATUS_INVALID_DEVICE_STATE ((long)0xc0000184)
#define STATUS_DEVICE_NOT_READY ((long)0xc00000a3)
#define STATUS_DEVICE_BUSY ((long)0x80000011)
#define STATUS_IO_TIMEOUT ((long)0xc00000b5)
#define STATUS_NOT_SUPPORTED ((long)0xc00000bb)
#define STATUS_BUFFER_TOO_SMALL ((long)0xc0000023)
#define STATUS_RETRY ((long)0xc000022d)
#define STATUS_IO_DEVICE_ERROR ((long)0xc0000185)
static unsigned checks,fails;
#define CHECK(x) do { ++checks; if (!(x)) { ++fails; printf("FAIL line %d: %s\n",__LINE__,#x); } } while (0)
static ULONG frame, lock, high, selector, surface, flip, triple, reads,writes,guards;
static ULONGLONG live,pending;
static LONGLONG clock_us;
static unsigned h2,armed,owner,resumed,ended,fault,begin_fail,resume_fail,end_fail;
static unsigned fail_read,fail_write,fail_guard,after_write,unlock_stuck,lock_stuck;
static unsigned no_frames,restore_fail,programs,invalid_owner_writes,power_on_read;
static unsigned unlock_delay,unlock_countdown;
static int irql,otg;
static long InterlockedCompareExchange(volatile LONG *p,LONG v,LONG cmp)
{ LONG old=*p;if(old==cmp)*p=v;return old; }
static int KeGetCurrentIrql(void) { return irql; }
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *f)
{ LARGE_INTEGER r;if(f)f->QuadPart=1000000;r.QuadPart=clock_us;return r; }
static void KeStallExecutionProcessor(ULONG us)
{
    ULONG next;
    clock_us+=us;
    next=no_frames?frame:(ULONG)(clock_us/1000);
    if(next!=frame) {
        frame=next;
        if(pending && (!lock || (h2 && armed))) { live=pending;pending=0; }
    }
}
static ULONG DisplaySourceWidth(const BC250_DEVICE *d) {return d->Post.Width;}
static ULONG DisplaySourceHeight(const BC250_DEVICE *d) {return d->Post.Height;}
static int DcnOtgRunning(const BC250_DEVICE *d) {(void)d;return otg;}
static NTSTATUS MmioDcnRead(BC250_DEVICE *d,ULONG offset,ULONG *v)
{
    ++reads;
    if(reads==power_on_read)d->RetainedPowerPhase=1;
    if(reads==fail_read)return STATUS_IO_DEVICE_ERROR;
    switch(offset) {
    case BC250_REG_DMU_OTG0_OTG_STATUS_FRAME_COUNT:*v=frame;break;
    case BC250_REG_DMU_OTG0_OTG_GLOBAL_CONTROL0:*v=selector;break;
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:
        if(unlock_countdown && --unlock_countdown==0)lock=0;
        *v=lock;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE_HIGH:*v=(ULONG)(live>>32);break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_EARLIEST_INUSE:*v=(ULONG)live;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_SURFACE_CONTROL:*v=surface;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL:*v=flip;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_FLIP_CONTROL2:*v=triple;break;
    default:CHECK(0);return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}
static NTSTATUS MmioDcnWriteEx(BC250_DEVICE *d,ULONG offset,ULONG value,BOOLEAN quiet)
{
    (void)quiet;++writes;
    if(!owner || !d->Mmio || d->RetainedPowerPhase)++invalid_owner_writes;
    if(writes==fail_write && !after_write)return STATUS_IO_DEVICE_ERROR;
    switch(offset) {
    case BC250_REG_DMU_OTG0_OTG_MASTER_UPDATE_LOCK:
        if(value)lock=value|(lock_stuck?0:OTG0_OTG_MASTER_UPDATE_LOCK__UPDATE_LOCK_STATUS_MASK);
        else if(!unlock_stuck) {if(unlock_delay)unlock_countdown=unlock_delay;else lock=0;}
        break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS_HIGH:high=value;break;
    case BC250_REG_DMU_HUBPREQ0_DCSURF_PRIMARY_SURFACE_ADDRESS:
        pending=((ULONGLONG)high<<32)|value;armed=0;break;
    case BC250_REG_DMU_OTG0_OTG_TRIGA_MANUAL_TRIG:armed=1;break;
    default:CHECK(0);return STATUS_INVALID_PARAMETER;
    }
    return writes==fail_write?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
}
static BOOLEAN WddmPrimaryExclusiveBegin(BC250_DEVICE *d,ULONG kind,ULONG *g)
{ (void)d;CHECK(kind==BC250_WRITER_LATCH_PROBE);if(begin_fail)return FALSE;owner=1;*g=12;return TRUE; }
static NTSTATUS WddmWriterResume(BC250_DEVICE *d)
{ (void)d;if(resume_fail)return STATUS_IO_DEVICE_ERROR;resumed=1;return STATUS_SUCCESS; }
static NTSTATUS WddmLatchProbeCheck(BC250_DEVICE *d,ULONG gen,BOOLEAN fail)
{
    (void)d;++guards;CHECK(gen==12);
    if(fail)fault=1;
    if(guards==fail_guard || !owner || !resumed || fault)return STATUS_INVALID_DEVICE_STATE;
    return STATUS_SUCCESS;
}
static NTSTATUS WddmPrimaryExclusiveEnd(BC250_DEVICE *d,ULONG gen,BOOLEAN invalidate)
{
    (void)d;CHECK(gen==12 && !invalidate && owner);ended++;owner=0;
    return end_fail || fault?STATUS_IO_DEVICE_ERROR:STATUS_SUCCESS;
}
static NTSTATUS DcnFlipSourceAddress(BC250_DEVICE *d,ULONGLONG card,ULONG pitch,ULONG format,
    ULONGLONG bytes,BOOLEAN immediate,void *witness,BOOLEAN *wrote)
{
    (void)format;(void)witness;
    CHECK(owner && resumed && !lock && !immediate);
    CHECK(pitch==d->Post.Pitch && bytes==(ULONGLONG)pitch*d->Post.Height);
    ++programs;*wrote=FALSE;
    if(restore_fail && programs>=3)return STATUS_IO_DEVICE_ERROR;
    *wrote=TRUE;pending=card-d->VramMcBase+(ULONGLONG)d->VramPhysical.QuadPart;
    armed=1;return STATUS_SUCCESS;
}
/* ACTUAL_PROBE */
static void Setup(BC250_DEVICE *d)
{
    memset(d,0,sizeof(*d));d->Mmio=d;d->RuntimeFramebuffer=d;
    d->Post.Width=1920;d->Post.Height=1200;d->Post.Pitch=7680;
    d->VidPnFlipEnabled=d->VramEnabled=d->DcnWriteEnabled=1;
    d->VramPhysical.QuadPart=0x100000000ull;d->VramMcBase=0x200000000ull;
    d->VramLength=0x80000000ull;d->RuntimeFramebufferPhysical.QuadPart=0x101000000ull;
    d->RuntimeFramebufferLength=4096ull*2160*4;
    frame=lock=high=selector=surface=flip=triple=reads=writes=guards=0;
    live=(ULONGLONG)d->RuntimeFramebufferPhysical.QuadPart;pending=0;clock_us=0;
    h2=armed=owner=resumed=ended=fault=begin_fail=resume_fail=end_fail=0;
    fail_read=fail_write=fail_guard=after_write=unlock_stuck=lock_stuck=0;
    no_frames=restore_fail=programs=invalid_owner_writes=power_on_read=0;
    unlock_delay=unlock_countdown=0;irql=0;otg=1;
}
int main(void)
{
    BC250_DEVICE d;BC250_LATCH_PROBE_RESULT r;
    unsigned model,arm,i,nread,nwrite,nguard,applied;long status;
    for(model=0;model<2;model++)for(arm=0;arm<Bc250LatchArmCount;arm++) {
        Setup(&d);h2=model;
        CHECK(DcnLatchProbe(&d,arm,&r)==STATUS_SUCCESS);
        CHECK(r.Restored && r.BoundaryObserved && ended==1 && !owner && !lock && !fault);
        CHECK(r.Boundary.Physical==((arm==0 || (arm==2 && model))?r.AddressB:r.AddressA));
        CHECK(r.AfterUnlock.Physical==r.AddressB && live==r.AddressA);
        CHECK(!invalid_owner_writes && clock_us<100000);
    }
    Setup(&d);unlock_delay=5;CHECK(DcnLatchProbe(&d,1,&r)==STATUS_SUCCESS && !lock);
    Setup(&d);CHECK(DcnLatchProbe(&d,1,&r)==STATUS_SUCCESS);
    nread=reads;nwrite=writes;nguard=guards;
    // Every MMIO read, write (before/after effect) and ownership checkpoint can fail.
    for(i=1;i<=nread;i++) {
        Setup(&d);fail_read=i;status=DcnLatchProbe(&d,1,&r);
        CHECK(!NT_SUCCESS(status));CHECK(!owner && !lock && !invalid_owner_writes);
        CHECK(clock_us<500000 && (!ended || r.Restored || fault));
    }
    for(applied=0;applied<2;applied++)for(i=1;i<=nwrite;i++) {
        Setup(&d);fail_write=i;after_write=applied;status=DcnLatchProbe(&d,1,&r);
        CHECK(!NT_SUCCESS(status));CHECK(!owner && !lock && !invalid_owner_writes);
        CHECK(clock_us<500000 && (r.Restored || fault));
    }
    for(i=1;i<=nguard;i++) {
        Setup(&d);fail_guard=i;status=DcnLatchProbe(&d,1,&r);
        CHECK(!NT_SUCCESS(status));CHECK(!owner && !lock && !invalid_owner_writes);
        CHECK(clock_us<500000 && (r.Restored || fault));
    }
    // Power withdrawal prohibits further MMIO writes, including cleanup.
    for(i=1;i<=nread;i++) {
        Setup(&d);power_on_read=i;status=DcnLatchProbe(&d,1,&r);
        CHECK(!NT_SUCCESS(status));CHECK(!owner && !invalid_owner_writes);
        CHECK(clock_us<500000 && (!ended || fault));
    }
    Setup(&d);unlock_stuck=1;CHECK(!NT_SUCCESS(DcnLatchProbe(&d,1,&r)) && fault && !r.Restored && ended==1);
    Setup(&d);lock_stuck=1;CHECK(!NT_SUCCESS(DcnLatchProbe(&d,1,&r)) && !lock && r.Restored);
    Setup(&d);no_frames=1;CHECK(DcnLatchProbe(&d,0,&r)==STATUS_IO_TIMEOUT && clock_us<100000 && ended==1);
    Setup(&d);restore_fail=1;CHECK(!NT_SUCCESS(DcnLatchProbe(&d,0,&r)) && fault && !r.Restored);
    Setup(&d);begin_fail=1;CHECK(!NT_SUCCESS(DcnLatchProbe(&d,0,&r)) && !writes && !programs && !ended);
    Setup(&d);resume_fail=1;CHECK(!NT_SUCCESS(DcnLatchProbe(&d,0,&r)) && !programs && fault && ended==1);
    Setup(&d);end_fail=1;CHECK(!NT_SUCCESS(DcnLatchProbe(&d,0,&r)) && r.Restored);
    for(i=0;i<12;i++) {
        Setup(&d);
        switch(i) {
        case 0:d.RuntimeFramebufferLength=1;break;
        case 1:d.Post.Pitch++;break;
        case 2:d.Modeset.PipeChanged=1;break;
        case 3:d.Modeset.InheritedFixed=1;break;
        case 4:d.RetainedPowerPhase=1;break;
        case 5:d.Mmio=NULL;break;
        case 6:otg=0;break;
        case 7:surface=HUBPREQ0_DCSURF_SURFACE_CONTROL__PRIMARY_SURFACE_DCC_EN_MASK;break;
        case 8:triple=HUBPREQ0_DCSURF_FLIP_CONTROL2__SURFACE_TRIPLE_BUFFER_ENABLE_MASK;break;
        case 9:selector=OTG0_OTG_GLOBAL_CONTROL0__OTG_MASTER_UPDATE_LOCK_SEL_MASK;break;
        case 10:lock=OTG0_OTG_MASTER_UPDATE_LOCK__OTG_MASTER_UPDATE_LOCK_MASK;break;
        case 11:irql=2;break;
        }
        CHECK(!NT_SUCCESS(DcnLatchProbe(&d,0,&r)) && !writes && !programs && !ended);
    }
    Setup(&d);CHECK(DcnLatchProbe(NULL,0,&r)==r.Status && !NT_SUCCESS(r.Status));
    Setup(&d);CHECK(DcnLatchProbe(&d,Bc250LatchArmCount,&r)==r.Status && !NT_SUCCESS(r.Status));
    Setup(&d);CHECK(DcnLatchProbe(&d,0,NULL)==STATUS_INVALID_PARAMETER);
    printf("latch probe: %u checks, %u failures (software H1/H2 and fault injection only)\n",checks,fails);
    return fails?1:0;
}
