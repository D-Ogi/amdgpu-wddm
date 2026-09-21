// Interrupts (milestone M6, ADR 0007): the IH ring of the GPU's interrupt controller and the miniport's interrupt and
// DPC routines.
//
// What amdgpu does on this part (navi10_ih.c, E03 trace 0.252832 to 0.252845 s): a ring buffer in GTT memory that the
// interrupt controller writes 32-byte vectors into, a write-back slot for its write pointer, a doorbell for the read
// pointer. Sources (CP end-of-pipe, faults, SDMA traps) are enabled elsewhere (gfx.c's interrupt stage); with the ring
// disabled their enable bits do nothing.
//
// Three contexts touch this file, and they share as little as possible:
//   the escape (PASSIVE/APC_LEVEL, Device->GartLock held)  sets the ring up and tears it down through the shim, as every
//                                                          other sequence; flips Active last on the way up, first on
//                                                          the way down
//   the interrupt routine (DIRQL)                           counts, asks for the DPC; touches no register
//   the DPC (DISPATCH_LEVEL)                                the only consumer of the ring: reads the write-back slot and
//                                                          the ring (memory), advances the read pointer (doorbell);
//                                                          never goes through adev->backend, which belongs to the
//                                                          escape's sequence
//
// State of this file: the plumbing and the STATE query. INIT, PLAN and FINI arrive with driver/shim's IH code.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"

#define BC250_IH_TAG 'hI2B'

typedef struct _BC250_IH {
    volatile LONG Active;               // the ring is enabled and the DPC may consume it
    volatile LONG OurInterrupts;        // interrupt routine calls taken as ours
    volatile LONG DpcCount;
} BC250_IH;

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

// At DISPATCH_LEVEL.
void IhDpc(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih = (BC250_IH*)Device->Ih;

    if (ih == NULL || ih->Active == 0) return;
    InterlockedIncrement(&ih->DpcCount);
}

void IhEscape(_Inout_ BC250_DEVICE* Device, _Inout_ BC250_ESCAPE_IH* Data)
{
    BC250_IH* ih;
    NTSTATUS status = STATUS_SUCCESS;

    Data->Version = BC250_KMD_VERSION;
    Data->Result = 0;
    Data->FaultOffset = 0;
    Data->WriteCount = 0;
    Data->EntryCount = 0;
    Data->KindCount = 0;
    Data->LastCount = 0;

    ExAcquireFastMutex(&Device->GartLock);
    ih = (BC250_IH*)Device->Ih;
    // What Windows assigned and how often the routine ran are known without the gate: they are about Windows, not the GPU.
    Data->InterruptIsMessage = Device->InterruptIsMessage;
    Data->InterruptVector = Device->InterruptVector;
    Data->InterruptCount = (unsigned long)Device->InterruptCount;
    Data->LastMessageNumber = (unsigned long)Device->LastMessageNumber;
    Data->OurInterrupts = ih != NULL ? (unsigned long)ih->OurInterrupts : 0;
    Data->DpcCount = ih != NULL ? (unsigned long)ih->DpcCount : 0;
    Data->Active = ih != NULL && ih->Active != 0;
    if (Data->Op > BC250_IH_OP_STATE) status = STATUS_INVALID_PARAMETER;
    else if (Data->Op != BC250_IH_OP_STATE) status = (ih == NULL) ? STATUS_DEVICE_NOT_READY : STATUS_NOT_IMPLEMENTED;
    GuardLog("ih: op %u -> 0x%08X, %u interrupt routine calls, %u ours, %u DPCs", Data->Op, status, Data->InterruptCount,
             Data->OurInterrupts, Data->DpcCount);
    ExReleaseFastMutex(&Device->GartLock);

    Data->NtStatus = (unsigned long)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// With GartLock held. For gfx.c: the memory of the ring is GTT memory that a GFX fini would otherwise give back.
BOOLEAN IhIsActive(_In_ const BC250_DEVICE* Device)
{
    const BC250_IH* ih = (const BC250_IH*)Device->Ih;

    return ih != NULL && ih->Active != 0;
}

NTSTATUS IhStart(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih = (BC250_IH*)Device->Ih;

    // A previous start's object is kept and reused, never freed here: whether dxgkrnl has the interrupt connected
    // during DxgkDdiStartDevice is not documented, and the interrupt routine reads Device->Ih without a lock. The only
    // free is IhRemove's. With the gate closed the object stays, inactive.
    if (!Device->MmioIhEnabled || Device->GpuMem == NULL) return STATUS_SUCCESS;
    if (ih == NULL)
    {
        ih = (BC250_IH*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*ih), BC250_IH_TAG);
        if (ih == NULL) return STATUS_SUCCESS;      // never fails the start
    }
    InterlockedExchange(&ih->Active, 0);
    InterlockedExchange(&ih->OurInterrupts, 0);
    InterlockedExchange(&ih->DpcCount, 0);
    Device->Ih = ih;
    GuardLog("ih: ready");
    return STATUS_SUCCESS;
}

// First of the stops (pnp.c): no interrupt of ours after this returns. The object itself stays until dxgkrnl has
// disconnected the interrupt, i.e. it is freed at remove only, never under a running DPC.
void IhStop(_Inout_ BC250_DEVICE* Device)
{
    BC250_IH* ih;

    ExAcquireFastMutex(&Device->GartLock);
    ih = (BC250_IH*)Device->Ih;
    if (ih != NULL) InterlockedExchange(&ih->Active, 0);
    ExReleaseFastMutex(&Device->GartLock);
}

void IhRemove(_Inout_ BC250_DEVICE* Device)
{
    PVOID ih = InterlockedExchangePointer(&Device->Ih, NULL);

    if (ih != NULL) ExFreePoolWithTag(ih, BC250_IH_TAG);
}
