/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "gfx_completion_queue.h"
#include "bc250_fence_order.h"
#include "submit_watchdog.h"          /* BD-114: the budget, the progress window and the two stamps */
#include "gfx_pipeline_defines.inc"    /* wddm.c's own BC250_WDDM_HOLD_BUCKETS and hold deadline, by the generator */
#define KernelMode 0
#define Executive 0
#define APC_LEVEL 1
#define DISPATCH_LEVEL 2
#define IO_NO_INCREMENT 0
#define STATUS_SUCCESS 0
#define STATUS_TIMEOUT 0x102
#define C_ASSERT(e) typedef char __C_ASSERT__[(e)?1:-1]    /* as winnt.h */
#define TRUE 1
#define FALSE 0
#define BC250_WDDM_LOG_CALLS 8
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
    int HwPending, DeferredValid, WatchdogFaulted[2];
    UINT HwSeq,HwFence,HwNode,DeferredFence;
    ULONGLONG HwEpoch;
    struct { ULONGLONG Epoch; } FenceLedger[2];
    LONG HwCompleted,HwSubmitted,HwRefused,HwTimeouts;
    LONG FaultSnapshots;                /* KMD193: register snapshots taken by the watchdog */
    /* BD-114: the submit watchdog's budget, its cadence and its staleness window, types as in wddm.c. The budget
     * is latched in WddmStart, which is above the extracted region, so main() sets it the way a start would. */
    ULONG SubmitBudgetMs, SubmitTickMs, SubmitTdrMs;
    BC250_SUBMIT_WATCHDOG SubmitWatchdog;
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
static int refuse, timer,mutex, armed=1, root_wait;
/* KMD196's held submission: every phase-1 stall and every phase-2 wait is one step; the job blocking the ring
 * completes at step complete_on_step, with its end-of-pipe interrupt unless lost_interrupt is set. */
static unsigned int steps, stalls, waits, complete_on_step, lost_interrupt, signals;
static KIRQL irql_now;
static BC250_DEVICE* the_device;
static LONGLONG due_time;
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %u: %s\n",__LINE__,#x); } } while(0)
static void KeAcquireSpinLock(int* l,KIRQL* i) { (void)l; *i=0; }
static void KeReleaseSpinLock(int* l,KIRQL i) { (void)l; (void)i; }
static void ExAcquireFastMutex(FAST_MUTEX* m) { (void)m; CHECK(!mutex); mutex=1; }
static void ExReleaseFastMutex(FAST_MUTEX* m) { (void)m; CHECK(mutex); mutex=0; }
static ULONGLONG KeQueryInterruptTime(void) { return mock_now; }
static void KeSetTimer(int* t,LARGE_INTEGER d,int* p) { (void)t; (void)p; timer=1; due_time=d.QuadPart; }
static void KeCancelTimer(int* t) { (void)t; timer=0; }
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
#ifndef NO_RECOVERY_LEDGER
static void WddmRecordFenceLedgerLocked(BC250_WDDM* w,UINT n,ULONGLONG e,UINT f,BOOLEAN hw)
{ (void)w; (void)n; (void)e; (void)hw; ledger=f; }
#endif
static void WddmRecordCompletionLocked(BC250_WDDM* w,UINT f,UINT n) { (void)w; (void)n; reported=f; }
static void WddmQueueReport(BC250_WDDM* w) { (void)w; ++reports; }
/* The ring-gap instrument (ring_gap.h, KMD 0.7.213): WddmGfxHeadLocked calls one edge of it, and the gap
 * arithmetic itself is driven by run_ring_gap.ps1 with a clock of its own. Here it is a counted stub, so this
 * harness states what the extracted region calls without taking a second model of the histogram. The node
 * number is wddm.c's, which is above the extracted region. */
#define BC250_WDDM_NODE_3D 0u
static unsigned int ringGapEdges, ringGapBusy;
static void WddmRingGapEdgeLocked(BC250_WDDM* w,UINT node,BOOLEAN busy)
{ (void)w; CHECK(node==BC250_WDDM_NODE_3D); ++ringGapEdges; if(busy) ++ringGapBusy; }
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
static ULONGLONG WddmSubmitProgress(BC250_DEVICE* d) { (void)d; return mock_progress; }
static int GfxSubmitReady(BC250_DEVICE* d) { (void)d; return armed && completed==mock_seq; }
static int GfxSubmitBusy(BC250_DEVICE* d) { (void)d; return armed && completed!=mock_seq; }
/* No KeDelayExecutionThread: KMD196 removed the bare 1 ms sleep from both hold phases, and a return of it fails
 * this build rather than pass unnoticed. */
