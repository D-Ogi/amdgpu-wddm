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
#include "vmid_pool.h"
#include "startup.h"
#include "bc250_gfx.h"
#include "dcn_translate.h"
#include "umd_blob.h"
#include "hip_journal.h"
#include "umd_caps.h"
#include "gpu_clock.h"            // BD-056: the SMUIO TSC read and its pairing with the QPC
#include "firmware_metadata.h"
#include "gfx_completion_queue.h"
#include "gfx_blt.h"
#include "hang_recovery.h"
#include "submit_watchdog.h"     // BD-114: the private submit watchdog's budget, its progress window and its stamps
#include "gfx_copy.h"
#include "paging_private.h"
#include "paging_ddi_status.h"   // BD-235: the statuses that BuildPagingBuffer may give to the OS
#include "paging_drain.h"
#include "object_index.h"
#include "ring_gap.h"            // C48/C49: the node's own idle-gap accounting, the same object the ETW analysis used
#include "notify_pairing.h"      // C50: which pass pairs a completion report with DxgkCbNotifyDpc
#include "flip_state.h"          // BD-170 r6: the decisions of the flip state machine
#include <limits.h>               // UINT_MAX, ULLONG_MAX: the UNKNOWN values of shared/d3dkmdt.h (BD-170 E5)
// shared/d3dkmdt.h writes DXGK_PRIMITIVE_API_SEQUENCE_NUMBER_UNKNOWN as ULONGLONG_MAX, a name of ntintsafe.h. That
// header redefines stdint.h's UINT32_MAX in this file, so the name gets limits.h's value of the same type instead.
#ifndef ULONGLONG_MAX
#define ULONGLONG_MAX ULLONG_MAX
#endif
#include "bc250kmd_escape.h"     // BC250_PJ_* record kinds of the paging journal
#include "regs.generated.h"      // the CP/GRBM/GCVM offsets of the timeout snapshot (tools/regcalc)
#include "ih_fault.h"            // GCVM_L2_PROTECTION_FAULT_STATUS field decode and the gfxhub CID names
#include <ntstrsafe.h>

#define BC250_WDDM_TAG 'wW2B'
#define BC250_WDDM_LOG_CALLS 8              // how many first calls of each DDI reach the guard log
#define BC250_WDDM_PRESENT_LIST_QWORDS 12u  // how much of a present's allocation list is read: 3 entries of either arm
// KMD196: buckets of the held-submission histogram, microseconds. The edges are chosen so that the regimes this
// change is about fall in different buckets and cannot be confused in a summary: the spin catch (<100, 100-199),
// the plain submit cost of about 550 us plus the ~130 us the blocking job still had to run (500-999), the 4.7 ms
// hold being removed (2000-4999), and the 14-16 ms clock-tick mode of the LOW session (10000-19999). The edges
// themselves are in WddmHoldBucket, next to the names.
#define BC250_WDDM_HOLD_BUCKETS 9

// Segment ids are one-based: DXGK_QUERYSEGMENTOUT4.PagingBufferSegmentId is "the index (starting from 1)".
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_SEGMENT_TABLES 3u
// An aperture segment, as Microsoft's RosKmd has one: VidMm backs it with system pages and asks for them to be mapped
// with BuildPagingBuffer (MapApertureSegment), which stage A answers inertly like every other operation. It exists
// because a GPU-VA context's DMA buffers must be VidMm allocations in an aperture segment: with "system memory"
// (segment set 0) dxgmms2 maps a NULL allocation into the context's address space and the machine goes down
// (E16 run 008, VIDMM_DMA_POOL::AddDmaBufferToPool). Stage B gives it the real GART behind it.
#define BC250_WDDM_SEGMENT_APERTURE 2u
// Its size is no longer a constant (0.7.216.8): WddmStart latches ApertureSegmentMegabytes into
// Device->WddmApertureRequest, and Device->WddmAperture.bytes is what the GART capture granted.
#define BC250_WDDM_SEGMENT_SET(id) (1u << ((id) - 1))
#define BC250_WDDM_NODE_3D 0u
// ADR 0008 stage D (docs/design/paging-node.md): node 1, DXGK_ENGINE_TYPE_COPY on SDMA0, the paging node,
// behind EnablePagingNode. BC250_WDDM_NODE_COUNT is no longer a compile-time fact - wddm->NodeCount (1 or 2,
// WddmStart) is what every bound check and caps answer below now reads; the macro stays only as the value that
// field is initialized to with the gate closed, so that a grep for "how many nodes" still finds one definition.
#define BC250_WDDM_NODE_COPY 1u
#define BC250_WDDM_NODE_COUNT 1u            // gate closed: the value wddm->NodeCount starts at, and the only one C_ASSERT still checks
// WDDM node 2 (docs/design/compute-node.md): DXGK_ENGINE_TYPE_OTHER on MEC compute ring 1, behind
// the node 2 selector (open in the release flavour) and the paging node: wddm->NodeCount is 3 only when node 1 exists too, so the
// compute node's ordinal is always 2, the ordinal the RADV winsys names (RADV_DXGI_COMPUTE_NODE).
#define BC250_WDDM_NODE_COMPUTE 2u
#define BC250_WDDM_NODE_COUNT_MAX 3u        // sizes every per-node array below, gates open or closed
#define BC250_WDDM_REPORT_RETRY_MAX 4L      // passes a completion report may be retried before it is dropped

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

// KMD183: the index of BC250_WDDM.PagingXferCount/PagingXferBytes. Virtual transfers are sorted by
// TransferVirtual.TransferDirection (d3dkmddi.h DXGK_MEMORY_TRANSFER_DIRECTION), physical ones by which end is
// segment 0 (system memory). WddmCountTransfer is the only writer.
typedef enum _BC250_WDDM_XFER {
    BC250WddmXferVirtualToSystem = 0,   // DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM
    BC250WddmXferVirtualFromSystem,     // DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL
    BC250WddmXferVirtualOther,          // DXGK_MEMORY_TRANSFER_LOCAL_TO_LOCAL and anything newer
    BC250WddmXferPhysicalToSystem,      // Transfer.Destination.SegmentId == 0
    BC250WddmXferPhysicalFromSystem,    // Transfer.Source.SegmentId == 0
    BC250WddmXferPhysicalOther,         // segment to segment
    BC250WddmXferKinds
} BC250_WDDM_XFER;

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

#include "gdi_private.h"
#define BC250_ADMISSION_COUNT(p) ((void)InterlockedIncrement(p))
#include "gdi_admission.h"
#include "surface_resource_private.h"
#include "present_range.h"
#include "present_snapshot.h"        // BD-065: the refusal reasons of the GPU Present allocation snapshot
#include "scanout_admit.h"           // M15.14: which allocation SetVidPnSourceAddress may scan out
#include "gdi_emit.h"                // HLK gap G1: the GDI contract layer and its DMA_DATA stream
#include "gdi_dispatch.h"            // HLK gap G1 increment 2: the compute dispatch of the six kernels
#include "gdi_batch.h"               // HLK gap G1 increment 2: the capacity contract of one call
#include "gdi_walk.h"                // HLK gap G1: the bounded walk of the OS command buffer

#define BC250_GDI_BIND_SLOTS 4u                  // the most allocation-list entries one command names
// One pass must hold the whole command buffer, because the guaranteed contract DMA buffer model
// gives no second one (display/using-the-guaranteed-contract-dma-buffer-model.md; the whole rule is
// quoted at gdi_batch.h). So the DMA buffer is not a round number chosen here: it is computed from
// the largest command buffer this driver undertakes to translate, by the arithmetic of the capacity
// induction itself (Bc250GdiContextDmaBytes). The sub-rectangles of a command live inside the
// command, so a command buffer of L bytes holds at most (L - 8) / 16 of them, each costing at most
// one bounded dispatch; the 192 KiB bound therefore comes to 3.8 MB of DMA buffer and 12287
// guaranteed sub-rectangles in one call, and the 96 KiB floor to 1.9 MB and 6143. The bound stops
// there because the whole buffer is submitted as one indirect buffer, whose length field settles
// what any translation may emit (rule 3 of gdi_batch.h, BC250_GDI_IB_DWORDS_MAX): the capacity of
// each call is computed from that length as well, so neither a large DMA buffer nor a sequence of
// calls into one buffer can produce a stream the submitter would have to refuse. Below the bound a
// context was created with, Bc250GdiBatchBegin cannot refuse; above it a call is counted in
// GdiRenderCapacityShort, its loss in GdiRenderOmittedSubRects, and every GDI context created
// afterwards asks for a buffer that fits it. GdiRenderPeakCommandLength says what the field
// actually produces. The bound itself is BC250_GDI_CMDBUF_BOUND, in gdi_batch.h with the proof and
// the host test that checks it.
// The command buffer of the ordinary producer, which is the input scope the design establishes:
// cdd.dll allocates 65536 bytes and submits what it has written of it, on every build that was read
// (docs/design/gdi-hw-accel.md names them). Nothing in the DDI enforces it and no translation
// decision is taken on it. It is here so that the field can say whether that scope was right: a
// longer call is counted in GdiRenderAboveProducer, and a refusal at or below it in
// GdiRenderScopeRefused, which is the number that would mean this design's input bound is wrong.
#define BC250_GDI_PRODUCER_BYTES 65536u
// The smallest DMA buffer that can hold one row pair beside the cache operation, as a multiple of
// the ring's eight-dword IB granule. A buffer below it is not translated at all.
#define BC250_GDI_MIN_DWORDS 16u
#include "../contract/bc250_scanout_caps.h"  // the trailer that publishes this start's scan-out answer
// The two numbers gdi_batch.h's input bound is written with. sizeof(RECT) is the stride of the
// sub-rectangle array, and ColorFill is the smallest command that can carry one at all - the other
// five arguments are larger, and the one smaller command, Escape, carries no sub-rectangle ("Driver
// ignores this command"). If a future WDK changes either, the build stops here instead of inflating
// a bound that no longer follows from the headers.
C_ASSERT(sizeof(RECT) == BC250_GDI_SUBRECT_BYTES);
C_ASSERT(FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command) + sizeof(DXGK_GDIARG_COLORFILL) ==
         BC250_GDI_MIN_COMMAND_BYTES);
// The floor of the count bound is the command header, because a sub-rectangle array may overlap its
// own command's fixed part (gdi_batch.h, BC250_GDI_RECT_FLOOR_BYTES).
C_ASSERT(FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command) == BC250_GDI_RECT_FLOOR_BYTES);
C_ASSERT(sizeof(DXGK_GDIARG_COLORFILL) <= sizeof(DXGK_GDIARG_BITBLT));
C_ASSERT(sizeof(DXGK_GDIARG_COLORFILL) <= sizeof(DXGK_GDIARG_ALPHABLEND));
C_ASSERT(sizeof(DXGK_GDIARG_COLORFILL) <= sizeof(DXGK_GDIARG_STRETCHBLT));
C_ASSERT(sizeof(DXGK_GDIARG_COLORFILL) <= sizeof(DXGK_GDIARG_TRANSPARENTBLT));
C_ASSERT(sizeof(DXGK_GDIARG_COLORFILL) <= sizeof(DXGK_GDIARG_CLEARTYPEBLEND));
C_ASSERT(sizeof(BC250_WDDM_ALLOCATION_PRIVATE)==32);
C_ASSERT(sizeof(BC250_GDI_PRIVATE)==48);

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
    void* HashNext;                     // BC250_WDDM::ObjectIndex chain (object_index.h), under BC250_WDDM::Lock
    ULONGLONG Serial;                   // adapter-unique, nonzero, never reused: given under the lock at insertion
    ULONG Magic;
    BC250_DEVICE* Device;
    UINT NodeOrdinal;                   // contexts
    ULONGLONG RootPhysical;             // contexts: the root page table VidMm last set, as a physical address; 0 = none
    PAGING_CAPTURE_OWNER Captures;     // contexts: CPU-only plans, released on completion or object teardown
    HANDLE OwnerDevice;                 // contexts/opened allocations: DDI device identity
    HANDLE BackingAllocation;           // opened: verified CreateAllocation object, never guessed. A value only:
    ULONGLONG BackingSerial;            // valid while an indexed ALLOCATION at that address has this Serial
                                        // (WddmBackingAllocationLocked); nothing clears it when the allocation goes
    UINT AllocationListSize;            // contexts: what CreateContext answered, i.e. how long a list dxgkrnl keeps for it
    BC250_WDDM_ALLOCATION_PRIVATE Allocation;
    ULONG GdiType;                      // retained standard-surface contract, zero for legacy LB7A
    // M8: a context or allocation that arrived as a contract blob (umd_blob.c), not the GDI one above.
    // ExAllocatePool2 zeroes these, so a GDI object stays "not UMD" without a store. UmdRequestedVa is
    // recorded and not applied: VidMm places the pages, and the winsys maps the GPU VA itself.
    BOOLEAN UmdAlloc;
    BOOLEAN UmdContext;
    BOOLEAN SystemContext;
    // BD-167 (V2): dxgkrnl's DXGK_CREATECONTEXTFLAGS.GdiContext, kept so the non-UMD submit arms admit a context
    // by the class dxgkrnl assigned it (SystemContext for VidMm/DWM, GdiContext for the CDD) rather than only by
    // "has no UMD private data". A UMD context is never a GDI context (CreateContext refuses that combination).
    // HLK gap G1: it is also "the context that is used for GDI Hardware Acceleration"
    // (display/initialization-and-dma-buffer-creation.md), and DxgkDdiRenderGdi admits no other context.
    BOOLEAN GdiContext;
    // Rule 3 of gdi_batch.h, per GDI context. GdiDmaBytes is the DMA buffer size this context asked
    // dxgkrnl for, which the DDI says is a floor the operating system keeps, so a call at the start
    // of a buffer can check that sentence. GdiDma holds the DMA buffers this context has written
    // into and not submitted since, which is where the dwords ahead of a call come from: dxgkrnl
    // may fill one buffer with several calls before it submits it, and the whole buffer leaves as
    // one indirect buffer. The DDI promises neither one call per buffer nor an order of calls and
    // submissions, so nothing here is inferred from the order of addresses: a call this map cannot
    // place is refused as a whole and counted. GdiDmaLock serialises the map alone, and is never
    // held across the translation itself.
    ULONG GdiDmaBytes;
    BC250_GDI_DMA_MAP GdiDma;
    KSPIN_LOCK GdiDmaLock;
    // BD-127: this context's BC2C blob carried BC250_UMD_C_COMPUTE and it was counted into the adapter's live
    // compute-context count. The flag is what DestroyContext counts out again, so a context that was never
    // counted (a GDI context, a refused blob, a version 2 blob) cannot lower the count.
    BOOLEAN ComputeContext;
    unsigned long UmdIpType;
    unsigned long UmdHeap;
    ULONGLONG UmdBytes;
    ULONGLONG UmdRequestedVa;
    // KMD193 (bsod-245 items 3 and 4): identity the journal records of this object's destroy and of every job
    // submitted on it would otherwise not have. Values only, taken at CreateAllocation/CreateContext; a dump
    // reader reads them out of the journal, never out of the object, which may be freed by then.
    // M15.14: the scan-out request its creator made, recorded at CreateAllocation and re-derived at
    // every flip by Bc250ScanoutAdmit. Scanout* describe the surface for a BC2A allocation, which has
    // no LB7A description of its own; an LB7A one keeps describing itself in Allocation above and these
    // stay zero. ExAllocatePool2 zeroes them, so an allocation that asked for nothing asks for nothing.
    BOOLEAN ScanoutRequested;
    ULONG ScanoutWidth, ScanoutHeight, ScanoutPitch, ScanoutFormat;
    unsigned long UmdBlobVersion;       // allocations: the BC2A version word (0 = not a UMD allocation)
    ULONGLONG UmdGemFlags;              // allocations: the BC2A gem_flags
    unsigned long CreatorProcessId;     // allocations and contexts: PsGetCurrentProcessId at creation
    volatile LONG InteropUser;          // devices: 1 once an interop Blt present of it was counted (interop.c)
    BC250_HIP_JOURNAL_OWNER HipJournalOwner;
} BC250_WDDM_OBJECT;

#define BC250_PRESENT_OBSERVATIONS 16
#define BC250_GPU_PRESENT_REFUSAL_LOGS 16     // detailed lines for the first snapshot refusals of a start
// One writer per slot; immutable after Published. Handles are values only.
// Retained per adapter start, independently of the rolling GuardLog.
typedef struct _BC250_PRESENT_OBSERVATION {
    volatile LONG Published;
    HANDLE Context, OwnerDevice, Handles[2];
    UINT Flags, Node, ListSize, DmaBytes, PrivateBytes, Offset, SubRects;
    UINT PhysicalAdapter[2];
    BOOLEAN UmdContext, SystemContext, ListValid, SnapshotValid;
    ULONGLONG InterruptTime, Qpc, Va[2];
    RECT Src, Dst;
    BC250_WDDM_ALLOCATION_PRIVATE Allocations[2];
} BC250_PRESENT_OBSERVATION;

// The software VSync. FlipOnVSyncMmIo means dxgkrnl retires a queued flip when the driver reports
// DXGK_INTERRUPT_CRTC_VSYNC, and nothing else retires it: with MaxQueuedFlipOnVSync = 1 and no report at all, the
// second flip never leaves the queue and DWM stops. Stage A may not read the display core (ADR 0006 point 2), so
// there is no real VSync to report - a periodic timer stands in for it. The firmware's mode carries no refresh
// rate (display.c reports D3DKMDT_FREQUENCY_NOTSPECIFIED for exactly that reason), so 60 Hz is the assumption.
// KeSetTimerEx takes whole milliseconds, so the period is 16 ms and the real rate is about 62.5 Hz; the system
// clock's own granularity is coarser than that error, and nothing in stage A depends on the exact number.
#define BC250_WDDM_VSYNC_HZ 60
#define BC250_WDDM_VSYNC_MS (1000 / BC250_WDDM_VSYNC_HZ)

// BD-170: one call of ControlInterrupt for DXGK_INTERRUPT_CRTC_VSYNC (WddmVSyncControlNote). Call is 1 for the first
// call of this start, Qpc its KeQueryPerformanceCounter, Owed the address of a next-VSync success that no report
// carried yet at the time (0: none).
#define BC250_WDDM_VSYNC_CONTROL_EVENTS 16u
typedef struct _BC250_WDDM_VSYNC_CONTROL {
    LONG Call;
    BOOLEAN On;
    LONGLONG Qpc;
    LONGLONG Owed;
} BC250_WDDM_VSYNC_CONTROL;

// BD-170 r6: the record of one W-u deposit (flip_state.h, docs/design/vidpn-flip.md section 7.2), written inside L by
// the flip transaction and closed by the writer's End. The last BC250_WDDM_WU_RECORDS are kept.
#define BC250_WDDM_WU_RECORDS 8u
typedef struct _BC250_WDDM_WU_RECORD {
    LONG Record;                // 1, 2, ... in the order of the deposits; 0 for a slot never written
    ULONG Code;                 // BC250_FLIP_RECORD_WRITER_UNPROVEN
    ULONGLONG Request;          // the request's card address
    ULONG Flags;                // its DXGK_SETVIDPNSOURCEADDRESS_FLAGS
    ULONG Generation;           // the primary generation at the deposit (odd: the writer's)
    ULONG Writer;               // the writer's BC250_WRITER_*
    ULONG Deposit;              // DepositSequence of this deposit
    BOOLEAN Unblank;            // the writer's unblank programmed this deposit
    ULONG End;                  // BC250_WU_*, BC250_WU_OPEN until the writer's End
    LONGLONG DepositQpc;
    LONG ToEndUs;               // from the deposit to the writer's End; -1 until then
} BC250_WDDM_WU_RECORD;
// The summary's copy of them, taken in one section (wddm.c's WddmWuCopy).
typedef struct _BC250_WDDM_WU_COPY {
    LONG Next;
    LONG ToEndMaxUs;
    BC250_WDDM_WU_RECORD Records[BC250_WDDM_WU_RECORDS];
} BC250_WDDM_WU_COPY;

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
// BD-170 r6, the lab flavour's witnesses (section 7.2), never in the release flavour: one immediate request that edge E
// programmed, with the shipping decision first (Outcome, Result, Q) and the witnesses read after it (E, R).
#define BC250_WDDM_WITNESS_RECORDS 16u
typedef struct _BC250_WDDM_WITNESS_RECORD {
    LONG Request;               // 1, 2, ... for the immediate requests edge E programmed
    ULONG Generation;           // the transaction's (odd) primary generation
    ULONG Flags;                // DXGK_SETVIDPNSOURCEADDRESS_FLAGS
    ULONG Outcome;              // DcnImmediateObserve's BC250_DCN_IMMEDIATE_*, the shipping reader's
    ULONG Result;               // Bc250FlipResult: the shipping decision, made before any witness read
    BOOLEAN Q;                  // P' (DONE or DONE_LATE)
    BOOLEAN Fresh;              // the flip event was cleared before the programming (DcnWitnessArm)
    BOOLEAN E;                  // SURFACE_FLIP_OCCURRED after the decision
    BOOLEAN R1, R2;             // EARLIEST_INUSE names the request: low word first, high word first
    NTSTATUS ArmStatus, ReadStatus;
    ULONGLONG Card, Physical;   // the request, and as the plane holds it
    ULONGLONG Earliest1, Earliest2;     // EARLIEST_INUSE, masked, in the two orders
    ULONG InterruptBefore, InterruptCleared, Interrupt;
    ULONG PendingBefore, PendingAfter;
    ULONG Low1, High1, High2, Low2;     // EARLIEST_INUSE raw words in read order
} BC250_WDDM_WITNESS_RECORD;
typedef struct _BC250_WDDM_WITNESS_COPY {
    LONG Requests, ReadFailed, Stale, Qe, Qr, Er, Order, Lost, Kept;
    BC250_WDDM_WITNESS_RECORD Records[BC250_WDDM_WITNESS_RECORDS];
    BC250_WDDM_WITNESS_RECORD Last;
} BC250_WDDM_WITNESS_COPY;
#endif

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

// M15.14 increment 2: the E26R resource record a CreateAllocation arrived with, as a counter index.
// Version 0 is a call with no record, including standard allocations and the compositor's own primary.
// Versions 1 to 3 keep their original buckets. The v4 private GPU-only group uses OTHER; the separate
// lb7a cpu view summary counts that explicit intent without changing the scan-out table's shape.
#define BC250_WDDM_RECORD_NONE 0u
#define BC250_WDDM_RECORD_V3 3u
#define BC250_WDDM_RECORD_OTHER 4u
#define BC250_WDDM_RECORD_KINDS 5u
static __inline ULONG WddmRecordKind(unsigned long Version)
{
    return Version <= BC250_WDDM_RECORD_V3 ? (ULONG)Version : BC250_WDDM_RECORD_OTHER;
}

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
    BOOLEAN RetainedPowerPause; // Stopping also closes private work during retained suspend
    LIST_ENTRY Objects;                 // devices, contexts, processes and allocations alive
    LONG ObjectCount;
    // The same objects by address, for membership tests without a scan (object_index.h). Joined and left in the
    // critical sections that join and leave Objects; the stop drains Objects and frees the buckets.
    BC250_OBJECT_INDEX ObjectIndex;
    ULONGLONG ObjectSerial;             // last BC250_WDDM_OBJECT::Serial given, under Lock

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
    BC250_HANG_NODE_STATE Recovery[BC250_WDDM_NODE_COUNT_MAX]; // under Lock, per-node completion/publication/reset
    // BD-114: the newest fence id dxgkrnl has submitted on this node, in the wrap-aware order
    // bc250_fence_reached uses. SubmittedFence above is NOT this value - it carries the fence of a completion
    // waiting to be reported - and the engine-reset contract's upper bound (hang_recovery.h,
    // Bc250AbortedFenceValid) needs the real last submitted one. Written in the SubmitCommandVirtual wrapper,
    // the one place every submission of either node passes through, whether or not it reaches the ring.
    volatile LONG LastSubmittedFence[BC250_WDDM_NODE_COUNT_MAX];
    BOOLEAN LastSubmittedValid[BC250_WDDM_NODE_COUNT_MAX];
    BOOLEAN RefusalPending[BC250_WDDM_NODE_COUNT_MAX]; // valid DMA never dispatched; cannot retire in software
    BOOLEAN RejectedPending[BC250_WDDM_NODE_COUNT_MAX];
    UINT RejectedFence[BC250_WDDM_NODE_COUNT_MAX];
    BOOLEAN WatchdogFaulted[BC250_WDDM_NODE_COUNT_MAX]; // sticky until adapter state is rebuilt
    volatile LONG CompletionPending[BC250_WDDM_NODE_COUNT_MAX];    // set by the submit, cleared by the DPC
    // A completion report that did not reach dxgkrnl must not advance LastReportedFence: that value is what a
    // later preemption report hands back as "the fence you were told about", so advancing it there would name a
    // fence dxgkrnl never saw completed. The pending flag goes back instead and the pass runs again, at most
    // BC250_WDDM_REPORT_RETRY_MAX times a node, so a callback that keeps failing cannot spin this DPC for ever.
    volatile LONG CompletionRetries[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG CompletionsDropped[BC250_WDDM_NODE_COUNT_MAX];
    // The preemption ack is not retried: its locked part has already released the preempted paging ownership,
    // and running that release twice is a worse failure than the lost ack, which the watchdog still sees. It is
    // counted and logged, so "the scheduler waits for a preemption that never came" has a line of its own.
    volatile LONG PreemptionReportsLost;
    volatile LONG LastCompletedFence;   // the fence of the newest completion report on EITHER node, for the two log
                                         // lines that print it (summary, stop; tools/runcompare reads the stop line).
                                         // Diagnostics only: fence ids are per node, so this is never a bound for
                                         // one node's fences. "The last completed fence ID" dxgkrnl checks against
                                         // is per node: LastReportedFence[node] (hang-recovery.md, 0.7.216.16)
    UINT NodeCount;                     // 1 with EnablePagingNode closed, 2 open, 3 with node 2 too; WddmStart
    // M15.12 (docs/design/hang-recovery.md): the hang-recovery switch, read once. 0 = refusal
    // (ResetEngine refuses, ResetFromTimeout fails, 0x116). 1 or absent = a node-0 soft
    // recovery (kill the hung job's VMID waves, wait for the fence). Start-latched like EnablePagingNode.
    BOOLEAN HangRecoveryMode;
    BOOLEAN HangRecoveryWaves;          // independent start-latched diagnostic capture; absent = on
    volatile LONG SoftRecoveries;       // ResetEngine soft recoveries that drained the ring (reported as aborted)

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
    KTIMER PagingDrainTimer;            // KMD172: the drain's quota requeue (WddmRequeuePagingDrain)
    KDPC PagingDrainDpc;
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
    // Memory manager stage 1d (0.7.216.8): the halves PagingXferBytes does not carry. Fill bytes (FILL and
    // VIRTUAL_FILL; PagingBytesMoved sums them with the transfers) and aperture pages mapped and unmapped, so
    // two summaries give a session's paging volume without the journal ring and its loss.
    volatile LONG64 PagingFillBytes;
    volatile LONG64 PagingMapPages;
    volatile LONG64 PagingUnmapPages;
    // KMD183: built transfers by kind and direction, count and bytes, indexed by BC250_WDDM_XFER (trial 211 could
    // not tell evictions to system memory from restores without the journal, which wrapped).
    volatile LONG PagingXferCount[BC250WddmXferKinds];
    volatile LONG64 PagingXferBytes[BC250WddmXferKinds];
    volatile LONG PagingInsufficientBuffer;
    volatile LONG PagingUnsupported[BC250PagingNotContiguous + 1]; // indexed by BC250_WDDM_PAGING_UNSUPPORTED
    // BD-235: builds whose status the DDI does not permit (never reported to the OS as success), and page table
    // updates or TLB flushes whose builder failed (each of them goes to the OS as that failure; a failure with a
    // BC250_WDDM_PAGING_UNSUPPORTED reason is also counted in PagingUnsupported).
    volatile LONG PagingContractRefusals;
    volatile LONG PagingContractLastStatus, PagingContractLastOperation;
    volatile LONG PagingInnerFailures;
    // Per-buffer private data owns commands; these counters track node routing only.
    volatile LONG PagingVirtualSubmits[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG PagingVirtualUnmapped;
    volatile LONG64 PagingDmaVaBuilds, PagingDmaZeroVaBuilds;
    volatile LONG PagingDmaVaObserved, PagingDmaVaMatched;
    ULONGLONG PagingDmaLastVa, PagingDmaLastRoot, PagingDmaLastPa, PagingDmaLastCpu;
    BOOLEAN NativePteCopies;
    volatile LONG64 PagingNativePtes;
    volatile LONG64 PagingNativeTransfers, PagingNativeFills, PagingNativeBytes, PagingDmaGapProofs;

    // Stage C: a DMA buffer with bytes in it goes down the gfx ring (gfx.c, one in flight at most) and its fence is
    // reported when the hardware's arrives - from the IH DPC, from the submit itself if the interrupt won the race,
    // or from the watchdog. Software completions that come while one is in flight are held back and published
    // with it: a completion of fence N retires every fence up to N, so N + 1 must not be reported first.
    // All of it under Lock.
    FAST_MUTEX GfxSubmitMutex;
    BC250_GFX_COMPLETION_QUEUE GfxPending;
    BOOLEAN HwPending;
    ULONG HwSeq;                        // gfx.c's sequence number of the submission in flight
    UINT HwFence;
    UINT HwNode;
    BOOLEAN DeferredValid;
    UINT DeferredFence;
    KTIMER SubmitTimer;                 // there is no GPU reset on this part (facts M53): a fence that does not
    KDPC SubmitDpc;                     // arrive is completed in software and the ring is left alone from then on
    // BD-114, latched at WddmStart and read everywhere the watchdog's budget is needed. SubmitBudgetMs is the
    // budget of one head job, SubmitTickMs how often the DPC looks for progress inside it, SubmitWatchdog the
    // staleness window of node 0 (under Lock, like the queue it watches).
    ULONG SubmitBudgetMs;
    ULONG SubmitTickMs;
    BOOLEAN PagingDiagnosticTaken;     // reset for each node-1 hardware head
    BC250_SUBMIT_WATCHDOG SubmitWatchdog;
    volatile LONG SubmitRearms;         // timer rearms, independent of activity observations
    volatile LONG SubmitActivityChanges;
    volatile LONG SubmitPrimes;
    volatile LONG SubmitGapResets;
    volatile LONG SubmitChecks;         // checks the DPC made at all
    volatile LONG SubmitHeadMaxMs;      // sampled software-head age high water, not GPU execution time
    volatile LONG SubmitQueueMaxMs;     // the longest a job waited in the queue before it became the head
    volatile LONG HwSubmitted;
    volatile LONG HwCompleted;
    volatile LONG HwTimeouts;
    volatile LONG HwRefused;
    // WDDM node 2 (docs/design/compute-node.md): the compute ring's channel, node 0's shape, under Lock as well.
    // ComputeTimer polls the fence while a job is at the head; there is no diagnostic watchdog for node 2.
    FAST_MUTEX ComputeSubmitMutex;
    BC250_GFX_COMPLETION_QUEUE ComputePending;
    BOOLEAN ComputeHwPending;
    BOOLEAN ComputeDeferredValid;
    UINT ComputeDeferredFence;
    KTIMER ComputeTimer;
    KDPC ComputeDpc;
    volatile LONG ComputeHwSubmitted;
    volatile LONG ComputeHwCompleted;
    volatile LONG ComputeHwRefused;
    volatile LONG ComputePolls;
    // M8: contract blobs (umd_blob.c). Alloc refusals and submits that did not reach the ring are counted
    // separately from the GDI path, so a desktop present cannot spend the evidence.
    volatile LONG UmdAllocs;
    volatile LONG UmdAllocRefused;
    // Memory manager stage 1c (0.7.216.8): EnableSharedResidency, latched at WddmStart, and the UMD
    // allocations by the segment sets they got (umd_blob.c UmdBlobPlacement): local or aperture alone, or
    // local first with the aperture as VidMm's demotion target.
    BOOLEAN SharedResidency;
    volatile LONG UmdAllocsLocalOnly;
    volatile LONG UmdAllocsShared;
    volatile LONG UmdAllocsAperture;
    // BD-210: the UMD allocations created without a CPU view (UmdBlobAllocCpuVisible 0), and the bytes
    // created on each side. Created, not live: the summary says how much a session asked for of each.
    volatile LONG UmdAllocsGpuOnly;
    volatile LONG64 UmdGpuOnlyBytes;
    volatile LONG64 UmdCpuVisibleBytes;
    // BD-210: cumulative type-0 create-step counters, separate from BC2A and standard GDI types.
    volatile LONG Lb7aAllocs, Lb7aAllocsGpuOnly, Lb7aNoCpuAccess;
    volatile LONG64 Lb7aGpuOnlyBytes, Lb7aCpuVisibleBytes;
    volatile LONG UmdContexts;
    volatile LONG ContextsLogged;       // KMD193: the capped "context %p pid ..." identity line, every kind
    volatile LONG FaultSnapshots;       // KMD193: HARDWARE FENCE TIMEOUT register snapshots taken
    volatile LONG UmdSubmitHw;
    volatile LONG UmdSubmitSoft;
    BOOLEAN TraceUmdProbes;             // diagnostic reads only; no synchronization policy
    volatile LONG UmdProfileCalls;
    volatile LONG UmdProbeCalls;
    volatile LONG64 UmdSubmitTicks;     // QPC elapsed time inside WddmSubmitUmd, including waits
    volatile LONG64 UmdProbeTicks;      // subset spent reading/logging IB and shader contents
    LARGE_INTEGER UmdProfileFrequency;
    // KMD196: the cost of a submission the gfx ring would not take at once, so that one lab session can price
    // the spin-then-event wait against the 1 ms sleep it replaced. Held time is wall clock from the first
    // refusal to the submit that succeeded or to the refusal that gave up - the GFX pipe is idle for part of
    // it, which is what sessions 313/314 measured as 2.9-4.0 ms a frame. The three wake sources are counted
    // apart on purpose: Spins says the bounded spin was enough, Event says a retirement reached a real wait,
    // and a Timeout share that is not small means end-of-pipe interrupts are being missed - a correctness
    // signal, not a slow wake.
    volatile LONG SubmitHolds;          // submissions held at least once (node 0, UMD and GPU Present together)
    volatile LONG64 SubmitHeldUs;       // sum of their held times, microseconds from QPC
    volatile LONG64 SubmitHeldMaxUs;    // the longest single hold
    volatile LONG64 SubmitHoldSpins;    // phase 1 stalls over all holds
    volatile LONG SubmitHoldSpinOnly;   // of SubmitHolds, those the spin resolved without any wait
    volatile LONG64 SubmitHoldEventWakes;
    volatile LONG64 SubmitHoldTimeoutWakes;
    volatile LONG SubmitHeldHistogram[BC250_WDDM_HOLD_BUCKETS];  // held time by bucket, g_WddmHoldBucketNames

    // C48/C49 (ring_gap.h), always on, no gate: how long each node's ring stood idle, and how much of that idle
    // ended at a display VSync. Sessions 418-420 needed a 3 GB xperf dump per session to read this; here it costs
    // two QueryPerformanceCounter reads per packet boundary (about 374 a second at the 187 completions a second
    // session 418 measured) and four lines a node in the summary. Written under Lock, where both edges already
    // are, so the gap of a node is never half updated. The VSync time is adapter-wide (one display, one OTG) and
    // is kept interlocked instead, because the VSync path does not hold Lock. The two stamp counters say which
    // grid it came from: a hardware vblank or the 16 ms software timer, which are different phase grids, so an
    // idle-desktop baseline and a game window must not be read against each other without checking them.
    BC250_RING_GAP RingGap[BC250_WDDM_NODE_COUNT_MAX];
    volatile LONG64 RingGapLastVsyncQpc;
    volatile LONG RingGapVsyncStampsHw;     // vblank times taken from DcnVsyncInterrupt's acknowledged vblank
    volatile LONG RingGapVsyncStampsTimer;  // vblank times taken from the software timer tick
    LARGE_INTEGER RingGapFrequency;     // read once in WddmStart, with the counter
    // C50 (notify_pairing.h): the report and the DPC-level notification dxgkrnl waits for. With NotifyDpcInReport
    // closed the pairing waits for the next dxgkrnl DPC, a 12 to 20 us hop at 187 completions a second (about
    // 0.03 ms of a 70 Hz frame); with it open the report pass makes the call itself, which is what the DDI text
    // asks for, and the otherwise empty dxgkrnl DPC is not asked for at all. The counters price both shapes in the
    // same run. The 8 ms VSync-ended stalls are NOT this (C48 died on its own clause 1): notify_pairing.h says so
    // at the top, next to the evidence.
    BOOLEAN NotifyDpcInReport;
    BC250_NOTIFY_PAIRING NotifyPairing;
    // Two DPCs can reach DxgkCbNotifyDpc once the report pass makes the call itself: ours and dxgkrnl's device
    // DPC, on two processors. Nothing in the DDI text describes that call as re-entrant across processors, so one
    // of them makes it and the other hands its work to DxgkCbQueueDpc - which is exactly the shape the gate-closed
    // driver always had, so a contended notification is never a lost one.
    volatile LONG NotifyDpcBusy;

    KTIMER VSyncTimer;
    KDPC VSyncDpc;
    BOOLEAN VSyncArmed;                 // the timer is running (a source is visible)
    BOOLEAN VSyncEnabled;               // ControlInterrupt turned CRTC_VSYNC on
    D3DDDI_VIDEO_PRESENT_TARGET_ID VSyncTargetId;
    volatile LONG VSyncTicks;           // timer ticks, whether or not anybody was listening
    volatile LONG VSyncReports;         // of those, the ones reported to dxgkrnl as DXGK_INTERRUPT_CRTC_VSYNC
    LARGE_INTEGER VSyncLast;            // the performance counter at the last tick, for GetScanLine's phase
    LARGE_INTEGER VSyncFrequency;

    BOOLEAN PrimaryNeedsRestore; // hardware address may change across retained power loss
    ULONG PrimaryPitch;
    ULONG PrimaryPlaneFormat;           // M15.14: plane_format.h format of the published primary; 0 = none yet
    ULONGLONG PrimaryBytes;
    PHYSICAL_ADDRESS PrimaryAddress;    // last successfully programmed address (retire only after flip pending clears)
    volatile LONG PrimaryProgrammedSequence; // advances only after changed hardware programming succeeds
    volatile LONG PrimarySequence;      // even = published; odd = programming, never spin at DIRQL
    UINT PrimarySegment;

    // E20 (ADR 0011): the diagnostic CPU blit of a Blt present into the firmware framebuffer, behind EnablePresentBlit.
    volatile LONG GdiSurfaceTypesLogged;       // first size/fill request per GDI type, bounded to20 lines
    BC250_STDALLOC_COUNTERS StdAlloc;          // BD-060: standard allocation requests/answers, LB7A create/open
    ULONG OsBuildNumber;                     // BD-135: standard-shadow placement on 24H2 and later
    BOOLEAN BlitGate;
    BOOLEAN HandleIdentityProbe;
    volatile LONG HandleIdentityProbeCalls[2]; // at most16 non-BC2A and16 BC2A opens per start
    BOOLEAN GpuPresentGate; // EnableGpuPresentBlit: producer/consumer gate, no interop cap implied (interop.c)
    BOOLEAN CddDwmInterop; // EnableCddDwmInterop: DRIVERCAPS interop cap; both default on since 0.7.181, start-latched
    volatile LONG64 GpuPresentCalls, GpuPresentRecords, GpuPresentRotates, GpuPresentRefused;
    volatile LONG64 GpuPresentSubmits, GpuPresentSubmitRejected, GpuPresentSubmitFailed;
    volatile LONG64 GpuPresentStatuses[4]; // invalid parameter/handle/color/other failures
    volatile LONG64 GpuPresentSnapshotRefusals[Bc250SnapshotRefusalCount]; // BD-065, by first failing check
    volatile LONG64 PresentObservationCalls;
    BC250_PRESENT_OBSERVATION PresentObservations[BC250_PRESENT_OBSERVATIONS];
    volatile LONG64 DriverCapsInteropReturned[2]; // successful replies, not registry state
    volatile LONG64 DriverCapsFirstTime[2], DriverCapsLastTime[2]; // interrupt time, 100 ns

    volatile LONG Blits;                        // presents copied
    volatile LONG BlitSkips;                    // presents that named no usable source (reason in the log)
    volatile LONG BlitTranslations;             // sources whose complete page range translated contiguously
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
    volatile LONG ImmediatePromotions;          // BD-170: a pending flip reprogrammed for an immediate request
    volatile LONG DmaFlipsRefused;              // BD-170: Present calls for a flip with a DMA buffer, refused
    // Admission refusals remain failures, including independent immediate flips.
    // Counters distinguish immediate and next-VSync exclusive requests.
    volatile LONG ExclusiveRefused;
    volatile LONG ExclusiveFailed;
    // BD-170 r7, the flip state machine (flip_state.h, docs/design/vidpn-flip.md section 7.2). Every field of this
    // block is written inside L, except the atomic WriterRecovery request. L is the interrupt lock of message 0: the flip transaction (WddmFlipLocked), the writer
    // sections (WddmPrimaryExclusiveBegin, WddmWriterResume, WddmPrimaryExclusiveEnd,
    // WddmFlipFlushed), E5 (WddmFlipFault) and WddmNotifyRoutine. The summary and the E5 DPC read them without L.
    // Desired is the published primary above (PrimaryAddress, PrimaryPitch, PrimaryPlaneFormat, PrimaryBytes,
    // PrimarySegment): the newest request this driver answered success for, programmed or deposited.
    BOOLEAN WriterActive;               // a writer is between Begin and End; PrimarySequence is odd for it
    BOOLEAN WriterResumed;              // the active writer called Resume: no DRAINED readback counts any more
    BOOLEAN WriterDeposit;              // software-gate-closed Desired was deposited during the active writer
    volatile LONG WriterBusy;           // hardware flips refused while a long writer owns the pipe
    volatile LONG WriterBusyByKind[BC250_WRITER_KINDS];
    // BD-195: every FALSE of WddmPrimaryExclusiveBegin, by writer kind, and the last one's kind, site
    // (BC250_WRITER_REFUSED_*), answer and status. Written outside L: diagnostics, read without L by the summary.
    volatile LONG WriterRefused;
    volatile LONG WriterRefusedByKind[BC250_WRITER_KINDS];
    ULONG WriterRefusedKind, WriterRefusedSite, WriterRefusedAnswer;
    NTSTATUS WriterRefusedStatus;
    volatile LONG WriterBridgePrograms, WriterBridgeRetired, WriterBridgeFailures;
    BOOLEAN WriterReady;                // bridge retirement proved outside L, then committed under L
    volatile LONG WriterRecovery;       // queued section failure; consumed only inside L
    BOOLEAN DesiredOwed;                // Desired is a next-VSync success that no CRTC_VSYNC report carried yet
    ULONG DepositSequence;              // advances at every deposit, so that a programmed deposit is the newest one
    volatile LONG Faulting;             // E5 was entered on this start (F0); this block is freed at the stop
    ULONG FaultCode;                    // the first fault's BC250_FLIP_FAULT_* code
    UINT FaultNode;                     // and its node ordinal
    NTSTATUS FaultBlank;                // DcnFaultBlank's status
    volatile LONG FaultNotify;          // 1: the E5 DPC owes the DMA_PAGE_FAULTED notification
    volatile LONG FaultDrain;           // the E5 DPC's drain record, BC250_FLIP_DRAIN_*
    volatile LONG FaultDrainReads;      // and its reads of HUBP0_DCHUBP_CNTL
    // Counts. Edges: each request by its edge (BC250_FLIP_EDGE_*). Results: the requests edge E programmed, by
    // Bc250FlipResult. Faults: E5 entries, a repeat in FAULTING included. Notified and NotifyFailed: the
    // DMA_PAGE_FAULTED notifications made and those that could not be made. WriterFaulting: Begin refused in
    // FAULTING. WriterHigh: a writer section refused above DISPATCH_LEVEL.
    // WriterSyncFailed and LowSyncFailed: DxgkCbSynchronizeExecution failed for a writer section or for the flip
    // DDI's low entry. Deposits by Resume, End's programs, End's refusals (OwedInvalidated), the reports that carried
    // an owed Desired, the D3 flushes and the destroy restores whose Begin was refused.
    volatile LONG FlipEdges[BC250_FLIP_EDGES];
    volatile LONG FlipResults[BC250_FLIP_RESULTS];
    volatile LONG FlipFaults;
    volatile LONG FaultNotified;
    volatile LONG FaultNotifyFailed;
    volatile LONG WriterFaulting;
    volatile LONG WriterHigh;
    volatile LONG WriterSyncFailed;
    volatile LONG LowSyncFailed;
    volatile LONG DepositsResumed;
    volatile LONG EndPrograms;
    volatile LONG OwedInvalidated;
    volatile LONG OwedReported;
    volatile LONG FlipFlushes;
    volatile LONG DestroyUnsynchronized;
    volatile LONG FlipLockedMaxUs;      // the longest flip transaction inside L, programming and E5 included
    // W-u: the active writer's BC250_WRITER_* (Begin), the deposits' records and the longest time from a W-u deposit
    // to its writer's End. FlipEdges[BC250_FLIP_EDGE_DEPOSIT_UNPROVEN] counts them.
    ULONG WriterKind;
    volatile LONG WuRecordNext;
    volatile LONG WuToEndMaxUs;
    BC250_WDDM_WU_RECORD WuRecords[BC250_WDDM_WU_RECORDS];
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    // BD-170 r6, lab flavour only (WddmWitness): per immediate request that edge E programmed, the disagreements of
    // the shipping decision Q with the witnesses E and R read after it. ReadFailed: the witness reads failed. Stale: the
    // event clear before the programming failed, so E is not compared. Qe, Qr, Er: Q against E, Q against R (low word
    // first), E against R. Order: R low word first against R high word first. The first BC250_WDDM_WITNESS_RECORDS
    // requests with any of these are kept, later ones counted (WitnessLost); WitnessLast is the last request whatever
    // it showed.
    volatile LONG WitnessRequests, WitnessReadFailed, WitnessStale, WitnessQe, WitnessQr, WitnessEr, WitnessOrder;
    volatile LONG WitnessLost, WitnessKept;
    BC250_WDDM_WITNESS_RECORD WitnessRecords[BC250_WDDM_WITNESS_RECORDS];
    BC250_WDDM_WITNESS_RECORD WitnessLast;
#endif
    // BD-170: the CRTC_VSYNC switch of ControlInterrupt. Calls, On and Off count the calls. The last
    // BC250_WDDM_VSYNC_CONTROL_EVENTS calls are kept, with their time and the address owed then.
    volatile LONG VSyncControlCalls;
    volatile LONG VSyncControlOn;
    volatile LONG VSyncControlOff;
    BC250_WDDM_VSYNC_CONTROL VSyncControl[BC250_WDDM_VSYNC_CONTROL_EVENTS];
    // M15.14. ScanoutFlips is the measurement the lab trial reads: of the flips above, those whose
    // allocation was an application's own scan-out surface rather than dxgkrnl's shared primary.
    // ScanoutAdmits counts every candidate by Bc250ScanoutAdmit's answer, so a trial that sees no
    // scan-out flip says which clause refused it instead of only that nothing happened.
    // ScanoutFlips counts only the flips the display hardware was actually programmed with, so a run
    // with the flip gate closed (EnableVidPnFlip, mmio.c) reports zero rather than one per present.
    volatile LONG ScanoutFlips;
    volatile LONG ScanoutRequests;              // candidates whose creator had asked for scan-out
    volatile LONG ScanoutAdmits[BC250_SCANOUT_STATUSES];
    volatile LONG ScanoutNotes;                 // guard-log budget of the scan-out flip lines, its own
    volatile LONG ScanoutTeardowns;             // and of the teardown lines, which a flip must not crowd out
    // M15.14 increment 2. ScanoutRequests is a SetVidPnSourceAddress-time counter, so a zero there cannot
    // tell "user mode never marked its buffers" from "the request never reached this DDI". This is the
    // create-time answer: every type-0 allocation this driver placed, by the resource record it arrived
    // with and by whether that record asked for scan-out ([...][0] asked, [...][1] did not).
    //   The record kind is the first index because the one input of the handshake that no document
    // establishes is what record the compositor's own output primaries carry, and a standard primary
    // (D3DKMDT_STANDARDALLOCATION_PRIMARY) carries none at all: it reaches CreateAllocation as a 32-byte
    // LB7A blob with no resource private data, so a counter kept only for records with the PRIMARY bit
    // would be silent for exactly the surface DWM flips today. BC250_WDDM_RECORD_NONE is therefore a
    // bucket of its own, and ScanoutCreatePrimaries says how many of all of them did carry PRIMARY.
    volatile LONG ScanoutCreates[BC250_WDDM_RECORD_KINDS][2];
    volatile LONG ScanoutCreatePrimaries;       // of those, the records that carried the PRIMARY bit
    volatile LONG ScanoutCreateResources;       // and those created as part of a resource group (hResource)
    volatile LONG ScanoutCreateNotes;           // the create-time lines' own guard-log budget
    // DXGK_SETVIDPNSOURCEADDRESS_FLAGS of every address call this DDI accepted, per bit:
    // [0] ModeChange, [1] FlipImmediate, [2] SharedPrimaryTransition, [3] IndependentFlipExclusive.
    // The last two are the kernel-side witness that the OS really entered DirectFlip and then independent
    // flip; they are counted for every accepted call, admitted or refused, because the flags describe the
    // video present source's mode and not the allocation, and because a refusal that arrives after the OS
    // has taken a SharedPrimaryTransition is the one shape that blanks the screen.
    //   Counted by reading the bitfields, never a transcribed mask: the trailing comments of
    // DXGK_SETVIDPNSOURCEADDRESS_FLAGS in d3dkmddi.h give 0x00000010 for both FlipStereoTemporaryMono and
    // FlipStereoPreferRight and are shifted by one bit from there on (Reserved:23 after nine named bits
    // fixes the real layout at SharedPrimaryTransition 0x40 and IndependentFlipExclusive 0x80), so a
    // number copied out of those comments would make a run that did enter independent flip report that it
    // never did. The raw Flags.Value goes into the scan-out flip line for the same reason.
    // They are observed and never obeyed: refusing a flip for a flag is the hazard, because the OS does
    // not fall back to composition seamlessly after a SharedPrimaryTransition.
    volatile LONG ScanoutFlipFlags[4];
    // M15.14 (0.7.216.20): the scan-out flips the hardware was written with, by the plane format of the surface
    // (plane_format.h order: [0] unused, argb8888, abgr8888, abgr2101010). A run that scans out a game's
    // R8G8B8A8 chain shows it here, apart from the compositor's B8G8R8A8 flips.
    volatile LONG ScanoutFlipsByFormat[BC250_PLANE_FORMATS];
    volatile LONG RedirectedPresents;           // DxgkDdiPresent calls carrying Flags.RedirectedFlip
    // The operator's switch for the handshake, read once at WddmStart (EnableDirectFlipHandshake, absent
    // = on from 0.7.213, and 0 is its bisect switch) and ANDed with ScanoutAdmitGate. It is published to the compositor's user-mode driver in the
    // bc250_scanout_caps trailer and nowhere else: the kernel driver's own admission does not read it, so
    // a closed switch can never leave the shell agreeing to a flip this driver would refuse.
    BOOLEAN DirectFlipHandshake;
    // The operator's switch for M15.14, read once at WddmStart (EnableScanoutAdmit, absent = on). Closed, a
    // candidate that asks for scan-out is refused with BC250_SCANOUT_GATED and every other candidate keeps the
    // four checks it had in 0.7.205.1, so this start behaves as that revision did. It exists because the b18
    // train carries three changes at once and a lab failure must be attributable to one of them without a
    // rebuild (scratch/train/TRAIN-b18.md rule 4).
    BOOLEAN ScanoutAdmitGate;
    // The application allocation the plane is reading, as its BC250_WDDM_OBJECT::Serial and never as its
    // address: DestroyAllocation compares the serial of the allocation it is about to free against it and
    // restores the firmware surface on a match, because nothing else ties an application swap-chain
    // buffer's lifetime to the video present source.
    //   The serial and not the pointer, because this value outlives the object it names in one path: a
    // programming sequence that failed puts the previous flip's value back (see
    // Bc250WddmSetVidPnSourceAddress), and a destroy may already have freed that object. A pointer would
    // then be a stale address that the next object allocated at the same address would match, which would
    // take the plane back to the firmware surface for a buffer nobody destroyed. A serial is
    // adapter-unique, nonzero and never reused, so the compare can only ever match the object it means.
    volatile LONG64 ScanoutObject;

    // HLK gap G1, kernel-mode GDI hardware acceleration (docs/design/gdi-hw-accel.md). The gate is
    // EnableGdiHwAccel, read once at WddmStart; GdiHwAccel is that gate AND a translator that can
    // perform every operation the OS may then send, which is what the capability bit promises.
    // DxgkDdiRenderGdi cannot report anything (its return table bugchecks on any status but
    // STATUS_SUCCESS), so these counters are the only account of what it did:
    //   Calls/Commands/Items/Dwords  work done: calls, commands walked, sub-rectangles emitted, dwords
    //   Refused[]                    by bc250_gdi_refusal, the admission's own classes
    //   RefusedCall                  a call whose arguments were not translatable at all
    //   Malformed                    a CommandSize the command buffer does not hold
    //   Outside                      a sub-rectangle array outside its own command
    //   Empty                        a command with no sub-rectangle, which paints nothing
    //   Overflow                     a command buffer that did not fit one DMA buffer: dropped work
    //   KernelWanted/Kernels[]       operations that need increment 2's compute kernels
    //   ColorFillRedFirst            the one case where the header and the page disagree about Color
    BOOLEAN GdiHwAccelGate;
    BOOLEAN GdiHwAccel;
    volatile LONG64 GdiRenderCalls, GdiRenderCommands, GdiRenderItems, GdiRenderDwords;
    volatile LONG64 GdiRenderRefused[Bc250GdiRefusals];
    volatile LONG64 GdiRenderKernels[Bc250GdiKernels];
    volatile LONG64 GdiRenderRefusedCall, GdiRenderMalformed, GdiRenderOutside, GdiRenderEmpty;
    volatile LONG64 GdiRenderOverflow, GdiRenderItemRefused, GdiRenderColorFillRedFirst;
    // HLK gap G1: the batch capacity contract of gdi_batch.h. BatchRefused counts the calls whose
    // sub-rectangles could not all be guaranteed and that were therefore translated to an empty DMA
    // buffer; Fallback counts the items that gave up the per-row engine path for a dispatch. The two
    // peaks are statistics and are updated without a lock: a lost sample costs a smaller maximum,
    // which is why no decision reads them.
    volatile LONG64 GdiRenderBatchRefused, GdiRenderFallback;
    volatile LONG64 GdiRenderPeakSubRects, GdiRenderPeakDwords;
    // CapacityShort counts the calls whose command buffer was larger than the bound this adapter's
    // DMA buffers were sized for - the only way Begin's refusal can be reached at all. PeakCommandLength
    // is the largest CommandLength seen, which is what turns that bound into a measured number.
    volatile LONG64 GdiRenderCapacityShort, GdiRenderPeakCommandLength;
    // The sub-rectangles this adapter has dropped because a whole call could not be guaranteed.
    // Nonzero means pixels stayed stale, and it is the number the design page's limitation section
    // is read against.
    volatile LONG64 GdiRenderOmittedSubRects;
    // Raised past the bound by such a call, and read by the next CreateContext, so a context created
    // afterwards asks for a buffer that fits it. It cannot repair the call it happened on and it
    // cannot resize a context that already exists. Advisory: nothing refuses a call on it.
    volatile LONG GdiCmdBytesWanted;
    // Rule 3 of gdi_batch.h, and the self-checks of its scope. IbFull counts the calls that found
    // the DMA buffer already filled to the length one indirect buffer can carry, so they had no
    // room at all; IbOverflow counts the ones whose finished stream would have passed that length,
    // which the capacity makes unreachable and which is therefore the counter that says the bound
    // was computed wrongly; DmaShort counts the calls that were given the start of a buffer and
    // reported less space than this context asked dxgkrnl for, which DXGK_CONTEXTINFO says cannot
    // happen. AboveProducer counts the calls longer than the command buffer of the ordinary
    // producer, which is the input scope the design read, and ScopeRefused the drops inside that
    // scope - nonzero means an ordinary paint was dropped, whatever the cause, and it is not a
    // completeness witness while it is zero (docs/design/gdi-hw-accel.md, "What this capability
    // still does not complete").
    //   Every drop of a whole call goes through WddmGdiDropCall, so BatchRefused and
    // OmittedSubRects cover all of them: the five below are which of them it was.
    volatile LONG64 GdiRenderIbFull, GdiRenderIbOverflow, GdiRenderDmaShort;
    volatile LONG64 GdiRenderAboveProducer, GdiRenderScopeRefused;
    // Where the dwords ahead of a call came from (Bc250GdiDmaClaim). DmaAppend counts the calls that
    // continued a buffer an earlier call had written into, DmaRestart the buffers offered again from
    // their start, DmaUnknown the windows this driver could not place and DmaFull the calls that
    // found every slot holding unsubmitted output. The last two drop the call. DmaLost counts a
    // record that could not be kept after a successful translation, which the claim makes
    // unreachable; a buffer in that state can then only be refused. DmaSubmitted counts the
    // submissions that released a buffer of this context, and DwordsCommitted the dwords actually
    // handed to the submitter, which is what GdiRenderDwords is read against: that one counts what
    // the batch generated, this one what left.
    volatile LONG64 GdiRenderDmaAppend, GdiRenderDmaRestart, GdiRenderDmaUnknown;
    volatile LONG64 GdiRenderDmaFull, GdiRenderDmaLost, GdiRenderDmaSubmitted;
    volatile LONG64 GdiRenderDwordsCommitted;
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

// The table selected at DriverEntry is immutable for this driver load. PnP
// must inspect that selection without consuming the diagnostic gate again.
BOOLEAN WddmFullTableSelected(void)
{
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
// never tears down is freed by the stop instead of leaked for the life of the boot. It is in the adapter's object
// index for exactly as long, which is what membership tests consult instead of walking the list.
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
    // stop had drained it would never be freed. The list and the index change together, and the serial is given
    // before either publishes the object.
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->Stopping)
    {
        KeReleaseSpinLock(&wddm->Lock, irql);
        ExFreePoolWithTag(object, BC250_WDDM_TAG);
        return NULL;
    }
    if(Captures)RtlZeroMemory(Captures,sizeof(*Captures));
    object->Serial = ++wddm->ObjectSerial;
    InsertTailList(&wddm->Objects, &object->Link);
    Bc250ObjectIndexInsert(&wddm->ObjectIndex, object);
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
// adapter's live objects in the index under the lock, and only a match is dereferenced. <= DISPATCH_LEVEL.
static BC250_WDDM_OBJECT* WddmIndexedObject(_In_ BC250_WDDM* Wddm, _In_opt_ const HANDLE Handle, ULONG Magic)
{
    BC250_WDDM_OBJECT* found;
    KIRQL irql;

    if (Handle == NULL) return NULL;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    found = (BC250_WDDM_OBJECT*)Bc250ObjectIndexFind(&Wddm->ObjectIndex, Handle);
    if (found != NULL && found->Magic != Magic) found = NULL;
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
    HipJournalOwnerDestroy(&Object->HipJournalOwner);
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
        // Leaving the index ends every opened object's binding to this allocation: WddmBackingAllocationLocked
        // finds nothing at this address any more, or, once the pool reuses it, an object with another serial.
        // That replaces the scan that cleared each BackingAllocation here before KMD 0.7.192.
        (void)Bc250ObjectIndexRemove(&wddm->ObjectIndex, Object);
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
    BC250_WDDM* wddm = (BC250_WDDM*)notify->Device->Wddm;

    notify->Device->Dxgk.DxgkCbNotifyInterrupt(notify->Device->Dxgk.DeviceHandle, &notify->Data);
    if (notify->Data.InterruptType == DXGK_INTERRUPT_CRTC_VSYNC)
    {
        InterlockedExchange64(&notify->Device->DcnVsyncNotifyTime, (LONG64)KeQueryInterruptTime());
        // BD-170 r6: inside L, like every write of DesiredOwed. The report that carries Desired completes the
        // next-VSync success the OS waits for, so a writer's End no longer owes it (WddmWriterEndLocked).
        if (wddm != NULL && wddm->DesiredOwed &&
            notify->Data.CrtcVsync.PhysicalAddress.QuadPart == wddm->PrimaryAddress.QuadPart)
        {
            wddm->DesiredOwed = FALSE;
            InterlockedIncrement(&wddm->OwedReported);
        }
    }
    return TRUE;
}

// Stage A runs nothing on the GPU, so every packet is finished before this returns. Raise to interrupt level,
// report there, then queue the DPC the contract requires ("after the driver calls DXGKCB_NOTIFY_INTERRUPT but
// before the driver exits its ISR, the driver must queue a DPC"); Bc250DpcRoutine calls DxgkCbNotifyDpc.
//
// C50: PairInThisPass says the caller is itself a DPC and will call DxgkCbNotifyDpc before it returns, so the
// DxgkCbQueueDpc whose only job is to bring that call about is not made - without this the gate would add a
// notification per completion instead of moving one earlier. FALSE is the shape of every revision up to 0.7.208.1.
//
// Returns TRUE only when the report actually reached dxgkrnl. FALSE means the callback table is incomplete or
// DxgkCbSynchronizeExecution failed; the caller must then not count a report and must not notify for nothing.
static BOOLEAN WddmReport(_Inout_ BC250_DEVICE* Device, _In_ const DXGKARGCB_NOTIFY_INTERRUPT_DATA* Data,
                          BOOLEAN PairInThisPass)
{
    BC250_WDDM_NOTIFY notify;
    BOOLEAN returned = FALSE;
    NTSTATUS status;

    if (Device->Dxgk.DxgkCbSynchronizeExecution == NULL || Device->Dxgk.DxgkCbNotifyInterrupt == NULL ||
        Device->Dxgk.DxgkCbQueueDpc == NULL)
        return FALSE;                   // all three are needed: the report is worthless without the DPC that pairs with it
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
        return FALSE;
    }
    if (!PairInThisPass) Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
    return TRUE;
}

// The pairing counters, under Lock. They are read-modify-write on plain counters and two DPCs on two processors
// reach them (ours and dxgkrnl's device DPC), so a lost update would make the summary's "unpaired" line - which
// says a number above 1 is a correctness question - fire on nothing, or hide a real one. The callbacks themselves
// stay outside the lock; only the arithmetic is inside it.
static void WddmPairingReport(_Inout_ BC250_WDDM* Wddm, BOOLEAN Vsync)
{
    KIRQL irql;

    KeAcquireSpinLock(&Wddm->Lock, &irql);
    if (Vsync) Bc250NotifyPairingVsyncReport(&Wddm->NotifyPairing);
    else Bc250NotifyPairingReport(&Wddm->NotifyPairing);
    KeReleaseSpinLock(&Wddm->Lock, irql);
}

// The one place that calls DxgkCbNotifyDpc, which is where dxgkrnl's scheduler looks at what was reported.
// SamePass says whether the reports this call carries were made in this same DPC: that is true of the report
// pass with NotifyDpcInReport open, and false of the completion path with it closed, where the pairing waits for
// the next dxgkrnl DPC (notify_pairing.h has the measured shape and the DDI text behind it).
//
// Single flight. Until 0.7.208.1 this call had exactly one caller, dxgkrnl's own device DPC, which cannot run
// concurrently with itself for one adapter. The report pass is a second KDPC and can run on another processor, and
// the DDI text ("the display miniport driver's DPC callback routine calls DXGKCB_NOTIFY_DPC") never describes the
// call as re-entrant across processors. So the second caller does not wait and does not skip: it asks dxgkrnl for
// its own DPC instead, which is the gate-closed shape, and the pairing arrives one hop later rather than never.
// No lock is held across the callback - a spinlock held into dxgkrnl would invite a lock-order inversion with
// whatever the device DPC path holds.
static void WddmNotifyDpcNow(_Inout_ BC250_DEVICE* Device, BOOLEAN SamePass)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    KIRQL irql;

    if (Device->Dxgk.DxgkCbNotifyDpc == NULL) return;
    if (wddm == NULL) { Device->Dxgk.DxgkCbNotifyDpc(Device->Dxgk.DeviceHandle); return; }
    if (InterlockedCompareExchange(&wddm->NotifyDpcBusy, 1, 0) != 0)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        Bc250NotifyPairingContend(&wddm->NotifyPairing);
        KeReleaseSpinLock(&wddm->Lock, irql);
        if (Device->Dxgk.DxgkCbQueueDpc != NULL) Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
        return;
    }
    KeAcquireSpinLock(&wddm->Lock, &irql);
    (void)Bc250NotifyPairingNotifyDpc(&wddm->NotifyPairing, SamePass ? 1 : 0);
    KeReleaseSpinLock(&wddm->Lock, irql);
    Device->Dxgk.DxgkCbNotifyDpc(Device->Dxgk.DeviceHandle);
    InterlockedExchange(&wddm->NotifyDpcBusy, 0);
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
    Bc250HangObserveCompleted(&Wddm->Recovery[NodeOrdinal], FenceId);
    Wddm->SubmittedNode[NodeOrdinal] = (LONG)NodeOrdinal;
    Wddm->SubmittedFence[NodeOrdinal] = (LONG)FenceId;
    Wddm->CompletionPending[NodeOrdinal] = 1;
}

// Caller holds Lock. Is a job of this IB node (0 or 2) on its ring?
static BOOLEAN WddmRingPendingLocked(_In_ const BC250_WDDM* Wddm, UINT Node)
{
    return Node == BC250_WDDM_NODE_COMPUTE ? Wddm->ComputeHwPending : Wddm->HwPending;
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
    busy = NodeOrdinal == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : WddmRingPendingLocked(wddm, NodeOrdinal);
    active = wddm->ActiveSubmissions[NodeOrdinal];
    KeReleaseSpinLock(&wddm->Lock, irql);
    GuardLog("wddm: preemption queued fence %u node %u hardware pending %u active submits %ld",
             FenceId, NodeOrdinal, busy, active);
    WddmQueueReport(wddm);
}

// ---- stage C: the hardware path --------------------------------------------------------------------------------

// KMD214: no fixed VMID any more. WDDM submits at BC250_VMID_AUTO (vmid_pool.h) and gfx.c chooses: VMID 1 with
// EnableVmidPool 0, else a VMID per root from the pool. The VMID a job ran at comes back from GfxSubmitIb.
// BD-152: elapsed time only authorizes diagnostics; OS TDR owns recovery.
// Two bounds that are NOT the watchdog's budget and keep the old number on purpose:
//   - BC250_WDDM_HOLD_DEADLINE_MS bounds a CPU wait inside SubmitCommandVirtual, on a dxgkrnl worker thread.
//     Letting it grow to a ten-second budget would block that thread for ten seconds, which is a different
//     hazard from the one this change removes. A held submission that runs out still closes the node, so this
//     is a remaining instance of the same class, recorded in docs/design/hang-recovery.md.
//   - BC250_WDDM_STOP_DRAIN_MS bounds the drain of WddmStop. A device stop must not wait out a long budget.
#define BC250_WDDM_HOLD_DEADLINE_MS 500
#define BC250_WDDM_STOP_DRAIN_MS 600

// A completion that did not come from the hardware. While a hardware submission is in flight ON THAT NODE it
// waits for it: the two nodes run on different rings, with different fences and different watchdogs, and node
// 1's completion has no business waiting behind node 0's packet or being published under node 0's ordinal (the
// PagingDeferredValid field's own comment).
static BOOLEAN WddmSubmitPagingHardwareRoot(BC250_DEVICE* Device, BC250_WDDM* Wddm,
    const void* PrivateData, ULONG PrivateBytes, ULONGLONG Start,
    ULONG ByteCount, BOOLEAN VirtualAddress, UINT FenceId, ULONGLONG Root);
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
    deferred = paging ? (wddm->PagingHead != NULL) : WddmRingPendingLocked(wddm, NodeOrdinal);
    if (deferred && paging) { wddm->PagingDeferredValid = TRUE; wddm->PagingDeferredFence = FenceId; }
    else if (deferred && NodeOrdinal == BC250_WDDM_NODE_COMPUTE)
    { wddm->ComputeDeferredValid = TRUE; wddm->ComputeDeferredFence = FenceId; }
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
    else if (Node==BC250_WDDM_NODE_COMPUTE) wddm->ComputeDeferredValid=FALSE;
    else wddm->DeferredValid=FALSE;
    if (Node==BC250_WDDM_NODE_COPY) GfxPagingSubmitFail(Device);
    else if (Node==BC250_WDDM_NODE_COMPUTE) GfxComputeClose(Device);     // node 2 does not witness the start
    else GfxSubmitFail(Device);
    KeReleaseSpinLock(&wddm->Lock,irql);
    // KMD196: node 0's wake is inside GfxSubmitFail; node 1 does not come here on the held path, but the flag
    // this function just set (WatchdogFaulted) closes node 0's submissions too, so waiters are woken either way.
    if (Node==BC250_WDDM_NODE_COPY) GfxRetireSignal(Device);
    if (first) GuardLog("wddm: fence %u node %u NOT dispatched; node closed, no completion, recovery required",FenceId,Node);
}

// BD-167 (G3): close the gfx node after a CP privilege/decode fault. Called from the IH DPC (ih.c) at
// DISPATCH_LEVEL with no submission fence of its own. It marks the node faulted and wakes its waiters the same
// way WddmFailSubmission does, so QueryEngineStatus reports the node unresponsive and dxgkrnl runs recovery and
// removes the faulting context. It does not manufacture a DXGK_INTERRUPT_DMA_FAULTED (see the note above).
// Compute is the vector's ring id decoded (ih_fault.h): a fault of a MEC queue closes node 2 when node 2 exists,
// and node 0 otherwise, because node 0 is then the only node that runs IBs.
void WddmReportEngineFault(_In_ const BC250_DEVICE* Device, _In_z_ const char* Why, BOOLEAN Compute)
{
    BC250_DEVICE* device=(BC250_DEVICE*)Device;   // the IH DPC holds a const device; the node close mutates wddm state
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    UINT node;
    KIRQL irql;
    BOOLEAN first;
    if (wddm==NULL) return;
    node=(Compute && wddm->NodeCount>BC250_WDDM_NODE_COMPUTE) ? BC250_WDDM_NODE_COMPUTE : BC250_WDDM_NODE_3D;
    KeAcquireSpinLock(&wddm->Lock,&irql);
    first=!wddm->WatchdogFaulted[node];
    wddm->RefusalPending[node]=TRUE;
    wddm->WatchdogFaulted[node]=TRUE;
    if (node==BC250_WDDM_NODE_COMPUTE) { wddm->ComputeDeferredValid=FALSE; GfxComputeClose(device); }
    else { wddm->DeferredValid=FALSE; GfxSubmitFail(device); }
    KeReleaseSpinLock(&wddm->Lock,irql);
    if (first) GuardLog("wddm: gfx engine fault (%s); node %u closed, QueryEngineStatus unresponsive, recovery required",
                        Why, node);
}

// KMD193 (bsod-245 item 2): what the CP and the GCVM fault latch held when the 500 ms watchdog gave up. In 245
// the ring, the IB1 address and the fault page all came out of the dump afterwards; none of it was in the live
// log, and there is no dump when the machine survives the TDR. Read-only, at DISPATCH_LEVEL in the watchdog
// DPC, outside wddm->Lock, once per timeout.
//
// Two honest limits of these reads, stated here so that nobody reads the line as more than it is:
//   - GRBM_STATUS_SE0 and the CP_IB*/CP_STAT family are banked by GRBM_GFX_INDEX. Reader admission excludes
//     a wave capture's bank selection; these reads use the bank the shim left behind.
//   - CP_IB1/CP_IB2 are the command processor's live fetch registers, so they describe where the CP is now,
//     not necessarily the timed-out job; the journal's BC250_PJ_GFX_SUBMIT record is what names the job.
static void WddmTimeoutSnapshot(_Inout_ BC250_DEVICE* Device, ULONG Seq, UINT Fence, UINT Node)
{
    static const struct { const char* Name; ULONG Offset; } registers[] = {
        { "CP_RB0_RPTR", BC250_REG_GC_CP_RB0_RPTR }, { "CP_RB0_WPTR", BC250_REG_GC_CP_RB0_WPTR },
        { "CP_IB1_BASE_LO", BC250_REG_GC_CP_IB1_BASE_LO }, { "CP_IB1_BASE_HI", BC250_REG_GC_CP_IB1_BASE_HI },
        { "CP_IB1_BUFSZ", BC250_REG_GC_CP_IB1_BUFSZ },
        { "CP_IB2_BASE_LO", BC250_REG_GC_CP_IB2_BASE_LO }, { "CP_IB2_BASE_HI", BC250_REG_GC_CP_IB2_BASE_HI },
        { "CP_IB2_BUFSZ", BC250_REG_GC_CP_IB2_BUFSZ },
        { "CP_STAT", BC250_REG_GC_CP_STAT }, { "CP_BUSY_STAT", BC250_REG_GC_CP_BUSY_STAT },
        { "CP_STALLED_STAT1", BC250_REG_GC_CP_STALLED_STAT1 },
        { "CP_STALLED_STAT2", BC250_REG_GC_CP_STALLED_STAT2 },
        { "CP_STALLED_STAT3", BC250_REG_GC_CP_STALLED_STAT3 },
        { "CP_CPF_STATUS", BC250_REG_GC_CP_CPF_STATUS }, { "CP_ME_CNTL", BC250_REG_GC_CP_ME_CNTL },
        { "GRBM_STATUS", BC250_REG_GC_GRBM_STATUS }, { "GRBM_STATUS2", BC250_REG_GC_GRBM_STATUS2 },
        { "GRBM_STATUS_SE0", BC250_REG_GC_GRBM_STATUS_SE0 },
        { "GCVM_FAULT_STATUS", BC250_REG_GC_GCVM_L2_PROTECTION_FAULT_STATUS },
        { "GCVM_FAULT_ADDR_LO32", BC250_REG_GC_GCVM_L2_PROTECTION_FAULT_ADDR_LO32 },
        { "GCVM_FAULT_ADDR_HI32", BC250_REG_GC_GCVM_L2_PROTECTION_FAULT_ADDR_HI32 },
    };
    // 21 registers, three to a line: seven lines in the ring per timeout, and a timeout already means the
    // device is finished for this start.
    ULONG values[21];
    ULONG i, refused = 0, status;
    ULONGLONG page;

    C_ASSERT(RTL_NUMBER_OF(registers) == RTL_NUMBER_OF(values));
    C_ASSERT(RTL_NUMBER_OF(values) % 3 == 0);
    if (!GfxWaveSampleBegin(Device)) {
        GuardLog("wddm: timeout seq %lu register snapshot skipped during wave capture", Seq);
        return;
    }
    for (i = 0; i < RTL_NUMBER_OF(values); i++)
        if (!NT_SUCCESS(MmioRead(Device, registers[i].Offset, &values[i]))) { values[i] = 0; refused++; }
    GfxWaveSampleEnd(Device);
    for (i = 0; i < RTL_NUMBER_OF(values); i += 3)
        GuardLog("wddm: timeout seq %lu fence %u node %u %s 0x%08X %s 0x%08X %s 0x%08X", Seq, Fence, Node,
                 registers[i].Name, values[i], registers[i + 1].Name, values[i + 1],
                 registers[i + 2].Name, values[i + 2]);
    // The latch of the first fault of a burst, decoded. ADDR_LO32/HI32 hold the page frame, not the byte
    // address (amdgpu gmc_v10_0 prints "page starting at"), so the page is the pair shifted by 12.
    status = values[18];
    page = (((ULONGLONG)values[20] << 32) | values[19]) << 12;
    GuardLog("wddm: timeout seq %lu fault cid %lu %s vmid %lu rw %lu perm 0x%lX walker 0x%lX more %lu "
             "mapping %lu page 0x%llX, %lu register(s) refused", Seq, BC250_GCVM_FAULT_CID(status),
             Bc250GfxhubClientName(BC250_GCVM_FAULT_CID(status)), BC250_GCVM_FAULT_VMID(status),
             BC250_GCVM_FAULT_RW(status), BC250_GCVM_FAULT_PERMISSIONS(status),
             BC250_GCVM_FAULT_WALKER_ERROR(status), BC250_GCVM_FAULT_MORE(status),
             BC250_GCVM_FAULT_MAPPING(status), page, refused);
    // KMD214: the latch names a VMID; with the pool that is not always the timed-out job's. Who held it.
    if (status != 0) GfxVmidReport(Device, "wddm: timeout latch", BC250_GCVM_FAULT_VMID(status));
}

// BD-114 (ANALYSIS.md 7.2): the progress token of node 0, one 64-bit value out of the things that move while a
// packet is healthy. Read at DISPATCH_LEVEL in the watchdog DPC, outside wddm->Lock, once per check.
//
// Two sources, and they answer different questions:
//   - the submission fence slot the CP writes. It moves when a packet retires, which is completion progress and
//     the only one of the two whose meaning is beyond doubt.
//   - the command processor's live fetch registers: the ring read pointer and the IB1/IB2 base and size pairs.
//     They move while the CP walks the ring and the indirect buffers. A single long packet alone on the ring -
//     the g12 arm of the two dumps, where SubmitSeq equalled the timed-out head's sequence - retires no fence
//     for hundreds of milliseconds, so the fence slot alone would say nothing about it.
// The honest limits of the second source are the ones WddmTimeoutSnapshot states above: the CP_IB* family is
// banked by GRBM_GFX_INDEX, so the sampler excludes wave capture, and those registers describe where the CP is now
// rather than the head job. Token changes reset our inactivity window without
// proving head progress; A/B/A/B activity can postpone this watchdog indefinitely.
// A healthy long shader can also keep a constant token. Windows' independent
// preemption timeout remains responsible for recovery; no ordering is guaranteed.
//
// A register the path refuses contributes its zero, and a mix collision is read as no progress, which is the
// behaviour of every build before this one.
static BOOLEAN WddmSubmitProgress(_Inout_ BC250_DEVICE* Device, _Out_ ULONGLONG* Token)
{
    static const ULONG registers[] = {
        BC250_REG_GC_CP_RB0_RPTR,
        BC250_REG_GC_CP_IB1_BASE_LO, BC250_REG_GC_CP_IB1_BUFSZ,
        BC250_REG_GC_CP_IB2_BASE_LO, BC250_REG_GC_CP_IB2_BUFSZ,
    };
    ULONGLONG token = BC250_SUBMIT_PROGRESS_SEED;
    ULONG value, i;

    // No waiting in the DPC. Leave the previous watchdog observation unchanged
    // if the capture owns the bank; its cleanup restores the bank before release.
    if (!GfxWaveSampleBegin(Device)) return FALSE;
    if (GfxFenceObserved(Device, &value)) token = Bc250SubmitProgressMix(token, value);
    for (i = 0; i < RTL_NUMBER_OF(registers); i++)
    {
        if (!NT_SUCCESS(MmioRead(Device, registers[i], &value))) value = 0;
        token = Bc250SubmitProgressMix(token, value);
    }
    GfxWaveSampleEnd(Device);
    *Token = token;
    return TRUE;
}

// ---- C48/C49: the two edges of a node's busy state ---------------------------------------------------------
//
// The project's goal is that the GPU must not wait. Offline analysis of RotTR sessions 418-420 found the 3D ring
// idle for a median 8 ms, 418 to 605 times per 105 s, with a dispatchable packet already queued and the gap
// ending within 300 us of a display VSync: 0.45 to 0.67 ms of every frame, and 59 to 64 % of all ring idle. It
// took a 3 GB event-trace dump per session to see that. The driver owns both edges of its own ring, so it can
// say the same number itself, in every workload, from the log summary: ring_gap.h holds the arithmetic and the
// 300 us definition, this is where the two edges are.
//
// Each edge exists in exactly one place, and both already run under Lock:
//   node 0: WddmGfxHeadLocked below. The gfx completion queue going empty is the ring going idle; a submit
//           pushing onto an empty queue is the ring going busy. A retirement that leaves other jobs queued is
//           neither edge, and a second call while the ring is already idle must not restart the clock, which is
//           what Bc250RingGapOpen refuses.
//   node 1: the two assignments to PagingHwPending in WddmGpuFencePaging.
// The gfx ring is node 0's ring; if a packet of another node ever rode it, it would still occupy this ring, and
// this number is about the ring, not about the bookkeeping node.
static void WddmRingGapEdgeLocked(_Inout_ BC250_WDDM* Wddm, UINT Node, BOOLEAN Busy)
{
    LARGE_INTEGER qpc;

    if (Node >= BC250_WDDM_NODE_COUNT_MAX) return;
    qpc = KeQueryPerformanceCounter(NULL);       // callable at any IRQL, which is why the edges need nothing else
    if (Busy)
        (void)Bc250RingGapClose(&Wddm->RingGap[Node], (ULONGLONG)qpc.QuadPart,
                                (ULONGLONG)Wddm->RingGapFrequency.QuadPart,
                                (ULONGLONG)InterlockedCompareExchange64(&Wddm->RingGapLastVsyncQpc, 0, 0));
    else
        Bc250RingGapOpen(&Wddm->RingGap[Node], (ULONGLONG)qpc.QuadPart);
}

// Every vblank this driver acknowledged, whether or not the report that follows it is deferred: a deferred
// report still means the display reached its blanking interval, and that is what the pacing question is about.
// Interlocked rather than under Lock, because the VSync paths do not hold it.
static void WddmRingGapVsync(_Inout_ BC250_WDDM* Wddm)
{
    LARGE_INTEGER qpc = KeQueryPerformanceCounter(NULL);

    InterlockedExchange64(&Wddm->RingGapLastVsyncQpc, qpc.QuadPart);
    InterlockedIncrement(&Wddm->RingGapVsyncStampsHw);   // so a vsync-ended count of 0 can be told from no stamp
}

// Caller owns Lock. BD-114 (ANALYSIS.md 7.3): the budget belongs to the job AT THE HEAD and is stamped when it
// gets there, not when it was written to the ring. Until 0.7.216.27 the stamp was the ring write, so a packet
// behind six others on this seven-deep queue could have its whole budget spent queueing - which is the half of
// the defect the q27 dump needed (the g12 dump's queue held exactly one job, so it needed 7.1 and 7.2 instead).
//
// A job that already carries a deadline keeps it, which is the rule this function has always stated - appending
// work must not extend a hung job's watchdog - and is now also what stops a queued job from being charged for
// its wait. What a push behind a running head MAY move is the moment the watchdog next looks, because the look is
// a fixed cadence and not a fresh budget: the deadline is absolute and the staleness window keeps accumulating
// across a deferred check, so the cost of a deferral is one tick of detection latency, and the queue is seven
// deep (BC250_GFX_PENDING_MAX), so it is bounded by six of them. In exchange this function keeps the property it
// had before BD-114 - it leaves the timer armed whenever a head exists - instead of making the watchdog depend on
// an unbroken chain of DPC self-re-arms.
static void WddmGfxHeadLocked(BC250_WDDM* Wddm)
{
    BC250_GFX_COMPLETION* job = Bc250GfxQueueHead(&Wddm->GfxPending);
    LARGE_INTEGER due;
    ULONGLONG now;
    ULONG tick;
    WddmRingGapEdgeLocked(Wddm, BC250_WDDM_NODE_3D, job != NULL);
    Wddm->HwPending = job != NULL;
    if (!job)
    {
        KeCancelTimer(&Wddm->SubmitTimer);
        Bc250SubmitWatchdogIdle(&Wddm->SubmitWatchdog);     // no head: no window to judge
        return;
    }
    Wddm->HwSeq = job->Seq;
    Wddm->HwFence = job->Fence;
    Wddm->HwNode = job->Node;
    now = KeQueryInterruptTime();
    if (job->Deadline == 0ull)
    {
        // A NEW head. One moment supplies 7.3's stamp, 7.2's window and the queue-wait measurement, so the three
        // can never disagree about when this job started running.
        job->HeadSince = now;
        job->Deadline = Bc250SubmitHeadDeadline(job->Deadline, now, Wddm->SubmitBudgetMs);
        // What the job spent waiting, measured: the term section 4.2 of the analysis could not separate.
        if (job->Submitted != 0ull && now > job->Submitted)
        {
            LONG queued = (LONG)Bc250SubmitElapsedMs(job->Submitted, now);
            if (queued > Wddm->SubmitQueueMaxMs) Wddm->SubmitQueueMaxMs = queued;
        }
        Bc250SubmitWatchdogArm(&Wddm->SubmitWatchdog, now);
    }
    // The DPC looks for progress every tick inside the budget and re-arms itself; this is the next look, whether
    // the head is new or was already running.
    tick = Wddm->SubmitTickMs != 0ul ? Wddm->SubmitTickMs : 1ul;
    due.QuadPart = -10000ll * (LONGLONG)tick;
    if (!Wddm->Stopping) KeSetTimer(&Wddm->SubmitTimer, due, &Wddm->SubmitDpc);
}

void WddmGpuFence(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN done = FALSE;
    UINT fence = 0, node = 0;
    KIRQL irql;
    BC250_GFX_COMPLETION* job;
    if (wddm == NULL) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    while (!wddm->Recovery[BC250_WDDM_NODE_3D].ResetActive &&
           (job = Bc250GfxQueueHead(&wddm->GfxPending)) != NULL &&
           GfxFenceArrived(Device, job->Seq))
    {
        done = TRUE;
        node = job->Node;
        fence = job->ReportFence;
        HipJournalComplete(job->HipJournalId);
        Bc250GfxQueuePop(&wddm->GfxPending);
        if (!wddm->GfxPending.Count && wddm->DeferredValid) {
            fence = wddm->DeferredFence;
            wddm->DeferredValid = FALSE;
        }
        WddmRecordCompletionLocked(wddm, fence, node);
    }
    if (done) WddmGfxHeadLocked(wddm);
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!done) return;
    // KMD196: a completion-queue slot was freed, which is the other thing a held submission can be waiting for
    // (BC250_GFX_PENDING_MAX in WddmSubmitHardware). gfx.c signals the fence itself; this signals the queue.
    GfxRetireSignal(Device);
    if (InterlockedIncrement(&wddm->HwCompleted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: hardware fence arrived, reporting fence %u", fence);
    WddmQueueReport(wddm);
}

// Caller holds Lock. Latch one observation per head in the current recovery
// epoch. This must not change a fence, admission gate or health witness.
static BOOLEAN WddmLatchSubmitDiagnosticLocked(BC250_WDDM* Wddm, ULONGLONG Epoch)
{
    if (Wddm->Stopping ||
        !Bc250HangTimeoutCurrent(&Wddm->Recovery[BC250_WDDM_NODE_3D], Epoch)) return FALSE;
    if (Wddm->SubmitWatchdog.SnapshotTaken) return FALSE;
    Wddm->SubmitWatchdog.SnapshotTaken = 1;
    return TRUE;
}

static KDEFERRED_ROUTINE WddmSubmitDpcRoutine;
static KDEFERRED_ROUTINE WddmSubmitDpcCheck;
static void WddmSubmitDpcCheck(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    BOOLEAN timedOut = FALSE, rearm = FALSE, progressAvailable;
    UINT fence = 0, node = 0;
    ULONG seq = 0, process = 0, contextFlags = 0, vmid = 0;
    ULONG headMs = 0, queuedMs = 0, staleMs = 0, budgetMs = 0, tick = 0;
    ULONGLONG context = 0, now, progress, age = 0, epoch;
    ULONG sampledSeq;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    WddmGpuFence(device);               // late is still arrived
    KeAcquireSpinLock(&wddm->Lock, &irql);
    epoch = wddm->Recovery[BC250_WDDM_NODE_3D].Epoch;
    sampledSeq = wddm->HwSeq;
    if (wddm->Stopping || wddm->Recovery[BC250_WDDM_NODE_3D].ResetActive) {
        KeReleaseSpinLock(&wddm->Lock, irql);
        return;
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    // BD-114 (ANALYSIS.md 7.2): what the hardware is doing, read BEFORE the lock, because it costs register
    // reads. A watchdog that knows only the wall clock cannot tell a long healthy job from a dead one; this is
    // the only input that can. The token is read as "progress observed" whenever it changes at all, which is the
    // conservative direction. Token activity can belong to other work; it does not
    // prove head-job progress. This clock and Windows' preemption clock differ.
    progressAvailable = WddmSubmitProgress(device, &progress);
    now = KeQueryInterruptTime();
    KeAcquireSpinLock(&wddm->Lock, &irql);
    if (wddm->Stopping || sampledSeq != wddm->HwSeq ||
        !Bc250HangTimeoutCurrent(&wddm->Recovery[BC250_WDDM_NODE_3D], epoch)) {
        KeReleaseSpinLock(&wddm->Lock, irql);
        return;
    }
    budgetMs = wddm->SubmitBudgetMs;
    if (wddm->HwPending && !wddm->WatchdogFaulted[wddm->HwNode])
    {
        const BC250_GFX_COMPLETION* head = Bc250GfxQueueHead(&wddm->GfxPending);
        int stale = 0;
        if (progressAvailable) {
            stale = Bc250SubmitWatchdogCheck(&wddm->SubmitWatchdog, progress, now,
                                            10000ull * (ULONGLONG)budgetMs, &age);
            switch (wddm->SubmitWatchdog.LastObservation) {
            case BC250_SUBMIT_OBSERVATION_PRIME: wddm->SubmitPrimes++; break;
            case BC250_SUBMIT_OBSERVATION_ACTIVITY: wddm->SubmitActivityChanges++; break;
            case BC250_SUBMIT_OBSERVATION_GAP_RESET: wddm->SubmitGapResets++; break;
            default: break;
            }
        }

        // Both, not either: the staleness window alone already implies the deadline (the window opens no earlier
        // than the head stamp), and saying so twice means no refactor of one can fault a job before its budget.
        if (stale && head != NULL && now >= head->Deadline)
        {
            timedOut = WddmLatchSubmitDiagnosticLocked(wddm, epoch);
            fence = wddm->HwFence;
            node = wddm->HwNode;
            seq = wddm->HwSeq;
            context = head->Context;            // KMD193: read under the lock, logged outside it
            process = head->ProcessId;
            contextFlags = head->ContextFlags;
            vmid = head->Vmid;
            headMs = Bc250SubmitElapsedMs(head->HeadSince, now);
            queuedMs = head->Submitted != 0ull ? Bc250SubmitElapsedMs(head->Submitted, head->HeadSince) : 0ul;
            staleMs = (ULONG)(age / 10000ull);
            // Preserve HwPending: timeout is not a hardware completion.
        }
        if (head != NULL)
        {
            LONG held;

            rearm = !wddm->Stopping;
            if (head->HeadSince != 0ull)
            {
                held = (LONG)Bc250SubmitElapsedMs(head->HeadSince, now);
                if (held > wddm->SubmitHeadMaxMs) wddm->SubmitHeadMaxMs = held;
            }
        }
    }
    // UNDER THE LOCK, and this is not a style choice. WddmSuspendRetained sets Stopping and cancels this timer in
    // one critical section; WddmStop sets Stopping under the lock and cancels after releasing it (step 1 of its
    // own comment). Either shape is safe only for a re-arm that holds the same lock: such a re-arm is either
    // before the Stopping store, and is then taken back by the cancel, the KeRemoveQueueDpc and the
    // KeFlushQueuedDpcs that follow it, or after it, and then reads Stopping as TRUE and does not arm at all. A
    // re-arm outside the lock has neither guarantee: it can read Stopping as FALSE and still call KeSetTimer
    // after the cancel, the KeRemoveQueueDpc and the KeFlushQueuedDpcs - on a KTIMER and a KDPC that live inside
    // the BC250_WDDM allocation WddmStop then frees. KeSetTimer and
    // KeCancelTimer both run at IRQL <= DISPATCH_LEVEL, which is where this lock is held, so the invariant costs
    // nothing. WddmGfxHeadLocked arms the same timer the same way, under the same lock.
    if (rearm)
    {
        LARGE_INTEGER due;
        // The DPC owns its own cadence inside a head's budget. A head change re-arms it from the new stamp
        // (WddmGfxHeadLocked); an empty queue cancels it.
        tick = wddm->SubmitTickMs != 0ul ? wddm->SubmitTickMs : 1ul;
        due.QuadPart = -10000ll * (LONGLONG)tick;
        wddm->SubmitRearms++;                   // under the lock, like the two high-water marks beside it
        KeSetTimer(&wddm->SubmitTimer, due, &wddm->SubmitDpc);
    }
    wddm->SubmitChecks++;
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // Diagnostic only: preserve admission, deferred work and real fence retirement.
    // The scheduler's preemption clock, not this sampled inactivity, owns TDR.
    InterlockedIncrement(&wddm->HwTimeouts);
    // BD-114 (ANALYSIS.md 7.7): the measured numbers, not the constant. Until 0.7.216.27 this line printed the
    // budget itself, so "after 500 ms" meant "after at least 500 ms, by an unknown amount" and the offline
    // analysis of two bugchecks could bound a packet's duration but never measure one. Three numbers now:
    // head is how long the job has been at the head of the ring, queued how long it waited behind others before
    // that, and stale how long the progress token stood still - the quantity the watchdog actually judged.
    GuardLog("wddm: submit diagnostic v2 seq %lu fence %u: observed inactivity, admission unchanged", seq, fence);
    GuardLog("wddm: timeout measured: head %lu ms, queued %lu ms, stale %lu ms, budget %lu ms", headMs, queuedMs,
             staleMs, budgetMs);
    // KMD193: who the job belonged to, then what the hardware held. Both once per timeout; the identity comes
    // out of the queue entry, which keeps it from the submit (bsod-245 items 2 and 4).
    // KMD214: and the VMID it ran at, with that VMID's tenants (the pool may have recycled it since).
    GuardLog("wddm: timeout seq %lu fence %u node %u vmid %lu ctx 0x%llX pid %lu ctxflags 0x%lX", seq, fence, node,
             vmid, context, process, contextFlags);
    if (InterlockedIncrement(&wddm->FaultSnapshots) <= BC250_WDDM_LOG_CALLS)
    {
        GfxVmidReport(device, "wddm: timeout job", vmid);
        WddmTimeoutSnapshot(device, seq, fence, node);
    }
    // No DMA_COMPLETED or preemption notification is synthesized here.
}

// Progress record around the check, outside it so that none of its early returns can skip the exit (hang.c).
static void WddmSubmitDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    ProgressEnter(ProgressSiteSubmitWatchdogDpc);
    WddmSubmitDpcCheck(Dpc, Context, Arg1, Arg2);
    ProgressExit(ProgressSiteSubmitWatchdogDpc, 0);
}

// PASSIVE_LEVEL (SubmitCommandVirtual). TRUE = the packet is on the ring and its completion will come by itself.
static BOOLEAN WddmSubmitHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, _In_ const BC250_WDDM_OBJECT* Context,
                                  ULONGLONG GpuVa, ULONG Bytes, UINT FenceId, UINT Node, ULONGLONG HipId)
{
    ULONG seq = 0, vmid = 0;
    NTSTATUS status;
    KIRQL irql;
    BC250_GFX_COMPLETION job;
    BC250_GFX_SUBMIT_IDENTITY identity;
    BOOLEAN allowed;
    // KMD193: values only, copied once here, so that the ring, the journal and the pending queue all name the
    // same context without gfx.c ever holding a pointer to a WDDM object.
    identity.Context = (ULONGLONG)(ULONG_PTR)Context;
    identity.ProcessId = Context->CreatorProcessId;
    identity.ContextFlags = (Context->UmdContext ? BC250_PJ_CTX_UMD : 0u) |
                            (Context->SystemContext ? BC250_PJ_CTX_SYSTEM : 0u);
    identity.Fence = FenceId;
    identity.Node = Node;
    ExAcquireFastMutex(&Wddm->GfxSubmitMutex);
    WddmGpuFence(Device);
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    allowed = !Wddm->Stopping && !Wddm->WatchdogFaulted[Node] &&
              Wddm->GfxPending.Count < BC250_GFX_PENDING_MAX;
    job.Epoch = 0; //151 baseline has adapter-lifetime ownership, no recovery ledger
    KeReleaseSpinLock(&Wddm->Lock, irql);
    if (!allowed) { ExReleaseFastMutex(&Wddm->GfxSubmitMutex); return FALSE; }
    status = GfxSubmitIb(Device, BC250_VMID_AUTO, Context->RootPhysical, GpuVa, Bytes, &identity, &seq, &vmid);
    if (!NT_SUCCESS(status))
    {
        ExReleaseFastMutex(&Wddm->GfxSubmitMutex);
        if (status != STATUS_DEVICE_BUSY && InterlockedIncrement(&Wddm->HwRefused) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: ring refused 0x%08X (fence %u, va 0x%llX, %u bytes): NOT completed", status,
                     FenceId, GpuVa, Bytes);
        return FALSE;
    }
    job.Seq = seq;
    job.Fence = job.ReportFence = FenceId;
    job.Node = Node;
    job.Context = identity.Context;
    job.ProcessId = identity.ProcessId;
    job.ContextFlags = identity.ContextFlags;
    job.Vmid = vmid;
    job.HipJournalId = HipId;
    HipJournalSubmitted(HipId, seq, vmid, Context->RootPhysical);
    // BD-114 (ANALYSIS.md 7.3): the ring write is recorded, not charged. WddmGfxHeadLocked stamps the deadline
    // when this job reaches the head, so the budget pays for execution and not for the queue.
    job.Submitted = KeQueryInterruptTime();
    job.Deadline = 0ull;
    job.HeadSince = 0ull;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    // A software-only fence between two HW jobs belongs to the older tail,
    // never to the new job or to the oldest unrelated completion.
    if (Wddm->DeferredValid && Wddm->GfxPending.Count) {
        Bc250GfxQueueTail(&Wddm->GfxPending)->ReportFence = Wddm->DeferredFence;
        Wddm->DeferredValid = FALSE;
    }
    (void)Bc250GfxQueuePush(&Wddm->GfxPending, job); // reserved by GfxSubmitMutex
    WddmGfxHeadLocked(Wddm);
    KeReleaseSpinLock(&Wddm->Lock, irql);
    ExReleaseFastMutex(&Wddm->GfxSubmitMutex);
    if (InterlockedIncrement(&Wddm->HwSubmitted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: fence %u on the gfx ring: sequence %u, vmid %u, root 0x%llX, va 0x%llX, %u bytes", FenceId,
                 seq, vmid, Context->RootPhysical, GpuVa, Bytes);
    WddmGpuFence(Device);
    return TRUE;
}

// ---- KMD196: the held submission ------------------------------------------------------------------------------
//
// gfx.c SubmitIbLocked never changes the root of a VMID whose job has not retired. With EnableVmidPool 0 every job
// runs at VMID 1, so a submission that arrives while another process's job is on the ring is refused and has to
// wait for that job's fence; with the pool (KMD214) it is refused only when no VMID of the pool is free. Both
// waiters below - the UMD one and the GPU Present one - use this state and these two calls, so there is one
// definition of what "held" means, one deadline and one set of counters.
//
// The wait has two phases, and the order is what the measurement of sessions 313/314 dictates (both aligned to
// the microsecond against dxgkrnl ETW):
//
//   - DWM's composition job is 0.29 ms long (p10-p90 0.26-0.31).
//   - dxgkrnl's node-0 worker calls SubmitCommand for the game's next packet 0.14 ms after that job started,
//     which is about 0.13 ms BEFORE DWM's completion interrupt. So at the moment of the refusal the blocking
//     job has roughly 0.13 ms left to run.
//   - The KeDelayExecutionThread(1 ms) this replaces actually lasted p50 4.2 ms / p90 5.6 ms in the HIGH
//     session and p50 2.3 / p90 14.2 ms in the LOW one: a relative sleep expires on a clock tick, so the
//     "1 ms" was a tick, not a millisecond. Readied-to-running was only 0.04-0.06 ms, so nothing was starved
//     of CPU - the timer simply fired late.
//   - Cost: 4.7 ms per DWM-to-game handover, 0.6-1.0 holds a frame, 2.9-4.0 ms of GFX idle a frame.
//
// Phase 1 is therefore a bounded spin of BC250_WDDM_HOLD_SPIN_US, which is long enough to cover that 0.13 ms
// several times over and short enough to be cheap: a submission caught here costs no context switch at all and
// no timer at all. Phase 2 is the event wait, for the cases the spin does not catch - a longer blocking job, a
// queue slot, several waiters - with a BC250_WDDM_HOLD_WAIT_MS fallback timeout so that a lost end-of-pipe
// interrupt still ends the wait. A bare KeDelayExecutionThread appears nowhere in either phase.
//
// The spin runs at APC_LEVEL or below, never at DISPATCH_LEVEL, so the fence DPC that ends it can preempt this
// thread on the same core; and every spin step calls WddmGpuFence through the caller's loop, which reads the
// fence page directly, so the spin does not actually depend on that DPC being scheduled at all.
#define BC250_WDDM_HOLD_SPIN_US 500     // phase 1 budget: the blocking job has ~130 us left at the refusal
#define BC250_WDDM_HOLD_SPIN_STEP_US 20 // one stall between retries; 25 retries fill the budget
#define BC250_WDDM_HOLD_WAIT_MS 1       // phase 2 fallback only; the event normally wakes it first

static const char* const g_WddmHoldBucketNames[BC250_WDDM_HOLD_BUCKETS] = {
    "<100", "100", "200", "500", "1k", "2k", "5k", "10k", "20k+"
};

// WddmSummaryOf prints the nine buckets by name in one line, one argument pair each: adding a bucket means
// growing that line, so it is pinned here rather than left to be noticed in a log that silently lost a column.
C_ASSERT(BC250_WDDM_HOLD_BUCKETS == 9);

static ULONG WddmHoldBucket(ULONG HeldUs)
{
    if (HeldUs < 100) return 0;
    if (HeldUs < 200) return 1;
    if (HeldUs < 500) return 2;
    if (HeldUs < 1000) return 3;
    if (HeldUs < 2000) return 4;
    if (HeldUs < 5000) return 5;
    if (HeldUs < 10000) return 6;
    if (HeldUs < 20000) return 7;
    return 8;
}

typedef struct _BC250_WDDM_HOLD {
    ULONGLONG Deadline;                 // interrupt time; BC250_WDDM_HOLD_DEADLINE_MS, which BD-114 left at 500 ms
                                        // on purpose (this bounds a CPU wait, not the GPU's execution)
    LARGE_INTEGER Start;                // QPC at the first refusal, for the held time in microseconds
    LARGE_INTEGER Frequency;            // QPC frequency, read once with Start
    LONG Generation;                    // Device->GfxRetireGeneration as of the last condition test
    ULONG Spins;                        // phase 1 stalls
    ULONG EventWakes;
    ULONG TimeoutWakes;
} BC250_WDDM_HOLD;

// Microseconds since the first refusal. The frequency comes out of the same KeQueryPerformanceCounter call as
// Start, so this needs nothing from the adapter block and works before WddmStart has recorded a frequency.
static ULONG WddmHoldElapsedUs(_In_ const BC250_WDDM_HOLD* Hold)
{
    LONGLONG ticks = KeQueryPerformanceCounter(NULL).QuadPart - Hold->Start.QuadPart;

    if (ticks < 0 || Hold->Frequency.QuadPart <= 0) return 0;
    return (ULONG)((ULONGLONG)ticks * 1000000ull / (ULONGLONG)Hold->Frequency.QuadPart);
}

// Before the first condition test, so that a retirement between that test and the first wait is not lost.
static void WddmHoldBegin(_Inout_ BC250_DEVICE* Device, _Out_ BC250_WDDM_HOLD* Hold)
{
    Hold->Deadline = KeQueryInterruptTime() + 10000ull * BC250_WDDM_HOLD_DEADLINE_MS;
    Hold->Start = KeQueryPerformanceCounter(&Hold->Frequency);
    Hold->Generation = InterlockedCompareExchange(&Device->GfxRetireGeneration, 0, 0);
    Hold->Spins = 0;
    Hold->EventWakes = 0;
    Hold->TimeoutWakes = 0;
}

// TRUE: something may have changed, test the submit condition again. FALSE: the deadline passed, the adapter is
// stopping, or this thread may not wait - the caller makes its terminal attempt and gives up.
//
// Phase 2's shape: clear, then re-read the generation, then wait. A retirement after the caller's test has
// already bumped the generation, so this returns at once rather than waiting for a fence that has arrived; and
// a concurrent waiter's clear cannot swallow this one's wake, because the generation it reads has moved too.
// The event is a NotificationEvent on purpose: one retirement releases every held submission, and they then
// compete for GfxSubmitMutex exactly as they competed for the ring before.
static BOOLEAN WddmHoldWait(_Inout_ BC250_DEVICE* Device, _In_opt_ BC250_WDDM* Wddm, _Inout_ BC250_WDDM_HOLD* Hold)
{
    LARGE_INTEGER timeout;
    LONG generation;
    NTSTATUS status;

    // A wait with a timeout needs APC_LEVEL or below, which both call sites already check before they get here;
    // this is the bound restated where the waiting happens, not a new policy. It governs the spin as well: a
    // spin at DISPATCH_LEVEL could outlast the DPC that would end it.
    if (KeGetCurrentIrql() > APC_LEVEL) return FALSE;
    // Teardown: once Stopping is set WddmSubmitHardware refuses every submission anyway, so waiting out the
    // remaining deadline would only delay the stop. WddmStop signals the event after setting it, so this is
    // observed on the first wake and not on a timeout.
    if (Wddm != NULL && WddmStopping(Wddm)) return FALSE;
    if (KeQueryInterruptTime() >= Hold->Deadline) return FALSE;

    // Phase 1. Measured by QPC, not by counting stalls: KeStallExecutionProcessor is a lower bound on the
    // delay, so a counted budget would be a budget only on paper.
    if (WddmHoldElapsedUs(Hold) < BC250_WDDM_HOLD_SPIN_US)
    {
        KeStallExecutionProcessor(BC250_WDDM_HOLD_SPIN_STEP_US);
        Hold->Spins++;
        // The generation is refreshed so that phase 2, if it is reached, does not treat a retirement the spin
        // already saw as a reason to skip its wait.
        Hold->Generation = InterlockedCompareExchange(&Device->GfxRetireGeneration, 0, 0);
        return TRUE;
    }

    // Phase 2.
    KeClearEvent(&Device->GfxRetireEvent);
    generation = InterlockedCompareExchange(&Device->GfxRetireGeneration, 0, 0);
    if (generation != Hold->Generation)
    {
        Hold->Generation = generation;
        Hold->EventWakes++;
        return TRUE;
    }
    timeout.QuadPart = -10000ll * BC250_WDDM_HOLD_WAIT_MS;
    status = KeWaitForSingleObject(&Device->GfxRetireEvent, Executive, KernelMode, FALSE, &timeout);
    Hold->Generation = InterlockedCompareExchange(&Device->GfxRetireGeneration, 0, 0);
    if (status == STATUS_TIMEOUT) Hold->TimeoutWakes++;
    else Hold->EventWakes++;
    return TRUE;
}

// Once per submission that was held at all, whatever its outcome. Microseconds from QPC: the line this replaced
// printed the loop's iteration count as "1 ms" every time, which is why session 314's 4081 held submits all
// looked identical and none of them could be added up.
static void WddmHoldReport(_In_opt_ BC250_WDDM* Wddm, _In_ const BC250_WDDM_HOLD* Hold, UINT FenceId,
                           _In_z_ const char* What)
{
    ULONG held;
    LONG64 seen;

    if (Wddm == NULL || (Hold->Spins == 0 && Hold->EventWakes == 0 && Hold->TimeoutWakes == 0)) return;
    held = WddmHoldElapsedUs(Hold);
    InterlockedIncrement(&Wddm->SubmitHolds);
    InterlockedAdd64(&Wddm->SubmitHeldUs, (LONG64)held);
    InterlockedAdd64(&Wddm->SubmitHoldSpins, (LONG64)Hold->Spins);
    InterlockedAdd64(&Wddm->SubmitHoldEventWakes, (LONG64)Hold->EventWakes);
    InterlockedAdd64(&Wddm->SubmitHoldTimeoutWakes, (LONG64)Hold->TimeoutWakes);
    InterlockedIncrement(&Wddm->SubmitHeldHistogram[WddmHoldBucket(held)]);
    // A submission the spin caught never reached the event wait: counting those separately is how the lab run
    // tells "the spin was the right call" from "the spin only burned CPU".
    if (Hold->EventWakes == 0 && Hold->TimeoutWakes == 0) InterlockedIncrement(&Wddm->SubmitHoldSpinOnly);
    for (;;)
    {
        seen = InterlockedCompareExchange64(&Wddm->SubmitHeldMaxUs, 0, 0);
        if ((LONG64)held <= seen) break;
        if (InterlockedCompareExchange64(&Wddm->SubmitHeldMaxUs, (LONG64)held, seen) == seen) break;
    }
    // Uncapped, like the line it replaces: 4081 lines in 220 s is 18 a second, and the per-submission
    // distribution is the whole point of the comparison the summary counters only total up.
    GuardLog("wddm: %s submit fence %u held %lu us (%lu spins, %lu event, %lu timeout)",
             What, FenceId, held, Hold->Spins, Hold->EventWakes, Hold->TimeoutWakes);
}

// BGP1 may arrive in a burst when CDD stops pacing to vblank (M659).
// Idle is not a submission prerequisite: the lower layer admits same-root jobs
// and reports temporary ring/root-switch pressure. Match the UMD ready-or-busy
// policy, with a wall-clock deadline and no completion synthesized on failure.
static BOOLEAN WddmSubmitPresentHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm,
    _In_ const BC250_WDDM_OBJECT* Context, ULONGLONG GpuVa, ULONG Bytes, UINT FenceId, UINT Node)
{
    BC250_WDDM_HOLD hold;

    WddmHoldBegin(Device, &hold);
    for (;;) {
        if ((GfxSubmitReady(Device) || GfxSubmitBusy(Device)) &&
            WddmSubmitHardware(Device, Wddm, Context, GpuVa, Bytes, FenceId, Node, 0)) {
            WddmHoldReport(Wddm, &hold, FenceId, "present");
            return TRUE;
        }
        // Refresh retirement before the terminal retry: a completion can race
        // the separate ready/busy observations or release the last queue slot.
        WddmGpuFence(Device);
        if (!GfxSubmitBusy(Device) || !WddmHoldWait(Device, Wddm, &hold)) {
            BOOLEAN submitted = (GfxSubmitReady(Device) || GfxSubmitBusy(Device)) &&
                WddmSubmitHardware(Device, Wddm, Context, GpuVa, Bytes, FenceId, Node, 0);
            WddmHoldReport(Wddm, &hold, FenceId, "present");
            return submitted;
        }
    }
}

// ---- WDDM node 2: the compute ring's channel (docs/design/compute-node.md) ------------------------------------
//
// Node 0's channel above, on its own queue, mutex and fence (gfx.c GfxSubmitCompute, slot 11), under the same Lock.
// It keeps node 0's rules: the completion queue is in submit order, a completion of fence N retires every fence up
// to N, and a software completion that comes while a job is on the ring waits for it (ComputeDeferredValid). Two
// things are left out on purpose. There is no diagnostic watchdog: a hang is the OS TDR's (BD-152), and
// ResetEngine node 2 is the recovery. ComputeTimer only polls the fence while a job is at the head, so that a lost
// end-of-pipe interrupt cannot leave a finished job unreported.

// Caller owns Lock. The head of node 2's queue decides ComputeHwPending and the poll timer.
static void WddmComputeHeadLocked(BC250_WDDM* Wddm)
{
    BC250_GFX_COMPLETION* job = Bc250GfxQueueHead(&Wddm->ComputePending);
    LARGE_INTEGER due;
    ULONG tick;

    WddmRingGapEdgeLocked(Wddm, BC250_WDDM_NODE_COMPUTE, job != NULL);
    Wddm->ComputeHwPending = job != NULL;
    if (!job)
    {
        KeCancelTimer(&Wddm->ComputeTimer);
        return;
    }
    if (job->HeadSince == 0ull) job->HeadSince = KeQueryInterruptTime();
    // Under the lock, for the reason WddmSubmitDpcCheck states for SubmitTimer: Stopping is read under it.
    tick = Wddm->SubmitTickMs != 0ul ? Wddm->SubmitTickMs : 1ul;
    due.QuadPart = -10000ll * (LONGLONG)tick;
    if (!Wddm->Stopping) KeSetTimer(&Wddm->ComputeTimer, due, &Wddm->ComputeDpc);
}

// WddmGpuFence for node 2: from the IH DPC (pnp.c), from the submit and from the poll timer, at <= DISPATCH_LEVEL.
void WddmGpuFenceCompute(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    BOOLEAN done = FALSE;
    UINT fence = 0, node = BC250_WDDM_NODE_COMPUTE;
    KIRQL irql;
    BC250_GFX_COMPLETION* job;

    if (wddm == NULL || wddm->NodeCount <= BC250_WDDM_NODE_COMPUTE) return;
    KeAcquireSpinLock(&wddm->Lock, &irql);
    while (!wddm->Recovery[BC250_WDDM_NODE_COMPUTE].ResetActive &&
           (job = Bc250GfxQueueHead(&wddm->ComputePending)) != NULL &&
           GfxComputeFenceArrived(Device, job->Seq))
    {
        done = TRUE;
        node = job->Node;
        fence = job->ReportFence;
        HipJournalComplete(job->HipJournalId);
        Bc250GfxQueuePop(&wddm->ComputePending);
        if (!wddm->ComputePending.Count && wddm->ComputeDeferredValid) {
            fence = wddm->ComputeDeferredFence;
            wddm->ComputeDeferredValid = FALSE;
        }
        WddmRecordCompletionLocked(wddm, fence, node);
    }
    if (done) WddmComputeHeadLocked(wddm);
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!done) return;
    GfxRetireSignal(Device);            // a queue slot was freed, as in WddmGpuFence
    if (InterlockedIncrement(&wddm->ComputeHwCompleted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: compute fence arrived, reporting fence %u node %u", fence, node);
    WddmQueueReport(wddm);
}

static KDEFERRED_ROUTINE WddmComputeDpcRoutine;
static void WddmComputeDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    LARGE_INTEGER due;
    ULONG tick;
    KIRQL irql;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    WddmGpuFenceCompute(device);
    KeAcquireSpinLock(&wddm->Lock, &irql);
    // A reset in progress ends with WddmComputeHeadLocked, which arms the timer again if a head is left.
    if (!wddm->Stopping && wddm->ComputeHwPending && !wddm->Recovery[BC250_WDDM_NODE_COMPUTE].ResetActive)
    {
        tick = wddm->SubmitTickMs != 0ul ? wddm->SubmitTickMs : 1ul;
        due.QuadPart = -10000ll * (LONGLONG)tick;
        KeSetTimer(&wddm->ComputeTimer, due, &wddm->ComputeDpc);
    }
    wddm->ComputePolls++;
    KeReleaseSpinLock(&wddm->Lock, irql);
}

// WddmSubmitHardware for node 2. PASSIVE_LEVEL (SubmitCommandVirtual). TRUE = the job is on the compute ring.
static BOOLEAN WddmSubmitComputeHardware(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm,
                                         _In_ const BC250_WDDM_OBJECT* Context, ULONGLONG GpuVa, ULONG Bytes,
                                         UINT FenceId, UINT Node, ULONGLONG HipId)
{
    ULONG seq = 0, vmid = 0;
    NTSTATUS status;
    KIRQL irql;
    BC250_GFX_COMPLETION job;
    BC250_GFX_SUBMIT_IDENTITY identity;
    BOOLEAN allowed;

    identity.Context = (ULONGLONG)(ULONG_PTR)Context;
    identity.ProcessId = Context->CreatorProcessId;
    identity.ContextFlags = (Context->UmdContext ? BC250_PJ_CTX_UMD : 0u) |
                            (Context->SystemContext ? BC250_PJ_CTX_SYSTEM : 0u);
    identity.Fence = FenceId;
    identity.Node = Node;
    ExAcquireFastMutex(&Wddm->ComputeSubmitMutex);
    WddmGpuFenceCompute(Device);
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    allowed = !Wddm->Stopping && !Wddm->WatchdogFaulted[Node] &&
              Wddm->ComputePending.Count < BC250_GFX_PENDING_MAX;
    KeReleaseSpinLock(&Wddm->Lock, irql);
    if (!allowed) { ExReleaseFastMutex(&Wddm->ComputeSubmitMutex); return FALSE; }
    status = GfxSubmitCompute(Device, Context->RootPhysical, GpuVa, Bytes, &identity, &seq, &vmid);
    if (!NT_SUCCESS(status))
    {
        ExReleaseFastMutex(&Wddm->ComputeSubmitMutex);
        if (status != STATUS_DEVICE_BUSY && InterlockedIncrement(&Wddm->ComputeHwRefused) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: compute ring refused 0x%08X (fence %u, va 0x%llX, %u bytes): NOT completed", status,
                     FenceId, GpuVa, Bytes);
        return FALSE;
    }
    RtlZeroMemory(&job, sizeof(job));
    job.Seq = seq;
    job.Fence = job.ReportFence = FenceId;
    job.Node = Node;
    job.Context = identity.Context;
    job.ProcessId = identity.ProcessId;
    job.ContextFlags = identity.ContextFlags;
    job.Vmid = vmid;
    job.HipJournalId = HipId;
    HipJournalSubmitted(HipId, seq, vmid, Context->RootPhysical);
    job.Submitted = KeQueryInterruptTime();
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    // The rule of WddmSubmitHardware: a software fence between two jobs belongs to the older one.
    if (Wddm->ComputeDeferredValid && Wddm->ComputePending.Count) {
        Bc250GfxQueueTail(&Wddm->ComputePending)->ReportFence = Wddm->ComputeDeferredFence;
        Wddm->ComputeDeferredValid = FALSE;
    }
    (void)Bc250GfxQueuePush(&Wddm->ComputePending, job);    // reserved by ComputeSubmitMutex
    WddmComputeHeadLocked(Wddm);
    KeReleaseSpinLock(&Wddm->Lock, irql);
    ExReleaseFastMutex(&Wddm->ComputeSubmitMutex);
    if (InterlockedIncrement(&Wddm->ComputeHwSubmitted) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: fence %u on the compute ring: sequence %u, vmid %u, root 0x%llX, va 0x%llX, %u bytes",
                 FenceId, seq, vmid, Context->RootPhysical, GpuVa, Bytes);
    WddmGpuFenceCompute(Device);
    return TRUE;
}

// The ring channel of a node that runs IBs: node 0 (gfx) or node 2 (compute). Used where the two paths are
// otherwise one: the UMD submit with its hold, and ResetEngine.
static BOOLEAN WddmRingAdmits(_In_ const BC250_DEVICE* Device, UINT Node)
{
    if (Node == BC250_WDDM_NODE_COMPUTE) return GfxComputeReady(Device) || GfxComputeBusy(Device);
    return GfxSubmitReady(Device) || GfxSubmitBusy(Device);
}

static BOOLEAN WddmRingBusy(_In_ const BC250_DEVICE* Device, UINT Node)
{
    return Node == BC250_WDDM_NODE_COMPUTE ? GfxComputeBusy(Device) : GfxSubmitBusy(Device);
}

static void WddmRingFence(_Inout_ BC250_DEVICE* Device, UINT Node)
{
    if (Node == BC250_WDDM_NODE_COMPUTE) WddmGpuFenceCompute(Device);
    else WddmGpuFence(Device);
}

static BOOLEAN WddmRingSubmit(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm,
                              _In_ const BC250_WDDM_OBJECT* Context, ULONGLONG GpuVa, ULONG Bytes, UINT FenceId,
                              UINT Node, ULONGLONG HipId)
{
    if (Node == BC250_WDDM_NODE_COMPUTE)
        return WddmSubmitComputeHardware(Device, Wddm, Context, GpuVa, Bytes, FenceId, Node, HipId);
    return WddmSubmitHardware(Device, Wddm, Context, GpuVa, Bytes, FenceId, Node, HipId);
}

// Caller owns Lock.
static BC250_GFX_COMPLETION_QUEUE* WddmRingQueue(_In_ BC250_WDDM* Wddm, UINT Node)
{
    return Node == BC250_WDDM_NODE_COMPUTE ? &Wddm->ComputePending : &Wddm->GfxPending;
}

static void WddmRingHeadLocked(_Inout_ BC250_WDDM* Wddm, UINT Node)
{
    if (Node == BC250_WDDM_NODE_COMPUTE) WddmComputeHeadLocked(Wddm);
    else WddmGfxHeadLocked(Wddm);
}

// ---- ADR 0008 stage D: node 1's own hardware channel (docs/design/paging-node.md section 5) ---------------------
//
// A parallel channel to stage C's above, not a generalization of it: WddmSubmitPagingHardware runs at
// DISPATCH_LEVEL (Bc250WddmSubmitCommand, exactly, design note section 4), where WddmSubmitHardware runs at
// PASSIVE_LEVEL (SubmitCommandVirtual) and may call GfxSubmitIb's GartLock-taking path; this one calls
// GfxSubmitPaging instead, which takes no lock beyond gfx.c's own Sdma0RingLock. The two channels fail
// independently: a node-1 timeout calls GfxPagingSubmitFail, never GfxSubmitFail, and vice versa.

// A drain that stopped at its quota may leave the head queued with no packet in flight, so no interrupt will come
// for it: the next invocation has to be arranged here. A timer rather than KeInsertQueueDpc: a DPC
// queued from a DPC can run in the same DPC drain, with no return to PASSIVE_LEVEL in between. A relative due time
// of -1 asks for the earliest expiry; it does not promise that threads run before it, nor any bound on latency
// under load; both are left to measurement. Armed under Lock against Stopping, like every other timer of this
// file, so WddmStop's cancel is final.
static void WddmRequeuePagingDrain(_Inout_ BC250_WDDM* Wddm)
{
    LARGE_INTEGER due;
    KIRQL irql;

    due.QuadPart = -1;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    if (!Wddm->Stopping) KeSetTimer(&Wddm->PagingDrainTimer, due, &Wddm->PagingDrainDpc);
    KeReleaseSpinLock(&Wddm->Lock, irql);
}

// Has node 1's in-flight fence arrived? Same shape as WddmGpuFence, called from the same places (the IH DPC,
// the submit itself, the watchdog), at <= DISPATCH_LEVEL.
// One GPU packet at a time preserves the shared temporary mapping window.
// Later OS packets retain their private command storage in FIFO order.
// KMD172: at most PAGING_DRAIN_QUOTA retirements per invocation (paging_drain.h). Until 171 a caller retired for
// as long as other callers kept publishing packets the GPU finished in between, at DISPATCH_LEVEL, with no bound.
void WddmGpuFencePaging(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    unsigned retiredCount=0;
    ULONG iterations=0;
    PAGING_DRAIN_EXIT exitReason=PagingDrainExitIdle;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    static volatile LONG labRetiredOnce;     // BD-235: the receipt at the first retired paging fence, once per boot
#endif
    if (!wddm) return;
    ProgressEnter(ProgressSitePagingDrain);
    for (;;) {
        BC250_PAGING_JOB* retired=NULL;
        BOOLEAN completed=FALSE, failed=FALSE;
        UINT fence=0;
        ULONG seq=0;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        ULONG labSeq=0;
#endif
        NTSTATUS status;
        KIRQL irql;
        LARGE_INTEGER due;
        PAGING_DRAIN_NEXT next;
        iterations++;
        KeAcquireSpinLock(&wddm->Lock,&irql);
        if (wddm->Stopping) {
            KeReleaseSpinLock(&wddm->Lock,irql);
            exitReason=PagingDrainExitStopping;
            break;
        }
        if (wddm->PagingHwPending && GfxPagingFenceArrived(Device,wddm->PagingHwSeq)) {
            retired=wddm->PagingHead;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
            labSeq=wddm->PagingHwSeq;       // BD-235: the retired sequence, for the first-retired receipt
#endif
            wddm->PagingHwPending=FALSE;
            WddmRingGapEdgeLocked(wddm, BC250_WDDM_NODE_COPY, FALSE);   // C48: node 1's ring went idle
            KeCancelTimer(&wddm->PagingSubmitTimer);
            completed=TRUE;
        } else if (!wddm->PagingHwPending && wddm->PagingHead &&
                   !wddm->PreemptionPending[BC250_WDDM_NODE_COPY] &&
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
                    WddmRingGapEdgeLocked(wddm, BC250_WDDM_NODE_COPY, TRUE);  // C48: node 1's ring went busy
                    wddm->PagingHwSeq=seq;
                    wddm->PagingHwFence=job->Fence;
                    PagingJournalStampSeq(job->Fence,seq);
                    // BD-114: node 1 gets the same budget as node 0. A paging copy is not the hang class of
                    // the two dumps, but its refusal path is the same one-way door to a 0x116, and the
                    // argument of 7.1 - a private watchdog must not undercut the OS's own - does not care
                    // which ring the packet is on. Node 1 keeps the flat deadline: one copy is in flight at
                    // a time, so there is no queue wait to separate (7.3) and no ring of its own to read for
                    // progress (7.2).
                    wddm->PagingDeadline=KeQueryInterruptTime()+10000ull*wddm->SubmitBudgetMs;
                    wddm->PagingDiagnosticTaken=FALSE;
                    due.QuadPart=-10000ll*(LONGLONG)wddm->SubmitBudgetMs;
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
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        if (completed && labSeq != 0 && InterlockedCompareExchange(&labRetiredOnce, 1, 0) == 0) {
            GuardLog("lab: sdma0 first retired paging fence: seq %lu", labSeq);
            GfxLabSdmaReceipt(Device, "first retired", 0);
        }
#endif
        if (failed) WddmFailSubmission(Device,fence,BC250_WDDM_NODE_COPY);
        if (completed) WddmQueueReport(wddm);   // every retirement is reported, the quota's last one included
        next=PagingDrainNext(&retiredCount,completed,PAGING_DRAIN_QUOTA);
        if (next==PagingDrainContinue) continue;
        if (next==PagingDrainYield) {
            WddmRequeuePagingDrain(wddm);
            exitReason=PagingDrainExitQuota;
        } else if (failed) exitReason=PagingDrainExitRefused;
        break;                  // no polling loop while the GPU is executing
    }
    ProgressDrainDone(iterations,retiredCount,(ULONG)exitReason);
    ProgressExit(ProgressSitePagingDrain,(LONG)exitReason);
}

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
// BD-235 (lab flavour only): what the paging SDMA held when the paging deadline passed. In BD-235 the paging fence
// stopped for 12 s and the log had only its number. Read only, at DISPATCH_LEVEL in the paging watchdog DPC,
// outside wddm->Lock, at most once per paging submission: PagingDiagnosticTaken is set when the deadline passes
// and cleared again when the next paging submission goes on the ring (WddmGpuFencePaging). All registers are on the
// READ_REG list, and none of them is banked by GRBM_GFX_INDEX, so no wave capture admission is needed. The reads
// are sequential CPU observations, not one atomic sample of the engine.
//   - The SDMA0 GFX queue carries the paging ring. RB_RPTR/WPTR are the engine's 64-bit byte pointers (the ring
//     offset is the pointer modulo the ring size) and RB_RPTR_FETCH the pointer it has fetched up to; IB_BASE,
//     IB_OFFSET, IB_RPTR and IB_SIZE describe the IB that the engine fetches now, which need not be the stopped
//     submission.
//   - The marker slot is the value of the last fence/poll barrier. A ring stopped on that poll shows a marker that
//     differs from the value the poll waits for.
static void WddmPagingTimeoutSnapshot(_Inout_ BC250_DEVICE* Device, ULONG Seq, UINT Fence)
{
    static const struct { const char* Name; ULONG Offset; } registers[] = {
        { "SDMA0_RB_RPTR", BC250_REG_GC_SDMA0_GFX_RB_RPTR }, { "SDMA0_RB_RPTR_HI", BC250_REG_GC_SDMA0_GFX_RB_RPTR_HI },
        { "SDMA0_RB_WPTR", BC250_REG_GC_SDMA0_GFX_RB_WPTR }, { "SDMA0_RB_WPTR_HI", BC250_REG_GC_SDMA0_GFX_RB_WPTR_HI },
        { "SDMA0_IB_BASE_LO", BC250_REG_GC_SDMA0_GFX_IB_BASE_LO },
        { "SDMA0_IB_BASE_HI", BC250_REG_GC_SDMA0_GFX_IB_BASE_HI },
        { "SDMA0_IB_OFFSET", BC250_REG_GC_SDMA0_GFX_IB_OFFSET }, { "SDMA0_IB_RPTR", BC250_REG_GC_SDMA0_GFX_IB_RPTR },
        { "SDMA0_IB_SIZE", BC250_REG_GC_SDMA0_GFX_IB_SIZE },
        { "SDMA0_CONTEXT_STATUS", BC250_REG_GC_SDMA0_GFX_CONTEXT_STATUS },
        { "SDMA0_STATUS", BC250_REG_GC_SDMA0_STATUS_REG }, { "SDMA0_STATUS1", BC250_REG_GC_SDMA0_STATUS1_REG },
        { "GCVM_FAULT_STATUS", BC250_REG_GC_GCVM_L2_PROTECTION_FAULT_STATUS },
        { "GCVM_FAULT_ADDR_LO32", BC250_REG_GC_GCVM_L2_PROTECTION_FAULT_ADDR_LO32 },
        { "GCVM_FAULT_ADDR_HI32", BC250_REG_GC_GCVM_L2_PROTECTION_FAULT_ADDR_HI32 },
        { "SDMA0_RPTR_FETCH", BC250_REG_GC_SDMA0_RB_RPTR_FETCH },
        { "SDMA0_RPTR_FETCH_HI", BC250_REG_GC_SDMA0_RB_RPTR_FETCH_HI },
        { "SDMA0_RB_CNTL", BC250_REG_GC_SDMA0_GFX_RB_CNTL },
        { "SDMA0_WPTR_POLL_CNTL", BC250_REG_GC_SDMA0_GFX_RB_WPTR_POLL_CNTL },
    };
    // 19 registers: six lines of three and one of one, per timed-out paging submission. A
    // register that MmioRead refuses keeps the value 0 in its line, and its bit is clear in the validity mask
    // (bit n = registers[n] read), so an unread fault latch is never taken for "no fault".
    ULONG values[19];
    ULONG i, valid = 0, status, fenceSlot, markerSlot;
    ULONGLONG page;
    BOOLEAN slots;

    C_ASSERT(RTL_NUMBER_OF(registers) == RTL_NUMBER_OF(values));
    C_ASSERT(RTL_NUMBER_OF(values) < 32);
    for (i = 0; i < RTL_NUMBER_OF(values); i++) {
        if (NT_SUCCESS(MmioRead(Device, registers[i].Offset, &values[i]))) valid |= 1ul << i;
        else values[i] = 0;
    }
    slots = GfxPagingSlots(Device, &fenceSlot, &markerSlot);
    for (i = 0; i + 3 <= RTL_NUMBER_OF(values); i += 3)
        GuardLog("wddm: paging timeout %s 0x%08X %s 0x%08X %s 0x%08X",
                 registers[i].Name, values[i], registers[i + 1].Name, values[i + 1],
                 registers[i + 2].Name, values[i + 2]);
    for (; i < RTL_NUMBER_OF(values); i++)
        GuardLog("wddm: paging timeout %s 0x%08X", registers[i].Name, values[i]);
    GuardLog("wddm: paging timeout validity mask 0x%05lX (bit n = register n read, %lu registers)", valid,
             (ULONG)RTL_NUMBER_OF(values));
    // The fault latch, decoded as in WddmTimeoutSnapshot: ADDR_LO32/HI32 hold the page frame.
    // Decoded only when all three latch registers were read.
    if ((valid & (7ul << 12)) == (7ul << 12)) {
        status = values[12];
        page = (((ULONGLONG)values[14] << 32) | values[13]) << 12;
        GuardLog("wddm: paging timeout fault cid %lu %s vmid %lu rw %lu page 0x%llX",
                 BC250_GCVM_FAULT_CID(status), Bc250GfxhubClientName(BC250_GCVM_FAULT_CID(status)),
                 BC250_GCVM_FAULT_VMID(status), BC250_GCVM_FAULT_RW(status), page);
    } else GuardLog("wddm: paging timeout fault latch not read (validity mask 0x%05lX)", valid);
    if (slots)
        GuardLog("wddm: paging timeout seq %lu fence %u: fence slot 0x%08X, marker slot 0x%08X",
                 Seq, Fence, fenceSlot, markerSlot);
    else GuardLog("wddm: paging timeout seq %lu: fence and marker slots not readable", Seq);
}
#endif

static KDEFERRED_ROUTINE WddmPagingSubmitDpcRoutine;
static KDEFERRED_ROUTINE WddmPagingSubmitDpcCheck;
static void WddmPagingSubmitDpcCheck(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
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
    if (!wddm->Stopping && !wddm->Recovery[BC250_WDDM_NODE_COPY].ResetActive &&
        wddm->PagingHwPending && !wddm->PagingDiagnosticTaken &&
        !wddm->WatchdogFaulted[BC250_WDDM_NODE_COPY] &&
        KeQueryInterruptTime()>=wddm->PagingDeadline)
    {
        timedOut = TRUE;
        fence = wddm->PagingHwFence;
        seq = wddm->PagingHwSeq;
        wddm->PagingDiagnosticTaken = TRUE;
        // Preserve PagingHwPending until a real fence or the OS recovery path.
    }
    KeReleaseSpinLock(&wddm->Lock, irql);
    if (!timedOut) return;
    // As on node 0, elapsed time is not evidence of a failed transfer.
    InterlockedIncrement(&wddm->PagingHwTimeouts);
    GuardLog("wddm: paging diagnostic v2 after %lu ms (sequence %u): fence %u pending, admission unchanged",
             wddm->SubmitBudgetMs, seq, fence);
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
    WddmPagingTimeoutSnapshot(device, seq, fence);   // BD-235: the SDMA state at the deadline
#endif
    // No completion report for a fence that has not arrived.
}

static void WddmPagingSubmitDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    ProgressEnter(ProgressSitePagingWatchdogDpc);
    WddmPagingSubmitDpcCheck(Dpc, Context, Arg1, Arg2);
    ProgressExit(ProgressSitePagingWatchdogDpc, 0);
}

// The quota requeue (WddmRequeuePagingDrain): one more drain invocation, a clock tick after the last one yielded.
static KDEFERRED_ROUTINE WddmPagingDrainDpcRoutine;
static void WddmPagingDrainDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL) return;
    ProgressEnter(ProgressSitePagingDrainDpc);
    WddmGpuFencePaging(device);         // reads Device->Wddm itself: NULL once the stop has detached it
    ProgressExit(ProgressSitePagingDrainDpc, 0);
}

// Accept driver-built work independently of whether the preceding GPU packet
// has retired. Every builder reserves queue storage; submission never allocates.
static BOOLEAN WddmSubmitPagingHardwareRoot(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm,
    const void* PrivateData, ULONG PrivateBytes, ULONGLONG Start,
    ULONG ByteCount, BOOLEAN VirtualAddress, UINT FenceId, ULONGLONG Root)
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
    if(VirtualAddress && !PagingPrivateBindRoot((void*)PrivateData,PrivateBytes,Start,ByteCount,Root)) {
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

static BOOLEAN WddmSubmitPagingHardware(BC250_DEVICE* Device, BC250_WDDM* Wddm,
    const void* PrivateData, ULONG PrivateBytes, ULONGLONG Start,
    ULONG ByteCount, BOOLEAN VirtualAddress, UINT FenceId)
{
    return WddmSubmitPagingHardwareRoot(Device,Wddm,PrivateData,PrivateBytes,Start,
        ByteCount,VirtualAddress,FenceId,0);
}

// Caller owns Lock and has observed no hardware packet or active submit. Windows
// resubmits preempted paging packets with their original fence IDs (Microsoft,
// display/gpu-preemption.md). Release only our borrowed queue links, not command
// payloads or OS storage, before reporting preemption. Never complete these jobs
// or restart them autonomously: the scheduler owns replay and its ordering.
static void WddmReleasePreemptedPagingLocked(BC250_WDDM* Wddm)
{
    while (Wddm->PagingHead) {
        BC250_PAGING_JOB* job=Wddm->PagingHead;
        Wddm->PagingHead=job->Next;
        RtlZeroMemory(job,sizeof(*job));
    }
    Wddm->PagingTail=NULL;
}

// The report side, at DISPATCH_LEVEL, with the DDI long returned. Completion first, then preemption: that order
// is what makes DmaPreempted.LastCompletedFenceId the fence dxgkrnl has just been told about, and it means a
// packet is never reported as completed after it has been declared preempted.
static KDEFERRED_ROUTINE WddmReportDpcRoutine;
static KDEFERRED_ROUTINE WddmReportDpcPublish;
static void WddmReportDpcPublish(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;
    LONG fence;
    KIRQL reportIrql;
    UINT node;
    BOOLEAN reported = FALSE;       // C50: did this pass publish anything that now owes a DxgkCbNotifyDpc
    BOOLEAN pairInPass;             // C50: this pass pairs its own reports, so WddmReport need not queue the DPC

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
    pairInPass = wddm->NotifyDpcInReport;   // read once, so the whole pass has one shape even across a reload
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
        if (wddm->Recovery[node].ResetActive) {
            KeReleaseSpinLock(&wddm->Lock, irql);
            continue;
        }
        complete = wddm->CompletionPending[node] != 0 && Bc250HangReportBegin(&wddm->Recovery[node]);
        fence = wddm->SubmittedFence[node];
        if (complete) wddm->CompletionPending[node] = 0;
        KeReleaseSpinLock(&wddm->Lock, irql);
        if (complete)
        {
            RtlZeroMemory(&data, sizeof(data));
            data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            data.DmaCompleted.SubmissionFenceId = (UINT)fence;
            data.DmaCompleted.NodeOrdinal = node;
            data.DmaCompleted.EngineOrdinal = 0;
            if (WddmReport(device, &data, pairInPass))
            {
                WddmPairingReport(wddm, FALSE);
                reported = TRUE;
                KeAcquireSpinLock(&wddm->Lock, &irql);
                InterlockedExchange(&wddm->LastCompletedFence, fence);
                InterlockedExchange(&wddm->LastReportedFence[node], fence);
                wddm->LastReportedValid[node] = TRUE;
                InterlockedExchange(&wddm->CompletionRetries[node], 0);
                Bc250HangReportEnd(&wddm->Recovery[node], (UINT)fence, TRUE, FALSE);
                KeReleaseSpinLock(&wddm->Lock, irql);
            }
            else
            {
                // The report did not reach dxgkrnl, so the fence it carried is not a fence dxgkrnl was told
                // about. Nothing advances here; the pending flag goes back and this pass asks for another one.
                // Bounded, because the failure modes are "the callbacks are gone" and "SynchronizeExecution
                // keeps failing", and a DPC that requeues itself for ever on either would be worse than a
                // dropped completion with a line in the log.
                BOOLEAN retry;

                KeAcquireSpinLock(&wddm->Lock, &irql);
                retry = (InterlockedIncrement(&wddm->CompletionRetries[node]) <= BC250_WDDM_REPORT_RETRY_MAX);
                if (retry)
                {
                    wddm->CompletionPending[node] = 1;
                    wddm->ReportAgain = TRUE;
                }
                else InterlockedIncrement(&wddm->CompletionsDropped[node]);
                Bc250HangReportEnd(&wddm->Recovery[node], (UINT)fence, FALSE, !retry);
                KeReleaseSpinLock(&wddm->Lock, irql);
                GuardLog("wddm: completion report node %u fence %ld not delivered, %s (retry %ld of %ld)",
                         node, fence, retry ? "pending again" : "DROPPED",
                         wddm->CompletionRetries[node], (LONG)BC250_WDDM_REPORT_RETRY_MAX);
            }
        }

        // DMA-buffer-boundary preemption cannot be acknowledged while that buffer is
        // executing or while its completion has yet to reach dxgkrnl. ActiveSubmissions
        // also covers GfxSubmitIb before it installs HwPending. Completion and submit
        // exit requeue this DPC, so no spinning or timer is needed while we defer.
        KeAcquireSpinLock(&wddm->Lock, &irql);
        if (wddm->Recovery[node].ResetActive) {
            KeReleaseSpinLock(&wddm->Lock, irql);
            continue;
        }
        // SubmitCommandVirtual's invalid-parameter contract: the OS retires a
        // rejected fence after prior work. Update our notion without reporting a
        // successful DMA completion for work that was never submitted.
        if (wddm->RejectedPending[node] && !wddm->RefusalPending[node] &&
            wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&
            !(node == BC250_WDDM_NODE_COPY ? (wddm->PagingHead != NULL) : WddmRingPendingLocked(wddm, node)))
        {
            UINT rejected=wddm->RejectedFence[node];
            if (!wddm->Recovery[node].BoundaryKnown ||
                (LONG)(rejected-wddm->Recovery[node].BoundaryFence) > 0)
            {
                wddm->Recovery[node].BoundaryFence=rejected;
                wddm->Recovery[node].BoundaryKnown=TRUE;
            }
            wddm->RejectedPending[node]=FALSE;
        }
        // A faulted paging engine cannot accept scheduler replay. Keep its queued
        // ownership for recovery even if the last hardware fence arrived late.
        preempt = wddm->PreemptionPending[node] != 0 && !wddm->RefusalPending[node] &&
                  wddm->ActiveSubmissions[node] == 0 && wddm->CompletionPending[node] == 0 &&
                  !(node == BC250_WDDM_NODE_COPY ? wddm->PagingHwPending : WddmRingPendingLocked(wddm, node)) &&
                  !(node == BC250_WDDM_NODE_COPY && wddm->PagingHead && wddm->WatchdogFaulted[node]);
        if (preempt)
        {
            preempt = Bc250HangReportBegin(&wddm->Recovery[node]) != 0;
        }
        if (preempt)
        {
            preemptFence = (UINT)wddm->PreemptionFence[node];
            lastFence = (UINT)wddm->LastReportedFence[node];
            (void)Bc250HangSchedulerBoundary(&wddm->Recovery[node], wddm->LastReportedValid[node],
                                             lastFence, &lastFence);
            if (node==BC250_WDDM_NODE_COPY) WddmReleasePreemptedPagingLocked(wddm);
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
            if (WddmReport(device, &data, pairInPass))
            {
                WddmPairingReport(wddm, FALSE);
                reported = TRUE;
            }
            else
            {
                InterlockedIncrement(&wddm->PreemptionReportsLost);
                GuardLog("wddm: preemption report fence %u node %u NOT delivered (lost %ld); recovery is the"
                         " watchdog's", preemptFence, node, wddm->PreemptionReportsLost);
            }
            KeAcquireSpinLock(&wddm->Lock, &irql);
            // Preemption is publication, but never a packet completion.
            Bc250HangReportEnd(&wddm->Recovery[node], 0, FALSE, FALSE);
            KeReleaseSpinLock(&wddm->Lock, irql);
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
    // C50, behind NotifyDpcInReport: pair what this pass published with the DPC-level notification here, which is
    // what the DDI asks for ("The display miniport driver's DPC callback routine calls DXGKCB_NOTIFY_DPC",
    // d3dkmddi.md:2392; NotifyDpc is a DISPATCH_LEVEL call, :2433, and this is a DPC). Without it the pairing
    // waits for the dxgkrnl DPC that the DxgkCbQueueDpc inside WddmReport brings about: a 12 to 20 us hop, worth
    // about 0.03 ms of a 70 Hz frame at the 187 completions a second of session 418. It is not what holds the ring
    // for 8 ms - notify_pairing.h carries that correction and the evidence for it.
    //
    // Three things make the call safe here. It is outside wddm->Lock, so a scheduler that re-enters a DDI of ours
    // cannot deadlock on it. It is after ReportActive has been cleared, so a report queued from inside the call
    // runs as an ordinary new pass rather than being folded into this one. And the submit path it can reach runs
    // at PASSIVE_LEVEL from SubmitCommandVirtual (node 0) or takes no fast mutex at all (node 1), so a
    // DISPATCH_LEVEL caller here cannot end up acquiring GfxSubmitMutex.
    //
    // WddmReport did not queue the dxgkrnl DPC for these reports, so whenever this pass does not make the call
    // after all - the stop began between the report and here - the DPC is asked for instead. A report that reached
    // dxgkrnl always gets its pairing from somewhere.
    if (reported && pairInPass)
    {
        if (!WddmStopping(wddm)) WddmNotifyDpcNow(device, TRUE);
        else if (device->Dxgk.DxgkCbQueueDpc != NULL) device->Dxgk.DxgkCbQueueDpc(device->Dxgk.DeviceHandle);
    }
}

static void WddmReportDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    ProgressEnter(ProgressSiteReportDpc);
    WddmReportDpcPublish(Dpc, Context, Arg1, Arg2);
    ProgressExit(ProgressSiteReportDpc, 0);
}

// Sample one stable programming generation around the hardware pending test.
// A writer starting during the sample invalidates it; retry at the next vblank,
// never wait here (the writer may be the interrupt that preempted this DPC).
static BOOLEAN WddmReadCompletedPrimary(_In_ BC250_DEVICE* Device, _In_ BC250_WDDM* Wddm,
                                       _Out_ PHYSICAL_ADDRESS* Address, _Out_opt_ ULONG* Sequence)
{
    LONG generation = InterlockedCompareExchange(&Wddm->PrimarySequence, 0, 0);
    LONGLONG address;
    ULONG programmedSequence;
    if (generation & 1) return FALSE;
    address = InterlockedCompareExchange64(&Wddm->PrimaryAddress.QuadPart, 0, 0);
    if (Device->VidPnFlipEnabled && DcnFlipPending(Device, (ULONGLONG)address)) return FALSE;
    programmedSequence=(ULONG)InterlockedCompareExchange(&Wddm->PrimaryProgrammedSequence,0,0);
    if (InterlockedCompareExchange(&Wddm->PrimarySequence, 0, 0) != generation) return FALSE;
    Address->QuadPart = address;
    if (Sequence!=NULL) *Sequence=programmedSequence;
    return TRUE;
}

// BD-170: one CRTC_VSYNC call of ControlInterrupt into the history the summary prints. Evidence only: the slot is
// written without a lock, so a summary that meets a call in progress can print a mixed slot.
static void WddmVSyncControlNote(_Inout_ BC250_WDDM* Wddm, BOOLEAN On)
{
    const LONG call = InterlockedIncrement(&Wddm->VSyncControlCalls);
    BC250_WDDM_VSYNC_CONTROL* slot = &Wddm->VSyncControl[(ULONG)(call - 1) % BC250_WDDM_VSYNC_CONTROL_EVENTS];

    InterlockedIncrement(On ? &Wddm->VSyncControlOn : &Wddm->VSyncControlOff);
    slot->Call = call;
    slot->On = On;
    slot->Qpc = KeQueryPerformanceCounter(NULL).QuadPart;
    slot->Owed = Wddm->DesiredOwed ? InterlockedCompareExchange64(&Wddm->PrimaryAddress.QuadPart, 0, 0) : 0;
}

// ---- the software VSync ------------------------------------------------------------------------------------------

static KDEFERRED_ROUTINE WddmVSyncDpcRoutine;
static KDEFERRED_ROUTINE WddmVSyncDpcTick;
static void WddmVSyncDpcTick(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
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
    InterlockedExchange64(&wddm->RingGapLastVsyncQpc, wddm->VSyncLast.QuadPart);   // C48: the software source
    InterlockedIncrement(&wddm->RingGapVsyncStampsTimer);   // 62.5 Hz, a different grid from the hardware vblank
    InterlockedIncrement(&wddm->VSyncTicks);
    // ControlInterrupt has not asked for CRTC_VSYNC.
    if (!wddm->VSyncEnabled) return;

    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    data.CrtcVsync.VidPnTargetId = wddm->VSyncTargetId;
    if (!WddmReadCompletedPrimary(device, wddm, &data.CrtcVsync.PhysicalAddress, NULL)) return;
    data.CrtcVsync.PhysicalAdapterMask = 0;     // not in a link, so Flags.ValidPhysicalAdapterMask stays 0 too
    InterlockedIncrement(&wddm->VSyncReports);
    // C50: a CRTC_VSYNC report is a DxgkCbNotifyInterrupt call too, and it owes the same pairing. This one is made
    // from the timer's own DPC, with no WddmDpc behind it, so WddmReport queues the dxgkrnl DPC that pairs it.
    if (WddmReport(device, &data, FALSE)) WddmPairingReport(wddm, TRUE);
}

// Progress record around the tick, outside it so that none of its early returns can skip the exit (hang.c).
static void WddmVSyncDpcRoutine(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    ProgressEnter(ProgressSiteVSyncDpc);
    WddmVSyncDpcTick(Dpc, Context, Arg1, Arg2);
    ProgressExit(ProgressSiteVSyncDpc, 0);
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
// WDDM lock orders decisions against stop; DcnVsyncEnable then uses the graphics
// interrupt-synchronization callback to serialize MMIO and armed state with ISR.
// The synchronized callback never takes this WDDM lock (one-way lock order).
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
            changed = NT_SUCCESS(DcnVsyncEnable(Device, On));
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
    // Visibility controls pixels, not timing. Keep generating requested vsyncs
    // while hidden (DXGKARG_SETVIDPNSOURCEVISIBILITY); stop disarms at teardown.
    if (Visible) WddmVSyncArm(Device, TRUE);
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
    ULONG completedSequence;

    if (wddm == NULL || !Device->VidPnFlipEnabled) return;
    if (InterlockedExchange(&Device->DcnVsyncAcked, 0) == 0) return;
    WddmRingGapVsync(wddm);             // C48: the vblank happened, whatever this function decides to report
    if (WddmStopping(wddm)) return;
    if (!wddm->VSyncEnabled) return;

    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
    data.CrtcVsync.VidPnTargetId = wddm->VSyncTargetId;
    // A stable completed request may be retired. Otherwise preserve this vblank
    // by reporting the buffer hardware is still reading, never the queued one.
    if (!WddmReadCompletedPrimary(Device, wddm, &data.CrtcVsync.PhysicalAddress, &completedSequence))
    {
        ULONGLONG scanned;
        LONG generation;
        InterlockedIncrement(&Device->DcnVsyncDeferred); // completion deferred, not necessarily the vblank
        generation = InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0);
        if (generation & 1)
        {
            InterlockedIncrement(&Device->DcnVsyncSkipOddGeneration);
            InterlockedExchange64(&Device->DcnVsyncSkipOddGenerationTime, (LONG64)KeQueryInterruptTime());
            return;
        }
        if (!NT_SUCCESS(DcnReadScanoutAddress(Device, &scanned)))
        {
            InterlockedIncrement(&Device->DcnVsyncSkipReadFailure);
            InterlockedExchange64(&Device->DcnVsyncSkipReadFailureTime, (LONG64)KeQueryInterruptTime());
            return;
        }
        // If the pending bit outlives the address latch, reporting the requested
        // address would still retire the flip early. Only the distinct previous
        // buffer is safe here; a matching address needs the completed path above.
        if (scanned == (ULONGLONG)InterlockedCompareExchange64(&wddm->PrimaryAddress.QuadPart, 0, 0))
        {
            InterlockedIncrement(&Device->DcnVsyncSkipSameAddress);
            InterlockedExchange64(&Device->DcnVsyncSkipSameAddressTime, (LONG64)KeQueryInterruptTime());
            return;
        }
        if (InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0) != generation)
        {
            InterlockedIncrement(&Device->DcnVsyncSkipChangedGeneration);
            InterlockedExchange64(&Device->DcnVsyncSkipChangedGenerationTime, (LONG64)KeQueryInterruptTime());
            return;
        }
        data.CrtcVsync.PhysicalAddress.QuadPart = (LONGLONG)scanned;
        InterlockedIncrement(&Device->DcnVsyncOldBufferReports);
    } else {
        StartHealthCompleted(Device,completedSequence);
    }
    data.CrtcVsync.PhysicalAdapterMask = 0;
    InterlockedIncrement(&wddm->VSyncReports);
    // C50: counted like any other report. Bc250DpcRoutine calls WddmDpc right after this, so in practice the
    // pairing happens in this very pass; PairInThisPass stays FALSE because that ordering belongs to pnp.c, not
    // here, and one extra dxgkrnl DPC a vblank is 62 a second.
    if (WddmReport(Device, &data, FALSE)) WddmPairingReport(wddm, TRUE);
}

// ---- the summary -------------------------------------------------------------------------------------------------

// Everything stage A counted, written into the log ring as ordinary lines so that bc250kmd_cli log carries it off
// the headless machine in one go. Called at the stop, and on demand through BC250_ESCAPE_LOG_SUMMARY - a run that
// ends in a frozen desktop never reaches a stop, and the counters are exactly what the experiment is run for.
//
// Read without the lock: every counter here is interlocked or write-once, the numbers are evidence rather than
// control flow, and a summary that took the lock could not be asked for from a DPC-level path later.
// BD-060: standard allocations by kind, GDI surfaces by type, CreateAllocation calls by outcome (gdi_admission.h).
// Rows with nothing in them are left out; the head, call and mask lines are always there, so a missing row means
// zero. Worst case 148 of the 160 bytes of a line.
static void WddmStdAllocSummary(_In_ BC250_WDDM* Wddm)
{
    static const char* const kinds[BC250_STDALLOC_KINDS] = {
        "other", "primary", "shadow", "staging", "gdi", "vgpu", "fence" };
    const BC250_STDALLOC_COUNTERS* c = &Wddm->StdAlloc;
    BC250_GDI_MASKS m;
    ULONG i;

    GuardLog("wddm summary: ---- standard allocations; refused flags/type/geom/buf/input ----");
    for (i = 0; i < BC250_STDALLOC_KINDS; i++)
        if (c->Requests[i][0] || c->Requests[i][1])
            GuardLog("wddm summary: stdalloc %s size/fill %ld/%ld ok %ld refused %ld/%ld/%ld/%ld/%ld", kinds[i],
                     c->Requests[i][0], c->Requests[i][1], c->Answers[i][BC250_STDALLOC_OK],
                     c->Answers[i][BC250_STDALLOC_FLAGS], c->Answers[i][BC250_STDALLOC_TYPE],
                     c->Answers[i][BC250_STDALLOC_GEOMETRY], c->Answers[i][BC250_STDALLOC_BUFFER],
                     c->Answers[i][BC250_STDALLOC_INPUT]);
    for (i = 0; i < BC250_GDI_SLOTS; i++) {
        if (c->GdiRequests[i][0] || c->GdiRequests[i][1])
            GuardLog("wddm summary: gdi type %lu size/fill %ld/%ld ok %ld refused %ld/%ld/%ld/%ld/%ld", i,
                     c->GdiRequests[i][0], c->GdiRequests[i][1], c->GdiAnswers[i][BC250_STDALLOC_OK],
                     c->GdiAnswers[i][BC250_STDALLOC_FLAGS], c->GdiAnswers[i][BC250_STDALLOC_TYPE],
                     c->GdiAnswers[i][BC250_STDALLOC_GEOMETRY], c->GdiAnswers[i][BC250_STDALLOC_BUFFER],
                     c->GdiAnswers[i][BC250_STDALLOC_INPUT]);
        if (c->GdiCreated[i][0] || c->GdiCreated[i][1] || c->GdiCreated[i][2] || c->GdiOpened[i][0] || c->GdiOpened[i][1])
            GuardLog("wddm summary: lb7a type %lu create ok/refused/rolled-back %ld/%ld/%ld open ok/refused %ld/%ld", i,
                     c->GdiCreated[i][BC250_CREATE_CREATED], c->GdiCreated[i][BC250_CREATE_REFUSED],
                     c->GdiCreated[i][BC250_CREATE_ROLLED_BACK], c->GdiOpened[i][0], c->GdiOpened[i][1]);
    }
    GuardLog("wddm summary: CreateAllocation calls ok %ld resource-data %ld invalid %ld no-memory %ld",
             c->CreateCalls[BC250_CREATE_OK], c->CreateCalls[BC250_CREATE_RESOURCE_DATA],
             c->CreateCalls[BC250_CREATE_INVALID], c->CreateCalls[BC250_CREATE_NO_MEMORY]);
    // One line a deploy-kit gate can assert on after DWM started: refused standard GDI types == 0x000. A zero
    // create mask says nothing about calls that failed before any allocation: read the calls line beside it.
    Bc250GdiMasks(c, &m);
    GuardLog("wddm summary: gdi type masks requested 0x%03lX refused 0x%03lX create-refused 0x%03lX "
             "rolled-back 0x%03lX open-refused 0x%03lX", m.Requested, m.Refused, m.CreateRefused,
             m.CreateRolledBack, m.OpenRefused);
}

// BD-170 r6: the W-u records, copied in one section (the flip state machine's block below).
static BOOLEAN WddmWuCopy(_In_ BC250_DEVICE* Device, _In_ BC250_WDDM* Wddm, _Out_ BC250_WDDM_WU_COPY* Copy);
// BD-195: the name of a writer refusal's site, in the order of the BC250_WRITER_REFUSED_* values of the same block.
// It is defined here, outside the block, because only log lines use it.
static const char* WddmWriterRefusedSite(ULONG Site)
{
    static const char* const sites[] = { "irql", "section", "bridge", "faulting", "retry", "retire" };
    return Site < RTL_NUMBER_OF(sites) ? sites[Site] : "none";
}
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
static BOOLEAN WddmWitnessCopy(_In_ BC250_DEVICE* Device, _In_ BC250_WDDM* Wddm, _Out_ BC250_WDDM_WITNESS_COPY* Copy);
#endif

// BD-170: a whole copy of one immediate-flip record under its sequence lock (bc250kmd.h): 1 copied, 0 never
// written, 2 the slot kept changing for BC250_DCN_IMMEDIATE_READS tries. Never a mixture of two flips.
static ULONG WddmImmediateRecordCopy(_In_ const BC250_DEVICE* Device, ULONG Slot, _Out_ BC250_DCN_IMMEDIATE_RECORD* Copy)
{
    const BC250_DCN_IMMEDIATE_RECORD* slot = &Device->DcnImmediateRecords[Slot % BC250_DCN_IMMEDIATE_RECORDS];
    ULONG tries;
    LONG before;

    RtlZeroMemory(Copy, sizeof(*Copy));
    for (tries = 0; tries < BC250_DCN_IMMEDIATE_READS; tries++)
    {
        if (tries) KeStallExecutionProcessor(1);    // a writer holds a slot for the copy of one record only
        before = InterlockedCompareExchange((volatile LONG*)&slot->Version, 0, 0);
        if (before == 0) return 0;
        if (before & 1) continue;
        RtlCopyMemory(Copy, (const void*)slot, sizeof(*Copy));
        if (InterlockedCompareExchange((volatile LONG*)&slot->Version, 0, 0) == before) return 1;
    }
    RtlZeroMemory(Copy, sizeof(*Copy));
    return 2;
}

static const char* WddmImmediateOutcomeText(ULONG Outcome)
{
    static const char* const text[BC250_DCN_IMMEDIATE_OUTCOMES] = {
        "unknown", "done", "done-late", "pending", "primary-other", "read-failed", "superseded" };
    return Outcome < BC250_DCN_IMMEDIATE_OUTCOMES ? text[Outcome] : "unknown";
}

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

    // Two lines since 0.7.208 (BD-070): the one line this was is 174 characters at its widest, 15
    // more than a log line holds, and the count that fell off the end was the live one. The
    // allocation pair, the one that grows into six digits, has a line to itself, and it goes
    // first on purpose: both lines carry the same name, so a reader that keeps the last line of
    // that name - and several of our own scripts do - still ends up with the pairs and the live
    // count it always had. runcompare reads the two lines as one block, the allocation line
    // first, and stops at the line that ends in "alive".
    //
    // WddmSummary also runs while the adapter runs (the overlay polls it), so the nine counters
    // go into locals first. Read once per GuardLog call they would be two samples, and a reader
    // could no longer add the pairs up against the live count inside one report.
    {
        LONG devMade = Wddm->Calls[WddmDdiCreateDevice], devGone = Wddm->Calls[WddmDdiDestroyDevice];
        LONG ctxMade = Wddm->Calls[WddmDdiCreateContext], ctxGone = Wddm->Calls[WddmDdiDestroyContext];
        LONG procMade = Wddm->Calls[WddmDdiCreateProcess], procGone = Wddm->Calls[WddmDdiDestroyProcess];
        LONG allocMade = Wddm->Calls[WddmDdiCreateAllocation], allocGone = Wddm->Calls[WddmDdiDestroyAllocation];
        LONG live = Wddm->ObjectCount;

        GuardLog("wddm summary: objects created/destroyed: alloc %ld/%ld", allocMade, allocGone);
        GuardLog("wddm summary: objects created/destroyed: dev %ld/%ld ctx %ld/%ld proc %ld/%ld, %ld alive",
                 devMade, devGone, ctxMade, ctxGone, procMade, procGone, live);
    }
    {
        // The three move together under the lock; read apart, a concurrent create would look like a mismatch.
        ULONG indexed, misses;
        LONG alive;
        KIRQL irql;

        KeAcquireSpinLock(&Wddm->Lock, &irql);
        indexed = Wddm->ObjectIndex.Count;
        misses = Wddm->ObjectIndex.Misses;
        alive = Wddm->ObjectCount;
        KeReleaseSpinLock(&Wddm->Lock, irql);
        if (misses != 0 || indexed != (ULONG)alive)
            GuardLog("wddm summary: OBJECT INDEX %lu indexed for %ld alive, %lu removals of unindexed objects",
                     indexed, alive, misses);
    }
    GuardLog("wddm summary: allocations opened/closed: %ld/%ld calls", Wddm->Calls[WddmDdiOpenAllocation], Wddm->Calls[WddmDdiCloseAllocation]);
    WddmStdAllocSummary(Wddm);
    GuardLog("wddm summary: submissions %ld physical + %ld virtual, %ld preemptions, last completed fence %ld",
             Wddm->Calls[WddmDdiSubmitCommand], Wddm->Calls[WddmDdiSubmitCommandVirtual],
             Wddm->Calls[WddmDdiPreemptCommand], Wddm->LastCompletedFence);
    GuardLog("wddm summary: node 0 hardware: %ld submitted, %ld completed, %ld timeouts, %ld refused, %ld soft-recovered",
             Wddm->HwSubmitted, Wddm->HwCompleted, Wddm->HwTimeouts, Wddm->HwRefused, Wddm->SoftRecoveries);
    // Timer rearming is not activity. Head age is sampled software residence, not
    // a retirement timestamp or GPU execution duration.
    GuardLog("wddm summary: submit diagnostic v2 %lu ms (OS TdrDelay unavailable), %ld checks, %ld re-armed",
             Wddm->SubmitBudgetMs, Wddm->SubmitChecks, Wddm->SubmitRearms);
    GuardLog("wddm summary: submit watchdog observations: activity %ld, primes %ld, gap resets %ld",
             Wddm->SubmitActivityChanges, Wddm->SubmitPrimes, Wddm->SubmitGapResets);
    GuardLog("wddm summary: sampled software-head high water %ld ms, queue wait %ld ms", Wddm->SubmitHeadMaxMs,
             Wddm->SubmitQueueMaxMs);
    GuardLog("wddm summary: umd: %ld allocs (%ld refused), %ld contexts, %ld submits on the ring, %ld not run",
             Wddm->UmdAllocs, Wddm->UmdAllocRefused, Wddm->UmdContexts, Wddm->UmdSubmitHw, Wddm->UmdSubmitSoft);
    // A line of its own, so that the line above keeps the text its readers parse.
    GuardLog("wddm summary: umd placement (shared residency %s): local %ld, local+aperture %ld, aperture %ld",
             Wddm->SharedResidency ? "on" : "off", Wddm->UmdAllocsLocalOnly, Wddm->UmdAllocsShared,
             Wddm->UmdAllocsAperture);
    // BD-210, a line of its own for the same reason.
    GuardLog("wddm summary: umd cpu view: gpu-only %ld (%lld MiB), cpu-visible %ld (%lld MiB)",
             Wddm->UmdAllocsGpuOnly, (long long)(Wddm->UmdGpuOnlyBytes >> 20),
             Wddm->UmdAllocs - Wddm->UmdAllocsGpuOnly, (long long)(Wddm->UmdCpuVisibleBytes >> 20));
    GuardLog("wddm summary: lb7a cpu view: gpu-only %ld (%lld MiB), cpu-visible %ld (%lld MiB), private %ld",
             Wddm->Lb7aAllocsGpuOnly, (long long)(Wddm->Lb7aGpuOnlyBytes >> 20),
             Wddm->Lb7aAllocs-Wddm->Lb7aAllocsGpuOnly, (long long)(Wddm->Lb7aCpuVisibleBytes >> 20),
             Wddm->Lb7aNoCpuAccess);
    // ADR 0008 stage D (docs/design/paging-node.md section 7): node 1 exists in this line whether or not the
    // gate is open - every counter stays 0 with it closed, same as every other stage-behind-a-gate counter here.
    GuardLog("wddm profile: umd calls %ld, elapsed ticks %lld, QPC frequency %lld",
             Wddm->UmdProfileCalls, Wddm->UmdSubmitTicks, Wddm->UmdProfileFrequency.QuadPart);
    GuardLog("wddm profile: probe enabled %u, calls %ld, elapsed ticks %lld (included in umd)",
             Wddm->TraceUmdProbes, Wddm->UmdProbeCalls, Wddm->UmdProbeTicks);
    // KMD196. Mean held time is Us/holds. The sleep this replaced measured 4700 us a hold over sessions 313/314;
    // the spin-then-event wait should land near 130 us (what the blocking job still had to run) plus the ~550 us
    // a plain submit costs. SpinOnly/holds is the share the bounded spin resolved with no wait at all. Timeout
    // wakes near zero means the end-of-pipe interrupt is doing the waking; a large share means it is being
    // missed and the fallback is carrying the path, which is a correctness question, not a performance one.
    GuardLog("wddm profile: holds %ld (spin-only %ld), held %lld us worst %lld, %lld spins",
             Wddm->SubmitHolds, Wddm->SubmitHoldSpinOnly, Wddm->SubmitHeldUs, Wddm->SubmitHeldMaxUs,
             Wddm->SubmitHoldSpins);
    GuardLog("wddm profile: hold wakes %lld event, %lld timeout",
             Wddm->SubmitHoldEventWakes, Wddm->SubmitHoldTimeoutWakes);
    // The histogram, one line. Nine counts and nine names, so a reader needs neither this file nor the edges.
    GuardLog("wddm profile: held us %s:%ld %s:%ld %s:%ld %s:%ld %s:%ld %s:%ld %s:%ld %s:%ld %s:%ld",
             g_WddmHoldBucketNames[0], Wddm->SubmitHeldHistogram[0],
             g_WddmHoldBucketNames[1], Wddm->SubmitHeldHistogram[1],
             g_WddmHoldBucketNames[2], Wddm->SubmitHeldHistogram[2],
             g_WddmHoldBucketNames[3], Wddm->SubmitHeldHistogram[3],
             g_WddmHoldBucketNames[4], Wddm->SubmitHeldHistogram[4],
             g_WddmHoldBucketNames[5], Wddm->SubmitHeldHistogram[5],
             g_WddmHoldBucketNames[6], Wddm->SubmitHeldHistogram[6],
             g_WddmHoldBucketNames[7], Wddm->SubmitHeldHistogram[7],
             g_WddmHoldBucketNames[8], Wddm->SubmitHeldHistogram[8]);
    // C48/C49, the number the project's goal is stated in: how long each ring stood idle, and how much of that
    // idle ended at a display VSync. The trace this instrument was built from (RotTR 418-420) counted 682 gaps
    // >= 4 ms per 105 s on node 0, 418 of them VSync-ended, 0.45 ms a frame - near it, NOT the same object:
    // ring_gap.h says why (the driver cannot see a packet dxgkrnl holds queued, so this is a superset of that
    // class mixed with application-starved idle, and its edges sit one DPC later). Every counter here is
    // cumulative since WddmStart, so an A/B is the difference of two reads - except "ns/frame", which is a ratio
    // of two cumulative counters and must be recomputed from the two vsync-ended/VSyncReports pairs, never
    // subtracted. "lost" must stay 0: it counts a close with no open gap and a counter that went backwards,
    // either of which would make the rest of the line fiction.
    {
        UINT gapNode;
        for (gapNode = 0; gapNode < BC250_WDDM_NODE_COUNT_MAX; gapNode++)
        {
            const BC250_RING_GAP* gap = &Wddm->RingGap[gapNode];
            if (gap->Gaps == 0u && gap->Lost == 0u) continue;        // a node that never ran says nothing
            // Four lines, not one: BC250_LOG_TEXT is 160 bytes and RtlStringCchVPrintfA truncates without a
            // word, which is how 0.7.207.1 lost every scan-out refusal count (BD-070). The bucket edges are
            // written into the format instead of passed as %s, so the guardlog-width gate sees the real worst
            // case of each line (149, 155, 114, 99 characters) rather than 32 characters a name.
            GuardLog("wddm profile: node %u ring idle %llu us in %lu gaps, worst %llu us, lost %lu, %lu vsyncs",
                     gapNode, gap->TotalUs, gap->Gaps, gap->MaxUs, gap->Lost, (ULONG)Wddm->VSyncReports);
            GuardLog("wddm profile: node %u ring >=4ms %lu/%llu us, vsync-ended %lu/%llu us, %llu ns/frame",
                     gapNode, gap->LongGaps, gap->LongUs, gap->VsyncEndedGaps, gap->VsyncEndedUs,
                     Bc250RingGapPerFrameNs(gap->VsyncEndedUs, (ULONG)Wddm->VSyncReports));
            GuardLog("wddm profile: node %u ring gap us <16:%lu 16:%lu 32:%lu 64:%lu 128:%lu",
                     gapNode, gap->Histogram[0], gap->Histogram[1], gap->Histogram[2], gap->Histogram[3],
                     gap->Histogram[4]);
            GuardLog("wddm profile: node %u ring gap us 512:%lu 2k:%lu 4k:%lu 8k+:%lu",
                     gapNode, gap->Histogram[5], gap->Histogram[6], gap->Histogram[7], gap->Histogram[8]);
        }
    }
    // Which phase grid the vsync-ended class was measured against, and whether any stamp was taken at all: a
    // vsync-ended count of 0 with no stamp means the instrument saw no vblank, not that no gap ended at one. The
    // hardware vblank and the 16 ms software timer are different grids, so a baseline and a window that do not
    // agree here cannot be read against each other.
    GuardLog("wddm profile: ring gap vblank stamps: %ld hardware, %ld software timer",
             Wddm->RingGapVsyncStampsHw, Wddm->RingGapVsyncStampsTimer);
    // C50: where the completion report's DPC-level notification came from. "same pass" is the contract's shape
    // and is what the gate produces; "deferred" is a report that waited for the next dxgkrnl DPC. "unpaired"
    // must be 0 or 1 at a summary taken mid-run and 0 at the stop; anything larger means reports are piling up
    // without a notification, which is a correctness question, not a latency one.
    GuardLog("wddm profile: notify pairing gate %u, %lu reports: %lu same pass, %lu deferred",
             Wddm->NotifyDpcInReport, Wddm->NotifyPairing.Reports, Wddm->NotifyPairing.SamePass,
             Wddm->NotifyPairing.Deferred);
    GuardLog("wddm profile: notify pairing %lu waiting now (worst %lu), %lu notifications carried no report",
             Wddm->NotifyPairing.Unpaired, Wddm->NotifyPairing.MaxUnpaired, Wddm->NotifyPairing.IdleNotifies);
    // The flip path's own reports, on their own counters: a notification that carries one of these is not idle,
    // and "waiting" above is about the completion path alone. "contended" counts the notifications that found
    // another processor inside DxgkCbNotifyDpc and asked dxgkrnl for its DPC instead - the gate-closed shape, so
    // none of them is a lost pairing. A number that is not small says the two DPCs are racing often.
    GuardLog("wddm profile: notify pairing %lu vsync reports (%lu waiting, worst %lu), %lu contended",
             Wddm->NotifyPairing.VsyncReports, Wddm->NotifyPairing.VsyncPending,
             Wddm->NotifyPairing.MaxVsyncPending, Wddm->NotifyPairing.Contended);
    // Undelivered reports. Every number here should be 0: a dropped completion is a fence dxgkrnl was never told
    // about, which ends as a scheduler timeout, and a lost preemption ack ends the same way. They are printed
    // even at 0 so that a TDR in the trail can be read against them instead of being guessed at.
    GuardLog("wddm profile: reports not delivered: node 0 retried %ld dropped %ld",
             Wddm->CompletionRetries[BC250_WDDM_NODE_3D], Wddm->CompletionsDropped[BC250_WDDM_NODE_3D]);
    GuardLog("wddm profile: reports not delivered: node 1 retried %ld dropped %ld, %ld preemption acks lost",
             Wddm->CompletionRetries[BC250_WDDM_NODE_COPY], Wddm->CompletionsDropped[BC250_WDDM_NODE_COPY],
             Wddm->PreemptionReportsLost);
    // D5: a quiet log must never be read as a quiet ring, so the lines HotSubmitLog left out are counted here.
    GuardLog("wddm profile: hot submit log lines left out: %lu (HotSubmitLog off)",
             GfxHotSubmitLinesSkipped(Wddm->Device));
    // BD-097: and the same for the paging submit line, which is rate limited rather than gated (log_rate.h).
    {
        ULONG submits = 0, skipped = 0, summaries = 0;
        GfxPagingLogCounts(Wddm->Device, &submits, &skipped, &summaries);
        GuardLog("wddm profile: paging submit lines: %lu submits, %lu with no line, %lu summary lines",
                 submits, skipped, summaries);
    }
    // KMD214: the VMID pool. RuleRefusals must be 0: a refusal is a root change of a VMID with a live job that
    // the rule check caught. Busy is the old wait, now only when no VMID of the pool is free.
    {
        BC250_GFX_VMID_COUNTERS vmid;
        GfxVmidCounters(Wddm->Device, &vmid);
        GuardLog("wddm summary: VMID pool %s, members 0x%04lX, excluded 0x%04lX", vmid.Gate ? "on" : "off",
                 (ULONG)vmid.Members, (ULONG)vmid.Excluded);
        GuardLog("wddm summary: VMID pool: %lu claims, %lu reuses, %lu busy, %lu rule refusals", vmid.Claims,
                 vmid.Reuses, vmid.Busy, vmid.RuleRefusals);
        GuardLog("wddm summary: VMID pool FLUSH_TLB: %lu built, %lu VMID invalidations", vmid.Flushes,
                 vmid.FlushVmids);
        // Option (b): the root write and the invalidation as packets in front of the frame. With the gate off
        // both numbers are 0 and the flush was MMIO ahead of every job, as before.
        GuardLog("wddm summary: ring VM flush %s: %lu frames, %lu of them on a root the VMID held",
                 vmid.RingFlushGate ? "on" : "off", vmid.RingFlushes, vmid.RingFlushSame);
    }
    GuardLog("wddm summary: node 1 (paging, %s): %ld hardware submitted, %ld completed, %ld timeouts, %ld refused",
             Wddm->NodeCount > BC250_WDDM_NODE_COPY ? "open" : "closed", Wddm->PagingHwSubmitted,
             Wddm->PagingHwCompleted, Wddm->PagingHwTimeouts, Wddm->PagingHwRefused);
    GuardLog("wddm summary: node 2 (compute, %s): %ld hardware submitted, %ld completed, %ld refused, %ld polls",
             Wddm->NodeCount > BC250_WDDM_NODE_COMPUTE ? "open" : "closed", Wddm->ComputeHwSubmitted,
             Wddm->ComputeHwCompleted, Wddm->ComputeHwRefused, Wddm->ComputePolls);
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
    GuardLog("wddm: native PTE copies gate%u ranges%lld",Wddm->NativePteCopies,Wddm->PagingNativePtes);
    GuardLog("wddm summary: native DMA exact disjoint checks %lld",Wddm->PagingDmaGapProofs);
    GuardLog("wddm summary: DMA mapping matches %ld/%ld last VA0x%llX root0x%llX PA0x%llX CPU_PA0x%llX",
             Wddm->PagingDmaVaMatched,Wddm->PagingDmaVaObserved,Wddm->PagingDmaLastVa,
             Wddm->PagingDmaLastRoot,Wddm->PagingDmaLastPa,Wddm->PagingDmaLastCpu);
    GuardLog("wddm summary: BuildPagingBuffer: %ld transfers, %ld fills, %lld bytes, %ld insufficient-buffer",
             Wddm->PagingTransfersBuilt, Wddm->PagingFillsBuilt, Wddm->PagingBytesMoved, Wddm->PagingInsufficientBuffer);
    GuardLog("wddm summary: BuildPagingBuffer: %ld statuses outside the DDI (last op%ld 0x%08lX), %ld inner failures",
             Wddm->PagingContractRefusals, Wddm->PagingContractLastOperation,
             (ULONG)Wddm->PagingContractLastStatus, Wddm->PagingInnerFailures);
    // BC250_LOG_TEXT is 160 bytes: count/bytes pairs keep realistic values (6-digit counts, 11-digit bytes) in one line.
    GuardLog("wddm summary: virtual transfers (count/bytes) to system %ld/%lld, from system %ld/%lld, other %ld/%lld",
             Wddm->PagingXferCount[BC250WddmXferVirtualToSystem], Wddm->PagingXferBytes[BC250WddmXferVirtualToSystem],
             Wddm->PagingXferCount[BC250WddmXferVirtualFromSystem], Wddm->PagingXferBytes[BC250WddmXferVirtualFromSystem],
             Wddm->PagingXferCount[BC250WddmXferVirtualOther], Wddm->PagingXferBytes[BC250WddmXferVirtualOther]);
    GuardLog("wddm summary: physical transfers (count/bytes) to system %ld/%lld, from system %ld/%lld, other %ld/%lld",
             Wddm->PagingXferCount[BC250WddmXferPhysicalToSystem], Wddm->PagingXferBytes[BC250WddmXferPhysicalToSystem],
             Wddm->PagingXferCount[BC250WddmXferPhysicalFromSystem], Wddm->PagingXferBytes[BC250WddmXferPhysicalFromSystem],
             Wddm->PagingXferCount[BC250WddmXferPhysicalOther], Wddm->PagingXferBytes[BC250WddmXferPhysicalOther]);
    GuardLog("wddm summary: aperture map batches %ld, unmap batches %ld",Wddm->PagingMapsBuilt,Wddm->PagingUnmapsBuilt);
    GuardLog("wddm summary: paging fills %ld/%lld bytes, aperture pages mapped %lld unmapped %lld",
             Wddm->PagingFillsBuilt,Wddm->PagingFillBytes,Wddm->PagingMapPages,Wddm->PagingUnmapPages);
    GuardLog("wddm summary: paging TLB invalidations %ld, PTE update batches %ld",
             Wddm->PagingFlushesBuilt,Wddm->PagingUpdatesBuilt);
    GuardLog("wddm summary: paging unsupported (not ready/no root/no translation/system memory/not contiguous) %ld/%ld/%ld/%ld/%ld",
             Wddm->PagingUnsupported[BC250PagingNotReady], Wddm->PagingUnsupported[BC250PagingNoRoot],
             Wddm->PagingUnsupported[BC250PagingNoTranslation], Wddm->PagingUnsupported[BC250PagingSystemMemory],
             Wddm->PagingUnsupported[BC250PagingNotContiguous]);
    GuardLog("wddm summary: presents %ld, flips %ld of %ld address calls (%ld arrived above DISPATCH_LEVEL)",
             Wddm->Calls[WddmDdiPresent], Wddm->Flips, Wddm->Calls[WddmDdiSetVidPnSourceAddress],
             Wddm->FlipsAboveDispatch);
    // Two lines since 0.7.208, because one line did not fit: on the lab the single line ended in
    // "format/geometry/pitch/size/segment/alignment/gated 0/0/" and every refusal count was gone
    // (BD-070). The first line keeps the text the scan-out trial parses, word for word. The second
    // line carries the refusals and is 158 of the 159 characters a log line holds, so two reason
    // names are short, "geom" and "align". scanout_admit.h gives all ten status names in full and
    // in the order of this line. docs/design/scanout-admission.md explains the rules, in the order
    // the admission function applies them, which is not this one: it tests alignment before
    // segment.
    //
    // WddmSummary also runs while the adapter runs (the overlay polls it), so the twelve counters
    // go into locals first. Read once per GuardLog call they would be two samples, and the
    // requested count would no longer have to equal the ten admission counts inside one report.
    {
        LONG admit[BC250_SCANOUT_STATUSES];
        LONG flips = Wddm->ScanoutFlips, requests = Wddm->ScanoutRequests;
        ULONG status;

        for (status = 0; status < BC250_SCANOUT_STATUSES; status++)
            admit[status] = Wddm->ScanoutAdmits[status];
        GuardLog("wddm summary: scan-out flips %ld of %ld requested candidates; admission ok/no-alloc/not-requested %ld/%ld/%ld",
                 flips, requests, admit[BC250_SCANOUT_ADMIT_OK], admit[BC250_SCANOUT_NO_ALLOCATION],
                 admit[BC250_SCANOUT_NOT_REQUESTED]);
        GuardLog("wddm summary: scan-out refusals format/geom/pitch/size/segment/align/gated %ld/%ld/%ld/%ld/%ld/%ld/%ld",
                 admit[BC250_SCANOUT_FORMAT], admit[BC250_SCANOUT_GEOMETRY], admit[BC250_SCANOUT_PITCH],
                 admit[BC250_SCANOUT_SIZE], admit[BC250_SCANOUT_SEGMENT], admit[BC250_SCANOUT_ALIGNMENT],
                 admit[BC250_SCANOUT_GATED]);
    }
    // M15.14 increment 2, the create-time and flip-mode witnesses. They are in the summary and not only in
    // the start-time guard-log lines because the per-create lines have a lifetime budget of
    // BC250_WDDM_LOG_CALLS: the compositor's own primaries spend it at boot, so a client started minutes
    // later writes none and "no line" cannot be told from "budget spent" or from "no such create". A
    // counter the trial cannot read is a counter the trial does not have - that was the unreadable verdict
    // of the first increment - so these are printed where `bc250kmd_cli log summary` returns them.
    //   Three lines, by BD-070's rule: what records arrived, what they were, and what the OS asked for.
    // Each record kind is printed as asked/not-asked, in BC250_WDDM_RECORD_* order.
    {
        LONG kind[BC250_WDDM_RECORD_KINDS][2];
        LONG flipFlags[4];
        ULONG slot;

        for (slot = 0; slot < BC250_WDDM_RECORD_KINDS; slot++) {
            kind[slot][0] = Wddm->ScanoutCreates[slot][0];
            kind[slot][1] = Wddm->ScanoutCreates[slot][1];
        }
        for (slot = 0; slot < 4; slot++) flipFlags[slot] = Wddm->ScanoutFlipFlags[slot];
        GuardLog("wddm summary: type0 creates asked/not by record none %ld/%ld v1 %ld/%ld v2 %ld/%ld v3 %ld/%ld",
                 kind[0][0], kind[0][1], kind[1][0], kind[1][1], kind[2][0], kind[2][1],
                 kind[3][0], kind[3][1]);
        GuardLog("wddm summary: type0 creates other %ld/%ld, PRIMARY records %ld, in a resource group %ld",
                 kind[BC250_WDDM_RECORD_OTHER][0], kind[BC250_WDDM_RECORD_OTHER][1],
                 Wddm->ScanoutCreatePrimaries, Wddm->ScanoutCreateResources);
        GuardLog("wddm summary: flip flags mode/immediate/shared-transition/independent %ld/%ld/%ld/%ld, redirected presents %ld",
                 flipFlags[0], flipFlags[1], flipFlags[2], flipFlags[3], Wddm->RedirectedPresents);
        // M15.14 (0.7.216.20): the plane format half. "plane formats 1" is this start's DcnPlaneFormats (the
        // firmware's format decoded), which is also what the caps trailer publishes as PLANE_FORMATS. The flip
        // counts are by plane format (plane_format.h): every flip the hardware was written with, the
        // compositor's own as well as a client's.
        GuardLog("wddm summary: plane formats %lu, flips argb8888/abgr8888/abgr2101010 %ld/%ld/%ld, changes %ld refused %ld",
                 (ULONG)(Wddm->Device->DcnPlaneFormats ? 1 : 0),
                 Wddm->ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ARGB8888],
                 Wddm->ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ABGR8888],
                 Wddm->ScanoutFlipsByFormat[BC250_PLANE_FORMAT_ABGR2101010],
                 Wddm->Device->DcnFormatChanges, Wddm->Device->DcnFormatRefused);
    }
    // The published answer of this start, in the summary and not only in the start-time line: the log ring
    // holds minutes, and a trial that reads the counters an hour after boot would otherwise have to guess
    // whether the compositor was ever offered the flip at all. Without this, "no candidate reached the
    // driver" cannot be told from "nobody was asked", which is the one distinction the whole handshake is
    // about. It is a state, so it is printed as a word, not as a count.
    GuardLog("wddm summary: DirectFlip handshake %s", Wddm->DirectFlipHandshake ? "on" : "off");
    // Cumulative counters are not bounded by the detailed-log budget. Read
    // closure after quiescence; individual atomic reads are not one snapshot.
    GuardLog("wddm: CDD interop%u GPU Present gate%u identity probe%u",
        Wddm->CddDwmInterop,Wddm->GpuPresentGate,Wddm->HandleIdentityProbe);
    GuardLog("wddm: DRIVERCAPS returned interop0 %lld interop1 %lld",
        InterlockedCompareExchange64(&Wddm->DriverCapsInteropReturned[0],0,0),
        InterlockedCompareExchange64(&Wddm->DriverCapsInteropReturned[1],0,0));
    for (i=0;i<2;i++)
        GuardLog("wddm: DRIVERCAPS interop%u first100ns%lld last100ns%lld",
            i,InterlockedCompareExchange64(&Wddm->DriverCapsFirstTime[i],0,0),
            InterlockedCompareExchange64(&Wddm->DriverCapsLastTime[i],0,0));
    GuardLog("wddm: Blt observation calls%lld capacity%u",
        InterlockedCompareExchange64(&Wddm->PresentObservationCalls,0,0),BC250_PRESENT_OBSERVATIONS);
    for (i=0;i<BC250_PRESENT_OBSERVATIONS;i++) {
        BC250_PRESENT_OBSERVATION* o=&Wddm->PresentObservations[i];
        UINT j;
        // Acquire the immutable payload, skipping a still-active writer.
        if (!InterlockedCompareExchange(&o->Published,0,0)) continue;
        GuardLog("wddm: Blt obs%u interrupt100ns%llu qpc%llu",i,o->InterruptTime,o->Qpc);
        GuardLog("wddm: Blt obs%u ctx%p dev%p flags%x node%u umd%u system%u",
            i,o->Context,o->OwnerDevice,o->Flags,o->Node,o->UmdContext,o->SystemContext);
        GuardLog("wddm: Blt obs%u dma%u private%u offset%u rects%u list%u valid%u snapshot%u",
            i,o->DmaBytes,o->PrivateBytes,o->Offset,o->SubRects,o->ListSize,o->ListValid,o->SnapshotValid);
        GuardLog("wddm: Blt obs%u src %ld,%ld,%ld,%ld dst %ld,%ld,%ld,%ld",
            i,o->Src.left,o->Src.top,o->Src.right,o->Src.bottom,
            o->Dst.left,o->Dst.top,o->Dst.right,o->Dst.bottom);
        for (j=0;j<2;j++) {
            const BC250_WDDM_ALLOCATION_PRIVATE* a=&o->Allocations[j];
            GuardLog("wddm: Blt obs%u side%u handle%p adapter%u va%llX",
                i,j,o->Handles[j],o->PhysicalAdapter[j],o->Va[j]);
            if (o->SnapshotValid)
                GuardLog("wddm: Blt obs%u side%u %ux%u pitch%u fmt%u bytes%llu",
                    i,j,a->Width,a->Height,a->Pitch,a->Format,a->Size);
        }
    }
    GuardLog("wddm: GPU Present calls%lld records%lld rotate%lld refused%lld",
        InterlockedCompareExchange64(&Wddm->GpuPresentCalls,0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentRecords,0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentRotates,0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentRefused,0,0));
    GuardLog("wddm: GPU Present submits%lld rejected%lld failed%lld",
        InterlockedCompareExchange64(&Wddm->GpuPresentSubmits,0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSubmitRejected,0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSubmitFailed,0,0));
    GuardLog("wddm: GPU Present errors parameter%lld handle%lld color%lld other%lld",
        InterlockedCompareExchange64(&Wddm->GpuPresentStatuses[0],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentStatuses[1],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentStatuses[2],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentStatuses[3],0,0));
    // BD-065: the "handle" errors above, split by the snapshot's first failing check.
    GuardLog("wddm: GPU Present snapshot refused stopping%lld unopened%lld owner%lld umdopen%lld unbound%lld "
        "gone%lld umdbacking%lld descriptor%lld same%lld format%lld",
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotStopping],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotNotOpened],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotOtherOwner],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotUmdOpened],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotUnbound],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotBackingGone],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotUmdBacking],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotDescriptor],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotSameBacking],0,0),
        InterlockedCompareExchange64(&Wddm->GpuPresentSnapshotRefusals[Bc250SnapshotFormat],0,0));
    GuardLog("wddm summary: blit gate %s, %ld blits, %ld skips, %ld sources translated contiguous", Wddm->BlitGate ? "open" : "closed",
             Wddm->Blits, Wddm->BlitSkips, Wddm->BlitTranslations);
    // 2026-09-22 (ADR 0011 consequences, facts M100): where the copy actually landed. BlitsToFlip should be
    // every blit once a flip is live and stays mappable; BlitsMapFailed says how many of BlitsToFirmware are a
    // fallback rather than the ordinary gate-closed/pre-flip case, and the two scanout-remap counters say how
    // much of that mapping work DcnScanoutMapping actually did (once a flip, not once a present - M97).
    // Two lines since 0.7.208 (BD-070): the text alone is 162 characters, so the one line this was
    // never printed its remap and last-blit numbers at all. Both lines keep the same prefix. They
    // are two samples while the adapter runs, like every other counter in this summary, and no
    // number on one line has to be added to a number on the other.
    GuardLog("wddm summary: blit destination: %ld to the flipped surface, %ld to the POST framebuffer (%ld a "
             "failed-mapping fallback)",
             Wddm->BlitsToFlip, Wddm->BlitsToFirmware, Wddm->BlitsMapFailed);
    GuardLog("wddm summary: blit destination: %ld scanout remaps (%ld failed), last blit %ld rows, %ld seeds",
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
    GuardLog("wddm summary: visibility calls %ld source 0x%08X requested %u status 0x%08X current %u",
             Wddm->Device->VisibilityCalls,Wddm->Device->VisibilityLastSource,
             Wddm->Device->VisibilityLastRequested,Wddm->Device->VisibilityLastStatus,Wddm->Device->SourceVisible);
    GuardLog("wddm summary: display mode %u blanked %u write %u commit calls %ld power-transition %u powered-off %u",
             Wddm->Device->ModeActive,Wddm->Device->DcnBlanked,Wddm->Device->DcnWriteEnabled,
             Wddm->Device->CommitPowerCalls,Wddm->Device->CommitLastPowerTransition,Wddm->Device->CommitLastPoweredOff);
    GuardLog("wddm summary: display commit power-only %ld visible-link-off %ld",
             Wddm->Device->CommitPowerOnly,Wddm->Device->VisibilityLinkOff);
    {
        LARGE_INTEGER frequency;
        ULONG count=(ULONG)Wddm->Device->VisibilityCalls;
        ULONG n,retained=count<BC250_VISIBILITY_HISTORY_COUNT?count:BC250_VISIBILITY_HISTORY_COUNT;
        (void)KeQueryPerformanceCounter(&frequency);
        GuardLog("wddm visibility: true %lu false %lu failures %lu first-true 0x%08X last-true 0x%08X QPC %lld",
                 Wddm->Device->VisibilityTrueCalls,Wddm->Device->VisibilityFalseCalls,Wddm->Device->VisibilityFailures,
                 Wddm->Device->VisibilityFirstTrueStatus,Wddm->Device->VisibilityLastTrueStatus,frequency.QuadPart);
        for (n=0;n<retained;n++)
        {
            const BC250_VISIBILITY_EVENT* event=&Wddm->Device->VisibilityHistory[
                (count-retained+n)%BC250_VISIBILITY_HISTORY_COUNT];
            GuardLog("wddm visibility event: %lu src %lu req %lu status %08X visible %lu blank %lu qpc %lld..%lld",
                     event->Call,event->Source,event->Requested,event->Status,event->SourceVisible,event->Blanked,
                     event->BeginQpc,event->EndQpc);
        }
    }
    GuardLog("wddm summary: dcn lock acknowledgement timeouts %ld",
             InterlockedCompareExchange(&Wddm->Device->DcnLockTimeouts, 0, 0));
    // Two lines since 0.7.208 (BD-070): the text of the one line this was is 133 characters, so
    // after a long session the five vsync counts fell off the end of it. The first line keeps the text the
    // overlay and the scan-out trial parse. The second line says "vidpn flip vsyncs" and not
    // "vidpn flip <state>:", so that neither parser can take it for the flip line. The two lines
    // are two samples while the adapter runs, and no number on one is added to a number on the
    // other: the vsync counts come from the interrupt, the flip counts from the present path.
    GuardLog("wddm summary: vidpn flip %s: %ld hardware flips, %ld refused",
             Wddm->Device->VidPnFlipEnabled ? "open" : "closed", Wddm->Device->DcnFlipsHardware,
             Wddm->Device->DcnFlipRefused);
    GuardLog("wddm summary: vidpn flip vsyncs %ld armed, %ld acked, %ld refused, %ld completion-deferred, %ld old-buffer-reports",
             Wddm->Device->DcnVsyncArmed, Wddm->Device->DcnVsyncTicks, Wddm->Device->DcnVsyncRefused,
             Wddm->Device->DcnVsyncDeferred, Wddm->Device->DcnVsyncOldBufferReports);
    // BD-170: the immediate flips (dcn.c's DcnImmediateObserve) with their outcomes, the same-address promotions and
    // the DMA flips Present refused; the flip state machine (section 7.2) by edge, result, writer and E5; then the
    // last immediate-flip records, oldest first. The mean time to P' is sum / done. The counters are sampled without
    // a lock like the lines above; each record is a whole copy under its own sequence lock.
    {
        const BC250_DEVICE* device = Wddm->Device;
        const LONG next = device->DcnImmediateRecordNext;
        ULONG n;
        BC250_DCN_IMMEDIATE_RECORD record;
        GuardLog("wddm summary: immediate %ld programmed, %ld same-frame, %ld promoted; Present DMA flips refused %ld",
                 device->DcnImmediateFlips, device->DcnImmediateSameFrame, Wddm->ImmediatePromotions,
                 Wddm->DmaFlipsRefused);
        GuardLog("wddm summary: immediate outcome done %ld late %ld pending %ld primary-other %ld read-failed %ld superseded %ld",
                 device->DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_DONE],
                 device->DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_DONE_LATE],
                 device->DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_PENDING],
                 device->DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_PRIMARY_OTHER],
                 device->DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_READ_FAILED],
                 device->DcnImmediateOutcomes[BC250_DCN_IMMEDIATE_SUPERSEDED]);
        GuardLog("wddm summary: immediate done max %ld us sum %lld us, observe max %ld us overruns %ld, flip in L max %ld us",
                 device->DcnImmediateDoneMaxUs, device->DcnImmediateDoneSumUs, device->DcnImmediateObserveMaxUs,
                 device->DcnImmediateOverruns, Wddm->FlipLockedMaxUs);
        // The edges in flip_state.h's order (F0 F1 A W-v W-i W-u E), the results of edge E (refused, success,
        // program fault, U), and the exclusive answers of A.
        GuardLog("wddm summary: flip edges f0 %ld f1 %ld a %ld wv %ld wi %ld wu %ld e %ld",
                 Wddm->FlipEdges[BC250_FLIP_EDGE_FAULTING], Wddm->FlipEdges[BC250_FLIP_EDGE_OVERLAP],
                 Wddm->FlipEdges[BC250_FLIP_EDGE_REFUSED], Wddm->FlipEdges[BC250_FLIP_EDGE_DEPOSIT_VSYNC],
                 Wddm->FlipEdges[BC250_FLIP_EDGE_DEPOSIT_DRAINED], Wddm->FlipEdges[BC250_FLIP_EDGE_DEPOSIT_UNPROVEN],
                 Wddm->FlipEdges[BC250_FLIP_EDGE_PROGRAM]);
        GuardLog("wddm summary: flip results of e refused %ld ok %ld fault %ld u %ld",
                 Wddm->FlipResults[BC250_FLIP_RESULT_REFUSED], Wddm->FlipResults[BC250_FLIP_RESULT_SUCCESS],
                 Wddm->FlipResults[BC250_FLIP_RESULT_FAULT_PROGRAM], Wddm->FlipResults[BC250_FLIP_RESULT_UNPROVEN]);
        GuardLog("wddm summary: flip writers faulting %ld high %ld sync-failed %ld low-sync-failed %ld resumed %ld",
                 Wddm->WriterFaulting, Wddm->WriterHigh, Wddm->WriterSyncFailed, Wddm->LowSyncFailed,
                 Wddm->DepositsResumed);
        GuardLog("wddm summary: flip end-programs %ld owed-invalidated %ld owed-reported %ld flushes %ld destroy-unsync %ld",
                 Wddm->EndPrograms, Wddm->OwedInvalidated, Wddm->OwedReported, Wddm->FlipFlushes,
                 Wddm->DestroyUnsynchronized);
        GuardLog("wddm summary: flip writer-busy %ld recovery-pending %ld", Wddm->WriterBusy, Wddm->WriterRecovery);
        {
            ULONG kind;
            for (kind = 0; kind < BC250_WRITER_KINDS; kind++)
                GuardLog("wddm summary: flip writer kind %lu busy %ld refused %ld", kind, Wddm->WriterBusyByKind[kind],
                         Wddm->WriterRefusedByKind[kind]);
        }
        GuardLog("wddm summary: flip writer refused %ld last kind %lu at %s answer %lu status %08lX",
                 Wddm->WriterRefused, Wddm->WriterRefusedKind, WddmWriterRefusedSite(Wddm->WriterRefusedSite),
                 Wddm->WriterRefusedAnswer, (ULONG)Wddm->WriterRefusedStatus);
        GuardLog("wddm summary: bridge programmed %ld retired %ld failed %ld",
                 Wddm->WriterBridgePrograms, Wddm->WriterBridgeRetired, Wddm->WriterBridgeFailures);
        GuardLog("wddm summary: flip e5 %ld faulting %ld code 0x%06lX node %u blank %08lX",
                 Wddm->FlipFaults, Wddm->Faulting, Wddm->FaultCode, Wddm->FaultNode, (ULONG)Wddm->FaultBlank);
        GuardLog("wddm summary: flip e5 notified %ld failed %ld drain %ld/%ld; exclusive refused %ld failed %ld",
                 Wddm->FaultNotified, Wddm->FaultNotifyFailed, Wddm->FaultDrain, Wddm->FaultDrainReads,
                 Wddm->ExclusiveRefused, Wddm->ExclusiveFailed);
        // W-u (section 7.2): the deposits made with the plane not proven drained, each with its record code, writer,
        // generation, request and time to the writer's End; then the longest such time. Oldest first.
        {
            static const char* const ends[BC250_WU_ENDS] = {
                "open", "unblank", "end", "end-refused", "end-fault", "superseded", "dropped" };
            BC250_WDDM_WU_COPY wu;
            ULONG k;
            if (!WddmWuCopy(Wddm->Device, Wddm, &wu))
                GuardLog("wddm summary: flip wu %ld, records unavailable (no section)",
                         Wddm->FlipEdges[BC250_FLIP_EDGE_DEPOSIT_UNPROVEN]);
            else
            {
                GuardLog("wddm summary: flip wu %ld, records %ld, to-end max %ld us",
                         Wddm->FlipEdges[BC250_FLIP_EDGE_DEPOSIT_UNPROVEN], wu.Next, wu.ToEndMaxUs);
                for (k = 0; k < BC250_WDDM_WU_RECORDS; k++)
                {
                    const BC250_WDDM_WU_RECORD* r = &wu.Records[((ULONG)wu.Next + k) % BC250_WDDM_WU_RECORDS];
                    if (r->Record == 0) continue;
                    GuardLog("flip wu %ld: code 0x%06lX writer %lu gen %lu card 0x%llX flags 0x%03lX", r->Record,
                             r->Code, r->Writer, r->Generation, r->Request, r->Flags);
                    GuardLog("flip wu %ld: deposit %lu to-end %ld us %s", r->Record, r->Deposit, r->ToEndUs,
                             r->End < BC250_WU_ENDS ? ends[r->End] : "unknown");
                }
            }
        }
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        // Lab flavour (WddmWitness): Q, the shipping decision, against the witnesses read after it. Three lines a
        // record, each with its slot and request: the decision and the witnesses (q, fresh, e, r1, r2 as 0 or 1), the
        // addresses, the raw words. Slot 16 is the last request, whatever it showed.
        {
            BC250_WDDM_WITNESS_COPY wc;
            ULONG k;
            if (!WddmWitnessCopy(Wddm->Device, Wddm, &wc))
                GuardLog("wddm summary: flip witness (lab) unavailable (no section)");
            else
            {
                GuardLog("wddm summary: flip witness (lab) requests %ld read-failed %ld stale %ld order %ld",
                         wc.Requests, wc.ReadFailed, wc.Stale, wc.Order);
                GuardLog("wddm summary: flip witness (lab) qe %ld qr %ld er %ld kept %ld lost %ld", wc.Qe, wc.Qr,
                         wc.Er, wc.Kept, wc.Lost);
                for (k = 0; k <= BC250_WDDM_WITNESS_RECORDS; k++)
                {
                    const BC250_WDDM_WITNESS_RECORD* r = k < BC250_WDDM_WITNESS_RECORDS ? &wc.Records[k] : &wc.Last;
                    if (r->Request == 0 || (k < BC250_WDDM_WITNESS_RECORDS && k >= (ULONG)wc.Kept)) continue;
                    GuardLog("flip witness %lu req %ld: gen %lu flags 0x%03lX outcome %lu result %lu "
                             "q/fresh/e/r1/r2 %c%c%c%c%c", k, r->Request, r->Generation, r->Flags, r->Outcome,
                             r->Result, r->Q ? '1' : '0', r->Fresh ? '1' : '0', r->E ? '1' : '0', r->R1 ? '1' : '0',
                             r->R2 ? '1' : '0');
                    GuardLog("flip witness %lu req %ld: card 0x%llX phys 0x%llX earliest 0x%llX/0x%llX", k,
                             r->Request, r->Card, r->Physical, r->Earliest1, r->Earliest2);
                    GuardLog("flip witness %lu req %ld: pending before %08lX after %08lX",
                             k, r->Request, r->PendingBefore, r->PendingAfter);
                    GuardLog("flip witness %lu req %ld: arm %08lX read %08lX int %08lX/%08lX/%08lX "
                             "words %08lX %08lX %08lX %08lX", k, r->Request, (ULONG)r->ArmStatus,
                             (ULONG)r->ReadStatus, r->InterruptBefore, r->InterruptCleared, r->Interrupt, r->Low1,
                             r->High1, r->High2, r->Low2);
                }
            }
        }
#endif
        {
            const LONG calls = Wddm->VSyncControlCalls;
            const LONG kept = calls < (LONG)BC250_WDDM_VSYNC_CONTROL_EVENTS ? calls : (LONG)BC250_WDDM_VSYNC_CONTROL_EVENTS;
            LARGE_INTEGER frequency;
            LONG k;
            // The QPC now and its frequency, so that the age of each event below can be read from the log alone.
            const LARGE_INTEGER now = KeQueryPerformanceCounter(&frequency);
            GuardLog("wddm summary: vsync control %ld calls, on %ld off %ld, last %ld, QPC now %lld at %lld/s", calls,
                     Wddm->VSyncControlOn, Wddm->VSyncControlOff, kept, now.QuadPart, frequency.QuadPart);
            for (k = calls - kept; k < calls; k++)
            {
                const BC250_WDDM_VSYNC_CONTROL* event = &Wddm->VSyncControl[(ULONG)k % BC250_WDDM_VSYNC_CONTROL_EVENTS];
                GuardLog("wddm vsync control %ld: %s qpc %lld owed 0x%llX", event->Call, event->On ? "on" : "off",
                         event->Qpc, (ULONGLONG)event->Owed);
            }
        }
        // Three lines a record. On the first, "physical" is the request and "seen" the last PRIMARY_SURFACE_ADDRESS
        // sample, both physical. The second line's times are ProgramUs/FirstUs/ClearUs/DoneUs/EndUs (bc250kmd.h). The
        // third line's "read S/N" is ReadStatus/Samples, and its flags are the request's.
        for (n = 0; n < BC250_DCN_IMMEDIATE_RECORDS; n++)
        {
            const ULONG slot = ((ULONG)next + n) % BC250_DCN_IMMEDIATE_RECORDS;
            const ULONG copied = WddmImmediateRecordCopy(device, slot, &record);
            if (copied == 0) continue;
            if (copied == 2) { GuardLog("immediate flip slot %lu: unavailable (changed during the copy)", slot); continue; }
            GuardLog("immediate flip %ld: %s card 0x%llX physical 0x%llX seen 0x%llX",
                     record.Sequence, WddmImmediateOutcomeText(record.Outcome), record.CardAddress, record.Physical,
                     record.Observed);
            GuardLog("immediate flip %ld: gen %lu frame %08lX pos %08lX..%08lX ctl %08lX us %ld/%ld/%ld/%ld/%ld",
                     record.Sequence, record.Generation, record.Frame, record.PositionBefore, record.PositionAfter,
                     record.Control, record.ProgramUs, record.FirstUs, record.ClearUs, record.DoneUs, record.EndUs);
            GuardLog("immediate flip %ld: read %08lX/%lu flags 0x%03lX", record.Sequence, (ULONG)record.ReadStatus,
                     record.Samples, record.Flags);
        }
    }
    // Independently sampled counters/times: no interrupt lock and no per-frame logging.
    // Counters/times are independently sampled, not an atomic incident record.
    GuardLog("vsync skip: odd %ld read %ld same %ld changed %ld",
             Wddm->Device->DcnVsyncSkipOddGeneration, Wddm->Device->DcnVsyncSkipReadFailure,
             Wddm->Device->DcnVsyncSkipSameAddress, Wddm->Device->DcnVsyncSkipChangedGeneration);
    GuardLog("vsync skip100ns: odd %lld read %lld same %lld changed %lld",
             InterlockedCompareExchange64(&Wddm->Device->DcnVsyncSkipOddGenerationTime,0,0),
             InterlockedCompareExchange64(&Wddm->Device->DcnVsyncSkipReadFailureTime,0,0),
             InterlockedCompareExchange64(&Wddm->Device->DcnVsyncSkipSameAddressTime,0,0),
             InterlockedCompareExchange64(&Wddm->Device->DcnVsyncSkipChangedGenerationTime,0,0));
    GuardLog("vsync vector: DPC polls %ld ACKs %ld sync-failures %ld",
             Wddm->Device->DcnVsyncDpcPolls, Wddm->Device->DcnVsyncDpcAcked,
             Wddm->Device->DcnVsyncDpcSyncFailures);
    GuardLog("vsync diagnostic: irq %ld no-mmio %ld flip-off %ld unarmed %ld",
             Wddm->Device->InterruptCount, Wddm->Device->DcnVsyncNoMmio,
             Wddm->Device->DcnVsyncFlipDisabled, Wddm->Device->DcnVsyncUnarmed);
    GuardLog("vsync diagnostic: no-event %ld read-fail %ld ack-fail %ld",
             Wddm->Device->DcnVsyncNoEvent, Wddm->Device->DcnVsyncReadFailed, Wddm->Device->DcnVsyncAckFailed);
    GuardLog("vsync diagnostic: 100ns irq %lld entry %lld ack %lld notify %lld status %08lX",
             InterlockedCompareExchange64(&Wddm->Device->InterruptLastTime, 0, 0),
             InterlockedCompareExchange64(&Wddm->Device->DcnVsyncEntryTime, 0, 0),
             InterlockedCompareExchange64(&Wddm->Device->DcnVsyncAckTime, 0, 0),
             InterlockedCompareExchange64(&Wddm->Device->DcnVsyncNotifyTime, 0, 0),
             (ULONG)Wddm->Device->DcnVsyncLastStatus);
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
    ConnectorLinkLogOpen(Device);
    DcnLogVsyncSnapshot(Device);
    DpmLogSummary(Device);
    InteropLogSummary(Device);
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

// The start witness's QUIET test (start_health.c): the generation of the last primary programming that succeeded,
// sampled the way WddmReadCompletedPrimary samples it, with interlocked reads only (no BAR access). The caller
// holds StartHealth.Lock over an open witness: every stop closes the witness under that lock (GuardRecordStopBegin,
// pnp.c) before WddmStop detaches and frees this state, so the state cannot go away under the read.
BOOLEAN WddmPrimaryProgrammed(_In_ const BC250_DEVICE* Device, _Out_ ULONG* Sequence)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    LONG generation;
    ULONG programmed;

    *Sequence = 0;
    if (wddm == NULL) return FALSE;
    generation = InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0);
    if (generation & 1) return FALSE;
    programmed = (ULONG)InterlockedCompareExchange(&wddm->PrimaryProgrammedSequence, 0, 0);
    if (InterlockedCompareExchange(&wddm->PrimarySequence, 0, 0) != generation) return FALSE;
    *Sequence = programmed;
    return TRUE;
}

// ---- start, stop, DPC ------------------------------------------------------------------------------------------

static BOOLEAN WddmMemoryLayout(_In_ const BC250_DEVICE* Device, _Out_ ULONGLONG* Offset, _Out_ ULONGLONG* Length,
                                _Out_ ULONGLONG* TableOffset, _Out_ ULONGLONG* TableLength);

NTSTATUS WddmStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm;
    BC250_START_REPORT* startup=NULL;
    void** buckets;
    BOOLEAN vidmmPrepared=FALSE;
    NTSTATUS status;
    ULONGLONG segmentOffset, segmentLength, tableOffset, tableLength;
    RTL_OSVERSIONINFOW osVersion;

    Device->FullWddm = g_FullWddm;
    Device->Wddm = NULL;
    RtlZeroMemory(&Device->WddmAperture,sizeof(Device->WddmAperture));
    Device->WddmApertureRequest = 0;
    Device->ComposedSourceModes = FALSE;
    Device->CommittedSourceFormat = 0;
    Device->LargePageLayout = FALSE;
    Device->LargePages = FALSE;
    if (!g_FullWddm) return STATUS_SUCCESS;                            // gate closed: this file does nothing at all
    RtlZeroMemory(&osVersion, sizeof(osVersion));
    osVersion.dwOSVersionInfoSize = sizeof(osVersion);
    status = RtlGetVersion(&osVersion);
    if (!NT_SUCCESS(status)) return status;
    // 0.7.201: source modes of the composed formats (display_modes.h), on unless the value is 0.
    Device->ComposedSourceModes = GuardReadSetting(L"OfferComposedSourceModes", 1) != 0;
    GuardLog("wddm: composed source modes %s", Device->ComposedSourceModes ? "offered" : "off");
    // Large pages for VRAM, as amdgpu maps it: a directory entry with PDE_PTE and the fragment of its range, 2 MiB at
    // level 1 (fact M808) and 1 GiB at level 2. Read before the first WddmMemoryLayout of this start, because the
    // layout, the GpuMmu caps and the translation of a LargePage entry must agree for the whole start.
    //   0 (absent): the layout and the 4 KB mappings of earlier drivers. Off by default: no 1 GiB entry has been
    //               read back on unit A, and the layout below costs the VRAM under the first 1 GiB boundary.
    //   1: large pages, on the layout of 2.
    //   2: the layout of 1 (segment base on 1 GiB, 2 MiB placement) with 4 KB mappings. The control that tells
    //      the effect of the page size from the effect of the layout.
    {
        const ULONG asked = GuardReadSetting(L"LargePages", 0);
        Device->LargePageLayout = (asked == 1 || asked == 2);
        Device->LargePages = (asked == 1);
        GuardLog("wddm: LargePages %lu: %s", asked, Device->LargePages ? "large VRAM pages on the aligned layout" :
                 (Device->LargePageLayout ? "aligned layout, 4 KB mappings" : "off"));
    }

    // BD-122: the sub-steps of this function inside the start record's WddmStart step (guard.c StartRecord).
    GuardStartSubStep(L"wddm-pool");
    // Required state must exist before the adapter starts accepting paging work.
    wddm = (BC250_WDDM*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*wddm), BC250_WDDM_TAG);
    if (wddm == NULL) { GuardLog("wddm: no pool for the adapter state"); return STATUS_INSUFFICIENT_RESOURCES; }
    startup=(BC250_START_REPORT*)ExAllocatePool2(POOL_FLAG_NON_PAGED,sizeof(*startup),BC250_WDDM_TAG);
    if (!startup) { ExFreePoolWithTag(wddm,BC250_WDDM_TAG); return STATUS_INSUFFICIENT_RESOURCES; }
    // Nonpaged and zeroed: lookups run under the spin lock at DISPATCH_LEVEL.
    buckets=(void**)ExAllocatePool2(POOL_FLAG_NON_PAGED,BC250_OBJECT_INDEX_BYTES,BC250_WDDM_TAG);
    if (!buckets) {
        GuardLog("wddm: no pool for the object index");
        ExFreePoolWithTag(startup,BC250_WDDM_TAG);
        ExFreePoolWithTag(wddm,BC250_WDDM_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    Bc250ObjectIndexInit(&wddm->ObjectIndex,buckets,(unsigned long)FIELD_OFFSET(BC250_WDDM_OBJECT,HashNext));
    wddm->Device = Device;
    wddm->OsBuildNumber = osVersion.dwBuildNumber;
    GuardLog("wddm: OS build %lu, CDD shadow backing %s", wddm->OsBuildNumber,
             wddm->OsBuildNumber >= 26100 ? "coherent aperture" : "local memory");
    wddm->NativePteCopies=(GuardReadSetting(L"EnableNativePteCopy",0)==1);
    wddm->TraceUmdProbes = (GuardReadSetting(L"TraceUmdProbes", 0) == 1);
    (void)KeQueryPerformanceCounter(&wddm->UmdProfileFrequency);
    ExInitializePushLock(&wddm->PagingBuildLock);
    KeInitializeSpinLock(&wddm->Lock);
    ExInitializeFastMutex(&wddm->GfxSubmitMutex);
    ExInitializeFastMutex(&wddm->ComputeSubmitMutex);
    InitializeListHead(&wddm->Objects);
    KeInitializeDpc(&wddm->ReportDpc, WddmReportDpcRoutine, Device);
    KeInitializeDpc(&wddm->VSyncDpc, WddmVSyncDpcRoutine, Device);
    wddm->HandleIdentityProbe = (GuardReadSetting(L"EnableHandleIdentityProbe", 0) == 1);
    // Start-latched: changing registry values does not enable existing unbound opens. Absent = on since
    // 0.7.181; closed for this start after an unclean boot, an invalid value or a registry failure (interop.c).
    InteropStart(Device, &wddm->GpuPresentGate, &wddm->CddDwmInterop);
    wddm->BlitGate = (GuardReadSetting(L"EnablePresentBlit", 0) == 1);   // E20: the diagnostic CPU blit (ADR 0011)
    // M15.14, read once for this start: with the value at 0 no application surface is scanned out and the
    // driver answers SetVidPnSourceAddress as 0.7.205.1 did. Absent = on, like OfferComposedSourceModes; the
    // INF writes neither. Logged, because a lab run that sees no scan-out flip must be able to tell a closed
    // switch from a refusal.
    wddm->ScanoutAdmitGate = (GuardReadSetting(L"EnableScanoutAdmit", 1) != 0);
    GuardLog("wddm: scan-out admission %s", wddm->ScanoutAdmitGate ? "on" : "off (0.7.205.1 behaviour)");
    // HLK gap G1 (docs/design/gdi-hw-accel.md), read once for this start. The gate is the field off
    // switch and is open by default; the capability bit needs more than the gate, which is what the
    // second line decides.
    //   SupportKernelModeCommandBuffer is a promise that every operation of DXGK_RENDERKM_OPERATION
    // is performed, because DXGK_PRESENTATIONCAPS has no "cannot" bit for an individual one: once
    // the bit is set, the OS sends AlphaBlend, StretchBlt, TransparentBlt and ClearTypeBlend as well
    // as the two the DMA_DATA engine does exactly, and DxgkDdiRenderGdi cannot refuse them (its
    // return table bugchecks on any status but STATUS_SUCCESS). So the bit follows the translator,
    // not the gate: Bc250GdiKernelsPresent is the six gfx1013 compute kernels of gdi_kernels.cl,
    // and an image without them reports the capability exactly as the release before them did.
    wddm->GdiHwAccelGate = (BOOLEAN)(GuardReadSetting(L"EnableGdiHwAccel", 1) != 0);
    wddm->GdiHwAccel = (BOOLEAN)(wddm->GdiHwAccelGate && Bc250GdiKernelsPresent());
    GuardLog("wddm: GDI hardware acceleration gate %u, kernels %u, capability %u",
             (UINT)wddm->GdiHwAccelGate, (UINT)Bc250GdiKernelsPresent(), (UINT)wddm->GdiHwAccel);
    if (wddm->GdiHwAccel)
        GuardLog("wddm: gdi code %u kernels, %u bytes, sha256 %s", Bc250GdiCodeKernelCount(),
                 Bc250GdiCodeBytes(), Bc250GdiCodeHash());
    // M15.14 increment 2, read once for this start: whether the bc250_scanout_caps trailer tells the
    // compositor's user-mode driver that a client scan-out flip will be admitted. Absent = on from
    // 0.7.213, the train rule for a finished feature; 0 is the bisect switch and gives back 0.7.208.1
    // byte for byte at every buffer size.
    //   A wrong TRUE costs a copy the operating system no longer makes, and the one process whose answer
    // changes is the compositor itself, so an open default is only safe because of what follows.
    //   The invariant this publishes is "a closed kernel path can never leave the shell agreeing to a flip
    // this driver will refuse", so every start-latched fact the flip path needs is ANDed in, not only
    // EnableScanoutAdmit. All of them are known here: MmioStart, VramStart and AcquirePostDisplayOwnership
    // all run before WddmStart (pnp.c).
    //   - VidPnFlipEnabled (EnableMmio && EnableDcnWrite && EnableVidPnFlip, mmio.c). This is the dangerous
    //     one. With it closed, SetVidPnSourceAddress skips DcnFlipSourceAddress altogether, still publishes
    //     and still returns STATUS_SUCCESS: an operator who closes the flip gate to recover a display fault
    //     and leaves this switch on would have DWM stop composing on a TRUE answer while HUBP0 keeps the
    //     last composed frame, so the screen freezes while the game runs and every counter says success.
    //   - VramEnabled and a mapped Mmio: DcnFlipSourceAddress then returns STATUS_ACCESS_DENIED or
    //     STATUS_DEVICE_NOT_READY, so the flip fails after the OS has taken the SharedPrimaryTransition it
    //     does not fall back from seamlessly (ref/ddi-display/d3dkmddi.md:12793) - a blank output.
    //   - page-aligned VRAM bases: the flip path refuses every requesting candidate with
    //     BC250_SCANOUT_ALIGNMENT on a board that breaks this, and the shell cannot see that.
    //   - a POST geometry at all: the trailer's own payload. Publishing 0x0 would be publishing nothing.
    // Logged for the same reason as the gate above, and with the reason, because a run that sees no flip
    // must be able to tell a closed switch from a refusal - and now from a closed flip gate as well.
    {
        const BOOLEAN aligned =
            ((Device->VramMcBase | (ULONGLONG)Device->VramPhysical.QuadPart) & 0xFFFull) == 0;
        const BOOLEAN pathOpen = (BOOLEAN)(wddm->ScanoutAdmitGate && Device->VidPnFlipEnabled &&
            Device->VramEnabled && Device->Mmio != NULL && aligned &&
            Device->Post.Width != 0 && Device->Post.Height != 0);
        const BOOLEAN asked = (BOOLEAN)(GuardReadSetting(L"EnableDirectFlipHandshake", 1) != 0);
        wddm->DirectFlipHandshake = (BOOLEAN)(asked && pathOpen);
        GuardLog("wddm: DirectFlip handshake %s", wddm->DirectFlipHandshake ? "on" : "off (no client flip offered)");
        // The inputs on their own line: one line holding all of them and the verdict text does not fit
        // BC250_LOG_TEXT, and a truncated line is how BD-070 lost the scan-out refusal counts. The POST
        // geometry itself is already logged by pnp.c before this; here it is only present or absent.
        GuardLog("wddm: DirectFlip handshake inputs: asked %u gate %u flip %u vram %u mmio %u aligned %u post %u",
                 (ULONG)(asked ? 1 : 0), (ULONG)(wddm->ScanoutAdmitGate ? 1 : 0),
                 (ULONG)(Device->VidPnFlipEnabled ? 1 : 0), (ULONG)(Device->VramEnabled ? 1 : 0),
                 (ULONG)(Device->Mmio != NULL ? 1 : 0), (ULONG)(aligned ? 1 : 0),
                 (ULONG)(Device->Post.Width != 0 && Device->Post.Height != 0 ? 1 : 0));
        // M15.14 (0.7.216.20): the plane's pixel format per flip. The firmware's format registers are read here,
        // before the first flip of this start, and only a plane that decodes to ARGB8888 lets a flip program
        // R8G8B8A8 or R10G10B10A2 (dcn.c DcnCaptureFirmwareFormat). EnableScanoutPlaneFormats, read once,
        // absent = on, is the bisect switch: 0 skips the read, so this start flips B8G8R8A8 surfaces only and
        // the caps trailer does not carry PLANE_FORMATS, which is 0.7.216.18 behaviour.
        if (Device->VidPnFlipEnabled) {
            const BOOLEAN formats = (BOOLEAN)(GuardReadSetting(L"EnableScanoutPlaneFormats", 1) != 0);
            if (formats) DcnCaptureFirmwareFormat(Device);
            GuardLog("wddm: plane formats %s", Device->DcnPlaneFormats ? "on" :
                     formats ? "off (firmware format not decoded)" : "off (EnableScanoutPlaneFormats 0)");
        }
    }
    // C50, read once for this start: absent = ON. The DDI text asks for the pairing (d3dkmddi.md:2392) and the
    // hop it removes is pure latency, so the finished behaviour rides the release, which is the project's release
    // rule. The value 0 is 0.7.208.1 behaviour exactly, which is what makes it the bisect
    // switch of this feature: one restart with NotifyDpcInReport 0 prices the pairing against the driver that
    // does not have it, in the same session shape.
    // Memory manager stage 1c (0.7.216.8), read once for this start: absent = ON. A VRAM allocation of the UMD that
    // is not a scan-out and not DISCARDABLE gets the aperture as its second segment, so VidMm can demote it to
    // system memory under local pressure instead of evicting it (umd_blob.c UmdBlobPlacement, the amdgpu rule).
    // Under GpuMmu such an allocation is "not mapped" into the aperture; its pages are reached through
    // system-memory leaves of the GPU page tables (gpu-segments.md), the PTE shape every GTT allocation already
    // uses. 0 gives every VRAM allocation the one local segment, which is 0.7.216.1 byte for byte.
    wddm->SharedResidency = (GuardReadSetting(L"EnableSharedResidency", 1) != 0);
    GuardLog("wddm: shared residency %s", wddm->SharedResidency ?
             "on (UMD VRAM allocations may be demoted to the aperture)" : "off (0.7.216.1 placement)");
    wddm->NotifyDpcInReport = (GuardReadSetting(L"NotifyDpcInReport", 1) != 0);
    GuardLog("wddm: completion report pairs its own notify dpc: %s",
             wddm->NotifyDpcInReport ? "yes" : "no (0.7.208.1 behaviour, one dxgkrnl DPC later)");
    // C48/C49: the ring-gap accounting, always on. One frequency read for both nodes; the counter and the
    // frequency come out of the same call, like the hold histogram's.
    {
        UINT ringNode;
        for (ringNode = 0; ringNode < BC250_WDDM_NODE_COUNT_MAX; ringNode++) Bc250RingGapReset(&wddm->RingGap[ringNode]);
        wddm->RingGapLastVsyncQpc = 0;
        wddm->RingGapVsyncStampsHw = wddm->RingGapVsyncStampsTimer = 0;
        (void)KeQueryPerformanceCounter(&wddm->RingGapFrequency);
        Bc250NotifyPairingReset(&wddm->NotifyPairing);
        wddm->NotifyDpcBusy = 0;
        for (ringNode = 0; ringNode < BC250_WDDM_NODE_COUNT_MAX; ringNode++)
        {
            wddm->CompletionRetries[ringNode] = 0;
            wddm->CompletionsDropped[ringNode] = 0;
        }
        wddm->PreemptionReportsLost = 0;
    }
    // BD-152: a start-latched diagnostic threshold, independent of OS TDR.
    // A legacy SubmitWatchdogMs value no longer authorizes gate closure.
    {
        int invalid = 0;
        ULONG requested = GuardReadSetting(L"SubmitWatchdogMs", 0);
        wddm->SubmitBudgetMs = Bc250SubmitDiagnosticMs(requested, &invalid);
        wddm->SubmitTickMs = Bc250SubmitTickMs(wddm->SubmitBudgetMs);
        Bc250SubmitWatchdogIdle(&wddm->SubmitWatchdog);
        wddm->SubmitRearms = 0;             // one statement each: the counters are volatile, and a chained
        wddm->SubmitActivityChanges = 0;
        wddm->SubmitPrimes = 0;
        wddm->SubmitGapResets = 0;
        wddm->SubmitChecks = 0;             // assignment reads each one back as the value of the next
        wddm->SubmitHeadMaxMs = 0;
        wddm->SubmitQueueMaxMs = 0;
        GuardLog("wddm: submit diagnostic v2 %lu ms (setting %lu, %s), sample every %lu ms; OS TdrDelay unavailable",
                 wddm->SubmitBudgetMs, requested,
                 invalid ? "invalid, diagnostic default" : (requested == 0 ? "default" : "configured"),
                 wddm->SubmitTickMs);
    }
    KeInitializeDpc(&wddm->SubmitDpc, WddmSubmitDpcRoutine, Device);
    KeInitializeTimer(&wddm->SubmitTimer);
    KeInitializeDpc(&wddm->ComputeDpc, WddmComputeDpcRoutine, Device);
    KeInitializeTimer(&wddm->ComputeTimer);
    // ADR 0008 stage D (docs/design/paging-node.md). Read once, like EnableGpuSubmit's own read in gfx.c: node 1's
    // existence for this whole device start is decided here. gfx.c's own gate (GfxStart) decides separately
    // whether GfxSubmitPaging itself may ever run; this one decides whether the table admits the node at all.
    wddm->NodeCount = (GuardReadSetting(L"EnablePagingNode", 0) == 1) ? BC250_WDDM_NODE_COPY + 1u : BC250_WDDM_NODE_COUNT;
    // Node 2 (docs/design/compute-node.md): GfxStart ran before this function (pnp.c) and latched the node 2 selector
    // with every gate the compute ring rests on, the paging node among them. Node 2 is added only on top of node 1,
    // so that its ordinal is 2 whatever else changes.
    if (wddm->NodeCount == BC250_WDDM_NODE_COPY + 1u && GfxComputeNodeGate(Device))
        wddm->NodeCount = BC250_WDDM_NODE_COMPUTE + 1u;
    GuardLog("wddm: %u nodes: 3D%s%s", wddm->NodeCount, wddm->NodeCount > BC250_WDDM_NODE_COPY ? ", paging" : "",
             wddm->NodeCount > BC250_WDDM_NODE_COMPUTE ? ", compute (MEC ring 1)" : "");
    // M15.12: start-latched hang-recovery switch (docs/design/hang-recovery.md). On by default from 0.7.216.18
    // (absent = 1, lab-proven in trial D1 of 0.7.216.17); 0 is the switch-off and leaves every TDR DDI exactly as
    // it was before 0.7.216.13. Any other value is on, as for the other finished features (EnableVmidPool).
    wddm->HangRecoveryMode = (GuardReadSetting(L"HangRecoveryMode", 1) != 0);
    wddm->HangRecoveryWaves = (GuardReadSetting(L"HangRecoveryWaves", 1) != 0);
    GuardLog("wddm: pre-kill wave capture %s", wddm->HangRecoveryWaves ? "on" : "off");
    if (wddm->HangRecoveryMode)
        GuardLog("wddm: HangRecoveryMode 1: a node-0 ResetEngine tries stage-1 soft recovery, verdicts in "
                 "Parameters\\HangRecovery");
    else
        GuardLog("wddm: HangRecoveryMode 0: ResetEngine refuses every engine reset, as before 0.7.216.13");
    KeInitializeDpc(&wddm->PagingSubmitDpc, WddmPagingSubmitDpcRoutine, Device);
    KeInitializeTimer(&wddm->PagingSubmitTimer);
    KeInitializeDpc(&wddm->PagingDrainDpc, WddmPagingDrainDpcRoutine, Device);
    KeInitializeTimer(&wddm->PagingDrainTimer);
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
    GuardStartSubStep(L"gpu-va-layout");
    // A carve-out with no room above its first 1 GiB physical boundary gets no large pages, not a failed start.
    if (Device->LargePageLayout && !WddmMemoryLayout(Device,&segmentOffset,&segmentLength,&tableOffset,&tableLength)) {
        Device->LargePageLayout = FALSE;
        Device->LargePages = FALSE;
        if (WddmMemoryLayout(Device,&segmentOffset,&segmentLength,&tableOffset,&tableLength))
            GuardLog("wddm: LargePages off: no VRAM segment above the first 1 GiB physical boundary");
    }
    if (GuardReadSetting(L"EnableGpuVa",0)!=1 ||
        !WddmMemoryLayout(Device,&segmentOffset,&segmentLength,&tableOffset,&tableLength)) {
        status=STATUS_DEVICE_NOT_READY;
        goto Failed;
    }
    // Memory manager stage 1a (0.7.216.8), read once for this start: the OS aperture (segment 2) size in MiB.
    // Absent = 384; the INF writes no value, so a driver update never changes an operator's choice. 256 is the
    // bisect value: it gives the 256 MiB aperture of every driver before this one, byte for byte. A value
    // outside [256, 447] is clamped and logged. If the GART cannot hold the size asked for, the start falls back
    // to 256 MiB once, the size every earlier start had, instead of failing the adapter.
    //   What the size changes is narrow, and the log says so in the same line: under GpuMmu an aperture-segment
    // allocation without AccessedPhysically is "not mapped" into the aperture - its pages are system memory
    // reached through the GPU page tables (windows-driver-docs gpu-segments.md, "Accessing allocations by
    // physical address", table row "Aperture Segment"). Only MAP_APERTURE_SEGMENT operations use this range, and
    // no recorded session of this driver has issued one (summary "aperture map batches 0, unmap batches 0").
    // The aperture's Size and CommitLimit follow the size (WddmQuerySegment4); the shared pool does not, because
    // dxgkrnl's implicit system-memory segment already has no commit limit (calculating-graphics-memory.md).
    {
        int clamped=0;
        const ULONG asked=GuardReadSetting(L"ApertureSegmentMegabytes",
                                           (ULONG)(PAGING_APERTURE_DEFAULT_BYTES/PAGING_APERTURE_MIB));
        Device->WddmApertureRequest=PagingApertureBytesForSetting(asked,&clamped);
        GuardLog("wddm: OS aperture asked %lu MiB, using %llu MiB%s (only MAP_APERTURE_SEGMENT uses it)",asked,
                 Device->WddmApertureRequest/PAGING_APERTURE_MIB,clamped?" (clamped to 256..447)":"");
    }
    GuardStartSubStep(L"os-aperture");
    status=GartCaptureAperture(Device,&Device->WddmAperture);
    if (!NT_SUCCESS(status) && Device->WddmApertureRequest!=PAGING_APERTURE_MIN_BYTES) {
        GuardLog("wddm: GART refused a %llu MiB OS aperture (0x%08X), retrying at 256 MiB",
                 Device->WddmApertureRequest/PAGING_APERTURE_MIB,status);
        Device->WddmApertureRequest=PAGING_APERTURE_MIN_BYTES;
        status=GartCaptureAperture(Device,&Device->WddmAperture);
    }
    if (!NT_SUCCESS(status)) goto Failed;
    GuardLog("wddm: OS aperture %llu MiB at gpu 0x%llX, PTE slice 0x%llX",
             Device->WddmAperture.bytes/PAGING_APERTURE_MIB,Device->WddmAperture.mc,Device->WddmAperture.table);
    vidmmPrepared=TRUE; // VidMmStartLayout initializes its lock even on failure.
    GuardStartSubStep(L"vidmm-layout");
    status=VidMmStartLayout(Device,segmentOffset,segmentLength,BC250_WDDM_SEGMENT_VRAM,
                           tableOffset,tableLength,BC250_WDDM_SEGMENT_TABLES);
    if (!NT_SUCCESS(status)) goto Failed;
    GuardStartSubStep(L"gpu-startup");     // startup.c names a step of its own at each phase
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
    if (wddm->HandleIdentityProbe)
        GuardLog("wddm: handle callbacks size %u get@%u=%p acquire@%u=%p release@%u=%p",
            Device->Dxgk.Size,
            (ULONG)FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbGetHandleData),(void*)Device->Dxgk.DxgkCbGetHandleData,
            (ULONG)FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbAcquireHandleData),(void*)Device->Dxgk.DxgkCbAcquireHandleData,
            (ULONG)FIELD_OFFSET(DXGKRNL_INTERFACE,DxgkCbReleaseHandleData),(void*)Device->Dxgk.DxgkCbReleaseHandleData);

    return STATUS_SUCCESS;
Failed:
    // No WDDM object was published and no OS work was accepted. The coordinator
    // unwinds attempted hardware phases; PnP cleanup handles prepared objects.
    if (vidmmPrepared) VidMmStop();
    RtlZeroMemory(&Device->WddmAperture,sizeof(Device->WddmAperture));
    Device->WddmApertureRequest = 0;
    ExFreePoolWithTag(startup,BC250_WDDM_TAG);
    ExFreePoolWithTag(wddm->ObjectIndex.Buckets,BC250_WDDM_TAG);
    ExFreePoolWithTag(wddm,BC250_WDDM_TAG);
    return status;
}

// Software half of a retained power transition. The future power coordinator
// must stop/join IH and hardware consumers separately before powering down.
// SetPowerState is Level Three: OS work is already idle. Never fake retirement
// or destroy live OS objects to make that invariant appear true.
static BOOLEAN WddmPowerIdleLocked(const BC250_WDDM* Wddm)
{
    UINT node;
    if (Wddm->HwPending || Wddm->DeferredValid || Wddm->ComputeHwPending || Wddm->ComputeDeferredValid ||
        Wddm->PagingHwPending ||
        Wddm->PagingHead || Wddm->PagingTail || Wddm->PagingDeferredValid ||
        Wddm->ReportActive || Wddm->ReportAgain) return FALSE;
    for (node=0;node<BC250_WDDM_NODE_COUNT_MAX;node++) {
        if (Wddm->ActiveSubmissions[node] || Wddm->CompletionPending[node] ||
            Wddm->PreemptionPending[node] || Wddm->RefusalPending[node] ||
            Wddm->RejectedPending[node] || Wddm->WatchdogFaulted[node]) return FALSE;
    }
    return TRUE;
}

NTSTATUS WddmSuspendRetained(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    KIRQL irql;
    NTSTATUS status=STATUS_SUCCESS;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (!wddm) return STATUS_DEVICE_NOT_READY;
    // Give already queued completion notifications their normal publication path.
    // This does not poll hardware, clear pending work or manufacture completion.
    KeFlushQueuedDpcs();
    KeAcquireSpinLock(&wddm->Lock,&irql);
    if (wddm->RetainedPowerPause) {
        KeReleaseSpinLock(&wddm->Lock,irql);
        return STATUS_SUCCESS;
    }
    if (wddm->Stopping || !WddmPowerIdleLocked(wddm)) {
        KeReleaseSpinLock(&wddm->Lock,irql);
        return STATUS_DEVICE_BUSY;
    }
    wddm->Stopping=TRUE;
    if (Device->DcnVsyncArmed) status=DcnVsyncEnable(Device,FALSE);
    if (!NT_SUCCESS(status)) {
        wddm->Stopping=FALSE;
        KeReleaseSpinLock(&wddm->Lock,irql);
        return status;
    }
    wddm->RetainedPowerPause=TRUE;
    wddm->PrimaryNeedsRestore=TRUE;
    // M15.14: the plane's format registers may change with its address. Unknown (NONE) makes the next flip
    // write the format, whatever it is, and keeps the CPU blit off the plane until then (dcn.c).
    if (Device->DcnPlaneFormats) Device->DcnPlaneFormat=BC250_PLANE_FORMAT_NONE;
    wddm->VSyncArmed=FALSE;
    KeCancelTimer(&wddm->VSyncTimer);
    KeCancelTimer(&wddm->SubmitTimer);
    KeCancelTimer(&wddm->PagingSubmitTimer);
    KeCancelTimer(&wddm->PagingDrainTimer);  // idle means no queued paging job, so nothing is owed a drain
    KeCancelTimer(&wddm->ComputeTimer);
    KeReleaseSpinLock(&wddm->Lock,irql);
    KeRemoveQueueDpc(&wddm->SubmitDpc);
    KeRemoveQueueDpc(&wddm->ComputeDpc);
    KeRemoveQueueDpc(&wddm->PagingSubmitDpc);
    KeRemoveQueueDpc(&wddm->PagingDrainDpc);
    KeRemoveQueueDpc(&wddm->VSyncDpc);
    KeRemoveQueueDpc(&wddm->ReportDpc);
    KeFlushQueuedDpcs();
    // Device->Wddm, Objects, aperture, capture owners and all fence values stay.
    return STATUS_SUCCESS;
}

// Call only after the hardware coordinator restored private backing, engines,
// translation, IRQ ownership and a synchronized black display. These cached
// readiness checks supplement that contract; they do not implement GPU resume.
NTSTATUS WddmResumeRetained(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Device->Wddm;
    LARGE_INTEGER due;
    KIRQL irql;
    NTSTATUS status=STATUS_SUCCESS;
    if (KeGetCurrentIrql()!=PASSIVE_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (!wddm) return STATUS_DEVICE_NOT_READY;
    if (Device->SourceVisible || !Device->DcnBlanked ||
        !GfxSubmitReady(Device) || !GfxPagingSubmitReady(Device))
        return STATUS_DEVICE_NOT_READY;
    KeAcquireSpinLock(&wddm->Lock,&irql);
    if (!wddm->RetainedPowerPause || !wddm->Stopping || !WddmPowerIdleLocked(wddm)) {
        KeReleaseSpinLock(&wddm->Lock,irql);
        return STATUS_INVALID_DEVICE_STATE;
    }
    // Notification intent survives suspend. Hardware blanking does not mean
    // timing stopped; leave pixels black until the OS supplies its first frame.
    if (wddm->VSyncEnabled && Device->VidPnFlipEnabled)
        status=DcnVsyncEnable(Device,TRUE);
    if (NT_SUCCESS(status)) {
        wddm->VSyncLast=KeQueryPerformanceCounter(&wddm->VSyncFrequency);
        wddm->RetainedPowerPause=FALSE;
        wddm->Stopping=FALSE;
        if (wddm->VSyncEnabled && !Device->VidPnFlipEnabled) {
            due.QuadPart=-((LONGLONG)BC250_WDDM_VSYNC_MS*10000);
            wddm->VSyncArmed=TRUE;
            KeSetTimerEx(&wddm->VSyncTimer,due,BC250_WDDM_VSYNC_MS,&wddm->VSyncDpc);
        }
    }
    KeReleaseSpinLock(&wddm->Lock,irql);
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
    // still on the ring gets a bounded moment to finish. PASSIVE_LEVEL. BC250_WDDM_STOP_DRAIN_MS ends the wait;
    // before BD-114 that bound was the watchdog's own 500 ms plus 100, and the watchdog could therefore end it
    // first. The diagnostic timer no longer closes admission; stop has its own deadline.
    {
        LARGE_INTEGER tick;
        ULONG waited;

        tick.QuadPart = -10000ll * 10;
        for (waited = 0; waited < BC250_WDDM_STOP_DRAIN_MS && wddm->HwPending; waited += 10)
        {
            WddmGpuFence(Device);
            if (wddm->HwPending) KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (waited != 0) GuardLog("wddm: stop waited %u ms for the packet in flight (%s)", waited, wddm->HwPending ? "STILL PENDING" : "done");

        // ADR 0008 stage D: node 1's own packet in flight, waited for independently - it may still be on SDMA0's
        // ring after node 0's has long finished (design note section 5).
        for (waited = 0; waited < BC250_WDDM_STOP_DRAIN_MS && wddm->PagingHead; waited += 10)
        {
            WddmGpuFencePaging(Device);
            if (wddm->PagingHwPending) KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (waited != 0) GuardLog("wddm: stop waited %u ms for node 1's packet in flight (%s)", waited,
                                  wddm->PagingHwPending ? "STILL PENDING" : "done");

        // Node 2's job in flight, on its own ring and its own bound.
        for (waited = 0; waited < BC250_WDDM_STOP_DRAIN_MS && wddm->ComputeHwPending; waited += 10)
        {
            WddmGpuFenceCompute(Device);
            if (wddm->ComputeHwPending) KeDelayExecutionThread(KernelMode, FALSE, &tick);
        }
        if (waited != 0) GuardLog("wddm: stop waited %u ms for node 2's job in flight (%s)", waited,
                                  wddm->ComputeHwPending ? "STILL PENDING" : "done");
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
    if (Device->DcnVsyncArmed != 0) (void)DcnVsyncEnable(Device, FALSE);
    KeReleaseSpinLock(&wddm->Lock, irql);
    // KMD196: after Stopping, before anything is freed. A held submission wakes, sees Stopping and drops its own
    // deadline; WddmHoldWait reads that flag under the lock, so the order here is the order it observes. A
    // waiter that had already read Stopping as FALSE cannot sleep through this either: it finds the generation
    // moved past its snapshot and retests instead of waiting, and WddmSubmitHardware then refuses it.
    GfxRetireSignal(Device);

    KeCancelTimer(&wddm->VSyncTimer);   // again, unconditionally: cheap, and it cannot be armed any more
    KeCancelTimer(&wddm->SubmitTimer);
    KeCancelTimer(&wddm->PagingSubmitTimer);
    KeCancelTimer(&wddm->PagingDrainTimer);
    KeCancelTimer(&wddm->ComputeTimer);
    KeRemoveQueueDpc(&wddm->SubmitDpc);
    KeRemoveQueueDpc(&wddm->PagingSubmitDpc);
    KeRemoveQueueDpc(&wddm->PagingDrainDpc);
    KeRemoveQueueDpc(&wddm->ComputeDpc);
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
    // until the next boot. No lock is needed now, nothing else can reach the list. The list is the drain's one
    // owner of every object; the index only points into it and goes as a whole, after the last object.
    while (!IsListEmpty(&wddm->Objects))
    {
        entry = RemoveHeadList(&wddm->Objects);
        WddmReleaseCaptures(CONTAINING_RECORD(entry, BC250_WDDM_OBJECT, Link));
        ExFreePoolWithTag(CONTAINING_RECORD(entry, BC250_WDDM_OBJECT, Link), BC250_WDDM_TAG);
        freed++;
    }
    GuardLog("wddm: stop, last completed fence %ld, %lu vsync ticks, %lu objects freed at the stop",
             wddm->LastCompletedFence, (ULONG)wddm->VSyncTicks, freed);
    // The GDI totals of this adapter's lifetime, because the per-call lines above cover only the
    // first calls and every overflow. Three lines and a line for each refusal class that happened:
    // the log ring's line is 160 bytes and a %lld can take 20 of them (tools\quality\guardlog_width.py).
    if (wddm->GdiRenderCalls != 0)
    {
        int klass;

        GuardLog("wddm: gdi totals calls%lld commands%lld items%lld dwords%lld",
                 wddm->GdiRenderCalls, wddm->GdiRenderCommands, wddm->GdiRenderItems,
                 wddm->GdiRenderDwords);
        // The dwords above are what the batches generated. These are the ones that left: a call
        // dropped after its stream was written counts in the first number and not in this one.
        GuardLog("wddm: gdi committed dwords%lld", wddm->GdiRenderDwordsCommitted);
        GuardLog("wddm: gdi drops call%lld malformed%lld outside%lld empty%lld",
                 wddm->GdiRenderRefusedCall, wddm->GdiRenderMalformed, wddm->GdiRenderOutside,
                 wddm->GdiRenderEmpty);
        GuardLog("wddm: gdi overflow%lld item-refused%lld red-first%lld", wddm->GdiRenderOverflow,
                 wddm->GdiRenderItemRefused, wddm->GdiRenderColorFillRedFirst);
        GuardLog("wddm: gdi batch-refused%lld fallback%lld peak rects%lld dwords%lld",
                 wddm->GdiRenderBatchRefused, wddm->GdiRenderFallback,
                 wddm->GdiRenderPeakSubRects, wddm->GdiRenderPeakDwords);
        GuardLog("wddm: gdi capacity-short%lld peak cmdlen%lld want%ld omitted subrects%lld",
                 wddm->GdiRenderCapacityShort, wddm->GdiRenderPeakCommandLength,
                 wddm->GdiCmdBytesWanted, wddm->GdiRenderOmittedSubRects);
        // Rule 3 and the scope self-checks. ib-full is a call that found no room left in a buffer
        // earlier calls had filled, and it dropped that paint; ib-over, dma-short and dma-lost are
        // states the design says cannot happen. scope-refused counts the drops of an ordinary
        // producer's paint, whatever dropped it, and a nonzero value is a correctness result.
        GuardLog("wddm: gdi ib-full%lld ib-over%lld dma-short%lld",
                 wddm->GdiRenderIbFull, wddm->GdiRenderIbOverflow, wddm->GdiRenderDmaShort);
        GuardLog("wddm: gdi above-producer%lld scope-refused%lld",
                 wddm->GdiRenderAboveProducer, wddm->GdiRenderScopeRefused);
        // Where the calls lay in their DMA buffers (Bc250GdiDmaClaim): how many continued a buffer,
        // how many found one offered again from its start, how many could not be placed at all and
        // were dropped, and how many submissions released a buffer of this context.
        GuardLog("wddm: gdi dma append%lld restart%lld submitted%lld",
                 wddm->GdiRenderDmaAppend, wddm->GdiRenderDmaRestart, wddm->GdiRenderDmaSubmitted);
        GuardLog("wddm: gdi dma unknown%lld full%lld lost%lld",
                 wddm->GdiRenderDmaUnknown, wddm->GdiRenderDmaFull, wddm->GdiRenderDmaLost);
        // The class numbers are bc250_gdi_refusal's own order, so a reader needs the header and
        // nothing else. A class that never happened says nothing, which keeps the stop short.
        for (klass = 0; klass < (int)Bc250GdiRefusals; klass++)
            if (wddm->GdiRenderRefused[klass] != 0)
                GuardLog("wddm: gdi refusal class %d count %lld", klass,
                         wddm->GdiRenderRefused[klass]);
    }
    ExFreePoolWithTag(wddm->ObjectIndex.Buckets, BC250_WDDM_TAG);
    ExFreePoolWithTag(wddm, BC250_WDDM_TAG);
}

// Called from Bc250DpcRoutine after WddmReport queued it. The scheduler has to hear about the same event a second
// time at DPC level (LEARN nc-d3dkmddi-dxgkcb_notify_interrupt).
void WddmDpc(_Inout_ BC250_DEVICE* Device)
{
    if (Device->Wddm == NULL || Device->Dxgk.DxgkCbNotifyDpc == NULL) return;
    // Never the same pass for a completion: Bc250DpcRoutine reads the fence and queues the report DPC, so whatever
    // of the completion path this call carries was published by an earlier pass. That hop is what NotifyDpcInReport
    // removes (C50). A CRTC_VSYNC report of this same pass (WddmDcnVsync, just above in Bc250DpcRoutine) is paired
    // here too; it is counted on its own counters, which carry no same-pass classification.
    WddmNotifyDpcNow(Device, FALSE);
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
    // Stage 4 handoff storage is owned from start to stop, outside all VidMm
    // allocations. Both the private caps and QUERYSEGMENT use this same layout.
    if (Device->DisplayReservationEnd>framebufferEnd)
        framebufferEnd=Device->DisplayReservationEnd;
    // The reservation must lie inside the usable VRAM before any rounding arithmetic on it: a reservation near
    // the top of the address space would otherwise underflow the overflow guard below and round to a plausible
    // layout.
    if (framebufferEnd>=limit) return FALSE;
    // With large pages the segment starts on a 1 GiB physical boundary. Without AllowNonAlignedLargePageAddress,
    // VidMm sets LargePage only on an address aligned to the leaf table coverage (DXGK_GPUMMUCAPS), and that
    // address is a segment offset. The translation needs a 2 MiB page 2 MiB aligned and a 1 GiB page 1 GiB aligned,
    // in the physical address, which is the base plus the offset. With the base on 1 GiB, an aligned offset is an
    // aligned address for both pages. The VRAM below the boundary is not offered.
    if (Device->LargePageLayout) {
        ULONGLONG base=(ULONGLONG)Device->VramPhysical.QuadPart;
        if (base>MAXULONGLONG-framebufferEnd-(BC250_VIDMM_GIANT_PAGE_BYTES-1)) return FALSE;
        framebufferEnd=((base+framebufferEnd+BC250_VIDMM_GIANT_PAGE_BYTES-1)&~(BC250_VIDMM_GIANT_PAGE_BYTES-1))-base;
    }
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

// The private UMD report must exclude the same POST buffer, private tail and
// page-table storage as the application segment in QUERYSEGMENT4. Geometry is
// start-latched: this query does not access registers or change a reservation.
// Both words describe the same fully CPU-visible pool. The captured GTT word is
// not a Windows host-memory capacity: the winsys must use GETSEGMENTSIZE's
// SharedSystemMemorySize. The aperture window is not that system-memory pool
// (WDDM 2.0 GPU segments, AccessedPhysically=0).
static void VramPatchCaps(_In_ const BC250_DEVICE* Device,
                         _Inout_updates_bytes_(Bytes) PVOID Caps, ULONG Bytes)
{
    ULONGLONG offset,length,tableOffset,tableLength;
    if (Caps==NULL || Bytes<UMD_CAPS_BYTES) return;
    if (!WddmMemoryLayout(Device,&offset,&length,&tableOffset,&tableLength) ||
        !PagingApertureBytesValid(Device->WddmAperture.bytes)) length=0;
    RtlCopyMemory((PUCHAR)Caps+UMD_CAPS_VRAM_TOTAL_OFFSET,&length,sizeof(length));
    RtlCopyMemory((PUCHAR)Caps+UMD_CAPS_VISIBLE_VRAM_TOTAL_OFFSET,&length,sizeof(length));
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

    // Geometry is captured before publishing WDDM state; queries never run setup. Since 0.7.216.8 the size is
    // whatever WddmStart's capture granted (ApertureSegmentMegabytes, 256 to 447 MiB), so the test is "a valid
    // captured aperture exists" rather than "it is 256 MiB": a start without one still offers no aperture.
    if (count != 0 && !PagingApertureBytesValid(Device->WddmAperture.bytes)) {
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
        // Revision 183: system memory reached through the aperture counts against the NON-local budget group
        // (d3dkmddi.h DXGK_SEGMENTFLAGS). With no segment in that group dxgkrnl reported a UMA-style budget:
        // local 11339 MiB = segments 1 + 3 + SharedSystemMemory - 768 MiB, non-local 0. A budget-sized client
        // then overflowed segment 1 by up to 3.4 GiB, and VidMm evicted into the RAM the OS runs on (trial 211,
        // K48). Placement is unchanged: segment ids, segment sets and the paging buffer segment stay as they were.
        descriptor->Flags.NonLocalBudgetGroup = 1;
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
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
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
    //  - PerEngineTDR: the three DDIs are in the table. ResetEngine soft-recovers node 0 under HangRecoveryMode
    //    (M15.12 stage 1, M835, M836) and refuses every other case. ResetFromTimeout always fails, because the
    //    part has no proven reset (M53).
    //  - SmoothRotation: "must support updating path rotation in UpdateActiveVidPnPresentPath"; ours does, and
    //    the only rotation it offers is identity.
    //  - DirectFlip: held while the DirectFlip handshake is on (M15.14 increment 2, EnableDirectFlipHandshake
    //    and the scan-out gate). The compositor's user-mode driver answers CheckDirectFlipSupport from the
    //    scan-out caps trailer, and SetVidPnSourceAddress scans out a primary that Bc250ScanoutAdmit admits
    //    (M844). With the handshake off no client is offered a flip.
    caps->SupportSmoothRotation = TRUE;
    caps->SupportPerEngineTDR = TRUE;
    caps->SupportDirectFlip = TRUE;
    caps->SupportSurpriseRemoval = FALSE;
    caps->InterruptMessageNumber = 0;                   // the INF asks for one MSI message, so it is message 0 (M38)

    // Kernel-mode GDI acceleration: every one of these is a "cannot", and they have to be said out loud, because a
    // zeroed DXGK_PRESENTATIONCAPS claims the driver *can* do all of them. Present builds work only for a plain
    // Blt (no present flag but Blt): a GPU IB behind EnableGpuPresentBlit (WddmBuildGpuPresent), or the diagnostic
    // CPU blit packet behind EnablePresentBlit. With neither switch on, a Present writes no DMA and returns
    // success, so a blt dxgkrnl believed in would be silently dropped and show as corruption on screen.
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
    // An explicit trial setting, independent of producer selection so the
    // existing CPU diagnostic path can first identify the CDD's actual shapes.
    // Never infer this capability merely from successful GPU submissions.
    if (Device->Wddm && ((BC250_WDDM*)Device->Wddm)->CddDwmInterop) {
        caps->PresentationCaps.DriverSupportsCddDwmInterop = 1;
        // Match the hosted frontend's maximum shared texture extent (8192).
        caps->PresentationCaps.MaxTextureWidthShift = 2;
        caps->PresentationCaps.MaxTextureHeightShift = 2;
    }
    // HLK gap G1: GDI hardware acceleration, "a required feature starting with WDDM 1.1", mandatory
    // for a full graphics and for a render-only driver in the feature caps table
    // (display/wddm-driver-and-feature-caps.md:40) and tested by the HLK job "GDIHW_Functional test
    // for GDI HW ACC (v1.1)". docs/design/gdi-hw-accel.md; the translator is Bc250WddmRenderGdi.
    //   DriverSupportsCddDwmInterop is the alternative to it and not a companion: d3dkmddi.h reads
    // that bit as "Driver does not support hardware GDI acceleration, but supports Cdd-Dwm interop",
    // so the two are answered as the exclusive pair they are, and today's deployed answer (interop,
    // no acceleration) is what a start without the kernels keeps.
    //   The gdi-hardware-acceleration page adds one condition of its own: "The driver should report
    // this type of support only if the cache-coherent GPU aperture segment exists and there's no
    // significant performance penalty when the CPU accesses GPU memory." The aperture segment is
    // declared CacheCoherent (which is also why NoCacheCoherentApertureMemory stays 0), and on this
    // SoC the CPU and the GPU share the same DRAM, so the second half holds by construction.
    if (wddm != NULL && wddm->GdiHwAccel) {
        caps->PresentationCaps.SupportKernelModeCommandBuffer = 1;
        caps->PresentationCaps.DriverSupportsCddDwmInterop = 0;
        // 8192 x 8192, the extent gdi_render.c admits (its GDI_MAX_EXTENT), stated here whether or
        // not the interop answer above already asked for it.
        caps->PresentationCaps.MaxTextureWidthShift = 2;
        caps->PresentationCaps.MaxTextureHeightShift = 2;
        // The ClearType blend reads the destination and writes it in one kernel, so the OS need not
        // hand us a temporary surface to read it from; TmpSurfAllocationIndex is then never read.
        caps->PresentationCaps.NoTempSurfaceForClearTypeBlend = 1;
        // Left clear on purpose, each one a "cannot" that the admission in gdi_render.c enforces:
        //  - SupportAllBltRops: no ROP3 in BitBlt or ColorFill (the 256 ternary operations need the
        //    brush the DDI does not describe here).
        //  - SupportMirrorStretchBlt, SupportMonoStretchBltModes: no mirrored stretch, and no
        //    BLACKONWHITE/WHITEONBLACK, so only COLORONCOLOR arrives.
        //  - SupportSoftwareDeviceBitmaps: type-6 CPU-visible redirection bitmaps stay refused at
        //    CreateAllocation (BD-060, WddmGdiAllocationPolicy refuses type > 4).
        //  - SupportLinearHeap, StagingRectStartPitchAligned: no linear heap out of a staging
        //    surface, and no pitch-aligned staging rectangle requirement.
        // The No*SameBitmap* family above stays set: one allocation as both source and destination
        // is the one case this driver refuses outright (gdi_render.c, Bc250GdiBadSameSurface).
    }


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

    // nodeCount (1 with the gate closed, 2 open, 3 with node 2) replaces the old compile-time constant here; the six per-node
    // fence fields in BC250_WDDM are arrays now for exactly this reason (design note section 5).
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = nodeCount;

    // Both zero means "use the entire available VA range", which is what we want while nothing reserves any of it.
    caps->InternalGpuVirtualAddressRangeStart = 0;
    caps->InternalGpuVirtualAddressRangeEnd = 0;

    // The flip model of a full miniport: DxgkDdiPresent produces no DMA and SetVidPnSourceAddress does the flip.
    // FlipIndependent: "WDDM 1.3 driver must support independent flip." is a refusal in the lab's dxgkrnl (behind
    // the same enforcement switch as DirectFlip). What it promises is that SetVidPnSourceAddress may be pointed at
    // a surface DWM did not present; ours takes whatever address it is given.
    // FlipImmediateMmIo (BD-170): "the driver must support an immediate flip and a flip interval of one"
    // (DXGK_FLIPCAPS). Without this cap an immediate flip (an exclusive-fullscreen present at SyncInterval 0) is a
    // DMA-buffer flip: dxgkrnl passes Present a DMA buffer for it, ours wrote none, and the scheduler marked the
    // device as an error (DxgKrnl VidSchMarkDeviceAsError reason 19 after its "DMABufferLength > 0" assertion), which
    // the application saw as DXGI_ERROR_DEVICE_HUNG on its first Present (a02). With it the flip comes to
    // SetVidPnSourceAddress with Flags.FlipImmediate, and the cap's promise is kept there: the flip takes effect
    // without waiting for a vsync (d3dkmddi.md:21235-21239). DcnFlipSourceAddress programs SURFACE_FLIP_TYPE 1 for
    // it, and DcnImmediateObserve (dcn.c) samples P', bounded, before the DDI returns, because dxgmms2 takes an
    // immediate flip as complete at a success return (dcn.c says where that is read). No answer is busy: the state
    // machine of docs/design/vidpn-flip.md section 7.2 answers every request (Bc250WddmSetVidPnSourceAddress). A
    // pending vsync flip to the same address is promoted, and a flip that still arrives with a DMA buffer
    // is refused (WddmPresentDmaFlip). The cap stands with the flip gate closed too (EnableVidPnFlip): then nothing
    // is programmed or observed and the DDI returns success, as for every flip. docs/design/vidpn-flip.md section 7.
    caps->FlipCaps.FlipOnVSyncMmIo = 1;
    caps->FlipCaps.FlipImmediateMmIo = 1;
    caps->FlipCaps.FlipIndependent = 1;
    caps->MaxQueuedFlipOnVSync = 1;

    // No hardware pointer (MaxPointerWidth/Height stay 0): dxgkrnl draws the cursor into the image it presents,
    // exactly as in the display-only build.

    // Successful replies, not merely the gate latched at start. Pre-start
    // replies have no adapter-owned storage and are identified in the log.
    if (wddm) {
        UINT interop=caps->PresentationCaps.DriverSupportsCddDwmInterop ? 1 : 0;
        LONG64 now=(LONG64)KeQueryInterruptTime(),previous;
        // Keep chronological bounds even when concurrent replies publish in
        // reverse order. Each field is atomic, not a transactional snapshot.
        do {
            previous=InterlockedCompareExchange64(&wddm->DriverCapsFirstTime[interop],0,0);
            if (previous && previous<=now) break;
        } while (InterlockedCompareExchange64(&wddm->DriverCapsFirstTime[interop],now,previous)!=previous);
        do {
            previous=InterlockedCompareExchange64(&wddm->DriverCapsLastTime[interop],0,0);
            if (previous>=now) break;
        } while (InterlockedCompareExchange64(&wddm->DriverCapsLastTime[interop],now,previous)!=previous);
        InterlockedIncrement64(&wddm->DriverCapsInteropReturned[interop]);
    }
    GuardLog("wddm: DRIVERCAPS reply started%u interop%u extent-shifts%u/%u",
        wddm!=NULL,caps->PresentationCaps.DriverSupportsCddDwmInterop,
        caps->PresentationCaps.MaxTextureWidthShift,caps->PresentationCaps.MaxTextureHeightShift);

    // What was promised, and into how large a structure: the size says which DXGK_DRIVERCAPS this dxgkrnl thinks
    // it is talking to. A cap that is wrong but accepted leaves no other trace (E16 run 1). Worst case 139 of
    // the 160 bytes of a log line: count before adding a field. Our own sizeof and the paging node are not in
    // it: both are constants of the build (592 = 0x250 at interface 0xE003, which is what the lab's dxgkrnl offers
    // to a table declaring >= 0xE003; it was 576 at 0x5023; node 0).
    if (WddmAnswersLogged(Device))
        GuardLog("wddm: DRIVERCAPS into %u bytes: wddm %u sched 0x%X mm 0x%X flip 0x%X slots %u "
                 "tdr %u dflip %u rot %u",
             Query->OutputDataSize, (ULONG)caps->WDDMVersion, caps->SchedulingCaps.Value,
             caps->MemoryManagementCaps.Value, caps->FlipCaps.Value,
             caps->MaxAllocationListSlotId, caps->SupportPerEngineTDR ? 1u : 0u, caps->SupportDirectFlip ? 1u : 0u,
             caps->SupportSmoothRotation ? 1u : 0u);
    return STATUS_SUCCESS;
}

static NTSTATUS WddmGpuMmuCaps(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    DXGK_GPUMMUCAPS* caps = (DXGK_GPUMMUCAPS*)Query->pOutputData;

    if (Query->OutputDataSize < sizeof(*caps) || caps == NULL) return STATUS_BUFFER_TOO_SMALL;
    RtlZeroMemory(caps, Query->OutputDataSize);
    // Page directories live in a local memory segment, where CPU_VIRTUAL is documented as not allowed.
    // bc250_pte_from_dxgk preserves CacheCoherent as AMDGPU_PTE_SNOOPED for
    // system pages. Advertise this when requesting cached GTT backing store.
    caps->CacheCoherentMemorySupported = 1;
    // Native GFX10 PRT terminal encoding exists at all four levels.
    caps->ZeroInPteSupported = 1;
    caps->PageTableUpdateMode = DXGK_PAGETABLEUPDATE_GPU_PHYSICAL;
    caps->VirtualAddressBitCount = BC250_WDDM_VA_BITS;
    caps->PageTableLevelCount = BC250_WDDM_LEVEL_COUNT;
    caps->LeafPageTableSizeFor64KPagesInBytes = 0;      // stage A declares no 64 KB pages on the segment
    // "All levels of page tables, except the leaf one, support large pages": vidmm.c maps level 1 (2 MiB) and
    // level 2 (1 GiB). Level 3 would need a 512 GiB allocation, which no segment here can hold. Large pages in
    // host memory (SysMemLargePageSupported) and on unaligned addresses (AllowNonAlignedLargePageAddress) stay off.
    caps->LargePageSupported = Device->LargePages ? 1 : 0;
    return STATUS_SUCCESS;
}

// DXGKQAITYPE_PHYSICALADAPTERCAPS. dxgkrnl tolerates a refusal, but then builds the physical-adapter caps itself
// from DRIVERCAPS members this driver leaves zero, and takes the FDO instead of our handle (static reading of
// 26100.9549, BD-134). The answer repeats DRIVERCAPS: the same node count and paging node, GpuMmu, and the handle
// dxgkrnl gave StartDevice. One physical adapter, no LDA.
static NTSTATUS WddmPhysicalAdapterCaps(_In_ const BC250_DEVICE* Device, _In_ const DXGKARG_QUERYADAPTERINFO* Query)
{
    const DXGK_QUERYPHYSICALADAPTERCAPSIN* in = (const DXGK_QUERYPHYSICALADAPTERCAPSIN*)Query->pInputData;
    DXGK_PHYSICALADAPTERCAPS* caps = (DXGK_PHYSICALADAPTERCAPS*)Query->pOutputData;
    const BC250_WDDM* wddm = (const BC250_WDDM*)Device->Wddm;
    UINT nodeCount = (wddm != NULL) ? wddm->NodeCount : BC250_WDDM_NODE_COUNT;

    if (Query->InputDataSize < sizeof(*in) || in == NULL) return STATUS_INVALID_PARAMETER;
    if (Query->OutputDataSize < sizeof(*caps) || caps == NULL) return STATUS_BUFFER_TOO_SMALL;
    if (in->PhysicalAdapterIndex != 0) return STATUS_INVALID_PARAMETER;
    // The handle exists only after StartDevice; a zero handle would be a wrong answer, not a cautious one.
    if (Device->Dxgk.DeviceHandle == NULL) return STATUS_DEVICE_NOT_READY;
    // The caller's buffer, not our sizeof: the WDDM 2.7 VirtualCopyNodeIndex tail stays zero, no virtual copy node.
    RtlZeroMemory(caps, Query->OutputDataSize);
    caps->NumExecutionNodes = (WORD)nodeCount;
    caps->PagingNodeIndex = (WORD)((nodeCount > BC250_WDDM_NODE_COPY) ? BC250_WDDM_NODE_COPY : BC250_WDDM_NODE_3D);
    caps->DxgkPhysicalAdapterHandle = Device->Dxgk.DeviceHandle;
    caps->Flags.GpuMmuSupported = 1;
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
        status = WddmGpuMmuCaps(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_PAGETABLELEVELDESC:
        status = WddmPageTableLevelDesc(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_PHYSICALADAPTERCAPS:
        status = WddmPhysicalAdapterCaps(device, QueryAdapterInfo);
        break;
    case DXGKQAITYPE_64BITONLYCAPS:
        // Four bytes and no WDK structure. dxgkrnl ignores a refusal; the answer sets only SupportsOnly64Bit (static
        // reading of 26100.9549, BD-134). Zero: this driver has 32-bit user-mode drivers too. VirtualBox's WDDM
        // miniport also answers this type with a zeroed buffer.
        if (QueryAdapterInfo->pOutputData == NULL || QueryAdapterInfo->OutputDataSize < sizeof(UINT)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        RtlZeroMemory(QueryAdapterInfo->pOutputData, QueryAdapterInfo->OutputDataSize);
        status = STATUS_SUCCESS;
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
    {
        struct bc250_umd_firmware firmware;
        ULONG smuVersion;
        // Keep the measured hardware template, replacing its historical
        // firmware replies with this session's successful load/start snapshot.
        // Both helpers only copy caches; no file or mailbox I/O in this DDI.
        if (QueryAdapterInfo->pOutputData == NULL) { status = STATUS_INVALID_PARAMETER; break; }
        if (!Bc250CapsQueryExtentSupported(QueryAdapterInfo->OutputDataSize)) {
            status = STATUS_INVALID_PARAMETER;
            break;
        }
        if (QueryAdapterInfo->OutputDataSize < UMD_CAPS_BYTES) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status=PspReadFirmware(device,&firmware);
        if(!NT_SUCCESS(status))break;
        status=SmuReadFirmwareVersion(&device->Smu,&smuVersion);
        if(!NT_SUCCESS(status))break;
        firmware.smc_version=smuVersion;
        RtlCopyMemory(QueryAdapterInfo->pOutputData, umd_caps_blob, UMD_CAPS_BYTES);
        RtlCopyMemory((PUCHAR)QueryAdapterInfo->pOutputData+UMD_CAPS_FIRMWARE_OFFSET,&firmware,sizeof(firmware));
        // num_cu, and with it RADV's scratch sizing, follows the registers of this start (cumode.c).
        CuModePatchCaps(device,QueryAdapterInfo->pOutputData,UMD_CAPS_BYTES);
        VramPatchCaps(device,QueryAdapterInfo->pOutputData,UMD_CAPS_BYTES);
        // Node 2 is a node a UMD may open a context on and submit to (bc250_umd_private.h, submittable_node_mask).
        if (wddm != NULL && wddm->NodeCount > BC250_WDDM_NODE_COMPUTE)
            UmdCapsOpenNode(QueryAdapterInfo->pOutputData, UMD_CAPS_BYTES, BC250_WDDM_NODE_COMPUTE);
        // DXGK_START_INFO.AdapterLuid is supplied by dxgkrnl at StartDevice.
        // Keep old-sized queries byte-compatible; never emit a partial trailer.
        if (QueryAdapterInfo->OutputDataSize >= BC250_ADAPTER_CAPS_BYTES) {
            struct bc250_adapter_identity identity = {0};
            identity.magic = BC250_ADAPTER_IDENTITY_MAGIC;
            identity.version = BC250_ADAPTER_IDENTITY_VERSION;
            identity.size = sizeof(identity);
            identity.luid_low = device->StartInfo.AdapterLuid.LowPart;
            identity.luid_high = (unsigned int)device->StartInfo.AdapterLuid.HighPart;
            RtlCopyMemory((PUCHAR)QueryAdapterInfo->pOutputData+BC250_ADAPTER_IDENTITY_OFFSET,
                          &identity,sizeof(identity));
        }
        // M15.14 increment 2: the second optional trailer, by the same rule. Two start-latched facts the
        // compositor's user-mode driver must have before it may answer CheckDirectFlipSupport TRUE: the
        // operator's switch (ANDed at WddmStart with the kernel gate and with every start-latched fact
        // the flip path needs) and the source geometry Bc250ScanoutAdmit admits now. That is the committed
        // source mode (display modes, modeset.c), or the POST geometry when none is committed; the field
        // names keep "post" for the ABI. Both are read at each query, so the trailer and the admission
        // change together when a mode is committed. A reader that queried
        // the shorter buffer gets exactly what it got before, and a shell against a driver without this
        // trailer reads zeros and refuses.
        //   Nothing is written at all while the handshake is off, not even a header with flags 0: a start
        // with the switch closed is then byte for byte 0.7.207.1 for every buffer size, and a reader that
        // keys on the magic and forgets the flag cannot act on a closed switch or read a live geometry
        // out of a start that offers no flip.
        if (wddm != NULL && wddm->DirectFlipHandshake &&
            QueryAdapterInfo->OutputDataSize >= BC250_SCANOUT_CAPS_TOTAL) {
            struct bc250_scanout_caps scanout = {0};
            scanout.magic = BC250_SCANOUT_CAPS_MAGIC;
            scanout.version = BC250_SCANOUT_CAPS_VERSION;
            scanout.size = sizeof(scanout);
            scanout.flags = BC250_SCANOUT_CAPS_DIRECT_FLIP;
            // M15.14 (0.7.216.20): the plane takes the table's non-firmware SCANOUT_PRIMARY rows (R8G8B8A8,
            // R10G10B10A2) only after the firmware's format was captured at WddmStart, which ran before this.
            // BD-197: and only while the flip path programs them, which is the rule of DcnPlaneFormatsOpen.
            if (DcnPlaneFormatsOpen(device)) scanout.flags |= BC250_SCANOUT_CAPS_PLANE_FORMATS;
            scanout.post_width = (unsigned int)DisplaySourceWidth(device);
            scanout.post_height = (unsigned int)DisplaySourceHeight(device);
            RtlCopyMemory((PUCHAR)QueryAdapterInfo->pOutputData+BC250_SCANOUT_CAPS_OFFSET,
                          &scanout,sizeof(scanout));
        }
        break;
    }
    default:
        // The pre-WDDM2 segment queries, which a WDDM 2 driver must not answer. Types 15 and 47, which the lab's
        // dxgkrnl asked for in E16 run 003, were refused here until 0.7.216.62 (above). Which types are asked for
        // is stage A evidence, and a refusal here is logged below.
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

    // At interface 0xE003 the member after FriendlyName is DXGK_NODEMETADATA_FLAGS (a reserved UINT32 at 2.0).
    // Zeroing it is the honest answer: no ContextSchedulingSupported (no hardware scheduling), no
    // RingBufferFenceRelease, no SupportTrackedWorkload, no UserModeSubmission. The lab's dxgkrnl reads these flags
    // for a table that declares interface 0x9000 or later (static reading, ADR 0019 B1), so this zero is now a
    // statement and not padding.
    RtlZeroMemory(pGetNodeMetadata, sizeof(*pGetNodeMetadata));
    if (node == BC250_WDDM_NODE_COPY)
    {
        // ADR 0013: node 1, SDMA0, the paging node. GpuMmuSupported TRUE states a hardware fact (SDMA can be
        // pointed at a VMID) - section 2 of the design note chooses not to use one, which is this build's policy,
        // not something worth mis-declaring to dxgkrnl.
        pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_COPY;
        RtlStringCchCopyW(pGetNodeMetadata->FriendlyName, DXGK_MAX_METADATA_NAME_LENGTH, L"BC-250 SDMA0");
    }
    else if (node == BC250_WDDM_NODE_COMPUTE)
    {
        // A compute-only engine. The enum has no compute type and an adapter has exactly one 3-D engine
        // (DXGK_ENGINE_TYPE remarks), so it is OTHER, which requires the FriendlyName.
        pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_OTHER;
        RtlStringCchCopyW(pGetNodeMetadata->FriendlyName, DXGK_MAX_METADATA_NAME_LENGTH, L"BC-250 Compute0");
    }
    else
    {
        pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;     // graphics, and compute when node 2 is closed
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
    // The process too (BD-090): after a runtime restart, whether DWM's process opened a device on the new start.
    if (WddmFirstCalls(wddm, WddmDdiCreateDevice)) GuardLog("wddm: CreateDevice flags 0x%08X pasid %u pid %lu", flags,
        pCreateDevice->Pasid, HandleToULong(PsGetCurrentProcessId()));
    return STATUS_SUCCESS;
}

static DXGKDDI_DESTROYDEVICE Bc250WddmDestroyDevice;
static NTSTATUS Bc250WddmDestroyDevice(_In_ const HANDLE hDevice)
{
    BC250_WDDM_OBJECT* object = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls((BC250_WDDM*)object->Device->Wddm, WddmDdiDestroyDevice)) GuardLog("wddm: DestroyDevice");
    // The last device that used the interop path ends the session (DWM exits before a clean shutdown ends).
    if (InterlockedExchange(&object->InteropUser, 0) == 1) InteropUserEnd(object->Device);
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
        !(pCreateContext->NodeOrdinal == BC250_WDDM_NODE_COPY && nodeCount > BC250_WDDM_NODE_COPY) &&
        !(pCreateContext->NodeOrdinal == BC250_WDDM_NODE_COMPUTE && nodeCount > BC250_WDDM_NODE_COMPUTE))
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
    object->OwnerDevice = hDevice;
    object->NodeOrdinal = pCreateContext->NodeOrdinal;
    object->SystemContext = (BOOLEAN)pCreateContext->Flags.SystemContext;
    object->GdiContext = (BOOLEAN)pCreateContext->Flags.GdiContext;   // BD-167 (V2): the context class dxgkrnl assigned
    // HLK gap G1, before any branch that could use it: every GDI context carries the map of rule 3 (gdi_batch.h),
    // whether or not this start reports the capability.
    if (object->GdiContext) KeInitializeSpinLock(&object->GdiDmaLock);
    // KMD193: every BC250_PJ_GFX_SUBMIT record of a job on this context carries this process ID, so that a
    // faulting job in a dump names the process that owns the context rather than only a pointer value.
    object->CreatorProcessId = HandleToULong(PsGetCurrentProcessId());
    if (umd)
    {
        object->UmdContext = TRUE;
        object->UmdIpType = umdView.ip_type;
        if (!object->SystemContext) {
            NTSTATUS journalStatus = HipJournalOwnerCreate(&object->HipJournalOwner, parent->Device, object, hDevice);
            if (!NT_SUCCESS(journalStatus)) { WddmFreeObject(object); return journalStatus; }
        }
    }
    // BD-127: the compute statement of an accepted blob, counted after the last path that can still fail, so the
    // count never carries a context that was never created. DestroyContext counts it out again; dxgkrnl destroys the
    // contexts of a dying process, so a killed compute client needs no process tracking of ours. The count selects
    // the DPM governor's compute threshold set and nothing else: it changes no submission and no allocation.
    if (umd && umdView.compute)
    {
        object->ComputeContext = TRUE;
        DpmComputeContextAdd(parent->Device, 1);
    }
    pCreateContext->hContext = object;

    RtlZeroMemory(&pCreateContext->ContextInfo, sizeof(pCreateContext->ContextInfo));
    // Stage A submits nothing, so the DMA buffer it asks for is the smallest that is still a buffer, and there is
    // no allocation or patch-location list to keep: with virtual addressing there is no patching to do at all.
    // One page, the size both reference drivers use (ROSD_COMMAND_BUFFER_SIZE, COS_COMMAND_BUFFER_SIZE).
    pCreateContext->ContextInfo.DmaBufferSize = PAGE_SIZE;
    // HLK gap G1: a GDI context's DMA buffer is the one this driver writes commands into, and the
    // guaranteed contract DMA buffer model leaves no second chance - "the display miniport driver
    // must be able to translate one command buffer into exactly one DMA buffer", and a driver that
    // answers out-of-space bugchecks the system. One page would hold 145 DMA_DATA rows; this asks
    // for the size docs/design/gdi-hw-accel.md derives instead, and only while the capability is
    // reported, so a start without it allocates exactly what it did before. GdiContext was set above.
    if (object->GdiContext && parentWddm != NULL && parentWddm->GdiHwAccel)
    {
        // The bound of this context: the largest this driver can translate inside its own DMA
        // ceiling, stepped down to what the aperture of this start can afford, and raised to what
        // an earlier short call asked for if that is larger and still affordable. The aperture is
        // the segment the DMA buffer is resident in for every submission, which is why its size has
        // a say. Bc250GdiContextBound never answers 0 or a bound the next line cannot meet.
        ULONG bound = Bc250GdiContextBound(parent->Device->WddmAperture.bytes,
                                           (ULONG)parentWddm->GdiCmdBytesWanted);
        ULONG bytes = Bc250GdiContextDmaBytes(bound);
        if (bytes != 0) pCreateContext->ContextInfo.DmaBufferSize = bytes;
        // What was asked for, so that a call at the start of a buffer can check the floor the DDI
        // promises (GdiRenderDmaShort). The OS may allocate more and may grow the buffer later;
        // neither is a problem, because the capacity of each call comes from the space that call
        // reports.
        object->GdiDmaBytes = (ULONG)pCreateContext->ContextInfo.DmaBufferSize;
        GuardLog("wddm: CreateContext: GDI dma %lu bytes for cmdbuf %lu of aperture %llu MiB",
                 bytes, bound, parent->Device->WddmAperture.bytes / PAGING_APERTURE_MIB);
    }
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
    if(!umd && pCreateContext->NodeOrdinal==BC250_WDDM_NODE_COPY) {
        pCreateContext->ContextInfo.DmaBufferSize=PAGING_PRIVATE_DMA_BYTES;
        pCreateContext->ContextInfo.DmaBufferPrivateDataSize=PAGING_PRIVATE_BUFFER_BYTES;
    }
    pCreateContext->ContextInfo.PatchLocationListSize = 0;
    // Unconditional (contract rule R37): the table has no DxgkDdiPatch and no patch-location list, so a context that
    // said "patch me" would meet a 0x113 later; nothing here can patch, whatever the flags say.
    pCreateContext->ContextInfo.Caps.NoPatchingRequired = 1;
    // Matches DRIVERCAPS.MemoryManagementCaps.PagingNode above: node 1 once the gate names it the paging node,
    // node 0 (today's answer) otherwise.
    pCreateContext->ContextInfo.PagingCompanionNodeId = (nodeCount > BC250_WDDM_NODE_COPY) ? BC250_WDDM_NODE_COPY : BC250_WDDM_NODE_3D;

    if (umd && parentWddm != NULL && InterlockedIncrement(&parentWddm->UmdContexts) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: umd context node %u ip %u %s, private slot %u", pCreateContext->NodeOrdinal,
                 umdView.ip_type, umdView.compute ? "compute" : "graphics", (ULONG)UMD_BLOB_SUBMIT_BYTES);
    // KMD193: one line per context, capped like every other first-calls log, so that the context value in a
    // GFX_SUBMIT journal record or a Blt observation can be traced back to a process and a kind.
    if (parentWddm != NULL && InterlockedIncrement(&parentWddm->ContextsLogged) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: context %p pid %lu node %u umd %u system %u ip %lu", object, object->CreatorProcessId,
                 object->NodeOrdinal, (UINT)object->UmdContext, (UINT)object->SystemContext, object->UmdIpType);
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
    // BD-127: out of the adapter's live compute-context count, before the object is freed. Exactly the contexts
    // CreateContext counted in, so the count returns to zero when the last compute client's contexts are gone and
    // the governor's graphics thresholds come back.
    if (object->ComputeContext)
    {
        object->ComputeContext = FALSE;
        DpmComputeContextAdd(object->Device, -1);
    }
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
    BOOLEAN first = WddmFirstCalls(WddmOf(hAdapter), WddmDdiSetRootPageTable);
    // The OS address is a segment and offset, resolved below for this context.
    if (first)
        GuardLog("wddm: SetRootPageTable segment %u offset 0x%llX, %u entries", pSetPageTable->Address.SegmentId,
                 pSetPageTable->Address.SegmentOffset, pSetPageTable->NumEntries);
    VidMmSetRootPageTable(pSetPageTable);
    // Stage C: the context remembers where its page tables start; gfx.c points the VMID there before its packet.
    {
        BC250_WDDM_OBJECT* context = WddmObject(pSetPageTable->hContext, BC250_WDDM_MAGIC_CONTEXT);
        ULONGLONG physical = 0;
        BOOLEAN resolved = FALSE;

        if (context != NULL) {
            resolved = VidMmRootPhysical(&pSetPageTable->Address, &physical);
            context->RootPhysical = resolved ? physical : 0;
        }
        if (first && KeGetCurrentIrql() <= DISPATCH_LEVEL)
            GuardLog("wddm: root binding handle%p ctx%p owner%p resolved%u physical%llX irql%u",
                pSetPageTable->hContext, (void*)context, context ? (void*)context->OwnerDevice : NULL,
                (UINT)resolved, physical, (UINT)KeGetCurrentIrql());
    }
}

// ---- allocations ------------------------------------------------------------------------------------------------

C_ASSERT(D3DKMDT_GDISURFACE_TEXTURE==1);
C_ASSERT(D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE==2);
C_ASSERT(D3DKMDT_GDISURFACE_STAGING==3);
C_ASSERT(D3DKMDT_GDISURFACE_LOOKUPTABLE==4);
C_ASSERT(D3DKMDT_GDISURFACE_TEXTURE_CPUVISIBLE_CROSSADAPTER==8);
C_ASSERT(D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE==BC250_STDALLOC_PRIMARY);
C_ASSERT(D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE==BC250_STDALLOC_SHADOW);
C_ASSERT(D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE==BC250_STDALLOC_STAGING);
C_ASSERT(D3DKMDT_STANDARDALLOCATION_GDISURFACE==BC250_STDALLOC_GDI);
// The shared format table and surface_format.h are plain integers; these tie them to the WDK's D3DDDIFORMAT.
C_ASSERT(AMDGPU_WDDM_D3DDDI_A8R8G8B8==D3DDDIFMT_A8R8G8B8);
C_ASSERT(AMDGPU_WDDM_D3DDDI_X8R8G8B8==D3DDDIFMT_X8R8G8B8);
C_ASSERT(AMDGPU_WDDM_D3DDDI_A2B10G10R10==D3DDDIFMT_A2B10G10R10);
C_ASSERT(AMDGPU_WDDM_D3DDDI_A8B8G8R8==D3DDDIFMT_A8B8G8R8);
C_ASSERT(AMDGPU_WDDM_D3DDDI_A16B16G16R16F==D3DDDIFMT_A16B16G16R16F);
C_ASSERT(AMDGPU_WDDM_D3DDDI_A8==D3DDDIFMT_A8);
C_ASSERT(BC250_FORMAT_X8B8G8R8==D3DDDIFMT_X8B8G8R8);

static DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA Bc250WddmGetStandardAllocationDriverData;
static NTSTATUS Bc250WddmGetStandardAllocationDriverData(_In_ const HANDLE hAdapter,
                                                         _Inout_ DXGKARG_GETSTANDARDALLOCATIONDRIVERDATA* pData)
{
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BC250_STDALLOC_REQUEST request;
    BC250_STDALLOC_ANSWER answer;
    BC250_GDI_PRIVATE gdi;
    int status;

    RtlZeroMemory(&request, sizeof(request));
    request.Kind = (ULONG)pData->StandardAllocationType;
    request.Fill = pData->pAllocationPrivateDriverData != NULL;
    request.Bytes = request.Fill ? pData->AllocationPrivateDriverDataSize : 0;
    request.OsBuild = wddm != NULL ? wddm->OsBuildNumber : 0;
    request.StagingAperture = wddm != NULL && wddm->GpuPresentGate && g_ApertureOffered;
    request.ResourceFill = pData->pResourcePrivateDriverData != NULL;
    request.ResourceBytes = request.ResourceFill ? pData->ResourcePrivateDriverDataSize : 0;
    switch (pData->StandardAllocationType)
    {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
        if ((request.Described = pData->pCreateSharedPrimarySurfaceData != NULL) != 0) {
            request.Width = pData->pCreateSharedPrimarySurfaceData->Width;
            request.Height = pData->pCreateSharedPrimarySurfaceData->Height;
            request.Format = (ULONG)pData->pCreateSharedPrimarySurfaceData->Format;
        }
        break;
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
        if ((request.Described = pData->pCreateShadowSurfaceData != NULL) != 0) {
            request.Width = pData->pCreateShadowSurfaceData->Width;
            request.Height = pData->pCreateShadowSurfaceData->Height;
            request.Format = (ULONG)pData->pCreateShadowSurfaceData->Format;
        }
        break;
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
        if ((request.Described = pData->pCreateStagingSurfaceData != NULL) != 0) {
            request.Width = pData->pCreateStagingSurfaceData->Width;
            request.Height = pData->pCreateStagingSurfaceData->Height;
        }
        break;
    case D3DKMDT_STANDARDALLOCATION_GDISURFACE:
        if ((request.Described = pData->pCreateGdiSurfaceData != NULL) != 0) {
            request.Width = pData->pCreateGdiSurfaceData->Width;
            request.Height = pData->pCreateGdiSurfaceData->Height;
            request.Format = (ULONG)pData->pCreateGdiSurfaceData->Format;
            request.GdiType = (ULONG)pData->pCreateGdiSurfaceData->Type;
            request.GdiFlags = pData->pCreateGdiSurfaceData->Flags.Value;
        }
        break;
    default:
        break;
    }
    // BD-060: counted on entry, before anything can refuse (review 900: Calls[] below advances on success only).
    if (wddm != NULL) Bc250StdAllocEnter(&wddm->StdAlloc, &request);

    // Retain the CDD/DWM allocation contract independently of the shared first-DDI
    // log budget: primary/shadow requests can exhaust it before a GDI request.
    // WDK10.0.26100 d3dkmdt.h defines types0..8; all future values share slot9.
    // Separate the size query from the private-data fill, at most20 lines per start.
    if (wddm != NULL && request.Kind == BC250_STDALLOC_GDI && request.Described)
    {
        ULONG slot = Bc250GdiSlot(request.GdiType);
        LONG bit = (LONG)(1u << (slot * 2 + (ULONG)request.Fill));
        if ((InterlockedOr(&wddm->GdiSurfaceTypesLogged, bit) & bit) == 0)
            GuardLog("wddm: GDI request type %u flags 0x%08X phase %s %ux%u format %u",
                     request.GdiType, request.GdiFlags, request.Fill ? "fill" : "size",
                     request.Width, request.Height, request.Format);
    }

    status = Bc250StdAllocDecide(&request, &answer);
    if (wddm != NULL) Bc250StdAllocLeave(&wddm->StdAlloc, &request, status);
    if (status != BC250_STDALLOC_OK) return STATUS_INVALID_PARAMETER;

    // These are output fields, not just copies in our private LB7A blob.
    // E26 ETW rejected shadow/staging creation when the public pitch was zero. The size query leaves the union
    // as it came (d3dkmddi.md:32953); the fill, which precedes CreateAllocation, publishes the pitch.
    if (answer.PublishPitch) {
        if (request.Kind == BC250_STDALLOC_SHADOW)
            pData->pCreateShadowSurfaceData->Pitch = answer.Surface.Pitch;
        else if (request.Kind == BC250_STDALLOC_STAGING)
            pData->pCreateStagingSurfaceData->Pitch = answer.Surface.Pitch;
        else if (request.Kind == BC250_STDALLOC_GDI)
            pData->pCreateGdiSurfaceData->Pitch = answer.Surface.Pitch;
    }

    // Two passes: a NULL buffer asks only for the size. Only a 24H2+ shadow and, on the GPU Present route, a
    // staging surface (BD-156) get an E26R resource record.
    if (request.Fill)
    {
        if (answer.PrivateBytes == sizeof(gdi)) {
            RtlZeroMemory(&gdi, sizeof(gdi));
            gdi.Surface = answer.Surface;
            gdi.Magic = BC250_GDI_PRIVATE_MAGIC;
            gdi.Type = request.GdiType;
            gdi.Flags = request.GdiFlags;
            RtlCopyMemory(pData->pAllocationPrivateDriverData, &gdi, sizeof(gdi));
        } else RtlCopyMemory(pData->pAllocationPrivateDriverData, &answer.Surface, sizeof(answer.Surface));
        if (answer.ResourceBytes)
            RtlCopyMemory(pData->pResourcePrivateDriverData, answer.Resource, answer.ResourceBytes);
    }
    pData->AllocationPrivateDriverDataSize = answer.PrivateBytes;
    if (!answer.ResourceBytes) pData->pResourcePrivateDriverData = NULL;
    pData->ResourcePrivateDriverDataSize = answer.ResourceBytes;

    if (WddmFirstCalls(wddm, WddmDdiGetStandardAllocationDriverData))
        GuardLog("wddm: GetStandardAllocationDriverData type %u %ux%u format %u -> %llu bytes, record %u",
                 request.Kind, answer.Surface.Width, answer.Surface.Height, answer.Surface.Format, answer.Surface.Size,
                 answer.ResourceBytes);
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

// E26R v1: magic, version, shared (12 bytes). V2 adds CPU access intent
// (16 bytes): PRIMARY=1, CPU_READ=2. Primary is not a public input bit in
// DXGK_ALLOCATIONINFOFLAGS_WDDM2_0; never infer it from reserved OS flags.
// MS Cached: readable CPU backing may be cached, but primaries must not be.
static NTSTATUS WddmSurfaceResourcePolicy(const void* Data, UINT Bytes,
                                         BOOLEAN* SharedCpu, BOOLEAN* CachedCpu, BOOLEAN* Scanout)
{
    int shared=0,cached=0;
    int valid=Bc250SurfaceResourcePolicy(Data,Bytes,&shared,&cached);
    *SharedCpu=(BOOLEAN)shared; *CachedCpu=(BOOLEAN)cached;
    *Scanout=(BOOLEAN)(valid && Bc250SurfaceResourceScanout(Data,Bytes));
    return valid ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
}

// CreateAllocation's per-allocation work, as the operations of Bc250CreateRun (gdi_admission.h), which owns the loop,
// the resource object and the rollback, so that the rollback paths are host-tested with injected failures and every
// call is counted by its final outcome (review 904).
typedef struct _BC250_CREATE_CALL {
    BC250_DEVICE* Device;
    BC250_WDDM* Wddm;
    DXGKARG_CREATEALLOCATION* Args;
    BOOLEAN SharedCpu, CachedCpu, Scanout;
    // The type-0 placement this call's resource record earns, derived once per call by the function the
    // compositor's user-mode driver calls as well (WddmGdiRecordPolicy), before the loop builds anything.
    // The record is resource-level: deriving it inside the per-allocation loop parsed the same bytes three
    // times per allocation and - worse - put a refusal after the allocation object existed and its handle
    // was published, which Bc250CreateRun's rollback does not undo for the failing index (gdi_admission.h:
    // "on a failure nothing is left behind"). SharedCpu, CachedCpu and Scanout above are the same record's
    // answers and stay for the callers that only need the three bits; RecordVersion and RecordAccess are
    // for the create-time witness, which must be able to name what the creator actually asked for.
    BC250_GDI_ALLOCATION_POLICY RecordPolicy;
    unsigned long RecordVersion, RecordAccess;
} BC250_CREATE_CALL;

static int WddmCreateAdmit(void* Context, unsigned long Index, unsigned long* Slot)
{
    BC250_CREATE_CALL* call = (BC250_CREATE_CALL*)Context;
    DXGKARG_CREATEALLOCATION* pCreateAllocation = call->Args;
    BC250_DEVICE* device = call->Device;
    BC250_WDDM* wddm = call->Wddm;
    UINT i = (UINT)Index;
    DXGK_ALLOCATIONINFO* info = &pCreateAllocation->pAllocationInfo[i];
    const BC250_WDDM_ALLOCATION_PRIVATE* private = (const BC250_WDDM_ALLOCATION_PRIVATE*)info->pPrivateDriverData;
    BC250_WDDM_OBJECT* object;
    ULONG gdiType=0;
    BC250_GDI_ALLOCATION_POLICY policy;
    int admission;

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
            return BC250_CREATE_STEP_REFUSED;
        }
        object = WddmNewObject(device, BC250_WDDM_MAGIC_ALLOCATION);
        if (object == NULL) return BC250_CREATE_STEP_NO_MEMORY;
        object->UmdAlloc = TRUE;
        object->UmdBytes = view.bytes;
        object->UmdHeap = view.heap;
        object->UmdRequestedVa = view.requested_va;
        // KMD193: kept for the DESTROY journal record. 245's four freed objects carried UmdAlloc, UmdHeap
        // and UmdBytes and still could not say which process had asked for them or with what intent.
        object->UmdBlobVersion = view.version;
        object->UmdGemFlags = view.gem_flags;
        object->CreatorProcessId = HandleToULong(PsGetCurrentProcessId());
        // M15.14: the scan-out request. umd_blob.c has already checked the shape (v3, VRAM heap, a
        // geometry whose rows fit in the allocation); recorded here so that every later flip of this
        // allocation is decided by Bc250ScanoutAdmit against the POST mode, and refused if it moved.
        object->ScanoutRequested = (BOOLEAN)view.scanout;
        object->ScanoutWidth = view.scanout_width;
        object->ScanoutHeight = view.scanout_height;
        object->ScanoutPitch = view.scanout_pitch;
        object->ScanoutFormat = view.scanout_format;
        segment = (view.heap == UMD_BLOB_HEAP_GTT) ? BC250_WDDM_SEGMENT_APERTURE : BC250_WDDM_SEGMENT_VRAM;
        // M15.14: the winsys's own granularity, except that a blob which asked for scan-out never gets
        // less than the flip clause's page (Bc250ScanoutBlobAlignment) - the BC2A half of the same
        // 0.7.209.1 rule the type-0 path below follows. Without it a UMD that asks for 64 bytes here
        // gets 64 bytes for a scan-out surface too, and the flip of it is refused after the OS has taken
        // SharedPrimaryTransition: a blank output, not a fallback to composition.
        align = (UINT)Bc250ScanoutBlobAlignment(view.scanout ? 1 : 0, view.alignment);
        info->hAllocation = object;
        info->Size = (SIZE_T)ROUND_TO_PAGES((SIZE_T)view.bytes);
        // With the large page layout, a VRAM allocation of 2 MiB or more starts on a 2 MiB segment offset, so that
        // VidMm can map each whole 2 MiB of it with one level 1 entry. The winsys asks for at most 256 KiB for VRAM
        // (radv_wddm2_bo.c), which would leave those ranges to chance. 2 MiB is a multiple of every scan-out
        // alignment, and the size does not change. LargePages 2 places the same way, for a like-for-like control.
        if (device->LargePageLayout && segment == BC250_WDDM_SEGMENT_VRAM &&
            info->Size >= BC250_VIDMM_HUGE_PAGE_BYTES && align < BC250_VIDMM_HUGE_PAGE_BYTES)
            align = (UINT)BC250_VIDMM_HUGE_PAGE_BYTES;
        info->Alignment = align;
        info->HintedBank.Value = 0;
        info->MaximumRenamingListLength = 0;
        info->pAllocationUsageHint = NULL;
        info->PitchAlignedSize = 0;
        // Memory manager stage 1c: the winsys heap still decides the first segment (segment above); a VRAM
        // allocation that is not a scan-out may also live in the aperture, second in the preference order, when
        // EnableSharedResidency is on. Preferences are ordered (d3dukmdt.h D3DDDI_SEGMENTPREFERENCE) and every
        // one of them is in both supported sets, which DXGK_ALLOCATIONINFO requires.
        //   EvictionSegmentSet stays 0 (the report's stage 1b is not taken): with 0 VidMm "transfer[s] the content
        // ... directly to paged-locked system memory" (d3dkmddi.h DXGK_ALLOCATIONINFO), and on this driver an
        // aperture endpoint and an MDL endpoint resolve to the same host physical page (gfx.c
        // PagingResolvePhysical), so staging through the aperture would add a map, a GART write and a TLB flush
        // per eviction and accelerate nothing.
        {
            struct umd_placement placement;
            const int shared = UmdBlobPlacement(&view, BC250_WDDM_SEGMENT_VRAM, BC250_WDDM_SEGMENT_APERTURE,
                                                wddm != NULL && wddm->SharedResidency, &placement);
            info->PreferredSegment.Value = 0;
            info->PreferredSegment.SegmentId0 = placement.preferred[0];
            info->PreferredSegment.SegmentId1 = placement.preferred[1];
            info->SupportedReadSegmentSet = placement.supported;
            info->SupportedWriteSegmentSet = placement.supported;
            if (wddm != NULL)
                InterlockedIncrement(shared ? &wddm->UmdAllocsShared :
                                     segment == BC250_WDDM_SEGMENT_APERTURE ? &wddm->UmdAllocsAperture :
                                     &wddm->UmdAllocsLocalOnly);
        }
        info->EvictionSegmentSet = 0;
        info->PhysicalAdapterIndex = 0;
        WddmCpuVisibleAllocationFlags(&info->FlagsWddm2);
        // BD-210: no CPU view for an allocation whose creator said it never maps it (umd_blob.c). Before
        // this, VidMm gave every DXVK device-local chunk a write-combined view in the creating process,
        // and a 32-bit game ran out of address space on views nothing ever mapped.
        info->FlagsWddm2.CpuVisible = (UINT)UmdBlobAllocCpuVisible(&view);
        // RADV's CPU_GTT_USWC requests write-combined storage; without it,
        // CPU-accessible GTT requires cached backing store. VidMm supplies
        // CacheCoherent PTEs, which the encoder maps to AMDGPU_PTE_SNOOPED.
        info->FlagsWddm2.Cached = (UINT)UmdBlobAllocCpuCached(&view);
        info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        if (wddm != NULL && !info->FlagsWddm2.CpuVisible) {
            InterlockedIncrement(&wddm->UmdAllocsGpuOnly);
            InterlockedAdd64(&wddm->UmdGpuOnlyBytes, (LONG64)info->Size);
        } else if (wddm != NULL)
            InterlockedAdd64(&wddm->UmdCpuVisibleBytes, (LONG64)info->Size);
        if (wddm != NULL && InterlockedIncrement(&wddm->UmdAllocs) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: umd alloc %llu heap 0x%lX align %u va 0x%llX gem 0x%llX cached %u cpu %u", view.bytes,
                     view.heap, align, view.requested_va, view.gem_flags, info->FlagsWddm2.Cached,
                     info->FlagsWddm2.CpuVisible);
        return BC250_CREATE_STEP_ADMITTED;
    }

    // Stage A can only size an allocation it described itself. An unknown blob is an honest failure: nothing
    // in the never-fail list reaches this DDI, and guessing a size would put VidMm and us out of step.
    admission = Bc250Lb7aAdmit(info->pPrivateDriverData, info->PrivateDriverDataSize, call->SharedCpu, call->CachedCpu,
                               g_ApertureOffered, &gdiType, &policy);
    *Slot = admission == BC250_LB7A_UNREAD ? BC250_GDI_SLOTS - 1 : Bc250GdiSlot(gdiType);
    if (admission != BC250_LB7A_ADMITTED)
    {
        if (WddmFirstCalls(wddm, WddmDdiCreateAllocation))
            GuardLog("wddm: CreateAllocation %u of %u refused, private data %u bytes", i,
                     pCreateAllocation->NumAllocations, info->PrivateDriverDataSize);
        return BC250_CREATE_STEP_REFUSED;
    }

    // Resource records can also request the aperture for type 0 (including the BD-135 CDD shadow).
    // Refuse before publishing any object if QUERYSEGMENT4 did not offer that segment.
    if (!gdiType && call->RecordPolicy.Aperture && !g_ApertureOffered)
        return BC250_CREATE_STEP_REFUSED;

    object = WddmNewObject(device, BC250_WDDM_MAGIC_ALLOCATION);
    if (object == NULL) return BC250_CREATE_STEP_NO_MEMORY;
    object->Allocation = *private;
    object->GdiType=gdiType;
    // M15.14: a type-0 surface whose resource record carries the scan-out intent. Its own LB7A blob is
    // the description, so nothing is copied; the flag says the creator meant this surface to reach the
    // display pipeline, which is what moves it out of the shared aperture below.
    object->ScanoutRequested = (BOOLEAN)(!gdiType && call->Scanout);
    // WDK26100: standard texture/staging/lookup surfaces are GPU-only;
    // CPU staging uses coherent aperture. Legacy type0 keeps its policy.
    info->hAllocation = object;
    info->Size = (SIZE_T)ROUND_TO_PAGES(private->Size);
    // DXGK_ALLOCATIONINFO is an OUT array that nobody promised to zero: every member is written, as both
    // reference drivers do (Alignment 64 is theirs too). A surface that asked for scan-out asks for the
    // page alignment its own flip clause demands instead (Bc250ScanoutCreateAlignment): the clause refuses
    // any other base, and that refusal arrives after the OS has taken SharedPrimaryTransition, which it
    // does not fall back from. Nothing else moves - 64 bytes for every other allocation, as before.
    info->Alignment = Bc250ScanoutCreateAlignment(object->ScanoutRequested ? 1 : 0);
    info->HintedBank.Value = 0;
    info->MaximumRenamingListLength = 0;
    info->pAllocationUsageHint = NULL;
    info->PitchAlignedSize = 0;                     // the aperture segment is not a pitch-aligned one
    info->PreferredSegment.Value = 0;
    // A scan-out surface belongs in the local segment, the only one whose descriptor says DirectFlip
    // (WddmQuerySegment4). Type 0's placement otherwise follows the resource record's shared bit, which
    // is what puts a composed swap-chain buffer in the aperture for the compositor to read cheaply; a
    // scanned-out surface has no such reader, and the display core cannot read the aperture at all.
    // CpuVisible as well: a VRAM-only CpuVisible allocation is what VidMm 0xF002 and later refuse (K84,
    // the CDD shadow), and a scanned-out surface needs no CPU mapping - the GPU renders it and the
    // display core reads it by physical address, which AccessedPhysically is what asks for. All four
    // bits move together in WddmGdiScanoutPolicy: AccessedPhysically is derived from Aperture, so
    // clearing Aperture here and not re-deriving it would declare a VRAM surface VidMm need not back
    // contiguously - which is exactly the surface the display core must not be given.
    //   Since 0.7.209.1 the whole type-0 placement is one call of WddmGdiRecordPolicy, which the
    // compositor's user-mode driver calls as well: the shell may only answer CheckDirectFlipSupport TRUE
    // about a surface the display core can read, and it must derive that from the same record and the
    // same arithmetic that place the allocation here. The call is made once per CreateAllocation, in
    // Bc250WddmCreateAllocation, before any object exists - a record the derivation refuses fails the
    // whole DDI there, not here, so no refusal can leave an allocation object and a published
    // hAllocation behind. The values are the ones Bc250Lb7aAdmit computed above for a type-0 blob, and
    // the gdi-admission gate asserts that equality for every record shape.
    if (!gdiType) policy = call->RecordPolicy;
    info->PreferredSegment.SegmentId0 = policy.Aperture ? BC250_WDDM_SEGMENT_APERTURE : BC250_WDDM_SEGMENT_VRAM;
    info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(info->PreferredSegment.SegmentId0);
    info->SupportedWriteSegmentSet = info->SupportedReadSegmentSet;
    info->EvictionSegmentSet = 0;                   // no explicit eviction segment; VidMm owns backing-store eviction
    info->PhysicalAdapterIndex = 0;
    WddmCpuVisibleAllocationFlags(&info->FlagsWddm2);
    info->FlagsWddm2.CpuVisible = policy.CpuVisible;
    info->FlagsWddm2.AccessedPhysically = policy.AccessedPhysically;
    info->FlagsWddm2.Cached = policy.Cached;
    info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
    // M15.14 increment 2, the create-time answer. ScanoutRequests is counted at flip time, so a zero
    // there cannot tell "the client never marked its buffers" from "the request never reached
    // SetVidPnSourceAddress".
    //   Every type-0 allocation is bucketed, not only the records that carry the PRIMARY bit. A standard
    // primary - the surface DWM flips today - arrives here as a 32-byte LB7A blob with no resource
    // private data at all, so its record version and access word are both 0 and a counter kept for
    // PRIMARY records only would be silent for it. A silent counter would then read as "DWM's primaries
    // carry no PRIMARY record" when it equally means "DWM's front buffer is not a resource of our shell",
    // and those two have opposite consequences for the handshake: in the second, the compositor never
    // opens the client's buffer and the handshake can never be asked about the pair.
    //   The log line adds what the buckets cannot carry: the geometry, the placement bits, and whether
    // this create was part of a resource group, which is what separates a shell resource from a bare
    // standard allocation. Its budget is the create lines' own.
    if (!gdiType && wddm != NULL) {
        InterlockedIncrement(&wddm->Lb7aAllocs);
        if (!policy.CpuVisible) {
            InterlockedIncrement(&wddm->Lb7aAllocsGpuOnly);
            InterlockedAdd64(&wddm->Lb7aGpuOnlyBytes,(LONG64)info->Size);
        } else InterlockedAdd64(&wddm->Lb7aCpuVisibleBytes,(LONG64)info->Size);
        if (call->RecordVersion==BC250_SURFACE_RESOURCE_GPU_ONLY_VERSION)
            InterlockedIncrement(&wddm->Lb7aNoCpuAccess);
        const BOOLEAN primary = (BOOLEAN)((call->RecordAccess & BC250_SURFACE_RESOURCE_PRIMARY) != 0);
        InterlockedIncrement(&wddm->ScanoutCreates[WddmRecordKind(call->RecordVersion)]
                                                  [object->ScanoutRequested ? 0 : 1]);
        if (primary) InterlockedIncrement(&wddm->ScanoutCreatePrimaries);
        if (pCreateAllocation->Flags.Resource || pCreateAllocation->hResource != NULL)
            InterlockedIncrement(&wddm->ScanoutCreateResources);
        if (InterlockedIncrement(&wddm->ScanoutCreateNotes) <= BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: type0 create req%u v%lu acc0x%lX res%u %lux%lu pitch%lu fmt%lu place0x%lX",
                     (ULONG)(object->ScanoutRequested ? 1 : 0), call->RecordVersion, call->RecordAccess,
                     (ULONG)(pCreateAllocation->Flags.Resource || pCreateAllocation->hResource != NULL ? 1 : 0),
                     object->Allocation.Width, object->Allocation.Height, object->Allocation.Pitch,
                     object->Allocation.Format, WddmGdiPolicyBits(&policy));
    }
    return BC250_CREATE_STEP_ADMITTED;
}

static unsigned long WddmCreateSlot(void* Context, unsigned long Index)
{
    const BC250_WDDM_OBJECT* object =
        (const BC250_WDDM_OBJECT*)((BC250_CREATE_CALL*)Context)->Args->pAllocationInfo[Index].hAllocation;
    return object->UmdAlloc ? BC250_CREATE_NOT_LB7A : Bc250GdiSlot(object->GdiType);
}

static void WddmCreateFree(void* Context, unsigned long Index)
{
    WddmFreeObject((BC250_WDDM_OBJECT*)((BC250_CREATE_CALL*)Context)->Args->pAllocationInfo[Index].hAllocation);
}

static int WddmCreateResource(void* Context)
{
    BC250_CREATE_CALL* call = (BC250_CREATE_CALL*)Context;
    if (call->Args->Flags.Resource && call->Args->hResource == NULL)
    {
        call->Args->hResource = WddmNewObject(call->Device, BC250_WDDM_MAGIC_RESOURCE);
        return call->Args->hResource != NULL;
    }
    return 1;
}

static const BC250_CREATE_OPS g_WddmCreateOps = { WddmCreateAdmit, WddmCreateSlot, WddmCreateFree, WddmCreateResource };

static DXGKDDI_CREATEALLOCATION Bc250WddmCreateAllocation;
static NTSTATUS Bc250WddmCreateAllocation(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_CREATEALLOCATION* pCreateAllocation)
{
    BC250_CREATE_CALL call;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    BOOLEAN sharedCpu = FALSE, cachedCpu = FALSE, scanout = FALSE;
    NTSTATUS resourceStatus=WddmSurfaceResourcePolicy(pCreateAllocation->pPrivateDriverData,
        pCreateAllocation->PrivateDriverDataSize,&sharedCpu,&cachedCpu,&scanout);
    int outcome;

    if (!NT_SUCCESS(resourceStatus)) {
        if (wddm != NULL) BC250_ADMISSION_COUNT(&wddm->StdAlloc.CreateCalls[BC250_CREATE_RESOURCE_DATA]);
        return resourceStatus;
    }
    call.Device = (BC250_DEVICE*)hAdapter;
    call.Wddm = wddm;
    call.Args = pCreateAllocation;
    call.SharedCpu = sharedCpu;
    call.CachedCpu = cachedCpu;
    call.Scanout = scanout;
    // M15.14 increment 2: the record is resource-level, so it is read once here, before the loop builds
    // anything, and not per allocation. Both calls can only fail on a record WddmSurfaceResourcePolicy
    // has already refused above - WddmGdiAllocationPolicy admits type 0 unconditionally - so this is
    // unreachable today; it is a refusal of the whole DDI rather than an assertion because the one wrong
    // way to fail is the way the loop used to: after an allocation object exists and its handle has been
    // published, which Bc250CreateRun's rollback does not undo for the failing index.
    RtlZeroMemory(&call.RecordPolicy, sizeof(call.RecordPolicy));
    call.RecordVersion = 0; call.RecordAccess = 0;
    if (!WddmGdiRecordPolicy(pCreateAllocation->pPrivateDriverData, pCreateAllocation->PrivateDriverDataSize,
                             &call.RecordPolicy) ||
        !Bc250SurfaceResourceIntent(pCreateAllocation->pPrivateDriverData,
                                    pCreateAllocation->PrivateDriverDataSize,
                                    &call.RecordVersion, &call.RecordAccess)) {
        if (wddm != NULL) BC250_ADMISSION_COUNT(&wddm->StdAlloc.CreateCalls[BC250_CREATE_RESOURCE_DATA]);
        return STATUS_INVALID_PARAMETER;
    }
    outcome = Bc250CreateRun(wddm != NULL ? &wddm->StdAlloc : NULL, &g_WddmCreateOps, &call,
                             pCreateAllocation->NumAllocations);
    if (outcome == BC250_CREATE_NO_MEMORY) return STATUS_INSUFFICIENT_RESOURCES;
    if (outcome != BC250_CREATE_OK) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls(wddm, WddmDdiCreateAllocation))
        GuardLog("wddm: CreateAllocation %u allocations, flags %x resource %s", pCreateAllocation->NumAllocations,
                 pCreateAllocation->Flags.Value, pCreateAllocation->hResource != NULL ? "yes" : "no");
    return STATUS_SUCCESS;
}

static NTSTATUS WddmDestroyRestore(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, ULONGLONG Serial);
static DXGKDDI_DESTROYALLOCATION Bc250WddmDestroyAllocation;
static NTSTATUS Bc250WddmDestroyAllocation(_In_ const HANDLE hAdapter,
                                           _In_ const DXGKARG_DESTROYALLOCATION* pDestroyAllocation)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT i;

    if (WddmFirstCalls(wddm, WddmDdiDestroyAllocation))
        GuardLog("wddm: DestroyAllocation %u allocations", pDestroyAllocation->NumAllocations);
    // Retire all relevant surfaces before freeing any handle. A failed protected
    // section leaves the complete request retryable, without a partially freed list.
    for (i = 0; i < pDestroyAllocation->NumAllocations; i++) {
        BC250_WDDM_OBJECT* object = WddmObject(pDestroyAllocation->pAllocationList[i], BC250_WDDM_MAGIC_ALLOCATION);
        if (object != NULL && wddm != NULL && object->Serial != 0 &&
            InterlockedCompareExchange64(&wddm->ScanoutObject, 0, 0) == (LONG64)object->Serial) {
            NTSTATUS retired = WddmDestroyRestore(device, wddm, object->Serial);
            if (!NT_SUCCESS(retired)) return retired;
        }
    }
    for (i = 0; i < pDestroyAllocation->NumAllocations; i++) {
        BC250_WDDM_OBJECT* object = WddmObject(pDestroyAllocation->pAllocationList[i], BC250_WDDM_MAGIC_ALLOCATION);
        if (object != NULL)
            PagingJournalDestroy(object->UmdRequestedVa, pDestroyAllocation->pAllocationList[i],
                                 object->UmdAlloc ? object->UmdBytes : object->Allocation.Size,
                                 object->UmdAlloc ? BC250_PJ_FLAG_UMD_ALLOCATION : 0u,
                                 object->CreatorProcessId, object->UmdBlobVersion, object->UmdGemFlags);
        // M15.14: the plane may be reading this allocation. Before M15.14 the only programmable surface
        // was dxgkrnl's own shared primary, whose lifetime dxgkrnl ties to the video present source; an
        // application swap-chain buffer has no such tie, so a game that exits or resizes with its chain
        // still bound would leave HUBP0 scanning VRAM that VidMm is free to hand to the next allocation,
        // with no way back to the firmware surface short of a reboot (no GPU reset exists here, facts
        // M53). The firmware surface goes back first, and only then is the object freed. Compare and
        // clear in one step: a flip on another processor either wins the record, in which case it owns
        // the restore, or finds it already taken away.
        //   The record holds the object's serial, so the nonzero test is part of the comparison and not a
        // formality: 0 is the record's own "no application surface is being scanned out", and an object
        // whose serial were 0 would match that and take the plane back to the firmware surface for a
        // buffer the plane never held. WddmNewObject gives every object a nonzero serial under the lock,
        // which is what makes that unreachable; the test says so rather than relying on it.
        //   BD-170 r6: the restore is a writer (WddmDestroyRestore), which takes the record inside L.
        WddmFreeObject(object);
    }
    if (pDestroyAllocation->Flags.DestroyResource && pDestroyAllocation->hResource != NULL)
        WddmFreeObject(WddmObject(pDestroyAllocation->hResource, BC250_WDDM_MAGIC_RESOURCE));
    return STATUS_SUCCESS;
}

static DXGKDDI_DESCRIBEALLOCATION Bc250WddmDescribeAllocation;
static NTSTATUS Bc250WddmDescribeAllocation(_In_ const HANDLE hAdapter,
                                            _Inout_ DXGKARG_DESCRIBEALLOCATION* pDescribeAllocation)
{
    BC250_DEVICE* device=(BC250_DEVICE*)hAdapter;
    BC250_WDDM_OBJECT* object = WddmObject(pDescribeAllocation->hAllocation, BC250_WDDM_MAGIC_ALLOCATION);
    D3DKMDT_VIDEO_SIGNAL_INFO signal;
    NTSTATUS status;

    if (object == NULL) return STATUS_INVALID_PARAMETER;
    if (ConnectorIsEnabled(device)) {
        status=ConnectorLinkCurrentSignal(device,&signal);
        if (!NT_SUCCESS(status)) return status;
    } else {
        if (!device->InheritedSignalValid) return STATUS_DEVICE_NOT_READY;
        signal=device->InheritedSignal;
    }
    pDescribeAllocation->Width = object->Allocation.Width;
    pDescribeAllocation->Height = object->Allocation.Height;
    pDescribeAllocation->Format = (D3DDDIFORMAT)object->Allocation.Format;
    pDescribeAllocation->MultisampleMethod.NumSamples = 0;
    pDescribeAllocation->MultisampleMethod.NumQualityLevels = 0;
    // The primary uses the committed target mode advertised by FillSignalInfo.
    // E26/M147: a different allocation refresh yields PRESENT_MODE_CHANGED.
    // PASSIVE_LEVEL DDI: copy the tuple under the connector operation owner.
    pDescribeAllocation->RefreshRate = signal.VSyncFreq;
    pDescribeAllocation->PrivateDriverFormatAttribute = 0;
    pDescribeAllocation->Rotation = D3DDDI_ROTATION_IDENTITY;
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiDescribeAllocation))
        GuardLog("wddm: DescribeAllocation %ux%u format %u refresh %u/%u", pDescribeAllocation->Width, pDescribeAllocation->Height,
                 (ULONG)pDescribeAllocation->Format,pDescribeAllocation->RefreshRate.Numerator,
                 pDescribeAllocation->RefreshRate.Denominator);
    return STATUS_SUCCESS;
}

#include "wddm_allocation_identity.inc"

static DXGKDDI_OPENALLOCATIONINFO Bc250WddmOpenAllocation;
static NTSTATUS Bc250WddmOpenAllocation(_In_ const HANDLE hDevice, _In_ const DXGKARG_OPENALLOCATION* pOpenAllocation)
{
    BC250_WDDM_OBJECT* parent = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);
    BC250_WDDM* wddm;
    LONG opens[BC250_GDI_SLOTS][2];     // this call's LB7A outcomes, added to StdAlloc.GdiOpened only when it succeeds
    UINT i, slot;

    if (parent == NULL) return STATUS_INVALID_PARAMETER;
    wddm = (BC250_WDDM*)parent->Device->Wddm;
    RtlZeroMemory(opens, sizeof(opens));
    // Opened handles are carried back in Present entries. Historical GetHandleData
    // returned NULL for both CDD LB7A and user BC2A opens; this is not a proven
    // CDD-specific restriction. Acquire/Release now establishes backing identity;
    // unresolved opens retain CPU compatibility but fail GPU Present admission.
    for (i = 0; i < pOpenAllocation->NumAllocations; i++)
    {
        DXGK_OPENALLOCATIONINFO* info = &pOpenAllocation->pOpenAllocation[i];
        const BC250_WDDM_ALLOCATION_PRIVATE* private = (const BC250_WDDM_ALLOCATION_PRIVATE*)info->pPrivateDriverData;
        BC250_WDDM_OBJECT* opened = NULL;
        BOOLEAN admitted = FALSE;
        ULONG gdiType=0;
        BC250_GDI_ALLOCATION_POLICY policy;

        if (UmdBlobIsAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize))
        {
            struct umd_alloc_view view;

            // Same blob CreateAllocation already accepted. A refusal here leaves a NULL handle, as a bad
            // GDI blob does, rather than failing the open: dxgkrnl opened what it created.
            if (UmdBlobParseAlloc(info->pPrivateDriverData, info->PrivateDriverDataSize, &view) == UMD_BLOB_OK)
            {
                admitted = TRUE;
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
        else
        {
            int admission = Bc250Lb7aAdmit(info->pPrivateDriverData, info->PrivateDriverDataSize, 0, 0, 1,
                                           &gdiType, &policy);
            if (admission == BC250_LB7A_ADMITTED)
            {
                admitted = TRUE;
                opened = WddmNewObject(parent->Device, BC250_WDDM_MAGIC_OPENED);
                if (opened != NULL) {
                    opened->Allocation = *private;
                    opened->GdiType=gdiType;
                }
            }
            // BD-060: a refused LB7A open leaves a NULL handle and still returns success; counted with the call.
            opens[admission != BC250_LB7A_UNREAD ? Bc250GdiSlot(gdiType) : BC250_GDI_SLOTS - 1][opened != NULL ? 0 : 1]++;
        }
        // DxgkDdiOpenAllocation: STATUS_SUCCESS promises a non-NULL hDeviceSpecificAllocation for every allocation
        // (d3dkmddi.md). A blob this driver does not admit keeps the BD-060 NULL handle above. An admitted blob
        // whose object could not be allocated fails the whole call with STATUS_NO_MEMORY, a listed return. The
        // objects this call already opened are freed first, and their handles cleared: none was published yet.
        // The call's counts are dropped with them, so an allocation that failed for memory is never counted as a
        // refused open.
        if (admitted && opened == NULL)
        {
            UINT j;
            for (j = 0; j < i; j++)
            {
                WddmFreeObject(WddmObject(pOpenAllocation->pOpenAllocation[j].hDeviceSpecificAllocation,
                                          BC250_WDDM_MAGIC_OPENED));
                pOpenAllocation->pOpenAllocation[j].hDeviceSpecificAllocation = NULL;
            }
            info->hDeviceSpecificAllocation = NULL;
            GuardLog("wddm: OpenAllocation [%u] of %u: no object for an admitted blob, call failed (no memory)",
                     i, pOpenAllocation->NumAllocations);
            return STATUS_NO_MEMORY;
        }
        if (opened != NULL) {
            opened->OwnerDevice = hDevice;
            WddmBindHandleIdentity(parent->Device,info,opened,pOpenAllocation->Flags.Value);
        }
        info->hDeviceSpecificAllocation = opened;
        if (parent->Device->Wddm != NULL && ((BC250_WDDM*)parent->Device->Wddm)->Calls[WddmDdiOpenAllocation] < BC250_WDDM_LOG_CALLS)
            GuardLog("wddm: OpenAllocation [%u] handle 0x%08X private %u bytes -> %p", i, (ULONG)info->hAllocation,
                     info->PrivateDriverDataSize, (void*)opened);
    }
    if (wddm != NULL)
        for (slot = 0; slot < BC250_GDI_SLOTS; slot++)
        {
            if (opens[slot][0] != 0) InterlockedAdd(&wddm->StdAlloc.GdiOpened[slot][0], opens[slot][0]);
            if (opens[slot][1] != 0) InterlockedAdd(&wddm->StdAlloc.GdiOpened[slot][1], opens[slot][1]);
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
    // Looked up in the object index before being freed, as the blit does, never dereferenced as given.
    for (i = 0; i < pCloseAllocation->NumAllocations; i++)
        WddmFreeObject(WddmIndexedObject((BC250_WDDM*)parent->Device->Wddm, pCloseAllocation->pOpenHandleList[i], BC250_WDDM_MAGIC_OPENED));
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


// One complete range per native record. MultipassOffset counts ranges, while
// the caller-owned write offset is restored before returning to dxgkrnl.
static NTSTATUS WddmBuildNativePagingCopies(BC250_DEVICE* Device, ULONGLONG Root,
    DXGKARG_BUILDPAGINGBUFFER* Build)
{
    ULONG originalOffset=Build->DmaBufferWriteOffset;
    BOOLEAN tracked=VidMmPagingRootTracked(Root);
    NTSTATUS status=STATUS_SUCCESS;
    if(!Root || !Build->DmaBufferGpuVirtualAddress || !Build->pDmaBuffer ||
       Build->MultipassOffset>Build->CopyPageTableEntries.NumRanges ||
       (Build->CopyPageTableEntries.NumRanges && !Build->CopyPageTableEntries.pRanges))
        return STATUS_INVALID_PARAMETER;
    while(Build->MultipassOffset<Build->CopyPageTableEntries.NumRanges) {
        const DXGK_BUILDPAGINGBUFFER_COPY_RANGE* range=&Build->CopyPageTableEntries.pRanges[Build->MultipassOffset];
        PAGING_NATIVE_RESULT built;
        ULONGLONG source,destination,sourcePhysical=0,destinationPhysical=0;
        BOOLEAN sourceSystem=FALSE,destinationSystem=FALSE;
        ULONG* record=(ULONG*)Build->pDmaBufferPrivateData;
        if(!range->NumPageTableEntries || range->SrcStartPteIndex>=512 || range->DstStartPteIndex>=512 ||
           range->NumPageTableEntries>512-range->SrcStartPteIndex ||
           range->NumPageTableEntries>512-range->DstStartPteIndex ||
           ((range->SrcPageTableAddress|range->DstPageTableAddress)&65535ull) ||
           range->SrcPageTableAddress>0xffffffffffffull-4095 ||
           range->DstPageTableAddress>0xffffffffffffull-4095) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        source=range->SrcPageTableAddress+(ULONGLONG)range->SrcStartPteIndex*8u;
        destination=range->DstPageTableAddress+(ULONGLONG)range->DstStartPteIndex*8u;
        if(!record || Build->DmaBufferPrivateDataSize<PAGING_PRIVATE_NATIVE_BYTES+PAGING_PRIVATE_JOB_BYTES) {
            status=STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;break;
        }
        record[0]=0;
        // Translation is used only for the pinned construction mirror, never
        // for the GPU operands. Untracked privileged tables need no CPU walk.
        if(tracked && (!VidMmTranslatePaging(Root,source,&sourcePhysical,&sourceSystem) ||
                       !VidMmTranslatePaging(Root,destination,&destinationPhysical,&destinationSystem) ||
                       sourceSystem || destinationSystem)) {
            status=STATUS_INVALID_PARAMETER;break;
        }
        status=GfxPagingBuildVirtualPtes(Device,source,destination,range->NumPageTableEntries,
            Build->pDmaBuffer,Build->DmaBufferGpuVirtualAddress,Build->DmaBufferWriteOffset,Build->DmaSize,&built);
        if(!NT_SUCCESS(status))break;
        if(!PagingPrivateQueuedNativeHeader((unsigned*)record,Build->DmaBufferPrivateDataSize,
            Build->DmaBufferWriteOffset,Build->DmaBufferGpuVirtualAddress,built.Bytes,Root,
            built.IbOffset,built.IbDwords,built.CsaOffset)) {status=STATUS_INVALID_PARAMETER;break;}
        if(tracked) {
            status=VidMmCommitPagingCopy(sourcePhysical,destinationPhysical,range->NumPageTableEntries);
            if(!NT_SUCCESS(status)){record[0]=0;break;}
        }
        Build->pDmaBuffer=(UCHAR*)Build->pDmaBuffer+built.Bytes;Build->DmaSize-=built.Bytes;
        Build->pDmaBufferPrivateData=(UCHAR*)record+PAGING_PRIVATE_NATIVE_BYTES+PAGING_PRIVATE_JOB_BYTES;
        Build->DmaBufferPrivateDataSize-=PAGING_PRIVATE_NATIVE_BYTES+PAGING_PRIVATE_JOB_BYTES;
        if(Build->DmaBufferPrivateDataSize>=sizeof(ULONG))*(ULONG*)Build->pDmaBufferPrivateData=0;
        Build->MultipassOffset++;Build->DmaBufferWriteOffset+=built.Bytes;
        if(Device->Wddm)InterlockedIncrement64(&((BC250_WDDM*)Device->Wddm)->PagingNativePtes);
    }
    Build->DmaBufferWriteOffset=originalOffset;
    return status;
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
        SIZE_T needed;
        void* storage;
        // No command can be accepted with zero output capacity. Ask for a fresh
        // OS buffer before retaining identities or acquiring capture storage.
        if(!Build->DmaSize || !Build->pDmaBufferPrivateData ||
           !PagingPrivateQueuedDirectCapacity(Build->DmaBufferPrivateDataSize))
            return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
        needed=GfxPagingCaptureStorageSize(src,dst,bytes);
        storage=PagingCaptureStorage(Owner,(ULONGLONG)needed);
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

// Where a build lands in the paging buffer, for the paging journal: the same position the private record header
// binds (WddmPublishPagingRecordCore), which SubmitCommand's DmaBufferVirtualAddress/DmaBufferSize range covers.
static ULONGLONG WddmPagingBuildPosition(_In_ const DXGKARG_BUILDPAGINGBUFFER* Build)
{
    return Build->DmaBufferGpuVirtualAddress + Build->DmaBufferWriteOffset;
}

// BD-235 Q3a: the journal's failure word for a fill or transfer that built a prefix (paging_journal.c,
// PagingJournalNote). The OS submits the words of a build only after SUCCESS or INSUFFICIENT_DMA_BUFFER (the DDI
// page's remarks: after ALLOCATION_BUSY VidMm calls the same operation again, and any other status stops the
// system). After any other status none of the prefix runs: the record keeps it as evidence of how far the build
// came, marked failed, and a later submission of the same buffer range does not stamp its fence on it. 0 for those
// two statuses, else the builder's own status (never 0, which is SUCCESS).
static ULONG WddmPagingBuildFailure(_In_ NTSTATUS Status)
{
    return Status == STATUS_SUCCESS || Status == STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER ? 0u : (ULONG)Status;
}

// A built transfer: the old totals plus the KMD183 split by kind and direction (BC250_WDDM_XFER).
static void WddmCountTransfer(_Inout_ BC250_WDDM* Wddm, _In_ BC250_WDDM_XFER Kind, _In_ ULONGLONG Moved)
{
    InterlockedIncrement(&Wddm->PagingTransfersBuilt);
    InterlockedAdd64(&Wddm->PagingBytesMoved,(LONG64)Moved);
    if ((ULONG)Kind < BC250WddmXferKinds) {
        InterlockedIncrement(&Wddm->PagingXferCount[Kind]);
        InterlockedAdd64(&Wddm->PagingXferBytes[Kind],(LONG64)Moved);
    }
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
            // MultipassOffset is the next page of the operation (WddmBuildAperture), so the step is the page count.
            const LONG64 pages=(LONG64)(pBuildPagingBuffer->MultipassOffset-before);
            if (unmap) { InterlockedIncrement(&wddm->PagingUnmapsBuilt); InterlockedAdd64(&wddm->PagingUnmapPages,pages); }
            else { InterlockedIncrement(&wddm->PagingMapsBuilt); InterlockedAdd64(&wddm->PagingMapPages,pages); }
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
            InterlockedAdd64(&wddm->PagingFillBytes,(LONG64)moved);
            PagingJournalNote(BC250_PJ_FILL,0,pBuildPagingBuffer->Fill.hAllocation,moved,
                WddmPagingBuildPosition(pBuildPagingBuffer),0,WddmPagingBuildFailure(status));
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
            InterlockedAdd64(&wddm->PagingFillBytes,(LONG64)moved);
            PagingJournalNote(BC250_PJ_VIRTUAL_FILL,pBuildPagingBuffer->FillVirtual.DestinationVirtualAddress,
                pBuildPagingBuffer->FillVirtual.hAllocation,moved,WddmPagingBuildPosition(pBuildPagingBuffer),0,
                WddmPagingBuildFailure(status));
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
            DXGK_MEMORY_TRANSFER_DIRECTION direction=pBuildPagingBuffer->TransferVirtual.TransferDirection;
            WddmCountTransfer(wddm,direction==DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM ? BC250WddmXferVirtualToSystem :
                direction==DXGK_MEMORY_TRANSFER_SYSTEM_TO_LOCAL ? BC250WddmXferVirtualFromSystem :
                BC250WddmXferVirtualOther,moved);
            PagingJournalNote(BC250_PJ_VIRTUAL_TRANSFER,pBuildPagingBuffer->TransferVirtual.SourceVirtualAddress,
                pBuildPagingBuffer->TransferVirtual.hAllocation,moved,WddmPagingBuildPosition(pBuildPagingBuffer),
                pBuildPagingBuffer->TransferVirtual.TransferDirection==DXGK_MEMORY_TRANSFER_LOCAL_TO_SYSTEM ?
                    BC250_PJ_FLAG_TO_SYSTEM : 0u,WddmPagingBuildFailure(status));
        }
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
        return status;
    }
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_TRANSFER) {
        ULONGLONG moved=0;
        NTSTATUS status=WddmBuildPhysicalTransfer((BC250_DEVICE*)hAdapter,pBuildPagingBuffer,&moved);
        if (moved) {
            WddmCountTransfer(wddm,pBuildPagingBuffer->Transfer.Destination.SegmentId==0 ? BC250WddmXferPhysicalToSystem :
                pBuildPagingBuffer->Transfer.Source.SegmentId==0 ? BC250WddmXferPhysicalFromSystem :
                BC250WddmXferPhysicalOther,moved);
            PagingJournalNote(BC250_PJ_TRANSFER,0,pBuildPagingBuffer->Transfer.hAllocation,moved,
                WddmPagingBuildPosition(pBuildPagingBuffer),0,WddmPagingBuildFailure(status));
        }
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement(&wddm->PagingInsufficientBuffer);
        return status;
    }
    // Paging-process CPU_VIRTUAL initialization must be immediate, including when
    // pDmaBuffer is NULL. GPU_PHYSICAL updates use the ordered path below.
    // BD-235: the journal records the update only when VidMmUpdatePageTable made it, and a failure goes to the OS
    // (Bc250WddmBuildPagingBuffer). A refusal by its checks (entry encoding, table identity, shadow room) comes before
    // the first write and leaves the table as it was. A fault caught during the writes does not: the table can then
    // hold part of the update, and the journal does not record it.
    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_UPDATE_PAGE_TABLE &&
        pBuildPagingBuffer->UpdatePageTable.UpdateMode == DXGK_PAGETABLEUPDATE_CPU_VIRTUAL) {
        NTSTATUS status=VidMmUpdatePageTable(&pBuildPagingBuffer->UpdatePageTable);
        if (status != STATUS_SUCCESS) {
            LONG n = InterlockedIncrement(&wddm->PagingInnerFailures);
            if (n <= 16)
                GuardLog("wddm: *** paging CPU update failed 0x%08X, not recorded (level %u start %u count %u #%ld) ***",
                         (ULONG)status,(ULONG)pBuildPagingBuffer->UpdatePageTable.PageTableLevel,
                         (ULONG)pBuildPagingBuffer->UpdatePageTable.StartIndex,
                         (ULONG)pBuildPagingBuffer->UpdatePageTable.NumPageTableEntries,n);
            return status;
        }
        PagingJournalUpdate(&pBuildPagingBuffer->UpdatePageTable,0,pBuildPagingBuffer->UpdatePageTable.NumPageTableEntries,
                            0,TRUE);
        return STATUS_SUCCESS;
    }

    if (wddm != NULL && pBuildPagingBuffer->Operation == DXGK_OPERATION_COPY_PAGE_TABLE_ENTRIES) {
        BC250_WDDM_OBJECT* context=WddmObject(pBuildPagingBuffer->hSystemContext,BC250_WDDM_MAGIC_CONTEXT);
        // Internal failure statuses remain part of the audited DDI error-policy
        // gap; never turn a refused nonempty copy into empty SUCCESS.
        if(wddm->NativePteCopies)
            return WddmBuildNativePagingCopies((BC250_DEVICE*)hAdapter,context ? context->RootPhysical : 0,pBuildPagingBuffer);
        return WddmBuildPagingCopies((BC250_DEVICE*)hAdapter,context ? context->RootPhysical : 0,pBuildPagingBuffer);
    }

    // GPU_PHYSICAL updates and FLUSH_TLB: a builder status is a result (published) or a failure (returned), below.
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
        // FLUSH_TLB: flush all addresses of every VMID that can hold the requested root, including when the root
        // is not bound now (gfx.c GfxPagingBuildFlush; VMID 1 alone with EnableVmidPool 0). No root rewrite.
        if (update)
            pagingStatus = GfxPagingBuildUpdate((BC250_DEVICE*)hAdapter,&pBuildPagingBuffer->UpdatePageTable,
                (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,pBuildPagingBuffer->DmaBufferWriteOffset,
                dmaFree,startByte,&written,&nextByte,&unsupported);
        else
        {
            ULONGLONG root = 0;
            if (!VidMmRootPhysical(&pBuildPagingBuffer->FlushTlb.RootPageTableAddress,&root)) root = 0;
            pagingStatus = GfxPagingBuildFlush((BC250_DEVICE*)hAdapter,root,
                (PUCHAR)record+PAGING_PRIVATE_HEADER_BYTES,pBuildPagingBuffer->DmaBufferWriteOffset,
                dmaFree,&written,&unsupported);
        }
        // BD-235: SUCCESS and INSUFFICIENT_DMA_BUFFER are the only results of this branch. Any other status is a
        // failure of the builder (not ready, an encoding, MC range or SDMA helper refusal, or a status that is not a
        // result at all): nothing of this call is published or journalled, the multipass position stays where the
        // last published slice left it, and the status goes to Bc250WddmBuildPagingBuffer, which gives a failure to
        // the OS unchanged (paging_ddi_status.h). The OS then stops the system; before, it got success and took
        // entries that nothing wrote. A SUCCESS with no command words is a result: the CPU bootstrap update writes the
        // table itself (GfxPagingBuildUpdate), and a flush with no VMID to invalidate has nothing to do.
        if (pagingStatus != STATUS_SUCCESS && pagingStatus != STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
        {
            LONG n = InterlockedIncrement(&wddm->PagingInnerFailures);
            if (unsupported > BC250PagingSupported && (ULONG)unsupported < RTL_NUMBER_OF(wddm->PagingUnsupported))
                InterlockedIncrement(&wddm->PagingUnsupported[unsupported]);
            if (n <= 16)
                GuardLog("wddm: *** paging %s failed 0x%08X reason %u (level %u start %u/%u #%ld) ***",
                         update ? "update" : "flush", (ULONG)pagingStatus, (ULONG)unsupported,
                         update ? (ULONG)pBuildPagingBuffer->UpdatePageTable.PageTableLevel : 0u, startByte,
                         update ? (ULONG)pBuildPagingBuffer->UpdatePageTable.NumPageTableEntries : 0u, n);
            return pagingStatus;
        }
        if (written != 0)
        {
            if (!NT_SUCCESS(WddmPublishPagingRecord(pBuildPagingBuffer,written,update,startByte,nextByte)))
                return STATUS_INVALID_PARAMETER; // inherited malformed-publication contract gap remains audited
            if (update) InterlockedIncrement(&wddm->PagingUpdatesBuilt);
            else InterlockedIncrement(&wddm->PagingFlushesBuilt);
            if (!update) InterlockedAdd64(&wddm->PagingBytesMoved, (LONG64)(nextByte - startByte));
            // The journal's record of this slice, at the position the record header above bound it to.
            if (update)
                PagingJournalUpdate(&pBuildPagingBuffer->UpdatePageTable,startByte,nextByte-startByte,
                                    WddmPagingBuildPosition(pBuildPagingBuffer),FALSE);
            else PagingJournalNote(BC250_PJ_FLUSH_TLB,0,NULL,0,WddmPagingBuildPosition(pBuildPagingBuffer),0,0);
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
    NTSTATUS status,built;
    int outside;
    if (!wddm) return (NTSTATUS)PagingDdiStatus(WddmBuildPagingBufferImpl(hAdapter,Build),&outside);
    ProgressEnterInput(ProgressSiteBuildPagingBuffer,(LONG)Build->Operation); // before the lock: a wait counts as inside
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
    built=WddmBuildPagingBufferImpl(hAdapter,Build);
    // BD-235: a builder status outside the DDI is counted and logged here, the one place that sees every operation.
    // It never becomes success: a failure goes to the OS unchanged, and the OS stops the system (bug check 0x10E/0xB)
    // instead of using an allocation that the buffer did not fill. paging_ddi_status.h has the rules.
    status=(NTSTATUS)PagingDdiStatus(built,&outside);
    if (outside) {
        LONG n=InterlockedIncrement(&wddm->PagingContractRefusals);
        InterlockedExchange(&wddm->PagingContractLastStatus,(LONG)built);
        InterlockedExchange(&wddm->PagingContractLastOperation,(LONG)Build->Operation);
        if (n<=16)
            GuardLog("wddm: *** paging build op%u status 0x%08X outside the DDI, 0x%08X to the OS (offset %u dma %u #%ld) ***",
                     (ULONG)Build->Operation,(ULONG)built,(ULONG)status,Build->MultipassOffset,Build->DmaSize,n);
        if (n<=16 && Build->Operation==DXGK_OPERATION_VIRTUAL_FILL)
            GuardLog("wddm: *** the fill: VA 0x%llX bytes 0x%llX allocation %p ***",
                     (ULONGLONG)Build->FillVirtual.DestinationVirtualAddress,
                     (ULONGLONG)Build->FillVirtual.FillSizeInBytes,Build->FillVirtual.hAllocation);
    }
    ExReleasePushLockExclusive(&wddm->PagingBuildLock);KeLeaveCriticalRegion();
    ProgressExit(ProgressSiteBuildPagingBuffer,(LONG)Build->Operation);
    return status;
}

// BD-114 (ANALYSIS.md 7.4): the upper bound of the engine-reset contract's fence range. Caller owns Lock.
//
// SubmittedFence[] is NOT this value - it carries the fence of a completion waiting to be reported - and
// LastReportedFence[] is the lower bound. Bc250AbortedFenceValid needs the newest fence dxgkrnl has actually
// submitted on this node, so it is recorded in the two submit DDI wrappers, which is where every submission of
// either node passes whether or not it reaches the ring. Both of them, not only the virtual one: a node-0
// context that ever submits through DxgkDdiSubmitCommand would otherwise leave this value behind the fence the
// report DPC has already published, and the guard would refuse a report the contract asks for. The wrap-aware
// comparison is bc250_fence_reached's, because fence ids are 32 bits and wrap; it is also the comparison
// RejectedFence uses a few lines below each call site.
static void WddmNoteSubmittedLocked(_Inout_ BC250_WDDM* Wddm, UINT Node, UINT Fence)
{
    if (!Wddm->LastSubmittedValid[Node] || (LONG)(Fence - (UINT)Wddm->LastSubmittedFence[Node]) > 0)
    {
        Wddm->LastSubmittedFence[Node] = (LONG)Fence;
        Wddm->LastSubmittedValid[Node] = TRUE;
    }
}

// Caller holds Lock. The upper watermark records entry BEFORE Impl, including
// an entry that Impl later rejects. It is an upper bound, not proof of execution.
// A reset freezes both hardware submissions and CPU/software completion paths.
// Keep any new packet outstanding for reset/replay; never invent a completion.
static BOOLEAN WddmBeginSubmissionLocked(BC250_WDDM* Wddm, UINT Node, UINT Fence)
{
    WddmNoteSubmittedLocked(Wddm, Node, Fence);
    if (Wddm->Recovery[Node].ResetActive) return FALSE;
    Wddm->ActiveSubmissions[Node]++;
    return TRUE;
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
    BC250_WDDM_OBJECT* context;
    UINT node;
    BOOLEAN tracked, admitted = TRUE;
    NTSTATUS status;
    KIRQL irql;
    ProgressEnterInput(ProgressSiteSubmitCommand, (LONG)pSubmitCommand->SubmissionFenceId);   // before WddmObject: its list walk counts as inside
    context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    node = context != NULL ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;
    tracked = wddm != NULL && node < BC250_WDDM_NODE_COUNT_MAX;
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        admitted = WddmBeginSubmissionLocked(wddm, node, pSubmitCommand->SubmissionFenceId);
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    if (!admitted) {
        ProgressExit(ProgressSiteSubmitCommand, (LONG)pSubmitCommand->SubmissionFenceId);
        return STATUS_SUCCESS;
    }
    status = Bc250WddmSubmitCommandImpl(hAdapter, pSubmitCommand);
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        wddm->ActiveSubmissions[node]--;
        KeReleaseSpinLock(&wddm->Lock, irql);
        WddmQueueReport(wddm);
    }
    ProgressExit(ProgressSiteSubmitCommand, (LONG)pSubmitCommand->SubmissionFenceId);
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
    ULONGLONG hipId = 0;
    NTSTATUS journalStatus;
    BOOLEAN ringNode;

    ib.num_ibs = 0;
    ib.ip_type = 0;
    ib.ib_va = 0;
    ib.ib_bytes = 0;
    ib.single_ib = 0;
    ib.fence_va = ib.fence_value = 0;
    if (bytes != NULL && umdLen != 0 && umdLen <= bufLen)
        st = UmdBlobParseSubmit(bytes, umdLen, &ib);
    // The IB nodes are node 0 and, when it exists, node 2; each runs one IP (UmdBlobIpRunsOnNode).
    ringNode = Node == BC250_WDDM_NODE_3D ||
               (Node == BC250_WDDM_NODE_COMPUTE && Wddm != NULL && Wddm->NodeCount > BC250_WDDM_NODE_COMPUTE);
    if (st != UMD_BLOB_OK || !ib.single_ib || !ringNode || !UmdBlobIpRunsOnNode(ib.ip_type, Node))
    {
        GuardLog("wddm: malformed virtual UMD submission fence %u rejected (blob %d, single %d, node %u)",
                 Submit->SubmissionFenceId,st,ib.single_ib,Node);
        return STATUS_INVALID_PARAMETER;
    }

    if (Context == NULL) return STATUS_INVALID_PARAMETER;
    journalStatus = HipJournalBind(&Context->HipJournalOwner, ib.ib_va, ib.fence_va, ib.fence_value,
                                   Submit->SubmissionFenceId, ib.ib_bytes, &hipId);
    // HipJournalBind also answers STATUS_INVALID_DEVICE_STATE (an owner no longer registered), and this DDI may
    // fail with STATUS_INVALID_PARAMETER only: any other failure is a bugcheck. Both mean this device's submission
    // is refused, so it gets the malformed blob's answer (tools/quality/ddi_returns.py checks the chain).
    if (!NT_SUCCESS(journalStatus))
    {
        GuardLog("wddm: virtual UMD submission fence %u refused by the HIP journal (0x%08X)",
                 Submit->SubmissionFenceId, journalStatus);
        return STATUS_INVALID_PARAMETER;
    }

    // The ring holds seven jobs (driver/shim/bc250_ring.c, bc250_ring_has_space) and the completion
    // queue BC250_GFX_PENDING_MAX. A UMD submit that arrives while either one is full - behind presents,
    // or with dxgkrnl pipelining packets - must not be retired here: dxgkrnl
    // would signal the monitored fence for an IB the GPU never fetched. This DDI is PASSIVE_LEVEL,
    // so wait the same bound the watchdog uses, on the retirement event, and try again. Anything else
    // (gate closed, ring abandoned, a bad blob) does not get that wait.
    if (st == UMD_BLOB_OK && ib.single_ib && ringNode && Context->RootPhysical != 0 &&
        Wddm != NULL && KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        BC250_WDDM_HOLD hold;
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

        WddmHoldBegin(Device, &hold);
        for (;;)
        {
            if (WddmRingAdmits(Device, Node) &&
                WddmRingSubmit(Device, Wddm, Context, ib.ib_va, ib.ib_bytes, Submit->SubmissionFenceId, Node, hipId))
            {
                WddmHoldReport(Wddm, &hold, Submit->SubmissionFenceId, "umd");
                if (InterlockedIncrement(&Wddm->UmdSubmitHw) <= 128)
                    GuardLog("wddm: umd submit fence %u ib 0x%llX %lu bytes", Submit->SubmissionFenceId,
                             ib.ib_va, ib.ib_bytes);
                return STATUS_SUCCESS;
            }
            // Refresh retirement before the terminal retry, the same reason WddmSubmitPresentHardware does it:
            // a completion can race the separate ready/busy observations or release the last queue slot.
            WddmRingFence(Device, Node);
            // Not busy: either the ring will not take an IB, or the one it held finished between
            // the ready check and this one. Try once more in the second case, and do not spin in
            // the first. A timeout is the same refusal the watchdog already makes.
            if (!WddmRingBusy(Device, Node) || !WddmHoldWait(Device, Wddm, &hold))
            {
                if (WddmRingAdmits(Device, Node) &&
                    WddmRingSubmit(Device, Wddm, Context, ib.ib_va, ib.ib_bytes, Submit->SubmissionFenceId, Node, hipId))
                {
                    WddmHoldReport(Wddm, &hold, Submit->SubmissionFenceId, "umd");
                    if (InterlockedIncrement(&Wddm->UmdSubmitHw) <= 128)
                        GuardLog("wddm: umd submit fence %u ib 0x%llX %lu bytes", Submit->SubmissionFenceId,
                                 ib.ib_va, ib.ib_bytes);
                    return STATUS_SUCCESS;
                }
                break;
            }
        }
        WddmHoldReport(Wddm, &hold, Submit->SubmissionFenceId, "umd");
    }
    if (st != UMD_BLOB_OK) why = UmdBlobStatusText(st);
    else if (!ib.single_ib) { why = "multiple ibs"; nIbs = ib.num_ibs; }
    else if (!ringNode) why = "not an IB node";
    else if (!UmdBlobIpRunsOnNode(ib.ip_type, Node)) why = "ip not of this node";
    else if (Context->RootPhysical == 0) why = "no root";
    else if (KeGetCurrentIrql() > APC_LEVEL) why = "irql";
    else if (!WddmRingAdmits(Device, Node)) why = "ring not ready";
    else why = "ring refused";
    if (Wddm != NULL && InterlockedIncrement(&Wddm->UmdSubmitSoft) <= 128)
        GuardLog("wddm: umd submit fence %u not run: %s (%u ibs, private %u/%u, first 0x%08lX)",
                 Submit->SubmissionFenceId, why, nIbs, umdLen, bufLen, UmdBlobFirstWord(bytes, umdLen));
    WddmFailSubmission(Device, Submit->SubmissionFenceId, Node);
    HipJournalFailed(hipId, STATUS_DEVICE_NOT_READY);
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
    // Rule 3 of gdi_batch.h: the DMA buffer of this submission is on its way to the ring as one
    // indirect buffer, so the next RenderGdi call into it starts its fill again. Only the buffer
    // this submission names is released, by its own address range, because a submission of one
    // buffer says nothing about the content of another (review r4, section 2).
    if (context != NULL && context->GdiContext && wddm != NULL)
    {
        KIRQL irql;
        unsigned int freed;

        KeAcquireSpinLock(&context->GdiDmaLock, &irql);
        freed = Bc250GdiDmaSubmitted(&context->GdiDma, pSubmitCommand->DmaBufferVirtualAddress,
                                     pSubmitCommand->DmaBufferSize);
        KeReleaseSpinLock(&context->GdiDmaLock, irql);
        if (freed != 0) InterlockedAdd64(&wddm->GdiRenderDmaSubmitted, (LONG64)freed);
    }
    if (first)
        GuardLog("wddm: SubmitCommandVirtual fence %u node %u flags 0x%08X va 0x%llX size %u",
                 pSubmitCommand->SubmissionFenceId, node, pSubmitCommand->Flags.Value,
                 (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress, pSubmitCommand->DmaBufferSize);

    // Node 1 retrieves commands only from this submission's OS-owned private buffer.
    // The record headers bind addresses to byte ranges; there is no global lookup/clamping.
    // BD-167 (V2/G2): the node-1 paging arm runs an OS-owned paging buffer at the context's root. Paging is
    // VidMm's own work, so the context must be a SystemContext, not merely "non-UMD with empty private data";
    // a non-system context that reaches node 1 falls through and is refused below (WddmFailSubmission).
    if (node == BC250_WDDM_NODE_COPY && wddm != NULL && wddm->NodeCount > BC250_WDDM_NODE_COPY &&
        context != NULL && !context->UmdContext && context->SystemContext &&
        pSubmitCommand->DmaBufferUmdPrivateDataSize == 0 &&
        pSubmitCommand->DmaBufferSize != 0 && KeGetCurrentIrql() <= APC_LEVEL)
    {
        // Before the submit: a submit that goes to the hardware at once stamps the sequence by this fence.
        PagingJournalStampFence(pSubmitCommand->DmaBufferVirtualAddress,pSubmitCommand->DmaBufferSize,
                                pSubmitCommand->SubmissionFenceId);
        if (WddmSubmitPagingHardwareRoot(device,wddm,pSubmitCommand->pDmaBufferPrivateData,
                pSubmitCommand->DmaBufferPrivateDataSize,pSubmitCommand->DmaBufferVirtualAddress,
                pSubmitCommand->DmaBufferSize,TRUE,pSubmitCommand->SubmissionFenceId,context->RootPhysical)) return STATUS_SUCCESS;
        InterlockedIncrement(&wddm->PagingVirtualUnmapped);
    }

    // Driver-generated GPU Present must be recognized before UMD BC2S dispatch.
    // The record is OS-owned private data, with an exact VA/length binding. No
    // malformed BGP1 can fall through to BC2S or CPU E26P completion.
    // M656: even a non-UMD Present reports our consumed24-byte private record
    // as DmaBufferUmdPrivateDataSize. It is not required to be zero here.
    // Accept exactly the producer's record span, never arbitrary UMD data.
    if (pSubmitCommand->Flags.Present && pSubmitCommand->pDmaBufferPrivateData != NULL &&
        pSubmitCommand->DmaBufferPrivateDataSize >= sizeof(ULONG) &&
        *(const ULONG*)pSubmitCommand->pDmaBufferPrivateData == BC250_GFX_PRESENT_MAGIC)
    {
        if (wddm == NULL || !wddm->GpuPresentGate || context == NULL || context->UmdContext ||
            node != BC250_WDDM_NODE_3D || context->RootPhysical == 0 ||
            KeGetCurrentIrql() > APC_LEVEL ||
            !Bc250GfxPresentSubmitMatches(pSubmitCommand->pDmaBufferPrivateData,
                pSubmitCommand->DmaBufferPrivateDataSize, pSubmitCommand->DmaBufferUmdPrivateDataSize,
                pSubmitCommand->DmaBufferVirtualAddress, pSubmitCommand->DmaBufferSize)) {
            LONG64 rejected = wddm ? InterlockedIncrement64(&wddm->GpuPresentSubmitRejected) : 0;
            // DWM026 built valid-looking IB spans but failed admission. Keep the
            // original checks; expose every input before changing any contract.
            if (rejected > 0 && rejected <= 16 && KeGetCurrentIrql() <= DISPATCH_LEVEL) {
                ULONG words[BC250_GFX_PRESENT_RECORD_BYTES / sizeof(ULONG)] = {0};
                if (pSubmitCommand->DmaBufferPrivateDataSize >= sizeof(words))
                    RtlCopyMemory(words, pSubmitCommand->pDmaBufferPrivateData, sizeof(words));
                GuardLog("wddm: GPU Present reject%lld ctx%p fence%llu node%u irql%u",
                    rejected, (void*)context, pSubmitCommand->SubmissionFenceId,
                    node, (UINT)KeGetCurrentIrql());
                GuardLog("wddm: GPU Present reject%lld gate%u umd%u root%llX private%u umdprivate%u",
                    rejected, (UINT)wddm->GpuPresentGate, context ? (UINT)context->UmdContext : 0,
                    context ? context->RootPhysical : 0,
                    pSubmitCommand->DmaBufferPrivateDataSize, pSubmitCommand->DmaBufferUmdPrivateDataSize);
                GuardLog("wddm: GPU Present reject%lld va%llX bytes%u match%u",
                    rejected, pSubmitCommand->DmaBufferVirtualAddress, pSubmitCommand->DmaBufferSize,
                    (UINT)Bc250GfxPresentMatches(pSubmitCommand->pDmaBufferPrivateData,
                        pSubmitCommand->DmaBufferPrivateDataSize, pSubmitCommand->DmaBufferVirtualAddress,
                        pSubmitCommand->DmaBufferSize));
                GuardLog("wddm: GPU Present reject%lld words %08X %08X %08X %08X %08X %08X",
                    rejected, words[0], words[1], words[2], words[3], words[4], words[5]);
            }
            return STATUS_INVALID_PARAMETER;
        }
        if (WddmSubmitPresentHardware(device,wddm,context,
                pSubmitCommand->DmaBufferVirtualAddress,pSubmitCommand->DmaBufferSize,
                pSubmitCommand->SubmissionFenceId,node)) {
            if (InterlockedIncrement64(&wddm->GpuPresentSubmits)<=16)
            {
                GuardLog("wddm: GPU Present submit ctx%p fence%llu va%llX bytes%u",
                    (void*)context,pSubmitCommand->SubmissionFenceId,
                    pSubmitCommand->DmaBufferVirtualAddress,pSubmitCommand->DmaBufferSize);
                GuardLog("wddm: GPU Present submitted ctx%p fence%llu root%llX node%u",
                    (void*)context,pSubmitCommand->SubmissionFenceId,context->RootPhysical,node);
            }
            return STATUS_SUCCESS;
        }
        InterlockedIncrement64(&wddm->GpuPresentSubmitFailed);
        WddmFailSubmission(device,pSubmitCommand->SubmissionFenceId,node);
        return STATUS_SUCCESS; // preserve the existing nonempty-work recovery contract
    }

    // M8. A UMD context's packet is the IB in its BC2S blob, not the DMA buffer a present uses. Handled
    // here, before stage C, so a UMD submit can never fall through onto DmaBufferVirtualAddress. One IB
    // takes the same gated gfx-ring path. Unsupported multi-IB packets are rejected before dispatch.
    if (context != NULL && context->UmdContext)
    {
        return WddmSubmitUmd(device, wddm, context, pSubmitCommand, node);
    }

    // Only driver-built, non-UMD present packets enter the CPU presentation path.
    // The scheduler has selected this context's root. Residency is owned by the
    // device residency list, not the Present allocation list. CDD/system callers
    // must retain both surfaces there through completion. Complete the fence only
    // after all copied rows are visible.
    // BD-167 (V2): the software Present packet is built by this driver's own Present DDI for the CDD/DWM, so it
    // is admitted only from the context classes dxgkrnl builds those on (SystemContext or GdiContext), not from
    // any non-UMD context. The BC250_PRESENT_PACKET_MAGIC below still validates the record itself.
    if (context != NULL && !context->UmdContext && (context->SystemContext || context->GdiContext) &&
        pSubmitCommand->Flags.Present &&
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
    // node == BC250_WDDM_NODE_3D: this hardware path is GfxSubmitIb's, the gfx ring at a VMID gfx.c chooses, and it stays
    // node 0's alone - node 1 has its own arm above, its own ring (SDMA0, no VMID) and its own failure counters.
    if (node == BC250_WDDM_NODE_3D && pSubmitCommand->DmaBufferSize != 0 && context != NULL && context->RootPhysical != 0 &&
        device->Wddm != NULL && KeGetCurrentIrql() <= APC_LEVEL && GfxSubmitReady(device) &&
        WddmSubmitHardware(device, (BC250_WDDM*)device->Wddm, context, (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress,
                           pSubmitCommand->DmaBufferSize, pSubmitCommand->SubmissionFenceId, node, 0))
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
    BC250_WDDM_OBJECT* context;
    UINT node;
    BOOLEAN tracked, admitted = TRUE;
    NTSTATUS status;
    KIRQL irql;
    ProgressEnterInput(ProgressSiteSubmitCommandVirtual, (LONG)pSubmitCommand->SubmissionFenceId);    // before WddmObject: its list walk counts as inside
    context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    node = context != NULL ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;
    tracked = wddm != NULL && node < BC250_WDDM_NODE_COUNT_MAX;
    if (tracked)
    {
        KeAcquireSpinLock(&wddm->Lock, &irql);
        admitted = WddmBeginSubmissionLocked(wddm, node, pSubmitCommand->SubmissionFenceId);
        KeReleaseSpinLock(&wddm->Lock, irql);
    }
    if (!admitted) {
        ProgressExit(ProgressSiteSubmitCommandVirtual, (LONG)pSubmitCommand->SubmissionFenceId);
        return STATUS_SUCCESS;
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
    ProgressExit(ProgressSiteSubmitCommandVirtual, (LONG)pSubmitCommand->SubmissionFenceId);
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
    if (device!=NULL) { StartHealthClose(device); GfxSubmitFail(device); GfxPagingSubmitFail(device); GfxComputeClose(device); }
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
    StartHealthClose((BC250_DEVICE*)hAdapter);
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

// PASSIVE_LEVEL only. On success return 0 WITH Lock held, so the caller's
// fence snapshot belongs to this admission. On refusal return the complete
// reason mask with Lock released and both submission gates unchanged.
// The first-level scheduler list does not name SubmitCommandVirtual. Do not
// assume that it excludes an existing held submitter. Give that call at most
// 500 ms to finish, with no lock held across the sleep and no DPC join. This
// is a software wait budget, not a guarantee about OS thread scheduling.
static ULONG WddmResetAdmit(BC250_DEVICE* Device, BC250_WDDM* Wddm, UINT Node, KIRQL* Irql)
{
    ULONGLONG deadline = KeQueryInterruptTime() + 10000ull * BC250_HANG_ADMISSION_WAIT_MS;
    BOOLEAN signalled = FALSE;
    ULONG reasons;
    LARGE_INTEGER delay;

    for (;;) {
        KeAcquireSpinLock(&Wddm->Lock, Irql);
        reasons = Bc250HangAdmissionReasons(&Wddm->Recovery[Node], Wddm->Stopping,
                                            Wddm->CompletionPending[Node], Wddm->ActiveSubmissions[Node]);
        if (reasons == 0) {
            // Same lock and conditions as the mask, so ResetBegin must succeed.
            (void)Bc250HangResetBegin(&Wddm->Recovery[Node], 0);
            Wddm->WatchdogFaulted[Node] = TRUE;
            if (Node == BC250_WDDM_NODE_COMPUTE) { GfxComputeClose(Device); KeCancelTimer(&Wddm->ComputeTimer); }
            else { GfxSubmitClose(Device); KeCancelTimer(&Wddm->SubmitTimer); }
            return 0;
        }
        KeReleaseSpinLock(&Wddm->Lock, *Irql);
        if (reasons != BC250_HANG_ADMISSION_ACTIVE_SUBMISSIONS ||
            KeQueryInterruptTime() >= deadline) return reasons;
        if (!signalled) {
            GfxRetireSignal(Device);
            signalled = TRUE;
        }
        delay.QuadPart = -10000; // one millisecond, outside Lock at PASSIVE_LEVEL
        (void)KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }
}

static DXGKDDI_RESETENGINE Bc250WddmResetEngine;
static NTSTATUS Bc250WddmResetEngine(_In_ const HANDLE hAdapter, _Inout_ DXGKARG_RESETENGINE* pResetEngine)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    (void)WddmFirstCalls(wddm, WddmDdiResetEngine);

    // M15.12 stage 1 (docs/design/hang-recovery.md), behind HangRecoveryMode, for the two IB nodes: node 0 (3D) and
    // node 2 (compute), each on its own ring, queue and fence. Node 1 (paging) is never soft-recovered: dxgkrnl
    // follows a successful paging-packet reset with an adapter-wide reset (TDR changes in Windows 8, step 9), which
    // on this part is the 0x116 path anyway, and the hang class never involves node 1.
    if (wddm != NULL && Bc250SoftRecoveryWanted(wddm->HangRecoveryMode, pResetEngine->NodeOrdinal))
    {
        const BC250_GFX_COMPLETION* head;
        const BC250_GFX_COMPLETION* tail;
        ULONG verdict, seq, kills = 0, micros = 0, vmid = 0, reasons;
        UINT hungFence = 0, lastCompleted = 0, lastSubmitted = 0, abortFence = 0, notifiedFence;
        BOOLEAN onRing;
        BC250_START_HEALTH_RECOVERY witness = BC250_START_HEALTH_RECOVERY_STICKY;
        int lastKnown, lastSubmittedKnown, completionPending, reportedKnown, abortReported = 0;
        unsigned node = pResetEngine->NodeOrdinal;
        ULONGLONG epoch, admittedAt, witnessEpoch = 0;
        BC250_HANG_NODE_STATE snapshot;
        BC250_GFX_COMPLETION_QUEUE* queue = WddmRingQueue(wddm, node);
        KIRQL irql;

        WddmRingFence(device, node);    // retire anything that arrived late before deciding there is still a hang
        reasons = WddmResetAdmit(device, wddm, node, &irql);
        if (reasons != 0) {
            GuardLog("wddm: ResetEngine node %u admission refused, reasons 0x%02lX", node, reasons);
            // Persist every failed gate from the same observation, even if a
            // later adapter reset/0x116 destroys the volatile log ring.
            GuardRecordHangRecovery(BC250_HANG_VERDICT_ADMISSION_GUARD, reasons, 0, 0, 0, FALSE);
            goto refuseReset;
        }
        // WddmResetAdmit returned with Lock held and both gates closed. No
        // callback is in flight, no new submit Impl or retirement can start.
        admittedAt = KeQueryInterruptTime();
        epoch = wddm->Recovery[node].Epoch;
        snapshot = wddm->Recovery[node];
        head = Bc250GfxQueueHead(queue);
        // Only a job of the node being reset. The queue is that node's, but the entry carries its node and the
        // DDI names one, so the two are compared instead of assumed.
        onRing = head != NULL && head->Node == pResetEngine->NodeOrdinal;
        // The head job's OS fence: submitted, never retired, so in [completed, submitted]. With nothing on the ring
        // there is no fence to abort, so 0. Its VMID is the one the kill may name (KMD214: the pool gives each
        // page-table root its own VMID, so there is no single application VMID any more).
        if (onRing) { hungFence = head->Fence; vmid = head->Vmid; }
        tail = Bc250GfxQueueTail(queue);
        seq = tail != NULL ? tail->Seq : 0;     // the newest sequence on the ring, the one a drain has to retire
        // The lower bound of the 0x119 range is THIS node's last reported fence, never the adapter-wide
        // LastCompletedFence: fence ids are per node, and node 1's run far ahead of node 0's. 0.7.216.13 read the
        // shared field, so a paging completion after the hang (trial D: node 1 at 9228, node 0 at 1185, hung 1186)
        // made the guard refuse a valid recovery (hang_recovery.h, Bc250HangNodeLastCompleted).
        lastKnown = Bc250HangNodeLastCompleted(wddm->LastReportedFence, wddm->LastReportedValid,
                                               BC250_WDDM_NODE_COUNT_MAX, pResetEngine->NodeOrdinal, &lastCompleted);
        reportedKnown = lastKnown;
        notifiedFence = lastCompleted;
        lastKnown = Bc250HangSchedulerBoundary(&snapshot, lastKnown, lastCompleted, &lastCompleted);
        // BD-114 (ANALYSIS.md 7.4): the two further reads the aborted-fence answer needs, taken in the same pass
        // under the same lock, so that "nothing on the ring" and "no completion pending" describe one moment.
        completionPending = wddm->CompletionPending[pResetEngine->NodeOrdinal] != 0;
        lastSubmittedKnown = wddm->LastSubmittedValid[pResetEngine->NodeOrdinal] != 0;
        lastSubmitted = (UINT)wddm->LastSubmittedFence[pResetEngine->NodeOrdinal];
        KeReleaseSpinLock(&wddm->Lock, irql);

        // Decided before the hardware is touched: no job of this node on the ring, an abort fence outside the
        // engine-reset contract, or a VMID a broadcast kill must not name, is today's refusal. The fence-range
        // guard is defence-in-depth against bugcheck 0x119: the aborted fence is the head job's own, so it is above
        // the last completed and no newer than itself, but we check rather than trust - and before the kill, so
        // that a refusal never follows a kill we cannot report.
        verdict = Bc250HangPreKillVerdict(onRing, hungFence, lastKnown, lastCompleted, vmid);
        // The empty-queue answer is the observed completion, not the last callback
        // or reset boundary. The frozen snapshot remains stable for this node.
        if (verdict == BC250_HANG_VERDICT_NOTHING_ON_RING &&
            Bc250HangAbortCompletedFence(&snapshot, onRing, completionPending, reportedKnown,
                                         notifiedFence, lastSubmittedKnown,
                                         lastSubmitted, &abortFence))
        {
            abortReported = 1;
            hungFence = abortFence;
            verdict = BC250_HANG_VERDICT_ABORT_REPORTED;
        }
        if (verdict == BC250_HANG_VERDICT_PENDING)
        {
            // On the disk before the first SQ_CMD write: should the kill itself take the machine down, the record
            // read after the reboot still says an attempt was under way (LastVerdict 0, Attempts one ahead).
            GuardRecordHangRecovery(BC250_HANG_VERDICT_PENDING, seq, hungFence, 0, 0, wddm->HangRecoveryWaves);
            // Capture and flush before the first destructive recovery write. Failure is diagnostic only.
            if (wddm->HangRecoveryWaves) GfxRecordWavesBeforeKill(device, seq, hungFence, vmid);
            GuardLog("wddm: soft recovery admit-to-kill %llu us, waves %u",
                     (KeQueryInterruptTime() - admittedAt) / 10ull, (ULONG)wddm->HangRecoveryWaves);
            // Kill the waves of the hung job's VMID until the newest sequence retires, for at most 10 ms. A
            // recovered verdict means the end-of-pipe behind the killed waves fired and the ring drained to idle.
            verdict = node == BC250_WDDM_NODE_COMPUTE ? GfxComputeSoftRecover(device, vmid, &seq, &kills, &micros) :
                      GfxSoftRecover(device, vmid, &seq, &kills, &micros);
        }
        if (Bc250HangVerdictRecovered(verdict))
        {
            KeAcquireSpinLock(&wddm->Lock, &irql);
            // Reopen both gates atomically with the epoch validation. The timer's
            // latch uses this same lock; its unlocked tail contains diagnostics only.
            if (wddm->Stopping || !wddm->Recovery[node].ResetActive ||
                wddm->Recovery[node].Epoch != epoch ||
                !(node == BC250_WDDM_NODE_COMPUTE ? GfxComputeReopenAfterAbort(device) : GfxReopenAfterAbort(device))) {
                Bc250HangResetEnd(&wddm->Recovery[node], FALSE, 0);
                KeReleaseSpinLock(&wddm->Lock, irql);
                // The empty-queue path has not written PENDING (which counts an
                // attempt); retain its refusal class so the counters stay balanced.
                verdict = abortReported ? BC250_HANG_VERDICT_NOTHING_ON_RING : BC250_HANG_VERDICT_NOT_DRAINED;
                GuardRecordHangRecovery(verdict, seq, hungFence, kills, micros, FALSE);
                goto refuseReset;
            }
            // Drop the hung job (and anything queued behind it on this node) so no stale entry double-reports;
            // the ring is idle, GfxReopenAfterAbort cleared SubmitInFlight. dxgkrnl resubmits the later render packets
            // with new fence ids ("Packets unaffected by engine reset"), so dropping them loses no work.
            while ((head = Bc250GfxQueueHead(queue)) != NULL) {
                HipJournalReset(head->HipJournalId, head->Fence != hungFence);
                Bc250GfxQueuePop(queue);
            }
            wddm->WatchdogFaulted[node] = FALSE;      // reopen the node: submits are admitted again
            wddm->RefusalPending[node] = FALSE;
            // A rejected packet was never dispatched. After a reported reset dxgkrnl re-issues the render packets
            // it still owes with NEW fence ids, so publishing the old rejected id afterwards would name a fence it
            // no longer tracks. Drop the pending report with the queue.
            wddm->RejectedPending[node] = FALSE;
            wddm->CompletionPending[node] = 0;
            wddm->CompletionRetries[node] = 0;        // 0.7.210: a fresh retry budget for the next pass
            if (node == BC250_WDDM_NODE_COMPUTE) wddm->ComputeDeferredValid = FALSE;
            else wddm->DeferredValid = FALSE;
            // The TDR issues a preempt request before ResetEngine, so PreemptionPending[0] is almost always set
            // here. The reset subsumes it: clear it so the DPC does not later report a stale DMA_PREEMPTED for a
            // preemption the aborted fence has already superseded.
            wddm->PreemptionPending[node] = 0;
            // dxgkrnl treats the aborted fence and everything below it as completed; advance this node's notion to
            // match, and only this node's. The adapter-wide LastCompletedFence is not written: it holds the newest
            // report of either node, and node 1's ids run ahead of node 0's, so writing hungFence there (0.7.216.13)
            // moved it backwards below fences node 1 had already reported.
            wddm->SubmittedFence[node] = (LONG)hungFence;
            Bc250HangResetEnd(&wddm->Recovery[node], TRUE, hungFence);
            // BD-120: the presentation start witness re-arms with the node, inside this same transaction, so that
            // no reader can see node 0 admitting work while the witness still reports the hang. It undoes only the
            // close this reset owns (the timeout latch's), which is why a fault of any other kind, before or after
            // the latch, keeps the witness closed for the rest of this device start. A witness our latch never
            // closed - the ordering in which the OS TDR arrives first - gets the new epoch all the same, so no
            // interval ever spans a hang. The three answers are logged after the lock, with the rest of the
            // recovery.
            // Node 2 does not witness the start (gfx.c GfxComputeClose), so its reset has nothing to reopen there.
            if (node == BC250_WDDM_NODE_3D) witness = StartHealthRecovered(device, &witnessEpoch);
            // Last, with the queue already empty: it clears HwPending, cancels the submit watchdog and closes the
            // ring-gap edge, which a hand-written HwPending = FALSE would have left open (0.7.210's histogram).
            WddmRingHeadLocked(wddm, node);
            KeReleaseSpinLock(&wddm->Lock, irql);
            // Only now: the node is open again in BOTH files. GfxReopenAfterAbort deliberately leaves the wake to
            // this line. GfxSoftRecover only drains; it never opens or signals either gate.
            GfxRetireSignal(device);
            InterlockedIncrement(&wddm->SoftRecoveries);
            GuardRecordHangRecovery(verdict, seq, hungFence, kills, micros, FALSE);
            // Valid by construction, by one of two arguments. Verdicts 1 and 5: hungFence was submitted and, when
            // read, still the unreported head of the queue, so it is in [LastCompletedFenceId, last submitted].
            // Verdict 7: hungFence is this node's observed completion and its validity comes from the range
            // guard of Bc250HangAbortCompletedFence, which refused the report if it was not inside that range. A
            // value outside it would be bugcheck 0x119. ALREADY_RETIRED is the contract's special case of a packet
            // that completed between the timeout and the reset: dxgkrnl treats it as aborted, which is what it
            // asks for (tdr-changes-in-windows-8.md).
            pResetEngine->LastAbortedFenceId = hungFence;
            // Two lines, not one: the log ring's line is 159 characters and the worst-case width gate
            // (tools/quality/guardlog_width.py, BD-070) counts every %lu at ten.
            if (abortReported)
                GuardLog("wddm: *** ResetEngine node %u: nothing on the ring, completed fence %u named as"
                         " aborted, node reopened ***", pResetEngine->NodeOrdinal, hungFence);
            else
                GuardLog("wddm: *** ResetEngine node %u: SOFT RECOVERED, aborted fence %u, node reopened ***",
                         pResetEngine->NodeOrdinal, hungFence);
            GuardLog("wddm: soft recovery: verdict %lu seq %lu vmid %lu, %lu kill(s) in %lu us", verdict, seq, vmid,
                     kills, micros);
            // BD-120. Which of the three the witness did, and the epoch a health reader now sees. The middle one
            // is not a fault report: it is the ordering in which the OS TDR arrived before our own latch, so
            // there was no close to undo and only the interval ended.
            if (node != BC250_WDDM_NODE_3D)
                ;                   // node 2: no start witness (above)
            else if (witness == BC250_START_HEALTH_RECOVERY_REOPENED)
                GuardLog("wddm: start witness reopened after recovery: epoch %llu, ready age and completions"
                         " restart now", witnessEpoch);
            else if (witness == BC250_START_HEALTH_RECOVERY_REARMED)
                GuardLog("wddm: start witness was still open at the recovery: epoch %llu, the hung interval"
                         " ends here", witnessEpoch);
            else
                GuardLog("wddm: start witness stays closed after recovery at epoch %llu: a fault of its own,"
                         " outside this reset, is recorded", witnessEpoch);
            if (abortReported)
                GuardLog("wddm: aborted-fence report: node %u lower boundary %u, last submitted %u (BD-114 7.4)",
                         pResetEngine->NodeOrdinal, lastCompleted, lastSubmitted);
            return STATUS_SUCCESS;
        }
        KeAcquireSpinLock(&wddm->Lock, &irql);
        Bc250HangResetEnd(&wddm->Recovery[node], FALSE, 0);
        KeReleaseSpinLock(&wddm->Lock, irql);
        GuardRecordHangRecovery(verdict, seq, hungFence, kills, micros, FALSE);
        // Not recovered (verdict 2, 3, 4 or 6): retain both closed gates
        // and fall through to today's refusal (then 0x116).
        GuardLog("wddm: ResetEngine node %u: soft recovery verdict %lu, refusing as before",
                 pResetEngine->NodeOrdinal, verdict);
        GuardLog("wddm: soft recovery refused: seq %lu fence %u vmid %lu, %lu kill(s) in %lu us", seq, hungFence,
                 vmid, kills, micros);
        // The bound the fence guard compared with, so that a verdict 4 says on its own which value refused it
        // (trial D needed a dump and a script to find that out).
        if (verdict == BC250_HANG_VERDICT_FENCE_GUARD)
            GuardLog("wddm: soft recovery fence guard: node %u last reported %u (known %d), abort fence %u",
                     pResetEngine->NodeOrdinal, lastCompleted, lastKnown, hungFence);
    }

    // Today's behaviour (HangRecoveryMode off, node 1, or a stage-1 verdict that is not a recovery): the documented
    // answer of hardware that "is incapable of resetting the nodes" - a failure status, after which
    // the scheduler falls back to the adapter-wide ResetFromTimeout. It is also the careful answer: a success
    // with a LastAbortedFenceId outside [last completed, last submitted] is bugcheck 0x119, and a failure names
    // no fence at all. Logged on every call, like the two timeout DDIs and for the same reason.
refuseReset:
    // BD-120: a refusal on THIS node is a fault of its own and stays sticky, exactly as before - the close
    // takes the pending-reset mark with it, so no later reset reopens the witness. A refusal on node 1 says
    // nothing about node 0: node 1 is never soft-recovered by policy, and clearing node 0's mark here would
    // leave the node-0 reset that follows with nothing to reopen.
    if (pResetEngine->NodeOrdinal == BC250_WDDM_NODE_3D) StartHealthClose(device);
    else StartHealthCloseKeepPendingReset(device);
    // A refusal before recovery admission may leave a held submission waiting
    // while the scheduler moves on to ResetFromTimeout (which closes the path). Wake
    // the waiters anyway: one extra retest on a TDR path is cheaper than reasoning about which recovery DDI the
    // scheduler happens to call first.
    if (hAdapter != NULL) GfxRetireSignal((BC250_DEVICE*)hAdapter);
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
// not the same as having seen it fail). What it asks for - clocks that do not move while a profiler looks - is the
// lab floor (1000 MHz): fixed-lab never leaves it, and the DPM governor pins itself there while this is on (dpm.c).
static DXGKDDI_SETSTABLEPOWERSTATE Bc250WddmSetStablePowerState;
static VOID Bc250WddmSetStablePowerState(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SETSTABLEPOWERSTATE* pArgs)
{
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiSetStablePowerState))
        GuardLog("wddm: SetStablePowerState enabled %u (the DPM governor pins the floor)", pArgs->Enabled ? 1u : 0u);
    DpmSetStable((BC250_DEVICE*)hAdapter, pArgs->Enabled ? TRUE : FALSE);
}

static LONG volatile g_CalibrateFallbackLogged;     // the QPC stand-in below is logged once per driver load

static int CalibrateRead(void* Context, unsigned long Offset, unsigned long* Value)
{
    ULONG value = 0;
    NTSTATUS status = MmioRead((const BC250_DEVICE*)Context, Offset, &value);
    *Value = value;
    return NT_SUCCESS(status) ? 0 : 1;
}

static unsigned long long CalibrateCpu(void* Context)
{
    UNREFERENCED_PARAMETER(Context);
    return (unsigned long long)KeQueryPerformanceCounter(NULL).QuadPart;
}

// BD-056. The D3D12 runtime hands GpuFrequency to applications as ID3D12CommandQueue::GetTimestampFrequency and the
// pair as GetClockCalibration, and the timestamps they scale are what the CP writes (RELEASE_MEM/EOP), in the SMUIO
// golden TSC's 100 MHz. Until 0.7.197 this answered stage A's CPU clock (the QPC, 10 MHz) as both counters, so every
// D3D12 GPU duration on this stack read 10x too long (frameloop quick-20261002T225506Z: GPU timeline over frame
// interval 10.01 and 9.97). Now: the TSC read as amdgpu reads it (gpu_clock.h), paired with the QPC around it, the
// narrowest of GPU_CLOCK_SAMPLES pairs, and the frequency the UMD already reads from the caps blob
// (device.gpu_counter_freq, 100000 kHz). Node 1 (copy, SDMA) stamps from the same counter and gets the same answer.
// If the BAR does not answer (no mapping, a refused read, all ones), the QPC scaled to that frequency stands in:
// durations stay right, only the correlation with the GPU's own timestamps is lost, and the log says so once.
static DXGKDDI_CALIBRATEGPUCLOCK Bc250WddmCalibrateGpuClock;
static NTSTATUS Bc250WddmCalibrateGpuClock(_In_ const HANDLE hAdapter, _In_ UINT32 NodeOrdinal, _In_ UINT32 EngineOrdinal,
                                           _Out_ DXGKARG_CALIBRATEGPUCLOCK* pClockCalibration)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    UINT nodeCount = (wddm != NULL) ? wddm->NodeCount : BC250_WDDM_NODE_COUNT;
    const unsigned long long gpuHz = UmdCapsGpuCounterHz();
    GPU_CLOCK_PAIR pair = {0};
    int error;

    UNREFERENCED_PARAMETER(EngineOrdinal);
    // ADR 0008 stage D: node 1 accepted once wddm->NodeCount says it exists (design note section 6).
    if (NodeOrdinal != BC250_WDDM_NODE_3D && !(NodeOrdinal == BC250_WDDM_NODE_COPY && nodeCount > BC250_WDDM_NODE_COPY) &&
        !(NodeOrdinal == BC250_WDDM_NODE_COMPUTE && nodeCount > BC250_WDDM_NODE_COMPUTE))
        return STATUS_INVALID_PARAMETER;
    RtlZeroMemory(pClockCalibration, sizeof(*pClockCalibration));
    error = GpuClockCalibrate(device, CalibrateRead, CalibrateCpu, BC250_REG_SMUIO_GOLDEN_TSC_COUNT_UPPER_Cyan_Skillfish,
                              BC250_REG_SMUIO_GOLDEN_TSC_COUNT_LOWER_Cyan_Skillfish, &pair);
    pClockCalibration->GpuFrequency = gpuHz;
    if (error == 0) {
        pClockCalibration->GpuClockCounter = pair.Gpu;
        pClockCalibration->CpuClockCounter = pair.Cpu;
    } else {
        LARGE_INTEGER qpcHz;
        LARGE_INTEGER qpc = KeQueryPerformanceCounter(&qpcHz);
        pClockCalibration->GpuClockCounter = GpuClockScale((ULONGLONG)qpc.QuadPart, (ULONGLONG)qpcHz.QuadPart, gpuHz);
        pClockCalibration->CpuClockCounter = (ULONGLONG)qpc.QuadPart;
        if (InterlockedCompareExchange(&g_CalibrateFallbackLogged, 1, 0) == 0)
            GuardLog("wddm: CalibrateGpuClock: TSC read failed (%d: 1 refused, 2 all ones), the QPC scaled to %llu Hz "
                     "stands in", error, gpuHz);
    }
    if (WddmFirstCalls(wddm, WddmDdiCalibrateGpuClock))
        GuardLog("wddm: CalibrateGpuClock node %u, frequency %llu, gpu %llu (tsc %08lX:%08lX%s), cpu %llu, window %llu",
                 NodeOrdinal, pClockCalibration->GpuFrequency, pClockCalibration->GpuClockCounter, pair.Upper, pair.Lower,
                 error ? ", QPC fallback" : "", pClockCalibration->CpuClockCounter, pair.Window);
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
// through the context's root, checking every page for contiguous VRAM backing. Endpoints alone do not
// establish contiguity for allocations without AccessedPhysically. The source is mapped read-only by physical
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

static int WddmPresentTranslate(void* Context, unsigned long long Va,
    unsigned long long* Physical, int* System)
{
    BOOLEAN system=FALSE;
    BOOLEAN result=VidMmTranslate(((BC250_WDDM_OBJECT*)Context)->RootPhysical,Va,Physical,&system);
    *System=system;
    return result;
}

static void WddmPresentBlitOwned(_In_ BC250_WDDM_OBJECT* Context, _In_ const DXGKARG_PRESENT* Present)
{
    BC250_DEVICE* device = Context->Device;
    BC250_WDDM* wddm = (BC250_WDDM*)device->Wddm;
    const DXGK_DISPLAY_INFORMATION* post=device->RuntimePostValid?&device->RuntimePost:&device->Post;
    PUCHAR framebuffer=device->RuntimePostValid?device->RuntimeFramebuffer:device->Framebuffer;
    SIZE_T framebufferLength=device->RuntimePostValid?device->RuntimeFramebufferLength:device->FramebufferLength;
    const ULONGLONG* raw;               // the list, read as qwords: both arms of the union are tried below
    HANDLE handle = NULL;
    BC250_WDDM_OBJECT* object = NULL;
    int reading = 0;
    const BC250_WDDM_ALLOCATION_PRIVATE* alloc = NULL;
    ULONGLONG va = 0, first = 0, last = 0;
    BOOLEAN verbose;
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
    UINT dstPitch = post->Pitch, dstWidth = DisplaySourceWidth(device), dstHeight = DisplaySourceHeight(device);


    verbose = (wddm->Calls[WddmDdiPresent] <= BC250_WDDM_LOG_CALLS);
    if (device->RuntimeDisplayChanged && !device->RuntimePostValid) why = "no active runtime target";
    else if (Present->pAllocationList == NULL || Context->AllocationListSize <= DXGK_PRESENT_MAX_INDEX) why = "no list";
    else if (KeGetCurrentIrql() != PASSIVE_LEVEL) why = "IRQL";
    else if (Context->RootPhysical == 0) why = "context has no root";
    else if (framebuffer == NULL || framebufferLength == 0) why = "no framebuffer";
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
            object = WddmIndexedObject(wddm, handle, BC250_WDDM_MAGIC_OPENED);
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
        if (!Bc250PresentVramRange(Context,WddmPresentTranslate,va,alloc->Size,&first,&last))
            why = "source pages not contiguous VRAM";
        else InterlockedIncrement(&wddm->BlitTranslations);
        if (verbose && object != NULL)
            GuardLog("wddm: blit source (reading %d) %p %ux%u pitch %u format %u size 0x%llX va 0x%llX -> 0x%llX .. 0x%llX%s%s", reading,
                     (void*)object, alloc->Width, alloc->Height, alloc->Pitch, alloc->Format, alloc->Size, va, first, last,
                     "", why != NULL ? " REFUSED" : "");
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
    dst = (UCHAR*)framebuffer;
    dstLength = framebufferLength;
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
        BC250_WDDM_OBJECT* destination = WddmIndexedObject(wddm, (HANDLE)(ULONG_PTR)raw[8], BC250_WDDM_MAGIC_OPENED);
        ULONGLONG dstFirst = 0, dstLast = 0, primaryPhysical = 0;
        const BC250_WDDM_ALLOCATION_PRIVATE* d = destination ? &destination->Allocation : NULL;
        if (d == NULL || d->Width == 0 || d->Height == 0 || d->Pitch < d->Width * 4ull ||
            d->Size < (ULONGLONG)d->Pitch * d->Height || d->Size > 0x10000000ull ||
            !WddmLinearColorFormat(d->Format) ||
            raw[9] > MAXULONGLONG - d->Size ||
            !Bc250PresentVramRange(Context,WddmPresentTranslate,raw[9],d->Size,&dstFirst,&dstLast) ||
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
        framebufferLength != 0 && device->VramLength != 0)
    {
        ULONGLONG vramBase = (ULONGLONG)device->VramPhysical.QuadPart;
        ULONGLONG seedAt = device->RuntimePostValid?(ULONGLONG)post->PhysicAddress.QuadPart:vramBase;
        SIZE_T seedBytes = framebufferLength;
        PHYSICAL_ADDRESS seedPhys;
        PVOID seedMap;

        if (!device->RuntimePostValid && device->DcnFirmwareKnown && device->DcnFirmwareAddress >= vramBase &&
            device->DcnFirmwareAddress - vramBase < device->VramLength)
            seedAt = device->DcnFirmwareAddress;
        if (seedAt >= vramBase && seedAt-vramBase < device->VramLength && seedAt != device->DcnScanoutMapAddress &&
            (ULONGLONG)seedBytes <= device->VramLength - (seedAt - vramBase))
        {
            seedPhys.QuadPart = (LONGLONG)seedAt;
            seedMap = VramMapCpuRange(device,seedPhys,seedBytes,PAGE_READONLY);
            if (seedMap != NULL)
            {
                ULONG firstPixel = *(volatile ULONG*)seedMap;

                BOOLEAN copied=WddmCopyScanoutRows(dst,dstLength,dstPitch,seedMap,seedBytes,
                    post->Pitch,post->Width,post->Height);
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
            // Use the selected target's actual pitch and bounded allocation.
            // The operation owner excludes a concurrent target-mode change.
            SIZE_T dstOff = (SIZE_T)y * dstPitch + (SIZE_T)left * 4;
            SIZE_T src = (SIZE_T)(y + dy) * alloc->Pitch + (SIZE_T)(left + dx) * 4;
            SIZE_T bytes = (SIZE_T)(right - left) * 4;

            if (dstOff + bytes > dstLength || src + bytes > alloc->Size) break;
            WddmCopyColorRow(dst + dstOff, map + src, bytes,
                WddmRedFirst(alloc->Format) != destinationRedFirst);
            if (destinationPrimary && !device->VidPnFlipEnabled &&
                right <= (LONG)post->Width && y < (LONG)post->Height)
            {
                SIZE_T postOffset = (SIZE_T)y * post->Pitch + (SIZE_T)left * 4;
                if (postOffset + bytes <= framebufferLength)
                    WddmCopyColorRow((UCHAR*)framebuffer + postOffset, map + src, bytes,
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

// The compatibility CPU path can run while a new sink is being detected.
// Serialize its geometry and surface access with passive connector transactions;
// high-IRQL Present calls retain the original no-copy result.
static void WddmPresentBlit(_In_ BC250_WDDM_OBJECT* Context, _In_ const DXGKARG_PRESENT* Present)
{
    BOOLEAN owned=ConnectorIsEnabled(Context->Device) && KeGetCurrentIrql()==PASSIVE_LEVEL;
    if (owned) ConnectorOperationAcquire(Context->Device);
    WddmPresentBlitOwned(Context,Present);
    if (owned) ConnectorOperationRelease(Context->Device);
}

// Build a complete OS-owned IB. The gate remains diagnostic until CDD/UMD
// admission, cache ordering and lifecycle have been verified on the exact build.
static NTSTATUS WddmBuildGpuPresent(BC250_WDDM_OBJECT* Context, DXGKARG_PRESENT* Present)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Context->Device->Wddm;
    HANDLE handles[2];
    BC250_WDDM_ALLOCATION_PRIVATE allocations[2];
    BC250_BLIT_SURFACE surface[2];
    BC250_BLIT_RECT src, dst, *dirty=NULL;
    ULONGLONG va[2], ib=Present->DmaBufferGpuVirtualAddress;
    ULONG record[BC250_GFX_PRESENT_RECORD_BYTES/sizeof(ULONG)];
    UINT i,next=Present->MultipassOffset,written=0;
    BC250_GFX_BLIT_RESULT result;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    // BC2C UMD admission is withheld until source AND destination residency on
    // that exact submitting device is established. ACQUIRE_MEM is not a wait
    // for another queue, and an allocation-list entry does not confer residency.
    if (Context->UmdContext || Present->Flags.Value!=1 || Context->NodeOrdinal!=BC250_WDDM_NODE_3D ||
        !Present->pAllocationInfo || Context->AllocationListSize<=DXGK_PRESENT_MAX_INDEX ||
        (Present->SubRectCnt && !Present->pDstSubRects)) return status;
    // A malformed available buffer is not exhaustion: retrying an equally
    // sized buffer cannot repair its alignment or an absent backing pointer.
    if ((Present->DmaSize && !Present->pDmaBuffer) ||
        ((ULONG_PTR)Present->pDmaBuffer&3u) ||
        (Present->DmaBufferPrivateDataSize && !Present->pDmaBufferPrivateData))
        return status;
    // DXGKARG_PRESENT reports remaining private bytes, not the context's
    // original capacity. Exhaustion can legitimately require buffer rotation.
    // Fresh non-UMD contexts have enough room for both our IB and its record.
    C_ASSERT(PAGE_SIZE>=64 && !(PAGE_SIZE&31u));
    C_ASSERT(sizeof(BC250_PRESENT_PACKET)>=BC250_GFX_PRESENT_RECORD_BYTES);
    if (Present->DmaSize<64 || Present->DmaBufferPrivateDataSize<sizeof(record))
        return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER;
    if (Present->DmaSize&31u) return status;
    if (!Bc250GfxPresentRecord(record,sizeof(record),ib,Present->DmaSize)) return status;
    for (i=0;i<2;i++) {
        const DXGK_PRESENTALLOCATIONINFO* info=Present->pAllocationInfo+
            (i ? DXGK_PRESENT_DESTINATION_INDEX : DXGK_PRESENT_SOURCE_INDEX);
        if (info->PhysicalAdapterIndex) return status;
        handles[i]=info->hDeviceSpecificAllocation;
    }
    {
        BC250_SNAPSHOT_REFUSAL why;
        BC250_WDDM_ALLOCATION_PRIVATE seen[2];
        UINT side;
        LONG64 refusal;
        if (!WddmSnapshotPresentAllocations(wddm,Context->OwnerDevice,handles,allocations,&why,&side,seen)) {
            // BD-065: the status stays INVALID_HANDLE for every reason, as before. A format mismatch is
            // documented as CANNOTCOLORCONVERT, after which the runtime stops the application; a dropped
            // frame is kept until that change of status has been measured on the lab.
            if ((UINT)why<Bc250SnapshotRefusalCount)
                InterlockedIncrement64(&wddm->GpuPresentSnapshotRefusals[why]);
            // Slot 0 (Admitted) is never a reason: it counts all refusals and bounds the log.
            refusal=InterlockedIncrement64(&wddm->GpuPresentSnapshotRefusals[Bc250SnapshotAdmitted]);
            if (refusal<=BC250_GPU_PRESENT_REFUSAL_LOGS)
                GuardLog("wddm: GPU Present refused%lld reason%u side%u ctx%p owner%p src%p fmt%u %ux%u pitch%u dst%p fmt%u %ux%u pitch%u",
                    refusal,(UINT)why,side,(void*)Context,(void*)Context->OwnerDevice,
                    handles[0],seen[0].Format,seen[0].Width,seen[0].Height,seen[0].Pitch,
                    handles[1],seen[1].Format,seen[1].Width,seen[1].Height,seen[1].Pitch);
            return STATUS_INVALID_HANDLE;
        }
    }
    for (i=0;i<2;i++) {
        const DXGK_PRESENTALLOCATIONINFO* info=Present->pAllocationInfo+
            (i ? DXGK_PRESENT_DESTINATION_INDEX : DXGK_PRESENT_SOURCE_INDEX);
        const BC250_WDDM_ALLOCATION_PRIVATE* a=&allocations[i];
        if (!WddmLinearColorFormat(a->Format)) return STATUS_GRAPHICS_CANNOTCOLORCONVERT;
        surface[i].Width=a->Width;surface[i].Height=a->Height;surface[i].Pitch=a->Pitch;
        surface[i].Bytes=a->Size;surface[i].Format=WddmRedFirst(a->Format)?Bc250BltRgba8:Bc250BltBgra8;
        va[i]=info->AllocationVirtualAddress;
        if (!va[i] || (va[i]&(PAGE_SIZE-1)) || !a->Size || va[i]>MAXULONGLONG-(a->Size-1)) return status;
        // The active command allocation must never be a copy source or target.
        if (ib<=va[i] ? va[i]-ib<Present->DmaSize : ib-va[i]<a->Size) return status;
    }
    src.Left=Present->SrcRect.left;src.Top=Present->SrcRect.top;
    src.Right=Present->SrcRect.right;src.Bottom=Present->SrcRect.bottom;
    dst.Left=Present->DstRect.left;dst.Top=Present->DstRect.top;
    dst.Right=Present->DstRect.right;dst.Bottom=Present->DstRect.bottom;
    if (Present->SubRectCnt) {
        if (Present->SubRectCnt>MAXULONG/sizeof(*dirty)) return status;
        dirty=ExAllocatePool2(POOL_FLAG_NON_PAGED,(SIZE_T)Present->SubRectCnt*sizeof(*dirty),BC250_WDDM_TAG);
        if (!dirty) return STATUS_INSUFFICIENT_RESOURCES;
        for(i=0;i<Present->SubRectCnt;i++) {
            dirty[i].Left=Present->pDstSubRects[i].left;dirty[i].Top=Present->pDstSubRects[i].top;
            dirty[i].Right=Present->pDstSubRects[i].right;dirty[i].Bottom=Present->pDstSubRects[i].bottom;
        }
    }
    result=Bc250EmitGfxPresentBltList(surface,surface+1,&src,&dst,dirty,Present->SubRectCnt,
        va[0],va[1],Present->MultipassOffset,&next,Present->pDmaBuffer,Present->DmaSize/4u,&written);
    if (dirty) ExFreePoolWithTag(dirty,BC250_WDDM_TAG);
    if (result!=Bc250GfxBltDone && result!=Bc250GfxBltMore) return status;
    // Consume the entire aligned DMA buffer, one record/IB. This prevents two
    // Present records being appended while the consumer expects one exact span.
    if (written!=Present->DmaSize/4u) return status;
    KeMemoryBarrier();
    RtlCopyMemory(Present->pDmaBufferPrivateData,record,sizeof(record));
    Present->pDmaBuffer=(UCHAR*)Present->pDmaBuffer+Present->DmaSize;
    Present->pDmaBufferPrivateData=(UCHAR*)Present->pDmaBufferPrivateData+sizeof(record);
    Present->MultipassOffset=next;
    if (InterlockedIncrement64(&wddm->GpuPresentRecords)<=16) {
        GuardLog("wddm: GPU Present built ctx%p ib%llX bytes%u next%u more%u",
            (void*)Context,ib,Present->DmaSize,next,result==Bc250GfxBltMore);
        GuardLog("wddm: GPU Present build ctx%p owner%p root%llX irql%u",
            (void*)Context,(void*)Context->OwnerDevice,Context->RootPhysical,(UINT)KeGetCurrentIrql());
        for (i=0;i<2;i++)
            GuardLog("wddm: GPU Present surface%u handle%p va%llX %ux%u pitch%u fmt%u bytes%llu",
                i,handles[i],va[i],allocations[i].Width,allocations[i].Height,
                allocations[i].Pitch,allocations[i].Format,allocations[i].Size);
    }
    return result==Bc250GfxBltMore ? STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER : STATUS_SUCCESS;
}

// Diagnostic only: no mapping, pixel access or GPU work. Uses the producer's
// GpuMmu list contract and locked snapshot even when the producer gate is 0.
static void WddmObservePresent(BC250_WDDM_OBJECT* Context, const DXGKARG_PRESENT* Present)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Context->Device->Wddm;
    BC250_PRESENT_OBSERVATION* o;
    LONG64 call;
    UINT i;
    if (!wddm || !Present->Flags.Blt ||
        (!wddm->HandleIdentityProbe && !wddm->CddDwmInterop)) return;
    call=InterlockedIncrement64(&wddm->PresentObservationCalls);
    if (call<=0 || call>BC250_PRESENT_OBSERVATIONS) return;
    o=&wddm->PresentObservations[call-1];
    o->InterruptTime=KeQueryInterruptTime();
    o->Qpc=(ULONGLONG)KeQueryPerformanceCounter(NULL).QuadPart;
    o->Context=Context;o->OwnerDevice=Context->OwnerDevice;
    o->Flags=Present->Flags.Value;o->Node=Context->NodeOrdinal;
    o->UmdContext=Context->UmdContext;o->SystemContext=Context->SystemContext;
    o->ListSize=Context->AllocationListSize;
    o->DmaBytes=Present->DmaSize;o->PrivateBytes=Present->DmaBufferPrivateDataSize;
    o->Offset=Present->MultipassOffset;o->SubRects=Present->SubRectCnt;
    o->Src=Present->SrcRect;o->Dst=Present->DstRect;
    if (Present->pAllocationInfo && Context->AllocationListSize>DXGK_PRESENT_MAX_INDEX) {
        for (i=0;i<2;i++) {
            const DXGK_PRESENTALLOCATIONINFO* a=Present->pAllocationInfo+
                (i ? DXGK_PRESENT_DESTINATION_INDEX : DXGK_PRESENT_SOURCE_INDEX);
            o->Handles[i]=a->hDeviceSpecificAllocation;
            o->PhysicalAdapter[i]=a->PhysicalAdapterIndex;
            o->Va[i]=a->AllocationVirtualAddress;
        }
        o->ListValid=TRUE;
        if (!o->PhysicalAdapter[0] && !o->PhysicalAdapter[1])
            o->SnapshotValid=WddmSnapshotPresentAllocations(wddm,Context->OwnerDevice,
                o->Handles,o->Allocations,NULL,NULL,NULL);
    }
    InterlockedExchange(&o->Published,1);
}

// The first Blt present of a DDI device while either interop switch is open marks the session (interop.c), so a
// death from here on closes both switches at the next start. Once per device; DestroyDevice ends it.
static void WddmInteropUse(BC250_WDDM_OBJECT* Context, const DXGKARG_PRESENT* Present)
{
    BC250_WDDM* wddm=(BC250_WDDM*)Context->Device->Wddm;
    BC250_WDDM_OBJECT* owner;
    if (!wddm || !Present->Flags.Blt || (!wddm->GpuPresentGate && !wddm->CddDwmInterop)) return;
    owner=WddmObject(Context->OwnerDevice,BC250_WDDM_MAGIC_DEVICE);
    if (!owner || InterlockedCompareExchange(&owner->InteropUser,1,0)!=0) return;
    if (!InteropUserBegin(Context->Device)) InterlockedExchange(&owner->InteropUser,0);
}

// BD-170: a flip that dxgkrnl sends down the DMA-buffer path. With FlipOnVSyncMmIo and FlipImmediateMmIo both
// declared (WddmDriverCaps), every flip this driver does is an MMIO flip: Present gets
// pDmaBuffer NULL and the flip itself arrives through SetVidPnSourceAddress (d3dkmddi.md:9701). A flip with a
// DMA buffer needs commands that flip from the GPU's own stream, and this driver has none. Until BD-170 such a
// call returned STATUS_SUCCESS with nothing written; dxgkrnl then asserted on the empty buffer (its AzureTriage
// events 9323 and 9372 in the a02 trace) and put the device in error. Now it is refused with the status of the
// DDI's table that says so, STATUS_ILLEGAL_INSTRUCTION (d3dkmddi.md:9640, "instructions that graphics hardware
// cannot support"): no byte written, no pointer moved, no completion pretended. It is counted, and its first
// calls are logged.
static NTSTATUS WddmPresentDmaFlip(_In_opt_ BC250_WDDM* Wddm, _In_ const DXGKARG_PRESENT* Present)
{
    LONG count;
    if (!Present->Flags.Flip || Present->pDmaBuffer == NULL) return STATUS_SUCCESS;
    count = Wddm != NULL ? InterlockedIncrement(&Wddm->DmaFlipsRefused) : 1;
    if (count <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: Present refused a DMA-buffer flip: flags 0x%08X, %u DMA bytes (this driver flips by MMIO only)",
                 Present->Flags.Value, Present->DmaSize);
    return STATUS_ILLEGAL_INSTRUCTION;
}

// ---- HLK gap G1: kernel-mode GDI hardware acceleration -----------------------------------------
//
// docs/design/gdi-hw-accel.md holds the design; this is the one layer that may see a WDK type. The
// arithmetic, the admission and the command stream are gdi_pixel.h, gdi_render.c and gdi_emit.c,
// which carry no WDK header and are tested on the development PC
// (driver/kmd/test/run_gdi_render.ps1, the 'gdi-render' gate of tools/quality/quick.ps1).
//
// DxgkDdiRenderGdi, not DxgkDdiRenderKm: "DxgkDdiRenderGdi is used when submitting Windows Graphics
// Device Interface (GDI) commands for contexts that support virtual addressing"
// (ref/ddi-display/d3dkmddi.md, DXGKDDI_RENDERGDI), and every context this adapter creates declares
// VirtualAddressing. DXGKARG_RENDERGDI has no patch-location list at all, so the absent
// DxgkDdiPatch and ContextInfo.Caps.NoPatchingRequired stay exactly as they are.
//
// Two contract sentences shape everything below, and both are worth reading twice:
//
//   1. DXGKDDI_RENDERGDI's return table has two rows: STATUS_SUCCESS, "The submitted command is
//      well-formed", and "(other) - All other return values will lead to the OS bugcheck".
//   2. The guaranteed contract DMA buffer model (display/using-the-guaranteed-contract-dma-buffer-
//      model.md): "The display miniport driver must be able to translate one command buffer into
//      exactly one DMA buffer ... The display miniport driver cannot return status that indicates
//      it is out of DMA buffer space ... doing so results in the video memory manager bug checking
//      the system."
//
// So this function has no way to report anything. It returns STATUS_SUCCESS on every path, it never
// returns STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER and it does not use MultipassOffset; a command it
// cannot translate is counted, logged and dropped - a visible artefact on the screen, never a
// bugcheck. That is also why the admission in gdi_render.c is exhaustive and host-tested: a mistake
// there cannot be answered with an error code.


// gdi_render.h and gdi_pixel.h are plain integers; these tie every one of them to the WDK's own ABI,
// so a header change that moves a value breaks the build instead of the screen.
C_ASSERT(BC250_GDIOP_BITBLT == DXGK_GDIOP_BITBLT);
C_ASSERT(BC250_GDIOP_COLORFILL == DXGK_GDIOP_COLORFILL);
C_ASSERT(BC250_GDIOP_ALPHABLEND == DXGK_GDIOP_ALPHABLEND);
C_ASSERT(BC250_GDIOP_STRETCHBLT == DXGK_GDIOP_STRETCHBLT);
C_ASSERT(BC250_GDIOP_ESCAPE == DXGK_GDIOP_ESCAPE);
C_ASSERT(BC250_GDIOP_TRANSPARENTBLT == DXGK_GDIOP_TRANSPARENTBLT);
C_ASSERT(BC250_GDIOP_CLEARTYPEBLEND == DXGK_GDIOP_CLEARTYPEBLEND);
C_ASSERT(BC250_GDI_ROP_INVALID == DXGK_GDIROP_INVALID);
C_ASSERT(BC250_GDI_ROP_SRCCOPY == DXGK_GDIROP_SRCCOPY);
C_ASSERT(BC250_GDI_ROP_SRCINVERT == DXGK_GDIROP_SRCINVERT);
C_ASSERT(BC250_GDI_ROP_SRCAND == DXGK_GDIROP_SRCAND);
C_ASSERT(BC250_GDI_ROP_SRCOR == DXGK_GDIROP_SRCOR);
C_ASSERT(BC250_GDI_ROP_ROP3 == DXGK_GDIROP_ROP3);
C_ASSERT(BC250_GDI_CF_INVALID == DXGK_GDIROPCF_INVALID);
C_ASSERT(BC250_GDI_CF_PATCOPY == DXGK_GDIROPCF_PATCOPY);
C_ASSERT(BC250_GDI_CF_PATINVERT == DXGK_GDIROPCF_PATINVERT);
C_ASSERT(BC250_GDI_CF_PDXN == DXGK_GDIROPCF_PDXN);
C_ASSERT(BC250_GDI_CF_DSTINVERT == DXGK_GDIROPCF_DSTINVERT);
C_ASSERT(BC250_GDI_CF_PATAND == DXGK_GDIROPCF_PATAND);
C_ASSERT(BC250_GDI_CF_PATOR == DXGK_GDIROPCF_PATOR);
C_ASSERT(BC250_GDI_CF_ROP3 == DXGK_GDIROPCF_ROP3);
C_ASSERT(BC250_GDI_INVALID_GAMMA == D3DKM_INVALID_GAMMA_INDEX);
C_ASSERT(BC250_GDISURFACE_TEXTURE == D3DKMDT_GDISURFACE_TEXTURE);
C_ASSERT(BC250_GDISURFACE_STAGING_CPUVISIBLE == D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE);
C_ASSERT(BC250_GDISURFACE_STAGING == D3DKMDT_GDISURFACE_STAGING);
C_ASSERT(BC250_GDISURFACE_LOOKUPTABLE == D3DKMDT_GDISURFACE_LOOKUPTABLE);
// MaxTextureWidthShift 2 with DXGK_TEXTURE_SIZE_SHIFT 11 is 8192, which is the extent gdi_render.c
// refuses above (its GDI_MAX_EXTENT). The caps answer and the admission come from the same number.
C_ASSERT((1u << (2u + DXGK_TEXTURE_SIZE_SHIFT)) == 8192u);
// The rectangles are copied field by field below; this only says that nothing was forgotten.
C_ASSERT(sizeof(BC250_GDI_RECT) == sizeof(RECT));

// One allocation-list entry, as the contract layer needs it. The same discipline as
// WddmSnapshotPresentAllocations: the handle is a value until the object index returns it, both
// descriptors are compared and copied under the lock, and no object pointer escapes. Serial is the
// backing allocation's adapter-unique identity, which is what tells two list entries naming one
// allocation apart from two entries naming two.
static int WddmGdiSurfaceAt(BC250_WDDM* Wddm, const BC250_WDDM_OBJECT* Context,
    const DXGK_ALLOCATIONLIST* List, UINT ListSize, UINT Index,
    BC250_GDI_SURFACE* Surface, ULONGLONG* Va, ULONGLONG* Serial)
{
    BC250_WDDM_ALLOCATION_PRIVATE allocation;
    ULONG type = 0, bpp;
    KIRQL irql;
    int found = 0;

    RtlZeroMemory(Surface, sizeof(*Surface));
    RtlZeroMemory(&allocation, sizeof(allocation));
    *Va = 0;
    *Serial = 0;
    if (Index >= ListSize) return 0;
    KeAcquireSpinLock(&Wddm->Lock, &irql);
    if (!Wddm->Stopping)
    {
        BC250_WDDM_OBJECT* opened = (BC250_WDDM_OBJECT*)Bc250ObjectIndexFind(&Wddm->ObjectIndex,
            List[Index].hDeviceSpecificAllocation);
        if (opened != NULL && opened->Magic == BC250_WDDM_MAGIC_OPENED && !opened->UmdAlloc &&
            opened->OwnerDevice == Context->OwnerDevice)
        {
            BC250_WDDM_OBJECT* backing = WddmBackingAllocationLocked(Wddm, opened);
            if (backing != NULL && !backing->UmdAlloc &&
                RtlCompareMemory(&backing->Allocation, &opened->Allocation,
                    sizeof(opened->Allocation)) == sizeof(opened->Allocation))
            {
                allocation = opened->Allocation;
                type = opened->GdiType;
                *Serial = backing->Serial;
                found = 1;
            }
        }
    }
    KeReleaseSpinLock(&Wddm->Lock, irql);
    if (!found) return 0;
    bpp = WddmSurfaceFormatBpp(allocation.Format, BC250_SURFACE_GDI);
    if (!bpp || !allocation.Width || !allocation.Height || !allocation.Size) return 0;
    *Va = List[Index].VirtualAddress;
    // The address is the one number here that a copy is written with: four-byte aligned, as the
    // pitch contract (AlignmentShift 2) requires of every row, and its span must not wrap.
    if (!*Va || (*Va & 3u) || *Va > MAXULONGLONG - (allocation.Size - 1u)) { *Va = 0; return 0; }
    Surface->Width = allocation.Width;
    Surface->Height = allocation.Height;
    Surface->Pitch = allocation.Pitch;
    Surface->Bpp = bpp;
    Surface->Bytes = allocation.Size;
    Surface->RedFirst = WddmRedFirst(allocation.Format) ? 1u : 0u;
    // GdiType 0 is a legacy LB7A surface - a CDD or compositor surface that did not come through
    // GetStandardAllocationDriverData - and D3DKMDT_GDISURFACETYPE has no such value. TEXTURE is the
    // conservative reading: the command's own pitch member is ignored for that type, which is what
    // we want for a surface whose pitch this driver placed itself.
    Surface->Type = type ? type : BC250_GDISURFACE_TEXTURE;
    Surface->Present = 1;
    return 1;
}


// Resolve the allocation-list indices one command names into a small local table and rewrite the
// command's indices to point at it, so that the contract layer never needs a 256-entry array. An
// index that appears twice keeps one slot, which is what lets gdi_render.c's own same-allocation
// refusal fire; two different indices that resolve to one allocation are caught here, by serial.
static int WddmGdiBind(BC250_WDDM* Wddm, const BC250_WDDM_OBJECT* Context,
    const DXGK_ALLOCATIONLIST* List, UINT ListSize, BC250_GDI_OP* Op,
    BC250_GDI_SURFACE* Surfaces, ULONGLONG* Vas, UINT* Count)
{
    UINT* fields[BC250_GDI_BIND_SLOTS];
    UINT originals[BC250_GDI_BIND_SLOTS] = {0};
    ULONGLONG serials[BC250_GDI_BIND_SLOTS] = {0};
    UINT used = 0, i, j;

    *Count = 0;
    // TmpSurfAllocationIndex is deliberately absent: NoTempSurfaceForClearTypeBlend is set, so that
    // slot is never read and a nonsense value in it must change nothing.
    fields[0] = &Op->DstAllocationIndex;
    fields[1] = Bc250GdiOpHasSource(Op->OpCode) ? &Op->SrcAllocationIndex : NULL;
    fields[2] = Op->OpCode == BC250_GDIOP_CLEARTYPEBLEND ? &Op->AlphaSurfAllocationIndex : NULL;
    fields[3] = (Op->OpCode == BC250_GDIOP_CLEARTYPEBLEND && Op->Gamma != BC250_GDI_INVALID_GAMMA) ?
        &Op->GammaSurfAllocationIndex : NULL;
    for (i = 0; i < BC250_GDI_BIND_SLOTS; i++)
    {
        UINT original, slot;
        if (fields[i] == NULL) continue;
        original = *fields[i];
        slot = used;
        for (j = 0; j < used; j++)
            if (originals[j] == original) { slot = j; break; }
        if (slot == used)
        {
            if (!WddmGdiSurfaceAt(Wddm, Context, List, ListSize, original, &Surfaces[used],
                                  &Vas[used], &serials[used]))
                return 0;
            originals[used] = original;
            used++;
        }
        *fields[i] = slot;
    }
    for (i = 0; i < used; i++)
        for (j = i + 1; j < used; j++)
            if (serials[i] == serials[j]) return 0;
    *Count = used;
    return 1;
}

// The DMA buffer this command is built into must be neither a source nor a destination of its own
// copies, the rule the Present producer states the same way.
static int WddmGdiDisjoint(ULONGLONG Ib, UINT IbBytes, ULONGLONG Va, ULONGLONG Bytes)
{
    return Ib <= Va ? Va - Ib >= IbBytes : Ib - Va >= Bytes;
}

// The sub-rectangles the whole command buffer holds, which is what gdi_batch.c budgets the call
// with. Only a command the translation will draw adds its count, which is what keeps this number
// inside the input's own bound: an admitted command of L bytes carries at most
// (L - 8) / sizeof(RECT) sub-rectangles, because the array is inside the command. The count
// saturates rather than wraps, and a saturated count refuses the call.
static unsigned int WddmGdiSubRectCount(const UCHAR* Buffer, UINT Bytes, UINT* Commands)
{
    ULONGLONG total = 0;
    UINT offset, index, next, subCount, why;
    const RECT* subRects;
    BC250_GDI_OP op;

    for (offset = 0, index = 0; offset + (UINT)FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command) <= Bytes;
         index++)
    {
        int kind = Bc250GdiWalkCommand(Buffer, Bytes, offset, &op, &subRects, &subCount, &next, &why);

        if (kind == BC250_GDI_CMD_STOP) break;
        offset = next;
        if (kind != BC250_GDI_CMD_DRAW) continue;
        total += subCount;
        if (total > 0xFFFFFFFFull) { total = 0xFFFFFFFFull; break; }
    }
    if (Commands != NULL) *Commands = index;
    return (unsigned int)total;
}

// The one exit of a call that was admitted and drew nothing. Review r4 asked for exactly this: the
// capacity of a call can run out for several reasons, and a reader of the counters must not have to
// know which of them reaches which counter. So every drop of a whole call lands here, whatever
// stopped it: BatchRefused counts the calls, OmittedSubRects the sub-rectangles lost with them, and
// ScopeRefused the drops of a command buffer inside the input scope the design read. The caller
// counts which of the drops it was. Nonzero ScopeRefused is a correctness result and not an
// observation about the operating system's capacity (docs/testing/gdi-hw-accel-lab.md, trial 3).
static void WddmGdiDropCall(_Inout_ BC250_WDDM* Wddm, LONG64 Call, _In_z_ const char* Why,
                            UINT Bytes, unsigned int Planned, unsigned int Needed,
                            unsigned int Capacity, unsigned int Ahead)
{
    InterlockedIncrement64(&Wddm->GdiRenderBatchRefused);
    InterlockedAdd64(&Wddm->GdiRenderOmittedSubRects, (LONG64)Planned);
    GuardLog("wddm: RenderGdi%lld dropped %s: subrects%u needs%u of%u ahead%u len%u", Call, Why,
             Planned, Needed, Capacity, Ahead, Bytes);
    if (Bytes <= BC250_GDI_PRODUCER_BYTES)
    {
        InterlockedIncrement64(&Wddm->GdiRenderScopeRefused);
        GuardLog("wddm: RenderGdi%lld DROPPED INSIDE SCOPE %s: len%u subrects%u", Call, Why, Bytes,
                 Planned);
    }
}

static DXGKDDI_RENDERGDI Bc250WddmRenderGdi;
static NTSTATUS Bc250WddmRenderGdi(_In_ const HANDLE hContext, _Inout_ DXGKARG_RENDERGDI* pRenderGdi)
{
    BC250_WDDM_OBJECT* context = WddmObject(hContext, BC250_WDDM_MAGIC_CONTEXT);
    BC250_WDDM* wddm = context != NULL ? (BC250_WDDM*)context->Device->Wddm : NULL;
    const UCHAR* buffer;
    unsigned int* out;
    BC250_GDI_BATCH batch;
    UINT bytes, offset, index, listSize;
    unsigned int capacity, used, planned, needed, ahead = 0, visited = 0;
    ULONGLONG ib;
    LONG64 call;
    int overflow = 0, place;

    // Nothing here may fail the call (the return table above), so an argument that would make the
    // translation unsafe ends it with an empty DMA buffer instead: dxgkrnl then submits no work of
    // ours and the operations are dropped. GdiRenderRefusedCall is this exit alone, and it stays
    // outside the drop path below on purpose: that path counts the sub-rectangles a call held, and
    // a call whose command buffer or DMA buffer fails these tests cannot be walked to find out what
    // it held. The counter is the account of those, and the lab plan reads it as lost drawing too.
    if (context == NULL || pRenderGdi == NULL) return STATUS_SUCCESS;
    if (wddm == NULL || !wddm->GdiHwAccel) return STATUS_SUCCESS;
    call = InterlockedIncrement64(&wddm->GdiRenderCalls);
    buffer = (const UCHAR*)pRenderGdi->pCommand;
    bytes = pRenderGdi->CommandLength;
    out = (unsigned int*)pRenderGdi->pDmaBuffer;
    ib = pRenderGdi->DmaBufferGpuVirtualAddress;
    listSize = pRenderGdi->AllocationListSize;
    capacity = pRenderGdi->DmaSize / 4u;
    // "Access to the kernel buffers does not require protection from try/except code" (the
    // DxgkDdiRenderKm Remarks), so the command buffer is read directly - but every offset into it is
    // checked against CommandLength below, because a malformed buffer would otherwise be read past
    // its end.
    if (!context->GdiContext || context->NodeOrdinal != BC250_WDDM_NODE_3D ||
        KeGetCurrentIrql() != PASSIVE_LEVEL ||
        buffer == NULL || bytes < (UINT)FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command) || (bytes & 3u) ||
        ((ULONG_PTR)buffer & 3u) || out == NULL || ((ULONG_PTR)out & 3u) || ib == 0 || (ib & 31u) ||
        capacity < BC250_GFX_ACQUIRE_DWORDS + BC250_GDI_MIN_DWORDS ||
        pRenderGdi->pAllocationList == NULL || listSize == 0)
    {
        InterlockedIncrement64(&wddm->GdiRenderRefusedCall);
        return STATUS_SUCCESS;
    }

    // What this call brought, counted before anything about its capacity is decided: review r4
    // found the length and the sub-rectangle account below a capacity exit, so a dropped call could
    // leave no trace of its input at all. Nothing between here and the end of the function returns
    // without the drop of a whole call being counted.
    if ((LONG64)bytes > wddm->GdiRenderPeakCommandLength)
        wddm->GdiRenderPeakCommandLength = (LONG64)bytes;
    // Longer than the command buffer of the ordinary producer, which is the input scope the design
    // read. Counted, because it is a producer the design did not read.
    if (bytes > BC250_GDI_PRODUCER_BYTES) InterlockedIncrement64(&wddm->GdiRenderAboveProducer);
    planned = WddmGdiSubRectCount(buffer, bytes, NULL);
    if (planned > (unsigned int)wddm->GdiRenderPeakSubRects) wddm->GdiRenderPeakSubRects = planned;
    // What this call's own command buffer could have asked for at worst. It is a measurement and a
    // signal, not a decision: Begin's exact check below is what protects the buffer, and it accepts
    // a call whose actual sub-rectangles still fit a smaller capacity.
    needed = Bc250GdiCallDwords(bytes);

    // Rule 3 of gdi_batch.h. The whole DMA buffer is submitted as one indirect buffer, whose length
    // field is what any translation may fill, and dxgkrnl may put several calls in one buffer
    // before it submits it: so this call may use only the part of that length the buffer has left.
    // Which part that is comes from the buffers this context has written into, by the address the
    // DDI gives for this one, and never from the order in which addresses arrived.
    {
        KIRQL irql;

        KeAcquireSpinLock(&context->GdiDmaLock, &irql);
        place = Bc250GdiDmaClaim(&context->GdiDma, ib, PAGE_SIZE, &ahead);
        KeReleaseSpinLock(&context->GdiDmaLock, irql);
    }
    if (place == BC250_GDI_DMA_APPEND) InterlockedIncrement64(&wddm->GdiRenderDmaAppend);
    else if (place == BC250_GDI_DMA_RESTART) InterlockedIncrement64(&wddm->GdiRenderDmaRestart);
    if (place == BC250_GDI_DMA_UNKNOWN || place == BC250_GDI_DMA_FULL)
    {
        // Either this window is a position inside a DMA buffer that no reading of the DDI produces,
        // or every slot of this context holds a buffer with output of ours that nothing has
        // submitted. In both cases the dwords ahead of this call are not established, so no
        // capacity may be computed for it and it draws nothing.
        if (place == BC250_GDI_DMA_FULL) InterlockedIncrement64(&wddm->GdiRenderDmaFull);
        else InterlockedIncrement64(&wddm->GdiRenderDmaUnknown);
        WddmGdiDropCall(wddm, call, place == BC250_GDI_DMA_FULL ? "dma-full" : "dma-unknown", bytes,
                        planned, needed, capacity, 0u);
        return STATUS_SUCCESS;
    }
    // "The DMA buffer can grow and shrink after the context is created; however, the DMA buffer can
    // never shrink smaller than the starting size that DmaBufferSize specifies" (DXGK_CONTEXTINFO).
    // A call at the start of a buffer therefore reports at least the size this context asked for,
    // and this counter is the check of that sentence, not the handling of an allocation failure. A
    // call that continues inside a buffer reports the window it has left, which is smaller by
    // design, so it is never counted here.
    if (context->GdiDmaBytes != 0 && ahead == 0 &&
        (ULONGLONG)pRenderGdi->DmaSize < (ULONGLONG)context->GdiDmaBytes)
        InterlockedIncrement64(&wddm->GdiRenderDmaShort);
    capacity = Bc250GdiCallCapacity(capacity, ahead);
    if (capacity < BC250_GFX_ACQUIRE_DWORDS + BC250_GDI_MIN_DWORDS)
    {
        // The indirect buffer the submitter will encode has no room left for work of ours, because
        // earlier calls into the same buffer have filled it. The call draws nothing, which is the
        // same drop as every other one and is counted with them.
        InterlockedIncrement64(&wddm->GdiRenderIbFull);
        WddmGdiDropCall(wddm, call, "ib-full", bytes, planned, needed, capacity, ahead);
        return STATUS_SUCCESS;
    }
    if (needed == 0u || needed > capacity)
    {
        InterlockedIncrement64(&wddm->GdiRenderCapacityShort);
        if ((LONG)bytes > wddm->GdiCmdBytesWanted)
            InterlockedExchange(&wddm->GdiCmdBytesWanted, (LONG)bytes);
        GuardLog("wddm: RenderGdi%lld len%u over capacity: needs%u of%u ahead%u", call, bytes,
                 needed, capacity, ahead);
    }
    // The capacity of the whole call, settled before the first packet. The guaranteed contract gives
    // one DMA buffer for one command buffer and no second pass, and RenderGdi has no status for a
    // short buffer, so a call whose sub-rectangles cannot all be guaranteed is translated to an
    // empty DMA buffer: it draws nothing, rather than drawing a prefix and dropping the rest from a
    // call that reported success. gdi_batch.h carries the rule and the proof.
    if (!Bc250GdiBatchBegin(&batch, out, capacity, ib, planned))
    {
        // The defined drop of a whole call. It is unreachable while the command buffer stays inside
        // the bound this context was created with and the buffer is its own, which is the point of
        // sizing the DMA buffer from that bound; it is reachable above the bound, because no
        // producer is obliged to stay there and the DDI has no status to ask for more space, and
        // inside the bound when an earlier call has taken part of the same buffer. A call that
        // cannot be translated whole draws nothing: a prefix of it would leave a half-drawn screen,
        // and any status but success bugchecks Windows. What is lost is counted, not guessed at.
        WddmGdiDropCall(wddm, call, "batch", bytes, planned, Bc250GdiBatchDwords(planned), capacity,
                        ahead);
        return STATUS_SUCCESS;
    }

    for (offset = 0, index = 0; offset + (UINT)FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command) <= bytes; index++)
    {
        BC250_GDI_SURFACE surfaces[BC250_GDI_BIND_SLOTS];
        ULONGLONG vas[BC250_GDI_BIND_SLOTS];
        BC250_GDI_OP op;
        const RECT* subRects;
        UINT subCount, next, slots, sub, why, start = offset;
        unsigned int pattern = 0;
        int disjoint = 1;
        // The structure of this command, read exactly as the counting walk above read it.
        int kind = Bc250GdiWalkCommand(buffer, bytes, start, &op, &subRects, &subCount, &next, &why);

        if (kind == BC250_GDI_CMD_STOP)
        {
            // An opcode outside the table has no size this driver can trust, and neither has a
            // command whose size the buffer does not hold, so the rest of the buffer is not walked.
            if (why == BC250_GDI_WHY_OPCODE)
                InterlockedIncrement64(&wddm->GdiRenderRefused[Bc250GdiBadOpCode]);
            else
                InterlockedIncrement64(&wddm->GdiRenderMalformed);
            break;
        }
        offset = next;
        InterlockedIncrement64(&wddm->GdiRenderCommands);
        if (kind == BC250_GDI_CMD_SKIP)
        {
            // An escape is not ours to run. A command with no sub-rectangle paints nothing, because
            // every page says the sub-rectangles are what is painted, bounded by DstRect. An array
            // that is not inside its own command is the one input here that cannot be bounds-checked
            // any other way, and `CommandSize` is what says it must be inside.
            if (why == BC250_GDI_WHY_ESCAPE) InterlockedIncrement64(&wddm->GdiRenderRefused[Bc250GdiSkip]);
            else if (why == BC250_GDI_WHY_EMPTY) InterlockedIncrement64(&wddm->GdiRenderEmpty);
            else InterlockedIncrement64(&wddm->GdiRenderOutside);
            continue;
        }
        if (!WddmGdiBind(wddm, context, pRenderGdi->pAllocationList, listSize, &op,
                         surfaces, vas, &slots))
        {
            InterlockedIncrement64(&wddm->GdiRenderRefused[Bc250GdiBadIndex]);
            continue;
        }
        for (sub = 0; sub < slots; sub++)
            if (!WddmGdiDisjoint(ib, pRenderGdi->DmaSize, vas[sub], surfaces[sub].Bytes)) disjoint = 0;
        if (!disjoint)
        {
            InterlockedIncrement64(&wddm->GdiRenderRefused[Bc250GdiBadSurface]);
            continue;
        }
        if (op.OpCode == BC250_GDIOP_COLORFILL)
        {
            // Color is in the destination surface's own format (DXGK_GDIARG_COLORFILL in the WDK's
            // d3dkmddi.h), so the fill pattern is the dword itself and nothing is converted here.
            // The DDI reference page documents the field in A8R8G8B8 instead; the header wins, and a
            // red-first destination - the one case where the two readings differ - is counted so the
            // lab can say whether it ever happens.
            pattern = op.Color;
            if (surfaces[op.DstAllocationIndex].RedFirst)
                InterlockedIncrement64(&wddm->GdiRenderColorFillRedFirst);
        }
        for (sub = 0; sub < subCount; sub++)
        {
            BC250_GDI_ITEM item;
            BC250_GDI_RECT rect;
            int result, status;

            visited++;
            Bc250GdiWalkRect(&subRects[sub], &rect);
            status = Bc250GdiPlanItem(&op, surfaces, slots, &rect, &item);
            if (status != Bc250GdiOk)
            {
                if ((UINT)status < (UINT)Bc250GdiRefusals)
                    InterlockedIncrement64(&wddm->GdiRenderRefused[status]);
                continue;
            }
            if (item.DstWidth == 0 || item.DstHeight == 0) continue;
            if (item.Work == (unsigned int)Bc250GdiWorkKernel)
                InterlockedIncrement64(&wddm->GdiRenderKernels[item.Kernel <
                    (unsigned int)Bc250GdiKernels ? item.Kernel : 0u]);
            // The batch owns the cache operation, the code block, the choice between the per-row
            // engine path and a dispatch, and the capacity of both. Remaining is how many
            // sub-rectangles may still be offered, this one included, which is what keeps a tall
            // rectangle from spending the dwords the ones behind it are owed.
            result = Bc250GdiBatchItem(&batch, &op, surfaces, vas, slots, &item, pattern,
                                       planned > visited ? planned - visited + 1u : 1u);
            if (result == BC250_GDI_BATCH_REFUSED)
            {
                // An admitted item the emitters could not describe: a slot that resolved to no
                // address, or a geometry the row emitter calls invalid. Counted and drawn by
                // nothing, which is what the admission should have decided earlier.
                InterlockedIncrement64(&wddm->GdiRenderItemRefused);
                InterlockedIncrement64(&wddm->GdiRenderRefused[Bc250GdiBadSurface]);
                continue;
            }
            if (result == BC250_GDI_BATCH_NO_SPACE)
            {
                // Unreachable after Bc250GdiBatchBegin said yes (the proof is in gdi_batch.c). It is
                // still handled, because a command stream that stops is better than one that runs
                // past the end of its buffer, and this counter is how the lab would tell us.
                overflow = 1;
                break;
            }
            InterlockedIncrement64(&wddm->GdiRenderItems);
        }
        if (overflow) break;
    }
    used = Bc250GdiBatchEnd(&batch);
    // What the batch generated, which is not yet what left: the two checks below can still drop all
    // of it, and GdiRenderDwordsCommitted is the account of what was handed to the submitter.
    InterlockedAdd64(&wddm->GdiRenderDwords, (LONG64)batch.Dwords);
    InterlockedAdd64(&wddm->GdiRenderFallback, (LONG64)batch.Fallbacks);
    if ((LONG64)used > wddm->GdiRenderPeakDwords) wddm->GdiRenderPeakDwords = (LONG64)used;
    // An item that found no space after Begin said yes is unreachable (the induction in
    // gdi_batch.c), and the stream it leaves is a prefix of the call. A prefix is what this design
    // never submits, so the whole call is dropped and counted like every other drop.
    if (overflow)
    {
        InterlockedIncrement64(&wddm->GdiRenderOverflow);
        WddmGdiDropCall(wddm, call, "no-space", bytes, planned, used, capacity, ahead);
        used = 0;
    }
    // The end of rule 3, as a check and not as an assumption: the capacity above is the encodable
    // length less what the buffer already holds, so this cannot happen. If it ever does, the stream
    // stays where it is and pDmaBuffer does not move, so the submission carries what was already
    // there and nothing of ours: the alternative is a submission the ring emitter must refuse
    // whole, which would leave nonempty work outstanding for recovery. The dwords above were
    // generated and are not submitted, so this drop is counted with the others.
    if (used != 0 && (ULONGLONG)used + (ULONGLONG)ahead > BC250_GDI_IB_DWORDS_MAX)
    {
        InterlockedIncrement64(&wddm->GdiRenderIbOverflow);
        GuardLog("wddm: RenderGdi%lld IB OVERFLOW: dwords%u ahead%u of%u", call, used, ahead,
                 BC250_GDI_IB_DWORDS_MAX);
        WddmGdiDropCall(wddm, call, "ib-over", bytes, planned, used, capacity, ahead);
        used = 0;
    }

    if (used != 0)
    {
        KIRQL irql;
        int kept;

        // This buffer now holds these dwords of ours, which is what the next call into it has to
        // subtract. Recorded before pDmaBuffer moves, so no state can say less than the buffer
        // holds.
        KeAcquireSpinLock(&context->GdiDmaLock, &irql);
        kept = Bc250GdiDmaWrote(&context->GdiDma, ib, ahead, used);
        KeReleaseSpinLock(&context->GdiDmaLock, irql);
        if (!kept)
        {
            // Unreachable: the claim above took the slot this writes to. A buffer whose record is
            // lost is not unsafe, because the next call into it cannot be placed and is dropped.
            InterlockedIncrement64(&wddm->GdiRenderDmaLost);
            GuardLog("wddm: RenderGdi%lld dma record lost: ahead%u dwords%u", call, ahead, used);
        }
        InterlockedAdd64(&wddm->GdiRenderDwordsCommitted, (LONG64)used);
        KeMemoryBarrier();
        pRenderGdi->pDmaBuffer = (UCHAR*)pRenderGdi->pDmaBuffer + (SIZE_T)used * 4u;
    }
    if (call <= BC250_WDDM_LOG_CALLS || overflow)
        GuardLog("wddm: RenderGdi%lld ctx%p commands%u bytes%u dwords%u rects%u fall%u over%u",
                 call, (void*)context, index, bytes, used, planned, batch.Fallbacks,
                 (UINT)overflow);
    return STATUS_SUCCESS;
}

static DXGKDDI_PRESENT Bc250WddmPresent;
static NTSTATUS Bc250WddmPresent(_In_ const HANDLE hContext, _Inout_ DXGKARG_PRESENT* pPresent)
{
    BC250_WDDM_OBJECT* context = WddmObject(hContext, BC250_WDDM_MAGIC_CONTEXT);

    // A recycled OS private buffer may still contain a previous BGP1. Clear at
    // producer entry, including early-return/non-Blt paths. Do not clear during
    // submission: a legal scheduler resubmission must retain the same record.
    Bc250GfxPresentInvalidate(pPresent->pDmaBufferPrivateData,pPresent->DmaBufferPrivateDataSize);
    if (context) WddmObservePresent(context,pPresent);
    if (context) WddmInteropUse(context,pPresent);
    // M15.14 increment 2: DXGK_PRESENTFLAGS.RedirectedFlip is the OS saying this present belongs to a
    // chain it is flipping through the compositor rather than composing. It is the one witness here that
    // the flip model is in use at all, counted and never obeyed, and read as a bitfield so no transcribed
    // mask can be wrong about it.
    if (context != NULL && pPresent->Flags.RedirectedFlip)
        InterlockedIncrement(&((BC250_WDDM*)context->Device->Wddm)->RedirectedPresents);

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
    {
        const NTSTATUS flip = WddmPresentDmaFlip(context ? (BC250_WDDM*)context->Device->Wddm : NULL, pPresent);
        if (!NT_SUCCESS(flip)) return flip;
    }
    if (context != NULL && pPresent->Flags.Blt &&
        ((BC250_WDDM*)context->Device->Wddm)->GpuPresentGate) {
        BC250_WDDM* wddm=(BC250_WDDM*)context->Device->Wddm;
        UINT before=pPresent->MultipassOffset;
        LONG64 call=InterlockedIncrement64(&wddm->GpuPresentCalls);
        NTSTATUS status=WddmBuildGpuPresent(context,pPresent);
        if (status==STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER)
            InterlockedIncrement64(&wddm->GpuPresentRotates);
        else if (!NT_SUCCESS(status)) {
            UINT kind=status==STATUS_INVALID_PARAMETER ? 0 :
                status==STATUS_INVALID_HANDLE ? 1 :
                status==STATUS_GRAPHICS_CANNOTCOLORCONVERT ? 2 : 3;
            InterlockedIncrement64(&wddm->GpuPresentRefused);
            InterlockedIncrement64(&wddm->GpuPresentStatuses[kind]);
        }
        if (call<=16)
            GuardLog("wddm: GPU Present call%lld status%x dma%u private%u offset%u->%u",
                call,(UINT)status,pPresent->DmaSize,pPresent->DmaBufferPrivateDataSize,
                before,pPresent->MultipassOffset);
        return status;
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

// ---- BD-170 r6: the flip state machine (docs/design/vidpn-flip.md section 7.2, flip_state.h) ----------------------
//
// L is the interrupt lock of this driver's one MSI message (message 0, facts M38). Three paths hold it and no other:
// the kernel's dispatch of the ISR (pnp.c), dxgmms2's call of SetVidPnSourceAddress above DISPATCH_LEVEL (it makes the
// call through DpSynchronizeExecution, facts M904), and this driver's DxgkCbSynchronizeExecution sections, which run
// only from callers at or below DISPATCH_LEVEL (dispmprt.h, _IRQL_requires_max_(DISPATCH_LEVEL)). Code inside L never
// calls DxgkCbSynchronizeExecution (WddmSection refuses above DISPATCH_LEVEL), never logs and never waits for the other
// side. Every field of the state machine's block in BC250_WDDM is written inside L only.

// One DxgkCbSynchronizeExecution section on message 0, from a caller at or below DISPATCH_LEVEL. Above it, or with
// the callback missing, nothing runs and the caller decides (counted by the caller).
static NTSTATUS WddmSection(_In_ BC250_DEVICE* Device, _In_ PKSYNCHRONIZE_ROUTINE Routine, _In_ PVOID Context)
{
    BOOLEAN returned = FALSE;
    NTSTATUS status;
    if (KeGetCurrentIrql() > DISPATCH_LEVEL) return STATUS_INVALID_DEVICE_STATE;
    if (Device->Dxgk.DxgkCbSynchronizeExecution == NULL) return STATUS_DEVICE_NOT_READY;
    status = Device->Dxgk.DxgkCbSynchronizeExecution(Device->Dxgk.DeviceHandle, Routine, Context, 0, &returned);
    return NT_SUCCESS(status) && !returned ? STATUS_UNSUCCESSFUL : status;
}

// E5 (section 7.2, E5 steps 1 to 3), inside L: FAULTING, the blank request, and the notification handed to the DPC.
// DxgkCbNotifyInterrupt is not made here: this may run inside dxgmms2's own DIRQL flip call, and the callback is not
// documented as reentrant (d3dkmddi.md:2467-2481). The DPC that DxgkCbQueueDpc asks for makes it through WddmReport's
// own section (WddmFlipFaultDpc). A fault in FAULTING is counted only.
static void WddmFlipFault(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, ULONG Code, UINT Node)
{
    InterlockedIncrement(&Wddm->FlipFaults);
    if (InterlockedCompareExchange(&Wddm->Faulting, 1, 0) != 0) return;
    Wddm->FaultCode = Code;
    Wddm->FaultNode = Node;
    Wddm->FaultBlank = DcnFaultBlank(Device);
    InterlockedExchange(&Wddm->FaultNotify, 1);
    if (Device->Dxgk.DxgkCbQueueDpc != NULL) Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
    else InterlockedIncrement(&Wddm->FaultNotifyFailed);
}

// A refused interrupt section cannot grant its operation. The only change
// outside L is this atomic recovery request. The DPC or next flip consumes it
// inside a successfully acquired section; until then the writer stays owned.
static void WddmWriterSectionFailure(BC250_DEVICE* Device, BC250_WDDM* Wddm, NTSTATUS Status)
{
    InterlockedIncrement(Status == STATUS_INVALID_DEVICE_STATE ? &Wddm->WriterHigh : &Wddm->WriterSyncFailed);
    InterlockedExchange(&Wddm->WriterRecovery, 1);
    if (Device->Dxgk.DxgkCbQueueDpc != NULL) Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
}

static BOOLEAN WddmWriterRecoveryLocked(_In_ PVOID Context)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_WDDM* wddm = WddmOf(device);
    if (wddm != NULL && InterlockedExchange(&wddm->WriterRecovery, 0) != 0)
        WddmFlipFault(device, wddm, BC250_FLIP_FAULT_SECTION, BC250_WDDM_NODE_3D);
    return TRUE;
}

// E5 steps 3 and 5, from pnp.c's Bc250DpcRoutine at DISPATCH_LEVEL, outside L. The notification: DMA_PAGE_FAULTED, "a
// GPU error that needs OS recovery" (d3dkmddi.md:46003-46007), with the payload of section 7.2 (WDK shared/d3dkmddi.h
// DmaPageFaulted, shared/d3dkmdt.h DXGK_PAGE_FAULT_FLAGS, DXGK_FAULT_ERROR_CODE and the UNKNOWN values). WddmReport makes
// the call inside its own section and queues the DPC that pairs it. Then the drain record: at most
// BC250_FLIP_DRAIN_READS reads of HUBP0_DCHUBP_CNTL, 1 us apart. Nothing is released either way: the OS still holds
// every surface it did not see complete. One notification per FAULTING.
void WddmFlipFaultDpc(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA data;
    ULONG reads, step;
    BOOLEAN notified;

    if (wddm == NULL) return;
    if (InterlockedCompareExchange(&wddm->WriterRecovery, 0, 0) != 0)
        (void)WddmSection(Device, WddmWriterRecoveryLocked, Device);
    if (InterlockedExchange(&wddm->FaultNotify, 0) == 0) return;
    RtlZeroMemory(&data, sizeof(data));
    data.InterruptType = DXGK_INTERRUPT_DMA_PAGE_FAULTED;
    data.DmaPageFaulted.FaultedFenceId = 0;
    data.DmaPageFaulted.FaultedPrimitiveAPISequenceNumber = DXGK_PRIMITIVE_API_SEQUENCE_NUMBER_UNKNOWN;
    data.DmaPageFaulted.FaultedPipelineStage = DXGK_RENDER_PIPELINE_STAGE_UNKNOWN;
    data.DmaPageFaulted.FaultedBindTableEntry = DXGK_BIND_TABLE_ENTRY_UNKNOWN;
    data.DmaPageFaulted.PageFaultFlags =
        (DXGK_PAGE_FAULT_FLAGS)(DXGK_PAGE_FAULT_FENCE_INVALID | DXGK_PAGE_FAULT_ADAPTER_RESET_REQUIRED);
    data.DmaPageFaulted.FaultedVirtualAddress = 0;     // D3DGPU_NULL: the fault has another cause, the code says which
    data.DmaPageFaulted.NodeOrdinal = wddm->FaultNode;
    data.DmaPageFaulted.EngineOrdinal = 0;              // each node of this driver has one engine
    data.DmaPageFaulted.PageTableLevel = 0;
    data.DmaPageFaulted.FaultErrorCode.IsDeviceSpecificCode = 1;
    data.DmaPageFaulted.FaultErrorCode.DeviceSpecificCode = wddm->FaultCode;
    data.DmaPageFaulted.FaultedProcessHandle = NULL;
    notified = WddmReport(Device, &data, FALSE);
    InterlockedIncrement(notified ? &wddm->FaultNotified : &wddm->FaultNotifyFailed);
    for (reads = 1;; reads++)
    {
        const int drained = DcnHubpDrained(Device);
        step = Bc250FlipDrainStep(drained >= 0 ? 1u : 0u, drained == 1 ? 1u : 0u, reads);
        if (step != BC250_FLIP_DRAIN_AGAIN) break;
        KeStallExecutionProcessor(1);
    }
    InterlockedExchange(&wddm->FaultDrainReads, (LONG)reads);
    InterlockedExchange(&wddm->FaultDrain, (LONG)step);
    GuardLog("wddm: BD-170 E5 code 0x%06lX node %u blank 0x%08X notified %u drain %lu (1 drained, 2 unknown) reads %lu",
             wddm->FaultCode, wddm->FaultNode, (ULONG)wddm->FaultBlank, notified ? 1u : 0u, step, reads);
}

// The writers' sections. A writer (modeset.c, connector_link.c, hdmi_link.c, the destroy restore below) runs at
// PASSIVE_LEVEL and takes L for admission, retirement commit, Resume before any unblank, and End. Its long
// work runs outside L. Between Begin and End the generation is odd. Hardware flip requests are refused; only
// the gate-closed software-report route deposits Desired (the historical W labels).
typedef struct _BC250_WDDM_WRITER {
    BC250_DEVICE* Device;
    BC250_WDDM* Wddm;
    ULONG Kind;
    ULONG Generation;
    BOOLEAN Invalidate;
    NTSTATUS Status;
    ULONG Answer;
    BOOLEAN Claimed;
    BOOLEAN Wrote;
    BC250_DCN_WRITER_BRIDGE Bridge;
} BC250_WDDM_WRITER;

// W-u (section 7.2), inside L: the record of a deposit that the flip transaction just made, in the next ring slot.
static void WddmWuRecord(_Inout_ BC250_WDDM* Wddm, _In_ const DXGKARG_SETVIDPNSOURCEADDRESS* Args, ULONG Generation)
{
    const LONG record = InterlockedIncrement(&Wddm->WuRecordNext);
    BC250_WDDM_WU_RECORD* r = &Wddm->WuRecords[(ULONG)(record - 1) % BC250_WDDM_WU_RECORDS];

    RtlZeroMemory(r, sizeof(*r));
    r->Record = record;
    r->Code = BC250_FLIP_RECORD_WRITER_UNPROVEN;
    r->Request = (ULONGLONG)Args->PrimaryAddress.QuadPart;
    r->Flags = Args->Flags.Value;
    r->Generation = Generation;
    r->Writer = Wddm->WriterKind;
    r->Deposit = Wddm->DepositSequence;
    r->End = BC250_WU_OPEN;
    r->DepositQpc = KeQueryPerformanceCounter(NULL).QuadPart;
    r->ToEndUs = -1;
}

// W-u, inside L, at the writer's End: each open record of this writer gets its time to End and how it left the writer
// (Bc250WuEnd). Answer and Result are End's own (Result only for BC250_WRITER_END_PROGRAM).
static void WddmWuClose(_In_ const BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, ULONG Generation, ULONG Answer,
                        ULONG Result)
{
    const LONGLONG now = KeQueryPerformanceCounter(NULL).QuadPart;
    ULONG n;

    for (n = 0; n < BC250_WDDM_WU_RECORDS; n++)
    {
        BC250_WDDM_WU_RECORD* r = &Wddm->WuRecords[n];
        LONGLONG us = now - r->DepositQpc;
        if (r->Record == 0 || r->End != BC250_WU_OPEN || r->Generation != Generation) continue;
        us = Device->DcnQpcFrequency > 0 && us > 0 ? us * 1000000 / Device->DcnQpcFrequency : 0;
        r->ToEndUs = us > 0x7FFFFFFF ? 0x7FFFFFFF : (LONG)us;
        r->End = Bc250WuEnd(r->Unblank ? 1u : 0u, r->Deposit == Wddm->DepositSequence ? 1u : 0u, Answer, Result);
        if (r->ToEndUs > Wddm->WuToEndMaxUs) Wddm->WuToEndMaxUs = r->ToEndUs;
    }
}

// The summary's copy of the W-u records, in one section (the records change only inside L). FALSE when the section
// could not run: the summary then says so and prints none.
typedef struct _BC250_WDDM_WU_SECTION {
    BC250_WDDM* Wddm;
    BC250_WDDM_WU_COPY* Copy;
} BC250_WDDM_WU_SECTION;

static BOOLEAN WddmWuCopyLocked(_In_ PVOID Context)
{
    BC250_WDDM_WU_SECTION* s = (BC250_WDDM_WU_SECTION*)Context;
    ULONG n;
    s->Copy->Next = s->Wddm->WuRecordNext;
    s->Copy->ToEndMaxUs = s->Wddm->WuToEndMaxUs;
    for (n = 0; n < BC250_WDDM_WU_RECORDS; n++) s->Copy->Records[n] = s->Wddm->WuRecords[n];
    return TRUE;
}

static BOOLEAN WddmWuCopy(_In_ BC250_DEVICE* Device, _In_ BC250_WDDM* Wddm, _Out_ BC250_WDDM_WU_COPY* Copy)
{
    BC250_WDDM_WU_SECTION s;
    RtlZeroMemory(Copy, sizeof(*Copy));
    s.Wddm = Wddm;
    s.Copy = Copy;
    return NT_SUCCESS(WddmSection(Device, WddmWuCopyLocked, &s));
}

#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
// BD-170 r6, lab flavour only (section 7.2, the witnesses), inside L, for an immediate request that edge E programmed,
// AFTER its shipping decision: Outcome and Result are the decision as the shipping reader made it, recorded as such,
// and only then are E and R read (DcnWitnessRead) and compared with Q. Nothing here changes the answer.
static void WddmWitness(_In_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, _In_ const BC250_DCN_WITNESS_ARM* Arm,
                        _In_ const DXGKARG_SETVIDPNSOURCEADDRESS* Args, ULONGLONG Physical, ULONG Generation,
                        ULONG Outcome, ULONG Result)
{
    BC250_WDDM_WITNESS_RECORD r;
    BC250_DCN_WITNESS_READ read;
    BOOLEAN disagree = FALSE;

    RtlZeroMemory(&r, sizeof(r));
    r.Request = InterlockedIncrement(&Wddm->WitnessRequests);
    r.Generation = Generation;
    r.Flags = Args->Flags.Value;
    r.Outcome = Outcome;
    r.Result = Result;
    r.Q = (Outcome == BC250_DCN_IMMEDIATE_DONE || Outcome == BC250_DCN_IMMEDIATE_DONE_LATE) ? TRUE : FALSE;
    r.Card = (ULONGLONG)Args->PrimaryAddress.QuadPart;
    r.Physical = Physical;
    r.ArmStatus = Arm->Status;
    r.Fresh = Arm->Fresh ? TRUE : FALSE;
    r.InterruptBefore = Arm->InterruptBefore;
    r.InterruptCleared = Arm->InterruptCleared;
    r.PendingBefore = Arm->PendingBefore;
    r.PendingAfter = Arm->PendingAfter;
    r.ReadStatus = DcnWitnessRead(Device, &read);
    r.Interrupt = read.Interrupt;
    r.Low1 = read.Low1;
    r.High1 = read.High1;
    r.High2 = read.High2;
    r.Low2 = read.Low2;
    r.Earliest1 = read.Earliest1;
    r.Earliest2 = read.Earliest2;
    if (!NT_SUCCESS(r.ReadStatus))
    {
        InterlockedIncrement(&Wddm->WitnessReadFailed);
        disagree = TRUE;
    }
    else
    {
        r.E = read.Event ? TRUE : FALSE;
        r.R1 = read.Earliest1 == Physical ? TRUE : FALSE;
        r.R2 = read.Earliest2 == Physical ? TRUE : FALSE;
        if (!r.Fresh) { InterlockedIncrement(&Wddm->WitnessStale); disagree = TRUE; }
        if (r.Fresh && r.Q != r.E) { InterlockedIncrement(&Wddm->WitnessQe); disagree = TRUE; }
        if (r.Q != r.R1) { InterlockedIncrement(&Wddm->WitnessQr); disagree = TRUE; }
        if (r.Fresh && r.E != r.R1) { InterlockedIncrement(&Wddm->WitnessEr); disagree = TRUE; }
        if (r.R1 != r.R2) { InterlockedIncrement(&Wddm->WitnessOrder); disagree = TRUE; }
    }
    Wddm->WitnessLast = r;
    if (!disagree) return;
    if ((ULONG)Wddm->WitnessKept < BC250_WDDM_WITNESS_RECORDS) Wddm->WitnessRecords[Wddm->WitnessKept++] = r;
    else InterlockedIncrement(&Wddm->WitnessLost);
}

typedef struct _BC250_WDDM_WITNESS_SECTION {
    BC250_WDDM* Wddm;
    BC250_WDDM_WITNESS_COPY* Copy;
} BC250_WDDM_WITNESS_SECTION;

static BOOLEAN WddmWitnessCopyLocked(_In_ PVOID Context)
{
    BC250_WDDM_WITNESS_SECTION* s = (BC250_WDDM_WITNESS_SECTION*)Context;
    const BC250_WDDM* w = s->Wddm;
    BC250_WDDM_WITNESS_COPY* c = s->Copy;
    ULONG n;
    c->Requests = w->WitnessRequests;
    c->ReadFailed = w->WitnessReadFailed;
    c->Stale = w->WitnessStale;
    c->Qe = w->WitnessQe;
    c->Qr = w->WitnessQr;
    c->Er = w->WitnessEr;
    c->Order = w->WitnessOrder;
    c->Lost = w->WitnessLost;
    c->Kept = w->WitnessKept;
    for (n = 0; n < BC250_WDDM_WITNESS_RECORDS; n++) c->Records[n] = w->WitnessRecords[n];
    c->Last = w->WitnessLast;
    return TRUE;
}

static BOOLEAN WddmWitnessCopy(_In_ BC250_DEVICE* Device, _In_ BC250_WDDM* Wddm, _Out_ BC250_WDDM_WITNESS_COPY* Copy)
{
    BC250_WDDM_WITNESS_SECTION s;
    RtlZeroMemory(Copy, sizeof(*Copy));
    s.Wddm = Wddm;
    s.Copy = Copy;
    return NT_SUCCESS(WddmSection(Device, WddmWitnessCopyLocked, &s));
}
#endif

static BOOLEAN WddmWriterBeginLocked(_In_ PVOID Context)
{
    BC250_WDDM_WRITER* w = (BC250_WDDM_WRITER*)Context;
    BC250_WDDM* wddm = w->Wddm;
    const ULONG generation = (ULONG)wddm->PrimarySequence;
    (void)WddmWriterRecoveryLocked(w->Device);
    w->Answer = Bc250WriterBegin(wddm->Faulting != 0 ? 1u : 0u, generation & 1u);
    if (w->Answer != BC250_WRITER_BEGIN_TAKE) return TRUE;
    // Publish BUSY before programming. Keep the old application allocation
    // retained while EARLIEST catches up outside L, possibly at the next VSync.
    w->Generation = generation;
    w->Claimed = TRUE;
    InterlockedExchange(&wddm->PrimarySequence, (LONG)(generation + 1u));
    wddm->WriterActive = TRUE;
    wddm->WriterReady = FALSE;
    wddm->WriterResumed = FALSE;
    wddm->WriterDeposit = FALSE;
    wddm->WriterKind = w->Kind;
    w->Status = w->Device->VidPnFlipEnabled ?
        DcnWriterBridge(w->Device, &wddm->PrimarySequence, generation + 1u, &w->Wrote, &w->Bridge) : STATUS_SUCCESS;
    if (w->Wrote) InterlockedIncrement(&wddm->WriterBridgePrograms);
    if (!NT_SUCCESS(w->Status)) {
        if (w->Wrote) {
            InterlockedIncrement(&wddm->WriterBridgeFailures);
            WddmFlipFault(w->Device, wddm, BC250_FLIP_FAULT_BRIDGE, BC250_WDDM_NODE_3D);
        } else {
            wddm->WriterActive = FALSE;
            InterlockedExchange(&wddm->PrimarySequence, (LONG)generation);
            w->Claimed = FALSE;
        }
        w->Answer = w->Wrote ? BC250_WRITER_BEGIN_FAULTING : BC250_WRITER_BEGIN_REFUSED;
        return TRUE;
    }
    wddm->PrimaryNeedsRestore = TRUE;
    return TRUE;
}

// Commit the PASSIVE retirement proof under L. A fault or changed owner during
// the observation cannot grant the long operation, including allocation free.
static BOOLEAN WddmWriterRetiredLocked(_In_ PVOID Context)
{
    BC250_WDDM_WRITER* w = (BC250_WDDM_WRITER*)Context;
    BC250_WDDM* wddm = w->Wddm;
    (void)WddmWriterRecoveryLocked(w->Device);
    if (!wddm->WriterActive || (ULONG)wddm->PrimarySequence != w->Generation + 1u) {
        w->Status = STATUS_INVALID_DEVICE_STATE;
        WddmFlipFault(w->Device, wddm, BC250_FLIP_FAULT_SECTION, BC250_WDDM_NODE_3D);
    } else if (wddm->Faulting) {
        w->Status = STATUS_DEVICE_HARDWARE_ERROR;
    } else if (!NT_SUCCESS(w->Status)) {
        InterlockedIncrement(&wddm->WriterBridgeFailures);
        WddmFlipFault(w->Device, wddm, BC250_FLIP_FAULT_BRIDGE, BC250_WDDM_NODE_3D);
    } else {
        if (w->Device->VidPnFlipEnabled) InterlockedIncrement(&wddm->WriterBridgeRetired);
        wddm->WriterReady = TRUE;
    }
    return TRUE;
}

// modeset.c's side of the primary transaction (bc250kmd.h says what it is for). PASSIVE_LEVEL. Another writer's odd
// generation is tried again, at most 200 tries 10 us apart (2 ms) with the stall outside L; FAULTING refuses at once.
#define BC250_WDDM_EXCLUSIVE_TRIES 200u
#define BC250_WDDM_EXCLUSIVE_STEP_US 10u

// BD-195: where WddmPrimaryExclusiveBegin answered FALSE. 31v2's refused LINK_DISABLE (a bridge admission without a
// write) left no counter and no line, only the caller's BUSY.
#define BC250_WRITER_REFUSED_IRQL 0u        // above PASSIVE_LEVEL (also WriterHigh)
#define BC250_WRITER_REFUSED_SECTION 1u     // a section failed (also WriterSyncFailed, or recovery when claimed)
#define BC250_WRITER_REFUSED_BRIDGE 2u      // BEGIN_REFUSED: DcnWriterBridge failed before any write
#define BC250_WRITER_REFUSED_FAULTING 3u    // BEGIN_FAULTING: FAULTING, or a bridge that failed after its write
#define BC250_WRITER_REFUSED_RETRY 4u       // another writer held the odd generation for every try
#define BC250_WRITER_REFUSED_RETIRE 5u      // the bridge did not retire, or the commit of its proof refused

// Counted at every site. Logged at PASSIVE_LEVEL outside L, the first 16 and then every 256th, with the state the
// bridge decided on: the runtime display flag and the RuntimePost pitch that I3 keeps nonzero.
static void WddmWriterRefused(_Inout_ BC250_WDDM* Wddm, _In_ const BC250_WDDM_WRITER* W, ULONG Site, NTSTATUS Status)
{
    const LONG n = InterlockedIncrement(&Wddm->WriterRefused);
    InterlockedIncrement(&Wddm->WriterRefusedByKind[W->Kind < BC250_WRITER_KINDS ? W->Kind : BC250_WRITER_NONE]);
    Wddm->WriterRefusedKind = W->Kind;
    Wddm->WriterRefusedSite = Site;
    Wddm->WriterRefusedAnswer = W->Answer;
    Wddm->WriterRefusedStatus = Status;
    if (Site == BC250_WRITER_REFUSED_IRQL || (n > 16 && (n & 255) != 0)) return;
    GuardLog("wddm: writer kind %lu refused at %s answer %lu status %08lX wrote %u",
             W->Kind, WddmWriterRefusedSite(Site), W->Answer, (ULONG)Status, W->Wrote);
    GuardLog("wddm: writer refused sequence %ld holder %lu faulting %ld changed %ld post-valid %ld pitch %lu",
             Wddm->PrimarySequence, Wddm->WriterKind, Wddm->Faulting, W->Device->RuntimeDisplayChanged,
             W->Device->RuntimePostValid, W->Device->RuntimePost.Pitch);
}

BOOLEAN WddmPrimaryExclusiveBegin(_Inout_ BC250_DEVICE* Device, ULONG Kind, _Out_ ULONG* Generation)
{
    BC250_WDDM* wddm = WddmOf(Device);
    BC250_WDDM_WRITER w;
    NTSTATUS status;
    ULONG i;

    *Generation = 0;
    if (wddm == NULL) return TRUE;
    RtlZeroMemory(&w, sizeof(w));
    w.Device = Device;
    w.Wddm = wddm;
    w.Kind = Kind;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        InterlockedIncrement(&wddm->WriterHigh);
        WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_IRQL, STATUS_INVALID_DEVICE_STATE);
        return FALSE;
    }
    for (i = 0; i < BC250_WDDM_EXCLUSIVE_TRIES; i++)
    {
        status = WddmSection(Device, WddmWriterBeginLocked, &w);
        if (!NT_SUCCESS(status))
        {
            // No acquired owner and no write: ordinary BEGIN_REFUSED, no E5.
            if (w.Claimed) WddmWriterSectionFailure(Device, wddm, status);
            else InterlockedIncrement(&wddm->WriterSyncFailed);
            WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_SECTION, status);
            return FALSE;
        }
        if (w.Answer == BC250_WRITER_BEGIN_TAKE) {
            if (Device->VidPnFlipEnabled)
                w.Status = DcnWriterBridgeRetire(Device, &wddm->PrimarySequence, w.Generation + 1u, &w.Bridge);
            status = WddmSection(Device, WddmWriterRetiredLocked, &w);
            if (!NT_SUCCESS(status)) {
                WddmWriterSectionFailure(Device, wddm, status);
                WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_SECTION, status);
                return FALSE;
            }
            if (!NT_SUCCESS(w.Status)) {
                WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_RETIRE, w.Status);
                return FALSE;
            }
            *Generation = w.Generation;
            return TRUE;
        }
        if (w.Answer == BC250_WRITER_BEGIN_REFUSED) {
            WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_BRIDGE, w.Status);
            return FALSE;
        }
        if (w.Answer == BC250_WRITER_BEGIN_FAULTING) {
            InterlockedIncrement(&wddm->WriterFaulting);
            WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_FAULTING, w.Status);
            return FALSE;
        }
        KeStallExecutionProcessor(BC250_WDDM_EXCLUSIVE_STEP_US);
    }
    WddmWriterRefused(wddm, &w, BC250_WRITER_REFUSED_RETRY, STATUS_DEVICE_BUSY);
    return FALSE;
}

// Only driver-owned bridge storage may escape L during a writer. Resume
// grants that operation, never an application address or a stale Desired copy.
static BOOLEAN WddmWriterResumeLocked(_In_ PVOID Context)
{
    BC250_WDDM_WRITER* w = (BC250_WDDM_WRITER*)Context;
    (void)WddmWriterRecoveryLocked(w->Device);
    w->Status = w->Wddm->Faulting ? STATUS_DEVICE_HARDWARE_ERROR :
        (w->Wddm->WriterActive && w->Wddm->WriterReady ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE);
    if (NT_SUCCESS(w->Status)) w->Wddm->WriterResumed = TRUE;
    return TRUE;
}

NTSTATUS WddmWriterResume(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = WddmOf(Device);
    BC250_WDDM_WRITER w;
    NTSTATUS status;
    if (wddm == NULL) return STATUS_SUCCESS;
    RtlZeroMemory(&w, sizeof(w));
    w.Device = Device;
    w.Wddm = wddm;
    status = WddmSection(Device, WddmWriterResumeLocked, &w);
    if (!NT_SUCCESS(status)) {
        WddmWriterSectionFailure(Device, wddm, status);
        return status;
    }
    return w.Status;
}

// The fields DestroyAllocation and an invalidating End reset when the plane goes back to the firmware surface, the
// plane format included (M15.14): the firmware surface is ARGB8888 (DcnFlipToFirmwareSurface).
static void WddmPrimaryInvalidate(_Inout_ BC250_WDDM* Wddm)
{
    Wddm->PrimaryNeedsRestore = TRUE;
    InterlockedExchange64(&Wddm->PrimaryAddress.QuadPart, 0);
    Wddm->PrimaryPitch = 0;
    Wddm->PrimaryPlaneFormat = 0;
    InterlockedExchange64(&Wddm->ScanoutObject, 0);
}

// End chooses the latest Desired while holding L. The long writer used only
// reserved storage, so no application snapshot can be programmed after this
// handoff. With no deposit, a non-invalidating writer restores its initial
// Desired. A changed mode can refuse that old layout; the bridge remains safe.
static BOOLEAN WddmWriterEndLocked(_In_ PVOID Context)
{
    BC250_WDDM_WRITER* w = (BC250_WDDM_WRITER*)Context;
    BC250_DEVICE* device = w->Device;
    BC250_WDDM* wddm = w->Wddm;
    BOOLEAN programmed = FALSE;
    const BOOLEAN owes = wddm->WriterDeposit || wddm->DesiredOwed;
    const BOOLEAN restore = !w->Invalidate && wddm->PrimaryAddress.QuadPart != 0 && wddm->PrimaryPitch != 0;
    ULONG result = BC250_FLIP_RESULT_REFUSED;

    w->Status = STATUS_SUCCESS;
    if (!wddm->WriterActive || (ULONG)wddm->PrimarySequence != w->Generation + 1u) {
        w->Status = STATUS_INVALID_DEVICE_STATE;
        return TRUE;
    }
    (void)WddmWriterRecoveryLocked(device);
    w->Answer = BC250_WRITER_END_NOTHING;
    if ((owes || restore) && device->VidPnFlipEnabled && wddm->Faulting == 0)
        w->Answer = Bc250WriterEnd(1u, 0u, DcnOtgRunning(device));
    if (w->Answer == BC250_WRITER_END_FAULT) {
        WddmFlipFault(device, wddm, BC250_FLIP_FAULT_OWED_STOPPED, BC250_WDDM_NODE_3D);
        w->Status = STATUS_DEVICE_HARDWARE_ERROR;
    } else if (w->Answer == BC250_WRITER_END_PROGRAM) {
        BOOLEAN wrote = FALSE;
        const NTSTATUS status = DcnFlipSourceAddress(device, (ULONGLONG)wddm->PrimaryAddress.QuadPart,
            wddm->PrimaryPitch, wddm->PrimaryPlaneFormat, wddm->PrimaryBytes, FALSE, NULL, &wrote);
        result = Bc250FlipResult(wrote ? 1u : 0u, NT_SUCCESS(status) ? 1u : 0u, 0u, 0u);
        if (result == BC250_FLIP_RESULT_SUCCESS) {
            programmed = TRUE;
            wddm->PrimaryNeedsRestore = FALSE;
            InterlockedExchange(&wddm->PrimaryProgrammedSequence, (LONG)(w->Generation + 2u));
            InterlockedIncrement(&wddm->EndPrograms);
        } else if (result == BC250_FLIP_RESULT_REFUSED) {
            InterlockedIncrement(&wddm->OwedInvalidated);
            wddm->DesiredOwed = FALSE;
            wddm->PrimaryNeedsRestore = TRUE;
            w->Status = status;
        } else {
            WddmFlipFault(device, wddm, BC250_FLIP_FAULT_PROGRAM, BC250_WDDM_NODE_3D);
            w->Status = STATUS_DEVICE_HARDWARE_ERROR;
        }
    }
    if (wddm->Faulting) w->Status = STATUS_DEVICE_HARDWARE_ERROR;
    if (w->Invalidate && !programmed && !(owes && !device->VidPnFlipEnabled)) WddmPrimaryInvalidate(wddm);
    WddmWuClose(device, wddm, w->Generation + 1u, w->Answer, result);
    wddm->WriterDeposit = FALSE;
    wddm->WriterResumed = FALSE;
    wddm->WriterActive = FALSE;
    wddm->WriterReady = FALSE;
    InterlockedExchange(&wddm->PrimarySequence, (LONG)(w->Generation + 2u));
    return TRUE;
}

NTSTATUS WddmPrimaryExclusiveEnd(_Inout_ BC250_DEVICE* Device, ULONG Generation, BOOLEAN Invalidate)
{
    BC250_WDDM* wddm = WddmOf(Device);
    BC250_WDDM_WRITER w;
    NTSTATUS status;
    if (wddm == NULL) return STATUS_SUCCESS;
    RtlZeroMemory(&w, sizeof(w));
    w.Device = Device;
    w.Wddm = wddm;
    w.Generation = Generation;
    w.Invalidate = Invalidate;
    status = WddmSection(Device, WddmWriterEndLocked, &w);
    if (!NT_SUCCESS(status)) {
        WddmWriterSectionFailure(Device, wddm, status);
        return status;
    }
    if (w.Status == STATUS_INVALID_DEVICE_STATE) WddmWriterSectionFailure(Device, wddm, w.Status);
    return w.Status;
}

// At the start of D3 (pnp.c's Bc250SetPowerState), before the power writers: dxgmms2 flushed the flip queue
// (VidSchFlushAdapter, VidSchFlushDevice, facts M904), so no next-VSync success is owed any more, and the D3 writers'
// End must not program one into a pipe that is going down.
static BOOLEAN WddmFlipFlushedLocked(_In_ PVOID Context)
{
    BC250_WDDM_WRITER* w = (BC250_WDDM_WRITER*)Context;
    w->Wddm->DesiredOwed = FALSE;
    InterlockedIncrement(&w->Wddm->FlipFlushes);
    return TRUE;
}

void WddmFlipFlushed(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm = WddmOf(Device);
    BC250_WDDM_WRITER w;

    if (wddm == NULL) return;
    RtlZeroMemory(&w, sizeof(w));
    w.Device = Device;
    w.Wddm = wddm;
    if (!NT_SUCCESS(WddmSection(Device, WddmFlipFlushedLocked, &w))) InterlockedIncrement(&wddm->WriterSyncFailed);
}

// A live writer owns only reserved storage. Its Desired may be invalidated
// under L without waiting for the PASSIVE operation. Otherwise Begin first
// retires the scanned application buffer to the reserved bridge.
typedef struct _BC250_WDDM_DESTROY {
    BC250_WDDM* Wddm;
    ULONGLONG Serial;
    BOOLEAN Taken;
    BOOLEAN NeedWriter;
    BOOLEAN Refused;
} BC250_WDDM_DESTROY;

static BOOLEAN WddmDestroyTakeLocked(_In_ PVOID Context)
{
    BC250_WDDM_DESTROY* d = (BC250_WDDM_DESTROY*)Context;
    BC250_WDDM* wddm = d->Wddm;
    d->NeedWriter = FALSE;
    d->Taken = FALSE;
    d->Refused = FALSE;
    if ((ULONGLONG)wddm->ScanoutObject != d->Serial) return TRUE;
    if (!wddm->WriterActive) { d->NeedWriter = TRUE; return TRUE; }
    // Begin has published BUSY but may still be retiring the old application
    // surface at PASSIVE. Active alone does not authorize that object's free.
    if (!wddm->WriterReady || wddm->Faulting) { d->Refused = TRUE; return TRUE; }
    wddm->DesiredOwed = FALSE;
    wddm->WriterDeposit = FALSE;
    WddmPrimaryInvalidate(wddm);
    d->Taken = TRUE;
    return TRUE;
}

static NTSTATUS WddmDestroyRestore(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_WDDM* Wddm, ULONGLONG Serial)
{
    BC250_WDDM_DESTROY d;
    ULONG generation = 0;
    NTSTATUS status, ended;
    d.Wddm = Wddm;
    d.Serial = Serial;
    d.Taken = FALSE;
    d.NeedWriter = FALSE;
    d.Refused = FALSE;
    status = WddmSection(Device, WddmDestroyTakeLocked, &d);
    if (!NT_SUCCESS(status)) {
        WddmWriterSectionFailure(Device, Wddm, status);
        return status;
    }
    if (d.Refused) return STATUS_DEVICE_BUSY;
    if (d.NeedWriter) {
        if (!WddmPrimaryExclusiveBegin(Device, BC250_WRITER_DESTROY, &generation)) return STATUS_DEVICE_BUSY;
        status = WddmSection(Device, WddmDestroyTakeLocked, &d);
        if (!NT_SUCCESS(status)) {
            WddmWriterSectionFailure(Device, Wddm, status);
            return status;
        }
        ended = WddmPrimaryExclusiveEnd(Device, generation, d.Taken);
        if (!NT_SUCCESS(ended)) return ended;
        if (d.Refused) return STATUS_DEVICE_BUSY;
    }
    if (d.Taken && InterlockedIncrement(&Wddm->ScanoutTeardowns) <= BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: DestroyAllocation retired scanned surface to reserved storage");
    return STATUS_SUCCESS;
}

// One flip request: the DDI's arguments, the answer, and what the low entry logs after its section.
typedef struct _BC250_WDDM_FLIP {
    BC250_DEVICE* Device;
    BC250_WDDM* Wddm;
    const DXGKARG_SETVIDPNSOURCEADDRESS* Args;
    NTSTATUS Result;
    BOOLEAN LogRefusal;                 // the scan-out refusal line, with Admit and Candidate
    int Admit;
    BC250_SCANOUT_CANDIDATE Candidate;
    BOOLEAN LogScanout;                 // the scan-out flip line, with Physical, Pitch, Bytes and PlaneFormat
    ULONGLONG Physical;
    ULONG Pitch;
    ULONGLONG Bytes;
    ULONG PlaneFormat;
} BC250_WDDM_FLIP;

// E5's node (section 7.2, E5 step 3): the node of the first context that the flip call carries, when it is a context
// of this driver, else node 0 (the 3D node). dxgkrnl passes the context handles this driver returned from CreateContext.
static UINT WddmFlipNode(_In_ const DXGKARG_SETVIDPNSOURCEADDRESS* Args)
{
    const BC250_WDDM_OBJECT* context =
        Args->ContextCount > 0 ? WddmObject(Args->Context[0], BC250_WDDM_MAGIC_CONTEXT) : NULL;
    return context != NULL && context->NodeOrdinal < BC250_WDDM_NODE_COUNT_MAX ? context->NodeOrdinal : BC250_WDDM_NODE_3D;
}

// The request published as Desired (section 7.2): the address, the segment, the pitch, the bytes and the format.
static void WddmFlipPublish(_Inout_ BC250_WDDM* Wddm, _In_ const DXGKARG_SETVIDPNSOURCEADDRESS* Args, ULONG Pitch,
                            ULONGLONG Bytes, ULONG PlaneFormat)
{
    InterlockedExchange64(&Wddm->PrimaryAddress.QuadPart, Args->PrimaryAddress.QuadPart);
    Wddm->PrimarySegment = Args->PrimarySegment;
    Wddm->PrimaryPitch = Pitch;
    Wddm->PrimaryBytes = Bytes;
    Wddm->PrimaryPlaneFormat = PlaneFormat;
}

// The flip transaction, inside L: at DIRQL from dxgmms2's own synchronized call, or inside the low entry's section.
// The edges of flip_state.h in their order: F0 FAULTING, F1 overlap, A admission, W a writer (deposit), E program.
static BOOLEAN WddmFlipTransaction(_In_ PVOID Context)
{
    BC250_WDDM_FLIP* flip = (BC250_WDDM_FLIP*)Context;
    BC250_DEVICE* device = flip->Device;
    BC250_WDDM* wddm = flip->Wddm;
    const DXGKARG_SETVIDPNSOURCEADDRESS* args = flip->Args;
    const ULONG generation = (ULONG)wddm->PrimarySequence;
    const BOOLEAN immediate = args->Flags.FlipImmediate ? TRUE : FALSE;
    // A refusal cannot report completion: immediate SUCCESS retires the OS
    // primary. M904's legacy exclusive queue assertion is an OS error-path
    // risk, not permission to acknowledge an unimplemented request.
    const BOOLEAN exclusive = args->Flags.IndependentFlipExclusive ? TRUE : FALSE;
    const LONGLONG address = args->PrimaryAddress.QuadPart;
    BC250_FLIP_FACTS facts;
    ULONG edge, pitch, planeFormat;
    const BC250_PLANE_ENCODING* plane;
    ULONGLONG bytes = 0;
    BOOLEAN changed, scanout = FALSE;
    BC250_WDDM_OBJECT* scanoutObject = NULL;
    NTSTATUS status = STATUS_SUCCESS;

    // M15.14 increment 2, the kernel-side witness of the OS's flip mode, counted for every call, admitted or refused:
    // these flags describe the video present source's mode, not this allocation. The bitfields are read, never a
    // mask: the trailing comments of DXGK_SETVIDPNSOURCEADDRESS_FLAGS repeat 0x10 twice and are shifted by a bit from
    // FlipStereoPreferRight on, so a transcribed number would report a run that did enter independent flip as a run
    // that never did.
    if (args->Flags.ModeChange) InterlockedIncrement(&wddm->ScanoutFlipFlags[0]);
    if (args->Flags.FlipImmediate) InterlockedIncrement(&wddm->ScanoutFlipFlags[1]);
    if (args->Flags.SharedPrimaryTransition) InterlockedIncrement(&wddm->ScanoutFlipFlags[2]);
    if (args->Flags.IndependentFlipExclusive) InterlockedIncrement(&wddm->ScanoutFlipFlags[3]);
    // A NULL allocation preserves current private properties; initially POST, whose format is the firmware's
    // ARGB8888. The surface has the committed source mode's size (modeset.c). The bytes follow the format's own size,
    // never a literal 4 (M15.14).
    pitch=wddm->PrimaryPitch?wddm->PrimaryPitch:
        (device->RuntimePostValid?device->RuntimePost.Pitch:device->Post.Pitch);
    planeFormat=wddm->PrimaryPlaneFormat?wddm->PrimaryPlaneFormat:BC250_PLANE_FORMAT_ARGB8888;
    plane=Bc250PlaneEncoding(planeFormat);
    if (!plane || !DcnLinearSurfaceBytes(DisplaySourceWidth(device),DisplaySourceHeight(device),pitch,
                                         plane->BytesPerPixel,&bytes))
        status=STATUS_INVALID_PARAMETER;
    if (args->hAllocation)
    {
        // M15.14. The rule lives in scanout_admit.h, is host-tested through each of its refusals, and is still
        // followed by dcn.c's AddressAllowed, which holds the address itself against the firmware plane and the VRAM
        // carve-out. dxgkrnl's own shared primary asks for no scan-out and is admitted by the same four checks.
        BC250_WDDM_OBJECT* allocation=WddmObject(args->hAllocation,BC250_WDDM_MAGIC_ALLOCATION);
        BC250_SCANOUT_CANDIDATE candidate;
        int admit;
        RtlZeroMemory(&candidate,sizeof(candidate));
        if (allocation) {
            // A BC2A allocation has no LB7A description, so its scan-out words are the description; an LB7A one
            // describes itself and left those words zero.
            candidate.UmdAlloc=allocation->UmdAlloc?1:0;
            candidate.ScanoutRequested=allocation->ScanoutRequested?1:0;
            candidate.Width=allocation->UmdAlloc?allocation->ScanoutWidth:allocation->Allocation.Width;
            candidate.Height=allocation->UmdAlloc?allocation->ScanoutHeight:allocation->Allocation.Height;
            candidate.Pitch=allocation->UmdAlloc?allocation->ScanoutPitch:allocation->Allocation.Pitch;
            candidate.Format=allocation->UmdAlloc?allocation->ScanoutFormat:allocation->Allocation.Format;
            candidate.Size=allocation->UmdAlloc?allocation->UmdBytes:allocation->Allocation.Size;
            candidate.Segment=args->PrimarySegment;
            candidate.Address=(ULONGLONG)args->PrimaryAddress.QuadPart;
        }
        // The predicate holds the candidate's 4 KiB alignment against the card address, which is what this DDI
        // carries; the plane is given the translated physical one (dcn_translate.c). Page alignment survives that
        // subtraction only while both bases are page-aligned themselves, so the invariant is checked here.
        if (allocation && allocation->ScanoutRequested && !wddm->ScanoutAdmitGate)
            admit=BC250_SCANOUT_GATED;      // the operator's switch: 0.7.205.1 behaviour for this start
        else if (allocation && allocation->ScanoutRequested &&
            ((device->VramMcBase | (ULONGLONG)device->VramPhysical.QuadPart) & 0xFFFull))
            admit=BC250_SCANOUT_ALIGNMENT;
        else
            admit=Bc250ScanoutAdmit(allocation?&candidate:NULL,DisplaySourceWidth(device),DisplaySourceHeight(device),
                                    BC250_WDDM_SEGMENT_VRAM,&pitch,&bytes);
        InterlockedIncrement(&wddm->ScanoutAdmits[admit]);
        if (candidate.ScanoutRequested) InterlockedIncrement(&wddm->ScanoutRequests);
        if (admit!=BC250_SCANOUT_ADMIT_OK) {
            // A refusal publishes nothing: the plane keeps the surface it has. The line is logged after L, with a
            // budget of its own (a refusal recurs for the life of the swap chain).
            status=STATUS_INVALID_PARAMETER;
            if (candidate.ScanoutRequested) { flip->LogRefusal=TRUE; flip->Admit=admit; flip->Candidate=candidate; }
        }
        else {
            scanout=candidate.ScanoutRequested?TRUE:FALSE;
            if (scanout) scanoutObject=allocation;
            // Admission guarantees a plane encoding of the row's own size (scanout_admit.h).
            planeFormat=Bc250PlaneFormatOf(candidate.Format);
            status=STATUS_SUCCESS;
        }
    }

    RtlZeroMemory(&facts, sizeof(facts));
    (void)WddmWriterRecoveryLocked(device);
    facts.Faulting = wddm->Faulting != 0 ? 1u : 0u;
    facts.GenerationOdd = generation & 1u;
    facts.WriterActive = wddm->WriterActive ? 1u : 0u;
    facts.Admitted = NT_SUCCESS(status) ? 1u : 0u;
    // With the flip gate closed (EnableVidPnFlip) no request reaches the plane, so none is immediate for it.
    facts.Immediate = immediate && device->VidPnFlipEnabled ? 1u : 0u;
    facts.Resumed = wddm->WriterResumed ? 1u : 0u;
    // A readback only where a rule uses it: an immediate request during a writer (W-i).
    if (facts.WriterActive && facts.Immediate) facts.Drained = DcnHubpDrained(device) == 1 ? 1u : 0u;
    edge = Bc250FlipEdge(&facts);
    InterlockedIncrement(&wddm->FlipEdges[edge]);
    // The format is part of what the plane shows: the same address in another format is a change.
    changed = wddm->PrimaryNeedsRestore || wddm->PrimaryAddress.QuadPart != address || pitch != wddm->PrimaryPitch ||
              planeFormat != wddm->PrimaryPlaneFormat;

    switch (edge)
    {
    case BC250_FLIP_EDGE_FAULTING:
        // F0: nothing is programmed. The OS keeps the previous primary referenced (facts M904) and recovers.
        flip->Result = STATUS_DEVICE_HARDWARE_ERROR;
        return TRUE;
    case BC250_FLIP_EDGE_OVERLAP:
        WddmFlipFault(device, wddm, BC250_FLIP_FAULT_OVERLAP, WddmFlipNode(args));
        flip->Result = STATUS_DEVICE_HARDWARE_ERROR;
        return TRUE;
    case BC250_FLIP_EDGE_REFUSED:
        if (exclusive && immediate) { InterlockedIncrement(&wddm->ExclusiveRefused); flip->Result = status; }
        else { if (exclusive) InterlockedIncrement(&wddm->ExclusiveFailed); flip->Result = status; }
        return TRUE;
    case BC250_FLIP_EDGE_DEPOSIT_VSYNC:
    case BC250_FLIP_EDGE_DEPOSIT_DRAINED:
    case BC250_FLIP_EDGE_DEPOSIT_UNPROVEN:
        // A safe bridge does not implement the requested flip. Do not promise
        // immediate or next-VSync presentation while a long writer owns the pipe.
        // Refusal preserves OS ownership and is ordinary contention, not E5.
        if (device->VidPnFlipEnabled) {
            InterlockedIncrement(&wddm->WriterBusy);
            if (wddm->WriterKind < BC250_WRITER_KINDS)
                InterlockedIncrement(&wddm->WriterBusyByKind[wddm->WriterKind]);
            flip->Result = STATUS_DEVICE_BUSY;
            return TRUE;
        }
        // Gate-closed software reporting keeps its existing Desired route.
        InterlockedExchange64(&wddm->ScanoutObject, (LONG64)(scanoutObject ? scanoutObject->Serial : 0ull));
        WddmFlipPublish(wddm, args, pitch, bytes, planeFormat);
        wddm->PrimaryNeedsRestore = TRUE;
        wddm->WriterDeposit = TRUE;
        wddm->DepositSequence++;
        wddm->DesiredOwed = immediate ? FALSE : TRUE;
        if (changed) InterlockedIncrement(&wddm->Flips);
        if (edge == BC250_FLIP_EDGE_DEPOSIT_UNPROVEN) WddmWuRecord(wddm, args, generation);
        flip->Result = STATUS_SUCCESS;
        return TRUE;
    default:
        break;
    }

    // E: no writer, an even generation. The generation is odd while this transaction runs, so that the vblank DPC's
    // lock-free reads (WddmReadCompletedPrimary) never cross it.
    InterlockedExchange(&wddm->PrimarySequence, (LONG)(generation + 1u));
    if (!device->VidPnFlipEnabled)
    {
        // The flip gate is closed (EnableVidPnFlip): publish only. The software timer reports Desired.
        WddmFlipPublish(wddm, args, pitch, bytes, planeFormat);
        wddm->PrimaryNeedsRestore = FALSE;
        wddm->DesiredOwed = immediate ? FALSE : TRUE;
        if (changed) { InterlockedIncrement(&wddm->Flips); InterlockedExchange64(&wddm->ScanoutObject, 0); }
        flip->Result = STATUS_SUCCESS;
    }
    else if (!changed && !(immediate && DcnFlipPending(device, (ULONGLONG)address)))
    {
        // The address is published already. A next-VSync request waits for the report that carries it; an immediate
        // one is complete by the stricter vblank test (DcnFlipPending: the pending bit clear and EARLIEST_INUSE on it).
        wddm->DesiredOwed = immediate ? FALSE : TRUE;
        flip->Result = STATUS_SUCCESS;
    }
    else
    {
        // E2 and E3. An immediate request for the address already published but still pending after a next-VSync
        // request for it is programmed again with the immediate type (same-address promotion). The teardown record
        // is taken before the plane is programmed, not after: a record published after the write would leave a window
        // in which DestroyAllocation frees a buffer the plane reads, with no way back on a part with no GPU reset
        // (facts M53). A record taken for a buffer the plane has not reached yet only restores the firmware surface
        // early, which the next flip undoes.
        const LONG64 record = (LONG64)(scanoutObject ? scanoutObject->Serial : 0ull);
        const LONG64 previous = InterlockedExchange64(&wddm->ScanoutObject, record);
        LONGLONG programStart;
        BOOLEAN wrote = FALSE;
        ULONG proof = 0, result, outcome = 0;
        ULONGLONG physical = 0;
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        // Lab flavour (WddmWitness): the flip event cleared before the programming and outside the observation's
        // bound, so that an event read after the decision is this flip's.
        BC250_DCN_WITNESS_ARM witnessArm;
        DcnWitnessArm(device, immediate, &witnessArm);
#endif
        programStart = KeQueryPerformanceCounter(NULL).QuadPart;
        status = DcnFlipSourceAddress(device, (ULONGLONG)address, pitch, planeFormat, bytes, immediate, &physical,
                                      &wrote);
        if (wrote && NT_SUCCESS(status) && immediate)
        {
            const LONGLONG programEnd = KeQueryPerformanceCounter(NULL).QuadPart;
            outcome = DcnImmediateObserve(device, (ULONGLONG)address, physical, programStart, programEnd,
                                          &wddm->PrimarySequence, generation + 1u, args->Flags.Value);
            proof = (outcome == BC250_DCN_IMMEDIATE_DONE || outcome == BC250_DCN_IMMEDIATE_DONE_LATE) ? 1u : 0u;
        }
        result = Bc250FlipResult(wrote ? 1u : 0u, NT_SUCCESS(status) ? 1u : 0u, immediate ? 1u : 0u, proof);
        InterlockedIncrement(&wddm->FlipResults[result]);
#if defined(AMDGPU_WDDM_LAB_FLAVOUR)
        // Lab flavour: the witnesses, after the decision and never before it.
        if (wrote && NT_SUCCESS(status) && immediate)
            WddmWitness(device, wddm, &witnessArm, args, physical, generation + 1u, outcome, result);
#endif
        if (result == BC250_FLIP_RESULT_REFUSED)
        {
            // Nothing written: A after the fact (DcnFlipSourceAddress's own checks). The record goes back to the object
            // the previous flip left there, unless a destroy took it away meanwhile.
            InterlockedCompareExchange64(&wddm->ScanoutObject, previous, record);
            if (exclusive && immediate) InterlockedIncrement(&wddm->ExclusiveRefused);
            else if (exclusive) InterlockedIncrement(&wddm->ExclusiveFailed);
            flip->Result = status;
        }
        else if (result == BC250_FLIP_RESULT_FAULT_PROGRAM)
        {
            // The write sequence ran and failed: unknown, never proof that nothing was written. E5; the record keeps
            // the new object, which the plane may now read.
            WddmFlipFault(device, wddm, BC250_FLIP_FAULT_PROGRAM, WddmFlipNode(args));
            flip->Result = STATUS_DEVICE_HARDWARE_ERROR;
        }
        else
        {
            // Programmed. Desired is the request in both cases, because the hardware has it: SUCCESS, or U (E3
            // without P'), which is E5 (section 7.2; facts M903: P' held in the call for every immediate flip
            // measured, the vertical blank included).
            if (!changed) InterlockedIncrement(&wddm->ImmediatePromotions);
            if (planeFormat < BC250_PLANE_FORMATS) InterlockedIncrement(&wddm->ScanoutFlipsByFormat[planeFormat]);
            WddmFlipPublish(wddm, args, pitch, bytes, planeFormat);
            wddm->PrimaryNeedsRestore = FALSE;
            InterlockedExchange(&wddm->PrimaryProgrammedSequence, (LONG)(generation + 2u));
            if (changed) InterlockedIncrement(&wddm->Flips);
            if (changed && scanout)
            {
                // Only a flip the hardware was written with counts as a scan-out flip, and its line gives the address
                // the plane was given, so that the display side of an ETW capture can be matched to this allocation.
                InterlockedIncrement(&wddm->ScanoutFlips);
                flip->LogScanout = TRUE;
                flip->Physical = physical;
                flip->Pitch = pitch;
                flip->Bytes = bytes;
                flip->PlaneFormat = planeFormat;
            }
            if (result == BC250_FLIP_RESULT_UNPROVEN)
            {
                WddmFlipFault(device, wddm, BC250_FLIP_FAULT_UNPROVEN, WddmFlipNode(args));
                flip->Result = STATUS_DEVICE_HARDWARE_ERROR;
            }
            else
            {
                wddm->DesiredOwed = immediate ? FALSE : TRUE;
                flip->Result = STATUS_SUCCESS;
            }
        }
    }
    InterlockedExchange(&wddm->PrimarySequence, (LONG)(generation + 2u));
    return TRUE;
}

// The transaction and its time inside L, the programming, the observation and E5 included: what the high entry spends
// at the device's IRQL (FlipLockedMaxUs, in the summary). Only code inside L writes the maximum.
static BOOLEAN WddmFlipLocked(_In_ PVOID Context)
{
    BC250_WDDM_FLIP* flip = (BC250_WDDM_FLIP*)Context;
    const LONGLONG frequency = flip->Device->DcnQpcFrequency;
    const LONGLONG start = KeQueryPerformanceCounter(NULL).QuadPart;
    LONGLONG us;

    (void)WddmFlipTransaction(Context);
    us = KeQueryPerformanceCounter(NULL).QuadPart - start;
    if (frequency > 0 && us > 0)
    {
        us = us * 1000000 / frequency;
        if (us > 0x7FFFFFFF) us = 0x7FFFFFFF;
        if ((LONG)us > flip->Wddm->FlipLockedMaxUs) flip->Wddm->FlipLockedMaxUs = (LONG)us;
    }
    return TRUE;
}

static DXGKDDI_SETVIDPNSOURCEADDRESS Bc250WddmSetVidPnSourceAddress;
static NTSTATUS Bc250WddmSetVidPnSourceAddress(_In_ const HANDLE hAdapter,
                                               _In_ const DXGKARG_SETVIDPNSOURCEADDRESS* pSetVidPnSourceAddress)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM* wddm = WddmOf(hAdapter);
    // This is the one DDI in the table whose ceiling is above DISPATCH_LEVEL: d3dkmddi.h annotates it
    // _IRQL_requires_max_(PROFILE_LEVEL - 1), and a driver with FlipOnVSyncMmIo gets its flips "at device interrupt
    // request level (DIRQL)" (d3dkmddi.md:12801). Two entries (section 7.2): above DISPATCH_LEVEL dxgmms2 already holds
    // L (facts M904), and the transaction runs directly; at or below it, the same transaction runs inside one
    // DxgkCbSynchronizeExecution section. Log lines and WddmVSyncArm come after the transaction, for the low entry only:
    // GuardLog drops a line above DISPATCH_LEVEL, and WddmVSyncArm takes a spin lock.
    const BOOLEAN high = (KeGetCurrentIrql() > DISPATCH_LEVEL);
    BC250_WDDM_FLIP flip;

    if (pSetVidPnSourceAddress->VidPnSourceId != 0) return STATUS_INVALID_PARAMETER;
    if (wddm == NULL) return STATUS_DEVICE_NOT_READY;
    RtlZeroMemory(&flip, sizeof(flip));
    flip.Device = device;
    flip.Wddm = wddm;
    flip.Args = pSetVidPnSourceAddress;
    if (high)
    {
        (void)WddmFirstCalls(wddm, WddmDdiSetVidPnSourceAddress);  // the count only: FALSE above DISPATCH_LEVEL
        InterlockedIncrement(&wddm->FlipsAboveDispatch);
        (void)WddmFlipLocked(&flip);
        return flip.Result;
    }
    {
        // A failed section ran nothing: nothing is published or programmed, and the request fails like a refusal.
        const NTSTATUS section = WddmSection(device, WddmFlipLocked, &flip);
        if (!NT_SUCCESS(section)) { InterlockedIncrement(&wddm->LowSyncFailed); return section; }
    }
    // The refusal line is deliberately left as it is: its worst case is already baselined over the guard-log
    // line's width (BD-070), and the flags of a refused flip are in the summary. BD-170 folded the logging of
    // this DDI into the flip state machine and left this sentence behind with the block it explained.
    if (flip.LogRefusal && InterlockedIncrement(&wddm->ScanoutNotes)<=BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: scan-out refused: %s (%lux%lu pitch %lu format %lu segment %lu address 0x%llX)",
                 Bc250ScanoutStatusText(flip.Admit),flip.Candidate.Width,flip.Candidate.Height,flip.Candidate.Pitch,
                 flip.Candidate.Format,flip.Candidate.Segment,flip.Candidate.Address);
    // The raw Flags word rides along on the scan-out flip line: it is the one place a reader can see which bits the
    // OS set on a flip this driver programmed, without trusting the shifted trailing comments in d3dkmddi.h.
    if (flip.LogScanout && InterlockedIncrement(&wddm->ScanoutNotes)<=BC250_WDDM_LOG_CALLS)
        GuardLog("wddm: scan-out flip: card 0x%llX physical 0x%llX pitch %lu bytes %llu flags 0x%08X fmt %lu",
                 (ULONGLONG)pSetVidPnSourceAddress->PrimaryAddress.QuadPart,flip.Physical,flip.Pitch,flip.Bytes,
                 pSetVidPnSourceAddress->Flags.Value,flip.PlaneFormat);
    if (WddmFirstCalls(wddm, WddmDdiSetVidPnSourceAddress))
        GuardLog("wddm: SetVidPnSourceAddress segment %u address 0x%llX flags 0x%08X (firmware framebuffer 0x%llX) -> 0x%08X",
                 pSetVidPnSourceAddress->PrimarySegment, (ULONGLONG)pSetVidPnSourceAddress->PrimaryAddress.QuadPart,
                 pSetVidPnSourceAddress->Flags.Value, (ULONGLONG)device->Post.PhysicAddress.QuadPart,
                 (ULONG)flip.Result);
    WddmVSyncArm(device, TRUE);         // a flip has been queued: something must retire it
    return flip.Result;
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
            WddmVSyncControlNote(wddm, EnableInterrupt);
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
    UINT height = device->RuntimePostValid?device->RuntimePost.Height:device->Post.Height;

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
// At interface 0x5023 this said that nothing after the WDDM 2.0 block exists. ADR 0019 B1 compiles the 2.9 table,
// so the WDDM 2.1-2.9 members do exist now and are NULLs we defend instead: the 2.0 block still ends where it did
// (SetVideoProtectedRegion is its last member), the structure ends at the 2.9 block's last member
// (SetInterruptTargetPresentId, so nothing of 3.x is compiled in), and WddmCheckReserved checks at run time that
// the whole tail between the two is NULL. A stage that fills a member of that tail moves BC250_WDDM_TABLE_TAIL_START or adds an exception there.
C_ASSERT(FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSetVideoProtectedRegion) + sizeof(PVOID) == 832);
C_ASSERT(sizeof(DRIVER_INITIALIZATION_DATA) ==
         FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSetInterruptTargetPresentId) + sizeof(PVOID));
#define BC250_WDDM_TABLE_TAIL_START \
    (FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiSetVideoProtectedRegion) + sizeof(PVOID))
// GetChildContainerId is a WDDM 1.2 member and this table fills it (step 4 of DP audio), so it has to sit before
// that tail, or WddmCheckReserved would report it as a member of a version this driver does not declare.
C_ASSERT(FIELD_OFFSET(DRIVER_INITIALIZATION_DATA, DxgkDdiGetChildContainerId) < BC250_WDDM_TABLE_TAIL_START);
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

    SIZE_T offset;

    for (i = 0; i < RTL_NUMBER_OF(reserved); i++)
        if (*(PVOID* const*)((const UCHAR*)Data + reserved[i].Offset) != NULL)
            GuardLog("wddm: table member %s must be zero and is not", reserved[i].Name);
    // ADR 0019 B1: every WDDM 2.1-3.1 member stays NULL while WDDMVersion is 2.0 (see the asserts above).
    for (offset = BC250_WDDM_TABLE_TAIL_START; offset < sizeof(*Data); offset += sizeof(PVOID))
        if (*(PVOID const*)((const UCHAR*)Data + offset) != NULL)
            GuardLog("wddm: table member at offset %u (after the WDDM 2.0 block) must be NULL and is not", (ULONG)offset);
}

// ---- the display side, traced -----------------------------------------------------------------------------------

// E16 run 005: the full adapter starts and then nothing on the display side happens, and the display DDIs this
// table shares with the display-only one were silent. These wrappers change no answer; they count each call and
// log the first few with the status, so that the next kept log says how far dxgkrnl's display core got. The
// counters are file-scope because the child DDIs may run before BC250_WDDM exists. All of these DDIs are
// PASSIVE_LEVEL ones; the IRQL test only guards the log's spin lock against an annotation being wrong.
typedef enum _BC250_WDDM_TRACED {
    TracedQueryChildRelations = 0, TracedQueryChildStatus, TracedQueryDeviceDescriptor, TracedGetChildContainerId,
    TracedIsSupportedVidPn,
    TracedRecommendFunctionalVidPn, TracedEnumVidPnCofuncModality, TracedSetVidPnSourceVisibility, TracedCommitVidPn,
    TracedUpdateActiveVidPnPresentPath, TracedRecommendMonitorModes, TracedQueryVidPnHWCapability, TracedCount
} BC250_WDDM_TRACED;

static volatile LONG g_TracedCalls[TracedCount];

static NTSTATUS WddmTraced(BC250_WDDM_TRACED Slot, _In_z_ const char* Name, NTSTATUS Status, ULONG Detail)
{
    LONG calls = InterlockedIncrement(&g_TracedCalls[Slot]);
    // STATUS_MONITOR_NO_DESCRIPTOR is the designed answer of GetChildContainerId (step 4 of DP audio): it keeps the
    // container ID the operating system offers. NT_SUCCESS is false for it, so without this line the designed answer
    // of every call would be logged as a failure for the first 64 calls (b26 review finding F2). The exemption is
    // that one slot's, because ModesetQueryDescriptor answers the same status when no EDID was served, and there it
    // is a failure a kept log must still count (modeset.c, DescriptorServed).
    BOOLEAN failed = !NT_SUCCESS(Status) &&
                     !(Slot == TracedGetChildContainerId && Status == STATUS_MONITOR_NO_DESCRIPTOR);

    if (KeGetCurrentIrql() <= DISPATCH_LEVEL && (calls <= 6 || (failed && calls <= 64)))
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

static DXGKDDI_GET_CHILD_CONTAINER_ID Bc250WddmGetChildContainerId;
static NTSTATUS Bc250WddmGetChildContainerId(_In_ const PVOID Context, _In_ ULONG ChildUid,
                                             _Inout_ PDXGK_CHILD_CONTAINER_ID ContainerId)
{
    return WddmTraced(TracedGetChildContainerId, "GetChildContainerId",
                      Bc250GetChildContainerId(Context, ChildUid, ContainerId), ChildUid);
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
    // ADR 0019 B1: the table version is the compiled interface version (bc250kmd.h says why 2.9); the WDDM
    // feature level is DXGK_DRIVERCAPS.WDDMVersion, which stays 2.0 until stage B4.
    Data->Version = DXGKDDI_INTERFACE_VERSION_WDDM2_9;

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
    // WDDM 1.2, inside the WDDM 2.0 block this table fills (the assert near WddmCheckReserved says so). It keeps
    // dxgkrnl's default container ID and takes the child's port ID for the ELD of the DP audio endpoint
    // (pnp.c Bc250GetChildContainerId, docs/design/dp-audio.md step 4).
    Data->DxgkDdiGetChildContainerId = Bc250WddmGetChildContainerId;
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
    // HLK gap G1. The initialization page names DxgkDdiRenderKm, which is the physical-addressing
    // entry point; DXGKDDI_RENDERGDI is the one "used when submitting ... GDI commands for contexts
    // that support virtual addressing", and every context of this adapter has virtual addressing.
    // It is in the table whatever the capability answer is, and reports nothing while the
    // capability bit is clear, because dxgkrnl calls it only for a driver that set the bit.
    Data->DxgkDdiRenderGdi = Bc250WddmRenderGdi;
    Data->DxgkDdiSetVidPnSourceAddress = Bc250WddmSetVidPnSourceAddress;
    // The other half of the flip contract. Claiming FlipOnVSyncMmIo without these two leaves dxgkrnl no way to
    // switch the VSync on and no way to ask where the beam is, and a queued flip is then never retired.
    Data->DxgkDdiControlInterrupt = Bc250WddmControlInterrupt;
    Data->DxgkDdiGetScanLine = Bc250WddmGetScanLine;

    // Left NULL on purpose: Patch and Render (physical addressing only), QueryCurrentFence, RecommendVidPnTopology,
    // StopCapture, every overlay and multi-plane member, LinkDevice, SetDisplayPrivateDriverFormat, RenderKm
    // (the physical-addressing GDI entry point; RenderGdi above is its virtual-addressing form, HLK gap G1),
    // ControlInterrupt2 (WDDM 2.1+), CancelCommand (CancelCommandAware = 0), MapCpuHostAperture and
    // UnmapCpuHostAperture (no host aperture), SetVideoProtectedRegion, and everything the 2.1-3.1 headers append
    // after SetStablePowerState: the hardware-queue and hardware-scheduling DDIs (no HWS), the native-fence and
    // doorbell DDIs (no native fence, no user-mode submission), the flip-queue DDIs, SetTargetGamma and
    // SetTargetAdjustedColorimetry (not before stage B4), and the diagnostic DDIs, which dxgkrnl requires in
    // pairs (QueryDiagnosticTypesSupport with ControlDiagnosticReporting, from WDDMVersion 2.4). The interface
    // version alone obliges none of them; WDDMVersion stays 2.0 (ADR 0019 B1). QueryInterface, ControlEtwLogging,
    // NotifyAcpiEvent, SetPalette, NotifySurpriseRemoval, SetPowerComponentFState,
    // PowerRuntimeControlRequest and PowerRuntimeSetDeviceHandle stay NULL exactly as they are in the
    // display-only table today. ControlInterrupt, GetScanLine and GetChildContainerId are **not** in this list
    // any more: the flip path above sets the first two, and step 4 of DP audio sets the third. Nor are the per-engine TDR set, CollectDbgInfo and SetStablePowerState (0.7.4).
    WddmCheckReserved(Data);
}
