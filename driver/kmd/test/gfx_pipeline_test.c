/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "gfx_completion_queue.h"
#include "bc250_fence_order.h"
#include "submit_watchdog.h"          /* BD-114: the budget, the progress window and the two stamps */
#include "hang_recovery.h"
#include "gfx_pipeline_defines.inc"    /* wddm.c's own BC250_WDDM_HOLD_BUCKETS and hold deadline, by the generator */
#define KernelMode 0
#define Executive 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define IO_NO_INCREMENT 0
#define STATUS_SUCCESS 0
#define STATUS_TIMEOUT 0x102
#define C_ASSERT(e) _Static_assert(e, #e)    /* winnt.h has a typedef; the check is the same */
#define TRUE 1
#define FALSE 0
#define BC250_WDDM_LOG_CALLS 8
#define BC250_WDDM_NODE_COPY 1u
#define BC250_WDDM_NODE_COMPUTE 2u        /* wddm.c, above the extracted region */
#define RtlZeroMemory(p,n) memset((p),0,(n))
#define BC250_VMID_AUTO 0xFFFFFFFEu   /* vmid_pool.h: the WDDM path lets gfx.c choose (KMD214) */
#define STATUS_DEVICE_BUSY (-1)
#define NT_SUCCESS(x) ((x)>=0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
typedef int BOOLEAN, NTSTATUS, KIRQL, KDPC, FAST_MUTEX;
typedef unsigned int UINT, ULONG;
typedef unsigned long long ULONG_PTR;       /* x64; the submit path takes a context pointer as a value */
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG, LONG64;
typedef void* PVOID;
typedef int KEVENT;                         /* 1 = signaled; a NotificationEvent stays so until cleared */
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
typedef void KDEFERRED_ROUTINE(KDPC*,PVOID,PVOID,PVOID);
typedef struct {
    FAST_MUTEX GfxSubmitMutex;
    BC250_GFX_COMPLETION_QUEUE GfxPending;
    int Lock, Stopping, SubmitTimer, SubmitDpc;
    int HwPending, DeferredValid, WatchdogFaulted[3];
    UINT HwSeq,HwFence,HwNode,DeferredFence;
    ULONGLONG HwEpoch;
    struct { ULONGLONG Epoch; } FenceLedger[2];
    LONG HwCompleted,HwSubmitted,HwRefused,HwTimeouts;
    LONG FaultSnapshots;                /* KMD193: register snapshots taken by the watchdog */
    /* BD-114: the submit watchdog's budget, its cadence and its staleness window, types as in wddm.c. The budget
     * is latched in WddmStart, which is above the extracted region, so main() sets it the way a start would. */
    ULONG SubmitBudgetMs, SubmitTickMs;
    int PagingHwPending, PagingDiagnosticTaken, PagingDeferredValid;
    ULONG PagingHwFence, PagingHwSeq;
    ULONGLONG PagingDeadline;
    LONG PagingHwTimeouts;
    BC250_SUBMIT_WATCHDOG SubmitWatchdog;
    BC250_HANG_NODE_STATE Recovery[3];
    /* Node 2's channel (compute-node.md), types as in wddm.c. NodeCount is latched in WddmStart. */
    UINT NodeCount;
    FAST_MUTEX ComputeSubmitMutex;
    BC250_GFX_COMPLETION_QUEUE ComputePending;
    int ComputeHwPending, ComputeDeferredValid, ComputeTimer, ComputeDpc;
    UINT ComputeDeferredFence;
    volatile LONG ComputeHwSubmitted, ComputeHwCompleted, ComputeHwRefused, ComputePolls;
    volatile LONG SubmitActivityChanges, SubmitPrimes, SubmitGapResets;
    volatile LONG SubmitRearms, SubmitChecks, SubmitHeadMaxMs, SubmitQueueMaxMs;
    /* KMD196: the held-submission counters, types as in wddm.c */
    volatile LONG SubmitHolds, SubmitHoldSpinOnly, SubmitHeldHistogram[BC250_WDDM_HOLD_BUCKETS];
    volatile LONG64 SubmitHeldUs, SubmitHeldMaxUs, SubmitHoldSpins, SubmitHoldEventWakes, SubmitHoldTimeoutWakes;
} BC250_WDDM;
typedef struct { void* Wddm; volatile LONG GfxRetireGeneration; KEVENT GfxRetireEvent; } BC250_DEVICE;
/* KMD193: the identity WddmSubmitHardware copies into gfx.c's call and into the queue entry. */
typedef struct { ULONGLONG RootPhysical; ULONG CreatorProcessId; BOOLEAN UmdContext, SystemContext; } BC250_WDDM_OBJECT;
typedef struct { ULONGLONG Context; ULONG ProcessId, ContextFlags, Fence, Node; } BC250_GFX_SUBMIT_IDENTITY;
#define BC250_PJ_CTX_UMD 1u
#define BC250_PJ_CTX_SYSTEM 2u
static ULONGLONG mock_now;
#ifdef NO_RECOVERY_LEDGER
#define EXPECT_LEDGER(v) 1
#else
#define EXPECT_LEDGER(v) (ledger==(v))
#endif
static unsigned int completed,mock_seq,reported,ledger,reports,failures,bad,checks,dispatches;
/* Node 2's model: its own sequence and completion, so a gfx completion never retires a compute job. */
static unsigned int ccompleted,cmock_seq,cdispatches,reported_node;
static int crefuse, ctimer;
static int* compute_timer;
static int refuse, timer,mutex, armed=1, root_wait;
/* KMD196's held submission: every phase-1 stall and every phase-2 wait is one step; the job blocking the ring
 * completes at step complete_on_step, with its end-of-pipe interrupt unless lost_interrupt is set. */
