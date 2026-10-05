#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int BOOLEAN;
typedef unsigned long ULONG;
typedef long NTSTATUS;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define PAGE_SIZE 4096
#define BC250_GFX_STAGE_INTERRUPTS 8
#define BC250_PAGING_FENCE_SLOT 4
#define BC250_PAGING_MARKER_SLOT 5
#define STATUS_SUCCESS 0L
#define STATUS_INVALID_PARAMETER (-1L)
#define STATUS_INVALID_DEVICE_STATE (-2L)
#define STATUS_DEVICE_HARDWARE_ERROR (-3L)
#define STATUS_DEVICE_NOT_READY (-4L)
#define STATUS_IO_DEVICE_ERROR (-5L)
#define NT_SUCCESS(s) ((s)>=0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define GuardLog(...) ((void)0)
struct amdgpu_device { struct {struct {void*cpu;}fence_mem;}sdma; };
typedef struct {unsigned align_mask;} FUNCS;
typedef struct {FUNCS*funcs;unsigned max_dw;} RING;
typedef struct {
 int SubmitGate,PagingGate,Failed,SubmitFailed,PagingSubmitFailed,SubmitInFlight,PagingSubmitInFlight;
 int SetUp,StagesDone,PagingCpuBootstrap,PagingWindowReady,PagingReady;
 int FencePage,SdmaFencePage,IbPage;
 struct {unsigned long long size,mc;} PagingCopyStaging;
 RING*PagingRing;struct amdgpu_device*PagingDevicePtr;
} BC250_GFX;
typedef struct {unsigned long long mc,table,bytes;} PAGING_APERTURE;
typedef struct {
 int Started,FullWddm,GpuStopUnconfirmed,MmioGartEnabled,MmioPspEnabled,MmioIhEnabled,MmioGfxEnabled;
 int VramWriteEnabled,InterruptIsMessage,GfxPagingLock,GartLock;
 void *Wddm,*Mmio,*Gart,*Psp,*Ih,*GpuMem,*Gfx;
 struct {void*DxgkCbSynchronizeExecution,*DxgkCbNotifyInterrupt,*DxgkCbQueueDpc,*DxgkCbNotifyDpc;}Dxgk;
 PAGING_APERTURE WddmAperture;
} BC250_DEVICE;
#include "startup.h"
static int irql,shared,critical,mutex,checks,failures,missingFence;
static int KeGetCurrentIrql(void){return irql;}
static void KeEnterCriticalRegion(void){critical++;}
static void KeLeaveCriticalRegion(void){critical--;}
static void ExAcquirePushLockShared(int*p){(void)p;shared++;}
static void ExReleasePushLockShared(int*p){(void)p;shared--;}
static void ExAcquireFastMutex(int*p){(void)p;mutex++;}
static void ExReleaseFastMutex(int*p){(void)p;mutex--;}
static unsigned long long bc250_sdma_fence_addr(struct amdgpu_device*a,unsigned slot)
{(void)a;return missingFence==(int)slot?0:4096+slot*8;}
static void check(int ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
