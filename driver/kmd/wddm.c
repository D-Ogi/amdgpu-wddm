// M7 stage A (ADR 0008): the full WDDM DDI table, behind the EnableFullWddm gate of entry.c.
//
// What stage A is: the smallest driver dxgkrnl will accept as a full graphics miniport, built to answer one
// question that no public source answers - does the adapter start, and does the desktop survive, when a started
// full WDDM adapter is the only one in the machine and no Direct3D user-mode driver can open it
// (docs/research/m7-full-wddm-miniport.md sections 2.2 and 2.3). It is therefore built to be inert:
//
//   - **No hardware.** Not one register, not one BAR mapping, not one doorbell in any DDI of this file. The
//     memory segment's geometry is read out of what vram.c already identified at start behind its own gate; the
//     display DDIs use the framebuffer display.c already owns. Nothing here calls into gart.c, psp.c, gfx.c or ih.c.
//   - **No submission.** A submitted packet is finished the moment dxgkrnl hands it over: the fence is reported
//     in software, through DxgkCbNotifyInterrupt at interrupt level and the DPC that the contract pairs with it.
//   - **No failure where a failure is a bugcheck.** BuildPagingBuffer, SubmitCommand, SubmitCommandVirtual,
//     PreemptCommand, ResetFromTimeout and RestartFromTimeout return success on every path (research 1.2(d)).
//   - **A log of what was called.** Every new DDI writes its first BC250_WDDM_LOG_CALLS calls to the guard log.
//     Which DDIs dxgkrnl really calls on an adapter nobody can render on, and in which order, is stage A's
//     evidence; it is not derivable from the documentation.
//
// The caps are research section 5.1: WDDM 2.0, one VRAM segment, one 3D node, GpuMmu, MultiEngineAware,
// PreemptionAware at DMA-buffer-boundary granularity, no ComputeOnly, no swizzling ranges. Since 0.7.4 (E16 runs
// 002 to 004, facts M64 and M65) also what the lab's dxgkrnl and every WDDM 1.2+ sample insist on: per-engine
// TDR with its three DDIs, DirectFlip, FlipIndependent, SmoothRotation, SetStablePowerState, CollectDbgInfo.
#include "bc250kmd.h"
#include "bc250_gfx.h"
#include "umd_blob.h"
#include "umd_caps.h"
#include <ntstrsafe.h>

#define BC250_WDDM_TAG 'wW2B'
#define BC250_WDDM_LOG_CALLS 8              // how many first calls of each DDI reach the guard log
#define BC250_WDDM_PRESENT_LIST_QWORDS 12u  // how much of a present's allocation list is read: 3 entries of either arm

// Segment ids are one-based: DXGK_QUERYSEGMENTOUT4.PagingBufferSegmentId is "the index (starting from 1)".
#define BC250_WDDM_SEGMENT_VRAM 1u
// An aperture segment, as Microsoft's RosKmd has one: VidMm backs it with system pages and asks for them to be mapped
// with BuildPagingBuffer (MapApertureSegment), which stage A answers inertly like every other operation. It exists
// because a GPU-VA context's DMA buffers must be VidMm allocations in an aperture segment: with "system memory"
// (segment set 0) dxgmms2 maps a NULL allocation into the context's address space and the machine goes down
// (E16 run 008, VIDMM_DMA_POOL::AddDmaBufferToPool). Stage B gives it the real GART behind it.
#define BC250_WDDM_SEGMENT_APERTURE 2u
#define BC250_WDDM_APERTURE_BYTES 0x10000000ull     // 256 MB of GPU address space, no memory behind it in stage A
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

// The paging buffer dxgkrnl allocates for us, in system memory (PagingBufferSegmentId = 0). Stage A writes nothing
// into it; the size only has to be plausible.
#define BC250_WDDM_PAGING_BUFFER_BYTES 0x10000ul

// One object kind per magic, so that a handle that is not ours is caught before it is dereferenced.
#define BC250_WDDM_MAGIC_DEVICE     'vD7M'
#define BC250_WDDM_MAGIC_CONTEXT    'xC7M'
#define BC250_WDDM_MAGIC_PROCESS    'cP7M'
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

typedef struct _BC250_WDDM_OBJECT {
    LIST_ENTRY Link;                    // BC250_WDDM::Objects: the stop frees whatever is still on this list
    ULONG Magic;
    BC250_DEVICE* Device;
    UINT NodeOrdinal;                   // contexts
    ULONGLONG RootPhysical;             // contexts: the root page table VidMm last set, as a physical address; 0 = none
    UINT AllocationListSize;            // contexts: what CreateContext answered, i.e. how long a list dxgkrnl keeps for it
    BC250_WDDM_ALLOCATION_PRIVATE Allocation;
    // M8: a context or allocation that arrived as a contract blob (umd_blob.c), not the GDI one above.
    // ExAllocatePool2 zeroes these, so a GDI object stays "not UMD" without a store. UmdRequestedVa is
    // recorded and not applied: VidMm places the pages, and the winsys maps the GPU VA itself.
    BOOLEAN UmdAlloc;
    BOOLEAN UmdContext;
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
    volatile LONG ReportFailures;                   // DxgkCbSynchronizeExecution refusals in WddmReport

    // Everything that can add work - an object, a timer, a DPC - is decided and done inside this lock, and
    // Stopping is what makes the stop final: it is set first, under the lock, so that a timer or a DPC cannot
    // re-arm or re-queue itself behind the cancel and the flush that follow. It does not remove the need for
    // dxgkrnl's own guarantee that no DDI arrives during StopDevice; it removes every race this file could
    // cause itself, which is the part we control.
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
    KDPC ReportDpc;

    // ADR 0008 stage D (docs/design/paging-node.md section 5): node 1's own hardware channel, parallel to stage
    // C's node-0 one below and never touching it - the two nodes fail independently, on their own hardware.
    BOOLEAN PagingHwPending;
    ULONG PagingHwSeq;                  // gfx.c's PagingSubmitSeq of the submission in flight
    UINT PagingHwFence;
    // Node 1's own deferral, the counterpart of DeferredValid/DeferredFence below. A completion of fence N
    // retires every fence up to N, so a software completion that arrives while a submission is in flight must
    // wait for it - but that ordering is per node, and until this field existed node 1 borrowed node 0's flag
    // and node 0's slot: a node-1 fence could be published early (node 0 idle) or, worse, published under node
    // 0's ordinal (node 0 busy). Unreachable while nothing ran on node 1; ADR 0008 stage D makes it a certainty,
    // because a second paging submission arriving while the first is on the ring is refused with
    // STATUS_DEVICE_BUSY and completed in software, which is exactly this case.
    BOOLEAN PagingDeferredValid;
    UINT PagingDeferredFence;
    KTIMER PagingSubmitTimer;
    KDPC PagingSubmitDpc;
    volatile LONG PagingHwSubmitted;
    volatile LONG PagingHwCompleted;
    volatile LONG PagingHwTimeouts;
    volatile LONG PagingHwRefused;
    // BuildPagingBuffer's own counters (design note section 7): built vs. answered inertly, by reason.
    volatile LONG PagingTransfersBuilt;
    volatile LONG PagingFillsBuilt;
    volatile LONG PagingBytesMoved;
    volatile LONG PagingInsufficientBuffer;
    volatile LONG PagingUnsupported[BC250PagingNotContiguous + 1]; // indexed by BC250_WDDM_PAGING_UNSUPPORTED
    // E24 run 006 (facts M110): the paging buffer leaves through SubmitCommandVirtual, not SubmitCommand, because
    // VidMm creates node 1's system context with DXGK_CREATECONTEXTFLAGS::VirtualAddressing set (flags 0x5 in the
    // run's own log). DXGKARG_SUBMITCOMMANDVIRTUAL has no submission start/end offsets - only
    // DmaBufferVirtualAddress. The struct calls it a GPU address. On this machine, for the system paging
    // buffer, DmaBufferGpuVirtualAddress is 0 and the submission's DmaBufferVirtualAddress is already the byte
    // offset the shadow is indexed by (facts M112: 0x0, 0x140, 0x640). A non-zero base still means "subtract".
    //
    // ONE buffer, and one is not a simplification: Gfx->PagingShadowMem is a single buffer indexed by
    // DmaBufferWriteOffset (design note section 4a), so the packets of two paging buffers being built at the
    // same time land on each other at the same offsets. Tracking several would be a claim to know where four
    // buffers' packets are while the shadow can hold one - it would match a submission against bytes that
    // belong to a different buffer and put them on SDMA0. So: the buffer whose packets are in the shadow right
    // now, and nothing else. PagingVirtualUnmapped counts what that costs, and a run where it is not 0 is the
    // evidence that would justify a shadow per buffer.
    //
    // Written is how far into that buffer this driver has actually put packets. The submit never runs past it,
    // whatever size dxgkrnl names, and it is reset whenever the buffer changes or dxgkrnl restarts one at
    // offset 0. A base of 0 with a mark of 0 means nothing has been recorded. A base of 0 with a mark above 0
    // is the system paging buffer of M112, whose submissions already speak in shadow offsets.
    // These two are one value in two words and are only ever touched under Lock (review 24, two MUST-FIX
    // items that were the same mistake): an address paired with a mark that belongs to a different buffer is
    // not a stale read to be range-checked away, it is a correct-looking match that puts another buffer's
    // packets on the ring. Interlocked singles cannot express "these agree"; the lock can, and this path runs
    // a few times a second.
    LONG64 PagingBufferGpuVa;             // buffer identity: a GPU VA, or 0 when the submission address is the offset (M112)
    LONG PagingBufferWritten;             // high-water mark in bytes: packets exist in the shadow below this
    volatile LONG PagingVirtualSubmits[BC250_WDDM_NODE_COUNT_MAX];  // SubmitCommandVirtual, by node
    volatile LONG PagingVirtualUnmapped;  // node-1 submissions that named no buffer the shadow was holding
    volatile LONG PagingVirtualClamped;   // node-1 submissions cut back to the bytes actually written
    // The question the paragraph above turns on, asked of the hardware instead of assumed: how often the shadow
    // was taken over by a different paging buffer while it still held packets of the one before. 0 means VidMm
    // builds one buffer at a time and a single shadow is the right shape; anything else means the packets of
    // two buffers were landing on each other at the same offsets, and the answer is a shadow per buffer.
    volatile LONG PagingBufferSwitches;

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

    KTIMER VSyncTimer;
    KDPC VSyncDpc;
    BOOLEAN VSyncArmed;                 // the timer is running (a source is visible)
    BOOLEAN VSyncEnabled;               // ControlInterrupt turned CRTC_VSYNC on
    D3DDDI_VIDEO_PRESENT_TARGET_ID VSyncTargetId;
    volatile LONG VSyncTicks;           // timer ticks, whether or not anybody was listening
    volatile LONG VSyncReports;         // of those, the ones reported to dxgkrnl as DXGK_INTERRUPT_CRTC_VSYNC
    LARGE_INTEGER VSyncLast;            // the performance counter at the last tick, for GetScanLine's phase
    LARGE_INTEGER VSyncFrequency;

    PHYSICAL_ADDRESS PrimaryAddress;    // what SetVidPnSourceAddress was last asked to scan out
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
static BC250_WDDM_OBJECT* WddmNewObject(_Inout_ BC250_DEVICE* Device, ULONG Magic)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BC250_WDDM_OBJECT* object;
    KIRQL irql;

    if (wddm == NULL) return NULL;      // the start found no pool, or the stop has already run: create nothing
    object = (BC250_WDDM_OBJECT*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*object), BC250_WDDM_TAG);
    if (object == NULL) return NULL;
    object->Magic = Magic;
    object->Device = Device;

    // The Stopping check and the insertion are one critical section: an object that got onto the list after the
    // stop had drained it would never be freed.
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->Stopping)
    {
        KeReleaseSpinLock(&wddm->Lock, irql);
        ExFreePoolWithTag(object, BC250_WDDM_TAG);
        return NULL;
    }
    InsertTailList(&wddm->Objects, &object->Link);
    wddm->ObjectCount++;
    KeReleaseSpinLock(&wddm->Lock, irql);
    return object;
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
static void WddmCompleteFence(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;

    if (wddm == NULL || NodeOrdinal >= BC250_WDDM_NODE_COUNT_MAX) return;
    InterlockedExchange(&wddm->SubmittedNode[NodeOrdinal], (LONG)NodeOrdinal);
    InterlockedExchange(&wddm->SubmittedFence[NodeOrdinal], (LONG)FenceId);
    InterlockedExchange(&wddm->CompletionPending[NodeOrdinal], 1);
    WddmQueueReport(wddm);
}

