// Interrupts (milestone M6, ADR 0007): the IH ring of the GPU's interrupt controller and the miniport's interrupt and
// DPC routines.
//
// What amdgpu does on this part (navi10_ih.c, E03 trace 0.252832 to 0.252845 s): a ring buffer in GTT memory that the
// interrupt controller writes 32-byte vectors into, a write-back slot for its write pointer, a doorbell for the read
// pointer. Sources (CP end-of-pipe, faults, SDMA traps) are enabled elsewhere (gfx.c's interrupt stage); with the ring
// disabled their enable bits do nothing. The sequence is driver/shim/bc250_ih.c, replayed on the host against that trace.
//
//   <service key>\Parameters
//     EnableIh    REG_DWORD  1 = allow the IH command. Needs EnableGfx (the ring is GTT memory of gpumem.c). Default 0.
//
// One escape (BC250_ESCAPE_RUN_IH), four operations:
//   PLAN    ring set up in memory, navi10_ih_irq_init() against the real registers without executing a write, torn down
//   INIT    needs the GART enabled. Ring set up, programmed, enabled; from here on the interrupt routine takes interrupts
//   FINI    ring disabled, DPCs drained, memory given back if the ring reads disabled
//   STATE   the out fields only; answers with the gate closed as well (what Windows assigned, the counts)
// The driver does FINI by itself when the device stops, before everything else.
//
// Three contexts touch this file, and they share as little as possible:
//   the escape (PASSIVE/APC_LEVEL, Device->GartLock held)  sets the ring up and tears it down through the shim, as every
//                                                          other sequence; flips Active last on the way up, first on
//                                                          the way down
//   the interrupt routine (DIRQL)                           counts, asks for the DPC; touches no register
//   the DPC (DISPATCH_LEVEL)                                the only consumer of the ring: reads the write-back slot and
//                                                          the ring (memory), advances the read pointer (doorbell).
//                                                          It has a struct amdgpu_device OF ITS OWN (a copy made after
//                                                          the init, with a sequence of its own as the backend): the
//                                                          escapes' adev->backend belongs to whoever holds GartLock.
//                                                          Its registers: IH_RB_CNTL, IH_RB_RPTR, IH_RB_WPTR, nothing
//                                                          else, and the one doorbell.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "bc250_gmc.h"
#include "bc250_ih.h"

#define BC250_IH_TAG 'hI2B'
#define BC250_IH_RB_ENABLE 0x00000001ul     // IH_RB_CNTL.RB_ENABLE (osssys_5_0_0_sh_mask.h), for the "is it off" read only
#define BC250_IH_RB_OVERFLOW_CLEAR 0x80000000ul     // IH_RB_CNTL.WPTR_OVERFLOW_CLEAR, for the plan's model only
#define BC250_IH_DPC_ROUNDS 4              // amdgpu_ih_process() looks again after publishing the read pointer

typedef struct _BC250_IH_STATS {
    ULONG Rptr;
    ULONG EntryCount, OverflowCount, DecodeErrors;
    ULONG KindCount;
    BC250_ESCAPE_IV_KIND Kinds[BC250_IH_MAX_KINDS];
    ULONG LastNext, LastCount;
    BC250_ESCAPE_IV Last[BC250_IH_MAX_LAST];
} BC250_IH_STATS;

typedef struct _BC250_IH {
    volatile LONG Active;               // the ring is enabled and the DPC may consume it
    volatile LONG OurInterrupts;        // interrupt routine calls taken as ours
    volatile LONG DpcCount;
    volatile LONG InDpc;                // one consumer at a time: nothing documented says the DPC is not re-entered
    volatile LONG DpcAgain;             // a DPC that found the consumer busy asks it for another pass
    BC250_SEQUENCE Sequence;            // the escape's
    BC250_SEQUENCE DpcSequence;         // the DPC's: begun once per INIT, no list of writes, Dpc set
    struct amdgpu_device* DpcAdev;      // the DPC's own device, valid while Active
    BOOLEAN SetUp;                      // bc250_ih_setup has allocated
    BOOLEAN Enabled;                    // navi10_ih_irq_init() ran on the hardware
    ULONG Rptr;                         // the DPC's alone
    // Written by the DPC, copied by the escape, both under StatsLock, which is held over memory only: never across a
    // register access, and never while the caller's buffer is written (that happens from Snapshot, without the lock).
    KSPIN_LOCK StatsLock;
    BC250_IH_STATS Stats;
    BC250_IH_STATS Snapshot;            // the escape's, under GartLock
} BC250_IH;