#include "gfx_pipeline_actual.inc"
static LONG HistogramTotal(const BC250_WDDM* w)
{ LONG n=0; unsigned int i; for(i=0;i<BC250_WDDM_HOLD_BUCKETS;i++) n+=w->SubmitHeldHistogram[i]; return n; }
int main(void)
{
    BC250_WDDM w={0}; BC250_DEVICE d={&w}; BC250_WDDM_OBJECT c={0x1000,4242,TRUE,FALSE};
    unsigned int i; ULONGLONG t0;
    C_ASSERT(BC250_WDDM_HOLD_SPIN_US==25*BC250_WDDM_HOLD_SPIN_STEP_US);   /* the step counts below */
    /* BD-114: what WddmStart latches, with the lab's own configuration - TdrDelay 10 s (the GUI's default) and no
     * SubmitWatchdogMs - so the budget below is 12 s and the DPC looks for progress every 250 ms. The held
     * submission's own bound stays at BC250_WDDM_HOLD_DEADLINE_MS, which the generator reads from wddm.c. */
    { int defaulted=0, raised=0;
      w.SubmitTdrMs=Bc250SubmitTdrMs(10);
      w.SubmitBudgetMs=Bc250SubmitBudgetMs(0,10,&defaulted,&raised);
      w.SubmitTickMs=Bc250SubmitTickMs(w.SubmitBudgetMs);
      CHECK(defaulted==1 && !raised && w.SubmitBudgetMs==12000 && w.SubmitTickMs==250); }
    w.FenceLedger[0].Epoch=1; mock_now=100;
    for(i=1;i<=7;i++) CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,i,0));
    CHECK(w.GfxPending.Count==7 && w.HwFence==1 && w.HwPending && dispatches==7);
    /* KMD193: gfx.c is told who submitted, and the queue entry keeps it for the watchdog and for a dump. */
    CHECK(last_identity.Context!=0 && last_identity.ProcessId==4242);
    CHECK(last_identity.ContextFlags==BC250_PJ_CTX_UMD && last_identity.Fence==7 && last_identity.Node==0);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].ProcessId==4242);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].ContextFlags==BC250_PJ_CTX_UMD);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].Context==last_identity.Context);
    { unsigned k; for(k=0;k<w.GfxPending.Count;k++){ const BC250_GFX_COMPLETION* j=&w.GfxPending.Items[(w.GfxPending.Head+k)%BC250_GFX_PENDING_MAX];
      CHECK(j->Vmid==ModelVmid(j->Seq)); } }
    CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,8,0));
    CHECK(dispatches==7 && reported==0 && ledger==0);
    completed=2; WddmGpuFence(&d); // coalesced interrupt must retire both 1 and 2
    CHECK(w.GfxPending.Count==5 && reported==2 && w.HwFence==3 && EXPECT_LEDGER(2));
    // Software fence after current tail must never be reported for the head.
    w.DeferredValid=1; w.DeferredFence=8; mock_now=200;
    CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,9,0));
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
    refuse=1; CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,10,0));
    CHECK(reported==9 && w.GfxPending.Count==0); refuse=0;
    mock_now=6000000; CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,10,0));
    /* BD-114: the watchdog judges OBSERVED PROGRESS over the budget now, not wall clock since the ring write, so
     * it fires only after the DPC has WATCHED the token stand still for the whole budget. The first look reads
     * the token and opens the window; a gap of a budget or more between two looks starts a new window instead of
     * firing (a debugger break is not a hang), so what fires is the 49th look at the 250 ms cadence - one tick to
     * open the window and 12 s of looking at a token that does not move. One look earlier it must not fire.
     * A job that keeps the token moving never faults at all: that case is submit_watchdog_test.c's. */
    for(i=1;i<=48;i++){ mock_now+=10000ull*250; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
                        CHECK(!w.WatchdogFaulted[0] && failures==0 && w.SubmitRearms==(LONG)(1+i)); }
    mock_now+=10000ull*250; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(failures==1 && reported==9 && w.HwPending && w.WatchdogFaulted[0]);
    CHECK(mock_now-6000000==10000ull*(12000+250));           /* the budget plus the look that opened the window */
    /* The look that faults does not re-arm, so no further look is scheduled: a faulted node is not watched. The
     * KTIMER itself is simply left alone, which is why this is a count and not a KeCancelTimer. */
    CHECK(w.SubmitChecks==50 && w.SubmitRearms==49);
    CHECK(snapshots==1);    /* KMD193: one register snapshot per timeout, no more */
    CHECK(vmidReports==1 && lastReportedVmid==ModelVmid(mock_seq));   /* KMD214: the timed-out job's VMID */
    CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,11,0));
    CHECK(reported==9);
    // Recovery epoch mismatch may not retire stale hardware work.
#ifndef NO_RECOVERY_LEDGER
    w.FenceLedger[0].Epoch=2; completed=mock_seq; WddmGpuFence(&d);
    CHECK(reported==9 && w.HwPending);
