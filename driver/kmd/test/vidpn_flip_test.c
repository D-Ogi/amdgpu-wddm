// Deterministic interleavings of actual source, not a multicore test.
//
// BD-170 r7 (docs/design/vidpn-flip.md section 7.2): the flip DDI's transaction, the writers' sections, E5 and the
// report of a VSync run under L, the interrupt lock of message 0. This fixture models L. DxgkCbSynchronizeExecution
// runs its routine with in_lock set, at the device's interrupt level, and refuses a section inside a section, a caller
// above DISPATCH_LEVEL and another message. The flip DDI's high entry runs with in_lock set, because dxgmms2 holds L
// there (facts M904). A log line, a stall or an arm inside L is a failure, and so is a read or write of the display
// hardware by the state machine outside L. Another processor is work done where L is free: a vblank DPC during a
// programming, which must see only the odd generation, a flip during a writer's restore, and a flip that wins L just
// before a writer's section (the section hook).
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "dcn_translate.h"
#include "surface_format.h"
#include "scanout_admit.h"
#include "flip_state.h"
#include "dcn_scale.h"   /* BD-223: BC250_SCALER_PLAN, the shape of a multiplane-overlay plane 0 */
typedef int32_t NTSTATUS;
typedef long LONG;
typedef unsigned long ULONG;
typedef int64_t LONGLONG;
typedef int64_t LONG64;
typedef uint64_t ULONGLONG;
typedef uint64_t UINT64;
typedef int BOOLEAN;
typedef void *HANDLE;
typedef void *PVOID;
typedef unsigned UINT;
typedef unsigned long long ULONG_PTR;
typedef struct { LONGLONG QuadPart; } PHYSICAL_ADDRESS;
typedef struct { LONGLONG QuadPart; } LARGE_INTEGER;
#define TRUE 1
#define FALSE 0
#define DISPATCH_LEVEL 2
#define PASSIVE_LEVEL 0
#define TEST_DIRQL 5                /* the device's interrupt level: L, and dxgmms2's flip call (facts M904) */
#define STATUS_SUCCESS ((NTSTATUS)0)
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001u)
#define STATUS_ACCESS_DENIED ((NTSTATUS)0xC0000022u)
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000Du)
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3u)
#define STATUS_DEVICE_BUSY ((NTSTATUS)0x80000011u)
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5u)
#define STATUS_ILLEGAL_INSTRUCTION ((NTSTATUS)0xC000001Du)
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBu)
#define STATUS_INVALID_DEVICE_STATE ((NTSTATUS)0xC0000184u)
#define STATUS_DEVICE_HARDWARE_ERROR ((NTSTATUS)0xC0000483u)
#define NT_SUCCESS(s) ((s)>=0)
/* DXGK_INTERRUPT_TYPE values of shared/d3dkmddi.h 10.0.26100 (line 689 on). */
typedef unsigned DXGK_INTERRUPT_TYPE;
#define DXGK_INTERRUPT_DMA_COMPLETED 1
#define DXGK_INTERRUPT_CRTC_VSYNC 3
#define DXGK_INTERRUPT_DMA_PAGE_FAULTED 9
/* E5's payload (shared/d3dkmddi.h 10.0.26100 DmaPageFaulted, line 1064 on) and its types and values (shared/d3dkmdt.h
 * 10.0.26100, line 2297 on): the UNKNOWN values, the page fault flags, and DXGK_FAULT_ERROR_CODE with UINT in place of
 * the enum bit field, which /W4 calls nonstandard. wddm.c gets ULONGLONG_MAX the same way (limits.h). */
#define ULONGLONG_MAX ULLONG_MAX
#define DXGK_PRIMITIVE_API_SEQUENCE_NUMBER_UNKNOWN ULONGLONG_MAX
#define DXGK_BIND_TABLE_ENTRY_UNKNOWN UINT_MAX
typedef enum { DXGK_RENDER_PIPELINE_STAGE_UNKNOWN = 0 } DXGK_RENDER_PIPELINE_STAGE;
typedef enum {
    DXGK_PAGE_FAULT_WRITE = 0x1,
    DXGK_PAGE_FAULT_FENCE_INVALID = 0x2,
    DXGK_PAGE_FAULT_ADAPTER_RESET_REQUIRED = 0x4
} DXGK_PAGE_FAULT_FLAGS;
typedef struct {
    union {
        struct { UINT IsDeviceSpecificCode:1; UINT GeneralErrorCode:31; };
        struct { UINT IsDeviceSpecificCodeReservedBit:1; UINT DeviceSpecificCode:31; };
    };
} DXGK_FAULT_ERROR_CODE;
typedef struct {
    DXGK_INTERRUPT_TYPE InterruptType;
    union {
        struct { unsigned VidPnTargetId, PhysicalAdapterMask; PHYSICAL_ADDRESS PhysicalAddress; } CrtcVsync;
        struct {
            UINT FaultedFenceId;
            UINT64 FaultedPrimitiveAPISequenceNumber;
            DXGK_RENDER_PIPELINE_STAGE FaultedPipelineStage;
            UINT FaultedBindTableEntry;
            DXGK_PAGE_FAULT_FLAGS PageFaultFlags;
            ULONGLONG FaultedVirtualAddress;
            UINT NodeOrdinal, EngineOrdinal, PageTableLevel;
            DXGK_FAULT_ERROR_CODE FaultErrorCode;
            HANDLE FaultedProcessHandle;
        } DmaPageFaulted;
    };
} DXGKARGCB_NOTIFY_INTERRUPT_DATA;
#define WddmDdiSetVidPnSourceAddress 0
#define WddmDdiDestroyAllocation 1
#define WddmDdiControlInterrupt 2
#define RtlZeroMemory(p,n) memset(p,0,n)
#define BC250_WDDM_SEGMENT_VRAM 1u
// DXGK_SETVIDPNSOURCEADDRESS_FLAGS, in the DECLARATION ORDER of the WDK's shared/d3dkmddi.h (10.0.26100,
// around line 6213). The order is the ABI and the trailing comments in that header are not: they give
// 0x00000010 for both FlipStereoTemporaryMono and FlipStereoPreferRight and are shifted by one bit from
// there on, while Reserved:23 after nine named bits fixes the layout. The driver therefore reads these as
// bitfields and never as transcribed masks, and FlipFlagBits() below asserts what this copy comes out as,
// so a transcription error in this fixture cannot pass either. Context and ContextCount: the contexts the flip
// call carries (E5's node, WddmFlipNode), 1 + D3DDDI_MAX_BROADCAST_CONTEXT (shared/d3dukmdt.h, 64) of them.
typedef struct {
    HANDLE hAllocation;
    unsigned VidPnSourceId, PrimarySegment;
    PHYSICAL_ADDRESS PrimaryAddress;
    UINT ContextCount;
    HANDLE Context[1 + 64];
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
typedef NTSTATUS DXGKDDI_SETVIDPNSOURCEADDRESS(const HANDLE, const DXGKARG_SETVIDPNSOURCEADDRESS*);
/* The part of DXGKARG_PRESENT that WddmPresentDmaFlip reads: DXGK_PRESENTFLAGS in the WDK's declaration order
 * (shared/d3dkmddi.h 10.0.26100, Blt 0x1, ColorFill 0x2, Flip 0x4, FlipWithNoWait 0x8), the DMA buffer and its size. */
typedef struct {
    union {
        struct {
            unsigned Blt:1;
            unsigned ColorFill:1;
            unsigned Flip:1;
            unsigned FlipWithNoWait:1;
            unsigned Reserved:28;
        };
        unsigned Value;
    } Flags;
    void *pDmaBuffer;
    unsigned DmaSize;
} DXGKARG_PRESENT;
/* The graphics kernel's three callbacks the flip path makes (dispmprt.h): the section on an interrupt message, the
 * notification, and the DPC request. */
typedef BOOLEAN (*PKSYNCHRONIZE_ROUTINE)(PVOID);
typedef NTSTATUS (*TEST_SYNCHRONIZE_EXECUTION)(HANDLE, PKSYNCHRONIZE_ROUTINE, PVOID, ULONG, BOOLEAN*);
typedef void (*TEST_NOTIFY_INTERRUPT)(HANDLE, const DXGKARGCB_NOTIFY_INTERRUPT_DATA*);
typedef BOOLEAN (*TEST_QUEUE_DPC)(HANDLE);
/* BD-170: wddm.c's record of one CRTC_VSYNC call of ControlInterrupt. */
#define BC250_WDDM_VSYNC_CONTROL_EVENTS 16u
typedef struct { LONG Call; BOOLEAN On; LONGLONG Qpc; LONGLONG Owed; } BC250_WDDM_VSYNC_CONTROL;
/* BD-170 r6: wddm.c's W-u record and the summary's copy of the records. */
#define BC250_WDDM_WU_RECORDS 8u
typedef struct {
    LONG Record; ULONG Code; ULONGLONG Request; ULONG Flags; ULONG Generation; ULONG Writer; ULONG Deposit;
    BOOLEAN Unblank; ULONG End; LONGLONG DepositQpc; LONG ToEndUs;
} BC250_WDDM_WU_RECORD;
typedef struct { LONG Next; LONG ToEndMaxUs; BC250_WDDM_WU_RECORD Records[BC250_WDDM_WU_RECORDS]; } BC250_WDDM_WU_COPY;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
/* BD-170 r6, the lab flavour's witnesses: bc250kmd.h's arm and read, wddm.c's record and copy. */
typedef struct { NTSTATUS Status; ULONG InterruptBefore, InterruptCleared, PendingBefore, PendingAfter; BOOLEAN Fresh; } BC250_DCN_WITNESS_ARM;
typedef struct {
    ULONG Interrupt; BOOLEAN Event; ULONG Low1, High1, High2, Low2; ULONGLONG Earliest1, Earliest2;
} BC250_DCN_WITNESS_READ;
#define BC250_WDDM_WITNESS_RECORDS 16u
typedef struct {
    LONG Request; ULONG Generation, Flags, Outcome, Result; BOOLEAN Q, Fresh, E, R1, R2; NTSTATUS ArmStatus, ReadStatus;
    ULONGLONG Card, Physical, Earliest1, Earliest2; ULONG InterruptBefore, InterruptCleared, Interrupt;
    ULONG PendingBefore, PendingAfter;
    ULONG Low1, High1, High2, Low2;
} BC250_WDDM_WITNESS_RECORD;
typedef struct {
    LONG Requests, ReadFailed, Stale, Qe, Qr, Er, Order, Lost, Kept;
    BC250_WDDM_WITNESS_RECORD Records[BC250_WDDM_WITNESS_RECORDS];
    BC250_WDDM_WITNESS_RECORD Last;
} BC250_WDDM_WITNESS_COPY;
#endif
typedef struct {
    PHYSICAL_ADDRESS PrimaryAddress;
    volatile LONG PrimarySequence, PrimaryProgrammedSequence;
    BOOLEAN PrimaryNeedsRestore;
    ULONG PrimaryPitch; ULONGLONG PrimaryBytes;
    ULONG PrimaryPlaneFormat;       /* M15.14: plane_format.h format of the published primary, 0 = none yet */
    unsigned PrimarySegment;
    volatile LONG Flips, FlipsAboveDispatch, VSyncReports, ReportFailures;
    volatile LONG ImmediatePromotions, DmaFlipsRefused;     /* BD-170 */
    volatile LONG ExclusiveRefused, ExclusiveFailed;
    volatile LONG WriterBusy;
    volatile LONG WriterBusyByKind[BC250_WRITER_KINDS];
    /* BD-195: every FALSE answer of WddmPrimaryExclusiveBegin, by kind, and the last one's site. */
    volatile LONG WriterRefused;
    volatile LONG WriterRefusedByKind[BC250_WRITER_KINDS];
    ULONG WriterRefusedKind, WriterRefusedSite, WriterRefusedAnswer;
    NTSTATUS WriterRefusedStatus;
    volatile LONG WriterBridgePrograms, WriterBridgeRetired, WriterBridgeFailures;
    BOOLEAN WriterReady;
    /* BD-170 r6, the flip state machine's block (wddm.c says what each field is). */
    BOOLEAN WriterActive, WriterResumed, WriterDeposit, DesiredOwed;
    volatile LONG WriterRecovery;
    ULONG DepositSequence;
    volatile LONG Faulting;
    ULONG FaultCode;
    UINT FaultNode;
    NTSTATUS FaultBlank;
    volatile LONG FaultNotify, FaultDrain, FaultDrainReads;
    volatile LONG FlipEdges[BC250_FLIP_EDGES];
    volatile LONG FlipResults[BC250_FLIP_RESULTS];
    volatile LONG FlipFaults, FaultNotified, FaultNotifyFailed;
    volatile LONG WriterFaulting, WriterHigh, WriterSyncFailed, LowSyncFailed;
    volatile LONG DepositsResumed, EndPrograms, OwedInvalidated, OwedReported, FlipFlushes, DestroyUnsynchronized;
    volatile LONG FlipLockedMaxUs;
    ULONG WriterKind;
    volatile LONG WuRecordNext, WuToEndMaxUs;
    BC250_WDDM_WU_RECORD WuRecords[BC250_WDDM_WU_RECORDS];
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    volatile LONG WitnessRequests, WitnessReadFailed, WitnessStale, WitnessQe, WitnessQr, WitnessEr, WitnessOrder;
    volatile LONG WitnessLost, WitnessKept;
    BC250_WDDM_WITNESS_RECORD WitnessRecords[BC250_WDDM_WITNESS_RECORDS];
    BC250_WDDM_WITNESS_RECORD WitnessLast;
#endif
    volatile LONG VSyncControlCalls, VSyncControlOn, VSyncControlOff;
    BC250_WDDM_VSYNC_CONTROL VSyncControl[BC250_WDDM_VSYNC_CONTROL_EVENTS];
    volatile LONG64 RingGapLastVsyncQpc;    /* C48's stamp of the last hardware vblank */
    volatile LONG ScanoutFlips, ScanoutRequests, ScanoutAdmits[BC250_SCANOUT_STATUSES], ScanoutNotes, ScanoutTeardowns;
    /* M15.14 increment 2: the OS's own flip mode, per bit, for every accepted address call. */
    volatile LONG ScanoutFlipFlags[4];
    volatile LONG ScanoutFlipsByFormat[BC250_PLANE_FORMATS];   /* M15.14: written flips by plane format */
    volatile LONGLONG ScanoutObject;
    BOOLEAN ScanoutAdmitGate;       /* EnableScanoutAdmit, absent = on (WddmStart) */
    int VSyncEnabled;
    unsigned VSyncTargetId;
} BC250_WDDM;
/* BD-170: bc250kmd.h's immediate-flip record, for the summary's copy of it (WddmImmediateRecordCopy). */
#define BC250_DCN_IMMEDIATE_RECORDS 8u
#define BC250_DCN_IMMEDIATE_READS 3u
typedef struct {
    volatile LONG Version; LONG Sequence; ULONG Generation; ULONG Outcome; ULONG Flags;
    ULONGLONG CardAddress; ULONGLONG Physical; ULONGLONG Observed; LONG ReadStatus; ULONG Samples;
    ULONG Frame; ULONG PositionBefore; ULONG PositionAfter; ULONG Control;
    LONG ProgramUs; LONG FirstUs; LONG ClearUs; LONG DoneUs; LONG EndUs;
} BC250_DCN_IMMEDIATE_RECORD;
typedef struct {
    struct {ULONG Width,Height,Pitch;} Post,RuntimePost;
    BOOLEAN RuntimePostValid;
    BC250_WDDM *Wddm;
    // M15.14: the two bases the card address is translated through. The scan-out clause refuses a
    // candidate when either is not page aligned, because the predicate's 4 KiB test is on the card
    // address and the plane is given the physical one.
    ULONGLONG VramMcBase;
    PHYSICAL_ADDRESS VramPhysical;
    int VidPnFlipEnabled;
    struct {
        HANDLE DeviceHandle;
        TEST_SYNCHRONIZE_EXECUTION DxgkCbSynchronizeExecution;
        TEST_NOTIFY_INTERRUPT DxgkCbNotifyInterrupt;
        TEST_QUEUE_DPC DxgkCbQueueDpc;
    } Dxgk;
    volatile LONG DcnVsyncAcked, DcnVsyncDeferred, DcnVsyncOldBufferReports;
    volatile LONG DcnVsyncSkipOddGeneration, DcnVsyncSkipReadFailure, DcnVsyncSkipSameAddress, DcnVsyncSkipChangedGeneration;
    volatile LONG64 DcnVsyncSkipOddGenerationTime, DcnVsyncSkipReadFailureTime, DcnVsyncSkipSameAddressTime,
        DcnVsyncSkipChangedGenerationTime, DcnVsyncNotifyTime;
    BC250_DCN_IMMEDIATE_RECORD DcnImmediateRecords[BC250_DCN_IMMEDIATE_RECORDS];
    LONGLONG DcnQpcFrequency;           /* 1 MHz here: one tick of the stub is 1 us */
} BC250_DEVICE;
/* Display modes (bc250kmd.h): the source size is the committed mode when one is set, else the inherited mode.
 * This fixture commits no mode, so the source size is always the inherited one. */
static ULONG DisplaySourceWidth(const BC250_DEVICE *d){return d->Post.Width;}
static ULONG DisplaySourceHeight(const BC250_DEVICE *d){return d->Post.Height;}
#define BC250_WDDM_MAGIC_ALLOCATION 123
#define BC250_WDDM_MAGIC_RESOURCE 124
#define BC250_WDDM_MAGIC_CONTEXT 125
#define BC250_WDDM_NODE_3D 0u
#define BC250_WDDM_NODE_COUNT_MAX 2u
#define BC250_WDDM_LOG_CALLS 8
#define BC250_PJ_FLAG_UMD_ALLOCATION 1u
#define D3DDDIFMT_A8R8G8B8 21
#define D3DDDIFMT_X8R8G8B8 22
#define D3DDDIFMT_A8B8G8R8 32
#define D3DDDIFMT_A2B10G10R10 31
#define D3DDDIFMT_A8 28
#define D3DDDIFMT_A16B16G16R16F 113
/* Serial: adapter-unique, nonzero, never reused (wddm.c, WddmNewObject under the lock). The teardown record
 * holds it instead of the object's address, so a value that outlived its object cannot match a later object
 * allocated where the old one was. NodeOrdinal: a context's node. */
typedef struct {ULONG Magic; ULONGLONG Serial; int UmdAlloc; struct {ULONG Width,Height,Pitch,Format;ULONGLONG Size;} Allocation;
    int ScanoutRequested; ULONG ScanoutWidth,ScanoutHeight,ScanoutPitch,ScanoutFormat; ULONGLONG UmdBytes;
    ULONGLONG UmdRequestedVa,UmdGemFlags; ULONG CreatorProcessId,UmdBlobVersion; UINT NodeOrdinal;} BC250_WDDM_OBJECT;
static BC250_WDDM_OBJECT* WddmObject(HANDLE h,ULONG magic){BC250_WDDM_OBJECT *a=h;return a && a->Magic==magic?a:NULL;}
typedef struct {
    UINT NumAllocations;
    HANDLE *pAllocationList, hResource;
    struct { int DestroyResource; } Flags;
} DXGKARG_DESTROYALLOCATION;
#define CHECK(x) do { ++checks; if(!(x)){++fails;printf("FAIL line %d: %s\n",__LINE__,#x);} } while(0)
static unsigned checks,fails;
/* L. in_lock: a section's routine or the high entry is running. No test exempts a protected operation from L.
 * sync_refusals: the next sections the callback refuses. sync_refuse_at: refuse the exact numbered section,
 * including a later stage of a multi-section operation. before_section: work another processor finishes just
 * before the next section takes L, once. */
static int irq,in_lock;
static unsigned syncs,sync_refusals,sync_refuse_at;
static int sync_no_callback;
static PKSYNCHRONIZE_ROUTINE sync_refuse_routine;
static void (*before_section)(BC250_DEVICE*);
static PKSYNCHRONIZE_ROUTINE after_section_routine;
static void (*after_section)(BC250_DEVICE*);
/* What the graphics kernel was told: CRTC_VSYNC reports (the last address), DMA_PAGE_FAULTED notifications (the last
 * payload), and DPC requests. */
static unsigned reports,faults_notified,dpcs;
static LONGLONG reported;
static DXGKARGCB_NOTIFY_INTERRUPT_DATA fault_data;
static unsigned arms,logs,stalls1,stalls10;
static NTSTATUS TestSync(HANDLE h,PKSYNCHRONIZE_ROUTINE routine,PVOID context,ULONG message,BOOLEAN *returned)
{
    const int level=irq;
    CHECK(h!=NULL && message==0 && !in_lock && irq<=DISPATCH_LEVEL);
    ++syncs;
    if(sync_no_callback){sync_no_callback=0;*returned=FALSE;return STATUS_SUCCESS;}
    if(sync_refusals){--sync_refusals;return STATUS_UNSUCCESSFUL;}
    if(sync_refuse_at==syncs){sync_refuse_at=0;return STATUS_UNSUCCESSFUL;}
    if(sync_refuse_routine==routine){sync_refuse_routine=NULL;return STATUS_UNSUCCESSFUL;}
    if(before_section){void (*first)(BC250_DEVICE*)=before_section;before_section=NULL;first((BC250_DEVICE*)h);}
    in_lock=1;irq=TEST_DIRQL;
    *returned=routine(context);
    in_lock=0;irq=level;
    if(after_section && after_section_routine==routine){
        void (*next)(BC250_DEVICE*)=after_section;after_section=NULL;after_section_routine=NULL;next((BC250_DEVICE*)h);
    }
    return STATUS_SUCCESS;
}
/* DxgkCbNotifyInterrupt must be called inside L (wddm.c's WddmNotifyRoutine), never reentrantly. */
static void TestNotify(HANDLE h,const DXGKARGCB_NOTIFY_INTERRUPT_DATA *data)
{
    (void)h;
    CHECK(in_lock);
    if(data->InterruptType==DXGK_INTERRUPT_CRTC_VSYNC){reports++;reported=data->CrtcVsync.PhysicalAddress.QuadPart;}
    else if(data->InterruptType==DXGK_INTERRUPT_DMA_PAGE_FAULTED){faults_notified++;fault_data=*data;}
    else CHECK(!"another interrupt type");
}
static BOOLEAN TestQueueDpc(HANDLE h){(void)h;++dpcs;return TRUE;}
static int KeGetCurrentIrql(void){return irq;}
static ULONGLONG KeQueryInterruptTime(void){return 1;}
/* GuardLog takes a spin lock: never inside L, never above DISPATCH_LEVEL. Its arguments are not evaluated here. */
static void TestLog(void){CHECK(!in_lock && irq<=DISPATCH_LEVEL);++logs;}
#define GuardLog(...) TestLog()
static ULONG programmed_pitch;
static ULONG programmed_format;     /* M15.14: the plane format the last written flip carried */
static unsigned freed,journaled,hardware,healthCompletions,pairedVsyncReports;
static ULONG healthSequence;
static NTSTATUS hardware_result;
/* hardware_wrote: a failing DcnFlipSourceAddress had run its write sequence (E5) rather than refused before it (A). */
static int hardware_wrote,pending,inject_update,inject_fallback_update;
static DXGKARG_SETVIDPNSOURCEADDRESS interleave_request;
static LONGLONG plane_card,displayed;
/* The teardown record a test requires while the plane is written (the lifetime tie), and how often it was seen. */
static LONGLONG expect_record;
static unsigned record_checked;
/* The state machine's display hardware: the HUBP drained readback (1, 0, or -1 for a failed read; or, with
 * hubp_drain_after, 0 until that read and 1 from it on), the OTG (1, 0, -1), and E5's blank request. */
static int hubp_state,otg_state;
/* begin_kind: the writer that the test's Begin names (a W-u record keeps it). */
static ULONG begin_kind;
static unsigned hubp_reads,hubp_drain_after,otg_reads,fault_blanks;
static NTSTATUS fault_blank_status;
#define BRIDGE_CARD 0xF00000ull
static unsigned bridge_writes;
static NTSTATUS bridge_result;
static BOOLEAN bridge_wrote;
static NTSTATUS bridge_retire_result;
static unsigned bridge_retires;
static void (*retire_hook)(BC250_DEVICE*);
typedef struct {
    ULONGLONG Physical;
    ULONG Pitch;
    LONGLONG ProgramEndQpc;
    BOOLEAN Retired;
} BC250_DCN_WRITER_BRIDGE;
static LONG InterlockedCompareExchange(volatile LONG *p,LONG value,LONG compare){LONG old=*p;if(old==compare)*p=value;return old;}
static LONG InterlockedExchange(volatile LONG *p,LONG value){LONG old=*p;*p=value;return old;}
static LONGLONG InterlockedCompareExchange64(volatile LONGLONG *p,LONGLONG value,LONGLONG compare){LONGLONG old=*p;if(old==compare)*p=value;return old;}
static LONGLONG InterlockedExchange64(volatile LONGLONG *p,LONGLONG value){LONGLONG old=*p;*p=value;return old;}
static LONG InterlockedIncrement(volatile LONG *p){return ++*p;}
static BC250_WDDM *WddmOf(HANDLE h){return ((BC250_DEVICE*)h)->Wddm;}
static int WddmFirstCalls(BC250_WDDM *w,int call){(void)w;(void)call;return 0;}
static void WddmVSyncArm(BC250_DEVICE *d,int on){(void)d;CHECK(on && irq<=DISPATCH_LEVEL && !in_lock);++arms;}
static int WddmStopping(BC250_WDDM *w){(void)w;return 0;}
/* C48: the vblank time the ring-gap accounting keys on. Counted here, not timed: what the fixture has to
 * prove is that it is taken for EVERY acknowledged vblank, before any of the deferral paths can return. */
static int ringGapVsyncs;
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *frequency);
static void WddmRingGapVsync(BC250_WDDM *w){++ringGapVsyncs;w->RingGapLastVsyncQpc=KeQueryPerformanceCounter(NULL).QuadPart;}
/* C50: a CRTC_VSYNC report owes the same DPC-level notification as any other, so this path has to count one. The stub
 * insists on the vsync flavour, because the only paired reports this fixture can produce are vblank ones. */
