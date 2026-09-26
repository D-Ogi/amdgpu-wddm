
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <stdarg.h>
#define UNREFERENCED_PARAMETER(x) (void)(x)
#define TRUE 1
#define FALSE 0
#define BC250_WDDM_NODE_COPY 1
#define BC250_WDDM_NODE_COUNT_MAX 2
#define BC250_WDDM_SUBMIT_TIMEOUT_MS 500
#define BC250_WDDM_TAG 1
#include "paging_private.h"
#define C_ASSERT(e) typedef char assert_job_fits[(e)?1:-1]
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef unsigned char UCHAR;
#define POOL_FLAG_NON_PAGED 1
#define NT_SUCCESS(s) ((s)>=0)
#define FIELD_OFFSET(t,f) offsetof(t,f)
#define RtlCopyMemory memcpy
typedef unsigned long ULONG,UINT;
typedef unsigned long long ULONGLONG;
typedef size_t SIZE_T;
typedef unsigned char BOOLEAN;
typedef int KIRQL;
typedef long NTSTATUS,LONG;
typedef struct { long long QuadPart; } LARGE_INTEGER;
typedef void KDPC;
typedef void* PVOID;
typedef struct _BC250_PAGING_JOB {
    struct _BC250_PAGING_JOB* Next;
    ULONGLONG Start;
    ULONGLONG Epoch; // borrowed queue ownership cannot migrate to a new recovery epoch
    ULONG ByteCount, PrivateBytes;
    UINT Fence;
    BOOLEAN VirtualAddress, Borrowed;
    const UCHAR* Data;
} BC250_PAGING_JOB;
C_ASSERT(sizeof(BC250_PAGING_JOB)<=PAGING_PRIVATE_JOB_BYTES);

// OS ownership, actual GPU execution and notification are distinct histories.
// Submitted includes successful DDIs whose undispatched packet needs recovery;
// Hardware never includes software completion or a rejected virtual submission.
// Lock protects this owner-local epoch and every ledger field. Future recovery
// must close/join publishers before advancing the epoch; power suspend retains it.
typedef struct _BC250_WDDM_FENCE_LEDGER {
    ULONGLONG Epoch;
    UINT Submitted, Hardware;
    BOOLEAN SubmittedValid, HardwareValid;
} BC250_WDDM_FENCE_LEDGER;


