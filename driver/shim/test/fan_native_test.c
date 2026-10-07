/* Compiles the actual miniport binding of the fan control (driver/kmd/fan.c) and the reader it reads its inputs from
 * (driver/kmd/hwmon.c) on the host, replacing only the Windows kernel primitives, and drives both against the EC
 * model of hwmon_ec_mock.h with the M803 engine. What is under test here is the part the pure-shim test
 * (fan_test.c) cannot reach: the gate, the step's inputs, each exit path as the miniport calls it (device stop and
 * remove, out of D0, unload, the watchdog, the bugcheck callback, DxgkDdiResetDevice), the escape and the stored
 * choice.
 *
 * The rule the owner set is checked after every exit path: the chip is back at the board's own values, the mode
 * bit of fan 1 clear and the duty target at the value the board had.
 */
#define WIN32_NO_STATUS
#include "fan_native_mock.h"
#include "hwmon-fan.inc"
#include "fan-native.inc"

static BC250_DEVICE device;

#define SECOND (1000ull * 10000ull)
#define TARGET1 BC250_HWMON_REG_DUTY_WRITE(BC250_FAN_CHANNEL)

/* ---- the wire ------------------------------------------------------------------------------------------ */

/* The control app (KmdReply.ParseFan, checked by its FanTests.cs) reads this reply by byte offset: every offset is pinned. */
C_ASSERT(sizeof(BC250_ESCAPE_FAN) == 272);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Magic) == 0);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Command) == 4);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Status) == 8);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Version) == 12);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, NtStatus) == 16);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, AbiVersion) == 20);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Op) == 24);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Flags) == 28);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Mode) == 32);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, State) == 36);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Reason) == 40);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, DoubtReason) == 44);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Profile) == 48);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Points) == 52);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, CurveC) == 56);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, CurvePct) == 88);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, FixedPct) == 120);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, LeaseMs) == 124);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Store) == 128);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, TargetPct) == 132);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, AppliedPct) == 136);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, WrittenRaw) == 140);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, ReadbackRaw) == 144);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, GuardMc) == 148);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Rpm) == 152);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Channel) == 156);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, SavedMode) == 160);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, SavedTarget) == 164);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Error) == 168);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, StoredMode) == 172);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, StoredProfile) == 176);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Gate) == 180);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Takeovers) == 184);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Handbacks) == 192);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Writes) == 200);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Failures) == 208);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Emergencies) == 216);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Doubts) == 224);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, LeaseExpiries) == 232);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, WatchdogFires) == 240);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Generation) == 248);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, ExpectedGeneration) == 256);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_FAN, Reserved) == 264);

/* ---- helpers ------------------------------------------------------------------------------------------- */

static void Fresh(int enable)
{
    NativeClearSettings();
    ec_unit_a_m803(&native_ec);
    native_ec.data_admitted = 1;
    /* The EC's own reading of the die, 70.0 C, so that it agrees with the Tctl the steps below hand in. */
    ec_put16(&native_ec, BC250_HWMON_REG_MON(EC_MOCK_APU_CHANNEL), 0x4600u);
    native_ports_outside = 0;
    native_stalls = 0;
    native_lines = 0;
    native_rundown = 1;
    native_rundown_held = 0;
    native_delays = 0;
    native_base = BC250_HWMON_BASE_DEFAULT;
    native_time = 10ull * SECOND;
    memset(&device, 0, sizeof(device));
    device.StartHealth.Generation = 7;
    HwmonInitialize(&device.Hwmon);
    native_port_lock = &device.Hwmon.PortLock;
    FanInitialize(&device);
    native_busy = &device.Fan.Busy;
    NativeSetSetting(L"EnableHwmon", 1);
    if (enable >= 0) NativeSetSetting(L"EnableFanControl", (ULONG)enable);
}

static void Start(void)
{
    HwmonStart(&device);
    FanStart(&device);
}

/* One governor second: the reader's sample, then the fan step on this Tctl. */
static void Second(LONG tctl)
{
    native_time += SECOND;
    HwmonSample(&device);
    FanStep(&device, tctl, TRUE);
}