static void WddmPairingReport(BC250_WDDM *w,BOOLEAN vsync){(void)w;CHECK(vsync);++pairedVsyncReports;}
static BOOLEAN WddmReadCompletedPrimary(BC250_DEVICE*,BC250_WDDM*,PHYSICAL_ADDRESS*,ULONG*);
static NTSTATUS Bc250WddmSetVidPnSourceAddress(const HANDLE,const DXGKARG_SETVIDPNSOURCEADDRESS*);
void WddmDcnVsync(BC250_DEVICE*);
void WddmFlipFaultDpc(BC250_DEVICE*);
BOOLEAN WddmPrimaryExclusiveBegin(BC250_DEVICE*,ULONG,ULONG*);
NTSTATUS WddmPrimaryExclusiveEnd(BC250_DEVICE*,ULONG,BOOLEAN);
NTSTATUS WddmWriterResume(BC250_DEVICE*);
void WddmFlipFlushed(BC250_DEVICE*);
/* The flip DDI as dxgmms2 calls it: at or below DISPATCH_LEVEL without L, above it inside L (facts M904). */
static NTSTATUS Flip(BC250_DEVICE *d,const DXGKARG_SETVIDPNSOURCEADDRESS *request)
{
    NTSTATUS status;
    if(irq<=DISPATCH_LEVEL) return Bc250WddmSetVidPnSourceAddress(d,request);
    CHECK(!in_lock);
    in_lock=1;status=Bc250WddmSetVidPnSourceAddress(d,request);in_lock=0;
    return status;
}
static void PagingJournalDestroy(ULONGLONG va,HANDLE h,ULONGLONG bytes,unsigned flags,ULONG pid,ULONG version,ULONGLONG gem)
{(void)va;(void)h;(void)bytes;(void)flags;(void)pid;(void)version;(void)gem;++journaled;}
static void WddmFreeObject(BC250_WDDM_OBJECT *o){if(o)++freed;}
#define PHYS(card) ((ULONGLONG)(card)+0x400000000ull)  /* the stub's translation of a card address */
static unsigned checks_at_vblank;
static unsigned immediate_writes,observed,pending_samples;
static int programmed_immediate;
/* BD-170: the outcomes DcnImmediateObserve reports (bc250kmd.h's BC250_DCN_IMMEDIATE_*); only DONE and DONE_LATE are P'. */
#define BC250_DCN_IMMEDIATE_DONE 1u
#define BC250_DCN_IMMEDIATE_DONE_LATE 2u
#define BC250_DCN_IMMEDIATE_PENDING 3u
#define BC250_DCN_IMMEDIATE_PRIMARY_OTHER 4u
#define BC250_DCN_IMMEDIATE_READ_FAILED 5u
#define BC250_DCN_IMMEDIATE_SUPERSEDED 6u
static ULONG observe_outcome=BC250_DCN_IMMEDIATE_DONE,observed_flags;
static LONGLONG observed_address,qpc=1000;
/* One tick a call. The frequency is the device's, never asked for here. */
static LARGE_INTEGER KeQueryPerformanceCounter(LARGE_INTEGER *frequency)
{LARGE_INTEGER now;CHECK(frequency==NULL);now.QuadPart=++qpc;return now;}
/* BD-223: the pipe shape of dcn.c. A full-size write puts the 1:1 shape back (shape_on 0); a shaped write leaves
 * its shape on the pipe. Both are read only inside L, by the transaction. */
static int shape_on;
static BC250_SCALER_PLAN shape_now;
static unsigned shaped_writes;
static BOOLEAN DcnPlaneShapeIs(const BC250_DEVICE *d,const BC250_SCALER_PLAN *shape)
{(void)d;CHECK(in_lock);return shape?(shape_on && memcmp(shape,&shape_now,sizeof(*shape))==0):!shape_on;}
static BOOLEAN DcnPlaneShapeCurrent(const BC250_DEVICE *d,BC250_SCALER_PLAN *shape)
{(void)d;CHECK(in_lock);*shape=shape_now;return shape_on;}
static NTSTATUS DcnFlipSourceAddress(BC250_DEVICE *d,ULONGLONG address,ULONG pitch,ULONG format,ULONGLONG bytes,
    BOOLEAN immediate,ULONGLONG *out,BOOLEAN *wrote)
{
    PHYSICAL_ADDRESS sample={-1};
    const BC250_PLANE_ENCODING *plane=Bc250PlaneEncoding(format);
    CHECK(in_lock);
    /* M15.14: every flip names a plane format with an encoding, and the pitch is whole pixels of it. */
    CHECK(plane!=NULL && plane && pitch%plane->BytesPerPixel==0 && pitch>=d->Post.Width*plane->BytesPerPixel);
    CHECK(bytes==(ULONGLONG)pitch*d->Post.Height);
    hardware++;
    /* The generation is odd while the plane is written: the lock-free completion read refuses it, and a vblank DPC on
     * another processor reports nothing and never waits for L. Its stamp is put back, so this check is no vblank. */
    CHECK(!WddmReadCompletedPrimary(d,d->Wddm,&sample,NULL));
    CHECK(sample.QuadPart==-1);
    {
        const LONG64 stamp=d->Wddm->RingGapLastVsyncQpc;
        const unsigned before=reports,sections=syncs;
        const int held=in_lock,level=irq;
        in_lock=0;irq=DISPATCH_LEVEL;
        d->DcnVsyncAcked=1;WddmDcnVsync(d);
        CHECK(reports==before && syncs==sections);
        in_lock=held;irq=level;
        d->Wddm->RingGapLastVsyncQpc=stamp;
        ++checks_at_vblank;
    }
    /* The lifetime tie: the teardown record names the new buffer before the plane is written with it, so a destroy
     * that reads it on another processor now waits for L and then restores. */
    if(expect_record){CHECK(d->Wddm->ScanoutObject==expect_record);++record_checked;expect_record=0;}
    *wrote=NT_SUCCESS(hardware_result) || hardware_wrote;
    if(NT_SUCCESS(hardware_result)){plane_card=(LONGLONG)address;programmed_pitch=pitch;programmed_format=format;shape_on=0;
        programmed_immediate=immediate;if(immediate)++immediate_writes;if(out)*out=PHYS(address);}
    return hardware_result;
}
/* The shaped write: every check of the full-size write, at the shape's source size (the allocation is that size). */
static NTSTATUS DcnFlipSourceAddressShaped(BC250_DEVICE *d,ULONGLONG address,ULONG pitch,ULONG format,ULONGLONG bytes,
    BOOLEAN immediate,const BC250_SCALER_PLAN *shape,ULONGLONG *out,BOOLEAN *wrote)
{
    const ULONG width=d->Post.Width,height=d->Post.Height;
    NTSTATUS status;
    CHECK(shape!=NULL && shape->Scaling==BC250_SCALING_PLANE);
    d->Post.Width=shape->SourceWidth;d->Post.Height=shape->SourceHeight;
    status=DcnFlipSourceAddress(d,address,pitch,format,bytes,immediate,out,wrote);
    d->Post.Width=width;d->Post.Height=height;
    if(NT_SUCCESS(status)){shape_on=1;shape_now=*shape;++shaped_writes;}
    return status;
}
/* The report's call (wddm.c WddmNotifyCall): the shipping flavour passes the data on as it is. */
static void WddmNotifyCall(BC250_DEVICE *d,DXGKARGCB_NOTIFY_INTERRUPT_DATA *data)
{d->Dxgk.DxgkCbNotifyInterrupt(d->Dxgk.DeviceHandle,data);}
/* P' is observed inside L, in the transaction that wrote the plane: the generation is the odd value the transaction
 * holds it at, passed by reference; the physical address is the one the write returned; the programming time is taken
 * around the write, start before end. Nothing is published yet. */
static ULONG DcnImmediateObserve(BC250_DEVICE *d,ULONGLONG address,ULONGLONG physical,LONGLONG programStart,
    LONGLONG programEnd,volatile LONG *generation,ULONG expected,ULONG flags)
{
    CHECK(in_lock);
    CHECK(generation==&d->Wddm->PrimarySequence && (ULONG)*generation==expected && (expected&1));
    CHECK(plane_card==(LONGLONG)address && physical==PHYS(address));
    CHECK(programmed_immediate && programStart>1000 && programStart<programEnd && programEnd<=qpc);
    ++observed;observed_address=(LONGLONG)address;observed_flags=flags;
    return observe_outcome;
}
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
/* The witnesses. The arm comes before the programming of an immediate request, inside L; the read comes after the
 * shipping decision: P' was observed and the result counted since the arm, or the read is refused with a FAIL line.
 * witness_*: what the read returns (the event, and whether EARLIEST_INUSE names the request in each order). */
static unsigned witness_arms,witness_reads,witness_observed,witness_results,witness_hardware;
static int witness_fresh=1,witness_event,witness_r1,witness_r2;
static ULONG witness_pending_before,witness_pending_after;
static NTSTATUS witness_status;
static unsigned Results(const BC250_WDDM *w)
{unsigned n=0,i;for(i=0;i<BC250_FLIP_RESULTS;i++)n+=(unsigned)w->FlipResults[i];return n;}
static void DcnWitnessArm(const BC250_DEVICE *d,BOOLEAN immediate,BC250_DCN_WITNESS_ARM *arm)
{
    CHECK(in_lock);
    memset(arm,0,sizeof(*arm));arm->Status=STATUS_NOT_SUPPORTED;
    if(!immediate)return;
    ++witness_arms;witness_observed=observed;witness_results=Results(d->Wddm);witness_hardware=hardware;
    arm->Status=STATUS_SUCCESS;arm->InterruptBefore=0x101;arm->InterruptCleared=witness_fresh?0:0x100;
    arm->PendingBefore=witness_pending_before;arm->PendingAfter=witness_pending_after;
    arm->Fresh=witness_fresh && !witness_pending_before && !witness_pending_after?TRUE:FALSE;
}
static NTSTATUS DcnWitnessRead(const BC250_DEVICE *d,BC250_DCN_WITNESS_READ *read)
{
    CHECK(in_lock);
    CHECK(observed==witness_observed+1 && Results(d->Wddm)==witness_results+1 && hardware==witness_hardware+1);
    ++witness_reads;
    memset(read,0,sizeof(*read));
    read->Interrupt=witness_event?0x100:0;read->Event=witness_event?TRUE:FALSE;
    read->Earliest1=witness_r1?PHYS(plane_card):PHYS(0x1000);read->Earliest2=witness_r2?PHYS(plane_card):PHYS(0x1000);
    read->Low1=(ULONG)read->Earliest1;read->High1=(ULONG)(read->Earliest1>>32);
    read->Low2=(ULONG)read->Earliest2;read->High2=(ULONG)(read->Earliest2>>32);
    return witness_status;
}
#endif
/* const as in bc250kmd.h. The injected flip is the DDI on the same adapter, from another processor. */
static BOOLEAN DcnFlipPending(const BC250_DEVICE *d, ULONGLONG expected)
{
    ++pending_samples;
    if(inject_update){
        DXGKARG_SETVIDPNSOURCEADDRESS next={0};
        inject_update=0;
        next.PrimaryAddress.QuadPart=30;next.PrimarySegment=3;
        CHECK(Flip((BC250_DEVICE*)(ULONG_PTR)d,&next)==STATUS_SUCCESS);
    }
    return pending || displayed != (LONGLONG)expected;
}
static NTSTATUS DcnReadScanoutAddress(BC250_DEVICE *d, ULONGLONG *out)
{
    *out=(ULONGLONG)displayed;
    if(inject_fallback_update){inject_fallback_update=0;d->Wddm->PrimarySequence+=2;}
    return STATUS_SUCCESS;
}
static int DcnHubpDrained(const BC250_DEVICE *d)
{
    (void)d;
    ++hubp_reads;
    if(hubp_drain_after) return hubp_reads>=hubp_drain_after?1:0;
    return hubp_state;
}
static int DcnOtgRunning(const BC250_DEVICE *d){(void)d;CHECK(in_lock);++otg_reads;return otg_state;}
static NTSTATUS DcnFaultBlank(BC250_DEVICE *d){(void)d;CHECK(in_lock);++fault_blanks;return fault_blank_status;}
/* Programming is not retirement. The separate actual-DCN suite measures full-frame EARLIEST lag. This boundary
 * leaves the old displayed surface until the PASSIVE retirement operation; the hook injects OS work meanwhile. */
