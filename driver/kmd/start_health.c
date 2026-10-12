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
    // BD-120. Both are zero in the device context already and StartHealthBegin clears them before anything can
    // read them, but the two marks are stated here with Closed, like the rest of this file.
    Device->StartHealth.PendingReset=FALSE;
    Device->StartHealth.Recovered=FALSE;
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
    h->PendingReset=FALSE;
    h->Recovered=FALSE;
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
// BD-120: the pending-reset mark goes with that witness, but Recovered does not. A resume does not make a
// clock/voltage or CU request that hung node 0 before the sleep safe to confirm; only a new start does.
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
    h->PendingReset=FALSE;
    h->Engines=TRUE;
    h->Visible=FALSE;
    h->Mode=Mode;
    KeReleaseSpinLock(&h->Lock,irql);
    StartHealthLeave(Device);
}
// BD-120. Every close of the witness, differing only in what it does to the pending-reset mark - the record of
// a close that a node-0 engine reset still owes an answer to:
//   Mark > 0  the node-0 hardware fence timeout latch's own close (wddm.c, WddmLatchSubmitTimeoutLocked). The
//             mark is taken only over a witness that was open with its engines up, so a close that a real
//             fault made first - including a startup engine failure, which StartHealthReady keeps sticky -
//             keeps that fault's stickiness and the reset finds nothing of its own to undo.
//   Mark == 0 every real fault. It takes the mark away as well, whichever of the two ran first, so an engine
//             reset never reopens a witness that has seen a fault of its own.
//   Mark < 0  a close that says nothing about node 0: today only a ResetEngine refusal on another node. The
//             witness closes, as it always did on a refusal, but node 0's own reset keeps the answer it is
//             owed. Such a close never creates a mark.
static void HealthCloseLocked(BC250_START_HEALTH_STATE* H, int Mark)
{
    if (!H->Closed) {
        HealthInvalidate(H);
        if (Mark>0 && H->Engines) H->PendingReset=TRUE;
    }
    if (Mark==0) H->PendingReset=FALSE;
    H->Closed=TRUE;
    H->Engines=FALSE;
}
void StartHealthFault(BC250_DEVICE* Device)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    KeAcquireSpinLock(&h->Lock,&irql);
    HealthCloseLocked(h,0);
    KeReleaseSpinLock(&h->Lock,irql);
}
// BD-120. The timeout latch's close. No Lifecycle mutex, like StartHealthFault: the caller holds wddm->Lock at
// DISPATCH_LEVEL.
void StartHealthFaultPendingReset(BC250_DEVICE* Device)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    KeAcquireSpinLock(&h->Lock,&irql);
    HealthCloseLocked(h,1);
    KeReleaseSpinLock(&h->Lock,irql);
}
// BD-120. A RECOVERED engine reset (wddm.c, the DxgkDdiResetEngine success path, under wddm->Lock in the same
// transaction as the epoch validation and GfxReopenAfterAbort): node 0 takes work again and the display never
// stopped, so the witness reports the real state again instead of the flags=1 that asked the lab for a restart
// it did not need. A new epoch starts, READY comes back with the engines, VISIBLE comes back with it because
// Visible and Mode belong to the display and the hang never touched them, and StartHealthCompleted counts
// again, so the flags reach a healthy boot's value once the usual ready age and freshness hold.
//
// CONFIRMED survives. Three reasons, which is also why ConfirmedGeneration is not touched here:
//  - BD-098: CONFIRMED reports the start, not the current interval. A recovered reset advances Epoch exactly as
//    a mode set or a visibility change does, and those already keep a durably confirmed start confirmed.
//  - The boot guard counts starts that never reached a healthy desktop (guard.c, UnconfirmedStarts). This start
//    did reach one, the registry says so durably, and a hang hours later cannot make that false. Dropping
//    CONFIRMED would send the logon task to confirm a start that is already confirmed and tell the guard
//    nothing new.
//  - CONFIRM itself still certifies one interval only: it carries ExpectedGeneration/ExpectedEpoch, and the
//    re-check after the registry flush compares both again, so a CONFIRM in flight across this reopen returns
//    STATUS_RETRY and leaves ConfirmedEpoch on the interval it actually measured, never on the new epoch.
//
// What does not come back is the CU-mode and the DPM confirmation of this start (Recovered, read by
// StartHealthIsReady and by the confirmation tail of StartHealthRequest). The witness re-arms, so the desktop's
// own gate passes again without a restart, but a clock/voltage or CU request that hung node 0 does not become
// the durable default on the strength of the start it hung. Until this change that followed from the witness
// staying closed for the rest of the boot; it is now a rule of its own, so the next start still falls back.
//
// CONFIRMED does become visible again for a confirmation that returned STATUS_RETRY across this reopen: the
// start was durably confirmed before the hang (GuardConfirmStartDurable already wrote the registry), and
// CONFIRMED reports the start. Only ConfirmedEpoch stays on the interval that CONFIRM measured.
//
// Of the three answers, only REOPENED undoes a close, and only the one this reset owns (PendingReset). REARMED
// is the other ordering, and the normal one: Bc250SubmitBudgetMs is never shorter than TdrDelay
// (submit_watchdog.h), so dxgkrnl usually reaches ResetEngine before our own latch fires, and the witness is
// then still open. Nothing is there to reopen, but the interval it is reporting contains a GPU hang that no
// epoch of ours ever recorded, so it ends here too: a new epoch, no completions, the ready age re-clocked. The
// epoch rule - a recovered reset always starts a new interval - therefore holds in both orderings, and a
// CONFIRM in flight cannot certify an interval that contained the hang. The new epoch is returned in Epoch for
// the caller's log line; STICKY changes nothing and reports the epoch as it stands.
BC250_START_HEALTH_RECOVERY StartHealthRecovered(BC250_DEVICE* Device, _Out_ ULONGLONG* Epoch)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    BC250_START_HEALTH_RECOVERY result;
    KIRQL irql;
    KeAcquireSpinLock(&h->Lock,&irql);
    // This start needed a GPU reset, whatever the witness was in. CU mode and DPM read this below.
    h->Recovered=TRUE;
    if (!h->Closed) {
        HealthInvalidate(h);
        HealthClock(h);     // Engines is left as it stands: a startup engine failure is not this reset's to fix
        result=BC250_START_HEALTH_RECOVERY_REARMED;
    } else if (h->PendingReset) {
        HealthInvalidate(h);
        h->Closed=FALSE;
        h->PendingReset=FALSE;
        h->Engines=TRUE;
        HealthClock(h);
        result=BC250_START_HEALTH_RECOVERY_REOPENED;
    } else result=BC250_START_HEALTH_RECOVERY_STICKY;
    *Epoch=h->Epoch;
    KeReleaseSpinLock(&h->Lock,irql);
    return result;
}
void StartHealthClose(BC250_DEVICE* Device)
{
    StartHealthEnter(Device);
    StartHealthFault(Device);
    StartHealthLeave(Device);
}
// BD-120. The close of a ResetEngine refusal on a node that is not node 0 (wddm.c, refuseReset). It is a close
// like any other refusal's, but it leaves node 0's pending-reset mark standing: node 1 is never soft-recovered
// by policy (Bc250SoftRecoveryWanted), so its refusal is no evidence about node 0, and clearing the mark there
// would leave the node-0 reset that follows with nothing to reopen - the BD-120 symptom, unfixed.
void StartHealthCloseKeepPendingReset(BC250_DEVICE* Device)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    StartHealthEnter(Device);
    KeAcquireSpinLock(&h->Lock,&irql);
    HealthCloseLocked(h,-1);
    KeReleaseSpinLock(&h->Lock,irql);
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
    // CONFIRMED reports the start, not the interval (BD-098): a durably confirmed generation stays confirmed through
    // the mode sets and visibility changes that advance Epoch. A fault or a close (Closed) takes it away; a resume and
    // a new start zero ConfirmedGeneration. CONFIRM itself still certifies only the current interval (ExpectedEpoch).
    if (H->ConfirmedGeneration==H->Generation && H->Generation!=0 && !H->Closed)
        Data->Flags|=BC250_START_HEALTH_CONFIRMED;
    Data->Generation=H->Generation;
    Data->Epoch=H->Epoch;
    Data->Completed=H->Completed;
    Data->LastCompletionAgeMs=H->LastCompletion ? (now-H->LastCompletion)/10000ull : ~0ull;
    Data->ReadyAgeMs=H->ReadySince ? (now-H->ReadySince)/10000ull : 0;
}
// Serialize with CONFIRM, retain the pre-stop witness, then close admission in
// the same critical section. No registry access occurs under the spin lock.
void StartHealthStop(BC250_DEVICE* Device, BC250_ESCAPE_START_HEALTH* Snapshot)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    RtlZeroMemory(Snapshot,sizeof(*Snapshot));
    StartHealthEnter(Device);
    KeAcquireSpinLock(&h->Lock,&irql);
    HealthSnapshot(h,Snapshot);
    // BD-120: a stop is a real close of this start, so it goes through the one close with Mark 0 and
    // takes the pending-reset mark away: a node-0 reset can never reopen a witness that a stop closed.
    HealthCloseLocked(h,0);
    KeReleaseSpinLock(&h->Lock,irql);
    StartHealthLeave(Device);
}
// The healthy milestone of a CONFIRM, for the CU mode's own confirmation (cumode.c) and the DPM one (dpm.c):
// the same flags, completions, age and freshness, without the durable start-counter write.
// BD-120: and never on a start that needed an engine reset (Recovered). The witness re-arms after a recovered
// reset, so this milestone would hold again a minute later, and these two callers would then write the very
// clock/voltage or CU request that hung node 0 as the next boot's default. The desktop's own confirmation is
// not affected; only these two hardware requests are held back, exactly as the closed witness held them back
// before the re-arm existed.
BOOLEAN StartHealthIsReady(BC250_DEVICE* Device, _Out_ ULONGLONG* Generation)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    BC250_ESCAPE_START_HEALTH data;
    BOOLEAN ready;
    KIRQL irql;
    RtlZeroMemory(&data,sizeof(data));
    KeAcquireSpinLock(&h->Lock,&irql);
    HealthSnapshot(h,&data);
    ready=(data.Flags&BC250_START_HEALTH_REQUIRED)==BC250_START_HEALTH_REQUIRED && h->Completed && !h->Recovered &&
        data.ReadyAgeMs>=BC250_START_HEALTH_MIN_MS && data.LastCompletionAgeMs<=BC250_START_HEALTH_FRESH_MS;
    *Generation=h->Generation;
    KeReleaseSpinLock(&h->Lock,irql);
    return ready;
}
void StartHealthRequest(BC250_DEVICE* Device, BC250_ESCAPE_START_HEALTH* Data, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_START_HEALTH_STATE* h=&Device->StartHealth;
    KIRQL irql;
    NTSTATUS status=STATUS_INVALID_PARAMETER;
    ULONGLONG generation=0, epoch=0;
    // BD-120: whether this start needed an engine reset, read with the snapshot after the durable write.
    BOOLEAN recovered=FALSE;
    BOOLEAN confirm=Data->Op==BC250_START_HEALTH_CONFIRM;
    // Both operations take NoAdapterSynchronization=1 and every other flag zero (0.7.213). CONFIRM asked for
    // HardwareAccess up to 0.7.212, which buys the Level Two exclusion: dxgkrnl suspends the GPU scheduler for
    // up to one VSync while this handler holds Lifecycle across a registry flush (GuardConfirmStartDurable),
    // and an administrator logon retries it every 5.5 s for two minutes (tools/release/installer/start-confirm.ps1).
    // Nothing here is a register, a mailbox or state WddmStop frees: only StartHealth under its own spin lock,
    // mutex and rundown, then the registry. The CU mode's own CONFIRM has run the same milestone over the same
    // spin lock with NoAdapterSynchronization alone since it existed (cumode.c, StartHealthIsReady), and so has
    // the DPM one (dpm.c) - and the three share CuModeConfirm/DpmConfirm below.
    // The old flag word stays admitted for one release, so a CLI, DLL or overlay of 0.7.212 or older still
    // confirms a start; every other combination is refused, as before.
    D3DDDI_ESCAPEFLAGS expectedFlags={0}, legacyFlags={0};
    expectedFlags.NoAdapterSynchronization=1;
    legacyFlags.HardwareAccess=1;
    Data->Version=BC250_KMD_VERSION;
    Data->Flags=0;
    Data->Generation=Data->Epoch=Data->Completed=Data->ReadyAgeMs=0;
    Data->LastCompletionAgeMs=~0ull;
    Data->Status=BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus=(ULONG)status;
    if (Data->AbiVersion!=BC250_START_HEALTH_ABI || Data->Reserved[0] || Data->Reserved[1] ||
        (Data->Op!=BC250_START_HEALTH_READ && !confirm) ||
        (EscapeFlags!=expectedFlags.Value && !(confirm && EscapeFlags==legacyFlags.Value))) return;
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
        recovered=h->Recovered;
        HealthSnapshot(h,Data);
        KeReleaseSpinLock(&h->Lock,irql);
        // A durably confirmed start also confirms its 40 CU request (cumode.c). Its own failure is
        // logged there and costs only a fallback at the next start, never this confirmation.
        // BD-120: not on a start that needed an engine reset. The desktop is confirmed - that is the line above
        // and the durable counter - but the request that hung node 0 is left pending, so the next start falls
        // back to the safe clock/voltage and 24 CU, as it did while the closed witness blocked this path.
        if (NT_SUCCESS(status) && recovered)
            GuardLog("health: start confirmed, CU mode and DPM left pending: this start needed a GPU reset");
        if (NT_SUCCESS(status) && !recovered) (void)CuModeConfirm(Device,"start-health");
        // The same for a DPM start (dpm.c).
        if (NT_SUCCESS(status) && !recovered) (void)DpmConfirm(Device,"start-health");
    }
    if (confirm) StartHealthLeave(Device);
    ExReleaseRundownProtection(&h->Readers);
    Data->NtStatus=(ULONG)status;
    Data->Status=NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
