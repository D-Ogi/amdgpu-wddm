// Deterministic callback interleavings of actual source, not a multicore test.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dcn_translate.h"
#include "surface_format.h"
#include "scanout_admit.h"
typedef int32_t NTSTATUS;
typedef long LONG;
typedef unsigned long ULONG;
typedef int64_t LONGLONG;
typedef int64_t LONG64;
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
#define BC250_WDDM_SEGMENT_VRAM 1u
#define GuardLog(...) ((void)0)
// DXGK_SETVIDPNSOURCEADDRESS_FLAGS, in the DECLARATION ORDER of the WDK's shared/d3dkmddi.h (10.0.26100,
// around line 6213). The order is the ABI and the trailing comments in that header are not: they give
// 0x00000010 for both FlipStereoTemporaryMono and FlipStereoPreferRight and are shifted by one bit from
// there on, while Reserved:23 after nine named bits fixes the layout. The driver therefore reads these as
// bitfields and never as transcribed masks, and FlipFlagBits() below asserts what this copy comes out as,
// so a transcription error in this fixture cannot pass either.
typedef struct {
    HANDLE hAllocation;
    unsigned VidPnSourceId, PrimarySegment;
    PHYSICAL_ADDRESS PrimaryAddress;
    union {
        struct {
            unsigned ModeChange:1;
            unsigned FlipImmediate:1;
            unsigned FlipOnNextVSync:1;
            unsigned FlipStereo:1;
            unsigned FlipStereoTemporaryMono:1;
            unsigned FlipStereoPreferRight:1;
            unsigned SharedPrimaryTransition:1;
            unsigned IndependentFlipExclusive:1;
            unsigned MoveFlip:1;
            unsigned Reserved:23;
        };
        unsigned Value;
    } Flags;
} DXGKARG_SETVIDPNSOURCEADDRESS;
typedef struct {
    unsigned InterruptType;
    struct { unsigned VidPnTargetId, PhysicalAdapterMask; PHYSICAL_ADDRESS PhysicalAddress; } CrtcVsync;
} DXGKARGCB_NOTIFY_INTERRUPT_DATA;
typedef struct {
    PHYSICAL_ADDRESS PrimaryAddress;
    volatile LONG PrimarySequence, PrimaryProgrammedSequence;
    BOOLEAN PrimaryNeedsRestore;
    ULONG PrimaryPitch; ULONGLONG PrimaryBytes;
    unsigned PrimarySegment;
    volatile LONG Flips, FlipsAboveDispatch, VSyncReports;
    volatile LONG ScanoutFlips, ScanoutRequests, ScanoutAdmits[BC250_SCANOUT_STATUSES], ScanoutNotes, ScanoutTeardowns;
    /* M15.14 increment 2: the OS's own flip mode, per bit, for every accepted address call. */
    volatile LONG ScanoutFlipFlags[4];
    volatile LONGLONG ScanoutObject;
    BOOLEAN ScanoutAdmitGate;       /* EnableScanoutAdmit, absent = on (WddmStart) */
    int VSyncEnabled;
    unsigned VSyncTargetId;
} BC250_WDDM;
typedef struct {
    struct {ULONG Width,Height,Pitch;} Post;
    BC250_WDDM *Wddm;
    // M15.14: the two bases the card address is translated through. The scan-out clause refuses a
    // candidate when either is not page aligned, because the predicate's 4 KiB test is on the card
    // address and the plane is given the physical one.
    ULONGLONG VramMcBase;
    PHYSICAL_ADDRESS VramPhysical;
    int VidPnFlipEnabled;
    volatile LONG DcnVsyncAcked, DcnVsyncDeferred, DcnVsyncOldBufferReports;
    volatile LONG DcnVsyncSkipOddGeneration, DcnVsyncSkipReadFailure, DcnVsyncSkipSameAddress, DcnVsyncSkipChangedGeneration;
    volatile LONG64 DcnVsyncSkipOddGenerationTime, DcnVsyncSkipReadFailureTime, DcnVsyncSkipSameAddressTime,
        DcnVsyncSkipChangedGenerationTime;
} BC250_DEVICE;
/* Display modes (bc250kmd.h): the source size is the committed mode when one is set, else the inherited mode.
 * This fixture commits no mode, so the source size is always the inherited one. */
