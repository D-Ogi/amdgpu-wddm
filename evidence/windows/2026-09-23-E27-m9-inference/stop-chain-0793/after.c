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
typedef int BOOLEAN;
typedef long NTSTATUS;
typedef struct {NTSTATUS Fault;unsigned WriteCount;} BC250_SEQUENCE;
struct amdgpu_device {void* backend;};
typedef struct {BC250_SEQUENCE Sequence;int StagesDone,SetUp,Failed;} BC250_GFX;
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
static int sequenceRelease;
static long bc250_sdma_hw_fini(struct amdgpu_device*a){(void)a;return 0;}
static long bc250_gfx_hw_fini(struct amdgpu_device*a)
{((BC250_SEQUENCE*)a->backend)->Fault=sequenceFail?-1:0;return undoFail?-1:0;}
static int bc250_nbio_enable_doorbell_selfring_aperture(struct amdgpu_device*a,int enabled)
{(void)a;(void)enabled;return 0;}
static void GrbmSelectDefault(BC250_DEVICE*d){(void)d;}
static int EnginesHalted(BC250_DEVICE*d){(void)d;return finiQuiet;}
static void TearDown(BC250_GFX*g,struct amdgpu_device*a){(void)a;g->SetUp=0;}
static void GpuMemRelease(BC250_DEVICE*d,BC250_SEQUENCE*s,int quiet)
{(void)d;(void)s;sequenceRelease=quiet;}
static void GpuMemStop(BC250_DEVICE*d,int quiet){(void)d;memoryRelease=quiet;}
static NTSTATUS Unload(BC250_PSP*p,long*r)
{unloadCalls++;*r=unloadFail?-1:0;if(!unloadFail)p->RingUp=p->TmrUp=0;return *r;}
static NTSTATUS WriteSnapshotBack(BC250_GART*g){(void)g;restoreCalls++;return restoreFail?-1:0;}
static void MmFreeContiguousMemory(void*p){(void)p;dummyFrees++;}
static void MmUnmapIoSpace(void*p,unsigned n){(void)p;(void)n;viewUnmaps++;}
static void ExFreePoolWithTag(void*p,unsigned tag){(void)p;poolFrees[tag]++;}
static void check(int ok,const char*msg,unsigned mask)
{checks++;if(!ok){failures++;if(failures<20)printf("FAIL mask=%u %s\n",mask,msg);}}
static BOOLEAN Fini(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev, _Out_ long* Undo)
{
    BOOLEAN quiet;
    long sdma = 0;

    *Undo = 0;
    GfxAccessClose(Device);

    if (Gfx->StagesDone >= BC250_GFX_STAGE_CP)
    {
        sdma = bc250_sdma_hw_fini(Adev);
        // Disables the three fault sources of late_init. The end-of-pipe enables of stage 8 stay set: harmless with the
        // queues unmapped and both CPs halted, but a second run to stage 8 meets them enabled.
        *Undo = bc250_gfx_hw_fini(Adev);
        if (*Undo == 0) *Undo = sdma;
        // nv_common_hw_fini(): the self-ring aperture goes last.
        if (Gfx->StagesDone >= BC250_GFX_STAGE_INTERRUPTS) (void)bc250_nbio_enable_doorbell_selfring_aperture(Adev, false);
        // The undo selects queues as well (the KIQ's dequeue), and a sequence that faults in between writes nothing any more.
        GrbmSelectDefault(Device);
    }
    // Before the CP stage no engine was released by us and no queue was mapped: nothing of ours is in use. (The PSP
    // releases SDMA by itself, facts M35, but an SDMA engine without a ring has no address of ours.)
    quiet = (Gfx->StagesDone < BC250_GFX_STAGE_CP) || EnginesHalted(Device);
    TearDown(Gfx, Adev);
    // An undo that failed leaves an engine that may still hold an address of ours: its pages stay (they go back with a
    // later undo that succeeds, or never), but the state is reset all the same, because the way out of this is the next
    // bring-up's recovery branch (bc250_kiq_init_register), not a second undo on a halted MEC.
    Device->GfxStopQuiet=quiet && *Undo==0 && NT_SUCCESS(Gfx->Sequence.Fault);
    GpuMemRelease(Device, &Gfx->Sequence, Device->GfxStopQuiet);
    if (quiet) { Gfx->StagesDone = 0; Gfx->Failed = FALSE; }
    return quiet;
}
void GfxStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled, quiet = Device->GfxStopQuiet;
    long undo = 0;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    GfxAccessClose(Device);
    gfx = (BC250_GFX*)Device->Gfx;
    Device->Gfx = NULL;
    if (gfx != NULL && (gfx->StagesDone != 0 || gfx->SetUp))
    {
        quiet = FALSE;
        if (NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled)))
        {
            void* previousBackend = adev->backend;

            adev->backend = &gfx->Sequence;
            SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
            GpuMemBeginSequence(Device, NULL, 0);
            quiet = Fini(Device, gfx, adev, &undo) && undo == 0 && NT_SUCCESS(gfx->Sequence.Fault);
            adev->backend = previousBackend;
        }
    }
    Device->GfxStopQuiet=quiet;
    if (!quiet || !Device->IhQuiet) Device->GpuStopUnconfirmed=TRUE;
    GpuMemStop(Device, quiet && Device->IhQuiet);       // ih.c stopped before us and said whether its ring is off
    ExReleaseFastMutex(&Device->GartLock);
    if (gfx != NULL) ExFreePoolWithTag(gfx, BC250_GFX_TAG);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
}
void PspStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_PSP* psp;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled;
    long result = 0;

    ExAcquireFastMutex(&Device->GartLock);
    psp = (BC250_PSP*)Device->Psp;
    Device->Psp = NULL;
    if (Device->GfxStopQuiet && psp != NULL && (psp->RingUp || psp->TmrUp) && NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled)))
    {
        void* previousBackend = adev->backend;

        adev->backend = &psp->Sequence;
        SequenceBegin(&psp->Sequence, Device, FALSE, NULL, 0);
        Unload(psp, &result);
        adev->backend = previousBackend;
    }
    ExReleaseFastMutex(&Device->GartLock);
    if (psp == NULL) return;
    Device->PspStopQuiet=!(psp->RingUp || psp->TmrUp);
    if (!Device->PspStopQuiet)
    {
        // Keep firmware state and its CPU views when a consumer could still run,
        // or when PSP teardown itself failed. Do not dismantle GART afterwards.
        Device->GpuStopUnconfirmed=TRUE;
        GuardLog("psp: stop unconfirmed (gfx quiet %u, ring %u, TMR %u), retaining state",
                 Device->GfxStopQuiet,psp->RingUp,psp->TmrUp);
        return;
    }
    if (psp->Pages != NULL) MmUnmapIoSpace(psp->Pages, BC250_PSP_PAGES_LENGTH);
    ExFreePoolWithTag(psp, BC250_PSP_TAG);
}
void GartStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GART* gart;

    ExAcquireFastMutex(&Device->GartLock);
    gart = (BC250_GART*)Device->Gart;
    Device->Gart = NULL;
    ExReleaseFastMutex(&Device->GartLock);      // nobody can find the context any more; the rest needs no lock
    if (gart == NULL) return;
    if (!Device->GfxStopQuiet || !Device->IhQuiet || !Device->PspStopQuiet) {
        Device->GpuStopUnconfirmed=TRUE;
        GuardLog("gart: consumer stop unconfirmed, retaining translation state and dummy page");
        return;
    }
    if (gart->Enabled)
    {
        SequenceBegin(&gart->Sequence, Device, FALSE, NULL, 0);
        if (!NT_SUCCESS(WriteSnapshotBack(gart)))
        {
            Device->GpuStopUnconfirmed=TRUE;
            GuardLog("gart: restore at stop failed, keeping the dummy page allocated");
            return;
        }
        GuardLog("gart: firmware state restored at stop (%u writes)", gart->Sequence.WriteCount);
    }
    MmFreeContiguousMemory(gart->DummyPage);
    ExFreePoolWithTag(gart, BC250_GART_TAG);
}

