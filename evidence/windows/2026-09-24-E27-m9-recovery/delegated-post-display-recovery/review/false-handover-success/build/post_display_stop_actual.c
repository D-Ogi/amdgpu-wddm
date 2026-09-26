#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int32_t NTSTATUS;
typedef unsigned long ULONG;
typedef int BOOLEAN;
typedef void* PVOID;
typedef int KIRQL;
typedef struct {int64_t QuadPart;} LARGE_INTEGER;
typedef unsigned D3DDDI_VIDEO_PRESENT_TARGET_ID;
typedef struct {unsigned Width,Height,Pitch,TargetId;uint64_t PhysicAddress;} DXGK_DISPLAY_INFORMATION,*PDXGK_DISPLAY_INFORMATION;
typedef struct LIST_ENTRY {struct LIST_ENTRY *Flink,*Blink;} LIST_ENTRY;
typedef struct BC250_PAGING_JOB {struct BC250_PAGING_JOB*Next;} BC250_PAGING_JOB;
typedef struct {LIST_ENTRY Link;} BC250_WDDM_OBJECT;
typedef struct {
    int HwPending,PagingHwPending,Lock,Stopping,VSyncArmed,VSyncTimer,SubmitTimer,PagingSubmitTimer;
    int SubmitDpc,PagingSubmitDpc,VSyncDpc,ReportDpc;
    long LastCompletedFence,VSyncTicks;
    BC250_PAGING_JOB *PagingHead,*PagingTail;
    LIST_ENTRY Objects;
} BC250_WDDM;
typedef struct {
    BC250_WDDM *Wddm;
    int Smu,Started,ModeActive,SourceVisible,CommitSeen,PresentSeen,FullWddm,SystemDisplayReady;
    volatile long DcnVsyncArmed;
    BOOLEAN PostDisplayStopAttempted;
    NTSTATUS PostDisplayStopStatus;
    void *Framebuffer;
    DXGK_DISPLAY_INFORMATION Post;
} BC250_DEVICE;
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_CHILD_UID 0x250001u
#define D3DDDI_ID_UNINITIALIZED 0xffffffffu
#define BC250_WDDM_SUBMIT_TIMEOUT_MS 1
#define BC250_WDDM_TAG 1
#define KernelMode 0
#define StageStopEnter 70
#define StageStopDone 79
#define GuardLog(...) ((void)0)
#define RtlZeroMemory(p,n) memset(p,0,n)
#define CONTAINING_RECORD(p,t,f) ((t*)((char*)(p)-offsetof(t,f)))
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
static struct {unsigned joined,smu,restores,vidmm,objects,unmaps;NTSTATUS result;} model;
static void WddmGpuFence(BC250_DEVICE*d){(void)d;}
static void WddmGpuFencePaging(BC250_DEVICE*d){(void)d;}
static void KeDelayExecutionThread(int k,int a,LARGE_INTEGER*t){(void)k;(void)a;(void)t;}
static void KeAcquireSpinLock(int*l,KIRQL*i){(void)l;*i=0;}
static void KeReleaseSpinLock(int*l,KIRQL i){(void)l;(void)i;}
static void KeCancelTimer(int*t){(void)t;}
static void KeRemoveQueueDpc(int*d){(void)d;}
static long InterlockedExchange(volatile long*p,long v){long old=*p;*p=v;return old;}
static void InterlockedExchangePointer(BC250_WDDM**p,void*v){*p=v;}
static NTSTATUS DcnVsyncEnable(BC250_DEVICE*d,BOOLEAN on){(void)d;CHECK(!on);return STATUS_SUCCESS;}
static void KeFlushQueuedDpcs(void){model.joined++;}
static NTSTATUS DcnStop(BC250_DEVICE*d)
{ CHECK(model.smu && d->Framebuffer);CHECK(model.objects==0 && model.vidmm==0);model.restores++;return model.result; }
static void VidMmStop(void){CHECK(model.joined && model.restores==1);model.vidmm++;}
static void WddmSummaryOf(BC250_WDDM*w){(void)w;}
static int IsListEmpty(LIST_ENTRY*h){return h->Flink==h;}
static LIST_ENTRY*RemoveHeadList(LIST_ENTRY*h){LIST_ENTRY*e=h->Flink;h->Flink=e->Flink;h->Flink->Blink=h;return e;}
static void WddmReleaseCaptures(BC250_WDDM_OBJECT*o){(void)o;CHECK(model.restores==1);}
static void ExFreePoolWithTag(void*p,int tag){(void)p;(void)tag;CHECK(model.restores==1 && model.vidmm==1);model.objects++;}
static void GuardStage(int s){(void)s;}
static void GuardLogKeep(void){}
static void SmuOwnerStop(int*s){(void)s;model.smu++;}
#define STOP_STUB(n) static void n(BC250_DEVICE*d){(void)d;CHECK(model.smu && model.restores==1);}
STOP_STUB(IhStop)
STOP_STUB(GfxPrepareStop)
STOP_STUB(PspStop)
STOP_STUB(GartPrepareStop)
STOP_STUB(GfxStop)
STOP_STUB(GartStop)
STOP_STUB(VramStop)
STOP_STUB(MmioStop)
static void GfxTraceRlcState(BC250_DEVICE*d,const char*s){(void)d;(void)s;}
static void DisplayUnmapFramebuffer(BC250_DEVICE*d){CHECK(model.restores==1);d->Framebuffer=NULL;model.unmaps++;}
void WddmStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    ULONG freed = 0;
    LIST_ENTRY* entry;
    KIRQL irql;

    if (wddm == NULL) return;

    // Stage C: GfxStop comes after this function (pnp.c) and the state below is about to be freed, so a packet
    // still on the ring gets a bounded moment to finish. PASSIVE_LEVEL. The watchdog ends the wait at the latest.
    {
        LARGE_INTEGER tick;
        ULONG waited;

        tick.QuadPart = -10000ll * 10;
        for (waited = 0; waited < BC250_WDDM_SUBMIT_TIMEOUT_MS + 100 && wddm->HwPending; waited += 10)
        {
            WddmGpuFence(Device);
            if (wddm->HwPending) KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (waited != 0) GuardLog("wddm: stop waited %u ms for the packet in flight (%s)", waited, wddm->HwPending ? "STILL PENDING" : "done");

        // ADR 0008 stage D: node 1's own packet in flight, waited for independently - it may still be on SDMA0's
        // ring after node 0's has long finished (design note section 5).
        for (waited = 0; waited < BC250_WDDM_SUBMIT_TIMEOUT_MS + 100 && wddm->PagingHead; waited += 10)
        {
            WddmGpuFencePaging(Device);
            if (wddm->PagingHwPending) KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (waited != 0) GuardLog("wddm: stop waited %u ms for node 1's packet in flight (%s)", waited,
                                  wddm->PagingHwPending ? "STILL PENDING" : "done");
    }

    // Order matters, and this is the order:
    //
    //  1. Stopping under the lock. From here nothing of ours arms a timer, queues a DPC or joins the object
    //     list, so the cancel and the flush below are final rather than a race they might lose. The hardware
    //     vsync interrupt is disabled and acked here too (ADR 0011 point 3 step 3), in the same critical
    //     section as the timer's own cancel and for the same reason (WddmVSyncArm's own comment) - so that this
    //     is strictly before DcnStop's surface restore and MmioStop (pnp.c's Bc250StopDevice), not merely
    //     usually before them.
    //  2. Cancel timers and take back queued private DPCs, then atomically detach
    //     Device->Wddm BEFORE KeFlushQueuedDpcs. IH is still enabled: its DPC may
    //     arrive after the flush boundary. New DPC entries must already see NULL.
    //  3. Flush joins DPCs that captured the old pointer before detach; only then
    //     read summaries or free the object. The ISR itself never reads Wddm.
    //
    // What this does not cover: a DDI that read Device->Wddm before step 2 and then touches the
    // state after it is freed. That one rests on dxgkrnl's own guarantee that no DDI arrives during StopDevice
    // (Level Three, see WddmSummary).
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
    // OS level-three exclusion has stopped flip DDIs and our DPCs are joined.
    // Restore the reserved POST surface before VidMm/object release can retire
    // the buffer DCN was scanning. Keep failure distinct from StopDevice success.
    Device->PostDisplayStopStatus=DcnStop(Device);
    Device->PostDisplayStopAttempted=TRUE;
    while (wddm->PagingHead) {
        BC250_PAGING_JOB* job=wddm->PagingHead;
        wddm->PagingHead=job->Next;
        RtlZeroMemory(job,sizeof(*job));
    }
    wddm->PagingTail=NULL;
    VidMmStop();
    // DcnStop already released the CPU scanout alias before the verified restore.

    // The counters are the point of stage A: all of them, once, at the stop. The state is ours alone now, so the
    // summary cannot race anything.
    WddmSummaryOf(wddm);

    // Whatever dxgkrnl did not destroy is ours to free: a process or a device left behind would otherwise live
    // until the next boot. No lock is needed now, nothing else can reach the list.
    while (!IsListEmpty(&wddm->Objects))
    {
        entry = RemoveHeadList(&wddm->Objects);
        WddmReleaseCaptures(CONTAINING_RECORD(entry, BC250_WDDM_OBJECT, Link));
        ExFreePoolWithTag(CONTAINING_RECORD(entry, BC250_WDDM_OBJECT, Link), BC250_WDDM_TAG);
        freed++;
    }
    GuardLog("wddm: stop, last completed fence %ld, %lu vsync ticks, %lu objects freed at the stop",
             wddm->LastCompletedFence, (ULONG)wddm->VSyncTicks, freed);
    ExFreePoolWithTag(wddm, BC250_WDDM_TAG);
}

NTSTATUS Bc250StopDevice(_In_ const PVOID MiniportDeviceContext)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    GuardStage(StageStopEnter);
    SmuOwnerStop(&device->Smu); // join clients before any engine/translation teardown
    device->SystemDisplayReady=FALSE;
    device->PostDisplayStopAttempted=FALSE;
    device->PostDisplayStopStatus=STATUS_DEVICE_NOT_READY;
    device->Started = FALSE;
    device->ModeActive = FALSE;
    device->SourceVisible = FALSE;      // so that the next start writes its own first-commit and first-present breadcrumbs
    device->CommitSeen = FALSE;
    device->PresentSeen = FALSE;
    WddmStop(device);       // first: it logs what dxgkrnl called, and nothing below it is allowed to have run
    // Display-only starts have no WDDM object and therefore no WddmStop restore.
    if (!device->PostDisplayStopAttempted) {
        device->PostDisplayStopStatus=DcnStop(device);
        device->PostDisplayStopAttempted=TRUE;
    }
    IhStop(device);         // no interrupt of ours from here on
    GfxPrepareStop(device); // halt engines, retain storage through firmware/translation retirement
    PspStop(device);
    GartPrepareStop(device);
    GfxStop(device);        // destroy storage while the GART owner/table remain available
    GartStop(device);       // final firmware restore and owner destruction
    if (device->FullWddm) GfxTraceRlcState(device,"after-gart-stop");
    VramStop(device);
    MmioStop(device);
    DisplayUnmapFramebuffer(device);
    GuardStage(StageStopDone);
    // Last, so that the file holds the stop as well. Does nothing unless Parameters\KeepLog is set: dxgkrnl may
    // end a full WDDM start by itself and unload the driver after, ring and all (E16 run 1).
    GuardLogKeep();
    return STATUS_SUCCESS;
}