static ULONG DisplaySourceWidth(const BC250_DEVICE *d){return d->Post.Width;}
static ULONG DisplaySourceHeight(const BC250_DEVICE *d){return d->Post.Height;}
#define BC250_WDDM_MAGIC_ALLOCATION 123
#define BC250_WDDM_MAGIC_RESOURCE 124
#define WddmDdiDestroyAllocation 1
#define BC250_WDDM_LOG_CALLS 8
#define BC250_PJ_FLAG_UMD_ALLOCATION 1u
typedef unsigned UINT;
typedef unsigned long long ULONG_PTR;
#define D3DDDIFMT_A8R8G8B8 21
#define D3DDDIFMT_X8R8G8B8 22
#define D3DDDIFMT_A8B8G8R8 32
/* Serial: adapter-unique, nonzero, never reused (wddm.c, WddmNewObject under the lock). The teardown record
 * holds it instead of the object's address, so a value that outlived its object cannot match a later object
 * allocated where the old one was. */
typedef struct {ULONG Magic; ULONGLONG Serial; int UmdAlloc; struct {ULONG Width,Height,Pitch,Format;ULONGLONG Size;} Allocation;
    int ScanoutRequested; ULONG ScanoutWidth,ScanoutHeight,ScanoutPitch,ScanoutFormat; ULONGLONG UmdBytes;
    ULONGLONG UmdRequestedVa,UmdGemFlags; ULONG CreatorProcessId,UmdBlobVersion;} BC250_WDDM_OBJECT;
static BC250_WDDM_OBJECT* WddmObject(HANDLE h,ULONG magic){BC250_WDDM_OBJECT *a=h;return a && a->Magic==magic?a:NULL;}
typedef struct {
    UINT NumAllocations;
    HANDLE *pAllocationList, hResource;
    struct { int DestroyResource; } Flags;
} DXGKARG_DESTROYALLOCATION;
static unsigned freed,journaled,restores;
static NTSTATUS restore_result;
static void PagingJournalDestroy(ULONGLONG va,HANDLE h,ULONGLONG bytes,unsigned flags,ULONG pid,ULONG version,ULONGLONG gem)
{(void)va;(void)h;(void)bytes;(void)flags;(void)pid;(void)version;(void)gem;++journaled;}
static void WddmFreeObject(BC250_WDDM_OBJECT *o){if(o)++freed;}
static NTSTATUS DcnRestorePostDisplay(BC250_DEVICE *d){(void)d;++restores;return restore_result;}
static ULONG programmed_pitch;
static unsigned checks,failures,hardware,arms,reports;
static int irq,pending,inject_update,nested_writer,inject_fallback_update;
/* The one interleaving the lifetime tie exists for: dxgkrnl frees the buffer of a flip that is still inside
 * SetVidPnSourceAddress, with the plane already written. Set this to the allocation handle the destroy carries,
 * and DcnFlipSourceAddress runs that destroy at the moment the plane becomes the new buffer's. */