static NTSTATUS DcnWriterBridge(BC250_DEVICE *d,volatile LONG *generation,ULONG expected,BOOLEAN *wrote,
                              BC250_DCN_WRITER_BRIDGE *bridge)
{
    CHECK(in_lock && generation==&d->Wddm->PrimarySequence && (ULONG)*generation==expected && (expected&1));
    CHECK(d->Wddm->WriterActive && !d->Wddm->WriterReady);
    ++bridge_writes;*wrote=NT_SUCCESS(bridge_result) || bridge_wrote;
    memset(bridge,0,sizeof(*bridge));bridge->Physical=BRIDGE_CARD;
    if(NT_SUCCESS(bridge_result)){plane_card=(LONGLONG)BRIDGE_CARD;pending=0;}
    return bridge_result;
}
static NTSTATUS DcnWriterBridgeRetire(BC250_DEVICE *d,volatile LONG *generation,ULONG expected,
                                     const BC250_DCN_WRITER_BRIDGE *bridge)
{
    CHECK(!in_lock && irq==PASSIVE_LEVEL && d->Wddm->WriterActive && !d->Wddm->WriterReady);
    CHECK(generation==&d->Wddm->PrimarySequence && (ULONG)*generation==expected);
    CHECK(bridge->Physical==BRIDGE_CARD);
    ++bridge_retires;
    if(retire_hook){void (*hook)(BC250_DEVICE*)=retire_hook;retire_hook=NULL;hook(d);}
    if(NT_SUCCESS(bridge_retire_result)) displayed=(LONGLONG)BRIDGE_CARD;
    return bridge_retire_result;
}
/* BD-170: a writer the summary's copy can meet. Each copy of a record moves the slot's Version by writer_step
 * for writer_copies copies (a writer that finished one or more records meanwhile). */
static unsigned record_copies,writer_copies;
static LONG writer_step;
/* Stalls: 1 us between record copies and between drain reads, 10 us between a writer's tries. Never inside L. */
static void KeStallExecutionProcessor(ULONG us)
{
    CHECK(!in_lock && (us==1 || us==10));
    if(us==1)++stalls1;else ++stalls10;
}
static void TestRecordCopy(void *to,const void *from,size_t bytes)
{
    BC250_DCN_IMMEDIATE_RECORD *slot=(BC250_DCN_IMMEDIATE_RECORD*)(ULONG_PTR)from;
    CHECK(bytes==sizeof(BC250_DCN_IMMEDIATE_RECORD) && !(slot->Version&1));
    memcpy(to,from,bytes);
    ++record_copies;
    if(writer_copies){--writer_copies;slot->Version+=writer_step;slot->Sequence+=8;}
}
#define RtlCopyMemory(to,from,bytes) TestRecordCopy((to),(from),(bytes))
static void StartHealthCompleted(BC250_DEVICE *d,ULONG sequence)
{
    (void)d;
    if(sequence && sequence!=healthSequence){healthSequence=sequence;healthCompletions++;}
}
/* ACTUAL_SOURCE */
/* A started adapter on the inherited 1366x768 mode, its desktop primary at 0x1000, the flip gate open, CRTC_VSYNC on,
 * the OTG running and the plane drained; every stub reset. level 1: the flip DDI arrives above DISPATCH_LEVEL. */
static void Setup(BC250_DEVICE *d,BC250_WDDM *w,int level)
{
    memset(d,0,sizeof(*d));memset(w,0,sizeof(*w));
    d->Post.Width=1366;d->Post.Height=768;d->Post.Pitch=5632;w->PrimaryPitch=5632;d->DcnQpcFrequency=1000000;
    w->PrimaryBytes=5632ull*768;w->PrimaryPlaneFormat=BC250_PLANE_FORMAT_ARGB8888;
    d->Wddm=w;d->VidPnFlipEnabled=1;w->VSyncEnabled=1;w->ScanoutAdmitGate=TRUE;
    w->PrimaryAddress.QuadPart=0x1000;w->PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    d->Dxgk.DeviceHandle=d;d->Dxgk.DxgkCbSynchronizeExecution=TestSync;
    d->Dxgk.DxgkCbNotifyInterrupt=TestNotify;d->Dxgk.DxgkCbQueueDpc=TestQueueDpc;
    irq=level?TEST_DIRQL:0;in_lock=0;syncs=sync_refusals=sync_refuse_at=0;sync_no_callback=0;
    sync_refuse_routine=after_section_routine=NULL;before_section=after_section=NULL;
    reports=faults_notified=dpcs=0;reported=0;memset(&fault_data,0,sizeof(fault_data));
    arms=logs=stalls1=stalls10=0;
    freed=journaled=hardware=healthCompletions=pairedVsyncReports=0;healthSequence=0;
    hardware_result=STATUS_SUCCESS;
    hardware_wrote=pending=inject_update=inject_fallback_update=0;
    plane_card=displayed=0x1000;programmed_pitch=programmed_format=0;expect_record=0;record_checked=0;
    hubp_state=1;otg_state=1;hubp_reads=hubp_drain_after=otg_reads=fault_blanks=0;fault_blank_status=STATUS_SUCCESS;
    bridge_writes=0;bridge_result=STATUS_SUCCESS;bridge_wrote=FALSE;
    bridge_retire_result=STATUS_SUCCESS;bridge_retires=0;retire_hook=NULL;
    immediate_writes=observed=pending_samples=0;programmed_immediate=0;
    observe_outcome=BC250_DCN_IMMEDIATE_DONE;observed_flags=0;observed_address=0;
    ringGapVsyncs=0;begin_kind=BC250_WRITER_MODESET_COMMIT;
    shape_on=0;memset(&shape_now,0,sizeof(shape_now));shaped_writes=0;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    witness_arms=witness_reads=0;witness_fresh=witness_event=witness_r1=witness_r2=1;witness_status=STATUS_SUCCESS;
    witness_pending_before=witness_pending_after=0;
#endif
}
/* The vblank DPC, at DISPATCH_LEVEL outside L. */
static void Vblank(BC250_DEVICE *d){const int level=irq;irq=DISPATCH_LEVEL;d->DcnVsyncAcked=1;WddmDcnVsync(d);irq=level;}
/* pnp.c's DPC, which makes E5's notification. */
static void FaultDpc(BC250_DEVICE *d){const int level=irq;irq=DISPATCH_LEVEL;WddmFlipFaultDpc(d);irq=level;}
/* The PASSIVE_LEVEL callers: DestroyAllocation, and a writer's Begin, Resume and End. */
static NTSTATUS Destroy(BC250_DEVICE *d,HANDLE allocation)
{
    DXGKARG_DESTROYALLOCATION destroy={0};
    HANDLE list[1];
    const int level=irq;
    NTSTATUS status;
    list[0]=allocation;destroy.NumAllocations=1;destroy.pAllocationList=list;
    irq=0;status=Bc250WddmDestroyAllocation(d,&destroy);irq=level;
    return status;
}
static BOOLEAN Begin(BC250_DEVICE *d,ULONG *generation)
{const int level=irq;BOOLEAN taken;irq=0;taken=WddmPrimaryExclusiveBegin(d,begin_kind,generation);irq=level;return taken;}
/* The summary's copy of the W-u records, from PASSIVE_LEVEL. */
static BOOLEAN WuCopy(BC250_DEVICE *d,BC250_WDDM_WU_COPY *copy)
{const int level=irq;BOOLEAN copied;irq=0;copied=WddmWuCopy(d,d->Wddm,copy);irq=level;return copied;}
static NTSTATUS Resume(BC250_DEVICE *d)
{const int level=irq;NTSTATUS status;irq=0;status=WddmWriterResume(d);irq=level;return status;}
static NTSTATUS End(BC250_DEVICE *d,ULONG generation,BOOLEAN invalidate)
{const int level=irq;NTSTATUS status;irq=0;status=WddmPrimaryExclusiveEnd(d,generation,invalidate);irq=level;return status;}
/* E5's notification as the DPC made it: DMA_PAGE_FAULTED with the payload of section 7.2. */
static void CheckFault(ULONG code,UINT node)
{
    CHECK(faults_notified==1 && fault_data.InterruptType==DXGK_INTERRUPT_DMA_PAGE_FAULTED && DXGK_INTERRUPT_DMA_PAGE_FAULTED==9);
    CHECK(fault_data.DmaPageFaulted.FaultedFenceId==0 && fault_data.DmaPageFaulted.FaultedVirtualAddress==0);
    CHECK(fault_data.DmaPageFaulted.FaultedPrimitiveAPISequenceNumber==0xFFFFFFFFFFFFFFFFull);
    CHECK(fault_data.DmaPageFaulted.FaultedPipelineStage==0 && fault_data.DmaPageFaulted.FaultedBindTableEntry==0xFFFFFFFFu);
    /* FENCE_INVALID 0x2 and ADAPTER_RESET_REQUIRED 0x4, nothing else. */
    CHECK((unsigned)fault_data.DmaPageFaulted.PageFaultFlags==0x6u);
    CHECK(fault_data.DmaPageFaulted.NodeOrdinal==node && fault_data.DmaPageFaulted.EngineOrdinal==0);
    CHECK(fault_data.DmaPageFaulted.PageTableLevel==0 && fault_data.DmaPageFaulted.FaultedProcessHandle==NULL);
    CHECK(fault_data.DmaPageFaulted.FaultErrorCode.IsDeviceSpecificCode==1);
    CHECK(fault_data.DmaPageFaulted.FaultErrorCode.DeviceSpecificCode==code);
}
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
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT allocation={0};
    unsigned written;

    allocation.Magic=BC250_WDDM_MAGIC_ALLOCATION;

    request.Flags.Value=0;request.Flags.ModeChange=1;CHECK(request.Flags.Value==0x00000001u);
    request.Flags.Value=0;request.Flags.FlipImmediate=1;CHECK(request.Flags.Value==0x00000002u);
    request.Flags.Value=0;request.Flags.FlipOnNextVSync=1;CHECK(request.Flags.Value==0x00000004u);
    request.Flags.Value=0;request.Flags.FlipStereo=1;CHECK(request.Flags.Value==0x00000008u);
    request.Flags.Value=0;request.Flags.FlipStereoTemporaryMono=1;CHECK(request.Flags.Value==0x00000010u);
    request.Flags.Value=0;request.Flags.FlipStereoPreferRight=1;CHECK(request.Flags.Value==0x00000020u);
    request.Flags.Value=0;request.Flags.SharedPrimaryTransition=1;CHECK(request.Flags.Value==0x00000040u);
    request.Flags.Value=0;request.Flags.IndependentFlipExclusive=1;CHECK(request.Flags.Value==0x00000080u);
    request.Flags.Value=0;request.Flags.MoveFlip=1;CHECK(request.Flags.Value==0x00000100u);

    Setup(&d,&w,0);
    /* The compositor's own primary, with the two bits that say the OS entered DirectFlip and then
     * independent flip. Both are counted and neither changes the answer. */
    request.hAllocation=NULL;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    request.PrimaryAddress.QuadPart=0x2000;
    request.Flags.Value=0;request.Flags.SharedPrimaryTransition=1;request.Flags.IndependentFlipExclusive=1;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(w.ScanoutFlipFlags[0]==0 && w.ScanoutFlipFlags[1]==0);
    CHECK(w.ScanoutFlipFlags[2]==1 && w.ScanoutFlipFlags[3]==1);
    request.PrimaryAddress.QuadPart=0x3000;
    request.Flags.Value=0;request.Flags.ModeChange=1;request.Flags.FlipImmediate=1;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
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
    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
    CHECK(w.ScanoutFlipFlags[2]==2 && hardware==written && w.ScanoutFlips==0);
    CHECK(w.FlipEdges[BC250_FLIP_EDGE_REFUSED]==1 && !w.Faulting);
    /* A call refused before the transaction is not counted twice: VidPnSourceId != 0 never gets that far. */
    request.VidPnSourceId=1;
    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
    CHECK(w.ScanoutFlipFlags[2]==2 && w.FlipEdges[BC250_FLIP_EDGE_REFUSED]==1);
    request.VidPnSourceId=0;
}
/* M15.14 (0.7.216.20): the plane format travels with the flip. Each admitted SCANOUT_PRIMARY format reaches the
 * hardware with its own plane format, the same address in another format is a change, a NULL allocation keeps
 * the published format, and the composed-only formats (A8, A16B16G16R16F) and a mismatched pitch are refused
 * before hardware. The compositor's B8G8R8A8 flip takes the plane back to the firmware's format. */
static void PlaneFormatFlips(void)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT game={0},desktop={0};
    unsigned written;
    Setup(&d,&w,0);
    game.Magic=BC250_WDDM_MAGIC_ALLOCATION;game.Serial=0x7001;game.UmdAlloc=1;game.ScanoutRequested=1;
    game.ScanoutWidth=1366;game.ScanoutHeight=768;game.ScanoutPitch=5632;game.UmdBytes=5632ull*768;
    game.ScanoutFormat=D3DDDIFMT_A8B8G8R8;
    desktop.Magic=BC250_WDDM_MAGIC_ALLOCATION;desktop.Serial=0x7002;
    desktop.Allocation.Width=1366;desktop.Allocation.Height=768;desktop.Allocation.Pitch=5632;
    desktop.Allocation.Format=D3DDDIFMT_A8R8G8B8;desktop.Allocation.Size=5632ull*768;
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    /* The Witcher 3 shape: an R8G8B8A8 swap chain (lab session 458). */
    request.hAllocation=&game;request.PrimaryAddress.QuadPart=0x10000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==1 && programmed_format==BC250_PLANE_FORMAT_ABGR8888 && w.PrimaryPlaneFormat==BC250_PLANE_FORMAT_ABGR8888);
    CHECK(w.ScanoutFlips==1 && w.ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ABGR8888]==1 && w.ScanoutObject==(LONGLONG)game.Serial);
    /* Same address, same pitch, another format: a change, so the hardware is written. */
    game.ScanoutFormat=D3DDDIFMT_A2B10G10R10;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==2 && programmed_format==BC250_PLANE_FORMAT_ABGR2101010);
    CHECK(w.ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ABGR2101010]==1 && w.PrimaryPlaneFormat==BC250_PLANE_FORMAT_ABGR2101010);
    /* The identical flip again is no change. */
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==2);
    /* A NULL allocation keeps the published format and pitch. */
    request.hAllocation=NULL;request.PrimaryAddress.QuadPart=0x20000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==3 && programmed_format==BC250_PLANE_FORMAT_ABGR2101010 && programmed_pitch==5632);
    /* The compositor's own B8G8R8A8 primary takes the plane back to the firmware's format. */
    request.hAllocation=&desktop;request.PrimaryAddress.QuadPart=0x30000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==4 && programmed_format==BC250_PLANE_FORMAT_ARGB8888 && w.PrimaryPlaneFormat==BC250_PLANE_FORMAT_ARGB8888);
    CHECK(w.ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ARGB8888]==1 && w.ScanoutObject==0);
    /* Refused before hardware: the composed-only rows and a pitch that is not whole pixels of the row. The
     * plane keeps the surface and the format it has. */
    {
        LONG formats=w.ScanoutAdmits[BC250_SCANOUT_FORMAT],pitches=w.ScanoutAdmits[BC250_SCANOUT_PITCH];
        written=hardware;request.hAllocation=&game;request.PrimaryAddress.QuadPart=0x40000;
        game.ScanoutFormat=D3DDDIFMT_A16B16G16R16F;game.ScanoutPitch=1366*8;game.UmdBytes=1366ull*8*768;
        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
        game.ScanoutFormat=D3DDDIFMT_A8;game.ScanoutPitch=1408;game.UmdBytes=1408ull*768;
        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
        CHECK(w.ScanoutAdmits[BC250_SCANOUT_FORMAT]==formats+2);
        game.ScanoutFormat=D3DDDIFMT_A8B8G8R8;game.UmdBytes=5632ull*768;
        game.ScanoutPitch=1366*4-4;     /* shorter than a row */
        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
        game.ScanoutPitch=5634;         /* not a whole number of 4-byte pixels */
        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
        CHECK(w.ScanoutAdmits[BC250_SCANOUT_PITCH]==pitches+2);
        CHECK(hardware==written && w.PrimaryPlaneFormat==BC250_PLANE_FORMAT_ARGB8888 && w.PrimaryAddress.QuadPart==0x30000);
        game.ScanoutPitch=5632;
    }
    /* The kernel refuses another format before its write sequence when the firmware's format was not captured
     * (dcn.c, STATUS_NOT_SUPPORTED): nothing is published and the format stays. */
    hardware_result=STATUS_NOT_SUPPORTED;
    CHECK(Flip(&d,&request)==STATUS_NOT_SUPPORTED);
    CHECK(w.PrimaryPlaneFormat==BC250_PLANE_FORMAT_ARGB8888 && w.PrimaryAddress.QuadPart==0x30000);
    CHECK(w.ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ABGR8888]==1 && w.ScanoutObject==0);
    CHECK(w.FlipResults[BC250_FLIP_RESULT_REFUSED]==1 && !w.Faulting);
    hardware_result=STATUS_SUCCESS;
    /* The game's buffer scanned out again, then destroyed: the firmware surface comes back and the next flip
     * starts from the firmware's format. */
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(programmed_format==BC250_PLANE_FORMAT_ABGR8888 && w.ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ABGR8888]==2);
    CHECK(Destroy(&d,&game)==STATUS_SUCCESS);
    CHECK(plane_card==(LONGLONG)BRIDGE_CARD && w.PrimaryPlaneFormat==0 && w.PrimaryPitch==0);
    request.hAllocation=NULL;request.PrimaryAddress.QuadPart=0x50000;w.PrimaryPitch=0;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(programmed_format==BC250_PLANE_FORMAT_ARGB8888 && programmed_pitch==5632);
    /* A detached allocation has no private pitch; use the current runtime
     * handoff pitch rather than the immutable boot surface. */
    d.RuntimePostValid=TRUE;d.RuntimePost.Pitch=5888;w.PrimaryPitch=0;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(programmed_pitch==5888 && w.PrimaryPitch==5888 && d.Post.Pitch==5632);
}
/* The publication path through the actual DDI: a refusal before the write sequence publishes nothing, a programmed
 * next-VSync request is Desired and owed until a vblank report carries it, every vblank reports (the old buffer while
 * the new one is not proven on the plane), and the admission of the compositor's primary and of an application's
 * scan-out surface, with the teardown record that ties the plane to the surface it reads. At PASSIVE and above
 * DISPATCH alike. */