static int Held(void) { return (ec_peek8(&native_ec, BC250_HWMON_REG_MODE) & (1u << BC250_FAN_CHANNEL)) != 0u; }

static int AtRest(void)
{
    return ec_peek8(&native_ec, BC250_HWMON_REG_MODE) == BC250_HWMON_MODE_REST &&
           ec_peek8(&native_ec, TARGET1) == BC250_HWMON_TARGET_REST &&
           (ec_peek8(&native_ec, BC250_HWMON_REG_ENGINE) & BC250_HWMON_ENGINE_LOCK) != 0u;
}

/* Every data-port write went to one of the three admitted registers, inside a phase, through the latch. */
static int CleanWrites(int lockless)
{
    unsigned int i;

    for (i = 0; i < native_ec.logged; i++)
        if (!bc250_hwmon_write_allowed(native_ec.log[i].reg)) return 0;
    return native_ec.writes_other == 0 && native_ec.sequence_errors == 0 && native_ports_outside == 0 &&
           native_ec.protocol_errors == 0 && (lockless || native_ec.outside_hold == 0);
}

static void Ask(BC250_ESCAPE_FAN *f, ULONG op, BOOLEAN admin)
{
    D3DDDI_ESCAPEFLAGS flags = {0};

    flags.NoAdapterSynchronization = 1;
    f->Magic = BC250_ESCAPE_MAGIC;
    f->Command = BC250_ESCAPE_RUN_FAN;
    f->AbiVersion = BC250_FAN_ABI;
    f->Op = op;
    if (op != BC250_FAN_OP_READ && f->ExpectedGeneration == 0) f->ExpectedGeneration = device.StartHealth.Generation;
    FanRequest(&device, f, admin, flags.Value);
}

static void Read(BC250_ESCAPE_FAN *f)
{
    memset(f, 0, sizeof(*f));
    Ask(f, BC250_FAN_OP_READ, FALSE);
}

/* ---- the gate ------------------------------------------------------------------------------------------ */

static void gate(void)
{
    BC250_ESCAPE_FAN f;
    unsigned int i;

    /* Absent is on: the INF writes 1, and a machine without the value runs the control as well. */
    Fresh(-1);
    Start();
    CHECK(device.Fan.Enabled && device.Fan.Gate == BC250_FAN_GATE_OK);
    CHECK(device.Fan.Timer.Armed && device.Fan.BugCheck.Registered);
    Read(&f);
    CHECK(f.Status == BC250_ESCAPE_STATUS_DONE && f.Version == BC250_KMD_VERSION && f.Generation == 7u);
    CHECK((f.Flags & BC250_FAN_FLAG_ENABLED) && !(f.Flags & BC250_FAN_FLAG_CONTROLLING));
    CHECK(f.Mode == BC250_FAN_MODE_CURVE && f.Profile == BC250_FAN_PROFILE_STANDARD && f.Points == 5u);
    CHECK(f.CurveC[0] == 40u && f.CurvePct[0] == 50u && f.CurveC[4] == 85u && f.CurvePct[4] == 100u);
    CHECK(f.Channel == BC250_FAN_CHANNEL && f.State == BC250_FAN_STATE_BOARD);
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* EnableFanControl 0: not one data-port write, at any temperature, and the escape says why. */
    Fresh(0);
    Start();
    CHECK(!device.Fan.Enabled && device.Fan.Gate == BC250_FAN_GATE_SETTING);
    CHECK(!device.Fan.Timer.Armed && !device.Fan.BugCheck.Registered);
    for (i = 0; i < 10u; i++) Second(88000);
    CHECK(native_ec.writes_data == 0u && native_ec.logged == 0u && AtRest());
    Read(&f);
    CHECK((f.Flags & BC250_FAN_FLAG_GATED) && f.Gate == BC250_FAN_GATE_SETTING && f.State == BC250_FAN_STATE_OFF);
    memset(&f, 0, sizeof(f));
    f.FixedPct = 60;
    f.LeaseMs = 30000;
    Ask(&f, BC250_FAN_OP_FIXED, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_REFUSED && f.NtStatus == (ULONG)STATUS_INVALID_DEVICE_STATE);
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* No reader, no control. */
    Fresh(1);
    NativeSetSetting(L"EnableHwmon", 0);
    Start();
    CHECK(!device.Fan.Enabled && device.Fan.Gate == BC250_FAN_GATE_READER);
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* A chip that is not unit A's and not pinned: the driver does not guess. Pinned, it drives it. */
    Fresh(1);
    ec_put16(&native_ec, BC250_HWMON_REG_CUSTOMER, 0x0E2Cu);
    Start();
    CHECK(!device.Fan.Enabled && device.Fan.Gate == BC250_FAN_GATE_CHIP);
    for (i = 0; i < 5u; i++) Second(70000);
    CHECK(native_ec.writes_data == 0u);
    FanStop(&device, BC250_FAN_REASON_STOP);
    Fresh(1);
    ec_put16(&native_ec, BC250_HWMON_REG_CUSTOMER, 0x0E2Cu);
    NativeSetSetting(L"HwmonExpectId", 0x0E2Cu);
    Start();
    CHECK(device.Fan.Enabled);
    FanStop(&device, BC250_FAN_REASON_STOP);
}

