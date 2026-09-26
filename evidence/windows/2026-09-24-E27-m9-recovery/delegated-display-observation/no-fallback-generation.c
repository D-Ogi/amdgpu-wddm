#include "paging_capture.h"
#include "paging_stream.h"
#include "paging_permutation.h"
// Full WDDM miniport, enabled through EnableFullWddm. Started as M7 stage A;
// current paths include hardware UMD submission, SDMA paging and CPU Present.
// Completion must follow actual work. Malformed virtual submission may return
// INVALID_PARAMETER; physical submission has a different error contract.
// ResetFromTimeout currently fails because hardware quiescence is not proven.
// See docs/research/m9-dma-contract-audit.md for remaining contract gaps.
#include "bc250kmd.h"
#include "startup.h"
#include "bc250_gfx.h"
#include "dcn_translate.h"
#include "umd_blob.h"
#include "umd_caps.h"
#include "paging_private.h"
#include <ntstrsafe.h>

#define BC250_WDDM_TAG 'wW2B'
#define BC250_WDDM_LOG_CALLS 8              // how many first calls of each DDI reach the guard log
#define BC250_WDDM_PRESENT_LIST_QWORDS 12u  // how much of a present's allocation list is read: 3 entries of either arm

// Segment ids are one-based: DXGK_QUERYSEGMENTOUT4.PagingBufferSegmentId is "the index (starting from 1)".
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_SEGMENT_TABLES 3u
// An aperture segment, as Microsoft's RosKmd has one: VidMm backs it with system pages and asks for them to be mapped
// with BuildPagingBuffer (MapApertureSegment), which stage A answers inertly like every other operation. It exists
// because a GPU-VA context's DMA buffers must be VidMm allocations in an aperture segment: with "system memory"
// (segment set 0) dxgmms2 maps a NULL allocation into the context's address space and the machine goes down
// (E16 run 008, VIDMM_DMA_POOL::AddDmaBufferToPool). Stage B gives it the real GART behind it.
#define BC250_WDDM_SEGMENT_APERTURE 2u
#define BC250_WDDM_APERTURE_BYTES PAGING_APERTURE_BYTES
#define BC250_WDDM_SEGMENT_SET(id) (1u << ((id) - 1))
#define BC250_WDDM_NODE_3D 0u
// ADR 0008 stage D (docs/design/paging-node.md): node 1, DXGK_ENGINE_TYPE_COPY on SDMA0, the paging node,
// behind EnablePagingNode. BC250_WDDM_NODE_COUNT is no longer a compile-time fact - wddm->NodeCount (1 or 2,
// WddmStart) is what every bound check and caps answer below now reads; the macro stays only as the value that
// field is initialized to with the gate closed, so that a grep for "how many nodes" still finds one definition.
#define BC250_WDDM_NODE_COPY 1u
#define BC250_WDDM_NODE_COUNT 1u            // gate closed: the value wddm->NodeCount starts at, and the only one C_ASSERT still checks
#define BC250_WDDM_NODE_COUNT_MAX 2u        // sizes every per-node array below, gate open or closed

// The GPU virtual address space stage A declares. Four levels of nine index bits over 4 KB pages is what GFX10's
// GPUVM does and what M4's page table format (facts M37) is built for, so stage B can keep the numbers; the root
// table is one 4 KB page either way. ASSUMPTION until stage B drives a real allocation through it: nothing in
// stage A creates a page table, so nothing here has been tested against VidMm.
#define BC250_WDDM_PAGE_SHIFT 12u
#define BC250_WDDM_LEVEL_BITS 9u
#define BC250_WDDM_LEVEL_COUNT 4u
#define BC250_WDDM_VA_BITS (BC250_WDDM_PAGE_SHIFT + BC250_WDDM_LEVEL_BITS * BC250_WDDM_LEVEL_COUNT)
#define BC250_WDDM_PTES_PER_LEVEL (1u << BC250_WDDM_LEVEL_BITS)
#define BC250_WDDM_PTE_BYTES 8u
#define BC250_WDDM_PAGE_TABLE_BYTES (BC250_WDDM_PTES_PER_LEVEL * BC250_WDDM_PTE_BYTES)

// OS-owned paging DMA storage. Use the prepared aperture so VidMm can assign
// GPU virtual addresses; the private command copy remains until native IB admission.
#define BC250_WDDM_PAGING_BUFFER_BYTES 0x10000ul

// One object kind per magic, so that a handle that is not ours is caught before it is dereferenced.
#define BC250_WDDM_MAGIC_DEVICE     'vD7M'
#define BC250_WDDM_MAGIC_CONTEXT    'xC7M'
#define BC250_WDDM_MAGIC_PROCESS    'cP7M'
#define BC250_WDDM_MAGIC_RESOURCE   'sR7M'
#define BC250_WDDM_MAGIC_ALLOCATION 'lA7M'
#define BC250_WDDM_MAGIC_OPENED     'pO7M'  // an allocation opened on a device: what a DXGK_ALLOCATIONLIST entry names

// The DDIs this file adds, in the order the table declares them. Only used to count calls for the log.
typedef enum _BC250_WDDM_DDI {
    WddmDdiQueryAdapterInfo = 0,
    WddmDdiGetNodeMetadata,
    WddmDdiCreateDevice,
    WddmDdiDestroyDevice,
    WddmDdiCreateContext,
    WddmDdiDestroyContext,
    WddmDdiCreateProcess,
    WddmDdiDestroyProcess,
    WddmDdiGetRootPageTableSize,
    WddmDdiSetRootPageTable,
    WddmDdiCreateAllocation,
    WddmDdiDestroyAllocation,
    WddmDdiDescribeAllocation,
    WddmDdiGetStandardAllocationDriverData,
    WddmDdiOpenAllocation,
    WddmDdiCloseAllocation,
    WddmDdiBuildPagingBuffer,
    WddmDdiSubmitCommand,
    WddmDdiSubmitCommandVirtual,
    WddmDdiPreemptCommand,
    WddmDdiResetFromTimeout,
    WddmDdiRestartFromTimeout,
    WddmDdiQueryDependentEngineGroup,
    WddmDdiQueryEngineStatus,
    WddmDdiResetEngine,
    WddmDdiSetStablePowerState,
    WddmDdiCalibrateGpuClock,
    WddmDdiFormatHistoryBuffer,
    WddmDdiPresent,
    WddmDdiSetVidPnSourceAddress,
    WddmDdiControlInterrupt,
    WddmDdiGetScanLine,
    WddmDdiCount
} BC250_WDDM_DDI;

static const char* const g_DdiNames[] = {
    "QueryAdapterInfo", "GetNodeMetadata", "CreateDevice", "DestroyDevice", "CreateContext", "DestroyContext",
    "CreateProcess", "DestroyProcess", "GetRootPageTableSize", "SetRootPageTable", "CreateAllocation",
    "DestroyAllocation", "DescribeAllocation", "GetStandardAllocationDriverData", "OpenAllocation",
    "CloseAllocation", "BuildPagingBuffer", "SubmitCommand", "SubmitCommandVirtual", "PreemptCommand",
    "ResetFromTimeout", "RestartFromTimeout", "QueryDependentEngineGroup", "QueryEngineStatus", "ResetEngine",
    "SetStablePowerState", "CalibrateGpuClock", "FormatHistoryBuffer", "Present", "SetVidPnSourceAddress", "ControlInterrupt",
    "GetScanLine",
};
C_ASSERT(RTL_NUMBER_OF(g_DdiNames) == WddmDdiCount);

// CollectDbgInfo is a Level Zero DDI and may not touch BC250_WDDM (see the function): its counter lives here.
static volatile LONG g_CollectDbgInfoCalls;

// A counter is only half the evidence: "QueryAdapterInfo was called 31 times" does not say which 31 things
// dxgkrnl wanted. These tables hold the distinct argument values a DDI was called with - QueryAdapterInfo's
// information type, BuildPagingBuffer's operation - with the count and the log sequence number of the first
// call for each. 24 slots is more than either enum has values that stage A can meet.
#define BC250_WDDM_KINDS 24

typedef struct _BC250_WDDM_KIND {
    volatile LONG Value;                // the argument plus one, so that 0 means "this slot is free"
    volatile LONG Count;
    volatile LONG FirstSequence;
} BC250_WDDM_KIND;

// What stage A knows about an allocation. This is our own private data, not the user-mode contract of ADR 0008
// point 8 (driver/contract/): stage A has no user-mode driver to agree with, and the blob dies with this stage.
#define BC250_WDDM_ALLOCATION_PRIVATE_MAGIC 0x4137424Cul    // "LB7A"
typedef struct _BC250_WDDM_ALLOCATION_PRIVATE {
    ULONG Magic;
    ULONG Version;
    ULONG Width;
    ULONG Height;
    ULONG Pitch;
    ULONG Format;                       // D3DDDIFORMAT
    ULONGLONG Size;
} BC250_WDDM_ALLOCATION_PRIVATE;

// E26: Present constructs a packet; SubmitCommandVirtual executes it after
// SetRootPageTable. Never dereference a transient Present rectangle pointer later.
#define BC250_PRESENT_PACKET_MAGIC 0x50363245ul // "E26P", software command, not a GPU opcode
#define BC250_PRESENT_PACKET_RECTS 128u
typedef struct _BC250_PRESENT_PACKET {
    ULONG Magic;
    ULONG RectCount;
    DXGK_PRESENTALLOCATIONINFO Allocations[3];
    RECT SrcRect, DstRect;
    RECT Rects[BC250_PRESENT_PACKET_RECTS];
} BC250_PRESENT_PACKET;

typedef struct _BC250_WDDM_OBJECT {
    LIST_ENTRY Link;                    // BC250_WDDM::Objects: the stop frees whatever is still on this list
    ULONG Magic;
    BC250_DEVICE* Device;
    UINT NodeOrdinal;                   // contexts
    ULONGLONG RootPhysical;             // contexts: the root page table VidMm last set, as a physical address; 0 = none
    PAGING_CAPTURE_OWNER Captures;     // contexts: CPU-only plans, released on completion or object teardown
    UINT AllocationListSize;            // contexts: what CreateContext answered, i.e. how long a list dxgkrnl keeps for it
    BC250_WDDM_ALLOCATION_PRIVATE Allocation;
    // M8: a context or allocation that arrived as a contract blob (umd_blob.c), not the GDI one above.
    // ExAllocatePool2 zeroes these, so a GDI object stays "not UMD" without a store. UmdRequestedVa is
    // recorded and not applied: VidMm places the pages, and the winsys maps the GPU VA itself.
    BOOLEAN UmdAlloc;
    BOOLEAN UmdContext;
    BOOLEAN SystemContext;
    unsigned long UmdIpType;
    unsigned long UmdHeap;
    ULONGLONG UmdBytes;
    ULONGLONG UmdRequestedVa;
} BC250_WDDM_OBJECT;

// The software VSync. FlipOnVSyncMmIo means dxgkrnl retires a queued flip when the driver reports
// DXGK_INTERRUPT_CRTC_VSYNC, and nothing else retires it: with MaxQueuedFlipOnVSync = 1 and no report at all, the
// second flip never leaves the queue and DWM stops. Stage A may not read the display core (ADR 0006 point 2), so
// there is no real VSync to report - a periodic timer stands in for it. The firmware's mode carries no refresh
// rate (display.c reports D3DKMDT_FREQUENCY_NOTSPECIFIED for exactly that reason), so 60 Hz is the assumption.
// KeSetTimerEx takes whole milliseconds, so the period is 16 ms and the real rate is about 62.5 Hz; the system
// clock's own granularity is coarser than that error, and nothing in stage A depends on the exact number.
#define BC250_WDDM_VSYNC_HZ 60
#define BC250_WDDM_VSYNC_MS (1000 / BC250_WDDM_VSYNC_HZ)

// Every paging builder reserves this OS-private slot before submission.
// Storage belongs to the DMA buffer and is retained until its real fence.
typedef struct _BC250_PAGING_JOB {
    struct _BC250_PAGING_JOB* Next;
    ULONGLONG Start;
    ULONG ByteCount, PrivateBytes;
    UINT Fence;
    BOOLEAN VirtualAddress, Borrowed;
    const UCHAR* Data;
} BC250_PAGING_JOB;
C_ASSERT(sizeof(BC250_PAGING_JOB)<=PAGING_PRIVATE_JOB_BYTES);

typedef struct _BC250_WDDM {
    BC250_DEVICE* Device;
    volatile LONG Calls[WddmDdiCount];
    volatile LONG FirstSequence[WddmDdiCount];      // the log sequence number of each DDI's first call

    // What dxgkrnl asked for, not only how often. Both are written lock-free from paths that may run at
    // DISPATCH_LEVEL; WddmSummary prints them at the stop or on demand.
    BC250_WDDM_KIND AdapterInfo[BC250_WDDM_KINDS];  // DXGKARG_QUERYADAPTERINFO.Type
    BC250_WDDM_KIND PagingOps[BC250_WDDM_KINDS];    // DXGK_BUILDPAGINGBUFFER_OPERATION
    volatile LONG AdapterInfoOverflow;              // distinct values that found no free slot
    volatile LONG PagingOpsOverflow;
    // Adapter-lifetime totals survive context release and diagnostic ring wrap.
    volatile LONG64 CaptureReservedTotal, CaptureHeapTotal;
    volatile LONG64 CaptureContextPeakPlans, CaptureContextPeakReservedBytes;
    volatile LONG ReportFailures;                   // DxgkCbSynchronizeExecution refusals in WddmReport

    // Everything that can add work - an object, a timer, a DPC - is decided and done inside this lock, and
    // Stopping is what makes the stop final: it is set first, under the lock, so that a timer or a DPC cannot
    // re-arm or re-queue itself behind the cancel and the flush that follow. It does not remove the need for
    // dxgkrnl's own guarantee that no DDI arrives during StopDevice; it removes every race this file could
    // cause itself, which is the part we control.
    EX_PUSH_LOCK PagingBuildLock; // serializes logical mappings across each paging DDI batch
    KSPIN_LOCK Lock;                    // taken at <= DISPATCH_LEVEL, never held across a call into dxgkrnl
    BOOLEAN Stopping;
    LIST_ENTRY Objects;                 // devices, contexts, processes and allocations alive
    LONG ObjectCount;

    // Submission. The DDI records the fence and queues ReportDpc; the report happens there, after the DDI has
    // returned, so that nothing calls back into dxgkrnl from inside a submit. A completion of fence N retires
    // every fence up to N, which is what real hardware reports out of a write-back slot, so one pair of values
    // per node is enough and the order is preserved by construction. ADR 0008 stage D: with node 1 real, both
    // nodes can have an outstanding completion at once, so this is a pair of values PER NODE
    // ([BC250_WDDM_NODE_3D]/[BC250_WDDM_NODE_COPY]), not the single shared pair the C_ASSERT below used to bind -
    // that assert now only pins wddm->NodeCount's gate-closed starting value, not the array width.
    volatile LONG SubmittedFence[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG SubmittedNode[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG ActiveSubmissions[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG LastReportedFence[BC250_WDDM_NODE_COUNT_MAX];
    BOOLEAN LastReportedValid[BC250_WDDM_NODE_COUNT_MAX];
    BOOLEAN RefusalPending[BC250_WDDM_NODE_COUNT_MAX]; // valid DMA never dispatched; cannot retire in software
    BOOLEAN RejectedPending[BC250_WDDM_NODE_COUNT_MAX];
    UINT RejectedFence[BC250_WDDM_NODE_COUNT_MAX];
    BOOLEAN WatchdogFaulted[BC250_WDDM_NODE_COUNT_MAX]; // sticky until adapter state is rebuilt
    volatile LONG CompletionPending[BC250_WDDM_NODE_COUNT_MAX];    // set by the submit, cleared by the DPC
    volatile LONG LastCompletedFence;   // "the driver must always maintain the last completed fence ID value";
                                         // shared across nodes on purpose (design note section 5): a lab
                                         // simplification, not a claim that dxgkrnl only ever sees one node's value
    UINT NodeCount;                     // 1 with EnablePagingNode closed, 2 open; read once in WddmStart

    // Preemption goes through the same DPC, and after the completion, so that the fence it reports as last
    // completed is the one the completion just published rather than the one before it. Per node, same reason
    // as the completion pair above.
    volatile LONG PreemptionFence[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG PreemptionNode[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG PreemptionPending[BC250_WDDM_NODE_COUNT_MAX];
    BOOLEAN ReportActive;
    BOOLEAN ReportAgain;
    KDPC ReportDpc;

    // ADR 0008 stage D (docs/design/paging-node.md section 5): node 1's own hardware channel, parallel to stage
    // C's node-0 one below and never touching it - the two nodes fail independently, on their own hardware.
    BC250_PAGING_JOB* PagingHead;
    BC250_PAGING_JOB* PagingTail;
    ULONGLONG PagingDeadline;
    BOOLEAN PagingHwPending;
    ULONG PagingHwSeq;                  // gfx.c's PagingSubmitSeq of the submission in flight
    UINT PagingHwFence;
    // Legacy software-deferral fields; queued node-1 software work now uses FIFO jobs.
    BOOLEAN PagingDeferredValid;
    UINT PagingDeferredFence;
    KTIMER PagingSubmitTimer;
    KDPC PagingSubmitDpc;
    volatile LONG PagingQueueBorrowed;
    volatile LONG PagingHwSubmitted;
    volatile LONG PagingHwCompleted;
    volatile LONG PagingHwTimeouts;
    volatile LONG PagingHwRefused;
    // BuildPagingBuffer's own counters (design note section 7): built vs. answered inertly, by reason.
    volatile LONG PagingTransfersBuilt;
    volatile LONG PagingFillsBuilt;
    volatile LONG PagingFlushesBuilt;
    volatile LONG PagingUpdatesBuilt;
    volatile LONG PagingMapsBuilt;
    volatile LONG PagingUnmapsBuilt;
    volatile LONG64 PagingBytesMoved;
    volatile LONG PagingInsufficientBuffer;
    volatile LONG PagingUnsupported[BC250PagingNotContiguous + 1]; // indexed by BC250_WDDM_PAGING_UNSUPPORTED
    // Per-buffer private data owns commands; these counters track node routing only.
    volatile LONG PagingVirtualSubmits[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG PagingVirtualUnmapped;
    volatile LONG64 PagingDmaVaBuilds, PagingDmaZeroVaBuilds;
    volatile LONG PagingDmaVaObserved, PagingDmaVaMatched;
    ULONGLONG PagingDmaLastVa, PagingDmaLastRoot, PagingDmaLastPa, PagingDmaLastCpu;
    volatile LONG64 PagingNativeTransfers, PagingNativeFills, PagingNativeBytes, PagingDmaGapProofs;

    // Stage C: a DMA buffer with bytes in it goes down the gfx ring (gfx.c, one in flight at most) and its fence is
    // reported when the hardware's arrives - from the IH DPC, from the submit itself if the interrupt won the race,
    // or from the watchdog. Software completions that come while one is in flight are held back and published
    // with it: a completion of fence N retires every fence up to N, so N + 1 must not be reported first.
    // All of it under Lock.
    BOOLEAN HwPending;
    ULONG HwSeq;                        // gfx.c's sequence number of the submission in flight
    UINT HwFence;
    UINT HwNode;
    BOOLEAN DeferredValid;
    UINT DeferredFence;
    KTIMER SubmitTimer;                 // there is no GPU reset on this part (facts M53): a fence that does not
    KDPC SubmitDpc;                     // arrive is completed in software and the ring is left alone from then on
    volatile LONG HwSubmitted;
    volatile LONG HwCompleted;
    volatile LONG HwTimeouts;
    volatile LONG HwRefused;
    // M8: contract blobs (umd_blob.c). Alloc refusals and submits that did not reach the ring are counted
    // separately from the GDI path, so a desktop present cannot spend the evidence.
    volatile LONG UmdAllocs;
    volatile LONG UmdAllocRefused;
    volatile LONG UmdContexts;
    volatile LONG UmdSubmitHw;
    volatile LONG UmdSubmitSoft;
    BOOLEAN TraceUmdProbes;             // diagnostic reads only; no synchronization policy
    volatile LONG UmdProfileCalls;
    volatile LONG UmdProbeCalls;
    volatile LONG64 UmdSubmitTicks;     // QPC elapsed time inside WddmSubmitUmd, including waits
    volatile LONG64 UmdProbeTicks;      // subset spent reading/logging IB and shader contents
    LARGE_INTEGER UmdProfileFrequency;

    KTIMER VSyncTimer;
    KDPC VSyncDpc;
    BOOLEAN VSyncArmed;                 // the timer is running (a source is visible)
    BOOLEAN VSyncEnabled;               // ControlInterrupt turned CRTC_VSYNC on
    D3DDDI_VIDEO_PRESENT_TARGET_ID VSyncTargetId;
    volatile LONG VSyncTicks;           // timer ticks, whether or not anybody was listening
    volatile LONG VSyncReports;         // of those, the ones reported to dxgkrnl as DXGK_INTERRUPT_CRTC_VSYNC
    LARGE_INTEGER VSyncLast;            // the performance counter at the last tick, for GetScanLine's phase
    LARGE_INTEGER VSyncFrequency;

    ULONG PrimaryPitch;
    ULONGLONG PrimaryBytes;
    PHYSICAL_ADDRESS PrimaryAddress;    // last successfully programmed address (retire only after flip pending clears)
    volatile LONG PrimarySequence;      // even = published; odd = programming, never spin at DIRQL
    UINT PrimarySegment;

    // E20 (ADR 0011): the diagnostic CPU blit of a Blt present into the firmware framebuffer, behind EnablePresentBlit.
    BOOLEAN BlitGate;
    volatile LONG Blits;                        // presents copied
    volatile LONG BlitSkips;                    // presents that named no usable source (reason in the log)
    volatile LONG BlitTranslations;             // sources whose first and last page translated and were contiguous
    // 2026-09-22 (ADR 0011 consequences, facts M100): of Blits, the breakdown by destination. BlitsToFlip +
    // BlitsToFirmware == Blits always; BlitsMapFailed is the subset of BlitsToFirmware that landed there only
    // because the flip surface would not map (a fallback, never a skip - see WddmPresentBlit).
    volatile LONG BlitsToFlip;                  // copied to the surface the scanout has actually flipped to
    volatile LONG BlitsToFirmware;              // copied to the POST framebuffer (flip not live, or the fallback below)
    volatile LONG BlitsMapFailed;                // of BlitsToFirmware, a fallback because DcnScanoutMapping refused
    volatile LONG BlitRowsLast;                 // rows the last blit actually copied; 0 with BlitsToFlip climbing was M115
    volatile LONG BlitRowsMax;                  // the most rows any one blit copied; 1200 would be a full frame
    volatile LONG BlitSeeds;                    // flip targets that got one copy of the firmware framebuffer (M116)
    volatile LONG BlitReadingLast;              // which present-list slot the last blit took (1 entry 0, 2 the 24-byte misread, 3 entry 1)
    volatile LONG BlitWidthLast;
    volatile LONG BlitHeightLast;
    volatile LONG BlitPitchLast;
    volatile LONG BlitSubRectsLast;
    volatile LONGLONG BlitSourceLast;           // system physical of the last blit source, 0 if none translated
    volatile LONG Flips;                        // SetVidPnSourceAddress calls that changed the scanout address
    volatile LONG FlipsAboveDispatch;           // of all SetVidPnSourceAddress calls, those that arrived at DIRQL
} BC250_WDDM;

// The gate-closed starting value of wddm->NodeCount (WddmStart) and of the DXGK_DRIVERCAPS answer before
// EnablePagingNode is read: this build must report exactly one node until the gate says otherwise.
C_ASSERT(BC250_WDDM_NODE_COUNT == 1);
C_ASSERT(BC250_WDDM_NODE_COPY < BC250_WDDM_NODE_COUNT_MAX);

static BOOLEAN g_FullWddm;              // the gate, read once in DriverEntry
static BOOLEAN g_ApertureOffered;       // QUERYSEGMENT4 described segment 2; CreateContext may only name it then

// TRUE once WddmStop has begun. Read under the lock, because the whole point of the flag is the ordering.
static BOOLEAN WddmStopping(_In_ BC250_WDDM* Wddm)
{
    BOOLEAN stopping;
    KIRQL irql;

    KeAcquireSpinLock(&Wddm->Lock, &irql);
    stopping = Wddm->Stopping;
    KeReleaseSpinLock(&Wddm->Lock, irql);
    return stopping;
}

// ---- the gate --------------------------------------------------------------------------------------------------

BOOLEAN WddmGateOpen(void)
{
    // 1 opens the gate for this driver load only (the value is 0 on disk again before the table is handed over);
    // 2 keeps it open across loads and boots, for the day the full table has earned that.
    ULONG gate = GuardConsumeSetting(L"EnableFullWddm", 0);

    g_FullWddm = (gate == 1 || gate == 2);
    GuardLog("gate: EnableFullWddm %u%s", gate, gate == 1 ? " (one shot: closed again on disk)" : "");
    return g_FullWddm;
}

// ---- the log ---------------------------------------------------------------------------------------------------

// TRUE for the first BC250_WDDM_LOG_CALLS calls of this DDI, so that the caller may log. The count itself is
// interlocked and is taken at any IRQL; only the permission to log is withheld above DISPATCH_LEVEL, because the
// ring behind GuardLog needs a spin lock. One DDI in this table is annotated for that height
// (DxgkDdiSetVidPnSourceAddress, _IRQL_requires_max_(PROFILE_LEVEL - 1)), and it is counted there like any other.
static BOOLEAN WddmFirstCalls(_In_opt_ BC250_WDDM* Wddm, BC250_WDDM_DDI Ddi)
{
    LONG calls;

    if (Wddm == NULL) return FALSE;
    calls = InterlockedIncrement(&Wddm->Calls[Ddi]);
    if (calls == 1) InterlockedExchange(&Wddm->FirstSequence[Ddi], (LONG)GuardLogSequence());
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) return FALSE;
    return calls <= BC250_WDDM_LOG_CALLS;
}

// TRUE while QueryAdapterInfo is still in its first 64 calls, i.e. during adapter initialization. The lines that
// log what a query was answered (not only that it was asked) are behind this, so that a dxgkrnl that asks for
// the caps again at every power transition cannot push the start-up out of the ring's tail. It reads the
// counter WddmFirstCalls keeps and does not count itself.
static BOOLEAN WddmAnswersLogged(_In_ const BC250_DEVICE* Device)
{
    const BC250_WDDM* wddm = (const BC250_WDDM*)Device->Wddm;

    return wddm != NULL && wddm->Calls[WddmDdiQueryAdapterInfo] < 64;
}

// Count one argument value in a kind table. Lock-free, because it is called from DDIs that may run at
// DISPATCH_LEVEL and the summary that reads it never runs concurrently with a stop. A slot is claimed with one
// compare-exchange and never released; a value that finds no free slot is counted in the overflow instead of
// being dropped silently. Both callers pass a small enum, so the plus-one that marks a slot as taken cannot wrap.
static void WddmNoteKind(_Inout_updates_(BC250_WDDM_KINDS) BC250_WDDM_KIND* Table, ULONG Value,
                         _Inout_ volatile LONG* Overflow)
{
    const LONG stored = (LONG)(Value + 1);
    ULONG i;

    for (i = 0; i < BC250_WDDM_KINDS; i++)
    {
        LONG seen = Table[i].Value;
        if (seen == 0) seen = InterlockedCompareExchange(&Table[i].Value, stored, 0);
        if (seen == 0)
        {
            InterlockedExchange(&Table[i].FirstSequence, (LONG)GuardLogSequence());
            InterlockedIncrement(&Table[i].Count);
            return;
        }
        if (seen == stored) { InterlockedIncrement(&Table[i].Count); return; }
    }
    InterlockedIncrement(Overflow);
}

static BC250_WDDM* WddmOf(_In_ const HANDLE hAdapter)
{
    const BC250_DEVICE* device = (const BC250_DEVICE*)hAdapter;

    return (device != NULL) ? (BC250_WDDM*)device->Wddm : NULL;
}

// ---- objects ---------------------------------------------------------------------------------------------------

// Every object is on the adapter's list from the moment it exists, so that a process or a device that dxgkrnl
// never tears down is freed by the stop instead of leaked for the life of the boot.
// Captures stays caller-owned on refusal. On success, ownership moves before
// list publication, so StopDevice never sees a partly prepared reservation.
static BC250_WDDM_OBJECT* WddmNewObjectPrepared(_Inout_ BC250_DEVICE* Device, ULONG Magic,
    PAGING_CAPTURE_OWNER* Captures)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BC250_WDDM_OBJECT* object;
    KIRQL irql;

    if (wddm == NULL) return NULL;      // the start found no pool, or the stop has already run: create nothing
    object = (BC250_WDDM_OBJECT*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*object), BC250_WDDM_TAG);
    if (object == NULL) return NULL;
    object->Magic = Magic;
    object->Device = Device;
    if(Captures)object->Captures=*Captures;

    // The Stopping check and the insertion are one critical section: an object that got onto the list after the
    // stop had drained it would never be freed.
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->Stopping)
    {
        KeReleaseSpinLock(&wddm->Lock, irql);
        ExFreePoolWithTag(object, BC250_WDDM_TAG);
        return NULL;
    }
    if(Captures)RtlZeroMemory(Captures,sizeof(*Captures));
    InsertTailList(&wddm->Objects, &object->Link);
    wddm->ObjectCount++;
    KeReleaseSpinLock(&wddm->Lock, irql);
    return object;
}

static BC250_WDDM_OBJECT* WddmNewObject(_Inout_ BC250_DEVICE* Device, ULONG Magic)
{
    return WddmNewObjectPrepared(Device,Magic,NULL);
}

static BC250_WDDM_OBJECT* WddmObject(_In_opt_ const HANDLE Handle, ULONG Magic)
{
    BC250_WDDM_OBJECT* object = (BC250_WDDM_OBJECT*)Handle;

    return (object != NULL && object->Magic == Magic) ? object : NULL;
}

// For a handle that did not come back through a DDI's own handle parameter but out of an array dxgkrnl filled - the
// DXGK_ALLOCATIONLIST entries of a Present (review 16). Nothing is read through the value: it is compared against the
// objects on the adapter's list under the lock, and only a match is dereferenced. <= DISPATCH_LEVEL.
static BC250_WDDM_OBJECT* WddmListedObject(_In_ BC250_WDDM* Wddm, _In_opt_ const HANDLE Handle, ULONG Magic)
{
    BC250_WDDM_OBJECT* found = NULL;
    const LIST_ENTRY* entry;
    KIRQL irql;

    if (Handle == NULL) return NULL;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    for (entry = Wddm->Objects.Flink; entry != &Wddm->Objects; entry = entry->Flink)
    {
        BC250_WDDM_OBJECT* object = CONTAINING_RECORD(entry, BC250_WDDM_OBJECT, Link);

        if ((HANDLE)object == Handle) { found = (object->Magic == Magic) ? object : NULL; break; }
    }
    KeReleaseSpinLock(&Wddm->Lock, irql);
    return found;
}

// Called only after the object is detached from the shared object list, or by
// StopDevice after admission/drain. Never runs while holding the spin lock.
static NTSTATUS WddmReserveCaptures(PAGING_CAPTURE_OWNER* Owner)
{
    // MS system paging process uses a 1 GiB VA window. Reserve once at context
    // admission, where allocation failure is legal, not during normal paging.
    SIZE_T bytes=GfxPagingCaptureStorageSize(0,0,1ull<<30);
    Owner->Storage=ExAllocatePool2(POOL_FLAG_NON_PAGED,bytes,BC250_WDDM_TAG);
    if(!Owner->Storage)return STATUS_INSUFFICIENT_RESOURCES;
    Owner->StorageBytes=bytes;
    GuardLog("wddm: capture reservation ready %llu bytes",(ULONGLONG)bytes);
    return STATUS_SUCCESS;
}

static void WddmReleaseCaptureOwner(PAGING_CAPTURE_OWNER* Owner)
{
    PAGING_CAPTURE* capture=PagingCaptureTakeAll(Owner);
    if(Owner->ReservedCaptures || Owner->HeapCaptures)
        GuardLog("wddm: capture release reserved %u heap %u",Owner->ReservedCaptures,Owner->HeapCaptures);
    while(capture) {
        PAGING_CAPTURE* next=capture->Next;
        if(!capture->ReservationBytes)ExFreePoolWithTag(capture,capture->PoolTag);
        capture=next;
    }
    if(Owner->Storage)ExFreePoolWithTag(Owner->Storage,BC250_WDDM_TAG);
    Owner->Storage=NULL;Owner->StorageBytes=0;
}

static BC250_WDDM_OBJECT* WddmNewContext(BC250_DEVICE* Device,BOOLEAN SystemContext)
{
    PAGING_CAPTURE_OWNER captures={0};
    BC250_WDDM_OBJECT* object;
    if(SystemContext && !NT_SUCCESS(WddmReserveCaptures(&captures)))return NULL;
    object=WddmNewObjectPrepared(Device,BC250_WDDM_MAGIC_CONTEXT,&captures);
    // Successful admission moved the reservation; a refusal leaves it here.
    WddmReleaseCaptureOwner(&captures);
    return object;
}

static void WddmReleaseCaptures(BC250_WDDM_OBJECT* Object)
{
    WddmReleaseCaptureOwner(&Object->Captures);
}

static void WddmFreeObject(_In_opt_ BC250_WDDM_OBJECT* Object)
{
    BC250_WDDM* wddm;
    KIRQL irql;

    if (Object == NULL) return;
    wddm = (Object->Device != NULL) ? (BC250_WDDM*)Object->Device->Wddm : NULL;
    if (wddm != NULL)
    {
        // Once the stop has begun, every object on the list belongs to the stop: taking one off here and freeing
        // it would race the drain and free it twice. The removal and the decision are one critical section.
        KeAcquireSpinLock(&wddm->Lock, &irql);
        if (wddm->Stopping) { KeReleaseSpinLock(&wddm->Lock, irql); return; }
        RemoveEntryList(&Object->Link);
        wddm->ObjectCount--;
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    WddmReleaseCaptures(Object);
    Object->Magic = 0;
    ExFreePoolWithTag(Object, BC250_WDDM_TAG);
}

// ---- reporting a packet as finished ----------------------------------------------------------------------------

typedef struct _BC250_WDDM_NOTIFY {
    BC250_DEVICE* Device;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA Data;
} BC250_WDDM_NOTIFY;

// Runs at the device's interrupt IRQL, under the interrupt's own lock: DxgkCbNotifyInterrupt must be called at
// DIRQL and never reentrantly, and DxgkCbSynchronizeExecution is the documented way to get there from a DDI
// (LEARN nc-d3dkmddi-dxgkcb_notify_interrupt, and the same recipe in the PreemptCommand page). No log here: this
// is above DISPATCH_LEVEL.
static BOOLEAN WddmNotifyRoutine(_In_ PVOID Context)
{
    BC250_WDDM_NOTIFY* notify = (BC250_WDDM_NOTIFY*)Context;

    notify->Device->Dxgk.DxgkCbNotifyInterrupt(notify->Device->Dxgk.DeviceHandle, &notify->Data);
    return TRUE;
}

// Stage A runs nothing on the GPU, so every packet is finished before this returns. Raise to interrupt level,
// report there, then queue the DPC the contract requires ("after the driver calls DXGKCB_NOTIFY_INTERRUPT but
// before the driver exits its ISR, the driver must queue a DPC"); Bc250DpcRoutine calls DxgkCbNotifyDpc.
static void WddmReport(_Inout_ BC250_DEVICE* Device, _In_ const DXGKARGCB_NOTIFY_INTERRUPT_DATA* Data)
{
    BC250_WDDM_NOTIFY notify;
    BOOLEAN returned = FALSE;
    NTSTATUS status;

    if (Device->Dxgk.DxgkCbSynchronizeExecution == NULL || Device->Dxgk.DxgkCbNotifyInterrupt == NULL ||
        Device->Dxgk.DxgkCbQueueDpc == NULL)
        return;                         // all three are needed: the report is worthless without the DPC that pairs with it
    notify.Device = Device;
    notify.Data = *Data;
    // One message was asked for in the INF and one was granted (facts M38), so the message number is 0.
    status = Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle, WddmNotifyRoutine, &notify, 0, &returned);
    if (!NT_SUCCESS(status))
    {
        // Nothing can be done from here: the DDIs this is called from must not fail. It is logged so that a
        // scheduler timeout in stage A has an explanation in the trail - the first few times and then every
        // 1024th, because the VSync DPC gets here 62 times a second and a failure that persists would push the
        // middle of the run out of the ring within seconds. The summary carries the full count.
        BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
        LONG failures = (wddm != NULL) ? InterlockedIncrement(&wddm->ReportFailures) : 1;

        if (failures <= 8 || (failures & 0x3FF) == 0)
            GuardLog("wddm: synchronize for interrupt type %u failed 0x%08X (failure %ld)",
                     (ULONG)Data->InterruptType, status, failures);
        return;
    }
    Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
}

// Queue the one DPC that reports, if the stop has not begun. Both callers are DDIs, so this runs at
// <= DISPATCH_LEVEL; the Stopping check and the insertion are one critical section, or a packet queued behind
// KeFlushQueuedDpcs would run against a freed state.
static void WddmQueueReport(_Inout_ BC250_WDDM* Wddm)
{
    KIRQL irql;

    KeAcquireSpinLock(&Wddm->Lock, &irql);
    if (!Wddm->Stopping) KeInsertQueueDpc(&Wddm->ReportDpc, NULL, NULL);    // FALSE only means one is already queued
    KeReleaseSpinLock(&Wddm->Lock, irql);
}

// The submit side. It records and queues; it must not report, because a DDI that calls back into dxgkrnl from
// inside the submit path re-enters the scheduler with the submit still on the stack.
// Caller holds Lock. Publish completion atomically with clearing hardware-pending state:
// otherwise a preemption DPC can observe idle hardware before its completion is queued.
static void WddmRecordCompletionLocked(BC250_WDDM* Wddm, UINT FenceId, UINT NodeOrdinal)
{
    Wddm->SubmittedNode[NodeOrdinal] = (LONG)NodeOrdinal;
    Wddm->SubmittedFence[NodeOrdinal] = (LONG)FenceId;
    Wddm->CompletionPending[NodeOrdinal] = 1;
}

// Same for a preemption. The DDI may not report inline either, and the fence it wants to name as last completed
// is only right once any pending completion has been published - which is why both go through one DPC.
static void WddmPreemptFence(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    KIRQL irql;
    BOOLEAN busy;
    LONG active;

    if (wddm == NULL || NodeOrdinal >= BC250_WDDM_NODE_COUNT_MAX) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    wddm->PreemptionNode[NodeOrdinal] = (LONG)NodeOrdinal;
    wddm->PreemptionFence[NodeOrdinal] = (LONG)FenceId;
    wddm->PreemptionPending[NodeOrdinal] = 1;
    busy = NodeOrdinal == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : wddm->HwPending;
    active = wddm->ActiveSubmissions[NodeOrdinal];
    KeReleaseSpinLock(&wddm->Lock, irql);
    GuardLog("wddm: preemption queued fence %u node %u hardware pending %u active submits %ld",
             FenceId, NodeOrdinal, busy, active);
    WddmQueueReport(wddm);
}

// ---- stage C: the hardware path --------------------------------------------------------------------------------

#define BC250_WDDM_VMID 1u                  // the one hardware VMID, re-pointed at the submitter's root by gfx.c
#define BC250_WDDM_SUBMIT_TIMEOUT_MS 500    // an M6 dispatch takes 28 us (facts M57); the TDR default is 2 s

// A completion that did not come from the hardware. While a hardware submission is in flight ON THAT NODE it
// waits for it: the two nodes run on different rings, with different fences and different watchdogs, and node
// 1's completion has no business waiting behind node 0's packet or being published under node 0's ordinal (the
// PagingDeferredValid field's own comment).
static BOOLEAN WddmSubmitPagingHardware(BC250_DEVICE* Device, BC250_WDDM* Wddm,
    const void* PrivateData, ULONG PrivateBytes, ULONGLONG Start,
    ULONG ByteCount, BOOLEAN VirtualAddress, UINT FenceId);
static void WddmFailSubmission(BC250_DEVICE* Device, UINT FenceId, UINT Node);

static void WddmCompleteSoftware(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN paging = NodeOrdinal == BC250_WDDM_NODE_COPY;
    BOOLEAN deferred;
    KIRQL irql;

    if (wddm == NULL || NodeOrdinal >= BC250_WDDM_NODE_COUNT_MAX) return;
    if (paging) {
        BOOLEAN nodeClosed;
        KeAcquireSpinLock(&wddm->Lock,&irql);
        nodeClosed=wddm->Stopping || wddm->WatchdogFaulted[NodeOrdinal];
        KeReleaseSpinLock(&wddm->Lock,irql);
        if (nodeClosed) return;
        if (!WddmSubmitPagingHardware(Device,wddm,NULL,0,0,0,FALSE,FenceId))
            WddmFailSubmission(Device,FenceId,NodeOrdinal);
        return;
    }
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->WatchdogFaulted[NodeOrdinal]) {
        KeReleaseSpinLock(&wddm->Lock, irql);
        return; // no later software fence may retire uncompleted work on a faulted node
    }
    deferred = paging ? (wddm->PagingHead != NULL) : wddm->HwPending;
    if (deferred && paging) { wddm->PagingDeferredValid = TRUE; wddm->PagingDeferredFence = FenceId; }
    else if (deferred) { wddm->DeferredValid = TRUE; wddm->DeferredFence = FenceId; }
    else WddmRecordCompletionLocked(wddm, FenceId, NodeOrdinal);
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!deferred) WddmQueueReport(wddm);
}

