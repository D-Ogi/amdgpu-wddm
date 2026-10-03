// GPU DWM interop switches: EnableGpuPresentBlit and EnableCddDwmInterop, on unless set to 0, closed by the
// driver itself after a boot that died with the path in use (docs/design/gpu-dwm-interop-switches.md).
//
// What lives where:
//   interop_policy.c   the start decision (host-tested, test/interop_policy_test.c)
//   this file          the registry around it, the session marker, the snapshot, the escape
//   wddm.c             the two consumers (DRIVERCAPS, Present) and the hooks: WddmStart calls InteropStart,
//                      the first interop Blt present of a DDI device InteropUserBegin, its DestroyDevice
//                      InteropUserEnd; pnp.c Bc250StopDevice calls InteropStop, Bc250RemoveDevice InteropRemove,
//                      Bc250SetPowerState InteropAdapterPower; entry.c DriverEntry InteropDriverInit, Bc250Unload
//                      InteropDriverUnload (the \Callback\PowerState registration)
//
// Settings, all REG_DWORD under Services\bc250kmd\Parameters:
//   EnableGpuPresentBlit, EnableCddDwmInterop   operator's; absent = 1 since 0.7.181, 0 off, else invalid
//   InteropSession        this boot's BootId, on disk while a DDI device that used the path is alive
//   InteropClosedReason   the reason the driver wrote both switches 0 (4, unclean); deleted when they open again
//   InteropLastState      effective bits | requested bits << 8, written every full start
//   InteropLastReason     enum bc250_interop_reason, written every full start
//   InteropLastEnd        how the last session ended: 1 device stop, 2 last user destroyed, 3 a system sleep or
//                         shutdown began (power callback), 4 the adapter went down for one (BC250_INTEROP_END_*)
// and the volatile subkey InteropBoot (value Marked = 1): this boot marked a session, so a marker the next start
// of the same boot finds is stale, not the trace of a dead machine.
//
// Why a user count and not the start: dxgkrnl does not stop the adapter at shutdown, so a marker written at start
// would outlive every clean reboot. Nor are DWM's DDI devices destroyed at a clean restart (BD-059: boot 168 to
// 169 found the marker with no end recorded), so the session also ends where a clean restart can be seen: at the
// start of a system power transition, while the registry is still up - the \Callback\PowerState notification
// (PO_CB_SYSTEM_STATE_LOCK, sent before the system set-power IRP), and as a second hook the adapter's D3 for a
// system action. Nothing marks again until the system is back in S0. A machine that dies with the path in use
// (hang, 0x116, power loss) dies before either, so a marker still there at the next start means exactly that.
// The transitions themselves are interop_policy.c's bc250_interop_session_step, host-tested; this file writes.
// Both switches stay start-latched: dxgkrnl reads DRIVERCAPS once per adapter start and keeps it, and the
// identity binding of wddm_allocation_identity.inc happens only at OpenAllocation, so a mid-start change would
// leave opens unbound. Closing takes effect at the next start, which after an unclean boot is this one.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"

#define INTEROP_SETTING_BLIT L"EnableGpuPresentBlit"
#define INTEROP_SETTING_CDD L"EnableCddDwmInterop"
#define INTEROP_SETTING_SESSION L"InteropSession"
#define INTEROP_SETTING_CLOSED L"InteropClosedReason"
#define INTEROP_SETTING_LAST_STATE L"InteropLastState"
#define INTEROP_SETTING_LAST_REASON L"InteropLastReason"
#define INTEROP_SETTING_LAST_END L"InteropLastEnd"
#define INTEROP_BOOT_KEY L"InteropBoot"
#define INTEROP_BOOT_VALUE L"Marked"