// ---- the DPC's registers ---------------------------------------------------------------------------------------------

static BOOLEAN DpcMayTouch(ULONG Offset)
{
    return Offset == BC250_REG_OSSSYS_IH_RB_CNTL || Offset == BC250_REG_OSSSYS_IH_RB_RPTR || Offset == BC250_REG_OSSSYS_IH_RB_WPTR;
}

static NTSTATUS DpcRead(_In_ const BC250_DEVICE* Device, ULONG Offset, _Out_ ULONG* Value)
{
    *Value = 0;
    return DpcMayTouch(Offset) ? MmioIhRead(Device, Offset, Value) : STATUS_ACCESS_DENIED;
}

static NTSTATUS DpcWrite(_In_ const BC250_DEVICE* Device, ULONG Offset, ULONG Value)
{
    return DpcMayTouch(Offset) ? MmioIhWrite(Device, Offset, Value) : STATUS_ACCESS_DENIED;
}

// ---- the interrupt routine and the DPC -------------------------------------------------------------------------------

// At DIRQL. With a message interrupt every call is ours; on a line anything may share it, and nothing is ours while the
// ring is off.
BOOLEAN IhInterrupt(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih = (BC250_IH*)Device->Ih;

    if (ih == NULL || ih->Active == 0) return FALSE;
    InterlockedIncrement(&ih->OurInterrupts);
    Device->Dxgk.DxgkCbQueueDpc(Device->Dxgk.DeviceHandle);
    return TRUE;
}

static void Note(_Inout_ BC250_IH_STATS* Ih, _In_ const struct bc250_iv_entry* Entry)
{
    BC250_ESCAPE_IV* last = &Ih->Last[Ih->LastNext];
    ULONG i;

    Ih->EntryCount++;
    for (i = 0; i < Ih->KindCount; i++)
        if (Ih->Kinds[i].ClientId == Entry->client_id && Ih->Kinds[i].SourceId == Entry->src_id) break;
    if (i < BC250_IH_MAX_KINDS)
    {
        if (i == Ih->KindCount) { Ih->Kinds[i].ClientId = Entry->client_id; Ih->Kinds[i].SourceId = Entry->src_id; Ih->Kinds[i].Count = 0; Ih->KindCount++; }
        Ih->Kinds[i].Count++;
    }
    last->ClientId = Entry->client_id;
    last->SourceId = Entry->src_id;
    last->RingId = Entry->ring_id;
    last->VmId = Entry->vmid;
    last->VmIdSrc = Entry->vmid_src;
    last->Pasid = Entry->pasid;
    last->SrcData[0] = Entry->src_data[0];
    last->SrcData[1] = Entry->src_data[1];
    last->SrcData[2] = Entry->src_data[2];
    last->SrcData[3] = Entry->src_data[3];
    last->Timestamp = Entry->timestamp;
    Ih->LastNext = (Ih->LastNext + 1) % BC250_IH_MAX_LAST;
    if (Ih->LastCount < BC250_IH_MAX_LAST) Ih->LastCount++;
}

