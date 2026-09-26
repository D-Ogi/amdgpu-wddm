/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "gfx_completion_queue.h"
#include "bc250_fence_order.h"
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
} BC250_WDDM;
typedef struct { void* Wddm; } BC250_DEVICE;
typedef struct { ULONGLONG RootPhysical; } BC250_WDDM_OBJECT;
static ULONGLONG mock_now;
#ifdef NO_RECOVERY_LEDGER
#define EXPECT_LEDGER(v) 1
#else
#define EXPECT_LEDGER(v) (ledger==(v))
#endif
static unsigned int completed,mock_seq,reported,ledger,reports,failures,bad,checks,dispatches;
static int refuse, timer,mutex;
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
static int GfxFenceArrived(BC250_DEVICE* d,ULONG v) { (void)d; return bc250_fence_reached(completed,v); }
static void GfxSubmitFail(BC250_DEVICE* d) { (void)d; ++failures; }
#ifndef NO_RECOVERY_LEDGER
static void WddmRecordFenceLedgerLocked(BC250_WDDM* w,UINT n,ULONGLONG e,UINT f,BOOLEAN hw)
{ (void)w; (void)n; (void)e; (void)hw; ledger=f; }
#endif
static void WddmRecordCompletionLocked(BC250_WDDM* w,UINT f,UINT n) { (void)w; (void)n; reported=f; }
static void WddmQueueReport(BC250_WDDM* w) { (void)w; ++reports; }
static NTSTATUS GfxSubmitIb(BC250_DEVICE* d,ULONG v,ULONGLONG root,ULONGLONG va,ULONG bytes,ULONG* s)
{ (void)d; (void)v; (void)root; (void)va; (void)bytes; if(refuse)return STATUS_DEVICE_BUSY; *s=++mock_seq; ++dispatches; return 0; }
#include "gfx_pipeline_actual.inc"
int main(void)
{
    BC250_WDDM w={0}; BC250_DEVICE d={&w}; BC250_WDDM_OBJECT c={0x1000};
    unsigned int i;
    w.FenceLedger[0].Epoch=1; mock_now=100;
    for(i=1;i<=7;i++) CHECK(WddmSubmitHardware(&d,&w,&c,0x4000,128,i,0));
    CHECK(w.GfxPending.Count==7 && w.HwFence==1 && w.HwPending && dispatches==7);
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
    CHECK(!WddmSubmitHardware(&d,&w,&c,0x4000,128,11,0));
    CHECK(reported==9);
    // Recovery epoch mismatch may not retire stale hardware work.
#ifndef NO_RECOVERY_LEDGER
    w.FenceLedger[0].Epoch=2; completed=mock_seq; WddmGpuFence(&d);
    CHECK(reported==9 && w.HwPending);
#endif
    printf("GFX WDDM actual submit/completion/watchdog: %u checks, %u failures\n",checks,bad);
    return bad ? 1:0;
}
