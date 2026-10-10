// The case fan control (Part B of docs/design/fan.md). The board's NCT6686D turns fan 1 from the BIOS "Fan Setting"
// curve. The owner allowed the driver to take the fan from that curve while it runs (2026-10-06, "TAK"), on two
// conditions: the BIOS setting stays as it is, and the driver gives the fan back to the chip's own automatic mode on
// every exit path. This file is those exit paths, the gate and the escape. The decisions are in the shim.
//
// Where the parts live:
//   driver/shim/bc250_fan.c     the policy: the handshake, the restore record, the curve, doubt, the emergency, the
//                               slope rule, leases, faults. No OS call, host-tested against a model of the chip.
//   driver/shim/bc250_hwmon.c   the write allowlist (0x0A01, 0x0A00, 0x0A29) and the latch sequence.
//   driver/kmd/hwmon.c          the ports, the lock and the reader's sample this control reads its inputs from.
//   this file                   the gate, the controller's hold, the step, the exit paths, the watchdog, the bugcheck
//                               callback, the snapshot, the escape and the stored choice.
//   driver/kmd/dpm.c            the one caller of FanStep, in the governor thread, right after HwmonSample.
//
// The exit paths, each one a call of bc250_fan_handback() with its reason:
//   device stop and remove   FanStop(STOP), from Bc250StopDevice and Bc250RemoveDevice, after DpmStop
//   out of D0                FanPause, from Bc250SetPowerState after DpmPause; and FanStop(POWER) on the
//                            display-only branch that stops the governor
//   driver unload            FanDriverUnload, from Bc250Unload: a device the stop paths missed is handed back here
//   the step stops running   the watchdog DPC, BC250_FAN_WATCHDOG_MS without a step
//   the user asks            RUN_FAN BOARD, applied at the next step
//   a lease runs out         inside the policy, at the step that sees it
//   a bugcheck               the classic bugcheck callback and Bc250ResetDevice: port writes only, no lock, a bounded
//                            poll, no verification (bc250_fan_handback_blind)
//
// Settings, REG_DWORD under Services\bc250kmd\Parameters:
//   EnableFanControl   1 (the default, and what the INF and the release write) lets the driver take the fan. 0, and
//                      any other value, means this file never writes the chip: that is the bisect switch.
//   FanMode            the stored choice: 0 the board's curve, 1 the driver's curve. Absent is the driver's curve.
//   FanProfile         with FanMode 1: 1 standard (absent), 2 quiet, 3 performance, 0 custom.
//   FanCurvePoints     with FanProfile 0: 2..8, and FanCurve0..FanCurve7 each (degrees C << 8) | percent.
//   FanLoadBoost       1 (the default) lets the load feed-forward take the fan to full speed under a sustained
//                      heavy load, before the temperature curve gets there (rule 10 of bc250_fan.h, owner
//                      2026-10-10). 0, and any other value, leaves the duty to the curve alone.
// A stored choice the policy refuses runs the standard curve, and the log says so.
#include "bc250kmd.h"
#include "bc250kmd_escape.h"

#define FAN_SETTING_ENABLE L"EnableFanControl"
#define FAN_SETTING_MODE L"FanMode"
#define FAN_SETTING_PROFILE L"FanProfile"
#define FAN_SETTING_POINTS L"FanCurvePoints"
#define FAN_SETTING_BOOST L"FanLoadBoost"

static const PCWSTR g_FanCurveSetting[BC250_FAN_POINTS_MAX] = {
    L"FanCurve0", L"FanCurve1", L"FanCurve2", L"FanCurve3",
    L"FanCurve4", L"FanCurve5", L"FanCurve6", L"FanCurve7",
};

// The reply's layout. The control app reads it by byte offset (KmdReply.ParseFan, checked against this header by
// test/FanTests.cs); bc250kmd_cli includes the header itself; test_escape_flags.py checks the request flags.
C_ASSERT(sizeof(BC250_ESCAPE_FAN) == 272);      // ABI 1
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Op) == 24);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, CurveC) == 56);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, CurvePct) == 88);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, FixedPct) == 120);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, GuardMc) == 148);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Gate) == 180);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Takeovers) == 184);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Generation) == 248);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, ExpectedGeneration) == 256);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Reserved) == 264);
// The enums travel as numbers; the policy's counts must match what the escape documents.
C_ASSERT(BC250_FAN_MODE_COUNT == 3 && BC250_FAN_PROFILE_COUNT == 4 && BC250_FAN_STATE_COUNT == 7);
C_ASSERT(BC250_FAN_OP_BOARD == BC250_FAN_MODE_BOARD + 1u && BC250_FAN_OP_CURVE == BC250_FAN_MODE_CURVE + 1u &&
         BC250_FAN_OP_FIXED == BC250_FAN_MODE_FIXED + 1u);

static const char* const g_FanGate[] = { "ok", "EnableFanControl 0", "reader offline", "customer ID not unit A" };
C_ASSERT(RTL_NUMBER_OF(g_FanGate) == BC250_FAN_GATE_COUNT);

// The device a stop path has not stopped yet, for Bc250Unload, which has no device argument. One adapter.
static BC250_DEVICE* volatile g_FanDevice;

static ULONG FanMsSince(ULONGLONG Now, ULONGLONG Then)
{
    ULONGLONG ms;

    if (Then == 0 || Now <= Then) return 0;
    ms = (Now - Then) / 10000ull;
    return ms > 600000ull ? 600000u : (ULONG)ms;
}

// ---- the controller's hold --------------------------------------------------------------------------------------

static BOOLEAN FanTryHold(BC250_FAN_OWNER* Owner)
{
    return InterlockedCompareExchange(&Owner->Busy, 1, 0) == 0;
}

static void FanRelease(BC250_FAN_OWNER* Owner)
{
    InterlockedExchange(&Owner->Busy, 0);
}