// Has the fence of the submission in flight arrived? Called from the IH DPC (pnp.c), from the submit and from the
// watchdog, at <= DISPATCH_LEVEL. GfxFenceArrived is a memory read.
// A valid command that never reached hardware remains outstanding to VidSch.
// Do not manufacture DMA_COMPLETED or misuse the OS-reserved DMA_FAULTED event.
// Closing the node also blocks later software retirement and boundary preemption.
static void WddmFailSubmission(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT Node)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    KIRQL irql;
    BOOLEAN first;
    if (wddm==NULL || Node>=BC250_WDDM_NODE_COUNT_MAX) return;
    KeAcquireSpinLock(&wddm->Lock,&irql);
    first=!wddm->RefusalPending[Node];
    wddm->RefusalPending[Node]=TRUE;
    wddm->WatchdogFaulted[Node]=TRUE;
    if (Node==BC250_WDDM_NODE_COPY) wddm->PagingDeferredValid=FALSE;
    else wddm->DeferredValid=FALSE;
    KeReleaseSpinLock(&wddm->Lock,irql);
    if (Node==BC250_WDDM_NODE_COPY) GfxPagingSubmitFail(Device);
    else GfxSubmitFail(Device);
    if (first) GuardLog("wddm: fence %u node %u NOT dispatched; node closed, no completion, recovery required",FenceId,Node);
}

void WddmGpuFence(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN done = FALSE;
    UINT fence = 0, node = 0;
    KIRQL irql;

    if (wddm == NULL) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->HwPending && GfxFenceArrived(Device, wddm->HwSeq))
    {
        done = TRUE;
        fence = wddm->DeferredValid ? wddm->DeferredFence : wddm->HwFence;
        node = wddm->HwNode;
        wddm->HwPending = FALSE;
        wddm->DeferredValid = FALSE;
        KeCancelTimer(&wddm->SubmitTimer);
        WddmRecordCompletionLocked(wddm, fence, node);
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!done) return;
    if (InterlockedIncrement(&wddm->HwCompleted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: hardware fence arrived, reporting fence %u", fence);
    WddmQueueReport(wddm);
}

static KDEFERRED_ROUTINE WddmSubmitDpcRoutine;
static void WddmSubmitDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    BOOLEAN timedOut = FALSE;
    UINT fence = 0, node = 0;
    ULONG seq = 0;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    WddmGpuFence(device);               // late is still arrived
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->HwPending && !wddm->WatchdogFaulted[wddm->HwNode])
    {
        timedOut = TRUE;
        fence = wddm->HwFence;
        node = wddm->HwNode;
        seq = wddm->HwSeq;
        wddm->WatchdogFaulted[node] = TRUE;
        wddm->DeferredValid = FALSE;
        // Preserve HwPending: timeout is not a hardware completion.
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // Stop further submissions, but leave the uncompleted fence visible to the OS.
    // Its normal TDR path owns recovery. A later real fence may still complete this job.
    InterlockedIncrement(&wddm->HwTimeouts);
    GfxSubmitFail(device);
    GuardLog("wddm: HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u remains pending for OS TDR, ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    // No DMA_COMPLETED or preemption notification is synthesized here.
}

// PASSIVE_LEVEL (SubmitCommandVirtual). TRUE = the packet is on the ring and its completion will come by itself.
static BOOLEAN WddmSubmitHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, _In_ const BC250_WDDM_OBJECT* Context,
                                  ULONGLONG GpuVa, ULONG Bytes, UINT FenceId, UINT Node)
{
    LARGE_INTEGER due;
    ULONG seq = 0;
    NTSTATUS status;
    KIRQL irql;

    status = GfxSubmitIb(Device, BC250_WDDM_VMID, Context->RootPhysical, GpuVa, Bytes, &seq);
    if (!NT_SUCCESS(status))
    {
        if (InterlockedIncrement(&Wddm->HwRefused) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: ring refused 0x%08X (fence %u, va 0x%llX, %u bytes): completed in software", status,
                     FenceId, GpuVa, Bytes);
        return FALSE;
    }
    due.QuadPart = -10000ll * BC250_WDDM_SUBMIT_TIMEOUT_MS;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    Wddm->HwPending = TRUE;
    Wddm->HwSeq = seq;
    Wddm->HwFence = FenceId;
    Wddm->HwNode = Node;
    if (!Wddm->Stopping) KeSetTimer(&Wddm->SubmitTimer, due, &Wddm->SubmitDpc);
    KeReleaseSpinLock(&Wddm->Lock, irql);
    if (InterlockedIncrement(&Wddm->HwSubmitted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: fence %u on the gfx ring: sequence %u, vmid %u, root 0x%llX, va 0x%llX, %u bytes", FenceId,
                 seq, (ULONG)BC250_WDDM_VMID, Context->RootPhysical, GpuVa, Bytes);
    WddmGpuFence(Device);               // the interrupt may have come and gone before HwPending was set
    return TRUE;
}

// ---- ADR 0008 stage D: node 1's own hardware channel (docs/design/paging-node.md section 5) ---------------------
//
// A parallel channel to stage C's above, not a generalization of it: WddmSubmitPagingHardware runs at
// DISPATCH_LEVEL (Bc250WddmSubmitCommand, exactly, design note section 4), where WddmSubmitHardware runs at
// PASSIVE_LEVEL (SubmitCommandVirtual) and may call GfxSubmitIb's GartLock-taking path; this one calls
// GfxSubmitPaging instead, which takes no lock beyond gfx.c's own Sdma0RingLock. The two channels fail
// independently: a node-1 timeout calls GfxPagingSubmitFail, never GfxSubmitFail, and vice versa.

// Has node 1's in-flight fence arrived? Same shape as WddmGpuFence, called from the same places (the IH DPC,
// the submit itself, the watchdog), at <= DISPATCH_LEVEL.
// One GPU packet at a time preserves the shared temporary mapping window.
// Later OS packets retain their private command storage in FIFO order.
void WddmGpuFencePaging(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    if (!wddm) return;
    for (;;) {
        BC250_PAGING_JOB* retired=NULL;
        BOOLEAN completed=FALSE, failed=FALSE;
        UINT fence=0;
        ULONG seq=0;
        NTSTATUS status;
        KIRQL irql;
        LARGE_INTEGER due;
        KeAcquireSpinLock(&wddm->Lock,&irql);
        if (wddm->Stopping) {
            KeReleaseSpinLock(&wddm->Lock,irql);
            return;
        }
        if (wddm->PagingHwPending && GfxPagingFenceArrived(Device,wddm->PagingHwSeq)) {
            retired=wddm->PagingHead;
            wddm->PagingHwPending=FALSE;
            KeCancelTimer(&wddm->PagingSubmitTimer);
            completed=TRUE;
        } else if (!wddm->PagingHwPending && wddm->PagingHead &&
                   !wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY]) {
            BC250_PAGING_JOB* job=wddm->PagingHead;
            if (!job->ByteCount) {
                retired=job;
                completed=TRUE;
            } else {
                // Lock covers GPU publication and its CPU pending state together.
                // An immediate IH DPC cannot observe a half-published submission.
                status=GfxSubmitPaging(Device,job->Data,job->PrivateBytes,job->Start,
                    job->ByteCount,job->VirtualAddress,&seq);
                if (NT_SUCCESS(status)) {
                    wddm->PagingHwPending=TRUE;
                    wddm->PagingHwSeq=seq;
                    wddm->PagingHwFence=job->Fence;
                    wddm->PagingDeadline=KeQueryInterruptTime()+10000ull*BC250_WDDM_SUBMIT_TIMEOUT_MS;
                    due.QuadPart=-10000ll*BC250_WDDM_SUBMIT_TIMEOUT_MS;
                    KeSetTimer(&wddm->PagingSubmitTimer,due,&wddm->PagingSubmitDpc);
                    InterlockedIncrement(&wddm->PagingHwSubmitted);
                } else {
                    failed=TRUE;
                    fence=job->Fence;
                    // Stop another caller from retrying this head before fail publication.
                    wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY]=TRUE;
                    InterlockedIncrement(&wddm->PagingHwRefused);
                    GuardLog("wddm: queued paging dispatch refused 0x%08X fence %u",status,fence);
                }
            }
        }
        if (completed) {
            fence=retired->Fence;
            wddm->PagingHead=retired->Next;
            if (!wddm->PagingHead) wddm->PagingTail=NULL;
            if (retired->ByteCount) InterlockedIncrement(&wddm->PagingHwCompleted);
            // Publishing completion can let another CPU reuse the OS buffer.
            // Clear borrowed ownership first and never touch that slot again.
            RtlZeroMemory(retired,sizeof(*retired));
            retired=NULL;
            WddmRecordCompletionLocked(wddm,fence,BC250_WDDM_NODE_COPY);
        }
        KeReleaseSpinLock(&wddm->Lock,irql);
        if (failed) WddmFailSubmission(Device,fence,BC250_WDDM_NODE_COPY);
        if (!completed) return; // no polling loop while the GPU is executing
        WddmQueueReport(wddm);
    }
}

static KDEFERRED_ROUTINE WddmPagingSubmitDpcRoutine;
static void WddmPagingSubmitDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    BOOLEAN timedOut = FALSE;
    UINT fence = 0;
    ULONG seq = 0;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    WddmGpuFencePaging(device);         // late is still arrived
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->PagingHwPending && !wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] &&
        KeQueryInterruptTime()>=wddm->PagingDeadline)
    {
        timedOut = TRUE;
        fence = wddm->PagingHwFence;
        wddm->PagingDeferredValid = FALSE;
        seq = wddm->PagingHwSeq;
        wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] = TRUE;
        // Preserve PagingHwPending until a real fence or the OS recovery path.
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // As on node0, do not retire unexecuted paging commands. OS TDR sees the
    // still-pending fence; closing node1 does not invent a successful memory transfer.
    InterlockedIncrement(&wddm->PagingHwTimeouts);
    GfxPagingSubmitFail(device);
    GuardLog("wddm: PAGING HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u remains pending for OS TDR, node 1 ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    // No completion report for a fence that has not arrived.
}

// Accept driver-built work independently of whether the preceding GPU packet
// has retired. Every builder reserves queue storage; submission never allocates.
static BOOLEAN WddmSubmitPagingHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm,
    const void* PrivateData, ULONG PrivateBytes, ULONGLONG Start,
    ULONG ByteCount, BOOLEAN VirtualAddress, UINT FenceId)
{
    BC250_PAGING_JOB* job;
    KIRQL irql;
    if (!ByteCount || PrivateBytes>PAGING_PRIVATE_BUFFER_BYTES) return FALSE;
    job=(BC250_PAGING_JOB*)PagingPrivateQueueSlot((void*)PrivateData,PrivateBytes,
        Start,ByteCount,VirtualAddress);
    if (!job) return FALSE; // malformed or stale legacy records are not built here
    KeAcquireSpinLock(&Wddm->Lock,&irql);
    if (Wddm->Stopping || Wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] || job->Borrowed) {
        KeReleaseSpinLock(&Wddm->Lock,irql);
        return FALSE;
    }
    // All live slot ownership changes are serialized with queue retirement.
    // Replaying a still-owned start cannot overwrite its existing fence/link.
    job->Next=NULL;job->Borrowed=TRUE;
    job->Start=Start;job->ByteCount=ByteCount;job->PrivateBytes=PrivateBytes;
    job->Fence=FenceId;job->VirtualAddress=VirtualAddress;
    job->Data=(const UCHAR*)PrivateData;
    if (Wddm->PagingTail) Wddm->PagingTail->Next=job;
    else Wddm->PagingHead=job;
    Wddm->PagingTail=job;
    InterlockedIncrement(&Wddm->PagingQueueBorrowed);
    KeReleaseSpinLock(&Wddm->Lock,irql);
    WddmGpuFencePaging(Device);
    return TRUE;
}

// The report side, at DISPATCH_LEVEL, with the DDI long returned. Completion first, then preemption: that order
// is what makes DmaPreempted.LastCompletedFenceId the fence dxgkrnl has just been told about, and it means a
// packet is never reported as completed after it has been declared preempted.
static KDEFERRED_ROUTINE WddmReportDpcRoutine;
static void WddmReportDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;
    LONG fence;
    KIRQL reportIrql;
    UINT node;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    if (WddmStopping(wddm)) return;
    // The same KDPC may be requeued from another processor while this invocation
    // reports to dxgkrnl. Keep publication and preemption ordered across invocations.
    KeAcquireSpinLock(&wddm->Lock, &reportIrql);
    if (wddm->ReportActive)
    {
        wddm->ReportAgain = TRUE;
        KeReleaseSpinLock(&wddm->Lock, reportIrql);
        return;
    }
    wddm->ReportActive = TRUE;
    KeReleaseSpinLock(&wddm->Lock, reportIrql);

    // ADR 0008 stage D: both nodes' pending completion/preemption are checked, not only node 0's - the array
    // slot a gate-closed device never sets stays 0, so this loop reports nothing new for node 1 until the gate
    // opens and something actually submits to it (design note section 5).
    for (node = 0; node < BC250_WDDM_NODE_COUNT_MAX; node++)
    {
        KIRQL irql;
        BOOLEAN complete, preempt;
        UINT preemptFence = 0, lastFence = 0;

        KeAcquireSpinLock(&wddm->Lock, &irql);
        complete = wddm->CompletionPending[node] != 0;
        fence = wddm->SubmittedFence[node];
        wddm->CompletionPending[node] = 0;
        KeReleaseSpinLock(&wddm->Lock, irql);
        if (complete)
        {
            RtlZeroMemory(&data, sizeof(data));
            data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            data.DmaCompleted.SubmissionFenceId = (UINT)fence;
            data.DmaCompleted.NodeOrdinal = node;
            data.DmaCompleted.EngineOrdinal = 0;
            WddmReport(device, &data);
            InterlockedExchange(&wddm->LastCompletedFence, fence);
            InterlockedExchange(&wddm->LastReportedFence[node], fence);
            wddm->LastReportedValid[node] = TRUE;
        }

        // DMA-buffer-boundary preemption cannot be acknowledged while that buffer is
        // executing or while its completion has yet to reach dxgkrnl. ActiveSubmissions
        // also covers GfxSubmitIb before it installs HwPending. Completion and submit
        // exit requeue this DPC, so no spinning or timer is needed while we defer.
        KeAcquireSpinLock(&wddm->Lock, &irql);
        // SubmitCommandVirtual's invalid-parameter contract: the OS retires a
        // rejected fence after prior work. Update our notion without reporting a
        // successful DMA completion for work that was never submitted.
        if (wddm->RejectedPending[node] && !wddm->RefusalPending[node] &&
            wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&
            !(node == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : wddm->HwPending))
        {
            UINT rejected=wddm->RejectedFence[node];
            if (!wddm->LastReportedValid[node] ||
                (LONG)(rejected-(UINT)wddm->LastReportedFence[node]) > 0)
            {
                wddm->LastReportedFence[node]=(LONG)rejected;
                wddm->LastReportedValid[node]=TRUE;
                wddm->LastCompletedFence=(LONG)rejected;
            }
            wddm->RejectedPending[node]=FALSE;
        }
        preempt = wddm->PreemptionPending[node] != 0 && !wddm->RefusalPending[node] &&
                  wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&
                  !(node == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : wddm->HwPending);
        if (preempt)
        {
            preemptFence = (UINT)wddm->PreemptionFence[node];
            lastFence = (UINT)wddm->LastReportedFence[node];
            wddm->PreemptionPending[node] = 0;
        }
        KeReleaseSpinLock(&wddm->Lock, irql);
        if (preempt)
        {
            RtlZeroMemory(&data, sizeof(data));
            data.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
            data.DmaPreempted.PreemptionFenceId = preemptFence;
            data.DmaPreempted.LastCompletedFenceId = lastFence;
            data.DmaPreempted.NodeOrdinal = node;
            data.DmaPreempted.EngineOrdinal = 0;
            GuardLog("wddm: preemption report fence %u node %u last completed %u at DMA boundary",
                     preemptFence, node, lastFence);
            WddmReport(device, &data);
        }
    }
    KeAcquireSpinLock(&wddm->Lock, &reportIrql);
    wddm->ReportActive = FALSE;
    if (wddm->ReportAgain)
    {
        wddm->ReportAgain = FALSE;
        if (!wddm->Stopping) KeInsertQueueDpc(&wddm->ReportDpc, NULL, NULL);
    }
    KeReleaseSpinLock(&wddm->Lock, reportIrql);
}

// Sample one stable programming generation around the hardware pending test.
// A writer starting during the sample invalidates it; retry at the next vblank,
// never wait here (the writer may be the interrupt that preempted this DPC).
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

// ---- the software VSync ------------------------------------------------------------------------------------------

static KDEFERRED_ROUTINE WddmVSyncDpcRoutine;
static void WddmVSyncDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    if (WddmStopping(wddm)) return;

    // The phase is kept whether or not anyone is listening, so that GetScanLine answers the same way either way.
    wddm->VSyncLast = KeQueryPerformanceCounter(&wddm->VSyncFrequency);
    InterlockedIncrement(&wddm->VSyncTicks);
    if (!wddm->VSyncEnabled) return;            // ControlInterrupt has not asked for CRTC_VSYNC

    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    data.CrtcVsync.VidPnTargetId = wddm->VSyncTargetId;
    if (!WddmReadCompletedPrimary(device, wddm, &data.CrtcVsync.PhysicalAddress)) return;
    data.CrtcVsync.PhysicalAdapterMask = 0;     // not in a link, so Flags.ValidPhysicalAdapterMask stays 0 too
    InterlockedIncrement(&wddm->VSyncReports);
    WddmReport(device, &data);
}

// On while a source is visible, off otherwise. No hardware is touched either way with Device->VidPnFlipEnabled
// closed - the software timer below is byte for byte 0.7.23's. Open, the "source" the flip needs is the DCN
// hardware interrupt instead (ADR 0011 point 3 step 3): the branch just below does the same job the timer code
// does further down, on OTG0_OTG_GLOBAL_SYNC_STATUS in place of a KTIMER.
//
// The decision and the act are one critical section, and that is the whole point of this function. KeSetTimerEx
// and KeCancelTimer are both legal at DISPATCH_LEVEL, which is where this lock puts us, so there is no reason to
// split them - and splitting them was wrong: SetVidPnSourceVisibility(FALSE) racing the arm that every
// SetVidPnSourceAddress does could interleave so that the cancel landed last while VSyncArmed still read TRUE.
// Nothing would ever re-arm after that, no flip would ever be retired again, and nothing would say so. The same
// argument holds for DcnVsyncEnable's register write, which is why it is inside the lock too: MmioDcnRead/
// MmioDcnWrite (mmio.c) take no lock of their own and are legal at DISPATCH_LEVEL, so nesting them under this
// one costs nothing and closes the same race the timer comment describes.
static void WddmVSyncArm(_Inout_ BC250_DEVICE* Device, BOOLEAN On)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    LARGE_INTEGER due;
    BOOLEAN changed = FALSE;
    KIRQL irql;

    if (wddm == NULL) return;

    if (Device->VidPnFlipEnabled)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        if (wddm->Stopping && On) { KeReleaseSpinLock(&wddm->Lock, irql); return; }
        if (On != (Device->DcnVsyncArmed != 0))
        {
            InterlockedExchange(&Device->DcnVsyncArmed, On ? 1 : 0);
            (void)DcnVsyncEnable(Device, On);
            changed = TRUE;
        }
        KeReleaseSpinLock(&wddm->Lock, irql);
        if (changed)
            GuardLog("wddm: hardware vsync %s (OTG0 VUPDATE_NO_LOCK, EnableVidPnFlip)", On ? "on" : "off");
        return;
    }

    due.QuadPart = -((LONGLONG)BC250_WDDM_VSYNC_MS * 10000);

    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->Stopping && On)
    {
        KeReleaseSpinLock(&wddm->Lock, irql);   // the stop does the disarm; nothing may arm behind it
        return;
    }
    if (On != wddm->VSyncArmed)
    {
        wddm->VSyncArmed = On;
        changed = TRUE;
        if (On) KeSetTimerEx(&wddm->VSyncTimer, due, BC250_WDDM_VSYNC_MS, &wddm->VSyncDpc);
        else KeCancelTimer(&wddm->VSyncTimer);
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!changed) return;                       // logging stays outside the lock

    if (On)
        GuardLog("wddm: software vsync on, %u ms period, target %u", (ULONG)BC250_WDDM_VSYNC_MS,
                 (ULONG)wddm->VSyncTargetId);
    else
        GuardLog("wddm: software vsync off after %ld ticks", wddm->VSyncTicks);
}

// display.c's SetVidPnSourceVisibility calls this; it is the earliest point at which a flip can be queued.
void WddmSourceVisibility(_Inout_ BC250_DEVICE* Device, BOOLEAN Visible)
{
    WddmVSyncArm(Device, Visible);
}

// ---- ADR 0011 point 3 step 3: the hardware vsync's own report ------------------------------------------------
//
// Called from pnp.c's Bc250DpcRoutine on every DPC, the same unconditional shape as WddmGpuFence/
// WddmGpuFencePaging above: a no-op unless Device->DcnVsyncAcked says the ISR (dcn.c's DcnVsyncInterrupt) found
// at least one real VUPDATE_NO_LOCK event since the last time this ran. <= DISPATCH_LEVEL.
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
        /* negative control: accept changing fallback generation */
        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;
    }
    data.CrtcVsync.PhysicalAdapterMask = 0;
    InterlockedIncrement(&wddm->VSyncReports);
    WddmReport(Device, &data);
}

// ---- the summary -------------------------------------------------------------------------------------------------

// Everything stage A counted, written into the log ring as ordinary lines so that bc250kmd_cli log carries it off
// the headless machine in one go. Called at the stop, and on demand through BC250_ESCAPE_LOG_SUMMARY - a run that
// ends in a frozen desktop never reaches a stop, and the counters are exactly what the experiment is run for.
//
// Read without the lock: every counter here is interlocked or write-once, the numbers are evidence rather than
// control flow, and a summary that took the lock could not be asked for from a DPC-level path later.
static void WddmSummaryOf(_In_ BC250_WDDM* Wddm)
{
    ULONG i;

    GuardLog("wddm summary: ---- DDI calls: name, count, first log line ----");
    for (i = 0; i < WddmDdiCount; i++)
        if (Wddm->Calls[i] != 0)
            GuardLog("wddm summary: %-30s %6ld  first at %ld", g_DdiNames[i], Wddm->Calls[i],
                     Wddm->FirstSequence[i]);

    GuardLog("wddm summary: ---- QueryAdapterInfo types: type, count, first log line ----");
    for (i = 0; i < BC250_WDDM_KINDS; i++)
        if (Wddm->AdapterInfo[i].Value != 0)
            GuardLog("wddm summary: adapter info type %3ld  %6ld  first at %ld", Wddm->AdapterInfo[i].Value - 1,
                     Wddm->AdapterInfo[i].Count, Wddm->AdapterInfo[i].FirstSequence);
    if (Wddm->AdapterInfoOverflow != 0)
        GuardLog("wddm summary: %ld adapter info calls with types past the %u slots", Wddm->AdapterInfoOverflow,
                 (ULONG)BC250_WDDM_KINDS);

    GuardLog("wddm summary: capture plans reserved %lld heap %lld",
             InterlockedCompareExchange64(&Wddm->CaptureReservedTotal,0,0),
             InterlockedCompareExchange64(&Wddm->CaptureHeapTotal,0,0));

    GuardLog("wddm summary: capture context peaks plans %lld reserved-bytes %lld",
             InterlockedCompareExchange64(&Wddm->CaptureContextPeakPlans,0,0),
             InterlockedCompareExchange64(&Wddm->CaptureContextPeakReservedBytes,0,0));

    GuardLog("wddm summary: ---- BuildPagingBuffer operations: operation, count, first log line ----");
    for (i = 0; i < BC250_WDDM_KINDS; i++)
        if (Wddm->PagingOps[i].Value != 0)
            GuardLog("wddm summary: paging operation %3ld  %6ld  first at %ld", Wddm->PagingOps[i].Value - 1,
                     Wddm->PagingOps[i].Count, Wddm->PagingOps[i].FirstSequence);
    if (Wddm->PagingOpsOverflow != 0)
        GuardLog("wddm summary: %ld paging buffer calls with operations past the %u slots", Wddm->PagingOpsOverflow,
                 (ULONG)BC250_WDDM_KINDS);

    // Short on purpose: nine numbers have to fit into BC250_LOG_TEXT with room to grow.
    GuardLog("wddm summary: objects created/destroyed: dev %ld/%ld ctx %ld/%ld proc %ld/%ld alloc %ld/%ld, %ld alive",
             Wddm->Calls[WddmDdiCreateDevice], Wddm->Calls[WddmDdiDestroyDevice],
             Wddm->Calls[WddmDdiCreateContext], Wddm->Calls[WddmDdiDestroyContext],
             Wddm->Calls[WddmDdiCreateProcess], Wddm->Calls[WddmDdiDestroyProcess],
             Wddm->Calls[WddmDdiCreateAllocation], Wddm->Calls[WddmDdiDestroyAllocation], Wddm->ObjectCount);
    GuardLog("wddm summary: allocations opened/closed: %ld/%ld calls", Wddm->Calls[WddmDdiOpenAllocation], Wddm->Calls[WddmDdiCloseAllocation]);
    GuardLog("wddm summary: submissions %ld physical + %ld virtual, %ld preemptions, last completed fence %ld",
             Wddm->Calls[WddmDdiSubmitCommand], Wddm->Calls[WddmDdiSubmitCommandVirtual],
             Wddm->Calls[WddmDdiPreemptCommand], Wddm->LastCompletedFence);
    GuardLog("wddm summary: node 0 hardware: %ld submitted, %ld completed, %ld timeouts, %ld refused",
             Wddm->HwSubmitted, Wddm->HwCompleted, Wddm->HwTimeouts, Wddm->HwRefused);
    GuardLog("wddm summary: umd: %ld allocs (%ld refused), %ld contexts, %ld submits on the ring, %ld not run",
             Wddm->UmdAllocs, Wddm->UmdAllocRefused, Wddm->UmdContexts, Wddm->UmdSubmitHw, Wddm->UmdSubmitSoft);
    // ADR 0008 stage D (docs/design/paging-node.md section 7): node 1 exists in this line whether or not the
    // gate is open - every counter stays 0 with it closed, same as every other stage-behind-a-gate counter here.
    GuardLog("wddm profile: umd calls %ld, elapsed ticks %lld, QPC frequency %lld",
             Wddm->UmdProfileCalls, Wddm->UmdSubmitTicks, Wddm->UmdProfileFrequency.QuadPart);
    GuardLog("wddm profile: probe enabled %u, calls %ld, elapsed ticks %lld (included in umd)",
             Wddm->TraceUmdProbes, Wddm->UmdProbeCalls, Wddm->UmdProbeTicks);
    GuardLog("wddm summary: node 1 (paging, %s): %ld hardware submitted, %ld completed, %ld timeouts, %ld refused",
             Wddm->NodeCount > BC250_WDDM_NODE_COPY ? "open" : "closed", Wddm->PagingHwSubmitted,
             Wddm->PagingHwCompleted, Wddm->PagingHwTimeouts, Wddm->PagingHwRefused);
    GuardLog("wddm: paging OS-private queue admissions=%ld (no submit allocation)",Wddm->PagingQueueBorrowed);
    // Which node SubmitCommandVirtual was called on, and how the node-1 ones were resolved against the paging
    // buffers. Run 006 had to infer the node-1 count by subtracting presents from submissions (facts M110);
    // no run after it does.
    GuardLog("wddm summary: SubmitCommandVirtual by node: 0: %ld, 1: %ld, %ld private-buffer refusals",
             Wddm->PagingVirtualSubmits[0],Wddm->PagingVirtualSubmits[1],Wddm->PagingVirtualUnmapped);
    GuardLog("wddm summary: paging DMA VA nonzero %lld zero %lld, mapping observations %ld",
             Wddm->PagingDmaVaBuilds,Wddm->PagingDmaZeroVaBuilds,Wddm->PagingDmaVaObserved);
    GuardLog("wddm summary: native DMA transfers %lld fills %lld bytes %lld",
             Wddm->PagingNativeTransfers,Wddm->PagingNativeFills,Wddm->PagingNativeBytes);
    GuardLog("wddm summary: native DMA exact disjoint checks %lld",Wddm->PagingDmaGapProofs);
    GuardLog("wddm summary: DMA mapping matches %ld/%ld last VA0x%llX root0x%llX PA0x%llX CPU_PA0x%llX",
             Wddm->PagingDmaVaMatched,Wddm->PagingDmaVaObserved,Wddm->PagingDmaLastVa,
             Wddm->PagingDmaLastRoot,Wddm->PagingDmaLastPa,Wddm->PagingDmaLastCpu);
    GuardLog("wddm summary: BuildPagingBuffer: %ld transfers, %ld fills, %lld bytes, %ld insufficient-buffer",
             Wddm->PagingTransfersBuilt, Wddm->PagingFillsBuilt, Wddm->PagingBytesMoved, Wddm->PagingInsufficientBuffer);
    GuardLog("wddm summary: aperture map batches %ld, unmap batches %ld",Wddm->PagingMapsBuilt,Wddm->PagingUnmapsBuilt);
    GuardLog("wddm summary: paging TLB invalidations %ld, PTE update batches %ld",
             Wddm->PagingFlushesBuilt,Wddm->PagingUpdatesBuilt);
    GuardLog("wddm summary: paging unsupported (not ready/no root/no translation/system memory/not contiguous) %ld/%ld/%ld/%ld/%ld",
             Wddm->PagingUnsupported[BC250PagingNotReady], Wddm->PagingUnsupported[BC250PagingNoRoot],
             Wddm->PagingUnsupported[BC250PagingNoTranslation], Wddm->PagingUnsupported[BC250PagingSystemMemory],
             Wddm->PagingUnsupported[BC250PagingNotContiguous]);
    GuardLog("wddm summary: presents %ld, flips %ld of %ld address calls (%ld arrived above DISPATCH_LEVEL)",
             Wddm->Calls[WddmDdiPresent], Wddm->Flips, Wddm->Calls[WddmDdiSetVidPnSourceAddress],
             Wddm->FlipsAboveDispatch);
    GuardLog("wddm summary: blit gate %s, %ld blits, %ld skips, %ld sources translated contiguous", Wddm->BlitGate ? "open" : "closed",
             Wddm->Blits, Wddm->BlitSkips, Wddm->BlitTranslations);
    // 2026-09-22 (ADR 0011 consequences, facts M100): where the copy actually landed. BlitsToFlip should be
    // every blit once a flip is live and stays mappable; BlitsMapFailed says how many of BlitsToFirmware are a
    // fallback rather than the ordinary gate-closed/pre-flip case, and the two scanout-remap counters say how
    // much of that mapping work DcnScanoutMapping actually did (once a flip, not once a present - M97).
    GuardLog("wddm summary: blit destination: %ld to the flipped surface, %ld to the POST framebuffer (%ld a "
             "failed-mapping fallback), %ld scanout remaps (%ld failed), last blit %ld rows, %ld seeds",
             Wddm->BlitsToFlip, Wddm->BlitsToFirmware, Wddm->BlitsMapFailed,
             Wddm->Device->DcnScanoutRemaps, Wddm->Device->DcnScanoutMapFailed, Wddm->BlitRowsLast, Wddm->BlitSeeds);
    GuardLog("wddm summary: widest blit %ld rows", Wddm->BlitRowsMax);
    GuardLog("wddm summary: last blit reading %ld, %ldx%ld pitch %ld, %ld rectangles, source physical 0x%llX",
             Wddm->BlitReadingLast, Wddm->BlitWidthLast, Wddm->BlitHeightLast, Wddm->BlitPitchLast,
             Wddm->BlitSubRectsLast, (ULONGLONG)Wddm->BlitSourceLast);
    GuardLog("wddm summary: vsync %s, %ld ticks, %ld reported to dxgkrnl",
             Wddm->VSyncEnabled ? "enabled" : "not enabled by ControlInterrupt", Wddm->VSyncTicks,
             Wddm->VSyncReports);
    // ADR 0011 point 3 step 3 (0.7.24): VidPn flip gate. Every counter here stays 0 with EnableVidPnFlip
    // closed, same convention as the node 1 line above.
    GuardLog("wddm summary: dcn lock acknowledgement timeouts %ld",
             InterlockedCompareExchange(&Wddm->Device->DcnLockTimeouts, 0, 0));
    GuardLog("wddm summary: vidpn flip %s: %ld hardware flips, %ld refused, %ld hardware vsyncs armed %ld "
             "acked %ld refused %ld deferred (flip still pending)",
             Wddm->Device->VidPnFlipEnabled ? "open" : "closed", Wddm->Device->DcnFlipsHardware, Wddm->Device->DcnFlipRefused,
             Wddm->Device->DcnVsyncArmed, Wddm->Device->DcnVsyncTicks, Wddm->Device->DcnVsyncRefused, Wddm->Device->DcnVsyncDeferred);
    if (Wddm->ReportFailures != 0)
        GuardLog("wddm summary: *** %ld reports refused by DxgkCbSynchronizeExecution ***", Wddm->ReportFailures);

    // The one result that changes what stage A means: the scheduler declaring this adapter hung. Loud, and
    // repeated here even though every call was logged where it happened, because the summary may be all anybody
    // reads.
    if (Wddm->Calls[WddmDdiResetFromTimeout] != 0 || Wddm->Calls[WddmDdiRestartFromTimeout] != 0 ||
        Wddm->Calls[WddmDdiResetEngine] != 0)
        GuardLog("wddm summary: *** TDR: ResetEngine %ld, ResetFromTimeout %ld, RestartFromTimeout %ld - the "
                 "scheduler timed this adapter out ***", Wddm->Calls[WddmDdiResetEngine],
                 Wddm->Calls[WddmDdiResetFromTimeout], Wddm->Calls[WddmDdiRestartFromTimeout]);
    else
        GuardLog("wddm summary: no TDR (ResetEngine, ResetFromTimeout and RestartFromTimeout were never called)");
    // Counted since the driver image was loaded, not since this start: the counter cannot live in the block.
    GuardLog("wddm summary: CollectDbgInfo called %ld time(s) since the driver was loaded", g_CollectDbgInfoCalls);
}

