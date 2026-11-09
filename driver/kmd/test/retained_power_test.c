// Actual-source retained-owner tests. Kernel operations are ordered mock events.
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "plane_format.h"   /* M15.14: BC250_PLANE_FORMAT_* */
#ifndef _Inout_
#define _Inout_
#endif
#include "power_drain.h"    /* BD-188: the bounded wait of the way down */
typedef int BOOLEAN, NTSTATUS, KIRQL;
typedef unsigned UINT;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
#define KernelMode 0
typedef struct {long long QuadPart;} LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE -1
#define STATUS_DEVICE_NOT_READY -2
#define STATUS_DEVICE_BUSY -3
#define NT_SUCCESS(x) ((x)>=0)
#define BC250_WDDM_NODE_COUNT_MAX 3
#define BC250_WDDM_NODE_3D 0
#define BC250_WDDM_NODE_COMPUTE 2
#define BC250_WDDM_VSYNC_MS 16
typedef struct {
 int Lock,Stopping,RetainedPowerPause,HwPending,DeferredValid,PagingHwPending;
 void *PagingHead,*PagingTail;
 int ReportActive,ReportAgain;
 /* Node 2's channel (compute-node.md): a job on the compute ring or a fence held for it is outstanding work. */
 int ComputeHwPending,ComputeDeferredValid,ComputeTimer,ComputeDpc;
 int ActiveSubmissions[3],CompletionPending[3],PreemptionPending[3];
 int RefusalPending[3],RejectedPending[3],WatchdogFaulted[3];
 int PrimaryNeedsRestore;
 /* BD-188 D-2: the forced pause's record of lost work (before Objects: it may change, the owners may not). */
 ULONG PowerLostSeq[3],PowerLostWork;
 int VSyncArmed,VSyncEnabled,VSyncTimer,SubmitTimer,PagingSubmitTimer,PagingDrainTimer;
 int SubmitDpc,PagingSubmitDpc,PagingDrainDpc,VSyncDpc,ReportDpc;
 LARGE_INTEGER VSyncLast,VSyncFrequency;
 // Independent owner identities and fence history must survive byte for byte.
 void *Objects,*Aperture,*CaptureOwner;
 unsigned SubmittedFence[2],LastReportedFence[2],LastCompletedFence;
} BC250_WDDM;
typedef struct {
 BC250_WDDM *Wddm;
 int DcnVsyncArmed,SourceVisible,DcnBlanked,VidPnFlipEnabled;
 /* M15.14 (0.7.216.20): the plane format state; the retained pause makes the current format unknown. */
 int DcnPlaneFormats;unsigned long DcnPlaneFormat;
} BC250_DEVICE;
static unsigned checks,failures,flushes,cancels,removes,arms,mmio;
static int currentIrql,lockHeld,gfxReady=1,pagingReady=1,enableStatus;
static BC250_WDDM *observed;
#define CHECK(x) do {checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static int KeGetCurrentIrql(void){return currentIrql;}
static void KeAcquireSpinLock(int*l,int*i){(void)l;CHECK(!lockHeld);lockHeld=1;*i=0;}
static void KeReleaseSpinLock(int*l,int i){(void)l;(void)i;CHECK(lockHeld);lockHeld=0;}
static void KeFlushQueuedDpcs(void){CHECK(!lockHeld);flushes++;}
static void KeCancelTimer(int*t){(void)t;CHECK(lockHeld);cancels++;}
static void KeRemoveQueueDpc(int*d){(void)d;CHECK(!lockHeld && observed->Stopping);removes++;}
static void KeSetTimerEx(int*t,LARGE_INTEGER d,int ms,int*k){(void)t;(void)k;CHECK(lockHeld && !observed->Stopping && d.QuadPart==-160000 && ms==16);arms++;}
static NTSTATUS DcnVsyncEnable(BC250_DEVICE*d,int on){CHECK(lockHeld);mmio++;if(enableStatus)return enableStatus;d->DcnVsyncArmed=on;return 0;}
static int GfxSubmitReady(BC250_DEVICE*d){(void)d;CHECK(!lockHeld);return gfxReady;}
static int GfxPagingSubmitReady(BC250_DEVICE*d){(void)d;CHECK(!lockHeld);return pagingReady;}
static LONGLONG InterlockedExchange64(volatile LONGLONG*p,LONGLONG v){LONGLONG old=*p;CHECK(lockHeld);*p=v;return old;}
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER*f){LARGE_INTEGER n={1234};f->QuadPart=1000000;return n;}
/* BD-188 drain model: the interrupt clock moves only in the delay. stepTicks is how far one 1 ms delay really moves
 * it (the timer resolution can make it 15.6 ms). completeAfter > 0: the pending job completes on its normal path
 * (here: HwPending clears) in that delay. delays stops at a hard limit so that an unbounded wait ends the test. */
