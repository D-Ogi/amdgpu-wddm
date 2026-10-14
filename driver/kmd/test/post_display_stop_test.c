#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
/* Plain integers, and the runner's include path holds driver\kmd: the count of the refusal
 * classes that the extracted WddmStop walks. */
#include "gdi_render.h"
typedef int32_t NTSTATUS;
typedef unsigned long ULONG;
typedef unsigned UINT;
typedef unsigned long long ULONGLONG;
typedef int BOOLEAN;
typedef void* PVOID;
typedef int KIRQL;
typedef struct {int64_t QuadPart;} LARGE_INTEGER;
typedef unsigned D3DDDI_VIDEO_PRESENT_TARGET_ID;
typedef struct {unsigned Width,Height,Pitch,ColorFormat,TargetId;LARGE_INTEGER PhysicAddress;} DXGK_DISPLAY_INFORMATION,*PDXGK_DISPLAY_INFORMATION;
typedef struct LIST_ENTRY {struct LIST_ENTRY *Flink,*Blink;} LIST_ENTRY;
typedef struct BC250_PAGING_JOB {struct BC250_PAGING_JOB*Next;} BC250_PAGING_JOB;
typedef struct {LIST_ENTRY Link;} BC250_WDDM_OBJECT;
typedef struct {BOOLEAN BoardAllowed;} BOARD_BLOCK;
typedef struct {
    int HwPending,PagingHwPending,Lock,Stopping,VSyncArmed,VSyncTimer,SubmitTimer,PagingSubmitTimer,PagingDrainTimer;
    int SubmitDpc,PagingSubmitDpc,PagingDrainDpc,VSyncDpc,ReportDpc;
    int ComputeHwPending,ComputeTimer,ComputeDpc;       /* node 2's channel (compute-node.md) */
    long LastCompletedFence,VSyncTicks;
    BC250_PAGING_JOB *PagingHead,*PagingTail;
    LIST_ENTRY Objects;
    struct {void *Buckets;} ObjectIndex;
    /* The GDI totals the real WddmStop writes to the log. The count of the refusal classes comes
     * from the shipping header, so a new class reaches this harness by itself. */
    long long GdiRenderCalls,GdiRenderCommands,GdiRenderItems,GdiRenderDwords;
    long long GdiRenderRefusedCall,GdiRenderMalformed,GdiRenderOutside,GdiRenderEmpty;
    long long GdiRenderOverflow,GdiRenderKernelRefused,GdiRenderColorFillRedFirst;
    long long GdiRenderRefused[Bc250GdiRefusals];
} BC250_WDDM;
typedef struct _BC250_DEVICE {
    BC250_WDDM *Wddm;
    void* PhysicalDeviceObject;
    BOARD_BLOCK Smu,Hwmon;
    int Started,ModeActive,SourceVisible,CommitSeen,PresentSeen,FullWddm,SystemDisplayReady;
    volatile long DcnVsyncArmed;
    volatile long StartVisibilityPending;
    BOOLEAN InheritedSignalValid;
    BOOLEAN PostDisplayStopAttempted;
    NTSTATUS PostDisplayStopStatus;
    BOOLEAN StopDone;
    void *Framebuffer;
    size_t FramebufferLength;
    DXGK_DISPLAY_INFORMATION Post;
    DXGK_DISPLAY_INFORMATION RuntimePost;
    volatile long RuntimePostValid, RuntimeDisplayChanged;
    void* RuntimeFramebuffer;
    size_t RuntimeFramebufferLength;
    LARGE_INTEGER RuntimeFramebufferPhysical;
} BC250_DEVICE;
#include "connector_handoff.h"
#define UNREFERENCED_PARAMETER(x) ((void)(x))
#define TRUE 1
#define FALSE 0
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define STATUS_DEVICE_DATA_ERROR ((NTSTATUS)0xC000009Cu)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBu)
#define STATUS_IO_DEVICE_ERROR ((NTSTATUS)0xC0000185u)
#define NT_SUCCESS(s) ((s)>=0)
#define BC250_CHILD_UID 0x250001u
#define D3DDDI_ID_UNINITIALIZED 0xffffffffu
#define BC250_WDDM_STOP_DRAIN_MS 1          // BD-114: the stop drain has its own bound now, not the watchdog budget
#define BC250_WDDM_TAG 1
#define KernelMode 0
#define StageStopEnter 70
#define StageStopDone 79
#define RtlZeroMemory(p,n) memset(p,0,n)
#define CONTAINING_RECORD(p,t,f) ((t*)((char*)(p)-offsetof(t,f)))
static unsigned checks,failures;
#define CHECK(x) do {++checks;if(!(x)){++failures;printf("FAIL %d: %s\n",__LINE__,#x);}}while(0)
/* BD-090: the lines the stop and the release write, as the ring holds them, so that the self-check is tested too. */
static char logLines[128][256];
static unsigned logCount,keptThrough;
static void GuardLog(const char* format,...)
{ va_list a;va_start(a,format);vsnprintf(logLines[logCount%128],sizeof(logLines[0]),format,a);va_end(a);logCount++; }
static int logged(const char* line)
{ unsigned i;for(i=0;i<logCount && i<128;i++)if(strcmp(logLines[i],line)==0)return 1;return 0; }
static int ownedPdo,foreignPdo;
static void* ownerIdentity;
static int AdapterOwnerIs(void* identity){return identity && identity==ownerIdentity;}
/* Reset by init(), so these are "in this run", unlike the totals the stubs below count. */
static struct {unsigned joined,smu,restores,vidmm,objects,unmaps,dpm,hdmiStops;NTSTATUS result,hdmiResult;} model;
static void WddmGpuFence(BC250_DEVICE*d){(void)d;}
static void WddmGpuFencePaging(BC250_DEVICE*d){(void)d;}
static void WddmGpuFenceCompute(BC250_DEVICE*d){(void)d;}
static void KeDelayExecutionThread(int k,int a,LARGE_INTEGER*t){(void)k;(void)a;(void)t;}
static void KeAcquireSpinLock(int*l,KIRQL*i){(void)l;*i=0;}
static void KeReleaseSpinLock(int*l,KIRQL i){(void)l;(void)i;}
static void KeCancelTimer(int*t){(void)t;}
static void KeRemoveQueueDpc(int*d){(void)d;}
static long InterlockedExchange(volatile long*p,long v){long old=*p;*p=v;return old;}
/* Not static: the release of 8869edf5 (the base peel) calls none of these, and a static one it never calls would not
   build under /W4 /WX. */