// At DISPATCH_LEVEL. amdgpu_ih_process(): consume up to the write pointer, publish the read pointer, look again. Every
// vector advances the read pointer whether its source is known or not: an unknown source must not wedge the ring.
static void Consume(_Inout_ BC250_IH* ih)
{
    struct amdgpu_device* adev;
    struct bc250_iv_entry entry;
    ULONG round, budget;
    u32 wptr, rptr;
    bool overflowed;

    adev = ih->DpcAdev;
    budget = adev->irq.ih.ring_size / 32;           // one pass over the whole ring per DPC at most
    for (round = 0; round < BC250_IH_DPC_ROUNDS; round++)
    {
        ULONG errors = 0;

        wptr = bc250_ih_get_wptr(adev, &overflowed);
        // A refused access stops the DPC's sequence (sequence.c: reads answer all ones from then on, which would look
        // like an overflow with a write pointer of garbage). Nothing is consumed any more; STATE reports the offset.
        // The same for a write pointer that is not a multiple of a vector: the decode reads eight dwords from it.
        if (!NT_SUCCESS(ih->DpcSequence.Fault) || (wptr & 31) != 0 || (overflowed && (adev->irq.ih.rptr & 31) != 0))
        {
            InterlockedExchange(&ih->Active, 0);
            errors = 1;
        }
        else if (overflowed) ih->Rptr = adev->irq.ih.rptr;
        rptr = ih->Rptr;

        KeAcquireSpinLockAtDpcLevel(&ih->StatsLock);        // memory only from here to the release
        if (overflowed) ih->Stats.OverflowCount++;
        while (errors == 0 && rptr != wptr && budget != 0)
        {
            if (bc250_ih_decode(adev, &rptr, &entry) != 0) { errors++; rptr = wptr; break; }
            Note(&ih->Stats, &entry);
            budget--;
        }
        ih->Stats.DecodeErrors += errors;
        ih->Stats.Rptr = rptr;
        KeReleaseSpinLockFromDpcLevel(&ih->StatsLock);

        if (ih->Active == 0 || rptr == ih->Rptr) break;     // stopped, or nothing new
        ih->Rptr = rptr;
        bc250_ih_set_rptr(adev, rptr);
        if (budget == 0) break;
    }
}

// The read pointer and the doorbell are outside the spin lock, so two consumers at once would take vectors twice and
// could move the read pointer backwards. Whoever finds the consumer busy leaves a request and looks once more: either
// the consumer sees the request after it lets go, or the latecomer sees the consumer gone (both are interlocked).
void IhDpc(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih = (BC250_IH*)Device->Ih;

    if (ih == NULL || ih->Active == 0) return;
    InterlockedIncrement(&ih->DpcCount);
    for (;;)
    {
        if (InterlockedCompareExchange(&ih->InDpc, 1, 0) != 0)
        {
            InterlockedExchange(&ih->DpcAgain, 1);
            if (ih->InDpc != 0) return;
            continue;
        }
        InterlockedExchange(&ih->DpcAgain, 0);
        Consume(ih);
        InterlockedExchange(&ih->InDpc, 0);
        if (InterlockedExchange(&ih->DpcAgain, 0) == 0 || ih->Active == 0) return;
    }
}

// ---- the escape ------------------------------------------------------------------------------------------------------

// As gfx.c's: a PLAN answers a read of a register it has planned a write to with the planned value (IH_RB_CNTL is
// read, modified and written four times in a row).
static BOOLEAN IhPlanAnswers(_In_ BC250_SEQUENCE* Sequence, ULONG DwordIndex, _Out_ ULONG* Value)
{
    ULONG offset = DwordIndex * 4, i;

    *Value = 0;
    if (Sequence->Writes == NULL) return FALSE;
    for (i = min(Sequence->WriteCount, Sequence->MaxWrites); i-- > 0; )
    {
        if (Sequence->Writes[i].Offset != offset) continue;
        *Value = Sequence->Writes[i].Value;
        // IH_RB_CNTL.WPTR_OVERFLOW_CLEAR does not stay written: unit A's trace has C03101A0 going in at 0.252838 s and
        // 403101A0 read back at 0.252845 s. The same declaration as the host replay's (driver/shim/test/replay_ih.c).
        if (offset == BC250_REG_OSSSYS_IH_RB_CNTL) *Value &= ~BC250_IH_RB_OVERFLOW_CLEAR;
        return TRUE;
    }
    return FALSE;
}

// As gfx.c's: the ring is GTT memory, gpumem.c flushes the TLB after the bind, and the semaphore of that flush goes back
// even when the sequence has stopped.
static BOOLEAN IhPassesFault(_In_ BC250_SEQUENCE* Sequence, ULONG DwordIndex, ULONG Value)
{
    UNREFERENCED_PARAMETER(Sequence);
    return Value == 0 && DwordIndex == BC250_REG_MMHUB_MMVM_INVALIDATE_ENG17_SEM / 4;
}

