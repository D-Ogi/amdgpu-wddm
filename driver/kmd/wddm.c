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
// PreemptionAware at DMA-buffer-boundary granularity, no ComputeOnly, no per-engine TDR, no swizzling ranges.
#include "bc250kmd.h"
#include <ntstrsafe.h>

#define BC250_WDDM_TAG 'wW2B'
#define BC250_WDDM_LOG_CALLS 8              // how many first calls of each DDI reach the guard log

// Segment ids are one-based: DXGK_QUERYSEGMENTOUT4.PagingBufferSegmentId is "the index (starting from 1)".
#define BC250_WDDM_SEGMENT_VRAM 1u
#define BC250_WDDM_SEGMENT_SET(id) (1u << ((id) - 1))
#define BC250_WDDM_NODE_3D 0u
#define BC250_WDDM_NODE_COUNT 1u            // what DXGK_DRIVERCAPS.GpuEngineTopology reports; see the assert below

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

// The paging buffer dxgkrnl allocates for us. Stage A writes nothing into it; the size only has to be plausible.
#define BC250_WDDM_PAGING_BUFFER_BYTES 0x10000ul

// One object kind per magic, so that a handle that is not ours is caught before it is dereferenced.
#define BC250_WDDM_MAGIC_DEVICE     'vD7M'
#define BC250_WDDM_MAGIC_CONTEXT    'xC7M'
#define BC250_WDDM_MAGIC_PROCESS    'cP7M'
#define BC250_WDDM_MAGIC_ALLOCATION 'lA7M'

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
    "ResetFromTimeout", "RestartFromTimeout", "CalibrateGpuClock", "FormatHistoryBuffer", "Present",
    "SetVidPnSourceAddress", "ControlInterrupt", "GetScanLine",
};
C_ASSERT(RTL_NUMBER_OF(g_DdiNames) == WddmDdiCount);

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
    BC250_WDDM_ALLOCATION_PRIVATE Allocation;
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
    // is enough and the order is preserved by construction - but only because there is exactly one node. The
    // assert below binds that: a second node needs a fence pair per node, not one shared pair.
    volatile LONG SubmittedFence;
    volatile LONG SubmittedNode;
    volatile LONG CompletionPending;    // set by the submit, cleared by the DPC
    volatile LONG LastCompletedFence;   // "the driver must always maintain the last completed fence ID value"

    // Preemption goes through the same DPC, and after the completion, so that the fence it reports as last
    // completed is the one the completion just published rather than the one before it.
    volatile LONG PreemptionFence;
    volatile LONG PreemptionNode;
    volatile LONG PreemptionPending;
    KDPC ReportDpc;

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
    volatile LONG Flips;                        // SetVidPnSourceAddress calls that changed the scanout address
    volatile LONG FlipsAboveDispatch;           // of all SetVidPnSourceAddress calls, those that arrived at DIRQL
} BC250_WDDM;

// One shared fence pair is only correct while there is one node. GpuEngineTopology below reports this same
// constant, so raising the node count breaks the build here instead of silently reporting one node's fence
// against another node's packets.
C_ASSERT(BC250_WDDM_NODE_COUNT == 1);

static BOOLEAN g_FullWddm;              // the gate, read once in DriverEntry

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
    g_FullWddm = (GuardReadSetting(L"EnableFullWddm", 0) == 1);
    GuardLog("gate: EnableFullWddm %u", g_FullWddm ? 1u : 0u);
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

// The submit side. It records and queues; it must not report, because a DDI that calls back into dxgkrnl from
// inside the submit path re-enters the scheduler with the submit still on the stack.
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

    if (wddm == NULL) return;
    InterlockedExchange(&wddm->SubmittedNode, (LONG)NodeOrdinal);
    InterlockedExchange(&wddm->SubmittedFence, (LONG)FenceId);
    InterlockedExchange(&wddm->CompletionPending, 1);
    WddmQueueReport(wddm);
}

