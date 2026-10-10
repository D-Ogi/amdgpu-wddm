// DPM: a load-driven GFX clock between the lab floor (1000 MHz / 820 mV) and at most 2000 MHz
// (docs/design/dpm.md). Owner decision 2026-09-30: The Witcher 3 with RT ran 91.5 % GPU-busy at the fixed
// 1000 MHz, so the clock was the limit.
//
// Since 0.7.207 an idle GPU runs at 500 MHz (owner decision 2026-10-05: "jak lab nie pracuje, to ustawiaj mu
// zegar gpu na 500 MHz" - when the lab does not work, set its GPU clock to 500 MHz). The policy is in the shim
// (idle_step); this file feeds it the ring's state, reads its three registry settings, applies the point with the
// same checked transaction as every other level, and reports the state in the telemetry and the escape.
//
// Since 0.7.205 the thermal cap alone may go under that floor, to 900 or 800 MHz at the floor's own 820 mV
// (owner decision 2026-10-05, after RotTR scene 2 held 88 C with the governor already at 1000 MHz and the GPU
// 97 % busy). The firmware accepts both points (facts M785: session 402 held 800 and 900 MHz at VID 116
// with no refusal), and 1000 MHz is only the lowest level the power tables publish (facts M47). A part that
// does refuse costs nothing: DpmApply treats a refused
// sub-floor transition as "this part has no sub-floor": one log line, the lab floor applied instead, the SMU
// give-up counter untouched, and the governor's cap stops at the lab floor for the rest of the start.
//
// What lives where:
//   driver/shim/bc250_dpm.c     the policy: guard decision, governor step, session marker (host-tested)
//   driver/shim/bc250_clock.c   the operating-point table and the one SMU transaction every change uses
//   this file                   the thread, the busy sampling, the registry around the guard, telemetry
//
// Settings, all REG_DWORD under Services\bc250kmd\Parameters:
//   DpmMode         0 fixed-lab (today's 1000/820, set once at start), 1 DPM. Absent = BC250_DPM_DEFAULT_MODE.
//   DpmMaxMHz       optional ceiling with DPM, 1000..2000, rounded down to the 100 MHz grid. Absent =
//                   BC250_DPM_DEFAULT_MAX_MHZ (2000); 2000 is the hard ceiling.
//   DpmPending      the encoded DPM request of a start nobody confirmed yet
//   DpmConfirmed    the encoded DPM request a healthy start confirmed
//   DpmSession      written when the governor first leaves the floor, deleted after 10 s at the floor or
//                   at a clean stop: a start that finds it knows the last one died above the floor
//   DpmIdleMHz      the idle point (0.7.207): a table clock below 1000 MHz, or 0 for no idle state at all,
//                   which is exactly 0.7.205 behaviour. Absent = BC250_DPM_IDLE_MHZ (500).
//   DpmIdleHoldMs   how long the GPU must have no work before the clock goes to the idle point. Absent =
//                   BC250_DPM_IDLE_HOLD_MS (3000); BC250_DPM_IDLE_MIN/MAX_HOLD_MS bound it.
//   DpmIdleBusyPermille  the busy share the hold window still admits, in permille of its wall time. Absent =
//                   BC250_DPM_IDLE_BUSY_PERMILLE (2); at most BC250_DPM_IDLE_MAX_BUSY_PERMILLE. The desktop
//                   on the GPU wakes for single frames, so the rule is this mean, not a strict zero.
//   DpmIdleLeavePermille  the slow exit (0.7.216.6): the idle point is left when the work of its trailing window
//                   (DpmIdleHoldMs long) reaches this share; one tick at BC250_DPM_IDLE_EXIT_PERMILLE (500) leaves at
//                   once. Absent = BC250_DPM_IDLE_LEAVE_PERMILLE (150), range 10..400 and above DpmIdleBusyPermille.
//                   Work on the GFX ring no longer leaves the point by itself: every DWM frame is a submission.
//   DpmThermalZone  the soft thermal zone (0.7.213, BD-087): absent or any non-zero value runs it, which is the
//                   default, and 0 runs the 0.7.212 thermal rules instead (the hot cap at 87 C alone, no soft
//                   release, the warm zone back at 87 C). It is one switch for a whole start, for bisecting a
//                   regression against the rules the lab measured before it; the zone's own numbers are tunable at
//                   run time through RUN_DPM_TUNE ABI 3, and a RESET there turns the zone back on.
//   DpmJointGovernor  the joint power arm (0.7.216.7, C62): 1 runs it in a governing start, absent or any other value
//                   leaves it off, which is the default. While the GPU is bound and its clock held by heat, the arm
//                   lowers the CPU's maximum boost clock (cpu.c) one step at a time and gives it back when either
//                   ends; it needs CpuTune 1 and a CPU read stage that answered. docs/design/dpm.md says what a lab
//                   trial must show before the default changes.
//   DpmLastMode, DpmLastReason   what the last start did, for the tools when the adapter is gone
//   DpmClosedReason the reason the driver itself wrote DpmMode 0 (3 unconfirmed, 4 unclean, 8 SMU error).
//                   PersistFallback writes it, the first start that reads a DpmMode other than 0 deletes it,
//                   and a start that reads DpmMode 0 leaves it alone. DpmLastReason cannot do this work,
//                   because every start overwrites it: with DpmMode 0 the next start writes 1 (not requested)
//                   over the fallback. The release installer reads the record one boot later and offers its
//                   repair instead of taking the 0 for a setting of the tester (BD-069). InteropClosedReason
//                   works the same way (interop.c).
//
// The thread runs in both modes when the native SMU owner is online. Fixed-lab: it samples busy and
// temperature and logs them, and sends no SET. DPM: every BC250_DPM_TICK_MS it samples, asks the policy
// for a level and applies it through SmuSetPoint, the same checked transaction the start uses (voltage up
// before a raise, down after a lowering, read back). Every change is logged; so is a telemetry block every
// five seconds, which the game trials' kernel-log stream records, and every two minutes at the idle point
// (BD-097, dpm_log_cadence.h).
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
#define DPM_SETTING_IDLE_LOG L"TelemetryIdleLogMs"
#define DPM_SETTING_IDLE_MHZ L"DpmIdleMHz"
#define DPM_SETTING_IDLE_HOLD L"DpmIdleHoldMs"
#define DPM_SETTING_IDLE_BUSY L"DpmIdleBusyPermille"
#define DPM_SETTING_IDLE_LEAVE L"DpmIdleLeavePermille"
#define DPM_SETTING_ZONE L"DpmThermalZone"
#define DPM_SETTING_JOINT L"DpmJointGovernor"
#define DPM_SETTING_LAST_MODE L"DpmLastMode"
#define DPM_SETTING_LAST_REASON L"DpmLastReason"
#define DPM_SETTING_CLOSED L"DpmClosedReason"
// The operator's V/F curve (0.7.210): 11 named values, one per level from the lab floor up, plus the boot guard's
// two marks over the curve's checksum and the default trial window. An absent value means the table's own line at
// that clock, so a machine that nobody tuned has none of these.
#define DPM_SETTING_CURVE_PENDING L"DpmCurvePending"
#define DPM_SETTING_CURVE_CONFIRMED L"DpmCurveConfirmed"
#define DPM_SETTING_CURVE_TRIAL L"DpmCurveTrialMs"
#define DPM_SETTING_CURVE_REASON L"DpmCurveLastReason"
// What DpmCurveLastReason says about this start's curve (docs/design/tuner.md). The tools read it when the
// adapter is gone, which is exactly the case this guard exists for.
#define DPM_CURVE_REASON_NONE 0u            // no stored curve: the table's own line
#define DPM_CURVE_REASON_OK 1u              // the stored curve runs this start
#define DPM_CURVE_REASON_REFUSED 2u         // the stored values broke a rule of bc250_clock_curve_check
#define DPM_CURVE_REASON_UNCONFIRMED 3u     // an earlier start ran the curve and never became healthy
#define DPM_CURVE_REASON_REGISTRY 4u        // the pending mark would not reach the disk
#define DPM_CURVE_REASON_NOT_GOVERNING 5u   // this start applies no level, so no curve acts

C_ASSERT(sizeof(BC250_ESCAPE_DPM) == 192);        // ABI 2 (0.7.207); the ABI 1 prefix is 160 bytes
C_ASSERT(BC250_DPM_ABI1_SIZE == 160);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPM, IdleMHz) == BC250_DPM_ABI1_SIZE);
C_ASSERT(sizeof(BC250_ESCAPE_DPM) == BC250_DPM_ABI2_SIZE);
C_ASSERT(sizeof(BC250_DPM_METRICS) == 56);
C_ASSERT(sizeof(BC250_ESCAPE_DPM_EX) == BC250_DPM_ABI3_SIZE);   // ABI 3 (0.7.215): ABI 2 and the metrics tail
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPM_EX, Metrics) == BC250_DPM_ABI2_SIZE);
C_ASSERT(BC250_DPM_THROTTLE_COUNT == 12);         // 0.7.207 appended "idle", 0.7.213 "thermal-zone"
C_ASSERT(sizeof(BC250_ESCAPE_DPM_TUNE) == 184);   // ABI 3 (0.7.213); ABI 2 is 152 bytes, ABI 1 120
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPM_TUNE, HotStepMs) == BC250_DPM_TUNE_ABI1_SIZE);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPM_TUNE, ZoneDeltaMc) == BC250_DPM_TUNE_ABI2_SIZE);
C_ASSERT(BC250_DPM_TUNE_ABI2_SIZE == 152);
C_ASSERT(BC250_DPM_TUNE_HOT_MC == BC250_DPM_HOT_MC); // the deltas of the escape are counted down from this one value
C_ASSERT(BC250_DPM_TUNE_COUNT == 9);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPM_TUNE, Status) == FIELD_OFFSET(BC250_ESCAPE, Status) &&
         FIELD_OFFSET(BC250_ESCAPE_DPM_TUNE, Version) == FIELD_OFFSET(BC250_ESCAPE, Version));
C_ASSERT(sizeof(BC250_ESCAPE_DPM_CURVE) == 360);  // ABI 1 (0.7.210); one size, so the dispatch has one test
C_ASSERT(BC250_DPM_CURVE_POINTS == BC250_CURVE_POINTS);
C_ASSERT(BC250_CURVE_FIRST_LEVEL == BC250_DPM_FLOOR_LEVEL);
C_ASSERT(BC250_CURVE_FIRST_LEVEL + BC250_CURVE_POINTS == BC250_CLOCK_LEVELS);
C_ASSERT(BC250_CLOCK_CURVE_ERROR_COUNT == 7);     // 0.7.211 added UNTRIED: a KEEP of a candidate nobody applied
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_DPM_CURVE, Status) == FIELD_OFFSET(BC250_ESCAPE, Status) &&
         FIELD_OFFSET(BC250_ESCAPE_DPM_CURVE, Version) == FIELD_OFFSET(BC250_ESCAPE, Version));

// One registry value per editable level. The names carry the clock, so an operator reading the key sees what each
// one is; the order of this table is the order of the curve vector.
static const PCWSTR g_CurveSetting[BC250_CURVE_POINTS] = {
    L"DpmCurve1000", L"DpmCurve1100", L"DpmCurve1200", L"DpmCurve1300", L"DpmCurve1400", L"DpmCurve1500",
    L"DpmCurve1600", L"DpmCurve1700", L"DpmCurve1800", L"DpmCurve1900", L"DpmCurve2000"
};

static const char* const g_Throttle[BC250_DPM_THROTTLE_COUNT] = {
    "none", "thermal-soft", "thermal-hard", "sensor", "max-setting", "stable", "smu", "fixed", "thermal-warm",
    "thermal-ramp", "idle", "thermal-zone"
};

static const char* const g_JointReason[BC250_JOINT_REASON_COUNT] = {
    "off", "no-cpu", "no-room", "free", "wait", "capping", "hold", "raising", "blind"
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
    bc250_dpm_tune_default(&s->Tune);
    // The table's own line, so that a read before the first start (and a start that never governs) reports the
    // voltage of the table and not a zero.
    bc250_dpm_curve_init(&s->Curve, NULL);
    s->CurveTrialMs = BC250_DPM_CURVE_TRIAL_MS;
}

// ---- runtime tuning (0.7.185, BC250_ESCAPE_RUN_DPM_TUNE) ----------------------------------------------------------

static BOOLEAN ThermalEqual(const struct bc250_dpm_tune* A, const struct bc250_dpm_tune* B)
{
    return A->hot_step_ms == B->hot_step_ms && A->soft_delta_mc == B->soft_delta_mc && A->soft_step_ms == B->soft_step_ms;
}

// The soft zone's own three values (0.7.213). Separate from ThermalEqual, because the escape reports them under their
// own flag and an ABI 2 caller never sees them.
static BOOLEAN ZoneEqual(const struct bc250_dpm_tune* A, const struct bc250_dpm_tune* B)
{
    return A->zone_delta_mc == B->zone_delta_mc && A->zone_step_ms == B->zone_step_ms &&
           A->zone_lead_ms == B->zone_lead_ms;
}

static BOOLEAN TuneEqual(const struct bc250_dpm_tune* A, const struct bc250_dpm_tune* B)
{
    return A->up_permille == B->up_permille && A->target_permille == B->target_permille &&
           A->down_permille == B->down_permille && A->down_hold_ms == B->down_hold_ms && A->floor_level == B->floor_level &&
           ThermalEqual(A, B) && ZoneEqual(A, B);
}

