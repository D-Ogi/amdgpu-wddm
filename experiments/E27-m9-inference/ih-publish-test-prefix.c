#include <stdio.h>
#include <string.h>
typedef int BOOLEAN;
typedef long NTSTATUS;
typedef long LONG;
typedef unsigned long ULONG;
typedef int KIRQL;
typedef void* PVOID;
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
#define STATUS_SUCCESS 0L
#define STATUS_DEVICE_NOT_READY (-1L)
#define STATUS_IO_DEVICE_ERROR (-2L)
#define RtlZeroMemory(p,n) memset(p,0,n)
typedef struct {ULONG Offset,Value;} BC250_SEQUENCE_WRITE;
typedef struct {
 const char*Name;void*Read;void*Write;int Dpc;
 NTSTATUS Fault;ULONG FaultOffset,WriteCount,MaxWrites;BC250_SEQUENCE_WRITE*Writes;
} BC250_SEQUENCE;
struct amdgpu_device {void*backend;int ringPrepared;};
typedef struct {
 LONG Active,OurInterrupts;ULONG Rptr;
 BC250_SEQUENCE Sequence,DpcSequence,EnableSequence;
 BC250_SEQUENCE_WRITE EnableWrite;
 struct amdgpu_device*DpcAdev;
 int StatsLock;int Stats;
} BC250_IH;
typedef struct {
 BC250_IH*Ih;
 struct {PVOID DeviceHandle;NTSTATUS (*DxgkCbSynchronizeExecution)(PVOID,BOOLEAN (*)(PVOID),PVOID,ULONG,BOOLEAN*);
 BOOLEAN (*DxgkCbQueueDpc)(PVOID);} Dxgk;
} BC250_DEVICE;
static int checks,failures,locked,atDirql,mode,queued,enableCalls,shimResult;
static NTSTATUS ioFault;
static BC250_IH modelIh;
static struct amdgpu_device adev,dpc;
static BC250_DEVICE dev;
static BC250_SEQUENCE_WRITE caller[4];
static void*DpcRead=(void*)1;
static void*DpcWrite=(void*)2;
static void check(int ok,const char*n){checks++;if(!ok){failures++;printf("FAIL %s\n",n);}}
static LONG InterlockedExchange(LONG*p,LONG v){LONG old=*p;*p=v;return old;}
static LONG InterlockedIncrement(LONG*p){return ++*p;}
static void KeAcquireSpinLock(int*p,KIRQL*i){(void)p;check(!atDirql,"stats outside DIRQL");locked=1;*i=0;}
static void KeReleaseSpinLock(int*p,KIRQL i){(void)p;(void)i;locked=0;}
static void SequenceBegin(BC250_SEQUENCE*s,BC250_DEVICE*d,BOOLEAN plan,BC250_SEQUENCE_WRITE*w,ULONG n)
{(void)d;(void)plan;check(!atDirql,"sequence setup outside callback");s->Fault=0;s->FaultOffset=0;s->WriteCount=0;s->Writes=w;s->MaxWrites=n;}
static int bc250_ih_hw_enable(struct amdgpu_device*a){
 BC250_SEQUENCE*s=(BC250_SEQUENCE*)a->backend;enableCalls++;
 check(atDirql&&!locked,"enable synchronized, no stats lock");
 check(modelIh.Active==1&&modelIh.DpcAdev->ringPrepared&&modelIh.DpcAdev->backend==&modelIh.DpcSequence&&modelIh.Rptr==0&&modelIh.Stats==0,
       "consumer ready at hardware enable");
 check(s==&modelIh.EnableSequence&&s->Dpc&&s->Writes==&modelIh.EnableWrite&&s->MaxWrites==1,"owned nonpaged callback report");
 check(s->Read==DpcRead&&s->Write==DpcWrite,"restricted MMIO backend");
 check(caller[1].Value==99,"caller report untouched at DIRQL");
 if(ioFault){s->Fault=ioFault;s->FaultOffset=88;return 0;}
 s->Writes[0].Offset=88;s->Writes[0].Value=123;s->WriteCount=1;
 return shimResult;
}