// PASSIVE_LEVEL. The other holders finish by themselves: a step and a watchdog pass each run one bounded handshake
// at most (the shim's polls, some 100 ms on a chip that never answers, a few ms on unit A).
static void FanHold(BC250_FAN_OWNER* Owner)
{
    LARGE_INTEGER interval;

    interval.QuadPart = -10000ll;       // 1 ms
    while (!FanTryHold(Owner)) KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

// ---- the snapshot and the log -----------------------------------------------------------------------------------

// The holder publishes after every change. The read-back and the speed are the inputs of the step that ran last; a
// publish without inputs (an exit path, the watchdog) keeps the ones already published.
static void FanPublish(BC250_DEVICE* Device, const struct bc250_fan_input* In)
{
    BC250_FAN_OWNER* owner = &Device->Fan;
    BC250_FAN_SNAP snap;
    KIRQL irql;

    RtlZeroMemory(&snap, sizeof(snap));
    snap.Ctl = owner->Ctl;
    snap.Gate = owner->Gate;
    if (In != NULL) {
        snap.ReadbackRaw = In->readback_valid ? In->readback_raw : 0u;
        snap.Rpm = In->rpm_valid ? In->rpm : 0u;
    }
    snap.WatchdogFires = owner->WatchdogFires;
    snap.Generation = Device->StartHealth.Generation;
    if (owner->Enabled) snap.Flags |= BC250_FAN_FLAG_ENABLED;
    if (owner->Gate == BC250_FAN_GATE_SETTING) snap.Flags |= BC250_FAN_FLAG_GATED;
    if (owner->Paused) snap.Flags |= BC250_FAN_FLAG_PAUSED;
    if (owner->Ctl.controlling) snap.Flags |= BC250_FAN_FLAG_CONTROLLING;
    if (owner->Ctl.emergency) snap.Flags |= BC250_FAN_FLAG_EMERGENCY;
    if (owner->Ctl.lease_ms != 0u) snap.Flags |= BC250_FAN_FLAG_LEASED;
    if (owner->Ctl.fault) snap.Flags |= BC250_FAN_FLAG_FAULT;
    if (owner->Ctl.held_back) snap.Flags |= BC250_FAN_FLAG_HELD_BACK;
    if (owner->Ctl.restore.valid) snap.Flags |= BC250_FAN_FLAG_RESTORE_SAVED;
    if (owner->Ctl.restore.substituted) snap.Flags |= BC250_FAN_FLAG_SUBSTITUTED;
    // The raise and not the arm: while a lease holds the fan at the operator's own duty the rule stays armed, and
    // a window that said "at full speed now" over a fan turning at 40 % would be a plain untruth (rule 10).
    if (owner->Ctl.boost_raised) snap.Flags |= BC250_FAN_FLAG_BOOST;
    if (owner->Ctl.boost) snap.Flags |= BC250_FAN_FLAG_BOOST_ARMED;
    if (!owner->Ctl.boost_enabled) snap.Flags |= BC250_FAN_FLAG_BOOST_OFF;
    if ((owner->Ctl.boost_why & BC250_FAN_BOOST_WHY_BUSY) != 0u) snap.Flags |= BC250_FAN_FLAG_BOOST_BUSY;
    if ((owner->Ctl.boost_why & BC250_FAN_BOOST_WHY_POWER) != 0u) snap.Flags |= BC250_FAN_FLAG_BOOST_POWER;
    if ((owner->Ctl.boost_why & BC250_FAN_BOOST_WHY_RISE) != 0u) snap.Flags |= BC250_FAN_FLAG_BOOST_RISE;
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    if (In == NULL) {
        snap.ReadbackRaw = owner->Snap.ReadbackRaw;
        snap.Rpm = owner->Snap.Rpm;
    }
    owner->Snap = snap;
    KeReleaseSpinLock(&owner->SnapLock, irql);
}

static void FanLogTemperature(_In_z_ const char* What, const BC250_FAN_OWNER* Owner)
{
    LONG guard = Owner->Ctl.guard_mc;

    // Two lines: BC250_LOG_TEXT is 160 bytes, and one line with every field would lose its tail (BD-070).
    GuardLog("fan: %s %s, duty %lu%% (raw %lu), guard %ld.%01ld C", What, bc250_fan_state_name(Owner->Ctl.state),
             Owner->Ctl.controlling ? Owner->Ctl.applied_pct : 0u, Owner->Ctl.controlling ? Owner->Ctl.written_raw : 0u,
             guard / 1000, (guard < 0 ? -guard : guard) % 1000 / 100);
    GuardLog("fan: %s reason %s, doubt %s", What, bc250_fan_reason_name(Owner->Ctl.reason),
             bc250_fan_reason_name(Owner->Ctl.doubt));
}

// One line when the load feed-forward engages or lets go (rule 10), and not one per second.
static void FanLogBoost(BC250_FAN_OWNER* Owner)
{
    LONG guard = Owner->Ctl.guard_mc;

    // The step that raises a duty, not the step that arms the rule: under a fixed duty (a lease) the account runs
    // on and raises nothing, and a line saying "full speed, duty was 40%" would be about a fan at 40 %.
    if (Owner->Ctl.boost_raised == Owner->LoggedBoost) return;
    Owner->LoggedBoost = Owner->Ctl.boost_raised;
    if (Owner->Ctl.boost_raised)
        GuardLog("fan: load boost on (%s): full speed at guard %ld.%01ld C, duty was %lu%%",
                 bc250_fan_boost_name(Owner->Ctl.boost_why), guard / 1000,
                 (guard < 0 ? -guard : guard) % 1000 / 100, Owner->Ctl.applied_pct);
    else
        GuardLog("fan: load boost off after %llu ms, duty %lu%%, guard %ld.%01ld C", Owner->Ctl.boost_ms,
                 Owner->Ctl.applied_pct, guard / 1000, (guard < 0 ? -guard : guard) % 1000 / 100);
}

// One line when the state, the reason or the doubt changes, and not one per second.
static void FanLogChange(BC250_FAN_OWNER* Owner, int Status)
{
    FanLogBoost(Owner);
    if (Owner->Ctl.state == Owner->LoggedState && Owner->Ctl.reason == Owner->LoggedReason &&
        Owner->Ctl.doubt == Owner->LoggedDoubt && Status == 0)
        return;
    Owner->LoggedState = Owner->Ctl.state;
    Owner->LoggedReason = Owner->Ctl.reason;
    Owner->LoggedDoubt = Owner->Ctl.doubt;
    FanLogTemperature("now", Owner);
    if (Status != 0)
        GuardLog("fan: the chip refused a step (%d): the board has the fan, no further write in this start", Status);
}

// The telemetry line, next to the reader's. Only while the control is enabled.
void FanLogLine(BC250_DEVICE* Device, _In_z_ const char* What)
{
    BC250_FAN_OWNER* owner = &Device->Fan;
    BC250_FAN_SNAP snap;
    KIRQL irql;

    if (!owner->Configured || !owner->Enabled) return;
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    snap = owner->Snap;
    KeReleaseSpinLock(&owner->SnapLock, irql);
    // Four lines every time, each inside the 159 characters of one log entry (tools/quality/guardlog_width.py,
    // BD-070). The boost pair below joins them only when there is a boost to report.
    GuardLog("fan: %s %s %s/%s", What, bc250_fan_state_name(snap.Ctl.state), bc250_fan_mode_name(snap.Ctl.mode),
             bc250_fan_profile_name(snap.Ctl.profile));
    GuardLog("fan: %s target %lu%% applied %lu%% raw %lu rb %lu, %lu rpm", What, snap.Ctl.target_pct,
             snap.Ctl.controlling ? snap.Ctl.applied_pct : 0u, snap.Ctl.controlling ? snap.Ctl.written_raw : 0u,
             snap.ReadbackRaw, snap.Rpm);
    GuardLog("fan: %s %llu takeovers %llu handbacks %llu writes", What, snap.Ctl.takeovers, snap.Ctl.handbacks,
             snap.Ctl.writes);
    GuardLog("fan: %s %llu failures %llu emergencies %llu doubts", What, snap.Ctl.failures, snap.Ctl.emergencies,
             snap.Ctl.doubts);
    // Two more lines, and only while the feed-forward has something to say: a boost now, or at least one in this
    // start. A cool or idle run keeps the block at the four lines it had, because the log ring rotates 768 lines
    // and a 5 s block of six would shorten what the ring holds of a long session (BD-097). Nothing is lost: the
    // start logs whether the rule is enabled, and FanLogBoost logs every engage and release as it happens. Two
    // lines again, not one: one with every field would lose its tail (tools/quality/guardlog_width.py, BD-070).
    if (!snap.Ctl.boost && snap.Ctl.boosts == 0) return;
    // "armed" is the rule holding a heavy load with no duty of ours to raise: a lease's fixed duty, or the wait
    // after a give-back. The account runs there, the fan does not answer to it.
    GuardLog("fan: %s boost %s (%s)", What,
             snap.Ctl.boost_raised ? "on" : snap.Ctl.boost ? "armed" : snap.Ctl.boost_enabled ? "off" : "disabled",
             bc250_fan_boost_name(snap.Ctl.boost_why));
    GuardLog("fan: %s boost %llu times, %llu ms", What, snap.Ctl.boosts, snap.Ctl.boost_ms);
}

// ---- the inputs -------------------------------------------------------------------------------------------------

// One second of inputs out of the reader's last sample (hwmon.c), which HwmonSample took a moment earlier in the same
// governor tick. The reader's fields are the governor thread's own, and so is this call.
static void FanInput(BC250_FAN_OWNER* Owner, LONG TctlMc, BOOLEAN TctlValid, const BC250_FAN_LOAD* Load, ULONG DtMs,
                     ULONGLONG Now, struct bc250_fan_input* In)
{
    const BC250_HWMON_OWNER* hwmon = Owner->Hwmon;
    const ULONG fan = BC250_FAN_CHANNEL;
    ULONG i;

    RtlZeroMemory(In, sizeof(*In));
    In->dt_ms = DtMs;
    In->tctl_mc = TctlMc;
    In->tctl_valid = TctlValid ? 1 : 0;
    // The governor's load feed (rule 10). A step without one leaves every field at zero, and the feed-forward
    // then never engages: load_valid is what admits it.
    if (Load != NULL && Load->Valid) {
        In->load_valid = 1;
        In->busy_permille = Load->BusyPermille > 1000u ? 1000u : Load->BusyPermille;
        In->gfx_mhz = Load->Mhz;
        In->power_valid = Load->PowerValid ? 1 : 0;
        In->socket_mw = Load->PowerValid ? Load->SocketMw : 0u;
    }
    // Fresh: online, and an accepted sample inside the reader's own freshness window.
    In->reader_valid = hwmon->Online && hwmon->LastValid && hwmon->LastAt != 0 && Now >= hwmon->LastAt &&
                       (Now - hwmon->LastAt) / 10000ull <= BC250_HWMON_FRESH_MS;
    if (!hwmon->LastValid) return;
    for (i = 0; i < BC250_HWMON_TEMP_MAX; i++) {
        if (hwmon->Identity.source[i] != BC250_HWMON_SOURCE_APU) continue;
        In->tsi_mapped = 1;
        if ((hwmon->Last.temperature_valid & (1u << i)) != 0u) {
            In->tsi_valid = 1;
            In->tsi_mc = hwmon->Last.temperature_mc[i];
        }
        break;
    }
    if ((hwmon->Last.rpm_valid & (1u << fan)) != 0u) {
        In->rpm_valid = 1;
        In->rpm = hwmon->Last.rpm[fan];
    }
    if ((hwmon->Last.duty_valid & (1u << fan)) != 0u) {
        In->readback_valid = 1;
        In->readback_raw = hwmon->Last.duty[fan];
    }
}

// ---- the step ---------------------------------------------------------------------------------------------------

// The governor thread, PASSIVE_LEVEL, once per hardware-monitor sample (dpm.c, right after HwmonSample). A held
// controller (an exit path or the watchdog) skips this step: the next one comes a second later.
void FanStep(BC250_DEVICE* Device, LONG TctlMc, BOOLEAN TctlValid, const BC250_FAN_LOAD* Load)
{
    BC250_FAN_OWNER* owner = &Device->Fan;
    BC250_HWMON_PORTS ports;
    struct bc250_hwmon_io io;
    struct bc250_fan_input in;
    struct bc250_fan_request request;
    ULONGLONG now, last;
    ULONG dt, lease = 0;
    BOOLEAN set, renew;
    KIRQL irql;
    int status;

    if (!owner->Configured || !FanTryHold(owner)) return;
    now = KeQueryInterruptTime();
    last = (ULONGLONG)InterlockedCompareExchange64(&owner->LastStepAt, 0, 0);
    dt = FanMsSince(now, last);
    if (dt == 0 || dt > 60000u) dt = BC250_HWMON_PERIOD_MS;
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    set = owner->PendingSet;
    renew = owner->PendingRenew;
    request = owner->Pending;
    lease = owner->PendingLeaseMs;
    owner->PendingSet = owner->PendingRenew = FALSE;
    KeReleaseSpinLock(&owner->SnapLock, irql);
    // The escape checked both already; the policy checks again, and a refusal here changes nothing.
    if (set) {
        int error = bc250_fan_set(&owner->Ctl, &request);
        GuardLog("fan: request %s/%s fixed %lu%% lease %lu ms -> error %d", bc250_fan_mode_name(request.mode),
                 bc250_fan_profile_name(request.profile), (ULONG)request.fixed_pct, (ULONG)request.lease_ms, error);
    }
    if (renew) (void)bc250_fan_renew(&owner->Ctl, lease);
    FanInput(owner, TctlMc, TctlValid, Load, dt, now, &in);
    HwmonWriteIo(&ports, owner->Hwmon, &io, FALSE);
    // A closed gate never reaches the policy: no port access at all, whatever the controller holds.
    status = owner->Paused || !owner->Enabled ? 0 : bc250_fan_tick(&io, &owner->Ctl, &in);
    InterlockedExchange64(&owner->LastStepAt, (LONG64)now);
    FanPublish(Device, &in);
    FanLogChange(owner, status);
    FanRelease(owner);
}

// ---- the exit paths ---------------------------------------------------------------------------------------------

static int FanHandBack(BC250_DEVICE* Device, unsigned int Reason, _In_z_ const char* What)
{
    BC250_FAN_OWNER* owner = &Device->Fan;
    BC250_HWMON_PORTS ports;
    struct bc250_hwmon_io io;
    BOOLEAN held = owner->Ctl.controlling != 0u;
    int status;

    HwmonWriteIo(&ports, owner->Hwmon, &io, FALSE);
    status = bc250_fan_handback(&io, &owner->Ctl, Reason);
    if (status != 0)
        GuardLog("fan: %s: the restore FAILED (%d), the fan may still run at %lu%%", What, status,
                 owner->Ctl.applied_pct);
    else if (held)
        GuardLog("fan: %s: fan %lu is the board's again (target %lu, mode bit clear)", What,
                 (ULONG)BC250_FAN_CHANNEL, owner->Ctl.restore.valid ? owner->Ctl.restore.target : 128u);
    owner->LoggedState = owner->Ctl.state;
    owner->LoggedReason = owner->Ctl.reason;
    return status;
}

// Port writes only: no lock, no log, no allocation. The bugcheck callback and Bc250ResetDevice, at HIGH_LEVEL with
// the other processors stopped. Nothing happens unless the driver holds the fan.
// The blind restore. It runs only while the controller it is given holds the fan, so the boost and rise accounts
// that bc250_fan_handback_blind() clears are cleared here for the one caller whose controller lives on: this
// device's own, through FanResetDevice before a hibernation. The bugcheck callback's start never comes back, and
// the watchdog hands it a copy on purpose, so the real controller keeps its accounts for the step that holds it.
static void FanBlind(BC250_FAN_OWNER* Owner, struct bc250_fan_ctl* Ctl)
{
    BC250_HWMON_PORTS ports;
    struct bc250_hwmon_io io;

    if (Owner->Hwmon == NULL || !Ctl->controlling) return;
    HwmonWriteIo(&ports, Owner->Hwmon, &io, TRUE);
    bc250_fan_handback_blind(&io, Ctl);
}

static KBUGCHECK_CALLBACK_ROUTINE FanBugCheck;
static VOID FanBugCheck(_In_ PVOID Buffer, _In_ ULONG Length)
{
    BC250_FAN_OWNER* owner = (BC250_FAN_OWNER*)Buffer;

    if (owner == NULL || Length != sizeof(*owner)) return;
    // Whatever a stopped processor was doing with the controller, the record in it is stable once taken, and the
    // blind restore reads nothing else of it.
    FanBlind(owner, &owner->Ctl);
}

// DxgkDdiResetDevice: on the way to a bugcheck or a hibernation, at high IRQL.
void FanResetDevice(BC250_DEVICE* Device)
{
    BC250_FAN_OWNER* owner = &Device->Fan;

    if (!owner->Configured || !owner->Enabled) return;
    FanBlind(owner, &owner->Ctl);
}

// The watchdog's normal handback, at PASSIVE_LEVEL (audit finding F1). The DPC below took the hold and queued this
// item; the hold is still ours, so nothing else can be in the controller. The normal handshake polls the chip with
// 250-microsecond stalls (BC250_FAN_POLL_US), which a DPC may not do: "DPC routines that call the
// KeStallExecutionProcessor routine to delay execution must not specify delays of more than 100 microseconds"
// (ref/windows-driver-docs/windows-driver-docs-pr/kernel/guidelines-for-writing-dpc-routines.md:35). So the work
// happens here, where a stall of a few milliseconds costs this thread alone.
static IO_WORKITEM_ROUTINE FanHandBackWorker;
static VOID FanHandBackWorker(_In_opt_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_FAN_OWNER* owner;

    UNREFERENCED_PARAMETER(DeviceObject);
    if (device == NULL) return;
    owner = &device->Fan;
    if (owner->Ctl.controlling) {
        if (FanHandBack(device, BC250_FAN_REASON_WATCHDOG, "watchdog") != 0)
            owner->WatchdogRetryAt = KeQueryInterruptTime() + 10000ull * BC250_FAN_FAULT_RETRY_MS;
        FanPublish(device, NULL);
    }
    // The order matters for FanStop: it waits for the hold, so the hold must be the last thing this item lets go.
    InterlockedExchange(&owner->WorkerQueued, 0);
    FanRelease(owner);
}

// The watchdog: the step stopped running for BC250_FAN_WATCHDOG_MS while the driver holds the fan. A DISPATCH_LEVEL
// DPC, so it never waits for the controller: it tries the hold once and comes back a second later. It never talks to
// the chip over the normal handshake either; that is the work item above.
static KDEFERRED_ROUTINE FanWatchdog;
static VOID FanWatchdog(_In_ PKDPC Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Argument1, _In_opt_ PVOID Argument2)
{
    BC250_DEVICE* device = (BC250_DEVICE*)Context;
    BC250_FAN_OWNER* owner;
    ULONGLONG now;
    ULONG silent;

    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Argument1);
    UNREFERENCED_PARAMETER(Argument2);
    if (device == NULL) return;
    owner = &device->Fan;
    now = KeQueryInterruptTime();
    silent = FanMsSince(now, (ULONGLONG)InterlockedCompareExchange64(&owner->LastStepAt, 0, 0));
    if (silent < BC250_FAN_WATCHDOG_MS) return;
    if (!FanTryHold(owner)) {
        // A step holds the controller and has not let go for this long. Inside the policy every poll is bounded,
        // so the thread is stuck outside it. The blind restore works on a copy: the holder still owns the real one.
        // A hold taken by this watchdog's own work item is not such a stuck step until the item itself is as late
        // as a stuck step would be: the item polls the chip, which takes milliseconds, not seconds.
        const BOOLEAN worker = owner->WorkerQueued != 0 &&
                               FanMsSince(now, owner->WorkerQueuedAt) < BC250_FAN_WATCHDOG_FORCE_MS;
        if (silent >= BC250_FAN_WATCHDOG_FORCE_MS && !owner->BlindDone && !worker) {
            struct bc250_fan_ctl copy = owner->Ctl;
            owner->BlindDone = TRUE;
            owner->WatchdogFires++;
            FanBlind(owner, &copy);
            GuardLog("fan: watchdog: the step held the controller for %lu ms: blind restore", silent);
        }
        return;
    }
    if (owner->Ctl.controlling && now >= owner->WatchdogRetryAt) {
        owner->WatchdogFires++;
        GuardLog("fan: watchdog: no control step for %lu ms", silent);
        if (owner->Worker != NULL && InterlockedCompareExchange(&owner->WorkerQueued, 1, 0) == 0) {
            // The hold stays taken: the work item owns the controller from here and releases it at the end.
            owner->WorkerQueuedAt = now;
            IoQueueWorkItem(owner->Worker, FanHandBackWorker, DelayedWorkQueue, device);
            return;
        }
        // No work item for this device object (the start could not allocate one): the bounded blind restore is
        // the fallback, 100-microsecond stalls and a 2 ms ceiling (BC250_FAN_BLIND_POLL_US/_MAX), which a DPC may
        // do. It gives the fan back without the handshake's verification, and the log says which path ran.
        FanBlind(owner, &owner->Ctl);
        GuardLog("fan: watchdog: no work item: blind restore, the fan is the board's without a read-back");
        FanPublish(device, NULL);
    }
    FanRelease(owner);
}