// The floor as the escape and the log give it: the clock, 0 for none (BC250_DPM_FLOOR_LEVEL is the lab floor,
// which means no runtime floor; nothing below it is admitted as one).
static ULONG TuneFloorMHz(const struct bc250_dpm_tune* T)
{
    return T->floor_level != BC250_DPM_FLOOR_LEVEL ? bc250_dpm_level_mhz(T->floor_level) : 0;
}

// One line per change, old and new side by side, in the driver log (160 bytes a line: this one stays near 125).
static void DpmLogTuneChange(const char* What, const struct bc250_dpm_tune* Old, const struct bc250_dpm_tune* New,
                             ULONG Serial)
{
    GuardLog("dpm: tune (%s): up %lu->%lu target %lu->%lu down %lu->%lu permille, hold %lu->%lu ms, floor %lu->%lu MHz, "
             "serial %lu", What, Old->up_permille, New->up_permille, Old->target_permille, New->target_permille,
             Old->down_permille, New->down_permille, Old->down_hold_ms, New->down_hold_ms, TuneFloorMHz(Old),
             TuneFloorMHz(New), Serial);
    // The thermal timing (0.7.197) on a line of its own, only when it changed: the line above keeps its 0.7.185 shape.
    if (!ThermalEqual(Old, New))
        GuardLog("dpm: tune (%s): hot step %lu->%lu ms, soft release delta %lu->%lu mC step %lu->%lu ms, serial %lu",
                 What, Old->hot_step_ms, New->hot_step_ms, Old->soft_delta_mc, New->soft_delta_mc, Old->soft_step_ms,
                 New->soft_step_ms, Serial);
    // The soft zone (0.7.213) on a line of its own too, with the thresholds it puts in force spelled out in mC: the
    // deltas are relative to HOT_MC and nobody should have to subtract in their head while reading a game's log.
    if (!ZoneEqual(Old, New)) {
        GuardLog("dpm: tune (%s): zone delta %lu->%lu mC (at %d->%d mC), serial %lu", What, Old->zone_delta_mc,
                 New->zone_delta_mc, bc250_dpm_zone_mc(Old), bc250_dpm_zone_mc(New), Serial);
        GuardLog("dpm: tune (%s): zone step %lu->%lu ms lead %lu->%lu ms, no raise from %d->%d mC", What,
                 Old->zone_step_ms, New->zone_step_ms, Old->zone_lead_ms, New->zone_lead_ms,
                 bc250_dpm_warm_mc(Old), bc250_dpm_warm_mc(New));
    }
}

// The governor's own values (Gov.tune) next to the telemetry, while they are not the defaults, and in the summary.
static void DpmLogTune(const char* What, const struct bc250_dpm_tune* T, ULONG Serial, ULONG Applied, ULONG FloorTicks,
                       ULONG SoftReleases, const BC250_DPM_SNAP* Snap)
{
    GuardLog("dpm: %s up %lu target %lu down %lu permille, hold %lu ms, floor %lu MHz, serial %lu applied %lu, "
             "floor ticks %lu", What, T->up_permille, T->target_permille, T->down_permille, T->down_hold_ms,
             TuneFloorMHz(T), Serial, Applied, FloorTicks);
    GuardLog("dpm: %s thermal: hot step %lu ms, soft release delta %lu mC step %lu ms, soft raises %lu", What,
             T->hot_step_ms, T->soft_delta_mc, T->soft_step_ms, SoftReleases);
    // The soft zone (0.7.213, BD-087): the three tunable values, the two thresholds they put in force, and what the
    // zone has actually done this start. A reader of a game's log needs the thresholds in the same units as the
    // temperature samples beside them, so both are printed in mC.
    if (T->zone_delta_mc) {
        GuardLog("dpm: %s zone: at %d mC (delta %lu) step %lu ms, no raise from %d mC", What, bc250_dpm_zone_mc(T),
                 T->zone_delta_mc, T->zone_step_ms, bc250_dpm_warm_mc(T));
        GuardLog("dpm: %s zone lead: %lu ms over %lu ms, now %+d mC, %lu ticks with none", What, T->zone_lead_ms,
                 (ULONG)BC250_DPM_ZONE_SLOPE_MS, Snap->ZoneLeadMc, Snap->ZoneLeadGaps);
        // "slope none" is not "lead 0": the first says the ring could not measure the die's rise at all, so every soft
        // threshold read the raw sensor, which is not what the zone's numbers were chosen for (0.7.213 review).
        GuardLog("dpm: %s zone: %lu steps, %lu ticks, %lu idle holds, slope %s", What, Snap->ZoneSteps,
                 Snap->ZoneTicks, Snap->ZoneIdleHolds, Snap->ZoneLeadOk ? "measured" : "none");
    } else
        GuardLog("dpm: %s zone: off (DpmThermalZone 0): the hot cap at %d mC is the only thermal rule, no raise "
                 "from %d mC", What, (int)BC250_DPM_HOT_MC, bc250_dpm_warm_mc(T));
}