C_ASSERT(sizeof(BC250_ESCAPE_INTEROP) == 104);
C_ASSERT(BC250_INTEROP_SWITCH_BLIT == BC250_INTEROP_BLIT && BC250_INTEROP_SWITCH_CDD == BC250_INTEROP_CDD);
C_ASSERT(BC250_INTEROP_FLAG_UNCLEAN == BC250_INTEROP_DECISION_UNCLEAN);
C_ASSERT(BC250_INTEROP_FLAG_STALE == BC250_INTEROP_DECISION_STALE);
C_ASSERT(BC250_INTEROP_FLAG_CLOSED_BY_DRIVER == BC250_INTEROP_DECISION_CLOSED_BY_DRIVER);
C_ASSERT(BC250_INTEROP_END_NONE == BC250_INTEROP_SESSION_END_NONE &&
         BC250_INTEROP_END_STOP == BC250_INTEROP_SESSION_END_STOP &&
         BC250_INTEROP_END_USERS == BC250_INTEROP_SESSION_END_USERS &&
         BC250_INTEROP_END_SYSTEM_POWER == BC250_INTEROP_SESSION_END_SYSTEM_POWER &&
         BC250_INTEROP_END_ADAPTER_D3 == BC250_INTEROP_SESSION_END_ADAPTER_D3);
C_ASSERT(PowerActionSleep == 2 && PowerActionShutdownOff == 6);     // bc250_interop_system_action's range

// The adapters the power callback walks: listed by InteropStart, delisted by InteropStop and InteropRemove. Lock
// order: g_InteropListLock, then an adapter's Lock; nothing takes the list lock while holding an adapter's.
static LIST_ENTRY g_InteropList;
static KMUTEX g_InteropListLock;
static PCALLBACK_OBJECT g_InteropPowerObject;
static PVOID g_InteropPowerRegistration;
static volatile LONG g_InteropPowerAbove;       // notifications that came above PASSIVE_LEVEL: nothing written

static void InteropLock(BC250_INTEROP_STATE* S)
{
    KeWaitForSingleObject(&S->Lock, Executive, KernelMode, FALSE, NULL);
}
static void InteropUnlock(BC250_INTEROP_STATE* S)
{
    KeReleaseMutex(&S->Lock, FALSE);
}

// Work -> Snap. Caller holds Lock.
static void InteropPublish(BC250_INTEROP_STATE* S)
{
    KIRQL irql;
    KeAcquireSpinLock(&S->SnapLock, &irql);
    S->Snap = S->Work;
    KeReleaseSpinLock(&S->SnapLock, irql);
}

void InteropInitialize(BC250_DEVICE* Device)
{
    BC250_INTEROP_STATE* s = &Device->Interop;
    KeInitializeMutex(&s->Lock, 0);
    KeInitializeSpinLock(&s->SnapLock);
    InitializeListHead(&s->Link);
    s->Work.Reason = s->Snap.Reason = BC250_INTEROP_REASON_NOT_RUN;
}

static struct bc250_interop_value InteropQuery(PCWSTR Name)
{
    struct bc250_interop_value v;
    ULONG value = 0;
    NTSTATUS status = GuardQuerySetting(Name, &value);
    v.value = 0;
    if (NT_SUCCESS(status)) {
        v.state = BC250_INTEROP_PRESENT;
        v.value = value;
    } else if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
        v.state = BC250_INTEROP_ABSENT;
    } else {
        v.state = BC250_INTEROP_UNREADABLE;
        GuardLog("interop: reading %ws failed 0x%08X", Name, status);
    }
    return v;
}

static NTSTATUS InteropStore(PCWSTR Name, ULONG Value)
{
    NTSTATUS status = GuardStoreSetting(Name, Value);
    if (!NT_SUCCESS(status)) GuardLog("interop: writing %ws = %lu failed 0x%08X", Name, Value, status);
    return status;
}

static NTSTATUS InteropDelete(PCWSTR Name)
{
    NTSTATUS status = GuardDeleteSetting(Name);
    if (!NT_SUCCESS(status)) GuardLog("interop: deleting %ws failed 0x%08X", Name, status);
    return status;
}

static ULONG InteropSettingFlags(const struct bc250_interop_value* V, ULONG Absent, ULONG Unreadable)
{
    return V->state == BC250_INTEROP_ABSENT ? Absent : V->state == BC250_INTEROP_UNREADABLE ? Unreadable : 0;
}

static void InteropList(BC250_INTEROP_STATE* S, BOOLEAN In)
{
    KeWaitForSingleObject(&g_InteropListLock, Executive, KernelMode, FALSE, NULL);
    if (In && !S->Listed) InsertTailList(&g_InteropList, &S->Link);
    if (!In && S->Listed) RemoveEntryList(&S->Link);
    S->Listed = In;
    KeReleaseMutex(&g_InteropListLock, FALSE);
}

