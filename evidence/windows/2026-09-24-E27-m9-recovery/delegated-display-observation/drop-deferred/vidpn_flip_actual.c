// Deterministic callback interleavings of actual source, not a multicore test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dcn_translate.h"
typedef int32_t NTSTATUS;
typedef long LONG;
typedef unsigned long ULONG;
typedef int64_t LONGLONG;
typedef uint64_t ULONGLONG;
typedef int BOOLEAN;
typedef void *HANDLE;
typedef struct { LONGLONG QuadPart; } PHYSICAL_ADDRESS;
#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011u)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define NT_SUCCESS(s) ((s)>=0)
#define DXGK_INTERRUPT_CRTC_VSYNC 1
#define WddmDdiSetVidPnSourceAddress 0
#define RtlZeroMemory(p,n) memset(p,0,n)
#define GuardLog(...) ((void)0)
typedef struct {
    HANDLE hAllocation;
    unsigned VidPnSourceId, PrimarySegment;
    PHYSICAL_ADDRESS PrimaryAddress;
    struct { unsigned Value; } Flags;
} DXGKARG_SETVIDPNSOURCEADDRESS;
typedef struct {
    unsigned InterruptType;
    struct { unsigned VidPnTargetId, PhysicalAdapterMask; PHYSICAL_ADDRESS PhysicalAddress; } CrtcVsync;
} DXGKARGCB_NOTIFY_INTERRUPT_DATA;
typedef struct {
    PHYSICAL_ADDRESS PrimaryAddress;
    volatile LONG PrimarySequence;
    ULONG PrimaryPitch; ULONGLONG PrimaryBytes;
    unsigned PrimarySegment;
    volatile LONG Flips, FlipsAboveDispatch, VSyncReports;
    int VSyncEnabled;
    unsigned VSyncTargetId;
} BC250_WDDM;
typedef struct {
    struct {ULONG Width,Height,Pitch;} Post;
    BC250_WDDM *Wddm;
    int VidPnFlipEnabled;
    volatile LONG DcnVsyncAcked, DcnVsyncDeferred;
} BC250_DEVICE;
#define BC250_WDDM_MAGIC_ALLOCATION 123
#define D3DDDIFMT_A8R8G8B8 21
#define D3DDDIFMT_X8R8G8B8 22
typedef struct {ULONG Magic; int UmdAlloc; struct {ULONG Width,Height,Pitch,Format;ULONGLONG Size;} Allocation;} BC250_WDDM_OBJECT;
static BC250_WDDM_OBJECT* WddmObject(HANDLE h,ULONG magic){BC250_WDDM_OBJECT *a=h;return a && a->Magic==magic?a:NULL;}
static ULONG programmed_pitch;
static unsigned checks,failures,hardware,arms,reports;
static int irq,pending,inject_update,nested_writer;
static NTSTATUS hardware_result;
static LONGLONG programmed,displayed,reported;
#define CHECK(x) do { ++checks; if(!(x)){++failures;printf("FAIL line %d: %s\n",__LINE__,#x);} } while(0)
static LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG compare){LONG old=*p;if(old==compare)*p=value;return old;}
static LONG InterlockedExchange(volatile LONG *p,LONG value){LONG old=*p;*p=value;return old;}
static LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p,LONGLONG value,LONGLONG compare){LONGLONG old=*p;if(old==compare)*p=value;return old;}
static LONGLONG InterlockedExchange64(volatile LONGLONG *p,LONGLONG value){LONGLONG old=*p;*p=value;return old;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static int KeGetCurrentIrql(void){return irq;}
static BC250_WDDM *WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static int WddmFirstCalls(BC250_WDDM *w,int call){(void)w;(void)call;return 0;}
static void WddmVSyncArm(BC250_DEVICE *d,int on){(void)d;CHECK(on && irq<=DISPATCH_LEVEL);++arms;}
static int WddmStopping(BC250_WDDM *w){(void)w;return 0;}
static void WddmReport(BC250_DEVICE *d,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data){(void)d;reports++;reported=data->CrtcVsync.PhysicalAddress.QuadPart;}
static BOOLEAN WddmReadCompletedPrimary(BC250_DEVICE*,BC250_WDDM*,PHYSICAL_ADDRESS*);
static NTSTATUS Bc250WddmSetVidPnSourceAddress(const HANDLE,const DXGKARG_SETVIDPNSOURCEADDRESS*);
void WddmDcnVsync(BC250_DEVICE*);
static NTSTATUS DcnFlipSourceAddress(BC250_DEVICE *d,ULONGLONG address,ULONG pitch,ULONGLONG bytes,ULONGLONG *out)
{
    PHYSICAL_ADDRESS sample={-1};
    unsigned before=reports;
    DXGKARG_SETVIDPNSOURCEADDRESS other={0};
    (void)out;
    CHECK(bytes==(ULONGLONG)pitch*d->Post.Height);
    hardware++;
    CHECK(!WddmReadCompletedPrimary(d,d->Wddm,&sample));
    CHECK(sample.QuadPart==-1);
    d->DcnVsyncAcked=1;
    WddmDcnVsync(d);
    CHECK(reports==before);
    if(nested_writer){
        nested_writer=0;
        other.PrimaryAddress.QuadPart=99;
        CHECK(Bc250WddmSetVidPnSourceAddress(d,&other)==STATUS_DEVICE_BUSY);
    }
    if(NT_SUCCESS(hardware_result)){programmed=(LONGLONG)address;programmed_pitch=pitch;}
    return hardware_result;
}
static BOOLEAN DcnFlipPending(BC250_DEVICE *d, ULONGLONG expected)
{
    if(inject_update){
        DXGKARG_SETVIDPNSOURCEADDRESS next={0};
        inject_update=0;
        next.PrimaryAddress.QuadPart=30;next.PrimarySegment=3;
        CHECK(Bc250WddmSetVidPnSourceAddress(d,&next)==STATUS_SUCCESS);
    }
    return pending || displayed != (LONGLONG)expected;
}
static NTSTATUS DcnReadScanoutAddress(BC250_DEVICE *d, ULONGLONG *out)
{ (void)d; *out=(ULONGLONG)displayed; return STATUS_SUCCESS; }
static BOOLEAN WddmReadCompletedPrimary(_In_ BC250_DEVICE* Device, _In_ BC250_WDDM* Wddm,
                                       _Out_ PHYSICAL_ADDRESS* Address)
{
    LONG generation = InterlockedCompareExchange(&Wddm->PrimarySequence, 0, 0);
    LONGLONG address;
    if (generation & 1) return FALSE;
    address = InterlockedCompareExchange64(&Wddm->PrimaryAddress.QuadPart, 0, 0);
    if (Device->VidPnFlipEnabled && DcnFlipPending(Device, (ULONGLONG)address)) return FALSE;
    if (InterlockedCompareExchange(&Wddm->PrimarySequence, 0, 0) != generation) return FALSE;
    Address->QuadPart = address;
    return TRUE;
}

static NTSTATUS Bc250WddmSetVidPnSourceAddress(_In_ const HANDLE hAdapter,
                                               _In_ const DXGKARG_SETVIDPNSOURCEADDRESS* pSetVidPnSourceAddress)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);

    // This is the one DDI in the table whose ceiling is above DISPATCH_LEVEL: d3dkmddi.h annotates it
    // _IRQL_requires_min_(PASSIVE_LEVEL) / _IRQL_requires_max_(PROFILE_LEVEL - 1), because a flip may be programmed
    // from the VSync interrupt itself. Everything below is therefore either interlocked or refused up there:
    // GuardLog takes a spin lock for its ring, and WddmVSyncArm takes ours and calls KeSetTimerEx, and neither is
    // legal at DIRQL. The arm being skipped costs nothing in practice - SetVidPnSourceVisibility runs at
    // PASSIVE_LEVEL and has armed the timer long before any flip arrives - and the count below says how often it
    // happened, so the assumption is measured rather than hoped for.
    const BOOLEAN high = (KeGetCurrentIrql() > DISPATCH_LEVEL);

    if (pSetVidPnSourceAddress->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (wddm == NULL) return STATUS_DEVICE_NOT_READY;
    {
        ULONG generation = (ULONG)InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0);
        BOOLEAN changed;
        ULONG pitch;
        ULONGLONG bytes;
        NTSTATUS status = STATUS_SUCCESS;
        // Nonblocking ownership: a high-IRQL caller must never spin behind a
        // preempted lower-IRQL programmer. The generation also protects vsync's
        // address + FLIP_PENDING snapshot from crossing this transaction.
        if ((generation & 1u) ||
            (ULONG)InterlockedCompareExchange(&wddm->PrimarySequence, (LONG)(generation + 1u),
                                             (LONG)generation) != generation)
            return STATUS_DEVICE_BUSY;
        // A NULL allocation preserves current private properties; initially POST.
        pitch=wddm->PrimaryPitch?wddm->PrimaryPitch:device->Post.Pitch;
        if (!DcnSurfaceBytes(device->Post.Width,device->Post.Height,pitch,&bytes)) status=STATUS_INVALID_PARAMETER;
        if (pSetVidPnSourceAddress->hAllocation)
        {
            BC250_WDDM_OBJECT* allocation=WddmObject(pSetVidPnSourceAddress->hAllocation,BC250_WDDM_MAGIC_ALLOCATION);
            if (!allocation || allocation->UmdAlloc || allocation->Allocation.Width!=device->Post.Width ||
                allocation->Allocation.Height!=device->Post.Height ||
                (allocation->Allocation.Format!=D3DDDIFMT_A8R8G8B8 && allocation->Allocation.Format!=D3DDDIFMT_X8R8G8B8) ||
                !DcnSurfaceBytes(allocation->Allocation.Width,allocation->Allocation.Height,allocation->Allocation.Pitch,&bytes) ||
                bytes>allocation->Allocation.Size) status=STATUS_INVALID_PARAMETER;
            else { pitch=allocation->Allocation.Pitch; status=STATUS_SUCCESS; }
        }
        changed = InterlockedCompareExchange64(&wddm->PrimaryAddress.QuadPart, 0, 0) !=
                  pSetVidPnSourceAddress->PrimaryAddress.QuadPart || pitch!=wddm->PrimaryPitch;
        if (high) InterlockedIncrement(&wddm->FlipsAboveDispatch);
        if (NT_SUCCESS(status) && changed && device->VidPnFlipEnabled)
            status=DcnFlipSourceAddress(device,(ULONGLONG)pSetVidPnSourceAddress->PrimaryAddress.QuadPart,pitch,bytes,NULL);
        if (NT_SUCCESS(status))
        {
            // Publish only after the programming sequence succeeds. A refused
            // request leaves both fields and Flips unchanged; the same address
            // can be submitted again and still takes the hardware path.
            InterlockedExchange64(&wddm->PrimaryAddress.QuadPart, pSetVidPnSourceAddress->PrimaryAddress.QuadPart);
            wddm->PrimarySegment = pSetVidPnSourceAddress->PrimarySegment;
            wddm->PrimaryPitch=pitch;wddm->PrimaryBytes=bytes;
            if (changed) InterlockedIncrement(&wddm->Flips);
        }
        InterlockedExchange(&wddm->PrimarySequence, (LONG)(generation + 2u));
        if (!NT_SUCCESS(status)) return status;
    }
    if (WddmFirstCalls(wddm, WddmDdiSetVidPnSourceAddress))     // FALSE above DISPATCH_LEVEL, count taken all the same
        GuardLog("wddm: SetVidPnSourceAddress segment %u address 0x%llX flags 0x%08X (firmware framebuffer 0x%llX)",
                 pSetVidPnSourceAddress->PrimarySegment, (ULONGLONG)pSetVidPnSourceAddress->PrimaryAddress.QuadPart,
                 pSetVidPnSourceAddress->Flags.Value, (ULONGLONG)device->Post.PhysicAddress.QuadPart);
    if (!high) WddmVSyncArm(device, TRUE);      // a flip has been queued: something must retire it
    return STATUS_SUCCESS;
}