#endif
    // BGP1 burst: the second job must enter while the first is outstanding.
    memset(&w,0,sizeof(w)); w.FenceLedger[0].Epoch=1;
    mock_seq=completed=reported=dispatches=failures=0; mock_now=0;
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x4000,128,1,0));
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x4000,128,2,0));
    CHECK(dispatches==2 && reported==0 && steps==0 && w.GfxPending.Count==2 && w.SubmitHolds==0);
    // Root-switch pressure clears only when genuine completion is observed. KMD196: the blocking job has ~0.13 ms
    // left at the refusal, so phase 1 catches it: two 20 us stalls, no wait, one hold counted as spin-only.
    the_device=&d; root_wait=1; complete_on_step=2;
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,3,0));
    CHECK(stalls==2 && waits==0 && dispatches==3 && reported==2 && w.GfxPending.Count==1);
    CHECK(w.SubmitHolds==1 && w.SubmitHoldSpinOnly==1 && w.SubmitHoldSpins==2);
    CHECK(w.SubmitHoldEventWakes==0 && w.SubmitHoldTimeoutWakes==0);
    CHECK(w.SubmitHeldUs==40 && w.SubmitHeldMaxUs==40 && w.SubmitHeldHistogram[0]==1);
    root_wait=0; complete_on_step=0;
    // Fill remaining completion slots; bounded admission waits for capacity.
    for(i=4;i<=9;i++) CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,i,0));
    CHECK(w.GfxPending.Count==7 && steps==2);
    complete_on_step=steps+1;
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,10,0));
    CHECK(steps==3 && reported==9 && dispatches==10 && w.GfxPending.Count==1 && w.SubmitHolds==2);
    // Past the spin budget: 25 stalls (500 us), then waits on the retirement event. Two time out, the completion
    // interrupt ends the third half-way: 0.5 + 2 + 0.5 ms held, the 2k-5k bucket.
    for(i=11;i<=16;i++) CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,i,0));
    CHECK(w.GfxPending.Count==7 && steps==3);
    complete_on_step=steps+BC250_WDDM_HOLD_SPIN_US/BC250_WDDM_HOLD_SPIN_STEP_US+3;
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,17,0));
    CHECK(stalls==3+25 && waits==3 && reported==16 && dispatches==17 && w.GfxPending.Count==1);
    CHECK(w.SubmitHolds==3 && w.SubmitHoldSpinOnly==2 && w.SubmitHoldEventWakes==1 && w.SubmitHoldTimeoutWakes==2);
    CHECK(w.SubmitHeldMaxUs==3000 && w.SubmitHeldHistogram[5]==1);
    // A lost end-of-pipe interrupt: nobody signals, the 1 ms timeout ends the wait and the retry's own fence read
    // retires the job.
    for(i=18;i<=23;i++) CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,i,0));
    complete_on_step=steps+25+1; lost_interrupt=1;
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,24,0));
    CHECK(waits==4 && reported==23 && dispatches==24 && w.SubmitHolds==4 && w.SubmitHoldTimeoutWakes==3);
    CHECK(w.SubmitHoldSpinOnly==2 && w.SubmitHoldEventWakes==1);   /* a timeout wake is no spin-only hold */
    CHECK(w.SubmitHeldMaxUs==3000 && w.SubmitHeldHistogram[4]==1);    /* 0.5 + 1 ms */
    complete_on_step=0; lost_interrupt=0;
    // A stopping adapter and a caller at DISPATCH_LEVEL do not wait at all: one terminal attempt, refused.
    for(i=25;i<=30;i++) CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,i,0));
    CHECK(w.GfxPending.Count==7);
    i=steps; w.Stopping=1;
    CHECK(!WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,31,0));
    CHECK(steps==i && dispatches==30 && w.SubmitHolds==4);
    w.Stopping=0; irql_now=DISPATCH_LEVEL;
    CHECK(!WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,31,0));
    CHECK(steps==i && dispatches==30 && w.SubmitHolds==4);
    irql_now=0;
    // No progress: the wait is the deadline plus at most one 1 ms wait, with no new dispatch or fabricated
    // retirement.
    refuse=1; i=waits; t0=mock_now;
    CHECK(!WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,31,0));
    CHECK(mock_now-t0>=10000ull*BC250_WDDM_HOLD_DEADLINE_MS && mock_now-t0<=10000ull*BC250_WDDM_HOLD_DEADLINE_MS+10000);
    CHECK(waits>i && reported==23 && dispatches==30 && w.SubmitHolds==5 && w.SubmitHoldEventWakes==1);
    CHECK(w.SubmitHoldTimeoutWakes==3+(LONG64)(waits-i) && w.SubmitHeldHistogram[8]==1);
    // Closed hardware does not wait or submit even with an outstanding job.
    armed=0; i=steps;
    CHECK(!WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,31,0));
    CHECK(steps==i && reported==23 && dispatches==30);
    // Every hold is counted once, in one bucket; every retirement signalled the waiters.
    CHECK(HistogramTotal(&w)==w.SubmitHolds && d.GfxRetireGeneration!=0);
    printf("GFX WDDM actual submit/completion/watchdog: %u checks, %u failures\n",checks,bad);
    return bad ? 1:0;
}
