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
static void Stop(DEVICE* Device) { WDDM* wddm=Device->Wddm; int irql;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    wddm->Stopping = TRUE;
    if (wddm->VSyncArmed) { wddm->VSyncArmed = FALSE; KeCancelTimer(&wddm->VSyncTimer); }
    if (Device->DcnVsyncArmed != 0) { InterlockedExchange(&Device->DcnVsyncArmed, 0); (void)DcnVsyncEnable(Device, FALSE); }
    KeReleaseSpinLock(&wddm->Lock, irql);

    KeCancelTimer(&wddm->VSyncTimer);   // again, unconditionally: cheap, and it cannot be armed any more
    KeCancelTimer(&wddm->SubmitTimer);
    KeCancelTimer(&wddm->PagingSubmitTimer);
    KeRemoveQueueDpc(&wddm->SubmitDpc);
    KeRemoveQueueDpc(&wddm->PagingSubmitDpc);
    KeRemoveQueueDpc(&wddm->VSyncDpc);
    KeRemoveQueueDpc(&wddm->ReportDpc);
    InterlockedExchangePointer(&Device->Wddm, NULL);
    KeFlushQueuedDpcs();                // join readers admitted before detach, even while IH remains enabled
}

#define CHECK(c) do{if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;}}while(0)
int main(void){
 WDDM w={0};DEVICE d={&w,1};device=&d;w.VSyncArmed=1;
 Stop(&d);
 CHECK(priorReaderDone && flushed==1 && d.Wddm==NULL);
 CHECK(lateReader==NULL); /* otherwise it can dereference the object after free */
 CHECK(w.Stopping && !w.VSyncArmed && !d.DcnVsyncArmed && !badOrder);
 CHECK(cancelled==4 && removed==4 && vsyncDisabled==1);
 memset(&w,0,sizeof(w));d.Wddm=&w;d.DcnVsyncArmed=0;flushed=priorReaderDone=cancelled=removed=vsyncDisabled=0;
 Stop(&d);CHECK(lateReader==NULL && priorReaderDone && !badOrder && cancelled==3 && removed==4 && vsyncDisabled==0);
 puts("PASS: actual stop order detaches before DPC flush boundary, joins earlier reader, blocks late pointer capture, cancels timers/reports with vsync on/off");return 0;
}