// Same for a preemption. The DDI may not report inline either, and the fence it wants to name as last completed
// is only right once any pending completion has been published - which is why both go through one DPC.
static void WddmPreemptFence(_Inout_ BC250_DEVICE* Device, UINT FenceId, UINT NodeOrdinal)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;

    if (wddm == NULL) return;
    InterlockedExchange(&wddm->PreemptionNode, (LONG)NodeOrdinal);
    InterlockedExchange(&wddm->PreemptionFence, (LONG)FenceId);
    InterlockedExchange(&wddm->PreemptionPending, 1);
    WddmQueueReport(wddm);
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

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (device == NULL || (wddm = (BC250_WDDM*)device->Wddm) == NULL) return;
    if (WddmStopping(wddm)) return;

    if (InterlockedExchange(&wddm->CompletionPending, 0) != 0)
    {
        fence = InterlockedCompareExchange(&wddm->SubmittedFence, 0, 0);
        InterlockedExchange(&wddm->LastCompletedFence, fence);
        RtlZeroMemory(&data, sizeof(data));
        data.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
        data.DmaCompleted.SubmissionFenceId = (UINT)fence;
        data.DmaCompleted.NodeOrdinal = (UINT)InterlockedCompareExchange(&wddm->SubmittedNode, 0, 0);
        data.DmaCompleted.EngineOrdinal = 0;    // "for adapters that are not part of a link, always 0"
        WddmReport(device, &data);
    }

    if (InterlockedExchange(&wddm->PreemptionPending, 0) != 0)
    {
        RtlZeroMemory(&data, sizeof(data));
        data.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
        data.DmaPreempted.PreemptionFenceId = (UINT)InterlockedCompareExchange(&wddm->PreemptionFence, 0, 0);
        data.DmaPreempted.LastCompletedFenceId = (UINT)InterlockedCompareExchange(&wddm->LastCompletedFence, 0, 0);
        data.DmaPreempted.NodeOrdinal = (UINT)InterlockedCompareExchange(&wddm->PreemptionNode, 0, 0);
        data.DmaPreempted.EngineOrdinal = 0;
        WddmReport(device, &data);
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

// On while a source is visible, off otherwise. No hardware is touched either way.
//
// The decision and the act are one critical section, and that is the whole point of this function. KeSetTimerEx
// and KeCancelTimer are both legal at DISPATCH_LEVEL, which is where this lock puts us, so there is no reason to
// split them - and splitting them was wrong: SetVidPnSourceVisibility(FALSE) racing the arm that every
// SetVidPnSourceAddress does could interleave so that the cancel landed last while VSyncArmed still read TRUE.
// Nothing would ever re-arm after that, no flip would ever be retired again, and nothing would say so.
static void WddmVSyncArm(_Inout_ BC250_DEVICE* Device, BOOLEAN On)
{
    BC250_WDDM* wddm = (BC250_WDDM*)Device->Wddm;
    LARGE_INTEGER due;
    BOOLEAN changed = FALSE;
    KIRQL irql;

    if (wddm == NULL) return;
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
    GuardLog("wddm summary: submissions %ld physical + %ld virtual, %ld preemptions, last completed fence %ld",
             Wddm->Calls[WddmDdiSubmitCommand], Wddm->Calls[WddmDdiSubmitCommandVirtual],
             Wddm->Calls[WddmDdiPreemptCommand], Wddm->LastCompletedFence);
    GuardLog("wddm summary: presents %ld, flips %ld of %ld address calls (%ld arrived above DISPATCH_LEVEL)",
             Wddm->Calls[WddmDdiPresent], Wddm->Flips, Wddm->Calls[WddmDdiSetVidPnSourceAddress],
             Wddm->FlipsAboveDispatch);
    GuardLog("wddm summary: vsync %s, %ld ticks, %ld reported to dxgkrnl",
             Wddm->VSyncEnabled ? "enabled" : "not enabled by ControlInterrupt", Wddm->VSyncTicks,
             Wddm->VSyncReports);
    if (Wddm->ReportFailures != 0)
        GuardLog("wddm summary: *** %ld reports refused by DxgkCbSynchronizeExecution ***", Wddm->ReportFailures);

    // The one result that changes what stage A means: the scheduler declaring this adapter hung. Loud, and
    // repeated here even though every call was logged where it happened, because the summary may be all anybody
    // reads.
    if (Wddm->Calls[WddmDdiResetFromTimeout] != 0 || Wddm->Calls[WddmDdiRestartFromTimeout] != 0)
        GuardLog("wddm summary: *** TDR: ResetFromTimeout %ld, RestartFromTimeout %ld - the scheduler timed this "
                 "adapter out ***", Wddm->Calls[WddmDdiResetFromTimeout], Wddm->Calls[WddmDdiRestartFromTimeout]);
    else
        GuardLog("wddm summary: no TDR (ResetFromTimeout and RestartFromTimeout were never called)");
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
}

// ---- start, stop, DPC ------------------------------------------------------------------------------------------

void WddmStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_WDDM* wddm;

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

    // Order matters, and this is the order:
    //
    //  1. Stopping under the lock. From here nothing of ours arms a timer, queues a DPC or joins the object
    //     list, so the cancel and the flush below are final rather than a race they might lose.
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
    KeReleaseSpinLock(&wddm->Lock, irql);

    KeCancelTimer(&wddm->VSyncTimer);   // again, unconditionally: cheap, and it cannot be armed any more
    KeRemoveQueueDpc(&wddm->VSyncDpc);
    KeRemoveQueueDpc(&wddm->ReportDpc);
    KeFlushQueuedDpcs();
    Device->Wddm = NULL;                // from here no DDI and no DPC of ours can find the state

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
        // PreservedDuringStandby and PreservedDuringHibernate stay 0: whether this memory survives S3 on this
        // board is not known, and claiming it wrongly would hand back corrupt surfaces after a resume.
        descriptor->BaseAddress.QuadPart = (LONGLONG)(Device->VramMcBase + offset);
        descriptor->CpuTranslatedAddress.QuadPart = Device->VramPhysical.QuadPart + (LONGLONG)offset;
        descriptor->Size = (SIZE_T)length;
        // CommitLimit is left 0: the header says it applies to aperture segments only, and this is a memory
        // segment. Everything else in the descriptor - the VPR range, the invalid ranges - is zero for the same
        // reason: stage A has none of it.
    }
    out->NbSegment = count;
    out->PagingBufferSegmentId = count != 0 ? BC250_WDDM_SEGMENT_VRAM : 0;
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

    if (Query->OutputDataSize < sizeof(*caps) || caps == NULL) return STATUS_BUFFER_TOO_SMALL;
    // The caller's buffer, not our sizeof: this driver is compiled at WDDM 2.0 and will run under a dxgkrnl that
    // knows a longer DXGK_DRIVERCAPS. Zeroing all of it means the members we have never heard of read as zero
    // rather than as whatever was on the stack. The same rule applies everywhere below.
    RtlZeroMemory(caps, Query->OutputDataSize);

    caps->WDDMVersion = DXGKDDI_WDDMv2;                 // ADR 0008 point 2: the version that introduced GpuMmu
    caps->HighestAcceptableAddress.QuadPart = -1;       // no addressing limit; too low a value fails the load
    caps->SupportNonVGA = TRUE;
    caps->NumberOfSwizzlingRanges = 0;                  // swizzling range support has been removed from WDDM
    caps->SupportPerEngineTDR = FALSE;                  // so the three per-engine DDIs may stay NULL
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
    caps->PresentationCaps.NoCacheCoherentApertureMemory = 1;   // there is no aperture segment at all
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
    caps->MemoryManagementCaps.PagingNode = BC250_WDDM_NODE_3D;

    // One node, and paging rides on it: that sidesteps PHYSICALADAPTERCAPS and halves the fence plumbing. The
    // shared fence pair in BC250_WDDM is only correct at this value, and a C_ASSERT there binds the two.
    caps->GpuEngineTopology.NbAsymetricProcessingNodes = BC250_WDDM_NODE_COUNT;

    // Both zero means "use the entire available VA range", which is what we want while nothing reserves any of it.
    caps->InternalGpuVirtualAddressRangeStart = 0;
    caps->InternalGpuVirtualAddressRangeEnd = 0;

    // The flip model of a full miniport: DxgkDdiPresent produces no DMA and SetVidPnSourceAddress does the flip.
    caps->FlipCaps.FlipOnVSyncMmIo = 1;
    caps->MaxQueuedFlipOnVSync = 1;

    // No hardware pointer (MaxPointerWidth/Height stay 0): dxgkrnl draws the cursor into the image it presents,
    // exactly as in the display-only build.

    // What was promised, and into how large a structure: the size says which DXGK_DRIVERCAPS this dxgkrnl thinks
    // it is talking to. A cap that is wrong but accepted leaves no other trace (E16 run 1). Worst case 129 of
    // the 160 bytes of a log line: count before adding a field.
    if (WddmAnswersLogged(Device))
        GuardLog("wddm: DRIVERCAPS %u of %u bytes: wddm %u sched 0x%X mm 0x%X paging node %u flip 0x%X slots %u",
             (ULONG)sizeof(*caps), Query->OutputDataSize, (ULONG)caps->WDDMVersion, caps->SchedulingCaps.Value,
             caps->MemoryManagementCaps.Value, caps->MemoryManagementCaps.PagingNode, caps->FlipCaps.Value,
             caps->MaxAllocationListSlotId);
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
    default:
        // Including UMDRIVERPRIVATE and the pre-WDDM2 segment queries, which a WDDM 2 driver must not answer.
        // STATUS_NOT_SUPPORTED is safe for an unhandled type, and which types are asked for is stage A evidence.
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

    // "Must return STATUS_INVALID_PARAMETER for NodeOrdinal >= GetNumNodes(), and all calls for in-range ordinals
    // must be successful." GetNumNodes is what DRIVERCAPS reported, so the bound is the same constant.
    if (node >= BC250_WDDM_NODE_COUNT || adapter != 0) return STATUS_INVALID_PARAMETER;

    // At WDDM 2.0 the member after FriendlyName is a reserved UINT32, not DXGK_NODEMETADATA_FLAGS (that arrives at
    // WDDM 2.2); zeroing covers it either way.
    RtlZeroMemory(pGetNodeMetadata, sizeof(*pGetNodeMetadata));
    pGetNodeMetadata->EngineType = DXGK_ENGINE_TYPE_3D;         // there is no compute engine type: compute rides on 3D
    RtlStringCchCopyW(pGetNodeMetadata->FriendlyName, DXGK_MAX_METADATA_NAME_LENGTH, L"BC-250 GFX");
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
    BC250_WDDM_OBJECT* object;

    if (parent == NULL) return STATUS_INVALID_PARAMETER;
    if (pCreateContext->NodeOrdinal != BC250_WDDM_NODE_3D) return STATUS_INVALID_PARAMETER;
    object = WddmNewObject(parent->Device, BC250_WDDM_MAGIC_CONTEXT);
    if (object == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    object->NodeOrdinal = pCreateContext->NodeOrdinal;
    pCreateContext->hContext = object;

    RtlZeroMemory(&pCreateContext->ContextInfo, sizeof(pCreateContext->ContextInfo));
    // Stage A submits nothing, so the DMA buffer it asks for is the smallest that is still a buffer, and there is
    // no allocation or patch-location list to keep: with virtual addressing there is no patching to do at all.
    pCreateContext->ContextInfo.DmaBufferSize = PAGE_SIZE;
    pCreateContext->ContextInfo.DmaBufferSegmentSet = 0;            // system memory, not one of our segments
    pCreateContext->ContextInfo.DmaBufferPrivateDataSize = 0;
    pCreateContext->ContextInfo.AllocationListSize = 0;
    pCreateContext->ContextInfo.PatchLocationListSize = 0;
    pCreateContext->ContextInfo.Caps.NoPatchingRequired = pCreateContext->Flags.VirtualAddressing ? 1u : 0u;
    pCreateContext->ContextInfo.PagingCompanionNodeId = BC250_WDDM_NODE_3D;

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
        info->PitchAlignedSize = 0;                     // no pitch-aligned aperture segment exists
        info->PreferredSegment.Value = 0;
        info->PreferredSegment.SegmentId0 = BC250_WDDM_SEGMENT_VRAM;
        info->SupportedReadSegmentSet = BC250_WDDM_SEGMENT_SET(BC250_WDDM_SEGMENT_VRAM);
        info->SupportedWriteSegmentSet = BC250_WDDM_SEGMENT_SET(BC250_WDDM_SEGMENT_VRAM);
        info->EvictionSegmentSet = 0;                   // nothing to evict to: there is no aperture segment
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
    // reported as not specified, exactly as display.c reports it in the VidPN.
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
    // Stage A keeps no per-device state for an allocation, so the device-specific handle is NULL and there is
    // nothing for CloseAllocation to give back. Note that hAllocation here is a D3DKMT_HANDLE, not the kernel
    // handle CreateAllocation returned, so it cannot be mapped back to our object from here anyway.
    for (i = 0; i < pOpenAllocation->NumAllocations; i++)
        pOpenAllocation->pOpenAllocation[i].hDeviceSpecificAllocation = NULL;
    if (WddmFirstCalls((BC250_WDDM*)parent->Device->Wddm, WddmDdiOpenAllocation))
        GuardLog("wddm: OpenAllocation %u allocations flags 0x%08X", pOpenAllocation->NumAllocations,
                 pOpenAllocation->Flags.Value);
    return STATUS_SUCCESS;
}

