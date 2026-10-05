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
typedef struct {BC250_SEQUENCE Sequence;int Enabled;void* DummyPage;struct amdgpu_device Adev;} BC250_GART;
typedef struct {void* Gfx;void* Psp;void* Gart;int GartLock,GfxPagingLock;
 int GfxStopQuiet,PspStopQuiet,IhQuiet,GpuStopUnconfirmed;
 int GfxStopPrepared,GartStopPrepared,GartStopQuiet;} BC250_DEVICE;
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
static int disableFail,disableCalls;
#define bc250_gmc_gart_disable(a) (disableCalls++, ((BC250_SEQUENCE*)(a)->backend)->Fault=disableFail?-1:0)
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
static BOOLEAN HaltEngines(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev, _Out_ long* Undo)
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
        GfxTraceRlcState(Device,"after-stop");
    }
    // Before the CP stage no engine was released by us and no queue was mapped: nothing of ours is in use. (The PSP
    // releases SDMA by itself, facts M35, but an SDMA engine without a ring has no address of ours.)
    quiet = (Gfx->StagesDone < BC250_GFX_STAGE_CP) || EnginesHalted(Device);
    return quiet;
}
static void ReleaseStoppedStorage(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx,
                                  _In_ struct amdgpu_device* Adev, BOOLEAN Quiet, long Undo)
{
    Gfx->Sequence.TraceRlcRetirement = !Gfx->Sequence.Plan && Gfx->StagesDone >= BC250_GFX_STAGE_CP;
    if (Gfx->Sequence.TraceRlcRetirement) GfxTraceRlcState(Device,"before-gfx-teardown");
    TearDown(Gfx, Adev);
    if (Gfx->Sequence.TraceRlcRetirement) GfxTraceRlcState(Device,"before-gfx-memory-release");
    Gfx->Sequence.TraceRlcRetirement = FALSE;
    // An undo that failed leaves an engine that may still hold an address of ours: its pages stay (they go back with a
    // later undo that succeeds, or never), but the state is reset all the same, because the way out of this is the next
    // bring-up's recovery branch (bc250_kiq_init_register), not a second undo on a halted MEC.
    Device->GfxStopQuiet=Quiet && Undo==0 && NT_SUCCESS(Gfx->Sequence.Fault);
    GpuMemRelease(Device, &Gfx->Sequence, Device->GfxStopQuiet);
    if (Gfx->StagesDone >= BC250_GFX_STAGE_CP) GfxTraceRlcState(Device,"after-gfx-memory-release");
    if (Quiet) { Gfx->StagesDone = 0; Gfx->CpStepDone = 0; Gfx->Failed = FALSE; }
}
#define HAS_HALT_PHASE 1
static BOOLEAN Fini(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_GFX* Gfx, _In_ struct amdgpu_device* Adev, _Out_ long* Undo)
{
    BOOLEAN quiet = HaltEngines(Device, Gfx, Adev, Undo);
    ReleaseStoppedStorage(Device, Gfx, Adev, quiet, *Undo);
    return quiet;
}
void GfxPrepareStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled, quiet;
    long undo = 0;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    GfxAccessClose(Device);
    if (!Device->GfxStopPrepared) {
        Device->GfxStopPrepared = TRUE;
        quiet = Device->GfxStopQuiet;
        gfx = (BC250_GFX*)Device->Gfx;
        if (gfx != NULL && (gfx->StagesDone != 0 || gfx->SetUp)) {
            quiet = FALSE;
            if (NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled))) {
                void* previousBackend = adev->backend;
                adev->backend = &gfx->Sequence;
                SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
                GpuMemBeginSequence(Device, NULL, 0);
                quiet = HaltEngines(Device, gfx, adev, &undo) && undo == 0 && NT_SUCCESS(gfx->Sequence.Fault);
                adev->backend = previousBackend;
            }
        }
        Device->GfxStopQuiet = quiet;
        if (!quiet || !Device->IhQuiet) Device->GpuStopUnconfirmed = TRUE;
    }
    ExReleaseFastMutex(&Device->GartLock);
    ExReleasePushLockExclusive(&Device->GfxPagingLock);
    KeLeaveCriticalRegion();
}
void GfxStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GFX* gfx;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled, quiet;

    KeEnterCriticalRegion();
    ExAcquirePushLockExclusive(&Device->GfxPagingLock);
    ExAcquireFastMutex(&Device->GartLock);
    GfxAccessClose(Device);
    quiet = Device->GfxStopPrepared && Device->GfxStopQuiet && Device->IhQuiet &&
            Device->PspStopQuiet && Device->GartStopPrepared && Device->GartStopQuiet;
    gfx = (BC250_GFX*)Device->Gfx;
    if (!quiet) {
        Device->GpuStopUnconfirmed = TRUE;
        GuardLog("gfx: retirement incomplete, retaining storage and owner");
        goto Done;
    }
    if (gfx != NULL && (gfx->StagesDone != 0 || gfx->SetUp)) {
        if (!NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled))) {
            Device->GfxStopQuiet = FALSE;
            Device->GpuStopUnconfirmed = TRUE;
            goto Done;
        }
        {
            void* previousBackend = adev->backend;
            adev->backend = &gfx->Sequence;
            SequenceBegin(&gfx->Sequence, Device, FALSE, NULL, 0);
            GpuMemBeginSequence(Device, NULL, 0);
            ReleaseStoppedStorage(Device, gfx, adev, TRUE, 0);
            adev->backend = previousBackend;
        }
    }
    if (!Device->GfxStopQuiet) Device->GpuStopUnconfirmed = TRUE;
    GpuMemStop(Device, Device->GfxStopQuiet);
    Device->Gfx = NULL;
    if (gfx != NULL) ExFreePoolWithTag(gfx, BC250_GFX_TAG);