// ---- life cycle -------------------------------------------------------------------------------------------------

// AddDevice. The locks, the timer and a zeroed controller: the restore record starts empty once per device object.
void FanInitialize(BC250_DEVICE* Device)
{
    BC250_FAN_OWNER* owner = &Device->Fan;

    RtlZeroMemory(owner, sizeof(*owner));
    KeInitializeSpinLock(&owner->SnapLock);
    KeInitializeMutex(&owner->RequestLock, 0);
    KeInitializeTimerEx(&owner->Timer, NotificationTimer);
    KeInitializeDpc(&owner->Dpc, FanWatchdog, Device);
    KeInitializeCallbackRecord(&owner->BugCheck);
    owner->Gate = BC250_FAN_GATE_READER;
}

// The stored choice, as a request. FALSE when there is none (the standard curve runs).
static BOOLEAN FanReadChoice(struct bc250_fan_request* Start, ULONG* Points)
{
    ULONG mode = 0, profile = BC250_FAN_PROFILE_STANDARD, points = 0, value = 0, i;

    RtlZeroMemory(Start, sizeof(*Start));
    *Points = 0;
    if (!NT_SUCCESS(GuardQuerySetting(FAN_SETTING_MODE, &mode))) return FALSE;
    (void)GuardQuerySetting(FAN_SETTING_PROFILE, &profile);
    Start->mode = mode;
    Start->profile = profile;
    if (mode == BC250_FAN_MODE_CURVE && profile == BC250_FAN_PROFILE_CUSTOM &&
        NT_SUCCESS(GuardQuerySetting(FAN_SETTING_POINTS, &points)) && points <= BC250_FAN_POINTS_MAX) {
        Start->curve.points = points;
        for (i = 0; i < points; i++) {
            if (!NT_SUCCESS(GuardQuerySetting(g_FanCurveSetting[i], &value))) value = 0;
            Start->curve.p[i].c = (value >> 8) & 0xFFu;
            Start->curve.p[i].pct = value & 0xFFu;
        }
        *Points = points;
    }
    return TRUE;
}

