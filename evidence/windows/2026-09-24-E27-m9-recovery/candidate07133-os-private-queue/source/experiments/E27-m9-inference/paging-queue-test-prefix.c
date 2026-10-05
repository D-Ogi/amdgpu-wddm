
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
/* ACTUAL_JOB_TYPE */
typedef struct {
 BC250_PAGING_JOB *PagingHead,*PagingTail;
 ULONGLONG PagingDeadline;
 int Stopping,WatchdogFaulted[2],PagingHwPending,PagingDeferredValid;
 ULONG PagingHwSeq;
 UINT PagingHwFence;
 int Lock,PagingSubmitTimer,PagingSubmitDpc;
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
 (void)w;(void)node;check(lockHeld,"completion published under lock");
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
