// DPM: a load-driven GFX clock between the lab floor (1000 MHz / 820 mV) and at most 2000 MHz
// (docs/design/dpm.md). Owner decision 2026-09-30: The Witcher 3 with RT ran 91.5 % GPU-busy at the fixed
// 1000 MHz, so the clock was the limit.
//
// What lives where:
//   driver/shim/bc250_dpm.c     the policy: guard decision, governor step, session marker (host-tested)
//   driver/shim/bc250_clock.c   the operating-point table and the one SMU transaction every change uses
//   this file                   the thread, the busy sampling, the registry around the guard, telemetry
//
// Settings, all REG_DWORD under Services\bc250kmd\Parameters:
//   DpmMode         0 fixed-lab (today's 1000/820, set once at start), 1 DPM. Absent = BC250_DPM_DEFAULT_MODE.
//   DpmMaxMHz       optional ceiling with DPM, 1000..2000, rounded down to the 100 MHz grid. Absent =
//                   BC250_DPM_DEFAULT_MAX_MHZ (1500); 2000 is the hard ceiling.
//   DpmPending      the encoded DPM request of a start nobody confirmed yet
//   DpmConfirmed    the encoded DPM request a healthy start confirmed
//   DpmSession      written when the governor first leaves the floor, deleted after 10 s at the floor or
//                   at a clean stop: a start that finds it knows the last one died above the floor
//   DpmLastMode, DpmLastReason   what the last start did, for the tools when the adapter is gone
//
// The thread runs in both modes when the native SMU owner is online. Fixed-lab: it samples busy and
// temperature and logs them, and sends no SET. DPM: every BC250_DPM_TICK_MS it samples, asks the policy
// for a level and applies it through SmuSetPoint, the same checked transaction the start uses (voltage up
// before a raise, down after a lowering, read back). Every change is logged; so is a telemetry line every
// five seconds, which the game trials' kernel-log stream records.
//
// Busy (0.7.177): the share of GRBM_STATUS.GUI_ACTIVE samples, read by a high-resolution timer every
// BC250_DPM_HW_SAMPLE_US (DpmHwSample) - the graphics engine's own activity, as amdgpu's gfx_v10_0_is_idle reads
// it. 0.7.175 used the GFX ring's submit-to-fence time (gfx.c DpmBusyBegin/DpmBusyEnd): the KMD's view, closed only
// when a fence is observed (docs/design/dpm.md, "Busy"). It stays beside the samples as telemetry and as the
// fallback for a tick without samples. SDMA0 (the paging node) is sampled as well, for the telemetry only.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"
#include "regs.generated.h"
#include "gc_10_1_0_sh_mask.h"      // GRBM_STATUS and SDMA0_STATUS_REG field masks; the offsets come from regcalc

#define DPM_SETTING_MODE L"DpmMode"
#define DPM_SETTING_MAX L"DpmMaxMHz"
#define DPM_SETTING_PENDING L"DpmPending"
#define DPM_SETTING_CONFIRMED L"DpmConfirmed"
#define DPM_SETTING_SESSION L"DpmSession"
#define DPM_SETTING_LAST_MODE L"DpmLastMode"
#define DPM_SETTING_LAST_REASON L"DpmLastReason"

C_ASSERT(sizeof(BC250_ESCAPE_DPM) == 160);
C_ASSERT(BC250_DPM_THROTTLE_COUNT == 8);

static const char* const g_Throttle[BC250_DPM_THROTTLE_COUNT] = {
    "none", "thermal-soft", "thermal-hard", "sensor", "max-setting", "stable", "smu", "fixed"
};

static void DpmLock(BC250_DPM_STATE* S)
{
    KeWaitForSingleObject(&S->Lock, Executive, KernelMode, FALSE, NULL);
}
static void DpmUnlock(BC250_DPM_STATE* S)
{
    KeReleaseMutex(&S->Lock, FALSE);
}

void DpmInitialize(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    KeInitializeMutex(&s->Lock, 0);
    KeInitializeMutex(&s->TickLock, 0);
    KeInitializeSpinLock(&s->SnapLock);
    KeInitializeEvent(&s->StopEvent, NotificationEvent, FALSE);
    s->Decision.reason = BC250_DPM_REASON_NOT_RUN;
    s->Snap.Reason = BC250_DPM_REASON_NOT_RUN;
}

static BOOLEAN QueryPresent(PCWSTR Name, unsigned int* Value)
{
    ULONG value = 0;
    NTSTATUS status = GuardQuerySetting(Name, &value);
    *Value = value;
    if (NT_SUCCESS(status)) return TRUE;
    *Value = 0;
    if (status != STATUS_OBJECT_NAME_NOT_FOUND)
        GuardLog("dpm: reading %ws failed 0x%08X, treated as absent", Name, status);
    return FALSE;
}

static void StoreLogged(PCWSTR Name, ULONG Value)
{
    NTSTATUS status = GuardStoreSetting(Name, Value);
    if (!NT_SUCCESS(status)) GuardLog("dpm: writing %ws = %lu failed 0x%08X", Name, Value, status);
}

static void DeleteLogged(PCWSTR Name)
{
    NTSTATUS status = GuardDeleteSetting(Name);
    if (!NT_SUCCESS(status)) GuardLog("dpm: deleting %ws failed 0x%08X", Name, status);
}