// StartDevice, PASSIVE_LEVEL, after the start can no longer fail and before the governor thread exists. Never fails
// the start: a closed gate is one log line and a control that writes nothing.
void FanStart(BC250_DEVICE* Device)
{
    BC250_FAN_OWNER* owner = &Device->Fan;
    struct bc250_fan_request start;
    BOOLEAN stored, setting;
    ULONG points = 0, gate;
    KIRQL irql;

    owner->Hwmon = &Device->Hwmon;
    setting = GuardReadSetting(FAN_SETTING_ENABLE, 1) == 1;
    if (!setting) gate = BC250_FAN_GATE_SETTING;
    else if (!Device->Hwmon.Online) gate = BC250_FAN_GATE_READER;
    else if (!Device->Hwmon.IdPinned && Device->Hwmon.Identity.customer_id != BC250_FAN_CUSTOMER_UNIT_A)
        gate = BC250_FAN_GATE_CHIP;
    else gate = BC250_FAN_GATE_OK;
    stored = FanReadChoice(&start, &points);
    bc250_fan_init(&owner->Ctl, gate == BC250_FAN_GATE_OK, stored ? &start : NULL);
    // The load feed-forward (rule 10), on unless this start's registry says otherwise.
    bc250_fan_load_boost(&owner->Ctl, GuardReadSetting(FAN_SETTING_BOOST, 1) == 1);
    if (stored && (owner->Ctl.mode != start.mode ||
                   (start.mode == BC250_FAN_MODE_CURVE && owner->Ctl.profile != start.profile)))
        GuardLog("fan: the stored choice FanMode %lu FanProfile %lu (%lu points) is refused: the standard curve runs",
                 (ULONG)start.mode, (ULONG)start.profile, points);
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    owner->Stored = stored;
    owner->StoredMode = stored ? start.mode : 0u;
    owner->StoredProfile = stored ? start.profile : 0u;
    owner->PendingSet = owner->PendingRenew = FALSE;
    KeReleaseSpinLock(&owner->SnapLock, irql);
    owner->Gate = gate;
    owner->Enabled = gate == BC250_FAN_GATE_OK;
    owner->Paused = FALSE;
    owner->BlindDone = FALSE;
    owner->WatchdogRetryAt = 0;
    owner->LoggedState = owner->Ctl.state;
    owner->LoggedReason = owner->LoggedDoubt = owner->LoggedBoost = 0;
    InterlockedExchange64(&owner->LastStepAt, (LONG64)KeQueryInterruptTime());
    owner->Configured = TRUE;
    g_FanDevice = Device;
    if (owner->Enabled) {
        LARGE_INTEGER due;
        owner->BugCheckRegistered = KeRegisterBugCheckCallback(&owner->BugCheck, FanBugCheck, owner,
                                                               sizeof(*owner), (PUCHAR)"bc250kmd fan");
        // The watchdog's handback runs in this item, at PASSIVE_LEVEL (audit finding F1). A device object with no
        // item left is not a failed start: the watchdog then falls back to the bounded blind restore and says so.
        owner->WorkerQueued = 0;
        owner->WorkerQueuedAt = 0;
        owner->Worker = Device->PhysicalDeviceObject != NULL ? IoAllocateWorkItem(Device->PhysicalDeviceObject)
                                                             : NULL;
        due.QuadPart = -10000ll * BC250_FAN_WATCHDOG_PERIOD_MS;
        (void)KeSetTimerEx(&owner->Timer, due, BC250_FAN_WATCHDOG_PERIOD_MS, &owner->Dpc);
        owner->TimerArmed = TRUE;
        GuardLog("fan: control on: mode %s profile %s", bc250_fan_mode_name(owner->Ctl.mode),
                 bc250_fan_profile_name(owner->Ctl.profile));
        GuardLog("fan: load boost %s (FanLoadBoost)", owner->Ctl.boost_enabled ? "on" : "off");
        GuardLog("fan: restore record %s, bugcheck callback %s",
                 owner->Ctl.restore.valid ? "kept" : "taken at the first change",
                 owner->BugCheckRegistered ? "on" : "REFUSED");
        GuardLog("fan: the watchdog's handback runs %s",
                 owner->Worker != NULL ? "in a work item at PASSIVE_LEVEL" : "blind: NO WORK ITEM");
    } else {
        GuardLog("fan: control off (%s): the board's curve runs the fan, no write to the chip",
                 g_FanGate[gate]);
    }
    FanPublish(Device, NULL);
}

