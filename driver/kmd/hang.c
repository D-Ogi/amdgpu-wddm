// Hang evidence (docs/design/hang-detector.md): progress records that a kernel dump can be searched for, and a
// test-only detector that turns a machine whose threads stopped running into a bugcheck with a dump.
//
//   <service key>\Parameters
//     EnableHangBugcheck   REG_DWORD  1 = arm the detector at the next device start (full WDDM only). Default 0,
//                                     closed by every install: with 0 no thread, timer or callback is created.
//     HangBugcheckSeconds  REG_DWORD  how long the heartbeat may stand still, 5..60, default 10 (outside: 10).
//
// Why this exists: twice on 2026-09-29 unit A stopped serving user mode under a second after a game loaded our
// user-mode driver: no bugcheck, no 0x133, no TDR, no dump, steady power for minutes. The
// cause is not known. The next occurrence should leave a dump that names what was running.
//
// Three parts, and what each may do:
//   the recorders (any IRQL)       interlocked stores into g_Bc250Progress, nothing else: no lock, no allocation,
//                                  no register, no log. They run in the ISR and in every DPC this driver has.
//   the heartbeat thread (PASSIVE) a normal-priority system thread counting every 500 ms. It stands for "an ordinary
//                                  thread still gets a processor", which is what SSH and the desktop lacked.
//   the check DPC (DISPATCH)       a 1 s periodic timer. If the heartbeat has not moved for HangBugcheckSeconds while
//                                  the device is started and in D0, KeBugCheckEx. Nothing before that call takes a
//                                  lock, touches MMIO, allocates or logs: whatever wedged the machine may hold any of
//                                  them.
// And the dump: a KbCallbackAddPages callback adds the progress structure and the log ring to a kernel or complete
// memory dump. Both are static storage in this image, so a kernel dump normally holds them anyway; the callback
// makes it explicit. It runs at HIGH_LEVEL and only names addresses.
//
// Limits, plainly: the check DPC must itself run. If every processor is held at DISPATCH_LEVEL or above (a spin
// on a lock every CPU wants, an interrupt storm on all of them), the timer never fires and this detects nothing;
// only the keyboard crash or NMI route remains then. It also detects starvation, not deadlock: a thread blocked on
// our mutex leaves the heartbeat running.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "paging_drain.h"

#define BC250_HANG_BUGCHECK_CODE 0xBC250BADul   // driver-defined, lab diagnostic only (KeBugCheckEx contract)
#define BC250_HANG_HEARTBEAT_MS 500
#define BC250_HANG_CHECK_MS 1000
#define BC250_HANG_HEARTBEAT_PRIORITY 8         // normal: the base priority of an ordinary process's threads
#define BC250_HANG_DUMP_RANGES 3

// Initialized at compile time, so the signature is in the image from load on and a dump of any age can be
// searched for it ("s -q <range> 4752503035324342" in kd), armed or not.
BC250_PROGRESS g_Bc250Progress = {
    BC250_PROGRESS_SIGNATURE, BC250_PROGRESS_VERSION, (unsigned long)sizeof(BC250_PROGRESS), BC250_KMD_VERSION,
    ProgressSiteCount,
};

// ---- the recorders ----------------------------------------------------------------------------------------------

void ProgressEnter(BC250_PROGRESS_SITE_ID Site)
{
    BC250_PROGRESS_SITE* site;

    if ((ULONG)Site >= ProgressSiteCount) return;
    site = &g_Bc250Progress.Sites[Site];
    InterlockedExchange64(&site->LastEntryTime, (LONG64)KeQueryInterruptTime());
    InterlockedExchange(&site->LastCpu, (LONG)KeGetCurrentProcessorNumberEx(NULL));
    InterlockedIncrement(&site->Entries);
}