// Reached from DxgkDdiEscape, and holds the pointer across some 40 GuardLog calls while WddmStop frees the block.
// No lock covers that, and none inside the block could (it would be freed with it). What covers it is dxgkrnl:
// DxgkDdiStopDevice is a Level Three call - "only a single thread (the calling thread) is within the kernel-mode
// driver", the one documented exception being QueryAdapterInfo against SetPowerState and QueryChildRelations - so
// no escape runs while WddmStop does (LEARN display/threading-and-synchronization-third-level). The escape's own
// side of it is enforced, not assumed: display.c refuses BC250_ESCAPE_LOG_SUMMARY unless the caller asked for
// HardwareAccess without NoAdapterSynchronization, which makes this a Level Two call (-second-level).
void WddmSummary(_In_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;

    if (wddm == NULL)
    {
        GuardLog("wddm summary: the full table is not running (EnableFullWddm closed, or no pool at the start)");
        return;
    }
    WddmSummaryOf(wddm);
    VidMmSummary();
}

// BC250_ESCAPE_GET_INFO (2026-09-22, the overlay's live panel): the two counters worth a glance from outside
// stage A, without handing the whole BC250_WDDM struct across translation units the way WddmSummary's caller
// never needs to. Both 0 when the gate is closed (Device->Wddm is NULL then), same as WddmSummary answers either way.
void WddmCounters(_In_ const BC250_DEVICE* Device, _Out_ LONG* Blits, _Out_ LONG* Flips)
{
    const BC250_WDDM* wddm = (const BC250_WDDM*)Device->Wddm;

    *Blits = (wddm != NULL) ? wddm->Blits : 0;
    *Flips = (wddm != NULL) ? wddm->Flips : 0;
}

// ---- start, stop, DPC ------------------------------------------------------------------------------------------

static BOOLEAN WddmMemoryLayout(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset, _Out_ ULONGLONG* Length,
                                _Out_ ULONGLONG* TableOffset, _Out_ ULONGLONG* TableLength);

NTSTATUS WddmStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm;
    BC250_START_REPORT* startup=NULL;
    BOOLEAN vidmmPrepared=FALSE;
    NTSTATUS status;
    ULONGLONG segmentOffset, segmentLength, tableOffset, tableLength;

    Device->FullWddm = g_FullWddm;
    Device->Wddm = NULL;
    RtlZeroMemory(&Device->WddmAperture,sizeof(Device->WddmAperture));
    if (!g_FullWddm) return STATUS_SUCCESS;                            // gate closed: this file does nothing at all

    // Required state must exist before the adapter starts accepting paging work.
    wddm = (BC250_WDDM*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*wddm), BC250_WDDM_TAG);
    if (wddm == NULL) { GuardLog("wddm: no pool for the adapter state"); return STATUS_INSUFFICIENT_RESOURCES; }
    startup=(BC250_START_REPORT*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*startup),BC250_WDDM_TAG);
    if (!startup) { ExFreePoolWithTag(wddm,BC250_WDDM_TAG); return STATUS_INSUFFICIENT_RESOURCES; }
    wddm->Device = Device;
    wddm->TraceUmdProbes = (GuardReadSetting(L"TraceUmdProbes", 0) == 1);
    (void)KeQueryPerformanceCounter(&wddm->UmdProfileFrequency);
    ExInitializePushLock(&wddm->PagingBuildLock);
    KeInitializeSpinLock(&wddm->Lock);
    InitializeListHead(&wddm->Objects);
    KeInitializeDpc(&wddm->ReportDpc, WddmReportDpcRoutine, Device);
    KeInitializeDpc(&wddm->VSyncDpc, WddmVSyncDpcRoutine, Device);
    wddm->BlitGate = (GuardReadSetting(L"EnablePresentBlit", 0) == 1);   // E20: the diagnostic CPU blit (ADR 0011)
    KeInitializeDpc(&wddm->SubmitDpc, WddmSubmitDpcRoutine, Device);
    KeInitializeTimer(&wddm->SubmitTimer);
    // ADR 0008 stage D (docs/design/paging-node.md). Read once, like EnableGpuSubmit's own read in gfx.c: node 1's
    // existence for this whole device start is decided here. gfx.c's own gate (GfxStart) decides separately
    // whether GfxSubmitPaging itself may ever run; this one decides whether the table admits the node at all.
    wddm->NodeCount = (GuardReadSetting(L"EnablePagingNode", 0) == 1) ? BC250_WDDM_NODE_COPY + 1u : BC250_WDDM_NODE_COUNT;
    KeInitializeDpc(&wddm->PagingSubmitDpc, WddmPagingSubmitDpcRoutine, Device);
    KeInitializeTimer(&wddm->PagingSubmitTimer);
    KeInitializeTimerEx(&wddm->VSyncTimer, SynchronizationTimer);
    // The one target this driver has. Every VidPN target is a child's UID and Bc250QueryChildRelations reports
    // exactly one child, so this is the only id a CRTC_VSYNC report or a GetScanLine question can carry. 0.7.1
    // took Post.TargetId here, which is whatever the previous owner of the display called its target, and
    // D3DDDI_ID_UNINITIALIZED after a driver reload (E16 step 1: "target 4294967295"): every VSync report would
    // have named a target that does not exist, no flip would have retired, and the run would have measured that.
    wddm->VSyncTargetId = BC250_CHILD_UID;
    if (Device->Post.TargetId != BC250_CHILD_UID)
        GuardLog("wddm: POST display target %u is not ours, vsync reports name child 0x%X",
                 (ULONG)Device->Post.TargetId, (ULONG)BC250_CHILD_UID);
    wddm->VSyncLast = KeQueryPerformanceCounter(&wddm->VSyncFrequency);
    // Production full WDDM requires its GPU VA/paging resources at admission.
    if (GuardReadSetting(L"EnableGpuVa",0)!=1 ||
        !WddmMemoryLayout(Device,&segmentOffset,&segmentLength,&tableOffset,&tableLength)) {
        status=STATUS_DEVICE_NOT_READY;
        goto Failed;
    }
    status=GartCaptureAperture(Device,&Device->WddmAperture);
    if (!NT_SUCCESS(status)) goto Failed;
    vidmmPrepared=TRUE; // VidMmStartLayout initializes its lock even on failure.
    status=VidMmStartLayout(Device,segmentOffset,segmentLength,BC250_WDDM_SEGMENT_VRAM,
                           tableOffset,tableLength,BC250_WDDM_SEGMENT_TABLES);
    if (!NT_SUCCESS(status)) goto Failed;
    status=GpuStartupInitialize(Device,startup);
    if (!NT_SUCCESS(status)) goto Failed;
    ExFreePoolWithTag(startup,BC250_WDDM_TAG);
    startup=NULL;
    // DPC readers may begin only after the complete CPU and GPU setup.
    InterlockedExchangePointer(&Device->Wddm,wddm);
    GuardLog("wddm: GPU initialized before adapter admission");
    // dxgkrnl fills DXGKRNL_INTERFACE to the size the declared version defines, and pnp.c copies only that much,
    // so the four callbacks every report in this file depends on are only there because the full table declares
    // WDDM 2.0. Say so once at the start rather than discover it from a silent no-op in the lab.
    GuardLog("wddm: callbacks sync %u notify %u queuedpc %u notifydpc %u, interface %u of %u bytes",
             Device->Dxgk.DxgkCbSynchronizeExecution != NULL, Device->Dxgk.DxgkCbNotifyInterrupt != NULL,
             Device->Dxgk.DxgkCbQueueDpc != NULL, Device->Dxgk.DxgkCbNotifyDpc != NULL,
             Device->Dxgk.Size, (ULONG)sizeof(Device->Dxgk));
    return STATUS_SUCCESS;
Failed:
    // No WDDM object was published and no OS work was accepted. The coordinator
    // unwinds attempted hardware phases; PnP cleanup handles prepared objects.
    if (vidmmPrepared) VidMmStop();
    RtlZeroMemory(&Device->WddmAperture,sizeof(Device->WddmAperture));
    ExFreePoolWithTag(startup,BC250_WDDM_TAG);
    ExFreePoolWithTag(wddm,BC250_WDDM_TAG);
    return status;
}

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
    while (wddm->PagingHead) {
        BC250_PAGING_JOB* job=wddm->PagingHead;
        wddm->PagingHead=job->Next;
        RtlZeroMemory(job,sizeof(*job));
    }
    wddm->PagingTail=NULL;
    VidMmStop();
    // 2026-09-22 (ADR 0011 consequences): the present path's own destination mapping (dcn.c), torn down here -
    // first in the stop order (docs/design/vidpn-flip.md section 8) - so it is gone before DcnStop's own
    // restore-to-firmware write runs and before MmioStop unmaps BAR5, whatever happens to either of those.
    DcnUnmapScanout(Device);

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

// Called from Bc250DpcRoutine after WddmReport queued it. The scheduler has to hear about the same event a second
// time at DPC level (LEARN nc-d3dkmddi-dxgkcb_notify_interrupt).
void WddmDpc(_Inout_ BC250_DEVICE* Device)
{
    if (Device->Wddm == NULL || Device->Dxgk.DxgkCbNotifyDpc == NULL) return;
    Device->Dxgk.DxgkCbNotifyDpc(Device->Dxgk.DeviceHandle);
}

// ---- the memory segment ----------------------------------------------------------------------------------------

// The one part of VRAM stage A offers VidMm: everything between the firmware's framebuffer at the bottom and the
// reserved tail at the top (bc250kmd.h holds that table; facts M31, M33, M34). Both ends come from what vram.c
// identified at start behind EnableVram, never from a literal here, and no register is read to get them.
//
// FALSE means there is nothing to offer: with EnableVram closed the carve-out's address and size are unknown, and
// stage A then declares zero segments rather than guess. That is a measurement of its own - whether dxgkrnl will
// create an adapter with no memory segment at all - and the stage A run sheet covers both settings.
static BOOLEAN WddmMemoryLayout(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset, _Out_ ULONGLONG* Length,
                                _Out_ ULONGLONG* TableOffset, _Out_ ULONGLONG* TableLength)
{
    ULONGLONG fbOffset,framebufferEnd=0,available,limit,tableBytes;
    *Offset=*Length=*TableOffset=*TableLength=0;
    if (!Device->VramEnabled || Device->VramLength<=BC250_VRAM_TOP_RESERVED) return FALSE;
    limit=Device->VramLength-BC250_VRAM_TOP_RESERVED;
    // A missing BAR0 identity is not proof that the displayed POST buffer lies
    // outside VRAM. Resolve it through either verified view before admitting VidMm.
    if (!VramFramebufferOffset(Device,&fbOffset)) return FALSE;
    {
        ULONGLONG bytes=(ULONGLONG)Device->Post.Pitch*Device->Post.Height;
        if (!bytes || fbOffset>limit || bytes>limit-fbOffset) return FALSE;
        framebufferEnd=fbOffset+bytes;
    }
    if (framebufferEnd>MAXULONGLONG-65535ull) return FALSE;
    framebufferEnd=(framebufferEnd+65535ull)&~65535ull;
    if (framebufferEnd>=limit) return FALSE;
    available=(limit-framebufferEnd)&~65535ull;
    // Capacity policy: reserve 1/32 of usable VRAM, rounded down to64KiB,
    // at least4MiB for the pinned1GiB paging hierarchy plus application tables.
    // This is a driver budget choice, not a hardware limit or performance claim.
    tableBytes=(available/32u)&~65535ull;
    if (tableBytes<4ull*1024*1024) tableBytes=4ull*1024*1024;
    if (tableBytes>=available || available-tableBytes<65536ull) return FALSE;
    *Offset=framebufferEnd;*Length=available-tableBytes;
    *TableOffset=*Offset+*Length;*TableLength=tableBytes;
    return TRUE;
}

// SegmentAddress already includes the advertised MC base. Offset is an
// allocation-relative byte offset (TransferOffset), never another segment base.
static BOOLEAN WddmLocalPagingEndpoint(const BC250_DEVICE* Device, UINT Segment,
    ULONGLONG SegmentAddress, ULONGLONG Offset, ULONGLONG Bytes, BC250_PAGING_ENDPOINT* Endpoint)
{
    ULONGLONG appOffset,appLength,tableOffset,tableLength,base,length,address;
    RtlZeroMemory(Endpoint,sizeof(*Endpoint));
    if (!Bytes) return FALSE;
    if (Segment==BC250_WDDM_SEGMENT_APERTURE) {
        base=Device->WddmAperture.mc;length=Device->WddmAperture.bytes;
        if (!length || SegmentAddress<base || SegmentAddress-base>=length ||
            Offset>MAXULONGLONG-SegmentAddress) return FALSE;
        address=SegmentAddress+Offset;
        if (address-base>=length || Bytes>length-(address-base)) return FALSE;
        Endpoint->Address=address;Endpoint->Length=length-(address-base);Endpoint->Aperture=TRUE;
        return TRUE;
    }
    if (!WddmMemoryLayout(Device,&appOffset,&appLength,&tableOffset,&tableLength)) return FALSE;
    if (Segment==BC250_WDDM_SEGMENT_VRAM) { base=appOffset;length=appLength; }
    else if (Segment==BC250_WDDM_SEGMENT_TABLES) { base=tableOffset;length=tableLength; }
    else return FALSE; // Unknown segment is not a local-memory endpoint.
    if (base>MAXULONGLONG-Device->VramMcBase) return FALSE;
    base+=Device->VramMcBase;
    if (SegmentAddress<base || SegmentAddress-base>=length ||
        Offset>MAXULONGLONG-SegmentAddress) return FALSE;
    address=SegmentAddress+Offset;
    if (address-base>=length || Bytes>length-(address-base) || Bytes>MAXULONGLONG-address) return FALSE;
    Endpoint->Address=address;Endpoint->Length=length-(address-base);
    return TRUE;
}

// Preparation only: no packets, mappings or ownership changes. Start/End flags
// describe OS sub-transfers; they do not reset MultipassOffset on repeated calls.
BOOLEAN WddmPreparePhysicalTransfer(const BC250_DEVICE* Device,
    const DXGKARG_BUILDPAGINGBUFFER* Build, BC250_PAGING_ENDPOINT* Source,
    BC250_PAGING_ENDPOINT* Destination, ULONGLONG* Progress)
{
    BC250_PAGING_ENDPOINT source,destination;
    ULONGLONG bytes,progress;
    if (!Source || !Destination || !Progress) return FALSE;
    RtlZeroMemory(Source,sizeof(*Source));RtlZeroMemory(Destination,sizeof(*Destination));*Progress=0;
    if (!Device || !Build || Source==Destination || !Build->Transfer.TransferSize ||
        Build->Transfer.Flags.Swizzle || Build->Transfer.Flags.Unswizzle || Build->Transfer.Flags.Reserved)
        return FALSE;
    bytes=Build->Transfer.TransferSize;
    RtlZeroMemory(&source,sizeof(source));RtlZeroMemory(&destination,sizeof(destination));
    if (Build->Transfer.Source.SegmentId==0) {
        source.Mdl=Build->Transfer.Source.pMdl;source.FirstPage=Build->Transfer.MdlOffset;source.Length=bytes;
        if (!source.Mdl) return FALSE;
    } else if (!WddmLocalPagingEndpoint(Device,Build->Transfer.Source.SegmentId,
        (ULONGLONG)Build->Transfer.Source.SegmentAddress.QuadPart,Build->Transfer.TransferOffset,bytes,&source))
        return FALSE;
    if (Build->Transfer.Destination.SegmentId==0) {
        destination.Mdl=Build->Transfer.Destination.pMdl;
        destination.FirstPage=Build->Transfer.MdlOffset;destination.Length=bytes;
        if (!destination.Mdl) return FALSE;
    } else if (!WddmLocalPagingEndpoint(Device,Build->Transfer.Destination.SegmentId,
        (ULONGLONG)Build->Transfer.Destination.SegmentAddress.QuadPart,Build->Transfer.TransferOffset,bytes,&destination))
        return FALSE;
    if (!GfxPagingEndpointValid(Device,&source,bytes) || !GfxPagingEndpointValid(Device,&destination,bytes) ||
        !PagingStreamTokenDecode(FALSE,source.Address,destination.Address,bytes,Build->MultipassOffset,&progress))
        return FALSE;
    // Whole-range logical preflight once per DDI, not for every copy slice.
    if ((source.Aperture && !VidMmApertureRangeValid(source.Address,bytes)) ||
        (destination.Aperture && !VidMmApertureRangeValid(destination.Address,bytes))) return FALSE;
    *Source=source;*Destination=destination;*Progress=progress;
    return TRUE;
}

// Two passes, as documented: the first asks only for the count and no other member may be touched; the second
// fills the array, which is iterated with the stride dxgkrnl gives and never with sizeof.
static NTSTATUS WddmQuerySegment4(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_QUERYSEGMENTOUT4* out = (DXGK_QUERYSEGMENTOUT4*)Query->pOutputData;
    DXGK_SEGMENTDESCRIPTOR4* descriptor;
    ULONGLONG offset, length, tableOffset, tableLength;
    UINT count;

    if (Query->OutputDataSize < sizeof(*out) || out == NULL) return STATUS_BUFFER_TOO_SMALL;
    count = WddmMemoryLayout(Device, &offset, &length, &tableOffset, &tableLength) ? 1u : 0u;

    // Geometry is captured before publishing WDDM state; queries never run setup.
    if (count != 0 && Device->WddmAperture.bytes!=PAGING_APERTURE_BYTES) {
        g_ApertureOffered=FALSE;
        return STATUS_DEVICE_NOT_READY;
    }
    if (count != 0) count = 3;          // application local, aperture, table local
    g_ApertureOffered = (count == 3);
    if (out->NbSegment == 0)
    {
        out->NbSegment = count;
        if (WddmAnswersLogged(Device)) GuardLog("wddm: QUERYSEGMENT4 pass 1: %u segment(s)", count);
        return STATUS_SUCCESS;
    }
    if (out->NbSegment < count || out->pSegmentDescriptor == NULL) return STATUS_INVALID_PARAMETER;
    if (count != 0)
    {
        if (out->SegmentDescriptorStride < sizeof(*descriptor)) return STATUS_INVALID_PARAMETER;
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)out->pSegmentDescriptor;
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->Flags.CpuVisible = 1;               // the carve-out is reachable by physical address (M31)
        descriptor->Flags.LocalBudgetGroup = 1;         // local memory, not an aperture: Aperture and Agp stay 0
        descriptor->Flags.DirectFlip = 1;               // the cap says DirectFlip; this is the segment scanned out
        // PreservedDuringStandby and PreservedDuringHibernate stay 0: whether this memory survives S3 on this
        // board is not known, and claiming it wrongly would hand back corrupt surfaces after a resume.
        descriptor->BaseAddress.QuadPart = (LONGLONG)(Device->VramMcBase + offset);
        descriptor->CpuTranslatedAddress.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)offset;
        descriptor->Size = (SIZE_T)length;
        // CommitLimit is left 0: the header says it applies to aperture segments only, and this is a memory
        // segment. Everything else in the descriptor - the VPR range, the invalid ranges - is zero for the same
        // reason: stage A has none of it.

        // The permanent OS aperture excludes private GTT and temporary paging
        // slots. CPU mapping policy is unchanged and remains an audited gap.
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)out->pSegmentDescriptor + out->SegmentDescriptorStride);
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->Flags.Aperture = 1;
        descriptor->Flags.CacheCoherent = 1;
        descriptor->Flags.CpuVisible = 1;
        descriptor->BaseAddress.QuadPart = (LONGLONG)Device->WddmAperture.mc;
        descriptor->CpuTranslatedAddress.QuadPart = (LONGLONG)0xFFFFFFFE00000000ull;
        descriptor->Size = (SIZE_T)Device->WddmAperture.bytes;
        descriptor->CommitLimit = (SIZE_T)Device->WddmAperture.bytes;

        // Table storage is excluded from every application allocation mask.
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)out->pSegmentDescriptor + 2u*out->SegmentDescriptorStride);
        RtlZeroMemory(descriptor,sizeof(*descriptor));
        descriptor->Flags.CpuVisible=1;
        descriptor->Flags.LocalBudgetGroup=1;
        descriptor->BaseAddress.QuadPart=(LONGLONG)(Device->VramMcBase+tableOffset);
        descriptor->CpuTranslatedAddress.QuadPart=Device->VramPhysical.QuadPart+(LONGLONG)tableOffset;
        descriptor->Size=(SIZE_T)tableLength;

    }
    out->NbSegment = count;
    // DXGK_QUERYSEGMENTOUT: an aperture segment or0. The former system-memory
    // choice produced VA0 paging submissions. Our aperture now has prepared GART
    // backing before OS admission; select it for OS-owned, GPU-addressable buffers.
    // M66 rejected local segment1, not the valid aperture segment2 used here.
    out->PagingBufferSegmentId = count>1 ? BC250_WDDM_SEGMENT_APERTURE : 0;
    out->PagingBufferSize = BC250_WDDM_PAGING_BUFFER_BYTES;
    out->PagingBufferPrivateDataSize = PAGING_PRIVATE_BUFFER_BYTES;
    // The segment table is the centre of two suspects of E16 run 1 and was invisible in the log.
    if (!WddmAnswersLogged(Device)) return STATUS_SUCCESS;
    GuardLog("wddm: QUERYSEGMENT4 pass 2: %u segment(s), stride %u, paging buffer segment %u, %u bytes",
             count, (ULONG)out->SegmentDescriptorStride, out->PagingBufferSegmentId, out->PagingBufferSize);
    if (count != 0)
    {
        // CpuTranslatedAddress shares a union with CpuHostAperture and is the valid arm only while
        // Flags.SupportsCpuHostAperture is 0, which it is here.
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)out->pSegmentDescriptor;
        GuardLog("wddm: segment 1 flags 0x%08X gpu 0x%llX cpu 0x%llX size 0x%llX", descriptor->Flags.Value,
                 (ULONGLONG)descriptor->BaseAddress.QuadPart, (ULONGLONG)descriptor->CpuTranslatedAddress.QuadPart,
                 (ULONGLONG)descriptor->Size);
    }
    return STATUS_SUCCESS;
}

// ---- QueryAdapterInfo ------------------------------------------------------------------------------------------

static NTSTATUS WddmDriverCaps(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_DRIVERCAPS* caps = (DXGK_DRIVERCAPS*)Query->pOutputData;
    const BC250_WDDM* wddm = (const BC250_WDDM*)Device->Wddm;
    // A query before a successful start uses the gate-closed node count.
    // Required WDDM/VidMm resource failures now fail StartDevice.
    UINT nodeCount = (wddm != NULL) ? wddm->NodeCount : BC250_WDDM_NODE_COUNT;

    if (Query->OutputDataSize < sizeof(*caps) || caps == NULL) return STATUS_BUFFER_TOO_SMALL;
    // The caller's buffer, not our sizeof: this driver is compiled at WDDM 2.0 and will run under a dxgkrnl that
    // knows a longer DXGK_DRIVERCAPS. Zeroing all of it means the members we have never heard of read as zero
    // rather than as whatever was on the stack. The same rule applies everywhere below.
    RtlZeroMemory(caps, Query->OutputDataSize);

    caps->WDDMVersion = DXGKDDI_WDDMv2;                 // ADR 0008 point 2: the version that introduced GpuMmu
    caps->HighestAcceptableAddress.QuadPart = -1;       // no addressing limit; too low a value fails the load
    caps->SupportNonVGA = TRUE;
    caps->NumberOfSwizzlingRanges = 0;                  // swizzling range support has been removed from WDDM
    // The caps Microsoft documents as mandatory for a full graphics driver that claims WDDM 1.2 or later ("WDDM 1.2
    // driver enforcement"), and which its own sample drivers all set. They are NOT what refused E16 runs 001 and
    // 002 (that was the missing UMD name, facts M64), but the lab's dxgkrnl does test DirectFlip further down
    // DXGADAPTER::Initialize when its enforcement switch is on, and an answer that differs from every known-good
    // driver in three mandatory members is a poor place to save three lines.
    //  - PerEngineTDR: the three DDIs are in the table; ResetEngine refuses, as the part has no reset (M53).
    //  - SmoothRotation: "must support updating path rotation in UpdateActiveVidPnPresentPath"; ours does, and
    //    the only rotation it offers is identity.
    //  - DirectFlip: a promise about shared primaries that an adapter nobody can render on is never held to.
    caps->SupportSmoothRotation = TRUE;
    caps->SupportPerEngineTDR = TRUE;
    caps->SupportDirectFlip = TRUE;
    caps->SupportSurpriseRemoval = FALSE;
    caps->InterruptMessageNumber = 0;                   // the INF asks for one MSI message, so it is message 0 (M38)

    // Kernel-mode GDI acceleration: every one of these is a "cannot", and they have to be said out loud, because a
    // zeroed DXGK_PRESENTATIONCAPS claims the driver *can* do all of them. Stage A has no engine and its Present
    // writes no DMA, so a blt dxgkrnl believed in would be silently dropped and show as corruption on screen.
    caps->PresentationCaps.NoScreenToScreenBlt = 1;
    caps->PresentationCaps.NoOverlapScreenBlt = 1;
    caps->PresentationCaps.NoSameBitmapBitBlt = 1;
    caps->PresentationCaps.NoSameBitmapOverlappedBitBlt = 1;
    caps->PresentationCaps.NoSameBitmapAlphaBlend = 1;
    caps->PresentationCaps.NoSameBitmapOverlappedAlphaBlend = 1;
    caps->PresentationCaps.NoSameBitmapStretchBlt = 1;
    caps->PresentationCaps.NoSameBitmapOverlappedStretchBlt = 1;
    caps->PresentationCaps.NoSameBitmapTransparentBlt = 1;
    // NoCacheCoherentApertureMemory stays 0 since 0.7.9: the aperture segment is declared CacheCoherent, and the cap
    // would contradict the segment it describes (coherent is also true of this SoC's unified memory).
    // Not a flag: the header says this field must be >= 2. Four-byte pitch alignment, which is what a 32 bits per
    // pixel surface needs anyway. Zero here would be out of contract, and nothing in the research says so.
    caps->PresentationCaps.AlignmentShift = 2;

    // "MultiEngineAware means the driver supports contexts", which a GpuMmu driver must; PreemptionAware needs it
    // set or adapter initialization is halted. A packet scheduler that owns the ring can honestly promise not to
    // start packets it has not written yet, which is what DMA_BUFFER_BOUNDARY claims, and claiming NONE invites
    // the TDR reset loop. LowIrqlPreemptCommand because stage A's PreemptCommand touches nothing.
    caps->SchedulingCaps.MultiEngineAware = 1;
    caps->SchedulingCaps.PreemptionAware = 1;
    caps->SchedulingCaps.LowIrqlPreemptCommand = 1;
    caps->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
    caps->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;

    // GpuMmu, never IoMmu: the two may not be set together, and IoMmu can only address system memory while this
    // part has 8 GB of local memory to reach (facts M31). Unit A has no IOMMU either (facts M47).
    caps->MemoryManagementCaps.VirtualAddressingSupported = 1;
    caps->MemoryManagementCaps.GpuMmuSupported = 1;
    // ADR 0008 stage D (docs/design/paging-node.md section 6): with EnablePagingNode open, node 1 (SDMA0) is the
    // paging node, exactly what ADR 0013 point 2 asks for; closed, paging still rides on node 0, byte-for-byte
    // as before.
    caps->MemoryManagementCaps.PagingNode = (nodeCount > BC250_WDDM_NODE_COPY) ? BC250_WDDM_NODE_COPY : BC250_WDDM_NODE_3D;

    // nodeCount (1 with the gate closed, 2 open) replaces the old compile-time constant here; the six per-node
    // fence fields in BC250_WDDM are arrays now for exactly this reason (design note section 5).
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = nodeCount;

    // Both zero means "use the entire available VA range", which is what we want while nothing reserves any of it.
    caps->InternalGpuVirtualAddressRangeStart = 0;
    caps->InternalGpuVirtualAddressRangeEnd = 0;

    // The flip model of a full miniport: DxgkDdiPresent produces no DMA and SetVidPnSourceAddress does the flip.
    // FlipIndependent: "WDDM 1.3 driver must support independent flip." is a refusal in the lab's dxgkrnl (behind
    // the same enforcement switch as DirectFlip). What it promises is that SetVidPnSourceAddress may be pointed at
    // a surface DWM did not present; ours takes whatever address it is given.
    caps->FlipCaps.FlipOnVSyncMmIo = 1;
    caps->FlipCaps.FlipIndependent = 1;
    caps->MaxQueuedFlipOnVSync = 1;

    // No hardware pointer (MaxPointerWidth/Height stay 0): dxgkrnl draws the cursor into the image it presents,
    // exactly as in the display-only build.

    // What was promised, and into how large a structure: the size says which DXGK_DRIVERCAPS this dxgkrnl thinks
    // it is talking to. A cap that is wrong but accepted leaves no other trace (E16 run 1). Worst case 139 of
    // the 160 bytes of a log line: count before adding a field. Our own sizeof and the paging node are not in
    // it: both are constants of the build (576 at interface 0x5023, which is what dxgkrnl offers; node 0).
    if (WddmAnswersLogged(Device))
        GuardLog("wddm: DRIVERCAPS into %u bytes: wddm %u sched 0x%X mm 0x%X flip 0x%X slots %u "
                 "tdr %u dflip %u rot %u",
             Query->OutputDataSize, (ULONG)caps->WDDMVersion, caps->SchedulingCaps.Value,
             caps->MemoryManagementCaps.Value, caps->FlipCaps.Value,
             caps->MaxAllocationListSlotId, caps->SupportPerEngineTDR ? 1u : 0u, caps->SupportDirectFlip ? 1u : 0u,
             caps->SupportSmoothRotation ? 1u : 0u);
    return STATUS_SUCCESS;
}

