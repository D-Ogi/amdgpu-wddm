/* Actual DCN poll/dispatch and top-level DPC; register delivery is modeled. */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
typedef long LONG,NTSTATUS;typedef long long LONG64;typedef unsigned long ULONG;
typedef bool BOOLEAN;typedef void *PVOID;typedef unsigned long long ULONGLONG;
#define TRUE true
#define FALSE false
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_REG_DMU_OTG0_OTG_GLOBAL_SYNC_STATUS 0
#define OTG0_OTG_GLOBAL_SYNC_STATUS__VUPDATE_NO_LOCK_EVENT_OCCURRED_MASK 0x4000
#ifndef _Inout_
#define _Inout_
#define _In_
#endif
typedef struct {
 void *Mmio;BOOLEAN VidPnFlipEnabled;
 LONG DcnVsyncArmed,DcnVsyncNoMmio,DcnVsyncFlipDisabled,DcnVsyncUnarmed;
 LONG DcnVsyncReadFailed,DcnVsyncRefused,DcnVsyncLastStatus,DcnVsyncNoEvent;
 LONG DcnVsyncAckFailed,DcnVsyncTicks,DcnVsyncAcked;
 LONG DcnVsyncDpcPolls,DcnVsyncDpcAcked,DcnVsyncDpcSyncFailures;
 LONG64 DcnVsyncEntryTime,DcnVsyncAckTime;
 struct {void *DeviceHandle;void(*DxgkCbQueueDpc)(void*);
 NTSTATUS(*DxgkCbSynchronizeExecution)(void*,BOOLEAN(*)(PVOID),PVOID,ULONG,BOOLEAN*);} Dxgk;
} BC250_DEVICE;
static int event_pending,vector_pending,synchronized,fail_sync,fail_read,fail_write;
static unsigned acks,reports,queues;
static LONG InterlockedIncrement(LONG *p){return ++*p;}
static LONG InterlockedExchange(LONG *p,LONG v){LONG old=*p;*p=v;return old;}
static LONG64 InterlockedExchange64(LONG64 *p,LONG64 v){LONG64 old=*p;*p=v;return old;}
static ULONGLONG KeQueryInterruptTime(void){return 1;}
static NTSTATUS MmioDcnRead(BC250_DEVICE*d,ULONG reg,ULONG*v){(void)d;(void)reg;if(!synchronized)return -1;*v=event_pending?0x4000:0;return fail_read?-1:0;}
static ULONG DcnVsyncAckValue(ULONG value){return value;}
static NTSTATUS MmioDcnWriteEx(BC250_DEVICE*d,ULONG reg,ULONG v,BOOLEAN quiet){(void)d;(void)reg;(void)v;(void)quiet;if(!synchronized||fail_write)return -1;event_pending=0;acks++;return 0;}
static void queue(void*p){(void)p;queues++;}
static NTSTATUS sync_call(void*d,BOOLEAN(*cb)(PVOID),PVOID ctx,ULONG n,BOOLEAN*ret){(void)d;(void)n;if(fail_sync)return -1;synchronized=1;*ret=cb(ctx);synchronized=0;return 0;}
#include "vsync_vector_actual.inc"
/* hang.c's progress recorders: interlocked stores with no effect on the DPC's control flow. */
#define ProgressEnter(site) ((void)0)
#define ProgressExit(site,value) ((void)(value))
static void IhDpc(BC250_DEVICE*d){(void)d;}
static BOOLEAN IhTakeVsync(BC250_DEVICE*d){int old=vector_pending;(void)d;vector_pending=0;return old!=0;}
static void WddmGpuFence(BC250_DEVICE*d){(void)d;}
static void WddmGpuFencePaging(BC250_DEVICE*d){(void)d;}
static void WddmDcnVsync(BC250_DEVICE*d){if(InterlockedExchange(&d->DcnVsyncAcked,0))reports++;}
static void WddmDpc(BC250_DEVICE*d){(void)d;}
#include "vsync_dpc_actual.inc"
static BC250_DEVICE dev;
static void reset(void){memset(&dev,0,sizeof(dev));dev.Mmio=&dev;dev.VidPnFlipEnabled=true;dev.DcnVsyncArmed=1;dev.Dxgk.DxgkCbQueueDpc=queue;dev.Dxgk.DxgkCbSynchronizeExecution=sync_call;event_pending=vector_pending=synchronized=fail_sync=fail_read=fail_write=0;acks=reports=queues=0;}
#define CHECK(x) do{if(!(x)){printf("FAIL line%d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void){
 reset();synchronized=1;CHECK(!DcnVsyncInterrupt(&dev));synchronized=0;
 event_pending=vector_pending=1;Bc250DpcRoutine(&dev);CHECK(acks==1&&reports==1&&dev.DcnVsyncDpcAcked==1&&!event_pending);
 Bc250DpcRoutine(&dev);CHECK(acks==1&&reports==1);
 reset();event_pending=vector_pending=1;synchronized=1;CHECK(DcnVsyncInterrupt(&dev));synchronized=0;Bc250DpcRoutine(&dev);CHECK(acks==1&&reports==1&&dev.DcnVsyncDpcAcked==0);
 reset();event_pending=vector_pending=1;dev.DcnVsyncArmed=0;Bc250DpcRoutine(&dev);CHECK(acks==0&&reports==0);
 reset();event_pending=vector_pending=1;fail_sync=1;Bc250DpcRoutine(&dev);CHECK(acks==0&&reports==0&&dev.DcnVsyncDpcSyncFailures==1);
 reset();event_pending=vector_pending=1;fail_read=1;Bc250DpcRoutine(&dev);CHECK(acks==0&&reports==0&&dev.DcnVsyncReadFailed==1);
 reset();event_pending=vector_pending=1;fail_write=1;Bc250DpcRoutine(&dev);CHECK(acks==0&&reports==0&&dev.DcnVsyncAckFailed==1);
 reset();event_pending=1;Bc250DpcRoutine(&dev);CHECK(acks==0&&dev.DcnVsyncDpcPolls==0);
 puts("PASS: coalesced vector, repeated DPC, ISR idempotence, disabled, sync/read/write faults, no-vector");return 0;
}
