/* SPDX-License-Identifier: MIT */
#include "bc250kmd.h"
#include "hip_journal.h"

/* Fixed nonpaged driver-image storage: no acquisition path allocates memory.
 * The spinlock protects only owner/slot metadata. Payload copy and validation
 * happen outside it while WRITING pins the slot; readers reject odd generations.
 * DPC retirement acquires this lock only and never waits for payload work.
 * Lock order: WDDM queue lock -> journal lock; never the reverse.
 */
BC250_HIP_JOURNAL g_HipDispatchJournal;
static KSPIN_LOCK g_HipJournalLock;
static LIST_ENTRY g_HipOwners;
static volatile LONG g_HipInitialized;

static void HipJournalInit(void)
{
    if (InterlockedCompareExchange(&g_HipInitialized, 1, 0) == 0) {
        KeInitializeSpinLock(&g_HipJournalLock);
        InitializeListHead(&g_HipOwners);
        g_HipDispatchJournal.Magic = 0x4a504948u; /* HIPJ */
        g_HipDispatchJournal.Version = 1;
        g_HipDispatchJournal.Bytes = sizeof(g_HipDispatchJournal);
        g_HipDispatchJournal.SlotBytes = sizeof(BC250_HIP_JOURNAL_SLOT);
        g_HipDispatchJournal.SlotCount = BC250_HIP_JOURNAL_SLOTS;
        InterlockedExchange(&g_HipInitialized, 2);
    } else while (InterlockedCompareExchange(&g_HipInitialized, 2, 2) != 2) YieldProcessor();
}

static void HipWriteBegin(BC250_HIP_JOURNAL_SLOT* Slot)
{
    (void)InterlockedIncrement64(&Slot->Generation);
}
static void HipWriteEnd(BC250_HIP_JOURNAL_SLOT* Slot)
{
    KeMemoryBarrier();
    (void)InterlockedIncrement64(&Slot->Generation);
}
static BC250_HIP_JOURNAL_SLOT* HipFindId(ULONGLONG Id)
{
    ULONG i;
    if (!Id) return NULL;
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i)
        if (g_HipDispatchJournal.Slots[i].Id == Id) return &g_HipDispatchJournal.Slots[i];
    return NULL;
}
static BC250_HIP_JOURNAL_OWNER* HipFindOwner(PVOID Adapter, PVOID Context, PVOID RuntimeDevice, PEPROCESS Process)
{
    LIST_ENTRY* link;
    for (link = g_HipOwners.Flink; link != &g_HipOwners; link = link->Flink) {
        BC250_HIP_JOURNAL_OWNER* owner = CONTAINING_RECORD(link, BC250_HIP_JOURNAL_OWNER, Link);
        if (owner->Adapter == Adapter && owner->Context == Context && owner->RuntimeDevice == RuntimeDevice &&
            owner->Process == Process) return owner;
    }
    return NULL;
}

NTSTATUS HipJournalOwnerCreate(BC250_HIP_JOURNAL_OWNER* Owner, PVOID Adapter, PVOID Context, PVOID RuntimeDevice)
{
    KIRQL irql;
    PEPROCESS process = PsGetCurrentProcess();
    HipJournalInit();
    ObReferenceObject(process);
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    if (g_HipDispatchJournal.NextContext == MAXULONGLONG) {
        KeReleaseSpinLock(&g_HipJournalLock, irql);
        ObDereferenceObject(process);
        return STATUS_INTEGER_OVERFLOW;
    }
    Owner->Process = process;
    Owner->ProcessId = HandleToULong(PsGetCurrentProcessId());
    Owner->Adapter = Adapter;
    Owner->Context = Context;
    Owner->RuntimeDevice = RuntimeDevice;
    Owner->Cookie = ++g_HipDispatchJournal.NextContext;
    Owner->Enabled = FALSE;
    Owner->Registered = TRUE;
    InsertTailList(&g_HipOwners, &Owner->Link);
    KeReleaseSpinLock(&g_HipJournalLock, irql);
    return STATUS_SUCCESS;
}