static void Publication(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    PHYSICAL_ADDRESS sample={-1};
    unsigned before;
    int gapVsyncsBefore;
    Setup(&d,&w,level);
    w.PrimaryAddress.QuadPart=10;w.PrimarySegment=1;plane_card=displayed=10;
    request.PrimaryAddress.QuadPart=20;request.PrimarySegment=2;
    /* dcn.c refused the request before its write sequence (Wrote FALSE): A after the fact. Nothing is published, the
     * plane keeps what it had, and the answer is dcn.c's. */
    hardware_result=STATUS_ACCESS_DENIED;
    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED);
    CHECK(w.PrimaryAddress.QuadPart==10 && w.PrimarySegment==1 && w.Flips==0);
    CHECK(plane_card==10 && hardware==1 && !(w.PrimarySequence&1) && arms==(level?0u:1u));
    CHECK(w.FlipEdges[BC250_FLIP_EDGE_PROGRAM]==1 && w.FlipResults[BC250_FLIP_RESULT_REFUSED]==1 && !w.Faulting);
    CHECK(checks_at_vblank>0 && !w.DesiredOwed);
    before=reports;Vblank(&d);
    CHECK(reports==before+1 && reported==10);
    CHECK(healthCompletions==0); // rejected flip is not progress
    hardware_result=STATUS_SUCCESS;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(plane_card==20 && w.PrimaryAddress.QuadPart==20 && w.PrimarySegment==2);
    CHECK(w.Flips==1 && hardware==2 && !(w.PrimarySequence&1) && w.FlipResults[BC250_FLIP_RESULT_SUCCESS]==1);
    CHECK(w.DesiredOwed);   // a next-VSync success: owed until a report carries it
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==2 && w.Flips==1 && w.DesiredOwed);
    // Every vblank reports; neither pending nor cleared-before-inuse retires 20, and the old buffer completes nothing.
    before=reports;pending=1;Vblank(&d);
    CHECK(reports==before+1 && reported==10);
    before=reports;pending=0;Vblank(&d);
    CHECK(reports==before+1 && reported==10);
    CHECK(d.DcnVsyncOldBufferReports==2 && w.DesiredOwed && w.OwedReported==0);
    CHECK(healthCompletions==0); // old-buffer fallback is not progress
    // Fallback is also generation-protected, even for an ABA writer.
    before=reports;gapVsyncsBefore=ringGapVsyncs;pending=1;inject_fallback_update=1;Vblank(&d);
    CHECK(reports==before);
    // C48: the report was refused, the vblank still happened and its time was taken.
    CHECK(ringGapVsyncs==gapVsyncsBefore+1);
    // Matching address alone cannot override an asserted pending bit.
    before=reports;gapVsyncsBefore=ringGapVsyncs;displayed=20;pending=1;Vblank(&d);
    CHECK(reports==before && ringGapVsyncs==gapVsyncsBefore+1);
    // The report that carries 20 completes the owed success, inside L (WddmNotifyRoutine).
    pending=0;Vblank(&d);
    CHECK(reports==before+1 && reported==20);
    CHECK(!w.DesiredOwed && w.OwedReported==1);
    CHECK(healthCompletions==1);
    Vblank(&d);
    CHECK(healthCompletions==1 && w.OwedReported==1); // repeated completed primary is not progress
    // C50: every report this fixture made is a vblank one, and every one of them was counted as owing a
    // notification and queued the DPC that pairs it. A refused vblank (above) counts neither.
    CHECK(pairedVsyncReports==reports && dpcs==reports);
    // A whole new transaction during the observation invalidates completion.
    inject_update=1;
    CHECK(!WddmReadCompletedPrimary(&d,&w,&sample,NULL));
    CHECK(sample.QuadPart==-1);
    CHECK(!WddmReadCompletedPrimary(&d,&w,&sample,NULL));
    CHECK(sample.QuadPart==-1 && w.PrimaryAddress.QuadPart==30);
    displayed=30;
    CHECK(WddmReadCompletedPrimary(&d,&w,&sample,NULL) && sample.QuadPart==30);
    CHECK(w.PrimarySegment==3 && hardware==3);
    CHECK(level ? arms==0 : arms==4);
    request.VidPnSourceId=1;CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
    CHECK(hardware==3);
    {
        BC250_WDDM_OBJECT allocation={0};
        allocation.Magic=BC250_WDDM_MAGIC_ALLOCATION;
        allocation.Allocation.Width=1366;allocation.Allocation.Height=768;
        allocation.Allocation.Pitch=5888;allocation.Allocation.Format=D3DDDIFMT_A8R8G8B8;
        allocation.Allocation.Size=5888ull*768;
        request.VidPnSourceId=0;request.PrimaryAddress.QuadPart=30;
        request.hAllocation=&allocation;
        hardware_result=STATUS_ACCESS_DENIED;
        CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED);
        CHECK(w.PrimaryPitch==5632 && hardware==4 && programmed_pitch==5632);
        hardware_result=STATUS_SUCCESS;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS);
        CHECK(w.PrimaryPitch==5888 && hardware==5 && programmed_pitch==5888);
        request.hAllocation=NULL;request.PrimaryAddress.QuadPart=40;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS);
        CHECK(programmed_pitch==5888 && w.PrimaryBytes==5888ull*768);
        request.hAllocation=&allocation;allocation.Allocation.Size--;
        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
        CHECK(hardware==6 && !(w.PrimarySequence&1));
        {
            // Only the table's SCANOUT_PRIMARY rows with a plane encoding are scanned out (M15.14:
            // A8R8G8B8, X8R8G8B8, and from 0.7.216.20 A8B8G8R8 and A2B10G10R10, PlaneFormatFlips above).
            // A8, X8B8G8R8, A16B16G16R16F and the rest are refused before hardware.
            static const ULONG refused[]={0,28,33,35,113};
            unsigned k;
            allocation.Allocation.Size=5888ull*768;request.PrimaryAddress.QuadPart=50;
            for(k=0;k<sizeof(refused)/sizeof(refused[0]);k++){
                allocation.Allocation.Format=refused[k];
                CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            }
            CHECK(hardware==6 && w.PrimaryAddress.QuadPart==40 && !(w.PrimarySequence&1));
            allocation.Allocation.Format=D3DDDIFMT_X8R8G8B8;
            CHECK(Flip(&d,&request)==STATUS_SUCCESS);
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
            CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(w.ScanoutAdmits[BC250_SCANOUT_NOT_REQUESTED]==1 && w.ScanoutRequests==0);
            CHECK(w.Flips==flips && (LONG)hardware==hw && !(w.PrimarySequence&1));
            // The same allocation, now asking for scan-out and describing the POST surface: the
            // first flip of an application buffer this driver has ever programmed.
            umd.ScanoutRequested=1;
            CHECK(Flip(&d,&request)==STATUS_SUCCESS);
            CHECK(w.ScanoutRequests==1 && w.ScanoutAdmits[BC250_SCANOUT_ADMIT_OK]>=1);
            CHECK(w.Flips==flips+1 && w.ScanoutFlips==1 && programmed_pitch==5632);
            CHECK(w.PrimaryAddress.QuadPart==0x4000 && w.PrimaryBytes==5632ull*768);
            // Each refusal of a described surface: the wrong segment, an unaligned address, a
            // composed-only format, and geometry that is not the POST mode's. None reaches the
            // hardware, none publishes, and the plane keeps the surface it has.
            hw=(LONG)hardware;flips=w.Flips;
            {LONG formats=w.ScanoutAdmits[BC250_SCANOUT_FORMAT],sizes=w.ScanoutAdmits[BC250_SCANOUT_SIZE];
            request.PrimaryAddress.QuadPart=0x8000;request.PrimarySegment=2;
            CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(w.ScanoutAdmits[BC250_SCANOUT_SEGMENT]==1);
            request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x8010;
            CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==1);
            request.PrimaryAddress.QuadPart=0x8000;umd.ScanoutFormat=D3DDDIFMT_A16B16G16R16F;
            CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(w.ScanoutAdmits[BC250_SCANOUT_FORMAT]==formats+1);
            umd.ScanoutFormat=D3DDDIFMT_A8R8G8B8;umd.ScanoutHeight=767;
            CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(w.ScanoutAdmits[BC250_SCANOUT_GEOMETRY]==1);
            umd.ScanoutHeight=768;umd.UmdBytes=5632ull*768-1;
            CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(w.ScanoutAdmits[BC250_SCANOUT_SIZE]==sizes+1);
            }
            CHECK((LONG)hardware==hw && w.Flips==flips && w.ScanoutFlips==1 && !(w.PrimarySequence&1));
            CHECK(w.PrimaryAddress.QuadPart==0x4000 && w.PrimaryPitch==5632);
            // A handle that is not an allocation of this adapter.
            request.hAllocation=(HANDLE)&w;
            CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
            CHECK(w.ScanoutAdmits[BC250_SCANOUT_NO_ALLOCATION]==1);
            {
                // The two bases the card address is translated through. The predicate's 4 KiB test
                // is on the card address; the plane is given Address - VramMcBase + VramPhysical,
                // so page alignment only carries over while both bases are page aligned, and a
                // board where they are not refuses scan-out instead of programming the plane.
                BC250_WDDM_OBJECT good={0};
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
                CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==aligned+1 && w.ScanoutObject==record);
                d.VramPhysical.QuadPart=0x270000000;d.VramMcBase=0x800;
                CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
                CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==aligned+2);
                d.VramMcBase=0;
                // Admitted, programmed, and recorded as the surface the plane is reading.
                CHECK(Flip(&d,&request)==STATUS_SUCCESS);
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
                    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                    CHECK(w.ScanoutFlips==admitted+1 && w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==refusals);
                    request.PrimaryAddress.QuadPart+=(LONGLONG)Bc250ScanoutCreateAlignment(0);
                    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
                    CHECK(w.ScanoutAdmits[BC250_SCANOUT_ALIGNMENT]==refusals+1 && w.ScanoutFlips==admitted+1);
                    request.PrimaryAddress.QuadPart=0xC000;
                    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                    CHECK(w.ScanoutFlips==admitted+2 && w.ScanoutObject==(LONGLONG)good.Serial);
                }
                {   // With the flip gate closed this DDI still succeeds and still publishes, and
                    // no scan-out flip is counted: nothing was written to the display hardware,
                    // and a counter that moved here would report the whole increment's result on
                    // a machine where no address ever reached HUBP0.
                    LONG gated=w.ScanoutFlips;unsigned gatedHardware=hardware;
                    d.VidPnFlipEnabled=0;request.PrimaryAddress.QuadPart=0xD000;
                    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                    CHECK(w.ScanoutFlips==gated && hardware==gatedHardware && w.ScanoutObject==0 && w.DesiredOwed);
                    d.VidPnFlipEnabled=1;request.PrimaryAddress.QuadPart=0xC000;
                    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                    CHECK(w.ScanoutFlips==gated+1 && w.ScanoutObject==(LONGLONG)good.Serial);
                }
                // Freeing the surface the plane is reading restores the firmware surface before
                // the object goes: an application swap-chain buffer has no lifetime tie to the
                // video present source, so this is the only thing between a crashed game and a
                // plane scanning VRAM that VidMm has handed to somebody else. Begin first proves a
                // reserved bridge; flips during the writer are refused until End releases it.
                freed=journaled=0;
                // The flip lines are written per frame and spend their allowance in the first
                // seconds; the teardown line keeps its own, so the one line that says the plane
                // was taken back is still written after a long run.
                w.ScanoutNotes=notes=BC250_WDDM_LOG_CALLS*4;
                CHECK(Destroy(&d,&good)==STATUS_SUCCESS);
                CHECK(w.ScanoutTeardowns==1 && w.ScanoutNotes==notes);
                CHECK(plane_card==(LONGLONG)BRIDGE_CARD && freed==1 && journaled==1 && w.ScanoutObject==0);
                CHECK(w.PrimaryNeedsRestore && w.PrimaryAddress.QuadPart==0 && w.PrimaryPitch==0);
                CHECK(w.PrimaryPlaneFormat==0);   // M15.14: the restore put the firmware's format back
                CHECK(!w.DesiredOwed && !w.WriterActive && !(w.PrimarySequence&1) && w.DestroyUnsynchronized==0);
                // Any other allocation's destroy touches neither the plane nor the record.
                w.PrimaryNeedsRestore=FALSE;freed=0;
                CHECK(Destroy(&d,&umd)==STATUS_SUCCESS);
                CHECK(freed==1 && !w.PrimaryNeedsRestore && w.ScanoutObject==0);
                // The interleaving the tie exists for: dxgkrnl frees the buffer of a flip that is still
                // inside SetVidPnSourceAddress, after the plane has been written with it. The record must
                // already name that buffer while the plane is written, or a destroy on another processor
                // misses, frees it, and leaves HUBP0 reading VRAM that VidMm may hand to somebody else. The
                // record is 0 before this flip, so a record published after the programming would make this
                // the unrecoverable case. That destroy's section waits for L and runs after the flip.
                {
                    LONG teardowns=w.ScanoutTeardowns;
                    LONGLONG scanned;
                    request.hAllocation=&good;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
                    request.PrimaryAddress.QuadPart=0xE000;
                    hardware_result=STATUS_SUCCESS;freed=journaled=0;
                    expect_record=(LONGLONG)good.Serial;
                    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                    CHECK(record_checked==1 && !expect_record);
                    CHECK(Destroy(&d,&good)==STATUS_SUCCESS);
                    CHECK(plane_card==(LONGLONG)BRIDGE_CARD && freed==1 && journaled==1);
                    CHECK(w.ScanoutTeardowns==teardowns+1 && w.ScanoutObject==0);
                    // A programming refused before its write sequence gave the plane no address it keeps, so
                    // the record goes back to the object the previous flip left there.
                    w.PrimaryNeedsRestore=FALSE;request.PrimaryAddress.QuadPart=0xC000;
                    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                    scanned=w.ScanoutObject;
                    CHECK(scanned==(LONGLONG)good.Serial);
                    request.hAllocation=NULL;request.PrimaryAddress.QuadPart=0xF000;
                    hardware_result=STATUS_ACCESS_DENIED;
                    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED);
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
                        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
                        CHECK(w.ScanoutAdmits[BC250_SCANOUT_GATED]==gated+1);
                        CHECK(w.ScanoutFlips==gatedFlips && hardware==written && w.ScanoutObject==scanned);
                        request.hAllocation=&allocation;request.PrimarySegment=2;
                        request.PrimaryAddress.QuadPart=0x50;
                        CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                        CHECK(hardware==written+1 && w.ScanoutFlips==gatedFlips && w.ScanoutObject==0);
                        w.ScanoutAdmitGate=TRUE;
                    }
                    CHECK(!strcmp(Bc250ScanoutStatusText(BC250_SCANOUT_GATED),"gated"));
                    // Give the record back, so what follows starts from the compositor's own primary.
                    request.hAllocation=&good;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
                    request.PrimaryAddress.QuadPart=0xC000;
                    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
                    CHECK(w.ScanoutObject==scanned);
                    // The record names the object's serial and not its address. The same storage with a
                    // new serial is what a freed object and the next allocation at its address look
                    // like, and it must not match: a pointer record would have matched and taken the
                    // plane back to the firmware surface for a buffer nobody destroyed.
                    good.Serial=0x9002;freed=journaled=0;
                    CHECK(Destroy(&d,&good)==STATUS_SUCCESS);
                    CHECK(freed==1 && w.ScanoutObject==scanned);
                    // A serial of 0 is the record's own "nothing is being scanned out" and must never
                    // match either, whatever the record holds.
                    good.Serial=0;freed=journaled=0;
                    CHECK(Destroy(&d,&good)==STATUS_SUCCESS);
                    CHECK(freed==1 && w.ScanoutObject==scanned);
                    good.Serial=0x9001;freed=journaled=0;
                    CHECK(Destroy(&d,&good)==STATUS_SUCCESS);
                    CHECK(plane_card==(LONGLONG)BRIDGE_CARD && w.ScanoutObject==0);
                    w.PrimaryNeedsRestore=FALSE;
                }
                w.PrimaryPitch=5632;w.PrimaryAddress.QuadPart=0x4000;
            }
            request.hAllocation=NULL;request.PrimarySegment=3;
        }
    }
    request.hAllocation=NULL;request.PrimaryAddress=w.PrimaryAddress;
    w.PrimaryNeedsRestore=TRUE;before=hardware;hardware_result=STATUS_ACCESS_DENIED;
    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED && w.PrimaryNeedsRestore);
    CHECK(hardware==before+1);
    hardware_result=STATUS_SUCCESS;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && !w.PrimaryNeedsRestore);
    CHECK(hardware==before+2); // identical OS address still reprograms lost hardware
    CHECK(!w.Faulting && w.FlipFaults==0 && faults_notified==0 && w.DestroyUnsynchronized==0);
}
/* BD-170: the immediate flip through the actual DDI. FlipImmediate reaches the hardware as the immediate flip type,
 * for that flip only, and P' is observed inside the transaction; a vsync flip is unchanged and is not observed; rapid
 * A-B-C and A-B-A each program every request, in order; a next-vsync request for B still pending and then an
 * immediate request for B programs B again, now immediate (same-address promotion), while a plane already on B, a
 * vsync repeat and a closed flip gate program nothing. P' in the call (DONE, DONE_LATE) is the success. At PASSIVE
 * and above DISPATCH alike. ImmediateUnproven below: an immediate request programmed without P'. */
