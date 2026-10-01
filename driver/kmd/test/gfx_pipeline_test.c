/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gfx_completion_queue.h"
#include "bc250_fence_order.h"
#define KernelMode 0
#define TRUE 1
#define FALSE 0
#define BC250_WDDM_LOG_CALLS 8
#define BC250_WDDM_SUBMIT_TIMEOUT_MS 500
#define BC250_WDDM_VMID 1
#define STATUS_DEVICE_BUSY (-1)
#define NT_SUCCESS(x) ((x)>=0)
#define UNREFERENCED_PARAMETER(x) ((void)(x))
typedef int BOOLEAN, NTSTATUS, KIRQL, KDPC, FAST_MUTEX;
typedef unsigned int UINT, ULONG;
typedef unsigned long long ULONG_PTR;       /* x64; the submit path takes a context pointer as a value */
typedef long LONG;
typedef unsigned long long ULONGLONG;
typedef long long LONGLONG;
typedef void* PVOID;
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
} BC250_WDDM;
typedef struct { void* Wddm; } BC250_DEVICE;
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
static unsigned int delays, complete_on_delay;
static LONGLONG due_time;
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %u: %s\n",__LINE__,#x); } } while(0)
static void KeAcquireSpinLock(int* l,KIRQL* i) { (void)l; *i=0; }
static void KeReleaseSpinLock(int* l,KIRQL i) { (void)l; (void)i; }
static void ExAcquireFastMutex(FAST_MUTEX* m) { (void)m; CHECK(!mutex); mutex=1; }
static void ExReleaseFastMutex(FAST_MUTEX* m) { (void)m; CHECK(mutex); mutex=0; }
static ULONGLONG KeQueryInterruptTime(void) { return mock_now; }
static void KeSetTimer(int* t,LARGE_INTEGER d,int* p) { (void)t; (void)p; timer=1; due_time=d.QuadPart; }
static void KeCancelTimer(int* t) { (void)t; timer=0; }
static LONG InterlockedIncrement(LONG* n) { return ++*n; }
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
static BC250_GFX_SUBMIT_IDENTITY last_identity;
static NTSTATUS GfxSubmitIb(BC250_DEVICE* d,ULONG v,ULONGLONG root,ULONGLONG va,ULONG bytes,
                            const BC250_GFX_SUBMIT_IDENTITY* id,ULONG* s)
{ (void)d; (void)v; (void)root; (void)va; (void)bytes; CHECK(id!=0); if(id) last_identity=*id;
  if(refuse || (root_wait && completed != mock_seq))return STATUS_DEVICE_BUSY; *s=++mock_seq; ++dispatches; return 0; }
/* The timeout register snapshot is defined above the extracted region in wddm.c: modeled, and counted. */
static unsigned int snapshots;
static void WddmTimeoutSnapshot(const BC250_DEVICE* d,ULONG seq,UINT fence,UINT node)
{ (void)d; (void)seq; (void)fence; (void)node; ++snapshots; }
static int GfxSubmitReady(BC250_DEVICE* d) { (void)d; return armed && completed==mock_seq; }
static int GfxSubmitBusy(BC250_DEVICE* d) { (void)d; return armed && completed!=mock_seq; }
static void KeDelayExecutionThread(int mode,int alert,LARGE_INTEGER* tick)
{
    (void)mode; (void)alert; CHECK(!mutex && tick->QuadPart==-10000);
    mock_now+=10000; ++delays;
    if(complete_on_delay && delays==complete_on_delay) completed=mock_seq;
}
#include "gfx_pipeline_actual.inc"
int main(void)
{
    BC250_WDDM w={0}; BC250_DEVICE d={&w}; BC250_WDDM_OBJECT c={0x1000,4242,TRUE,FALSE};
    unsigned int i;
    w.FenceLedger[0].Epoch=1; mock_now=100;
    for(i=1;i<=7;i++) CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,i,0));
    CHECK(w.GfxPending.Count==7 && w.HwFence==1 && w.HwPending && dispatches==7);
    /* KMD193: gfx.c is told who submitted, and the queue entry keeps it for the watchdog and for a dump. */
    CHECK(last_identity.Context!=0 && last_identity.ProcessId==4242);
    CHECK(last_identity.ContextFlags==BC250_PJ_CTX_UMD && last_identity.Fence==7 && last_identity.Node==0);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].ProcessId==4242);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].ContextFlags==BC250_PJ_CTX_UMD);
    CHECK(w.GfxPending.Items[w.GfxPending.Head].Context==last_identity.Context);
    CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,8,0));
    CHECK(dispatches==7 && reported==0 && ledger==0);
    completed=2; WddmGpuFence(&d); // coalesced interrupt must retire both 1 and 2
    CHECK(w.GfxPending.Count==5 && reported==2 && w.HwFence==3 && EXPECT_LEDGER(2));
    // Software fence after current tail must never be reported for the head.
    w.DeferredValid=1; w.DeferredFence=8; mock_now=200;
    CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,9,0));
    CHECK(!w.DeferredValid && w.HwFence==3 && due_time==-(5000000-100));
    completed=7; WddmGpuFence(&d);
    CHECK(reported==8 && EXPECT_LEDGER(7) && w.HwFence==9 && w.GfxPending.Count==1);
    // Queued stale timer must not fault the newer head before its own deadline.
    mock_now=5000100; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(failures==0 && w.HwPending);
    completed=mock_seq; WddmGpuFence(&d);
    CHECK(reported==9 && !w.HwPending && !timer);
    // No fake completion when dispatch refuses, or when the watchdog expires.
    refuse=1; CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,10,0));
    CHECK(reported==9 && w.GfxPending.Count==0); refuse=0;
    mock_now=6000000; CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,10,0));
    mock_now+=5000000; WddmSubmitDpcRoutine(NULL,&d,NULL,NULL);
    CHECK(failures==1 && reported==9 && w.HwPending && w.WatchdogFaulted[0]);
    CHECK(snapshots==1);    /* KMD193: one register snapshot per timeout, no more */
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
    CHECK(dispatches==2 && reported==0 && delays==0 && w.GfxPending.Count==2);
    // Root-switch pressure clears only when genuine completion is observed.
    root_wait=1; complete_on_delay=2;
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,3,0));
    CHECK(delays==2 && dispatches==3 && reported==2 && w.GfxPending.Count==1);
    root_wait=0; complete_on_delay=0;
    // Fill remaining completion slots; bounded admission waits for capacity.
    for(i=4;i<=9;i++) CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,i,0));
    CHECK(w.GfxPending.Count==7);
    complete_on_delay=delays+1;
    CHECK(WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,10,0));
    CHECK(delays==3 && reported==9 && dispatches==10 && w.GfxPending.Count==1);
    // No progress: wait is finite, no new dispatch or fabricated retirement.
    complete_on_delay=0; refuse=1; i=delays;
    CHECK(!WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,11,0));
    CHECK(delays-i==BC250_WDDM_SUBMIT_TIMEOUT_MS && reported==9 && dispatches==10);
    // Closed hardware does not wait or submit even with an outstanding job.
    armed=0; i=delays;
    CHECK(!WddmSubmitPresentHardware(&d,&w,&c,0x8000,128,11,0));
    CHECK(delays==i && reported==9 && dispatches==10);
    printf("GFX WDDM actual submit/completion/watchdog: %u checks, %u failures\n",checks,bad);
    return bad ? 1:0;
}