long InterlockedCompareExchange(volatile long*p,long v,long c){long old=*p;if(old==c)*p=v;return old;}
static unsigned healthLocks;
void StartHealthEnter(BC250_DEVICE*d){(void)d;CHECK(!healthLocks);healthLocks++;}
void StartHealthLeave(BC250_DEVICE*d){(void)d;CHECK(healthLocks==1);healthLocks--;}
static void InterlockedExchangePointer(BC250_WDDM**p,void*v){*p=v;}
static NTSTATUS DcnVsyncEnable(BC250_DEVICE*d,BOOLEAN on){(void)d;CHECK(!on);return STATUS_SUCCESS;}
static void KeFlushQueuedDpcs(void){model.joined++;}
/* dcn.c's DcnRestorePostDisplaySerialized: once a detach or a runtime mode set RuntimeDisplayChanged, only a valid
   RuntimePost on the reserved buffer can be restored (DcnRuntimeSurfaceAuthorized). */
static NTSTATUS DcnStop(BC250_DEVICE*d)
{
    CHECK(d->StartVisibilityPending==0);CHECK(model.smu && d->Framebuffer);CHECK(!d->Smu.BoardAllowed && !d->Hwmon.BoardAllowed);CHECK(model.objects==0 && model.vidmm==0);model.restores++;
    if(d->RuntimeDisplayChanged && (!d->RuntimePostValid || d->RuntimePost.PhysicAddress.QuadPart!=d->RuntimeFramebufferPhysical.QuadPart))
        return STATUS_DEVICE_NOT_READY;
    return model.result;
}
/* connector_link.c's ConnectorLinkHandoff (connector_link_test runs the real one). The model gives its answer; an
   ENABLED answer leaves what a successful commit onto the reserved buffer leaves (Apply). */
