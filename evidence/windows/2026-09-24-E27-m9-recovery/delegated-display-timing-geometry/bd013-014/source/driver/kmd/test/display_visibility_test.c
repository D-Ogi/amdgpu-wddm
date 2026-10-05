// Deterministic interrupt-lock model, not a hardware/concurrency stress test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "regs.generated.h"
#include "dcn_2_0_1_sh_mask.h"
typedef unsigned long ULONG;
typedef int32_t NTSTATUS;
typedef int BOOLEAN;
typedef void* PVOID;
typedef void* HANDLE;
typedef int KIRQL;
typedef int64_t LONGLONG;
typedef struct {LONGLONG QuadPart;} LARGE_INTEGER;
typedef struct {int Lock,Stopping,VSyncArmed,VSyncTimer,VSyncDpc;long VSyncTicks;} BC250_WDDM;
typedef struct { unsigned VidPnSourceId;BOOLEAN Visible;} DXGKARG_SETVIDPNSOURCEVISIBILITY;
typedef struct {
    BC250_WDDM* Wddm;
    void *Mmio,*Framebuffer;size_t FramebufferLength;
    BOOLEAN DcnWriteEnabled,DcnBlanked,SourceVisible,VidPnFlipEnabled;
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
static struct {ULONG sync,blank,dbuf,last_sync;unsigned irq,sync_calls,queues,arms,writes,stalls;NTSTATUS sync_status;} model;
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
static void KeStallExecutionProcessor(ULONG us){CHECK(us==100);model.stalls++;}
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
    d.DcnWriteEnabled=FALSE;visible.Visible=FALSE;
    CHECK(Bc250SetVidPnSourceVisibility(&d,&visible)==STATUS_DEVICE_NOT_READY);
    CHECK(d.SourceVisible && !d.DcnBlanked && memcmp(fb,before,sizeof(fb))==0);
    CHECK(model.stalls==0); // immediate model latch is the positive control, no busy polling required
    printf("display visibility/sync: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