void WddmDcnVsync(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;

    if (wddm == NULL || !Device->VidPnFlipEnabled) return;
    if (InterlockedExchange(&Device->DcnVsyncAcked, 0) == 0) return;
    if (WddmStopping(wddm) || !wddm->VSyncEnabled) return;

    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    data.CrtcVsync.VidPnTargetId = wddm->VSyncTargetId;
    // A stable completed request may be retired. Otherwise preserve this vblank
    // by reporting the buffer hardware is still reading, never the queued one.
    if (!WddmReadCompletedPrimary(Device, wddm, &data.CrtcVsync.PhysicalAddress))
    {
        ULONGLONG scanned;
        LONG generation;
        InterlockedIncrement(&Device->DcnVsyncDeferred); // completion deferred, not necessarily the vblank
        generation = InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0);
        if (generation & 1) return;
        if (!NT_SUCCESS(DcnReadScanoutAddress(Device, &scanned))) return;
        // If the pending bit outlives the address latch, reporting the requested
        // address would still retire the flip early. Only the distinct previous
        // buffer is safe here; a matching address needs the completed path above.
        if (scanned == (ULONGLONG)InterlockedCompareExchange64(&wddm->PrimaryAddress.QuadPart, 0, 0)) return;
        if (InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0) != generation) return;