typedef struct {
 BC250_PAGING_JOB *PagingHead,*PagingTail;
 ULONGLONG PagingDeadline,PagingHwEpoch;
 BC250_WDDM_FENCE_LEDGER FenceLedger[2];
 int Stopping,WatchdogFaulted[2],PagingHwPending,PagingDeferredValid,PreemptionPending[2];
 ULONG PagingHwSeq;
 UINT PagingHwFence;
 int Lock,PagingSubmitTimer,PagingSubmitDpc;
 LONG SubmittedNode[2],PreemptionNode[2],CompletionPending[2],SubmittedFence[2],ActiveSubmissions[2],LastReportedFence[2],PreemptionFence[2];
 UINT NodeCount,HwNode,HwFence,DeferredFence; ULONG HwSeq; ULONGLONG HwEpoch;
 BOOLEAN DeferredValid; int SubmitTimer,SubmitDpc; LONG HwCompleted,HwSubmitted,HwRefused;
 LONG LastCompletedFence;
 BOOLEAN HwPending,ReportActive,ReportAgain,RejectedPending[2],RefusalPending[2],LastReportedValid[2];
 UINT RejectedFence[2]; KDPC* ReportDpc;
 LONG PagingQueueBorrowed,PagingHwSubmitted,PagingHwCompleted,PagingHwRefused,PagingHwTimeouts;
} BC250_WDDM;
typedef struct { void* Wddm; } BC250_DEVICE;
static int checks,failures,allocated,lockHeld,submitted,completionCount,reports,failureCount,allocationCalls,immediateFence;
static ULONG nextSeq,activeSeq,arrived;
static ULONGLONG now;
static unsigned char submittedBytes[256];
static UINT fences[256];
static void check(int yes,const char*n){checks++;if(!yes){failures++;printf("FAIL %s\n",n);}}
static void KeAcquireSpinLock(int*l,KIRQL*i){(void)l;*i=0;check(!lockHeld,"nonrecursive lock");lockHeld=1;}
static void KeReleaseSpinLock(int*l,KIRQL i){(void)l;(void)i;lockHeld=0;}
static ULONGLONG KeQueryInterruptTime(void){return now;}
static int KeCancelTimer(int*t){(void)t;return 0;}
static int KeSetTimer(int*t,LARGE_INTEGER d,int*p){(void)t;(void)d;(void)p;return 0;}
static LONG InterlockedIncrement(LONG*p){return ++*p;}
static void GuardLog(const char*f,...){(void)f;}
static BC250_PAGING_JOB* completionSlots[256];
static int RecordWord(void* unused,const PAGING_PRIVATE_SPAN* span)
{
 (void)unused;submittedBytes[submitted++]=(unsigned char)(span->Kind==PAGING_PRIVATE_NATIVE ? 77u : span->Words[0]);return 1;
}
static BOOLEAN GfxPagingFenceArrived(BC250_DEVICE*d,ULONG s)
{(void)d;if(arrived!=s)return FALSE;activeSeq=0;return TRUE;}
static NTSTATUS GfxSubmitPaging(BC250_DEVICE*d,const void*p,ULONG b,ULONGLONG start,ULONG count,BOOLEAN va,ULONG*seq)
{
 (void)d;(void)b;(void)start;(void)count;(void)va;
 check(!activeSeq,"never dispatch onto occupied ring");
 if(activeSeq)return -1;
 check(PagingPrivateVisitMixed(p,b,start,count,va,RecordWord,NULL),"real parser dispatch");
 activeSeq=++nextSeq;*seq=activeSeq;if(immediateFence)arrived=activeSeq;return 0;
}
static void WddmRecordCompletionLocked(BC250_WDDM*w,UINT fence,UINT node)
{
 unsigned i;unsigned char* slot=(unsigned char*)completionSlots[fence];
 w->CompletionPending[node]=1;w->SubmittedFence[node]=(LONG)fence;check(lockHeld,"completion published under lock");
 if(slot) {
  for(i=0;i<PAGING_PRIVATE_JOB_BYTES;i++)check(slot[i]==0,"slot released before completion");
  /* OS may reuse/rewrite its storage as soon as completion is published. */
  memset(slot,0xa5,PAGING_PRIVATE_JOB_BYTES);completionSlots[fence]=NULL;
 }
 fences[completionCount++]=fence;
}
static void WddmQueueReport(BC250_WDDM*w){(void)w;check(!lockHeld,"report outside lock");reports++;}
static void WddmFailSubmission(BC250_DEVICE*d,UINT fence,UINT node)
{(void)d;(void)fence;(void)node;failureCount++;}
static void GfxPagingSubmitFail(BC250_DEVICE*d){(void)d;failureCount++;}
static void WddmRecordFenceLedgerLocked(BC250_WDDM* Wddm, UINT Node,
    ULONGLONG Epoch, UINT Fence, BOOLEAN Hardware)
{
    BC250_WDDM_FENCE_LEDGER* ledger=&Wddm->FenceLedger[Node];
    UINT* value=Hardware ? &ledger->Hardware : &ledger->Submitted;
    BOOLEAN* valid=Hardware ? &ledger->HardwareValid : &ledger->SubmittedValid;
    if (Epoch!=ledger->Epoch) return;
    if (!*valid || (LONG)(Fence-*value)>0) *value=Fence;
    *valid=TRUE;
}
void WddmGpuFencePaging(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    if (!wddm) return;
    for (;;) {
        BC250_PAGING_JOB* retired=NULL;
        BOOLEAN completed=FALSE, failed=FALSE;
        UINT fence=0;
        ULONG seq=0;
        NTSTATUS status;
        KIRQL irql;
        LARGE_INTEGER due;
        KeAcquireSpinLock(&wddm->Lock,&irql);
        if (wddm->Stopping || (wddm->PagingHead &&
            wddm->PagingHead->Epoch!=wddm->FenceLedger[BC250_WDDM_NODE_COPY].Epoch) ||
            (wddm->PagingHwPending &&
            wddm->PagingHwEpoch!=wddm->FenceLedger[BC250_WDDM_NODE_COPY].Epoch)) {
            KeReleaseSpinLock(&wddm->Lock,irql);
            return;
        }
        if (wddm->PagingHwPending && GfxPagingFenceArrived(Device,wddm->PagingHwSeq)) {
            retired=wddm->PagingHead;
            WddmRecordFenceLedgerLocked(wddm,BC250_WDDM_NODE_COPY,wddm->PagingHwEpoch,
                wddm->PagingHwFence,TRUE);
            wddm->PagingHwPending=FALSE;
            KeCancelTimer(&wddm->PagingSubmitTimer);
            completed=TRUE;
        } else if (!wddm->PagingHwPending && wddm->PagingHead &&
                   !wddm->PreemptionPending[BC250_WDDM_NODE_COPY] &&
                   !wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY]) {
            BC250_PAGING_JOB* job=wddm->PagingHead;
            if (!job->ByteCount) {
                retired=job;
                completed=TRUE;
            } else {
                // Lock covers GPU publication and its CPU pending state together.
                // An immediate IH DPC cannot observe a half-published submission.
                status=GfxSubmitPaging(Device,job->Data,job->PrivateBytes,job->Start,
                    job->ByteCount,job->VirtualAddress,&seq);
                if (NT_SUCCESS(status)) {
                    wddm->PagingHwPending=TRUE;
                    wddm->PagingHwSeq=seq;
                    wddm->PagingHwEpoch=job->Epoch;
                    wddm->PagingHwFence=job->Fence;
                    wddm->PagingDeadline=KeQueryInterruptTime()+10000ull*BC250_WDDM_SUBMIT_TIMEOUT_MS;
                    due.QuadPart=-10000ll*BC250_WDDM_SUBMIT_TIMEOUT_MS;
                    KeSetTimer(&wddm->PagingSubmitTimer,due,&wddm->PagingSubmitDpc);
                    InterlockedIncrement(&wddm->PagingHwSubmitted);
                } else {
                    failed=TRUE;
                    fence=job->Fence;
                    // Stop another caller from retrying this head before fail publication.
                    wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY]=TRUE;
                    InterlockedIncrement(&wddm->PagingHwRefused);
                    GuardLog("wddm: queued paging dispatch refused 0x%08X fence %u",status,fence);
                }
            }
        }
        if (completed) {
            fence=retired->Fence;
            wddm->PagingHead=retired->Next;
            if (!wddm->PagingHead) wddm->PagingTail=NULL;
            if (retired->ByteCount) InterlockedIncrement(&wddm->PagingHwCompleted);
            // Publishing completion can let another CPU reuse the OS buffer.
            // Clear borrowed ownership first and never touch that slot again.
            RtlZeroMemory(retired,sizeof(*retired));
            retired=NULL;
            WddmRecordCompletionLocked(wddm,fence,BC250_WDDM_NODE_COPY);
        }
        KeReleaseSpinLock(&wddm->Lock,irql);
        if (failed) WddmFailSubmission(Device,fence,BC250_WDDM_NODE_COPY);
        if (!completed) return; // no polling loop while the GPU is executing
        WddmQueueReport(wddm);
    }
}
static void TestStopDrain(BC250_WDDM* wddm) {
    while (wddm->PagingHead) {
        BC250_PAGING_JOB* job=wddm->PagingHead;
        wddm->PagingHead=job->Next;
        RtlZeroMemory(job,sizeof(*job));
    }
    wddm->PagingTail=NULL;
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
    if (wddm->PagingHwPending && !wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] &&
        KeQueryInterruptTime()>=wddm->PagingDeadline)
    {
        timedOut = TRUE;
        fence = wddm->PagingHwFence;
        wddm->PagingDeferredValid = FALSE;
        seq = wddm->PagingHwSeq;
        wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] = TRUE;
        // Preserve PagingHwPending until a real fence or the OS recovery path.
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // As on node0, do not retire unexecuted paging commands. OS TDR sees the
    // still-pending fence; closing node1 does not invent a successful memory transfer.
    InterlockedIncrement(&wddm->PagingHwTimeouts);
    GfxPagingSubmitFail(device);
    GuardLog("wddm: PAGING HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u remains pending for OS TDR, node 1 ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    // No completion report for a fence that has not arrived.
}
static BOOLEAN WddmSubmitPagingHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm,
    const void* PrivateData, ULONG PrivateBytes, ULONGLONG Start,
    ULONG ByteCount, BOOLEAN VirtualAddress, UINT FenceId)
{
    BC250_PAGING_JOB* job;
    KIRQL irql;
    if (!ByteCount || PrivateBytes>PAGING_PRIVATE_BUFFER_BYTES) return FALSE;
    job=(BC250_PAGING_JOB*)PagingPrivateQueueSlot((void*)PrivateData,PrivateBytes,
        Start,ByteCount,VirtualAddress);
    if (!job) return FALSE; // malformed or stale legacy records are not built here
    KeAcquireSpinLock(&Wddm->Lock,&irql);
    if (Wddm->Stopping || Wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] || job->Borrowed) {
        KeReleaseSpinLock(&Wddm->Lock,irql);
        return FALSE;
    }
    // All live slot ownership changes are serialized with queue retirement.
    // Replaying a still-owned start cannot overwrite its existing fence/link.
    job->Next=NULL;job->Borrowed=TRUE;
    job->Epoch=Wddm->FenceLedger[BC250_WDDM_NODE_COPY].Epoch;
    job->Start=Start;job->ByteCount=ByteCount;job->PrivateBytes=PrivateBytes;
    job->Fence=FenceId;job->VirtualAddress=VirtualAddress;
    job->Data=(const UCHAR*)PrivateData;
    if (Wddm->PagingTail) Wddm->PagingTail->Next=job;
    else Wddm->PagingHead=job;
    Wddm->PagingTail=job;
    InterlockedIncrement(&Wddm->PagingQueueBorrowed);
    KeReleaseSpinLock(&Wddm->Lock,irql);
    WddmGpuFencePaging(Device);
    return TRUE;
}

