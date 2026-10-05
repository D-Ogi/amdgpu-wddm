
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
typedef struct { int Lock; LONG SubmittedNode[2],PreemptionNode[2],CompletionPending[2],SubmittedFence[2],ActiveSubmissions[2],LastReportedFence[2],PreemptionPending[2],PreemptionFence[2]; LONG LastCompletedFence; BOOLEAN HwPending,PagingHwPending,Stopping,ReportActive,ReportAgain,RejectedPending[2],WatchdogFaulted[2],RefusalPending[2],LastReportedValid[2]; UINT RejectedFence[2]; KDPC ReportDpc; } BC250_WDDM;
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
static void WddmReportDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;
    LONG fence;
    KIRQL reportIrql;
    UINT node;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    if (WddmStopping(wddm)) return;
    // The same KDPC may be requeued from another processor while this invocation
    // reports to dxgkrnl. Keep publication and preemption ordered across invocations.
    KeAcquireSpinLock(&wddm->Lock, &reportIrql);
    if (wddm->ReportActive)
    {
        wddm->ReportAgain = TRUE;
        KeReleaseSpinLock(&wddm->Lock, reportIrql);
        return;
    }
    wddm->ReportActive = TRUE;
    KeReleaseSpinLock(&wddm->Lock, reportIrql);

    // ADR 0008 stage D: both nodes' pending completion/preemption are checked, not only node 0's - the array
    // slot a gate-closed device never sets stays 0, so this loop reports nothing new for node 1 until the gate
    // opens and something actually submits to it (design note section 5).
    for (node = 0; node < BC250_WDDM_NODE_COUNT_MAX; node++)
    {
        KIRQL irql;
        BOOLEAN complete, preempt;
        UINT preemptFence = 0, lastFence = 0;

        KeAcquireSpinLock(&wddm->Lock, &irql);
        complete = wddm->CompletionPending[node] != 0;
        fence = wddm->SubmittedFence[node];
        wddm->CompletionPending[node] = 0;
        KeReleaseSpinLock(&wddm->Lock, irql);
        if (complete)
        {
            RtlZeroMemory(&data, sizeof(data));
            data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            data.DmaCompleted.SubmissionFenceId = (UINT)fence;
            data.DmaCompleted.NodeOrdinal = node;
            data.DmaCompleted.EngineOrdinal = 0;
            WddmReport(device, &data);
            InterlockedExchange(&wddm->LastCompletedFence, fence);
            InterlockedExchange(&wddm->LastReportedFence[node], fence);
            wddm->LastReportedValid[node] = TRUE;
        }

        // DMA-buffer-boundary preemption cannot be acknowledged while that buffer is
        // executing or while its completion has yet to reach dxgkrnl. ActiveSubmissions
        // also covers GfxSubmitIb before it installs HwPending. Completion and submit
        // exit requeue this DPC, so no spinning or timer is needed while we defer.
        KeAcquireSpinLock(&wddm->Lock, &irql);
        // SubmitCommandVirtual's invalid-parameter contract: the OS retires a
        // rejected fence after prior work. Update our notion without reporting a
        // successful DMA completion for work that was never submitted.
        if (wddm->RejectedPending[node] && !wddm->RefusalPending[node] &&
            wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&
            !(node == BC250_WDDM_NODE_COPY ? wddm->PagingHwPending : wddm->HwPending))
        {
            UINT rejected=wddm->RejectedFence[node];
            if (!wddm->LastReportedValid[node] ||
                (LONG)(rejected-(UINT)wddm->LastReportedFence[node]) > 0)
            {
                wddm->LastReportedFence[node]=(LONG)rejected;
                wddm->LastReportedValid[node]=TRUE;
                wddm->LastCompletedFence=(LONG)rejected;
            }
            wddm->RejectedPending[node]=FALSE;
        }
        preempt = wddm->PreemptionPending[node] != 0 && !wddm->RefusalPending[node] &&
                  wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&
                  !(node == BC250_WDDM_NODE_COPY ? wddm->PagingHwPending : wddm->HwPending);
        if (preempt)
        {
            preemptFence = (UINT)wddm->PreemptionFence[node];
            lastFence = (UINT)wddm->LastReportedFence[node];
            wddm->PreemptionPending[node] = 0;
        }
        KeReleaseSpinLock(&wddm->Lock, irql);
        if (preempt)
        {
            RtlZeroMemory(&data, sizeof(data));
            data.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
            data.DmaPreempted.PreemptionFenceId = preemptFence;
            data.DmaPreempted.LastCompletedFenceId = lastFence;
            data.DmaPreempted.NodeOrdinal = node;
            data.DmaPreempted.EngineOrdinal = 0;
            GuardLog("wddm: preemption report fence %u node %u last completed %u at DMA boundary",
                     preemptFence, node, lastFence);
            WddmReport(device, &data);
        }
    }
    KeAcquireSpinLock(&wddm->Lock, &reportIrql);
    wddm->ReportActive = FALSE;
    if (wddm->ReportAgain)
    {
        wddm->ReportAgain = FALSE;
        if (!wddm->Stopping) KeInsertQueueDpc(&wddm->ReportDpc, NULL, NULL);
    }
    KeReleaseSpinLock(&wddm->Lock, reportIrql);
}


