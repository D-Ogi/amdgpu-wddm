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
 int Lock; BOOLEAN HwPending,PagingHwPending,DeferredValid,PagingDeferredValid,WatchdogFaulted[2],RefusalPending[2];
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