static ULONGLONG interruptTime=0,stepTicks=10000;
static unsigned delays,completeAfter,logs,delayLimitHit;
static ULONGLONG KeQueryInterruptTime(void){return interruptTime;}
static NTSTATUS KeDelayExecutionThread(int mode,int alertable,LARGE_INTEGER*interval){
 (void)mode;(void)alertable;CHECK(!lockHeld && interval->QuadPart==-10000);
 delays++;interruptTime+=stepTicks;
 if(completeAfter && delays==completeAfter) observed->HwPending=0;
 if(delays>=100000){delayLimitHit=1;observed->HwPending=0;observed->WatchdogFaulted[0]=0;}
 return 0;}
static void GuardLog(const char*f,...){(void)f;logs++;}
/* BD-188 D-2: the newest job of each ring queue (Seq 0: none), and the restore answer with lost work. */
typedef struct {ULONG Seq;} BC250_GFX_COMPLETION;
static BC250_GFX_COMPLETION lostJob[3];
static int restoredLost=1;
static unsigned inserts,requeues;
static int WddmRingPendingLocked(const BC250_WDDM*w,UINT n){CHECK(lockHeld);return n==BC250_WDDM_NODE_COMPUTE?w->ComputeHwPending:w->HwPending;}
static BC250_GFX_COMPLETION*WddmRingQueue(BC250_WDDM*w,UINT n){(void)w;return &lostJob[n];}
static BC250_GFX_COMPLETION*Bc250GfxQueueTail(BC250_GFX_COMPLETION*q){return q->Seq?q:NULL;}
static int GfxSubmitRestoredWithLostWork(BC250_DEVICE*d){(void)d;CHECK(!lockHeld);return restoredLost;}
static int KeInsertQueueDpc(int*k,void*a,void*b){(void)k;(void)a;(void)b;CHECK(lockHeld&&!observed->Stopping);inserts++;return 1;}
static void WddmRequeuePagingDrain(BC250_WDDM*w){(void)w;CHECK(!lockHeld&&!observed->Stopping);requeues++;}/* ACTUAL_SOURCE */
static void setup(BC250_DEVICE*d,BC250_WDDM*w){
 memset(w,0,sizeof(*w));memset(d,0,sizeof(*d));d->Wddm=w;observed=w;
 d->DcnBlanked=1;d->VidPnFlipEnabled=1;d->DcnVsyncArmed=1;
 w->VSyncEnabled=1;w->Objects=d;w->Aperture=&w->Lock;w->CaptureOwner=&w->Objects;
 w->SubmittedFence[0]=123;w->SubmittedFence[1]=456;
 w->LastReportedFence[0]=123;w->LastReportedFence[1]=456;w->LastCompletedFence=456;
 flushes=cancels=removes=arms=mmio=0;currentIrql=enableStatus=0;gfxReady=pagingReady=1;
 delays=completeAfter=logs=delayLimitHit=0;stepTicks=10000;
 inserts=requeues=0;restoredLost=1;memset(lostJob,0,sizeof(lostJob));
}
static void retained(BC250_WDDM*w,const BC250_WDDM*before){
 size_t off=offsetof(BC250_WDDM,Objects);
 CHECK(memcmp((char*)w+off,(const char*)before+off,sizeof(*w)-off)==0);
}
int main(void){
 BC250_DEVICE d;BC250_WDDM w,before;unsigned i;
 const size_t scalar[]={offsetof(BC250_WDDM,HwPending),offsetof(BC250_WDDM,DeferredValid),offsetof(BC250_WDDM,PagingHwPending),offsetof(BC250_WDDM,ReportActive),offsetof(BC250_WDDM,ReportAgain),
                         offsetof(BC250_WDDM,ComputeHwPending),offsetof(BC250_WDDM,ComputeDeferredValid)};
 const size_t arrays[]={offsetof(BC250_WDDM,ActiveSubmissions),offsetof(BC250_WDDM,CompletionPending),offsetof(BC250_WDDM,PreemptionPending),offsetof(BC250_WDDM,RefusalPending),offsetof(BC250_WDDM,RejectedPending),offsetof(BC250_WDDM,WatchdogFaulted)};
 setup(&d,&w);before=w;
 d.DcnPlaneFormats=1;d.DcnPlaneFormat=BC250_PLANE_FORMAT_ABGR8888;
 CHECK(WddmSuspendRetained(&d)==0 && w.Stopping && w.RetainedPowerPause && w.PrimaryNeedsRestore);
 /* M15.14: the format registers may change with the address over power loss, so the next flip writes them. */
 CHECK(d.DcnPlaneFormat==BC250_PLANE_FORMAT_NONE);
 // KMD172: + paging drain timer; the compute node: + its poll timer and DPC.
 CHECK(d.Wddm==&w && !d.DcnVsyncArmed && flushes==2 && cancels==5 && removes==6);
 retained(&w,&before);
 CHECK(WddmSuspendRetained(&d)==0 && cancels==5 && removes==6);
 gfxReady=0;CHECK(WddmResumeRetained(&d)==STATUS_DEVICE_NOT_READY && w.Stopping);gfxReady=1;
 pagingReady=0;CHECK(WddmResumeRetained(&d)==STATUS_DEVICE_NOT_READY && w.Stopping);pagingReady=1;
 d.SourceVisible=1;CHECK(WddmResumeRetained(&d)==STATUS_DEVICE_NOT_READY);d.SourceVisible=0;
 d.DcnBlanked=0;CHECK(WddmResumeRetained(&d)==STATUS_DEVICE_NOT_READY);d.DcnBlanked=1;
 enableStatus=-9;CHECK(WddmResumeRetained(&d)==-9 && w.Stopping && w.RetainedPowerPause);enableStatus=0;
 CHECK(WddmResumeRetained(&d)==0 && !w.Stopping && !w.RetainedPowerPause && d.DcnVsyncArmed);
 CHECK(!d.SourceVisible && d.DcnBlanked && d.Wddm==&w);retained(&w,&before);
 CHECK(WddmResumeRetained(&d)==STATUS_INVALID_DEVICE_STATE);
 // Every independent outstanding-work shape must prevent suspend, unchanged.
 for(i=0;i<sizeof(scalar)/sizeof(scalar[0]);i++){
  setup(&d,&w);*(int*)((char*)&w+scalar[i])=1;before=w;
  CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY);CHECK(!memcmp(&w,&before,sizeof(w)) && !cancels && !mmio);
 }
 for(i=0;i<sizeof(arrays)/sizeof(arrays[0]);i++){unsigned n;for(n=0;n<3;n++){
  setup(&d,&w);((int*)((char*)&w+arrays[i]))[n]=1;before=w;
  CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY);CHECK(!memcmp(&w,&before,sizeof(w)) && !removes);
 }}
 setup(&d,&w);w.PagingHead=&d;before=w;CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY);CHECK(!memcmp(&w,&before,sizeof(w)));
 setup(&d,&w);w.PagingTail=&d;CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY);
 setup(&d,&w);enableStatus=-9;before=w;CHECK(WddmSuspendRetained(&d)==-9);CHECK(!memcmp(&w,&before,sizeof(w)) && !cancels);
 setup(&d,&w);d.VidPnFlipEnabled=0;d.DcnVsyncArmed=0;w.VSyncArmed=1;before=w;
 CHECK(WddmSuspendRetained(&d)==0 && !w.VSyncArmed);CHECK(WddmResumeRetained(&d)==0 && w.VSyncArmed && arms==1 && !mmio);retained(&w,&before);
 setup(&d,&w);w.VSyncEnabled=0;CHECK(WddmSuspendRetained(&d)==0);CHECK(WddmResumeRetained(&d)==0 && !d.DcnVsyncArmed && !arms);
 setup(&d,&w);currentIrql=2;CHECK(WddmSuspendRetained(&d)==STATUS_INVALID_DEVICE_STATE && !flushes);CHECK(WddmResumeRetained(&d)==STATUS_INVALID_DEVICE_STATE);currentIrql=0;
 d.Wddm=NULL;CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_NOT_READY);CHECK(WddmResumeRetained(&d)==STATUS_DEVICE_NOT_READY);
 // BD-188 drain: a job that completes on its normal path inside the bound lets the way down go on.
 setup(&d,&w);w.HwPending=1;completeAfter=3;
 CHECK(WddmSuspendRetained(&d)==0 && w.RetainedPowerPause && w.Stopping && delays==3 && flushes==5 && logs==1);
 // The same with a coarse timer: the bound is elapsed time, so 15.6 ms steps still complete at the third delay.
 setup(&d,&w);w.HwPending=1;completeAfter=3;stepTicks=156250;
 CHECK(WddmSuspendRetained(&d)==0 && delays==3);
 // Work that does not complete: refused after the bound, the state unchanged, nothing cancelled.
 setup(&d,&w);w.HwPending=1;before=w;
 CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY);
 CHECK(!memcmp(&w,&before,sizeof(w)) && !cancels && !removes && !mmio && !delayLimitHit);
 CHECK(delays==BC250_WDDM_SUSPEND_DRAIN_MS && interruptTime>=(ULONGLONG)BC250_WDDM_SUSPEND_DRAIN_MS*10000u && logs==1);
 // With 15.6 ms steps the bound is reached after 32 delays (500 ms), not after 500 delays (7.8 s).
 setup(&d,&w);interruptTime=0;w.PagingHwPending=1;stepTicks=156250;
 CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY && delays==32 && !delayLimitHit);
 // A watchdog fault or a started stop can end only by a reset or the stop itself: no wait at all.
 setup(&d,&w);w.WatchdogFaulted[1]=1;before=w;
 CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY && delays==0 && !memcmp(&w,&before,sizeof(w)) && logs==1);
 setup(&d,&w);w.HwPending=1;w.WatchdogFaulted[0]=1;CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY && delays==0);
 setup(&d,&w);w.Stopping=1;before=w;CHECK(WddmSuspendRetained(&d)==STATUS_DEVICE_BUSY && delays==0 && !memcmp(&w,&before,sizeof(w)));
 // The busy mask names each shape.
 setup(&d,&w);CHECK(WddmPowerBusyLocked(&w)==0);
 w.HwPending=1;w.PagingHead=&d;w.RefusalPending[2]=1;
 CHECK(WddmPowerBusyLocked(&w)==(BC250_POWER_BUSY_GFX_HW|BC250_POWER_BUSY_PAGING_QUEUED|BC250_POWER_BUSY_REFUSAL));
 CHECK(Bc250PowerDrainWorthWaiting(BC250_POWER_BUSY_GFX_HW) && !Bc250PowerDrainWorthWaiting(0) &&
       !Bc250PowerDrainWorthWaiting(BC250_POWER_BUSY_GFX_HW|BC250_POWER_BUSY_WATCHDOG) &&
       !Bc250PowerDrainWorthWaiting(BC250_POWER_BUSY_STOPPING));
 // BD-188 D-2: the forced pause keeps every pending record and every fence, records the sequence that was on the
 // ring, and the resume accepts exactly the work that the pause recorded.
 setup(&d,&w);w.HwPending=1;w.CompletionPending[0]=1;lostJob[0].Seq=77;before=w;
 WddmPauseForPowerLoss(&d);
 CHECK(w.Stopping&&w.RetainedPowerPause&&w.HwPending&&w.CompletionPending[0]&&w.PowerLostSeq[0]==77&&w.PowerLostSeq[2]==0);
 CHECK(w.PowerLostWork==(BC250_POWER_BUSY_GFX_HW|BC250_POWER_BUSY_COMPLETION)&&cancels==5&&removes==6&&!d.DcnVsyncArmed);
 retained(&w,&before);
 gfxReady=pagingReady=0;     // the lost job keeps the normal answer false
 restoredLost=0;CHECK(WddmResumeRetained(&d)==STATUS_DEVICE_NOT_READY&&w.Stopping);restoredLost=1;
 w.HwPending=0;CHECK(WddmResumeRetained(&d)==STATUS_INVALID_DEVICE_STATE&&w.Stopping);w.HwPending=1;
 CHECK(WddmResumeRetained(&d)==0&&!w.Stopping&&!w.RetainedPowerPause&&w.PowerLostWork==0&&w.PowerLostSeq[0]==77);
 CHECK(w.HwPending&&w.CompletionPending[0]&&inserts==1&&!requeues);retained(&w,&before);
 // A compute job and a queued paging job: both recorded, and the paging drain is armed again at D0.
 setup(&d,&w);w.ComputeHwPending=1;lostJob[2].Seq=9;w.PagingHead=&d;WddmPauseForPowerLoss(&d);
 CHECK(w.PowerLostSeq[2]==9&&w.PowerLostSeq[0]==0&&w.PowerLostWork==(BC250_POWER_BUSY_COMPUTE_HW|BC250_POWER_BUSY_PAGING_QUEUED));
 CHECK(WddmResumeRetained(&d)==0&&requeues==1&&!inserts);
 // A pause that already ran: the forced form changes nothing.
 setup(&d,&w);CHECK(WddmSuspendRetained(&d)==0);before=w;cancels=0;WddmPauseForPowerLoss(&d);
 CHECK(!memcmp(&w,&before,sizeof(w))&&!cancels);
 printf("Retained WDDM power: %u checks, %u failures\n",checks,failures);return failures?1:0;
}