static void ImmediateFlips(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    static const LONGLONG abc[]={0xB000,0xC000,0xD000}, aba[]={0xB000,0xC000,0xB000};
    unsigned before,seen,i;
    LONG flips;
    Setup(&d,&w,level);
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    /* Interval 1: the vsync flip type, no observation - the path 0.7.216.64 had. */
    request.PrimaryAddress.QuadPart=0xA000;request.Flags.Value=0;request.Flags.FlipOnNextVSync=1;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==1 && plane_card==0xA000 && !programmed_immediate && observed==0 && w.Flips==1 && w.DesiredOwed);
    displayed=0xA000;
    /* Interval 0, rapid A-B-C and then A-B-A: each request is a change, programmed immediate and observed. An
     * immediate success owes no report. */
    request.Flags.Value=0;request.Flags.FlipImmediate=1;
    for(i=0;i<3;i++){
        request.PrimaryAddress.QuadPart=abc[i];
        CHECK(Flip(&d,&request)==STATUS_SUCCESS);
        CHECK(plane_card==abc[i] && programmed_immediate && observed_address==abc[i] && !w.DesiredOwed);
        displayed=abc[i];
    }
    for(i=0;i<3;i++){
        request.PrimaryAddress.QuadPart=aba[i];
        CHECK(Flip(&d,&request)==STATUS_SUCCESS);
        CHECK(plane_card==aba[i] && programmed_immediate && observed_address==aba[i]);
        displayed=aba[i];
    }
    CHECK(hardware==7 && immediate_writes==6 && observed==6 && w.Flips==7 && !(w.PrimarySequence&1));
    CHECK(w.ScanoutFlipFlags[1]==6 && w.ImmediatePromotions==0 && w.FlipResults[BC250_FLIP_RESULT_SUCCESS]==7);
    /* An immediate request for the surface the plane is already on: nothing to program, nothing to observe. */
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==7 && observed==6 && w.ImmediatePromotions==0);
    /* Same-address promotion: a next-vsync request for E, which the hardware has not latched yet ... */
    request.Flags.Value=0;request.Flags.FlipOnNextVSync=1;request.PrimaryAddress.QuadPart=0xE000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==8 && plane_card==0xE000 && !programmed_immediate && observed==6);
    flips=w.Flips;pending=1;
    /* ... and its vsync repeat programs nothing, as before BD-170 ... */
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==8 && w.ImmediatePromotions==0);
    /* ... but an immediate request for E programs E again, immediate, and is observed. Not a new flip. */
    request.Flags.Value=0;request.Flags.FlipImmediate=1;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==9 && plane_card==0xE000 && programmed_immediate && observed==7 && observed_address==0xE000);
    CHECK(w.ImmediatePromotions==1 && w.Flips==flips && w.PrimaryAddress.QuadPart==0xE000 && !w.DesiredOwed);
    /* The pending bit alone is enough (EARLIEST_INUSE may already name E), and so is EARLIEST_INUSE alone. */
    displayed=0xE000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==10 && w.ImmediatePromotions==2);
    pending=0;displayed=0xB000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==11 && w.ImmediatePromotions==3);
    /* Once the hardware is on E, an immediate E is complete as it stands. */
    displayed=0xE000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==11 && w.ImmediatePromotions==3 && observed==9);
    /* A promotion that dcn.c refuses before its write sequence returns that refusal, publishes nothing new and is not
     * observed. */
    pending=1;hardware_result=STATUS_ACCESS_DENIED;seen=observed;before=hardware;
    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED);
    CHECK(hardware==before+1 && observed==seen && w.ImmediatePromotions==3 && w.PrimaryAddress.QuadPart==0xE000);
    CHECK(!(w.PrimarySequence&1) && !w.Faulting);
    hardware_result=STATUS_SUCCESS;
    /* With the flip gate closed no hardware is written, so there is nothing to promote or observe. */
    d.VidPnFlipEnabled=0;before=hardware;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==before && observed==seen && w.ImmediatePromotions==3);
    request.PrimaryAddress.QuadPart=0xF000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==before && observed==seen && w.PrimaryAddress.QuadPart==0xF000);
    d.VidPnFlipEnabled=1;pending=0;displayed=0xF000;
    /* After immediate flips, the next vsync flip carries the vsync type again. */
    request.Flags.Value=0;request.PrimaryAddress.QuadPart=0x10000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(plane_card==0x10000 && !programmed_immediate && observed==seen);
    /* A refused immediate programming is not observed either. */
    request.Flags.Value=0;request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x11000;
    hardware_result=STATUS_ACCESS_DENIED;
    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED);
    CHECK(observed==seen && w.PrimaryAddress.QuadPart==0x10000);
    hardware_result=STATUS_SUCCESS;
    /* DONE_LATE (the first sample already past the bound) is P' too: success. */
    observe_outcome=BC250_DCN_IMMEDIATE_DONE_LATE;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(observed==seen+1 && w.PrimaryAddress.QuadPart==0x11000 && !w.Faulting);
    /* The observation is legal above DISPATCH_LEVEL; only the vsync arm is skipped there. */
    CHECK(level ? w.FlipsAboveDispatch>0 : w.FlipsAboveDispatch==0);
    CHECK(w.FlipResults[BC250_FLIP_RESULT_UNPROVEN]==0 && w.FlipFaults==0 && dpcs==0);
}
/* BD-170 r6: U, an immediate request programmed without P' in the call (PENDING, PRIMARY_OTHER, READ_FAILED,
 * SUPERSEDED). The hardware has the request, so it is Desired; the answer is STATUS_DEVICE_HARDWARE_ERROR, never
 * success (the OS would release a surface the plane may still read) and never STATUS_DEVICE_BUSY (facts M902). E5:
 * FAULTING, the blank request and the DPC inside L, the node of the call's first context. From then on F0: every
 * request is answered the error with nothing programmed, and no writer can begin. The DPC makes one DMA_PAGE_FAULTED
 * notification and the drain record. */
static void ImmediateUnproven(int level)
{
    static const ULONG unproven[]={BC250_DCN_IMMEDIATE_PENDING,BC250_DCN_IMMEDIATE_PRIMARY_OTHER,
                                   BC250_DCN_IMMEDIATE_READ_FAILED,BC250_DCN_IMMEDIATE_SUPERSEDED};
    BC250_WDDM_OBJECT context={0};
    unsigned i;
    context.Magic=BC250_WDDM_MAGIC_CONTEXT;context.NodeOrdinal=1;
    for(i=0;i<sizeof(unproven)/sizeof(unproven[0]);i++){
        BC250_WDDM w;BC250_DEVICE d;
        DXGKARG_SETVIDPNSOURCEADDRESS request={0};
        ULONG generation=7;
        Setup(&d,&w,level);
        /* Half of the cases with the VSync off: r5b answered success there when its own vblank was not live, and r6
         * has no such exception. */
        w.VSyncEnabled=(i&1)?0:1;
        request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.Flags.FlipImmediate=1;
        request.ContextCount=1;request.Context[0]=&context;
        observe_outcome=unproven[i];request.PrimaryAddress.QuadPart=0x20000;
        CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
        CHECK(hardware==1 && observed==1 && plane_card==0x20000 && programmed_immediate);
        CHECK(w.PrimaryAddress.QuadPart==0x20000 && w.Flips==1 && !w.PrimaryNeedsRestore && !w.DesiredOwed);
        CHECK(w.FlipEdges[BC250_FLIP_EDGE_PROGRAM]==1 && w.FlipResults[BC250_FLIP_RESULT_UNPROVEN]==1);
        CHECK(w.Faulting==1 && w.FaultCode==BC250_FLIP_FAULT_UNPROVEN && w.FaultNode==1 && w.FlipFaults==1);
        /* Inside L: the blank request and the DPC request, no notification yet (DxgkCbNotifyInterrupt is not made
         * inside dxgmms2's own DIRQL call). */
        CHECK(fault_blanks==1 && dpcs==1 && w.FaultNotify==1 && faults_notified==0 && !(w.PrimarySequence&1));
        CHECK(arms==(level?0u:1u));
        /* F0: nothing programmed, the error answered, the fault not entered again. */
        request.PrimaryAddress.QuadPart=0x21000;
        CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
        request.Flags.Value=0;request.PrimaryAddress.QuadPart=0x22000;
        CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
        CHECK(hardware==1 && w.FlipEdges[BC250_FLIP_EDGE_FAULTING]==2 && w.FlipFaults==1 && dpcs==1);
        CHECK(w.PrimaryAddress.QuadPart==0x20000 && fault_blanks==1);
        /* The DPC: the notification once, with the drain record. */
        FaultDpc(&d);
        CheckFault(BC250_FLIP_FAULT_UNPROVEN,1);
        CHECK(w.FaultNotified==1 && w.FaultNotifyFailed==0 && w.FaultNotify==0 && dpcs==2);
        CHECK(w.FaultDrain==BC250_FLIP_DRAIN_DRAINED && w.FaultDrainReads==1);
        FaultDpc(&d);
        CHECK(faults_notified==1 && w.FaultNotified==1);
        /* No writer begins in FAULTING. */
        CHECK(!Begin(&d,&generation) && generation==0 && w.WriterFaulting==1 && !w.WriterActive);
    }
}
/* BD-170 r6: E5's DPC. The node: the call's first context when it is one of this driver's with a node in range, else
 * node 0. The drain record: at most BC250_FLIP_DRAIN_READS reads 1 us apart, DRAINED or UNKNOWN. The notification
 * that could not be made is counted and not made again; a fault in FAULTING is counted only. */
static void FaultDpcs(void)
{
    static const struct {int state;unsigned after;ULONG drain;LONG reads;unsigned stalls;} drains[]={
        {1,0,BC250_FLIP_DRAIN_DRAINED,1,0},
        {0,3,BC250_FLIP_DRAIN_DRAINED,3,2},
        {0,0,BC250_FLIP_DRAIN_UNKNOWN,(LONG)BC250_FLIP_DRAIN_READS,BC250_FLIP_DRAIN_READS-1},
        {-1,0,BC250_FLIP_DRAIN_UNKNOWN,1,0}};
    BC250_WDDM_OBJECT context={0},allocation={0};
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    unsigned i;
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x30000;
    for(i=0;i<sizeof(drains)/sizeof(drains[0]);i++){
        Setup(&d,&w,0);observe_outcome=BC250_DCN_IMMEDIATE_PENDING;
        CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR && w.Faulting);
        hubp_state=drains[i].state;hubp_drain_after=drains[i].after;hubp_reads=0;stalls1=0;
        FaultDpc(&d);
        CHECK(w.FaultDrain==(LONG)drains[i].drain && w.FaultDrainReads==drains[i].reads && stalls1==drains[i].stalls);
        CHECK(faults_notified==1 && w.FaultNotified==1);
    }
    /* The node: a context of this driver on node 1; another object; a node out of range; no context at all. */
    context.Magic=BC250_WDDM_MAGIC_CONTEXT;allocation.Magic=BC250_WDDM_MAGIC_ALLOCATION;allocation.NodeOrdinal=1;
    {
        static const struct {int which;UINT node;UINT expect;} nodes[]={{1,1,1},{2,1,0},{1,BC250_WDDM_NODE_COUNT_MAX,0},{0,1,0}};
        for(i=0;i<sizeof(nodes)/sizeof(nodes[0]);i++){
            Setup(&d,&w,0);observe_outcome=BC250_DCN_IMMEDIATE_PENDING;
            context.NodeOrdinal=nodes[i].node;
            request.ContextCount=nodes[i].which?1:0;
            request.Context[0]=nodes[i].which==1?(HANDLE)&context:(HANDLE)&allocation;
            CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR && w.FaultNode==nodes[i].expect);
            FaultDpc(&d);
            CheckFault(BC250_FLIP_FAULT_UNPROVEN,nodes[i].expect);
        }
        request.ContextCount=0;request.Context[0]=NULL;
    }
    /* The notification fails (no NotifyInterrupt): counted, NOT sent, the drain still recorded, and not tried again. */
    Setup(&d,&w,0);observe_outcome=BC250_DCN_IMMEDIATE_PENDING;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
    d.Dxgk.DxgkCbNotifyInterrupt=NULL;
    FaultDpc(&d);
    CHECK(w.FaultNotifyFailed==1 && w.FaultNotified==0 && w.FaultDrain==BC250_FLIP_DRAIN_DRAINED && w.FaultNotify==0);
    d.Dxgk.DxgkCbNotifyInterrupt=TestNotify;
    FaultDpc(&d);
    CHECK(faults_notified==0 && w.FaultNotifyFailed==1);
    /* No DPC could be requested at the fault: counted; the notification still waits for the next DPC of this driver. */
    Setup(&d,&w,0);observe_outcome=BC250_DCN_IMMEDIATE_PENDING;
    d.Dxgk.DxgkCbQueueDpc=NULL;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(w.FaultNotifyFailed==1 && w.FaultNotify==1 && dpcs==0 && fault_blanks==1);
    d.Dxgk.DxgkCbQueueDpc=TestQueueDpc;
    FaultDpc(&d);
    CHECK(faults_notified==1 && w.FaultNotified==1);
    /* The blank request's status is kept, whatever it is. */
    Setup(&d,&w,0);observe_outcome=BC250_DCN_IMMEDIATE_PENDING;fault_blank_status=STATUS_IO_TIMEOUT;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR && w.FaultBlank==STATUS_IO_TIMEOUT);
    /* No WDDM state: the DPC does nothing. */
    d.Wddm=NULL;FaultDpc(&d);d.Wddm=&w;
    CHECK(faults_notified==0);
}
/* A write sequence that ran and failed is unknown, never proof that nothing was written: E5 with
 * BC250_FLIP_FAULT_PROGRAM. Nothing is published, and the teardown record keeps the new object, which the plane may
 * read. Begin refuses in FAULTING, so destroy must keep the allocation instead of taking an unsynchronized fallback. */
static void FaultProgram(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT game={0},context={0};
    Setup(&d,&w,level);
    game.Magic=BC250_WDDM_MAGIC_ALLOCATION;game.Serial=0x6001;game.UmdAlloc=1;game.ScanoutRequested=1;
    game.ScanoutWidth=1366;game.ScanoutHeight=768;game.ScanoutPitch=5632;game.UmdBytes=5632ull*768;
    game.ScanoutFormat=D3DDDIFMT_A8R8G8B8;
    context.Magic=BC250_WDDM_MAGIC_CONTEXT;context.NodeOrdinal=1;
    w.ScanoutObject=0x6000;
    hardware_result=STATUS_IO_TIMEOUT;hardware_wrote=1;
    request.hAllocation=&game;request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x61000;
    request.ContextCount=1;request.Context[0]=&context;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(hardware==1 && w.FlipResults[BC250_FLIP_RESULT_FAULT_PROGRAM]==1 && w.ScanoutFlips==0);
    CHECK(w.Faulting && w.FaultCode==BC250_FLIP_FAULT_PROGRAM && w.FaultNode==1 && fault_blanks==1 && dpcs==1);
    CHECK(w.ScanoutObject==(LONGLONG)game.Serial && w.PrimaryAddress.QuadPart==0x1000 && !w.DesiredOwed);
    CHECK(!(w.PrimarySequence&1));
    /* F0 from here on: the same request again programs nothing. */
    hardware_result=STATUS_SUCCESS;hardware_wrote=0;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR && hardware==1 && w.FlipEdges[BC250_FLIP_EDGE_FAULTING]==1);
    FaultDpc(&d);
    CheckFault(BC250_FLIP_FAULT_PROGRAM,1);
    CHECK(!NT_SUCCESS(Destroy(&d,&game)));
    CHECK(freed==0 && w.DestroyUnsynchronized==0 && w.WriterFaulting==1);
    CHECK(w.ScanoutObject==(LONGLONG)game.Serial && w.PrimaryAddress.QuadPart==0x1000);
}
/* BD-170 r6: F1. A request that meets an odd generation with no writer, or a writer with an even generation, was not
 * serialized by L, which facts M904 says cannot happen: E5 with BC250_FLIP_FAULT_OVERLAP, nothing programmed. */
static void Overlap(int level)
{
    int k;
    for(k=0;k<2;k++){
        BC250_WDDM w;BC250_DEVICE d;
        DXGKARG_SETVIDPNSOURCEADDRESS request={0};
        Setup(&d,&w,level);
        if(k==0)w.PrimarySequence=3;else w.WriterActive=TRUE;
        request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x60000;
        CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
        CHECK(w.FlipEdges[BC250_FLIP_EDGE_OVERLAP]==1 && hardware==0 && w.PrimaryAddress.QuadPart==0x1000 && w.Flips==0);
        CHECK(w.Faulting && w.FaultCode==BC250_FLIP_FAULT_OVERLAP && w.FaultNode==BC250_WDDM_NODE_3D);
        CHECK(fault_blanks==1 && dpcs==1 && w.FaultNotify==1 && w.PrimarySequence==(k?0:3) && hubp_reads==0);
        FaultDpc(&d);
        CheckFault(BC250_FLIP_FAULT_OVERLAP,0);
    }
}
/* R7 writer ownership: Begin retires the application surface through the reserved bridge before it publishes
 * WriterActive. That proof does not fulfill a new request's immediate or next-VSync timing. Requests during a
 * hardware writer are refused without publishing anything; End restores the original Desired inside L. */