// The automatic fallback: DpmMode back to fixed-lab, durably, so that the next start does not try again.
static void PersistFallback(ULONG Reason)
{
    StoreLogged(DPM_SETTING_MODE, BC250_DPM_MODE_FIXED);
    DeleteLogged(DPM_SETTING_CONFIRMED);
    DeleteLogged(DPM_SETTING_PENDING);
    DeleteLogged(DPM_SETTING_SESSION);
    StoreLogged(DPM_SETTING_LAST_REASON, Reason);
}

static BOOLEAN Governing(const BC250_DPM_STATE* S)
{
    return S->Decision.mode == BC250_DPM_MODE_DPM && !S->GaveUp;
}

// Total GFX busy time up to Now, in KeQueryInterruptTime units. The submit path's two hooks can race a
// completion against a new submission; the sampler repairs what they left against the ring's own state,
// so an error lasts one tick at most.
static ULONGLONG DpmBusyTotal(BC250_DEVICE* Device, BC250_DPM_STATE* S, ULONGLONG Now)
{
    BOOLEAN inflight = GfxSubmitBusy(Device);
    LONG64 since = InterlockedCompareExchange64(&S->BusySince, 0, 0);
    if (!inflight && since != 0) {
        if (InterlockedCompareExchange64(&S->BusySince, 0, since) == since && (LONG64)Now > since)
            InterlockedAdd64(&S->BusyAccum, (LONG64)Now - since);
    } else if (inflight && since == 0)
        (void)InterlockedCompareExchange64(&S->BusySince, (LONG64)Now, 0);
    since = InterlockedCompareExchange64(&S->BusySince, 0, 0);
    return (ULONGLONG)InterlockedCompareExchange64(&S->BusyAccum, 0, 0) +
           (since != 0 && (LONG64)Now > since ? (ULONGLONG)((LONG64)Now - since) : 0);
}

// ---- the thread's own state, under TickLock ---------------------------------------------------------------------

typedef struct _DPM_TICK {
    ULONGLONG Begin, Last, LastBusy, NextVerify, NextLog, Ticks, ClockAt;
    ULONG Permille, ObservedMHz, ObservedVid, Target;
    ULONG SubmitPermille, SdmaPermille, HwSamples;
    enum bc250_dpm_busy_source Source;
    LONG TemperatureMc;
    BOOLEAN TemperatureValid, SessionRefused;
    ULONG Errors, Resyncs;
} DPM_TICK;

static void DpmPublish(BC250_DEVICE* Device, BC250_DPM_STATE* S, const DPM_TICK* T, BOOLEAN Running)
{
    BC250_DPM_SNAP snap;
    const struct bc250_dpm_governor* g = &S->Gov;
    ULONGLONG now = KeQueryInterruptTime();
    KIRQL irql;
    BOOLEAN governing = Governing(S);

    RtlZeroMemory(&snap, sizeof(snap));
    snap.Flags = (Running ? BC250_DPM_FLAG_RUNNING : 0) |
                 (Running && governing ? BC250_DPM_FLAG_GOVERNING : 0) |
                 (S->Pending ? BC250_DPM_FLAG_PENDING : 0) |
                 (S->Confirmed ? BC250_DPM_FLAG_CONFIRMED : 0) |
                 (InterlockedCompareExchange(&S->Paused, 0, 0) ? BC250_DPM_FLAG_PAUSED : 0) |
                 (InterlockedCompareExchange(&S->Stable, 0, 0) ? BC250_DPM_FLAG_STABLE : 0) |
                 (S->Session.marked ? BC250_DPM_FLAG_SESSION : 0);
    snap.Mode = S->Decision.mode;
    snap.Requested = S->Decision.requested;
    snap.Reason = S->GaveUp ? BC250_DPM_REASON_SMU_ERROR : S->Decision.reason;
    snap.Throttle = !governing ? (S->GaveUp ? BC250_DPM_THROTTLE_SMU : BC250_DPM_THROTTLE_FIXED) : g->throttle;
    snap.MaxMHz = bc250_dpm_level_mhz(S->Decision.max_level);
    snap.CapMHz = bc250_dpm_level_mhz(g->thermal_cap < g->max_level ? g->thermal_cap : g->max_level);
    snap.WantMHz = bc250_dpm_level_mhz(g->want);
    snap.CurrentMHz = bc250_dpm_level_mhz(g->level);
    snap.CurrentMv = bc250_dpm_level_mv(g->level);
    snap.Raises = g->raises;
    snap.Lowers = g->lowers;
    snap.ThermalEvents = g->thermal_events;
    snap.BusyAvgPermille = g->avg_permille;
    snap.Generation = S->Generation;
    if (T != NULL) {
        snap.TargetMHz = bc250_dpm_level_mhz(T->Target);
        snap.BusyPermille = T->Permille;
        snap.SubmitBusyPermille = T->SubmitPermille;
        snap.SdmaBusyPermille = T->SdmaPermille;
        snap.BusySource = T->Source;
        snap.HwSamples = T->HwSamples;
        if (T->Source == BC250_DPM_BUSY_GRBM) snap.Flags |= BC250_DPM_FLAG_HW_BUSY;
        snap.TemperatureMc = T->TemperatureMc;
        if (T->TemperatureValid) snap.Flags |= BC250_DPM_FLAG_TEMPERATURE;
        snap.ObservedMHz = T->ObservedMHz;
        snap.ObservedVid = T->ObservedVid;
        if (T->ClockAt && now - T->ClockAt <= 10000ull * BC250_DPM_VERIFY_MS + 10000ull * BC250_DPM_TICK_MS * 4)
            snap.Flags |= BC250_DPM_FLAG_CLOCK;
        snap.Errors = T->Errors;
        snap.Resyncs = T->Resyncs;
        snap.Ticks = T->Ticks;
        snap.UptimeMs = (now - T->Begin) / 10000ull;
        snap.BusyTime100ns = T->LastBusy;
    } else {
        KeAcquireSpinLock(&S->SnapLock, &irql);
        snap.TargetMHz = S->Snap.TargetMHz;
        snap.TemperatureMc = S->Snap.TemperatureMc;
        snap.ObservedMHz = S->Snap.ObservedMHz;
        snap.ObservedVid = S->Snap.ObservedVid;
        snap.Errors = S->Snap.Errors;
        snap.Resyncs = S->Snap.Resyncs;
        snap.Ticks = S->Snap.Ticks;
        snap.UptimeMs = S->Snap.UptimeMs;
        snap.BusyTime100ns = S->Snap.BusyTime100ns;
        snap.BusyPermille = S->Snap.BusyPermille;
        snap.SubmitBusyPermille = S->Snap.SubmitBusyPermille;
        snap.SdmaBusyPermille = S->Snap.SdmaBusyPermille;
        snap.BusySource = S->Snap.BusySource;
        snap.HwSamples = S->Snap.HwSamples;
        snap.Flags |= S->Snap.Flags & BC250_DPM_FLAG_HW_BUSY;
        KeReleaseSpinLock(&S->SnapLock, irql);
    }
    UNREFERENCED_PARAMETER(Device);
    KeAcquireSpinLock(&S->SnapLock, &irql);
    S->Snap = snap;
    KeReleaseSpinLock(&S->SnapLock, irql);
}