Done:
    ExReleaseFastMutex(&Device->GartLock);
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
void GartPrepareStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GART* gart;
    ExAcquireFastMutex(&Device->GartLock);
    if (!Device->GartStopPrepared) {
        Device->GartStopPrepared = TRUE;
        Device->GartStopQuiet = FALSE;
        gart = (BC250_GART*)Device->Gart;
        if (Device->GfxStopPrepared && Device->GfxStopQuiet && Device->IhQuiet && Device->PspStopQuiet) {
            if (gart == NULL || !gart->Enabled) Device->GartStopQuiet = TRUE;
            else {
                void* previousBackend = gart->Adev.backend;
                gart->Adev.backend = &gart->Sequence;
                SequenceBegin(&gart->Sequence, Device, FALSE, NULL, 0);
                bc250_gmc_gart_disable(&gart->Adev);
                Device->GartStopQuiet = NT_SUCCESS(gart->Sequence.Fault);
                gart->Adev.backend = previousBackend;
                GuardLog("gart: hardware disable status 0x%08X",gart->Sequence.Fault);
                GfxTraceRlcState(Device,"after-gart-hardware-disable");
            }
        }
        if (!Device->GartStopQuiet) Device->GpuStopUnconfirmed = TRUE;
    }
    ExReleaseFastMutex(&Device->GartLock);
}
void GartStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_GART* gart;
    ExAcquireFastMutex(&Device->GartLock);
    gart = (BC250_GART*)Device->Gart;
    if (gart == NULL) { ExReleaseFastMutex(&Device->GartLock); return; }
    if (!Device->GartStopPrepared || !Device->GartStopQuiet || !Device->GfxStopQuiet ||
        !Device->IhQuiet || !Device->PspStopQuiet) {
        Device->GpuStopUnconfirmed = TRUE;
        GuardLog("gart: consumer/translation stop unconfirmed, retaining owner and dummy page");
        ExReleaseFastMutex(&Device->GartLock);
        return;
    }
    if (gart->Enabled) {
        SequenceBegin(&gart->Sequence, Device, FALSE, NULL, 0);
        if (!NT_SUCCESS(WriteSnapshotBack(gart))) {
            Device->GpuStopUnconfirmed = TRUE;
            Device->GartStopQuiet = FALSE; // latch failed restoration; repeated stop cannot retry it
            GuardLog("gart: restore at stop failed, retaining owner and dummy page");
            ExReleaseFastMutex(&Device->GartLock);
            return;
        }
        GuardLog("gart: firmware state restored at stop (%u writes)",gart->Sequence.WriteCount);
    }
    Device->Gart = NULL;
    ExReleaseFastMutex(&Device->GartLock);
    MmFreeContiguousMemory(gart->DummyPage);
    ExFreePoolWithTag(gart, BC250_GART_TAG);
}
#define HAS_PHASED_STOP 1

