
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifndef _In_
#define _In_
#endif
#ifndef _In_opt_
#define _In_opt_
#endif
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define FALSE 0
#define TRUE 1
#define BC250_WDDM_NODE_COUNT_MAX 2
#define BC250_WDDM_NODE_COPY 1
#define DXGK_INTERRUPT_DMA_COMPLETED 1
#define DXGK_INTERRUPT_DMA_PREEMPTED 2
typedef int BOOLEAN; typedef int LONG; typedef unsigned UINT; typedef int KIRQL; typedef int KDPC; typedef void* PVOID;
typedef struct { UINT SubmissionFenceId,NodeOrdinal,EngineOrdinal; } COMPLETE;
typedef struct { UINT PreemptionFenceId,LastCompletedFenceId,NodeOrdinal,EngineOrdinal; } PREEMPT;
typedef struct { int InterruptType; COMPLETE DmaCompleted; PREEMPT DmaPreempted; } DXGKARGCB_NOTIFY_INTERRUPT_DATA;
typedef struct _BC250_PAGING_JOB { struct _BC250_PAGING_JOB* Next; } BC250_PAGING_JOB;
typedef struct { void* PagingHead; void* PagingTail; int Lock; LONG SubmittedNode[2],PreemptionNode[2],CompletionPending[2],SubmittedFence[2],ActiveSubmissions[2],LastReportedFence[2],PreemptionPending[2],PreemptionFence[2]; LONG LastCompletedFence; BOOLEAN HwPending,PagingHwPending,Stopping,ReportActive,ReportAgain,RejectedPending[2],WatchdogFaulted[2],RefusalPending[2],LastReportedValid[2]; UINT RejectedFence[2]; KDPC ReportDpc; } BC250_WDDM;
typedef struct { BC250_WDDM* Wddm; } BC250_DEVICE;
static DXGKARGCB_NOTIFY_INTERRUPT_DATA reports[32]; static int nr,queued,inject;
static int WddmStopping(BC250_WDDM*w) {return w->Stopping;}
static void KeAcquireSpinLock(int*l,KIRQL*i) { (void)l;*i=0; }
static void KeReleaseSpinLock(int*l,KIRQL i) {(void)l;(void)i;}
static LONG InterlockedCompareExchange(LONG*p,LONG v,LONG c) {LONG old=*p;if(old==c)*p=v;return old;}
static LONG InterlockedExchange(LONG*p,LONG n) {LONG v=*p;*p=n;return v;}
static void RtlZeroMemory(void*p,size_t n) {memset(p,0,n);}
static int KeInsertQueueDpc(KDPC*d,void*a,void*b) {(void)d;(void)a;(void)b;queued++;return 1;}
#define GuardLog(...) ((void)0)
static void WddmReport(BC250_DEVICE*d,DXGKARGCB_NOTIFY_INTERRUPT_DATA*r) {
 reports[nr++]=*r;
 if(inject && r->InterruptType==DXGK_INTERRUPT_DMA_COMPLETED) {
  inject=0; d->Wddm->SubmittedFence[0]=102; d->Wddm->CompletionPending[0]=1;
 }
}
