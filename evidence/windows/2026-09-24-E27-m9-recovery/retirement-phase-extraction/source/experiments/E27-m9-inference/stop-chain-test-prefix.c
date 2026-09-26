#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#define BC250_GFX_STAGE_CP 6
#define BC250_GFX_STAGE_INTERRUPTS 8
#define TRUE 1
#define FALSE 0
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_GFX_TAG 1
#define BC250_PSP_TAG 2
#define BC250_GART_TAG 3
#define BC250_PSP_PAGES_LENGTH 4096
#define GuardLog(...) ((void)0)
#define GfxTraceRlcState(...) ((void)0)
typedef int BOOLEAN;
typedef long NTSTATUS;
typedef struct {NTSTATUS Fault;unsigned WriteCount;int Plan,TraceRlcRetirement;} BC250_SEQUENCE;
struct amdgpu_device {void* backend;};
typedef struct {BC250_SEQUENCE Sequence;int StagesDone,SetUp,Failed,CpStepDone;} BC250_GFX;
typedef struct {BC250_SEQUENCE Sequence;int RingUp,TmrUp;void* Pages;} BC250_PSP;
typedef struct {BC250_SEQUENCE Sequence;int Enabled;void* DummyPage;} BC250_GART;
typedef struct {void* Gfx;void* Psp;void* Gart;int GartLock,GfxPagingLock;
 int GfxStopQuiet,PspStopQuiet,IhQuiet,GpuStopUnconfirmed;} BC250_DEVICE;
static struct amdgpu_device modelAdev;
static int locks,regions,lookupFail,finiQuiet,undoFail,sequenceFail,unloadFail,restoreFail;
static int unloadCalls,restoreCalls,dummyFrees,viewUnmaps,poolFrees[4],memoryRelease,checks,failures;
static void KeEnterCriticalRegion(void){regions++;}
static void KeLeaveCriticalRegion(void){regions--;}
static void ExAcquirePushLockExclusive(int*p){(void)p;locks++;}
static void ExReleasePushLockExclusive(int*p){(void)p;locks--;}
static void ExAcquireFastMutex(int*p){(void)p;locks++;}
static void ExReleaseFastMutex(int*p){(void)p;locks--;}
static void GfxAccessClose(BC250_DEVICE*d){(void)d;}
static NTSTATUS GartDevice(BC250_DEVICE*d,struct amdgpu_device**a,BOOLEAN*e)
{(void)d;*a=&modelAdev;*e=1;return lookupFail?-1:0;}
static void SequenceBegin(BC250_SEQUENCE*s,BC250_DEVICE*d,int plan,void*p,unsigned n)
{(void)s;(void)d;(void)plan;(void)p;(void)n;}
static void GpuMemBeginSequence(BC250_DEVICE*d,void*p,unsigned n){(void)d;(void)p;(void)n;}
static int sequenceRelease,sequenceReleaseCalls,teardownCalls;
static long bc250_sdma_hw_fini(struct amdgpu_device*a){(void)a;return 0;}
static long bc250_gfx_hw_fini(struct amdgpu_device*a)
{((BC250_SEQUENCE*)a->backend)->Fault=sequenceFail?-1:0;return undoFail?-1:0;}
static int bc250_nbio_enable_doorbell_selfring_aperture(struct amdgpu_device*a,int enabled)
{(void)a;(void)enabled;return 0;}
static void GrbmSelectDefault(BC250_DEVICE*d){(void)d;}
static int EnginesHalted(BC250_DEVICE*d){(void)d;return finiQuiet;}
static void TearDown(BC250_GFX*g,struct amdgpu_device*a){(void)a;g->SetUp=0;teardownCalls++;}
static void GpuMemRelease(BC250_DEVICE*d,BC250_SEQUENCE*s,int quiet)
{(void)d;(void)s;sequenceRelease=quiet;sequenceReleaseCalls++;}
static void GpuMemStop(BC250_DEVICE*d,int quiet){(void)d;memoryRelease=quiet;}
static NTSTATUS Unload(BC250_PSP*p,long*r)
{unloadCalls++;*r=unloadFail?-1:0;if(!unloadFail)p->RingUp=p->TmrUp=0;return *r;}
static NTSTATUS WriteSnapshotBack(BC250_GART*g){(void)g;restoreCalls++;return restoreFail?-1:0;}
static void MmFreeContiguousMemory(void*p){(void)p;dummyFrees++;}
static void MmUnmapIoSpace(void*p,unsigned n){(void)p;(void)n;viewUnmaps++;}
static void ExFreePoolWithTag(void*p,unsigned tag){(void)p;poolFrees[tag]++;}
static void check(int ok,const char*msg,unsigned mask)
{checks++;if(!ok){failures++;if(failures<20)printf("FAIL mask=%u %s\n",mask,msg);}}