static void StopPhases(BC250_DEVICE*d)
{
#ifdef HAS_PHASED_STOP
 GfxPrepareStop(d);PspStop(d);GartPrepareStop(d);GfxStop(d);GartStop(d);
#else
 GfxStop(d);PspStop(d);GartStop(d);
#endif
}
int main(void)
{
 unsigned mask;
 for(mask=0;mask<256;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};BC250_PSP psp={0};BC250_GART gart={0};
  int expectedGfx,expectedPsp,expectedRestore,expectedFree,oldUnloads,oldRestores,oldFrees;
  lookupFail=(mask>>0)&1;finiQuiet=!((mask>>1)&1);undoFail=(mask>>2)&1;
  sequenceFail=(mask>>3)&1;unloadFail=(mask>>4)&1;restoreFail=(mask>>5)&1;
  d.IhQuiet=!((mask>>6)&1);disableFail=(mask>>7)&1;
  expectedGfx=!lookupFail&&finiQuiet&&!undoFail&&!sequenceFail;
  expectedPsp=expectedGfx&&!unloadFail;
  expectedRestore=expectedGfx&&expectedPsp&&d.IhQuiet;
#ifdef HAS_PHASED_STOP
  expectedRestore=expectedRestore&&!disableFail;
#endif
  expectedFree=expectedRestore&&!restoreFail;
  locks=regions=unloadCalls=restoreCalls=dummyFrees=viewUnmaps=0;
  memset(poolFrees,0,sizeof(poolFrees));memoryRelease=-1;sequenceRelease=0;disableCalls=0;
  gfx.StagesDone=8;gfx.SetUp=1;psp.RingUp=psp.TmrUp=1;psp.Pages=&psp;
  gart.Enabled=1;gart.DummyPage=&gart;
  d.Gfx=&gfx;d.Psp=&psp;d.Gart=&gart;d.GfxStopQuiet=d.PspStopQuiet=1;
  StopPhases(&d);
  check(d.GfxStopQuiet==expectedGfx,"GFX result includes halt, undo and sequence fault",mask);
#ifdef HAS_PHASED_STOP
  check(sequenceRelease==expectedRestore,"GFX release waits for PSP and GART hardware retirement",mask);
  check(memoryRelease==(expectedRestore?1:-1),"memory destruction only after all hardware phases",mask);
  check(disableCalls==(expectedGfx&&expectedPsp&&d.IhQuiet),"translation disable follows retired consumers",mask);
#else
  check(sequenceRelease==expectedGfx,"Fini releases sequence pages only after clean halt and undo",mask);
  check(memoryRelease==(expectedGfx&&d.IhQuiet),"GTT release needs engine and IH retirement",mask);
#endif
  check(unloadCalls==expectedGfx,"PSP unload never follows unconfirmed GFX stop",mask);
  check(d.PspStopQuiet==expectedPsp,"PSP verdict preserves failed ring/TMR teardown",mask);
  check(restoreCalls==expectedRestore,"GART restore requires every consumer retired",mask);
  check(dummyFrees==expectedFree && poolFrees[3]==expectedFree,"dummy and GART owner survive failed restore or consumer",mask);
  check(viewUnmaps==expectedPsp && poolFrees[2]==expectedPsp,"PSP views and owner survive uncertain teardown",mask);
#ifdef HAS_PHASED_STOP
  check(poolFrees[1]==expectedRestore && (!d.Gfx)==expectedRestore && !d.Psp && (!d.Gart)==expectedFree,
        "owners remain until corresponding retirement succeeds",mask);
#else
  check(poolFrees[1]==1 && !d.Gfx && !d.Psp && !d.Gart,"software owners detach once",mask);
#endif
  check(d.GpuStopUnconfirmed==!expectedFree,"uncertain stop latches device-object quarantine",mask);
  check(!locks&&!regions,"all stop paths balance locks and regions",mask);
  oldUnloads=unloadCalls;oldRestores=restoreCalls;oldFrees=dummyFrees;
  StopPhases(&d);
  check(d.GfxStopQuiet==expectedGfx && d.PspStopQuiet==expectedPsp && d.GpuStopUnconfirmed==!expectedFree,
        "second stop cannot erase failed-stop verdicts",mask);
  check(unloadCalls==oldUnloads && restoreCalls==oldRestores && dummyFrees==oldFrees &&
#ifdef HAS_PHASED_STOP
        poolFrees[1]==expectedRestore &&
#else
        poolFrees[1]==1 &&
#endif
        !locks&&!regions,"second stop cannot repeat destruction or releases",mask);
 }
 for(mask=0;mask<8;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;
  int expected;
  finiQuiet=!((mask>>0)&1);undoFail=(mask>>1)&1;sequenceFail=(mask>>2)&1;lookupFail=0;
  expected=finiQuiet&&!undoFail&&!sequenceFail;memoryRelease=-1;disableFail=0;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;d.IhQuiet=1;d.GfxStopQuiet=1;
  modelAdev.backend=&gfx.Sequence;
  (void)Fini(&d,&gfx,&modelAdev,&undo);
  check(d.GfxStopQuiet==expected && sequenceRelease==expected,"manual Fini preserves precise retirement verdict",mask);
  // A quiet-but-unsuccessful undo can clear StagesDone; Stop must still retain.
  if(finiQuiet) {
#ifdef HAS_PHASED_STOP
   d.PspStopQuiet=1;GfxPrepareStop(&d);GartPrepareStop(&d);
#endif
   GfxStop(&d);
   check(d.GfxStopQuiet==expected && memoryRelease==
#ifdef HAS_PHASED_STOP
         (expected?1:-1) &&
#else
         expected &&
#endif
         d.GpuStopUnconfirmed==!expected,"PnP stop retains earlier manual Fini failure even with cleared stages",mask);
  }
 }
