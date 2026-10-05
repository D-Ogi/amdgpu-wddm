#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define TRUE 1
#define FALSE 0
#define BC250_WDDM_NODE_COPY 1
#define BC250_WDDM_NODE_COUNT_MAX 2
#define BC250_WDDM_LOG_CALLS 8
#define BC250_WDDM_SUBMIT_TIMEOUT_MS 500
#define UNREFERENCED_PARAMETER(x) (void)(x)
static void GuardLog(const char* format, ...){(void)format;}
typedef unsigned UINT,ULONG,KIRQL,KDPC;
typedef int BOOLEAN,LONG;
typedef void* PVOID;
typedef struct {
 int Lock; BOOLEAN HwPending,PagingHwPending,DeferredValid,PagingDeferredValid,WatchdogFaulted[2];
 UINT HwFence,HwNode,DeferredFence,PagingHwFence,PagingDeferredFence;
 ULONG HwSeq,PagingHwSeq,SubmitTimer,PagingSubmitTimer;
 LONG HwCompleted,PagingHwCompleted,HwTimeouts,PagingHwTimeouts;
} BC250_WDDM;
typedef struct {void* Wddm;} BC250_DEVICE;
static unsigned reports,completions,lastFence,lastNode,closed[2];
static int arrived[2];
static void KeAcquireSpinLock(int* l,KIRQL* i){(void)l;*i=0;}
static void KeReleaseSpinLock(int* l,KIRQL i){(void)l;(void)i;}
static int KeCancelTimer(ULONG* t){(void)t;return 1;}
static LONG InterlockedIncrement(LONG* v){return ++*v;}
static void WddmRecordCompletionLocked(BC250_WDDM* w,UINT f,UINT n){(void)w;completions++;lastFence=f;lastNode=n;}
static void WddmQueueReport(BC250_WDDM* w){(void)w;reports++;}
static void GfxSubmitFail(BC250_DEVICE* d){(void)d;closed[0]++;}
static void GfxPagingSubmitFail(BC250_DEVICE* d){(void)d;closed[1]++;}
static int GfxFenceArrived(BC250_DEVICE* d,ULONG seq){(void)d;(void)seq;return arrived[0];}
static int GfxPagingFenceArrived(BC250_DEVICE* d,ULONG seq){(void)d;(void)seq;return arrived[1];}
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line%d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void WddmCompleteSoftware(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN paging = NodeOrdinal == BC250_WDDM_NODE_COPY;
    BOOLEAN deferred;
    KIRQL irql;

    if (wddm == NULL || NodeOrdinal >= BC250_WDDM_NODE_COUNT_MAX) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    deferred = paging ? wddm->PagingHwPending : wddm->HwPending;
    if (deferred && paging) { wddm->PagingDeferredValid = TRUE; wddm->PagingDeferredFence = FenceId; }
    else if (deferred) { wddm->DeferredValid = TRUE; wddm->DeferredFence = FenceId; }
    else WddmRecordCompletionLocked(wddm, FenceId, NodeOrdinal);
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!deferred) WddmQueueReport(wddm);
}
void WddmGpuFence(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN done = FALSE;
    UINT fence = 0, node = 0;
    KIRQL irql;

    if (wddm == NULL) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->HwPending && GfxFenceArrived(Device, wddm->HwSeq))
    {
        done = TRUE;
        fence = wddm->DeferredValid ? wddm->DeferredFence : wddm->HwFence;
        node = wddm->HwNode;
        wddm->HwPending = FALSE;
        wddm->DeferredValid = FALSE;
        KeCancelTimer(&wddm->SubmitTimer);
        WddmRecordCompletionLocked(wddm, fence, node);
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!done) return;
    if (InterlockedIncrement(&wddm->HwCompleted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: hardware fence arrived, reporting fence %u", fence);
    WddmQueueReport(wddm);
}
static void WddmSubmitDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    BOOLEAN timedOut = FALSE;
    UINT fence = 0, node = 0;
    ULONG seq = 0;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    WddmGpuFence(device);               // late is still arrived
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->HwPending)
    {
        timedOut = TRUE;
        fence = wddm->DeferredValid ? wddm->DeferredFence : wddm->HwFence;
        node = wddm->HwNode;
        seq = wddm->HwSeq;
        wddm->HwPending = FALSE;
        wddm->DeferredValid = FALSE;
        WddmRecordCompletionLocked(wddm, fence, node);
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // Nobody can reset this GPU. The packet is declared finished so that the scheduler never starts a TDR it
    // cannot win, and the ring is not written again in this device start.
    InterlockedIncrement(&wddm->HwTimeouts);
    GfxSubmitFail(device);
    GuardLog("wddm: HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u completed in software, ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    WddmQueueReport(wddm);
}
void WddmGpuFencePaging(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN done = FALSE;
    UINT fence = 0;
    KIRQL irql;

    if (wddm == NULL) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->PagingHwPending && GfxPagingFenceArrived(Device, wddm->PagingHwSeq))
    {
        done = TRUE;
        // The held-back completion, if there is one, is the later fence and retires this one with it.
        fence = wddm->PagingDeferredValid ? wddm->PagingDeferredFence : wddm->PagingHwFence;
        wddm->PagingDeferredValid = FALSE;
        wddm->PagingHwPending = FALSE;
        KeCancelTimer(&wddm->PagingSubmitTimer);
        WddmRecordCompletionLocked(wddm, fence, BC250_WDDM_NODE_COPY);
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!done) return;
    if (InterlockedIncrement(&wddm->PagingHwCompleted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: paging hardware fence arrived, reporting fence %u", fence);
    WddmQueueReport(wddm);
}
static void WddmPagingSubmitDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    BOOLEAN timedOut = FALSE;
    UINT fence = 0;
    ULONG seq = 0;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    WddmGpuFencePaging(device);         // late is still arrived
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->PagingHwPending)
    {
        timedOut = TRUE;
        fence = wddm->PagingDeferredValid ? wddm->PagingDeferredFence : wddm->PagingHwFence;
        wddm->PagingDeferredValid = FALSE;
        seq = wddm->PagingHwSeq;
        wddm->PagingHwPending = FALSE;
        WddmRecordCompletionLocked(wddm, fence, BC250_WDDM_NODE_COPY);
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // Same answer as node 0's own timeout: nobody can reset this GPU, so the packet is declared finished and the
    // SDMA0 ring path is closed for node 1 only - node 0's ring path, and node 0's own SubmitFailed, are untouched.
    InterlockedIncrement(&wddm->PagingHwTimeouts);
    GfxPagingSubmitFail(device);
    GuardLog("wddm: PAGING HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u completed in software, node 1 ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    WddmQueueReport(wddm);
}
int main(void) {
 unsigned node;
 for(node=0;node<2;node++) {
  BC250_WDDM w={0};BC250_DEVICE d={&w};
  void (*watch)(KDPC*,PVOID,PVOID,PVOID)=node?WddmPagingSubmitDpcRoutine:WddmSubmitDpcRoutine;
  reports=completions=closed[0]=closed[1]=0;arrived[0]=arrived[1]=0;
  w.HwPending=!node;w.PagingHwPending=!!node;w.HwFence=w.PagingHwFence=7;
  w.HwNode=0;w.HwSeq=w.PagingHwSeq=1;
  w.DeferredValid=w.PagingDeferredValid=1;w.DeferredFence=w.PagingDeferredFence=8;
  watch(0,&d,0,0);
  CHECK(completions==0 && reports==0 && closed[node]==1);
  CHECK(node?w.PagingHwPending:w.HwPending);CHECK(w.WatchdogFaulted[node]);
  WddmCompleteSoftware(&d,9,node);CHECK(completions==0 && reports==0);
  CHECK(node?!w.PagingDeferredValid:!w.DeferredValid);
  watch(0,&d,0,0);CHECK(closed[node]==1); // idempotent timeout
  // Actual late arrival may retire ONLY the submitted hardware fence.
  arrived[node]=1;watch(0,&d,0,0);
  CHECK(completions==1 && lastFence==7 && lastNode==node && reports==1);
  CHECK(node?!w.PagingHwPending:!w.HwPending);
  WddmCompleteSoftware(&d,10,node);CHECK(completions==1);
  // Positive control: before deadline an arrived fence completes normally.
  memset(&w,0,sizeof(w));reports=completions=closed[0]=closed[1]=0;
  w.HwPending=!node;w.PagingHwPending=!!node;w.HwFence=w.PagingHwFence=11;
  watch(0,&d,0,0);CHECK(completions==1 && lastFence==11 && !closed[node] && !w.WatchdogFaulted[node]);
 }
 puts("PASS: both watchdogs keep pending work, emit no fake completion, block later software fences, accept only actual late arrival, and preserve normal completion");
 return 0;
}