static void Writers(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT game[3];
    BC250_WDDM_WU_COPY copy;
    ULONG generation;
    unsigned k,i,hw,sections,timing;
    memset(game,0,sizeof(game));
    for(i=0;i<3;i++){
        game[i].Magic=BC250_WDDM_MAGIC_ALLOCATION;game[i].Serial=0x5001+i;game[i].UmdAlloc=1;game[i].ScanoutRequested=1;
        game[i].ScanoutWidth=1366;game[i].ScanoutHeight=768;game[i].ScanoutPitch=5632;game[i].UmdBytes=5632ull*768;
        game[i].ScanoutFormat=D3DDDIFMT_A8B8G8R8;
    }
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    for(k=BC250_WRITER_MODESET_COMMIT;k<BC250_WRITER_KINDS;k++)for(timing=0;timing<2;timing++){
        Setup(&d,&w,level);begin_kind=k;
        request.Flags.Value=0;request.hAllocation=&game[0];request.PrimaryAddress.QuadPart=0x30000;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS);
        displayed=0x30000;Vblank(&d);
        CHECK(reported==0x30000 && !w.DesiredOwed);
        hw=hardware;
        CHECK(Begin(&d,&generation));
        CHECK(w.WriterActive && (w.PrimarySequence&1) && !w.WriterDeposit && !w.WriterResumed);
        CHECK(bridge_writes==1 && plane_card==(LONGLONG)BRIDGE_CARD && hardware==hw);
        /* W-u's old success could retire A without satisfying B's timing. Both timing modes keep A owned and
         * unchanged, irrespective of the reserved bridge's DRAINED readback. */
        hubp_state=(k%3)==0?1:(k%3)==1?0:-1;
        request.hAllocation=&game[1];request.PrimaryAddress.QuadPart=0x40000;
        request.Flags.Value=0;request.Flags.FlipImmediate=timing;request.Flags.FlipOnNextVSync=!timing;
        CHECK(Flip(&d,&request)==STATUS_DEVICE_BUSY && !w.Faulting && !w.DesiredOwed);
        CHECK(hardware==hw && plane_card==(LONGLONG)BRIDGE_CARD && w.ScanoutObject==(LONGLONG)game[0].Serial);
        CHECK(w.PrimaryAddress.QuadPart==0x30000 && !w.WriterDeposit && w.WuRecordNext==0);
        /* Resume authorizes reserved storage only. Neither a stale B nor C can become End's target. */
        CHECK(Resume(&d)==STATUS_SUCCESS && w.WriterResumed);
        request.hAllocation=&game[2];request.PrimaryAddress.QuadPart=0x50000;
        CHECK(Flip(&d,&request)==STATUS_DEVICE_BUSY && !w.Faulting && !w.WriterDeposit);
        CHECK(w.PrimaryAddress.QuadPart==0x30000 && w.ScanoutObject==(LONGLONG)game[0].Serial && !w.DesiredOwed);
        CHECK(w.WriterBusy==2 && w.Flips==1 && w.WuRecordNext==0);
        CHECK(Destroy(&d,&game[2])==STATUS_SUCCESS && freed==1);
        CHECK(plane_card==(LONGLONG)BRIDGE_CARD && hardware==hw && w.WriterActive);
        CHECK(End(&d,generation,FALSE)==STATUS_SUCCESS);
        CHECK(hardware==hw+1 && plane_card==0x30000 && w.PrimaryAddress.QuadPart==0x30000);
        CHECK(!w.WriterActive && !w.WriterDeposit && !(w.PrimarySequence&1));
        CHECK(w.ScanoutObject==(LONGLONG)game[0].Serial && !w.PrimaryNeedsRestore && !w.DesiredOwed);
        CHECK(w.FlipFaults==0 && fault_blanks==0 && faults_notified==0);
        /* After End the OS can retry B. This real immediate completion can finally release A. */
        request.hAllocation=&game[1];request.PrimaryAddress.QuadPart=0x40000;
        request.Flags.Value=0;request.Flags.FlipImmediate=1;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS && plane_card==0x40000 && w.ScanoutObject==(LONGLONG)game[1].Serial);
        CHECK(Destroy(&d,&game[0])==STATUS_SUCCESS && freed==2 && plane_card==0x40000);
    }

    /* A next-VSync flip accepted before Begin remains owed. Refusing a new request cannot consume that debt. */
    Setup(&d,&w,level);
    request.Flags.Value=0;request.hAllocation=NULL;request.PrimaryAddress.QuadPart=0x20000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.DesiredOwed);
    CHECK(Begin(&d,&generation));
    request.PrimaryAddress.QuadPart=0x21000;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_BUSY && !w.WriterDeposit && w.DesiredOwed);
    CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS && plane_card==0x20000);
    CHECK(w.PrimaryAddress.QuadPart==0x20000 && !w.PrimaryNeedsRestore);
    CHECK(w.DesiredOwed && !programmed_immediate);
    pending=0;displayed=0x20000;Vblank(&d);
    CHECK(reported==0x20000 && !w.DesiredOwed && w.OwedReported==1);
    CHECK(Begin(&d,&generation));
    request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x22000;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_BUSY && !w.DesiredOwed);
    CHECK(End(&d,generation,FALSE)==STATUS_SUCCESS && plane_card==0x20000);

    /* Resume with no writer exports no app state; high IRQL cannot acquire its section. */
    sections=syncs;
    CHECK(Resume(&d)==STATUS_INVALID_DEVICE_STATE && syncs==sections+1 && !w.WriterResumed);
    irq=TEST_DIRQL;
    CHECK(!NT_SUCCESS(WddmWriterResume(&d)) && syncs==sections+1);
    irq=level?TEST_DIRQL:0;

    /* Invalidate removes the old Desired. A request refused during the writer cannot survive as a new Desired. */
    Setup(&d,&w,level);
    w.ScanoutObject=0x55;w.PrimaryPlaneFormat=BC250_PLANE_FORMAT_ARGB8888;
    CHECK(Begin(&d,&generation));
    CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS);
    CHECK(w.PrimaryAddress.QuadPart==0 && w.PrimaryNeedsRestore && w.PrimaryPitch==0 && w.ScanoutObject==0);
    CHECK(plane_card==(LONGLONG)BRIDGE_CARD && hardware==0);
    w.PrimaryPitch=5632;
    CHECK(Begin(&d,&generation));
    request.Flags.Value=0;request.PrimaryAddress.QuadPart=0x27000;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_BUSY);
    CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS);
    CHECK(plane_card==(LONGLONG)BRIDGE_CARD && w.PrimaryAddress.QuadPart==0 && w.PrimaryNeedsRestore);

    /* A closed flip gate carries only software state. The timer still owns a next-VSync completion. */
    Setup(&d,&w,level);d.VidPnFlipEnabled=0;
    CHECK(Begin(&d,&generation) && bridge_writes==0);
    request.Flags.Value=0;request.PrimaryAddress.QuadPart=0x28000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.DesiredOwed);
    CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS);
    CHECK(hardware==0 && otg_reads==0 && w.PrimaryAddress.QuadPart==0x28000 && w.DesiredOwed);
    CHECK(Begin(&d,&generation) && bridge_writes==0);
    request.Flags.Value=0;request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x28500;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && !w.DesiredOwed);
    CHECK(hubp_reads==0 && w.FlipEdges[BC250_FLIP_EDGE_DEPOSIT_VSYNC]==2 && w.WuRecordNext==0);
    CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS);
    CHECK(hardware==0 && otg_reads==0 && w.PrimaryAddress.QuadPart==0x28500);
    /* D3 flush removes report debt before the power writer invalidates the primary. */
    Setup(&d,&w,level);
    request.Flags.Value=0;request.PrimaryAddress.QuadPart=0x29000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.DesiredOwed);
    irq=0;WddmFlipFlushed(&d);irq=level?TEST_DIRQL:0;
    CHECK(!w.DesiredOwed && w.FlipFlushes==1);
    hw=hardware;CHECK(Begin(&d,&generation));CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS);
    CHECK(hardware==hw && w.PrimaryAddress.QuadPart==0);

    /* End cannot complete an owed application flip through a stopped or unreadable OTG. */
    for(k=0;k<2;k++){
        Setup(&d,&w,level);
        request.PrimaryAddress.QuadPart=0x2A000;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS);CHECK(Begin(&d,&generation));
        otg_state=k?-1:0;hw=hardware;
        CHECK(End(&d,generation,FALSE)==STATUS_DEVICE_HARDWARE_ERROR);
        CHECK(w.Faulting && w.FaultCode==BC250_FLIP_FAULT_OWED_STOPPED && hardware==hw && fault_blanks==1);
    }

    /* A refused bridge has not released the old allocation. Failure after a write is a hardware failure, not a
     * normal writer conflict. A post-write failure must retain ownership for recovery. */
    /* BD-195: a refused bridge (31v2's LINK_DISABLE) is counted by kind and logged with its site and status, outside
     * L and without E5; 31v2 left neither a counter nor a line. */
    Setup(&d,&w,level);bridge_result=STATUS_ACCESS_DENIED;begin_kind=BC250_WRITER_LINK_DISABLE;
    CHECK(!Begin(&d,&generation) && !w.WriterActive && !(w.PrimarySequence&1));
    CHECK(plane_card==0x1000 && hardware==0 && !w.Faulting);
    CHECK(w.WriterRefused==1 && w.WriterRefusedByKind[BC250_WRITER_LINK_DISABLE]==1);
    CHECK(w.WriterRefusedKind==BC250_WRITER_LINK_DISABLE && w.WriterRefusedSite==BC250_WRITER_REFUSED_BRIDGE);
    CHECK(w.WriterRefusedAnswer==BC250_WRITER_BEGIN_REFUSED && w.WriterRefusedStatus==STATUS_ACCESS_DENIED);
    CHECK(logs==2 && w.FlipFaults==0 && faults_notified==0 && dpcs==0);
    Setup(&d,&w,level);bridge_result=STATUS_IO_TIMEOUT;bridge_wrote=TRUE;
    CHECK(!Begin(&d,&generation) && w.WriterActive && (w.PrimarySequence&1) && w.Faulting && fault_blanks==1);
    CHECK(w.WriterRefused==1 && w.WriterRefusedSite==BC250_WRITER_REFUSED_FAULTING);
    CHECK(w.WriterRefusedStatus==STATUS_IO_TIMEOUT);

    /* A different writer owns the generation: Begin is bounded and never makes the interrupted writer progress. */
    Setup(&d,&w,level);w.PrimarySequence=5;w.WriterActive=TRUE;
    CHECK(!Begin(&d,&generation) && syncs==200 && stalls10==200 && w.PrimarySequence==5 && bridge_writes==0);
    CHECK(w.WriterRefused==1 && w.WriterRefusedSite==BC250_WRITER_REFUSED_RETRY && logs==2);
    CHECK(w.WriterRefusedStatus==STATUS_DEVICE_BUSY && w.WriterRefusedAnswer==BC250_WRITER_BEGIN_RETRY);
    w.PrimarySequence=6;w.WriterActive=FALSE;w.Faulting=1;sections=syncs;
    CHECK(!Begin(&d,&generation) && syncs==sections+1 && bridge_writes==0);
    CHECK(w.WriterRefused==2 && w.WriterRefusedSite==BC250_WRITER_REFUSED_FAULTING && logs==4);
    /* Above PASSIVE_LEVEL the refusal is counted, never logged (GuardLog takes a spin lock). */
    w.Faulting=0;irq=TEST_DIRQL;
    CHECK(!WddmPrimaryExclusiveBegin(&d,BC250_WRITER_HDMI_START,&generation) && syncs==sections+1);
    CHECK(w.WriterRefused==3 && w.WriterRefusedByKind[BC250_WRITER_HDMI_START]==1 && logs==4);
    CHECK(w.WriterRefusedSite==BC250_WRITER_REFUSED_IRQL && w.WriterHigh==1);
    irq=level?TEST_DIRQL:0;
    d.Wddm=NULL;generation=9;
    CHECK(Begin(&d,&generation) && generation==0);
    CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS);

    /* The record-copy section is independently refused; no copied diagnostic data can grant ownership. */
    Setup(&d,&w,level);sections=syncs;
    CHECK(WuCopy(&d,&copy) && syncs==sections+1 && copy.Next==0);
    sync_refusals=1;
    CHECK(!WuCopy(&d,&copy) && copy.Next==0);
}
/* A flip on another processor that wins L just before a section: interleave_request at PASSIVE_LEVEL. */
static void FirstFlip(BC250_DEVICE *d)
{
    const int level=irq;
    irq=0;CHECK(Bc250WddmSetVidPnSourceAddress(d,&interleave_request)==STATUS_SUCCESS);irq=level;
}
static void RefusedFlip(BC250_DEVICE *d)
{
    const int level=irq;
    irq=0;CHECK(Bc250WddmSetVidPnSourceAddress(d,&interleave_request)==STATUS_DEVICE_BUSY);irq=level;
}
/* DestroyAllocation retires the application surface before it frees it. An active writer already owns a reserved
 * bridge, so destroy can remove its matching Desired under L without nesting another writer or touching hardware. */
static void DestroyWriter(void)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT game[4];
    unsigned i,sections;
    ULONG generation;
    memset(game,0,sizeof(game));
    for(i=0;i<4;i++){
        game[i].Magic=BC250_WDDM_MAGIC_ALLOCATION;game[i].Serial=0x8001+i;game[i].UmdAlloc=1;game[i].ScanoutRequested=1;
        game[i].ScanoutWidth=1366;game[i].ScanoutHeight=768;game[i].ScanoutPitch=5632;game[i].UmdBytes=5632ull*768;
        game[i].ScanoutFormat=D3DDDIFMT_A8B8G8R8;
    }
    Setup(&d,&w,0);
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    request.hAllocation=&game[0];request.PrimaryAddress.QuadPart=0x30000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.ScanoutObject==(LONGLONG)game[0].Serial && w.DesiredOwed);
    /* Begin proves the reserved bridge before Take clears the app Desired; End then closes ownership. */
    sections=syncs;
    CHECK(Destroy(&d,&game[0])==STATUS_SUCCESS);
    CHECK(bridge_writes==1 && plane_card==(LONGLONG)BRIDGE_CARD && syncs>sections && w.DestroyUnsynchronized==0);
    CHECK(w.ScanoutObject==0 && w.PrimaryAddress.QuadPart==0 && w.PrimaryNeedsRestore && !w.DesiredOwed);
    CHECK(!w.WriterActive && !(w.PrimarySequence&1) && w.ScanoutTeardowns==1 && freed==1);
    /* A flip on another processor after Begin is refused. It cannot replace the destroyed Desired or make End
     * program a surface for which the caller never received success. */
    w.PrimaryPitch=5632;
    request.hAllocation=&game[1];request.PrimaryAddress.QuadPart=0x31000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.ScanoutObject==(LONGLONG)game[1].Serial);
    memset(&interleave_request,0,sizeof(interleave_request));
    interleave_request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;interleave_request.PrimaryAddress.QuadPart=0x32000;
    after_section_routine=WddmWriterBeginLocked;after_section=RefusedFlip;
    CHECK(Destroy(&d,&game[1])==STATUS_SUCCESS);
    CHECK(after_section==NULL && w.FlipEdges[BC250_FLIP_EDGE_DEPOSIT_VSYNC]==1 && w.WriterBusy==1);
    CHECK(plane_card==(LONGLONG)BRIDGE_CARD && w.EndPrograms==0 && w.PrimaryAddress.QuadPart==0 && w.PrimaryNeedsRestore);
    CHECK(w.ScanoutObject==0 && !w.WriterActive && !(w.PrimarySequence&1));
    /* The record moved on before the section: a flip on another processor won L first. Nothing taken, nothing
     * restored, and End does not invalidate the primary that flip published. */
    request.hAllocation=&game[2];request.PrimaryAddress.QuadPart=0x33000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.ScanoutObject==(LONGLONG)game[2].Serial);
    {
        static DXGKARG_SETVIDPNSOURCEADDRESS first;
        memset(&first,0,sizeof(first));
        first.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;first.PrimaryAddress.QuadPart=0x34000;
        interleave_request=first;
        before_section=FirstFlip;
    }
    CHECK(Destroy(&d,&game[2])==STATUS_SUCCESS);
    CHECK(w.ScanoutObject==0 && w.PrimaryAddress.QuadPart==0x34000 && !w.PrimaryNeedsRestore);
    CHECK(w.DestroyUnsynchronized==0 && !w.WriterActive && !(w.PrimarySequence&1));
    /* Another writer owns a proved bridge: taking its Desired is safe, needs no nested Begin, and leaves its
     * generation held. This is not the old unsynchronized restore after a failed Begin. */
    request.hAllocation=&game[3];request.PrimaryAddress.QuadPart=0x35000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.ScanoutObject==(LONGLONG)game[3].Serial);
    CHECK(Begin(&d,&generation));sections=syncs;
    CHECK(Destroy(&d,&game[3])==STATUS_SUCCESS);
    CHECK(w.DestroyUnsynchronized==0 && w.ScanoutObject==0 && w.PrimaryAddress.QuadPart==0);
    CHECK(syncs>sections && stalls10==0 && w.WriterActive && (w.PrimarySequence&1));
    CHECK(plane_card==(LONGLONG)BRIDGE_CARD);
    CHECK(End(&d,generation,FALSE)==STATUS_SUCCESS);
}
/* R2 regressions: the callback's failure did not run its routine. A failed acquisition must not run that routine
 * directly, publish an End, restore a surface, or free the allocation that still owns the active scan-out. Every
 * hardware stub keeps its L assertion enabled. The named-routine refusal targets a later stage without relying on
 * how many safe sections Begin uses. */
static BC250_WDDM_OBJECT *retire_allocation;
static void DuringBridgeRetirement(BC250_DEVICE *d)
{
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    unsigned immediate;
    CHECK(d->Wddm->WriterActive && !d->Wddm->WriterReady && (d->Wddm->PrimarySequence&1));
    CHECK(displayed==0x62000 && freed==0);
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x63000;
    for(immediate=0;immediate<2;immediate++) {
        request.Flags.FlipImmediate=immediate;
        CHECK(Flip(d,&request)==STATUS_DEVICE_BUSY);
        CHECK(d->Wddm->PrimaryAddress.QuadPart==0x62000 && displayed==0x62000);
    }
    CHECK(Destroy(d,retire_allocation)==STATUS_DEVICE_BUSY);
    CHECK(freed==0 && d->Wddm->ScanoutObject==(LONGLONG)retire_allocation->Serial && !d->Wddm->Faulting);
}