static BOOLEAN RingReadsDisabled(_In_ const BC250_DEVICE* Device)
{
    ULONG cntl = 0xFFFFFFFFul;

    if (!NT_SUCCESS(MmioIhRead(Device, BC250_REG_OSSSYS_IH_RB_CNTL, &cntl))) return FALSE;
    GuardLog("ih: IH_RB_CNTL 0x%08X", cntl);
    return (cntl & BC250_IH_RB_ENABLE) == 0;
}

// With GartLock held and Adev->backend pointing at Ih->Sequence. No interrupt of ours after the first two lines; the
// memory goes back only if the ring reads disabled (gpumem.c's rule).
static BOOLEAN Fini(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_IH* Ih, _In_ struct amdgpu_device* Adev)
{
    BOOLEAN quiet = TRUE;

    InterlockedExchange(&Ih->Active, 0);
    KeFlushQueuedDpcs();                // a DPC that saw Active set has finished when this returns
    if (Ih->Enabled)
    {
        bc250_ih_hw_fini(Adev);
        quiet = RingReadsDisabled(Device);
    }
    if (Ih->SetUp) bc250_ih_teardown(Adev);
    GpuMemRelease(Device, &Ih->Sequence, quiet);
    Ih->SetUp = FALSE;
    if (quiet) Ih->Enabled = FALSE;
    return quiet;
}

void IhEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_IH* Data)
{
    BC250_IH* ih;
    struct amdgpu_device* adev = NULL;
    void* previousBackend = NULL;
    BOOLEAN gartEnabled = FALSE, plan = (Data->Op == BC250_IH_OP_PLAN);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG vram, gtt;
    KIRQL irql;
    long result = 0;

    Data->Version = BC250_KMD_VERSION;
    Data->Result = 0;
    Data->FaultOffset = 0;
    Data->WriteCount = 0;
    Data->EntryCount = 0;
    Data->OverflowCount = 0;
    Data->Rptr = 0;
    Data->Wptr = 0;
    Data->KindCount = 0;
    Data->LastCount = 0;
    RtlZeroMemory(Data->Kinds, sizeof(Data->Kinds));
    RtlZeroMemory(Data->Last, sizeof(Data->Last));
    RtlZeroMemory(Data->Writes, sizeof(Data->Writes));

    ExAcquireFastMutex(&Device->GartLock);
    ih = (BC250_IH*)Device->Ih;
    if (Data->Op > BC250_IH_OP_STATE) status = STATUS_INVALID_PARAMETER;
    else if (Data->Op != BC250_IH_OP_STATE && (ih == NULL || !Device->MmioIhEnabled || Device->GpuMem == NULL)) status = STATUS_DEVICE_NOT_READY;
    if (NT_SUCCESS(status) && Data->Op != BC250_IH_OP_STATE) status = GartDevice(Device, &adev, &gartEnabled);
    if (NT_SUCCESS(status) && Data->Op != BC250_IH_OP_STATE)
    {
        previousBackend = adev->backend;
        adev->backend = &ih->Sequence;
        SequenceBegin(&ih->Sequence, Device, plan, Data->Writes, BC250_IH_MAX_WRITES);
        GpuMemBeginSequence(Device, NULL, 0);

        switch (Data->Op)
        {
        case BC250_IH_OP_PLAN:
        case BC250_IH_OP_INIT:
            // A plan is about hardware this driver instance has not touched; an init needs the GART the ring lives behind.
            // And an init needs the message interrupt: IH_RB_CNTL.RPTR_REARM is what re-raises an interrupt for vectors
            // that arrive while the DPC runs, and amdgpu sets it for MSI only. On a line this driver would lose them.
            if (ih->SetUp || ih->Enabled || (!plan && (!gartEnabled || !Device->InterruptIsMessage))) { status = STATUS_INVALID_DEVICE_STATE; break; }
            result = bc250_ih_setup(adev, Device->InterruptIsMessage != FALSE);
            if (result != 0) { status = STATUS_INSUFFICIENT_RESOURCES; GpuMemRelease(Device, &ih->Sequence, TRUE); break; }
            ih->SetUp = TRUE;
            if (!plan) ih->Enabled = TRUE;          // from its first write on, the init has touched the hardware
            result = bc250_ih_hw_init(adev);
            GuardLog("ih: init%s: rc %d, %u writes", plan ? " planned" : "", result, ih->Sequence.WriteCount);
            if (plan)
            {
                // No register write was executed and a PLAN's pages are not entered into the GART table (gpumem.c).
                bc250_ih_teardown(adev);
                ih->SetUp = FALSE;
                GpuMemRelease(Device, &ih->Sequence, TRUE);
                break;
            }
            if (result != 0 || !NT_SUCCESS(ih->Sequence.Fault)) { (void)Fini(Device, ih, adev); break; }
            // The DPC's own device: everything the shim's three DPC functions read (register bases, the ring), and a
            // backend nobody else uses.
            *ih->DpcAdev = *adev;
            ih->DpcAdev->backend = &ih->DpcSequence;
            SequenceBegin(&ih->DpcSequence, Device, FALSE, NULL, 0);
            ih->Rptr = 0;
            KeAcquireSpinLock(&ih->StatsLock, &irql);
            RtlZeroMemory(&ih->Stats, sizeof(ih->Stats));
            KeReleaseSpinLock(&ih->StatsLock, irql);
            InterlockedExchange(&ih->Active, 1);
            break;

        case BC250_IH_OP_FINI:
            if (!ih->SetUp && !ih->Enabled) { status = STATUS_INVALID_DEVICE_STATE; break; }
            if (!Fini(Device, ih, adev)) status = STATUS_IO_DEVICE_ERROR;
            break;
        }
        if (NT_SUCCESS(status)) status = ih->Sequence.Fault;
        Data->FaultOffset = ih->Sequence.FaultOffset;
        Data->WriteCount = ih->Sequence.WriteCount;
        (void)GpuMemEndSequence(Device, &vram, &gtt);
        ih->Sequence.Writes = NULL;             // the caller's buffer goes away with this call
        ih->Sequence.MaxWrites = 0;
        adev->backend = previousBackend;
    }

    // What Windows assigned and how often the routine ran are known without the gate: they are about Windows, not the GPU.
    Data->InterruptIsMessage = Device->InterruptIsMessage;
    Data->InterruptVector = Device->InterruptVector;
    Data->InterruptCount = (unsigned long)Device->InterruptCount;
    Data->LastMessageNumber = (unsigned long)Device->LastMessageNumber;
    Data->OurInterrupts = ih != NULL ? (unsigned long)ih->OurInterrupts : 0;
    Data->DpcCount = ih != NULL ? (unsigned long)ih->DpcCount : 0;
    Data->Active = ih != NULL && ih->Active != 0;
    if (ih != NULL)
    {
        const BC250_IH_STATS* snap = &ih->Snapshot;
        ULONG i, first;

        // The caller's buffer is not touched at DISPATCH_LEVEL: whether dxgkrnl hands an escape non-paged memory is not
        // documented.
        KeAcquireSpinLock(&ih->StatsLock, &irql);
        ih->Snapshot = ih->Stats;
        KeReleaseSpinLock(&ih->StatsLock, irql);
        Data->EntryCount = snap->EntryCount;
        Data->OverflowCount = snap->OverflowCount;
        Data->Rptr = snap->Rptr;
        if (ih->Active != 0 && ih->DpcAdev->irq.ih.wptr_cpu != NULL) Data->Wptr = *ih->DpcAdev->irq.ih.wptr_cpu;
        Data->KindCount = snap->KindCount;
        RtlCopyMemory(Data->Kinds, snap->Kinds, sizeof(Data->Kinds));
        Data->LastCount = snap->LastCount;
        first = (snap->LastNext + BC250_IH_MAX_LAST - snap->LastCount) % BC250_IH_MAX_LAST;
        for (i = 0; i < snap->LastCount; i++) Data->Last[i] = snap->Last[(first + i) % BC250_IH_MAX_LAST];      // oldest first
        if (Data->FaultOffset == 0 && !NT_SUCCESS(ih->DpcSequence.Fault)) Data->FaultOffset = ih->DpcSequence.FaultOffset;
    }
    GuardLog("ih: op %u -> 0x%08X, result %d, %u writes; %u interrupt routine calls, %u ours, %u DPCs, %u vectors", Data->Op, status,
             result, Data->WriteCount, Data->InterruptCount, Data->OurInterrupts, Data->DpcCount, Data->EntryCount);
    ExReleaseFastMutex(&Device->GartLock);

    Data->Result = result;
    Data->NtStatus = (unsigned long)status;
    Data->Status = (NT_SUCCESS(status) && result == 0) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// With GartLock held. For gart.c: the ring lives behind the GART, a restore under it is refused.
BOOLEAN IhIsActive(_In_ const BC250_DEVICE* Device)
{
    const BC250_IH* ih = (const BC250_IH*)Device->Ih;

    return ih != NULL && (ih->SetUp || ih->Enabled);
}

// ---- start, stop, remove ---------------------------------------------------------------------------------------------

NTSTATUS IhStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih = (BC250_IH*)Device->Ih;

    Device->IhQuiet = TRUE;
    // A previous start's object is kept and reused, never freed here: whether dxgkrnl has the interrupt connected
    // during DxgkDdiStartDevice is not documented, and the interrupt routine reads Device->Ih without a lock. The only
    // free is IhRemove's. With the gate closed the object stays, inactive.
    if (!Device->MmioIhEnabled || Device->GpuMem == NULL) return STATUS_SUCCESS;
    if (ih == NULL)
    {
        ih = (BC250_IH*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ih), BC250_IH_TAG);
        if (ih == NULL) return STATUS_SUCCESS;      // never fails the start
        ih->DpcAdev = (struct amdgpu_device*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ih->DpcAdev), BC250_IH_TAG);
        if (ih->DpcAdev == NULL) { ExFreePoolWithTag(ih, BC250_IH_TAG); return STATUS_SUCCESS; }
        KeInitializeSpinLock(&ih->StatsLock);
    }
    InterlockedExchange(&ih->Active, 0);
    InterlockedExchange(&ih->OurInterrupts, 0);
    InterlockedExchange(&ih->DpcCount, 0);
    ih->SetUp = FALSE;
    ih->Enabled = FALSE;
    ih->Sequence.Name = "ih";
    ih->Sequence.Read = MmioIhRead;
    ih->Sequence.Write = MmioIhWrite;
    ih->Sequence.PlanAnswers = IhPlanAnswers;
    ih->Sequence.PassesFault = IhPassesFault;
    ih->DpcSequence.Name = "ih dpc";
    ih->DpcSequence.Read = DpcRead;
    ih->DpcSequence.Write = DpcWrite;
    ih->DpcSequence.Dpc = TRUE;
    Device->Ih = ih;
    GuardLog("ih: ready");
    return STATUS_SUCCESS;
}