static unsigned int steps, stalls, waits, complete_on_step, lost_interrupt;
static KIRQL irql_now;
static BC250_DEVICE* the_device;
static LONGLONG due_time;
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %u: %s\n",__LINE__,#x); } } while(0)
static void KeAcquireSpinLock(int* l,KIRQL* i) { (void)l; *i=0; }
static void KeReleaseSpinLock(int* l,KIRQL i) { (void)l; (void)i; }
static void ExAcquireFastMutex(FAST_MUTEX* m) { (void)m; CHECK(!mutex); mutex=1; }
static void ExReleaseFastMutex(FAST_MUTEX* m) { (void)m; CHECK(mutex); mutex=0; }
static ULONGLONG KeQueryInterruptTime(void) { return mock_now; }
static void KeSetTimer(int* t,LARGE_INTEGER d,int* p)
{ (void)p; if(t==compute_timer){ ctimer=1; return; } timer=1; due_time=d.QuadPart; }
static void KeCancelTimer(int* t) { if(t==compute_timer){ ctimer=0; return; } timer=0; }
static LONG InterlockedIncrement(volatile LONG* n) { return ++*n; }
static LONG InterlockedCompareExchange(volatile LONG* p,LONG v,LONG c) { LONG x=*p; if(x==c) *p=v; return x; }
static LONG64 InterlockedCompareExchange64(volatile LONG64* p,LONG64 v,LONG64 c) { LONG64 x=*p; if(x==c) *p=v; return x; }
static LONG64 InterlockedAdd64(volatile LONG64* p,LONG64 v) { return *p+=v; }
/* One clock: interrupt time and QPC both count 100 ns here, and only stalls and waits move them. */
static LONGLONG mock_qpc;
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER* f)
{ LARGE_INTEGER r; if(f) f->QuadPart=10000000; r.QuadPart=mock_qpc; return r; }
static void Advance(ULONGLONG ticks) { mock_now+=ticks; mock_qpc+=(LONGLONG)ticks; }
static KIRQL KeGetCurrentIrql(void) { return irql_now; }
static void KeClearEvent(KEVENT* e) { *e=0; }
static LONG KeSetEvent(KEVENT* e,int increment,BOOLEAN wait) { LONG old=*e; (void)increment; CHECK(!wait); *e=1; return old; }
void GfxRetireSignal(BC250_DEVICE* Device);   /* gfx.c, by the generator */
static void Step(void)
{
    if(++steps>100000){ printf("FAIL: a held submission never ended (%u steps)\n",steps); exit(1); }
    if(complete_on_step && steps==complete_on_step){ completed=mock_seq; if(!lost_interrupt) GfxRetireSignal(the_device); }
}
static void KeStallExecutionProcessor(ULONG us) { CHECK(!mutex && irql_now<=APC_LEVEL); Advance(us*10ull); ++stalls; Step(); }
/* The completion, if it comes, arrives half-way through the 1 ms timeout. */
static NTSTATUS KeWaitForSingleObject(KEVENT* e,int reason,int mode,BOOLEAN alertable,LARGE_INTEGER* timeout)
{
    CHECK(!mutex && irql_now<=APC_LEVEL && reason==Executive && mode==KernelMode && !alertable);
    CHECK(timeout && timeout->QuadPart==-10000);
    if(++waits>100000){ printf("FAIL: the event wait never blocked (%u waits)\n",waits); exit(1); }
    if(*e) return STATUS_SUCCESS;
    Advance(5000); Step();
    if(*e) return STATUS_SUCCESS;
    Advance(5000);
    return STATUS_TIMEOUT;
}
static void GuardLog(const char* fmt,...) { (void)fmt; }
/* hang.c's progress recorders: interlocked stores with no effect on the control flow under test. The watchdog
 * DPC has been wrapped in them since KMD172; this harness never declared them, which the Mesa half of the
 * generator hid by failing first. */