// Caller holds Lock. The volatile record first: a marker without it would read as a dead boot on a restart of this
// one. Best effort: a failure is counted and logged, and the next BEGIN or UP tries again.
static void InteropMark(BC250_INTEROP_STATE* S, PCSTR Why)
{
    ULONG bootId = SharedUserData->BootId;
    NTSTATUS status = GuardVolatileStore(INTEROP_BOOT_KEY, INTEROP_BOOT_VALUE, 1);
    if (NT_SUCCESS(status)) status = InteropStore(INTEROP_SETTING_SESSION, bootId);
    if (NT_SUCCESS(status)) {
        S->Session.marked = 1;
        S->Work.Marks++;
        S->Work.Flags |= BC250_INTEROP_FLAG_SESSION;
    } else {
        S->Work.MarkFailures++;
    }
    GuardLog("interop: session marked (%s) for boot %lu (users %lu), status 0x%08X", Why, bootId, S->Session.users,
             status);
}

// Caller holds Lock. A failed delete keeps the marker and marked: the next event that ends the session tries again.
static void InteropUnmark(BC250_INTEROP_STATE* S, ULONG End)
{
    static const char* const why[] = {"none", "device stop", "last user gone", "system power transition",
                                      "adapter down for a system action"};
    NTSTATUS status;
    (void)InteropStore(INTEROP_SETTING_LAST_END, End);
    status = InteropDelete(INTEROP_SETTING_SESSION);
    GuardLog("interop: session unmarked (%s), status 0x%08X", End < RTL_NUMBER_OF(why) ? why[End] : "?", status);
    if (!NT_SUCCESS(status)) return;
    S->Session.marked = 0;
    S->Work.Unmarks++;
    S->Work.LastEnd = End;
    S->Work.Flags &= ~BC250_INTEROP_FLAG_SESSION;
}

// Caller holds Lock: one event through the policy, its answer carried out, the snapshot published.
static void InteropEvent(BC250_INTEROP_STATE* S, unsigned int Event, PCSTR Why)
{
    struct bc250_interop_step step;
    bc250_interop_session_step(&S->Session, Event, &step);
    if (step.unmark) InteropUnmark(S, step.unmark);
    if (step.mark) InteropMark(S, Why);
    S->Work.Users = S->Session.users;
    S->Work.Flags = (S->Work.Flags & ~(BC250_INTEROP_FLAG_DOWN | BC250_INTEROP_FLAG_POWER_CALLBACK)) |
        (S->Session.down ? BC250_INTEROP_FLAG_DOWN : 0) |
        (g_InteropPowerRegistration ? BC250_INTEROP_FLAG_POWER_CALLBACK : 0);
    InteropPublish(S);
}

// \Callback\PowerState. PO_CB_SYSTEM_STATE_LOCK with Argument2 0: a system sleep or shutdown is imminent, sent
// before the system set-power IRP; 1: the system is back in S0. The registry calls need PASSIVE_LEVEL; a
// notification above it is counted and left to the adapter's D3.
static VOID InteropPowerCallback(PVOID Context, PVOID Argument1, PVOID Argument2)
{
    PLIST_ENTRY entry;
    unsigned int event;

    UNREFERENCED_PARAMETER(Context);
    if ((ULONG_PTR)Argument1 != PO_CB_SYSTEM_STATE_LOCK) return;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) { InterlockedIncrement(&g_InteropPowerAbove); return; }
    event = Argument2 == NULL ? BC250_INTEROP_EVENT_SYSTEM_DOWN : BC250_INTEROP_EVENT_UP;
    KeWaitForSingleObject(&g_InteropListLock, Executive, KernelMode, FALSE, NULL);
    for (entry = g_InteropList.Flink; entry != &g_InteropList; entry = entry->Flink) {
        BC250_INTEROP_STATE* s = CONTAINING_RECORD(entry, BC250_INTEROP_STATE, Link);
        InteropLock(s);
        GuardLog("interop: system power state lock %s (users %lu, marked %d)", event == BC250_INTEROP_EVENT_UP ?
                 "released, back in S0" : "taken, sleep or shutdown imminent", s->Session.users, s->Session.marked);
        InteropEvent(s, event, "system back in S0");
        InteropUnlock(s);
    }
    KeReleaseMutex(&g_InteropListLock, FALSE);
}