static DXGKDDI_CLOSEALLOCATION Bc250WddmCloseAllocation;
static NTSTATUS Bc250WddmCloseAllocation(_In_ const HANDLE hDevice, _In_ const DXGKARG_CLOSEALLOCATION* pCloseAllocation)
{
    BC250_WDDM_OBJECT* parent = WddmObject(hDevice, BC250_WDDM_MAGIC_DEVICE);

    if (parent == NULL) return STATUS_INVALID_PARAMETER;
    if (WddmFirstCalls((BC250_WDDM*)parent->Device->Wddm, WddmDdiCloseAllocation))
        GuardLog("wddm: CloseAllocation %u allocations", pCloseAllocation->NumAllocations);
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
    if (WddmFirstCalls(wddm, WddmDdiBuildPagingBuffer))
        GuardLog("wddm: BuildPagingBuffer operation %u, %u bytes free, pass offset %u",
                 (ULONG)pBuildPagingBuffer->Operation, pBuildPagingBuffer->DmaSize, pBuildPagingBuffer->MultipassOffset);
    return STATUS_SUCCESS;
}

static DXGKDDI_SUBMITCOMMAND Bc250WddmSubmitCommand;
static NTSTATUS Bc250WddmSubmitCommand(_In_ const HANDLE hAdapter, _In_ const DXGKARG_SUBMITCOMMAND* pSubmitCommand)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = (context != NULL) ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;

    // This is also the path paging buffers take: hContext is NULL there and NodeOrdinal names the node. A failure
    // return bugchecks (0x119, parameter 1 = 0x2), so there is none.
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiSubmitCommand))
        GuardLog("wddm: SubmitCommand fence %u node %u flags 0x%08X segment %u size %u", pSubmitCommand->SubmissionFenceId,
                 node, pSubmitCommand->Flags.Value, pSubmitCommand->DmaBufferSegmentId, pSubmitCommand->DmaBufferSize);
    WddmCompleteFence(device, pSubmitCommand->SubmissionFenceId, node);
    return STATUS_SUCCESS;
}