static void DpmLogLine(const char* What, const BC250_DPM_SNAP* P)
{
    LONG t = P->TemperatureMc;
    GuardLog("dpm: %s %s %lu MHz/%lu mV (SMU %lu MHz VID %lu) %ld.%01ld C busy %lu.%lu%% avg %lu.%lu%% "
             "want %lu cap %lu max %lu throttle %s, raises %lu lowers %lu thermal %lu errors %lu resyncs %lu, busy from %s "
             "(%lu samples), submit %lu.%lu%%, sdma %lu.%lu%%",
             What, P->Mode == BC250_DPM_MODE_DPM ? "dpm" : "fixed", P->CurrentMHz, P->CurrentMv,
             P->ObservedMHz, P->ObservedVid, t / 1000, (t < 0 ? -t : t) % 1000 / 100,
             P->BusyPermille / 10, P->BusyPermille % 10, P->BusyAvgPermille / 10, P->BusyAvgPermille % 10,
             P->WantMHz, P->CapMHz, P->MaxMHz,
             P->Throttle < BC250_DPM_THROTTLE_COUNT ? g_Throttle[P->Throttle] : "?",
             P->Raises, P->Lowers, P->ThermalEvents, P->Errors, P->Resyncs,
             P->BusySource == BC250_DPM_BUSY_GRBM ? "grbm" : "submit", P->HwSamples,
             P->SubmitBusyPermille / 10, P->SubmitBusyPermille % 10, P->SdmaBusyPermille / 10, P->SdmaBusyPermille % 10);
}

// The one place a level reaches the hardware. TRUE when the hardware is at Level now.
static BOOLEAN DpmApply(BC250_DEVICE* Device, BC250_DPM_STATE* S, DPM_TICK* T, ULONG Level, const char* Why)
{
    struct bc250_clock_report report;
    ULONG from = bc250_dpm_level_mhz(S->Gov.level);
    NTSTATUS status = SmuSetPoint(&Device->Smu, bc250_dpm_level_mhz(Level), bc250_dpm_level_mv(Level), &report);
    if (NT_SUCCESS(status) && report.ready) {
        bc250_dpm_commit(&S->Gov, Level);
        S->ErrorsInRow = 0;
        T->ObservedMHz = report.observed_mhz;
        T->ObservedVid = report.observed_vid;
        T->ClockAt = KeQueryInterruptTime();
        GuardLog("dpm: %lu -> %lu MHz/%lu mV VID %u%s, %u settle reads (%s, busy %lu permille, %d.%01d C, throttle %s)",
                 from, report.observed_mhz, report.requested_mv, report.observed_vid,
                 report.voltage_staged ? " staged" : "", report.settle_reads, Why, T->Permille, report.temperature_mc / 1000,
                 (report.temperature_mc < 0 ? -report.temperature_mc : report.temperature_mc) % 1000 / 100,
                 S->Gov.throttle < BC250_DPM_THROTTLE_COUNT ? g_Throttle[S->Gov.throttle] : "?");
        return TRUE;
    }
    if (report.status == BC250_CLOCK_TOO_HOT) {
        // The transaction's own gate saw 85 C before the governor's sample did: nothing was requested.
        GuardLog("dpm: %lu -> %lu MHz refused by the clock gate at %d mC (%s)", from,
                 bc250_dpm_level_mhz(Level), report.temperature_mc, Why);
        return FALSE;
    }
    T->Errors++;
    S->ErrorsInRow++;
    InterlockedExchange(&S->Resync, 1);
    GuardLog("dpm: %lu -> %lu MHz FAILED 0x%08X (clock status %d, %u of %u messages, initial %u MHz VID %u, "
             "observed %u MHz VID %u, %u settle reads), %lu in a row (%s)", from, bc250_dpm_level_mhz(Level), status,
             report.status, report.messages_completed, report.messages_attempted, report.initial_mhz,
             report.initial_vid, report.observed_mhz, report.observed_vid, report.settle_reads, S->ErrorsInRow, Why);
    return FALSE;
}

