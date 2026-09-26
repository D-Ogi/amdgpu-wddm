// Actual-source retained-owner tests. Kernel operations are ordered mock events.
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#ifndef _Inout_
#define _Inout_
#endif
typedef int BOOLEAN, NTSTATUS, KIRQL;
typedef unsigned UINT;
typedef long long LONGLONG;
typedef struct {long long QuadPart;} LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE -1
#define STATUS_DEVICE_NOT_READY -2
#define STATUS_DEVICE_BUSY -3
#define NT_SUCCESS(x) ((x)>=0)
#define BC250_WDDM_NODE_COUNT_MAX 2
#define BC250_WDDM_VSYNC_MS 16
typedef struct {
 int Lock,Stopping,RetainedPowerPause,HwPending,DeferredValid,PagingHwPending;
 void *PagingHead,*PagingTail;
 int PagingDeferredValid,ReportActive,ReportAgain;
 int ActiveSubmissions[2],CompletionPending[2],PreemptionPending[2];
 int RefusalPending[2],RejectedPending[2],WatchdogFaulted[2];
 int VSyncArmed,VSyncEnabled,VSyncTimer,SubmitTimer,PagingSubmitTimer;
 int SubmitDpc,PagingSubmitDpc,VSyncDpc,ReportDpc;
 LARGE_INTEGER VSyncLast,VSyncFrequency;
 // Independent owner identities and fence history must survive byte for byte.
 void *Objects,*Aperture,*CaptureOwner;
 unsigned SubmittedFence[2],LastReportedFence[2],LastCompletedFence;
} BC250_WDDM;
typedef struct {
 BC250_WDDM *Wddm;
 int DcnVsyncArmed,SourceVisible,DcnBlanked,VidPnFlipEnabled;
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
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER*f){LARGE_INTEGER n={1234};f->QuadPart=1000000;return n;}
/* ACTUAL_SOURCE */
static void setup(BC250_DEVICE*d,BC250_WDDM*w){
 memset(w,0,sizeof(*w));memset(d,0,sizeof(*d));d->Wddm=w;observed=w;
 d->DcnBlanked=1;d->VidPnFlipEnabled=1;d->DcnVsyncArmed=1;
 w->VSyncEnabled=1;w->Objects=d;w->Aperture=&w->Lock;w->CaptureOwner=&w->Objects;
 w->SubmittedFence[0]=123;w->SubmittedFence[1]=456;
 w->LastReportedFence[0]=123;w->LastReportedFence[1]=456;w->LastCompletedFence=456;
 flushes=cancels=removes=arms=mmio=0;currentIrql=enableStatus=0;gfxReady=pagingReady=1;
}
static void retained(BC250_WDDM*w,const BC250_WDDM*before){
 size_t off=offsetof(BC250_WDDM,Objects);
 CHECK(memcmp((char*)w+off,(const char*)before+off,sizeof(*w)-off)==0);
}
int main(void){
 BC250_DEVICE d;BC250_WDDM w,before;unsigned i;
 const size_t scalar[]={offsetof(BC250_WDDM,HwPending),offsetof(BC250_WDDM,DeferredValid),offsetof(BC250_WDDM,PagingHwPending),offsetof(BC250_WDDM,PagingDeferredValid),offsetof(BC250_WDDM,ReportActive),offsetof(BC250_WDDM,ReportAgain)};
 const size_t arrays[]={offsetof(BC250_WDDM,ActiveSubmissions),offsetof(BC250_WDDM,CompletionPending),offsetof(BC250_WDDM,PreemptionPending),offsetof(BC250_WDDM,RefusalPending),offsetof(BC250_WDDM,RejectedPending),offsetof(BC250_WDDM,WatchdogFaulted)};
 setup(&d,&w);before=w;
 CHECK(WddmSuspendRetained(&d)==0 && w.Stopping && w.RetainedPowerPause);
 CHECK(d.Wddm==&w && !d.DcnVsyncArmed && flushes==2 && cancels==3 && removes==4);
 retained(&w,&before);
 CHECK(WddmSuspendRetained(&d)==0 && cancels==3 && removes==4);
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
 for(i=0;i<sizeof(arrays)/sizeof(arrays[0]);i++){unsigned n;for(n=0;n<2;n++){
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
 printf("Retained WDDM power: %u checks, %u failures\n",checks,failures);return failures?1:0;
}