// As ProgressEnter, with the call's input stored before the entry is counted: an open call whose Value still
// shows the previous completion leaves its own argument here (unless a later caller has entered since).
void ProgressEnterInput(BC250_PROGRESS_SITE_ID Site, LONG Input)
{
    if ((ULONG)Site >= ProgressSiteCount) return;
    InterlockedExchange(&g_Bc250Progress.Sites[Site].Input, Input);
    ProgressEnter(Site);
}

void ProgressExit(BC250_PROGRESS_SITE_ID Site, LONG Value)
{
    BC250_PROGRESS_SITE* site;

    if ((ULONG)Site >= ProgressSiteCount) return;
    site = &g_Bc250Progress.Sites[Site];
    InterlockedExchange(&site->Value, Value);
    InterlockedExchange64(&site->LastExitTime, (LONG64)KeQueryInterruptTime());
    InterlockedIncrement(&site->Exits);
}

// From ih.c's Note, once per decoded vector. A fault storm (a source nobody acknowledges) shows as one BySource
// slot running away; Recent keeps the order of the last sixteen, best-effort: the slot's sequence is cleared,
// the slot written, then the sequence published, so a crash in between leaves that slot marked invalid rather
// than showing old contents as the newest vector. IH consumption is serialized, so one writer is the norm.
void ProgressIhVector(ULONG ClientId, ULONG SourceId)
{
    BC250_PROGRESS_IH* ih = &g_Bc250Progress.Ih;
    LONG number = InterlockedIncrement(&ih->RecentNext);
    ULONG slot = (ULONG)(number - 1) & (BC250_PROGRESS_IH_RECENT - 1u);

    InterlockedExchange(&ih->RecentSeq[slot], 0);
    InterlockedExchange(&ih->Recent[slot], (LONG)(((ClientId & 0xFFu) << 8) | (SourceId & 0xFFu)));
    InterlockedExchange(&ih->RecentSeq[slot], number);
    InterlockedIncrement(&ih->BySource[SourceId & (BC250_PROGRESS_IH_SOURCES - 1u)]);
    InterlockedIncrement(&ih->Vectors);
}

void ProgressIhDone(ULONG Passes, BOOLEAN Requeued)
{
    if (Passes > 1) InterlockedExchangeAdd(&g_Bc250Progress.Ih.Handoffs, (LONG)(Passes - 1));
    if (Requeued) InterlockedIncrement(&g_Bc250Progress.Ih.Requeues);
}

void ProgressDrainDone(ULONG Iterations, ULONG Retired, ULONG Exit)
{
    BC250_PROGRESS_DRAIN* drain = &g_Bc250Progress.Drain;
    LONG seen;

    InterlockedExchangeAdd(&drain->Iterations, (LONG)Iterations);
    InterlockedExchangeAdd(&drain->Retired, (LONG)Retired);
    if (Exit == PagingDrainExitQuota) InterlockedIncrement(&drain->QuotaExits);
    InterlockedExchange(&drain->LastIterations, (LONG)Iterations);
    InterlockedExchange(&drain->LastExit, (LONG)Exit);
    // A maximum without a lock. Each failed exchange means another invocation raised it; at most one retry per
    // concurrent drain caller, of which there are a handful.
    for (seen = drain->MaxIterations; (LONG)Iterations > seen;) {
        LONG previous = InterlockedCompareExchange(&drain->MaxIterations, (LONG)Iterations, seen);
        if (previous == seen) break;
        seen = previous;
    }
}

// ---- the detector -----------------------------------------------------------------------------------------------

typedef struct _BC250_HANG_DUMP_RANGE {
    const void* Base;
    SIZE_T Bytes;
} BC250_HANG_DUMP_RANGE;