// First of the stops (pnp.c): no interrupt of ours after this returns, the ring disabled while the GART is still there.
// The object itself stays until dxgkrnl has disconnected the interrupt, i.e. it is freed at remove only, never under a
// running DPC. Device->IhQuiet tells gpumem.c's stop whether the ring's pages may go back to Windows.
void IhStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih;
    struct amdgpu_device* adev = NULL;
    BOOLEAN gartEnabled;

    ExAcquireFastMutex(&Device->GartLock);
    ih = (BC250_IH*)Device->Ih;
    if (ih != NULL)
    {
        InterlockedExchange(&ih->Active, 0);
        KeFlushQueuedDpcs();                // here and not only in Fini: no DPC is inside the ring when this returns, whatever follows
        if (ih->SetUp || ih->Enabled)
        {
            Device->IhQuiet = FALSE;
            if (NT_SUCCESS(GartDevice(Device, &adev, &gartEnabled)))
            {
                void* previousBackend = adev->backend;

                adev->backend = &ih->Sequence;
                SequenceBegin(&ih->Sequence, Device, FALSE, NULL, 0);
                GpuMemBeginSequence(Device, NULL, 0);
                Device->IhQuiet = Fini(Device, ih, adev);
                adev->backend = previousBackend;
            }
        }
    }
    ExReleaseFastMutex(&Device->GartLock);
}

void IhRemove(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih = (BC250_IH*)InterlockedExchangePointer(&Device->Ih, NULL);

    if (ih == NULL) return;
    if (ih->DpcAdev != NULL) ExFreePoolWithTag(ih->DpcAdev, BC250_IH_TAG);
    ExFreePoolWithTag(ih, BC250_IH_TAG);
}
