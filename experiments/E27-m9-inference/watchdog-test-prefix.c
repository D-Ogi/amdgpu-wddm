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
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
typedef struct {long long QuadPart;} LARGE_INTEGER;
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_WDDM_TAG 1
typedef struct _BC250_PAGING_JOB {
 struct _BC250_PAGING_JOB* Next;
 unsigned char Data[4]; ULONG PrivateBytes,ByteCount;ULONGLONG Start;BOOLEAN VirtualAddress;UINT Fence;
} BC250_PAGING_JOB;
typedef struct {
 BC250_PAGING_JOB *PagingHead,*PagingTail;ULONGLONG PagingDeadline;int Stopping,PagingSubmitDpc;
 LONG PagingHwSubmitted,PagingHwRefused;
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

static ULONGLONG KeQueryInterruptTime(void){return 100000000;}
static void ExFreePoolWithTag(void*p,int tag){(void)tag;free(p);}
static int KeSetTimer(ULONG*t,LARGE_INTEGER d,int*p){(void)t;(void)d;(void)p;return 0;}
static NTSTATUS GfxSubmitPaging(BC250_DEVICE*d,const void*p,ULONG b,ULONGLONG s,ULONG n,BOOLEAN v,ULONG*q)
{(void)d;(void)p;(void)b;(void)s;(void)n;(void)v;(void)q;CHECK(0);return -1;}
static BOOLEAN WddmSubmitPagingHardware(BC250_DEVICE*d,BC250_WDDM*w,const void*p,ULONG b,ULONGLONG s,ULONG n,BOOLEAN v,UINT f)
{(void)d;(void)w;(void)p;(void)b;(void)s;(void)n;(void)v;(void)f;CHECK(0);return FALSE;}
static void WddmFailSubmission(BC250_DEVICE*d,UINT f,UINT n){(void)d;(void)f;(void)n;CHECK(0);}
static void SetPagingHead(BC250_WDDM*w,UINT fence){
 BC250_PAGING_JOB*j=calloc(1,sizeof(*j));CHECK(j!=NULL);j->Fence=fence;j->ByteCount=4;w->PagingHead=w->PagingTail=j;
}