// DriverEntry, before the DDI table is handed over. Never fails the load: without the callback the adapter's D3
// is the only hook, and the escape shows the registration missing (no POWER_CALLBACK flag).
void InteropDriverInit(void)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    NTSTATUS status;

    InitializeListHead(&g_InteropList);
    KeInitializeMutex(&g_InteropListLock, 0);
    RtlInitUnicodeString(&name, L"\\Callback\\PowerState");
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    status = ExCreateCallback(&g_InteropPowerObject, &attributes, FALSE, TRUE);
    if (NT_SUCCESS(status)) {
        g_InteropPowerRegistration = ExRegisterCallback(g_InteropPowerObject, InteropPowerCallback, NULL);
        if (g_InteropPowerRegistration == NULL) {
            ObDereferenceObject(g_InteropPowerObject);
            g_InteropPowerObject = NULL;
            status = STATUS_UNSUCCESSFUL;
        }
    }
    GuardLog("interop: \\Callback\\PowerState registration 0x%08X", status);
}

// Bc250Unload: no adapter is started any more, so the list is empty; ExUnregisterCallback returns after a
// notification in progress has finished with this image's code.
void InteropDriverUnload(void)
{
    if (g_InteropPowerRegistration) ExUnregisterCallback(g_InteropPowerRegistration);
    g_InteropPowerRegistration = NULL;
    if (g_InteropPowerObject) ObDereferenceObject(g_InteropPowerObject);
    g_InteropPowerObject = NULL;
    if (g_InteropPowerAbove) GuardLog("interop: %ld power notifications came above PASSIVE_LEVEL", g_InteropPowerAbove);
}