static HANDLE inject_destroy;
static NTSTATUS hardware_result;
static LONGLONG programmed,displayed,reported;
#define CHECK(x) do { ++checks; if(!(x)){++failures;printf("FAIL line %d: %s\n",__LINE__,#x);} } while(0)
static LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG compare){LONG old=*p;if(old==compare)*p=value;return old;}
static LONG InterlockedExchange(volatile LONG *p,LONG value){LONG old=*p;*p=value;return old;}
static LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p,LONGLONG value,LONGLONG compare){LONGLONG old=*p;if(old==compare)*p=value;return old;}
static LONGLONG InterlockedExchange64(volatile LONGLONG *p,LONGLONG value){LONGLONG old=*p;*p=value;return old;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static int KeGetCurrentIrql(void){return irq;}
static ULONGLONG KeQueryInterruptTime(void){return 1;}
static BC250_WDDM *WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static int WddmFirstCalls(BC250_WDDM *w,int call){(void)w;(void)call;return 0;}
static void WddmVSyncArm(BC250_DEVICE *d,int on){(void)d;CHECK(on && irq<=DISPATCH_LEVEL);++arms;}
static int WddmStopping(BC250_WDDM *w){(void)w;return 0;}
/* C48: the vblank time the ring-gap accounting keys on. Counted here, not timed: what the fixture has to
 * prove is that it is taken for EVERY acknowledged vblank, before any of the deferral paths can return. */
static int ringGapVsyncs;
static void WddmRingGapVsync(BC250_WDDM *w){(void)w;++ringGapVsyncs;}
/* C50: a CRTC_VSYNC report owes the same DPC-level notification as any other, so this path has to count one -
 * counting only the completion path made the summary's "notifications carried no report" wrong by one a vblank.
 * The stub insists on the vsync flavour, because the only reports this fixture can produce are vblank ones. */
static unsigned pairedVsyncReports;
static void WddmPairingReport(BC250_WDDM *w,BOOLEAN vsync){(void)w;CHECK(vsync);++pairedVsyncReports;}
/* PairInThisPass must stay FALSE here: WddmDcnVsync has no notification of its own behind it, so the report still
 * needs the dxgkrnl DPC that WddmReport queues. A TRUE would leave this report unpaired for ever. */
static BOOLEAN WddmReport(BC250_DEVICE *d,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data,BOOLEAN pairInPass)
{(void)d;CHECK(!pairInPass);reports++;reported=data->CrtcVsync.PhysicalAddress.QuadPart;return TRUE;}
static BOOLEAN WddmReadCompletedPrimary(BC250_DEVICE*,BC250_WDDM*,PHYSICAL_ADDRESS*,ULONG*);
static NTSTATUS Bc250WddmSetVidPnSourceAddress(const HANDLE,const DXGKARG_SETVIDPNSOURCEADDRESS*);
static NTSTATUS Bc250WddmDestroyAllocation(const HANDLE,const DXGKARG_DESTROYALLOCATION*);
void WddmDcnVsync(BC250_DEVICE*);
static NTSTATUS DcnFlipSourceAddress(BC250_DEVICE *d,ULONGLONG address,ULONG pitch,ULONGLONG bytes,ULONGLONG *out)
{
    PHYSICAL_ADDRESS sample={-1};
    unsigned before=reports;
    DXGKARG_SETVIDPNSOURCEADDRESS other={0};
    (void)out;
    CHECK(bytes==(ULONGLONG)pitch*d->Post.Height);
    hardware++;
    CHECK(!WddmReadCompletedPrimary(d,d->Wddm,&sample,NULL));
    CHECK(sample.QuadPart==-1);
    d->DcnVsyncAcked=1;
    WddmDcnVsync(d);
    CHECK(reports==before);
    if(nested_writer){
        nested_writer=0;
        other.PrimaryAddress.QuadPart=99;
        CHECK(Bc250WddmSetVidPnSourceAddress(d,&other)==STATUS_DEVICE_BUSY);
    }
    if(inject_destroy){
        DXGKARG_DESTROYALLOCATION destroy={0};
        HANDLE list[1];
        list[0]=inject_destroy;inject_destroy=0;
        destroy.NumAllocations=1;destroy.pAllocationList=list;
        CHECK(Bc250WddmDestroyAllocation(d,&destroy)==STATUS_SUCCESS);
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
{
    *out=(ULONGLONG)displayed;
    if(inject_fallback_update){inject_fallback_update=0;d->Wddm->PrimarySequence+=2;}
    return STATUS_SUCCESS;
}
static ULONG healthSequence;
static unsigned healthCompletions;
static void StartHealthCompleted(BC250_DEVICE *d,ULONG sequence)
{
    (void)d;
    if(sequence && sequence!=healthSequence){healthSequence=sequence;healthCompletions++;}
}
/* ACTUAL_SOURCE */
/* M15.14 increment 2: the kernel-side witness of the OS's flip mode. Two things are asserted, because the
 * counters were declared and written by nothing at all before this, and because the one way to get them
 * wrong is to copy a number out of the WDK header's trailing comments.
 *   First, that the four named bits of DXGK_SETVIDPNSOURCEADDRESS_FLAGS land where the declaration order
 * puts them, not where those comments say: SharedPrimaryTransition is 0x40 and IndependentFlipExclusive is
 * 0x80. An analyser or a lab predicate reading 0x20 and 0x40 would call a run that did enter independent
 * flip a run that never did.
 *   Second, that the driver counts every accepted address call, admitted or refused, and does not obey any
 * of the bits. A refusal arriving after the OS has taken a SharedPrimaryTransition is the shape that blanks
 * the output (the OS does not fall back to composition seamlessly), so that case must be visible. */
static void FlipFlagBits(void)
{
    BC250_WDDM w={0};BC250_DEVICE d={0};
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT allocation={BC250_WDDM_MAGIC_ALLOCATION};
    unsigned written;

    request.Flags.Value=0;request.Flags.ModeChange=1;CHECK(request.Flags.Value==0x00000001u);
    request.Flags.Value=0;request.Flags.FlipImmediate=1;CHECK(request.Flags.Value==0x00000002u);
    request.Flags.Value=0;request.Flags.FlipOnNextVSync=1;CHECK(request.Flags.Value==0x00000004u);
    request.Flags.Value=0;request.Flags.FlipStereo=1;CHECK(request.Flags.Value==0x00000008u);
    request.Flags.Value=0;request.Flags.FlipStereoTemporaryMono=1;CHECK(request.Flags.Value==0x00000010u);
    request.Flags.Value=0;request.Flags.FlipStereoPreferRight=1;CHECK(request.Flags.Value==0x00000020u);
    request.Flags.Value=0;request.Flags.SharedPrimaryTransition=1;CHECK(request.Flags.Value==0x00000040u);
    request.Flags.Value=0;request.Flags.IndependentFlipExclusive=1;CHECK(request.Flags.Value==0x00000080u);
    request.Flags.Value=0;request.Flags.MoveFlip=1;CHECK(request.Flags.Value==0x00000100u);

    d.Post.Width=1366;d.Post.Height=768;d.Post.Pitch=5632;w.PrimaryPitch=5632;
    d.Wddm=&w;d.VidPnFlipEnabled=1;w.VSyncEnabled=1;w.ScanoutAdmitGate=TRUE;
    w.PrimaryAddress.QuadPart=0x1000;w.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    irq=0;hardware_result=STATUS_SUCCESS;hardware=0;programmed=displayed=0x1000;
    /* The compositor's own primary, with the two bits that say the OS entered DirectFlip and then
     * independent flip. Both are counted and neither changes the answer. */
    request.hAllocation=NULL;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    request.PrimaryAddress.QuadPart=0x2000;
    request.Flags.Value=0;request.Flags.SharedPrimaryTransition=1;request.Flags.IndependentFlipExclusive=1;
    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
    CHECK(w.ScanoutFlipFlags[0]==0 && w.ScanoutFlipFlags[1]==0);
    CHECK(w.ScanoutFlipFlags[2]==1 && w.ScanoutFlipFlags[3]==1);
    request.PrimaryAddress.QuadPart=0x3000;
    request.Flags.Value=0;request.Flags.ModeChange=1;request.Flags.FlipImmediate=1;
    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
    CHECK(w.ScanoutFlipFlags[0]==1 && w.ScanoutFlipFlags[1]==1);
    CHECK(w.ScanoutFlipFlags[2]==1 && w.ScanoutFlipFlags[3]==1);
    /* A refused candidate still counts its flags: the OS has already transitioned, and that is exactly the
     * run an operator has to be able to see. Nothing was programmed. */
    allocation.UmdAlloc=1;allocation.ScanoutRequested=1;allocation.UmdBytes=4096;
    allocation.ScanoutWidth=1;allocation.ScanoutHeight=1;allocation.ScanoutPitch=4;
    allocation.ScanoutFormat=D3DDDIFMT_A8R8G8B8;
    written=hardware;
    request.hAllocation=&allocation;request.PrimaryAddress.QuadPart=0x4000;
    request.Flags.Value=0;request.Flags.SharedPrimaryTransition=1;
    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
    CHECK(w.ScanoutFlipFlags[2]==2 && hardware==written && w.ScanoutFlips==0);
    /* A call refused before the transaction is not counted twice: VidPnSourceId != 0 never gets that far. */
    request.VidPnSourceId=1;
    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
    CHECK(w.ScanoutFlipFlags[2]==2);
    request.VidPnSourceId=0;
}
int main(void)
{
    int level;
    for(level=0;level<2;level++){
        BC250_WDDM w={0};BC250_DEVICE d={0};
        DXGKARG_SETVIDPNSOURCEADDRESS request={0};
        PHYSICAL_ADDRESS sample={-1};
        unsigned before;
        int gapVsyncsBefore;
        d.Post.Width=1366;d.Post.Height=768;d.Post.Pitch=5632;w.PrimaryPitch=5632;
        d.Wddm=&w;d.VidPnFlipEnabled=1;w.VSyncEnabled=1;w.ScanoutAdmitGate=TRUE;
        w.PrimaryAddress.QuadPart=10;w.PrimarySegment=1;
        request.PrimaryAddress.QuadPart=20;request.PrimarySegment=2;
        healthSequence=healthCompletions=0;
        irq=level?3:0;hardware=arms=reports=pairedVsyncReports=0;pending=inject_update=nested_writer=0;
        hardware_result=STATUS_IO_TIMEOUT;programmed=displayed=10;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT);
        CHECK(w.PrimaryAddress.QuadPart==10 && w.PrimarySegment==1 && w.Flips==0);
        CHECK(programmed==10 && hardware==1 && arms==0 && !(w.PrimarySequence&1));
        before=reports;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==10);
        CHECK(healthCompletions==0); // rejected flip is not progress
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
        CHECK(d.DcnVsyncOldBufferReports==2);
        CHECK(healthCompletions==0); // old-buffer fallback is not progress
        // Fallback is also generation-protected, even for an ABA writer.
        before=reports;gapVsyncsBefore=ringGapVsyncs;pending=1;inject_fallback_update=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before);
        // C48: the report was refused, the vblank still happened and its time was taken.
        CHECK(ringGapVsyncs==gapVsyncsBefore+1);
        // Matching address alone cannot override an asserted pending bit.
        before=reports;gapVsyncsBefore=ringGapVsyncs;displayed=20;pending=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before && ringGapVsyncs==gapVsyncsBefore+1);
        pending=0;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==20);
        CHECK(healthCompletions==1);
        d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(healthCompletions==1); // repeated completed primary is not progress
        // C50: every report this fixture made is a vblank one, and every one of them was counted as owing a
        // notification. A refused vblank (above) counts no report and must not count a pairing either.
        CHECK(pairedVsyncReports==reports);
        // A whole new transaction during the observation invalidates completion.
        inject_update=1;
        CHECK(!WddmReadCompletedPrimary(&d,&w,&sample,NULL));
        CHECK(sample.QuadPart==-1);
        CHECK(!WddmReadCompletedPrimary(&d,&w,&sample,NULL));
        CHECK(sample.QuadPart==-1 && w.PrimaryAddress.QuadPart==30);
        displayed=30;
        CHECK(WddmReadCompletedPrimary(&d,&w,&sample,NULL) && sample.QuadPart==30);
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
            {
                // The flip never programs a pixel format: only the firmware plane's own two are scanned
                // out. A8B8G8R8, A2B10G10R10 (DXGI R10G10B10A2) and the rest are refused before hardware.
                static const ULONG refused[]={0,28,31,32,33,35,113};
                unsigned k;
                allocation.Allocation.Size=5888ull*768;request.PrimaryAddress.QuadPart=50;
                for(k=0;k<sizeof(refused)/sizeof(refused[0]);k++){
                    allocation.Allocation.Format=refused[k];
                    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                }
                CHECK(hardware==6 && w.PrimaryAddress.QuadPart==40 && !(w.PrimarySequence&1));
                allocation.Allocation.Format=D3DDDIFMT_X8R8G8B8;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                CHECK(hardware==7 && w.PrimaryAddress.QuadPart==50 && programmed_pitch==5888);
            }
            // M15.14: the inherited primary above asked for no scan-out, so none of these flips counted
            // as one, and dxgkrnl's shared primary reached the hardware all the same.
            CHECK(w.ScanoutFlips==0 && w.ScanoutRequests==0);
            {
                // An application allocation. Without a scan-out request it is refused exactly as every
                // UMD allocation was before this revision, and the refusal is now named.
                BC250_WDDM_OBJECT umd={0};
                LONG flips=w.Flips,hw=(LONG)hardware;
                umd.Magic=BC250_WDDM_MAGIC_ALLOCATION;umd.UmdAlloc=1;
                umd.ScanoutWidth=1366;umd.ScanoutHeight=768;umd.ScanoutPitch=5632;
                umd.ScanoutFormat=D3DDDIFMT_A8R8G8B8;umd.UmdBytes=5632ull*768;
                request.hAllocation=&umd;request.PrimaryAddress.QuadPart=0x4000;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_NOT_REQUESTED]==1 && w.ScanoutRequests==0);
                CHECK(w.Flips==flips && (LONG)hardware==hw && !(w.PrimarySequence&1));
                // The same allocation, now asking for scan-out and describing the POST surface: the
                // first flip of an application buffer this driver has ever programmed.
                umd.ScanoutRequested=1;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                CHECK(w.ScanoutRequests==1 && w.ScanoutAdmits[BC250_SCANOUT_ADMIT_OK]>=1);
                CHECK(w.Flips==flips+1 && w.ScanoutFlips==1 && programmed_pitch==5632);
                CHECK(w.PrimaryAddress.QuadPart==0x4000 && w.PrimaryBytes==5632ull*768);
                // Each refusal of a described surface: the wrong segment, an unaligned address, a
                // composed-only format, and geometry that is not the POST mode's. None reaches the
                // hardware, none publishes, and the plane keeps the surface it has.
                hw=(LONG)hardware;flips=w.Flips;
                {LONG formats=w.ScanoutAdmits[BC250_SCANOUT_FORMAT],sizes=w.ScanoutAdmits[BC250_SCANOUT_SIZE];
                request.PrimaryAddress.QuadPart=0x8000;request.PrimarySegment=2;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_SEGMENT]==1);
                request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x8010;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==1);
                request.PrimaryAddress.QuadPart=0x8000;umd.ScanoutFormat=D3DDDIFMT_A8B8G8R8;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_FORMAT]==formats+1);
                umd.ScanoutFormat=D3DDDIFMT_A8R8G8B8;umd.ScanoutHeight=767;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_GEOMETRY]==1);
                umd.ScanoutHeight=768;umd.UmdBytes=5632ull*768-1;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_SIZE]==sizes+1);
                }
                CHECK((LONG)hardware==hw && w.Flips==flips && w.ScanoutFlips==1 && !(w.PrimarySequence&1));
                CHECK(w.PrimaryAddress.QuadPart==0x4000 && w.PrimaryPitch==5632);
                // A handle that is not an allocation of this adapter.
                request.hAllocation=(HANDLE)&w;
                CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_NO_ALLOCATION]==1);
                {
                    // The two bases the card address is translated through. The predicate's 4 KiB test
                    // is on the card address; the plane is given Address - VramMcBase + VramPhysical,
                    // so page alignment only carries over while both bases are page aligned, and a
                    // board where they are not refuses scan-out instead of programming the plane.
                    BC250_WDDM_OBJECT good={0};
                    DXGKARG_DESTROYALLOCATION destroy={0};
                    HANDLE list[1];
                    LONG aligned=w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT];
                    LONG notes;
                    LONGLONG record=w.ScanoutObject;   // a refusal must leave the teardown record alone
                    hardware_result=STATUS_SUCCESS;
                    good.Magic=BC250_WDDM_MAGIC_ALLOCATION;good.Serial=0x9001;good.UmdAlloc=1;good.ScanoutRequested=1;
                    good.ScanoutWidth=1366;good.ScanoutHeight=768;good.ScanoutPitch=5632;
                    good.ScanoutFormat=D3DDDIFMT_A8R8G8B8;good.UmdBytes=5632ull*768;
                    request.hAllocation=&good;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
                    request.PrimaryAddress.QuadPart=0xC000;
                    d.VramPhysical.QuadPart=0x270000800;
                    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                    CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==aligned+1 && w.ScanoutObject==record);
                    d.VramPhysical.QuadPart=0x270000000;d.VramMcBase=0x800;
                    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                    CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==aligned+2);
                    d.VramMcBase=0;
                    // Admitted, programmed, and recorded as the surface the plane is reading.
                    CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                    CHECK(w.ScanoutFlips==2 && w.ScanoutObject==(LONGLONG)good.Serial);
                    {   // P5.4. The base a scan-out create asks VidMm for and the base this clause admits
                        // are one number, asserted here through the actual DDI and not only over the pure
                        // header: a create that asked for the wrong alignment would still pass the header
                        // test. One step of the alignment every other allocation keeps (64 bytes) above an
                        // admitted base is refused, and that refusal is the one that blanks the output when
                        // it arrives after SharedPrimaryTransition.
                        LONG refusals=w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT];
                        LONG admitted=w.ScanoutFlips;
                        request.PrimaryAddress.QuadPart=(LONGLONG)Bc250ScanoutCreateAlignment(1)*7;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                        CHECK(w.ScanoutFlips==admitted+1 && w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==refusals);
                        request.PrimaryAddress.QuadPart+=(LONGLONG)Bc250ScanoutCreateAlignment(0);
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                        CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==refusals+1 && w.ScanoutFlips==admitted+1);
                        request.PrimaryAddress.QuadPart=0xC000;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                        CHECK(w.ScanoutFlips==admitted+2 && w.ScanoutObject==(LONGLONG)good.Serial);
                    }
                    {   // With the flip gate closed this DDI still succeeds and still publishes, and
                        // no scan-out flip is counted: nothing was written to the display hardware,
                        // and a counter that moved here would report the whole increment's result on
                        // a machine where no address ever reached HUBP0.
                        LONG gated=w.ScanoutFlips;unsigned gatedHardware=hardware;
                        d.VidPnFlipEnabled=0;request.PrimaryAddress.QuadPart=0xD000;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                        CHECK(w.ScanoutFlips==gated && hardware==gatedHardware && w.ScanoutObject==0);
                        d.VidPnFlipEnabled=1;request.PrimaryAddress.QuadPart=0xC000;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                        CHECK(w.ScanoutFlips==gated+1 && w.ScanoutObject==(LONGLONG)good.Serial);
                    }
                    // Freeing the surface the plane is reading restores the firmware surface before
                    // the object goes: an application swap-chain buffer has no lifetime tie to the
                    // video present source, so this is the only thing between a crashed game and a
                    // plane scanning VRAM that VidMm has handed to somebody else.
                    list[0]=&good;destroy.NumAllocations=1;destroy.pAllocationList=list;
                    freed=journaled=restores=0;restore_result=STATUS_SUCCESS;
                    // The flip lines are written per frame and spend their allowance in the first
                    // seconds; the teardown line keeps its own, so the one line that says the plane
                    // was taken back is still written after a long run.
                    w.ScanoutNotes=notes=BC250_WDDM_LOG_CALLS*4;
                    CHECK(Bc250WddmDestroyAllocation(&d,&destroy)==STATUS_SUCCESS);
                    CHECK(w.ScanoutTeardowns==1 && w.ScanoutNotes==notes);
                    CHECK(restores==1 && freed==1 && journaled==1 && w.ScanoutObject==0);
                    CHECK(w.PrimaryNeedsRestore && w.PrimaryAddress.QuadPart==0 && w.PrimaryPitch==0);
                    // Any other allocation's destroy touches neither the plane nor the record.
                    w.PrimaryNeedsRestore=FALSE;list[0]=&umd;restores=0;freed=0;
                    CHECK(Bc250WddmDestroyAllocation(&d,&destroy)==STATUS_SUCCESS);
                    CHECK(restores==0 && freed==1 && !w.PrimaryNeedsRestore && w.ScanoutObject==0);
                    // The interleaving the tie exists for: dxgkrnl frees the buffer of a flip that is still
                    // inside SetVidPnSourceAddress, after the plane has been written with it. The record must
                    // already name that buffer here, or the destroy misses, frees it, and leaves HUBP0 reading
                    // VRAM that VidMm may hand to somebody else. The record is 0 at this point, so a record
                    // published after the programming would make this the unrecoverable case.
                    {
                        LONG teardowns=w.ScanoutTeardowns;
                        LONGLONG scanned;
                        request.hAllocation=&good;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
                        request.PrimaryAddress.QuadPart=0xE000;
                        hardware_result=STATUS_SUCCESS;freed=journaled=restores=0;
                        inject_destroy=&good;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                        CHECK(!inject_destroy && restores==1 && freed==1 && journaled==1);
                        CHECK(w.ScanoutTeardowns==teardowns+1 && w.ScanoutObject==0);
                        // A programming sequence that fails gave the plane no address it keeps, so the record
                        // goes back to the object the previous flip left there.
                        w.PrimaryNeedsRestore=FALSE;request.PrimaryAddress.QuadPart=0xC000;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                        scanned=w.ScanoutObject;
                        CHECK(scanned==(LONGLONG)good.Serial);
                        request.hAllocation=NULL;request.PrimaryAddress.QuadPart=0xF000;
                        hardware_result=STATUS_IO_TIMEOUT;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT);
                        CHECK(w.ScanoutObject==scanned);
                        hardware_result=STATUS_SUCCESS;
                        // The operator's switch (EnableScanoutAdmit 0): a candidate that asks for scan-out is
                        // refused and named, dxgkrnl's own primary still reaches the hardware, and no record is
                        // taken. That is 0.7.205.1 behaviour, reachable without a rebuild.
                        {
                            LONG gated=w.ScanoutAdmits[BC250_SCANOUT_GATED],gatedFlips=w.ScanoutFlips;
                            unsigned written=hardware;
                            w.ScanoutAdmitGate=FALSE;
                            request.hAllocation=&good;request.PrimaryAddress.QuadPart=0xD000;
                            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_INVALID_PARAMETER);
                            CHECK(w.ScanoutAdmits[BC250_SCANOUT_GATED]==gated+1);
                            CHECK(w.ScanoutFlips==gatedFlips && hardware==written && w.ScanoutObject==scanned);
                            request.hAllocation=&allocation;request.PrimarySegment=2;
                            request.PrimaryAddress.QuadPart=0x50;
                            CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                            CHECK(hardware==written+1 && w.ScanoutFlips==gatedFlips && w.ScanoutObject==0);
                            w.ScanoutAdmitGate=TRUE;
                        }
                        CHECK(!strcmp(Bc250ScanoutStatusText(BC250_SCANOUT_GATED),"gated"));
                        // Give the record back, so what follows starts from the compositor's own primary.
                        request.hAllocation=&good;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
                        request.PrimaryAddress.QuadPart=0xC000;
                        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS);
                        CHECK(w.ScanoutObject==scanned);
                        // The record names the object's serial and not its address. The same storage with a
                        // new serial is what a freed object and the next allocation at its address look
                        // like, and it must not match: a pointer record would have matched and taken the
                        // plane back to the firmware surface for a buffer nobody destroyed.
                        good.Serial=0x9002;
                        list[0]=&good;freed=journaled=restores=0;
                        CHECK(Bc250WddmDestroyAllocation(&d,&destroy)==STATUS_SUCCESS);
                        CHECK(restores==0 && freed==1 && w.ScanoutObject==scanned);
                        // A serial of 0 is the record's own "nothing is being scanned out" and must never
                        // match either, whatever the record holds.
                        good.Serial=0;
                        list[0]=&good;freed=journaled=restores=0;
                        CHECK(Bc250WddmDestroyAllocation(&d,&destroy)==STATUS_SUCCESS);
                        CHECK(restores==0 && freed==1 && w.ScanoutObject==scanned);
                        good.Serial=0x9001;
                        list[0]=&good;freed=journaled=restores=0;
                        CHECK(Bc250WddmDestroyAllocation(&d,&destroy)==STATUS_SUCCESS);
                        CHECK(restores==1 && w.ScanoutObject==0);
                        w.PrimaryNeedsRestore=FALSE;
                    }
                    w.PrimaryPitch=5632;w.PrimaryAddress.QuadPart=0x4000;
                }
                request.hAllocation=NULL;request.PrimarySegment=3;
            }
        }
        request.hAllocation=NULL;request.PrimaryAddress=w.PrimaryAddress;
        w.PrimaryNeedsRestore=TRUE;before=hardware;hardware_result=STATUS_IO_TIMEOUT;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_IO_TIMEOUT && w.PrimaryNeedsRestore);
        CHECK(hardware==before+1);
        hardware_result=STATUS_SUCCESS;
        CHECK(Bc250WddmSetVidPnSourceAddress(&d,&request)==STATUS_SUCCESS && !w.PrimaryNeedsRestore);
        CHECK(hardware==before+2); // identical OS address still reprograms lost hardware
    }
    FlipFlagBits();
    printf("vidpn publication: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