// Same for a preemption. The DDI may not report inline either, and the fence it wants to name as last completed
// is only right once any pending completion has been published - which is why both go through one DPC.
static void WddmPreemptFence(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;

    if (wddm == NULL || NodeOrdinal >= BC250_WDDM_NODE_COUNT_MAX) return;
    InterlockedExchange(&wddm->PreemptionNode[NodeOrdinal], (LONG)NodeOrdinal);
    InterlockedExchange(&wddm->PreemptionFence[NodeOrdinal], (LONG)FenceId);
    InterlockedExchange(&wddm->PreemptionPending[NodeOrdinal], 1);
    WddmQueueReport(wddm);
}

// ---- stage C: the hardware path --------------------------------------------------------------------------------

#define BC250_WDDM_VMID 1u                  // the one hardware VMID, re-pointed at the submitter's root by gfx.c
#define BC250_WDDM_SUBMIT_TIMEOUT_MS 500    // an M6 dispatch takes 28 us (facts M57); the TDR default is 2 s

// A completion that did not come from the hardware. While a hardware submission is in flight ON THAT NODE it
// waits for it: the two nodes run on different rings, with different fences and different watchdogs, and node
// 1's completion has no business waiting behind node 0's packet or being published under node 0's ordinal (the
// PagingDeferredValid field's own comment).
static void WddmCompleteSoftware(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN paging = NodeOrdinal == BC250_WDDM_NODE_COPY;
    BOOLEAN deferred;
    KIRQL irql;

    if (wddm == NULL) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    deferred = paging ? wddm->PagingHwPending : wddm->HwPending;
    if (deferred && paging) { wddm->PagingDeferredValid = TRUE; wddm->PagingDeferredFence = FenceId; }
    else if (deferred) { wddm->DeferredValid = TRUE; wddm->DeferredFence = FenceId; }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!deferred) WddmCompleteFence(Device, FenceId, NodeOrdinal);
}