// Device stop and remove, and the display-only power branch: after DpmStop, so no step runs, and before HwmonStop.
// Idempotent. Every reason gives the fan back and takes the watchdog and the bugcheck callback away.
void FanStop(BC250_DEVICE* Device, ULONG Reason)
{
    BC250_FAN_OWNER* owner = &Device->Fan;

    if (!owner->Configured) return;
    if (owner->TimerArmed) {
        (void)KeCancelTimer(&owner->Timer);
        owner->TimerArmed = FALSE;
    }
    KeFlushQueuedDpcs();                // the watchdog DPC runs this image's code and holds the controller
    // The hold also waits for the watchdog's work item, which releases it last: after this call no handback of
    // that item is in flight and the item itself is no longer queued (audit finding F1).
    FanHold(owner);
    (void)FanHandBack(Device, Reason, bc250_fan_reason_name(Reason));
    FanPublish(Device, NULL);
    if (owner->BugCheckRegistered) {
        // Before the device memory goes. A restore that failed above is still the board's problem after this; the
        // log says so, and the record survives in the controller for the next start's first handback.
        (void)KeDeregisterBugCheckCallback(&owner->BugCheck);
        owner->BugCheckRegistered = FALSE;
    }
    GuardLog("fan: stop (%s) after %llu takeovers, %llu handbacks", bc250_fan_reason_name(Reason),
             owner->Ctl.takeovers, owner->Ctl.handbacks);
    GuardLog("fan: stop (%s): %llu writes, %llu failures", bc250_fan_reason_name(Reason), owner->Ctl.writes,
             owner->Ctl.failures);
    owner->Configured = FALSE;
    owner->Enabled = FALSE;
    if (owner->Worker != NULL) {
        IoFreeWorkItem(owner->Worker);
        owner->Worker = NULL;
    }
    if (g_FanDevice == Device) g_FanDevice = NULL;
    FanRelease(owner);
}