static NTSTATUS WddmGpuMmuCaps(_In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_GPUMMUCAPS* caps = (DXGK_GPUMMUCAPS*)Query->pOutputData;

    if (Query->OutputDataSize < sizeof(*caps) || caps == NULL) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(caps, Query->OutputDataSize);
    // Page directories live in a local memory segment, where CPU_VIRTUAL is documented as not allowed.
    // bc250_pte_from_dxgk preserves CacheCoherent as AMDGPU_PTE_SNOOPED for
    // system pages. Advertise this when requesting cached GTT backing store.
    caps->CacheCoherentMemorySupported = 1;
    caps->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    caps->VirtualAddressBitCount = BC250_WDDM_VA_BITS;
    caps->PageTableLevelCount = BC250_WDDM_LEVEL_COUNT;
    caps->LeafPageTableSizeFor64KPagesInBytes = 0;      // stage A declares no 64 KB pages on the segment
    return STATUS_SUCCESS;
}

// Every level of the page table has the same shape here, so the answer does not depend on which end LevelIndex
// counts from - a point the documentation leaves open and stage B has to settle before the shape can differ.
static NTSTATUS WddmPageTableLevelDesc(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    const DXGK_QUERYPAGETABLELEVELDESCIN* in = (const DXGK_QUERYPAGETABLELEVELDESCIN*)Query->pInputData;
    DXGK_PAGE_TABLE_LEVEL_DESC* desc = (DXGK_PAGE_TABLE_LEVEL_DESC*)Query->pOutputData;
    ULONGLONG offset, length, tableOffset, tableLength;

    if (Query->InputDataSize < sizeof(*in) || in == NULL) return STATUS_INVALID_PARAMETER;
    if (Query->OutputDataSize < sizeof(*desc) || desc == NULL) return STATUS_BUFFER_TOO_SMALL;
    if (in->LevelIndex >= BC250_WDDM_LEVEL_COUNT) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(desc, Query->OutputDataSize);
    desc->PageTableIndexBitCount = BC250_WDDM_LEVEL_BITS;
    desc->PageTableSizeInBytes = BC250_WDDM_PAGE_TABLE_BYTES;
    desc->PageTableAlignmentInBytes = 0;                // 0 means the page size of the memory segment
    if (WddmMemoryLayout(Device, &offset, &length, &tableOffset, &tableLength))
    {
        desc->PageTableSegmentId = BC250_WDDM_SEGMENT_TABLES;
        desc->PagingProcessPageTableSegmentId = BC250_WDDM_SEGMENT_TABLES;
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERYADAPTERINFO Bc250WddmQueryAdapterInfo;
static NTSTATUS Bc250WddmQueryAdapterInfo(_In_ const HANDLE hAdapter, _In_ const DXGKARG_QUERYADAPTERINFO* QueryAdapterInfo)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    NTSTATUS status;

    switch (QueryAdapterInfo->Type)
    {
    case DXGKQAITYPE_DRIVERCAPS:
        status = WddmDriverCaps(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_QUERYSEGMENT4:
        status = WddmQuerySegment4(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_GPUMMUCAPS:
        status = WddmGpuMmuCaps(QueryAdapterInfo);
        break;
    case DXGKQAITYPE_PAGETABLELEVELDESC:
        status = WddmPageTableLevelDesc(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_NUMPOWERCOMPONENTS:
        // No runtime power management of our own: zero components, and the power DDIs stay as the display-only
        // driver has them.
        if (QueryAdapterInfo->OutputDataSize < sizeof(UINT)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        *(UINT*)QueryAdapterInfo->pOutputData = 0;
        status = STATUS_SUCCESS;
        break;
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:
        // ASSUMPTION: zero bits is how a driver without a GPU timestamp counter says it has none. Nothing in the
        // documentation names a floor, and FormatHistoryBuffer logs it if dxgkrnl asks anyway.
        if (QueryAdapterInfo->OutputDataSize < sizeof(DXGKARG_HISTORYBUFFERPRECISION)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        ((DXGKARG_HISTORYBUFFERPRECISION*)QueryAdapterInfo->pOutputData)->PrecisionBits = 0;
        status = STATUS_SUCCESS;
        break;
    case DXGKQAITYPE_UMDRIVERPRIVATE:
        // The caps blob, verbatim (umd_caps.c, unit A's measured answers). A short buffer is a refusal the
        // UMD can see; this DDI is allowed to fail. The bytes are not a register read and not a guess made here.
        if (QueryAdapterInfo->pOutputData == NULL) { status = STATUS_INVALID_PARAMETER; break; }
        if (QueryAdapterInfo->OutputDataSize < UMD_CAPS_BYTES) { status = STATUS_BUFFER_TOO_SMALL; break; }
        RtlCopyMemory(QueryAdapterInfo->pOutputData, umd_caps_blob, UMD_CAPS_BYTES);
        status = STATUS_SUCCESS;
        break;
    default:
        // The pre-WDDM2 segment queries, which a WDDM 2 driver must not answer. Seen on the lab and tolerated
        // by its dxgkrnl (E16 run 003): type 15 PHYSICALADAPTERCAPS and type 47 64BITONLYCAPS, for which the
        // WDK has no structure at all. Two refusals in a kept log are expected. STATUS_NOT_SUPPORTED is safe
        // for an unhandled type, and which types are asked for is stage A evidence.
        status = STATUS_NOT_SUPPORTED;
        break;
    }
    // Which types dxgkrnl asks a full miniport for, and how often, is half of what stage A is run for: the
    // counter table keeps all of them, the log below only the first few calls.
    if (wddm != NULL) WddmNoteKind(wddm->AdapterInfo, (ULONG)QueryAdapterInfo->Type, &wddm->AdapterInfoOverflow);
    // The first few calls, and every refusal among the first 256: E16 run 1 ended with dxgkrnl stopping the
    // adapter right after the start, and a question this function refused is the first suspect. PASSIVE_LEVEL DDI.
    if (WddmFirstCalls(wddm, WddmDdiQueryAdapterInfo) ||
        (!NT_SUCCESS(status) && wddm != NULL && wddm->Calls[WddmDdiQueryAdapterInfo] <= 256))
        GuardLog("wddm: QueryAdapterInfo type %u in %u out %u -> 0x%08X", (ULONG)QueryAdapterInfo->Type,
                 QueryAdapterInfo->InputDataSize, QueryAdapterInfo->OutputDataSize, status);
    return status;
}

// ---- nodes ---------------------------------------------------------------------------------------------------

static DXGKDDI_GETNODEMETADATA Bc250WddmGetNodeMetadata;
static NTSTATUS Bc250WddmGetNodeMetadata(_In_ const HANDLE hAdapter, UINT NodeOrdinalAndAdapterIndex,
                                         _Out_ DXGKARG_GETNODEMETADATA* pGetNodeMetadata)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT node = NodeOrdinalAndAdapterIndex & 0xFFFF;
    UINT adapter = NodeOrdinalAndAdapterIndex >> 16;
    UINT nodeCount = (wddm != NULL) ? wddm->NodeCount : BC250_WDDM_NODE_COUNT;

    // "Must return STATUS_INVALID_PARAMETER for NodeOrdinal >= GetNumNodes(), and all calls for in-range ordinals
    // must be successful." GetNumNodes is what DRIVERCAPS reported, so the bound is the same value (ADR 0008
    // stage D: wddm->NodeCount, not the compile-time constant, docs/design/paging-node.md section 6).
    if (node >= nodeCount || adapter != 0) return STATUS_INVALID_PARAMETER;

    // At WDDM 2.0 the member after FriendlyName is a reserved UINT32, not DXGK_NODEMETADATA_FLAGS (that arrives at
    // WDDM 2.2); zeroing covers it either way.
    RtlZeroMemory(pGetNodeMetadata, sizeof(*pGetNodeMetadata));
    if (node == BC250_WDDM_NODE_COPY)
    {
        // ADR 0013: node 1, SDMA0, the paging node. GpuMmuSupported TRUE states a hardware fact (SDMA can be
        // pointed at a VMID) - section 2 of the design note chooses not to use one, which is this build's policy,
        // not something worth mis-declaring to dxgkrnl.
        pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_COPY;
        RtlStringCchCopyW(pGetNodeMetadata->FriendlyName, DXGK_MAX_METADATA_NAME_LENGTH, L"BC-250 SDMA0");
    }
    else
    {
        pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;     // there is no compute engine type: compute rides on 3D
        RtlStringCchCopyW(pGetNodeMetadata->FriendlyName, DXGK_MAX_METADATA_NAME_LENGTH, L"BC-250 GFX");
    }
    pGetNodeMetadata->GpuMmuSupported = TRUE;
    pGetNodeMetadata->IoMmuSupported = FALSE;
    if (WddmFirstCalls(wddm, WddmDdiGetNodeMetadata)) GuardLog("wddm: GetNodeMetadata node %u adapter %u", node, adapter);
    return STATUS_SUCCESS;
}

// ---- devices, contexts, processes -------------------------------------------------------------------------------

static DXGKDDI_CREATEDEVICE Bc250WddmCreateDevice;
static NTSTATUS Bc250WddmCreateDevice(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_CREATEDEVICE* pCreateDevice)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT flags = pCreateDevice->Flags.Value;
    BC250_WDDM_OBJECT* object = WddmNewObject(device, BC250_WDDM_MAGIC_DEVICE);

    if (object == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    pCreateDevice->hDevice = object;
    if (WddmFirstCalls(wddm, WddmDdiCreateDevice)) GuardLog("wddm: CreateDevice flags 0x%08X pasid %u", flags, pCreateDevice->Pasid);
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYDEVICE Bc250WddmDestroyDevice;
static NTSTATUS Bc250WddmDestroyDevice(_In_ const HANDLE hDevice)
{
    BC250_WDDM_OBJECT* object = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls((BC250_WDDM*)object->Device->Wddm, WddmDdiDestroyDevice)) GuardLog("wddm: DestroyDevice");
    WddmFreeObject(object);
    return STATUS_SUCCESS;
}

static DXGKDDI_CREATECONTEXT Bc250WddmCreateContext;
static NTSTATUS Bc250WddmCreateContext(_In_ const HANDLE hDevice, _Inout_ DXGKARG_CREATECONTEXT* pCreateContext)
{
    BC250_WDDM_OBJECT* parent = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);
    BC250_WDDM* parentWddm;
    BC250_WDDM_OBJECT* object;
    UINT nodeCount;
    struct umd_context_view umdView;
    int umdStatus = UMD_BLOB_OK;
    BOOLEAN umd = FALSE;

    RtlZeroMemory(&umdView, sizeof(umdView));
    if (parent == NULL) return STATUS_INVALID_PARAMETER;
    parentWddm = (BC250_WDDM*)parent->Device->Wddm;
    nodeCount = (parentWddm != NULL) ? parentWddm->NodeCount : BC250_WDDM_NODE_COUNT;
    // ADR 0008 stage D: node 1 accepted only once wddm->NodeCount says it exists - one more value the check
    // accepts, the same check otherwise (design note section 6).
    if (pCreateContext->NodeOrdinal != BC250_WDDM_NODE_3D &&
        !(pCreateContext->NodeOrdinal == BC250_WDDM_NODE_COPY && nodeCount > BC250_WDDM_NODE_COPY))
        return STATUS_INVALID_PARAMETER;
    // Empty private data stays the GDI / VidMm path. A blob is a UMD context: GFX, node 0, virtual
    // addressing, and a private-data slot big enough for the submit blob. CreateContext may fail.
    if (pCreateContext->PrivateDriverDataSize != 0)
    {
        umd = TRUE;
        umdStatus = UmdBlobParseContext(pCreateContext->pPrivateDriverData, pCreateContext->PrivateDriverDataSize,
                                        pCreateContext->NodeOrdinal, &umdView);
        if (umdStatus != UMD_BLOB_OK || pCreateContext->Flags.GdiContext || !pCreateContext->Flags.VirtualAddressing)
        {
            GuardLog("wddm: umd context refused, %s, node %u private %u flags 0x%08X",
                     umdStatus != UMD_BLOB_OK ? UmdBlobStatusText(umdStatus) : "gdi or no va",
                     pCreateContext->NodeOrdinal, pCreateContext->PrivateDriverDataSize, pCreateContext->Flags.Value);
            return STATUS_INVALID_PARAMETER;
        }
    }
    object = WddmNewContext(parent->Device,(BOOLEAN)pCreateContext->Flags.SystemContext);
    if (object == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    object->NodeOrdinal = pCreateContext->NodeOrdinal;
    object->SystemContext = (BOOLEAN)pCreateContext->Flags.SystemContext;
    if (umd)
    {
        object->UmdContext = TRUE;
        object->UmdIpType = umdView.ip_type;
    }
    pCreateContext->hContext = object;

    RtlZeroMemory(&pCreateContext->ContextInfo, sizeof(pCreateContext->ContextInfo));
    // Stage A submits nothing, so the DMA buffer it asks for is the smallest that is still a buffer, and there is
    // no allocation or patch-location list to keep: with virtual addressing there is no patching to do at all.
    // One page, the size both reference drivers use (ROSD_COMMAND_BUFFER_SIZE, COS_COMMAND_BUFFER_SIZE).
    pCreateContext->ContextInfo.DmaBufferSize = PAGE_SIZE;
    // The aperture segment. Not 0 ("system memory"): for a GPU-VA context dxgmms2 maps the DMA buffer's VidMm
    // allocation into the context's address space, and with segment set 0 there is no allocation (E16 run 008).
    // Not the local segment either: DMA buffers may only come from aperture segments (facts M66).
    // With EnableVram closed no segment is declared at all, and a set naming one dxgkrnl never heard of would be
    // the same class of mistake; 0 is then the only answer left, and that configuration does not reach the CDD.
    pCreateContext->ContextInfo.DmaBufferSegmentSet =
        g_ApertureOffered ? BC250_WDDM_SEGMENT_SET(BC250_WDDM_SEGMENT_APERTURE) : 0;
    // UMD contexts retain the BC2S contract. Non-UMD contexts carry one bounded
    // software Present packet, built here and consumed only after scheduling.
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize = umd ? UMD_BLOB_SUBMIT_BYTES : sizeof(BC250_PRESENT_PACKET);
    if (!umd && pCreateContext->NodeOrdinal == BC250_WDDM_NODE_COPY)
        pCreateContext->ContextInfo.DmaBufferPrivateDataSize = PAGING_PRIVATE_BUFFER_BYTES;
    // 0.7.14: a GDI context gets the allocation list the header sizes for it (RosKmdContext.cpp does the same).
    // With 0 here every Present of the CDD arrived with NumSrcAllocations = NumDstAllocations = 0 (E16 run 009, E18
    // run 003): dxgkrnl had nowhere to put the two surfaces of a Blt, and a driver that cannot name the source
    // cannot show it. Still no patch-location list: with virtual addressing there is nothing to patch.
    pCreateContext->ContextInfo.AllocationListSize =
        !umd ? DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT : 0;
    object->AllocationListSize = pCreateContext->ContextInfo.AllocationListSize;
    // Review 14: the adapter declares GpuMmu, so no context should come without virtual addressing. If one does, it
    // has a list, no patch list and no Patch DDI behind it - say so here rather than leave it to a 0x113 later.
    if (pCreateContext->Flags.GdiContext && !pCreateContext->Flags.VirtualAddressing)
        GuardLog("wddm: CreateContext: a GDI context WITHOUT virtual addressing (flags 0x%08X) - unexpected", pCreateContext->Flags.Value);
    pCreateContext->ContextInfo.PatchLocationListSize = 0;
    // Unconditional (contract rule R37): the table has no DxgkDdiPatch and no patch-location list, so a context that
    // said "patch me" would meet a 0x113 later; nothing here can patch, whatever the flags say.
    pCreateContext->ContextInfo.Caps.NoPatchingRequired = 1;
    // Matches DRIVERCAPS.MemoryManagementCaps.PagingNode above: node 1 once the gate names it the paging node,
    // node 0 (today's answer) otherwise.
    pCreateContext->ContextInfo.PagingCompanionNodeId = (nodeCount > BC250_WDDM_NODE_COPY) ? BC250_WDDM_NODE_COPY : BC250_WDDM_NODE_3D;

    if (umd && parentWddm != NULL && InterlockedIncrement(&parentWddm->UmdContexts) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: umd context node %u ip %u, private slot %u", pCreateContext->NodeOrdinal,
                 umdView.ip_type, (ULONG)UMD_BLOB_SUBMIT_BYTES);
    if (WddmFirstCalls((BC250_WDDM*)parent->Device->Wddm, WddmDdiCreateContext))
    {
        GuardLog("wddm: CreateContext node %u engine 0x%X flags 0x%08X private %u", pCreateContext->NodeOrdinal,
                 pCreateContext->EngineAffinity, pCreateContext->Flags.Value, pCreateContext->PrivateDriverDataSize);
        // The answer as well as the question: a wrong answer that succeeds looks like a right one otherwise.
        GuardLog("wddm: CreateContext answered dma %u bytes segment set 0x%X, lists %u/%u, caps 0x%08X",
                 pCreateContext->ContextInfo.DmaBufferSize, pCreateContext->ContextInfo.DmaBufferSegmentSet,
                 pCreateContext->ContextInfo.AllocationListSize, pCreateContext->ContextInfo.PatchLocationListSize,
                 pCreateContext->ContextInfo.Caps.Value);
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYCONTEXT Bc250WddmDestroyContext;
static NTSTATUS Bc250WddmDestroyContext(_In_ const HANDLE hContext)
{
    BC250_WDDM_OBJECT* object = WddmObject(hContext, BC250_WDDM_MAGIC_CONTEXT);

    BC250_WDDM* wddm;

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls((BC250_WDDM*)object->Device->Wddm, WddmDdiDestroyContext)) GuardLog("wddm: DestroyContext");
    wddm=(BC250_WDDM*)object->Device->Wddm;
    // Captures are CPU-only but builders may still be reading their arrays.
    // Join that owner before detaching/freeing the context; the lock outlives it.
    if(wddm){KeEnterCriticalRegion();ExAcquirePushLockExclusive(&wddm->PagingBuildLock);}
    WddmFreeObject(object);
    if(wddm){ExReleasePushLockExclusive(&wddm->PagingBuildLock);KeLeaveCriticalRegion();}
    return STATUS_SUCCESS;
}

static DXGKDDI_CREATEPROCESS Bc250WddmCreateProcess;
static NTSTATUS Bc250WddmCreateProcess(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_CREATEPROCESS* pArgs)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_WDDM_OBJECT* object = WddmNewObject(device, BC250_WDDM_MAGIC_PROCESS);

    if (object == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    pArgs->hKmdProcess = object;
    if (WddmFirstCalls(wddm, WddmDdiCreateProcess)) GuardLog("wddm: CreateProcess flags 0x%08X pasids %u", pArgs->Flags.Value, pArgs->NumPasid);
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYPROCESS Bc250WddmDestroyProcess;
static NTSTATUS Bc250WddmDestroyProcess(_In_ const HANDLE hAdapter, _In_ const HANDLE hKmdProcess)
{
    BC250_WDDM_OBJECT* object = WddmObject(hKmdProcess, BC250_WDDM_MAGIC_PROCESS);

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiDestroyProcess)) GuardLog("wddm: DestroyProcess");
    WddmFreeObject(object);
    return STATUS_SUCCESS;
}

// ---- the root page table ----------------------------------------------------------------------------------------

static DXGKDDI_GETROOTPAGETABLESIZE Bc250WddmGetRootPageTableSize;
static SIZE_T Bc250WddmGetRootPageTableSize(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_GETROOTPAGETABLESIZE* pArgs)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT requested = pArgs->NumberOfPte;

    // The root level of the declared layout is one page of BC250_WDDM_PTES_PER_LEVEL entries, whatever was asked
    // for; NumberOfPte is in and out, so the answer is written back.
    pArgs->NumberOfPte = BC250_WDDM_PTES_PER_LEVEL;
    if (WddmFirstCalls(wddm, WddmDdiGetRootPageTableSize))
        GuardLog("wddm: GetRootPageTableSize asked %u, answered %u entries", requested, pArgs->NumberOfPte);
    return (SIZE_T)BC250_WDDM_PAGE_TABLE_BYTES;
}

static DXGKDDI_SETROOTPAGETABLE Bc250WddmSetRootPageTable;
static VOID Bc250WddmSetRootPageTable(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETROOTPAGETABLE* pSetPageTable)
{
    // Stage A has no VM context to program: the address is recorded in the log and nowhere else. It is not a plain
    // physical address - D3DGPU_PHYSICAL_ADDRESS is a segment id and an offset into that segment.
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiSetRootPageTable))
        GuardLog("wddm: SetRootPageTable segment %u offset 0x%llX, %u entries", pSetPageTable->Address.SegmentId,
                 pSetPageTable->Address.SegmentOffset, pSetPageTable->NumEntries);
    VidMmSetRootPageTable(pSetPageTable);
    // Stage C: the context remembers where its page tables start; gfx.c points the VMID there before its packet.
    {
        BC250_WDDM_OBJECT* context = WddmObject(pSetPageTable->hContext, BC250_WDDM_MAGIC_CONTEXT);
        ULONGLONG physical = 0;

        if (context != NULL) context->RootPhysical = VidMmRootPhysical(&pSetPageTable->Address, &physical) ? physical : 0;
    }
}

// ---- allocations ------------------------------------------------------------------------------------------------

static DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA Bc250WddmGetStandardAllocationDriverData;
static NTSTATUS Bc250WddmGetStandardAllocationDriverData(_In_ const HANDLE hAdapter,
                                                         _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA* pData)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_WDDM_ALLOCATION_PRIVATE private;

    RtlZeroMemory(&private, sizeof(private));
    private.Magic = BC250_WDDM_ALLOCATION_PRIVATE_MAGIC;
    private.Version = 1;

    switch (pData->StandardAllocationType)
    {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        if (pData->pCreateSharedPrimarySurfaceData == NULL) return STATUS_INVALID_PARAMETER;
        private.Width = pData->pCreateSharedPrimarySurfaceData->Width;
        private.Height = pData->pCreateSharedPrimarySurfaceData->Height;
        private.Format = (ULONG)pData->pCreateSharedPrimarySurfaceData->Format;
        break;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        if (pData->pCreateShadowSurfaceData == NULL) return STATUS_INVALID_PARAMETER;
        private.Width = pData->pCreateShadowSurfaceData->Width;
        private.Height = pData->pCreateShadowSurfaceData->Height;
        private.Format = (ULONG)pData->pCreateShadowSurfaceData->Format;
        break;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        if (pData->pCreateStagingSurfaceData == NULL) return STATUS_INVALID_PARAMETER;
        private.Width = pData->pCreateStagingSurfaceData->Width;
        private.Height = pData->pCreateStagingSurfaceData->Height;
        private.Format = (ULONG)D3DDDIFMT_A8R8G8B8;
        break;
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE:
        if (pData->pCreateGdiSurfaceData == NULL) return STATUS_INVALID_PARAMETER;
        private.Width = pData->pCreateGdiSurfaceData->Width;
        private.Height = pData->pCreateGdiSurfaceData->Height;
        private.Format = (ULONG)pData->pCreateGdiSurfaceData->Format;
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    // Scanout rows are aligned to 256 bytes (64 32-bit pixels). Other CPU
    // surfaces retain their linear pitch. Extent includes every padded row.
    if (!private.Width || private.Width>MAXULONG/4) return STATUS_INVALID_PARAMETER;
    private.Pitch=pData->StandardAllocationType==D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE ?
        DcnPrimaryPitch(private.Width):private.Width*4;
    if (!DcnSurfaceBytes(private.Width,private.Height,private.Pitch,&private.Size)) return STATUS_INVALID_PARAMETER;
    // These are output fields, not just copies in our private LB7A blob.
    // E26 ETW rejected shadow/staging creation when the public pitch was zero.
    if (pData->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE)
        pData->pCreateShadowSurfaceData->Pitch = private.Pitch;
    else if (pData->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE)
        pData->pCreateStagingSurfaceData->Pitch = private.Pitch;
    else if (pData->StandardAllocationType == D3DKMDT_STANDARDALLOCATION_GDISURFACE)
        pData->pCreateGdiSurfaceData->Pitch = private.Pitch;

    // Two passes: a NULL buffer asks only for the size. The resource blob stays empty in stage A.
    if (pData->pAllocationPrivateDriverData != NULL)
    {
        if (pData->AllocationPrivateDriverDataSize < sizeof(private)) return STATUS_INVALID_PARAMETER;
        RtlCopyMemory(pData->pAllocationPrivateDriverData, &private, sizeof(private));
    }
    pData->AllocationPrivateDriverDataSize = sizeof(private);
    pData->pResourcePrivateDriverData = NULL;
    pData->ResourcePrivateDriverDataSize = 0;

    if (WddmFirstCalls(wddm, WddmDdiGetStandardAllocationDriverData))
        GuardLog("wddm: GetStandardAllocationDriverData type %u %ux%u format %u -> %llu bytes",
                 (ULONG)pData->StandardAllocationType, private.Width, private.Height, private.Format, private.Size);
    return STATUS_SUCCESS;
}

static void WddmCpuVisibleAllocationFlags(DXGK_ALLOCATIONINFOFLAGS_WDDM2_0* Flags)
{
    // Reserved fields are inputs from dxgkrnl, not ours to zero. In particular,
    // sharing metadata must survive DxgkDdiCreateAllocation.
    Flags->CpuVisible = 1;
    Flags->PermanentSysMem = 0;
    Flags->Cached = 0;
    Flags->Protected = 0;
    Flags->ExistingSysMem = 0;
    Flags->ExistingKernelSysMem = 0;
    Flags->FromEndOfSegment = 0;
    Flags->DisableLargePageMapping = 0;
    Flags->Overlay = 0;
    Flags->Capture = 0;
    Flags->HistoryBuffer = 0;
    Flags->AccessedPhysically = 0;
    Flags->ExplicitResidencyNotification = 0;
    Flags->HardwareProtected = 0;
}

static DXGKDDI_CREATEALLOCATION Bc250WddmCreateAllocation;
static NTSTATUS Bc250WddmCreateAllocation(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION* pCreateAllocation)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT i;
    BOOLEAN sharedCpu = FALSE;
    // E26 resource descriptor, separate from the unchanged LB7A allocation ABI.
    // Windows rejects CPU-visible shared allocations in a local-memory-only set.
    if (pCreateAllocation->PrivateDriverDataSize == 3 * sizeof(ULONG) &&
        pCreateAllocation->pPrivateDriverData != NULL)
    {
        const ULONG* descriptor = (const ULONG*)pCreateAllocation->pPrivateDriverData;
        sharedCpu = descriptor[0] == 0x52363245ul && descriptor[1] == 1 && descriptor[2] == 1;
    }

    for (i = 0; i < pCreateAllocation->NumAllocations; i++)
    {
        DXGK_ALLOCATIONINFO* info = &pCreateAllocation->pAllocationInfo[i];
        const BC250_WDDM_ALLOCATION_PRIVATE* private = (const BC250_WDDM_ALLOCATION_PRIVATE*)info->pPrivateDriverData;
        BC250_WDDM_OBJECT* object;

        if (info->PrivateDriverDataSize == sizeof(BC250_WDDM_ALLOCATION_PRIVATE) && private != NULL &&
            private->Magic == BC250_WDDM_ALLOCATION_PRIVATE_MAGIC && private->Width == 64 && private->Height == 32)
            GuardLog("wddm: E26 shared control allocation reached KMD flags %x allocation flags %x bytes %llu format %u",
                     pCreateAllocation->Flags.Value, info->FlagsWddm2.Value, private->Size, private->Format);

        // M8: "BC2A" is a UMD allocation, beside the GDI "LB7A" below. The two magics differ on purpose.
        // requested_va is stored on the object and not programmed here: the winsys maps it afterwards.
        if (UmdBlobIsAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize))
        {
            struct umd_alloc_view view;
            int st = UmdBlobParseAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize, &view);
            UINT segment;
            UINT align;

            if (st != UMD_BLOB_OK || !g_ApertureOffered)
            {
                if (wddm != NULL && InterlockedIncrement(&wddm->UmdAllocRefused) <= BC250_WDDM_LOG_CALLS)
                    GuardLog("wddm: umd alloc refused, %s, private %u, segment %s",
                             st != UMD_BLOB_OK ? UmdBlobStatusText(st) : "no segment",
                             info->PrivateDriverDataSize, g_ApertureOffered ? "yes" : "no");
                while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
                return STATUS_INVALID_PARAMETER;
            }
            object = WddmNewObject(device, BC250_WDDM_MAGIC_ALLOCATION);
            if (object == NULL)
            {
                while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
                return STATUS_INSUFFICIENT_RESOURCES;
            }
            object->UmdAlloc = TRUE;
            object->UmdBytes = view.bytes;
            object->UmdHeap = view.heap;
            object->UmdRequestedVa = view.requested_va;
            segment = (view.heap == UMD_BLOB_HEAP_GTT) ? BC250_WDDM_SEGMENT_APERTURE : BC250_WDDM_SEGMENT_VRAM;
            align = 4096;
            if (view.alignment >= 64 && view.alignment <= 0x100000ull && (view.alignment & (view.alignment - 1ull)) == 0)
                align = (UINT)view.alignment;
            info->hAllocation = object;
            info->Size = (SIZE_T)ROUND_TO_PAGES((SIZE_T)view.bytes);
            info->Alignment = align;
            info->HintedBank.Value = 0;
            info->MaximumRenamingListLength = 0;
            info->pAllocationUsageHint = NULL;
            info->PitchAlignedSize = 0;
            info->PreferredSegment.Value = 0;
            info->PreferredSegment.SegmentId0 = segment;
            info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(segment);
            info->SupportedWriteSegmentSet = BC250_WDDM_SEGMENT_SET(segment);
            info->EvictionSegmentSet = 0;
            info->PhysicalAdapterIndex = 0;
            WddmCpuVisibleAllocationFlags(&info->FlagsWddm2);
            // RADV's CPU_GTT_USWC requests write-combined storage; without it,
            // CPU-accessible GTT requires cached backing store. VidMm supplies
            // CacheCoherent PTEs, which the encoder maps to AMDGPU_PTE_SNOOPED.
            info->FlagsWddm2.Cached = (UINT)UmdBlobAllocCpuCached(&view);
            info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
            if (wddm != NULL && InterlockedIncrement(&wddm->UmdAllocs) <= BC250_WDDM_LOG_CALLS)
                GuardLog("wddm: umd alloc %llu bytes heap 0x%lX align %u va 0x%llX gem 0x%llX cached %u", view.bytes, view.heap, align,
                         view.requested_va,view.gem_flags,info->FlagsWddm2.Cached);
            continue;
        }

        // Stage A can only size an allocation it described itself. An unknown blob is an honest failure: nothing
        // in the never-fail list reaches this DDI, and guessing a size would put VidMm and us out of step.
        if (private == NULL || info->PrivateDriverDataSize < sizeof(*private) ||
            private->Magic != BC250_WDDM_ALLOCATION_PRIVATE_MAGIC || private->Size == 0)
        {
            if (WddmFirstCalls(wddm, WddmDdiCreateAllocation))
                GuardLog("wddm: CreateAllocation %u of %u refused, private data %u bytes", i,
                         pCreateAllocation->NumAllocations, info->PrivateDriverDataSize);
            while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
            return STATUS_INVALID_PARAMETER;
        }

        object = WddmNewObject(device, BC250_WDDM_MAGIC_ALLOCATION);
        if (object == NULL)
        {
            while (i-- > 0) WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        object->Allocation = *private;

        info->hAllocation = object;
        info->Size = (SIZE_T)ROUND_TO_PAGES(private->Size);
        // DXGK_ALLOCATIONINFO is an OUT array that nobody promised to zero: every member is written, as both
        // reference drivers do (Alignment 64 is theirs too).
        info->Alignment = 64;
        info->HintedBank.Value = 0;
        info->MaximumRenamingListLength = 0;
        info->pAllocationUsageHint = NULL;
        info->PitchAlignedSize = 0;                     // the aperture segment is not a pitch-aligned one
        info->PreferredSegment.Value = 0;
        info->PreferredSegment.SegmentId0 = sharedCpu ? BC250_WDDM_SEGMENT_APERTURE : BC250_WDDM_SEGMENT_VRAM;
        info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(info->PreferredSegment.SegmentId0);
        info->SupportedWriteSegmentSet = info->SupportedReadSegmentSet;
        info->EvictionSegmentSet = 0;                   // surfaces live in the local segment only; no eviction target
        info->PhysicalAdapterIndex = 0;
        WddmCpuVisibleAllocationFlags(&info->FlagsWddm2);
        // Linear VRAM blits use physical mappings. System-memory pages are not assumed contiguous.
        info->FlagsWddm2.AccessedPhysically = !sharedCpu;
        info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    }
    if (pCreateAllocation->Flags.Resource && pCreateAllocation->hResource == NULL)
    {
        pCreateAllocation->hResource = WddmNewObject(device, BC250_WDDM_MAGIC_RESOURCE);
        if (pCreateAllocation->hResource == NULL)
        {
            for (i = 0; i < pCreateAllocation->NumAllocations; i++)
                WddmFreeObject((BC250_WDDM_OBJECT*)pCreateAllocation->pAllocationInfo[i].hAllocation);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }
    if (WddmFirstCalls(wddm, WddmDdiCreateAllocation))
        GuardLog("wddm: CreateAllocation %u allocations, flags %x resource %s", pCreateAllocation->NumAllocations,
                 pCreateAllocation->Flags.Value, pCreateAllocation->hResource != NULL ? "yes" : "no");
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYALLOCATION Bc250WddmDestroyAllocation;
static NTSTATUS Bc250WddmDestroyAllocation(_In_ const HANDLE hAdapter,
                                           _In_ const DXGKARG_DESTROYALLOCATION* pDestroyAllocation)
{
    UINT i;

    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiDestroyAllocation))
        GuardLog("wddm: DestroyAllocation %u allocations", pDestroyAllocation->NumAllocations);
    for (i = 0; i < pDestroyAllocation->NumAllocations; i++)
        WddmFreeObject(WddmObject(pDestroyAllocation->pAllocationList[i], BC250_WDDM_MAGIC_ALLOCATION));
    if (pDestroyAllocation->Flags.DestroyResource && pDestroyAllocation->hResource != NULL)
        WddmFreeObject(WddmObject(pDestroyAllocation->hResource, BC250_WDDM_MAGIC_RESOURCE));
    return STATUS_SUCCESS;
}