// Caller owns Lock and has observed no hardware packet or active submit. Windows
// resubmits preempted paging packets with their original fence IDs (Microsoft,
// display/gpu-preemption.md). Release only our borrowed queue links, not command
// payloads or OS storage, before reporting preemption. Never complete these jobs
// or restart them autonomously: the scheduler owns replay and its ordering.
static void WddmReleasePreemptedPagingLocked(BC250_WDDM* Wddm)
{
    while (Wddm->PagingHead) {
        BC250_PAGING_JOB* job=Wddm->PagingHead;
        Wddm->PagingHead=job->Next;
        RtlZeroMemory(job,sizeof(*job));
    }
    Wddm->PagingTail=NULL;
}


// Extends the existing queue harness with the actual report/preempt functions.
// These mocks represent scheduler callbacks; no GPU recovery is claimed.
#define DXGK_INTERRUPT_DMA_COMPLETED 1
#define DXGK_INTERRUPT_DMA_PREEMPTED 2
typedef struct { UINT SubmissionFenceId,NodeOrdinal,EngineOrdinal; } COMPLETE;
typedef struct { UINT PreemptionFenceId,LastCompletedFenceId,NodeOrdinal,EngineOrdinal; } PREEMPT;
typedef struct { int InterruptType; COMPLETE DmaCompleted; PREEMPT DmaPreempted; } DXGKARGCB_NOTIFY_INTERRUPT_DATA;
static DXGKARGCB_NOTIFY_INTERRUPT_DATA events[16];
static unsigned eventCount;
static const void* replayData;
static ULONG replayBytes;
static BC250_PAGING_JOB *preemptedOne,*preemptedTwo;
static BOOLEAN WddmStopping(BC250_WDDM* w) {return (BOOLEAN)w->Stopping;}
static LONG InterlockedExchange(LONG* p,LONG value) {LONG old=*p;*p=value;return old;}
static int KeInsertQueueDpc(KDPC** d,void* a,void* b) {(void)d;(void)a;(void)b;return 1;}
static void WddmReport(BC250_DEVICE* d,DXGKARGCB_NOTIFY_INTERRUPT_DATA* data)
{
 check(!lockHeld,"scheduler notification outside queue lock");
 events[eventCount++]=*data;
 if(data->InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED) {
  check(!d->Wddm || !((BC250_WDDM*)d->Wddm)->PagingHead,"preempt notification releases private FIFO");
  check(!preemptedOne->Borrowed&&!preemptedTwo->Borrowed,"unexecuted slots released before scheduler callback");
  if(replayData) {
   check(WddmSubmitPagingHardware(d,d->Wddm,replayData,replayBytes,0x20000,192,TRUE,52),"scheduler may replay same fence during callback");
   replayData=NULL;
  }
 }
}