#define CHECK(c) do {if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void) {
 BC250_WDDM w={0}; BC250_DEVICE d={&w};
 w.PreemptionPending[0]=1;w.PreemptionFence[0]=7;w.HwPending=1;
 WddmReportDpcRoutine(0,&d,0,0); CHECK(nr==0 && w.PreemptionPending[0]);
 w.HwPending=0;w.CompletionPending[0]=1;w.SubmittedFence[0]=101;
 WddmReportDpcRoutine(0,&d,0,0); CHECK(nr==2 && reports[0].InterruptType==1 && reports[1].DmaPreempted.LastCompletedFenceId==101 && !w.PreemptionPending[0]);
 memset(&w,0,sizeof(w));nr=0;w.PreemptionPending[0]=1;w.ActiveSubmissions[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.PreemptionPending[0]);
 w.ActiveSubmissions[0]=0;WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1);
 memset(&w,0,sizeof(w));nr=0;w.PreemptionPending[0]=1;w.CompletionPending[0]=1;w.SubmittedFence[0]=101;inject=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && w.PreemptionPending[0]);
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==3 && reports[2].DmaPreempted.LastCompletedFenceId==102);
 memset(&w,0,sizeof(w));nr=0;w.LastCompletedFence=999;w.LastReportedFence[1]=22;w.PreemptionPending[1]=1;w.PagingHwPending=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0);
 w.PagingHwPending=0;WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && reports[0].DmaPreempted.LastCompletedFenceId==22);
 memset(&w,0,sizeof(w));nr=0;queued=0;w.ReportActive=1;w.PreemptionPending[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.ReportAgain && w.PreemptionPending[0]);
 w.ReportActive=0;WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && queued==1 && !w.ReportAgain);

 /* A rejected fence must not retire an earlier executing packet, on either node.
    A real late fence after a watchdog still makes that dependency complete. */
 for (unsigned node=0;node<2;node++) {
  memset(&w,0,sizeof(w));nr=0;
  w.RejectedPending[node]=1;w.RejectedFence[node]=102;
  w.PreemptionPending[node]=1;w.PreemptionFence[node]=8;
  if(node) w.PagingHwPending=1;else w.HwPending=1;
  WddmReportDpcRoutine(0,&d,0,0);
  CHECK(nr==0 && w.RejectedPending[node] && !w.LastReportedValid[node]);
  w.WatchdogFaulted[node]=1;
  WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.RejectedPending[node]);
  w.HwPending=w.PagingHwPending=0;
  w.CompletionPending[node]=1;w.SubmittedFence[node]=101;
  WddmReportDpcRoutine(0,&d,0,0);
  CHECK(nr==2 && reports[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED);
  CHECK(reports[0].DmaCompleted.SubmissionFenceId==101);
  CHECK(reports[1].InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED);
  CHECK(reports[1].DmaPreempted.LastCompletedFenceId==102);
  CHECK(w.LastReportedValid[node] && !w.RejectedPending[node]);
 }
 /* Active submit and a completion arriving during publication both defer retirement. */
 memset(&w,0,sizeof(w));nr=0;w.RejectedPending[0]=1;w.RejectedFence[0]=103;
 w.ActiveSubmissions[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.RejectedPending[0]);
 w.ActiveSubmissions[0]=0;w.CompletionPending[0]=1;w.SubmittedFence[0]=101;inject=1;
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==1 && w.RejectedPending[0] && w.LastReportedFence[0]==101);
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==2 && reports[1].DmaCompleted.SubmissionFenceId==102);
 CHECK(w.LastReportedFence[0]==103 && !w.RejectedPending[0]);
 /* No fabricated interrupt for a rejected first packet, including high-bit IDs. */
 memset(&w,0,sizeof(w));nr=0;w.RejectedPending[0]=1;w.RejectedFence[0]=0x80000001U;
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==0 && (UINT)w.LastReportedFence[0]==0x80000001U && w.LastReportedValid[0]);
 /* A later completed packet subsumes rejection; an old rejected fence cannot regress it. */
 memset(&w,0,sizeof(w));nr=0;w.RejectedPending[0]=1;w.RejectedFence[0]=101;
 w.CompletionPending[0]=1;w.SubmittedFence[0]=102;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==1 && w.LastReportedFence[0]==102);
 /* Normal 32-bit fence wrap, with an established predecessor. */
 memset(&w,0,sizeof(w));nr=0;w.LastReportedValid[0]=1;w.LastReportedFence[0]=(LONG)0xfffffffeU;
 w.RejectedPending[0]=1;w.RejectedFence[0]=1;
 WddmReportDpcRoutine(0,&d,0,0);CHECK(nr==0 && w.LastReportedFence[0]==1);

 memset(&w,0,sizeof(w));nr=0;
 w.RefusalPending[0]=1;w.WatchdogFaulted[0]=1;
 w.PreemptionPending[0]=1;w.RejectedPending[0]=1;w.RejectedFence[0]=304;
 w.CompletionPending[0]=1;w.SubmittedFence[0]=302;
 WddmReportDpcRoutine(0,&d,0,0);
 CHECK(nr==1 && reports[0].DmaCompleted.SubmissionFenceId==302);
 CHECK(w.PreemptionPending[0] && w.RejectedPending[0] && w.LastReportedFence[0]==302);
 puts("PASS: six preemption controls; rejection ordering on both nodes, watchdog/late fence, active submit, publication race, high-bit initial fence, no regression, wrap");return 0;
}