// Out of D0: after DpmPause, which holds the governor's tick lock, so no step runs until FanResume. A leased mode
// does not survive the transition: it ends here as it would at its deadline, and the board keeps the fan.
void FanPause(BC250_DEVICE* Device)
{
    BC250_FAN_OWNER* owner = &Device->Fan;

    if (!owner->Configured) return;
    FanHold(owner);
    owner->Paused = TRUE;
    if (owner->Ctl.lease_ms != 0u) {
        owner->Ctl.lease_ms = 0;
        owner->Ctl.mode = BC250_FAN_MODE_BOARD;
    }
    (void)FanHandBack(Device, BC250_FAN_REASON_POWER, "out of D0");
    FanPublish(Device, NULL);
    FanRelease(owner);
}

// Back in D0: the next step takes the fan again if the mode in force says so.
void FanResume(BC250_DEVICE* Device)
{
    BC250_FAN_OWNER* owner = &Device->Fan;

    if (!owner->Configured) return;
    FanHold(owner);
    owner->Paused = FALSE;
    owner->WatchdogRetryAt = 0;
    InterlockedExchange64(&owner->LastStepAt, (LONG64)KeQueryInterruptTime());
    FanPublish(Device, NULL);
    FanRelease(owner);
}

// Bc250Unload. dxgkrnl removes the device before it unloads the image, so FanStop has normally run; this catches a
// device that no stop path reached, while the image's code is still there for the watchdog and the callback.
void FanDriverUnload(void)
{
    BC250_DEVICE* device = g_FanDevice;

    if (device != NULL) FanStop(device, BC250_FAN_REASON_UNLOAD);
}

