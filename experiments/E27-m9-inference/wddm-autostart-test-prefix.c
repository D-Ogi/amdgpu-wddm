#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
typedef int BOOLEAN;
typedef unsigned long ULONG;
typedef unsigned long long ULONGLONG;
typedef long NTSTATUS;
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_INSUFFICIENT_RESOURCES (-1L)
#define STATUS_DEVICE_NOT_READY (-2L)
#define BC250_WDDM_TAG 0
#define POOL_FLAG_NON_PAGED 0
#define BC250_WDDM_NODE_COPY 1
#define BC250_WDDM_NODE_COUNT 1
#define BC250_CHILD_UID 256
#define BC250_WDDM_SEGMENT_VRAM 1
#define BC250_WDDM_SEGMENT_TABLES 3
#define SynchronizationTimer 0
#define RtlZeroMemory(p,n) memset(p,0,n)
#define GuardLog(...) ((void)0)
#define WddmReportDpcRoutine 1
#define WddmVSyncDpcRoutine 2
#define WddmSubmitDpcRoutine 3
#define WddmPagingSubmitDpcRoutine 4
typedef struct {ULONGLONG bytes;} PAGING_APERTURE;
typedef struct {int FullWddm;void*Wddm;PAGING_APERTURE WddmAperture;struct {ULONG TargetId;}Post;} BC250_DEVICE;
#include "startup.h"
typedef struct {
 BC250_DEVICE*Device;int TraceUmdProbes,Lock,Objects,ReportDpc,VSyncDpc,BlitGate,SubmitDpc,SubmitTimer;
 ULONG NodeCount;int PagingSubmitDpc,PagingSubmitTimer,VSyncTimer;ULONG VSyncTargetId;
 long long UmdProfileFrequency,VSyncLast,VSyncFrequency;
} BC250_WDDM;
static int checks,failures,g_FullWddm=1,allocs,live,failAlloc,layoutOk,vaGate,geometryFail,vidmmFail,gpuFail,vidmmLive,vidmmStops,gpuCalls,gpuReady,published;
static void check(int ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
static void*ExAllocatePool2(unsigned flags,size_t size,unsigned tag)
{(void)flags;(void)tag;allocs++;if(allocs==failAlloc)return NULL;live++;return calloc(1,size);}
static void ExFreePoolWithTag(void*p,unsigned tag){(void)tag;check(p!=NULL,"free owns pointer");live--;free(p);}
static int GuardReadSetting(const wchar_t*key,int fallback)
{(void)fallback;if(!wcscmp(key,L"EnableGpuVa"))return vaGate;if(!wcscmp(key,L"EnablePagingNode"))return 1;return 0;}
static long long KeQueryPerformanceCounter(long long*f){*f=1;return 1;}
static void KeInitializeSpinLock(int*p){*p=1;}
static void InitializeListHead(int*p){*p=1;}
static void KeInitializeDpc(int*p,int f,void*d){(void)f;(void)d;*p=1;}
static void KeInitializeTimer(int*p){*p=1;}
static void KeInitializeTimerEx(int*p,int t){(void)t;*p=1;}
static BOOLEAN WddmMemoryLayout(BC250_DEVICE*d,ULONGLONG*a,ULONGLONG*b,ULONGLONG*c,ULONGLONG*f)
{(void)d;*a=0;*b=4096;*c=4096;*f=4096;return layoutOk;}
static NTSTATUS GartCaptureAperture(BC250_DEVICE*d,PAGING_APERTURE*a)
{check(!d->Wddm,"geometry before publication");a->bytes=4096;return geometryFail?-7:0;}
static NTSTATUS VidMmStartLayout(BC250_DEVICE*d,ULONGLONG a,ULONGLONG b,ULONG v,ULONGLONG c,ULONGLONG f,ULONG t)
{(void)a;(void)b;(void)v;(void)c;(void)f;(void)t;check(!d->Wddm,"VidMm before publication");vidmmLive=1;return vidmmFail?-8:0;}
static void VidMmStop(void){vidmmStops++;vidmmLive=0;}
NTSTATUS GpuStartupInitialize(BC250_DEVICE*d,BC250_START_REPORT*r)
{
 check(!d->Wddm&&d->FullWddm&&vidmmLive&&d->WddmAperture.bytes&&live==2,"startup before admission, all CPU resources owned");
 gpuCalls++;r->Ready=!gpuFail;gpuReady=!gpuFail;return gpuFail?-9:0;
}
static void*InterlockedExchangePointer(void**p,void*v)
{void*old=*p;check(!old&&gpuReady&&vidmmLive&&live==1,"publish only ready state after releasing report");*p=v;published++;return old;}