// WddmStart, before the adapter state is published: decides both switches for this start. PASSIVE_LEVEL.
// Never fails the start; every read or write failure closes toward "off".
void InteropStart(BC250_DEVICE* Device, _Out_ BOOLEAN* GpuPresent, _Out_ BOOLEAN* CddInterop)
{
    BC250_INTEROP_STATE* s = &Device->Interop;
    struct bc250_interop_inputs in;
    struct bc250_interop_decision d;
    struct bc250_interop_value lastEnd;
    ULONG bootRecord = 0, bootId = SharedUserData->BootId;
    NTSTATUS persist = STATUS_SUCCESS;

    *GpuPresent = *CddInterop = FALSE;
    InteropList(s, TRUE);
    InteropLock(s);
    RtlZeroMemory(&s->Work, sizeof(s->Work));
    RtlZeroMemory(&s->Session, sizeof(s->Session));
    in.blit = InteropQuery(INTEROP_SETTING_BLIT);
    in.cdd = InteropQuery(INTEROP_SETTING_CDD);
    in.session = InteropQuery(INTEROP_SETTING_SESSION);
    in.closed = InteropQuery(INTEROP_SETTING_CLOSED);
    lastEnd = InteropQuery(INTEROP_SETTING_LAST_END);
    in.boot_record = NT_SUCCESS(GuardVolatileQuery(INTEROP_BOOT_KEY, INTEROP_BOOT_VALUE, &bootRecord)) && bootRecord == 1;
    bc250_interop_decide(&in, &d);

    if (d.persist_close) {
        // Durable first, like DPM's PersistFallback: the next start must not open them again by itself.
        persist = InteropStore(INTEROP_SETTING_BLIT, 0);
        if (NT_SUCCESS(persist)) persist = InteropStore(INTEROP_SETTING_CDD, 0);
        if (NT_SUCCESS(persist)) persist = InteropStore(INTEROP_SETTING_CLOSED, d.reason);
    }
    if (d.clear_closed) (void)InteropDelete(INTEROP_SETTING_CLOSED);
    // A failed close keeps the marker: the next start sees the same dead session and tries again.
    if (d.clear_session && NT_SUCCESS(persist)) (void)InteropDelete(INTEROP_SETTING_SESSION);
    (void)InteropStore(INTEROP_SETTING_LAST_STATE, d.effective | (d.requested << 8));
    (void)InteropStore(INTEROP_SETTING_LAST_REASON, d.reason);

    s->Work.Flags = BC250_INTEROP_FLAG_VALID | d.flags |
        (d.persist_close ? (NT_SUCCESS(persist) ? BC250_INTEROP_FLAG_PERSISTED : BC250_INTEROP_FLAG_PERSIST_FAILED) : 0) |
        InteropSettingFlags(&in.blit, BC250_INTEROP_FLAG_BLIT_ABSENT, BC250_INTEROP_FLAG_BLIT_UNREADABLE) |
        InteropSettingFlags(&in.cdd, BC250_INTEROP_FLAG_CDD_ABSENT, BC250_INTEROP_FLAG_CDD_UNREADABLE);
    s->Work.Requested = d.requested;
    s->Work.Effective = d.effective;
    s->Work.Reason = d.reason;
    s->Work.ClosedReason = d.closed_reason;
    s->Work.BlitSetting = in.blit.value;
    s->Work.CddSetting = in.cdd.value;
    s->Work.BootId = bootId;
    s->Work.SessionBootId = in.session.state == BC250_INTEROP_PRESENT ? in.session.value : 0;
    s->Work.PreviousEnd = lastEnd.state == BC250_INTEROP_PRESENT ? lastEnd.value : BC250_INTEROP_END_NONE;
    s->Work.Generation = Device->StartHealth.Generation;
    if (g_InteropPowerRegistration) s->Work.Flags |= BC250_INTEROP_FLAG_POWER_CALLBACK;
    InteropPublish(s);
    InteropUnlock(s);

    *GpuPresent = (d.effective & BC250_INTEROP_BLIT) != 0;
    *CddInterop = (d.effective & BC250_INTEROP_CDD) != 0;
    if (d.persist_close)
        GuardLog("interop: the last boot died with the GPU DWM path in use (marker of boot %lu, this boot %lu): "
                 "EnableGpuPresentBlit and EnableCddDwmInterop closed, reason %lu (%s), durable status 0x%08X",
                 s->Work.SessionBootId, bootId, d.reason, bc250_interop_reason_name(d.reason), persist);
    else if (d.flags & BC250_INTEROP_DECISION_UNCLEAN)
        GuardLog("interop: the last boot died in a session (marker of boot %lu); both switches were already 0",
                 s->Work.SessionBootId);
    else if (d.flags & BC250_INTEROP_DECISION_STALE)
        GuardLog("interop: a marker of this boot (%lu) left by an earlier start was cleared", s->Work.SessionBootId);
    GuardLog("interop: requested 0x%lX effective 0x%lX reason %lu (%s) closed-reason %lu flags 0x%lX "
             "blit %lu cdd %lu previous-end %lu",
             d.requested, d.effective, d.reason, bc250_interop_reason_name(d.reason), d.closed_reason,
             s->Work.Flags, in.blit.value, in.cdd.value, s->Work.PreviousEnd);
}

// The first interop Blt present of a DDI device (wddm.c): mark the session before the path is used. Present is
// PASSIVE_LEVEL; anything above is refused (FALSE) and the caller does not count the device. The mark is best
// effort: a failure is counted and logged, the switches stay as the start latched them. During a system power
// transition the device is counted and nothing is marked.
BOOLEAN InteropUserBegin(BC250_DEVICE* Device)
{
    BC250_INTEROP_STATE* s = &Device->Interop;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return FALSE;
    InteropLock(s);
    InteropEvent(s, BC250_INTEROP_EVENT_BEGIN, "first interop present of a device");
    InteropUnlock(s);
    return TRUE;
}

// DestroyDevice of a DDI device InteropUserBegin counted. PASSIVE_LEVEL.
void InteropUserEnd(BC250_DEVICE* Device)
{
    BC250_INTEROP_STATE* s = &Device->Interop;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    InteropLock(s);
    InteropEvent(s, BC250_INTEROP_EVENT_END, "");
    InteropUnlock(s);
}

// Bc250StopDevice, after WddmStop: an orderly stop is not a death, whatever devices dxgkrnl left.
void InteropStop(BC250_DEVICE* Device)
{
    BC250_INTEROP_STATE* s = &Device->Interop;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    InteropList(s, FALSE);
    InteropLock(s);
    InteropEvent(s, BC250_INTEROP_EVENT_STOP, "");
    InteropUnlock(s);
}