static struct {unsigned calls;ULONG action;NTSTATUS status;} handoffModel;
NTSTATUS ConnectorLinkHandoff(BC250_DEVICE*d,BC250_LINK_HANDOFF*h)
{
    CHECK(healthLocks==1 && d->StartVisibilityPending==0);
    CHECK(model.smu==0 && model.restores==0 && model.dpm==0 && model.unmaps==0);   /* before any teardown */
    memset(h,0,sizeof(*h));handoffModel.calls++;h->Action=handoffModel.action;h->Status=handoffModel.status;
    if(h->Action==BC250_LINK_HANDOFF_ENABLED || h->Action==BC250_LINK_HANDOFF_FAILED) {
        h->Detached=1;h->Width=1920;h->Height=1200;h->PixelKhz=154000;
    }
    if(h->Action==BC250_LINK_HANDOFF_ENABLED) {
        d->RuntimePost=d->Post;d->RuntimePost.Pitch=7680;d->RuntimePost.PhysicAddress=d->RuntimeFramebufferPhysical;
        d->RuntimePostValid=1;d->RuntimeDisplayChanged=1;
    }
    return h->Status;
}
static void VidMmStop(void){CHECK(model.joined && model.restores==1);model.vidmm++;}
static void WddmSummaryOf(BC250_WDDM*w){(void)w;}
static int IsListEmpty(LIST_ENTRY*h){return h->Flink==h;}
static LIST_ENTRY*RemoveHeadList(LIST_ENTRY*h){LIST_ENTRY*e=h->Flink;h->Flink=e->Flink;h->Flink->Blink=h;return e;}
static void WddmReleaseCaptures(BC250_WDDM_OBJECT*o){(void)o;CHECK(model.restores==1);}
static void*freedOrder[4];
static void ExFreePoolWithTag(void*p,int tag){(void)tag;CHECK(model.restores==1 && model.vidmm==1);if(model.objects<4)freedOrder[model.objects]=p;model.objects++;}
static void HangDetectorStop(void){CHECK(model.smu==0 && model.restores==0);}   /* KMD172: before any teardown */
static void StartHealthClose(BC250_DEVICE*d){(void)d;}
static void GuardRecordStopBegin(BC250_DEVICE*d){StartHealthClose(d);CHECK(model.unmaps==0);}
static void GuardRecordStopDone(void){CHECK(model.unmaps==1);}
/* board_memory_service.c, board_memory_identity.c: the stop path ends the board memory operation and
   drops the captured identity. Neither one is what this test is about. */
static void BoardMemoryStop(BC250_DEVICE*d){(void)d;}
static NTSTATUS ConnectorStop(BC250_DEVICE*d){CHECK(d->StartVisibilityPending==0);CHECK(model.smu==0 && model.unmaps==0);return STATUS_SUCCESS;}
static void ConnectorBufferRelease(BC250_DEVICE*d){CHECK(model.restores==1);d->RuntimeFramebuffer=NULL;}
static void BoardMemoryIdentityClear(BC250_DEVICE*d){CHECK(!d->Smu.BoardAllowed && !d->Hwmon.BoardAllowed);}
static void GuardStage(int s){(void)s;}
static void GuardLogKeep(void){keptThrough=logCount;}
/* BD-090: the give-back runs once per stop of a started device, after the last block is down. */
static unsigned releases;
static void GuardReleaseStart(void){CHECK(model.unmaps==1);releases++;}
/* One kept log file per device stop (0.7.216.10), named before any teardown, so that the stop's own lines land in it. */
static unsigned stopEpisodes;
static void GuardLogKeepEpisode(const wchar_t*label)
{CHECK(wcscmp(label,L"stop")==0);CHECK(model.smu==0 && model.restores==0);stopEpisodes++;}
static unsigned dpmStops;
static int cpuStopped;          /* within one scenario: CpuStop ran; init clears it */
/* KMD175: the DPM governor puts the floor back while the SMU owner is still online, before any teardown. */
static void DpmStop(BC250_DEVICE*d){(void)d;CHECK(model.smu==0 && model.restores==0 && cpuStopped);model.dpm++;dpmStops++;}
static unsigned cpuStops;
/* The CPU trial is reverted while the SMU owner is still online and before the DPM governor stops, so
 * that a CPU sequence can never run while the governor is putting the floor back. */
