/* BD-188: ConnectorResumeDisplay (connector.c), compiled unchanged against host fakes. A resume that cannot restore
 * the same sink must not fail the D0: it replays no timing, takes the link out of its suspended state without a
 * hardware access, and leaves the sink to the worker (Pending), so that the engines resume behind a black output. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
typedef int NTSTATUS,BOOLEAN;
typedef long LONG;
typedef unsigned long ULONG;
typedef unsigned char UCHAR,KIRQL;
typedef unsigned long long SIZE_T;
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS 0
#define STATUS_INVALID_DEVICE_STATE -1
#define STATUS_INSUFFICIENT_RESOURCES -2
#define STATUS_RETRY -3
#define STATUS_IO_DEVICE_ERROR -4
#define STATUS_DEVICE_BUSY -5
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_DP_MAX_PIXEL_KHZ 1080000ul
enum {BC250_SINK_NONE,BC250_SINK_DP,BC250_SINK_HDMI};
typedef struct {ULONG type,max_tmds_khz;} BC250_ADAPTER;
typedef struct {ULONG Kind,EdidLength,DownstreamRead;BC250_ADAPTER Adapter;UCHAR Dpcd[16],DownstreamPort[4],Edid[256];int Plans;} BC250_SINK;
typedef struct {volatile LONG References;ULONG Generation;BC250_SINK Sink;} BC250_CONNECTOR_SNAPSHOT;
typedef struct {ULONG StalledUs;} BC250_DISCOVERY_IO;
typedef struct {volatile LONG Suspended,Running,Pending,Restoring,Generation,Enabled;ULONG OperationGeneration;int Lock;
    BC250_CONNECTOR_SNAPSHOT* Snapshot;BC250_DISCOVERY_IO DiscoveryIo;} BC250_CONNECTOR;
typedef struct {BC250_CONNECTOR Connector;volatile LONG RetainedPowerPhase;BOOLEAN ModeActive;} BC250_DEVICE;
typedef struct {BC250_DEVICE* Device;int (*Current)(void*);} BC250_SINK_GUARD;
static unsigned checks,failures;
#define CHECK(x) do{checks++;if(!(x)){failures++;printf("FAIL line %u: %s\n",__LINE__,#x);}}while(0)
#define GuardLog(...) ((void)0)
static BC250_DEVICE device;static BC250_CONNECTOR_SNAPSHOT oldSnap;static BC250_SINK found;
static int allocFail,discoverStatus,linkStatus,publishFail,locks,linkResumes,linkChanged,published,allocated,released;
static int DiscoveryCurrent(void*c){(void)c;return 1;}
static LONG ReadAtomic(volatile LONG*p){return *p;}
static LONG InterlockedIncrement(volatile LONG*p){return ++*p;}
static LONG InterlockedExchange(volatile LONG*p,LONG v){LONG o=*p;*p=v;return o;}
static void KeAcquireSpinLock(int*l,KIRQL*i){(void)l;*i=0;locks++;}
static void KeReleaseSpinLock(int*l,KIRQL i){(void)l;(void)i;locks--;}
static void ConnectorOperationAcquire(BC250_DEVICE*d){(void)d;locks++;}
static void ConnectorOperationRelease(BC250_DEVICE*d){(void)d;locks--;}
static BOOLEAN ConnectorIsEnabled(BC250_DEVICE*d){return d->Connector.Enabled!=0;}
static SIZE_T RtlCompareMemory(const void*a,const void*b,SIZE_T n){SIZE_T i=0;const UCHAR*x=a,*y=b;while(i<n&&x[i]==y[i])i++;return i;}
static BC250_CONNECTOR_SNAPSHOT* AllocateSnapshot(ULONG g)
{BC250_CONNECTOR_SNAPSHOT*s;if(allocFail)return NULL;s=calloc(1,sizeof(*s));s->References=1;s->Generation=g;allocated++;return s;}
static void ConnectorSnapshotRelease(BC250_CONNECTOR_SNAPSHOT*s)
{if(!s)return;CHECK(s->References>0);if(--s->References==0 && s!=&oldSnap){free(s);released++;}}
static NTSTATUS Bc250SinkDiscover(BC250_DISCOVERY_IO*io,BC250_SINK_GUARD*g,ULONG a,ULONG b,ULONG c,ULONG d,BC250_SINK*out)
{(void)io;(void)a;(void)b;(void)c;(void)d;CHECK(g->Device==&device&&locks==1);*out=found;return discoverStatus;}
static NTSTATUS ConnectorLinkResume(BC250_DEVICE*d,ULONG g){(void)d;CHECK(g==d->Connector.OperationGeneration);linkResumes++;return linkStatus;}
static void ConnectorLinkResumeChanged(BC250_DEVICE*d){(void)d;CHECK(locks==1);linkChanged++;}
static BOOLEAN Publish(BC250_DEVICE*d,BC250_CONNECTOR_SNAPSHOT*s)
{if(publishFail)return FALSE;CHECK(s->Sink.Plans==7);ConnectorSnapshotRelease(d->Connector.Snapshot);d->Connector.Snapshot=s;published++;return TRUE;}
/* ACTUAL_SOURCE */
static void reset(void)
{
    if(device.Connector.Snapshot&&device.Connector.Snapshot!=&oldSnap)ConnectorSnapshotRelease(device.Connector.Snapshot);
    memset(&device,0,sizeof(device));memset(&oldSnap,0,sizeof(oldSnap));
    device.Connector.Enabled=1;device.Connector.Suspended=1;device.RetainedPowerPhase=3;device.ModeActive=TRUE;
    device.Connector.Generation=14;
    oldSnap.References=1;oldSnap.Sink.Kind=BC250_SINK_DP;oldSnap.Sink.EdidLength=128;oldSnap.Sink.Edid[0]=0x00;
    oldSnap.Sink.Edid[1]=0xFF;oldSnap.Sink.Dpcd[0]=0x12;oldSnap.Sink.Plans=7;
    device.Connector.Snapshot=&oldSnap;
    found=oldSnap.Sink;found.Plans=0;       /* a fresh reparse carries no admitted plans */
    allocFail=publishFail=0;discoverStatus=linkStatus=STATUS_SUCCESS;
    locks=linkResumes=linkChanged=published=allocated=released=0;
}
static void restored(void)
{
    CHECK(!device.Connector.Pending && device.ModeActive && !linkChanged && linkResumes==1 && published==1);
}
static void foundAgain(void)
{
    CHECK(device.Connector.Pending==1 && !device.ModeActive && linkChanged==1 && !published);
}
static void common(void)
{
    CHECK(!locks && !device.Connector.Restoring && !device.Connector.Running && device.Connector.Generation==15);
}
int main(void)
{
    /* The same sink: the link resumes and the admitted plans are kept. */
    reset();CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);restored();common();
    CHECK(device.Connector.Snapshot!=&oldSnap && device.Connector.Snapshot->Sink.Plans==7);
    /* A changed sink (lab 2026-10-20, "same 0"): no replay, the link leaves its suspended state. */
    reset();found.Edid[1]=0xFE;CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);foundAgain();common();
    CHECK(!linkResumes);
    /* An unreadable sink. */
    reset();discoverStatus=STATUS_IO_DEVICE_ERROR;CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);foundAgain();common();
    /* BD-188: the same sink whose link resume is refused (a failed training, a busy primary) no longer fails the D0. */
    reset();linkStatus=STATUS_IO_DEVICE_ERROR;CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);
    CHECK(linkResumes==1);foundAgain();common();
    reset();linkStatus=STATUS_DEVICE_BUSY;CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);foundAgain();common();
    /* A stale publish after a resumed link: the worker finds the sink again. */
    reset();publishFail=1;CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);
    CHECK(device.Connector.Pending==1 && !device.ModeActive && linkResumes==1 && device.Connector.Snapshot==&oldSnap);common();
    CHECK(allocated==1 && released==1);
    /* No snapshot memory: nothing discovered, nothing replayed. */
    reset();allocFail=1;CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);foundAgain();common();CHECK(!linkResumes);
    /* No earlier snapshot. */
    reset();device.Connector.Snapshot=NULL;CHECK(ConnectorResumeDisplay(&device)==STATUS_SUCCESS);foundAgain();common();
    /* Preconditions still refuse before any lock or state change. */
    reset();device.RetainedPowerPhase=0;CHECK(ConnectorResumeDisplay(&device)==STATUS_INVALID_DEVICE_STATE);
    CHECK(!locks && !linkChanged && !device.Connector.Pending && device.Connector.Generation==14);
    reset();device.Connector.Suspended=0;CHECK(ConnectorResumeDisplay(&device)==STATUS_INVALID_DEVICE_STATE);
    reset();device.Connector.Enabled=0;CHECK(ConnectorResumeDisplay(&device)==STATUS_INVALID_DEVICE_STATE);
    reset();
    printf("Connector resume: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