#define ProgressSiteSubmitWatchdogDpc 0
#define ProgressEnter(site) ((void)(site))
#define ProgressExit(site,value) ((void)(site),(void)(value))
static int GfxFenceArrived(BC250_DEVICE* d,ULONG v) { (void)d; return bc250_fence_reached(completed,v); }
static void GfxSubmitFail(BC250_DEVICE* d) { (void)d; ++failures; }
static void WddmGpuFencePaging(BC250_DEVICE* d)
{
    BC250_WDDM* w=(BC250_WDDM*)d->Wddm;
    if(w->PagingHwPending && completed==w->PagingHwSeq) {
        w->PagingHwPending=0; reported=w->PagingHwFence;
    }
}
#ifndef NO_RECOVERY_LEDGER
static void WddmRecordFenceLedgerLocked(BC250_WDDM* w,UINT n,ULONGLONG e,UINT f,BOOLEAN hw)
{ (void)w; (void)n; (void)e; (void)hw; ledger=f; }
#endif
static void WddmRecordCompletionLocked(BC250_WDDM* w,UINT f,UINT n) { (void)w; reported=f; reported_node=n; }
static void WddmQueueReport(BC250_WDDM* w) { (void)w; ++reports; }
/* The ring-gap instrument (ring_gap.h, KMD 0.7.213): WddmGfxHeadLocked calls one edge of it, and the gap
 * arithmetic itself is driven by run_ring_gap.ps1 with a clock of its own. Here it is a counted stub, so this
 * harness states what the extracted region calls without taking a second model of the histogram. The node
 * number is wddm.c's, which is above the extracted region. */
#define BC250_WDDM_NODE_3D 0u
static unsigned int ringGapEdges, ringGapBusy;
static void WddmRingGapEdgeLocked(BC250_WDDM* w,UINT node,BOOLEAN busy)
{ (void)w; CHECK(node==BC250_WDDM_NODE_3D || node==BC250_WDDM_NODE_COMPUTE); ++ringGapEdges; if(busy) ++ringGapBusy; }
static BC250_GFX_SUBMIT_IDENTITY last_identity;
/* KMD214: wddm.c asks for BC250_VMID_AUTO and keeps the VMID gfx.c chose; the model chooses 3 + seq % 5. */
static ULONG ModelVmid(ULONG seq) { return 3u + seq % 5u; }
static NTSTATUS GfxSubmitIb(BC250_DEVICE* d,ULONG v,ULONGLONG root,ULONGLONG va,ULONG bytes,
                            const BC250_GFX_SUBMIT_IDENTITY* id,ULONG* s,ULONG* used)
{ (void)d; (void)root; (void)va; (void)bytes; CHECK(id!=0); CHECK(v==BC250_VMID_AUTO); if(id) last_identity=*id;
  if(used) *used=0;
  if(refuse || (root_wait && completed != mock_seq))return STATUS_DEVICE_BUSY; *s=++mock_seq; ++dispatches;
  if(used) *used=ModelVmid(*s); return 0; }
/* KMD214: the timeout report names the job's VMID and asks gfx.c who held it. */
static unsigned int vmidReports; static ULONG lastReportedVmid;
static void GfxVmidReport(const BC250_DEVICE* d,const char* who,ULONG vmid)
{ (void)d; (void)who; ++vmidReports; lastReportedVmid=vmid; }
/* The timeout register snapshot is defined above the extracted region in wddm.c: modeled, and counted. */
static unsigned int snapshots;
static void WddmTimeoutSnapshot(const BC250_DEVICE* d,ULONG seq,UINT fence,UINT node)
{ (void)d; (void)seq; (void)fence; (void)node; ++snapshots; }
/* BD-114: the hardware's progress token. WddmSubmitProgress is defined above the extracted region in wddm.c
 * (it reads the fence slot and the command processor's fetch registers), so it is modeled here. A case that wants
 * the watchdog to fire leaves it alone; one that wants a long healthy job moves it. */
static ULONGLONG mock_progress = 0x9E3779B97F4A7C15ull;
static BOOLEAN mock_progress_available = TRUE;
static BOOLEAN WddmSubmitProgress(BC250_DEVICE* d, ULONGLONG* token)
{ (void)d; if (!mock_progress_available) return FALSE; *token=mock_progress; return TRUE; }
/* The HIP dispatch journal (hip_journal.c): WddmSubmitHardware names the dispatch to it after the ring write, and
 * WddmGpuFence retires it with its job. Both are defined outside the extracted region, so they are counted here,
 * with the last identity kept. Untagged work carries ID zero. */