static void CpuStop(BC250_DEVICE*d){(void)d;CHECK(model.smu==0 && model.restores==0 && !cpuStopped);cpuStopped=1;cpuStops++;}
static unsigned metricsStops;
/* KMD 0.7.215: the SMU metrics page is unmapped after DpmStop has joined the governor thread (the one reader of
   the page), before the case fan goes back to the board and while the SMU owner is still online. */
static void SmuMetricsStop(BC250_DEVICE*d){(void)d;CHECK(model.dpm==1 && model.smu==0 && model.restores==0);metricsStops++;}
static unsigned hwmonStops;
/* The governor thread is what samples the board's hardware monitor, so the reader closes after
   DpmStop has joined that thread, and before the SMU owner goes offline. */
static void HwmonStop(BOARD_BLOCK*h){CHECK(h->BoardAllowed && model.dpm==1 && model.smu==0);hwmonStops++;}
/* The case fan goes back to the board after DpmStop (no fan step runs any more) and before the reader it
   reads its inputs from closes, while the SMU owner is still online (fan.c, docs/design/fan.md Part B). */
#define BC250_FAN_REASON_STOP 2u
static unsigned fanStops;
static void FanStop(BC250_DEVICE*d,unsigned r){(void)d;CHECK(r==BC250_FAN_REASON_STOP && model.dpm==1 && model.smu==0 && hwmonStops==fanStops && metricsStops==fanStops+1);fanStops++;}
static unsigned dpaudioStops;
/* DP audio: AUDIO_ENABLED goes to 0 while BAR5 and the display block are still ours, before the SMU owner goes
   offline and before WddmStop/DcnStop restore the surface. */