/* ---- the take and every exit path --------------------------------------------------------------------- */

static void TakeAt70(void)
{
    Fresh(1);
    Start();
    Second(70000);
    CHECK(Held() && ec_peek8(&native_ec, TARGET1) == bc250_fan_pct_to_raw(82));
    CHECK(device.Fan.Ctl.controlling && device.Fan.Ctl.restore.valid);
    CHECK(device.Fan.Ctl.restore.mode == BC250_HWMON_MODE_REST &&
          device.Fan.Ctl.restore.target == BC250_HWMON_TARGET_REST);
}

/* The engine closes a phase the blind restore left open at its next status polls. */
static void SettleEngine(void)
{
    unsigned int i;

    for (i = 0; i < 8u; i++) (void)ec_live(&native_ec, BC250_HWMON_REG_ENGINE);
}

static void exit_paths(void)
{
    BC250_ESCAPE_FAN f;
    unsigned int i;

    /* The step: the take, then the fan follows, and the escape shows it. */
    TakeAt70();
    Second(70000);
    Second(70000);
    Read(&f);
    CHECK((f.Flags & BC250_FAN_FLAG_CONTROLLING) && (f.Flags & BC250_FAN_FLAG_RESTORE_SAVED));
    CHECK(f.State == BC250_FAN_STATE_CURVE && f.AppliedPct == 82u && f.WrittenRaw == 209u);
    CHECK(f.ReadbackRaw == 209u && f.Rpm == 209u * 1720u / 255u && f.GuardMc == 70000);
    CHECK(f.SavedMode == BC250_HWMON_MODE_REST && f.SavedTarget == BC250_HWMON_TARGET_REST && f.Takeovers == 1u);
    FanLogLine(&device, "telemetry");       /* the mock holds every line to the ring's 160 bytes */
    CHECK(CleanWrites(0));

    /* Device stop: the fan back, the watchdog and the callback gone, and a second stop does nothing. */
    FanStop(&device, BC250_FAN_REASON_STOP);
    CHECK(AtRest() && !device.Fan.Ctl.controlling && device.Fan.Ctl.reason == BC250_FAN_REASON_STOP);
    CHECK(!device.Fan.Timer.Armed && !device.Fan.BugCheck.Registered && !device.Fan.Configured);
    native_ec.logged = 0;
    FanStop(&device, BC250_FAN_REASON_STOP);
    FanDriverUnload();
    CHECK(native_ec.logged == 0u && CleanWrites(0));

    /* Out of D0: back to the board, PAUSED, no step while paused; at D0 the next step takes it again. */
    TakeAt70();
    FanPause(&device);
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_POWER);
    Read(&f);
    CHECK((f.Flags & BC250_FAN_FLAG_PAUSED) && !(f.Flags & BC250_FAN_FLAG_CONTROLLING));
    native_ec.logged = 0;
    Second(80000);
    CHECK(native_ec.logged == 0u && AtRest());
    FanResume(&device);
    Second(70000);
    CHECK(Held() && device.Fan.Ctl.takeovers == 2u);
    FanStop(&device, BC250_FAN_REASON_STOP);
    CHECK(AtRest() && CleanWrites(0));

    /* A lease does not survive the transition. */
    TakeAt70();
    memset(&f, 0, sizeof(f));
    f.FixedPct = 60;
    f.LeaseMs = 60000;
    Ask(&f, BC250_FAN_OP_FIXED, TRUE);
    Second(70000);
    CHECK(device.Fan.Ctl.mode == BC250_FAN_MODE_FIXED);
    FanPause(&device);
    FanResume(&device);
    Second(70000);
    CHECK(device.Fan.Ctl.mode == BC250_FAN_MODE_BOARD && AtRest());
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* The display-only branch stops the governor and the fan with it. */
    TakeAt70();
    FanStop(&device, BC250_FAN_REASON_POWER);
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_POWER);

    /* Unload with a device no stop reached. */
    TakeAt70();
    FanDriverUnload();
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_UNLOAD && !device.Fan.Configured);
    CHECK(!device.Fan.BugCheck.Registered && CleanWrites(0));

    /* The watchdog: quiet for 2 s, nothing; for 3 s, the fan goes back. The next live step takes it again. */
    TakeAt70();
    native_time += 2ull * SECOND;
    device.Fan.Dpc.Routine(&device.Fan.Dpc, device.Fan.Dpc.Context, NULL, NULL);
    CHECK(Held() && device.Fan.WatchdogFires == 0u);
    native_time += 1ull * SECOND;
    device.Fan.Dpc.Routine(&device.Fan.Dpc, device.Fan.Dpc.Context, NULL, NULL);
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_WATCHDOG && device.Fan.WatchdogFires == 1u);
    Read(&f);
    CHECK(f.WatchdogFires == 1u && f.Reason == BC250_FAN_REASON_WATCHDOG);
    native_time += 5ull * SECOND;
    device.Fan.Dpc.Routine(&device.Fan.Dpc, device.Fan.Dpc.Context, NULL, NULL);
    CHECK(device.Fan.WatchdogFires == 1u);      /* nothing held, nothing to do */
    Second(70000);
    CHECK(Held());
    CHECK(CleanWrites(0));
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* The watchdog against a step that never lets go of the controller: nothing for 6 s, then the blind restore,
     * once, without the lock and without the hold. */
    TakeAt70();
    device.Fan.Busy = 1;
    native_time += 4ull * SECOND;
    device.Fan.Dpc.Routine(&device.Fan.Dpc, device.Fan.Dpc.Context, NULL, NULL);
    CHECK(Held() && device.Fan.WatchdogFires == 0u);
    native_time += 2ull * SECOND;
    device.Fan.Dpc.Routine(&device.Fan.Dpc, device.Fan.Dpc.Context, NULL, NULL);
    SettleEngine();
    CHECK(AtRest() && device.Fan.BlindDone && device.Fan.WatchdogFires == 1u);
    native_ec.logged = 0;
    device.Fan.Dpc.Routine(&device.Fan.Dpc, device.Fan.Dpc.Context, NULL, NULL);
    CHECK(native_ec.logged == 0u && device.Fan.WatchdogFires == 1u);
    device.Fan.Busy = 0;
    CHECK(CleanWrites(1));
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* The bugcheck callback, as the kernel calls it: the buffer and the length it was registered with. */
    TakeAt70();
    native_ec.logged = 0;
    {
        int holds = native_ec.holds;

        device.Fan.BugCheck.Routine(device.Fan.BugCheck.Buffer, device.Fan.BugCheck.Length);
        CHECK(native_ec.holds == holds);        /* no lock taken */
    }
    SettleEngine();
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_BUGCHECK && !device.Fan.Ctl.controlling);
    CHECK(native_ec.logged == 4u && native_ec.log[1].reg == TARGET1 && native_ec.log[2].reg == BC250_HWMON_REG_MODE);
    CHECK(CleanWrites(1));
    /* A wrong length is not our record: nothing. */
    device.Fan.Ctl.controlling = 1;
    native_ec.logged = 0;
    device.Fan.BugCheck.Routine(device.Fan.BugCheck.Buffer, device.Fan.BugCheck.Length - 1u);
    CHECK(native_ec.logged == 0u);
    device.Fan.Ctl.controlling = 0;
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* DxgkDdiResetDevice: the same blind restore. */
    TakeAt70();
    FanResetDevice(&device);
    SettleEngine();
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_BUGCHECK);
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* The step's doubt: the reader goes offline under the control. Full speed, then the board. */
    TakeAt70();
    device.Hwmon.Online = FALSE;
    for (i = 0; i < 4u; i++) {
        native_time += SECOND;
        FanStep(&device, 70000, TRUE);
        CHECK(Held() && ec_peek8(&native_ec, TARGET1) == 255u);
    }
    native_time += SECOND;
    FanStep(&device, 70000, TRUE);
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_READER);
    FanStop(&device, BC250_FAN_REASON_STOP);

    /* The emergency through the miniport: Tctl 87 C and the EC agreeing. */
    TakeAt70();
    ec_put16(&native_ec, BC250_HWMON_REG_MON(EC_MOCK_APU_CHANNEL), 0x5700u);     /* 87.0 C */
    Second(87000);
    CHECK(Held() && ec_peek8(&native_ec, TARGET1) == 255u);
    Read(&f);
    CHECK((f.Flags & BC250_FAN_FLAG_EMERGENCY) && f.State == BC250_FAN_STATE_EMERGENCY && f.Emergencies == 1u);
    FanStop(&device, BC250_FAN_REASON_STOP);
    CHECK(AtRest() && CleanWrites(0));
}