// Repeated SMU failures: the floor if it can still be had, the escape's SET back, fixed-lab next start.
static void DpmGiveUp(BC250_DEVICE* Device, BC250_DPM_STATE* S, DPM_TICK* T)
{
    S->ErrorsInRow = 0;
    (void)DpmApply(Device, S, T, BC250_DPM_FLOOR_LEVEL, "giving up");
    S->GaveUp = TRUE;
    InterlockedExchange(&Device->Smu.GovernorActive, 0);
    PersistFallback(BC250_DPM_REASON_SMU_ERROR);
    GuardLog("dpm: GAVE UP after %lu failed transitions in a row; DpmMode = 0 written, clock left at %lu MHz",
             (ULONG)BC250_DPM_ERROR_LIMIT, bc250_dpm_level_mhz(S->Gov.level));
}

// The hardware's operating point, read back, as the governor's level. A point outside the table (somebody
// else wrote it, a failed transaction stopped half-way) is replaced by the floor.
static void DpmResyncLevel(BC250_DEVICE* Device, BC250_DPM_STATE* S, DPM_TICK* T)
{
    ULONG mhz = 0, vid = 0;
    LONG degrees = 0;
    int level = -1;
    NTSTATUS status = SmuReadClock(&Device->Smu, &mhz, &vid, &degrees);
    T->Resyncs++;
    if (NT_SUCCESS(status)) {
        T->ObservedMHz = mhz;
        T->ObservedVid = vid;
        T->ClockAt = KeQueryInterruptTime();
        level = bc250_dpm_level_of(mhz);
        if (level >= 0 && vid == bc250_clock_points[level].vid) {
            S->Gov.level = (ULONG)level;
            return;
        }
    }
    GuardLog("dpm: resync read 0x%08X %lu MHz VID %lu: not a table point, back to the floor", status, mhz, vid);
    S->Gov.level = BC250_DPM_TOP_LEVEL;     // unknown: any floor request is a lowering
    if (!DpmApply(Device, S, T, BC250_DPM_FLOOR_LEVEL, "resync") && S->ErrorsInRow >= BC250_DPM_ERROR_LIMIT)
        DpmGiveUp(Device, S, T);
}

// The hardware sampler: one high-resolution timer callback every BC250_DPM_HW_SAMPLE_US at DISPATCH_LEVEL, two
// register reads from the general read table (MmioRead, no lock, no write), three counters. GRBM_STATUS is global
// (no GRBM_GFX_INDEX bank) and full WDDM runs without GFXOFF (gfx.c pp_gfxoff), so the read is always answered.
// Paused (a power transition) reads nothing; DpmPause flushes a callback in flight before it returns.
static EXT_CALLBACK DpmHwSample;
static void DpmHwSample(_In_ PEX_TIMER Timer, _In_opt_ PVOID Context)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_DPM_STATE* s;
    ULONG grbm = 0, sdma = 0;

    UNREFERENCED_PARAMETER(Timer);
    if (device == NULL) return;
    s = &device->Dpm;
    if (InterlockedCompareExchange(&s->Paused, 0, 0)) return;
    if (!NT_SUCCESS(MmioRead(device, BC250_REG_GC_GRBM_STATUS, &grbm))) return;
    InterlockedIncrement(&s->HwSamples);
    if ((grbm & GRBM_STATUS__GUI_ACTIVE_MASK) != 0) InterlockedIncrement(&s->HwGfxActive);
    if (NT_SUCCESS(MmioRead(device, BC250_REG_GC_SDMA0_STATUS_REG, &sdma)) && (sdma & SDMA0_STATUS_REG__IDLE_MASK) == 0)
        InterlockedIncrement(&s->HwSdmaActive);
}