static DXGKDDI_DESCRIBEALLOCATION Bc250WddmDescribeAllocation;
static NTSTATUS Bc250WddmDescribeAllocation(_In_ const HANDLE hAdapter,
                                            _Inout_ DXGKARG_DESCRIBEALLOCATION* pDescribeAllocation)
{
    BC250_WDDM_OBJECT* object = WddmObject(pDescribeAllocation->hAllocation, BC250_WDDM_MAGIC_ALLOCATION);

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    pDescribeAllocation->Width = object->Allocation.Width;
    pDescribeAllocation->Height = object->Allocation.Height;
    pDescribeAllocation->Format = (D3DDDIFORMAT)object->Allocation.Format;
    pDescribeAllocation->MultisampleMethod.NumSamples = 0;
    pDescribeAllocation->MultisampleMethod.NumQualityLevels = 0;
    // Match the nominal mode advertised by display.c for the full WDDM table.
    // E26: Dxgkrnl compares this frequency with the current display mode;
    // NOTSPECIFIED here caused STATUS_GRAPHICS_PRESENT_MODE_CHANGED.
    pDescribeAllocation->RefreshRate.Numerator = 60000;
    pDescribeAllocation->RefreshRate.Denominator = 1000;
    pDescribeAllocation->PrivateDriverFormatAttribute = 0;
    pDescribeAllocation->Rotation = D3DDDI_ROTATION_IDENTITY;
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiDescribeAllocation))
        GuardLog("wddm: DescribeAllocation %ux%u format %u", pDescribeAllocation->Width, pDescribeAllocation->Height,
                 (ULONG)pDescribeAllocation->Format);
    return STATUS_SUCCESS;
}

static DXGKDDI_OPENALLOCATIONINFO Bc250WddmOpenAllocation;
static NTSTATUS Bc250WddmOpenAllocation(_In_ const HANDLE hDevice, _In_ const DXGKARG_OPENALLOCATION* pOpenAllocation)
{
    BC250_WDDM_OBJECT* parent = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);
    UINT i;

    if (parent == NULL) return STATUS_INVALID_PARAMETER;
    // What goes into hDeviceSpecificAllocation comes back in every DXGK_ALLOCATIONLIST entry of a Present or a Render
    // (facts M82 saw the NULL 0.7.15 put there). 0.7.16 asked DxgkCbGetHandleData(DXGK_HANDLE_ALLOCATION) for the
    // object CreateAllocation stored and got something else for the CDD's handles (E20 run 003: 0xC00006C0,
    // 0xC0000000 - "not one of our allocations"); the raw answer is still logged below, for the record. What this
    // DDI does get, by contract, is the allocation's own private driver data - the blob GetStandardAllocationDriverData
    // wrote and CreateAllocation validated - so the handle handed out is an object of our own holding a copy of it,
    // and CloseAllocation frees it again. dxgkrnl closes what it opened, and the stop's sweep frees the rest.
    for (i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO* info = &pOpenAllocation->pOpenAllocation[i];
        const BC250_WDDM_ALLOCATION_PRIVATE* private = (const BC250_WDDM_ALLOCATION_PRIVATE*)info->pPrivateDriverData;
        BC250_WDDM_OBJECT* opened = NULL;
        void* raw = NULL;

        if (parent->Device->Dxgk.DxgkCbGetHandleData != NULL)
        {
            DXGKARGCB_GETHANDLEDATA data;

            data.hObject = info->hAllocation;
            data.Type = DXGK_HANDLE_ALLOCATION;
            data.Flags.Value = 0;
            raw = parent->Device->Dxgk.DxgkCbGetHandleData(&data);
        }
        if (UmdBlobIsAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize))
        {
            struct umd_alloc_view view;

            // Same blob CreateAllocation already accepted. A refusal here leaves a NULL handle, as a bad
            // GDI blob does, rather than failing the open: dxgkrnl opened what it created.
            if (UmdBlobParseAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize, &view) == UMD_BLOB_OK)
            {
                opened = WddmNewObject(parent->Device, BC250_WDDM_MAGIC_OPENED);
                if (opened != NULL)
                {
                    opened->UmdAlloc = TRUE;
                    opened->UmdBytes = view.bytes;
                    opened->UmdHeap = view.heap;
                    opened->UmdRequestedVa = view.requested_va;
                }
            }
        }
        else if (private != NULL && info->PrivateDriverDataSize >= sizeof(*private) && private->Magic == BC250_WDDM_ALLOCATION_PRIVATE_MAGIC &&
            private->Size != 0)
        {
            opened = WddmNewObject(parent->Device, BC250_WDDM_MAGIC_OPENED);
            if (opened != NULL) opened->Allocation = *private;
        }
        info->hDeviceSpecificAllocation = opened;
        if (parent->Device->Wddm != NULL && ((BC250_WDDM*)parent->Device->Wddm)->Calls[WddmDdiOpenAllocation] < BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: OpenAllocation [%u] handle 0x%08X private %u bytes -> %p (GetHandleData said %p)", i, (ULONG)info->hAllocation,
                     info->PrivateDriverDataSize, (void*)opened, raw);
    }
    if (WddmFirstCalls((BC250_WDDM*)parent->Device->Wddm, WddmDdiOpenAllocation))
        GuardLog("wddm: OpenAllocation %u allocations flags 0x%08X", pOpenAllocation->NumAllocations, pOpenAllocation->Flags.Value);
    return STATUS_SUCCESS;
}

static DXGKDDI_CLOSEALLOCATION Bc250WddmCloseAllocation;
static NTSTATUS Bc250WddmCloseAllocation(_In_ const HANDLE hDevice, _In_ const DXGKARG_CLOSEALLOCATION* pCloseAllocation)
{
    BC250_WDDM_OBJECT* parent = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);
    UINT i;

    if (parent == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls((BC250_WDDM*)parent->Device->Wddm, WddmDdiCloseAllocation))
        GuardLog("wddm: CloseAllocation %u allocations", pCloseAllocation->NumAllocations);
    // pOpenHandleList carries what OpenAllocation handed out: our opened objects, or NULL where it handed out nothing.
    // Looked up on the list before being freed, as the blit does, never dereferenced as given.
    for (i = 0; i < pCloseAllocation->NumAllocations; i++)
        WddmFreeObject(WddmListedObject((BC250_WDDM*)parent->Device->Wddm, pCloseAllocation->pOpenHandleList[i], BC250_WDDM_MAGIC_OPENED));
    return STATUS_SUCCESS;
}

// ---- paging and submission: the DDIs that may not fail ------------------------------------------------------------

// Private header and DMA capacity must be accepted before logical PTE publication.
// PnP excludes StopDevice during this DDI. OS buffers and any internally captured
// graph arrays remain valid through this call; no graph pointer reaches hardware.
// Internal failures do not advance the published buffer.
static BOOLEAN WddmNativeAllocation(BC250_DEVICE* Device, BC250_WDDM_OBJECT* Context,
                                   const DXGKARG_BUILDPAGINGBUFFER* Build, BOOLEAN Fill)
{
    HANDLE handle=Fill ? Build->FillVirtual.hAllocation : Build->TransferVirtual.hAllocation;
    ULONGLONG offset=Fill ? Build->FillVirtual.AllocationOffsetInBytes : Build->TransferVirtual.AllocationOffsetInBytes;
    ULONGLONG bytes=Fill ? Build->FillVirtual.FillSizeInBytes : Build->TransferVirtual.TransferSizeInBytes;
    BC250_WDDM_OBJECT* allocation=WddmObject(handle,BC250_WDDM_MAGIC_ALLOCATION);
    ULONGLONG size;
    if(!Context || !Context->SystemContext || !Context->RootPhysical || Context->Device!=Device ||
       !Build->DmaBufferGpuVirtualAddress || Build->MultipassOffset>=0x40000000u ||
       !allocation || allocation->Device!=Device)return FALSE;
    size=allocation->UmdAlloc ? allocation->UmdBytes : allocation->Allocation.Size;
    return bytes && offset<size && bytes<=size-offset;
}

