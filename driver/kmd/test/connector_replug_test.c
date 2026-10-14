/* Host-only admission/lifecycle tests of the actual lab replug request. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "bc250kmd_escape.h"
typedef unsigned char BOOLEAN;
typedef unsigned long ULONG;
typedef long LONG, NTSTATUS;
typedef unsigned char KIRQL;
#define FALSE 0
#define TRUE 1
#define PASSIVE_LEVEL 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xc00000a3L)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xc0000184L)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xc000000dL)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xc0000022L)
#define NT_SUCCESS(x) ((NTSTATUS)(x) >= 0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define RTL_NUMBER_OF(a) (sizeof(a)/sizeof((a)[0]))
#define DelayedWorkQueue 1
#define LabEscapeConnectorReplug 11
#pragma warning(push)
#pragma warning(disable:4201) // Same anonymous bitfield overlay as D3DDDI_ESCAPEFLAGS.
typedef union {
    ULONG Value;
    struct { ULONG HardwareAccess:1, Reserved0:2, NoAdapterSynchronization:1, Reserved1:28; };
} D3DDDI_ESCAPEFLAGS;
#pragma warning(pop)
typedef char REPLUG_SIZE_CHECK[(sizeof(BC250_ESCAPE_CONNECTOR_REPLUG_DATA)==64 && sizeof(BC250_ESCAPE)==64)?1:-1];
typedef struct {
    int Lock, Idle;
    volatile LONG Running, Enabled, Generation, Pending, RxPending, Suspended;
    LONG Queued;
    void* WorkItem;
} BC250_CONNECTOR;
typedef struct {
    BC250_CONNECTOR Connector;
    BOOLEAN FullWddm;
    /* The dispatcher fixture includes this helper fixture and uses these fields. */
    void* Mmio;
    BOOLEAN MmioWriteEnabled,VramEnabled,VramWriteEnabled,MmioGartEnabled,MmioPspEnabled;
    BOOLEAN MmioGfxEnabled,MmioIhEnabled,DcnWriteEnabled;
    int Smu;
    struct { ULONG Width,Height,Pitch,ColorFormat; } Post;
    volatile LONG RetainedPowerPhase;
    struct { BOOLEAN (*DxgkCbQueueDpc)(void*); void* DeviceHandle; } Dxgk;
} BC250_DEVICE;
static unsigned Checks, Failures, Queues, WorkQueues, Markers, LabChecks;
static KIRQL Irql;
static BOOLEAN Admin, LabEnabled, QueueResult;
static BC250_DEVICE* DeviceSeen;
#define CHECK(x) do { ++Checks; if (!(x)) { ++Failures; printf("FAIL line %u: %s\n",(unsigned)__LINE__,#x); } } while (0)
static KIRQL KeGetCurrentIrql(void) { return Irql; }
static LONG InterlockedCompareExchange(volatile LONG* p,LONG value,LONG compare)
{ LONG old=*p; if(old==compare)*p=value; return old; }
static LONG InterlockedIncrement(volatile LONG* p) { return ++*p; }
static LONG InterlockedExchange(volatile LONG* p,LONG value) { LONG old=*p;*p=value;return old; }
static void KeAcquireSpinLock(int* lock,KIRQL* old) { CHECK(!*lock);*old=Irql;*lock=1;Irql=2; }
static void KeReleaseSpinLock(int* lock,KIRQL old) { CHECK(*lock);*lock=0;Irql=old; }
static void KeClearEvent(int* event) { *event=0; }
static void ConnectorWorker(void* object,void* context) { (void)object;(void)context; }
static void IoQueueWorkItem(void* item,void (*worker)(void*,void*),int kind,void* context)
{
    CHECK(item==DeviceSeen->Connector.WorkItem && worker==ConnectorWorker);
    CHECK(kind==DelayedWorkQueue && context==DeviceSeen && DeviceSeen->Connector.Lock==1);
    ++WorkQueues;
}
static BOOLEAN Queue(void* context)
{
    BC250_DEVICE* d=context;
    CHECK(d==DeviceSeen && d->Connector.Lock==1 && Irql==2);
    CHECK(d->Connector.Pending==1);
    ++Queues; return QueueResult;
}
static BOOLEAN WaveCallerIsAdmin(void) { return Admin; }
static BOOLEAN LabDiagnosticsAllowed(void) { ++LabChecks;return LabEnabled; }
static void LabEscapeAdmitted(int family,const char* marker)
{ CHECK(family==LabEscapeConnectorReplug && !strcmp(marker,"lab:connector-replug"));++Markers; }
#define GuardLog(...) ((void)0)

/* ACTUAL_SOURCE */