static void DpmTick(BC250_DEVICE* Device, BC250_DPM_STATE* S, DPM_TICK* T)
{
    ULONGLONG now = KeQueryInterruptTime();
    ULONGLONG busy = DpmBusyTotal(Device, S, now);
    ULONGLONG dt = now > T->Last ? now - T->Last : 0;
    ULONG dtMs = (ULONG)min(dt / 10000ull, 60000ull);
    NTSTATUS status;
    BOOLEAN governing;

    T->SubmitPermille = dt ? (ULONG)min(1000ull, (busy > T->LastBusy ? busy - T->LastBusy : 0) * 1000ull / dt) : 0;
    {
        // The sampler's counts since the last tick. A sample that lands between these reads is counted in the
        // next tick, or its active bit without its sample: the helper clamps that.
        ULONG samples = (ULONG)InterlockedExchange(&S->HwSamples, 0);
        ULONG gfx = (ULONG)InterlockedExchange(&S->HwGfxActive, 0);
        ULONG sdma = (ULONG)InterlockedExchange(&S->HwSdmaActive, 0);
        enum bc250_dpm_busy_source unused;
        T->HwSamples = samples;
        T->Permille = bc250_dpm_busy_permille(samples, gfx, T->SubmitPermille, &T->Source);
        T->SdmaPermille = T->Source == BC250_DPM_BUSY_GRBM ? bc250_dpm_busy_permille(samples, sdma, 0, &unused) : 0;
    }
    T->Last = now;
    T->LastBusy = busy;
    T->Ticks++;
    if (InterlockedCompareExchange(&S->Paused, 0, 0)) {
        DpmPublish(Device, S, T, TRUE);
        return;
    }
    status = SmuReadTemperature(&Device->Smu, &T->TemperatureMc);
    T->TemperatureValid = NT_SUCCESS(status);
    governing = Governing(S);
    if (governing && InterlockedExchange(&S->Resync, 0)) DpmResyncLevel(Device, S, T);
    governing = Governing(S);
    if (governing) {
        struct bc250_dpm_input in;
        ULONG target;
        enum bc250_dpm_session_action action;
        in.busy_permille = T->Permille;
        in.temperature_mc = T->TemperatureMc;
        in.temperature_valid = T->TemperatureValid;
        in.dt_ms = dtMs;
        S->Gov.stable = InterlockedCompareExchange(&S->Stable, 0, 0) != 0;
        target = bc250_dpm_step(&S->Gov, &in);
        action = bc250_dpm_session_step(&S->Session, target, dtMs);
        if (action == BC250_DPM_SESSION_SET) {
            // Durable before the first raise, or no raise.
            status = GuardStoreSetting(DPM_SETTING_SESSION, S->Decision.encoded);
            if (NT_SUCCESS(status)) {
                S->Session.marked = 1;
                T->SessionRefused = FALSE;
            } else {
                if (!T->SessionRefused) GuardLog("dpm: session marker not durable 0x%08X, staying at the floor", status);
                T->SessionRefused = TRUE;
                target = BC250_DPM_FLOOR_LEVEL;
            }
        } else if (action == BC250_DPM_SESSION_CLEAR) {
            if (NT_SUCCESS(GuardDeleteSetting(DPM_SETTING_SESSION))) {
                S->Session.marked = 0;
                S->Session.floor_ms = 0;
            }
        }
        T->Target = target;
        if (target != S->Gov.level && !DpmApply(Device, S, T, target, "load") &&
            S->ErrorsInRow >= BC250_DPM_ERROR_LIMIT)
            DpmGiveUp(Device, S, T);
    } else {
        // Fixed-lab: the same average, for the telemetry, and nothing else.
        S->Gov.avg_permille = (S->Gov.avg_permille * 3u + T->Permille + 2u) / 4u;
        T->Target = S->Gov.level;
    }
    if (now >= T->NextVerify) {
        ULONG mhz = 0, vid = 0;
        LONG degrees = 0;
        T->NextVerify = now + 10000ull * BC250_DPM_VERIFY_MS;
        if (NT_SUCCESS(SmuReadClock(&Device->Smu, &mhz, &vid, &degrees))) {
            T->ObservedMHz = mhz;
            T->ObservedVid = vid;
            T->ClockAt = KeQueryInterruptTime();
            if (governing && Governing(S) &&
                (mhz != bc250_dpm_level_mhz(S->Gov.level) || vid != bc250_clock_points[S->Gov.level].vid)) {
                GuardLog("dpm: readback %lu MHz VID %lu is not the committed %lu MHz VID %u: resync", mhz, vid,
                         bc250_dpm_level_mhz(S->Gov.level), bc250_clock_points[S->Gov.level].vid);
                InterlockedExchange(&S->Resync, 1);
            }
        }
    }
    DpmPublish(Device, S, T, TRUE);
    if (now >= T->NextLog) {
        BC250_DPM_SNAP snap;
        KIRQL irql;
        T->NextLog = now + 10000ull * BC250_DPM_LOG_MS;
        KeAcquireSpinLock(&S->SnapLock, &irql);
        snap = S->Snap;
        KeReleaseSpinLock(&S->SnapLock, irql);
        DpmLogLine("telemetry", &snap);
    }
}

