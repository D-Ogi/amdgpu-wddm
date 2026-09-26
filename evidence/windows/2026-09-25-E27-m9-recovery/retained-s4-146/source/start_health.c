// Startup confirmation certifies the preceding healthy interval, not future work.
#include "bc250kmd.h"

static volatile LONG64 g_StartGeneration;

void StartHealthEnter(BC250_DEVICE* Device)
{
    KeWaitForSingleObject(&Device->StartHealth.Lifecycle, Executive, KernelMode, FALSE, NULL);
}
void StartHealthLeave(BC250_DEVICE* Device)
{
    KeReleaseMutex(&Device->StartHealth.Lifecycle, FALSE);
}
void StartHealthInitialize(BC250_DEVICE* Device)
{
    KeInitializeMutex(&Device->StartHealth.Lifecycle, 0);
    KeInitializeSpinLock(&Device->StartHealth.Lock);
    ExInitializeRundownProtection(&Device->StartHealth.Readers);
    Device->StartHealth.Closed=TRUE;
}
static void HealthInvalidate(BC250_START_HEALTH_STATE* H)
{
    ++H->Epoch;
    H->ReadySince=0;
    H->LastCompletion=0;
    H->LastSequence=0;
    H->Completed=0;
}
static BOOLEAN HealthReady(const BC250_START_HEALTH_STATE* H)
{
    return H->Full && H->Engines && !H->Closed;
}
static void HealthClock(BC250_START_HEALTH_STATE* H)
{
    if (HealthReady(H) && H->Visible && H->Mode && H->ReadySince==0)
        H->ReadySince=KeQueryInterruptTime();
}
void StartHealthBegin(BC250_DEVICE* Device, BOOLEAN Full)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    LONGLONG previous, next;
    KIRQL irql;
    // QPC is boot-monotonic across reloads. A serialized device start/reload
    // takes more than a QPC tick; the CAS also separates repeated test ticks
    // and multiple starts in the same loaded image. No wall clock is used.
    do {
        previous=InterlockedCompareExchange64(&g_StartGeneration,0,0);
        next=KeQueryPerformanceCounter(NULL).QuadPart;
        if (next<=previous) next=previous+1;
    } while (InterlockedCompareExchange64(&g_StartGeneration,next,previous)!=previous);
    StartHealthEnter(Device);
    KeAcquireSpinLock(&h->Lock,&irql);
    h->Generation=(ULONGLONG)next;
    h->Epoch=0;
    HealthInvalidate(h);
    h->Full=Full;
    h->Engines=FALSE;
    h->Visible=FALSE;
    h->Mode=FALSE;
    h->Closed=FALSE;
    h->ConfirmedGeneration=0;
    h->ConfirmedEpoch=0;
    KeReleaseSpinLock(&h->Lock,irql);
    StartHealthLeave(Device);
}
void StartHealthReady(BC250_DEVICE* Device, BOOLEAN Ready)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    StartHealthEnter(Device);
    KeAcquireSpinLock(&h->Lock,&irql);
    // A failure during startup remains sticky. A new StartDevice alone reopens.
    if (!h->Closed) h->Engines=Ready;
    HealthClock(h);
    KeReleaseSpinLock(&h->Lock,irql);
    StartHealthLeave(Device);
}
// Only the retained power coordinator calls this after actual restoration.
// Preserve the adapter generation but invalidate the entire pre-sleep witness.
// D0 arrival or a visibility request alone must never take this path.
void StartHealthResumeReady(BC250_DEVICE* Device, BOOLEAN Mode)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    StartHealthEnter(Device);
    KeAcquireSpinLock(&h->Lock,&irql);
    HealthInvalidate(h);
    h->ConfirmedGeneration=0;
    h->ConfirmedEpoch=0;
    h->Closed=FALSE;
    h->Engines=TRUE;
    h->Visible=FALSE;
    h->Mode=Mode;
    KeReleaseSpinLock(&h->Lock,irql);
    StartHealthLeave(Device);
}
void StartHealthFault(BC250_DEVICE* Device)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    KeAcquireSpinLock(&h->Lock,&irql);
    if (!h->Closed) HealthInvalidate(h);
    h->Closed=TRUE;
    h->Engines=FALSE;
    KeReleaseSpinLock(&h->Lock,irql);
}
void StartHealthClose(BC250_DEVICE* Device)
{
    StartHealthEnter(Device);
    StartHealthFault(Device);
    StartHealthLeave(Device);
}
void StartHealthRemove(BC250_DEVICE* Device)
{
    StartHealthClose(Device);
    // Adapter storage outlives all admitted no-synchronization readers. OS
    // hAdapter dispatch lifetime is still required to enter an escape at all.
    ExWaitForRundownProtectionRelease(&Device->StartHealth.Readers);
}
void StartHealthDisplayLocked(BC250_DEVICE* Device, BOOLEAN Visible, BOOLEAN Mode)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    KeAcquireSpinLock(&h->Lock,&irql);
    if (h->Visible!=Visible || h->Mode!=Mode) HealthInvalidate(h);
    h->Visible=Visible;
    h->Mode=Mode;
    HealthClock(h);
    KeReleaseSpinLock(&h->Lock,irql);
}
void StartHealthVisibilityLocked(BC250_DEVICE* Device, BOOLEAN Visible)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    KeAcquireSpinLock(&h->Lock,&irql);
    if (h->Visible!=Visible) HealthInvalidate(h);
    h->Visible=Visible;
    // Only a successful commit may reopen mode/path-power admission.
    HealthClock(h);
    KeReleaseSpinLock(&h->Lock,irql);
}
void StartHealthCompleted(BC250_DEVICE* Device, ULONG Sequence)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    KeAcquireSpinLock(&h->Lock,&irql);
    if (HealthReady(h) && h->Visible && h->Mode && Sequence!=0 &&
        !(Sequence&1) && Sequence!=h->LastSequence) {
        h->LastSequence=Sequence;
        ++h->Completed;
        h->LastCompletion=KeQueryInterruptTime();
    }
    KeReleaseSpinLock(&h->Lock,irql);
}
static void HealthSnapshot(const BC250_START_HEALTH_STATE* H, BC250_ESCAPE_START_HEALTH* Data)
{
    ULONGLONG now=KeQueryInterruptTime();
    Data->Flags=(H->Full ? BC250_START_HEALTH_FULL : 0) |
        (HealthReady(H) ? BC250_START_HEALTH_READY : 0) |
        (H->Visible && H->Mode && !H->Closed ? BC250_START_HEALTH_VISIBLE : 0);
    if (H->ConfirmedGeneration==H->Generation && H->ConfirmedEpoch==H->Epoch && H->Generation!=0)
        Data->Flags|=BC250_START_HEALTH_CONFIRMED;
    Data->Generation=H->Generation;
    Data->Epoch=H->Epoch;
    Data->Completed=H->Completed;
    Data->LastCompletionAgeMs=H->LastCompletion ? (now-H->LastCompletion)/10000ull : ~0ull;
    Data->ReadyAgeMs=H->ReadySince ? (now-H->ReadySince)/10000ull : 0;
}
void StartHealthRequest(BC250_DEVICE* Device, BC250_ESCAPE_START_HEALTH* Data, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    ULONGLONG generation=0, epoch=0;
    BOOLEAN confirm=Data->Op==BC250_START_HEALTH_CONFIRM;
    D3DDDI_ESCAPEFLAGS expectedFlags={0};
    if (confirm) expectedFlags.HardwareAccess=1;
    else expectedFlags.NoAdapterSynchronization=1;
    Data->Version=BC250_KMD_VERSION;
    Data->Flags=0;
    Data->Generation=Data->Epoch=Data->Completed=Data->ReadyAgeMs=0;
    Data->LastCompletionAgeMs=~0ull;
    Data->Status=BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus=(ULONG)status;
    if (Data->AbiVersion!=BC250_START_HEALTH_ABI || Data->Reserved[0] || Data->Reserved[1] ||
        (Data->Op!=BC250_START_HEALTH_READ && !confirm) ||
        EscapeFlags!=expectedFlags.Value) return;
    if (confirm && !Admin) {
        Data->Status=BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus=(ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    if (!ExAcquireRundownProtection(&h->Readers)) {
        Data->NtStatus=(ULONG)STATUS_DELETE_PENDING;
        return;
    }
    if (confirm) StartHealthEnter(Device);
    KeAcquireSpinLock(&h->Lock,&irql);
    HealthSnapshot(h,Data);
    status=STATUS_SUCCESS;
    if (confirm) {
        if (Data->ExpectedGeneration!=h->Generation || Data->ExpectedEpoch!=h->Epoch)
            status=STATUS_RETRY;
        else if ((Data->Flags&BC250_START_HEALTH_REQUIRED)!=BC250_START_HEALTH_REQUIRED ||
            !h->Completed || Data->ReadyAgeMs<BC250_START_HEALTH_MIN_MS ||
            Data->LastCompletionAgeMs>BC250_START_HEALTH_FRESH_MS)
            status=STATUS_DEVICE_NOT_READY;
        else { generation=h->Generation; epoch=h->Epoch; }
    }
    // Confirmation linearizes here. No spin lock is held over registry I/O.
    // Passive transitions serialize on Lifecycle; an asynchronous engine fault
    // immediately closes the snapshot and advances Epoch even during the flush.
    KeReleaseSpinLock(&h->Lock,irql);
    if (confirm && NT_SUCCESS(status)) {
        status=GuardConfirmStartDurable();
        KeAcquireSpinLock(&h->Lock,&irql);
        if (NT_SUCCESS(status)) {
            h->ConfirmedGeneration=generation;
            h->ConfirmedEpoch=epoch;
            // The preceding interval was durably confirmed. Do not certify a
            // later epoch or restore the counter if a DPC fault followed it.
            if (h->Generation!=generation || h->Epoch!=epoch) status=STATUS_RETRY;
        }
        HealthSnapshot(h,Data);
        KeReleaseSpinLock(&h->Lock,irql);
    }
    if (confirm) StartHealthLeave(Device);
    ExReleaseRundownProtection(&h->Readers);
    Data->NtStatus=(ULONG)status;
    Data->Status=NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