static void WriterRetirement(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    BC250_WDDM_OBJECT game={0};
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    ULONG generation,kind;
    game.Magic=BC250_WDDM_MAGIC_ALLOCATION;game.Serial=0xB001;game.UmdAlloc=1;game.ScanoutRequested=1;
    game.ScanoutWidth=1366;game.ScanoutHeight=768;game.ScanoutPitch=5632;game.UmdBytes=5632ull*768;
    game.ScanoutFormat=D3DDDIFMT_A8B8G8R8;
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x62000;request.hAllocation=&game;
    for(kind=BC250_WRITER_MODESET_COMMIT;kind<BC250_WRITER_KINDS;kind++) {
        Setup(&d,&w,level);begin_kind=kind;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS);displayed=0x62000;
        retire_allocation=&game;retire_hook=DuringBridgeRetirement;
        CHECK(Begin(&d,&generation) && !retire_hook && bridge_retires==1 && w.WriterReady);
        CHECK(w.WriterBusyByKind[kind]==2 && w.WriterBridgePrograms==1 && w.WriterBridgeRetired==1);
        CHECK(w.WriterBridgeFailures==0 && freed==0 && displayed==(LONGLONG)BRIDGE_CARD);
        CHECK(Destroy(&d,&game)==STATUS_SUCCESS && freed==1);
        CHECK(End(&d,generation,TRUE)==STATUS_SUCCESS && !w.WriterReady && !w.WriterActive);
    }
    Setup(&d,&w,level);CHECK(Flip(&d,&request)==STATUS_SUCCESS);displayed=0x62000;
    bridge_retire_result=STATUS_IO_TIMEOUT;
    CHECK(!Begin(&d,&generation));
    CHECK(w.WriterActive && !w.WriterReady && (w.PrimarySequence&1) && freed==0);
    CHECK(w.Faulting && w.FaultCode==BC250_FLIP_FAULT_BRIDGE && w.WriterBridgeFailures==1);
    CHECK(Destroy(&d,&game)==STATUS_DEVICE_BUSY && freed==0);

    Setup(&d,&w,level);CHECK(Flip(&d,&request)==STATUS_SUCCESS);displayed=0x62000;
    sync_refuse_routine=WddmWriterRetiredLocked;
    CHECK(!Begin(&d,&generation) && bridge_retires==1 && w.WriterRecovery && !w.WriterReady);
    CHECK(Destroy(&d,&game)==STATUS_DEVICE_BUSY && freed==0);
    FaultDpc(&d);CHECK(w.Faulting && w.FaultCode==BC250_FLIP_FAULT_SECTION);

    Setup(&d,&w,level);CHECK(Flip(&d,&request)==STATUS_SUCCESS);displayed=0x62000;
    bridge_retire_result=STATUS_IO_TIMEOUT;
    CHECK(!NT_SUCCESS(Destroy(&d,&game)) && freed==0 && bridge_retires==1 && w.Faulting);
}

static void WriterSynchronizationRefusals(void)
{
    BC250_WDDM w;BC250_DEVICE d;
    BC250_WDDM_OBJECT game={0};
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    ULONG generation;
    unsigned before;

    Setup(&d,&w,0);
    sync_refusals=1;
    CHECK(!Begin(&d,&generation));
    CHECK(w.WriterSyncFailed==1 && !w.WriterActive && w.PrimarySequence==0);
    CHECK(hardware==0 && fault_blanks==0 && w.PrimaryAddress.QuadPart==0x1000);
    CHECK(!w.WriterRecovery && !w.Faulting && dpcs==0);
    FaultDpc(&d);
    CHECK(!w.Faulting && faults_notified==0 && fault_blanks==0);
    CHECK(Begin(&d,&generation) && w.WriterReady);

    Setup(&d,&w,0);
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x60000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);CHECK(Begin(&d,&generation));
    before=hardware;sync_refusals=1;
    CHECK(!NT_SUCCESS(Resume(&d)));
    CHECK(w.WriterActive && !w.WriterResumed && (w.PrimarySequence&1) && !w.WriterDeposit && w.DesiredOwed);
    CHECK(w.WriterSyncFailed==1 && hardware==before && w.PrimaryAddress.QuadPart==0x60000);

    Setup(&d,&w,0);
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);CHECK(Begin(&d,&generation));
    before=hardware;sync_refusals=1;
    CHECK(!NT_SUCCESS(End(&d,generation,FALSE)));
    CHECK(w.WriterSyncFailed==1 && hardware==before && w.EndPrograms==0);
    CHECK(w.WriterActive && (w.PrimarySequence&1) && !w.WriterDeposit && w.DesiredOwed);
    CHECK(w.PrimaryAddress.QuadPart==0x60000);

    game.Magic=BC250_WDDM_MAGIC_ALLOCATION;game.Serial=0xA001;game.UmdAlloc=1;game.ScanoutRequested=1;
    game.ScanoutWidth=1366;game.ScanoutHeight=768;game.ScanoutPitch=5632;game.UmdBytes=5632ull*768;
    game.ScanoutFormat=D3DDDIFMT_A8B8G8R8;
    Setup(&d,&w,0);
    request.hAllocation=&game;request.PrimaryAddress.QuadPart=0x61000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    displayed=0x61000;Vblank(&d);
    before=hardware;sync_refusals=1;
    CHECK(!NT_SUCCESS(Destroy(&d,&game)));
    CHECK(freed==0 && hardware==before && w.ScanoutObject==(LONGLONG)game.Serial);
    CHECK(w.PrimaryAddress.QuadPart==0x61000 && !w.WriterActive && !(w.PrimarySequence&1));

    Setup(&d,&w,0);
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    displayed=0x61000;Vblank(&d);
    sync_refuse_routine=WddmDestroyTakeLocked;
    CHECK(!NT_SUCCESS(Destroy(&d,&game)));
    CHECK(sync_refuse_routine==NULL && freed==0);
    CHECK(w.ScanoutObject==(LONGLONG)game.Serial && w.PrimaryAddress.QuadPart==0x61000);

    /* The first member does not need retirement. A later failure must keep the complete list retryable, with no
     * journal or free of the earlier member and no partial resource destruction. */
    {
        BC250_WDDM_OBJECT unrelated={0},resource={0};
        unrelated.Magic=BC250_WDDM_MAGIC_ALLOCATION;resource.Magic=BC250_WDDM_MAGIC_RESOURCE;
        HANDLE list[2]={&unrelated,&game};
        DXGKARG_DESTROYALLOCATION destroy={0};
        Setup(&d,&w,0);CHECK(Flip(&d,&request)==STATUS_SUCCESS);
        unrelated.Serial=0xA002;destroy.NumAllocations=2;destroy.pAllocationList=list;
        destroy.Flags.DestroyResource=1;destroy.hResource=&resource;sync_refusals=1;
        CHECK(!NT_SUCCESS(Bc250WddmDestroyAllocation(&d,&destroy)));
        CHECK(freed==0 && journaled==0 && w.ScanoutObject==(LONGLONG)game.Serial);
    }

    /* Destroy's initial Take, Begin and second Take fail independently. A failed later Take keeps the bridge
     * active, but still cannot free an object whose protected teardown did not complete. */
    Setup(&d,&w,0);CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    sync_refuse_routine=WddmWriterBeginLocked;
    CHECK(!NT_SUCCESS(Destroy(&d,&game)) && freed==0 && bridge_writes==0 && w.ScanoutObject==(LONGLONG)game.Serial);
    Setup(&d,&w,0);CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    sync_refuse_at=syncs+3;
    CHECK(!NT_SUCCESS(Destroy(&d,&game)) && freed==0 && bridge_writes==1);
    CHECK(w.WriterActive && (w.PrimarySequence&1) && w.ScanoutObject==(LONGLONG)game.Serial);
    CHECK(plane_card==(LONGLONG)BRIDGE_CARD && w.WriterRecovery);

    /* The status alone is insufficient when the OS callback reports success without invoking the routine. */
    Setup(&d,&w,0);
    request.hAllocation=NULL;CHECK(Flip(&d,&request)==STATUS_SUCCESS);CHECK(Begin(&d,&generation));
    before=hardware;sync_no_callback=1;
    CHECK(End(&d,generation,FALSE)==STATUS_UNSUCCESSFUL);
    CHECK(w.WriterActive && (w.PrimarySequence&1) && !w.WriterDeposit && w.DesiredOwed && hardware==before && w.WriterRecovery);
}

static void WriterRecovery(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    ULONG generation;
    unsigned before;
    Setup(&d,&w,level);
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x62000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);CHECK(Begin(&d,&generation));
    before=hardware;sync_refusals=1;
    CHECK(End(&d,generation,FALSE)==STATUS_UNSUCCESSFUL);
    CHECK(w.WriterRecovery && !w.Faulting && w.WriterActive && !w.WriterDeposit && w.DesiredOwed);
    CHECK(hardware==before && fault_blanks==0 && dpcs==1);
    /* A refused recovery section keeps the request pending and still touches no protected state or MMIO. */
    sync_refusals=1;FaultDpc(&d);
    CHECK(w.WriterRecovery && !w.Faulting && hardware==before && fault_blanks==0 && faults_notified==0);
    FaultDpc(&d);
    CHECK(!w.WriterRecovery && w.Faulting && w.FaultCode==BC250_FLIP_FAULT_SECTION);
    CHECK(hardware==before && fault_blanks==1 && w.WriterActive && !w.WriterDeposit && w.DesiredOwed && (w.PrimarySequence&1));
    CheckFault(BC250_FLIP_FAULT_SECTION,0);
    FaultDpc(&d);CHECK(faults_notified==1);
    before=hardware;CHECK(End(&d,generation,FALSE)==STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(hardware==before && !w.WriterActive && !(w.PrimarySequence&1));
    /* The next flip can instead consume the queued request, in either legal DDI entry. It must refuse this flip
     * before any hardware programming or Desired update. */
    Setup(&d,&w,level);CHECK(Begin(&d,&generation));sync_refusals=1;
    CHECK(!NT_SUCCESS(Resume(&d)) && w.WriterRecovery);
    before=hardware;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR);
    CHECK(!w.WriterRecovery && w.Faulting && w.FaultCode==BC250_FLIP_FAULT_SECTION && hardware==before);
    CHECK(w.PrimaryAddress.QuadPart==0x1000 && !w.WriterDeposit);
}

/* The two entries of the flip DDI. Above DISPATCH_LEVEL dxgmms2 holds L: the transaction runs directly, no
 * section, no arm, no log line. At or below it: one section, then the logs and the arm. A section that fails ran
 * nothing: the request fails with the section's status, counted, and nothing is published or armed. */
static void Entries(void)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    unsigned sections;
    Setup(&d,&w,0);
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x40000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && syncs==1 && arms==1 && w.FlipsAboveDispatch==0);
    irq=DISPATCH_LEVEL;request.PrimaryAddress.QuadPart=0x41000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && syncs==2 && arms==2 && w.FlipsAboveDispatch==0);
    irq=TEST_DIRQL;request.PrimaryAddress.QuadPart=0x42000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && syncs==2 && arms==2 && w.FlipsAboveDispatch==1);
    CHECK(plane_card==0x42000 && w.PrimaryAddress.QuadPart==0x42000);
    /* The transaction's time inside L is kept on both entries: here at least the two reads around a programming. */
    CHECK(w.FlipLockedMaxUs>=2);
    irq=0;sections=syncs;sync_refusals=1;request.PrimaryAddress.QuadPart=0x43000;
    CHECK(Flip(&d,&request)==STATUS_UNSUCCESSFUL && w.LowSyncFailed==1 && syncs==sections+1);
    CHECK(w.PrimaryAddress.QuadPart==0x42000 && arms==2 && hardware==3);
    d.Dxgk.DxgkCbSynchronizeExecution=NULL;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_NOT_READY && w.LowSyncFailed==2 && arms==2);
    d.Dxgk.DxgkCbSynchronizeExecution=TestSync;
    request.VidPnSourceId=1;
    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER && syncs==sections+1);
    request.VidPnSourceId=0;d.Wddm=NULL;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_NOT_READY);
    d.Wddm=&w;
    /* The scan-out lines are logged after the section, never inside it (GuardLog's check). */
    {
        BC250_WDDM_OBJECT game={0};
        unsigned lines=logs;
        game.Magic=BC250_WDDM_MAGIC_ALLOCATION;game.Serial=0x4001;game.UmdAlloc=1;game.ScanoutRequested=1;
        game.ScanoutWidth=1366;game.ScanoutHeight=768;game.ScanoutPitch=5632;game.UmdBytes=5632ull*768;
        game.ScanoutFormat=D3DDDIFMT_A8R8G8B8;
        request.hAllocation=&game;request.PrimaryAddress.QuadPart=0x44000;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS && logs==lines+1);
        game.ScanoutFormat=D3DDDIFMT_A16B16G16R16F;game.ScanoutPitch=1366*8;game.UmdBytes=1366ull*8*768;
        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER && logs==lines+2);
        irq=TEST_DIRQL;
        CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER && logs==lines+2);
        irq=0;
    }
}
/* BD-170 r6: the flip gate closed (EnableVidPnFlip): the transaction publishes only. A next-VSync request is owed to
 * the software timer's report, an immediate one is not, and nothing is read or written. */
static void GateClosed(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    Setup(&d,&w,level);
    d.VidPnFlipEnabled=0;
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.PrimaryAddress.QuadPart=0x50000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==0 && w.PrimaryAddress.QuadPart==0x50000 && w.DesiredOwed && w.Flips==1 && w.PrimarySequence==2);
    request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x51000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(hardware==0 && observed==0 && hubp_reads==0 && !w.DesiredOwed && !w.Faulting && w.Flips==2);
    CHECK(w.FlipEdges[BC250_FLIP_EDGE_PROGRAM]==2 && w.FlipResults[BC250_FLIP_RESULT_SUCCESS]==0);
}
/* IndependentFlipExclusive changes the counter, never a refused request into success. Admission and prewrite
 * refusal keep the old primary and their status; a write without P' faults like any other immediate request.
 * Returning success without retiring the old primary would authorize the OS to release a fetched surface. */
static void Exclusive(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT game={0};
    Setup(&d,&w,level);
    game.Magic=BC250_WDDM_MAGIC_ALLOCATION;game.Serial=0x7101;game.UmdAlloc=1;game.ScanoutRequested=1;
    game.ScanoutWidth=1366;game.ScanoutHeight=768;game.ScanoutFormat=D3DDDIFMT_A16B16G16R16F;
    game.ScanoutPitch=1366*8;game.UmdBytes=1366ull*8*768;
    w.ScanoutObject=0x7100;
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;request.hAllocation=&game;request.PrimaryAddress.QuadPart=0x70000;
    request.Flags.Value=0x82u;
    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER);
    CHECK(w.ExclusiveRefused==1 && hardware==0 && w.PrimaryAddress.QuadPart==0x1000 && w.ScanoutObject==0x7100);
    CHECK(w.FlipEdges[BC250_FLIP_EDGE_REFUSED]==1 && !(w.PrimarySequence&1));
    request.Flags.Value=0x80u;
    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER && w.ExclusiveFailed==1);
    request.Flags.Value=0x2u;
    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER && w.ExclusiveFailed==1 && w.ExclusiveRefused==1);
    /* dcn.c's refusal before its write sequence: the same answers, and the record goes back. */
    request.hAllocation=NULL;hardware_result=STATUS_ACCESS_DENIED;request.Flags.Value=0x82u;
    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED);
    CHECK(w.ExclusiveRefused==2 && hardware==1 && w.PrimaryAddress.QuadPart==0x1000 && w.ScanoutObject==0x7100);
    CHECK(w.FlipResults[BC250_FLIP_RESULT_REFUSED]==1 && observed==0);
    request.Flags.Value=0x80u;
    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED && w.ExclusiveFailed==2);
    request.Flags.Value=0x2u;
    CHECK(Flip(&d,&request)==STATUS_ACCESS_DENIED && w.ExclusiveFailed==2 && w.ExclusiveRefused==2);
    hardware_result=STATUS_SUCCESS;
    /* P': success, the flags in the record as they came. */
    request.Flags.Value=0x182u;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && observed_flags==0x182u && w.PrimaryAddress.QuadPart==0x70000);
    /* Without P': U, E5. */
    observe_outcome=BC250_DCN_IMMEDIATE_PENDING;request.Flags.Value=0x82u;request.PrimaryAddress.QuadPart=0x71000;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR && w.Faulting && w.FaultCode==BC250_FLIP_FAULT_UNPROVEN);
    CHECK(w.ExclusiveRefused==2);
}
/* BD-170: the VSync switch's history. Every CRTC_VSYNC call of ControlInterrupt is counted on or off and kept with
 * its number, its time and the address owed then (a next-VSync success no report carried); the last
 * BC250_WDDM_VSYNC_CONTROL_EVENTS stay. Other types and a call with no WDDM state record nothing. */
static void VSyncControlHistory(void)
{
    BC250_WDDM w;BC250_DEVICE d;
    unsigned i;
    LONGLONG before;
    Setup(&d,&w,0);
    before=qpc;
    CHECK(Bc250WddmControlInterrupt(&d,DXGK_INTERRUPT_CRTC_VSYNC,TRUE)==STATUS_SUCCESS && arms==1);
    CHECK(w.VSyncControlCalls==1 && w.VSyncControlOn==1 && w.VSyncControlOff==0);
    CHECK(w.VSyncControl[0].Call==1 && w.VSyncControl[0].On && w.VSyncControl[0].Qpc>before && w.VSyncControl[0].Owed==0);
    w.DesiredOwed=TRUE;w.PrimaryAddress.QuadPart=0x9000;
    CHECK(Bc250WddmControlInterrupt(&d,DXGK_INTERRUPT_CRTC_VSYNC,FALSE)==STATUS_SUCCESS && !w.VSyncEnabled);
    CHECK(w.VSyncControl[1].Call==2 && !w.VSyncControl[1].On && w.VSyncControl[1].Owed==0x9000);
    CHECK(w.VSyncControl[1].Qpc>w.VSyncControl[0].Qpc && arms==1);
    w.DesiredOwed=FALSE;
    CHECK(Bc250WddmControlInterrupt(&d,DXGK_INTERRUPT_DMA_COMPLETED,FALSE)==STATUS_SUCCESS && w.VSyncControlCalls==2);
    CHECK(Bc250WddmControlInterrupt(&d,7,TRUE)==STATUS_NOT_SUPPORTED && w.VSyncControlCalls==2 && !w.VSyncEnabled);
    for(i=0;i<20;i++) CHECK(Bc250WddmControlInterrupt(&d,DXGK_INTERRUPT_CRTC_VSYNC,(i&1)?TRUE:FALSE)==STATUS_SUCCESS);
    CHECK(w.VSyncControlCalls==22 && w.VSyncControlOn==11 && w.VSyncControlOff==11);
    /* The ring: call n is in slot (n - 1) % 16, so calls 7 to 22 are kept. */
    for(i=0;i<BC250_WDDM_VSYNC_CONTROL_EVENTS;i++){
        const BC250_WDDM_VSYNC_CONTROL *event=&w.VSyncControl[i];
        CHECK((ULONG)(event->Call-1)%BC250_WDDM_VSYNC_CONTROL_EVENTS==i && event->Call>=7 && event->Call<=22);
        CHECK(event->On==((event->Call>2) ? (BOOLEAN)(((event->Call-3)&1)!=0) : (BOOLEAN)(event->Call==1)));
    }
    d.Wddm=NULL;
    CHECK(Bc250WddmControlInterrupt(&d,DXGK_INTERRUPT_CRTC_VSYNC,TRUE)==STATUS_SUCCESS && w.VSyncControlCalls==22);
}
/* BD-170: a flip that comes to Present with a DMA buffer is refused with a status of the DDI's own table, counted,
 * and nothing in the buffer is written or skipped over; an MMIO flip (no DMA buffer) and a Blt are not touched. */