static NTSTATUS WddmPublishPagingRecordCore(_Inout_ DXGKARG_BUILDPAGINGBUFFER* Build,
                                        ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next,
                                        ULONGLONG CopySource, ULONGLONG CopyDestination, ULONG CopyCount,
                                        ULONGLONG FillPhysical, ULONGLONG FillBytes, ULONG FillPattern,
                                        const BC250_PAGING_COPY_SLICE* Transfer, const PAGING_GRAPH_BATCH* Graph)
{
    ULONG* record;
    NTSTATUS status;
    if (Build==NULL || Build->pDmaBuffer==NULL || Build->pDmaBufferPrivateData==NULL ||
        Written==0 || Written>Build->DmaSize/sizeof(ULONG)) return STATUS_INVALID_PARAMETER;
    // A graph describes the complete logical effect of this record. Do not mix
    // it with another commit whose later failure could leave a partial effect.
    if(Graph && (Update || CopyCount || FillBytes || Transfer ||
       Build->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
       Build->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT))return STATUS_INVALID_PARAMETER;
    record=(ULONG*)Build->pDmaBufferPrivateData;
    if (!PagingPrivateQueuedHeader((unsigned*)record,Build->DmaBufferPrivateDataSize,
            Build->DmaBufferWriteOffset,Build->DmaBufferGpuVirtualAddress,Written*4u)) return STATUS_INVALID_PARAMETER;
    if (Update) {
        if (Next<=Start) { record[0]=0; return STATUS_INVALID_PARAMETER; }
        status=VidMmCommitPagingUpdate(&Build->UpdatePageTable,Start,Next-Start);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (CopyCount!=0) {
        status=VidMmCommitPagingCopy(CopySource,CopyDestination,CopyCount);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (FillBytes) {
        status=VidMmCommitPagingFill(FillPhysical,FillBytes,FillPattern);
        if (!NT_SUCCESS(status)) { record[0]=0; return status; }
    }
    if (Transfer) {
        status=VidMmCommitPagingTransfer(Transfer);
        if (!NT_SUCCESS(status)) { record[0]=0;return status; }
    }
    if (Build->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
        Build->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) {
        status=VidMmCommitPagingAperture(Build,Start,Next);
        if (!NT_SUCCESS(status)) {record[0]=0;return status;}
    }
    if(Graph) {
        status=VidMmCommitPagingGraph(Graph);
        if(!NT_SUCCESS(status)){record[0]=0;return status;}
    }
    RtlCopyMemory(Build->pDmaBuffer,(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Written*4u);
    Build->pDmaBufferPrivateData=(PUCHAR)record+PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,Written*4u);
    Build->DmaBufferPrivateDataSize-=PagingPrivateQueuedSize(PAGING_PRIVATE_QUEUED_DIRECT,Written*4u);
    if (Build->DmaBufferPrivateDataSize>=sizeof(ULONG)) *(ULONG*)Build->pDmaBufferPrivateData=0;
    Build->pDmaBuffer=(PUCHAR)Build->pDmaBuffer+Written*4u;
    Build->DmaSize-=Written*4u;
    return STATUS_SUCCESS;
}

static NTSTATUS WddmPublishPagingRecordFull(DXGKARG_BUILDPAGINGBUFFER* Build,
    ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next,
    ULONGLONG CopySource, ULONGLONG CopyDestination, ULONG CopyCount,
    ULONGLONG FillPhysical, ULONGLONG FillBytes, ULONG FillPattern)
{
    return WddmPublishPagingRecordCore(Build,Written,Update,Start,Next,CopySource,CopyDestination,
        CopyCount,FillPhysical,FillBytes,FillPattern,NULL,NULL);
}

static NTSTATUS WddmPublishPagingRecordEx(_Inout_ DXGKARG_BUILDPAGINGBUFFER* Build,
                                        ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next,
                                        ULONGLONG CopySource, ULONGLONG CopyDestination, ULONG CopyCount)
{
    return WddmPublishPagingRecordFull(Build,Written,Update,Start,Next,
        CopySource,CopyDestination,CopyCount,0,0,0);
}

static NTSTATUS WddmPublishPagingRecord(_Inout_ DXGKARG_BUILDPAGINGBUFFER* Build,
                                        ULONG Written, BOOLEAN Update, ULONG Start, ULONG Next)
{
    return WddmPublishPagingRecordEx(Build,Written,Update,Start,Next,0,0,0);
}

// MultipassOffset counts whole ranges, each at most one advertised 4 KiB table.
// Publish each accepted range before resolving the next: later ranges may depend
// on earlier logical copies, even before the GPU executes the buffer.
static NTSTATUS WddmBuildPagingCopies(_Inout_ BC250_DEVICE* Device, ULONGLONG Root,
                                     _Inout_ DXGKARG_BUILDPAGINGBUFFER* Build)
{
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    NTSTATUS status=STATUS_SUCCESS;
    if (Build->MultipassOffset>Build->CopyPageTableEntries.NumRanges ||
        (Build->CopyPageTableEntries.NumRanges && !Build->CopyPageTableEntries.pRanges))
        return STATUS_INVALID_PARAMETER;
    while (Build->MultipassOffset<Build->CopyPageTableEntries.NumRanges) {
        const DXGK_BUILDPAGINGBUFFER_COPY_RANGE* range=
            &Build->CopyPageTableEntries.pRanges[Build->MultipassOffset];
        ULONG written=0,freeBytes=Build->DmaSize;
        ULONGLONG source=0,destination=0;
        BC250_WDDM_PAGING_UNSUPPORTED unsupported;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (freeBytes>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
            freeBytes=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
        status=GfxPagingBuildCopyRange(Device,Root,range,(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,
            Build->DmaBufferWriteOffset,freeBytes,&written,&source,&destination,&unsupported);
        if (!NT_SUCCESS(status)) break;
        status=WddmPublishPagingRecordEx(Build,written,FALSE,0,0,
            source,destination,range->NumPageTableEntries);
        if (!NT_SUCCESS(status)) break;
        Build->MultipassOffset++;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    // The OS owns the input offset. Only pointers, remaining sizes and progress
    // are outputs; the local offset above accounts for every range in this call.
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

// UINT token counts page slices, not bytes. The first/last slices may be
// partial; this preserves unaligned DWORD fills and progress beyond 4 GiB.
static NTSTATUS WddmBuildPhysicalFill(BC250_DEVICE* Device, DXGKARG_BUILDPAGINGBUFFER* Build,
                                     ULONGLONG* Moved)
{
    BC250_PAGING_ENDPOINT destination;
    ULONGLONG bytes=Build->Fill.FillSize,start,next;
    ULONG token=Build->MultipassOffset,written=0,capacity;
    unsigned nextToken;
    ULONG* record;
    NTSTATUS status;
    *Moved=0;
    if (!WddmLocalPagingEndpoint(Device,Build->Fill.Destination.SegmentId,
        (ULONGLONG)Build->Fill.Destination.SegmentAddress.QuadPart,0,bytes,&destination) ||
        ((destination.Address | bytes)&3)!=0 || !Build->pDmaBuffer) return STATUS_INVALID_PARAMETER;
    if (destination.Aperture && !VidMmApertureRangeValid(destination.Address,bytes)) return STATUS_INVALID_PARAMETER;
    if (!PagingStreamTokenDecode(TRUE,0,destination.Address,bytes,token,&start))
        return STATUS_INVALID_PARAMETER;
    if (start==bytes) return STATUS_SUCCESS;
    if (!Build->pDmaBufferPrivateData || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    record=(ULONG*)Build->pDmaBufferPrivateData;record[0]=0;
    capacity=Build->DmaSize;
    if (capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
        capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
    status=GfxPagingBuildPhysical(Device,NULL,&destination,TRUE,bytes,Build->Fill.FillPattern,
        (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,
        start,&written,&next);
    if (status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) return status;
    if (!PagingStreamTokenEncode(TRUE,0,destination.Address,bytes,next,&nextToken))
        return STATUS_INVALID_PARAMETER;
    if (written) {
        ULONGLONG physical=0,fillBytes=0;
        NTSTATUS publish;
        if (Build->Fill.Destination.SegmentId==BC250_WDDM_SEGMENT_TABLES) {
            ULONGLONG offset=destination.Address-Device->VramMcBase+start;
            physical=(ULONGLONG)Device->VramPhysical.QuadPart;
            if (offset>MAXULONGLONG-physical) return STATUS_INVALID_PARAMETER;
            physical+=offset;fillBytes=next-start;
        }
        publish=WddmPublishPagingRecordFull(Build,written,FALSE,0,0,0,0,0,
            physical,fillBytes,Build->Fill.FillPattern);
        if (!NT_SUCCESS(publish)) return publish;
    }
    Build->MultipassOffset=nextToken;
    *Moved=next-start;
    return status;
}

// Stateless classification, not a retained PFN capture. Prove that the
// operation touches data backing and that copy endpoints have disjoint physical
// bounds. Table copies, aliases and unknown allocations keep their current path.
static BOOLEAN WddmNativeDataBounds(BC250_DEVICE* Device, ULONGLONG Root,
    ULONGLONG Va, ULONGLONG Bytes, BOOLEAN Write, ULONGLONG* Low, ULONGLONG* High)
{
    ULONGLONG app,length,table,tableLength,base,end,done=0;
    *Low=MAXULONGLONG;*High=0;
    if(!Bytes || Va>MAXULONGLONG-Bytes ||
       !WddmMemoryLayout(Device,&app,&length,&table,&tableLength))return FALSE;
    base=(ULONGLONG)Device->VramPhysical.QuadPart;
    if(base>MAXULONGLONG-Device->VramLength)return FALSE;
    end=base+Device->VramLength;
    while(done<Bytes) {
        ULONGLONG pa,next;BOOLEAN system;
        ULONG count=PAGE_SIZE-(ULONG)((Va+done)&(PAGE_SIZE-1));
        if(count>Bytes-done)count=(ULONG)(Bytes-done);
        if(!VidMmTranslatePagingAccess(Root,Va+done,Write,&pa,&system) || pa>MAXULONGLONG-count)return FALSE;
        next=pa+count;
        if(system) {if(pa<end && base<next)return FALSE;}
        else if(pa<base+app || pa-base-app>=length || count>length-(pa-base-app))return FALSE;
        if(pa<*Low)*Low=pa;if(next>*High)*High=next;
        done+=count;
    }
    return TRUE;
}

// Coarse physical bounds include holes between fragmented pages. When a DMA
// span intersects those bounds, inspect actual backing before calling it an
// alias. This bounded-memory walk never allocates or retains a PFN capture.
static BOOLEAN WddmNativeRangeDisjoint(ULONGLONG Root, ULONGLONG Va,
    ULONGLONG Bytes, ULONGLONG Low, ULONGLONG High)
{
    ULONGLONG done=0;
    while(done<Bytes) {
        ULONGLONG pa;BOOLEAN system;
        ULONG count=PAGE_SIZE-(ULONG)((Va+done)&(PAGE_SIZE-1));
        if(count>Bytes-done)count=(ULONG)(Bytes-done);
        if(!VidMmTranslatePagingAccess(Root,Va+done,FALSE,&pa,&system) ||
           pa>MAXULONGLONG-count || (pa<High && Low<pa+count))return FALSE;
        done+=count;
    }
    return TRUE;
}

// The CPU writes OS-owned DMA storage. Prove the captured GPU root names the
// same bytes on every page and permits the embedded CSA write. Do not create
// another CPU mapping or infer cache coherence from matching physical addresses.
static BOOLEAN WddmNativeDmaMapping(ULONGLONG Root, DXGKARG_BUILDPAGINGBUFFER* Build,
    const PAGING_NATIVE_RESULT* Built, BOOLEAN Fill, ULONGLONG SrcLow, ULONGLONG SrcHigh,
    ULONGLONG DstLow, ULONGLONG DstHigh, ULONG* GapProofs)
{
    ULONGLONG va=Build->DmaBufferGpuVirtualAddress+Build->DmaBufferWriteOffset;
    ULONGLONG destination=Fill?Build->FillVirtual.DestinationVirtualAddress:Build->TransferVirtual.DestinationVirtualAddress;
    ULONGLONG total=Fill?Build->FillVirtual.FillSizeInBytes:Build->TransferVirtual.TransferSizeInBytes;
    ULONG done=0;
    *GapProofs=0;
    while(done<Built->Bytes) {
        ULONGLONG pa,end;BOOLEAN system;
        ULONG count=PAGE_SIZE-(ULONG)((va+done)&(PAGE_SIZE-1));
        if(count>Built->Bytes-done)count=Built->Bytes-done;
        if(!VidMmTranslatePagingAccess(Root,va+done,FALSE,&pa,&system) ||
           pa!=(ULONGLONG)MmGetPhysicalAddress((UCHAR*)Build->pDmaBuffer+done).QuadPart ||
           pa>MAXULONGLONG-count)return FALSE;
        end=pa+count;
        // Data must not overwrite its commands/CSA, or source bytes become CSA.
        if(pa<DstHigh && DstLow<end) {
            if(!WddmNativeRangeDisjoint(Root,destination,total,pa,end))return FALSE;
            ++*GapProofs;
        }
        if(!Fill && pa<SrcHigh && SrcLow<end) {
            if(!WddmNativeRangeDisjoint(Root,Build->TransferVirtual.SourceVirtualAddress,total,pa,end))return FALSE;
            ++*GapProofs;
        }
        done+=count;
    }
    {
        ULONGLONG pa;BOOLEAN system;
        return VidMmTranslatePagingAccess(Root,va+Built->CsaOffset,TRUE,&pa,&system);
    }
}

static NTSTATUS WddmBuildNativeVirtual(BC250_DEVICE* Device, ULONGLONG Root,
    BOOLEAN Fill, DXGKARG_BUILDPAGINGBUFFER* Build, ULONGLONG* Moved)
{
    ULONGLONG source=Fill?0:Build->TransferVirtual.SourceVirtualAddress;
    ULONGLONG destination=Fill?Build->FillVirtual.DestinationVirtualAddress:Build->TransferVirtual.DestinationVirtualAddress;
    ULONGLONG total=Fill?Build->FillVirtual.FillSizeInBytes:Build->TransferVirtual.TransferSizeInBytes;
    ULONGLONG srcLow=0,srcHigh=0,dstLow,dstHigh;
    PAGING_NATIVE_RESULT built;
    ULONG gapProofs;
    NTSTATUS status;
    *Moved=0;
    if(!Root || !Build->DmaBufferGpuVirtualAddress ||
       !WddmNativeDataBounds(Device,Root,destination,total,TRUE,&dstLow,&dstHigh) ||
       (!Fill && (!WddmNativeDataBounds(Device,Root,source,total,FALSE,&srcLow,&srcHigh) ||
                 (srcLow<dstHigh && dstLow<srcHigh))))return STATUS_NOT_SUPPORTED;
    if(!Build->pDmaBufferPrivateData || Build->DmaBufferPrivateDataSize<PAGING_PRIVATE_NATIVE_BYTES+PAGING_PRIVATE_JOB_BYTES)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    status=GfxPagingBuildNative(Device,Fill,source,destination,total,Fill?Build->FillVirtual.FillPattern:0,
        Build->pDmaBuffer,Build->DmaBufferGpuVirtualAddress,Build->DmaBufferWriteOffset,
        Build->DmaSize,Build->MultipassOffset,&built);
    if(status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)return status;
    if(!built.Bytes)return status;
    if(!WddmNativeDmaMapping(Root,Build,&built,Fill,srcLow,srcHigh,dstLow,dstHigh,&gapProofs))return STATUS_NOT_SUPPORTED;
    if(!PagingPrivateQueuedNativeHeader((unsigned*)Build->pDmaBufferPrivateData,Build->DmaBufferPrivateDataSize,
        Build->DmaBufferWriteOffset,Build->DmaBufferGpuVirtualAddress,built.Bytes,Root,
        built.IbOffset,built.IbDwords,built.CsaOffset))return STATUS_INVALID_PARAMETER;
    Build->pDmaBuffer=(UCHAR*)Build->pDmaBuffer+built.Bytes;Build->DmaSize-=built.Bytes;
    Build->pDmaBufferPrivateData=(UCHAR*)Build->pDmaBufferPrivateData+PAGING_PRIVATE_NATIVE_BYTES+PAGING_PRIVATE_JOB_BYTES;
    Build->DmaBufferPrivateDataSize-=PAGING_PRIVATE_NATIVE_BYTES+PAGING_PRIVATE_JOB_BYTES;
    if(Build->DmaBufferPrivateDataSize>=sizeof(ULONG))*(ULONG*)Build->pDmaBufferPrivateData=0;
    Build->MultipassOffset=built.NextToken;*Moved=built.Moved;
    if(Device->Wddm) {
        BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
        InterlockedIncrement64(Fill?&wddm->PagingNativeFills:&wddm->PagingNativeTransfers);
        InterlockedAdd64(&wddm->PagingNativeBytes,(LONG64)built.Moved);
        InterlockedAdd64(&wddm->PagingDmaGapProofs,(LONG64)gapProofs);
    }
    return status;
}

static NTSTATUS WddmBuildVirtualFill(BC250_DEVICE* Device, ULONGLONG Root,
    DXGKARG_BUILDPAGINGBUFFER* Build, ULONGLONG* Moved)
{
    ULONGLONG bytes=Build->FillVirtual.FillSizeInBytes,va=Build->FillVirtual.DestinationVirtualAddress;
    ULONGLONG app,appLength,table,tableLength,tablePhysical,progress;
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    // Slice tokens preserve full64-bit byte progress without per-request storage.
    if (!Root || !Build->pDmaBuffer || !PagingStreamTokenDecode(TRUE,0,va,bytes,Build->MultipassOffset,&progress) ||
        !WddmMemoryLayout(Device,&app,&appLength,&table,&tableLength) ||
        table>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_INVALID_PARAMETER;
    tablePhysical=(ULONGLONG)Device->VramPhysical.QuadPart+table;
    while (progress<bytes) {
        ULONGLONG address=va+progress,physical;
        ULONG count=(ULONG)(PAGE_SIZE-(address&(PAGE_SIZE-1))),written=0,capacity=Build->DmaSize;
        unsigned nextToken;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        BOOLEAN system;
        if (count>bytes-progress) count=(ULONG)(bytes-progress);
        if (!PagingStreamTokenEncode(TRUE,0,va,bytes,progress+count,&nextToken)) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
            capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
        status=GfxPagingBuildFillPage(Device,Root,address,count,Build->FillVirtual.FillPattern,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,
            &written,&physical,&system);
        if (!NT_SUCCESS(status)) break;
        // Match the captured physical identity, never translate this VA again
        // after committing an earlier slice that could modify its mapping.
        {
            ULONGLONG fillBytes=0;
            if (!system && physical>=tablePhysical && physical-tablePhysical<tableLength) {
                if (count>tableLength-(physical-tablePhysical)) {status=STATUS_INVALID_PARAMETER;break;}
                fillBytes=count;
            }
            status=WddmPublishPagingRecordFull(Build,written,FALSE,0,0,0,0,0,
                physical,fillBytes,Build->FillVirtual.FillPattern);
        }
        if (!NT_SUCCESS(status)) break;
        progress+=count;Build->MultipassOffset=nextToken;*Moved+=count;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

// Both endpoints use the paging process root: SourcePageTableVaInTransfer is
// not advertised. Commit each accepted slice before translating the next one.
static NTSTATUS WddmBuildVirtualTransfer(BC250_DEVICE* Device, ULONGLONG Root,
    DXGKARG_BUILDPAGINGBUFFER* Build, ULONGLONG* Moved)
{
    ULONGLONG bytes=Build->TransferVirtual.TransferSizeInBytes;
    ULONGLONG src=Build->TransferVirtual.SourceVirtualAddress,dst=Build->TransferVirtual.DestinationVirtualAddress;
    ULONGLONG app,appLength,table,tableLength,tablePhysical,progress;
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    BOOLEAN graphResume=(BOOLEAN)((Build->MultipassOffset&PAGING_PERMUTATION_RESUME)!=0);
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    if (!Root || !Build->pDmaBuffer || !PagingStreamTokenDecode(FALSE,src,dst,bytes,graphResume?0:Build->MultipassOffset,&progress) ||
        !WddmMemoryLayout(Device,&app,&appLength,&table,&tableLength) ||
        table>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart) return STATUS_INVALID_PARAMETER;
    // Capture every system-page dependency before publishing a virtual prefix.
    // Local/mixed endpoints keep the table-shadow-aware path below; general
    // cross-page aliases in that path remain separate work.
    if(bytes>PAGE_SIZE && (src&(PAGE_SIZE-1))==(dst&(PAGE_SIZE-1))) {
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        ULONG capacity=Build->DmaSize,written=0;
        unsigned next=Build->MultipassOffset;
        if(progress==bytes)return STATUS_SUCCESS;
        if(!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        if(capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
            capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
        status=GfxPagingBuildVirtualPageGraph(Device,Root,src,dst,bytes,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,originalOffset,capacity,
            Build->MultipassOffset,&written,&next);
        if(status!=STATUS_NOT_SUPPORTED) {
            if(status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)return status;
            if(written) {
                NTSTATUS publish=WddmPublishPagingRecord(Build,written,FALSE,0,0);
                if(!NT_SUCCESS(publish))return publish;
            }
            Build->MultipassOffset=next;*Moved=status==STATUS_SUCCESS?bytes:0;
            return status;
        }
        status=STATUS_SUCCESS;
    }
    if(graphResume)return STATUS_INVALID_PARAMETER;
    tablePhysical=(ULONGLONG)Device->VramPhysical.QuadPart+table;
    while (progress<bytes) {
        ULONG count=(ULONG)(PAGE_SIZE-((src+progress)&(PAGE_SIZE-1)));
        ULONG destinationRoom=(ULONG)(PAGE_SIZE-((dst+progress)&(PAGE_SIZE-1)));
        ULONG written=0,capacity=Build->DmaSize;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        BC250_PAGING_COPY_SLICE slice;
        const BC250_PAGING_COPY_SLICE* commit=NULL;
        unsigned nextToken;
        if (count>destinationRoom) count=destinationRoom;
        if (count>bytes-progress) count=(ULONG)(bytes-progress);
        if (!PagingStreamTokenEncode(FALSE,src,dst,bytes,progress+count,&nextToken)) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
            capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
        // Per-slice aliases stage. Arbitrary cross-page physical alias dependencies
        // are still unresolved; VA ordering alone cannot establish physical order.
        status=GfxPagingBuildVirtualCopyPage(Device,Root,src+progress,dst+progress,count,FALSE,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,&written,&slice);
        if (!NT_SUCCESS(status)) break;
        if (!slice.DestinationSystem && slice.DestinationPhysical>=tablePhysical &&
            slice.DestinationPhysical-tablePhysical<tableLength) {
            if (count>tableLength-(slice.DestinationPhysical-tablePhysical)) {
                status=STATUS_INVALID_PARAMETER;break;
            }
            commit=&slice;
        }
        status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,commit,NULL);
        if (!NT_SUCCESS(status)) break;
        progress+=count;Build->MultipassOffset=nextToken;*Moved+=count;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

// The outer paging DDI owns PagingBuildLock. A retained plan outlives individual
// buffers, but hardware sees only copied command bytes, never the plan pointers.
static NTSTATUS WddmBuildCapturedVirtualTransfer(BC250_DEVICE* Device,ULONGLONG Root,
    PAGING_CAPTURE_OWNER* Owner,DXGKARG_BUILDPAGINGBUFFER* Build,ULONGLONG* Moved)
{
    PAGING_GRAPH_CAPTURE* capture;
    ULONGLONG src=Build->TransferVirtual.SourceVirtualAddress,dst=Build->TransferVirtual.DestinationVirtualAddress;
    ULONGLONG bytes=Build->TransferVirtual.TransferSizeInBytes;
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    if(!Root || !Owner || !Build->pDmaBuffer)return STATUS_INVALID_PARAMETER;
    // A transfer wholly inside matching page offsets has no cross-page dependency.
    if((src&4095)==(dst&4095) && bytes<=PAGE_SIZE-(src&4095))return WddmBuildVirtualTransfer(Device,Root,Build,Moved);
    if(Build->MultipassOffset==PAGING_CAPTURE_COMPLETE)return STATUS_SUCCESS;
    if(!Build->MultipassOffset) {
        SIZE_T needed=GfxPagingCaptureStorageSize(src,dst,bytes);
        void* storage=PagingCaptureStorage(Owner,(ULONGLONG)needed);
        if(storage)
            status=GfxPagingCaptureVirtualGraphInPlace(Device,Root,src,dst,bytes,
                storage,needed,&capture);
        else
            // Oversized/exhausted arena fallback remains explicit. Independent
            // multipass plans may share available reservation spans.
            status=GfxPagingCaptureVirtualGraph(Device,Root,src,dst,bytes,&capture);
        if(!NT_SUCCESS(status))return status;
        capture->Owner.ReservationBytes=storage?((ULONGLONG)needed+7)&~7ull:0;
        capture->Owner.Allocation=Build->TransferVirtual.hAllocation;
        capture->Owner.AllocationOffset=Build->TransferVirtual.AllocationOffsetInBytes;
        if(!PagingCaptureAttach(Owner,&capture->Owner)) {
            if(!capture->Owner.ReservationBytes)ExFreePoolWithTag(capture,capture->Owner.PoolTag);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        if(Device->Wddm) {
            BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
            // PagingBuildLock serializes writers; atomic publication allows a
            // concurrent summary read. Each maximum is per context, not a sum.
            if((LONGLONG)Owner->PeakPlans>wddm->CaptureContextPeakPlans)
                InterlockedAdd64(&wddm->CaptureContextPeakPlans,
                    (LONGLONG)Owner->PeakPlans-wddm->CaptureContextPeakPlans);
            if((LONGLONG)Owner->PeakReservedBytes>wddm->CaptureContextPeakReservedBytes)
                InterlockedAdd64(&wddm->CaptureContextPeakReservedBytes,
                    (LONGLONG)Owner->PeakReservedBytes-wddm->CaptureContextPeakReservedBytes);
        }
        if(capture->Owner.ReservationBytes) {
            if(Owner->ReservedCaptures<4)
                GuardLog("wddm: capture reserved token 0x%X bytes %llu pages %u identities %u linear %u",
                    capture->Owner.Token,bytes,capture->PageCount,capture->Identities,capture->Linear);
            if(Owner->ReservedCaptures!=MAXULONG)Owner->ReservedCaptures++;
            if(Device->Wddm)InterlockedIncrement64(&((BC250_WDDM*)Device->Wddm)->CaptureReservedTotal);
        } else {
            if(Owner->HeapCaptures<4)
                GuardLog("wddm: capture heap token 0x%X bytes %llu pages %u identities %u linear %u",
                    capture->Owner.Token,bytes,capture->PageCount,capture->Identities,capture->Linear);
            if(Owner->HeapCaptures!=MAXULONG)Owner->HeapCaptures++;
            if(Device->Wddm)InterlockedIncrement64(&((BC250_WDDM*)Device->Wddm)->CaptureHeapTotal);
        }
        Build->MultipassOffset=capture->Owner.Token;
    } else {
        capture=(PAGING_GRAPH_CAPTURE*)PagingCaptureFind(Owner,Build->MultipassOffset);
        if(!capture || capture->Owner.Root!=Root || capture->Owner.Source!=src || capture->Owner.Destination!=dst ||
           capture->Owner.Bytes!=bytes || capture->Owner.Allocation!=Build->TransferVirtual.hAllocation ||
           capture->Owner.AllocationOffset!=Build->TransferVirtual.AllocationOffsetInBytes)return STATUS_INVALID_PARAMETER;
    }
    if(capture->Linear) {
        ULONGLONG app,appLength,table,tableLength,tablePhysical;
        if(!WddmMemoryLayout(Device,&app,&appLength,&table,&tableLength) ||
           table>MAXULONGLONG-(ULONGLONG)Device->VramPhysical.QuadPart)return STATUS_INVALID_PARAMETER;
        tablePhysical=(ULONGLONG)Device->VramPhysical.QuadPart+table;
        while(capture->Progress<bytes) {
            ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
            ULONG capacity=Build->DmaSize,written=0;ULONGLONG next;
            BC250_PAGING_COPY_SLICE slice;const BC250_PAGING_COPY_SLICE* commit=NULL;
            if(!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES){status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;}
            if(capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
            record[0]=0;
            status=GfxPagingEmitCapturedLinear(Device,capture,(PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,
                Build->DmaBufferWriteOffset,capacity,&written,&slice,&next);
            if(!NT_SUCCESS(status))break;
            if(!slice.DestinationSystem && slice.DestinationPhysical>=tablePhysical && slice.DestinationPhysical-tablePhysical<tableLength) {
                if(slice.Bytes>tableLength-(slice.DestinationPhysical-tablePhysical)){status=STATUS_INVALID_PARAMETER;break;}
                commit=&slice;
            }
            status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,commit,NULL);
            if(!NT_SUCCESS(status))break;
            capture->Progress=next;Build->DmaBufferWriteOffset+=written*4u;
        }
    }
    while(!capture->Linear && capture->Owner.Band<capture->BandCount) {
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        ULONG capacity=Build->DmaSize,written=0;
        PAGING_GRAPH_BATCH batch;unsigned nextBand,nextAction;
        if(!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        if(capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
            capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
        record[0]=0;
        status=GfxPagingEmitCapturedGraph(Device,capture,capture->Owner.Band,capture->Owner.Action,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,
            &written,&batch,&nextBand,&nextAction);
        if(!NT_SUCCESS(status))break;
        if(written) {
            status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,NULL,&batch);
            if(!NT_SUCCESS(status))break;
        }
        // Only an accepted exact batch advances the retained cursor. Keep filling
        // available capacity across band boundaries instead of forcing a retry.
        capture->Owner.Band=nextBand;capture->Owner.Action=nextAction;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    if(NT_SUCCESS(status) && (capture->Linear?capture->Progress==bytes:capture->Owner.Band==capture->BandCount)) {
        PAGING_CAPTURE* finished=PagingCaptureDetach(Owner,capture->Owner.Token);
        *Moved=bytes;Build->MultipassOffset=PAGING_CAPTURE_COMPLETE;
        if(!finished->ReservationBytes)ExFreePoolWithTag(finished,finished->PoolTag);
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

static NTSTATUS WddmBuildAperture(BC250_DEVICE* Device, DXGKARG_BUILDPAGINGBUFFER* Build, BOOLEAN Unmap)
{
    BC250_PAGING_APERTURE_OP operation;
    ULONG written=0,next=Build->MultipassOffset,capacity=Build->DmaSize;
    ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
    NTSTATUS status,publish;
    RtlZeroMemory(&operation,sizeof(operation));operation.Unmap=Unmap;
    if (Unmap) {
        if (Build->UnmapApertureSegment.SegmentId!=BC250_WDDM_SEGMENT_APERTURE) return STATUS_INVALID_PARAMETER;
        operation.FirstPage=Build->UnmapApertureSegment.OffsetInPages;
        operation.PageCount=Build->UnmapApertureSegment.NumberOfPages;
        operation.DummyPhysical=(ULONGLONG)Build->UnmapApertureSegment.DummyPage.QuadPart;
    } else {
        if (Build->MapApertureSegment.SegmentId!=BC250_WDDM_SEGMENT_APERTURE ||
            Build->MapApertureSegment.Flags.Reserved) return STATUS_INVALID_PARAMETER;
        operation.FirstPage=Build->MapApertureSegment.OffsetInPages;
        operation.PageCount=Build->MapApertureSegment.NumberOfPages;
        operation.Mdl=Build->MapApertureSegment.pMdl;operation.MdlOffset=Build->MapApertureSegment.MdlOffset;
        operation.CacheCoherent=(BOOLEAN)(Build->MapApertureSegment.Flags.CacheCoherent!=0);
    }
    if (!Build->pDmaBuffer) return STATUS_INVALID_PARAMETER;
    if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    record[0]=0;
    if (capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
        capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
    status=GfxPagingBuildAperture(Device,&operation,Build->MultipassOffset,
        (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,&written,&next);
    if (written) {
        publish=WddmPublishPagingRecord(Build,written,FALSE,Build->MultipassOffset,next);
        if (!NT_SUCCESS(publish)) return publish;
        Build->MultipassOffset=next;
    }
    return status;
}

static NTSTATUS WddmBuildPhysicalTransfer(BC250_DEVICE* Device, DXGKARG_BUILDPAGINGBUFFER* Build,
                                         ULONGLONG* Moved)
{
    BC250_PAGING_ENDPOINT source,destination;
    ULONGLONG progress,total=Build->Transfer.TransferSize;
    unsigned count;
    DXGKARG_BUILDPAGINGBUFFER prepare=*Build;
    BOOLEAN permutationResume=(BOOLEAN)((Build->MultipassOffset&PAGING_PERMUTATION_RESUME)!=0);
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    BOOLEAN overlap=FALSE,reverse=FALSE;
    NTSTATUS status=STATUS_SUCCESS;
    *Moved=0;
    if(permutationResume)prepare.MultipassOffset=0;
    if (!Build->pDmaBuffer || !WddmPreparePhysicalTransfer(Device,&prepare,&source,&destination,&progress) ||
        !PagingStreamTokenEncode(FALSE,source.Address,destination.Address,total,total,&count))
        return STATUS_INVALID_PARAMETER;
    // Indirect physical page lists can contain arbitrary cross-page alias cycles.
    // Whole-page graphs drain source readers before overwrites, then execute
    // remaining cycles or bounded swaps. Equal-offset partial ranges use byte
    // bands; unequal in-page offsets remain separate work.
    if ((source.Mdl || source.Aperture) && (destination.Mdl || destination.Aperture) &&
        (source.Aperture || destination.Aperture || source.Mdl!=destination.Mdl) && total>PAGE_SIZE) {
        BOOLEAN disjoint=FALSE;
        status=GfxPagingCheckDisjoint(Device,&source,&destination,total,&disjoint);
        if (!NT_SUCCESS(status)) return status;
        if (!disjoint) {
            ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
            ULONG capacity=Build->DmaSize,written=0;
            unsigned next=Build->MultipassOffset;
            NTSTATUS publish;
            // The tagged token identifies the next atomic group in the plan. No
            // scratch value is retained across BuildPagingBuffer calls.
            if (progress==total) return STATUS_SUCCESS;
            if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES)
                return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
            if (capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
                capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
            record[0]=0;
            status=GfxPagingBuildPageGraph(Device,&source,&destination,total,
                (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,originalOffset,capacity,Build->MultipassOffset,&written,&next);
            // Keep the existing refusal policy for unimplemented alias graphs;
            // the restricted DDI error contract remains tracked in the audit.
            if (status==STATUS_NOT_SUPPORTED) return STATUS_INVALID_PARAMETER;
            if (status!=STATUS_SUCCESS && status!=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER) return status;
            if (written) {
                publish=WddmPublishPagingRecord(Build,written,FALSE,0,0);
                if (!NT_SUCCESS(publish)) return publish;
            }
            // Count logical transfer bytes once its entire command plan has
            // been built. Intermediate swaps are not finalized source pages.
            *Moved=status==STATUS_SUCCESS?total:0;
            Build->MultipassOffset=next;
            return status;
        }
    }
    if (permutationResume) return STATUS_INVALID_PARAMETER;
    if (!source.Mdl && !destination.Mdl && !source.Aperture && !destination.Aperture) {
        overlap=source.Address<=destination.Address ? destination.Address-source.Address<total :
            source.Address-destination.Address<total;
        reverse=overlap && destination.Address>source.Address;
    }
    while (Build->MultipassOffset<count) {
        unsigned index=reverse ? count-1-Build->MultipassOffset : Build->MultipassOffset;
        ULONGLONG begin,end;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        ULONG capacity=Build->DmaSize,written=0;
        BC250_PAGING_COPY_SLICE slice;
        if (!PagingStreamTokenDecode(FALSE,source.Address,destination.Address,total,index,&begin) ||
            !PagingStreamTokenDecode(FALSE,source.Address,destination.Address,total,index+1,&end) ||
            end<=begin || end-begin>PAGE_SIZE) {status=STATUS_INVALID_PARAMETER;break;}
        if (!record || Build->DmaBufferPrivateDataSize<=PAGING_PRIVATE_HEADER_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        if (capacity>PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
            capacity=PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize);
        status=GfxPagingBuildCopyPageEx(Device,&source,&destination,begin,(ULONG)(end-begin),overlap,
            (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,Build->DmaBufferWriteOffset,capacity,&written,&slice);
        if (!NT_SUCCESS(status)) break;
        status=WddmPublishPagingRecordCore(Build,written,FALSE,0,0,0,0,0,0,0,0,
            Build->Transfer.Destination.SegmentId==BC250_WDDM_SEGMENT_TABLES ? &slice : NULL,NULL);
        if (!NT_SUCCESS(status)) break;
        Build->MultipassOffset++;*Moved+=slice.Bytes;
        Build->DmaBufferWriteOffset+=written*4u;
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
}

static DXGKDDI_BUILDPAGINGBUFFER Bc250WddmBuildPagingBuffer;
static NTSTATUS WddmBuildPagingBufferImpl(_In_ const HANDLE hAdapter, _In_ DXGKARG_BUILDPAGINGBUFFER* pBuildPagingBuffer)
{
    // The DDI has a restricted return contract. Unsupported-operation/error
    // handling is still tracked in m9-dma-contract-audit.md; empty SUCCESS is
    // not proof of a completed transfer or invalidation.
    BC250_WDDM* wddm = WddmOf(hAdapter);

    // Which operations VidMm asks for is the other half of stage A's evidence, and it decides what stage B has to
    // build first. All of them are counted; only the first few are logged.
    if (wddm != NULL) WddmNoteKind(wddm->PagingOps, (ULONG)pBuildPagingBuffer->Operation, &wddm->PagingOpsOverflow);
    // The first few calls of EACH operation, not of the DDI: a thousand page table updates come first (E16 run 005)
    // and would use up the DDI's allowance before the first aperture mapping shows.
    {
        static volatile LONG seen[32];
        ULONG operation = (ULONG)pBuildPagingBuffer->Operation;
        ULONG segment = 0;

        (void)WddmFirstCalls(wddm, WddmDdiBuildPagingBuffer);
        if (operation == DXGK_OPERATION_MAP_APERTURE_SEGMENT) segment = pBuildPagingBuffer->MapApertureSegment.SegmentId;
        else if (operation == DXGK_OPERATION_UNMAP_APERTURE_SEGMENT) segment = pBuildPagingBuffer->UnmapApertureSegment.SegmentId;
        if (wddm != NULL && operation < RTL_NUMBER_OF(seen) && InterlockedIncrement(&seen[operation]) <= 4 &&
            KeGetCurrentIrql() <= DISPATCH_LEVEL)
            GuardLog("wddm: BuildPagingBuffer operation %u segment %u, %u bytes free, pass offset %u", operation, segment,
                     pBuildPagingBuffer->DmaSize, pBuildPagingBuffer->MultipassOffset);
    }
    if (wddm != NULL && (pBuildPagingBuffer->Operation==DXGK_OPERATION_MAP_APERTURE_SEGMENT ||
                         pBuildPagingBuffer->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT)) {
        BOOLEAN unmap=pBuildPagingBuffer->Operation==DXGK_OPERATION_UNMAP_APERTURE_SEGMENT;
        ULONG before=pBuildPagingBuffer->MultipassOffset;
        NTSTATUS status=WddmBuildAperture((BC250_DEVICE*)hAdapter,pBuildPagingBuffer,unmap);
        if (pBuildPagingBuffer->MultipassOffset!=before) {
            if (unmap) InterlockedIncrement(&wddm->PagingUnmapsBuilt);
            else InterlockedIncrement(&wddm->PagingMapsBuilt);
        }
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
        return status;
    }
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_FILL) {
        ULONGLONG moved=0;
        NTSTATUS status=WddmBuildPhysicalFill((BC250_DEVICE*)hAdapter,pBuildPagingBuffer,&moved);
        if (moved) {
            InterlockedIncrement(&wddm->PagingFillsBuilt);
            InterlockedAdd64(&wddm->PagingBytesMoved,(LONG64)moved);
        }
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
        // The restricted DDI error policy is still audited; do not swallow refusal.
        return status;
    }
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_VIRTUAL_FILL) {
        BC250_WDDM_OBJECT* context=WddmObject(pBuildPagingBuffer->hSystemContext,BC250_WDDM_MAGIC_CONTEXT);
        ULONGLONG moved=0;
        NTSTATUS status=STATUS_NOT_SUPPORTED;
        if(WddmNativeAllocation((BC250_DEVICE*)hAdapter,context,pBuildPagingBuffer,TRUE))
            status=WddmBuildNativeVirtual((BC250_DEVICE*)hAdapter,context->RootPhysical,TRUE,pBuildPagingBuffer,&moved);
        if(status==STATUS_NOT_SUPPORTED)
            status=WddmBuildVirtualFill((BC250_DEVICE*)hAdapter,context?context->RootPhysical:0,pBuildPagingBuffer,&moved);
        if (moved) {
            InterlockedIncrement(&wddm->PagingFillsBuilt);
            InterlockedAdd64(&wddm->PagingBytesMoved,(LONG64)moved);
        }
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
        return status;
    }
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_VIRTUAL_TRANSFER) {
        BC250_WDDM_OBJECT* context=WddmObject(pBuildPagingBuffer->hSystemContext,BC250_WDDM_MAGIC_CONTEXT);
        ULONGLONG moved=0;
        NTSTATUS status=STATUS_NOT_SUPPORTED;
        if(WddmNativeAllocation((BC250_DEVICE*)hAdapter,context,pBuildPagingBuffer,FALSE))
            status=WddmBuildNativeVirtual((BC250_DEVICE*)hAdapter,context->RootPhysical,FALSE,pBuildPagingBuffer,&moved);
        if(status==STATUS_NOT_SUPPORTED)
            status=WddmBuildCapturedVirtualTransfer((BC250_DEVICE*)hAdapter,context?context->RootPhysical:0,
                context?&context->Captures:NULL,pBuildPagingBuffer,&moved);
        if (moved) {
            InterlockedIncrement(&wddm->PagingTransfersBuilt);
            InterlockedAdd64(&wddm->PagingBytesMoved,(LONG64)moved);
        }
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
        return status;
    }
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_TRANSFER) {
        ULONGLONG moved=0;
        NTSTATUS status=WddmBuildPhysicalTransfer((BC250_DEVICE*)hAdapter,pBuildPagingBuffer,&moved);
        if (moved) {
            InterlockedIncrement(&wddm->PagingTransfersBuilt);
            InterlockedAdd64(&wddm->PagingBytesMoved,(LONG64)moved);
        }
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
        return status;
    }
    // Paging-process CPU_VIRTUAL initialization must be immediate, including when
    // pDmaBuffer is NULL. GPU_PHYSICAL updates use the ordered path below.
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE &&
        pBuildPagingBuffer->UpdatePageTable.UpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL)
        VidMmUpdatePageTable(&pBuildPagingBuffer->UpdatePageTable);

    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES) {
        BC250_WDDM_OBJECT* context=WddmObject(pBuildPagingBuffer->hSystemContext,BC250_WDDM_MAGIC_CONTEXT);
        // Internal failure statuses remain part of the audited DDI error-policy
        // gap; never turn a refused nonempty copy into empty SUCCESS.
        return WddmBuildPagingCopies((BC250_DEVICE*)hAdapter,context ? context->RootPhysical : 0,pBuildPagingBuffer);
    }

    // Remaining GPU update/flush helpers retain the audited bootstrap/error policy.
    if (wddm != NULL && (pBuildPagingBuffer->Operation == DXGK_OPERATION_FLUSH_TLB ||
                         (pBuildPagingBuffer->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE &&
                          pBuildPagingBuffer->UpdatePageTable.UpdateMode == DXGK_PAGETABLEUPDATE_GPU_PHYSICAL)))
    {
        BOOLEAN update = pBuildPagingBuffer->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE;
        // DmaSize is remaining space; command offsets exclude private headers.
        ULONG dmaFree = pBuildPagingBuffer->DmaSize;
        ULONG privateFree = pBuildPagingBuffer->DmaBufferPrivateDataSize;
        ULONG* record = (ULONG*)pBuildPagingBuffer->pDmaBufferPrivateData;

        ULONG written = 0, nextByte = pBuildPagingBuffer->MultipassOffset;
        ULONG startByte = nextByte;
        BC250_WDDM_PAGING_UNSUPPORTED unsupported = BC250PagingSupported;
        NTSTATUS pagingStatus;
        if (record == NULL || privateFree <= PAGING_PRIVATE_HEADER_BYTES)
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        record[0]=0; // invalidate stale contents before emitting a new record
        if (dmaFree > PagingPrivateQueuedDirectCapacity(privateFree)) dmaFree=PagingPrivateQueuedDirectCapacity(privateFree);
        // Flush all addresses in the single WDDM application VMID, including
        // when the requested root is not currently bound. No root rewrite.
        if (update)
            pagingStatus = GfxPagingBuildUpdate((BC250_DEVICE*)hAdapter,&pBuildPagingBuffer->UpdatePageTable,
                (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,pBuildPagingBuffer->DmaBufferWriteOffset,
                dmaFree,startByte,&written,&nextByte,&unsupported);
        else
            pagingStatus = GfxPagingBuildFlush((BC250_DEVICE*)hAdapter,BC250_WDDM_VMID,
                (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,pBuildPagingBuffer->DmaBufferWriteOffset,
                dmaFree,&written,&unsupported);
        if (written != 0)
        {
            if (!NT_SUCCESS(WddmPublishPagingRecord(pBuildPagingBuffer,written,update,startByte,nextByte)))
                return STATUS_INVALID_PARAMETER; // inherited malformed-publication contract gap remains audited
            if (update) InterlockedIncrement(&wddm->PagingUpdatesBuilt);
            else InterlockedIncrement(&wddm->PagingFlushesBuilt);
            if (!update) InterlockedAdd64(&wddm->PagingBytesMoved, (LONG64)(nextByte - startByte));
        }
        else if (unsupported > BC250PagingSupported && unsupported < RTL_NUMBER_OF(wddm->PagingUnsupported))
        {
            InterlockedIncrement(&wddm->PagingUnsupported[unsupported]);
        }
        pBuildPagingBuffer->MultipassOffset = nextByte;
        if (pagingStatus == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
        {
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
            return pagingStatus; // submitted prefix is published above before asking for another buffer
        }
    }
    return STATUS_SUCCESS;
}

// Lock order: paging builder -> GfxPagingLock -> VidMm CPU update lock.
// PnP excludes StopDevice during this DDI; this lock also prevents map/unmap
// publication from changing an aperture identity between slices of one batch.
static NTSTATUS Bc250WddmBuildPagingBuffer(HANDLE hAdapter, DXGKARG_BUILDPAGINGBUFFER* Build)
{
    BC250_WDDM* wddm=WddmOf(hAdapter);
    NTSTATUS status;
    if (!wddm) return WddmBuildPagingBufferImpl(hAdapter,Build);
    KeEnterCriticalRegion();ExAcquirePushLockExclusive(&wddm->PagingBuildLock);
    if (Build->DmaBufferGpuVirtualAddress) {
        InterlockedIncrement64(&wddm->PagingDmaVaBuilds);
        if (Build->pDmaBuffer && wddm->PagingDmaVaObserved<8 &&
            Build->DmaBufferGpuVirtualAddress<=MAXULONGLONG-Build->DmaBufferWriteOffset) {
            BC250_WDDM_OBJECT* context=WddmObject(Build->hSystemContext,BC250_WDDM_MAGIC_CONTEXT);
            ULONGLONG pa=0,root=context?context->RootPhysical:0;
            ULONGLONG va=Build->DmaBufferGpuVirtualAddress+Build->DmaBufferWriteOffset;
            ULONGLONG cpu=(ULONGLONG)MmGetPhysicalAddress(Build->pDmaBuffer).QuadPart;
            BOOLEAN system=FALSE,mapped=VidMmTranslatePaging(root,va,&pa,&system);
            if(root) {
                InterlockedIncrement(&wddm->PagingDmaVaObserved);
                if(mapped && system && pa==cpu)InterlockedIncrement(&wddm->PagingDmaVaMatched);
                wddm->PagingDmaLastVa=va;wddm->PagingDmaLastRoot=root;
                wddm->PagingDmaLastPa=pa;wddm->PagingDmaLastCpu=cpu;
            }
            GuardLog("wddm: paging DMA mapping op%u VA0x%llX root0x%llX translated%u system%u PA0x%llX CPU_PA0x%llX match%u offset%u",
                (ULONG)Build->Operation,va,root,mapped,system,pa,cpu,
                mapped && system && pa==cpu,Build->DmaBufferWriteOffset);
        }
    } else InterlockedIncrement64(&wddm->PagingDmaZeroVaBuilds);
    status=WddmBuildPagingBufferImpl(hAdapter,Build);
    ExReleasePushLockExclusive(&wddm->PagingBuildLock);KeLeaveCriticalRegion();
    return status;
}

static NTSTATUS Bc250WddmSubmitCommandImpl(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SUBMITCOMMAND* pSubmitCommand)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = (context != NULL) ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;

    // This is the path a paging buffer takes only where the paging context addresses physically: hContext is NULL
    // there and NodeOrdinal names the node. On this machine it does not - VidMm creates node 1's system context
    // with VirtualAddressing set, and E24 run 006 measured this DDI as never called once in 110 seconds while
    // 1581 paging buffers were built (facts M110). Kept, and kept correct, because nothing in the contract
    // promises the virtual route is the only one. A failure return bugchecks (0x119, parameter 1 = 0x2), so
    // there is none.
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiSubmitCommand))
        GuardLog("wddm: SubmitCommand fence %u node %u flags 0x%08X segment %u size %u", pSubmitCommand->SubmissionFenceId,
                 node, pSubmitCommand->Flags.Value, pSubmitCommand->DmaBufferSegmentId, pSubmitCommand->DmaBufferSize);
    // Physical submission offsets and private-record offsets are different byte spaces.
    // Validate both before forming a pointer; the parser requires exact command coverage.
    if (node == BC250_WDDM_NODE_COPY && device->Wddm != NULL &&
        pSubmitCommand->DmaBufferSubmissionStartOffset < pSubmitCommand->DmaBufferSubmissionEndOffset &&
        pSubmitCommand->DmaBufferSubmissionEndOffset <= pSubmitCommand->DmaBufferSize &&
        pSubmitCommand->pDmaBufferPrivateData != NULL &&
        pSubmitCommand->DmaBufferPrivateDataSubmissionStartOffset < pSubmitCommand->DmaBufferPrivateDataSubmissionEndOffset &&
        pSubmitCommand->DmaBufferPrivateDataSubmissionEndOffset <= pSubmitCommand->DmaBufferPrivateDataSize &&
        WddmSubmitPagingHardware(device,(BC250_WDDM*)device->Wddm,
            (PUCHAR)pSubmitCommand->pDmaBufferPrivateData+pSubmitCommand->DmaBufferPrivateDataSubmissionStartOffset,
            pSubmitCommand->DmaBufferPrivateDataSubmissionEndOffset-pSubmitCommand->DmaBufferPrivateDataSubmissionStartOffset,
            pSubmitCommand->DmaBufferSubmissionStartOffset,
            pSubmitCommand->DmaBufferSubmissionEndOffset-pSubmitCommand->DmaBufferSubmissionStartOffset,
            FALSE,pSubmitCommand->SubmissionFenceId)) return STATUS_SUCCESS;
    if (pSubmitCommand->DmaBufferSubmissionStartOffset != pSubmitCommand->DmaBufferSubmissionEndOffset)
        WddmFailSubmission(device,pSubmitCommand->SubmissionFenceId,node);
    else WddmCompleteSoftware(device,pSubmitCommand->SubmissionFenceId,node);
    return STATUS_SUCCESS;
}

static DXGKDDI_SUBMITCOMMAND Bc250WddmSubmitCommand;
static NTSTATUS Bc250WddmSubmitCommand(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SUBMITCOMMAND* pSubmitCommand)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = context != NULL ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;
    BOOLEAN tracked = wddm != NULL && node < BC250_WDDM_NODE_COUNT_MAX;
    NTSTATUS status;
    KIRQL irql;
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        wddm->ActiveSubmissions[node]++;
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    status = Bc250WddmSubmitCommandImpl(hAdapter, pSubmitCommand);
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        wddm->ActiveSubmissions[node]--;
        KeReleaseSpinLock(&wddm->Lock, irql);
        WddmQueueReport(wddm);
    }
    return status;
}

// SubmitCommandVirtual permits STATUS_INVALID_PARAMETER for malformed private data.
// The OS faults that calling device. Ordered bookkeeping below retires its rejected
// fence only after previous work; no DMA_COMPLETED interrupt is invented for it.
static NTSTATUS WddmSubmitUmdImpl(_Inout_ BC250_DEVICE* Device, _In_opt_ BC250_WDDM* Wddm, _In_ const BC250_WDDM_OBJECT* Context,
                          _In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* Submit, UINT Node)
{
    struct umd_submit_view ib;
    unsigned umdLen = Submit->DmaBufferUmdPrivateDataSize;
    unsigned bufLen = Submit->DmaBufferPrivateDataSize;
    const void* bytes = Submit->pDmaBufferPrivateData;
    int st = UMD_BLOB_TOO_SMALL;
    const char* why = "unknown";
    unsigned nIbs = 0;

    ib.num_ibs = 0;
    ib.ib_va = 0;
    ib.ib_bytes = 0;
    ib.single_ib = 0;
    if (bytes != NULL && umdLen != 0 && umdLen <= bufLen)
        st = UmdBlobParseSubmit(bytes, umdLen, &ib);
    if (st != UMD_BLOB_OK || !ib.single_ib || Node != BC250_WDDM_NODE_3D)
    {
        GuardLog("wddm: malformed virtual UMD submission fence %u rejected (blob %d, single %d, node %u)",
                 Submit->SubmissionFenceId,st,ib.single_ib,Node);
        return STATUS_INVALID_PARAMETER;
    }

    // One IB is already the ring's whole capacity (gfx.c). A second UMD submit that arrives before
    // that fence - a present, or dxgkrnl pipelining two packets - must not be retired here: dxgkrnl
    // would signal the monitored fence for an IB the GPU never fetched. This DDI is PASSIVE_LEVEL,
    // so wait the same bound the watchdog uses, polling the fence, and try again. Anything else
    // (gate closed, ring abandoned, a bad blob) does not get that wait.
    if (st == UMD_BLOB_OK && ib.single_ib && Node == BC250_WDDM_NODE_3D && Context->RootPhysical != 0 &&
        Wddm != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        LARGE_INTEGER tick;
        ULONG waited = 0;
        ULONGLONG ibPhys = 0, ibLeaf = 0;
        BOOLEAN ibSystem = FALSE;
        BOOLEAN ibMapped;
        ULONG ibDw[BC250_IB_PROBE_DWORDS];

        // These CPU mappings and DRAM reads diagnose packet contents; they do not
        // validate or synchronize submission. Keep them optional for performance runs.
        if (Wddm->TraceUmdProbes)
        {
            LARGE_INTEGER probeStart = KeQueryPerformanceCounter(NULL);
            /* Before the ring packet. The winsys reads the BO through VidMm's CPU mapping. This
             * reads the system page the PTE names, at the IB's own offset, which is what the CP
             * fetches. The +32 and +56 lines are the M127 window. They are bytes in DRAM, not yet
             * a claim about which packet the CP executed. PASSIVE_LEVEL. */
            ibMapped = VidMmProbeIb(Context->RootPhysical, ib.ib_va, &ibLeaf, &ibPhys, &ibSystem, ibDw);
            GuardLog("wddm: umd ib 0x%llX %lu bytes -> %s 0x%llX%s leaf 0x%llX dram %08lX %08lX %08lX %08lX",
                     ib.ib_va, ib.ib_bytes, ibMapped ? "phys" : "UNMAPPED", ibPhys,
                     ibMapped ? (ibSystem ? " system" : " vram") : "", ibLeaf, ibDw[0], ibDw[1], ibDw[2], ibDw[3]);
            if (ibMapped && ibSystem && (ib.ib_va & (PAGE_SIZE - 1)) == 0)
            {
                ULONG slice;
                static const ULONG slices[] = { 32u, 56u, 158u, 166u, 176u, 184u, 192u, 200u, 208u, 216u, 224u, 232u };
                if (ib.ib_bytes > 32u * 4u)
                    GuardLog("wddm: umd ib +32 %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                             ibDw[32], ibDw[33], ibDw[34], ibDw[35], ibDw[36], ibDw[37], ibDw[38], ibDw[39]);
                if (ib.ib_bytes > 56u * 4u)
                    GuardLog("wddm: umd ib +56 %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                             ibDw[56], ibDw[57], ibDw[58], ibDw[59], ibDw[60], ibDw[61], ibDw[62], ibDw[63]);
                for (slice = 2; slice < sizeof(slices) / sizeof(slices[0]); slice++)
                {
                    ULONG at = slices[slice];
                    if (ib.ib_bytes < (at + 8u) * 4u || at + 8u > BC250_IB_PROBE_DWORDS)
                        continue;
                    GuardLog("wddm: umd ib +%lu %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX",
                             at, ibDw[at], ibDw[at + 1], ibDw[at + 2], ibDw[at + 3],
                             ibDw[at + 4], ibDw[at + 5], ibDw[at + 6], ibDw[at + 7]);
                }
                {
                    u32 lo = 0, hi = 0, hiWritten = 0, ndw;
                    u64 shaderVa = 0;
                    ULONGLONG sLeaf = 0, sPhys = 0;
                    BOOLEAN sSystem = FALSE, sMapped;

                    ndw = ib.ib_bytes / 4u;
                    if (ndw > BC250_IB_PROBE_DWORDS) ndw = BC250_IB_PROBE_DWORDS;
                    if (bc250_pm4_shader_addr((const u32 *)ibDw, ndw, &shaderVa, &lo, &hi, &hiWritten))
                    {
                        /* Reuses ibDw. The slices above have already been logged. */
                        sMapped = VidMmProbeIb(Context->RootPhysical, shaderVa, &sLeaf, &sPhys, &sSystem, ibDw);
                        GuardLog("wddm: shader 0x%llX lo %08X hi %08X%s -> %s 0x%llX%s leaf 0x%llX dram %08lX %08lX %08lX %08lX",
                                 shaderVa, lo, hi, hiWritten ? "" : " (hi not in packet)",
                                 sMapped ? "phys" : "UNMAPPED", sPhys,
                                 sMapped ? (sSystem ? " system" : " vram") : "", sLeaf,
                                 ibDw[0], ibDw[1], ibDw[2], ibDw[3]);
                    }
                }
            }

            InterlockedAdd64(&Wddm->UmdProbeTicks, KeQueryPerformanceCounter(NULL).QuadPart - probeStart.QuadPart);
            InterlockedIncrement(&Wddm->UmdProbeCalls);
        }

        tick.QuadPart = -10000ll * 10;
        for (;;)
        {
            if (GfxSubmitReady(Device) &&
                WddmSubmitHardware(Device, Wddm, Context, ib.ib_va, ib.ib_bytes, Submit->SubmissionFenceId, Node))
            {
                if (waited != 0)
                    GuardLog("wddm: umd submit fence %u waited %u ms for the gfx ring", Submit->SubmissionFenceId, waited);
                if (InterlockedIncrement(&Wddm->UmdSubmitHw) <= 128)
                    GuardLog("wddm: umd submit fence %u ib 0x%llX %lu bytes", Submit->SubmissionFenceId,
                             ib.ib_va, ib.ib_bytes);
                return STATUS_SUCCESS;
            }
            // Not busy: either the ring will not take an IB, or the one it held finished between
            // the ready check and this one. Try once more in the second case, and do not spin in
            // the first. A timeout is the same refusal the watchdog already makes.
            if (!GfxSubmitBusy(Device) || waited >= BC250_WDDM_SUBMIT_TIMEOUT_MS)
            {
                if (GfxSubmitReady(Device) &&
                    WddmSubmitHardware(Device, Wddm, Context, ib.ib_va, ib.ib_bytes, Submit->SubmissionFenceId, Node))
                {
                    if (waited != 0)
                        GuardLog("wddm: umd submit fence %u waited %u ms for the gfx ring",
                                 Submit->SubmissionFenceId, waited);
                    if (InterlockedIncrement(&Wddm->UmdSubmitHw) <= 128)
                        GuardLog("wddm: umd submit fence %u ib 0x%llX %lu bytes", Submit->SubmissionFenceId,
                                 ib.ib_va, ib.ib_bytes);
                    return STATUS_SUCCESS;
                }
                break;
            }
            WddmGpuFence(Device);
            KeDelayExecutionThread(KernelMode, FALSE, &tick);
            waited += 10;
        }
        if (waited != 0)
            GuardLog("wddm: umd submit fence %u waited %u ms for the gfx ring", Submit->SubmissionFenceId, waited);
    }
    if (st != UMD_BLOB_OK) why = UmdBlobStatusText(st);
    else if (!ib.single_ib) { why = "multiple ibs"; nIbs = ib.num_ibs; }
    else if (Node != BC250_WDDM_NODE_3D) why = "not node 0";
    else if (Context->RootPhysical == 0) why = "no root";
    else if (KeGetCurrentIrql() > APC_LEVEL) why = "irql";
    else if (!GfxSubmitReady(Device)) why = "ring not ready";
    else why = "ring refused";
    if (Wddm != NULL && InterlockedIncrement(&Wddm->UmdSubmitSoft) <= 128)
        GuardLog("wddm: umd submit fence %u not run: %s (%u ibs, private %u/%u, first 0x%08lX)",
                 Submit->SubmissionFenceId, why, nIbs, umdLen, bufLen, UmdBlobFirstWord(bytes, umdLen));
    WddmFailSubmission(Device, Submit->SubmissionFenceId, Node);
    return STATUS_SUCCESS;
}

static void WddmPresentBlit(BC250_WDDM_OBJECT* Context, const DXGKARG_PRESENT* Present);

// Aggregate host-side elapsed time, not GPU execution time. QPC ticks and frequency
// are logged separately so analysis needs no rounding or overflow-prone conversion.
static NTSTATUS WddmSubmitUmd(_Inout_ BC250_DEVICE* Device, _In_opt_ BC250_WDDM* Wddm,
                          _In_ const BC250_WDDM_OBJECT* Context,
                          _In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* Submit, UINT Node)
{
    LARGE_INTEGER start = KeQueryPerformanceCounter(NULL);
    NTSTATUS status = WddmSubmitUmdImpl(Device, Wddm, Context, Submit, Node);
    if (Wddm != NULL)
    {
        InterlockedAdd64(&Wddm->UmdSubmitTicks, KeQueryPerformanceCounter(NULL).QuadPart - start.QuadPart);
        InterlockedIncrement(&Wddm->UmdProfileCalls);
    }
    return status;
}

static NTSTATUS Bc250WddmSubmitCommandVirtualImpl(_In_ const HANDLE hAdapter,
                                              _In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* pSubmitCommand)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = (context != NULL) ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;
    BOOLEAN first;

    // The first few calls of each NODE, not of the DDI. Until E24 run 006 this logged the first few calls full
    // stop, and the desktop's own presents on node 0 spent that budget in the first five seconds of the run - so
    // the node-1 paging submissions that arrived a minute later left no trace whatsoever, and the summary's
    // "0 hardware submitted" could not be told apart from "dxgkrnl never submitted anything" (facts M110). The
    // same shape BuildPagingBuffer already uses for its own operations, and for the same reason.
    {
        static volatile LONG seen[BC250_WDDM_NODE_COUNT_MAX];
        LONG calls = (node < RTL_NUMBER_OF(seen)) ? InterlockedIncrement(&seen[node]) : 0;
        first = calls > 0 && calls <= BC250_WDDM_LOG_CALLS && KeGetCurrentIrql() <= DISPATCH_LEVEL;
    }
    (void)WddmFirstCalls(wddm, WddmDdiSubmitCommandVirtual);    // the DDI's own call count, logging aside
    if (wddm != NULL && node < BC250_WDDM_NODE_COUNT_MAX) InterlockedIncrement(&wddm->PagingVirtualSubmits[node]);
    if (first)
        GuardLog("wddm: SubmitCommandVirtual fence %u node %u flags 0x%08X va 0x%llX size %u",
                 pSubmitCommand->SubmissionFenceId, node, pSubmitCommand->Flags.Value,
                 (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress, pSubmitCommand->DmaBufferSize);

    // Node 1 retrieves commands only from this submission's OS-owned private buffer.
    // The record headers bind addresses to byte ranges; there is no global lookup/clamping.
    if (node == BC250_WDDM_NODE_COPY && wddm != NULL && wddm->NodeCount > BC250_WDDM_NODE_COPY &&
        context != NULL && !context->UmdContext && pSubmitCommand->DmaBufferUmdPrivateDataSize == 0 &&
        pSubmitCommand->DmaBufferSize != 0 && KeGetCurrentIrql() <= APC_LEVEL)
    {
        if (WddmSubmitPagingHardware(device,wddm,pSubmitCommand->pDmaBufferPrivateData,
                pSubmitCommand->DmaBufferPrivateDataSize,pSubmitCommand->DmaBufferVirtualAddress,
                pSubmitCommand->DmaBufferSize,TRUE,pSubmitCommand->SubmissionFenceId)) return STATUS_SUCCESS;
        InterlockedIncrement(&wddm->PagingVirtualUnmapped);
    }

    // M8. A UMD context's packet is the IB in its BC2S blob, not the DMA buffer a present uses. Handled
    // here, before stage C, so a UMD submit can never fall through onto DmaBufferVirtualAddress. One IB
    // takes the same gated gfx-ring path. Unsupported multi-IB packets are rejected before dispatch.
    if (context != NULL && context->UmdContext)
    {
        return WddmSubmitUmd(device, wddm, context, pSubmitCommand, node);
    }

    // Only driver-built, non-UMD present packets enter the CPU presentation path.
    // The scheduler has now selected this context's root and made its allocations
    // resident. Complete the fence only after all copied rows are visible.
    if (context != NULL && !context->UmdContext && pSubmitCommand->Flags.Present &&
        pSubmitCommand->DmaBufferUmdPrivateDataSize == 0 &&
        pSubmitCommand->pDmaBufferPrivateData != NULL &&
        pSubmitCommand->DmaBufferPrivateDataSize >= sizeof(BC250_PRESENT_PACKET))
    {
        const BC250_PRESENT_PACKET* packet = (const BC250_PRESENT_PACKET*)pSubmitCommand->pDmaBufferPrivateData;
        if (packet->Magic == BC250_PRESENT_PACKET_MAGIC && packet->RectCount != 0 && packet->RectCount <= BC250_PRESENT_PACKET_RECTS)
        {
            DXGKARG_PRESENT present;
            RtlZeroMemory(&present, sizeof(present));
            present.Flags.Blt = 1;
            present.pAllocationInfo = (DXGK_PRESENTALLOCATIONINFO*)packet->Allocations;
            present.SrcRect = packet->SrcRect;
            present.DstRect = packet->DstRect;
            present.SubRectCnt = packet->RectCount;
            present.pDstSubRects = packet->Rects;
            WddmPresentBlit(context, &present);
            KeMemoryBarrier();
            WddmCompleteSoftware(device, pSubmitCommand->SubmissionFenceId, node);
            return STATUS_SUCCESS;
        }
    }

    // E26 packets are CPU commands, never GFX IBs, even if malformed.
    if (pSubmitCommand->Flags.Present)
    {
        GuardLog("wddm: malformed virtual present rejected");
        return STATUS_INVALID_PARAMETER;
    }

    // Stage C. An empty DMA buffer (every Present of stage A's inert DDI) has nothing to run; one with bytes in it
    // goes to the ring if the GPU is up (EnableGpuSubmit, stage 8, IH) and the context has a root.
    // A refused nonempty command remains outstanding for recovery; only empty work completes in software.
    // node == BC250_WDDM_NODE_3D: this hardware path is GfxSubmitIb's, the gfx ring at a fixed VMID, and it stays
    // node 0's alone - node 1 has its own arm above, its own ring (SDMA0, no VMID) and its own failure counters.
    if (node == BC250_WDDM_NODE_3D && pSubmitCommand->DmaBufferSize != 0 && context != NULL && context->RootPhysical != 0 &&
        device->Wddm != NULL && KeGetCurrentIrql() <= APC_LEVEL && GfxSubmitReady(device) &&
        WddmSubmitHardware(device, (BC250_WDDM*)device->Wddm, context, (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress,
                           pSubmitCommand->DmaBufferSize, pSubmitCommand->SubmissionFenceId, node))
        return STATUS_SUCCESS;
    if (pSubmitCommand->DmaBufferSize != 0)
        WddmFailSubmission(device,pSubmitCommand->SubmissionFenceId,node);
    else WddmCompleteSoftware(device,pSubmitCommand->SubmissionFenceId,node);
    return STATUS_SUCCESS;
}

static DXGKDDI_SUBMITCOMMANDVIRTUAL Bc250WddmSubmitCommandVirtual;
static NTSTATUS Bc250WddmSubmitCommandVirtual(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* pSubmitCommand)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = context != NULL ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;
    BOOLEAN tracked = wddm != NULL && node < BC250_WDDM_NODE_COUNT_MAX;
    NTSTATUS status;
    KIRQL irql;
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        wddm->ActiveSubmissions[node]++;
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    status = Bc250WddmSubmitCommandVirtualImpl(hAdapter, pSubmitCommand);
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        if (status == STATUS_INVALID_PARAMETER)
        {
            if (!wddm->RejectedPending[node] ||
                (LONG)(pSubmitCommand->SubmissionFenceId-wddm->RejectedFence[node]) > 0)
                wddm->RejectedFence[node]=pSubmitCommand->SubmissionFenceId;
            wddm->RejectedPending[node]=TRUE;
        }
        wddm->ActiveSubmissions[node]--;
        KeReleaseSpinLock(&wddm->Lock, irql);
        WddmQueueReport(wddm);
    }
    return status;
}

static DXGKDDI_PREEMPTCOMMAND Bc250WddmPreemptCommand;
static NTSTATUS Bc250WddmPreemptCommand(_In_ const HANDLE hAdapter, _In_ const DXGKARG_PREEMPTCOMMAND* pPreemptCommand)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);

    // Defer notification until the submitted DMA buffer has retired and its completion
    // has been published. Immediate idle reporting was only valid for stage A.
    if (WddmFirstCalls(wddm, WddmDdiPreemptCommand))
        GuardLog("wddm: PreemptCommand fence %u node %u", pPreemptCommand->PreemptionFenceId, pPreemptCommand->NodeOrdinal);
    WddmPreemptFence(device, pPreemptCommand->PreemptionFenceId, pPreemptCommand->NodeOrdinal);
    return STATUS_SUCCESS;
}

static DXGKDDI_RESETFROMTIMEOUT Bc250WddmResetFromTimeout;
static NTSTATUS Bc250WddmResetFromTimeout(_In_ const HANDLE hAdapter)
{
    BC250_DEVICE* device=(BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm=WddmOf(hAdapter);
    (void)WddmFirstCalls(wddm,WddmDdiResetFromTimeout);
    if (device!=NULL) { GfxSubmitFail(device); GfxPagingSubmitFail(device); }
    // Microsoft requires all GPU memory access stopped before a successful return.
    // Closing software gates does not prove that. Preserve outstanding fences and
    // return failure so Windows follows its failed-TDR path instead of freeing
    // memory under an engine this driver has not actually stopped. Recovery that
    // can return success still needs a verified hardware halt/reset implementation.
    GuardLog("wddm: ResetFromTimeout failed: GPU memory access not proven stopped");
    return STATUS_UNSUCCESSFUL;
}

static DXGKDDI_RESTARTFROMTIMEOUT Bc250WddmRestartFromTimeout;
static NTSTATUS Bc250WddmRestartFromTimeout(_In_ const HANDLE hAdapter)
{
    (void)WddmFirstCalls(WddmOf(hAdapter), WddmDdiRestartFromTimeout);
    GuardLog("wddm: *** RestartFromTimeout: the adapter is being restarted after a timeout ***");
    return STATUS_SUCCESS;       // "can simply return STATUS_SUCCESS immediately"
}

// Per-engine TDR. Not because this part can reset an engine - it cannot reset anything (facts M53) - but because
// Microsoft documents SupportPerEngineTDR as mandatory for a full graphics driver that claims WDDM 1.2 or later,
// and saying it obliges the table to carry all three (the lab's dxgkrnl refuses the cap without the DDIs).
static DXGKDDI_QUERYDEPENDENTENGINEGROUP Bc250WddmQueryDependentEngineGroup;
static NTSTATUS Bc250WddmQueryDependentEngineGroup(_In_ const HANDLE hAdapter,
                                                   _Inout_ DXGKARG_QUERYDEPENDENTENGINEGROUP* pQueryDependentEngineGroup)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT nodeCount = (wddm != NULL) ? wddm->NodeCount : BC250_WDDM_NODE_COUNT;

    if (WddmFirstCalls(wddm, WddmDdiQueryDependentEngineGroup))
        GuardLog("wddm: QueryDependentEngineGroup node %u engine %u", pQueryDependentEngineGroup->NodeOrdinal,
                 pQueryDependentEngineGroup->EngineOrdinal);
    // ADR 0008 stage D: the bound is wddm->NodeCount, not the compile-time constant (design note section 6).
    if (pQueryDependentEngineGroup->NodeOrdinal >= nodeCount) return STATUS_INVALID_PARAMETER;
    // A reset of a node affects that node and nobody else: node 1 has its own hardware channel, independent of
    // node 0's (section 5), so neither name is in the other's dependent mask.
    pQueryDependentEngineGroup->DependentNodeOrdinalMask = 1ull << pQueryDependentEngineGroup->NodeOrdinal;
    return STATUS_SUCCESS;
}

static DXGKDDI_QUERYENGINESTATUS Bc250WddmQueryEngineStatus;
static NTSTATUS Bc250WddmQueryEngineStatus(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_QUERYENGINESTATUS* pQueryEngineStatus)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT nodeCount = (wddm != NULL) ? wddm->NodeCount : BC250_WDDM_NODE_COUNT;

    if (WddmFirstCalls(wddm, WddmDdiQueryEngineStatus))
        GuardLog("wddm: QueryEngineStatus node %u engine %u", pQueryEngineStatus->NodeOrdinal,
                 pQueryEngineStatus->EngineOrdinal);
    if (pQueryEngineStatus->NodeOrdinal >= nodeCount) return STATUS_INVALID_PARAMETER;
    // Report the fault state instead of declaring a failed node responsive.
    pQueryEngineStatus->EngineStatus.Value = 0;
    if (wddm != NULL) {
        KIRQL irql;
        KeAcquireSpinLock(&wddm->Lock,&irql);
        pQueryEngineStatus->EngineStatus.Responsive = !wddm->WatchdogFaulted[pQueryEngineStatus->NodeOrdinal];
        KeReleaseSpinLock(&wddm->Lock,irql);
    } else pQueryEngineStatus->EngineStatus.Responsive = 1;
    return STATUS_SUCCESS;
}

static DXGKDDI_RESETENGINE Bc250WddmResetEngine;
static NTSTATUS Bc250WddmResetEngine(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_RESETENGINE* pResetEngine)
{
    // The documented answer of hardware that "is incapable of resetting the nodes": a failure status, after which
    // the scheduler falls back to the adapter-wide ResetFromTimeout. It is also the careful answer: a success
    // with a LastAbortedFenceId outside [last completed, last submitted] is bugcheck 0x119, and a failure names
    // no fence at all. Logged on every call, like the two timeout DDIs and for the same reason.
    (void)WddmFirstCalls(WddmOf(hAdapter), WddmDdiResetEngine);
    GuardLog("wddm: *** ResetEngine node %u engine %u: refused, this part has no engine reset ***",
             pResetEngine->NodeOrdinal, pResetEngine->EngineOrdinal);
    return STATUS_NOT_SUPPORTED;
}

// "A driver that supports these functions [per-engine TDR] must also support level zero synchronization for
// DxgkDdiCollectDbgInfo". Level Zero means it may run beside anything, a hanging adapter included, so this body
// takes no lock, reads no device state and does not even log: it has nothing to add to a debug report (stage A
// runs no engine). The structure has no "bytes written" member, so "nothing" has to be said with zeroes: on
// success dxgkrnl takes the whole buffer into the report, and what was in it before is not ours to publish.
// The call counter is a file-scope variable for the same reason the body stays away from Device->Wddm: WddmStop
// frees that block, and this DDI may run beside it (g_CollectDbgInfoCalls, declared with the DDI names).
static DXGKDDI_COLLECTDBGINFO Bc250WddmCollectDbgInfo;
static NTSTATUS Bc250WddmCollectDbgInfo(_In_ const HANDLE hAdapter, _In_ const DXGKARG_COLLECTDBGINFO* pCollectDbgInfo)
{
    UNREFERENCED_PARAMETER(hAdapter);
    (void)InterlockedIncrement(&g_CollectDbgInfoCalls);
    if (pCollectDbgInfo->pBuffer != NULL && pCollectDbgInfo->BufferSize != 0)
        RtlZeroMemory(pCollectDbgInfo->pBuffer, pCollectDbgInfo->BufferSize);
    return STATUS_SUCCESS;
}

// ---- clock and history buffer -------------------------------------------------------------------------------------

// Required next to CalibrateGpuClock: the lab's dxgkrnl (10.0.22621.6199) refuses a render adapter whose table is
// "compiled against WDDM2_0_M2_2_1 or greater, but does not fill in the pfnCalibrateGpuClock or
// pfnSetStablePowerState DDI" (facts M64; E16 run 003 stopped at a point consistent with that check, which is
// not the same as having seen it fail). What it asks for - clocks
// that do not move while a profiler looks - is what this part has anyway: one fixed clock, set once by the startup
// task (1000 MHz), and no power management in the driver. So there is nothing to do, and the DDI returns nothing.
static DXGKDDI_SETSTABLEPOWERSTATE Bc250WddmSetStablePowerState;
static VOID Bc250WddmSetStablePowerState(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETSTABLEPOWERSTATE* pArgs)
{
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiSetStablePowerState))
        GuardLog("wddm: SetStablePowerState enabled %u (clocks are fixed: nothing to do)", pArgs->Enabled ? 1u : 0u);
}

static DXGKDDI_CALIBRATEGPUCLOCK Bc250WddmCalibrateGpuClock;
static NTSTATUS Bc250WddmCalibrateGpuClock(_In_ const HANDLE hAdapter, _In_ UINT32 NodeOrdinal, _In_ UINT32 EngineOrdinal,
                                           _Out_ DXGKARG_CALIBRATEGPUCLOCK* pClockCalibration)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter = KeQueryPerformanceCounter(&frequency);
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT nodeCount = (wddm != NULL) ? wddm->NodeCount : BC250_WDDM_NODE_COUNT;

    UNREFERENCED_PARAMETER(EngineOrdinal);
    // ADR 0008 stage D: node 1 accepted once wddm->NodeCount says it exists (design note section 6); its answer
    // is the same shape as node 0's, below - the CPU counter, since node 1 has no GPU timestamp of its own either.
    if (NodeOrdinal != BC250_WDDM_NODE_3D && !(NodeOrdinal == BC250_WDDM_NODE_COPY && nodeCount > BC250_WDDM_NODE_COPY))
        return STATUS_INVALID_PARAMETER;
    // Stage A's engine is the CPU, so its clock is the CPU's: one counter, read once, reported as both. A GPU
    // timestamp of our own arrives with the first real submission.
    RtlZeroMemory(pClockCalibration, sizeof(*pClockCalibration));
    pClockCalibration->GpuFrequency = (ULONGLONG)frequency.QuadPart;
    pClockCalibration->GpuClockCounter = (ULONGLONG)counter.QuadPart;
    pClockCalibration->CpuClockCounter = (ULONGLONG)counter.QuadPart;
    if (WddmFirstCalls(wddm, WddmDdiCalibrateGpuClock))
        GuardLog("wddm: CalibrateGpuClock node %u, frequency %llu", NodeOrdinal, pClockCalibration->GpuFrequency);
    return STATUS_SUCCESS;
}

static DXGKDDI_FORMATHISTORYBUFFER Bc250WddmFormatHistoryBuffer;
static NTSTATUS Bc250WddmFormatHistoryBuffer(_In_ const HANDLE hContext, _In_ DXGKARG_FORMATHISTORYBUFFER* pFormatData)
{
    BC250_WDDM_OBJECT* context = WddmObject(hContext, BC250_WDDM_MAGIC_CONTEXT);

    // HISTORYBUFFERPRECISION answers zero bits, so this should never be reached. If it is, that is worth knowing:
    // there is nothing to format, and nothing is written into the formatted buffer.
    if (context != NULL && WddmFirstCalls((BC250_WDDM*)context->Device->Wddm, WddmDdiFormatHistoryBuffer))
        GuardLog("wddm: FormatHistoryBuffer %u timestamps, %u bits", pFormatData->NumTimestamps,
                 pFormatData->Precision.PrecisionBits);
    return STATUS_SUCCESS;
}

// ---- display: present and flip --------------------------------------------------------------------------------------

// E20, H3 (ADR 0011: a diagnostic, never the present path of a game). A Blt present of a GDI context names its
// source at DXGK_PRESENT_SOURCE_INDEX of pAllocationList (facts M82): our allocation object, once OpenAllocation
// hands it out, and the surface's GPU virtual address in the context's address space. The source is translated
// through the context's root (first and last byte: a VRAM allocation is one contiguous range of the segment, so the
// two must be Size - 1 apart, and the walk proves the tables rather than assuming them), mapped read-only by physical
// address for this one call - never through BAR0 (facts M32) - and its sub-rectangles are copied into the firmware
// framebuffer the way display.c's CopyRect does it: clamped to the mode, 4 bytes a pixel, no scaling. Nothing is
// cached, so no lifetime is shared with DestroyAllocation, and nothing is written except the framebuffer.
// PASSIVE_LEVEL only (MmUnmapIoSpace); a present is always answered STATUS_SUCCESS whatever happens here.
// The gate's scope (review 16): reading the list entry, looking the handle up and translating the source run for
// every Blt present under the full table with EnableGpuVa, gate or no gate, so that a run with the gate closed
// measures the translation; EnablePresentBlit guards only the mapping and the copy.
static BOOLEAN WddmLinearColorFormat(ULONG Format)
{
    return Format == D3DDDIFMT_A8R8G8B8 || Format == D3DDDIFMT_X8R8G8B8 ||
           Format == D3DDDIFMT_A8B8G8R8 || Format == D3DDDIFMT_X8B8G8R8;
}

static BOOLEAN WddmRedFirst(ULONG Format)
{
    return Format == D3DDDIFMT_A8B8G8R8 || Format == D3DDDIFMT_X8B8G8R8;
}

static void WddmCopyColorRow(UCHAR* Dst, const UCHAR* Src, SIZE_T Bytes, BOOLEAN SwapRB)
{
    SIZE_T i;
    if (!SwapRB) { RtlCopyMemory(Dst, Src, Bytes); return; }
    for (i = 0; i < Bytes; i += 4)
    {
        Dst[i] = Src[i + 2]; Dst[i + 1] = Src[i + 1];
        Dst[i + 2] = Src[i]; Dst[i + 3] = Src[i + 3];
    }
}

// Copy visible pixels row by row: firmware and primary may have different padding.
static BOOLEAN WddmCopyScanoutRows(PVOID Destination, SIZE_T DestinationBytes, ULONG DestinationPitch,
                                  const void* Source, SIZE_T SourceBytes, ULONG SourcePitch,
                                  ULONG Width, ULONG Height)
{
    ULONGLONG srcBytes,dstBytes;
    ULONG row;
    if (!DcnSurfaceBytes(Width,Height,SourcePitch,&srcBytes) || srcBytes>SourceBytes ||
        !DcnSurfaceBytes(Width,Height,DestinationPitch,&dstBytes) || dstBytes>DestinationBytes) return FALSE;
    for (row=0;row<Height;row++)
        RtlCopyMemory((UCHAR*)Destination+(SIZE_T)row*DestinationPitch,
            (const UCHAR*)Source+(SIZE_T)row*SourcePitch,(SIZE_T)Width*4);
    return TRUE;
}

static void WddmPresentBlit(_In_ BC250_WDDM_OBJECT* Context, _In_ const DXGKARG_PRESENT* Present)
{
    BC250_DEVICE* device = Context->Device;
    BC250_WDDM* wddm = (BC250_WDDM*)device->Wddm;
    const ULONGLONG* raw;               // the list, read as qwords: both arms of the union are tried below
    HANDLE handle = NULL;
    BC250_WDDM_OBJECT* object = NULL;
    int reading = 0;
    const BC250_WDDM_ALLOCATION_PRIVATE* alloc = NULL;
    ULONGLONG va = 0, first = 0, last = 0;
    BOOLEAN systemFirst = FALSE, systemLast = FALSE, verbose;
    const UCHAR* map;
    PHYSICAL_ADDRESS physical;
    RECT whole;
    const RECT* rects;
    UINT count, i, rows = 0;
    LONG dx, dy;                        // destination to source offset (no scaling: the rectangles are the same size)
    const char* why = NULL;
    UCHAR* dst;                          // where the copy lands: Device->Framebuffer, or the flipped surface below
    SIZE_T dstLength;
    BOOLEAN toFlip = FALSE, mapFailed = FALSE;    // for the counters and the log line at the end
    PVOID destinationMap = NULL;
    BOOLEAN destinationPrimary = FALSE, destinationRedFirst = FALSE;
    UINT dstPitch = device->Post.Pitch, dstWidth = device->Post.Width, dstHeight = device->Post.Height;


    verbose = (wddm->Calls[WddmDdiPresent] <= BC250_WDDM_LOG_CALLS);
    if (Present->pAllocationList == NULL || Context->AllocationListSize <= DXGK_PRESENT_MAX_INDEX) why = "no list";
    else if (KeGetCurrentIrql() != PASSIVE_LEVEL) why = "IRQL";
    else if (Context->RootPhysical == 0) why = "context has no root";
    else if (device->Framebuffer == NULL || device->FramebufferLength == 0) why = "no framebuffer";
    else if (Present->DstRect.right - Present->DstRect.left != Present->SrcRect.right - Present->SrcRect.left ||
             Present->DstRect.bottom - Present->DstRect.top != Present->SrcRect.bottom - Present->SrcRect.top) why = "scaled";
    if (why != NULL)
    {
        if (InterlockedIncrement(&wddm->BlitSkips) <= BC250_WDDM_LOG_CALLS) GuardLog("wddm: blit skipped: %s", why);
        return;
    }

    // The entry itself was read under __try by 0.7.15 in every logged present (facts M82: readable, handle 0 = what
    // OpenAllocation stored then); the value in it is still not dereferenced, only looked up (review 16).
    // The object found is used after the lock is released: a present's allocation holds dxgkrnl's reference for the
    // length of the call and CloseAllocation comes only once nothing references it (review 17) - and the blob it
    // carries came through OpenAllocation's private data, whose bounds the copy below checks on its own anyway.
    // E20 run 005 (0.7.17, handles handed out): the 24-byte reading's slot 1 still carried handle 0 with 0x8DC000 in
    // its third qword. Under the other arm of the union - 32-byte DXGK_PRESENTALLOCATIONINFO entries - those same
    // qwords are entry 1's handle and virtual address, and entry 0 (qwords 0..3) was never looked at. So both arms
    // are tried, by lookup only: (1) 32-byte entry 0, (2) 24-byte entry 1, (3) 32-byte entry 1; the first whose
    // handle is an object we opened wins, and the reading is logged with the result. The 96 bytes lie in the
    // pointer's page (checked by the caller) and were readable in every present so far.
    raw = (const ULONGLONG*)Present->pAllocationList;
    C_ASSERT(sizeof(DXGK_PRESENTALLOCATIONINFO) == 4 * sizeof(ULONGLONG));
    C_ASSERT(FIELD_OFFSET(DXGK_PRESENTALLOCATIONINFO, AllocationVirtualAddress) == sizeof(ULONGLONG));
    C_ASSERT(FIELD_OFFSET(DXGK_ALLOCATIONLIST, VirtualAddress) == 2 * sizeof(ULONGLONG));
    {
        // M83: 32-byte entry 1 is the source (handle at qword 4, VA at qword 5). Try it before entry 0
        // and before the 24-byte misreading, so a present that also names some other allocation of ours
        // in entry 0 still blits the source. `reading` stays 1, 2, 3 for those three slots.
        static const UINT candidates[3][2] = { { 0, 1 }, { 3, 5 }, { 4, 5 } };
        static const int prefer[3] = { 2, 0, 1 };
        int c, pi;

        for (pi = 0; pi < 3 && object == NULL; pi++)
        {
            c = prefer[pi];
            handle = (HANDLE)(ULONG_PTR)raw[candidates[c][0]];
            object = WddmListedObject(wddm, handle, BC250_WDDM_MAGIC_OPENED);
            va = raw[candidates[c][1]];
            reading = c + 1;
        }
    }
    if (object == NULL) why = "no slot names an allocation we opened";
    else
    {
        alloc = &object->Allocation;
        if (!WddmLinearColorFormat(alloc->Format)) why = "source format";
        else if (alloc->Width == 0 || alloc->Height == 0 || alloc->Pitch < alloc->Width * 4ull ||
                 alloc->Size < (ULONGLONG)alloc->Pitch * alloc->Height || alloc->Size > 0x10000000ull) why = "source geometry";
        else if (va > MAXULONGLONG - alloc->Size) why = "source VA overflow";
        else if ((va & (PAGE_SIZE - 1)) != 0) why = "source VA not page aligned";
    }
    if (why == NULL)
    {
        if (!VidMmTranslate(Context->RootPhysical, va, &first, &systemFirst) ||
            !VidMmTranslate(Context->RootPhysical, va + alloc->Size - 1, &last, &systemLast)) why = "source VA does not translate";
        else if (systemFirst || systemLast) why = "source in system memory";
        else if (last - first != alloc->Size - 1) why = "source not contiguous";
        else InterlockedIncrement(&wddm->BlitTranslations);
        if (verbose && object != NULL)
            GuardLog("wddm: blit source (reading %d) %p %ux%u pitch %u format %u size 0x%llX va 0x%llX -> 0x%llX .. 0x%llX%s%s", reading,
                     (void*)object, alloc->Width, alloc->Height, alloc->Pitch, alloc->Format, alloc->Size, va, first, last,
                     (systemFirst || systemLast) ? " SYSTEM" : "", why != NULL ? " REFUSED" : "");
    }
    if (why != NULL)
    {
        if (InterlockedIncrement(&wddm->BlitSkips) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: blit skipped: %s (reading %d handle %p va 0x%llX)", why, reading, handle, va);
        return;
    }
    if (!wddm->BlitGate) return;        // translation checked and counted; the copy itself needs the gate

    physical.QuadPart = (LONGLONG)first;
    map = (const UCHAR*)VramMapCpuRange(device,physical,(SIZE_T)alloc->Size,PAGE_READONLY);
    if (map == NULL)
    {
        if (InterlockedIncrement(&wddm->BlitSkips) <= BC250_WDDM_LOG_CALLS) GuardLog("wddm: blit skipped: no mapping for 0x%llX", first);
        return;
    }

    // Where the picture goes (2026-09-22, ADR 0011 consequences, facts M100): Device->Framebuffer - the POST
    // framebuffer - only until a flip has actually moved HUBP0 off it. Once VidPnFlipEnabled and DcnDiverged
    // are both true, nothing scans the POST framebuffer out any more, so the copy has to land at
    // Device->DcnCurrentAddress instead (dcn.c's DcnScanoutMapping, PASSIVE_LEVEL, guaranteed by the IRQL
    // check above). A mapping failure falls back to the POST framebuffer rather than dropping the present -
    // the picture is still produced somewhere the log can point at, never skipped silently - and is counted
    // apart from the ordinary gate-closed/pre-flip case so the two reasons stay distinguishable.
    dst = (UCHAR*)device->Framebuffer;
    dstLength = device->FramebufferLength;
    if (device->VidPnFlipEnabled && device->DcnDiverged)
    {
        PVOID flipMap = NULL;
        SIZE_T flipLength = 0;
        ULONG flipPitch=0;

        if (DcnScanoutMapping(device, &flipMap, &flipLength, &flipPitch))
        {
            dst = (UCHAR*)flipMap;
            dstLength = flipLength;
            dstPitch = flipPitch;
            toFlip = TRUE;
        }
        else
        {
            mapFailed = TRUE;
        }
    }

    // E26: entry 2 can be a window's redirection surface, not the scanout.
    // Honour its VA and geometry. Only the actual primary is mirrored to the
    // firmware framebuffer while the hardware flip gate remains closed.
    if (raw[8] != 0)
    {
        BC250_WDDM_OBJECT* destination = WddmListedObject(wddm, (HANDLE)(ULONG_PTR)raw[8], BC250_WDDM_MAGIC_OPENED);
        ULONGLONG dstFirst = 0, dstLast = 0, primaryPhysical = 0;
        BOOLEAN dstSystemFirst = FALSE, dstSystemLast = FALSE;
        const BC250_WDDM_ALLOCATION_PRIVATE* d = destination ? &destination->Allocation : NULL;
        if (d == NULL || d->Width == 0 || d->Height == 0 || d->Pitch < d->Width * 4ull ||
            d->Size < (ULONGLONG)d->Pitch * d->Height || d->Size > 0x10000000ull ||
            !WddmLinearColorFormat(d->Format) ||
            raw[9] > MAXULONGLONG - d->Size ||
            !VidMmTranslate(Context->RootPhysical, raw[9], &dstFirst, &dstSystemFirst) ||
            !VidMmTranslate(Context->RootPhysical, raw[9] + d->Size - 1, &dstLast, &dstSystemLast) ||
            dstSystemFirst || dstSystemLast || dstLast < dstFirst || dstLast - dstFirst != d->Size - 1 ||
            (dstFirst < last + 1 && first < dstLast + 1))
        {
            InterlockedIncrement(&wddm->BlitSkips);
            GuardLog("wddm: blit refused destination geometry, mapping or overlap");
            MmUnmapIoSpace((void*)map, (SIZE_T)alloc->Size);
            return;
        }
        physical.QuadPart = (LONGLONG)dstFirst;
        destinationMap = VramMapCpuRange(device,physical,(SIZE_T)d->Size,PAGE_READWRITE);
        if (destinationMap == NULL)
        {
            InterlockedIncrement(&wddm->BlitSkips);
            MmUnmapIoSpace((void*)map, (SIZE_T)alloc->Size);
            return;
        }
        dst = (UCHAR*)destinationMap;
        dstLength = (SIZE_T)d->Size;
        dstPitch = d->Pitch; dstWidth = d->Width; dstHeight = d->Height;
        destinationRedFirst = WddmRedFirst(d->Format);
        destinationPrimary = DcnTranslateCardAddress((ULONGLONG)wddm->PrimaryAddress.QuadPart,
            device->VramMcBase, (ULONGLONG)device->VramPhysical.QuadPart,
            device->VramLength, &primaryPhysical) && primaryPhysical == dstFirst;
        toFlip = FALSE; // this mapping follows the requested destination, not the diagnostic flip fallback
        if (verbose) GuardLog("wddm: blit destination %ux%u pitch %u va 0x%llX physical 0x%llX %s",
            dstWidth, dstHeight, dstPitch, raw[9], dstFirst, destinationPrimary ? "primary" : "offscreen");
    }

    // M115: dirty sub-rectangles assume the destination already holds the previous frame. The firmware
    // framebuffer does (M84). A flip target does not. M116: the source allocation does not either, outside
    // the rectangles GDI just wrote. M117: copying Device->Framebuffer, the write-combined BAR0 mapping
    // (Post.PhysicAddress 0xC0000000, M20), produced a frame that was 90% zero. M32: a CPU read of BAR0 is
    // not a coherent view of VRAM. The address HUBP was scanning before this flip is DcnFirmwareAddress,
    // captured from the register; M92 measured that as the carve-out base, which is VramPhysical (M31).
    // Seed from that physical address, once per flip target, then the dirty rectangles on top.
    if (toFlip && device->VramEnabled && device->DcnScanoutSeedAddress != device->DcnScanoutMapAddress &&
        device->FramebufferLength != 0 && device->VramLength != 0)
    {
        ULONGLONG vramBase = (ULONGLONG)device->VramPhysical.QuadPart;
        ULONGLONG seedAt = vramBase;
        SIZE_T seedBytes = device->FramebufferLength;
        PHYSICAL_ADDRESS seedPhys;
        PVOID seedMap;

        if (device->DcnFirmwareKnown && device->DcnFirmwareAddress >= vramBase &&
            device->DcnFirmwareAddress - vramBase < device->VramLength)
            seedAt = device->DcnFirmwareAddress;
        if (seedAt != device->DcnScanoutMapAddress &&
            (ULONGLONG)seedBytes <= device->VramLength - (seedAt - vramBase))
        {
            seedPhys.QuadPart = (LONGLONG)seedAt;
            seedMap = VramMapCpuRange(device,seedPhys,seedBytes,PAGE_READONLY);
            if (seedMap != NULL)
            {
                ULONG firstPixel = *(volatile ULONG*)seedMap;

                BOOLEAN copied=WddmCopyScanoutRows(dst,dstLength,dstPitch,seedMap,seedBytes,
                    device->Post.Pitch,device->Post.Width,device->Post.Height);
                MmUnmapIoSpace(seedMap, seedBytes);
                if (copied)
                {
                    device->DcnScanoutSeedAddress = device->DcnScanoutMapAddress;
                    InterlockedIncrement(&wddm->BlitSeeds);
                    GuardLog("wddm: flip target seeded from VRAM physical 0x%llX, %lu bytes, first pixel 0x%08X, onto 0x%llX",
                             seedAt, (ULONG)seedBytes, firstPixel, device->DcnScanoutMapAddress);
                }
            }
            else if (InterlockedIncrement(&wddm->BlitSkips) <= BC250_WDDM_LOG_CALLS)
                GuardLog("wddm: flip target seed failed: no mapping for 0x%llX", seedAt);
        }
    }

    whole = Present->DstRect;
    if (toFlip && Present->SubRectCnt != 0 && Present->pDstSubRects != NULL)
    {
        rects = Present->pDstSubRects;
        count = Present->SubRectCnt;
    }
    else if (toFlip)
    {
        rects = &whole;
        count = 0;
    }
    else
    {
        rects = (Present->SubRectCnt != 0 && Present->pDstSubRects != NULL) ? Present->pDstSubRects : &whole;
        count = (Present->SubRectCnt != 0 && Present->pDstSubRects != NULL) ? Present->SubRectCnt : 1;
    }
    dx = Present->SrcRect.left - Present->DstRect.left;
    dy = Present->SrcRect.top - Present->DstRect.top;
    for (i = 0; i < count; i++)
    {
        if (verbose && i < 4)
            GuardLog("wddm: blit rect %u (%d,%d)-(%d,%d)", i, rects[i].left, rects[i].top, rects[i].right, rects[i].bottom);
        // Clamp as display.c:CopyRect does: to the firmware mode on the destination side and to the allocation on
        // the source side; a rectangle that ends up empty or outside is simply skipped.
        LONG left = rects[i].left > Present->DstRect.left ? rects[i].left : Present->DstRect.left;
        LONG top = rects[i].top > Present->DstRect.top ? rects[i].top : Present->DstRect.top;
        LONG right = rects[i].right < Present->DstRect.right ? rects[i].right : Present->DstRect.right;
        LONG bottom = rects[i].bottom < Present->DstRect.bottom ? rects[i].bottom : Present->DstRect.bottom;
        LONG y;

        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > (LONG)dstWidth) right = (LONG)dstWidth;
        if (bottom > (LONG)dstHeight) bottom = (LONG)dstHeight;
        if (left + dx < 0 || top + dy < 0 || right + dx > (LONG)alloc->Width || bottom + dy > (LONG)alloc->Height) continue;
        if (right <= left || bottom <= top) continue;
        for (y = top; y < bottom; y++)
        {
            // The geometry (pitch, and so this offset) is always the firmware's mode, Device->Post - the one
            // the VidPn primary is pinned to (display.c's Bc250CommitVidPn) - whichever buffer dst points at;
            // only the bound changes between the two destinations. dstLength is one of the two driver-owned
            // lengths set above, never a number that came from user mode.
            SIZE_T dstOff = (SIZE_T)y * dstPitch + (SIZE_T)left * 4;
            SIZE_T src = (SIZE_T)(y + dy) * alloc->Pitch + (SIZE_T)(left + dx) * 4;
            SIZE_T bytes = (SIZE_T)(right - left) * 4;

            if (dstOff + bytes > dstLength || src + bytes > alloc->Size) break;
            WddmCopyColorRow(dst + dstOff, map + src, bytes,
                WddmRedFirst(alloc->Format) != destinationRedFirst);
            if (destinationPrimary && !device->VidPnFlipEnabled &&
                right <= (LONG)device->Post.Width && y < (LONG)device->Post.Height)
            {
                SIZE_T postOffset = (SIZE_T)y * device->Post.Pitch + (SIZE_T)left * 4;
                if (postOffset + bytes <= device->FramebufferLength)
                    WddmCopyColorRow((UCHAR*)device->Framebuffer + postOffset, map + src, bytes,
                        WddmRedFirst(alloc->Format));
            }
            rows++;
        }
    }
    if (destinationMap != NULL) MmUnmapIoSpace(destinationMap, dstLength);
    MmUnmapIoSpace((void*)map, (SIZE_T)alloc->Size);
    InterlockedIncrement(&wddm->Blits);
    InterlockedExchange(&wddm->BlitRowsLast, (LONG)rows);
    if ((LONG)rows > wddm->BlitRowsMax) InterlockedExchange(&wddm->BlitRowsMax, (LONG)rows);
    InterlockedExchange(&wddm->BlitReadingLast, reading);
    InterlockedExchange(&wddm->BlitWidthLast, (LONG)alloc->Width);
    InterlockedExchange(&wddm->BlitHeightLast, (LONG)alloc->Height);
    InterlockedExchange(&wddm->BlitPitchLast, (LONG)alloc->Pitch);
    InterlockedExchange(&wddm->BlitSubRectsLast, (LONG)count);
    InterlockedExchange64(&wddm->BlitSourceLast, (LONGLONG)first);
    if (toFlip) InterlockedIncrement(&wddm->BlitsToFlip);
    else if (destinationMap == NULL || (destinationPrimary && !device->VidPnFlipEnabled))
    {
        InterlockedIncrement(&wddm->BlitsToFirmware);
        if (mapFailed) InterlockedIncrement(&wddm->BlitsMapFailed);
    }
    if (verbose)
        GuardLog("wddm: blit %u rectangles, %u rows copied, destination %s%s", count, rows,
                 destinationMap ? (destinationPrimary ? "primary allocation" : "offscreen allocation") :
                 (toFlip ? "flipped surface" : "POST framebuffer"), mapFailed ? " (mapping failed, fell back)" : "");
}

static DXGKDDI_PRESENT Bc250WddmPresent;
static NTSTATUS Bc250WddmPresent(_In_ const HANDLE hContext, _Inout_ DXGKARG_PRESENT* pPresent)
{
    BC250_WDDM_OBJECT* context = WddmObject(hContext, BC250_WDDM_MAGIC_CONTEXT);

    // A flip is handled by SetVidPnSourceAddress. A gated Blt constructs one
    // software packet below; its pixels are copied only at SubmitCommandVirtual.
    // Colour fill remains unimplemented in this diagnostic path.
    if (context != NULL && WddmFirstCalls((BC250_WDDM*)context->Device->Wddm, WddmDdiPresent))
        GuardLog("wddm: Present flags 0x%08X source %u dest %u, %u sub-rectangles, %u DMA bytes free",
                 pPresent->Flags.Value, pPresent->NumSrcAllocations, pPresent->NumDstAllocations,
                 pPresent->SubRectCnt, pPresent->DmaSize);
    // 0.7.14, still inert: what the allocation list of a Present holds. The header puts pAllocationList
    // (DXGK_ALLOCATIONLIST, 24 bytes an entry) and pAllocationInfo (DXGK_PRESENTALLOCATIONINFO, 32 bytes) in one
    // union and does not say which arm a GpuMmu driver is given, so the first three qwords of each of the first two
    // entries are logged raw - inside the array under either reading - and the arm is decided from the log.
    // 0.7.15: E20 run 001 showed NumSrcAllocations = NumDstAllocations = 0 even with a list of 256, and 0.7.14 had
    // tied its dump to those counters, so it said nothing. The classic contract puts the source and the destination
    // at fixed indices (DXGK_PRESENT_SOURCE_INDEX 1, DXGK_PRESENT_DESTINATION_INDEX 2) of pAllocationList, 24 bytes
    // an entry. The pointer is logged whatever it is; entries 1 and 2 are read only if this context was given a list
    // that long AND the 72 bytes lie in the pointer's own page - a read that cannot fault whatever is behind it.
    if (context != NULL && ((BC250_WDDM*)context->Device->Wddm)->Calls[WddmDdiPresent] <= BC250_WDDM_LOG_CALLS)
    {
        const ULONGLONG* raw = (const ULONGLONG*)pPresent->pAllocationList;
        C_ASSERT(sizeof(DXGK_ALLOCATIONLIST) == 3 * sizeof(ULONGLONG));    // raw[3..5] is entry 1, raw[6..8] entry 2

        GuardLog("wddm: Present list %p (context list %u), private %p/%u, driver data %u", (void*)raw, context->AllocationListSize,
                 pPresent->pDmaBufferPrivateData, pPresent->DmaBufferPrivateDataSize, pPresent->PrivateDriverDataSize);
        if (raw != NULL && context->AllocationListSize > DXGK_PRESENT_MAX_INDEX &&
            ((ULONG_PTR)raw & (PAGE_SIZE - 1)) <= PAGE_SIZE - BC250_WDDM_PRESENT_LIST_QWORDS * sizeof(ULONGLONG))
        {
            // Review 15: the pointer is dxgkrnl's word and nothing here proves it. A kernel address that is not mapped
            // bugchecks whatever surrounds it; this catches the other case, a user-mode one, as vidmm.c does.
            __try
            {
                GuardLog("wddm: Present list q0-3  %016llX %016llX %016llX %016llX", raw[0], raw[1], raw[2], raw[3]);
                GuardLog("wddm: Present list q4-7  %016llX %016llX %016llX %016llX", raw[4], raw[5], raw[6], raw[7]);
                GuardLog("wddm: Present list q8-11 %016llX %016llX %016llX %016llX", raw[8], raw[9], raw[10], raw[11]);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                GuardLog("wddm: Present list %p could not be read (0x%08X)", (void*)raw, (ULONG)GetExceptionCode());
            }
        }
        GuardLog("wddm: Present dst (%d,%d)-(%d,%d) src (%d,%d)-(%d,%d) va 0x%llX", pPresent->DstRect.left, pPresent->DstRect.top,
                 pPresent->DstRect.right, pPresent->DstRect.bottom, pPresent->SrcRect.left, pPresent->SrcRect.top,
                 pPresent->SrcRect.right, pPresent->SrcRect.bottom, (ULONGLONG)pPresent->DmaBufferGpuVirtualAddress);
    }
    if (context != NULL && !context->UmdContext && pPresent->Flags.Value == 1 &&
        ((BC250_WDDM*)context->Device->Wddm)->BlitGate)
    {
        BC250_PRESENT_PACKET* packet;
        UINT total, start, count;
        if (pPresent->pAllocationInfo == NULL || context->AllocationListSize <= DXGK_PRESENT_MAX_INDEX)
            return STATUS_INVALID_PARAMETER;
        if (pPresent->pDmaBuffer == NULL || pPresent->DmaSize < sizeof(ULONG) ||
            pPresent->pDmaBufferPrivateData == NULL ||
            pPresent->DmaBufferPrivateDataSize < sizeof(BC250_PRESENT_PACKET))
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        total = pPresent->SubRectCnt ? pPresent->SubRectCnt : 1;
        start = pPresent->MultipassOffset;
        if (start >= total || (pPresent->SubRectCnt != 0 && pPresent->pDstSubRects == NULL))
            return STATUS_INVALID_PARAMETER;
        count = total - start;
        if (count > BC250_PRESENT_PACKET_RECTS) count = BC250_PRESENT_PACKET_RECTS;
        packet = (BC250_PRESENT_PACKET*)pPresent->pDmaBufferPrivateData;
        RtlZeroMemory(packet, sizeof(*packet));
        // WDDM2 supplies the 32-byte allocation entries measured in M83.
        RtlCopyMemory(packet->Allocations, pPresent->pAllocationInfo, sizeof(packet->Allocations));
        packet->SrcRect = pPresent->SrcRect;
        packet->DstRect = pPresent->DstRect;
        packet->RectCount = count;
        if (pPresent->SubRectCnt)
            RtlCopyMemory(packet->Rects, pPresent->pDstSubRects + start, count * sizeof(RECT));
        else packet->Rects[0] = pPresent->DstRect;
        packet->Magic = BC250_PRESENT_PACKET_MAGIC;
        // Consume this DMA buffer in full: its one private packet must not be
        // overwritten by another Present appended to the same DMA buffer.
        // These bytes are intercepted above and are never submitted to GFX.
        RtlZeroMemory(pPresent->pDmaBuffer, pPresent->DmaSize);
        *(ULONG*)pPresent->pDmaBuffer = BC250_PRESENT_PACKET_MAGIC;
        pPresent->pDmaBuffer = (UCHAR*)pPresent->pDmaBuffer + pPresent->DmaSize;
        pPresent->MultipassOffset = start + count;
        if (start + count < total) return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_SETVIDPNSOURCEADDRESS Bc250WddmSetVidPnSourceAddress;
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

// ---- interrupts ---------------------------------------------------------------------------------------------------

static DXGKDDI_CONTROLINTERRUPT Bc250WddmControlInterrupt;
static NTSTATUS Bc250WddmControlInterrupt(_In_ const HANDLE hAdapter, _In_ const DXGK_INTERRUPT_TYPE InterruptType,
                                          _In_ BOOLEAN EnableInterrupt)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    NTSTATUS status = STATUS_SUCCESS;

    switch (InterruptType)
    {
    case DXGK_INTERRUPT_CRTC_VSYNC:
        // The only knob stage A has: the timer keeps its phase either way, but reports only while this is on.
        // Enabling it also starts the timer, because this is not only the flip path - a bare
        // D3DKMTWaitForVerticalBlankEvent enables the VSync without any flip ever being queued, and would
        // otherwise wait on a timer nothing had armed.
        if (wddm != NULL)
        {
            wddm->VSyncEnabled = EnableInterrupt;
            if (EnableInterrupt) WddmVSyncArm(device, TRUE);
        }
        break;
    case DXGK_INTERRUPT_DMA_COMPLETED:
        // Always on and not ours to switch off: a submission is completed whether or not anyone asked for it.
        break;
    default:
        status = STATUS_NOT_SUPPORTED;      // and which types are asked for is stage A evidence
        break;
    }
    if (WddmFirstCalls(wddm, WddmDdiControlInterrupt))
        GuardLog("wddm: ControlInterrupt type %u %s -> 0x%08X", (ULONG)InterruptType, EnableInterrupt ? "on" : "off",
                 status);
    return status;
}

static DXGKDDI_GETSCANLINE Bc250WddmGetScanLine;
static NTSTATUS Bc250WddmGetScanLine(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_GETSCANLINE* pGetScanLine)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    LARGE_INTEGER now, frequency;
    ULONGLONG since, period;
    UINT height = device->Post.Height;

    if (wddm == NULL || height == 0) return STATUS_NOT_SUPPORTED;
    if (pGetScanLine->VidPnTargetId != wddm->VSyncTargetId) return STATUS_INVALID_PARAMETER;

    if (device->VidPnFlipEnabled)
    {
        NTSTATUS status = DcnReadScanLine(device, &pGetScanLine->InVerticalBlank, &pGetScanLine->ScanLine);
        if (!NT_SUCCESS(status)) return status;
        goto Report;
    }

    // Software-only path: how far into the period we are gives the scan line, and
    // the last few per cent of it are called the blank. There is no CRTC to ask (ADR 0006 point 2), so this is a
    // plausible answer rather than a measured one - it is here because a driver claiming FlipOnVSyncMmIo that
    // leaves GetScanLine NULL leaves dxgkrnl no way to ask where the beam is.
    now = KeQueryPerformanceCounter(&frequency);
    period = (frequency.QuadPart > 0) ? ((ULONGLONG)frequency.QuadPart * BC250_WDDM_VSYNC_MS) / 1000 : 0;
    since = (ULONGLONG)(now.QuadPart - wddm->VSyncLast.QuadPart);
    if (period == 0 || since >= period)
    {
        pGetScanLine->InVerticalBlank = TRUE;       // the tick is due or overdue
        pGetScanLine->ScanLine = 0;
    }
    else
    {
        // The last 1/32 of the period is the blank, which is the right order of magnitude for a real mode.
        pGetScanLine->InVerticalBlank = (since * 32 >= period * 31) ? TRUE : FALSE;
        pGetScanLine->ScanLine = (UINT)((since * height) / period);
        if (pGetScanLine->ScanLine >= height) pGetScanLine->ScanLine = height - 1;
    }
Report:
    if (WddmFirstCalls(wddm, WddmDdiGetScanLine))
        GuardLog("wddm: GetScanLine target %u -> line %u, blank %u", (ULONG)pGetScanLine->VidPnTargetId,
                 pGetScanLine->ScanLine, pGetScanLine->InVerticalBlank ? 1u : 0u);
    return STATUS_SUCCESS;
}

// ---- the table --------------------------------------------------------------------------------------------------

// Research section 5.6: check host-side, at compile time, that the structure we fill is the one we think it is.
//
// The members stage A needs exist (a missing one would not compile at all, so the asserts say something stronger:
// that they are where a WDDM 2.0 table has them).
C_ASSERT(FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, Version) == 0);
C_ASSERT(FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiPresent) > FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiDestroyDevice));
C_ASSERT(FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSubmitCommandVirtual) > FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSubmitCommand));
C_ASSERT(FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiGetNodeMetadata) > FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiCreateContext));
// Nothing after the WDDM 2.0 block exists: SetVideoProtectedRegion is that block's last member, so if the
// structure ends right after it, every WDDM 2.1 and later member is structurally absent, not a NULL we defend.
C_ASSERT(sizeof(DRIVER_INITIALIZATION_DATA) ==
         FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSetVideoProtectedRegion) + sizeof(PVOID));
// Swizzling ranges are gone from WDDM, so the two DDIs must stay NULL and the cap must stay 0; the pair is here so
// that the connection is visible where the table is built.
C_ASSERT(FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiReleaseSwizzlingRange) >
         FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiAcquireSwizzlingRange));
// The reservation table is ordered: the tail this file keeps clear covers every buffer the bring-up files place.
C_ASSERT(BC250_VRAM_TOP_RESERVED >= BC250_VRAM_PSP_BELOW);
C_ASSERT(BC250_VRAM_PSP_BELOW >= BC250_VRAM_GART_BELOW);
// The declared address space and the declared page tables have to describe one another.
C_ASSERT(BC250_WDDM_VA_BITS == BC250_WDDM_PAGE_SHIFT + BC250_WDDM_LEVEL_BITS * BC250_WDDM_LEVEL_COUNT);
C_ASSERT(BC250_WDDM_LEVEL_COUNT >= 2 && BC250_WDDM_LEVEL_COUNT <= DXGK_MAX_PAGE_TABLE_LEVEL_COUNT);

// The members Learn marks "reserved and should be set to zero". WddmBuildTable never assigns them, so this can
// only ever fire because somebody added an assignment; that is exactly what it is here to catch. The trap worth
// naming: DescribePageTable, UpdatePageTable, UpdatePageDirectory and MovePageDirectory look like the WDDM 2.x
// page-table DDIs and are the dead WDDM 1.x ones - the real page table work is BuildPagingBuffer operations.
static void WddmCheckReserved(_In_ const DRIVER_INITIALIZATION_DATA* Data)
{
    static const struct { const char* Name; SIZE_T Offset; } reserved[] = {
        { "DescribePageTable",   FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiDescribePageTable) },
        { "UpdatePageTable",     FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiUpdatePageTable) },
        { "UpdatePageDirectory", FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiUpdatePageDirectory) },
        { "MovePageDirectory",   FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiMovePageDirectory) },
        { "SubmitRender",        FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSubmitRender) },
        { "CreateAllocation2",   FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiCreateAllocation2) },
        { "Reserved",            FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, Reserved) },
        { "SetPowerPState",      FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSetPowerPState) },
        { "Reserved1",           FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, Reserved1) },
        { "Reserved2",           FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, Reserved2) },
        { "AcquireSwizzlingRange", FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiAcquireSwizzlingRange) },
        { "ReleaseSwizzlingRange", FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiReleaseSwizzlingRange) },
    };
    ULONG i;

    for (i = 0; i < RTL_NUMBER_OF(reserved); i++)
        if (*(PVOID* const*)((const UCHAR*)Data + reserved[i].Offset) != NULL)
            GuardLog("wddm: table member %s must be zero and is not", reserved[i].Name);
}