static void Reset(BC250_DEVICE* d,BC250_ESCAPE_CONNECTOR_REPLUG_DATA* p)
{
    memset(d,0,sizeof(*d));memset(p,0,sizeof(*p));
    d->FullWddm=TRUE;d->Connector.Running=d->Connector.Enabled=1;
    d->Connector.Generation=19;d->Connector.WorkItem=d;d->Connector.Idle=1;
    d->Dxgk.DeviceHandle=d;d->Dxgk.DxgkCbQueueDpc=Queue;
    p->Magic=BC250_ESCAPE_MAGIC;p->Command=BC250_ESCAPE_CONNECTOR_REPLUG;
    p->AbiVersion=BC250_CONNECTOR_REPLUG_ABI;
    Admin=LabEnabled=QueueResult=TRUE;Irql=0;Queues=WorkQueues=Markers=LabChecks=0;DeviceSeen=d;
}
static void Refused(BC250_DEVICE* d,BC250_ESCAPE_CONNECTOR_REPLUG_DATA* p,ULONG flags,NTSTATUS want)
{
    ConnectorReplugEscape(d,p,flags);
    CHECK((NTSTATUS)p->NtStatus==want && p->Status!=BC250_ESCAPE_STATUS_DONE);
    CHECK(!Queues && !WorkQueues && !Markers && d->Connector.Generation==19 && !d->Connector.Pending);
    CHECK(!p->Generation && !p->DpcQueued);
}
int main(void)
{
    BC250_DEVICE d;BC250_ESCAPE_CONNECTOR_REPLUG_DATA p;ULONG generation;BOOLEAN queued;unsigned i;
    D3DDDI_ESCAPEFLAGS flags={0};flags.NoAdapterSynchronization=1;
    Reset(&d,&p);ConnectorReplugEscape(&d,&p,flags.Value);
    CHECK(p.Status==BC250_ESCAPE_STATUS_DONE && p.NtStatus==0 && p.Generation==20 && p.DpcQueued==1);
    CHECK(Queues==1 && !WorkQueues && Markers==1 && d.Connector.Pending==1 && !d.Connector.Queued);
    ConnectorDpc(&d);
    CHECK(WorkQueues==1 && d.Connector.Queued==1 && d.Connector.Idle==0);
    ConnectorDpc(&d);CHECK(WorkQueues==1); // One worker even with repeated DPCs.
    Reset(&d,&p);QueueResult=FALSE;ConnectorReplugEscape(&d,&p,flags.Value);
    CHECK(p.Status==BC250_ESCAPE_STATUS_DONE && p.Generation==20 && !p.DpcQueued && d.Connector.Pending==1);
    ConnectorDpc(&d);CHECK(WorkQueues==1); // An already queued DPC consumes the request.
    Reset(&d,&p);Admin=FALSE;Refused(&d,&p,flags.Value,STATUS_ACCESS_DENIED);CHECK(!LabChecks);
    Reset(&d,&p);LabEnabled=FALSE;Refused(&d,&p,flags.Value,STATUS_ACCESS_DENIED);
    for(i=0;i<32;++i) {
        if(i==flags.Value)continue;
        Reset(&d,&p);Refused(&d,&p,i,STATUS_INVALID_PARAMETER);CHECK(!LabChecks);
    }
    Reset(&d,&p);p.AbiVersion++;Refused(&d,&p,flags.Value,STATUS_INVALID_PARAMETER);
    for(i=0;i<8;++i) { Reset(&d,&p);p.Reserved[i]=1;Refused(&d,&p,flags.Value,STATUS_INVALID_PARAMETER); }
    Reset(&d,&p);Irql=2;Refused(&d,&p,flags.Value,STATUS_INVALID_DEVICE_STATE);CHECK(!LabChecks);
    Reset(&d,&p);d.Connector.Running=0;Refused(&d,&p,flags.Value,STATUS_DEVICE_NOT_READY);
    Reset(&d,&p);d.Connector.Enabled=0;Refused(&d,&p,flags.Value,STATUS_DEVICE_NOT_READY);
    Reset(&d,&p);d.Connector.Suspended=1;Refused(&d,&p,flags.Value,STATUS_DEVICE_NOT_READY);
    Reset(&d,&p);d.Connector.WorkItem=NULL;Refused(&d,&p,flags.Value,STATUS_DEVICE_NOT_READY);
    Reset(&d,&p);d.FullWddm=FALSE;Refused(&d,&p,flags.Value,STATUS_DEVICE_NOT_READY);
    Reset(&d,&p);d.Dxgk.DxgkCbQueueDpc=NULL;Refused(&d,&p,flags.Value,STATUS_DEVICE_NOT_READY);
    for(i=1;i<4;++i) { Reset(&d,&p);d.RetainedPowerPhase=(LONG)i;Refused(&d,&p,flags.Value,STATUS_DEVICE_NOT_READY); }
    Reset(&d,&p);Irql=2;generation=99;queued=TRUE;
    CHECK(ConnectorReplug(&d,&generation,&queued)==STATUS_INVALID_DEVICE_STATE);
    CHECK(!generation && !queued && !Queues);
    Reset(&d,&p);ConnectorReplugEscape(&d,&p,flags.Value);d.Connector.Running=0;ConnectorDpc(&d);
    CHECK(Queues==1 && WorkQueues==0); // Stop wins after the request, before its DPC.
    printf("connector replug: %u checks, %u failures\n",Checks,Failures);
    return Failures ? 1 : 0;
}