static void DpAudioStop(BC250_DEVICE*d){(void)d;CHECK(model.dpm==1 && model.smu==0 && model.restores==0);dpaudioStops++;}
static BOOLEAN HdmiLinkStop(BC250_DEVICE*d)
{(void)d;CHECK(model.dpm==1 && model.smu==0 && model.restores==0 && dpaudioStops);model.hdmiStops++;return TRUE;}
static NTSTATUS HdmiLinkRelease(BC250_DEVICE*d)
{(void)d;CHECK(model.hdmiStops==1 && model.restores==1 && model.unmaps==0);return model.hdmiResult;}
static void SmuOwnerStop(BOARD_BLOCK*s){CHECK(s->BoardAllowed && model.dpm==1 && cpuStopped);model.smu++;}
static unsigned interopStops;
/* KMD181: the interop session marker goes after WddmStop (DDI devices are gone), registry only. */
static void InteropStop(BC250_DEVICE*d){(void)d;CHECK(model.smu==1);interopStops++;}
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
static unsigned retireSignals;
/* KMD196: a held submission is woken after Stopping is set and before anything it could touch is detached or freed. */
static void GfxRetireSignal(BC250_DEVICE*d)
{ CHECK(d->Wddm && d->Wddm->Stopping && !d->Wddm->VSyncArmed);CHECK(model.joined==0 && model.restores==0 && model.vidmm==0 && model.objects==0);retireSignals++; }
/* ACTUAL_SOURCE */
static char buckets[16];
static unsigned char post[64];  /* the POST framebuffer the release fills with black */
static int postIs(unsigned char v){size_t i;for(i=0;i<sizeof(post);i++)if(post[i]!=v)return 0;return 1;}
static void init(BC250_DEVICE*d,BC250_WDDM*w,BC250_WDDM_OBJECT*o)
{
    memset(d,0,sizeof(*d));memset(w,0,sizeof(*w));memset(o,0,sizeof(*o));memset(&model,0,sizeof(model));retireSignals=0;cpuStopped=0;
    memset(freedOrder,0,sizeof(freedOrder));w->ObjectIndex.Buckets=buckets;releases=0;memset(post,0xab,sizeof(post));
    memset(&handoffModel,0,sizeof(handoffModel));memset(logLines,0,sizeof(logLines));logCount=keptThrough=healthLocks=0;
    d->Wddm=w;d->Framebuffer=post;d->FramebufferLength=sizeof(post);d->FullWddm=1;d->DcnVsyncArmed=1;w->VSyncArmed=1;d->Started=1;
    d->StartVisibilityPending=1;
    ownerIdentity=&ownedPdo;d->PhysicalDeviceObject=&ownedPdo;d->Smu.BoardAllowed=d->Hwmon.BoardAllowed=TRUE;
    d->Post.Width=1920;d->Post.Height=1200;d->Post.Pitch=7680;d->Post.PhysicAddress.QuadPart=0x470000000ll;
    d->RuntimeFramebufferPhysical.QuadPart=0x480000000ll;
    w->Objects.Flink=w->Objects.Blink=&o->Link;o->Link.Flink=o->Link.Blink=&w->Objects;
}
int main(void)
{
    BC250_DEVICE d;BC250_WDDM w;BC250_WDDM_OBJECT o;DXGK_DISPLAY_INFORMATION info;
    init(&d,&w,&o);
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_SUCCESS);
    /* Three pool blocks: the one object, the object index's buckets (KMD 0.7.192) and the adapter state. */
    CHECK(model.smu==1 && model.joined==1 && model.restores==1 && model.objects==3 && model.unmaps==1 && dpmStops==1 && interopStops==1 && cpuStops==1);
    CHECK(hwmonStops==1 && fanStops==1 && dpaudioStops==1 && metricsStops==1);
    /* The list owns every object, the index only points into it: the objects go first, the index after the last
       of them, the adapter state last of all (WddmStop's drain). */
    CHECK(freedOrder[0]==&o && freedOrder[1]==buckets && freedOrder[2]==&w);
    CHECK(retireSignals==1);
    CHECK(info.Width==1920 && info.Height==1200 && info.Pitch==7680 && info.PhysicAddress.QuadPart==d.Post.PhysicAddress.QuadPart && info.TargetId==BC250_CHILD_UID);
    CHECK(d.PostDisplayStopAttempted && d.PostDisplayStopStatus==STATUS_SUCCESS);
    CHECK(postIs(0) && releases==1 && d.StopDone);  /* WDDM 1.2: black before the visible handover; BD-090 give-back */
    /* BD-090, the self-check: the release names what it handed back, and the stop keeps that line too. */
    CHECK(handoffModel.calls==1 && healthLocks==0);
    CHECK(logged("post-display release: target 2424833 started 1 runtime changed 0 valid 0"));
    CHECK(logged("post-display release: link static detached-mode 0 timing 0x0 0 kHz status 00000000"));
    CHECK(logged("stop: display handoff restore 00000000 hdmi 00000000 connector 00000000 result 00000000"));
    CHECK(logged("post-display release: handed back firmware 1920x1200 pitch 7680 address 470000000 target 2424833"));
    CHECK(keptThrough==logCount);
    init(&d,&w,&o);model.result=STATUS_IO_TIMEOUT;memset(&info,0xcc,sizeof(info));
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_IO_TIMEOUT);
    CHECK(info.Width==0 && info.Height==0 && info.Pitch==0 && info.PhysicAddress.QuadPart==0);
    CHECK(model.restores==1 && model.unmaps==1 && d.PostDisplayStopStatus==STATUS_IO_TIMEOUT);
    CHECK(logged("stop: display handoff restore C00000B5 hdmi 00000000 connector 00000000 result C00000B5"));
    CHECK(logged("post-display release: handed back nothing, status C00000B5: the display restore failed, see stop: display handoff"));
    CHECK(keptThrough==logCount);
    /* BD-090: dxgkrnl follows a failed release with StopDevice. The teardown already ran: nothing runs twice. */
    {unsigned cpu=cpuStops,dpm=dpmStops,interop=interopStops,fan=fanStops,audio=dpaudioStops;
     CHECK(Bc250StopDevice(&d)==STATUS_SUCCESS);
     CHECK(model.restores==1 && model.unmaps==1 && model.smu==1 && model.vidmm==1 && releases==1);
     CHECK(cpuStops==cpu && dpmStops==dpm && interopStops==interop && fanStops==fan && dpaudioStops==audio);
     CHECK(d.PostDisplayStopStatus==STATUS_IO_TIMEOUT);}
    // Ordinary stop remains successful even when display restoration failed.
    init(&d,&w,&o);model.result=STATUS_IO_TIMEOUT;
    CHECK(Bc250StopDevice(&d)==STATUS_SUCCESS);CHECK(d.PostDisplayStopStatus==STATUS_IO_TIMEOUT);
    CHECK(postIs(0xab) && releases==1);  /* the black fill belongs to the release only */
    /* A start that failed and stops itself (pnp.c's StartDevice error paths) has Started FALSE: it keeps its count. */
    init(&d,&w,&o);d.Started=0;
    CHECK(Bc250StopDevice(&d)==STATUS_SUCCESS && releases==0 && d.StopDone);
    // No WDDM instance: PnP performs the restore itself before MMIO teardown.
    init(&d,&w,&o);d.Wddm=NULL;
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,D3DDDI_ID_UNINITIALIZED,&info)==STATUS_SUCCESS);
    CHECK(model.joined==0 && model.restores==1 && model.vidmm==0 && info.TargetId==BC250_CHILD_UID);
    CHECK(retireSignals==0);   /* no WDDM state, no held submission to wake */
    /* One per stop that tears down (five here), and the post-display stop goes through the ordinary one. The BD-090
       repeat after a failed release returns before the episode: a stop that does nothing keeps no file. */
    CHECK(stopEpisodes==5);
    /* Stop of a different PDO must not tear down this image's current owner or
     * publish its framebuffer. This also checks the absent-owner/null cases. */
    {unsigned i;for(i=0;i<4;i++){
        init(&d,&w,&o);
        if(i==0)d.PhysicalDeviceObject=&foreignPdo;
        if(i==1)d.PhysicalDeviceObject=NULL;
        if(i==2)ownerIdentity=NULL;
        CHECK(Bc250StopDevice(i==3?NULL:&d)==STATUS_SUCCESS);
        memset(&info,0xcc,sizeof(info));
        CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(i==3?NULL:&d,BC250_CHILD_UID,&info)==STATUS_DEVICE_NOT_READY);
        CHECK(model.smu==0 && model.joined==0 && model.restores==0 && model.unmaps==0 && model.objects==0 && model.hdmiStops==0);
        CHECK(d.Started && !d.StopDone && d.StartVisibilityPending==1 && d.Smu.BoardAllowed && d.Hwmon.BoardAllowed && postIs(0xab));
        CHECK(info.Width==0 && info.Height==0 && info.Pitch==0 && info.PhysicAddress.QuadPart==0 && stopEpisodes==5);
        CHECK(handoffModel.calls==0 && logCount==0);   /* not this driver's device: no link touched, no line */
    }}
    // A final failed encoder restore cannot be reported as a successful handoff.
    init(&d,&w,&o);model.hdmiResult=STATUS_DEVICE_DATA_ERROR;memset(&info,0xcc,sizeof(info));
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_DEVICE_DATA_ERROR);
    CHECK(info.Width==0 && info.Height==0 && info.Pitch==0 && info.PhysicAddress.QuadPart==0);
    CHECK(model.unmaps==1 && d.PostDisplayStopStatus==STATUS_DEVICE_DATA_ERROR);
    CHECK(logged("stop: display handoff restore 00000000 hdmi C000009C connector 00000000 result C000009C"));
    // The active handoff surface is distinct from GOP and must be black before
    // release, while its returned geometry survives the CPU mapping teardown.
    {
        unsigned char runtime[48];unsigned i;
        init(&d,&w,&o);memset(runtime,0xcd,sizeof(runtime));
        d.RuntimePost=d.Post;d.RuntimePost.Width=1280;d.RuntimePost.Height=720;
        d.RuntimePost.Pitch=5120;d.RuntimePost.PhysicAddress.QuadPart=0x480000000ll;
        d.RuntimePostValid=TRUE;d.RuntimeFramebuffer=runtime;d.RuntimeFramebufferLength=sizeof(runtime);
        CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_SUCCESS);
        CHECK(info.Width==1280 && info.Height==720 && info.Pitch==5120 && info.PhysicAddress.QuadPart==0x480000000ll);
        CHECK(d.RuntimeFramebuffer==NULL && d.RuntimePostValid && postIs(0));
        for(i=0;i<sizeof(runtime);++i)CHECK(runtime[i]==0);
        CHECK(logged("post-display release: handed back runtime 1280x720 pitch 5120 address 480000000 target 2424833"));
    }
    /* BD-090 regression (31 line, 6de376e5 and f50880f4): an OS detach (an empty-topology commit) or a physical one
       turned the DP link off before the release, so RuntimeDisplayChanged is 1 and RuntimePostValid 0. The restore
       had nothing to put back, the release failed, and the next start got a POST display of zero size (Code 43).
       The release enables a target again (dispmprt.md, required steps 5 and 6) and hands that mode back. */
    {
        unsigned char runtime[48];unsigned i;
        init(&d,&w,&o);memset(runtime,0xcd,sizeof(runtime));
        d.RuntimeDisplayChanged=1;d.RuntimePostValid=0;d.RuntimeFramebuffer=runtime;d.RuntimeFramebufferLength=sizeof(runtime);
        d.RuntimePost=d.Post;d.RuntimePost.PhysicAddress=d.RuntimeFramebufferPhysical;   /* DcnLinkSurfaceAdopt */
        handoffModel.action=BC250_LINK_HANDOFF_ENABLED;memset(&info,0xcc,sizeof(info));
        CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_SUCCESS);
        CHECK(info.Width==1920 && info.Height==1200 && info.Pitch==7680);
        CHECK(info.PhysicAddress.QuadPart==0x480000000ll && info.TargetId==BC250_CHILD_UID);
        CHECK(handoffModel.calls==1 && model.restores==1 && model.unmaps==1 && d.StopDone && releases==1);
        for(i=0;i<sizeof(runtime);++i)CHECK(runtime[i]==0);
        CHECK(logged("post-display release: target 2424833 started 1 runtime changed 1 valid 0"));
        CHECK(logged("post-display release: link enabled detached-mode 1 timing 1920x1200 154000 kHz status 00000000"));
        CHECK(logged("post-display release: handed back runtime 1920x1200 pitch 7680 address 480000000 target 2424833"));
        CHECK(keptThrough==logCount);
    }
    /* The same detach with the enable refused: the release fails as before, and its line says why. */
    init(&d,&w,&o);d.RuntimeDisplayChanged=1;d.RuntimePostValid=0;
    handoffModel.action=BC250_LINK_HANDOFF_FAILED;handoffModel.status=STATUS_IO_DEVICE_ERROR;memset(&info,0xcc,sizeof(info));
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_DEVICE_NOT_READY);
    CHECK(info.Width==0 && info.Height==0 && info.Pitch==0 && info.PhysicAddress.QuadPart==0);
    CHECK(model.unmaps==1 && d.StopDone);
    CHECK(logged("post-display release: link enable-failed detached-mode 1 timing 1920x1200 154000 kHz status C0000185"));
    CHECK(logged("stop: display handoff restore C00000A3 hdmi 00000000 connector 00000000 result C00000A3"));
    CHECK(logged("post-display release: handed back nothing, status C00000A3: the target enable failed"));
    /* dispmprt.md, required step 2: no display connected. The stop still completes; the answer is
       STATUS_NOT_SUPPORTED, and dxgkrnl's StopDevice after it finds the teardown done. */
    init(&d,&w,&o);handoffModel.action=BC250_LINK_HANDOFF_NO_SINK;handoffModel.status=STATUS_NOT_SUPPORTED;memset(&info,0xcc,sizeof(info));
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_NOT_SUPPORTED);
    CHECK(info.Width==0 && info.Height==0 && info.Pitch==0 && info.PhysicAddress.QuadPart==0);
    CHECK(model.restores==1 && model.unmaps==1 && d.StopDone && releases==1);
    CHECK(logged("post-display release: link no-sink detached-mode 0 timing 0x0 0 kHz status C00000BB"));
    CHECK(logged("post-display release: handed back nothing, status C00000BB: no display connected"));
    CHECK(Bc250StopDevice(&d)==STATUS_SUCCESS && model.restores==1 && releases==1);
    printf("post display stop: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