return; /* negative control: lose the valid old-buffer vblank */
    }
    data.CrtcVsync.PhysicalAdapterMask = 0;
    InterlockedIncrement(&wddm->VSyncReports);
    WddmReport(Device, &data);
}

int main(void)
{
    int level;
    for(level=0;level<2;level++){
        BC250_WDDM w={0};BC250_DEVICE d={0};
        DXGKARG_SETVIDPNSOURCEADDRESS request={0};
        PHYSICAL_ADDRESS sample={-1};
        unsigned before;
        d.Post.Width=1366;d.Post.Height=768;d.Post.Pitch=5632;w.PrimaryPitch=5632;
        d.Wddm=&w;d.VidPnFlipEnabled=1;w.VSyncEnabled=1;
        w.PrimaryAddress.QuadPart=10;w.PrimarySegment=1;
        request.PrimaryAddress.QuadPart=20;request.PrimarySegment=2;
        irq=level?3:0;hardware=arms=reports=0;pending=inject_update=nested_writer=0;
        hardware_result=STATUS_IO_TIMEOUT;programmed=displayed=10;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT);
        CHECK(w.PrimaryAddress.QuadPart==10 && w.PrimarySegment==1 && w.Flips==0);
        CHECK(programmed==10 && hardware==1 && arms==0 && !(w.PrimarySequence&1));
        before=reports;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==10);
        hardware_result=STATUS_SUCCESS;nested_writer=1;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
        CHECK(programmed==20 && w.PrimaryAddress.QuadPart==20 && w.PrimarySegment==2);
        CHECK(w.Flips==1 && hardware==2 && !(w.PrimarySequence&1));
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
        CHECK(hardware==2 && w.Flips==1);
        // Every vblank reports; neither pending nor cleared-before-inuse retires 20.
        before=reports;pending=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==10);
        before=reports;pending=0;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==10);
        // Matching address alone cannot override an asserted pending bit.
        before=reports;displayed=20;pending=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before);
        pending=0;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==20);
        // A whole new transaction during the observation invalidates completion.
        inject_update=1;
        CHECK(!WddmReadCompletedPrimary(&d,&w,&sample));
        CHECK(sample.QuadPart==-1);
        CHECK(!WddmReadCompletedPrimary(&d,&w,&sample));
        CHECK(sample.QuadPart==-1 && w.PrimaryAddress.QuadPart==30);
        displayed=30;
        CHECK(WddmReadCompletedPrimary(&d,&w,&sample) && sample.QuadPart==30);
        CHECK(w.PrimarySegment==3 && hardware==3);
        CHECK(level ? arms==0 : arms==3);
        request.VidPnSourceId=1;CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
        CHECK(hardware==3);
        {
            BC250_WDDM_OBJECT allocation={0};
            allocation.Magic=BC250_WDDM_MAGIC_ALLOCATION;
            allocation.Allocation.Width=1366;allocation.Allocation.Height=768;
            allocation.Allocation.Pitch=5888;allocation.Allocation.Format=D3DDDIFMT_A8R8G8B8;
            allocation.Allocation.Size=5888ull*768;
            request.VidPnSourceId=0;request.PrimaryAddress.QuadPart=30;
            request.hAllocation=&allocation;
            hardware_result=STATUS_IO_TIMEOUT;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT);
            CHECK(w.PrimaryPitch==5632 && hardware==4 && programmed_pitch==5632);
            hardware_result=STATUS_SUCCESS;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
            CHECK(w.PrimaryPitch==5888 && hardware==5 && programmed_pitch==5888);
            request.hAllocation=NULL;request.PrimaryAddress.QuadPart=40;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
            CHECK(programmed_pitch==5888 && w.PrimaryBytes==5888ull*768);
            request.hAllocation=&allocation;allocation.Allocation.Size--;
            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(hardware==6 && !(w.PrimarySequence&1));
        }
    }
    printf("vidpn publication: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