// Bc250RemoveDevice, before the device's memory goes: the power callback must not find it. Idempotent.
void InteropRemove(BC250_DEVICE* Device)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL) InteropList(&Device->Interop, FALSE);
}

// Bc250SetPowerState for the adapter, PASSIVE_LEVEL: down for a system action (sleep, hibernate, shutdown) ends
// the session like the power callback; D0 is the way back. Device-level transitions (PowerActionNone) leave it.
void InteropAdapterPower(BC250_DEVICE* Device, DEVICE_POWER_STATE State, POWER_ACTION Action)
{
    BC250_INTEROP_STATE* s = &Device->Interop;
    unsigned int event;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (State == PowerDeviceD0) event = BC250_INTEROP_EVENT_UP;
    else if (bc250_interop_system_action((unsigned int)Action)) event = BC250_INTEROP_EVENT_ADAPTER_DOWN;
    else return;
    InteropLock(s);
    if (event == BC250_INTEROP_EVENT_UP && !s->Session.down) { InteropUnlock(s); return; }
    GuardLog("interop: adapter to D%d for action %d (users %lu, marked %d)", (int)State - 1, (int)Action,
             s->Session.users, s->Session.marked);
    InteropEvent(s, event, "adapter back in D0");
    InteropUnlock(s);
}

void InteropLogSummary(BC250_DEVICE* Device)
{
    BC250_INTEROP_STATE* s = &Device->Interop;
    BC250_INTEROP_SNAP snap;
    KIRQL irql;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    snap = s->Snap;
    KeReleaseSpinLock(&s->SnapLock, irql);
    GuardLog("interop summary: requested 0x%lX effective 0x%lX reason %lu (%s) closed-reason %lu flags 0x%lX "
             "users %lu marks %lu unmarks %lu mark-failures %lu last-end %lu",
             snap.Requested, snap.Effective, snap.Reason, bc250_interop_reason_name(snap.Reason), snap.ClosedReason,
             snap.Flags, snap.Users, snap.Marks, snap.Unmarks, snap.MarkFailures, snap.LastEnd);
}

// BC250_ESCAPE_RUN_INTEROP. Software state only: NoAdapterSynchronization=1, open to every caller.
void InteropRequest(BC250_DEVICE* Device, BC250_ESCAPE_INTEROP* Data, ULONG EscapeFlags)
{
    BC250_INTEROP_STATE* s = &Device->Interop;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    BC250_INTEROP_SNAP snap;
    KIRQL irql;

    expectedFlags.NoAdapterSynchronization = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)STATUS_INVALID_PARAMETER;
    Data->Flags = Data->Requested = Data->Effective = Data->Reason = Data->ClosedReason = 0;
    Data->BlitSetting = Data->CddSetting = Data->BootId = Data->SessionBootId = 0;
    Data->Users = Data->Marks = Data->Unmarks = Data->MarkFailures = Data->PreviousEnd = Data->LastEnd = 0;
    Data->Generation = 0;
    if (Data->AbiVersion != BC250_INTEROP_ABI || Data->Op != BC250_INTEROP_OP_READ || Data->Reserved[0] ||
        Data->Reserved[1] || EscapeFlags != expectedFlags.Value) return;
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    KeAcquireSpinLock(&s->SnapLock, &irql);
    snap = s->Snap;
    KeReleaseSpinLock(&s->SnapLock, irql);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);
    Data->Flags = snap.Flags;
    Data->Requested = snap.Requested;
    Data->Effective = snap.Effective;
    Data->Reason = snap.Reason;
    Data->ClosedReason = snap.ClosedReason;
    Data->BlitSetting = snap.BlitSetting;
    Data->CddSetting = snap.CddSetting;
    Data->BootId = snap.BootId;
    Data->SessionBootId = snap.SessionBootId;
    Data->Users = snap.Users;
    Data->Marks = snap.Marks;
    Data->Unmarks = snap.Unmarks;
    Data->MarkFailures = snap.MarkFailures;
    Data->PreviousEnd = snap.PreviousEnd;
    Data->LastEnd = snap.LastEnd;
    Data->Generation = snap.Generation;
    Data->NtStatus = (ULONG)STATUS_SUCCESS;
    Data->Status = BC250_ESCAPE_STATUS_DONE;
}