static unsigned int journalSubmitted, journalCompleted;
static ULONGLONG lastJournalSubmitted, lastJournalCompleted;
static ULONG lastJournalSeq;
static void HipJournalSubmitted(ULONGLONG id,ULONG seq,ULONG vmid,ULONGLONG root)
{ (void)vmid; (void)root; ++journalSubmitted; lastJournalSubmitted=id; lastJournalSeq=seq; }
static void HipJournalComplete(ULONGLONG id) { ++journalCompleted; lastJournalCompleted=id; }
static int GfxSubmitReady(const BC250_DEVICE* d) { (void)d; return armed && completed==mock_seq; }
static int GfxSubmitBusy(const BC250_DEVICE* d) { (void)d; return armed && completed!=mock_seq; }
/* gfx.c's node 2 answers. The fence order is a plain wrap-aware difference, not bc250_fence_order.h, so that the
 * EqualityFence control of node 0 leaves these cases alone. */
static int GfxComputeFenceArrived(BC250_DEVICE* d,ULONG v) { (void)d; return (int)(ccompleted - v) >= 0; }
static int GfxComputeReady(const BC250_DEVICE* d) { (void)d; return armed && ccompleted==cmock_seq; }
static int GfxComputeBusy(const BC250_DEVICE* d) { (void)d; return armed && ccompleted!=cmock_seq; }
static NTSTATUS GfxSubmitCompute(BC250_DEVICE* d,ULONGLONG root,ULONGLONG va,ULONG bytes,
                                 const BC250_GFX_SUBMIT_IDENTITY* id,ULONG* s,ULONG* used)
{ (void)d; (void)root; (void)va; (void)bytes; CHECK(id!=0); if(id) last_identity=*id;
  if(used) *used=0;
  if(crefuse) return STATUS_DEVICE_BUSY; *s=++cmock_seq; ++cdispatches;
  if(used) *used=ModelVmid(*s); return 0; }
/* No KeDelayExecutionThread: KMD196 removed the bare 1 ms sleep from both hold phases, and a return of it fails
 * this build rather than pass unnoticed. */