void HipJournalOwnerDestroy(BC250_HIP_JOURNAL_OWNER* Owner)
{
    KIRQL irql;
    PEPROCESS process = NULL;
    ULONG i;
    if (InterlockedCompareExchange(&g_HipInitialized, 2, 2) != 2) return;
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    if (Owner->Registered) {
        RemoveEntryList(&Owner->Link);
        Owner->Registered = FALSE;
        process = Owner->Process;
        Owner->Process = NULL;
        for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) {
            BC250_HIP_JOURNAL_SLOT* slot = &g_HipDispatchJournal.Slots[i];
            if (slot->ContextCookie != Owner->Cookie) continue;
            if (slot->State == BC250_HIP_SLOT_WRITING) slot->CancelRequested = TRUE;
            else if (slot->State == BC250_HIP_SLOT_UPLOADED) {
                HipWriteBegin(slot);
                slot->State = BC250_HIP_SLOT_ABANDONED;
                slot->Status = STATUS_PROCESS_IS_TERMINATING;
                slot->Finished100ns = KeQueryInterruptTime();
                HipWriteEnd(slot);
            }
        }
    }
    KeReleaseSpinLock(&g_HipJournalLock, irql);
    if (process) ObDereferenceObject(process);
}

static int HipPayloadValid(const UCHAR* Data, ULONG Bytes)
{
    BC250_HIP_JOURNAL_UPLOAD h;
    ULONG at, i, j;
    ULONGLONG ids[BC250_HIP_JOURNAL_MAX_RECORDS];
    if (Bytes < sizeof(h) || Bytes > BC250_HIP_JOURNAL_MAX_BYTES) return 0;
    RtlCopyMemory(&h, Data, sizeof(h));
    if (h.Magic != BC250_HIP_JOURNAL_MAGIC || h.Command != BC250_HIP_JOURNAL_COMMAND ||
        h.AbiVersion != BC250_HIP_JOURNAL_ABI || h.TotalBytes != Bytes || h.UploadId ||
        h.Status != 0xffffffffu || h.Version || h.NtStatus ||
        h.Operation != BC250_HIP_JOURNAL_UPLOAD_OP || !h.RecordCount || h.RecordCount > BC250_HIP_JOURNAL_MAX_RECORDS ||
        !h.IbVa || (h.IbVa & 3) || !h.FenceVa || (h.FenceVa & 7) || !h.FenceValue ||
        h.Reserved[0] || h.Reserved[1] || h.Reserved[2]) return 0;
    at = sizeof(h);
    for (i = 0; i < h.RecordCount; ++i) {
        BC250_HIP_DISPATCH_RECORD record;
        if (Bytes - at < sizeof(record)) return 0;
        RtlCopyMemory(&record, Data + at, sizeof(record));
        if (record.Bytes > Bytes - at || !Bc250HipRecordValid(Data + at, record.Bytes)) return 0;
        for (j = 0; j < i; ++j) if (ids[j] == record.DispatchId) return 0;
        ids[i] = record.DispatchId;
        at += record.Bytes;
    }
    return at == Bytes;
}

/* The context owner is looked up, never dereferenced from escape input. Stop and
 * context destruction remove it under this same lock, so this software-only
 * escape needs neither adapter synchronization nor access to freed WDDM state.
 */