#ifdef HAS_HALT_PHASE
 // Hardware retirement alone must preserve every software owner and page.
 // Exercise actual helpers, including a quiet halt with an unsuccessful undo.
 for(mask=0;mask<8;mask++) {
  BC250_DEVICE d={0};BC250_GFX gfx={0};long undo=0;int quiet;
  int oldRelease=sequenceReleaseCalls,oldTeardown=teardownCalls;
  finiQuiet=!((mask>>0)&1);undoFail=(mask>>1)&1;sequenceFail=(mask>>2)&1;
  gfx.StagesDone=8;gfx.SetUp=1;d.Gfx=&gfx;modelAdev.backend=&gfx.Sequence;
  quiet=HaltEngines(&d,&gfx,&modelAdev,&undo);
  check(d.Gfx==&gfx && gfx.SetUp && gfx.StagesDone==8 &&
        sequenceReleaseCalls==oldRelease && teardownCalls==oldTeardown,
        "halt phase retains owners, stages and all storage",mask);
  check(quiet==finiQuiet && (undo!=0)==undoFail,
        "halt-register result does not erase undo failure",mask);
  ReleaseStoppedStorage(&d,&gfx,&modelAdev,quiet,undo);
  check(sequenceReleaseCalls==oldRelease+1 && teardownCalls==oldTeardown+1 &&
        sequenceRelease==(finiQuiet&&!undoFail&&!sequenceFail),
        "release phase preserves the complete retirement verdict",mask);
 }
#endif
 printf("%d checks, %d failures\n",checks,failures);
 return failures?1:0;
}