static void WddmPreemptFence(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    KIRQL irql;
    BOOLEAN busy;
    LONG active;

    if (wddm == NULL || NodeOrdinal >= BC250_WDDM_NODE_COUNT_MAX) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    wddm->PreemptionNode[NodeOrdinal] = (LONG)NodeOrdinal;
    wddm->PreemptionFence[NodeOrdinal] = (LONG)FenceId;
    wddm->PreemptionPending[NodeOrdinal] = 1;
    busy = NodeOrdinal == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : wddm->HwPending;
    active = wddm->ActiveSubmissions[NodeOrdinal];
    KeReleaseSpinLock(&wddm->Lock, irql);
    GuardLog("wddm: preemption queued fence %u node %u hardware pending %u active submits %ld",
             FenceId, NodeOrdinal, busy, active);
    WddmQueueReport(wddm);
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
            !(node == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : wddm->HwPending))
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
        // A faulted paging engine cannot accept scheduler replay. Keep its queued
        // ownership for recovery even if the last hardware fence arrived late.
        preempt = wddm->PreemptionPending[node] != 0 && !wddm->RefusalPending[node] &&
                  wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&
                  !(node == BC250_WDDM_NODE_COPY ? wddm->PagingHwPending : wddm->HwPending) &&
                  !(node == BC250_WDDM_NODE_COPY && wddm->PagingHead && wddm->WatchdogFaulted[node]);
        if (preempt)
        {
            preemptFence = (UINT)wddm->PreemptionFence[node];
            lastFence = (UINT)wddm->LastReportedFence[node];
            if (node==BC250_WDDM_NODE_COPY) WddmReleasePreemptedPagingLocked(wddm);
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

static void reset(void);
static void preemption(void)
{
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 __declspec(align(8)) unsigned a[64]={0},b[64]={0},c[64]={0};
 BC250_PAGING_JOB *one,*two,*three;
 unsigned direct=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,4);
 unsigned native=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_NATIVE,192);
 unsigned char beforeB[PAGING_PRIVATE_NATIVE_BYTES],beforeC[28];int beforeAlloc=allocationCalls;
 reset();eventCount=0;
 a[6]=11;c[6]=33;
 check(PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4),"preempt direct A");
 check(PagingPrivateQueuedNativeHeader(b,sizeof(b),0,0x20000,192,0x4000,64,8,0),"preempt native B");
 check(PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4),"preempt direct C");
 one=PagingPrivateQueueSlot(a,direct,0x10000,4,TRUE);
 two=PagingPrivateQueueSlot(b,native,0x20000,192,TRUE);
 three=PagingPrivateQueueSlot(c,direct,0x30000,4,TRUE);
 check(one&&two&&three,"preempt slots valid");
 if(!one||!two||!three)return;
 preemptedOne=two;preemptedTwo=three;
 memcpy(beforeB,b,sizeof(beforeB));memcpy(beforeC,c,sizeof(beforeC));
 completionSlots[51]=one;completionSlots[52]=two;completionSlots[53]=three;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,51),"preempt first executing");
 check(WddmSubmitPagingHardware(&d,&w,b,native,0x20000,192,TRUE,52),"preempt second accepted");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,53),"preempt third accepted");
 WddmPreemptFence(&d,91,1);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!eventCount&&two->Borrowed&&three->Borrowed,"active DMA prevents preempt report or ownership release");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(submitted==1&&!w.PagingHwPending&&completionCount==1,"preemption stops exactly after executing buffer");
 if(submitted!=1)return; // Negative mutation has dispatched B; avoid pretending it halted.
 check(w.PagingHead==two&&two->Next==three,"preempted FIFO retained until notification decision");
 replayData=b;replayBytes=native;
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==2&&events[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED&&
  events[0].DmaCompleted.SubmissionFenceId==51,"completion of A precedes preemption");
 check(events[1].InterruptType==DXGK_INTERRUPT_DMA_PREEMPTED&&
  events[1].DmaPreempted.PreemptionFenceId==91&&events[1].DmaPreempted.LastCompletedFenceId==51,
  "preemption names only actual completed fence");
 check(submitted==2&&w.PagingHead==two&&two->Fence==52&&!three->Borrowed,"only scheduler replay launches B");
 check(!memcmp(beforeB,b,sizeof(beforeB))&&!memcmp(beforeC,c,sizeof(beforeC)),"command metadata and payload survive queue detachment");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,53),"scheduler replays C unchanged fence");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 arrived=activeSeq;WddmGpuFencePaging(&d);
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(submitted==3&&completionCount==3&&fences[0]==51&&fences[1]==52&&fences[2]==53,"each accepted buffer executes exactly once after replay");
 check(submittedBytes[0]==11&&submittedBytes[1]==77&&submittedBytes[2]==33,"replay preserves direct/native command interpretation");
 check(!w.PagingHead&&!w.PagingTail&&!w.PreemptionPending[1]&&!failureCount&&allocationCalls==beforeAlloc,"replay drains without allocation or stranded work");

 // A submit already in flight at the preempt decision may join the software
 // FIFO. Its active wrapper prevents detachment until admission has finished.
 reset();memset(&w,0,sizeof(w));eventCount=0;replayData=NULL;
 a[6]=11;c[6]=33;PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4);
 preemptedOne=one;preemptedTwo=three;
 WddmPreemptFence(&d,92,1);w.ActiveSubmissions[1]=1;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,61),"racing submit retains ownership while preempt pending");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,62),"racing second submit retained");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(!submitted&&!eventCount&&one->Borrowed&&three->Borrowed,"active admission blocks notification and hardware launch");
 w.ActiveSubmissions[1]=0;WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&!completionCount&&events[0].DmaPreempted.LastCompletedFenceId==0,"idle boundary preempts all queued work without completion");
 check(!w.PagingHead&&!one->Borrowed&&!three->Borrowed,"racing admissions returned to scheduler");
 completionSlots[61]=one;completionSlots[62]=three;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,61),"idle-boundary same-fence replay");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,62),"idle-boundary second replay");
 arrived=activeSeq;WddmGpuFencePaging(&d);arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==2&&!w.PagingHead&&!failureCount,"idle-boundary replay completes normally");

 // A watchdog's late completion is real, but cannot make the closed engine
 // available for replay. Retain the never-executed tail for recovery.
 reset();memset(&w,0,sizeof(w));eventCount=0;
 a[6]=11;c[6]=33;PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4);
 completionSlots[71]=one;completionSlots[72]=three;
 check(WddmSubmitPagingHardware(&d,&w,a,direct,0x10000,4,TRUE,71),"fault control executing");
 check(WddmSubmitPagingHardware(&d,&w,c,direct,0x30000,4,TRUE,72),"fault control accepted tail");
 WddmPreemptFence(&d,93,1);w.WatchdogFaulted[1]=1;
 arrived=activeSeq;WddmGpuFencePaging(&d);WddmReportDpcRoutine(NULL,&d,NULL,NULL);
 check(eventCount==1&&events[0].InterruptType==DXGK_INTERRUPT_DMA_COMPLETED&&
  events[0].DmaCompleted.SubmissionFenceId==71,"late fence reports only real completion");
 check(w.PreemptionPending[1]&&w.PagingHead==three&&three->Borrowed&&submitted==1,
  "faulted replay path retains accepted ownership without false preempt acknowledgement");
 TestStopDrain(&w); // Host cleanup after simulated hardware idle, not recovery evidence.
}

static void legacy(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};unsigned a[7]={0};
 PagingPrivateHeader(a,sizeof(a),0,0,4);a[6]=11;
 check(!WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0,4,TRUE,11),"legacy record rejected without allocator fallback");
 check(!WddmSubmitPagingHardware(&d,&w,NULL,0,0,0,FALSE,12),"empty submission is not admitted by paging callers");
 check(!w.PagingHead&&!w.PagingQueueBorrowed&&!submitted&&!completionCount&&!allocated&&!allocationCalls,"unsupported admission has no ownership or completion effects");
}