NTSTATUS HipJournalEscape(PVOID Adapter, const DXGKARG_ESCAPE* Escape)
{
    BC250_HIP_JOURNAL_UPLOAD h;
    BC250_HIP_JOURNAL_OWNER* owner;
    BC250_HIP_JOURNAL_SLOT* slot = NULL;
    PEPROCESS process = PsGetCurrentProcess();
    D3DDDI_ESCAPEFLAGS wanted = {0};
    KIRQL irql;
    ULONG i, bytes;
    ULONGLONG id = 0;
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    wanted.NoAdapterSynchronization = 1;
    if (!Escape || !Escape->pPrivateDriverData || Escape->PrivateDriverDataSize < sizeof(h) ||
        Escape->PrivateDriverDataSize > BC250_HIP_JOURNAL_MAX_BYTES || KeGetCurrentIrql() != PASSIVE_LEVEL)
        return STATUS_INVALID_PARAMETER;
    bytes = Escape->PrivateDriverDataSize;
    RtlCopyMemory(&h, Escape->pPrivateDriverData, sizeof(h));
    if (Escape->Flags.Value != wanted.Value || h.Magic != BC250_HIP_JOURNAL_MAGIC ||
        h.Command != BC250_HIP_JOURNAL_COMMAND || h.AbiVersion != BC250_HIP_JOURNAL_ABI ||
        h.TotalBytes != bytes || h.Status != 0xffffffffu || h.Version || h.NtStatus ||
        h.Reserved[0] || h.Reserved[1] || h.Reserved[2] ||
        h.Operation > BC250_HIP_JOURNAL_CANCEL_OP) goto output;
    if (InterlockedCompareExchange(&g_HipInitialized, 2, 2) != 2) { status = STATUS_ACCESS_DENIED; goto output; }
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    owner = HipFindOwner(Adapter, Escape->hContext, Escape->hDevice, process);
    if (!owner) { status = STATUS_ACCESS_DENIED; goto unlock; }
    if (h.Operation == BC250_HIP_JOURNAL_CANCEL_OP) {
        slot = HipFindId(h.UploadId);
        if (bytes != sizeof(h) || h.RecordCount || !slot || slot->ContextCookie != owner->Cookie) goto unlock;
        if (slot->ReplayPending || slot->State == BC250_HIP_SLOT_WRITING || slot->State == BC250_HIP_SLOT_BOUND ||
            slot->State == BC250_HIP_SLOT_SUBMITTED) { status = STATUS_DEVICE_BUSY; goto unlock; }
        if (slot->State == BC250_HIP_SLOT_UPLOADED) {
            HipWriteBegin(slot);
            slot->State = BC250_HIP_SLOT_CANCELLED;
            slot->Finished100ns = KeQueryInterruptTime();
            slot->Status = STATUS_CANCELLED;
            HipWriteEnd(slot);
            ++g_HipDispatchJournal.Cancelled;
        }
        id = slot->Id;
        status = STATUS_SUCCESS;
        goto unlock;
    }
    if (h.UploadId || !h.RecordCount || h.RecordCount > BC250_HIP_JOURNAL_MAX_RECORDS || !h.IbVa ||
        (h.IbVa & 3) || !h.FenceVa || (h.FenceVa & 7) || !h.FenceValue) goto unlock;
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) {
        BC250_HIP_JOURNAL_SLOT* old = &g_HipDispatchJournal.Slots[i];
        if (old->State >= BC250_HIP_SLOT_WRITING && old->State <= BC250_HIP_SLOT_SUBMITTED &&
            old->ContextCookie == owner->Cookie && old->IbVa == h.IbVa &&
            old->FenceVa == h.FenceVa && old->FenceValue == h.FenceValue) { status = STATUS_OBJECT_NAME_COLLISION; goto unlock; }
    }
    if (g_HipDispatchJournal.NextId == MAXULONGLONG) { status = STATUS_INTEGER_OVERFLOW; goto unlock; }
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) {
        ULONG at = (g_HipDispatchJournal.NextSlot + i) % BC250_HIP_JOURNAL_SLOTS;
        BC250_HIP_JOURNAL_SLOT* candidate = &g_HipDispatchJournal.Slots[at];
        if (!candidate->State || candidate->State >= BC250_HIP_SLOT_COMPLETED) {
            slot = candidate;
            g_HipDispatchJournal.NextSlot = (at + 1) % BC250_HIP_JOURNAL_SLOTS;
            break;
        }
    }
    if (!slot) { status = STATUS_DEVICE_BUSY; goto unlock; }
    if (slot->State) ++g_HipDispatchJournal.Overwritten;
    HipWriteBegin(slot);
    slot->Id = ++g_HipDispatchJournal.NextId;
    slot->State = BC250_HIP_SLOT_WRITING;
    slot->ContextCookie = owner->Cookie;
    slot->Context = (ULONGLONG)(ULONG_PTR)owner->Context;
    slot->Adapter = (ULONGLONG)(ULONG_PTR)owner->Adapter;
    slot->Process = (ULONGLONG)(ULONG_PTR)owner->Process;
    slot->ProcessId = owner->ProcessId;
    slot->DataBytes = bytes;
    slot->IbVa = h.IbVa; slot->FenceVa = h.FenceVa; slot->FenceValue = h.FenceValue;
    slot->Uploaded100ns = KeQueryInterruptTime();
    slot->Bound100ns = slot->Submitted100ns = slot->Finished100ns = slot->Root = 0;
    slot->OsFence = slot->Seq = slot->Vmid = slot->IbBytes = 0;
    slot->Status = STATUS_PENDING;
    slot->CancelRequested = FALSE;
    slot->ReplayPending = FALSE;
    slot->Attempts = 0;
    KeReleaseSpinLock(&g_HipJournalLock, irql);

    RtlZeroMemory(slot->Data, sizeof(slot->Data));
    RtlCopyMemory(slot->Data, Escape->pPrivateDriverData, bytes);
    status = HipPayloadValid(slot->Data, bytes) ? STATUS_SUCCESS : STATUS_INVALID_PARAMETER;
    /* Reject a changed envelope too. Identity used to reserve the slot and the
     * retained payload must be the same even if a producer violates its contract.
     */
    if (RtlCompareMemory(slot->Data, &h, sizeof(h)) != sizeof(h)) status = STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    owner = HipFindOwner(Adapter, Escape->hContext, Escape->hDevice, process);
    if (!owner || owner->Cookie != slot->ContextCookie || slot->CancelRequested) status = STATUS_PROCESS_IS_TERMINATING;
    if (NT_SUCCESS(status)) {
        owner->Enabled = TRUE;
        slot->State = BC250_HIP_SLOT_UPLOADED;
        ++g_HipDispatchJournal.Uploads;
        id = slot->Id;
    } else {
        slot->State = BC250_HIP_SLOT_FAILED;
        slot->Finished100ns = KeQueryInterruptTime();
    }
    slot->Status = status;
    HipWriteEnd(slot);