// ---- the stored choice ------------------------------------------------------------------------------------------

static NTSTATUS FanStoreChoice(const struct bc250_fan_request* Request)
{
    NTSTATUS status, first = STATUS_SUCCESS;
    ULONG i;

    status = GuardStoreSetting(FAN_SETTING_MODE, Request->mode);
    if (!NT_SUCCESS(status)) first = status;
    if (Request->mode == BC250_FAN_MODE_CURVE) {
        status = GuardStoreSetting(FAN_SETTING_PROFILE, Request->profile);
        if (!NT_SUCCESS(status) && NT_SUCCESS(first)) first = status;
        if (Request->profile == BC250_FAN_PROFILE_CUSTOM) {
            status = GuardStoreSetting(FAN_SETTING_POINTS, Request->curve.points);
            if (!NT_SUCCESS(status) && NT_SUCCESS(first)) first = status;
            for (i = 0; i < BC250_FAN_POINTS_MAX; i++) {
                if (i < Request->curve.points)
                    status = GuardStoreSetting(g_FanCurveSetting[i],
                                               (Request->curve.p[i].c << 8) | (Request->curve.p[i].pct & 0xFFu));
                else
                    status = GuardDeleteSetting(g_FanCurveSetting[i]);
                if (!NT_SUCCESS(status) && NT_SUCCESS(first)) first = status;
            }
        }
    }
    return first;
}