int main(void)
{
 unsigned mask;
 for(mask=0;mask<128;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};BC250_PSP psp={0};BC250_GART gart={0};
  int expectedGfx,expectedPsp,expectedRestore,expectedFree,oldUnloads,oldRestores,oldFrees;
  lookupFail=(mask>>0)&1;finiQuiet=!((mask>>1)&1);undoFail=(mask>>2)&1;
  sequenceFail=(mask>>3)&1;unloadFail=(mask>>4)&1;restoreFail=(mask>>5)&1;
  d.IhQuiet=!((mask>>6)&1);
  expectedGfx=!lookupFail&&finiQuiet&&!undoFail&&!sequenceFail;
  expectedPsp=expectedGfx&&!unloadFail;
  expectedRestore=expectedGfx&&expectedPsp&&d.IhQuiet;
  expectedFree=expectedRestore&&!restoreFail;
  locks=regions=unloadCalls=restoreCalls=dummyFrees=viewUnmaps=0;
  memset(poolFrees,0,sizeof(poolFrees));memoryRelease=-1;sequenceRelease=0;
  gfx.StagesDone=8;gfx.SetUp=1;psp.RingUp=psp.TmrUp=1;psp.Pages=&psp;
  gart.Enabled=1;gart.DummyPage=&gart;
  d.Gfx=&gfx;d.Psp=&psp;d.Gart=&gart;d.GfxStopQuiet=d.PspStopQuiet=1;
  GfxStop(&d);PspStop(&d);GartStop(&d);
  check(d.GfxStopQuiet==expectedGfx,"GFX result includes halt, undo and sequence fault",mask);
  check(sequenceRelease==expectedGfx,"Fini releases sequence pages only after clean halt and undo",mask);
  check(memoryRelease==(expectedGfx&&d.IhQuiet),"GTT release needs engine and IH retirement",mask);
  check(unloadCalls==expectedGfx,"PSP unload never follows unconfirmed GFX stop",mask);
  check(d.PspStopQuiet==expectedPsp,"PSP verdict preserves failed ring/TMR teardown",mask);
  check(restoreCalls==expectedRestore,"GART restore requires every consumer retired",mask);
  check(dummyFrees==expectedFree && poolFrees[3]==expectedFree,"dummy and GART owner survive failed restore or consumer",mask);
  check(viewUnmaps==expectedPsp && poolFrees[2]==expectedPsp,"PSP views and owner survive uncertain teardown",mask);
  check(poolFrees[1]==1 && !d.Gfx && !d.Psp && !d.Gart,"software owners detach once",mask);
  check(d.GpuStopUnconfirmed==!expectedFree,"uncertain stop latches device-object quarantine",mask);
  check(!locks&&!regions,"all stop paths balance locks and regions",mask);
  oldUnloads=unloadCalls;oldRestores=restoreCalls;oldFrees=dummyFrees;
  GfxStop(&d);PspStop(&d);GartStop(&d);
  check(d.GfxStopQuiet==expectedGfx && d.PspStopQuiet==expectedPsp && d.GpuStopUnconfirmed==!expectedFree,
        "second stop cannot erase failed-stop verdicts",mask);
  check(unloadCalls==oldUnloads && restoreCalls==oldRestores && dummyFrees==oldFrees &&
        poolFrees[1]==1 && !locks&&!regions,"second stop cannot repeat destruction or releases",mask);
 }
 for(mask=0;mask<8;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;
  int expected;
  finiQuiet=!((mask>>0)&1);undoFail=(mask>>1)&1;sequenceFail=(mask>>2)&1;lookupFail=0;
  expected=finiQuiet&&!undoFail&&!sequenceFail;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;d.IhQuiet=1;d.GfxStopQuiet=1;
  modelAdev.backend=&gfx.Sequence;
  (void)Fini(&d,&gfx,&modelAdev,&undo);
  check(d.GfxStopQuiet==expected && sequenceRelease==expected,"manual Fini preserves precise retirement verdict",mask);
  // A quiet-but-unsuccessful undo can clear StagesDone; Stop must still retain.
  if(finiQuiet) {
   GfxStop(&d);
   check(d.GfxStopQuiet==expected && memoryRelease==expected &&
         d.GpuStopUnconfirmed==!expected,"PnP stop retains earlier manual Fini failure even with cleared stages",mask);
  }
 }
 printf("%d checks, %d failures\n",checks,failures);
 return failures?1:0;
}