unlock:
    if (!NT_SUCCESS(status)) ++g_HipDispatchJournal.Refused;
    KeReleaseSpinLock(&g_HipJournalLock, irql);
output:
    h.Status = NT_SUCCESS(status) ? 0u : 1u;
    h.NtStatus = (ULONG)status;
    h.Version = 1;
    h.UploadId = id;
    RtlCopyMemory(Escape->pPrivateDriverData, &h, sizeof(h));
    return status;
}

NTSTATUS HipJournalBind(const BC250_HIP_JOURNAL_OWNER* Owner, ULONGLONG IbVa, ULONGLONG FenceVa,
                        ULONGLONG FenceValue, ULONG OsFence, ULONG IbBytes, ULONGLONG* Id)
{
    KIRQL irql;
    ULONG i;
    NTSTATUS status = STATUS_SUCCESS;
    *Id = 0;
    if (InterlockedCompareExchange(&g_HipInitialized, 2, 2) != 2) return STATUS_SUCCESS;
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    if (!Owner->Registered) {
        if (Owner->Cookie) status = STATUS_INVALID_DEVICE_STATE;
        goto done;
    }
    if (!Owner->Enabled) goto done;
    status = STATUS_INVALID_PARAMETER;
    for (i = 0; i < BC250_HIP_JOURNAL_SLOTS; ++i) {
        BC250_HIP_JOURNAL_SLOT* slot = &g_HipDispatchJournal.Slots[i];
        if (slot->State != BC250_HIP_SLOT_UPLOADED || slot->ContextCookie != Owner->Cookie ||
            slot->IbVa != IbVa || slot->FenceVa != FenceVa || slot->FenceValue != FenceValue) continue;
        HipWriteBegin(slot);
        slot->State = BC250_HIP_SLOT_BOUND;
        slot->Status = STATUS_PENDING;
        slot->OsFence = OsFence;
        slot->IbBytes = IbBytes;
        slot->Bound100ns = KeQueryInterruptTime();
        slot->Seq = slot->Vmid = 0;
        slot->Root = slot->Submitted100ns = slot->Finished100ns = 0;
        slot->ReplayPending = FALSE;
        if (slot->Attempts != 0xffffu) ++slot->Attempts;
        HipWriteEnd(slot);
        *Id = slot->Id;
        status = STATUS_SUCCESS;
        break;
    }
done:
    KeReleaseSpinLock(&g_HipJournalLock, irql);
    return status;
}