static KSTART_ROUTINE DpmThread;
static void DpmThread(_In_ PVOID Context)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_DPM_STATE* s = &device->Dpm;
    LARGE_INTEGER period;
    DPM_TICK tick;

    RtlZeroMemory(&tick, sizeof(tick));
    tick.Begin = tick.Last = KeQueryInterruptTime();
    tick.LastBusy = DpmBusyTotal(device, s, tick.Begin);
    tick.NextVerify = tick.Begin;
    tick.NextLog = tick.Begin + 10000ull * BC250_DPM_LOG_MS;
    period.QuadPart = -10000ll * BC250_DPM_TICK_MS;
    while (KeWaitForSingleObject(&s->StopEvent, Executive, KernelMode, FALSE, &period) == STATUS_TIMEOUT) {
        KeWaitForSingleObject(&s->TickLock, Executive, KernelMode, FALSE, NULL);
        DpmTick(device, s, &tick);
        KeReleaseMutex(&s->TickLock, FALSE);
    }
    // The last tick's telemetry stays readable after the stop.
    DpmPublish(device, s, &tick, FALSE);
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// StartDevice, last, after the start that set the floor (SmuPrepareClock) succeeded. PASSIVE_LEVEL.
void DpmStart(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    struct bc250_dpm_decision* d = &s->Decision;
    struct bc250_dpm_request r;
    OBJECT_ATTRIBUTES attributes;
    HANDLE handle;
    NTSTATUS status;

    DpmLock(s);
    if (s->Created) { DpmUnlock(s); return; }
    RtlZeroMemory(&r, sizeof(r));
    RtlZeroMemory(d, sizeof(*d));
    RtlZeroMemory(&s->Session, sizeof(s->Session));
    s->Pending = s->Confirmed = s->GaveUp = FALSE;
    s->ErrorsInRow = 0;
    InterlockedExchange(&s->Paused, 0);
    InterlockedExchange(&s->Resync, 0);
    InterlockedExchange(&s->Stable, 0);
    InterlockedExchange64(&s->BusySince, 0);
    InterlockedExchange64(&s->BusyAccum, 0);
    InterlockedExchange(&s->HwSamples, 0);
    InterlockedExchange(&s->HwGfxActive, 0);
    InterlockedExchange(&s->HwSdmaActive, 0);
    KeClearEvent(&s->StopEvent);
    s->Generation = Device->StartHealth.Generation;

    if (!Device->FullWddm) {
        d->reason = BC250_DPM_REASON_NOT_RUN;
        bc250_dpm_init(&s->Gov, BC250_DPM_FLOOR_LEVEL);
        DpmPublish(Device, s, NULL, FALSE);
        DpmUnlock(s);
        return;
    }
    r.smu_online = Device->Smu.Online ? 1 : 0;
    r.mode_present = QueryPresent(DPM_SETTING_MODE, &r.mode);
    r.max_present = QueryPresent(DPM_SETTING_MAX, &r.max_mhz);
    r.pending_present = QueryPresent(DPM_SETTING_PENDING, &r.pending);
    r.confirmed_present = QueryPresent(DPM_SETTING_CONFIRMED, &r.confirmed);
    r.session_present = QueryPresent(DPM_SETTING_SESSION, &r.session);
    bc250_dpm_decide(&r, d);

    if (d->force_fixed) {
        GuardLog("dpm: falling back to fixed-lab and writing DpmMode = 0: %s (pending 0x%08X, session 0x%08X)",
                 d->reason == BC250_DPM_REASON_UNCONFIRMED ? "an earlier DPM start was never confirmed" :
                 "an earlier start ended above the floor", r.pending, r.session);
        PersistFallback(d->reason);
    } else {
        if (d->clear_pending) DeleteLogged(DPM_SETTING_PENDING);
        if (d->clear_session) DeleteLogged(DPM_SETTING_SESSION);
    }
    if (d->mark_pending) {
        // Durable before the first raise, or no DPM at all.
        status = GuardStoreSetting(DPM_SETTING_PENDING, d->encoded);
        if (!NT_SUCCESS(status)) {
            GuardLog("dpm: pending mark not durable 0x%08X, running fixed-lab", status);
            d->mode = BC250_DPM_MODE_FIXED;
            d->max_mhz = BC250_CLOCK_FLOOR_MHZ;
            d->max_level = BC250_DPM_FLOOR_LEVEL;
            d->reason = BC250_DPM_REASON_REGISTRY;
            d->mark_pending = 0;
        } else s->Pending = TRUE;
    }
    s->Confirmed = d->confirmed && d->mode == BC250_DPM_MODE_DPM;
    if (r.smu_online) {
        StoreLogged(DPM_SETTING_LAST_MODE, d->mode);
        StoreLogged(DPM_SETTING_LAST_REASON, d->reason);
    }
    bc250_dpm_init(&s->Gov, d->max_level);
    GuardLog("dpm: DpmMode %lu%s DpmMaxMHz %lu%s -> %s, ceiling %lu MHz, reason %lu%s%s", r.mode,
             r.mode_present ? "" : " (absent)", r.max_mhz, r.max_present ? "" : " (absent)",
             d->mode == BC250_DPM_MODE_DPM ? "DPM" : "fixed-lab", d->max_mhz, d->reason,
             s->Pending ? ", pending" : "", s->Confirmed ? ", confirmed earlier" : "");
    if (!r.smu_online) {
        DpmPublish(Device, s, NULL, FALSE);
        DpmUnlock(s);
        return;
    }
    if (d->mode == BC250_DPM_MODE_DPM) InterlockedExchange(&Device->Smu.GovernorActive, 1);
    DpmPublish(Device, s, NULL, FALSE);
    InitializeObjectAttributes(&attributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, &attributes, NULL, NULL, DpmThread, Device);
    if (NT_SUCCESS(status)) {
        status = ObReferenceObjectByHandle(handle, SYNCHRONIZE, *PsThreadType, KernelMode, (PVOID*)&s->Thread, NULL);
        if (!NT_SUCCESS(status)) {
            // Without the object there is nothing to join at Stop: end it now.
            KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
            (void)ZwWaitForSingleObject(handle, FALSE, NULL);
            s->Thread = NULL;
        }
        ZwClose(handle);
    }
    if (!NT_SUCCESS(status)) {
        InterlockedExchange(&Device->Smu.GovernorActive, 0);
        GuardLog("dpm: governor thread NOT started 0x%08X, the clock stays at the floor", status);
    } else {
        s->Created = TRUE;
        // Without the sampler every tick falls back to the submit accounting; the governor still runs.
        s->HwTimer = ExAllocateTimer(DpmHwSample, Device, EX_TIMER_HIGH_RESOLUTION);
        if (s->HwTimer != NULL)
            (void)ExSetTimer(s->HwTimer, -10ll * BC250_DPM_HW_SAMPLE_US, 10ll * BC250_DPM_HW_SAMPLE_US, NULL);
        GuardLog("dpm: hardware busy sampler %s, every %lu us", s->HwTimer != NULL ? "running" : "NOT allocated (submit accounting only)",
                 (ULONG)BC250_DPM_HW_SAMPLE_US);
    }
    DpmUnlock(s);
}