NTSTATUS Bc250StopDeviceAndReleasePostDisplayOwnership(_In_ PVOID MiniportDeviceContext,
                                                       _In_ D3DDDI_VIDEO_PRESENT_TARGET_ID TargetId,
                                                       _Out_ PDXGK_DISPLAY_INFORMATION DisplayInfo)
{
    BC250_DEVICE* device = (BC250_DEVICE*)MiniportDeviceContext;

    NTSTATUS status;
    UNREFERENCED_PARAMETER(TargetId); // one active output; return its actual id below
    RtlZeroMemory(DisplayInfo,sizeof(*DisplayInfo));
    // Local dispmprt contract: failure makes dxgkrnl call ordinary StopDevice.
    // Never publish a framebuffer that DCN failed to latch. Ordinary StopDevice
    // still finishes teardown and returns its own completion result.
    status=Bc250StopDevice(MiniportDeviceContext);
    if (!NT_SUCCESS(status)) return status;
    /* negative: handover falsely claims success */
    *DisplayInfo = device->Post;
    DisplayInfo->TargetId = BC250_CHILD_UID;
    return STATUS_SUCCESS;
}

static void init(BC250_DEVICE*d,BC250_WDDM*w,BC250_WDDM_OBJECT*o)
{
    memset(d,0,sizeof(*d));memset(w,0,sizeof(*w));memset(o,0,sizeof(*o));memset(&model,0,sizeof(model));
    d->Wddm=w;d->Framebuffer=d;d->FullWddm=1;d->DcnVsyncArmed=1;w->VSyncArmed=1;
    d->Post.Width=1920;d->Post.Height=1200;d->Post.Pitch=7680;d->Post.PhysicAddress=0x470000000ull;
    w->Objects.Flink=w->Objects.Blink=&o->Link;o->Link.Flink=o->Link.Blink=&w->Objects;
}
int main(void)
{
    BC250_DEVICE d;BC250_WDDM w;BC250_WDDM_OBJECT o;DXGK_DISPLAY_INFORMATION info;
    init(&d,&w,&o);
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_SUCCESS);
    CHECK(model.smu==1 && model.joined==1 && model.restores==1 && model.objects==2 && model.unmaps==1);
    CHECK(info.Width==1920 && info.Height==1200 && info.Pitch==7680 && info.PhysicAddress==d.Post.PhysicAddress && info.TargetId==BC250_CHILD_UID);
    CHECK(d.PostDisplayStopAttempted && d.PostDisplayStopStatus==STATUS_SUCCESS);
    init(&d,&w,&o);model.result=STATUS_IO_TIMEOUT;memset(&info,0xcc,sizeof(info));
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_IO_TIMEOUT);
    CHECK(info.Width==0 && info.Height==0 && info.Pitch==0 && info.PhysicAddress==0);
    CHECK(model.restores==1 && model.unmaps==1 && d.PostDisplayStopStatus==STATUS_IO_TIMEOUT);
    // Ordinary stop remains successful even when display restoration failed.
    init(&d,&w,&o);model.result=STATUS_IO_TIMEOUT;
    CHECK(Bc250StopDevice(&d)==STATUS_SUCCESS);CHECK(d.PostDisplayStopStatus==STATUS_IO_TIMEOUT);
    // No WDDM instance: PnP performs the restore itself before MMIO teardown.
    init(&d,&w,&o);d.Wddm=NULL;
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,D3DDDI_ID_UNINITIALIZED,&info)==STATUS_SUCCESS);
    CHECK(model.joined==0 && model.restores==1 && model.vidmm==0 && info.TargetId==BC250_CHILD_UID);
    printf("post display stop: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