// Static, like the progress record: the DPC and the thread touch nothing whose lifetime a device stop or remove
// could end. Created/CallbackRegistered/Thread are changed only by Start/Stop, which dxgkrnl serializes.
typedef struct _BC250_HANG {
    BOOLEAN Created;
    BOOLEAN CallbackRegistered;
    PKTHREAD Thread;
    KEVENT StopEvent;
    KTIMER Timer;
    KDPC Dpc;
    volatile LONG InCheck;              // a periodic timer's DPC may be queued again while it runs on another CPU
    volatile LONG Reprime;              // Pause/Resume: the next check starts a fresh window
    BC250_HANG_WATCH_STATE State;       // the check DPC's alone, under InCheck
    ULONGLONG Limit;                    // HangBugcheckSeconds in 100 ns
    KBUGCHECK_REASON_CALLBACK_RECORD Callback;
    BC250_HANG_DUMP_RANGE Ranges[BC250_HANG_DUMP_RANGES];
} BC250_HANG;

static BC250_HANG g_Hang;

static KSTART_ROUTINE HangHeartbeatThread;
static void HangHeartbeatThread(_In_ PVOID Context)
{
    LARGE_INTEGER period;

    UNREFERENCED_PARAMETER(Context);
    // Explicit, so the criterion is exactly "a normal-priority thread", not whatever a system thread starts at.
    KeSetPriorityThread(KeGetCurrentThread(), BC250_HANG_HEARTBEAT_PRIORITY);
    period.QuadPart = -10000ll * BC250_HANG_HEARTBEAT_MS;
    do {
        InterlockedExchange64(&g_Bc250Progress.Watch.HeartbeatTime, (LONG64)KeQueryUnbiasedInterruptTime());
        InterlockedIncrement(&g_Bc250Progress.Watch.Heartbeat);
    } while (KeWaitForSingleObject(&g_Hang.StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// Sites somebody entered and has not left (bit n = BC250_PROGRESS_SITE_ID n). Plain reads of counters that are
// still moving on other processors: a snapshot for the bugcheck parameters, the dump holds the real values.
static ULONG HangOpenSites(void)
{
    ULONG site, open = 0;

    for (site = 0; site < ProgressSiteCount; site++)
        if (g_Bc250Progress.Sites[site].Entries != g_Bc250Progress.Sites[site].Exits) open |= 1ul << site;
    return open;
}

// The threshold. A normal-priority thread on a multi-core machine waits behind other normal threads for
// milliseconds, not seconds; 10 s without a single 500 ms heartbeat does not happen on a machine that is merely
// busy (a shader compile, a game loading), and the 068 hang had held for minutes. It is still a lab diagnostic,
// armed per run and closed by every install: a real workload with realtime threads pinning every processor
// would trip it, and nobody should ship that.
static KDEFERRED_ROUTINE HangCheckDpc;
static void HangCheckDpc(_In_ KDPC* Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    BC250_PROGRESS_WATCH* watch = &g_Bc250Progress.Watch;
    ULONGLONG now, age;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    if (InterlockedCompareExchange(&g_Hang.InCheck, 1, 0) != 0) return;
    // Unbiased: time asleep is not time without a heartbeat.
    now = KeQueryUnbiasedInterruptTime();
    InterlockedIncrement(&watch->Checks);
    InterlockedExchange64(&watch->CheckTime, (LONG64)now);
    if (InterlockedExchange(&g_Hang.Reprime, 0) != 0) g_Hang.State.Primed = 0;
    if (InterlockedCompareExchange(&watch->Active, 0, 0) == 0) g_Hang.State.Primed = 0;
    else if (Bc250HangWatchCheck(&g_Hang.State, InterlockedCompareExchange(&watch->Heartbeat, 0, 0), now,
                                 g_Hang.Limit, &age))
    {
        InterlockedExchange(&watch->Fired, 1);
        // 1: "BC250PRG", the signature to search for. 2: the progress record. 3: heartbeat age in ms.
        // 4: low 32 bits the sites entered and not left, high 32 bits the record's layout version.
        // C28159 asks to log and continue instead. Here the stop is the product: a machine that no longer runs
        // threads cannot log usefully, and the dump is the only evidence (armed only by EnableHangBugcheck=1).
#pragma warning(suppress: 28159)
        KeBugCheckEx(BC250_HANG_BUGCHECK_CODE, (ULONG_PTR)BC250_PROGRESS_SIGNATURE, (ULONG_PTR)&g_Bc250Progress,
                     (ULONG_PTR)(age / 10000ull), (ULONG_PTR)HangOpenSites() | ((ULONG_PTR)BC250_PROGRESS_VERSION << 32));
    }
    InterlockedExchange(&g_Hang.InCheck, 0);
}

// HIGH_LEVEL, other processors frozen. One call per range, Context counting them; every range is nonpaged static
// storage of this image, valid for as long as the callback is registered.
static KBUGCHECK_REASON_CALLBACK_ROUTINE HangAddPages;
static void HangAddPages(_In_ KBUGCHECK_CALLBACK_REASON Reason, _In_ struct _KBUGCHECK_REASON_CALLBACK_RECORD* Record,
                         _Inout_ PVOID ReasonSpecificData, _In_ ULONG ReasonSpecificDataLength)
{
    PKBUGCHECK_ADD_PAGES pages = (PKBUGCHECK_ADD_PAGES)ReasonSpecificData;
    ULONG_PTR index;

    UNREFERENCED_PARAMETER(Record);
    if (Reason != KbCallbackAddPages || pages == NULL || ReasonSpecificDataLength < sizeof(*pages)) return;
    index = (ULONG_PTR)pages->Context;
    if (index >= BC250_HANG_DUMP_RANGES) { pages->Flags = 0; pages->Count = 0; return; }
    pages->Address = (ULONG_PTR)PAGE_ALIGN(g_Hang.Ranges[index].Base);
    pages->Count = ADDRESS_AND_SIZE_TO_SPAN_PAGES(g_Hang.Ranges[index].Base, g_Hang.Ranges[index].Bytes);
    pages->Flags = KB_ADD_PAGES_FLAG_VIRTUAL_ADDRESS;
    if (index + 1 < BC250_HANG_DUMP_RANGES) pages->Flags |= KB_ADD_PAGES_FLAG_ADDITIONAL_RANGES_EXIST;
    pages->Context = (PVOID)(index + 1);
}

_IRQL_requires_(PASSIVE_LEVEL)
void HangDetectorStart(_In_ const BC250_DEVICE* Device)
{
    OBJECT_ATTRIBUTES attributes;
    HANDLE handle;
    LARGE_INTEGER due;
    ULONG requested, seconds;
    NTSTATUS status;

    if (g_Hang.Created || !Device->FullWddm || Device->Wddm == NULL) return;
    if (GuardReadSetting(L"EnableHangBugcheck", 0) != 1) return;     // closed: nothing is created
    requested = GuardReadSetting(L"HangBugcheckSeconds", BC250_HANG_SECONDS_DEFAULT);
    seconds = Bc250HangSeconds(requested);

    KeInitializeEvent(&g_Hang.StopEvent, NotificationEvent, FALSE);
    KeInitializeTimerEx(&g_Hang.Timer, NotificationTimer);
    KeInitializeDpc(&g_Hang.Dpc, HangCheckDpc, NULL);
    RtlZeroMemory(&g_Hang.State, sizeof(g_Hang.State));
    g_Hang.InCheck = 0;
    g_Hang.Reprime = 0;
    g_Hang.Limit = 10000000ull * seconds;

    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &attributes, NULL, NULL, HangHeartbeatThread, NULL);
    if (!NT_SUCCESS(status)) {
        GuardLog("hang: detector NOT armed, heartbeat thread refused 0x%08X", status);
        return;
    }
    status = ObReferenceObjectByHandle(handle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID*)&g_Hang.Thread, NULL);
    if (!NT_SUCCESS(status)) {
        // Without the object there is nothing to join at Stop, and the thread runs this image's code: end it now.
        KeSetEvent(&g_Hang.StopEvent, IO_NO_INCREMENT, FALSE);
        (void)ZwWaitForSingleObject(handle, FALSE, NULL);
        ZwClose(handle);
        g_Hang.Thread = NULL;
        GuardLog("hang: detector NOT armed, heartbeat thread reference refused 0x%08X", status);
        return;
    }
    ZwClose(handle);

    g_Hang.Ranges[0].Base = &g_Bc250Progress;
    g_Hang.Ranges[0].Bytes = sizeof(g_Bc250Progress);
    GuardLogDumpRegion(&g_Hang.Ranges[1].Base, &g_Hang.Ranges[1].Bytes, &g_Hang.Ranges[2].Base);
    g_Hang.Ranges[2].Bytes = sizeof(ULONG);
    KeInitializeCallbackRecord(&g_Hang.Callback);
    g_Hang.CallbackRegistered = KeRegisterBugCheckReasonCallback(&g_Hang.Callback, HangAddPages, KbCallbackAddPages,
                                                                 (PUCHAR)"bc250kmd");

    InterlockedExchange(&g_Bc250Progress.Watch.LimitSeconds, (LONG)seconds);
    InterlockedExchange(&g_Bc250Progress.Watch.Fired, 0);
    InterlockedExchange(&g_Bc250Progress.Watch.Armed, 1);
    g_Hang.Created = TRUE;
    due.QuadPart = -10000ll * BC250_HANG_CHECK_MS;
    KeSetTimerEx(&g_Hang.Timer, due, BC250_HANG_CHECK_MS, &g_Hang.Dpc);
    InterlockedExchange(&g_Bc250Progress.Watch.Active, 1);          // last: the check judges only from here on
    GuardLog("hang: detector ARMED: bugcheck 0x%08X after %lu s without a heartbeat (requested %lu), progress %p, "
             "dump pages %s", (ULONG)BC250_HANG_BUGCHECK_CODE, seconds, requested, (void*)&g_Bc250Progress,
             g_Hang.CallbackRegistered ? "registered" : "NOT registered");
}

// Order: stop judging, retire the timer and join its DPC, then end and join the thread, then the callback. After
// this nothing of the detector runs, so the device state can go and the image can unload.
_IRQL_requires_(PASSIVE_LEVEL)
void HangDetectorStop(void)
{
    if (!g_Hang.Created) return;
    InterlockedExchange(&g_Bc250Progress.Watch.Active, 0);
    KeCancelTimer(&g_Hang.Timer);
    KeRemoveQueueDpc(&g_Hang.Dpc);
    KeFlushQueuedDpcs();
    KeSetEvent(&g_Hang.StopEvent, IO_NO_INCREMENT, FALSE);
    (void)KeWaitForSingleObject(g_Hang.Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(g_Hang.Thread);
    g_Hang.Thread = NULL;
    if (g_Hang.CallbackRegistered) (void)KeDeregisterBugCheckReasonCallback(&g_Hang.Callback);
    g_Hang.CallbackRegistered = FALSE;
    InterlockedExchange(&g_Bc250Progress.Watch.Armed, 0);
    g_Hang.Created = FALSE;
    GuardLog("hang: detector stopped after %ld checks, heartbeat %ld", g_Bc250Progress.Watch.Checks,
             g_Bc250Progress.Watch.Heartbeat);
}

// A power transition may park threads legitimately; the detector judges only a started device in D0.
// Pause is a flag, not a barrier: a check DPC already past its Active test when Pause stores 0 can still reach
// KeBugCheckEx after Pause returns. Only HangDetectorStop closes the detector (its KeFlushQueuedDpcs). A Pause that
// excluded such a trip would need a synchronized disarm; this lab diagnostic accepts the narrow window.
_IRQL_requires_(PASSIVE_LEVEL)
void HangDetectorPause(void)
{
    if (!g_Hang.Created) return;
    InterlockedExchange(&g_Bc250Progress.Watch.Active, 0);
    InterlockedExchange(&g_Hang.Reprime, 1);
}

_IRQL_requires_(PASSIVE_LEVEL)
void HangDetectorResume(void)
{
    if (!g_Hang.Created) return;
    InterlockedExchange(&g_Hang.Reprime, 1);
    InterlockedExchange(&g_Bc250Progress.Watch.Active, 1);
}
