#include <stdio.h>
#include <string.h>
#ifndef _Inout_
#define _Inout_
#define _In_
#endif
typedef int BOOLEAN, NTSTATUS;
#define TRUE 1
#define FALSE 0
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS 0
#define STATUS_DEVICE_NOT_READY -1
#define STATUS_INVALID_DEVICE_STATE -2
#define STATUS_IO_DEVICE_ERROR -3
#define NT_SUCCESS(x) ((x)>=0)
typedef struct {NTSTATUS Fault;} SEQUENCE;
typedef struct {int SetUp,Enabled,Active,PowerSuspended;SEQUENCE Sequence;} BC250_IH;
struct amdgpu_device {void*backend;struct {struct {void*ring;volatile unsigned*wptr_cpu,*rptr_cpu;unsigned rptr;}ih;}irq;};
typedef struct {BC250_IH*Ih;int IhQuiet,GpuStopUnconfirmed,GartLock,MmioIhEnabled;} BC250_DEVICE;
static unsigned checks,failures,joins,halts,prepares,publishes,frees,releases,barriers;
static int irql,locked,closeOk=1,disabled=1,modelGartEnabled=1,prepareResult,publishResult;
static struct amdgpu_device modelAdev;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL %u: %s\n",__LINE__,#x);}}while(0)
static int KeGetCurrentIrql(void){return irql;}
static void ExAcquireFastMutex(int*l){(void)l;CHECK(!locked);locked=1;}
static void ExReleaseFastMutex(int*l){(void)l;CHECK(locked);locked=0;}
static void KeMemoryBarrier(void){barriers++;}
static NTSTATUS GartDevice(BC250_DEVICE*d,struct amdgpu_device**a,BOOLEAN*g){(void)d;CHECK(locked);*a=&modelAdev;*g=modelGartEnabled;return 0;}
static void SequenceBegin(SEQUENCE*s,BC250_DEVICE*d,int plan,void*w,int n){(void)d;CHECK(!plan&&!w&&!n&&locked);s->Fault=0;}
static BOOLEAN IhCloseInterruptAdmission(BC250_DEVICE*d,BC250_IH*i){(void)d;joins++;i->Active=0;return closeOk;}
static void bc250_ih_hw_fini(struct amdgpu_device*a){(void)a;CHECK(joins>halts);halts++;}
static BOOLEAN RingReadsDisabled(BC250_DEVICE*d){(void)d;CHECK(halts);return disabled;}
static void bc250_ih_teardown(struct amdgpu_device*a){a->irq.ih.ring=NULL;frees++;}
static void GpuMemRelease(BC250_DEVICE*d,SEQUENCE*s,int retain){(void)d;(void)s;CHECK(retain);releases++;}
static int bc250_ih_hw_prepare(struct amdgpu_device*a){
 CHECK(joins && halts && a->irq.ih.ring);
 prepares++;return prepareResult;
}
static NTSTATUS IhPublishAndEnable(BC250_DEVICE*d,BC250_IH*i,struct amdgpu_device*a,long*r){
 (void)d;CHECK(prepares && barriers && a->irq.ih.ring);CHECK(!*a->irq.ih.wptr_cpu && !*a->irq.ih.rptr_cpu && !a->irq.ih.rptr);publishes++;*r=0;if(!publishResult)i->Active=1;return publishResult;
}
/* ACTUAL_SOURCE */
static void setup(BC250_DEVICE*d,BC250_IH*i,unsigned*wb,void*ring){
 memset(d,0,sizeof(*d));memset(i,0,sizeof(*i));memset(&modelAdev,0,sizeof(modelAdev));
 d->Ih=i;d->MmioIhEnabled=1;i->SetUp=i->Enabled=i->Active=1;
 modelAdev.backend=d;modelAdev.irq.ih.ring=ring;modelAdev.irq.ih.wptr_cpu=wb;modelAdev.irq.ih.rptr_cpu=wb+1;
 wb[0]=64;wb[1]=32;modelAdev.irq.ih.rptr=32;
 joins=halts=prepares=publishes=frees=releases=barriers=0;irql=locked=prepareResult=publishResult=0;
 closeOk=disabled=modelGartEnabled=1;
}
int main(void){
 BC250_DEVICE d;BC250_IH i;unsigned wb[2],ring[8];
 setup(&d,&i,wb,ring);
 CHECK(IhSetPowerRetained(&d,FALSE)==0 && i.PowerSuspended && !i.Active && !i.Enabled && i.SetUp && d.IhQuiet);
 CHECK(!frees&&!releases&&modelAdev.backend==&d&&modelAdev.irq.ih.ring==ring&&modelAdev.irq.ih.wptr_cpu==wb&&wb[0]==64);
 CHECK(IhSetPowerRetained(&d,FALSE)==0 && halts==1);
 // Retained system backing contains old data after simulated sleep.
 wb[0]=0x80000100u;wb[1]=0xdeadbeefu;modelAdev.irq.ih.rptr=128;
 CHECK(IhSetPowerRetained(&d,TRUE)==0 && !i.PowerSuspended && i.SetUp && i.Enabled && i.Active && !d.IhQuiet);
 CHECK(!wb[0]&&!wb[1]&&!modelAdev.irq.ih.rptr&&!frees&&!releases&&modelAdev.backend==&d&&modelAdev.irq.ih.ring==ring);
 CHECK(IhSetPowerRetained(&d,TRUE)==STATUS_INVALID_DEVICE_STATE);
 // Destructive stop remains a separate deliberate operation.
 CHECK(Fini(&d,&i,&modelAdev) && frees==1&&releases==1&&!i.SetUp&&!i.Enabled&&!i.PowerSuspended);
 setup(&d,&i,wb,ring);disabled=0;
 CHECK(IhSetPowerRetained(&d,FALSE)==STATUS_IO_DEVICE_ERROR && d.GpuStopUnconfirmed && !d.IhQuiet && i.SetUp&&!frees&&!releases);
 setup(&d,&i,wb,ring);closeOk=0;
 CHECK(IhSetPowerRetained(&d,FALSE)==STATUS_IO_DEVICE_ERROR && !halts&&!frees&&i.SetUp);
 setup(&d,&i,wb,ring);CHECK(IhSetPowerRetained(&d,FALSE)==0);modelGartEnabled=0;
 CHECK(IhSetPowerRetained(&d,TRUE)==STATUS_DEVICE_NOT_READY && !prepares && i.PowerSuspended);
 modelGartEnabled=1;prepareResult=-1;
 CHECK(IhSetPowerRetained(&d,TRUE)==STATUS_IO_DEVICE_ERROR && i.PowerSuspended&&!i.Enabled&&!i.Active&&i.SetUp&&!frees&&!publishes);
 prepareResult=0;publishResult=-7;
 CHECK(IhSetPowerRetained(&d,TRUE)==-7 && i.PowerSuspended&&!i.Active&&i.SetUp&&!frees);
 publishResult=0;CHECK(IhSetPowerRetained(&d,TRUE)==0 && i.Active&&!i.PowerSuspended&&!frees);
 setup(&d,&i,wb,ring);i.Sequence.Fault=-8;
 CHECK(!IhHaltRetained(&d,&i,&modelAdev)&&d.GpuStopUnconfirmed&&!d.IhQuiet&&i.SetUp&&!frees);
 setup(&d,&i,wb,ring);irql=2;CHECK(IhSetPowerRetained(&d,FALSE)==STATUS_INVALID_DEVICE_STATE&&!joins);
 printf("Retained IH: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