// Has the fence of the submission in flight arrived? Called from the IH DPC (pnp.c), from the submit and from the
// watchdog, at <= DISPATCH_LEVEL. GfxFenceArrived is a memory read.
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
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!done) return;
    if (InterlockedIncrement(&wddm->HwCompleted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: hardware fence arrived, reporting fence %u", fence);
    WddmCompleteFence(Device, fence, node);
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
    if (wddm->HwPending)
    {
        timedOut = TRUE;
        fence = wddm->DeferredValid ? wddm->DeferredFence : wddm->HwFence;
        node = wddm->HwNode;
        seq = wddm->HwSeq;
        wddm->HwPending = FALSE;
        wddm->DeferredValid = FALSE;
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // Nobody can reset this GPU. The packet is declared finished so that the scheduler never starts a TDR it
    // cannot win, and the ring is not written again in this device start.
    InterlockedIncrement(&wddm->HwTimeouts);
    GfxSubmitFail(device);
    GuardLog("wddm: HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u completed in software, ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    WddmCompleteFence(device, fence, node);
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

// How much of the shadow a paging buffer may ever claim: the smaller of what this driver told dxgkrnl the
// paging buffer is (PagingBufferSize, the DDI's own number) and what the shadow physically is. The two are
// separate constants in separate files and were relied on being equal by nothing but coincidence (review 24
// SHOULD-FIX, and wddm.c's own older note); the smaller of them is correct whatever either becomes.
static ULONG WddmPagingBufferLimit(void)
{
    ULONG shadow = GfxPagingShadowBytes();

    return (shadow < BC250_WDDM_PAGING_BUFFER_BYTES) ? shadow : (ULONG)BC250_WDDM_PAGING_BUFFER_BYTES;
}

// The buffer the shadow is holding, and how far into it this driver has written - one pair, under Lock, never
// two atomics (the fields' own comment). BuildPagingBuffer is the only writer. The pair is reset on two events,
// and both mean the same thing - the shadow's contents no longer describe what the mark claims: a different
// buffer, and dxgkrnl restarting a buffer at offset 0, which it does on reuse. gfx.c's PagingBuildersActive
// says two builders can genuinely run at once, which is exactly why the reset and the raise have to be one
// critical section: a raise that lands after another builder's reset would otherwise stamp this buffer's mark
// onto that builder's address and leave the mismatch standing.
static void WddmPagingBufferWritten(_Inout_ BC250_WDDM* Wddm, LONG64 GpuVa, ULONG WriteOffset, ULONG WrittenEnd)
{
    LONG64 previous;
    LONG live = 0;
    BOOLEAN switched = FALSE;
    KIRQL irql;

    // GpuVa 0 is a real identity here (M112), not "nothing was passed". Only an end past the shadow is refused.
    if (Wddm == NULL || WrittenEnd > WddmPagingBufferLimit()) return;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    previous = Wddm->PagingBufferGpuVa;
    if (WriteOffset == 0 || previous != GpuVa)
    {
        live = Wddm->PagingBufferWritten;
        // A different buffer arriving while this one still had packets in the shadow is the case the single
        // shadow cannot serve (the PagingBufferSwitches field). A buffer dxgkrnl restarted at offset 0 is not
        // that case: it is the same buffer, and its old packets are the ones being replaced on purpose.
        switched = live > 0 && previous != GpuVa;
        Wddm->PagingBufferGpuVa = GpuVa;
        Wddm->PagingBufferWritten = 0;
    }
    if ((LONG)WrittenEnd > Wddm->PagingBufferWritten) Wddm->PagingBufferWritten = (LONG)WrittenEnd;
    KeReleaseSpinLock(&Wddm->Lock, irql);
    // Outside the lock: GuardLog takes its own, and this one is a diagnostic, not part of the invariant.
    if (switched && InterlockedIncrement(&Wddm->PagingBufferSwitches) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: paging buffer 0x%llX took the shadow from 0x%llX, which still held %ld bytes of "
                 "packets - a submission naming the old one will be refused from here on",
                 (ULONGLONG)GpuVa, (ULONGLONG)previous, live);
}

// The reverse: an address from a submission back to [offset, end) of the shadow, or FALSE unless the shadow is
// holding exactly that buffer and has packets at that point. Both fields are taken in one critical section, so
// the address and the mark are always the same buffer's - reading them one after the other, however atomically,
// is what let a switch land between them and hand a submission the next buffer's bytes. End is capped at the
// mark on purpose: dxgkrnl may name a size that covers the whole buffer, and the bytes above the mark are not
// this submission's.
static BOOLEAN WddmPagingBufferRange(_Inout_ BC250_WDDM* Wddm, LONG64 GpuVa, ULONG Size, _Out_ ULONG* Offset,
                                     _Out_ ULONG* End)
{
    LONG64 base;
    ULONGLONG offset;
    LONG written;
    KIRQL irql;

    *Offset = 0;
    *End = 0;
    if (Wddm == NULL) return FALSE;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    base = Wddm->PagingBufferGpuVa;
    written = Wddm->PagingBufferWritten;
    KeReleaseSpinLock(&Wddm->Lock, irql);

    // Nothing recorded yet. A base of 0 with a mark above 0 is M112: the submission address is the offset.
    if (written <= 0) return FALSE;
    if (base == 0)
        offset = (ULONGLONG)GpuVa;
    else if ((LONG64)GpuVa < base)
        return FALSE;
    else
        offset = (ULONGLONG)GpuVa - (ULONGLONG)base;
    if (offset >= WddmPagingBufferLimit()) return FALSE;
    if (written <= (LONG)offset) return FALSE;
    *Offset = (ULONG)offset;
    *End = ((ULONGLONG)offset + Size < (ULONGLONG)written) ? (ULONG)offset + Size : (ULONG)written;
    return TRUE;
}

// Has node 1's in-flight fence arrived? Same shape as WddmGpuFence, called from the same places (the IH DPC,
// the submit itself, the watchdog), at <= DISPATCH_LEVEL.
void WddmGpuFencePaging(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN done = FALSE;
    UINT fence = 0;
    KIRQL irql;

    if (wddm == NULL) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->PagingHwPending && GfxPagingFenceArrived(Device, wddm->PagingHwSeq))
    {
        done = TRUE;
        // The held-back completion, if there is one, is the later fence and retires this one with it.
        fence = wddm->PagingDeferredValid ? wddm->PagingDeferredFence : wddm->PagingHwFence;
        wddm->PagingDeferredValid = FALSE;
        wddm->PagingHwPending = FALSE;
        KeCancelTimer(&wddm->PagingSubmitTimer);
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!done) return;
    if (InterlockedIncrement(&wddm->PagingHwCompleted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: paging hardware fence arrived, reporting fence %u", fence);
    WddmCompleteFence(Device, fence, BC250_WDDM_NODE_COPY);
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
    if (wddm->PagingHwPending)
    {
        timedOut = TRUE;
        fence = wddm->PagingDeferredValid ? wddm->PagingDeferredFence : wddm->PagingHwFence;
        wddm->PagingDeferredValid = FALSE;
        seq = wddm->PagingHwSeq;
        wddm->PagingHwPending = FALSE;
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // Same answer as node 0's own timeout: nobody can reset this GPU, so the packet is declared finished and the
    // SDMA0 ring path is closed for node 1 only - node 0's ring path, and node 0's own SubmitFailed, are untouched.
    InterlockedIncrement(&wddm->PagingHwTimeouts);
    GfxPagingSubmitFail(device);
    GuardLog("wddm: PAGING HARDWARE FENCE TIMEOUT after %u ms (sequence %u): fence %u completed in software, node 1 ring path closed",
             (ULONG)BC250_WDDM_SUBMIT_TIMEOUT_MS, seq, fence);
    WddmCompleteFence(device, fence, BC250_WDDM_NODE_COPY);
}

// DISPATCH_LEVEL (Bc250WddmSubmitCommand, node 1). TRUE = the packet is on SDMA0's ring and its completion will
// come by itself. ShadowOffset/ByteCount name the range of Gfx's shadow buffer GfxPagingBuild already filled
// (design note section 4a); FenceId is dxgkrnl's own SubmissionFenceId for this node, unrelated to gfx.c's own
// sequence numbering, exactly as WddmSubmitHardware keeps the two apart for node 0.
static BOOLEAN WddmSubmitPagingHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, ULONG ShadowOffset,
                                        ULONG ByteCount, UINT FenceId)
{
    LARGE_INTEGER due;
    ULONG seq = 0;
    NTSTATUS status;
    KIRQL irql;

    status = GfxSubmitPaging(Device, ShadowOffset, ByteCount, &seq);
    if (!NT_SUCCESS(status))
    {
        if (InterlockedIncrement(&Wddm->PagingHwRefused) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: paging ring refused 0x%08X (fence %u, shadow offset 0x%lX, %lu bytes): completed in software",
                     status, FenceId, ShadowOffset, ByteCount);
        return FALSE;
    }
    due.QuadPart = -10000ll * BC250_WDDM_SUBMIT_TIMEOUT_MS;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    Wddm->PagingHwPending = TRUE;
    Wddm->PagingHwSeq = seq;
    Wddm->PagingHwFence = FenceId;
    if (!Wddm->Stopping) KeSetTimer(&Wddm->PagingSubmitTimer, due, &Wddm->PagingSubmitDpc);
    KeReleaseSpinLock(&Wddm->Lock, irql);
    if (InterlockedIncrement(&Wddm->PagingHwSubmitted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: fence %u on the SDMA0 ring: sequence %u, shadow offset 0x%lX, %lu bytes", FenceId, seq,
                 ShadowOffset, ByteCount);
    WddmGpuFencePaging(Device);         // the interrupt may have come and gone before PagingHwPending was set
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
    UINT node;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    if (WddmStopping(wddm)) return;

    // ADR 0008 stage D: both nodes' pending completion/preemption are checked, not only node 0's - the array
    // slot a gate-closed device never sets stays 0, so this loop reports nothing new for node 1 until the gate
    // opens and something actually submits to it (design note section 5).
    for (node = 0; node < BC250_WDDM_NODE_COUNT_MAX; node++)
    {
        if (InterlockedExchange(&wddm->CompletionPending[node], 0) != 0)
        {
            fence = InterlockedCompareExchange(&wddm->SubmittedFence[node], 0, 0);
            InterlockedExchange(&wddm->LastCompletedFence, fence);
            RtlZeroMemory(&data, sizeof(data));
            data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            data.DmaCompleted.SubmissionFenceId = (UINT)fence;
            data.DmaCompleted.NodeOrdinal = (UINT)InterlockedCompareExchange(&wddm->SubmittedNode[node], 0, 0);
            data.DmaCompleted.EngineOrdinal = 0;    // "for adapters that are not part of a link, always 0"
            WddmReport(device, &data);
        }

        if (InterlockedExchange(&wddm->PreemptionPending[node], 0) != 0)
        {
            RtlZeroMemory(&data, sizeof(data));
            data.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
            data.DmaPreempted.PreemptionFenceId = (UINT)InterlockedCompareExchange(&wddm->PreemptionFence[node], 0, 0);
            data.DmaPreempted.LastCompletedFenceId = (UINT)InterlockedCompareExchange(&wddm->LastCompletedFence, 0, 0);
            data.DmaPreempted.NodeOrdinal = (UINT)InterlockedCompareExchange(&wddm->PreemptionNode[node], 0, 0);
            data.DmaPreempted.EngineOrdinal = 0;
            WddmReport(device, &data);
        }
    }
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
    data.CrtcVsync.PhysicalAddress = wddm->PrimaryAddress;      // the address SetVidPnSourceAddress last asked for
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

    // M88's own gate (amdgpu's dm_vupdate_high_irq completes a flip only once dc_get_flip_pending_on_otg says
    // none is pending): postponed to the next tick rather than reported with a flip still in flight. A single,
    // non-blocking read (dcn.c's DcnFlipPending), never PollFlipPending's busy-wait - SURFACE_FLIP_PENDING
    // clears within one frame (facts M94), so this should not postpone more than once in practice.
    if (DcnFlipPending(Device))
    {
        InterlockedIncrement(&Device->DcnVsyncDeferred);
        return;
    }

    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    data.CrtcVsync.VidPnTargetId = wddm->VSyncTargetId;
    data.CrtcVsync.PhysicalAddress = wddm->PrimaryAddress;      // the address SetVidPnSourceAddress last asked for
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
    GuardLog("wddm summary: node 1 (paging, %s): %ld hardware submitted, %ld completed, %ld timeouts, %ld refused",
             Wddm->NodeCount > BC250_WDDM_NODE_COPY ? "open" : "closed", Wddm->PagingHwSubmitted,
             Wddm->PagingHwCompleted, Wddm->PagingHwTimeouts, Wddm->PagingHwRefused);
    // Which node SubmitCommandVirtual was called on, and how the node-1 ones were resolved against the paging
    // buffers. Run 006 had to infer the node-1 count by subtracting presents from submissions (facts M110);
    // no run after it does.
    GuardLog("wddm summary: SubmitCommandVirtual by node: 0: %ld, 1: %ld (%ld named no buffer, %ld clamped), "
             "shadow holds buffer 0x%llX to 0x%lX",
             Wddm->PagingVirtualSubmits[BC250_WDDM_NODE_3D], Wddm->PagingVirtualSubmits[BC250_WDDM_NODE_COPY],
             Wddm->PagingVirtualUnmapped, Wddm->PagingVirtualClamped, (ULONGLONG)Wddm->PagingBufferGpuVa,
             (ULONG)Wddm->PagingBufferWritten);
    GuardLog("wddm summary: paging buffer switches with packets still live: %ld (0 means one buffer at a time, "
             "which is what the single shadow assumes)", Wddm->PagingBufferSwitches);
    GuardLog("wddm summary: BuildPagingBuffer: %ld transfers, %ld fills, %ld bytes, %ld insufficient-buffer, "
             "unsupported (not ready/no root/no translation/system memory/not contiguous) %ld/%ld/%ld/%ld/%ld",
             Wddm->PagingTransfersBuilt, Wddm->PagingFillsBuilt, Wddm->PagingBytesMoved, Wddm->PagingInsufficientBuffer,
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

static BOOLEAN WddmSegment(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset, _Out_ ULONGLONG* Length);

void WddmStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm;
    ULONGLONG segmentOffset, segmentLength;

    Device->FullWddm = g_FullWddm;
    Device->Wddm = NULL;
    if (!g_FullWddm) return;                            // gate closed: this file does nothing at all

    // A failure here costs the whole file: every DDI below works with a NULL Wddm and does nothing.
    wddm = (BC250_WDDM*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*wddm), BC250_WDDM_TAG);
    if (wddm == NULL) { GuardLog("wddm: no pool for the adapter state"); return; }
    wddm->Device = Device;
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
    Device->Wddm = wddm;
    GuardLog("wddm: full table started, VRAM %s", Device->VramEnabled ? "identified" : "unknown (EnableVram closed)");
    // Stage B: the page tables VidMm keeps in the segment (vidmm.c). Without a segment it stays off.
    if (WddmSegment(Device, &segmentOffset, &segmentLength))
        VidMmStart(Device, segmentOffset, segmentLength, BC250_WDDM_SEGMENT_VRAM);
    // dxgkrnl fills DXGKRNL_INTERFACE to the size the declared version defines, and pnp.c copies only that much,
    // so the four callbacks every report in this file depends on are only there because the full table declares
    // WDDM 2.0. Say so once at the start rather than discover it from a silent no-op in the lab.
    GuardLog("wddm: callbacks sync %u notify %u queuedpc %u notifydpc %u, interface %u of %u bytes",
             Device->Dxgk.DxgkCbSynchronizeExecution != NULL, Device->Dxgk.DxgkCbNotifyInterrupt != NULL,
             Device->Dxgk.DxgkCbQueueDpc != NULL, Device->Dxgk.DxgkCbNotifyDpc != NULL,
             Device->Dxgk.Size, (ULONG)sizeof(Device->Dxgk));
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
        for (waited = 0; waited < BC250_WDDM_SUBMIT_TIMEOUT_MS + 100 && wddm->PagingHwPending; waited += 10)
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
    //  2. Cancel the timer and take back both DPCs, then KeFlushQueuedDpcs - PASSIVE_LEVEL only, which is where
    //     StopDevice runs - so that any DPC already running has finished.
    //  3. Only then drop Device->Wddm and read or free anything.
    //
    // What this does not cover, and cannot: a DDI that read Device->Wddm before step 3 and then touches the
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
    KeFlushQueuedDpcs();
    Device->Wddm = NULL;                // from here no DDI and no DPC of ours can find the state
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
static BOOLEAN WddmSegment(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset, _Out_ ULONGLONG* Length)
{
    ULONGLONG framebufferEnd = 0, fbOffset;

    *Offset = 0;
    *Length = 0;
    if (!Device->VramEnabled || Device->VramLength <= BC250_VRAM_TOP_RESERVED) return FALSE;
    if (VramFramebufferOffset(Device, &fbOffset))
    {
        // The **whole scan-out window**, stride times height, not the visible image: the firmware's pitch is
        // wider than Width * 4 on this board and the scan-out reads the padding too. This is the same arithmetic
        // display.c does for the mapping it owns (Pitch * Height), taken from Post rather than from
        // Device->FramebufferLength so that the answer is the same whether or not that mapping succeeded.
        framebufferEnd = fbOffset + (ULONGLONG)Device->Post.Pitch * Device->Post.Height;
    }

    *Offset = ROUND_TO_PAGES(framebufferEnd);
    if (*Offset >= Device->VramLength - BC250_VRAM_TOP_RESERVED) { *Offset = 0; return FALSE; }
    *Length = (Device->VramLength - BC250_VRAM_TOP_RESERVED) - *Offset;
    return TRUE;
}

// Two passes, as documented: the first asks only for the count and no other member may be touched; the second
// fills the array, which is iterated with the stride dxgkrnl gives and never with sizeof.
static NTSTATUS WddmQuerySegment4(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_QUERYSEGMENTOUT4* out = (DXGK_QUERYSEGMENTOUT4*)Query->pOutputData;
    DXGK_SEGMENTDESCRIPTOR4* descriptor;
    ULONGLONG offset, length;
    UINT count;

    if (Query->OutputDataSize < sizeof(*out) || out == NULL) return STATUS_BUFFER_TOO_SMALL;
    count = WddmSegment(Device, &offset, &length) ? 1u : 0u;

    if (count != 0) count = 2;          // the local segment, then the aperture segment
    g_ApertureOffered = (count == 2);
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

        // Segment 2, the aperture, filled as RosKmd fills its own: CPU-visible is the legacy lie the sample
        // documents ("a bad physical address that will never be used"), the base is the GART's place in the MC
        // address space on this part (gart_start 0, below the carve-out), and nothing is behind it yet.
        descriptor = (DXGK_SEGMENTDESCRIPTOR4*)((UCHAR*)out->pSegmentDescriptor + out->SegmentDescriptorStride);
        RtlZeroMemory(descriptor, sizeof(*descriptor));
        descriptor->Flags.Aperture = 1;
        descriptor->Flags.CacheCoherent = 1;
        descriptor->Flags.CpuVisible = 1;
        descriptor->BaseAddress.QuadPart = 0;
        descriptor->CpuTranslatedAddress.QuadPart = (LONGLONG)0xFFFFFFFE00000000ull;
        descriptor->Size = (SIZE_T)BC250_WDDM_APERTURE_BYTES;
        descriptor->CommitLimit = (SIZE_T)BC250_WDDM_APERTURE_BYTES;
    }
    out->NbSegment = count;
    // 0 is "system memory": VidMm then allocates the paging buffer itself, contiguous and write-combined. The
    // documented contract is "an aperture segment or 0", and segment 1 is local memory with Aperture clear; up to
    // 0.7.4 this said 1, and E16 run 004 was torn down right after CreateContext, before any root page table call
    // (facts M65, M66). A real aperture segment needs a working GART and belongs to stage B.
    out->PagingBufferSegmentId = 0;
    out->PagingBufferSize = BC250_WDDM_PAGING_BUFFER_BYTES;
    out->PagingBufferPrivateDataSize = 0;
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
    // WddmStart never fails the start (it answers with Device->Wddm left NULL instead, like every other
    // subsystem here), so a caps query can in principle land before or without it: the gate-closed node count
    // is the safe answer then, exactly what every other node-count reader in this file falls back to.
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
    ULONGLONG offset, length;

    if (Query->InputDataSize < sizeof(*in) || in == NULL) return STATUS_INVALID_PARAMETER;
    if (Query->OutputDataSize < sizeof(*desc) || desc == NULL) return STATUS_BUFFER_TOO_SMALL;
    if (in->LevelIndex >= BC250_WDDM_LEVEL_COUNT) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(desc, Query->OutputDataSize);
    desc->PageTableIndexBitCount = BC250_WDDM_LEVEL_BITS;
    desc->PageTableSizeInBytes = BC250_WDDM_PAGE_TABLE_BYTES;
    desc->PageTableAlignmentInBytes = 0;                // 0 means the page size of the memory segment
    if (WddmSegment(Device, &offset, &length))
    {
        desc->PageTableSegmentId = BC250_WDDM_SEGMENT_VRAM;
        desc->PagingProcessPageTableSegmentId = BC250_WDDM_SEGMENT_VRAM;
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
    object = WddmNewObject(parent->Device, BC250_WDDM_MAGIC_CONTEXT);
    if (object == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    object->NodeOrdinal = pCreateContext->NodeOrdinal;
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
    // 0 for GDI: nothing in that path puts a blob on the submit. UMD_BLOB_SUBMIT_BYTES is the whole
    // submit struct, which is what dxgkrnl allocates and copies the winsys's bytes into. The used
    // prefix is shorter; the reader checks that itself.
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize = umd ? UMD_BLOB_SUBMIT_BYTES : 0;
    // 0.7.14: a GDI context gets the allocation list the header sizes for it (RosKmdContext.cpp does the same).
    // With 0 here every Present of the CDD arrived with NumSrcAllocations = NumDstAllocations = 0 (E16 run 009, E18
    // run 003): dxgkrnl had nowhere to put the two surfaces of a Blt, and a driver that cannot name the source
    // cannot show it. Still no patch-location list: with virtual addressing there is nothing to patch.
    pCreateContext->ContextInfo.AllocationListSize =
        pCreateContext->Flags.GdiContext ? DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT : 0;
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

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls((BC250_WDDM*)object->Device->Wddm, WddmDdiDestroyContext)) GuardLog("wddm: DestroyContext");
    WddmFreeObject(object);
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
    // Stage A has no tiling and no alignment of its own, so a surface is its pitch times its height, linear. Four
    // bytes per pixel whatever the format was: that over-sizes a narrower format and never under-sizes one, and
    // it is already aligned to the four bytes the presentation caps above promise.
    private.Pitch = private.Width * 4;
    private.Size = (ULONGLONG)private.Pitch * private.Height;

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

static DXGKDDI_CREATEALLOCATION Bc250WddmCreateAllocation;
static NTSTATUS Bc250WddmCreateAllocation(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION* pCreateAllocation)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT i;

    for (i = 0; i < pCreateAllocation->NumAllocations; i++)
    {
        DXGK_ALLOCATIONINFO* info = &pCreateAllocation->pAllocationInfo[i];
        const BC250_WDDM_ALLOCATION_PRIVATE* private = (const BC250_WDDM_ALLOCATION_PRIVATE*)info->pPrivateDriverData;
        BC250_WDDM_OBJECT* object;

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
            info->FlagsWddm2.Value = 0;
            info->FlagsWddm2.CpuVisible = 1;
            /* Cached is a write-back CPU mapping, and the system PTE is not snooped unless
             * VidMm sets CacheCoherent. A command buffer wants write-combined. The run after
             * this change still timed out, and the CPU saw a real PACKET3 at the start of the
             * IB, so the cache was not what kept the fence from arriving. */
            info->AllocationPriority = 0;
            if (wddm != NULL && InterlockedIncrement(&wddm->UmdAllocs) <= BC250_WDDM_LOG_CALLS)
                GuardLog("wddm: umd alloc %llu bytes heap 0x%lX align %u va 0x%llX", view.bytes, view.heap, align,
                         view.requested_va);
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
        info->PreferredSegment.SegmentId0 = BC250_WDDM_SEGMENT_VRAM;
        info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(BC250_WDDM_SEGMENT_VRAM);
        info->SupportedWriteSegmentSet = BC250_WDDM_SEGMENT_SET(BC250_WDDM_SEGMENT_VRAM);
        info->EvictionSegmentSet = 0;                   // surfaces live in the local segment only; no eviction target
        info->PhysicalAdapterIndex = 0;
        info->FlagsWddm2.Value = 0;
        info->FlagsWddm2.CpuVisible = 1;                // the whole segment is CPU visible (facts M31)
        info->AllocationPriority = 0;
    }
    if (WddmFirstCalls(wddm, WddmDdiCreateAllocation))
        GuardLog("wddm: CreateAllocation %u allocations, resource %s", pCreateAllocation->NumAllocations,
                 pCreateAllocation->hResource != NULL ? "yes" : "no");
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
    // The firmware set the mode and its timing cannot be read without display-core MMIO, so the refresh rate is
    // reported as not specified. (The VidPN modes no longer say that for the full table: dxgkrnl refused them, E16
    // run 006, and display.c gives them a nominal 60 Hz. This field is informational and stays as it was.)
    pDescribeAllocation->RefreshRate.Numerator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
    pDescribeAllocation->RefreshRate.Denominator = D3DKMDT_FREQUENCY_NOTSPECIFIED;
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

static DXGKDDI_BUILDPAGINGBUFFER Bc250WddmBuildPagingBuffer;
static NTSTATUS Bc250WddmBuildPagingBuffer(_In_ const HANDLE hAdapter, _In_ DXGKARG_BUILDPAGINGBUFFER* pBuildPagingBuffer)
{
    // Only three return values are legal here; anything else, STATUS_NOT_IMPLEMENTED included, is bugcheck 0x119
    // with parameter 1 = 0x5. Stage A builds no paging buffer for any operation, known or not: leaving pDmaBuffer
    // where it was is how "no byte was written" is expressed, and that is a legal answer to every operation.
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
    // Stage B: page table updates are carried out by the CPU, here and now (vidmm.c); the paging buffer stays empty.
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE)
        VidMmUpdatePageTable(&pBuildPagingBuffer->UpdatePageTable);

    // ADR 0008 stage D (docs/design/paging-node.md): TRANSFER_VIRTUAL and FILL_VIRTUAL, node 1's own operations.
    // New arms, not a change to the UPDATE_PAGE_TABLE one above: with the gate closed VidMm is never told node 1
    // exists (WddmDriverCaps, Bc250WddmGetNodeMetadata), so by dxgkrnl's own contract it has no occasion to send
    // either - written defensively anyway (section 6), through the same gfx.c gate (GfxPagingBuild answers
    // BC250PagingNotReady, inertly, whenever Device->Gfx is NULL or the RUN escape has not reached stage 8).
    //
    // hSystemContext resolves to the same RootPhysical SetRootPageTable already recorded (design note section 2):
    // both virtual addresses are the paging process's own, so one root serves both Source and Destination -
    // TransferVirtual's own SourcePageTable/DestinationPageTable fields (for a transfer spanning two GPU MMU
    // contexts) are not read by this cut, an open item stated rather than silently assumed away (section 8).
    //
    // Only STATUS_SUCCESS and STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER ever leave this function (the comment at
    // its head): GfxPagingBuild's own STATUS_INVALID_PARAMETER answers (a malformed call this driver's own
    // callers never produce) are folded into STATUS_SUCCESS here, exactly like every other "cannot happen but
    // must not bugcheck" case in this file.
    if (wddm != NULL && (pBuildPagingBuffer->Operation == DXGK_OPERATION_VIRTUAL_TRANSFER ||
                         pBuildPagingBuffer->Operation == DXGK_OPERATION_VIRTUAL_FILL))
    {
        BOOLEAN fill = pBuildPagingBuffer->Operation == DXGK_OPERATION_VIRTUAL_FILL;
        BC250_WDDM_OBJECT* systemContext = WddmObject(pBuildPagingBuffer->hSystemContext, BC250_WDDM_MAGIC_CONTEXT);
        ULONGLONG root = (systemContext != NULL) ? systemContext->RootPhysical : 0;
        ULONGLONG bytes = fill ? pBuildPagingBuffer->FillVirtual.FillSizeInBytes : pBuildPagingBuffer->TransferVirtual.TransferSizeInBytes;
        ULONGLONG srcVa = fill ? 0 : pBuildPagingBuffer->TransferVirtual.SourceVirtualAddress;
        ULONGLONG dstVa = fill ? pBuildPagingBuffer->FillVirtual.DestinationVirtualAddress : pBuildPagingBuffer->TransferVirtual.DestinationVirtualAddress;
        ULONG pattern = fill ? pBuildPagingBuffer->FillVirtual.FillPattern : 0;
        // Review 23 MUST-FIX: DmaSize is the WHOLE buffer (the DDI contract's own naming - "current operation
        // offset in bytes from the start of the DMA buffer" for DmaBufferWriteOffset only makes sense against a
        // fixed total), not what is left once earlier operations in this same accumulated buffer have already
        // used DmaBufferWriteOffset bytes of it. Passing DmaSize itself as the room GfxPagingBuild may still
        // write into let it copy past the end of dxgkrnl's own pDmaBuffer on any call after the first one packed
        // into a shared buffer - masked today only because BC250_GFX_PAGING_SHADOW_BYTES happens to equal
        // BC250_WDDM_PAGING_BUFFER_BYTES (nothing ties the two together, see the SHOULD-FIX on GfxPagingShadowBytes
        // being dead code), not because this was actually safe.
        ULONG dmaFree = (pBuildPagingBuffer->DmaBufferWriteOffset < pBuildPagingBuffer->DmaSize)
                             ? pBuildPagingBuffer->DmaSize - pBuildPagingBuffer->DmaBufferWriteOffset : 0;
        ULONG written = 0;
        BC250_WDDM_PAGING_UNSUPPORTED unsupported = BC250PagingSupported;
        NTSTATUS pagingStatus = GfxPagingBuild((BC250_DEVICE*)hAdapter, root, fill, srcVa, dstVa, bytes, pattern,
                                               pBuildPagingBuffer->pDmaBuffer, pBuildPagingBuffer->DmaBufferWriteOffset,
                                               dmaFree, &written, &unsupported);
        if (pagingStatus == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
        {
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
            pBuildPagingBuffer->MultipassOffset = 0;    // nothing of this operation was written yet: redo it whole
            GuardLog("wddm: BuildPagingBuffer %s %llu bytes: insufficient buffer, %u free",
                     fill ? "fill" : "transfer", bytes, pBuildPagingBuffer->DmaSize);
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        }
        if (written != 0)
        {
            // The DDI's own words for pDmaBuffer: "[in/out] A virtual address to the first available byte in
            // the paging buffer ... Before the DxgkDdiBuildPagingBuffer function returns, the driver should
            // update pDmaBuffer to point past the last byte that is written to the paging buffer."
            // Until this line existed, E24 run 005 built four correct fills and dxgkrnl submitted none of them
            // (facts M108): leaving the pointer where it was is exactly how this DDI says "I wrote nothing",
            // and a paging buffer of zero length has nothing to submit. Every other arm of this function
            // genuinely writes nothing and leaves it alone on purpose - this is the one arm that must not.
            pBuildPagingBuffer->pDmaBuffer = (PVOID)((PUCHAR)pBuildPagingBuffer->pDmaBuffer + (SIZE_T)written * 4u);
            // Where this buffer starts in GPU address space and how far into it the packets now reach, for the
            // submission that will name an address inside it and nothing else (the PagingBuffers field's own
            // comment). After the write, so that a slot is only ever published for bytes that exist.
            WddmPagingBufferWritten(wddm, (LONG64)pBuildPagingBuffer->DmaBufferGpuVirtualAddress,
                                    pBuildPagingBuffer->DmaBufferWriteOffset,
                                    pBuildPagingBuffer->DmaBufferWriteOffset + written * 4u);
            if (fill) InterlockedIncrement(&wddm->PagingFillsBuilt); else InterlockedIncrement(&wddm->PagingTransfersBuilt);
            InterlockedExchangeAdd(&wddm->PagingBytesMoved, (LONG)(bytes > 0x7FFFFFFFull ? 0x7FFFFFFF : bytes));
        }
        else if (unsupported > BC250PagingSupported && unsupported < RTL_NUMBER_OF(wddm->PagingUnsupported))
        {
            InterlockedIncrement(&wddm->PagingUnsupported[unsupported]);
        }
    }
    return STATUS_SUCCESS;
}

static DXGKDDI_SUBMITCOMMAND Bc250WddmSubmitCommand;
static NTSTATUS Bc250WddmSubmitCommand(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SUBMITCOMMAND* pSubmitCommand)
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
    // ADR 0008 stage D (docs/design/paging-node.md sections 4 and 6): node 1's paging buffer, submitted through
    // this DDI (DISPATCH_LEVEL, exactly - the whole reason node 1's own path exists), never through
    // SubmitCommandVirtual's node-0 one. [Start, End) is the same coordinate space DmaBufferWriteOffset used
    // while BuildPagingBuffer filled Gfx->PagingShadowMem at those same offsets (design note section 4a); the
    // shadow, not DmaBufferPhysicalAddress, is what WddmSubmitPagingHardware actually reads.
    if (node == BC250_WDDM_NODE_COPY && device->Wddm != NULL && ((BC250_WDDM*)device->Wddm)->NodeCount > BC250_WDDM_NODE_COPY &&
        pSubmitCommand->DmaBufferSubmissionEndOffset > pSubmitCommand->DmaBufferSubmissionStartOffset &&
        GfxPagingSubmitReady(device) &&
        WddmSubmitPagingHardware(device, (BC250_WDDM*)device->Wddm, pSubmitCommand->DmaBufferSubmissionStartOffset,
                                 pSubmitCommand->DmaBufferSubmissionEndOffset - pSubmitCommand->DmaBufferSubmissionStartOffset,
                                 pSubmitCommand->SubmissionFenceId))
        return STATUS_SUCCESS;
    WddmCompleteSoftware(device, pSubmitCommand->SubmissionFenceId, node);
    return STATUS_SUCCESS;
}

// A UMD context's command buffer is the IB named in its BC2S blob. dxgkrnl copies that blob to the front of
// pDmaBufferPrivateData and reports its length in DmaBufferUmdPrivateDataSize (the slot itself is the size
// CreateContext asked for). One IB takes the gfx ring, still behind EnableGpuSubmit and a root page table.
// Two IBs are not half-run. A failure return from this DDI bugchecks, so a blob this reader refuses, or a
// ring that will not take it, is completed in software and logged. That retires the scheduler fence so the
// queue does not stall. It does not write the monitored fence the IB itself would have written, which is
// what the winsys waits on.
static void WddmSubmitUmd(_Inout_ BC250_DEVICE* Device, _In_opt_ BC250_WDDM* Wddm, _In_ const BC250_WDDM_OBJECT* Context,
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
                return;
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
                    return;
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
    WddmCompleteSoftware(Device, Submit->SubmissionFenceId, Node);
}

static DXGKDDI_SUBMITCOMMANDVIRTUAL Bc250WddmSubmitCommandVirtual;
static NTSTATUS Bc250WddmSubmitCommandVirtual(_In_ const HANDLE hAdapter,
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

    // ADR 0008 stage D, the half run 006 found missing. Node 1's paging buffer arrives HERE and not at
    // Bc250WddmSubmitCommand: VidMm creates the paging system context with DXGK_CREATECONTEXTFLAGS::
    // VirtualAddressing set (flags 0x5 in run 006's log, d3dkmddi.h line 1521), and a virtual-addressing context
    // is submitted by address. DXGKARG_SUBMITCOMMANDVIRTUAL carries no submission start/end offsets at all, so
    // the shadow range is recovered by WddmPagingBufferRange: DmaBufferVirtualAddress is the first byte of this
    // submission, the table holds the first byte of each buffer BuildPagingBuffer wrote into, and the difference
    // is the coordinate DmaBufferWriteOffset spoke in while the packets were written (design note section 4a).
    // A submission that names any buffer other than the one the shadow is holding is completed in software and
    // counted - never guessed at, because a wrong offset here does not draw a wrong picture, it runs a wrong DMA.
    if (node == BC250_WDDM_NODE_COPY && wddm != NULL && wddm->NodeCount > BC250_WDDM_NODE_COPY &&
        pSubmitCommand->DmaBufferSize != 0 && KeGetCurrentIrql() <= APC_LEVEL)
    {
        ULONG offset = 0, end = 0;

        if (!WddmPagingBufferRange(wddm, (LONG64)pSubmitCommand->DmaBufferVirtualAddress,
                                   pSubmitCommand->DmaBufferSize, &offset, &end))
        {
            if (InterlockedIncrement(&wddm->PagingVirtualUnmapped) <= BC250_WDDM_LOG_CALLS)
                GuardLog("wddm: node 1 submission at va 0x%llX, %u bytes: no paging buffer this driver wrote to "
                         "contains it - completed in software",
                         (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress, pSubmitCommand->DmaBufferSize);
        }
        else
        {
            if (end - offset != pSubmitCommand->DmaBufferSize &&
                InterlockedIncrement(&wddm->PagingVirtualClamped) <= BC250_WDDM_LOG_CALLS)
                GuardLog("wddm: node 1 submission at va 0x%llX names %u bytes, %lu were written: running "
                         "[0x%lX, 0x%lX) of the shadow", (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress,
                         pSubmitCommand->DmaBufferSize, (ULONG)(end - offset), offset, end);
            if (GfxPagingSubmitReady(device) &&
                WddmSubmitPagingHardware(device, wddm, offset, end - offset, pSubmitCommand->SubmissionFenceId))
                return STATUS_SUCCESS;
        }
    }

    // M8. A UMD context's packet is the IB in its BC2S blob, not the DMA buffer a present uses. Handled
    // here, before stage C, so a UMD submit can never fall through onto DmaBufferVirtualAddress. One IB
    // takes the same gated gfx-ring path. Two IBs are not half-run. This DDI still cannot fail.
    if (context != NULL && context->UmdContext)
    {
        WddmSubmitUmd(device, wddm, context, pSubmitCommand, node);
        return STATUS_SUCCESS;
    }

    // Stage C. An empty DMA buffer (every Present of stage A's inert DDI) has nothing to run; one with bytes in it
    // goes to the ring if the GPU is up (EnableGpuSubmit, stage 8, IH) and the context has a root. Everything else
    // is completed in software as before. The header says PASSIVE_LEVEL, and gfx.c's lock needs <= APC_LEVEL.
    // node == BC250_WDDM_NODE_3D: this hardware path is GfxSubmitIb's, the gfx ring at a fixed VMID, and it stays
    // node 0's alone - node 1 has its own arm above, its own ring (SDMA0, no VMID) and its own failure counters.
    if (node == BC250_WDDM_NODE_3D && pSubmitCommand->DmaBufferSize != 0 && context != NULL && context->RootPhysical != 0 &&
        device->Wddm != NULL && KeGetCurrentIrql() <= APC_LEVEL && GfxSubmitReady(device) &&
        WddmSubmitHardware(device, (BC250_WDDM*)device->Wddm, context, (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress,
                           pSubmitCommand->DmaBufferSize, pSubmitCommand->SubmissionFenceId, node))
        return STATUS_SUCCESS;
    WddmCompleteSoftware(device, pSubmitCommand->SubmissionFenceId, node);
    return STATUS_SUCCESS;
}

static DXGKDDI_PREEMPTCOMMAND Bc250WddmPreemptCommand;
static NTSTATUS Bc250WddmPreemptCommand(_In_ const HANDLE hAdapter, _In_ const DXGKARG_PREEMPTCOMMAND* pPreemptCommand)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);

    // Nothing is ever running here, which is the case the documentation covers explicitly: report the preemption
    // with the last fence that completed, rather than submit a preemption fence. The report is not made here: it
    // is recorded and handed to the same DPC the completions go through, which publishes any pending completion
    // first, so that LastCompletedFenceId is the fence dxgkrnl has just been told about and not the one before
    // it. A failure return would bugcheck, so this returns success whatever the DPC finds.
    if (WddmFirstCalls(wddm, WddmDdiPreemptCommand))
        GuardLog("wddm: PreemptCommand fence %u node %u", pPreemptCommand->PreemptionFenceId, pPreemptCommand->NodeOrdinal);
    WddmPreemptFence(device, pPreemptCommand->PreemptionFenceId, pPreemptCommand->NodeOrdinal);
    return STATUS_SUCCESS;
}

static DXGKDDI_RESETFROMTIMEOUT Bc250WddmResetFromTimeout;
static NTSTATUS Bc250WddmResetFromTimeout(_In_ const HANDLE hAdapter)
{
    // A failure return bugchecks, and nobody can reset this GPU (facts M53). What stage C can do, it does: the ring
    // path is closed for this device start (GfxSubmitFail, sticky), and the packet in flight, if there is one, is
    // forgotten WITHOUT a report - after this DDI the scheduler treats every submitted fence as completed by itself
    // (ref graphics-driver-samples, CosKmdAdapter.cpp ResetFromTimeout: "Implicitly sync up"), so the driver only
    // brings its own last completed fence up to date. No register is touched: halting the CP the way the undo path
    // does (ADR 0008 point 7, facts M44) stays with the escape, where a person decides it.
    //
    // These two are the only DDIs in the file that log on every call rather than the first few: a TDR means the
    // scheduler has decided this adapter is hung, which changes what the whole run means, and a run where it
    // happens a hundred times is a different result from one where it happens once.
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BOOLEAN pending = FALSE, pagingPending = FALSE;
    UINT fence = 0, pagingFence = 0;
    KIRQL irql;

    (void)WddmFirstCalls(wddm, WddmDdiResetFromTimeout);
    if (device != NULL) { GfxSubmitFail(device); GfxPagingSubmitFail(device); }
    if (wddm != NULL)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        if (wddm->HwPending)
        {
            pending = TRUE;
            fence = wddm->DeferredValid ? wddm->DeferredFence : wddm->HwFence;
            wddm->HwPending = FALSE;
            wddm->DeferredValid = FALSE;
            KeCancelTimer(&wddm->SubmitTimer);
            if ((LONG)fence > wddm->LastCompletedFence) InterlockedExchange(&wddm->LastCompletedFence, (LONG)fence);
        }
        // ADR 0008 stage D: node 1's own channel, checked and forgotten independently of node 0's above - the
        // scheduler's ResetFromTimeout is adapter-wide, not per-node, so both must be quiet before this returns
        // (design note section 6).
        if (wddm->PagingHwPending)
        {
            pagingPending = TRUE;
            pagingFence = wddm->PagingHwFence;
            wddm->PagingHwPending = FALSE;
            KeCancelTimer(&wddm->PagingSubmitTimer);
            if ((LONG)pagingFence > wddm->LastCompletedFence) InterlockedExchange(&wddm->LastCompletedFence, (LONG)pagingFence);
        }
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    if (pending) GuardLog("wddm: *** ResetFromTimeout with fence %u on the ring: ring path closed, fence dropped unreported ***", fence);
    else GuardLog("wddm: *** ResetFromTimeout: the scheduler timed this adapter out (nothing of ours was on the ring) ***");
    if (pagingPending)
        GuardLog("wddm: *** ResetFromTimeout with fence %u on the SDMA0 ring: node 1 ring path closed, fence dropped unreported ***", pagingFence);
    return STATUS_SUCCESS;
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
    // Stage A completes every fence in software, in the DPC that follows the submit (node 1 too, once its own
    // hardware channel retires one): the "engine" cannot stall either way.
    pQueryEngineStatus->EngineStatus.Value = 0;
    pQueryEngineStatus->EngineStatus.Responsive = 1;
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
        if (alloc->Format != (ULONG)D3DDDIFMT_A8R8G8B8 && alloc->Format != (ULONG)D3DDDIFMT_X8R8G8B8) why = "source format";
        else if (alloc->Width == 0 || alloc->Height == 0 || alloc->Pitch < alloc->Width * 4ull ||
                 alloc->Size < (ULONGLONG)alloc->Pitch * alloc->Height || alloc->Size > 0x10000000ull) why = "source geometry";
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
    map = (const UCHAR*)MmMapIoSpaceEx(physical, (SIZE_T)alloc->Size, PAGE_READONLY | PAGE_NOCACHE);
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

        if (DcnScanoutMapping(device, &flipMap, &flipLength))
        {
            dst = (UCHAR*)flipMap;
            dstLength = flipLength;
            toFlip = TRUE;
        }
        else
        {
            mapFailed = TRUE;
        }
    }

    // M115: dirty sub-rectangles assume the destination already holds the previous frame. The firmware
    // framebuffer does (M84). A flip target does not. M116: the source allocation does not either, outside
    // the rectangles GDI just wrote. M117: copying Device->Framebuffer, the write-combined BAR0 mapping
    // (Post.PhysicAddress 0xC0000000, M20), produced a frame that was 90% zero. M32: a CPU read of BAR0 is
    // not a coherent view of VRAM. The address HUBP was scanning before this flip is DcnFirmwareAddress,
    // captured from the register; M92 measured that as the carve-out base, which is VramPhysical (M31).
    // Seed from that physical address, once per flip target, then the dirty rectangles on top.
    if (toFlip && device->VramEnabled && device->DcnScanoutSeedAddress != device->DcnCurrentAddress &&
        device->FramebufferLength != 0 && device->VramLength != 0)
    {
        ULONGLONG vramBase = (ULONGLONG)device->VramPhysical.QuadPart;
        ULONGLONG seedAt = vramBase;
        SIZE_T seedBytes = dstLength < device->FramebufferLength ? dstLength : device->FramebufferLength;
        PHYSICAL_ADDRESS seedPhys;
        PVOID seedMap;

        if (device->DcnFirmwareKnown && device->DcnFirmwareAddress >= vramBase &&
            device->DcnFirmwareAddress - vramBase < device->VramLength)
            seedAt = device->DcnFirmwareAddress;
        if (seedAt != device->DcnCurrentAddress &&
            (ULONGLONG)seedBytes <= device->VramLength - (seedAt - vramBase))
        {
            seedPhys.QuadPart = (LONGLONG)seedAt;
            seedMap = MmMapIoSpaceEx(seedPhys, seedBytes, PAGE_READONLY | PAGE_NOCACHE);
            if (seedMap != NULL)
            {
                ULONG firstPixel = *(volatile ULONG*)seedMap;

                RtlCopyMemory(dst, seedMap, seedBytes);
                MmUnmapIoSpace(seedMap, seedBytes);
                device->DcnScanoutSeedAddress = device->DcnCurrentAddress;
                InterlockedIncrement(&wddm->BlitSeeds);
                GuardLog("wddm: flip target seeded from VRAM physical 0x%llX, %lu bytes, first pixel 0x%08X, onto 0x%llX",
                         seedAt, (ULONG)seedBytes, firstPixel, device->DcnCurrentAddress);
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
        if (right > (LONG)device->Post.Width) right = (LONG)device->Post.Width;
        if (bottom > (LONG)device->Post.Height) bottom = (LONG)device->Post.Height;
        if (left + dx < 0 || top + dy < 0 || right + dx > (LONG)alloc->Width || bottom + dy > (LONG)alloc->Height) continue;
        if (right <= left || bottom <= top) continue;
        for (y = top; y < bottom; y++)
        {
            // The geometry (pitch, and so this offset) is always the firmware's mode, Device->Post - the one
            // the VidPn primary is pinned to (display.c's Bc250CommitVidPn) - whichever buffer dst points at;
            // only the bound changes between the two destinations. dstLength is one of the two driver-owned
            // lengths set above, never a number that came from user mode.
            SIZE_T dstOff = (SIZE_T)y * device->Post.Pitch + (SIZE_T)left * 4;
            SIZE_T src = (SIZE_T)(y + dy) * alloc->Pitch + (SIZE_T)(left + dx) * 4;
            SIZE_T bytes = (SIZE_T)(right - left) * 4;

            if (dstOff + bytes > dstLength || src + bytes > alloc->Size) break;
            RtlCopyMemory(dst + dstOff, map + src, bytes);
            rows++;
        }
    }
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
    else
    {
        InterlockedIncrement(&wddm->BlitsToFirmware);
        if (mapFailed) InterlockedIncrement(&wddm->BlitsMapFailed);
    }
    if (verbose)
        GuardLog("wddm: blit %u rectangles, %u rows copied, destination %s%s", count, rows,
                 toFlip ? "flipped surface" : "POST framebuffer", mapFailed ? " (mapping failed, fell back)" : "");
}

static DXGKDDI_PRESENT Bc250WddmPresent;
static NTSTATUS Bc250WddmPresent(_In_ const HANDLE hContext, _Inout_ DXGKARG_PRESENT* pPresent)
{
    BC250_WDDM_OBJECT* context = WddmObject(hContext, BC250_WDDM_MAGIC_CONTEXT);

    // FlipOnVSyncMmIo is what this driver claims, so a flip generates no DMA at all: the flip itself happens in
    // SetVidPnSourceAddress. Stage A has no engine, so a blt or a colour fill generates no DMA either - they are
    // accepted and produce nothing, because a failure here would take the desktop down and stage A exists to see
    // the desktop survive. pDmaBuffer is left where it was: not one byte of the buffer is used.
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
    // 0.7.16 (E20 H3): a Blt is translated and, with EnablePresentBlit, copied to the firmware framebuffer. Flips and
    // colour fills still produce nothing; pDmaBuffer stays untouched either way.
    if (context != NULL && pPresent->Flags.Value == 1 /* Blt alone */ && pPresent->pAllocationList != NULL &&
        ((ULONG_PTR)pPresent->pAllocationList & (PAGE_SIZE - 1)) <= PAGE_SIZE - BC250_WDDM_PRESENT_LIST_QWORDS * sizeof(ULONGLONG))
        WddmPresentBlit(context, pPresent);
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
    // With Device->VidPnFlipEnabled closed there is exactly one scan-out address on this adapter and the
    // firmware programmed it; changing it needs the display core, which this driver does not touch outside the
    // gate (ADR 0006 point 2). The request is recorded, the firmware's framebuffer keeps scanning out, and
    // display.c stays the only owner of that mapping. The address is what the next CRTC_VSYNC report carries,
    // which is how dxgkrnl learns that this flip has retired - open, the same report still carries it, but the
    // scanout has actually moved there (ADR 0011 point 3 step 3, DcnFlipSourceAddress below).
    if (wddm != NULL)
    {
        // A call that names the address that is already being scanned out is not a flip; DWM redrawing into one
        // buffer looks exactly like that. Counting only the changes is what says whether anything is double
        // buffered on an adapter with no user-mode driver.
        //
        // No lock, and none can be taken here (this DDI may run above DISPATCH_LEVEL): the hand-off to the VSync
        // report is one interlocked exchange, so the reader sees the old address or the new one and never half
        // of each, and the value it replaced says whether this was a flip. The segment stored next to it is read
        // by nothing but a debugger.
        BOOLEAN changed = InterlockedExchange64(&wddm->PrimaryAddress.QuadPart, pSetVidPnSourceAddress->PrimaryAddress.QuadPart) !=
                          pSetVidPnSourceAddress->PrimaryAddress.QuadPart;

        if (changed) InterlockedIncrement(&wddm->Flips);
        wddm->PrimarySegment = pSetVidPnSourceAddress->PrimarySegment;
        if (high) InterlockedIncrement(&wddm->FlipsAboveDispatch);

        // 0.7.24, ADR 0011 point 3 step 3: a real address change is also the M87 write sequence onto HUBP0/OTG0
        // (DcnFlipSourceAddress, dcn.c) - never gated on !high, unlike WddmVSyncArm below: MmioDcnWrite takes no
        // lock (mmio.c) and is legal at any IRQL. Not called when the address did not change: a redundant flip
        // to the same surface would only add MMIO churn and a spurious trigger, never a visible difference.
        if (changed && device->VidPnFlipEnabled)
            (void)DcnFlipSourceAddress(device, (ULONGLONG)pSetVidPnSourceAddress->PrimaryAddress.QuadPart, NULL);
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

    // Answered from the timer's phase and nothing else: how far into the period we are gives the scan line, and
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