void HipJournalSubmitted(ULONGLONG Id, ULONG Seq, ULONG Vmid, ULONGLONG Root)
{
    KIRQL irql;
    BC250_HIP_JOURNAL_SLOT* slot;
    ULONG fence = 0, pid = 0;
    BOOLEAN stamped = FALSE;
    if (!Id) return;
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    slot = HipFindId(Id);
    if (slot && slot->State == BC250_HIP_SLOT_BOUND) {
        HipWriteBegin(slot);
        slot->Seq = Seq; slot->Vmid = Vmid; slot->Root = Root;
        slot->State = BC250_HIP_SLOT_SUBMITTED;
        slot->Submitted100ns = KeQueryInterruptTime();
        fence = slot->OsFence; pid = slot->ProcessId;
        stamped = TRUE;
        HipWriteEnd(slot);
    }
    KeReleaseSpinLock(&g_HipJournalLock, irql);
    if (stamped) GuardLog("hip: batch %llu pid %lu seq %lu fence %lu vmid %lu", Id, pid, Seq, fence, Vmid);
}
void HipJournalComplete(ULONGLONG Id)
{
    KIRQL irql;
    BC250_HIP_JOURNAL_SLOT* slot;
    if (!Id) return;
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    slot = HipFindId(Id);
    if (slot && slot->State == BC250_HIP_SLOT_SUBMITTED) {
        HipWriteBegin(slot);
        slot->State = BC250_HIP_SLOT_COMPLETED;
        slot->Status = STATUS_SUCCESS;
        slot->Finished100ns = KeQueryInterruptTime();
        HipWriteEnd(slot);
        ++g_HipDispatchJournal.Completed;
    }
    KeReleaseSpinLock(&g_HipJournalLock, irql);
}
void HipJournalFailed(ULONGLONG Id, NTSTATUS Status)
{
    KIRQL irql;
    BC250_HIP_JOURNAL_SLOT* slot;
    if (!Id) return;
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    slot = HipFindId(Id);
    if (slot && slot->State == BC250_HIP_SLOT_BOUND) {
        HipWriteBegin(slot);
        slot->State = BC250_HIP_SLOT_FAILED;
        slot->Status = Status;
        slot->Finished100ns = KeQueryInterruptTime();
        HipWriteEnd(slot);
    }
    KeReleaseSpinLock(&g_HipJournalLock, irql);
}

/* Called only after GfxSoftRecover proved drain, GfxReopenAfterAbort succeeded,
 * and the WDDM recovery epoch still matches. The aborted head is retained as a
 * failure; later render packets may be resubmitted by dxgkrnl with NEW OS fence
 * IDs (Microsoft, TDR changes in Windows 8, "Packets unaffected by engine reset").
 * Their immutable user payload remains pinned. Metadata names the last attempt.
 */
void HipJournalReset(ULONGLONG Id, BOOLEAN Replay)
{
    KIRQL irql;
    BC250_HIP_JOURNAL_SLOT* slot;
    LIST_ENTRY* link;
    BOOLEAN live = FALSE;
    if (!Id) return;
    KeAcquireSpinLock(&g_HipJournalLock, &irql);
    slot = HipFindId(Id);
    if (slot && slot->State == BC250_HIP_SLOT_SUBMITTED) {
        for (link = g_HipOwners.Flink; link != &g_HipOwners; link = link->Flink) {
            BC250_HIP_JOURNAL_OWNER* owner = CONTAINING_RECORD(link, BC250_HIP_JOURNAL_OWNER, Link);
            if (owner->Cookie == slot->ContextCookie) { live = TRUE; break; }
        }
        HipWriteBegin(slot);
        slot->State = Replay && live ? BC250_HIP_SLOT_UPLOADED : BC250_HIP_SLOT_FAILED;
        slot->ReplayPending = Replay && live;
        slot->Status = STATUS_CANCELLED;
        slot->Finished100ns = KeQueryInterruptTime();
        HipWriteEnd(slot);
    }
    KeReleaseSpinLock(&g_HipJournalLock, irql);
}