static DXGKDDI_SUBMITCOMMANDVIRTUAL Bc250WddmSubmitCommandVirtual;
static NTSTATUS Bc250WddmSubmitCommandVirtual(_In_ const HANDLE hAdapter,
                                              _In_ const DXGKARG_SUBMITCOMMANDVIRTUAL* pSubmitCommand)
{
    BC250_DEVICE* device = (BC250_DEVICE*)hAdapter;
    BC250_WDDM_OBJECT* context = WddmObject(pSubmitCommand->hContext, BC250_WDDM_MAGIC_CONTEXT);
    UINT node = (context != NULL) ? context->NodeOrdinal : pSubmitCommand->NodeOrdinal;

    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiSubmitCommandVirtual))
        GuardLog("wddm: SubmitCommandVirtual fence %u node %u flags 0x%08X va 0x%llX size %u",
                 pSubmitCommand->SubmissionFenceId, node, pSubmitCommand->Flags.Value,
                 (ULONGLONG)pSubmitCommand->DmaBufferVirtualAddress, pSubmitCommand->DmaBufferSize);
    WddmCompleteFence(device, pSubmitCommand->SubmissionFenceId, node);
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
    // A failure return bugchecks. Stage A has nothing to reset: it never started an engine, and its hard rule is
    // that no DDI of this file touches a register. Halting the CP the way the undo path does (ADR 0008 point 7,
    // facts M44) belongs to the stage that first submits something.
    //
    // These two are the only DDIs in the file that log on every call rather than the first few: a TDR means the
    // scheduler has decided this adapter is hung, which changes what the whole run means, and a run where it
    // happens a hundred times is a different result from one where it happens once.
    (void)WddmFirstCalls(WddmOf(hAdapter), WddmDdiResetFromTimeout);
    GuardLog("wddm: *** ResetFromTimeout: the scheduler timed this adapter out (nothing was running) ***");
    return STATUS_SUCCESS;
}