/* ---- the escape and the stored choice ------------------------------------------------------------------ */

static void escape(void)
{
    BC250_ESCAPE_FAN f;
    D3DDDI_ESCAPEFLAGS flags = {0};
    unsigned int i;
    ULONG value = 0;

    TakeAt70();

    /* Refusals: no administrator, a stale generation, the wrong flags, a reserved word, an unknown operation. */
    memset(&f, 0, sizeof(f));
    f.FixedPct = 40;
    f.LeaseMs = 5000;
    Ask(&f, BC250_FAN_OP_FIXED, FALSE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_NOT_ADMIN && f.NtStatus == (ULONG)STATUS_ACCESS_DENIED);
    memset(&f, 0, sizeof(f));
    f.FixedPct = 40;
    f.LeaseMs = 5000;
    f.ExpectedGeneration = 6;
    Ask(&f, BC250_FAN_OP_FIXED, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_REFUSED && f.NtStatus == (ULONG)STATUS_RETRY);
    memset(&f, 0, sizeof(f));
    f.Magic = BC250_ESCAPE_MAGIC;
    f.Command = BC250_ESCAPE_RUN_FAN;
    f.AbiVersion = BC250_FAN_ABI;
    flags.HardwareAccess = 1;
    FanRequest(&device, &f, TRUE, flags.Value);
    CHECK(f.Status == BC250_ESCAPE_STATUS_REFUSED && f.NtStatus == (ULONG)STATUS_INVALID_PARAMETER);
    memset(&f, 0, sizeof(f));
    f.Reserved[1] = 1;
    Ask(&f, BC250_FAN_OP_READ, FALSE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_REFUSED);
    memset(&f, 0, sizeof(f));
    Ask(&f, BC250_FAN_OP_RENEW + 1u, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_REFUSED);
    native_rundown = 0;
    Read(&f);
    CHECK(f.Status == BC250_ESCAPE_STATUS_REFUSED && f.NtStatus == (ULONG)STATUS_DELETE_PENDING);
    native_rundown = 1;
    CHECK(device.Fan.Ctl.mode == BC250_FAN_MODE_CURVE);

    /* A fixed duty under a lease, its renewal and its end. */
    memset(&f, 0, sizeof(f));
    f.FixedPct = 40;
    f.LeaseMs = 5000;
    Ask(&f, BC250_FAN_OP_FIXED, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_DONE && f.Error == BC250_FAN_ERROR_OK);
    Second(70000);
    CHECK(ec_peek8(&native_ec, TARGET1) == 102u);
    Read(&f);
    CHECK(f.State == BC250_FAN_STATE_FIXED && (f.Flags & BC250_FAN_FLAG_LEASED) && f.FixedPct == 40u &&
          f.LeaseMs == 4000u);
    memset(&f, 0, sizeof(f));
    f.LeaseMs = 10000;
    Ask(&f, BC250_FAN_OP_RENEW, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_DONE);
    Second(70000);
    Read(&f);
    CHECK(f.LeaseMs == 9000u);
    for (i = 0; i < 9u; i++) Second(70000);
    Read(&f);
    /* The lease ends with the durable mode from before it, the Standard curve: the driver keeps the fan. */
    CHECK(Held() && f.Mode == BC250_FAN_MODE_CURVE && f.Profile == BC250_FAN_PROFILE_STANDARD && f.LeaseExpiries == 1u);
    CHECK(f.State == BC250_FAN_STATE_CURVE && !(f.Flags & BC250_FAN_FLAG_LEASED) && (f.Flags & BC250_FAN_FLAG_CONTROLLING));
    /* Over a durable board, the lease ends with the board. */
    memset(&f, 0, sizeof(f));
    Ask(&f, BC250_FAN_OP_BOARD, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_DONE);
    Second(70000);
    memset(&f, 0, sizeof(f));
    f.FixedPct = 40;
    f.LeaseMs = 5000;
    Ask(&f, BC250_FAN_OP_FIXED, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_DONE && f.Error == BC250_FAN_ERROR_OK);
    for (i = 0; i < 6u; i++) Second(70000);
    Read(&f);
    CHECK(AtRest() && f.Reason == BC250_FAN_REASON_LEASE && f.Mode == BC250_FAN_MODE_BOARD && f.LeaseExpiries == 2u);
    CHECK(!(f.Flags & BC250_FAN_FLAG_LEASED) && !(f.Flags & BC250_FAN_FLAG_CONTROLLING));

    /* Requests the policy refuses: nothing changes, and Error names the rule. */
    memset(&f, 0, sizeof(f));
    f.FixedPct = 40;
    f.LeaseMs = 5000;
    f.Store = 1;
    Ask(&f, BC250_FAN_OP_FIXED, TRUE);
    CHECK(f.NtStatus == (ULONG)STATUS_INVALID_PARAMETER && f.Error == BC250_FAN_ERROR_LEASE);
    memset(&f, 0, sizeof(f));
    f.Profile = BC250_FAN_PROFILE_CUSTOM;
    f.Points = 9;
    Ask(&f, BC250_FAN_OP_CURVE, TRUE);
    CHECK(f.NtStatus == (ULONG)STATUS_INVALID_PARAMETER && f.Error == BC250_FAN_ERROR_POINTS);
    memset(&f, 0, sizeof(f));
    f.FixedPct = 10;
    f.LeaseMs = 5000;
    Ask(&f, BC250_FAN_OP_FIXED, TRUE);
    CHECK(f.Error == BC250_FAN_ERROR_DUTY);
    CHECK(!NT_SUCCESS(GuardQuerySetting(L"FanMode", &value)));

    /* A custom curve, stored: on disk, in force at the next step, and the choice of the next start. */
    memset(&f, 0, sizeof(f));
    f.Profile = BC250_FAN_PROFILE_CUSTOM;
    f.Points = 3;
    f.CurveC[0] = 30; f.CurvePct[0] = 40;
    f.CurveC[1] = 60; f.CurvePct[1] = 60;
    f.CurveC[2] = 80; f.CurvePct[2] = 100;
    f.Store = 1;
    Ask(&f, BC250_FAN_OP_CURVE, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_DONE && (f.Flags & BC250_FAN_FLAG_STORED));
    CHECK(f.StoredMode == BC250_FAN_MODE_CURVE && f.StoredProfile == BC250_FAN_PROFILE_CUSTOM);
    CHECK(NT_SUCCESS(GuardQuerySetting(L"FanMode", &value)) && value == 1u);
    CHECK(NT_SUCCESS(GuardQuerySetting(L"FanProfile", &value)) && value == 0u);
    CHECK(NT_SUCCESS(GuardQuerySetting(L"FanCurvePoints", &value)) && value == 3u);
    CHECK(NT_SUCCESS(GuardQuerySetting(L"FanCurve2", &value)) && value == ((80u << 8) | 100u));
    CHECK(!NT_SUCCESS(GuardQuerySetting(L"FanCurve3", &value)));
    Second(70000);
    CHECK(Held() && ec_peek8(&native_ec, TARGET1) == bc250_fan_pct_to_raw(80));
    Read(&f);
    CHECK(f.Profile == BC250_FAN_PROFILE_CUSTOM && f.Points == 3u && f.CurvePct[2] == 100u);
    FanStop(&device, BC250_FAN_REASON_STOP);
    HwmonStop(&device.Hwmon);
    Start();
    CHECK(device.Fan.Ctl.profile == BC250_FAN_PROFILE_CUSTOM && device.Fan.Ctl.curve.points == 3u);
    Read(&f);
    CHECK((f.Flags & BC250_FAN_FLAG_STORED) && f.StoredProfile == BC250_FAN_PROFILE_CUSTOM);

    /* The board's own curve, stored: given back at the next step, and the next start writes nothing. */
    Second(70000);
    CHECK(Held());
    memset(&f, 0, sizeof(f));
    f.Store = 1;
    Ask(&f, BC250_FAN_OP_BOARD, TRUE);
    CHECK(f.Status == BC250_ESCAPE_STATUS_DONE);
    Second(70000);
    CHECK(AtRest() && device.Fan.Ctl.reason == BC250_FAN_REASON_USER);
    CHECK(NT_SUCCESS(GuardQuerySetting(L"FanMode", &value)) && value == 0u);
    FanStop(&device, BC250_FAN_REASON_STOP);
    HwmonStop(&device.Hwmon);
    native_ec.logged = 0;
    Start();
    for (i = 0; i < 5u; i++) Second(80000);
    CHECK(native_ec.logged == 0u && device.Fan.Ctl.mode == BC250_FAN_MODE_BOARD);
    /* And the driver's curve again, unstored: durable for this start only. */
    memset(&f, 0, sizeof(f));
    f.Profile = BC250_FAN_PROFILE_QUIET;
    Ask(&f, BC250_FAN_OP_CURVE, TRUE);
    Second(80000);      /* the guard stayed at 80 C through the five seconds above: quiet asks 80 % there */
    CHECK(Held() && ec_peek8(&native_ec, TARGET1) == bc250_fan_pct_to_raw(80));
    CHECK(NT_SUCCESS(GuardQuerySetting(L"FanMode", &value)) && value == 0u);
    FanStop(&device, BC250_FAN_REASON_STOP);
    CHECK(AtRest() && CleanWrites(0));

    /* A stored custom curve the policy refuses runs the standard curve, and the log says so. */
    Fresh(1);
    NativeSetSetting(L"FanMode", 1);
    NativeSetSetting(L"FanProfile", 0);
    NativeSetSetting(L"FanCurvePoints", 1);
    NativeSetSetting(L"FanCurve0", (50u << 8) | 50u);
    Start();
    CHECK(device.Fan.Ctl.profile == BC250_FAN_PROFILE_STANDARD && native_log_has("is refused"));
    FanStop(&device, BC250_FAN_REASON_STOP);
}

int main(void)
{
    gate();
    exit_paths();
    escape();
    printf("fan control binding: %ld checks, %ld failures\n", native_checks, native_failures);
    return native_failures == 0 ? 0 : 1;
}