#include "gfx_pipeline_actual.inc"
static LONG HistogramTotal(const BC250_WDDM* w)
{ LONG n=0; unsigned int i; for(i=0;i<BC250_WDDM_HOLD_BUCKETS;i++) n+=w->SubmitHeldHistogram[i]; return n; }
int main(void)
{
    BC250_WDDM w={0}; BC250_DEVICE d={&w}; BC250_WDDM_OBJECT c={0x1000,4242,TRUE,FALSE};
    unsigned int i; ULONGLONG t0;
    (void)GfxSubmitFail;    /* the stub of a call that this extraction of wddm.c need not hold */
    C_ASSERT(BC250_WDDM_HOLD_SPIN_US==25*BC250_WDDM_HOLD_SPIN_STEP_US);   /* the step counts below */
    /* BD-114: what WddmStart latches, with the lab's own configuration - TdrDelay 10 s (the GUI's default) and no
     * SubmitWatchdogMs - so the budget below is 12 s and the DPC looks for progress every 250 ms. The held
     * submission's own bound stays at BC250_WDDM_HOLD_DEADLINE_MS, which the generator reads from wddm.c. */
    { int invalid=0;
      w.SubmitBudgetMs=Bc250SubmitDiagnosticMs(0,&invalid);
      w.SubmitTickMs=Bc250SubmitTickMs(w.SubmitBudgetMs);
      CHECK(!invalid && w.SubmitBudgetMs==12000 && w.SubmitTickMs==250); }
    w.FenceLedger[0].Epoch=1; mock_now=100;
    for(i=1;i<=7;i++) CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,i,0,0));
    CHECK(w.GfxPending.Count==7 && w.HwFence==1 && w.HwPending && dispatches==7);
    CHECK(journalSubmitted==7 && lastJournalSubmitted==0 && lastJournalSeq==mock_seq);   /* untagged: ID zero */
    /* KMD193: gfx.c is told who submitted, and the queue entry keeps it for the watchdog and for a dump. */
    CHECK(last_identity.Context!=0 && last_identity.ProcessId==4242);
    CHECK(last_identity.ContextFlags==BC250_PJ_CTX_UMD && last_identity.Fence==7 && last_identity.Node==0);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].ProcessId==4242);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].ContextFlags==BC250_PJ_CTX_UMD);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].Context==last_identity.Context);
    { unsigned k; for(k=0;k<w.GfxPending.Count;k++){ const BC250_GFX_COMPLETION* j=&w.GfxPending.Items[(w.GfxPending.Head+k)%BC250_GFX_PENDING_MAX];
      CHECK(j->Vmid==ModelVmid(j->Seq)); } }
    CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,8,0,0));
    CHECK(dispatches==7 && reported==0 && ledger==0);
    completed=2; WddmGpuFence(&d); // coalesced interrupt must retire both 1 and 2
    CHECK(w.GfxPending.Count==5 && reported==2 && w.HwFence==3 && EXPECT_LEDGER(2));
    // Software fence after current tail must never be reported for the head.
    w.DeferredValid=1; w.DeferredFence=8; mock_now=200;
    CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,9,0,0));
    /* BD-114: the timer is armed for the next LOOK, one tick away, and not for the head's own deadline. A push
     * behind a running head leaves that head's deadline alone (checked below) and only moves the next look. */
    CHECK(!w.DeferredValid && w.HwFence==3 && due_time==-10000LL*250);
    { const BC250_GFX_COMPLETION* h=&w.GfxPending.Items[w.GfxPending.Head];
      CHECK(h->Fence==3 && h->HeadSince==100 && h->Deadline==100+10000ull*12000); }
    completed=7; WddmGpuFence(&d);
    CHECK(reported==8 && EXPECT_LEDGER(7) && w.HwFence==9 && w.GfxPending.Count==1);
    // Queued stale timer must not fault the newer head before its own deadline.
    mock_now=5000100; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(failures==0 && w.HwPending);
    CHECK(w.SubmitChecks==1 && w.SubmitRearms==1 && timer);   /* BD-114: the DPC re-armed itself instead */
    completed=mock_seq; WddmGpuFence(&d);
    CHECK(reported==9 && !w.HwPending && !timer);
    // No fake completion when dispatch refuses, or when the watchdog expires.
    refuse=1; CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,10,0,0));
    CHECK(reported==9 && w.GfxPending.Count==0); refuse=0;
    mock_now=6000000; CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,10,0,0));
    /* BD-114: the watchdog judges OBSERVED PROGRESS over the budget now, not wall clock since the ring write, so
     * it fires only after the DPC has WATCHED the token stand still for the whole budget. The first look reads
     * the token and opens the window; a gap of a budget or more between two looks starts a new window instead of
     * firing (a debugger break is not a hang), so what fires is the 49th look at the 250 ms cadence - one tick to
     * open the window and 12 s of looking at a token that does not move. One look earlier it must not fire.
     * A job that keeps the token moving never faults at all: that case is submit_watchdog_test.c's. */
    for(i=1;i<=48;i++){ mock_now+=10000ull*250; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
                        CHECK(!w.WatchdogFaulted[0] && failures==0 && w.SubmitRearms==(LONG)(1+i)); }
    mock_now+=10000ull*250; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(failures==0 && reported==9 && w.HwPending && !w.WatchdogFaulted[0]);
    CHECK(mock_now-6000000==10000ull*(12000+250));           /* the budget plus the look that opened the window */
    /* Diagnostic expiry keeps the observation timer and admission active. */
    CHECK(w.SubmitChecks==50 && w.SubmitRearms==50);
    CHECK(snapshots==1);    /* KMD193: one register snapshot per timeout, no more */
    CHECK(vmidReports==1 && lastReportedVmid==ModelVmid(mock_seq));   /* KMD214: the timed-out job's VMID */
    CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,11,0,0));
    for(i=0;i<100;i++){ mock_now+=2500000; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL); }
    CHECK(snapshots==1 && failures==0 && !w.WatchdogFaulted[0]);
    completed=mock_seq; WddmGpuFence(&d);
    CHECK(reported==11 && !w.HwPending && !timer);
    CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,12,0,0));
    CHECK(reported==11);
    // Recovery epoch mismatch may not retire stale hardware work.
#ifndef NO_RECOVERY_LEDGER
    w.FenceLedger[0].Epoch=2; completed=mock_seq; WddmGpuFence(&d);
    CHECK(reported==11 && w.HwPending);