static void DmaFlips(void)
{
    BC250_WDDM w={0};
    DXGKARG_PRESENT present={0};
    unsigned char dma[64];
    unsigned i;
    memset(dma,0xA5,sizeof(dma));
    in_lock=0;irq=0;
    present.Flags.Flip=1;
    CHECK(WddmPresentDmaFlip(&w,&present)==STATUS_SUCCESS && w.DmaFlipsRefused==0);
    present.Flags.Value=0;present.Flags.Blt=1;present.pDmaBuffer=dma;present.DmaSize=sizeof(dma);
    CHECK(WddmPresentDmaFlip(&w,&present)==STATUS_SUCCESS && w.DmaFlipsRefused==0);
    present.Flags.Value=0;present.Flags.Flip=1;present.Flags.FlipWithNoWait=1;
    CHECK(present.Flags.Value==0x0000000Cu);
    CHECK(WddmPresentDmaFlip(&w,&present)==STATUS_ILLEGAL_INSTRUCTION);
    CHECK(w.DmaFlipsRefused==1 && present.pDmaBuffer==dma && present.DmaSize==sizeof(dma));
    for(i=0;i<sizeof(dma);i++) CHECK(dma[i]==0xA5);
    CHECK(WddmPresentDmaFlip(NULL,&present)==STATUS_ILLEGAL_INSTRUCTION && w.DmaFlipsRefused==1);
}
/* BD-170: the summary prints a record only as a whole copy under its sequence lock: an empty slot is skipped, a
 * stable one copied once, one that changed during a copy copied again, and one that stays odd or keeps changing
 * for three tries is reported unavailable, with nothing of it kept. */
static void RecordCopies(void)
{
    BC250_DEVICE d;
    BC250_DCN_IMMEDIATE_RECORD copy;
    BC250_DCN_IMMEDIATE_RECORD *slot=&d.DcnImmediateRecords[3];
    memset(&d,0,sizeof(d));in_lock=0;irq=0;
    record_copies=stalls1=writer_copies=0;writer_step=0;
    CHECK(WddmImmediateRecordCopy(&d,3,&copy)==0 && record_copies==0);
    slot->Version=2;slot->Sequence=4;slot->CardAddress=0xA000;slot->Outcome=1;slot->EndUs=7;
    CHECK(WddmImmediateRecordCopy(&d,3,&copy)==1 && record_copies==1 && stalls1==0);
    CHECK(copy.Version==2 && copy.Sequence==4 && copy.CardAddress==0xA000 && copy.Outcome==1 && copy.EndUs==7);
    /* The slot index wraps like the ring. */
    CHECK(WddmImmediateRecordCopy(&d,3+BC250_DCN_IMMEDIATE_RECORDS,&copy)==1 && copy.Sequence==4);
    /* A writer finished another record into the slot during the first copy: the second copy is the new one. */
    record_copies=0;writer_copies=1;writer_step=2;
    CHECK(WddmImmediateRecordCopy(&d,3,&copy)==1 && record_copies==2 && stalls1==1);
    CHECK(copy.Version==4 && copy.Sequence==12);
    /* A writer in the slot for every try (odd Version): no copy at all, unavailable. */
    record_copies=stalls1=0;slot->Version=5;
    CHECK(WddmImmediateRecordCopy(&d,3,&copy)==2 && record_copies==0 && stalls1==BC250_DCN_IMMEDIATE_READS-1);
    CHECK(copy.Version==0 && copy.Sequence==0 && copy.CardAddress==0);
    /* Changed during every copy: unavailable after three, nothing of them kept. */
    record_copies=stalls1=0;slot->Version=6;writer_copies=BC250_DCN_IMMEDIATE_READS;writer_step=2;
    CHECK(WddmImmediateRecordCopy(&d,3,&copy)==2 && record_copies==BC250_DCN_IMMEDIATE_READS);
    CHECK(copy.Version==0 && copy.Sequence==0 && copy.CardAddress==0);
    writer_copies=0;
}
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
/* BD-170 r6, the lab flavour's witnesses: each immediate request that edge E programs is armed before its programming,
 * decided by the shipping reader, and only then read for E and R. The answer is the shipping decision's whatever the
 * witnesses show; disagreements are counted per kind and the first sixteen kept, the rest counted as lost. */
static void Witnesses(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_WITNESS_COPY copy;
    unsigned k,sections;
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    /* Agreement: P', the event and EARLIEST_INUSE in both orders. Nothing kept but the last record. */
    Setup(&d,&w,level);
    request.Flags.Value=0;request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x40000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && witness_arms==1 && witness_reads==1 && hardware==1);
    CHECK(w.WitnessRequests==1 && w.WitnessKept==0 && w.WitnessLost==0 && w.WitnessReadFailed==0 && w.WitnessStale==0);
    CHECK(w.WitnessQe==0 && w.WitnessQr==0 && w.WitnessEr==0 && w.WitnessOrder==0);
    CHECK(w.WitnessLast.Request==1 && w.WitnessLast.Q && w.WitnessLast.Fresh && w.WitnessLast.E);
    CHECK(w.WitnessLast.R1 && w.WitnessLast.R2 && w.WitnessLast.Result==BC250_FLIP_RESULT_SUCCESS);
    CHECK(w.WitnessLast.Outcome==BC250_DCN_IMMEDIATE_DONE && w.WitnessLast.Card==0x40000);
    CHECK(w.WitnessLast.Physical==PHYS(0x40000) && w.WitnessLast.Generation==1 && w.WitnessLast.Flags==request.Flags.Value);
    CHECK(w.WitnessLast.PendingBefore==0 && w.WitnessLast.PendingAfter==0);
    /* A next-VSync request is neither armed nor read. */
    request.Flags.Value=0;request.PrimaryAddress.QuadPart=0x41000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && witness_arms==1 && witness_reads==1 && hardware==2);
    /* P' held but the event did not come and EARLIEST_INUSE still names the old surface: the answer stays success (the
     * witnesses never change it); Q against E and Q against R are counted and the request kept whole. */
    Setup(&d,&w,level);
    witness_event=0;witness_r1=witness_r2=0;
    request.Flags.Value=0;request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x42000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && !w.Faulting);
    CHECK(w.WitnessQe==1 && w.WitnessQr==1 && w.WitnessEr==0 && w.WitnessOrder==0 && w.WitnessKept==1);
    CHECK(w.WitnessRecords[0].Request==1 && w.WitnessRecords[0].Q && !w.WitnessRecords[0].E);
    CHECK(w.WitnessRecords[0].Earliest1==PHYS(0x1000) && w.WitnessRecords[0].Low1==(ULONG)(PHYS(0x1000)&0xFFFFFFFFull));
    CHECK(w.WitnessRecords[0].High1==(ULONG)(PHYS(0x1000)>>32) && w.WitnessRecords[0].InterruptBefore==0x101);
    /* U (P' did not hold): the shipping answer is E5 as before, and the witnesses still read after it. */
    Setup(&d,&w,level);
    observe_outcome=BC250_DCN_IMMEDIATE_PENDING;witness_event=0;witness_r1=witness_r2=0;
    request.PrimaryAddress.QuadPart=0x43000;
    CHECK(Flip(&d,&request)==STATUS_DEVICE_HARDWARE_ERROR && w.Faulting && witness_reads==1);
    CHECK(w.WitnessLast.Result==BC250_FLIP_RESULT_UNPROVEN && !w.WitnessLast.Q && w.WitnessKept==0);
    /* The clear failed: E is not this flip's and is not compared; counted as stale and kept. EARLIEST_INUSE in one
     * order only: an order disagreement. A failed read: counted and kept. */
    Setup(&d,&w,level);
    witness_fresh=0;witness_event=0;
    request.PrimaryAddress.QuadPart=0x44000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS);
    CHECK(w.WitnessStale==1 && w.WitnessQe==0 && w.WitnessEr==0 && w.WitnessQr==0 && w.WitnessKept==1);
    witness_fresh=1;witness_event=1;witness_r2=0;
    request.PrimaryAddress.QuadPart=0x45000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.WitnessOrder==1 && w.WitnessKept==2);
    witness_r2=1;witness_status=STATUS_IO_TIMEOUT;
    request.PrimaryAddress.QuadPart=0x46000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && w.WitnessReadFailed==1 && w.WitnessKept==3);
    CHECK(w.WitnessRecords[2].ReadStatus==STATUS_IO_TIMEOUT && w.WitnessLast.Request==3);
    /* The bound: sixteen records kept, the rest counted as lost. */
    witness_status=STATUS_SUCCESS;witness_event=0;
    for(k=0;k<14;k++){request.PrimaryAddress.QuadPart=0x47000+0x1000*k;CHECK(Flip(&d,&request)==STATUS_SUCCESS);}
    CHECK(w.WitnessKept==16 && w.WitnessLost==1 && w.WitnessRequests==17);
    /* The summary's copy: one section with everything; with the section refused, nothing. */
    {const int lv=irq;irq=0;sections=syncs;
     CHECK(WddmWitnessCopy(&d,&w,&copy) && syncs==sections+1 && copy.Kept==16 && copy.Lost==1 && copy.Qe==14);
     CHECK(copy.Records[15].Request==16 && copy.Last.Request==17);
     sync_refusals=1;CHECK(!WddmWitnessCopy(&d,&w,&copy) && copy.Requests==0);irq=lv;}
    /* A prior flip pending at either side of the arm excludes Q/E correlation, while retaining the raw controls. */
    for(k=0;k<2;k++){
        Setup(&d,&w,level);witness_event=0;
        witness_pending_before=k?0:0x10;witness_pending_after=k?0x10:0;
        request.Flags.Value=0;request.Flags.FlipImmediate=1;request.PrimaryAddress.QuadPart=0x60000;
        CHECK(Flip(&d,&request)==STATUS_SUCCESS && !w.Faulting);
        CHECK(w.WitnessStale==1 && w.WitnessQe==0 && w.WitnessEr==0 && w.WitnessKept==1);
        CHECK(!w.WitnessLast.Fresh && w.WitnessLast.PendingBefore==witness_pending_before);
        CHECK(w.WitnessLast.PendingAfter==witness_pending_after);
    }
}
#endif
/* BD-223: plane 0 of the multiplane-overlay flip DDI, smaller than the source mode (a 1366x704 game chain on the
 * 1366x768 mode), through the same entry and transaction as SetVidPnSourceAddress. The surface checks use the
 * shape's size, a shape is part of what the plane shows, a NULL allocation keeps the shape, a full-size flip ends it,
 * and a shape is refused with no allocation or with the flip gate closed. */
static NTSTATUS ShapedFlip(BC250_DEVICE *d,const DXGKARG_SETVIDPNSOURCEADDRESS *request,const BC250_SCALER_PLAN *shape)
{
    NTSTATUS status;
    if(irq<=DISPATCH_LEVEL) return WddmFlipEntry(d,request,shape);
    CHECK(!in_lock);
    in_lock=1;status=WddmFlipEntry(d,request,shape);in_lock=0;
    return status;
}
static void PlaneShapeFlips(int level)
{
    BC250_WDDM w;BC250_DEVICE d;
    DXGKARG_SETVIDPNSOURCEADDRESS request={0};
    BC250_WDDM_OBJECT game={0},desktop={0};
    BC250_SCALER_PLAN shape,other;
    Setup(&d,&w,level);
    memset(&shape,0,sizeof(shape));
    shape.SourceWidth=1366;shape.SourceHeight=704;shape.TargetWidth=1366;shape.TargetHeight=768;
    shape.Scaling=BC250_SCALING_PLANE;shape.Recout.Width=1366;shape.Recout.Height=768;
    other=shape;other.Recout.Y=32;other.Recout.Height=704;
    game.Magic=BC250_WDDM_MAGIC_ALLOCATION;game.Serial=0x7101;game.UmdAlloc=1;game.ScanoutRequested=1;
    game.ScanoutWidth=1366;game.ScanoutHeight=704;game.ScanoutPitch=5632;game.UmdBytes=5632ull*704;
    game.ScanoutFormat=D3DDDIFMT_A8R8G8B8;
    desktop.Magic=BC250_WDDM_MAGIC_ALLOCATION;desktop.Serial=0x7102;
    desktop.Allocation.Width=1366;desktop.Allocation.Height=768;desktop.Allocation.Pitch=5632;
    desktop.Allocation.Format=D3DDDIFMT_A8R8G8B8;desktop.Allocation.Size=5632ull*768;
    request.PrimarySegment=BC250_WDDM_SEGMENT_VRAM;
    /* Full size, the game chain is the wrong geometry, as before this work. */
    request.hAllocation=&game;request.PrimaryAddress.QuadPart=0x50000;
    CHECK(Flip(&d,&request)==STATUS_INVALID_PARAMETER && hardware==0);
    /* Shaped, it is the plane's own size: programmed with the shape, published like any scan-out flip. */
    CHECK(ShapedFlip(&d,&request,&shape)==STATUS_SUCCESS);
    CHECK(hardware==1 && shaped_writes==1 && shape_on && plane_card==0x50000 && programmed_pitch==5632);
    CHECK(w.PrimaryAddress.QuadPart==0x50000 && w.PrimaryBytes==5632ull*704 && w.ScanoutObject==(LONGLONG)game.Serial);
    CHECK(!in_lock);
    /* The same flip again is no change; the same address in another shape is one. */
    CHECK(ShapedFlip(&d,&request,&shape)==STATUS_SUCCESS && hardware==1);
    CHECK(ShapedFlip(&d,&request,&other)==STATUS_SUCCESS && hardware==2 && shaped_writes==2 && shape_now.Recout.Y==32);
    /* A NULL allocation keeps the surface and its shape (the DDI's rule): the shaped write at the kept shape. */
    request.hAllocation=NULL;request.PrimaryAddress.QuadPart=0x60000;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && hardware==3 && shaped_writes==3 && shape_on && shape_now.Recout.Y==32);
    /* A shape needs its allocation. */
    CHECK(ShapedFlip(&d,&request,&shape)==STATUS_NOT_SUPPORTED && hardware==3);
    /* The compositor's full-size primary at the published address ends the shape: a change, so written. */
    request.hAllocation=&desktop;
    CHECK(Flip(&d,&request)==STATUS_SUCCESS && hardware==4 && !shape_on && w.ScanoutObject==0);
    CHECK(w.PrimaryBytes==5632ull*768);
    /* The flip gate closed: no pipe to shape, refused; the full-size flip publishes as before. */
    request.hAllocation=&game;
    d.VidPnFlipEnabled=0;
    CHECK(ShapedFlip(&d,&request,&shape)==STATUS_NOT_SUPPORTED && hardware==4);
    d.VidPnFlipEnabled=1;
    /* A refused shaped write leaves the plane as it is. */
    hardware_result=STATUS_ACCESS_DENIED;
    CHECK(ShapedFlip(&d,&request,&shape)==STATUS_ACCESS_DENIED && !shape_on && w.PrimaryAddress.QuadPart==0x60000);
    hardware_result=STATUS_SUCCESS;
    CHECK(!in_lock);
}
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
static void LatchProbeOwner(void)
{
    BC250_WDDM w;BC250_DEVICE d;ULONG generation;unsigned before;
    Setup(&d,&w,0);begin_kind=BC250_WRITER_LATCH_PROBE;
    CHECK(!NT_SUCCESS(WddmLatchProbeCheck(&d,0,FALSE)));
    CHECK(Begin(&d,&generation));
    CHECK(!NT_SUCCESS(WddmLatchProbeCheck(&d,generation,FALSE)) && !w.Faulting);
    CHECK(NT_SUCCESS(Resume(&d)));
    CHECK(NT_SUCCESS(WddmLatchProbeCheck(&d,generation,FALSE)));
    CHECK(!NT_SUCCESS(WddmLatchProbeCheck(&d,generation+2,TRUE)) && !w.Faulting);
    before=hardware;
    CHECK(!NT_SUCCESS(WddmLatchProbeCheck(&d,generation,TRUE)) && w.Faulting);
    CHECK(!NT_SUCCESS(End(&d,generation,FALSE)) && hardware==before && !w.WriterActive);
    Setup(&d,&w,0);begin_kind=BC250_WRITER_LATCH_PROBE;
    CHECK(Begin(&d,&generation));
    CHECK(!NT_SUCCESS(WddmLatchProbeCheck(&d,generation,TRUE)) && w.Faulting && !w.WriterResumed);
    CHECK(!NT_SUCCESS(End(&d,generation,FALSE)) && !w.WriterActive);
    Setup(&d,&w,0);begin_kind=BC250_WRITER_LATCH_PROBE;
    CHECK(Begin(&d,&generation) && NT_SUCCESS(Resume(&d)));
    sync_refusals=1;before=hardware;
    CHECK(!NT_SUCCESS(WddmLatchProbeCheck(&d,generation,FALSE)) && w.WriterRecovery);
    CHECK(!NT_SUCCESS(End(&d,generation,FALSE)) && w.Faulting && hardware==before && !w.WriterActive);
    Setup(&d,&w,0);
    CHECK(Begin(&d,&generation) && NT_SUCCESS(Resume(&d)));
    CHECK(!NT_SUCCESS(WddmLatchProbeCheck(&d,generation,TRUE)) && !w.Faulting);
    CHECK(NT_SUCCESS(End(&d,generation,FALSE)));
}
#endif
int main(void)
{
    int level;
    for(level=0;level<2;level++){
        Publication(level);
        ImmediateFlips(level);
        ImmediateUnproven(level);
        FaultProgram(level);
        Overlap(level);
        Writers(level);
        WriterRetirement(level);
        WriterRecovery(level);
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        Witnesses(level);
#endif
        GateClosed(level);
        Exclusive(level);
        PlaneShapeFlips(level);
    }
    FlipFlagBits();
    PlaneFormatFlips();
    FaultDpcs();
    DestroyWriter();
    WriterSynchronizationRefusals();
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    LatchProbeOwner();
#endif
    Entries();
    VSyncControlHistory();
    RecordCopies();
    DmaFlips();
    CHECK(!in_lock);
    printf("vidpn publication: %u checks, %u failures\n",checks,fails);
    return fails?1:0;
}