// The thread, at the start of a governing tick: the escape's values, copied under SnapLock, at most once per change.
// The escape checked them against the same ceiling (Decision.max_level is Gov.max_level), so the governor's own check
// refuses nothing in practice; if it did, the governor keeps its values, Applied stays behind Serial and the log says
// so once.
static void DpmTakeTune(BC250_DPM_STATE* S)
{
    struct bc250_dpm_tune tune;
    ULONG serial;
    KIRQL irql;
    enum bc250_dpm_tune_error error;

    KeAcquireSpinLock(&S->SnapLock, &irql);
    serial = S->TuneSerial;
    tune = S->Tune;
    KeReleaseSpinLock(&S->SnapLock, irql);
    if (serial == S->TuneTaken || serial == S->TuneRefused) return;
    error = bc250_dpm_set_tune(&S->Gov, &tune);
    if (error == BC250_DPM_TUNE_OK) S->TuneTaken = serial;
    else {
        S->TuneRefused = serial;
        GuardLog("dpm: tune serial %lu refused by the governor (error %d), running serial %lu", serial, (int)error,
                 S->TuneTaken);
    }
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

static NTSTATUS StoreLogged(PCWSTR Name, ULONG Value)
{
    NTSTATUS status = GuardStoreSetting(Name, Value);
    if (!NT_SUCCESS(status)) GuardLog("dpm: writing %ws = %lu failed 0x%08X", Name, Value, status);
    return status;
}

static NTSTATUS DeleteLogged(PCWSTR Name)
{
    NTSTATUS status = GuardDeleteSetting(Name);
    if (!NT_SUCCESS(status)) GuardLog("dpm: deleting %ws failed 0x%08X", Name, status);
    return status;
}

// ---- the operator's V/F curve (0.7.210, docs/design/tuner.md, ADR 0020) --------------------------------------
// The policy and the trial are driver/shim/bc250_dpm.c; this part is the lock around them, the registry under
// them and the one forced re-apply that puts a new curve into the hardware.
//
// Every reader of the voltage column goes through these two, and both take SnapLock: the escape may replace the
// active curve at any moment, and a torn read of 11 values would ask the firmware for a voltage nobody chose.
// One spin lock per clock transition is nothing next to the transaction itself.
static ULONG DpmLevelMv(BC250_DPM_STATE* S, ULONG Level)
{
    KIRQL irql;
    ULONG mv;
    KeAcquireSpinLock(&S->SnapLock, &irql);
    mv = bc250_dpm_curve_level_mv(&S->Curve, Level);
    KeReleaseSpinLock(&S->SnapLock, irql);
    return mv;
}

static ULONG DpmLevelVid(BC250_DPM_STATE* S, ULONG Level)
{
    KIRQL irql;
    ULONG vid;
    KeAcquireSpinLock(&S->SnapLock, &irql);
    vid = bc250_dpm_curve_level_vid(&S->Curve, Level);
    KeReleaseSpinLock(&S->SnapLock, irql);
    return vid;
}

// A curve as a pair of log lines: the millivolts from 1000 MHz up, and whether they are the table's own line.
// A pair and not one line, by the rule of the guardlog-width gate: 11 values at their widest is 201 characters
// and BC250_LOG_TEXT holds 159, so one line would silently lose the last clocks (BD-070). Each line names the
// band of clocks it carries, so neither half can be read as the whole curve.
static void DpmLogCurve(const char* What, const struct bc250_clock_curve* Curve)
{
    GuardLog("dpm: curve (%s) 1000-1500 MHz: %lu %lu %lu %lu %lu %lu mV", What,
             Curve->mv[0], Curve->mv[1], Curve->mv[2], Curve->mv[3], Curve->mv[4], Curve->mv[5]);
    GuardLog("dpm: curve (%s) 1600-2000 MHz: %lu %lu %lu %lu %lu mV%s", What,
             Curve->mv[6], Curve->mv[7], Curve->mv[8], Curve->mv[9], Curve->mv[10],
             bc250_clock_curve_is_default(Curve) ? " (the table's own line)" : "");
}

// The stored curve of this start: 11 values, each absent meaning the table's line at that clock. A value that is
// not a whole millivolt, or a curve that breaks one of the rules, is refused as a whole: a half-applied curve is
// worse than none, and the operator's own typo is not something to interpolate around. PASSIVE_LEVEL, under Lock.
static void DpmReadCurve(BC250_DPM_STATE* S, struct bc250_clock_curve* Curve, BOOLEAN* Any, BOOLEAN* Refused)
{
    unsigned int i, level = 0;
    enum bc250_clock_curve_error error;
    UNREFERENCED_PARAMETER(S);
    bc250_clock_curve_default(Curve);
    *Any = FALSE;
    *Refused = FALSE;
    for (i = 0; i < BC250_CURVE_POINTS; i++) {
        unsigned int mv = 0;
        if (!QueryPresent(g_CurveSetting[i], &mv)) continue;
        *Any = TRUE;
        Curve->mv[i] = mv;
    }
    if (!*Any) return;
    error = bc250_clock_curve_check(Curve, &level);
    if (error == BC250_CLOCK_CURVE_OK) return;
    GuardLog("dpm: the stored curve is refused (error %d at %lu MHz): the table's own line runs this start",
             (int)error, bc250_dpm_level_mhz(level));
    bc250_clock_curve_default(Curve);
    *Refused = TRUE;
}

// 11 writes, each flushed. TRUE when every one of them reached the disk: a curve half on disk is a curve the next
// start would refuse as a whole (DpmReadCurve), which is safe but says nothing useful, so the caller is told.
static BOOLEAN DpmStoreCurve(const struct bc250_clock_curve* Curve)
{
    unsigned int i;
    BOOLEAN all = TRUE;
    for (i = 0; i < BC250_CURVE_POINTS; i++) {
        NTSTATUS status = GuardStoreSetting(g_CurveSetting[i], Curve->mv[i]);
        if (!NT_SUCCESS(status)) {
            GuardLog("dpm: writing %ws = %lu failed 0x%08X", g_CurveSetting[i], Curve->mv[i], status);
            all = FALSE;
        }
    }
    return all;
}

static void DpmDeleteCurve(void)
{
    unsigned int i;
    for (i = 0; i < BC250_CURVE_POINTS; i++) {
        NTSTATUS status = GuardDeleteSetting(g_CurveSetting[i]);
        if (!NT_SUCCESS(status) && status != STATUS_OBJECT_NAME_NOT_FOUND)
            GuardLog("dpm: deleting %ws failed 0x%08X", g_CurveSetting[i], status);
    }
}

// The automatic fallback: DpmMode back to fixed-lab, durably, so that the next start does not try again.
// DpmClosedReason records who wrote that 0. It outlives the boot, because no later start overwrites it; only a
// start that reads a DpmMode other than 0 deletes it (DpmStart below). DpmLastReason keeps its old meaning: the
// reason of the last start, overwritten at every start.
// The record goes first, before the 0 it describes. Both writes are flushed, so a start that dies between them
// leaves a record beside a DpmMode that still asks for the clock, and the next start deletes that record by itself
// (clear_closed). The other order leaves the 0 without its record, which is the state BD-069 is about. The 0 is
// written whatever the record's status, because a start that tries DPM again is the worse failure.
static void PersistFallback(ULONG Reason)
{
    NTSTATUS record = StoreLogged(DPM_SETTING_CLOSED, Reason);
    (void)StoreLogged(DPM_SETTING_MODE, BC250_DPM_MODE_FIXED);
    (void)DeleteLogged(DPM_SETTING_CONFIRMED);
    (void)DeleteLogged(DPM_SETTING_PENDING);
    (void)DeleteLogged(DPM_SETTING_SESSION);
    (void)StoreLogged(DPM_SETTING_LAST_REASON, Reason);
    if (NT_SUCCESS(record))
        GuardLog("dpm: DpmClosedReason %lu written next to DpmMode 0: this fallback is the driver's own, "
                 "not a setting", Reason);
    else
        GuardLog("dpm: the record of this fallback is not durable; DpmMode 0 stays, and an installer reads it "
                 "as a setting");
}

static BOOLEAN Governing(const BC250_DPM_STATE* S)
{
    return S->Decision.mode == BC250_DPM_MODE_DPM && !S->GaveUp;
}

// Total GFX busy time up to Now, in KeQueryInterruptTime units. The submit path's two hooks can race a
// completion against a new submission; the sampler repairs what they left against the ring's own state,
// so an error lasts one tick at most.
static ULONGLONG DpmBusyTotal(BC250_DEVICE* Device, BC250_DPM_STATE* S, ULONGLONG Now, BOOLEAN* Inflight)
{
    BOOLEAN inflight = GfxSubmitBusy(Device);
    if (Inflight != NULL) *Inflight = inflight;        // the idle state's ring input (0.7.207)
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
    ULONGLONG NextHwmon;                    // the board's hardware monitor, its own cadence (hwmon.c)
    ULONGLONG LastLog;                      // interrupt time of the last telemetry block in the driver log
    ULONG IdleLogMs;                        // the block's period at the idle point (BD-097, Parameters\TelemetryIdleLogMs)
    ULONG Permille, ObservedMHz, ObservedVid, Target;
    // The fan control's load feed (fan.h): the busy share weighted by each tick's own length, gathered over the
    // whole second between two fan steps. One 25 ms tick is too short a window for a rule about a sustained load.
    ULONGLONG FanBusyWeighted;
    ULONG FanBusyMs;
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
    snap.TuneApplied = S->TuneTaken;
    snap.FloorTicks = g->floor_ticks;
    snap.SoftReleases = g->soft_releases;
    snap.WarmHolds = g->warm_holds;
    snap.RampHolds = g->ramp_holds;
    snap.ZoneSteps = g->zone_steps;
    snap.ZoneTicks = g->zone_ticks;
    snap.ZoneLeadMc = g->zone_lead_mc;
    snap.ZoneLeadOk = g->zone_lead_ok ? TRUE : FALSE;
    snap.ZoneLeadGaps = g->zone_lead_gaps;
    snap.ZoneIdleHolds = g->zone_idle_holds;
    // The idle point only while this start governs the clock: a fixed-lab start never configures the state, and
    // a start that gave up after SMU failures no longer steps the governor, so neither may name a point that
    // nothing would apply (0.7.207, review). The flag below follows the same rule.
    snap.IdleMHz = governing ? bc250_dpm_idle_mhz(g) : 0;
    snap.IdleHoldMs = g->idle_hold_ms;
    snap.IdleBusyPermille = g->idle_busy_permille;
    snap.IdleEntries = g->idle_entries;
    snap.IdleExits = g->idle_exits;
    snap.IdleRefusals = g->idle_refusals;
    snap.IdleMs = g->idle_total_ms;
    snap.IdleLeavePermille = g->idle_leave_permille;
    snap.IdleFastExits = g->idle_fast_exits;
    snap.IdleSlowExits = g->idle_slow_exits;
    if (Running && governing && g->idle) snap.Flags |= BC250_DPM_FLAG_IDLE;
    // The joint power arm (0.7.216.7): the policy's half from S->Joint, the CPU surface's half from its interlocked
    // values. JOINT_CAP follows the chip, not the wish: it is set while cpu.c reports the arm's limit applied.
    snap.JointReason = S->JointOn ? S->Joint.reason : BC250_JOINT_OFF;
    snap.JointWantMHz = S->JointOn ? S->Joint.cap_mhz : 0;
    snap.JointAppliedMHz = (ULONG)InterlockedCompareExchange(&Device->Cpu.JointAppliedMHz, 0, 0);
    snap.JointBaseMHz = (ULONG)InterlockedCompareExchange(&Device->Cpu.JointBaseMHz, 0, 0);
    snap.JointReady = InterlockedCompareExchange(&Device->Cpu.JointReady, 0, 0) ? TRUE : FALSE;
    snap.JointSends = (ULONG)InterlockedCompareExchange(&Device->Cpu.JointSends, 0, 0);
    snap.JointRefusals = (ULONG)InterlockedCompareExchange(&Device->Cpu.JointRefusals, 0, 0);
    snap.JointEngages = S->Joint.engages;
    snap.JointStepsDown = S->Joint.steps_down;
    snap.JointStepsUp = S->Joint.steps_up;
    snap.JointReleases = S->Joint.releases;
    if (S->JointOn) snap.Flags |= BC250_DPM_FLAG_JOINT;
    if (snap.JointAppliedMHz) snap.Flags |= BC250_DPM_FLAG_JOINT_CAP;
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
    KeAcquireSpinLock(&S->SnapLock, &irql);
    // The curve's own three fields and the voltage it asks for at the level the governor committed (0.7.210).
    // CurrentMv was set from the table above; this is the value that actually reached the hardware.
    snap.CurrentMv = bc250_dpm_curve_level_mv(&S->Curve, g->level);
    snap.CurveSerial = S->Curve.serial;
    snap.CurveApplied = S->Curve.applied;
    snap.CurveTrialRemainingMs = bc250_dpm_curve_remaining_ms(&S->Curve);
    if (!bc250_clock_curve_is_default(&S->Curve.active)) snap.Flags |= BC250_DPM_FLAG_CURVE;
    if (S->Curve.trial) snap.Flags |= BC250_DPM_FLAG_CURVE_TRIAL;
    S->Snap = snap;
    KeReleaseSpinLock(&S->SnapLock, irql);
}

static void DpmLogLine(const char* What, const BC250_DPM_SNAP* P)
{
    LONG t = P->TemperatureMc;
    GuardLog("dpm: %s %s %lu MHz/%lu mV (SMU %lu MHz VID %lu) %ld.%01ld C busy %lu.%lu%% avg %lu.%lu%% "
             "want %lu cap %lu max %lu throttle %s, raises %lu lowers %lu thermal %lu warm %lu ramp %lu errors %lu resyncs %lu, busy from %s "
             "(%lu samples), submit %lu.%lu%%, sdma %lu.%lu%%",
             What, P->Mode == BC250_DPM_MODE_DPM ? "dpm" : "fixed", P->CurrentMHz, P->CurrentMv,
             P->ObservedMHz, P->ObservedVid, t / 1000, (t < 0 ? -t : t) % 1000 / 100,
             P->BusyPermille / 10, P->BusyPermille % 10, P->BusyAvgPermille / 10, P->BusyAvgPermille % 10,
             P->WantMHz, P->CapMHz, P->MaxMHz,
             P->Throttle < BC250_DPM_THROTTLE_COUNT ? g_Throttle[P->Throttle] : "?",
             P->Raises, P->Lowers, P->ThermalEvents, P->WarmHolds, P->RampHolds, P->Errors, P->Resyncs,
             P->BusySource == BC250_DPM_BUSY_GRBM ? "grbm" : "submit", P->HwSamples,
             P->SubmitBusyPermille / 10, P->SubmitBusyPermille % 10, P->SdmaBusyPermille / 10, P->SdmaBusyPermille % 10);
}

// The idle state, on its own line: the telemetry line above is already near the log's 160 bytes. Only while
// the state is configured, so a start with DpmIdleMHz 0 logs exactly what 0.7.205 logged.
static void DpmLogIdleLine(const char* What, const BC250_DPM_SNAP* P)
{
    if (P->IdleMHz == 0 && P->IdleEntries == 0 && P->IdleRefusals == 0) return;
    GuardLog("dpm: %s idle %lu MHz%s, hold %lu ms under %lu permille, entries %lu exits %lu refusals %lu, "
             "%llu ms at the point", What, P->IdleMHz, (P->Flags & BC250_DPM_FLAG_IDLE) ? " (now)" : "",
             P->IdleHoldMs, P->IdleBusyPermille, P->IdleEntries, P->IdleExits, P->IdleRefusals, P->IdleMs);
    // The hysteresis (0.7.216.6) on a line of its own: the line above is near the log's width already. The exits it
    // counts are the ones by a busy tick and by the window; the rest of the exits are the other rules' (a thermal
    // rule, a runtime floor, SetStablePowerState, a stop or a power transition).
    GuardLog("dpm: %s idle leave at %lu permille, exits %lu by a busy tick, %lu by the window", What,
             P->IdleLeavePermille, P->IdleFastExits, P->IdleSlowExits);
}

// The joint power arm (0.7.216.7) on two lines of its own, only in a start that runs it: a start with
// DpmJointGovernor 0 logs exactly what 0.7.216.6 logged. The first line is the state, the second the counters: the
// policy's (engages, steps, releases) and the CPU surface's (changes that went through, refused attempts).
static void DpmLogJointLine(const char* What, const BC250_DPM_SNAP* P)
{
    if (!(P->Flags & BC250_DPM_FLAG_JOINT)) return;
    // Two lines, by the rule of the guardlog-width gate: one would be some 260 characters at its widest.
    GuardLog("dpm: %s joint %s, want %lu applied %lu base %lu MHz, CPU ready %lu", What,
             P->JointReason < BC250_JOINT_REASON_COUNT ? g_JointReason[P->JointReason] : "?", P->JointWantMHz,
             P->JointAppliedMHz, P->JointBaseMHz, P->JointReady ? 1ul : 0ul);
    GuardLog("dpm: %s joint engages %lu down %lu up %lu releases %lu, CPU sent %lu refused %lu", What,
             P->JointEngages, P->JointStepsDown, P->JointStepsUp, P->JointReleases, P->JointSends,
             P->JointRefusals);
}

// The curve's state beside the telemetry, only while it is not the table's own line or a trial runs, so a start
// that nobody tuned logs exactly what 0.7.207 logged (0.7.210).
static void DpmLogCurveLine(const char* What, const BC250_DPM_SNAP* P)
{
    if (!(P->Flags & (BC250_DPM_FLAG_CURVE | BC250_DPM_FLAG_CURVE_TRIAL))) return;
    // The trial marker goes on its own line: one line with both markers is 177 characters at its widest, over
    // the 159 a log line holds (the guardlog-width gate, BD-070).
    GuardLog("dpm: %s curve serial %lu applied %lu, %lu mV at %lu MHz", What, P->CurveSerial, P->CurveApplied,
             P->CurrentMv, P->CurrentMHz);
    if (P->Flags & BC250_DPM_FLAG_CURVE_TRIAL) {
        GuardLog("dpm: %s curve on trial", What);
        GuardLog("dpm: %s curve trial: %lu ms left before the stored curve comes back", What,
                 P->CurveTrialRemainingMs);
    }
}

// The one place a level reaches the hardware. TRUE when the hardware is at Level now.
static BOOLEAN DpmApply(BC250_DEVICE* Device, BC250_DPM_STATE* S, DPM_TICK* T, ULONG Level, const char* Why)
{
    struct bc250_clock_report report;
    ULONG from = bc250_dpm_level_mhz(S->Gov.level);
    // The voltage comes from the active curve (0.7.210), which is the table's own line until an operator sets
    // one. DpmLevelMv takes SnapLock, so the value cannot be half of an old curve and half of a new one.
    NTSTATUS status = SmuSetPoint(&Device->Smu, bc250_dpm_level_mhz(Level), DpmLevelMv(S, Level), &report);
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
        // The transaction's own gate saw 87 C (BC250_CLOCK_HOT_MC) before the governor's sample did: nothing was requested.
        GuardLog("dpm: %lu -> %lu MHz refused by the clock gate at %d mC (%s)", from,
                 bc250_dpm_level_mhz(Level), report.temperature_mc, Why);
        return FALSE;
    }
    if (Level < BC250_DPM_FLOOR_LEVEL && S->Gov.idle && Level == S->Gov.idle_level) {
        // The idle point (0.7.207), refused. Same reasoning as the sub-floor below: a property of this part,
        // not an SMU fault, so the give-up counter is untouched. The idle point falls back one step
        // (bc250_dpm_idle_refused: 500 MHz, then the thermal floor, then off) and the lab floor goes in now;
        // the next quiet window asks for whatever is left, so the refused point is never asked for twice.
        // The test is the governor's own state and not the level alone (0.7.207, review): after the first
        // fallback the idle point IS the thermal floor, and a refusal of it has to count as the second idle
        // refusal, not as a sub-floor one. bc250_dpm_idle_refused then withdraws the cap's sub-floor as well,
        // which is what the branch below would have done. A clamped idle target (the cap holds the part lower
        // than the point) is the cap's own request and takes that branch.
        bc250_dpm_idle_refused(&S->Gov);
        GuardLog("dpm: idle point refused: %lu MHz 0x%08X (clock %d, %u/%u msgs, read %u MHz VID %u), idle now "
                 "%lu MHz, refusals %lu (%s)", bc250_dpm_level_mhz(Level), status, report.status,
                 report.messages_completed, report.messages_attempted, report.observed_mhz, report.observed_vid,
                 bc250_dpm_idle_mhz(&S->Gov), S->Gov.idle_refusals, Why);
        InterlockedExchange(&S->Resync, 1);
        (void)DpmApply(Device, S, T, BC250_DPM_FLOOR_LEVEL, "idle refused");
        return FALSE;       // the hardware is not at Level; the floor apply above reported its own result
    }
    if (Level < BC250_DPM_FLOOR_LEVEL) {
        // A thermal-only point under the lab floor (0.7.205). The tables publish no level below 1000 MHz
        // (facts M47: SCLK levels 1000/1500/2000, and Linux clamps its sysfs there) although unit A's firmware
        // accepts 800 and 900 MHz (facts M785), so a refusal here is a
        // property of this part, not an SMU fault: the governor loses the sub-floor for the rest of the start,
        // the lab floor goes in instead, and the SMU give-up counter (BC250_DPM_ERROR_LIMIT) is untouched, so
        // one unsupported point never costs the whole DPM start.
        // Kept under BC250_LOG_TEXT (160 bytes), so the whole line survives in the driver log.
        GuardLog("dpm: sub-floor refused: %lu MHz 0x%08X (clock %d, %u/%u msgs, read %u MHz VID %u), cap stops at "
                 "%lu MHz for this start (%s)", bc250_dpm_level_mhz(Level), status, report.status,
                 report.messages_completed, report.messages_attempted, report.observed_mhz, report.observed_vid,
                 bc250_dpm_level_mhz(BC250_DPM_FLOOR_LEVEL), Why);
        bc250_dpm_subfloor_refused(&S->Gov);
        InterlockedExchange(&S->Resync, 1);
        (void)DpmApply(Device, S, T, BC250_DPM_FLOOR_LEVEL, "sub-floor refused");
        return FALSE;       // the hardware is not at Level; the floor apply above reported its own result
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

// The candidate has reached the hardware: record its serial (0.7.211). Only a transaction that went through
// calls this, which is what makes BC250_DPM_CURVE_FLAG_APPLIED and the KEEP gate mean anything.
static void DpmCurveApplied(BC250_DPM_STATE* S, ULONG Serial)
{
    KIRQL irql;
    KeAcquireSpinLock(&S->SnapLock, &irql);
    bc250_dpm_curve_applied(&S->Curve, Serial);
    S->Snap.CurveApplied = S->Curve.applied;
    KeReleaseSpinLock(&S->SnapLock, irql);
}

// Repeated SMU failures: the floor if it can still be had, the escape's SET back, fixed-lab next start.
static void DpmGiveUp(BC250_DEVICE* Device, BC250_DPM_STATE* S, DPM_TICK* T)
{
    BOOLEAN curveCancelled;
    KIRQL irql;
    S->ErrorsInRow = 0;
    // A curve trial ends here (0.7.211). After GaveUp the governor stops ticking, so bc250_dpm_curve_tick
    // never runs again and the window would never end: the window said "the stored curve comes back by
    // itself", and only RESET could escape, which also deletes the stored curve. The floor apply below carries
    // the stored curve's own voltage, and at the floor every curve is pinned to BC250_CLOCK_FLOOR_MV anyway.
    KeAcquireSpinLock(&S->SnapLock, &irql);
    curveCancelled = bc250_dpm_curve_cancel(&S->Curve) ? TRUE : FALSE;
    (void)bc250_dpm_curve_take(&S->Curve);
    KeReleaseSpinLock(&S->SnapLock, irql);
    if (curveCancelled) GuardLog("dpm: the curve trial ends with the give-up; the stored curve is back");
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
        // Against the ACTIVE curve, not the table (0.7.210). With a curve set, the table's own VID is not what the
        // hardware was asked for, and comparing against it would read every level as "not a table point" and drop
        // the clock to the floor at the first readback after a curve change.
        if (level >= 0 && vid == DpmLevelVid(S, (ULONG)level)) {
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

// ---- the joint power arm (0.7.216.7, C62) -----------------------------------------------------------------------
// The policy is bc250_joint_step (driver/shim/bc250_dpm.c); cpu.c's worker owns every message. The cap the policy wants
// is written every tick, because cpu.c clears its copy at its own start, and the worker is woken on a change.

static void DpmJointPublishWant(BC250_DEVICE* Device, ULONG Want, BOOLEAN Changed)
{
    InterlockedExchange(&Device->Cpu.JointWantMHz, (LONG)Want);
    if (Changed) KeSetEvent(&Device->Cpu.Wake, IO_NO_INCREMENT, FALSE);
}

// Right after bc250_dpm_step, so the arm reads the governor's state of this very tick. The thread's, under TickLock.
static void DpmJointTick(BC250_DEVICE* Device, BC250_DPM_STATE* S, const struct bc250_dpm_input* In)
{
    struct bc250_joint_input j;
    ULONG before = S->Joint.cap_mhz, want;
    LONG t = In->temperature_mc;
    bc250_joint_read(&S->Gov, In, &j);
    j.cpu_ready = InterlockedCompareExchange(&Device->Cpu.JointReady, 0, 0) ? 1 : 0;
    j.base_mhz = (ULONG)InterlockedCompareExchange(&Device->Cpu.JointBaseMHz, 0, 0);
    want = bc250_joint_step(&S->Joint, &j);
    DpmJointPublishWant(Device, want, want != before ? TRUE : FALSE);
    // One line per change of the cap: at most one per ENGAGE_MS, FREE_MS or STEP_MS (the host test's fuzz bound).
    if (want != before)
        GuardLog("dpm: joint CPU cap %lu -> %lu MHz (%s): busy avg %lu, %ld.%01ld C, GPU cap %lu MHz", before,
                 want, S->Joint.reason < BC250_JOINT_REASON_COUNT ? g_JointReason[S->Joint.reason] : "?",
                 j.busy_permille, t / 1000, (t < 0 ? -t : t) % 1000 / 100,
                 bc250_dpm_level_mhz(S->Gov.thermal_cap < S->Gov.max_level ? S->Gov.thermal_cap : S->Gov.max_level));
}

// The cap is gone outside a governing tick: a governor that gave up, a power transition, a stop. Idempotent.
static void DpmJointEnd(BC250_DEVICE* Device, BC250_DPM_STATE* S, const char* Why)
{
    ULONG before = S->Joint.cap_mhz;
    if (!S->JointOn) return;
    bc250_joint_reset(&S->Joint);
    DpmJointPublishWant(Device, 0, before ? TRUE : FALSE);
    if (before) GuardLog("dpm: joint CPU cap %lu -> 0 MHz (%s)", before, Why);
}

static void DpmTick(BC250_DEVICE* Device, BC250_DPM_STATE* S, DPM_TICK* T)
{
    ULONGLONG now = KeQueryInterruptTime();
    BOOLEAN inflight = FALSE;
    ULONGLONG busy = DpmBusyTotal(Device, S, now, &inflight);
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
        // Out of D0: what the fan's load feed had gathered goes, because the step that would have read it never
        // runs (FanPause gave the fan back already).
        T->FanBusyWeighted = 0;
        T->FanBusyMs = 0;
        DpmPublish(Device, S, T, TRUE);
        return;
    }
    T->FanBusyWeighted += (ULONGLONG)T->Permille * dtMs;
    T->FanBusyMs += dtMs;
    status = SmuReadTemperature(&Device->Smu, &T->TemperatureMc);
    T->TemperatureValid = NT_SUCCESS(status);
    governing = Governing(S);
    if (governing && InterlockedExchange(&S->Resync, 0)) DpmResyncLevel(Device, S, T);
    governing = Governing(S);
    if (governing) {
        struct bc250_dpm_input in;
        ULONG target;
        enum bc250_dpm_session_action action;
        // The curve's window and its forced re-apply (0.7.210). Both under SnapLock, because the escape writes
        // the same state: the tick is where a trial's deadline is noticed and where a changed curve reaches the
        // hardware. The revert is therefore the kernel's own act and survives a killed or hung tool.
        BOOLEAN curveReverted, curveTake;
        ULONG curveSerial;
        struct bc250_clock_curve curveNow;
        KIRQL curveIrql;
        KeAcquireSpinLock(&S->SnapLock, &curveIrql);
        curveReverted = bc250_dpm_curve_tick(&S->Curve, dtMs) ? TRUE : FALSE;
        curveTake = bc250_dpm_curve_take(&S->Curve) ? TRUE : FALSE;
        curveSerial = S->Curve.serial;
        curveNow = S->Curve.active;
        KeReleaseSpinLock(&S->SnapLock, curveIrql);
        if (curveReverted) DpmLogCurve("trial over, stored curve back", &curveNow);
        in.busy_permille = T->Permille;
        in.temperature_mc = T->TemperatureMc;
        in.temperature_valid = T->TemperatureValid;
        in.dt_ms = dtMs;
        // Work submitted and not yet retired, whatever the hardware samples said: the idle state (0.7.207)
        // does not leave the lab floor for the idle point while the ring holds anything. Since 0.7.216.6 the ring
        // no longer ends an idle episode by itself (every DWM frame is a submission): the episode ends at the first
        // tick whose busy share reaches BC250_DPM_IDLE_EXIT_PERMILLE, or when the trailing window's work reaches
        // DpmIdleLeavePermille. Exit latency: this tick's detection (the period is BC250_DPM_TICK_MS, 25 ms)
        // plus one SmuSetPoint, so some 25 to 35 ms for the first work of a burst. Neither figure is a bound.
        // This thread is an ordinary system thread, it waits a relative 25 ms after each tick, DpmPause holds
        // TickLock across a power transition, and a raise re-reads the clock up to BC250_CLOCK_SETTLE_READS
        // times with BC250_CLOCK_SETTLE_US between the reads. The figure is unmeasured on the hardware so far.
        in.ring_busy = inflight ? 1 : 0;
        // The paging node's share of this tick, 0 for a tick that had too few hardware samples (then only the
        // GFX submit accounting is left, and ring_busy carries it). The idle state reads it beside the GRBM
        // share, so an eviction or an upload with the GFX ring empty neither enters the state nor runs at the
        // idle point; the load governor keeps the GFX-only share in in.busy_permille.
        in.sdma_permille = T->SdmaPermille;
        S->Gov.stable = InterlockedCompareExchange(&S->Stable, 0, 0) != 0;
        DpmTakeTune(S);
        target = bc250_dpm_step(&S->Gov, &in);
        // The joint power arm reads this tick's governor and decides the CPU limit; it never changes target.
        if (S->JointOn) DpmJointTick(Device, S, &in);
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
        if (target != S->Gov.level) {
            // The level changes, so the transaction carries the new curve's voltage anyway.
            if (!DpmApply(Device, S, T, target, curveTake ? "load, curve" : "load")) {
                if (S->ErrorsInRow >= BC250_DPM_ERROR_LIMIT) DpmGiveUp(Device, S, T);
            } else if (curveTake) DpmCurveApplied(S, curveSerial);
        } else if (curveTake) {
            // The level does not change, so without this the new voltage would reach the hardware at the next
            // level change, which can be minutes away. One re-apply of the same clock: the settle loop does not
            // run, and only the forced voltage identifier moves.
            if (!DpmApply(Device, S, T, target, "curve")) {
                if (S->ErrorsInRow >= BC250_DPM_ERROR_LIMIT) DpmGiveUp(Device, S, T);
            } else DpmCurveApplied(S, curveSerial);
        }
    } else {
        // Fixed-lab: the same average, for the telemetry, and nothing else.
        S->Gov.avg_permille = (S->Gov.avg_permille * 3u + T->Permille + 2u) / 4u;
        T->Target = S->Gov.level;
        // A governor that gave up after SMU failures takes the joint arm's cap with it (0.7.216.7).
        DpmJointEnd(Device, S, "the governor stopped governing");
    }
    if (now >= T->NextVerify) {
        ULONG mhz = 0, vid = 0;
        LONG degrees = 0;
        T->NextVerify = now + 10000ull * BC250_DPM_VERIFY_MS;
        if (NT_SUCCESS(SmuReadClock(&Device->Smu, &mhz, &vid, &degrees))) {
            T->ObservedMHz = mhz;
            T->ObservedVid = vid;
            T->ClockAt = KeQueryInterruptTime();
            // Against the active curve, for the same reason as DpmResyncLevel (0.7.210).
            if (governing && Governing(S) &&
                (mhz != bc250_dpm_level_mhz(S->Gov.level) || vid != DpmLevelVid(S, S->Gov.level))) {
                GuardLog("dpm: readback %lu MHz VID %lu is not the committed %lu MHz VID %lu: resync", mhz, vid,
                         bc250_dpm_level_mhz(S->Gov.level), DpmLevelVid(S, S->Gov.level));
                InterlockedExchange(&S->Resync, 1);
            }
        }
    }
    // The SMU metrics table (0.7.215, smu_metrics.c): at most one read a second, on its own clock, after this tick's
    // own SMU traffic. Fixed-lab starts read it too. A refusal ends it for the boot and costs this tick nothing more
    // than the refused message; a paused governor never gets here.
    SmuMetricsSample(Device);
    if (now >= T->NextHwmon) {
        // The board's own hardware monitor (hwmon.c): the fan speed, the duty read-back and the chip's own
        // temperature channels. Its own counter and not NextVerify, so a change to the SMU readback period
        // cannot move the fan cadence. This thread is an ordinary system thread at PASSIVE_LEVEL, which is
        // what the chip's port sequence needs, and it runs in fixed-lab mode as well as under DPM. The gate
        // EnableHwmon decides whether anything happens at all; HwmonSample returns at once when it is closed.
        BC250_FAN_LOAD load;
        BC250_DPM_METRICS metrics;
        T->NextHwmon = now + 10000ull * BC250_HWMON_PERIOD_MS;
        HwmonSample(Device);
        // The fan control's load feed (fan.h, rule 10 of bc250_fan.h): the mean busy share of the whole second,
        // the clock, and the socket power of the metrics table when it is fresh. The fan decides nothing from a
        // single 25 ms tick, so the governor hands over the window, not the sample. The clock is the level the
        // governor asks for while it governs, which is not a read-back of the chip: the read-back T->ObservedMHz
        // comes at most every BC250_DPM_VERIFY_MS and is what a start without a governor has.
        RtlZeroMemory(&load, sizeof(load));
        load.Valid = TRUE;
        load.BusyPermille = T->FanBusyMs != 0 ? (ULONG)(T->FanBusyWeighted / T->FanBusyMs) : T->Permille;
        load.Mhz = Governing(S) ? bc250_dpm_level_mhz(S->Gov.level) : T->ObservedMHz;
        load.PowerValid = SmuMetricsFill(Device, &metrics);
        load.SocketMw = load.PowerValid ? metrics.SocketPowerMw : 0;
        T->FanBusyWeighted = 0;
        T->FanBusyMs = 0;
        // The fan control (fan.c) on the sample just taken and this tick's Tctl. Its own gate, EnableFanControl,
        // decides whether it writes anything; with the gate closed it only publishes its state.
        FanStep(Device, T->TemperatureMc, T->TemperatureValid, &load);
    }
    DpmPublish(Device, S, T, TRUE);
    if (now >= T->NextLog) {
        BC250_DPM_SNAP snap;
        KIRQL irql;
        struct bc250_dpm_tune defaults;
        ULONG serial;
        T->NextLog = now + 10000ull * BC250_DPM_LOG_MS;
        KeAcquireSpinLock(&S->SnapLock, &irql);
        snap = S->Snap;
        serial = S->TuneSerial;
        KeReleaseSpinLock(&S->SnapLock, irql);
        // BD-097: at the idle point the block comes every IdleLogMs (120 s by default) instead of every 5 s.
        // Its twelve lines a tick used to fill the 768 wrapping lines of the ring in about five minutes, so an idle
        // desktop lost every event (a mode set, a refusal) before anyone read it; at 60 s the measured idle ring
        // still held only about 3000 s of the plan's 3600 s. The rule is dpm_log_cadence.h, which the host test
        // drives. The check still runs every 5 s, so the first block after the governor leaves the idle point comes
        // within 5 s, and a trial's stream keeps its cadence.
        if (!Bc250DpmTelemetryDue((snap.Flags & BC250_DPM_FLAG_IDLE) != 0, T->IdleLogMs, T->LastLog, now))
            return;
        T->LastLog = now;
        DpmLogLine("telemetry", &snap);
        DpmLogIdleLine("telemetry", &snap);
        DpmLogJointLine("telemetry", &snap);
        HwmonLogLine(Device, "telemetry");
        SmuMetricsLogLine(Device, "telemetry");
        FanLogLine(Device, "telemetry");
        DpmLogCurveLine("telemetry", &snap);
        // A tuned governor says so next to every telemetry line (a trial's kernel stream then shows what ran).
        bc250_dpm_tune_default(&defaults);
        if (!TuneEqual(&S->Gov.tune, &defaults) || serial != S->TuneTaken)
            DpmLogTune("tune", &S->Gov.tune, serial, S->TuneTaken, S->Gov.floor_ticks, S->Gov.soft_releases,
                       &snap);
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
    tick.LastBusy = DpmBusyTotal(device, s, tick.Begin, NULL);
    tick.NextVerify = tick.Begin;
    tick.NextHwmon = tick.Begin;
    tick.NextLog = tick.Begin + 10000ull * BC250_DPM_LOG_MS;
    // Read once per thread start (PASSIVE_LEVEL here). 0 or a value at or below the 5 s period gives the old cadence;
    // out of range takes the default, said in the log.
    tick.IdleLogMs = GuardReadSetting(DPM_SETTING_IDLE_LOG, BC250_DPM_IDLE_LOG_MS);
    if (tick.IdleLogMs != Bc250DpmIdleLogMs(tick.IdleLogMs)) {
        GuardLog("dpm: TelemetryIdleLogMs %lu out of range (max %lu), %lu used", tick.IdleLogMs,
                 BC250_DPM_IDLE_LOG_MAX_MS, BC250_DPM_IDLE_LOG_MS);
        tick.IdleLogMs = Bc250DpmIdleLogMs(tick.IdleLogMs);
    }
    GuardLog("dpm: telemetry in the log every %lu ms, every %lu ms at the idle point", BC250_DPM_LOG_MS,
             Bc250DpmTelemetryPeriodMs(1, tick.IdleLogMs));
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

// The idle state's three settings, read once per start and handed to the policy (0.7.207). A refused value
// leaves the state off and says so: the clock then behaves as it did in 0.7.205. Absent values are the
// defaults of bc250_dpm.h, so a lab machine with no DpmIdle* value at all runs the owner's 500 MHz.
// A start that does not govern (fixed-lab, an unconfirmed or unclean DPM request, no SMU owner) never reaches
// bc250_dpm_step, so the state could not act: it stays off, the log says why, and the escape and the CLI then
// read "off" instead of naming a point that nothing would ever apply (0.7.207, review).
// PASSIVE_LEVEL, under Lock, before the governor thread exists.
static void DpmConfigureIdle(BC250_DPM_STATE* S, BOOLEAN Dpm)
{
    unsigned int mhz = 0, hold = 0, permille = 0, leave = 0;
    BOOLEAN mhzPresent, holdPresent, busyPresent, leavePresent;
    enum bc250_dpm_idle_error error;

    if (!Dpm) {
        // bc250_dpm_init left the state off; nothing to configure and nothing to publish.
        GuardLog("dpm: idle state off: this start does not govern the clock (reason %lu)", S->Decision.reason);
        return;
    }
    mhzPresent = QueryPresent(DPM_SETTING_IDLE_MHZ, &mhz);
    holdPresent = QueryPresent(DPM_SETTING_IDLE_HOLD, &hold);
    busyPresent = QueryPresent(DPM_SETTING_IDLE_BUSY, &permille);
    if (!mhzPresent) mhz = BC250_DPM_IDLE_MHZ;
    if (!holdPresent) hold = BC250_DPM_IDLE_HOLD_MS;
    if (!busyPresent) permille = BC250_DPM_IDLE_BUSY_PERMILLE;
    error = bc250_dpm_idle_config(&S->Gov, mhz, hold, permille);
    // The slow exit's share (0.7.216.6, DpmIdleLeavePermille): the idle point holds while the trailing window's work
    // stays under it, and a desktop frame on the GFX ring no longer leaves the point by itself. Only for a state the
    // three values above turned on; a refused value turns the state off and is logged like the others.
    leavePresent = QueryPresent(DPM_SETTING_IDLE_LEAVE, &leave);
    if (!leavePresent) leave = BC250_DPM_IDLE_LEAVE_PERMILLE;
    if (error == BC250_DPM_IDLE_OK && S->Gov.idle_on) error = bc250_dpm_idle_set_leave(&S->Gov, leave);
    GuardLog("dpm: idle DpmIdleMHz %lu%s hold %lu ms%s under %lu permille%s -> %s (error %d)", (ULONG)mhz,
             mhzPresent ? "" : " (absent)", (ULONG)hold, holdPresent ? "" : " (absent)", (ULONG)permille,
             busyPresent ? "" : " (absent)",
             bc250_dpm_idle_mhz(&S->Gov) ? "on" : (mhz == 0 ? "off by setting" : "off"), (int)error);
    GuardLog("dpm: idle DpmIdleLeavePermille %lu%s: leaves at %lu permille over the window, or %lu in one tick",
             (ULONG)leave, leavePresent ? "" : " (absent)", (ULONG)S->Gov.idle_leave_permille,
             (ULONG)BC250_DPM_IDLE_EXIT_PERMILLE);
}

// The joint power arm's switch (0.7.216.7, C62): DpmJointGovernor 1 in a governing start, and nothing else, runs it.
// A start that does not govern never reaches bc250_dpm_step, so the arm could not act there and stays off.
// PASSIVE_LEVEL, under Lock, before the governor thread exists.
static void DpmConfigureJoint(BC250_DPM_STATE* S, BOOLEAN Dpm)
{
    unsigned int value = 0;
    BOOLEAN present = QueryPresent(DPM_SETTING_JOINT, &value);
    bc250_joint_init(&S->Joint);
    S->JointOn = (Dpm && present && value == 1u) ? TRUE : FALSE;
    GuardLog("dpm: joint power arm DpmJointGovernor %lu%s -> %s", (ULONG)value, present ? "" : " (absent)",
             S->JointOn ? "on: the CPU limit comes down while the GPU is bound and held by heat"
                        : (Dpm ? "off" : "off: this start does not govern the clock"));
}

// The soft thermal zone's one switch (0.7.213, BD-087). DpmThermalZone 0 runs the 0.7.212 thermal rules for this whole
// start: the hot cap at 87 C is then the only rule that reads the temperature, there is no soft release and the warm zone
// is back at 87 C. Anything else, including an absent value, runs the zone with the defaults of bc250_dpm.h, which is
// what the search over the recorded sessions picked. The switch is for bisecting a regression against the rules the lab
// measured before this change, so it acts once, at the start, on both copies of the tune: the escape's (S->Tune, with its
// serial bumped so a reader sees the change) and the governor's own (S->Gov.tune, which the thread will run with). A
// run-time RUN_DPM_TUNE reset goes back to the shim's defaults and therefore turns the zone on again; the escape's
// THERMAL operation is the way to set the zone's numbers while a start runs.
// PASSIVE_LEVEL, under Lock, before the governor thread exists.
static void DpmConfigureZone(BC250_DPM_STATE* S, BOOLEAN Dpm)
{
    unsigned int value = 0;
    BOOLEAN present = QueryPresent(DPM_SETTING_ZONE, &value);
    BOOLEAN off = present && value == 0;
    enum bc250_dpm_tune_error error = BC250_DPM_TUNE_OK;

    if (off) {
        struct bc250_dpm_tune tune;
        ULONG serial;
        KIRQL irql;
        bc250_dpm_tune_default(&tune);
        bc250_dpm_tune_zone_off(&tune);
        error = bc250_dpm_set_tune(&S->Gov, &tune);     // the thread does not exist yet: no lock needed for Gov
        if (error == BC250_DPM_TUNE_OK) {
            KeAcquireSpinLock(&S->SnapLock, &irql);
            S->Tune = tune;
            serial = ++S->TuneSerial;
            KeReleaseSpinLock(&S->SnapLock, irql);
            S->TuneTaken = S->TuneRefused = serial;
        }
    }
    GuardLog("dpm: thermal zone DpmThermalZone %lu%s -> %s, error %d", (ULONG)value, present ? "" : " (absent)",
             bc250_dpm_zone_mc(&S->Gov.tune) ? "on" : "off", (int)error);
    GuardLog("dpm: thermal zone: steps from %d mC, no raise from %d mC%s",
             bc250_dpm_zone_mc(&S->Gov.tune) ? bc250_dpm_zone_mc(&S->Gov.tune) : (int)BC250_DPM_HOT_MC,
             bc250_dpm_warm_mc(&S->Gov.tune), Dpm ? "" : " (this start does not govern the clock)");
}

// This start's V/F curve, with the same two-mark guard as DPM itself (docs/design/tuner.md, ADR 0020). A curve
// is on disk only after a KEEP, so what is read here once ran a whole trial window on this machine; the guard
// covers what a trial cannot see, which is a curve that is stable for 25 s and not stable for an hour, or one
// that survives the desktop and not the next game. A start that finds the pending mark of an earlier start drops
// the curve from the disk and runs the table's own line: one bad KEEP costs the curve, never the machine.
// A start that does not govern never applies a level, so it leaves the state at the table's line.
// PASSIVE_LEVEL, under Lock, before the governor thread exists.
static void DpmInstallCurve(BC250_DPM_STATE* S, const struct bc250_clock_curve* Curve)
{
    KIRQL irql;
    KeAcquireSpinLock(&S->SnapLock, &irql);
    bc250_dpm_curve_init(&S->Curve, Curve);
    KeReleaseSpinLock(&S->SnapLock, irql);
}

static void DpmConfigureCurve(BC250_DPM_STATE* S, BOOLEAN Dpm)
{
    struct bc250_clock_curve curve;
    unsigned int trial = 0, pending = 0, confirmed = 0, sum;
    BOOLEAN any = FALSE, refused = FALSE, pendingPresent, trialPresent;

    bc250_clock_curve_default(&curve);
    DpmInstallCurve(S, NULL);
    S->CurvePending = S->CurveConfirmed = FALSE;
    trialPresent = QueryPresent(DPM_SETTING_CURVE_TRIAL, &trial);
    if (!trialPresent) trial = BC250_DPM_CURVE_TRIAL_MS;
    if (trial < BC250_DPM_CURVE_TRIAL_MIN_MS) trial = BC250_DPM_CURVE_TRIAL_MIN_MS;
    if (trial > BC250_DPM_CURVE_TRIAL_MAX_MS) trial = BC250_DPM_CURVE_TRIAL_MAX_MS;
    S->CurveTrialMs = trial;
    if (!Dpm) {
        // The clock never leaves the floor here, and the floor's own voltage is not a curve's to change.
        GuardLog("dpm: curve off: this start does not govern the clock (reason %lu); trial window %lu ms",
                 S->Decision.reason, S->CurveTrialMs);
        StoreLogged(DPM_SETTING_CURVE_REASON, DPM_CURVE_REASON_NOT_GOVERNING);
        return;
    }
    pendingPresent = QueryPresent(DPM_SETTING_CURVE_PENDING, &pending);
    (void)QueryPresent(DPM_SETTING_CURVE_CONFIRMED, &confirmed);
    DpmReadCurve(S, &curve, &any, &refused);
    if (any && pendingPresent) {
        GuardLog("dpm: the stored curve (mark 0x%04X) ran a start that never became healthy: the curve is "
                 "deleted and the table's own line runs", pending);
        any = FALSE;
    } else if (refused) {
        any = FALSE;
    }
    if (!any) {
        ULONG reason = pendingPresent ? DPM_CURVE_REASON_UNCONFIRMED
                                      : (refused ? DPM_CURVE_REASON_REFUSED : DPM_CURVE_REASON_NONE);
        // Nothing stored and nothing marked is the state of a machine nobody tuned: write no reason and leave
        // the key as it was, so a start on such a machine touches none of these values.
        if (pendingPresent || refused) {
            DpmDeleteCurve();
            DeleteLogged(DPM_SETTING_CURVE_PENDING);
            DeleteLogged(DPM_SETTING_CURVE_CONFIRMED);
            StoreLogged(DPM_SETTING_CURVE_REASON, reason);
        }
        return;
    }
    sum = bc250_clock_curve_checksum(&curve);
    // Durable before the first transaction that carries the curve, or the table's own line instead.
    if (!NT_SUCCESS(GuardStoreSetting(DPM_SETTING_CURVE_PENDING, sum))) {
        GuardLog("dpm: the curve's pending mark is not durable: the table's own line runs this start");
        StoreLogged(DPM_SETTING_CURVE_REASON, DPM_CURVE_REASON_REGISTRY);
        return;
    }
    S->CurvePending = TRUE;
    S->CurveConfirmed = FALSE;
    DpmInstallCurve(S, &curve);
    StoreLogged(DPM_SETTING_CURVE_REASON, DPM_CURVE_REASON_OK);
    DpmLogCurve("stored", &curve);
    GuardLog("dpm: the stored curve runs this start: mark 0x%04X, earlier mark 0x%04X, trial window %lu ms",
             sum, confirmed, S->CurveTrialMs);
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
    {
        // Runtime tuning is never carried into a new start: the defaults, and a log line when that drops something.
        struct bc250_dpm_tune old, defaults;
        ULONG serial;
        KIRQL irql;
        bc250_dpm_tune_default(&defaults);
        KeAcquireSpinLock(&s->SnapLock, &irql);
        old = s->Tune;
        s->Tune = defaults;
        if (!TuneEqual(&old, &defaults)) s->TuneSerial++;
        serial = s->TuneSerial;
        KeReleaseSpinLock(&s->SnapLock, irql);
        // bc250_dpm_init below gives Gov the defaults too: the thread starts with this serial taken.
        s->TuneTaken = s->TuneRefused = serial;
        if (!TuneEqual(&old, &defaults)) DpmLogTuneChange("start", &old, &defaults, serial);
    }
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
    s->JointOn = FALSE;                 // DpmConfigureJoint below, in a start that governs
    bc250_joint_init(&s->Joint);

    if (!Device->FullWddm) {
        d->reason = BC250_DPM_REASON_NOT_RUN;
        bc250_dpm_init(&s->Gov, BC250_DPM_FLOOR_LEVEL);
        DpmInstallCurve(s, NULL);       // the table's own line: nothing here applies a level
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
    r.closed_present = QueryPresent(DPM_SETTING_CLOSED, &r.closed);
    bc250_dpm_decide(&r, d);

    if (d->force_fixed) {
        GuardLog("dpm: falling back to fixed-lab and writing DpmMode = 0: %s (pending 0x%08X, session 0x%08X)",
                 d->reason == BC250_DPM_REASON_UNCONFIRMED ? "an earlier DPM start was never confirmed" :
                 "an earlier start ended above the floor", r.pending, r.session);
        // The record is the shim's answer, not this reason read again: what the host test asserts is what the
        // key gets (DpmGiveUp has no shim decision and names its own reason).
        PersistFallback(d->closed_reason);
    } else {
        if (d->clear_pending) (void)DeleteLogged(DPM_SETTING_PENDING);
        if (d->clear_session) (void)DeleteLogged(DPM_SETTING_SESSION);
        if (d->clear_closed) {
            // DpmMode is not 0 any more: the tester or a repair wrote over the fallback, so its record goes.
            (void)DeleteLogged(DPM_SETTING_CLOSED);
            GuardLog("dpm: DpmClosedReason %lu deleted: DpmMode %lu asks for the clock again", r.closed, r.mode);
        }
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
        (void)StoreLogged(DPM_SETTING_LAST_MODE, d->mode);
        (void)StoreLogged(DPM_SETTING_LAST_REASON, d->reason);
    }
    bc250_dpm_init(&s->Gov, d->max_level);
    DpmConfigureZone(s, d->mode == BC250_DPM_MODE_DPM);
    DpmConfigureIdle(s, d->mode == BC250_DPM_MODE_DPM);
    DpmConfigureCurve(s, d->mode == BC250_DPM_MODE_DPM);
    DpmConfigureJoint(s, d->mode == BC250_DPM_MODE_DPM && r.smu_online);
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
    ULONG serial = 0;

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
    // A trial never outlives the start that began it: the stored curve goes back before the floor apply below,
    // so the clock this driver leaves behind carries a voltage somebody kept, not one somebody was trying.
    {
        BOOLEAN reverted;
        KIRQL irql;
        KeAcquireSpinLock(&s->SnapLock, &irql);
        reverted = bc250_dpm_curve_cancel(&s->Curve) ? TRUE : FALSE;
        (void)bc250_dpm_curve_take(&s->Curve);
        serial = s->Curve.serial;
        KeReleaseSpinLock(&s->SnapLock, irql);
        if (reverted) GuardLog("dpm: the curve trial ended with the stop; the stored curve is back");
    }
    // The lab floor, from above or from a point below it (a thermal-only one since 0.7.205, the idle point
    // since 0.7.207): the clock gate admits a request up to the floor that does not raise the voltage however
    // hot the part is, so the hardware does not keep an untested clock after the driver has given up ownership.
    if (Governing(s) && s->Gov.level != BC250_DPM_FLOOR_LEVEL &&
        DpmApply(Device, s, &tick, BC250_DPM_FLOOR_LEVEL, "stop"))
        DpmCurveApplied(s, serial);      // the floor apply carried the stored curve's own voltage
    bc250_dpm_idle_leave(&s->Gov);      // the thread is joined; the clock is not at the idle point any more
    DpmJointEnd(Device, s, "stop");     // cpu.c's stop, which runs first, has taken the cap out of the chip
    // "Not above the floor" ends a start cleanly, the same reading the session marker itself uses
    // (bc250_dpm_session_step): since 0.7.205 the thermal cap can leave the governor at 800 or 900 MHz, and a
    // stop from there is clean. Testing for the floor alone left the marker behind whenever the floor apply
    // above did not go through, and the next start then read it as an unclean end and wrote DpmMode = 0.
    if (s->Session.marked && s->Gov.level <= BC250_DPM_FLOOR_LEVEL &&
        NT_SUCCESS(GuardDeleteSetting(DPM_SETTING_SESSION)))
        s->Session.marked = 0;
    InterlockedExchange(&Device->Smu.GovernorActive, 0);
    {
        BC250_DPM_SNAP snap;
        KIRQL irql;
        KeAcquireSpinLock(&s->SnapLock, &irql);
        s->Snap.Flags &= ~(BC250_DPM_FLAG_RUNNING | BC250_DPM_FLAG_GOVERNING | BC250_DPM_FLAG_SESSION |
                           BC250_DPM_FLAG_IDLE);
        if (s->Session.marked) s->Snap.Flags |= BC250_DPM_FLAG_SESSION;
        s->Snap.CurrentMHz = bc250_dpm_level_mhz(s->Gov.level);
        // The active curve's voltage, which the cancel above has already put back to the stored one. The lock is
        // ours here, so this is the policy call and not DpmLevelMv.
        s->Snap.CurrentMv = bc250_dpm_curve_level_mv(&s->Curve, s->Gov.level);
        s->Snap.Flags &= ~BC250_DPM_FLAG_CURVE_TRIAL;
        s->Snap.CurveSerial = s->Curve.serial;
        s->Snap.CurveApplied = s->Curve.applied;
        s->Snap.CurveTrialRemainingMs = 0;
        snap = s->Snap;
        KeReleaseSpinLock(&s->SnapLock, irql);
        DpmLogLine("stopped", &snap);
        DpmLogIdleLine("stopped", &snap);
        DpmLogJointLine("stopped", &snap);
        DpmLogCurveLine("stopped", &snap);
    }
    DpmUnlock(s);
}

// Before a transition out of D0: no SMU traffic from here until DpmResume, and the floor if it can be had.
// The tick in flight, if any, finishes first (TickLock). PASSIVE_LEVEL.
void DpmPause(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    DPM_TICK tick;
    ULONG serial = 0;
    if (!s->Created) return;
    InterlockedExchange(&s->Paused, 1);
    KeFlushQueuedDpcs();                // a sampler callback that read Paused as 0 has finished its reads
    KeWaitForSingleObject(&s->TickLock, Executive, KernelMode, FALSE, NULL);
    RtlZeroMemory(&tick, sizeof(tick));
    // As at a stop: a trial does not cross a power transition. The operator's window is seconds and the machine
    // may stay in D3 for hours, so a resume with a candidate still active would be a trial nobody watches.
    {
        BOOLEAN reverted;
        KIRQL irql;
        KeAcquireSpinLock(&s->SnapLock, &irql);
        reverted = bc250_dpm_curve_cancel(&s->Curve) ? TRUE : FALSE;
        (void)bc250_dpm_curve_take(&s->Curve);
        serial = s->Curve.serial;
        KeReleaseSpinLock(&s->SnapLock, irql);
        if (reverted) GuardLog("dpm: the curve trial ended with the power transition; the stored curve is back");
    }
    // As in DpmStop: the floor is reachable from every point below it at any temperature, so D3 is entered at
    // the lab point and the resume transaction (power.c, SmuPrepareClock) finds the clock it expects.
    if (Governing(s) && s->Gov.level != BC250_DPM_FLOOR_LEVEL &&
        DpmApply(Device, s, &tick, BC250_DPM_FLOOR_LEVEL, "power down"))
        DpmCurveApplied(s, serial);      // the floor apply carried the stored curve's own voltage
    // The thread holds TickLock, so Gov is ours here. Leaving the idle state means the governor after the resume
    // measures a whole quiet window again instead of asking for the point at its first tick.
    bc250_dpm_idle_leave(&s->Gov);
    // The joint arm starts again from no cap after the resume; CpuPause, which runs first, took it out of the chip.
    DpmJointEnd(Device, s, "power down");
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
    // The stored curve of this start, the same two steps in the same order (0.7.210). Its own failure costs the
    // curve at the next start and never this confirmation, so the result is logged and not returned.
    if (s->CurvePending) {
        struct bc250_clock_curve stored;
        unsigned int sum;
        KIRQL irql;
        NTSTATUS curveStatus;
        KeAcquireSpinLock(&s->SnapLock, &irql);
        stored = s->Curve.stored;
        KeReleaseSpinLock(&s->SnapLock, irql);
        sum = bc250_clock_curve_checksum(&stored);
        curveStatus = GuardDeleteSetting(DPM_SETTING_CURVE_PENDING);
        if (NT_SUCCESS(curveStatus)) {
            s->CurvePending = FALSE;
            curveStatus = GuardStoreSetting(DPM_SETTING_CURVE_CONFIRMED, sum);
            if (NT_SUCCESS(curveStatus)) s->CurveConfirmed = TRUE;
        }
        GuardLog("dpm: the stored curve (mark 0x%04X) confirmed by %s: 0x%08X", sum, Why, curveStatus);
    }
    DpmUnlock(s);
    return status;
}

// The kernel-log summary (BC250_ESCAPE_LOG_SUMMARY): one line with the last tick's telemetry.
void DpmLogSummary(BC250_DEVICE* Device)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    BC250_DPM_SNAP snap;
    struct bc250_dpm_tune tune;
    ULONG serial;
    KIRQL irql;
    KeAcquireSpinLock(&s->SnapLock, &irql);
    snap = s->Snap;
    tune = s->Tune;
    serial = s->TuneSerial;
    KeReleaseSpinLock(&s->SnapLock, irql);
    DpmLogLine("summary", &snap);
    DpmLogIdleLine("summary", &snap);
    DpmLogJointLine("summary", &snap);
    HwmonLogLine(Device, "summary");
    SmuMetricsLogLine(Device, "summary");
    FanLogLine(Device, "summary");
    DpmLogCurveLine("summary", &snap);
    // The values the escape stored (the governor takes them at its next tick: applied == serial once it has).
    DpmLogTune("summary tune", &tune, serial, snap.TuneApplied, snap.FloorTicks, snap.SoftReleases, &snap);
}

// BC250_ESCAPE_RUN_DPM. Software state only, so NoAdapterSynchronization=1 for both operations.
// Size (0.7.207, 0.7.215): display.c admits BC250_DPM_ABI3_SIZE, sizeof(BC250_ESCAPE_DPM) (ABI 2) and
// BC250_DPM_ABI1_SIZE only. With a shorter size nothing past that prefix is read or written: abi2 guards every ABI 2
// field and is also true for ABI 3, which contains them; abi3 alone guards the metrics tail. A size that is not its
// AbiVersion's is refused before anything else, as in DpmTuneRequest. The tail is smu_metrics.c's published copy:
// this escape sends no SMU message.
void DpmRequest(BC250_DEVICE* Device, BC250_ESCAPE_DPM* Data, ULONG Size, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    ULONGLONG ready = 0;
    BOOLEAN confirm = Data->Op == BC250_DPM_OP_CONFIRM;
    const ULONG abi = Data->AbiVersion;
    const BOOLEAN abi3 = abi == BC250_DPM_ABI_3 && Size == sizeof(BC250_ESCAPE_DPM_EX);
    const BOOLEAN abi2 = (abi == BC250_DPM_ABI && Size == sizeof(BC250_ESCAPE_DPM)) || abi3;
    const BOOLEAN abi1 = abi == BC250_DPM_ABI_1 && Size == BC250_DPM_ABI1_SIZE;
    BC250_ESCAPE_DPM_EX* ex = (BC250_ESCAPE_DPM_EX*)Data;  // dereferenced under abi3 only
    BC250_DPM_METRICS metrics;
    BOOLEAN power = FALSE;
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
    if (abi2) {
        Data->IdleMHz = Data->IdleHoldMs = Data->IdleBusyPermille = 0;
        Data->IdleEntries = Data->IdleExits = Data->IdleRefusals = 0;
        Data->IdleMs = 0;
    }
    if (abi3) RtlZeroMemory(&ex->Metrics, sizeof(ex->Metrics));
    if (!(abi1 || abi2) || Data->SubmitBusyPermille || Data->SdmaBusyPermille ||
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
    RtlZeroMemory(&metrics, sizeof(metrics));
    if (abi3) power = SmuMetricsFill(Device, &metrics);
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
    if (abi2) {
        Data->IdleMHz = snap.IdleMHz;
        Data->IdleHoldMs = snap.IdleHoldMs;
        Data->IdleBusyPermille = snap.IdleBusyPermille;
        Data->IdleEntries = snap.IdleEntries;
        Data->IdleExits = snap.IdleExits;
        Data->IdleRefusals = snap.IdleRefusals;
        Data->IdleMs = snap.IdleMs;
    }
    if (abi3) {
        ex->Metrics = metrics;
        if (power) Data->Flags |= BC250_DPM_FLAG_POWER;
    }
    Data->NtStatus = (ULONG)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// BC250_ESCAPE_RUN_DPM_TUNE (0.7.185). Software state only, so NoAdapterSynchronization=1 for every operation; what
// makes that safe:
// - Lifetime: the adapter context is valid while dxgkrnl calls DxgkDdiEscape, as for RUN_DPM; the StartHealth.Readers
//   rundown taken below keeps it past a RemoveDevice (StartHealthRemove waits for it before the context is freed).
//   Lock, SnapLock and Tune are set up in DpmInitialize (AddDevice) and live as long as the context.
// - Writes take Lock, the KMUTEX DpmStart, DpmStop and DpmConfirm hold: Created, Decision, Generation and GaveUp's
//   meaning cannot change under the check, and two writes cannot interleave their log lines. Inside it, SnapLock
//   covers the store of Tune and TuneSerial; the governor thread copies both under SnapLock at the start of a
//   governing tick (DpmTakeTune) and runs on its own copy (Gov.tune), so it never sees half a change. GaveUp is the
//   thread's: a write that races the governor giving up is accepted and never taken (Applied stays behind Serial).
// - Lock order: Readers (no wait) -> Lock -> SnapLock, and GuardLog after SnapLock is released. The thread takes
//   TickLock -> SnapLock and never Lock; DpmStop holds Lock while it joins the thread, which needs neither Lock nor
//   anything the escape holds; DpmPause takes TickLock only. No cycle.
// - No hardware: no register, no SMU message, nothing a power transition turns off. A new floor reaches the SMU only
//   through the thread's next tick and its checked transaction (SmuSetPoint); a paused governor (DpmPause, D3) takes
//   nothing until DpmResume, and DpmStop joins the thread before the owner stops. PASSIVE_LEVEL (the KMUTEX wait).
// - Size (0.7.197, 0.7.213): display.c admits sizeof (ABI 3), BC250_DPM_TUNE_ABI2_SIZE and BC250_DPM_TUNE_ABI1_SIZE
//   only. With a shorter size nothing past that prefix is read or written (abi2 and abi3 below guard every field of
//   their own ABI); a size that is not its AbiVersion's is refused before anything else.
void DpmTuneRequest(BC250_DEVICE* Device, BC250_ESCAPE_DPM_TUNE* Data, ULONG Size, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    // The inputs, read once; the same fields carry the outputs.
    const ULONG abi = Data->AbiVersion;
    const BOOLEAN abi3 = abi == BC250_DPM_TUNE_ABI && Size == sizeof(BC250_ESCAPE_DPM_TUNE);
    const BOOLEAN abi2 = (abi == BC250_DPM_TUNE_ABI_2 && Size == BC250_DPM_TUNE_ABI2_SIZE) || abi3;
    const BOOLEAN abi1 = abi == BC250_DPM_TUNE_ABI_1 && Size == BC250_DPM_TUNE_ABI1_SIZE;
    const ULONG op = Data->Op;
    const BOOLEAN write = op != BC250_DPM_TUNE_OP_READ;
    const ULONG floorMHz = Data->FloorMHz;
    const ULONGLONG expected = Data->ExpectedGeneration;
    struct bc250_dpm_tune request, old, defaults, input;
    enum bc250_dpm_tune_error error = BC250_DPM_TUNE_OK;
    ULONG serial = 0, applied, flags;
    ULONGLONG floorTicks;
    KIRQL irql;
    int floorLevel = floorMHz == 0 ? (int)BC250_DPM_FLOOR_LEVEL : bc250_dpm_level_of(floorMHz);
    BOOLEAN inputClean = !Data->Reserved[0] && !Data->Reserved[1];

    RtlZeroMemory(&input, sizeof(input));
    input.up_permille = Data->UpPermille;
    input.target_permille = Data->TargetPermille;
    input.down_permille = Data->DownPermille;
    input.down_hold_ms = Data->DownHoldMs;
    if (abi2) {
        input.hot_step_ms = Data->HotStepMs;
        input.soft_delta_mc = Data->SoftReleaseDeltaMc;
        input.soft_step_ms = Data->SoftReleaseStepMs;
        inputClean = inputClean && !Data->Reserved2[0] && !Data->Reserved2[1];
    }
    if (abi3) {
        input.zone_delta_mc = Data->ZoneDeltaMc;
        input.zone_step_ms = Data->ZoneStepMs;
        input.zone_lead_ms = Data->ZoneLeadMs;
        inputClean = inputClean && !Data->Reserved3;
    }
    expectedFlags.NoAdapterSynchronization = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)status;
    Data->Flags = Data->Error = Data->MaxMHz = Data->Mode = 0;
    Data->UpPermille = Data->TargetPermille = Data->DownPermille = Data->DownHoldMs = Data->FloorMHz = 0;
    Data->DefaultUpPermille = Data->DefaultTargetPermille = Data->DefaultDownPermille = Data->DefaultDownHoldMs = 0;
    Data->Serial = Data->Applied = 0;
    Data->FloorTicks = Data->Generation = 0;
    if (abi2) {
        Data->HotStepMs = Data->SoftReleaseDeltaMc = Data->SoftReleaseStepMs = 0;
        Data->DefaultHotStepMs = Data->DefaultSoftReleaseDeltaMc = Data->DefaultSoftReleaseStepMs = 0;
    }
    if (abi3) {
        Data->ZoneDeltaMc = Data->ZoneStepMs = Data->ZoneLeadMs = 0;
        Data->DefaultZoneDeltaMc = Data->DefaultZoneStepMs = Data->DefaultZoneLeadMs = Data->ZoneSlopeMs = 0;
    }
    if (!(abi1 || abi2) || !inputClean || op > (abi2 ? BC250_DPM_TUNE_OP_THERMAL : BC250_DPM_TUNE_OP_RESET) ||
        EscapeFlags != expectedFlags.Value) return;
    if (write && !Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    status = STATUS_SUCCESS;
    bc250_dpm_tune_default(&defaults);
    if (write) {
        DpmLock(s);
        KeAcquireSpinLock(&s->SnapLock, &irql);
        old = s->Tune;
        KeReleaseSpinLock(&s->SnapLock, irql);
        if (expected != s->Generation) status = STATUS_RETRY;
        else if (op != BC250_DPM_TUNE_OP_RESET &&
                 (!s->Created || s->Decision.mode != BC250_DPM_MODE_DPM || s->GaveUp)) status = STATUS_INVALID_DEVICE_STATE;
        else {
            // Each operation changes its own part and keeps the rest as stored (an ABI 1 THRESHOLDS keeps the thermal
            // timing an ABI 2 caller set).
            request = old;
            if (op == BC250_DPM_TUNE_OP_THRESHOLDS) {
                request.up_permille = input.up_permille;
                request.target_permille = input.target_permille;
                request.down_permille = input.down_permille;
                request.down_hold_ms = input.down_hold_ms;
            } else if (op == BC250_DPM_TUNE_OP_FLOOR) {
                if (floorLevel < 0) error = BC250_DPM_TUNE_FLOOR;       // not a clock of the table
                else request.floor_level = (unsigned int)floorLevel;
            } else if (op == BC250_DPM_TUNE_OP_THERMAL) {
                request.hot_step_ms = input.hot_step_ms;
                request.soft_delta_mc = input.soft_delta_mc;
                request.soft_step_ms = input.soft_step_ms;
                // The soft zone only from an ABI 3 caller (0.7.213): an ABI 2 tool writes the three fields it knows and
                // keeps the stored zone, so it cannot switch the zone off by leaving three zeros where it sees nothing.
                if (abi3) {
                    request.zone_delta_mc = input.zone_delta_mc;
                    request.zone_step_ms = input.zone_step_ms;
                    request.zone_lead_ms = input.zone_lead_ms;
                }
            } else request = defaults;
            if (error == BC250_DPM_TUNE_OK) error = bc250_dpm_tune_check(&request, s->Decision.max_level);
            if (error != BC250_DPM_TUNE_OK) status = STATUS_INVALID_PARAMETER;
            else if (!TuneEqual(&old, &request)) {
                KeAcquireSpinLock(&s->SnapLock, &irql);
                s->Tune = request;
                serial = ++s->TuneSerial;
                KeReleaseSpinLock(&s->SnapLock, irql);
                DpmLogTuneChange(op == BC250_DPM_TUNE_OP_THRESHOLDS ? "thresholds" :
                                 op == BC250_DPM_TUNE_OP_FLOOR ? "floor" :
                                 op == BC250_DPM_TUNE_OP_THERMAL ? "thermal" : "reset", &old, &request, serial);
            }
        }
        if (!NT_SUCCESS(status))
            GuardLog("dpm: tune op %lu refused 0x%08X (error %d)", op, status, (int)error);
        DpmUnlock(s);
    }
    // The reply: what is stored now, how far the thread got, and this start's frame.
    KeAcquireSpinLock(&s->SnapLock, &irql);
    request = s->Tune;
    serial = s->TuneSerial;
    applied = s->Snap.TuneApplied;
    floorTicks = s->Snap.FloorTicks;
    flags = s->Snap.Flags;
    Data->Mode = s->Snap.Mode;
    Data->MaxMHz = s->Snap.MaxMHz;
    Data->Generation = s->Snap.Generation;
    KeReleaseSpinLock(&s->SnapLock, irql);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);
    Data->Flags = ((flags & BC250_DPM_FLAG_GOVERNING) ? BC250_DPM_TUNE_FLAG_GOVERNING : 0) |
                  (request.up_permille != defaults.up_permille || request.target_permille != defaults.target_permille ||
                   request.down_permille != defaults.down_permille || request.down_hold_ms != defaults.down_hold_ms
                   ? BC250_DPM_TUNE_FLAG_THRESHOLDS : 0) |
                  (request.floor_level != BC250_DPM_FLOOR_LEVEL ? BC250_DPM_TUNE_FLAG_FLOOR : 0) |
                  (applied == serial ? BC250_DPM_TUNE_FLAG_APPLIED : 0) |
                  (abi2 && !ThermalEqual(&request, &defaults) ? BC250_DPM_TUNE_FLAG_THERMAL : 0) |
                  (abi3 && !ZoneEqual(&request, &defaults) ? BC250_DPM_TUNE_FLAG_ZONE : 0) |
                  (abi3 && request.zone_delta_mc == 0u ? BC250_DPM_TUNE_FLAG_ZONE_OFF : 0);
    if (abi2) {
        Data->HotStepMs = request.hot_step_ms;
        Data->SoftReleaseDeltaMc = request.soft_delta_mc;
        Data->SoftReleaseStepMs = request.soft_step_ms;
        Data->DefaultHotStepMs = defaults.hot_step_ms;
        Data->DefaultSoftReleaseDeltaMc = defaults.soft_delta_mc;
        Data->DefaultSoftReleaseStepMs = defaults.soft_step_ms;
    }
    if (abi3) {
        Data->ZoneDeltaMc = request.zone_delta_mc;
        Data->ZoneStepMs = request.zone_step_ms;
        Data->ZoneLeadMs = request.zone_lead_ms;
        Data->DefaultZoneDeltaMc = defaults.zone_delta_mc;
        Data->DefaultZoneStepMs = defaults.zone_step_ms;
        Data->DefaultZoneLeadMs = defaults.zone_lead_ms;
        Data->ZoneSlopeMs = BC250_DPM_ZONE_SLOPE_MS;
    }
    Data->UpPermille = request.up_permille;
    Data->TargetPermille = request.target_permille;
    Data->DownPermille = request.down_permille;
    Data->DownHoldMs = request.down_hold_ms;
    Data->FloorMHz = TuneFloorMHz(&request);
    Data->DefaultUpPermille = defaults.up_permille;
    Data->DefaultTargetPermille = defaults.target_permille;
    Data->DefaultDownPermille = defaults.down_permille;
    Data->DefaultDownHoldMs = defaults.down_hold_ms;
    Data->Serial = serial;
    Data->Applied = applied;
    Data->FloorTicks = floorTicks;
    Data->Error = (ULONG)error;
    Data->NtStatus = (ULONG)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}

// BC250_ESCAPE_RUN_DPM_CURVE (0.7.210). Software state only, like RUN_DPM_TUNE, so NoAdapterSynchronization=1 for
// every operation and no HardwareAccess: a new curve reaches the SMU through the governor's next tick and its
// checked transaction (DpmApply -> SmuSetPoint), never from this thread. What makes that safe is the argument of
// DpmTuneRequest above, with one addition: the governor reads the voltage column at every level change, so the
// escape writes the curve under SnapLock and the thread copies nothing - DpmLevelMv and DpmLevelVid take the same
// lock, and a torn read of 11 values could ask the firmware for a voltage nobody chose.
//
// The trial is the kernel's, not the tool's: a SET starts a window of its own, and the only ways out are a KEEP
// inside it, a CANCEL, the deadline, a stop and a power transition. A tool that is killed, hung or disconnected in
// the middle of a trial changes nothing about that. Nothing reaches the registry before a KEEP.
void DpmCurveRequest(BC250_DEVICE* Device, BC250_ESCAPE_DPM_CURVE* Data, ULONG Size, BOOLEAN Admin, ULONG EscapeFlags)
{
    BC250_DPM_STATE* s = &Device->Dpm;
    D3DDDI_ESCAPEFLAGS expectedFlags = {0};
    NTSTATUS status = STATUS_INVALID_PARAMETER;
    const ULONG op = Data->Op;
    const BOOLEAN write = op != BC250_DPM_CURVE_OP_READ;
    const ULONGLONG expected = Data->ExpectedGeneration;
    const ULONG trialIn = Data->TrialMs;
    struct bc250_clock_curve candidate, active, stored, defaults;
    enum bc250_clock_curve_error error = BC250_CLOCK_CURVE_OK;
    unsigned int errorLevel = 0, i;
    BC250_DPM_SNAP snap;
    ULONG trialMs = 0, remaining, serial, applied, sets, keeps, cancels, reverts, level;
    BOOLEAN onTrial, kept = FALSE, persisted = FALSE;
    KIRQL irql;

    RtlZeroMemory(&candidate, sizeof(candidate));
    RtlZeroMemory(&stored, sizeof(stored));
    for (i = 0; i < BC250_CURVE_POINTS; i++) candidate.mv[i] = Data->CandidateMv[i];
    bc250_clock_curve_default(&defaults);
    expectedFlags.NoAdapterSynchronization = 1;
    Data->Version = BC250_KMD_VERSION;
    Data->Status = BC250_ESCAPE_STATUS_REFUSED;
    Data->NtStatus = (ULONG)status;
    RtlZeroMemory(Data->CandidateMv, sizeof(Data->CandidateMv));
    Data->Flags = Data->Error = Data->ErrorLevel = 0;
    Data->TrialMs = Data->TrialRemainingMs = Data->Serial = Data->Applied = 0;
    if (Data->AbiVersion != BC250_DPM_CURVE_ABI || Size != sizeof(BC250_ESCAPE_DPM_CURVE) ||
        op > BC250_DPM_CURVE_OP_RESET || Data->Reserved[0] || Data->Reserved[1] ||
        EscapeFlags != expectedFlags.Value) return;
    if (write && !Admin) {
        Data->Status = BC250_ESCAPE_STATUS_NOT_ADMIN;
        Data->NtStatus = (ULONG)STATUS_ACCESS_DENIED;
        return;
    }
    if (!ExAcquireRundownProtection(&Device->StartHealth.Readers)) {
        Data->NtStatus = (ULONG)STATUS_DELETE_PENDING;
        return;
    }
    status = STATUS_SUCCESS;
    if (write) {
        DpmLock(s);
        if (expected != s->Generation) status = STATUS_RETRY;
        // RESET is also the way out of a curve that a start refused, so it does not need a governing start: it
        // deletes the registry values and leaves the table's own line behind. Every other write acts on a trial,
        // which only a running governor can carry.
        // CANCEL is admitted after a give-up as well (0.7.211): the governor has stopped ticking, so the
        // window cannot end by itself, and RESET - the only other way out - also deletes the stored curve.
        else if (op != BC250_DPM_CURVE_OP_RESET &&
                 (!s->Created || s->Decision.mode != BC250_DPM_MODE_DPM ||
                  (s->GaveUp && op != BC250_DPM_CURVE_OP_CANCEL)))
            status = STATUS_INVALID_DEVICE_STATE;
        else if (op == BC250_DPM_CURVE_OP_SET) {
            ULONG asked = trialIn ? trialIn : s->CurveTrialMs;
            KeAcquireSpinLock(&s->SnapLock, &irql);
            error = bc250_dpm_curve_set(&s->Curve, &candidate, asked, &errorLevel);
            asked = s->Curve.trial_ms;
            KeReleaseSpinLock(&s->SnapLock, irql);
            if (error != BC250_CLOCK_CURVE_OK) status = STATUS_INVALID_PARAMETER;
            else {
                DpmLogCurve("on trial", &candidate);
                GuardLog("dpm: the curve trial runs for %lu ms; the stored curve comes back by itself", asked);
            }
        } else if (op == BC250_DPM_CURVE_OP_KEEP) {
            int answer;
            KeAcquireSpinLock(&s->SnapLock, &irql);
            answer = bc250_dpm_curve_keep(&s->Curve);
            kept = answer == 1 ? TRUE : FALSE;
            stored = s->Curve.stored;
            KeReleaseSpinLock(&s->SnapLock, irql);
            // A candidate the governor has not carried yet is its own answer, and not "nothing was on trial":
            // the caller shows it and asks again a tick later (0.7.211).
            if (answer < 0) {
                error = BC250_CLOCK_CURVE_UNTRIED;
                status = STATUS_DEVICE_NOT_READY;
                GuardLog("dpm: the curve KEEP is refused: the governor has not applied the candidate yet");
            } else if (!kept) status = STATUS_INVALID_DEVICE_STATE;     // nothing was on trial
            else {
                // The disk now, in the guard's order: the values, then the pending mark away, then the new mark.
                // A crash between the values and the marks leaves a curve whose mark is the old one, and the next
                // start drops it: one reconfirmation at worst, never a curve nobody chose. The next start marks
                // this curve pending again by itself (DpmConfigureCurve), so a boot it does not survive still
                // costs the curve and not the machine.
                unsigned int sum = bc250_clock_curve_checksum(&stored);
                persisted = DpmStoreCurve(&stored);
                DeleteLogged(DPM_SETTING_CURVE_PENDING);
                s->CurvePending = FALSE;
                StoreLogged(DPM_SETTING_CURVE_CONFIRMED, sum);
                StoreLogged(DPM_SETTING_CURVE_REASON, DPM_CURVE_REASON_OK);
                s->CurveConfirmed = TRUE;
                DpmLogCurve("kept", &stored);
                GuardLog("dpm: the curve is kept, mark 0x%04X, on disk %s", sum,
                         persisted ? "complete" : "INCOMPLETE (the next start refuses it as a whole)");
                if (!persisted) status = STATUS_UNSUCCESSFUL;
            }
        } else if (op == BC250_DPM_CURVE_OP_CANCEL) {
            BOOLEAN reverted;
            KeAcquireSpinLock(&s->SnapLock, &irql);
            reverted = bc250_dpm_curve_cancel(&s->Curve) ? TRUE : FALSE;
            stored = s->Curve.stored;
            KeReleaseSpinLock(&s->SnapLock, irql);
            if (!reverted) status = STATUS_INVALID_DEVICE_STATE;
            else DpmLogCurve("trial cancelled, the stored curve is back", &stored);
        } else {
            BOOLEAN changed;
            KeAcquireSpinLock(&s->SnapLock, &irql);
            changed = bc250_dpm_curve_reset(&s->Curve) ? TRUE : FALSE;
            KeReleaseSpinLock(&s->SnapLock, irql);
            DpmDeleteCurve();
            DeleteLogged(DPM_SETTING_CURVE_PENDING);
            DeleteLogged(DPM_SETTING_CURVE_CONFIRMED);
            StoreLogged(DPM_SETTING_CURVE_REASON, DPM_CURVE_REASON_NONE);
            s->CurvePending = s->CurveConfirmed = FALSE;
            GuardLog("dpm: the curve is the table's own line again%s", changed ? "" : " (it already was)");
        }
        if (!NT_SUCCESS(status))
            GuardLog("dpm: curve op %lu refused 0x%08X (error %d at level %lu)", op, status, (int)error,
                     (ULONG)errorLevel);
        DpmUnlock(s);
    }
    // The reply: the three curves, the grid they live on, and where the governor is inside them.
    KeAcquireSpinLock(&s->SnapLock, &irql);
    active = s->Curve.active;
    stored = s->Curve.stored;
    candidate = s->Curve.candidate;
    onTrial = s->Curve.trial ? TRUE : FALSE;
    trialMs = s->Curve.trial ? s->Curve.trial_ms : s->CurveTrialMs;
    remaining = bc250_dpm_curve_remaining_ms(&s->Curve);
    serial = s->Curve.serial;
    applied = s->Curve.applied;
    sets = s->Curve.sets;
    keeps = s->Curve.keeps;
    cancels = s->Curve.cancels;
    reverts = s->Curve.reverts;
    level = s->Gov.level;
    snap = s->Snap;
    KeReleaseSpinLock(&s->SnapLock, irql);
    ExReleaseRundownProtection(&Device->StartHealth.Readers);
    for (i = 0; i < BC250_CURVE_POINTS; i++) {
        Data->CandidateMv[i] = onTrial ? candidate.mv[i] : 0;
        Data->ActiveMv[i] = active.mv[i];
        Data->StoredMv[i] = stored.mv[i];
        Data->DefaultMv[i] = defaults.mv[i];
        Data->FloorMv[i] = bc250_clock_floor_mv(bc250_dpm_level_mhz(BC250_CURVE_FIRST_LEVEL + i));
    }
    Data->FirstMHz = bc250_dpm_level_mhz(BC250_CURVE_FIRST_LEVEL);
    Data->StepMHz = BC250_CLOCK_STEP_MHZ;
    Data->Points = BC250_CURVE_POINTS;
    Data->TrialMs = trialMs;
    Data->TrialRemainingMs = remaining;
    Data->Serial = serial;
    Data->Applied = applied;
    Data->Error = (ULONG)error;
    Data->ErrorLevel = errorLevel;
    Data->Level = level;
    Data->LevelMHz = bc250_dpm_level_mhz(level);
    Data->LevelMv = bc250_clock_curve_mv(&active, level);
    Data->ObservedMHz = snap.ObservedMHz;
    Data->ObservedVid = snap.ObservedVid;
    Data->TemperatureMc = snap.TemperatureMc;
    Data->CeilingMHz = snap.MaxMHz;
    Data->Mode = snap.Mode;
    Data->Sets = sets;
    Data->Keeps = keeps;
    Data->Cancels = cancels;
    Data->Reverts = reverts;
    Data->Generation = snap.Generation;
    Data->Flags = (Device->FullWddm ? BC250_DPM_CURVE_FLAG_VALID : 0) |
                  (onTrial ? BC250_DPM_CURVE_FLAG_ON_TRIAL : 0) |
                  (bc250_clock_curve_is_default(&stored) ? 0 : BC250_DPM_CURVE_FLAG_STORED) |
                  (s->CurvePending ? BC250_DPM_CURVE_FLAG_PENDING : 0) |
                  (s->CurveConfirmed ? BC250_DPM_CURVE_FLAG_CONFIRMED : 0) |
                  (bc250_clock_curve_is_default(&active) ? BC250_DPM_CURVE_FLAG_DEFAULT : 0) |
                  ((snap.Flags & BC250_DPM_FLAG_GOVERNING) ? BC250_DPM_CURVE_FLAG_GOVERNING : 0) |
                  (applied == serial ? BC250_DPM_CURVE_FLAG_APPLIED : 0);
    Data->NtStatus = (ULONG)status;
    Data->Status = NT_SUCCESS(status) ? BC250_ESCAPE_STATUS_DONE : BC250_ESCAPE_STATUS_REFUSED;
}
