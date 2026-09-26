// Deterministic interrupt-lock model, not a hardware/concurrency stress test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
typedef unsigned long ULONG;
typedef long LONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
typedef void* PVOID;
typedef void* HANDLE;
typedef int KIRQL;
typedef int64_t LONGLONG;
typedef struct {LONGLONG QuadPart;} LARGE_INTEGER;
typedef struct {int Lock,Stopping,VSyncArmed,VSyncTimer,VSyncDpc;long VSyncTicks;} BC250_WDDM;
typedef struct { unsigned VidPnSourceId;BOOLEAN Visible;} DXGKARG_SETVIDPNSOURCEVISIBILITY;
typedef struct {struct {unsigned PathPowerTransition,PathPoweredOff;} Flags;} DXGKARG_COMMITVIDPN;
/* VISIBILITY_EVENT_TYPE */
typedef struct {
    BC250_WDDM* Wddm;
    void *Mmio,*Framebuffer;size_t FramebufferLength;
    BOOLEAN DcnWriteEnabled,DcnBlanked,SourceVisible,VidPnFlipEnabled;
    volatile LONG VisibilityCalls,CommitPowerCalls;
    ULONG VisibilityLastSource;
    BOOLEAN VisibilityLastRequested,CommitLastPowerTransition,CommitLastPoweredOff;
    NTSTATUS VisibilityLastStatus;
    ULONG VisibilityTrueCalls,VisibilityFalseCalls,VisibilityFailures;
    NTSTATUS VisibilityFirstTrueStatus,VisibilityLastTrueStatus;
    BC250_VISIBILITY_EVENT VisibilityHistory[BC250_VISIBILITY_HISTORY_COUNT];
    volatile long DcnVsyncArmed,DcnVsyncRefused,DcnVsyncTicks,DcnVsyncAcked;
    struct {
        void* DeviceHandle;
        NTSTATUS (*DxgkCbSynchronizeExecution)(void*,BOOLEAN(*)(void*),void*,ULONG,BOOLEAN*);
        void (*DxgkCbQueueDpc)(void*);
    } Dxgk;
} BC250_DEVICE;
#define BC250_WDDM_VSYNC_MS 16
#define GuardLog(...) ((void)0)
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001u)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define BC250_DCNFLIP_POLL_MAX_US 50000
#define BC250_DCNFLIP_POLL_STEP_US 100
#define RtlZeroMemory(p,n) memset(p,0,n)
#define ALL_CLEAR (OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_EVENT_CLEAR_MASK | OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_EVENT_CLEAR_MASK | OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK | OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_EVENT_CLEAR_MASK)
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static struct {ULONG sync,blank,dbuf,last_sync;unsigned irq,sync_calls,queues,arms,writes,stalls,hold_blank;NTSTATUS sync_status;} model;
static LONGLONG TestQpc;
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER*frequency)
{
    LARGE_INTEGER value;
    if(frequency)frequency->QuadPart=10000000;
    value.QuadPart=++TestQpc;return value;
}
static NTSTATUS DcnVsyncEnable(BC250_DEVICE*,BOOLEAN);
static long InterlockedExchange(volatile long*p,long v){long old=*p;CHECK(model.irq);*p=v;return old;}
static long InterlockedIncrement(volatile long*p){return ++*p;}
static NTSTATUS MockSync(void*h,BOOLEAN(*f)(void*),void*c,ULONG message,BOOLEAN*out)
{
    (void)h;CHECK(!model.irq && message==0);model.sync_calls++;
    if(!NT_SUCCESS(model.sync_status))return model.sync_status;
    model.irq=1;*out=f(c);model.irq=0;return STATUS_SUCCESS;
}
static void MockQueue(void*h){(void)h;CHECK(model.irq);model.queues++;}
static void KeAcquireSpinLock(int*l,KIRQL*i){(void)l;CHECK(!model.irq);*i=0;model.arms++;}
static void KeReleaseSpinLock(int*l,KIRQL i){(void)l;(void)i;CHECK(!model.irq);}
static void KeSetTimerEx(int*t,LARGE_INTEGER due,unsigned period,int*d){(void)t;(void)due;(void)period;(void)d;}
static void KeCancelTimer(int*t){(void)t;}
static void KeStallExecutionProcessor(ULONG us){CHECK(us==100);model.stalls++;TestQpc+=(LONGLONG)us*10;}
static NTSTATUS MmioDcnRead(const BC250_DEVICE*d,ULONG reg,ULONG*out)
{
    if(!d->Mmio)return STATUS_DEVICE_NOT_READY;
    if(reg==BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS){CHECK(model.irq);*out=model.sync;}
    else if(reg==BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL)*out=model.blank;
    else if(reg==BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL)*out=model.dbuf;
    else {CHECK(0);return STATUS_INVALID_PARAMETER;}
    return STATUS_SUCCESS;
}
static NTSTATUS MmioDcnWriteEx(const BC250_DEVICE*d,ULONG reg,ULONG value,BOOLEAN quiet)
{
    CHECK(quiet);
    if(!d->DcnWriteEnabled)return STATUS_DEVICE_NOT_READY;
    model.writes++;
    if(reg==BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS){
        CHECK(model.irq);CHECK((value&ALL_CLEAR)==OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK);
        model.last_sync=value;model.sync=value&~ALL_CLEAR;
        if(value&OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_CLEAR_MASK)
            model.sync&=~OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK;
    } else if(reg==BC250_REG_DMU_OTG0_OTG_BLANK_CONTROL){
        model.blank=value&~OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        if(value&OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK)
            model.blank|=OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
        if(model.hold_blank)model.blank|=OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    } else if(reg==BC250_REG_DMU_OTG0_OTG_DOUBLE_BUFFER_CONTROL)model.dbuf=value;
    else {CHECK(0);return STATUS_INVALID_PARAMETER;}
    return STATUS_SUCCESS;
}
/* ACTUAL_SOURCE */
int main(void)
{
    BC250_DEVICE d={0};BC250_WDDM w={0};DXGKARG_SETVIDPNSOURCEVISIBILITY visible={0};
    ULONG fb[16],before[16];unsigned i,writes,arms;
    const ULONG other=OTG0_OTG_GLOBAL_SYNC_STATUS__VSTARTUP_INT_EN_MASK|OTG0_OTG_GLOBAL_SYNC_STATUS__VREADY_INT_EN_MASK;
    d.Wddm=&w;d.Mmio=&model;d.Framebuffer=fb;d.FramebufferLength=sizeof(fb);d.DcnWriteEnabled=TRUE;d.VidPnFlipEnabled=TRUE;d.SourceVisible=TRUE;
    d.Dxgk.DxgkCbSynchronizeExecution=MockSync;d.Dxgk.DxgkCbQueueDpc=MockQueue;
    model.sync=other|ALL_CLEAR;
    CHECK(DcnVsyncEnable(&d,TRUE)==STATUS_SUCCESS);
    CHECK(d.DcnVsyncArmed==1 && model.sync_calls==1 && (model.sync&other)==other);
    CHECK(model.sync&OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK);
    model.sync|=OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK;
    model.irq=1;CHECK(DcnVsyncInterrupt(&d));model.irq=0;
    CHECK(d.DcnVsyncTicks==1 && d.DcnVsyncAcked==1 && model.queues==1);
    CHECK((model.sync&other)==other && d.DcnVsyncArmed==1);
    CHECK(DcnVsyncEnable(&d,FALSE)==STATUS_SUCCESS);
    CHECK(d.DcnVsyncArmed==0 && !(model.sync&OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_INT_EN_MASK));
    CHECK(DcnVsyncEnable(&d,TRUE)==STATUS_SUCCESS); // gfx rearm uses the same synchronized owner
    writes=model.writes;model.sync_status=STATUS_UNSUCCESSFUL;
    CHECK(DcnVsyncEnable(&d,FALSE)==STATUS_UNSUCCESSFUL);CHECK(model.writes==writes && d.DcnVsyncArmed==1);
    model.sync_status=STATUS_SUCCESS;
    // Preserve source pixels and unrelated control bits across repeated hide/show.
    model.blank=0x40000000u;model.dbuf=0x80000000u|OTG0_OTG_DOUBLE_BUFFER_CONTROL__OTG_BLANK_DATA_DOUBLE_BUFFER_EN_MASK;
    for(i=0;i<16;i++)fb[i]=before[i]=0x12340000u+i;
    for(i=0;i<5;i++){
        visible.Visible=FALSE;arms=model.arms;
        CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_SUCCESS);
        CHECK(!d.SourceVisible && d.DcnBlanked && (model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
        CHECK(model.arms==arms && d.DcnVsyncArmed==1);
        CHECK((model.blank&0x40000000u)!=0 && model.dbuf==0x80000000u);
        CHECK(memcmp(fb,before,sizeof(fb))==0);
        visible.Visible=TRUE;
        CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_SUCCESS);
        CHECK(d.SourceVisible && !d.DcnBlanked && !(model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
        CHECK(model.arms==arms+1 && d.DcnVsyncArmed==1);
        CHECK(memcmp(fb,before,sizeof(fb))==0);
    }
    // Reinitialization/inherited hardware: cached false cannot suppress unblank.
    d.DcnBlanked=FALSE;d.SourceVisible=FALSE;visible.Visible=TRUE;
    model.blank|=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
        OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    writes=model.writes;
    CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_SUCCESS);
    CHECK(model.writes==writes+1 && d.SourceVisible && !d.DcnBlanked);
    CHECK(!(model.blank&(OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK |
        OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK)));
    CHECK(memcmp(fb,before,sizeof(fb))==0);
    // Already-visible positive path samples hardware and avoids redundant writes.
    writes=model.writes;CHECK(DcnSetVisibility(&d,TRUE)==STATUS_SUCCESS);CHECK(model.writes==writes);
    // Current state can still be blank with the request bit cleared.
    model.blank|=OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    CHECK(DcnSetVisibility(&d,TRUE)==STATUS_SUCCESS);
    CHECK(!(model.blank&OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK));
    d.DcnWriteEnabled=FALSE;visible.Visible=FALSE;
    CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_DEVICE_NOT_READY);
    CHECK(d.SourceVisible && !d.DcnBlanked && memcmp(fb,before,sizeof(fb))==0);
    // Preserve the inherited no-MMIO display-only show path and its fallback.
    d.VidPnFlipEnabled=FALSE;d.Mmio=NULL;visible.Visible=FALSE;
    CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_SUCCESS && !d.SourceVisible);
    for(i=0;i<16;i++)CHECK(fb[i]==0);
    visible.Visible=TRUE;CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_SUCCESS && d.SourceVisible);
    CHECK(DcnSetVisibility(&d,TRUE)==STATUS_SUCCESS);
    // Last request survives beyond the first32 per-device log calls.
    for(i=0;i<40;i++)CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_SUCCESS);
    CHECK(d.VisibilityCalls>32 && d.VisibilityLastSource==0 && d.VisibilityLastRequested==TRUE && d.VisibilityLastStatus==STATUS_SUCCESS);
    visible.VidPnSourceId=3;visible.Visible=FALSE;
    CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_INVALID_PARAMETER);
    CHECK(d.VisibilityLastSource==3 && !d.VisibilityLastRequested && d.VisibilityLastStatus==STATUS_INVALID_PARAMETER && d.SourceVisible);
    visible.VidPnSourceId=0;d.VidPnFlipEnabled=TRUE;
    CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_DEVICE_NOT_READY);
    CHECK(d.VisibilityLastSource==0 && !d.VisibilityLastRequested && d.VisibilityLastStatus==STATUS_DEVICE_NOT_READY && d.SourceVisible);
    {DXGKARG_COMMITVIDPN commit={{1,1}};DisplayRetainCommitPower(&d,&commit);
     CHECK(d.CommitPowerCalls==1 && d.CommitLastPowerTransition && d.CommitLastPoweredOff);
     commit.Flags.PathPoweredOff=0;DisplayRetainCommitPower(&d,&commit);
     CHECK(d.CommitPowerCalls==2 && d.CommitLastPowerTransition && !d.CommitLastPoweredOff);}
    CHECK(d.VisibilityTrueCalls+d.VisibilityFalseCalls==(ULONG)d.VisibilityCalls);
    CHECK(d.VisibilityFailures==3 && d.VisibilityFirstTrueStatus==STATUS_SUCCESS && d.VisibilityLastTrueStatus==STATUS_SUCCESS);
    for(i=0;i<BC250_VISIBILITY_HISTORY_COUNT;i++){
        ULONG sequence=(ULONG)d.VisibilityCalls-BC250_VISIBILITY_HISTORY_COUNT+i+1;
        BC250_VISIBILITY_EVENT*e=&d.VisibilityHistory[(sequence-1)%BC250_VISIBILITY_HISTORY_COUNT];
        CHECK(e->Call==sequence && e->EndQpc>e->BeginQpc);
        if(sequence==(ULONG)d.VisibilityCalls)CHECK(e->Requested==FALSE && e->Status==STATUS_DEVICE_NOT_READY && e->SourceVisible);
        else if(sequence+1==(ULONG)d.VisibilityCalls)CHECK(e->Source==3 && e->Status==STATUS_INVALID_PARAMETER);
        else CHECK(e->Requested==TRUE && e->Status==STATUS_SUCCESS);
    }
    CHECK(model.stalls==0); // immediate model latch is the positive control, no busy polling required
    // The diagnostic must retain TRUE timeout even after the next FALSE succeeds.
    d.Mmio=&model;d.DcnWriteEnabled=TRUE;d.DcnBlanked=TRUE;d.SourceVisible=FALSE;
    model.blank=OTG0_OTG_BLANK_CONTROL__OTG_BLANK_DATA_EN_MASK|OTG0_OTG_BLANK_CONTROL__OTG_CURRENT_BLANK_STATE_MASK;
    model.hold_blank=1;visible.Visible=TRUE;
    CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_IO_TIMEOUT);
    visible.Visible=FALSE;CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_SUCCESS);
    {BC250_VISIBILITY_EVENT*show=&d.VisibilityHistory[((ULONG)d.VisibilityCalls-2)%BC250_VISIBILITY_HISTORY_COUNT];
     BC250_VISIBILITY_EVENT*hide=&d.VisibilityHistory[((ULONG)d.VisibilityCalls-1)%BC250_VISIBILITY_HISTORY_COUNT];
     CHECK(show->Requested && show->Status==STATUS_IO_TIMEOUT && show->EndQpc-show->BeginQpc==500001);
     CHECK(!hide->Requested && hide->Status==STATUS_SUCCESS && hide->Blanked);
     CHECK(d.VisibilityFailures==4 && d.VisibilityLastTrueStatus==STATUS_IO_TIMEOUT && d.VisibilityLastStatus==STATUS_SUCCESS);}
    printf("display visibility/sync: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