// ---- the display side, traced -----------------------------------------------------------------------------------

// E16 run 005: the full adapter starts and then nothing on the display side happens, and the display DDIs this
// table shares with the display-only one were silent. These wrappers change no answer; they count each call and
// log the first few with the status, so that the next kept log says how far dxgkrnl's display core got. The
// counters are file-scope because the child DDIs may run before BC250_WDDM exists. All of these DDIs are
// PASSIVE_LEVEL ones; the IRQL test only guards the log's spin lock against an annotation being wrong.
typedef enum _BC250_WDDM_TRACED {
    TracedQueryChildRelations = 0, TracedQueryChildStatus, TracedQueryDeviceDescriptor, TracedIsSupportedVidPn,
    TracedRecommendFunctionalVidPn, TracedEnumVidPnCofuncModality, TracedSetVidPnSourceVisibility, TracedCommitVidPn,
    TracedUpdateActiveVidPnPresentPath, TracedRecommendMonitorModes, TracedQueryVidPnHWCapability, TracedCount
} BC250_WDDM_TRACED;

static volatile LONG g_TracedCalls[TracedCount];

static NTSTATUS WddmTraced(BC250_WDDM_TRACED Slot, _In_z_ const char* Name, NTSTATUS Status, ULONG Detail)
{
    LONG calls = InterlockedIncrement(&g_TracedCalls[Slot]);

    if (KeGetCurrentIrql() <= DISPATCH_LEVEL && (calls <= 6 || (!NT_SUCCESS(Status) && calls <= 64)))
        GuardLog("wddm: display %s call %ld detail 0x%X -> 0x%08X", Name, calls, Detail, Status);
    return Status;
}

