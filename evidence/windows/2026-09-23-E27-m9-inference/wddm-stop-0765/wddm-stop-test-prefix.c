#include <stdio.h>
#include <string.h>
#define TRUE 1
#define FALSE 0
typedef struct {int Lock,Stopping,VSyncArmed,VSyncTimer,SubmitTimer,PagingSubmitTimer,SubmitDpc,PagingSubmitDpc,VSyncDpc,ReportDpc;} WDDM;
typedef struct {void*Wddm;int DcnVsyncArmed;} DEVICE;
static DEVICE*device;
static WDDM*lateReader;
static int flushed,priorReaderDone,badOrder,cancelled,removed,vsyncDisabled;
static void KeAcquireSpinLock(int*l,int*i){(void)l;*i=0;}
static void KeReleaseSpinLock(int*l,int i){(void)l;(void)i;}
static int InterlockedExchange(int*p,int v){int old=*p;*p=v;return old;}
static void* InterlockedExchangePointer(void**p,void*v){void*old=*p;*p=v;return old;}
static void KeCancelTimer(int*t){(void)t;cancelled++;if(!((WDDM*)device->Wddm)->Stopping)badOrder++;}
static void KeRemoveQueueDpc(int*d){(void)d;removed++;}
static int DcnVsyncEnable(DEVICE*d,int on){(void)on;vsyncDisabled++;if(d->DcnVsyncArmed)badOrder++;return 0;}
static void KeFlushQueuedDpcs(void){
 /* Old reader is joined. IH may queue another DPC just after this flush's boundary;
    its first pointer load must see NULL even before Stop reaches its next line. */
 priorReaderDone=1;flushed++;
 lateReader=device->Wddm;
}