static void reset(void) {
 check(!allocated,"all legacy allocations retired");
 lockHeld=submitted=completionCount=reports=failureCount=immediateFence=0;
 nextSeq=activeSeq=arrived=0;now=0;memset(completionSlots,0,sizeof(completionSlots));
}
static void borrowed(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 __declspec(align(8)) unsigned a[64]={0},b[64]={0},native[32]={0};
 BC250_PAGING_JOB *one,*two,*three,*four;int before=allocationCalls;
 unsigned size=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,12);
 reset();a[6]=11;a[7]=22;a[8]=33;b[6]=44;b[7]=55;b[8]=66;
 check(PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,12),"queued A construction");
 check(PagingPrivateQueuedHeader(b,sizeof(b),0,0x10000,12),"queued B construction");
 check(PagingPrivateQueuedNativeHeader(native,sizeof(native),0,0x20000,192,0x4000,64,8,0),"queued native construction");
 one=PagingPrivateQueueSlot(a,size,0x10000,4,TRUE);
 two=PagingPrivateQueueSlot(a,size,0x10004,4,TRUE);
 three=PagingPrivateQueueSlot(b,size,0x10000,4,TRUE);
 four=PagingPrivateQueueSlot(native,sizeof(native),0x20000,192,TRUE);
 check(one&&two&&three&&four&&one!=two&&one!=three,"distinct actual queue nodes");
 completionSlots[21]=one;completionSlots[22]=two;completionSlots[23]=three;completionSlots[24]=four;
 check(WddmSubmitPagingHardware(&d,&w,a,size,0x10000,4,TRUE,21),"borrow first");
 check(WddmSubmitPagingHardware(&d,&w,a,size,0x10004,4,TRUE,22),"borrow second split while busy");
 check(WddmSubmitPagingHardware(&d,&w,b,size,0x10000,4,TRUE,23),"same VA independent buffer queued");
 check(WddmSubmitPagingHardware(&d,&w,native,sizeof(native),0x20000,192,TRUE,24),"native whole record queued");
 check(!WddmSubmitPagingHardware(&d,&w,a,size,0x10000,4,TRUE,25),"live duplicate start preserves ownership");
 check(one->Fence==21&&one->Next==two&&two->Next==three&&three->Next==four,"FIFO links intact");
 check(!allocated&&allocationCalls==before&&submitted==1,"borrowed queue never allocates");
 arrived=1;WddmGpuFencePaging(&d);
 check(completionCount==1&&submittedBytes[1]==22,"second split dispatch after real fence");
 arrived=2;WddmGpuFencePaging(&d);
 check(completionCount==2&&submittedBytes[2]==44,"independent same VA content retained");
 arrived=3;WddmGpuFencePaging(&d);
 check(completionCount==3&&submittedBytes[3]==77,"native record dispatch");
 arrived=4;WddmGpuFencePaging(&d);
 check(completionCount==4&&!w.PagingHead&&!w.PagingTail&&!activeSeq,"borrowed queue drains");
 check(allocationCalls==before&&!allocated&&!failureCount,"no allocation or failure across completion reuse");
 // OS rebuilds the completed buffer, immediately completing the next dispatch.
 check(PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,12),"OS buffer rebuilt for reuse");
 completionSlots[26]=one;immediateFence=1;
 check(WddmSubmitPagingHardware(&d,&w,a,size,0x10000,4,TRUE,26),"reused slot accepted");
 WddmGpuFencePaging(&d);
 check(completionCount==5&&fences[4]==26&&!w.PagingHead,"immediate fence retires rebuilt slot");
 // Physical direct ranges use the same reservation machinery.
 PagingPrivateQueuedHeader(a,sizeof(a),16,0,12);immediateFence=0;
 completionSlots[27]=PagingPrivateQueueSlot(a,size,20,4,FALSE);
 check(WddmSubmitPagingHardware(&d,&w,a,size,20,4,FALSE,27),"physical partial range accepted");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==6&&submittedBytes[5]==22&&!allocated,"physical partial content and retirement");
}
static void retention(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 __declspec(align(8)) unsigned a[24]={0};BC250_PAGING_JOB* slot;
 int before=allocationCalls;reset();a[6]=9;
 PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 slot=PagingPrivateQueueSlot(a,sizeof(a),0x10000,4,TRUE);
 completionSlots[31]=slot;
 check(WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0x10000,4,TRUE,31),"timeout fixture admitted");
 now=w.PagingDeadline;WddmPagingSubmitDpcRoutine(NULL,&d,NULL,NULL);
 check(w.PagingHead==slot&&slot->Borrowed&&w.PagingHwPending&&!completionCount,"timeout retains borrowed memory and pending fence");
 check(w.WatchdogFaulted[1]&&failureCount==1,"timeout closes engine admission");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==1&&!w.PagingHead,"late real fence permits retirement");
 check(allocationCalls==before&&!allocated,"timeout path never allocates/frees OS memory");
 reset();memset(&w,0,sizeof(w));PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);
 check(WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0x10000,4,TRUE,32),"stop fixture admitted");
 w.Stopping=1;TestStopDrain(&w);
 check(!w.PagingHead&&!w.PagingTail&&!slot->Borrowed&&!allocated,"stop drain clears borrowed ownership without freeing OS storage");
 check(!WddmSubmitPagingHardware(&d,&w,a,sizeof(a),0x10000,4,TRUE,33)&&!slot->Borrowed,"stopped admission leaves OS slot untouched");
 // Host drain only: hardware quiescence/OS cancellation must be proved separately.
 activeSeq=0;w.PagingHwPending=0;
}
static void built_records(void) {
 BC250_WDDM w={0};BC250_DEVICE d={&w};
 const char* names[2]={"direct.bin","native.bin"};unsigned char* data[2]={0};
 unsigned headers[2][4]={{0}};ULONGLONG starts[2]={0};unsigned i;
 char folder[512],path[600];size_t required=0;int before=allocationCalls;
 if(getenv_s(&required,folder,sizeof(folder),"BC250_QUEUE_FIXTURE_DIR") || !required)return;
 reset();
 for(i=0;i<2;i++) {
  FILE* file=NULL;
  check(sprintf_s(path,sizeof(path),"%s/%s",folder,names[i])>0,"import fixture path");
  check(!fopen_s(&file,path,"rb") && file,"actual builder fixture exists");
  if(!file)return;
  check(fread(headers[i],sizeof(headers[i]),1,file)==1 && fread(&starts[i],sizeof(starts[i]),1,file)==1,"builder fixture framing");
  check(headers[i][0]<=PAGING_PRIVATE_BUFFER_BYTES && headers[i][0]>0,"fixture private extent bounded");
  if(!headers[i][0] || headers[i][0]>PAGING_PRIVATE_BUFFER_BYTES){fclose(file);return;}
  data[i]=calloc(1,headers[i][0]);check(data[i]!=NULL,"host fixture storage");
  if(!data[i]){fclose(file);return;}
  check(fread(data[i],headers[i][0],1,file)==1 && fgetc(file)==EOF,"exact emitted record imported");
  fclose(file);
  completionSlots[41+i]=PagingPrivateQueueSlot(data[i],headers[i][0],starts[i],headers[i][1],headers[i][2]);
  check(completionSlots[41+i]!=NULL,"real builder output supplies queue node");
  check(WddmSubmitPagingHardware(&d,&w,data[i],headers[i][0],starts[i],headers[i][1],(BOOLEAN)headers[i][2],41+i),"actual builder-to-submit positive path");
 }
 check(w.PagingQueueBorrowed==2 && submitted==1&&!allocated&&allocationCalls==before,"both actual records borrowed while first is pending");
 arrived=activeSeq;WddmGpuFencePaging(&d);
 arrived=activeSeq;WddmGpuFencePaging(&d);
 check(completionCount==2&&fences[0]==41&&fences[1]==42&&!w.PagingHead&&!failureCount,"actual builder records retire in order");
 check(submittedBytes[0]==headers[0][3]&&submittedBytes[1]==headers[1][3],"queue preserves actual direct/native command interpretation");
 free(data[0]);free(data[1]);
}
// Actual DDI wrappers + queue/fence callbacks; mocks only select admission outcomes.
#define BC250_WDDM_LOG_CALLS 4
#define BC250_WDDM_VMID 1
#define BC250_WDDM_MAGIC_CONTEXT 1
#define STATUS_SUCCESS 0
#define STATUS_INVALID_PARAMETER ((NTSTATUS)-3)
typedef void* HANDLE;
typedef struct {UINT NodeOrdinal; ULONGLONG RootPhysical;} BC250_WDDM_OBJECT;
typedef struct {HANDLE hContext;UINT NodeOrdinal,SubmissionFenceId;} DXGKARG_SUBMITCOMMAND;
typedef DXGKARG_SUBMITCOMMAND DXGKARG_SUBMITCOMMANDVIRTUAL;
static int admissionMode,computeRefuse,computeReadCount;
static ULONG computeSequence,computeArrived;
static const void* admissionData;static ULONG admissionBytes;static ULONGLONG admissionStart;
static BOOLEAN WddmSubmitHardware(BC250_DEVICE*,BC250_WDDM*,const BC250_WDDM_OBJECT*,ULONGLONG,ULONG,UINT,UINT);
static BC250_WDDM* WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static BC250_WDDM_OBJECT* WddmObject(HANDLE h,UINT magic){(void)magic;return h;}
static BOOLEAN GfxFenceArrived(BC250_DEVICE*d,ULONG seq){(void)d;computeReadCount++;return seq==computeArrived;}
static NTSTATUS GfxSubmitIb(BC250_DEVICE*d,UINT vm,ULONGLONG root,ULONGLONG va,ULONG bytes,ULONG*seq)
{(void)d;(void)vm;(void)root;(void)va;(void)bytes;if(computeRefuse)return -1;*seq=++computeSequence;return 0;}
static NTSTATUS Bc250WddmSubmitCommandImpl(HANDLE h,const DXGKARG_SUBMITCOMMAND* s)
{
 BC250_DEVICE*d=h;BC250_WDDM*w=d->Wddm;UINT n=s->NodeOrdinal;BC250_WDDM_FENCE_LEDGER snap;UINT f=0;BOOLEAN v=FALSE;
 (void)snap;(void)f;(void)v;
 if(admissionMode==1)return STATUS_INVALID_PARAMETER;
 if(admissionMode==2){w->RefusalPending[n]=TRUE;return STATUS_SUCCESS;}
 if(admissionMode==3){w->FenceLedger[n].Epoch++;return STATUS_SUCCESS;} // stale publisher negative control
 if(n==1){check(WddmSubmitPagingHardware(d,w,admissionData,admissionBytes,admissionStart,4,TRUE,s->SubmissionFenceId),"actual queued admission");return STATUS_SUCCESS;}
 if(!WddmSubmitHardware(d,w,s->hContext,0x4000,64,s->SubmissionFenceId,n))w->RefusalPending[n]=TRUE;
 return STATUS_SUCCESS;
}
static NTSTATUS Bc250WddmSubmitCommandVirtualImpl(HANDLE h,const DXGKARG_SUBMITCOMMANDVIRTUAL*s)
{return Bc250WddmSubmitCommandImpl(h,s);}