static DXGKDDI_QUERY_CHILD_RELATIONS Bc250WddmQueryChildRelations;
static NTSTATUS Bc250WddmQueryChildRelations(_In_ const PVOID Context, _Inout_updates_bytes_(Size) PDXGK_CHILD_DESCRIPTOR Relations,
                                             _In_ ULONG Size)
{
    return WddmTraced(TracedQueryChildRelations, "QueryChildRelations", Bc250QueryChildRelations(Context, Relations, Size), Size);
}

static DXGKDDI_QUERY_CHILD_STATUS Bc250WddmQueryChildStatus;
static NTSTATUS Bc250WddmQueryChildStatus(_In_ const PVOID Context, _Inout_ PDXGK_CHILD_STATUS ChildStatus, _In_ BOOLEAN NonDestructiveOnly)
{
    ULONG type = (ULONG)ChildStatus->Type;

    return WddmTraced(TracedQueryChildStatus, "QueryChildStatus", Bc250QueryChildStatus(Context, ChildStatus, NonDestructiveOnly), type);
}

static DXGKDDI_QUERY_DEVICE_DESCRIPTOR Bc250WddmQueryDeviceDescriptor;
static NTSTATUS Bc250WddmQueryDeviceDescriptor(_In_ const PVOID Context, _In_ ULONG ChildUid, _Inout_ PDXGK_DEVICE_DESCRIPTOR Descriptor)
{
    return WddmTraced(TracedQueryDeviceDescriptor, "QueryDeviceDescriptor", Bc250QueryDeviceDescriptor(Context, ChildUid, Descriptor), ChildUid);
}

#define BC250_WDDM_TRACED_DDI(DdiType, Name, ArgType)                                                          \
    static DdiType Bc250WddmTraced##Name;                                                                      \
    static NTSTATUS Bc250WddmTraced##Name(_In_ const HANDLE hAdapter, ArgType pArgs)                           \
    {                                                                                                          \
        return WddmTraced(Traced##Name, #Name, Bc250##Name(hAdapter, pArgs), 0);                               \
    }

BC250_WDDM_TRACED_DDI(DXGKDDI_ISSUPPORTEDVIDPN, IsSupportedVidPn, DXGKARG_ISSUPPORTEDVIDPN*)
BC250_WDDM_TRACED_DDI(DXGKDDI_RECOMMENDFUNCTIONALVIDPN, RecommendFunctionalVidPn, const DXGKARG_RECOMMENDFUNCTIONALVIDPN* const)
BC250_WDDM_TRACED_DDI(DXGKDDI_ENUMVIDPNCOFUNCMODALITY, EnumVidPnCofuncModality, const DXGKARG_ENUMVIDPNCOFUNCMODALITY* const)
BC250_WDDM_TRACED_DDI(DXGKDDI_SETVIDPNSOURCEVISIBILITY, SetVidPnSourceVisibility, const DXGKARG_SETVIDPNSOURCEVISIBILITY*)
BC250_WDDM_TRACED_DDI(DXGKDDI_COMMITVIDPN, CommitVidPn, const DXGKARG_COMMITVIDPN* const)
BC250_WDDM_TRACED_DDI(DXGKDDI_UPDATEACTIVEVIDPNPRESENTPATH, UpdateActiveVidPnPresentPath, const DXGKARG_UPDATEACTIVEVIDPNPRESENTPATH* const)
BC250_WDDM_TRACED_DDI(DXGKDDI_RECOMMENDMONITORMODES, RecommendMonitorModes, const DXGKARG_RECOMMENDMONITORMODES* const)
BC250_WDDM_TRACED_DDI(DXGKDDI_QUERYVIDPNHWCAPABILITY, QueryVidPnHWCapability, DXGKARG_QUERYVIDPNHWCAPABILITY*)

void WddmBuildTable(_Out_ DRIVER_INITIALIZATION_DATA* Data)
{
    RtlZeroMemory(Data, sizeof(*Data));
    Data->Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;

    // The 28 pointers the display-only table already has; since 0.7.6 the child and VidPN ones go through the
    // tracing wrappers above, which change no answer. PresentDisplayOnly is the one member with no
    // home in this structure; Present plus SetVidPnSourceAddress take its place.
    Data->DxgkDdiAddDevice = Bc250AddDevice;
    Data->DxgkDdiStartDevice = Bc250StartDevice;
    Data->DxgkDdiStopDevice = Bc250StopDevice;
    Data->DxgkDdiRemoveDevice = Bc250RemoveDevice;
    Data->DxgkDdiResetDevice = Bc250ResetDevice;
    Data->DxgkDdiDispatchIoRequest = Bc250DispatchIoRequest;
    Data->DxgkDdiInterruptRoutine = Bc250InterruptRoutine;
    Data->DxgkDdiDpcRoutine = Bc250DpcRoutine;
    Data->DxgkDdiQueryChildRelations = Bc250WddmQueryChildRelations;
    Data->DxgkDdiQueryChildStatus = Bc250WddmQueryChildStatus;
    Data->DxgkDdiQueryDeviceDescriptor = Bc250WddmQueryDeviceDescriptor;
    Data->DxgkDdiSetPowerState = Bc250SetPowerState;
    Data->DxgkDdiUnload = Bc250Unload;
    Data->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = Bc250StopDeviceAndReleasePostDisplayOwnership;
    Data->DxgkDdiSetPointerPosition = Bc250SetPointerPosition;
    Data->DxgkDdiSetPointerShape = Bc250SetPointerShape;
    Data->DxgkDdiEscape = Bc250Escape;
    Data->DxgkDdiIsSupportedVidPn = Bc250WddmTracedIsSupportedVidPn;
    Data->DxgkDdiRecommendFunctionalVidPn = Bc250WddmTracedRecommendFunctionalVidPn;
    Data->DxgkDdiEnumVidPnCofuncModality = Bc250WddmTracedEnumVidPnCofuncModality;
    Data->DxgkDdiSetVidPnSourceVisibility = Bc250WddmTracedSetVidPnSourceVisibility;
    Data->DxgkDdiCommitVidPn = Bc250WddmTracedCommitVidPn;
    Data->DxgkDdiUpdateActiveVidPnPresentPath = Bc250WddmTracedUpdateActiveVidPnPresentPath;
    Data->DxgkDdiRecommendMonitorModes = Bc250WddmTracedRecommendMonitorModes;
    Data->DxgkDdiQueryVidPnHWCapability = Bc250WddmTracedQueryVidPnHWCapability;
    Data->DxgkDdiSystemDisplayEnable = Bc250SystemDisplayEnable;
    Data->DxgkDdiSystemDisplayWrite = Bc250SystemDisplayWrite;

    // QueryAdapterInfo is the one carried-over DDI with a body of its own here: the full table has six more types
    // to answer and a different DRIVERCAPS, and routing it through this file keeps display.c untouched.
    Data->DxgkDdiQueryAdapterInfo = Bc250WddmQueryAdapterInfo;

    // New, and required of a full graphics miniport with GPU virtual addressing.
    Data->DxgkDdiGetNodeMetadata = Bc250WddmGetNodeMetadata;
    Data->DxgkDdiCreateDevice = Bc250WddmCreateDevice;
    Data->DxgkDdiDestroyDevice = Bc250WddmDestroyDevice;
    Data->DxgkDdiCreateContext = Bc250WddmCreateContext;
    Data->DxgkDdiDestroyContext = Bc250WddmDestroyContext;
    Data->DxgkDdiCreateProcess = Bc250WddmCreateProcess;
    Data->DxgkDdiDestroyProcess = Bc250WddmDestroyProcess;
    Data->DxgkDdiGetRootPageTableSize = Bc250WddmGetRootPageTableSize;
    Data->DxgkDdiSetRootPageTable = Bc250WddmSetRootPageTable;
    Data->DxgkDdiCreateAllocation = Bc250WddmCreateAllocation;
    Data->DxgkDdiDestroyAllocation = Bc250WddmDestroyAllocation;
    Data->DxgkDdiDescribeAllocation = Bc250WddmDescribeAllocation;
    Data->DxgkDdiGetStandardAllocationDriverData = Bc250WddmGetStandardAllocationDriverData;
    Data->DxgkDdiOpenAllocation = Bc250WddmOpenAllocation;
    Data->DxgkDdiCloseAllocation = Bc250WddmCloseAllocation;
    Data->DxgkDdiBuildPagingBuffer = Bc250WddmBuildPagingBuffer;
    // Both submission DDIs, routed to the same completion: SubmitCommandVirtual is what a GpuMmu context uses, and
    // SubmitCommand is what a paging buffer arrives on (hContext NULL). Which one dxgkrnl uses for what is logged.
    Data->DxgkDdiSubmitCommand = Bc250WddmSubmitCommand;
    Data->DxgkDdiSubmitCommandVirtual = Bc250WddmSubmitCommandVirtual;
    Data->DxgkDdiPreemptCommand = Bc250WddmPreemptCommand;
    Data->DxgkDdiResetFromTimeout = Bc250WddmResetFromTimeout;
    Data->DxgkDdiRestartFromTimeout = Bc250WddmRestartFromTimeout;
    // SupportPerEngineTDR = 1 obliges the table to carry all three.
    Data->DxgkDdiQueryDependentEngineGroup = Bc250WddmQueryDependentEngineGroup;
    Data->DxgkDdiQueryEngineStatus = Bc250WddmQueryEngineStatus;
    Data->DxgkDdiResetEngine = Bc250WddmResetEngine;
    Data->DxgkDdiCollectDbgInfo = Bc250WddmCollectDbgInfo;              // Level Zero, as per-engine TDR requires
    Data->DxgkDdiSetStablePowerState = Bc250WddmSetStablePowerState;    // dxgkrnl wants it wherever CalibrateGpuClock is
    Data->DxgkDdiCalibrateGpuClock = Bc250WddmCalibrateGpuClock;
    Data->DxgkDdiFormatHistoryBuffer = Bc250WddmFormatHistoryBuffer;
    Data->DxgkDdiPresent = Bc250WddmPresent;
    Data->DxgkDdiSetVidPnSourceAddress = Bc250WddmSetVidPnSourceAddress;
    // The other half of the flip contract. Claiming FlipOnVSyncMmIo without these two leaves dxgkrnl no way to
    // switch the VSync on and no way to ask where the beam is, and a queued flip is then never retired.
    Data->DxgkDdiControlInterrupt = Bc250WddmControlInterrupt;
    Data->DxgkDdiGetScanLine = Bc250WddmGetScanLine;

    // Left NULL on purpose: Patch and Render (physical addressing only), QueryCurrentFence, RecommendVidPnTopology,
    // StopCapture, every overlay and multi-plane member, LinkDevice, SetDisplayPrivateDriverFormat, RenderKm,
    // RenderGdi, ControlInterrupt2 (WDDM 2.1+, and it does not exist at 2.0), CancelCommand (CancelCommandAware
    // = 0), MapCpuHostAperture and UnmapCpuHostAperture (no host aperture), SetVideoProtectedRegion, and the
    // hardware-scheduling DDIs, which do not exist at all at WDDM 2.0. QueryInterface, ControlEtwLogging,
    // NotifyAcpiEvent, SetPalette, NotifySurpriseRemoval, GetChildContainerId, SetPowerComponentFState,
    // PowerRuntimeControlRequest and PowerRuntimeSetDeviceHandle stay NULL exactly as they are in the
    // display-only table today. ControlInterrupt and GetScanLine are **not** in this list any more: the flip
    // path above sets both. Nor are the per-engine TDR set, CollectDbgInfo and SetStablePowerState (0.7.4).
    WddmCheckReserved(Data);
}
