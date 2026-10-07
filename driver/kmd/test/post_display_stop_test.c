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
    int HwPending,PagingHwPending,Lock,Stopping,VSyncArmed,VSyncTimer,SubmitTimer,PagingSubmitTimer,PagingDrainTimer;
    int SubmitDpc,PagingSubmitDpc,PagingDrainDpc,VSyncDpc,ReportDpc;
    long LastCompletedFence,VSyncTicks;
    BC250_PAGING_JOB *PagingHead,*PagingTail;
    LIST_ENTRY Objects;
    struct {void *Buckets;} ObjectIndex;
} BC250_WDDM;
typedef struct {
    BC250_WDDM *Wddm;
    int Smu,Hwmon,Started,ModeActive,SourceVisible,CommitSeen,PresentSeen,FullWddm,SystemDisplayReady;
    volatile long DcnVsyncArmed;
    BOOLEAN InheritedSignalValid;
    BOOLEAN PostDisplayStopAttempted;
    NTSTATUS PostDisplayStopStatus;
    BOOLEAN StopDone;
    void *Framebuffer;
    size_t FramebufferLength;
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
/* Reset by init(), so these are "in this run", unlike the totals the stubs below count. */
static struct {unsigned joined,smu,restores,vidmm,objects,unmaps,dpm;NTSTATUS result;} model;
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
static void*freedOrder[4];
static void ExFreePoolWithTag(void*p,int tag){(void)tag;CHECK(model.restores==1 && model.vidmm==1);if(model.objects<4)freedOrder[model.objects]=p;model.objects++;}
static void HangDetectorStop(void){CHECK(model.smu==0 && model.restores==0);}   /* KMD172: before any teardown */
static void StartHealthClose(BC250_DEVICE*d){(void)d;}
static void GuardStage(int s){(void)s;}
static void GuardLogKeep(void){}
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
static void HwmonStop(int*h){(void)h;CHECK(model.dpm==1 && model.smu==0);hwmonStops++;}
/* The case fan goes back to the board after DpmStop (no fan step runs any more) and before the reader it
   reads its inputs from closes, while the SMU owner is still online (fan.c, docs/design/fan.md Part B). */
#define BC250_FAN_REASON_STOP 2u
static unsigned fanStops;
static void FanStop(BC250_DEVICE*d,unsigned r){(void)d;CHECK(r==BC250_FAN_REASON_STOP && model.dpm==1 && model.smu==0 && hwmonStops==fanStops && metricsStops==fanStops+1);fanStops++;}
static unsigned dpaudioStops;
/* DP audio: AUDIO_ENABLED goes to 0 while BAR5 and the display block are still ours, before the SMU owner goes
   offline and before WddmStop/DcnStop restore the surface. */
static void DpAudioStop(BC250_DEVICE*d){(void)d;CHECK(model.dpm==1 && model.smu==0 && model.restores==0);dpaudioStops++;}
static void SmuOwnerStop(int*s){(void)s;model.smu++;}
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
    d->Wddm=w;d->Framebuffer=post;d->FramebufferLength=sizeof(post);d->FullWddm=1;d->DcnVsyncArmed=1;w->VSyncArmed=1;d->Started=1;
    d->Post.Width=1920;d->Post.Height=1200;d->Post.Pitch=7680;d->Post.PhysicAddress=0x470000000ull;
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
    CHECK(info.Width==1920 && info.Height==1200 && info.Pitch==7680 && info.PhysicAddress==d.Post.PhysicAddress && info.TargetId==BC250_CHILD_UID);
    CHECK(d.PostDisplayStopAttempted && d.PostDisplayStopStatus==STATUS_SUCCESS);
    CHECK(postIs(0) && releases==1 && d.StopDone);  /* WDDM 1.2: black before the visible handover; BD-090 give-back */
    init(&d,&w,&o);model.result=STATUS_IO_TIMEOUT;memset(&info,0xcc,sizeof(info));
    CHECK(Bc250StopDeviceAndReleasePostDisplayOwnership(&d,BC250_CHILD_UID,&info)==STATUS_IO_TIMEOUT);
    CHECK(info.Width==0 && info.Height==0 && info.Pitch==0 && info.PhysicAddress==0);
    CHECK(model.restores==1 && model.unmaps==1 && d.PostDisplayStopStatus==STATUS_IO_TIMEOUT);
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
    CHECK(stopEpisodes==4);    /* one per stop, and the post-display stop goes through the ordinary one */
    printf("post display stop: %u checks, %u failures\n",checks,failures);return failures?1:0;
}