static BOOLEAN WddmSnapshotFenceLedgerLocked(const BC250_WDDM* Wddm, UINT Node,
    BC250_WDDM_FENCE_LEDGER* Ledger, UINT* Reported, BOOLEAN* ReportedValid)
{
    if (Node>=Wddm->NodeCount || Wddm->ActiveSubmissions[Node]) return FALSE;
    *Ledger=Wddm->FenceLedger[Node];
    *Reported=(UINT)Wddm->LastReportedFence[Node];
    *ReportedValid=Wddm->LastReportedValid[Node];
    return TRUE;
}
void WddmGpuFence(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN done = FALSE;
    UINT fence = 0, node = 0;
    KIRQL irql;

    if (wddm == NULL) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->HwPending && wddm->HwEpoch==wddm->FenceLedger[wddm->HwNode].Epoch &&
        GfxFenceArrived(Device, wddm->HwSeq))
    {
        done = TRUE;
        WddmRecordFenceLedgerLocked(wddm,wddm->HwNode,wddm->HwEpoch,wddm->HwFence,TRUE);
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
static BOOLEAN WddmSubmitHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, _In_ const BC250_WDDM_OBJECT* Context,
                                  ULONGLONG GpuVa, ULONG Bytes, UINT FenceId, UINT Node)
{
    LARGE_INTEGER due;
    ULONG seq = 0;
    NTSTATUS status;
    KIRQL irql;

    ULONGLONG epoch;
    KeAcquireSpinLock(&Wddm->Lock,&irql);
    epoch=Wddm->FenceLedger[Node].Epoch;
    KeReleaseSpinLock(&Wddm->Lock,irql);
    status = GfxSubmitIb(Device, BC250_WDDM_VMID, Context->RootPhysical, GpuVa, Bytes, &seq);
    if (!NT_SUCCESS(status))
    {
        if (InterlockedIncrement(&Wddm->HwRefused) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: ring refused 0x%08X (fence %u, va 0x%llX, %u bytes): completed in software", status,
                     FenceId, GpuVa, Bytes);
        return FALSE;
    }
    due.QuadPart = -10000ll * BC250_WDDM_SUBMIT_TIMEOUT_MS;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    Wddm->HwPending = TRUE;
    Wddm->HwSeq = seq;
    Wddm->HwEpoch = epoch;
    Wddm->HwFence = FenceId;
    Wddm->HwNode = Node;
    if (!Wddm->Stopping) KeSetTimer(&Wddm->SubmitTimer, due, &Wddm->SubmitDpc);
    KeReleaseSpinLock(&Wddm->Lock, irql);
    if (InterlockedIncrement(&Wddm->HwSubmitted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: fence %u on the gfx ring: sequence %u, vmid %u, root 0x%llX, va 0x%llX, %u bytes", FenceId,
                 seq, (ULONG)BC250_WDDM_VMID, Context->RootPhysical, GpuVa, Bytes);
    WddmGpuFence(Device);               // the interrupt may have come and gone before HwPending was set
    return TRUE;
}
static NTSTATUS Bc250WddmSubmitCommand(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SUBMITCOMMAND* pSubmitCommand)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = context != NULL ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;
    BOOLEAN tracked = wddm != NULL && node < BC250_WDDM_NODE_COUNT_MAX;
    NTSTATUS status;
    KIRQL irql;
    ULONGLONG epoch=0;
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        epoch=wddm->FenceLedger[node].Epoch;
        wddm->ActiveSubmissions[node]++;
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    status = Bc250WddmSubmitCommandImpl(hAdapter, pSubmitCommand);
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        // STATUS_SUCCESS transfers responsibility to the driver, even when
        // RefusalPending retains a valid but undispatched packet for OS TDR.
        if (NT_SUCCESS(status))
            WddmRecordFenceLedgerLocked(wddm,node,epoch,pSubmitCommand->SubmissionFenceId,FALSE);
        wddm->ActiveSubmissions[node]--;
        KeReleaseSpinLock(&wddm->Lock, irql);
        WddmQueueReport(wddm);
    }
    return status;
}
static NTSTATUS Bc250WddmSubmitCommandVirtual(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* pSubmitCommand)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = context != NULL ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;
    BOOLEAN tracked = wddm != NULL && node < BC250_WDDM_NODE_COUNT_MAX;
    NTSTATUS status;
    KIRQL irql;
    ULONGLONG epoch=0;
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        epoch=wddm->FenceLedger[node].Epoch;
        wddm->ActiveSubmissions[node]++;
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    status = Bc250WddmSubmitCommandVirtualImpl(hAdapter, pSubmitCommand);
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        if (status == STATUS_INVALID_PARAMETER)
        {
            if (!wddm->RejectedPending[node] ||
                (LONG)(pSubmitCommand->SubmissionFenceId-wddm->RejectedFence[node]) > 0)
                wddm->RejectedFence[node]=pSubmitCommand->SubmissionFenceId;
            wddm->RejectedPending[node]=TRUE;
        }
        // STATUS_SUCCESS transfers responsibility to the driver, even when
        // RefusalPending retains a valid but undispatched packet for OS TDR.
        if (NT_SUCCESS(status))
            WddmRecordFenceLedgerLocked(wddm,node,epoch,pSubmitCommand->SubmissionFenceId,FALSE);
        wddm->ActiveSubmissions[node]--;
        KeReleaseSpinLock(&wddm->Lock, irql);
        WddmQueueReport(wddm);
    }
    return status;
}