// StopDevice (before SmuOwnerStop) and RemoveDevice. Joins the thread, leaves the floor behind, and
// clears the session marker of a clean stop. PASSIVE_LEVEL, idempotent.
void DpmStop(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    DPM_TICK tick;

    DpmLock(s);
    if (!s->Created) { DpmUnlock(s); return; }
    // The sampler first: after this no callback runs or will run (Wait), before the mapping can go.
    if (s->HwTimer != NULL) {
        (void)ExDeleteTimer(s->HwTimer, TRUE, TRUE, NULL);
        s->HwTimer = NULL;
    }
    KeSetEvent(&s->StopEvent, IO_NO_INCREMENT, FALSE);
    (void)KeWaitForSingleObject(s->Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(s->Thread);
    s->Thread = NULL;
    s->Created = FALSE;
    RtlZeroMemory(&tick, sizeof(tick));
    if (Governing(s) && s->Gov.level != BC250_DPM_FLOOR_LEVEL)
        (void)DpmApply(Device, s, &tick, BC250_DPM_FLOOR_LEVEL, "stop");
    if (s->Session.marked && s->Gov.level == BC250_DPM_FLOOR_LEVEL &&
        NT_SUCCESS(GuardDeleteSetting(DPM_SETTING_SESSION)))
        s->Session.marked = 0;
    InterlockedExchange(&Device->Smu.GovernorActive, 0);
    {
        BC250_DPM_SNAP snap;
        KIRQL irql;
        KeAcquireSpinLock(&s->SnapLock, &irql);
        s->Snap.Flags &= ~(BC250_DPM_FLAG_RUNNING | BC250_DPM_FLAG_GOVERNING | BC250_DPM_FLAG_SESSION);
        if (s->Session.marked) s->Snap.Flags |= BC250_DPM_FLAG_SESSION;
        s->Snap.CurrentMHz = bc250_dpm_level_mhz(s->Gov.level);
        s->Snap.CurrentMv = bc250_dpm_level_mv(s->Gov.level);
        snap = s->Snap;
        KeReleaseSpinLock(&s->SnapLock, irql);
        DpmLogLine("stopped", &snap);
    }
    DpmUnlock(s);
}

// Before a transition out of D0: no SMU traffic from here until DpmResume, and the floor if it can be had.
// The tick in flight, if any, finishes first (TickLock). PASSIVE_LEVEL.
void DpmPause(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    DPM_TICK tick;
    if (!s->Created) return;
    InterlockedExchange(&s->Paused, 1);
    KeFlushQueuedDpcs();                // a sampler callback that read Paused as 0 has finished its reads
    KeWaitForSingleObject(&s->TickLock, Executive, KernelMode, FALSE, NULL);
    RtlZeroMemory(&tick, sizeof(tick));
    if (Governing(s) && s->Gov.level != BC250_DPM_FLOOR_LEVEL)
        (void)DpmApply(Device, s, &tick, BC250_DPM_FLOOR_LEVEL, "power down");
    KeReleaseMutex(&s->TickLock, FALSE);
}

// After a successful return to D0: the start transaction put the floor back (power.c); read it to be sure.
void DpmResume(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    if (!s->Created) return;
    InterlockedExchange(&s->Resync, 1);
    InterlockedExchange(&s->Paused, 0);
}

// DxgkDdiSetStablePowerState: a profiler asks for clocks that do not move. The floor is the one clock this
// driver can promise.
void DpmSetStable(BC250_DEVICE* Device, BOOLEAN Enabled)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    if (InterlockedExchange(&s->Stable, Enabled ? 1 : 0) != (Enabled ? 1 : 0))
        GuardLog("dpm: stable power state %s", Enabled ? "on: pinned to the floor" : "off");
}