static DXGKDDI_RESTARTFROMTIMEOUT Bc250WddmRestartFromTimeout;
static NTSTATUS Bc250WddmRestartFromTimeout(_In_ const HANDLE hAdapter)
{
    (void)WddmFirstCalls(WddmOf(hAdapter), WddmDdiRestartFromTimeout);
    GuardLog("wddm: *** RestartFromTimeout: the adapter is being restarted after a timeout ***");
    return STATUS_SUCCESS;       // "can simply return STATUS_SUCCESS immediately"
}

// ---- clock and history buffer -------------------------------------------------------------------------------------

static DXGKDDI_CALIBRATEGPUCLOCK Bc250WddmCalibrateGpuClock;
static NTSTATUS Bc250WddmCalibrateGpuClock(_In_ const HANDLE hAdapter, _In_ UINT32 NodeOrdinal, _In_ UINT32 EngineOrdinal,
                                           _Out_ DXGKARG_CALIBRATEGPUCLOCK* pClockCalibration)
{
    LARGE_INTEGER frequency;
    LARGE_INTEGER counter = KeQueryPerformanceCounter(&frequency);

    UNREFERENCED_PARAMETER(EngineOrdinal);
    if (NodeOrdinal != BC250_WDDM_NODE_3D) return STATUS_INVALID_PARAMETER;
    // Stage A's engine is the CPU, so its clock is the CPU's: one counter, read once, reported as both. A GPU
    // timestamp of our own arrives with the first real submission.
    RtlZeroMemory(pClockCalibration, sizeof(*pClockCalibration));
    pClockCalibration->GpuFrequency = (ULONGLONG)frequency.QuadPart;
    pClockCalibration->GpuClockCounter = (ULONGLONG)counter.QuadPart;
    pClockCalibration->CpuClockCounter = (ULONGLONG)counter.QuadPart;
    if (WddmFirstCalls(WddmOf(hAdapter), WddmDdiCalibrateGpuClock))
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
    // There is exactly one scan-out address on this adapter and the firmware programmed it; changing it needs the
    // display core, which this driver does not touch (ADR 0006 point 2). The request is recorded, the firmware's
    // framebuffer keeps scanning out, and display.c stays the only owner of that mapping. The address is what the
    // next CRTC_VSYNC report carries, which is how dxgkrnl learns that this flip has retired.
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
        if (InterlockedExchange64(&wddm->PrimaryAddress.QuadPart, pSetVidPnSourceAddress->PrimaryAddress.QuadPart) !=
            pSetVidPnSourceAddress->PrimaryAddress.QuadPart)
            InterlockedIncrement(&wddm->Flips);
        wddm->PrimarySegment = pSetVidPnSourceAddress->PrimarySegment;
        if (high) InterlockedIncrement(&wddm->FlipsAboveDispatch);
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

void WddmBuildTable(_Out_ DRIVER_INITIALIZATION_DATA* Data)
{
    RtlZeroMemory(Data, sizeof(*Data));
    Data->Version = DXGKDDI_INTERFACE_VERSION_WDDM2_0;

    // The 28 pointers the display-only table already has, unchanged. PresentDisplayOnly is the one member with no
    // home in this structure; Present plus SetVidPnSourceAddress take its place.
    Data->DxgkDdiAddDevice = Bc250AddDevice;
    Data->DxgkDdiStartDevice = Bc250StartDevice;
    Data->DxgkDdiStopDevice = Bc250StopDevice;
    Data->DxgkDdiRemoveDevice = Bc250RemoveDevice;
    Data->DxgkDdiResetDevice = Bc250ResetDevice;
    Data->DxgkDdiDispatchIoRequest = Bc250DispatchIoRequest;
    Data->DxgkDdiInterruptRoutine = Bc250InterruptRoutine;
    Data->DxgkDdiDpcRoutine = Bc250DpcRoutine;
    Data->DxgkDdiQueryChildRelations = Bc250QueryChildRelations;
    Data->DxgkDdiQueryChildStatus = Bc250QueryChildStatus;
    Data->DxgkDdiQueryDeviceDescriptor = Bc250QueryDeviceDescriptor;
    Data->DxgkDdiSetPowerState = Bc250SetPowerState;
    Data->DxgkDdiUnload = Bc250Unload;
    Data->DxgkDdiStopDeviceAndReleasePostDisplayOwnership = Bc250StopDeviceAndReleasePostDisplayOwnership;
    Data->DxgkDdiSetPointerPosition = Bc250SetPointerPosition;
    Data->DxgkDdiSetPointerShape = Bc250SetPointerShape;
    Data->DxgkDdiEscape = Bc250Escape;
    Data->DxgkDdiIsSupportedVidPn = Bc250IsSupportedVidPn;
    Data->DxgkDdiRecommendFunctionalVidPn = Bc250RecommendFunctionalVidPn;
    Data->DxgkDdiEnumVidPnCofuncModality = Bc250EnumVidPnCofuncModality;
    Data->DxgkDdiSetVidPnSourceVisibility = Bc250SetVidPnSourceVisibility;
    Data->DxgkDdiCommitVidPn = Bc250CommitVidPn;
    Data->DxgkDdiUpdateActiveVidPnPresentPath = Bc250UpdateActiveVidPnPresentPath;
    Data->DxgkDdiRecommendMonitorModes = Bc250RecommendMonitorModes;
    Data->DxgkDdiQueryVidPnHWCapability = Bc250QueryVidPnHWCapability;
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
    // RenderGdi, ControlInterrupt2 (WDDM 2.1+, and it does not exist at 2.0), the three per-engine TDR DDIs
    // (SupportPerEngineTDR = 0), CancelCommand (CancelCommandAware = 0), MapCpuHostAperture and
    // UnmapCpuHostAperture (no host aperture), SetStablePowerState, SetVideoProtectedRegion, and the
    // hardware-scheduling DDIs, which do not exist at all at WDDM 2.0. QueryInterface, ControlEtwLogging,
    // NotifyAcpiEvent, CollectDbgInfo, SetPalette, NotifySurpriseRemoval, GetChildContainerId,
    // SetPowerComponentFState, PowerRuntimeControlRequest and PowerRuntimeSetDeviceHandle stay NULL exactly as
    // they are in the display-only table today. ControlInterrupt and GetScanLine are **not** in this list any
    // more: the flip path above sets both.
    WddmCheckReserved(Data);
}