static void ledger_tests(void)
{
 BC250_WDDM w={0};BC250_DEVICE d={&w};BC250_WDDM_OBJECT context={0,0x8000};
 DXGKARG_SUBMITCOMMAND call={NULL,1,101};BC250_WDDM_FENCE_LEDGER snap;UINT reported=0;BOOLEAN rv=FALSE;KIRQL irql;
 __declspec(align(8)) unsigned a[64]={0},b[64]={0},c[64]={0};unsigned bytes=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,4);
 reset();eventCount=0;w.NodeCount=2;w.FenceLedger[0].Epoch=7;w.FenceLedger[1].Epoch=7;
 a[6]=11;b[6]=22;c[6]=33;PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);PagingPrivateQueuedHeader(b,sizeof(b),0,0x20000,4);PagingPrivateQueuedHeader(c,sizeof(c),0,0x30000,4);
 admissionMode=0;admissionData=a;admissionBytes=bytes;admissionStart=0x10000;
 check(Bc250WddmSubmitCommand(&d,&call)==0,"physical A accepted by OS");
 call.SubmissionFenceId=102;admissionData=b;admissionStart=0x20000;check(Bc250WddmSubmitCommandVirtual(&d,&call)==0,"virtual B accepted");
 call.SubmissionFenceId=103;admissionData=c;admissionStart=0x30000;check(Bc250WddmSubmitCommandVirtual(&d,&call)==0,"virtual C accepted");
 check(w.FenceLedger[1].SubmittedValid&&w.FenceLedger[1].Submitted==103&&!w.FenceLedger[1].HardwareValid,"accepted ABC is distinct from unexecuted hardware");
 check(submitted==1&&w.PagingHwFence==101&&w.PagingHwEpoch==7,"only A reaches physical transport");
 w.PreemptionPending[1]=1;arrived=activeSeq;WddmGpuFencePaging(&d);
 check(w.FenceLedger[1].HardwareValid&&w.FenceLedger[1].Hardware==101&&w.FenceLedger[1].Submitted==103,"A hardware completion never becomes queued C");
 check(!w.LastReportedValid[1],"hardware completion recorded before OS notification");
 KeAcquireSpinLock(&w.Lock,&irql);check(WddmSnapshotFenceLedgerLocked(&w,1,&snap,&reported,&rv)&&snap.Hardware==101&&snap.Submitted==103&&!rv,"joined recovery snapshot separates A/C/mailbox");
 w.ActiveSubmissions[1]=1;check(!WddmSnapshotFenceLedgerLocked(&w,1,&snap,&reported,&rv),"snapshot waits for CPU publisher");w.ActiveSubmissions[1]=0;
 WddmReleasePreemptedPagingLocked(&w);w.PreemptionPending[1]=0;KeReleaseSpinLock(&w.Lock,irql);
 check(!w.PagingHead&&w.FenceLedger[1].Hardware==101,"empty-queue completion race still names real A");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);check(w.LastReportedFence[1]==101&&w.FenceLedger[1].Hardware==101,"notification has its own unchanged history");
 call.SubmissionFenceId=102;admissionData=b;admissionStart=0x20000;Bc250WddmSubmitCommandVirtual(&d,&call);
 check(w.FenceLedger[1].Submitted==103,"same-ID scheduler replay cannot regress latest OS-owned C");
 arrived=activeSeq;WddmGpuFencePaging(&d);call.SubmissionFenceId=103;admissionData=c;admissionStart=0x30000;Bc250WddmSubmitCommandVirtual(&d,&call);arrived=activeSeq;WddmGpuFencePaging(&d);
 check(w.FenceLedger[1].Hardware==103&&submitted==3&&!w.PagingHead,"scheduler replay executes ABC once");
 // True virtual refusal is not OS accepted; physical success-with-refusal is.
 admissionMode=1;call.SubmissionFenceId=104;check(Bc250WddmSubmitCommandVirtual(&d,&call)==STATUS_INVALID_PARAMETER,"invalid virtual packet rejected");
 check(w.FenceLedger[1].Submitted==103&&w.RejectedPending[1],"rejected virtual fence does not enter submitted ledger");
 WddmReportDpcRoutine(NULL,&d,NULL,NULL);check(w.LastReportedFence[1]==104&&w.FenceLedger[1].Hardware==103,"rejection bookkeeping cannot masquerade as GPU execution");
 admissionMode=2;call.SubmissionFenceId=105;check(Bc250WddmSubmitCommand(&d,&call)==0,"physical refused hardware retains successful OS status");
 check(w.FenceLedger[1].Submitted==105&&w.FenceLedger[1].Hardware==103&&w.RefusalPending[1],"OS-owned faulted packet remains in reset submitted history");
 // Hardware/notification divergence on compute when later software work waits.
 admissionMode=0;call.hContext=&context;call.NodeOrdinal=0;call.SubmissionFenceId=121;computeSequence=10;computeArrived=0;
 check(Bc250WddmSubmitCommandVirtual(&d,&call)==0,"compute actual publication");
 w.DeferredValid=TRUE;w.DeferredFence=122;computeArrived=w.HwSeq;WddmGpuFence(&d);
 check(w.FenceLedger[0].HardwareValid&&w.FenceLedger[0].Hardware==121&&w.SubmittedFence[0]==122,"GPU121 is distinct from deferred notification122");
 computeRefuse=1;call.SubmissionFenceId=123;Bc250WddmSubmitCommandVirtual(&d,&call);computeRefuse=0;
 check(w.FenceLedger[0].Submitted==123&&w.FenceLedger[0].Hardware==121,"ring refusal never invents execution but preserves OS ownership");
 // Epoch-stale CPU publication and GPU observations do not affect new history.
 admissionMode=3;call.SubmissionFenceId=124;Bc250WddmSubmitCommandVirtual(&d,&call);
 check(w.FenceLedger[0].Submitted==123,"stale wrapper cannot publish into changed epoch");
 w.HwPending=TRUE;w.HwNode=0;w.HwEpoch=7;w.HwFence=125;w.HwSeq=77;computeArrived=77;computeReadCount=0;
 WddmGpuFence(&d);check(w.HwPending&&w.FenceLedger[0].Hardware==121&&!computeReadCount,"old hardware epoch cannot retire or probe new owner");w.HwPending=FALSE;
 admissionMode=0;w.RefusalPending[1]=FALSE;w.FenceLedger[1].Epoch=9;call.hContext=NULL;call.NodeOrdinal=1;call.SubmissionFenceId=126;
 PagingPrivateQueuedHeader(a,sizeof(a),0,0x10000,4);admissionData=a;admissionStart=0x10000;Bc250WddmSubmitCommandVirtual(&d,&call);
 w.FenceLedger[1].Epoch=10;arrived=activeSeq;WddmGpuFencePaging(&d);
 check(w.PagingHead&&w.PagingHwPending&&w.FenceLedger[1].Hardware==103,"old paging epoch retains ownership without false completion");
 w.PagingHwPending=FALSE;WddmGpuFencePaging(&d);check(w.PagingHead&&submitted==4,"old queued epoch cannot autonomously dispatch");TestStopDrain(&w);activeSeq=0;
 // Fence zero is valid; wrapping and original-ID replay are not numeric max.
 memset(&w.FenceLedger[0],0,sizeof(w.FenceLedger[0]));w.FenceLedger[0].Epoch=11;
 WddmRecordFenceLedgerLocked(&w,0,11,0xfffffffEu,FALSE);WddmRecordFenceLedgerLocked(&w,0,11,0u,FALSE);WddmRecordFenceLedgerLocked(&w,0,11,0xffffffffu,FALSE);
 check(w.FenceLedger[0].SubmittedValid&&w.FenceLedger[0].Submitted==0,"wrap accepted zero and older replay cannot regress");
 WddmRecordFenceLedgerLocked(&w,0,10,9u,FALSE);check(w.FenceLedger[0].Submitted==0,"old epoch history update refused");
 check(!WddmSnapshotFenceLedgerLocked(&w,2,&snap,&reported,&rv),"node bound enforced");
}

int main(void) {
 legacy();borrowed();retention();built_records();preemption();ledger_tests();
 printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