// Clears this start's pending mark. PASSIVE_LEVEL. Pending goes first, Confirmed second: a crash in
// between costs a reconfirmation, never a false DPM. Nothing to do is success.
NTSTATUS DpmConfirm(BC250_DEVICE* Device, _In_z_ const char* Why)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    NTSTATUS status = STATUS_SUCCESS;
    DpmLock(s);
    if (s->Decision.mode == BC250_DPM_MODE_DPM && s->Pending) {
        status = GuardDeleteSetting(DPM_SETTING_PENDING);
        if (NT_SUCCESS(status)) {
            s->Pending = FALSE;
            status = GuardStoreSetting(DPM_SETTING_CONFIRMED, s->Decision.encoded);
            if (NT_SUCCESS(status)) s->Confirmed = TRUE;
        }
        GuardLog("dpm: request 0x%08X confirmed by %s: 0x%08X", s->Decision.encoded, Why, status);
    }
    DpmUnlock(s);
    return status;
}

// The kernel-log summary (BC250_ESCAPE_LOG_SUMMARY): one line with the last tick's telemetry.
void DpmLogSummary(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    BC250_DPM_SNAP snap;
    KIRQL irql;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    snap = s->Snap;
    KeReleaseSpinLock(&s->SnapLock, irql);
    DpmLogLine("summary", &snap);
}

// BC250_ESCAPE_RUN_DPM. Software state only, so NoAdapterSynchronization=1 for both operations.
void DpmRequest(BC250_DEVICE* Device, BC250_ESCAPE_DPM* Data, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONGLONG ready = 0;
    BOOLEAN confirm = Data->Op == BC250_DPM_OP_CONFIRM;
    BC250_DPM_SNAP snap;
    KIRQL irql;

    expectedFlags.NoAdapterSynchronization = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)status;
    Data->Flags = Data->Mode = Data->Requested = Data->Reason = Data->Throttle = 0;
    Data->MaxMHz = Data->CapMHz = Data->TargetMHz = Data->WantMHz = 0;
    Data->CurrentMHz = Data->CurrentMv = Data->ObservedMHz = Data->ObservedVid = 0;
    Data->TemperatureMc = 0;
    Data->BusyPermille = Data->BusyAvgPermille = 0;
    // SubmitBusyPermille and SdmaBusyPermille were Reserved: zero inputs (checked below), written on success.
    Data->Raises = Data->Lowers = Data->ThermalEvents = Data->Errors = Data->Resyncs = 0;
    Data->Ticks = Data->BusyTime100ns = Data->UptimeMs = Data->Generation = 0;
    if (Data->AbiVersion != BC250_DPM_ABI || Data->SubmitBusyPermille || Data->SdmaBusyPermille ||
        (Data->Op != BC250_DPM_OP_READ && !confirm) || EscapeFlags != expectedFlags.Value) return;
    if (confirm && !Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    status = STATUS_SUCCESS;
    if (confirm) {
        // The same healthy milestone a deploy confirms: READY, visible, completions, a minute of it.
        if (Data->ExpectedGeneration != s->Generation) status = STATUS_RETRY;
        else if (!StartHealthIsReady(Device, &ready) || ready != s->Generation) status = STATUS_DEVICE_NOT_READY;
        else status = DpmConfirm(Device, "escape");
        // The flags follow the confirmation at once, not at the next tick.
        KeAcquireSpinLock(&s->SnapLock, &irql);
        s->Snap.Flags &= ~(BC250_DPM_FLAG_PENDING | BC250_DPM_FLAG_CONFIRMED);
        s->Snap.Flags |= (s->Pending ? BC250_DPM_FLAG_PENDING : 0) | (s->Confirmed ? BC250_DPM_FLAG_CONFIRMED : 0);
        KeReleaseSpinLock(&s->SnapLock, irql);
    }
    KeAcquireSpinLock(&s->SnapLock, &irql);
    snap = s->Snap;
    KeReleaseSpinLock(&s->SnapLock, irql);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);
    Data->Flags = snap.Flags;
    Data->Mode = snap.Mode;
    Data->Requested = snap.Requested;
    Data->Reason = snap.Reason;
    Data->Throttle = snap.Throttle;
    Data->MaxMHz = snap.MaxMHz;
    Data->CapMHz = snap.CapMHz;
    Data->TargetMHz = snap.TargetMHz;
    Data->WantMHz = snap.WantMHz;
    Data->CurrentMHz = snap.CurrentMHz;
    Data->CurrentMv = snap.CurrentMv;
    Data->ObservedMHz = snap.ObservedMHz;
    Data->ObservedVid = snap.ObservedVid;
    Data->TemperatureMc = snap.TemperatureMc;
    Data->BusyPermille = snap.BusyPermille;
    Data->BusyAvgPermille = snap.BusyAvgPermille;
    Data->SubmitBusyPermille = snap.SubmitBusyPermille;
    Data->SdmaBusyPermille = snap.SdmaBusyPermille;
    Data->Raises = snap.Raises;
    Data->Lowers = snap.Lowers;
    Data->ThermalEvents = snap.ThermalEvents;
    Data->Errors = snap.Errors;
    Data->Resyncs = snap.Resyncs;
    Data->Ticks = snap.Ticks;
    Data->BusyTime100ns = snap.BusyTime100ns;
    Data->UptimeMs = snap.UptimeMs;
    Data->Generation = snap.Generation;
    Data->NtStatus = (ULONG)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
