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
    volatile LONG PrimarySequence, PrimaryProgrammedSequence;
    BOOLEAN PrimaryNeedsRestore;
    ULONG PrimaryPitch; ULONGLONG PrimaryBytes;
    unsigned PrimarySegment;
    volatile LONG Flips, FlipsAboveDispatch, VSyncReports;
    volatile LONG ScanoutFlips, ScanoutRequests, ScanoutAdmits[BC250_SCANOUT_STATUSES], ScanoutNotes, ScanoutTeardowns;
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
typedef struct {ULONG Magic; int UmdAlloc; struct {ULONG Width,Height,Pitch,Format;ULONGLONG Size;} Allocation;
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
static void WddmReport(BC250_DEVICE *d,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data){(void)d;reports++;reported=data->CrtcVsync.PhysicalAddress.QuadPart;}
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
int main(void)
{
    int level;
    for(level=0;level<2;level++){
        BC250_WDDM w={0};BC250_DEVICE d={0};
        DXGKARG_SETVIDPNSOURCEADDRESS request={0};
        PHYSICAL_ADDRESS sample={-1};
        unsigned before;
        d.Post.Width=1366;d.Post.Height=768;d.Post.Pitch=5632;w.PrimaryPitch=5632;
        d.Wddm=&w;d.VidPnFlipEnabled=1;w.VSyncEnabled=1;w.ScanoutAdmitGate=TRUE;
        w.PrimaryAddress.QuadPart=10;w.PrimarySegment=1;
        request.PrimaryAddress.QuadPart=20;request.PrimarySegment=2;
        healthSequence=healthCompletions=0;
        irq=level?3:0;hardware=arms=reports=0;pending=inject_update=nested_writer=0;
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
        before=reports;pending=1;inject_fallback_update=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before);
        // Matching address alone cannot override an asserted pending bit.
        before=reports;displayed=20;pending=1;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before);
        pending=0;d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(reports==before+1 && reported==20);
        CHECK(healthCompletions==1);
        d.DcnVsyncAcked=1;WddmDcnVsync(&d);
        CHECK(healthCompletions==1); // repeated completed primary is not progress
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
                    good.Magic=BC250_WDDM_MAGIC_ALLOCATION;good.UmdAlloc=1;good.ScanoutRequested=1;
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
                    CHECK(w.ScanoutFlips==2 && w.ScanoutObject==(LONGLONG)(ULONG_PTR)&good);
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
                        CHECK(w.ScanoutFlips==gated+1 && w.ScanoutObject==(LONGLONG)(ULONG_PTR)&good);
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
                        CHECK(scanned==(LONGLONG)(ULONG_PTR)&good);
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
    printf("vidpn publication: %u checks, %u failures\n",checks,failures);
    return failures?1:0;
}