// ---- the escape -------------------------------------------------------------------------------------------------
//
// BC250_ESCAPE_RUN_FAN. Software state only: READ copies the snapshot, a write leaves a request for the next step
// and, with Store, writes the registry. No port is touched here. So every operation takes NoAdapterSynchronization
// alone, as RUN_HWMON and RUN_DPM_CURVE do.
void FanRequest(BC250_DEVICE* Device, BC250_ESCAPE_FAN* Data, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_FAN_OWNER* owner = &Device->Fan;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    const ULONG op = Data->Op;
    const BOOLEAN write = op != BC250_FAN_OP_READ;
    const ULONGLONG expected = Data->ExpectedGeneration;
    const ULONG store = Data->Store;
    struct bc250_fan_request request;
    struct bc250_fan_curve resolved;
    NTSTATUS status = STATUS_SUCCESS;
    ULONG error = BC250_FAN_ERROR_OK, i, storedMode, storedProfile;
    BOOLEAN stored;
    BC250_FAN_SNAP snap;
    KIRQL irql;

    RtlZeroMemory(&request, sizeof(request));
    switch (op) {
    case BC250_FAN_OP_BOARD: request.mode = BC250_FAN_MODE_BOARD; break;
    case BC250_FAN_OP_CURVE: request.mode = BC250_FAN_MODE_CURVE; break;
    case BC250_FAN_OP_FIXED: request.mode = BC250_FAN_MODE_FIXED; break;
    default: break;
    }
    request.profile = Data->Profile;
    request.fixed_pct = Data->FixedPct;
    request.lease_ms = Data->LeaseMs;
    if (Data->Points <= BC250_FAN_POINTS_MAX) {
        request.curve.points = Data->Points;
        for (i = 0; i < Data->Points; i++) {
            request.curve.p[i].c = Data->CurveC[i];
            request.curve.p[i].pct = Data->CurvePct[i];
        }
    } else request.curve.points = BC250_FAN_POINTS_MAX + 1u;      // refused by the check as POINTS

    expectedFlags.NoAdapterSynchronization = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)STATUS_INVALID_PARAMETER;
    Data->Flags = Data->Mode = Data->State = Data->Reason = Data->DoubtReason = 0;
    Data->Profile = Data->Points = Data->FixedPct = Data->LeaseMs = Data->Store = 0;
    RtlZeroMemory(Data->CurveC, sizeof(Data->CurveC));
    RtlZeroMemory(Data->CurvePct, sizeof(Data->CurvePct));
    Data->TargetPct = Data->AppliedPct = Data->WrittenRaw = Data->ReadbackRaw = Data->Rpm = 0;
    Data->GuardMc = 0;
    Data->Channel = BC250_FAN_CHANNEL;
    Data->SavedMode = Data->SavedTarget = Data->Error = Data->StoredMode = Data->StoredProfile = Data->Gate = 0;
    Data->Takeovers = Data->Handbacks = Data->Writes = Data->Failures = 0;
    Data->Emergencies = Data->Doubts = Data->LeaseExpiries = Data->WatchdogFires = Data->Generation = 0;
    if (Data->AbiVersion != BC250_FAN_ABI || op > BC250_FAN_OP_RENEW || Data->Reserved[0] || Data->Reserved[1] ||
        store > 1u || EscapeFlags != expectedFlags.Value) return;
    if (write && !Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    if (write) {
        (void)KeWaitForSingleObject(&owner->RequestLock, Executive, KernelMode, FALSE, NULL);
        if (expected != Device->StartHealth.Generation) status = STATUS_RETRY;
        else if (!owner->Configured || !owner->Enabled) status = STATUS_INVALID_DEVICE_STATE;
        else if (op == BC250_FAN_OP_RENEW) {
            if (request.lease_ms < BC250_FAN_LEASE_MIN_MS || request.lease_ms > BC250_FAN_LEASE_MAX_MS) {
                error = BC250_FAN_ERROR_LEASE;
                status = STATUS_INVALID_PARAMETER;
            } else {
                KeAcquireSpinLock(&owner->SnapLock, &irql);
                // A renewal of a durable mode is no error worth a refusal: the policy ignores it.
                owner->PendingRenew = TRUE;
                owner->PendingLeaseMs = request.lease_ms;
                KeReleaseSpinLock(&owner->SnapLock, irql);
            }
        } else {
            error = (ULONG)bc250_fan_request_check(&request, &resolved);
            if (error == BC250_FAN_ERROR_OK && store && (request.mode == BC250_FAN_MODE_FIXED || request.lease_ms))
                error = BC250_FAN_ERROR_LEASE;          // a leased mode is never stored
            if (error != BC250_FAN_ERROR_OK) status = STATUS_INVALID_PARAMETER;
            else {
                if (request.mode == BC250_FAN_MODE_CURVE && request.profile != BC250_FAN_PROFILE_CUSTOM)
                    request.curve = resolved;
                KeAcquireSpinLock(&owner->SnapLock, &irql);
                owner->Pending = request;
                owner->PendingSet = TRUE;
                owner->PendingRenew = FALSE;
                KeReleaseSpinLock(&owner->SnapLock, irql);
                if (store) {
                    status = FanStoreChoice(&request);
                    KeAcquireSpinLock(&owner->SnapLock, &irql);
                    owner->Stored = TRUE;
                    owner->StoredMode = request.mode;
                    owner->StoredProfile = request.mode == BC250_FAN_MODE_CURVE ? request.profile : 0u;
                    KeReleaseSpinLock(&owner->SnapLock, irql);
                    GuardLog("fan: stored FanMode %lu FanProfile %lu (%lu points): 0x%08X", (ULONG)request.mode,
                             (ULONG)request.profile, (ULONG)request.curve.points, status);
                }
            }
        }
        KeReleaseMutex(&owner->RequestLock, FALSE);
    }
    KeAcquireSpinLock(&owner->SnapLock, &irql);
    snap = owner->Snap;
    stored = owner->Stored;
    storedMode = owner->StoredMode;
    storedProfile = owner->StoredProfile;
    KeReleaseSpinLock(&owner->SnapLock, irql);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);

    Data->Flags = snap.Flags | (stored ? BC250_FAN_FLAG_STORED : 0u);
    Data->Mode = snap.Ctl.mode;
    Data->State = snap.Ctl.state;
    Data->Reason = snap.Ctl.reason;
    Data->DoubtReason = snap.Ctl.doubt;
    Data->Profile = snap.Ctl.profile;
    Data->Points = snap.Ctl.curve.points <= BC250_FAN_POINTS_MAX ? snap.Ctl.curve.points : 0u;
    for (i = 0; i < Data->Points; i++) {
        Data->CurveC[i] = snap.Ctl.curve.p[i].c;
        Data->CurvePct[i] = snap.Ctl.curve.p[i].pct;
    }
    Data->FixedPct = snap.Ctl.fixed_pct;
    Data->LeaseMs = snap.Ctl.lease_ms;
    Data->TargetPct = snap.Ctl.target_pct;
    Data->AppliedPct = snap.Ctl.controlling ? snap.Ctl.applied_pct : 0u;
    Data->WrittenRaw = snap.Ctl.controlling ? snap.Ctl.written_raw : 0u;
    Data->ReadbackRaw = snap.ReadbackRaw;
    Data->GuardMc = snap.Ctl.guard_valid ? snap.Ctl.guard_mc : 0;
    Data->Rpm = snap.Rpm;
    Data->SavedMode = snap.Ctl.restore.valid ? snap.Ctl.restore.mode : 0u;
    Data->SavedTarget = snap.Ctl.restore.valid ? snap.Ctl.restore.target : 0u;
    Data->Error = error;
    Data->StoredMode = storedMode;
    Data->StoredProfile = storedProfile;
    Data->Gate = snap.Gate;
    Data->Takeovers = snap.Ctl.takeovers;
    Data->Handbacks = snap.Ctl.handbacks;
    Data->Writes = snap.Ctl.writes;
    Data->Failures = snap.Ctl.failures;
    Data->Emergencies = snap.Ctl.emergencies;
    Data->Doubts = snap.Ctl.doubts;
    Data->LeaseExpiries = snap.Ctl.lease_expiries;
    Data->WatchdogFires = snap.WatchdogFires;
    Data->Generation = Device->StartHealth.Generation;
    Data->NtStatus = (ULONG)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