#endif
    // The HIP dispatch journal: a tagged dispatch carries its ID from the ring write to its own retirement.
    memset(&w,0,sizeof(w)); w.FenceLedger[0].Epoch=1;
    mock_seq=completed=reported=dispatches=failures=0; mock_now=0;
    { unsigned int s0=journalSubmitted, c0=journalCompleted;
      CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,1,0,0x51));
      CHECK(journalSubmitted==s0+1 && lastJournalSubmitted==0x51 && lastJournalSeq==mock_seq);
      CHECK(w.GfxPending.Count==1 && w.GfxPending.Items[w.GfxPending.Head].HipJournalId==0x51);
      CHECK(journalCompleted==c0);                     /* not retired before its fence arrives */
      completed=mock_seq; WddmGpuFence(&d);
      CHECK(journalCompleted==c0+1 && lastJournalCompleted==0x51 && reported==1 && w.GfxPending.Count==0); }
    // BGP1 burst: the second job must enter while the first is outstanding.
    memset(&w,0,sizeof(w)); w.FenceLedger[0].Epoch=1;
    mock_seq=completed=reported=dispatches=failures=0; mock_now=0;
    CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x4000,128,1,0,"present"));
    CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x4000,128,2,0,"present"));
    CHECK(dispatches==2 && reported==0 && steps==0 && w.GfxPending.Count==2 && w.SubmitHolds==0);
    // Root-switch pressure clears only when genuine completion is observed. KMD196: the blocking job has ~0.13 ms
    // left at the refusal, so phase 1 catches it: two 20 us stalls, no wait, one hold counted as spin-only.
    the_device=&d; root_wait=1; complete_on_step=2;
    CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,3,0,"present"));
    CHECK(stalls==2 && waits==0 && dispatches==3 && reported==2 && w.GfxPending.Count==1);
    CHECK(w.SubmitHolds==1 && w.SubmitHoldSpinOnly==1 && w.SubmitHoldSpins==2);
    CHECK(w.SubmitHoldEventWakes==0 && w.SubmitHoldTimeoutWakes==0);
    CHECK(w.SubmitHeldUs==40 && w.SubmitHeldMaxUs==40 && w.SubmitHeldHistogram[0]==1);
    root_wait=0; complete_on_step=0;
    // Fill remaining completion slots; bounded admission waits for capacity.
    for(i=4;i<=9;i++) CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,i,0,"present"));
    CHECK(w.GfxPending.Count==7 && steps==2);
    complete_on_step=steps+1;
    CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,10,0,"present"));
    CHECK(steps==3 && reported==9 && dispatches==10 && w.GfxPending.Count==1 && w.SubmitHolds==2);
    // Past the spin budget: 25 stalls (500 us), then waits on the retirement event. Two time out, the completion
    // interrupt ends the third half-way: 0.5 + 2 + 0.5 ms held, the 2k-5k bucket.
    for(i=11;i<=16;i++) CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,i,0,"present"));
    CHECK(w.GfxPending.Count==7 && steps==3);
    complete_on_step=steps+BC250_WDDM_HOLD_SPIN_US/BC250_WDDM_HOLD_SPIN_STEP_US+3;
    CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,17,0,"present"));
    CHECK(stalls==3+25 && waits==3 && reported==16 && dispatches==17 && w.GfxPending.Count==1);
    CHECK(w.SubmitHolds==3 && w.SubmitHoldSpinOnly==2 && w.SubmitHoldEventWakes==1 && w.SubmitHoldTimeoutWakes==2);
    CHECK(w.SubmitHeldMaxUs==3000 && w.SubmitHeldHistogram[5]==1);
    // A lost end-of-pipe interrupt: nobody signals, the 1 ms timeout ends the wait and the retry's own fence read
    // retires the job.
    for(i=18;i<=23;i++) CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,i,0,"present"));
    complete_on_step=steps+25+1; lost_interrupt=1;
    CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,24,0,"present"));
    CHECK(waits==4 && reported==23 && dispatches==24 && w.SubmitHolds==4 && w.SubmitHoldTimeoutWakes==3);
    CHECK(w.SubmitHoldSpinOnly==2 && w.SubmitHoldEventWakes==1);   /* a timeout wake is no spin-only hold */
    CHECK(w.SubmitHeldMaxUs==3000 && w.SubmitHeldHistogram[4]==1);    /* 0.5 + 1 ms */
    complete_on_step=0; lost_interrupt=0;
    // A stopping adapter and a caller at DISPATCH_LEVEL do not wait at all: one terminal attempt, refused.
    for(i=25;i<=30;i++) CHECK(WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,i,0,"present"));
    CHECK(w.GfxPending.Count==7);
    i=steps; w.Stopping=1;
    CHECK(!WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,31,0,"present"));
    CHECK(steps==i && dispatches==30 && w.SubmitHolds==4);
    w.Stopping=0; irql_now=DISPATCH_LEVEL;
    CHECK(!WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,31,0,"present"));
    CHECK(steps==i && dispatches==30 && w.SubmitHolds==4);
    irql_now=0;
    // No progress: the wait is the deadline plus at most one 1 ms wait, with no new dispatch or fabricated
    // retirement.
    refuse=1; i=waits; t0=mock_now;
    CHECK(!WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,31,0,"present"));
    CHECK(mock_now-t0>=10000ull*BC250_WDDM_HOLD_DEADLINE_MS && mock_now-t0<=10000ull*BC250_WDDM_HOLD_DEADLINE_MS+10000);
    CHECK(waits>i && reported==23 && dispatches==30 && w.SubmitHolds==5 && w.SubmitHoldEventWakes==1);
    CHECK(w.SubmitHoldTimeoutWakes==3+(LONG64)(waits-i) && w.SubmitHeldHistogram[8]==1);
    // Closed hardware does not wait or submit even with an outstanding job.
    armed=0; i=steps;
    CHECK(!WddmSubmitHeldHardware(&d,&w,&c,0x8000,128,31,0,"present"));
    CHECK(steps==i && reported==23 && dispatches==30);
    // Every hold is counted once, in one bucket; every retirement signalled the waiters.
    CHECK(HistogramTotal(&w)==w.SubmitHolds && d.GfxRetireGeneration!=0);
    /* An exclusive wave read is not a token observation. The real DPC must
     * preserve the last sample/counters, rearm, and never timeout on it. */
    memset(&w,0,sizeof(w)); w.FenceLedger[0].Epoch=1;
    mock_seq=completed=reported=dispatches=failures=0; mock_now=100;
    armed=1; refuse=0; root_wait=0;
    w.SubmitBudgetMs=12000; w.SubmitTickMs=250;
    CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,1,0,0));
    mock_now+=2500000; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
    { BC250_SUBMIT_WATCHDOG saved=w.SubmitWatchdog;
      LONG primes=w.SubmitPrimes,activity=w.SubmitActivityChanges,gaps=w.SubmitGapResets;
      LONG rearms=w.SubmitRearms;
      mock_progress_available=FALSE; mock_progress++;
      mock_now+=130000000; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
      CHECK(memcmp(&saved,&w.SubmitWatchdog,sizeof(saved))==0);
      CHECK(w.SubmitPrimes==primes && w.SubmitActivityChanges==activity && w.SubmitGapResets==gaps);
      CHECK(w.SubmitRearms==rearms+1 && failures==0 && !w.WatchdogFaulted[0]);
      mock_progress_available=TRUE;
      mock_now+=2500000; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
      CHECK(w.SubmitGapResets==gaps+1 && failures==0 && !w.WatchdogFaulted[0]); }
    /* Node 1: expiry preserves pending/deferred work and queue admission. A
     * duplicate queued timer is bounded to one diagnostic; a late real fence
     * remains retirable. Stop/reset suppress diagnostics from stale timers. */
    memset(&w,0,sizeof(w)); mock_now=100; completed=reported=failures=0;
    w.PagingHwPending=1; w.PagingDeferredValid=1; w.PagingHwSeq=55;
    w.PagingHwFence=88; w.PagingDeadline=200; w.SubmitBudgetMs=12000;
    WddmPagingSubmitDpcCheck(NULL,&d,NULL,NULL);
    CHECK(!w.PagingDiagnosticTaken && !w.PagingHwTimeouts);
    mock_now=200;
    WddmPagingSubmitDpcCheck(NULL,&d,NULL,NULL);
    CHECK(w.PagingDiagnosticTaken && w.PagingHwTimeouts==1);
    CHECK(w.PagingHwPending && w.PagingDeferredValid && !w.WatchdogFaulted[1] && failures==0 && reported==0);
    WddmPagingSubmitDpcCheck(NULL,&d,NULL,NULL);
    CHECK(w.PagingHwTimeouts==1);
    completed=55;
    WddmPagingSubmitDpcCheck(NULL,&d,NULL,NULL);
    CHECK(!w.PagingHwPending && reported==88 && !w.WatchdogFaulted[1]);
    w.PagingHwPending=1; w.PagingHwSeq=56; w.PagingDiagnosticTaken=0;
    w.Stopping=1;
    WddmPagingSubmitDpcCheck(NULL,&d,NULL,NULL);
    CHECK(!w.PagingDiagnosticTaken && w.PagingHwTimeouts==1);
    w.Stopping=0; w.Recovery[1].ResetActive=1;
    WddmPagingSubmitDpcCheck(NULL,&d,NULL,NULL);
    CHECK(!w.PagingDiagnosticTaken && w.PagingHwTimeouts==1);
    /* Node 2 (compute-node.md): its own queue, fence and poll timer, with the rules of node 0. */
    memset(&w,0,sizeof(w)); w.NodeCount=3; w.SubmitTickMs=250; compute_timer=&w.ComputeTimer;
    mock_seq=completed=reported=dispatches=failures=0; mock_now=100; armed=1; refuse=0;
    cmock_seq=ccompleted=cdispatches=0; ctimer=0; timer=0;
    for(i=1;i<=7;i++) CHECK(WddmRingSubmit(&d,&w,&c,0x9000,256,i,BC250_WDDM_NODE_COMPUTE,0));
    CHECK(w.ComputePending.Count==7 && w.ComputeHwPending && ctimer && !timer && cdispatches==7 && dispatches==0);
    CHECK(w.GfxPending.Count==0 && !w.HwPending && last_identity.Node==BC250_WDDM_NODE_COMPUTE);
    CHECK(w.ComputeHwSubmitted==7 && w.ComputePending.Items[w.ComputePending.Head].Vmid==ModelVmid(1));
    CHECK(!WddmRingSubmit(&d,&w,&c,0x9000,256,8,BC250_WDDM_NODE_COMPUTE,0));      /* the queue is full */
    CHECK(cdispatches==7 && reported==0);
    completed=1000; WddmGpuFence(&d);                 /* a gfx completion retires no compute job */
    CHECK(w.ComputePending.Count==7 && reported==0);
    ccompleted=2; WddmGpuFenceCompute(&d);            /* a coalesced interrupt retires 1 and 2 */
    CHECK(w.ComputePending.Count==5 && reported==2 && reported_node==BC250_WDDM_NODE_COMPUTE && ctimer);
    /* A software fence after the tail belongs to the tail, never to the new job. */
    w.ComputeDeferredValid=1; w.ComputeDeferredFence=8;
    CHECK(WddmRingSubmit(&d,&w,&c,0x9000,256,9,BC250_WDDM_NODE_COMPUTE,0));
    CHECK(!w.ComputeDeferredValid && w.ComputePending.Count==6);
    ccompleted=7; WddmGpuFenceCompute(&d);
    CHECK(reported==8 && w.ComputePending.Count==1);
    /* A lost interrupt: the poll timer's DPC reads the slot, retires the job and stops polling. */
    WddmComputeDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(w.ComputePending.Count==1 && ctimer && w.ComputePolls==1);     /* nothing arrived: it polls again */
    ccompleted=cmock_seq; WddmComputeDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(reported==9 && w.ComputePending.Count==0 && !w.ComputeHwPending && !ctimer && w.ComputePolls==2);
    /* A reset in progress on node 2 retires nothing and does not poll. */
    CHECK(WddmRingSubmit(&d,&w,&c,0x9000,256,10,BC250_WDDM_NODE_COMPUTE,0));
    w.Recovery[BC250_WDDM_NODE_COMPUTE].ResetActive=1; ccompleted=cmock_seq; ctimer=0;
    WddmComputeDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(reported==9 && w.ComputePending.Count==1 && !ctimer);
    w.Recovery[BC250_WDDM_NODE_COMPUTE].ResetActive=0; WddmGpuFenceCompute(&d);
    CHECK(reported==10 && w.ComputePending.Count==0);
    /* The shared paths (the UMD hold, ResetEngine) reach each node's own answers. */
    ccompleted=cmock_seq; completed=mock_seq;
    CHECK(WddmRingAdmits(&d,BC250_WDDM_NODE_COMPUTE) && !WddmRingBusy(&d,BC250_WDDM_NODE_COMPUTE));
    CHECK(WddmRingSubmit(&d,&w,&c,0x9000,256,11,BC250_WDDM_NODE_COMPUTE,0));
    CHECK(WddmRingBusy(&d,BC250_WDDM_NODE_COMPUTE) && !WddmRingBusy(&d,BC250_WDDM_NODE_3D));
    CHECK(WddmRingQueue(&w,BC250_WDDM_NODE_COMPUTE)==&w.ComputePending);
    CHECK(WddmRingQueue(&w,BC250_WDDM_NODE_3D)==&w.GfxPending);
    WddmRingFence(&d,BC250_WDDM_NODE_3D); CHECK(w.ComputePending.Count==1);
    ccompleted=cmock_seq; WddmRingFence(&d,BC250_WDDM_NODE_COMPUTE);
    CHECK(w.ComputePending.Count==0 && reported==11);
    /* The head step of ResetEngine: an empty compute queue stops the poll and leaves node 0 alone. */
    ctimer=1; timer=1; WddmRingHeadLocked(&w,BC250_WDDM_NODE_COMPUTE);
    CHECK(!ctimer && timer && !w.ComputeHwPending); timer=0;
    /* A busy compute ring completes nothing and is not counted as a refusal. */
    crefuse=1; CHECK(!WddmRingSubmit(&d,&w,&c,0x9000,256,12,BC250_WDDM_NODE_COMPUTE,0));
    CHECK(w.ComputePending.Count==0 && reported==11 && w.ComputeHwRefused==0); crefuse=0;
    /* With two nodes the compute fence read touches nothing. */
    w.NodeCount=2; w.ComputePending.Count=1; w.ComputePending.Items[w.ComputePending.Head].Seq=cmock_seq;
    WddmGpuFenceCompute(&d);
    CHECK(w.ComputePending.Count==1 && reported==11);
    printf("GFX WDDM actual submit/completion/watchdog: %u checks, %u failures\n",checks,bad);
    return bad ? 1:0;
}
