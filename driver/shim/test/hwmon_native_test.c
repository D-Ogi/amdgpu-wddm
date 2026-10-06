/* Compiles the actual miniport binding (driver/kmd/hwmon.c) on the host, replacing only the Windows kernel
 * primitives, and drives it against the EC model of hwmon_ec_mock.h. What is under test here is the part the
 * pure-shim test (hwmon_test.c) cannot reach: the registry gate, the start that never fails, the identity
 * refusals, the sampler's retry and give-up rules, the published snapshot, the ageing, and the escape.
 *
 * The one rule the whole design rests on is checked in every section: the only writes this driver issues go to
 * the page and the index port of its own EC window. ec_mock counts everything else, and every section asserts
 * that counter is still zero.
 */
#define WIN32_NO_STATUS
#include "hwmon_native_mock.h"
#include "hwmon-native.inc"

static BC250_DEVICE device;

/* ---- helpers ------------------------------------------------------------------------------------------- */

static void Fresh(void)
{
    NativeClearSettings();
    ec_unit_a(&native_ec);
    native_ports_outside = 0;
    native_stalls = 0;
    native_lines = 0;
    native_rundown = 1;
    native_rundown_held = 0;
    native_base = BC250_HWMON_BASE_DEFAULT;
    native_time = 10ull * 1000ull * 10000ull;       /* 10 s after boot: never the 0 that means "no sample" */
    memset(&device, 0, sizeof(device));
    device.StartHealth.Generation = 7;
    HwmonInitialize(&device.Hwmon);
    native_port_lock = &device.Hwmon.PortLock;
}

static void Read(BC250_ESCAPE_HWMON *h)
{
    D3DDDI_ESCAPEFLAGS flags = {0};

    flags.NoAdapterSynchronization = 1;
    memset(h, 0, sizeof(*h));
    h->Magic = BC250_ESCAPE_MAGIC;
    h->Command = BC250_ESCAPE_RUN_HWMON;
    h->AbiVersion = BC250_HWMON_ABI;
    h->Op = BC250_HWMON_OP_READ;
    HwmonRequest(&device, h, flags.Value);
}

static ULONG Fastest(const BC250_ESCAPE_HWMON *h)
{
    ULONG i, best = 0;

    for (i = 0; i < BC250_HWMON_FAN_SLOTS; i++)
        if (h->Rpm[i] > best) best = h->Rpm[i];
    return best;
}

static int NoWriteOutsideTheLatch(void)
{
    return native_ec.writes_other == 0 && native_ec.outside_hold == 0 && native_ec.sequence_errors == 0 &&
           native_ports_outside == 0;
}

/* ---- the wire ------------------------------------------------------------------------------------------ */

/* The GUI (tools/win/amdgpu_wddm_control/src/KmdReply.cs) parses this reply by byte offset, and the overlay's
 * test_telemetry.py compares the same layout. A silent change here is a wrong fan speed on a machine that still
 * runs the installed application, so the size and every offset a reader uses are pinned here - at compile time,
 * so that a drift fails the build instead of a run. */
C_ASSERT(sizeof(BC250_ESCAPE_HWMON) == 216);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Magic) == 0);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Command) == 4);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Status) == 8);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Version) == 12);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, NtStatus) == 16);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, AbiVersion) == 20);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Op) == 24);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Flags) == 28);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, BasePort) == 32);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, CustomerId) == 36);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, EcVersion) == 40);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, EcBuild) == 44);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, FanPresentMask) == 48);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, DutyPresentMask) == 52);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, ModeMask) == 56);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Rpm) == 60);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, DutyPermille) == 92);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, TemperatureMc) == 124);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, TemperatureSource) == 140);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, AgeMs) == 156);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Samples) == 160);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Errors) == 168);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Retries) == 176);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Generation) == 184);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Reason) == 192);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Engine) == 196);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, RpmValidMask) == 200);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, DutyValidMask) == 204);
C_ASSERT(FIELD_OFFSET(BC250_ESCAPE_HWMON, Refusals) == 208);
/* The freshness window is three missed samples, and nothing may silently make it something else. */
C_ASSERT(BC250_HWMON_FRESH_MS == BC250_HWMON_PERIOD_MS * BC250_HWMON_STALE_SAMPLES);

static void wire(void)
{
    BC250_ESCAPE_HWMON h;
    D3DDDI_ESCAPEFLAGS flags = {0};

    /* Rule 5 of hwmon.c: this escape is a software snapshot read. If NoAdapterSynchronization ever moved, the
     * comparison in HwmonRequest would start admitting a Level Two escape, which stalls a running game. */
    flags.NoAdapterSynchronization = 1;
    CHECK(flags.Value == 8);
    flags.Value = 0;
    flags.HardwareAccess = 1;
    CHECK(flags.Value == 1);

    /* An unasked question gets no answer and no port access. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    HwmonStart(&device);
    CHECK(device.Hwmon.Online);
    HwmonSample(&device);
    {
        unsigned int reads = native_ec.reads;

        Read(&h);
        CHECK(h.Status == BC250_ESCAPE_STATUS_DONE && h.NtStatus == (ULONG)STATUS_SUCCESS);
        CHECK(h.Version == BC250_KMD_VERSION);
        CHECK(native_ec.reads == reads);            /* the escape reads the snapshot, never the chip */

        Read(&h);
        h.AbiVersion = BC250_HWMON_ABI + 1u;
        HwmonRequest(&device, &h, 8);
        CHECK(h.Status == BC250_ESCAPE_STATUS_REFUSED && h.NtStatus == (ULONG)STATUS_INVALID_PARAMETER);
        CHECK(h.Flags == 0 && h.BasePort == 0 && Fastest(&h) == 0 && h.Samples == 0 && h.Generation == 0);

        memset(&h, 0, sizeof(h));
        h.AbiVersion = BC250_HWMON_ABI;
        h.Op = BC250_HWMON_OP_READ + 1u;
        HwmonRequest(&device, &h, 8);
        CHECK(h.Status == BC250_ESCAPE_STATUS_REFUSED && h.Flags == 0);

        /* A HardwareAccess escape is refused outright, rather than quietly served: a caller that asked for one
         * has the wrong idea about what this costs, and the refusal is where that is corrected. */
        memset(&h, 0, sizeof(h));
        h.AbiVersion = BC250_HWMON_ABI;
        h.Op = BC250_HWMON_OP_READ;
        flags.Value = 0;
        flags.NoAdapterSynchronization = 1;
        flags.HardwareAccess = 1;
        HwmonRequest(&device, &h, flags.Value);
        CHECK(h.Status == BC250_ESCAPE_STATUS_REFUSED && h.Flags == 0);
        CHECK(native_ec.reads == reads);

        /* A tear-down in flight. The reader never walks a snapshot the adapter is dismantling. */
        native_rundown = 0;
        Read(&h);
        CHECK(h.Status == BC250_ESCAPE_STATUS_REFUSED && h.NtStatus == (ULONG)STATUS_DELETE_PENDING);
        CHECK(h.Flags == 0 && Fastest(&h) == 0);
        native_rundown = 1;
        CHECK(native_rundown_held == 0);
    }
    CHECK(NoWriteOutsideTheLatch());
}

/* ---- the gate ------------------------------------------------------------------------------------------ */

static void gate(void)
{
    BC250_ESCAPE_HWMON h;

    /* No setting at all. This is every machine that installs the driver: the window is shared with whatever
     * third-party monitor the owner runs, so nothing may touch it until somebody opens the gate by hand. */
    Fresh();
    HwmonStart(&device);
    CHECK(!device.Hwmon.Enabled && !device.Hwmon.Online);
    CHECK(native_ec.reads == 0 && native_ec.writes_latch == 0 && native_ec.holds == 0);
    CHECK(native_stalls == 0);
    CHECK(device.Hwmon.Reason == BC250_HWMON_REASON_GATED && device.Hwmon.BasePort == 0);
    CHECK(native_log_has("EnableHwmon 0"));
    Read(&h);
    CHECK(h.Status == BC250_ESCAPE_STATUS_DONE);
    CHECK(h.Flags == BC250_HWMON_FLAG_GATED && h.Reason == BC250_HWMON_REASON_GATED);
    CHECK(h.BasePort == 0 && h.AgeMs == 0 && h.Generation == 7);

    /* The sampler and the log line do nothing while the gate is closed, and HwmonStop says nothing either:
     * a machine with the gate closed behaves exactly as the revision before this one did. */
    native_lines = 0;
    HwmonSample(&device);
    HwmonLogLine(&device, "telemetry");
    HwmonStop(&device.Hwmon);
    CHECK(native_lines == 0 && native_ec.reads == 0 && native_ec.writes_latch == 0);

    /* Explicitly 0, and an unrelated value. Only 1 opens it. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 0);
    HwmonStart(&device);
    CHECK(!device.Hwmon.Enabled && native_ec.writes_latch == 0);
    Fresh();
    NativeSetSetting(L"EnableHwmon", 2);
    HwmonStart(&device);
    CHECK(!device.Hwmon.Enabled && native_ec.writes_latch == 0);
    CHECK(NoWriteOutsideTheLatch());

    /* A base the chip cannot sit on. Refused before any port access, which is the point: a typo in the
     * registry must not put a byte on some other device's port. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonBasePort", 0x0A21);
    HwmonStart(&device);
    CHECK(device.Hwmon.Enabled && !device.Hwmon.Online);
    CHECK(device.Hwmon.Reason == BC250_HWMON_REASON_BASE && device.Hwmon.BasePort == 0);
    CHECK(native_ec.reads == 0 && native_ec.writes_latch == 0 && native_ports_outside == 0);
    Read(&h);
    CHECK(h.Flags == 0 && h.Reason == BC250_HWMON_REASON_BASE);   /* enabled, so not GATED; offline, so not VALID */
    CHECK(NoWriteOutsideTheLatch());

    /* The one an operator could actually type: 0x0CF8 is printed in this design as the fan engine status
     * register, and it is also the PCI configuration ADDRESS port, with its DATA port at base + 4. The two
     * upstream rules admit it. This driver must not, because a sample would then write 0xFF and a page byte
     * into the configuration space of whatever device the last selector named, ninety times a second. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonBasePort", 0x0CF8);
    HwmonStart(&device);
    CHECK(device.Hwmon.Enabled && !device.Hwmon.Online);
    CHECK(device.Hwmon.Reason == BC250_HWMON_REASON_BASE && device.Hwmon.BasePort == 0);
    CHECK(native_ec.reads == 0 && native_ec.writes_latch == 0 && native_ports_outside == 0);
    CHECK(native_log_has("0x0CF8"));
    HwmonSample(&device);
    CHECK(native_ec.writes_latch == 0 && native_ports_outside == 0);
    CHECK(NoWriteOutsideTheLatch());

    /* A base of 0 means "the one measured on this board", and the driver must actually use it. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonBasePort", 0);
    HwmonStart(&device);
    CHECK(device.Hwmon.Online && device.Hwmon.BasePort == BC250_HWMON_BASE_DEFAULT);
    CHECK(native_ports_outside == 0);

    /* Another admitted base: the second window the DSDT reports. The model answers only on the window the
     * test configured, so a driver that ignored HwmonBasePort and used its own constant would count ports
     * outside and fail here. */
    Fresh();
    native_base = BC250_HWMON_BASE_ALT_2;
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonBasePort", BC250_HWMON_BASE_ALT_2);
    HwmonStart(&device);
    CHECK(device.Hwmon.Online && device.Hwmon.BasePort == BC250_HWMON_BASE_ALT_2);
    CHECK(native_ports_outside == 0);
    HwmonSample(&device);
    Read(&h);
    CHECK(h.BasePort == BC250_HWMON_BASE_ALT_2 && Fastest(&h) == 1589);
    CHECK(NoWriteOutsideTheLatch());
}

/* ---- the start ----------------------------------------------------------------------------------------- */

static void start(void)
{
    BC250_ESCAPE_HWMON h;
    ULONG i;

    /* Unit A as E01 measured it. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    HwmonStart(&device);
    CHECK(device.Hwmon.Online && device.Hwmon.Reason == BC250_HWMON_REASON_OK);
    CHECK(device.Hwmon.Identity.version == 0x0100);
    CHECK(device.Hwmon.Identity.build == ((21u << 16) | (7u << 8) | 28u));
    CHECK(device.Hwmon.Identity.customer_id == EC_MOCK_UNIT_A_CUSTOMER);
    CHECK(device.Hwmon.Identity.monitoring != 0);
    CHECK(device.Hwmon.Identity.fan_present == 0x1F && device.Hwmon.Identity.duty_present == 0x1F);
    CHECK(device.Hwmon.Identity.temperatures == 3 && device.Hwmon.Identity.voltages == 6);
    CHECK(!device.Hwmon.IdPinned && !device.Hwmon.DutyProven);
    /* The identity line carries every value it read: this is the line an operator pins the customer ID from,
     * and the one that answers the stage-1 question about the two UNPROVEN registers at rest. */
    CHECK(native_log_has("base 0x0A20") && native_log_has("ec 1.0") && native_log_has("build 07/28/21"));
    CHECK(native_log_has("temps 3 volts 6 mode 0x00 eng 0x00"));
    CHECK(native_log_has("is not pinned"));
    /* A 16-bit read is eight accesses in one hold, and no hold ever carried more: the index latch cannot move
     * between the two halves of a tachometer. */
    CHECK(native_ec.max_accesses_in_hold == 8);
    CHECK(native_ec.reads > 0 && native_stalls == native_ec.reads + native_ec.writes_latch);
    CHECK(NoWriteOutsideTheLatch());
    Read(&h);
    CHECK(h.Flags == (BC250_HWMON_FLAG_VALID | BC250_HWMON_FLAG_MONITORING));
    CHECK(h.BasePort == BC250_HWMON_BASE_DEFAULT && h.CustomerId == EC_MOCK_UNIT_A_CUSTOMER);
    CHECK(h.EcVersion == 0x0100 && h.EcBuild == ((21u << 16) | (7u << 8) | 28u));
    CHECK(h.FanPresentMask == 0x1F && h.DutyPresentMask == 0x1F);
    CHECK(h.AgeMs == 0 && h.Samples == 0);          /* the start publishes an identity, not a reading */
    CHECK(Fastest(&h) == 0);

    /* The pinned chip answered. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonExpectId", EC_MOCK_UNIT_A_CUSTOMER);
    HwmonStart(&device);
    CHECK(device.Hwmon.Online && device.Hwmon.IdPinned);
    CHECK(!native_log_has("is not pinned"));
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_ID_PINNED) != 0);

    /* A different chip answered. The reader does not guess, and it says which two values disagree. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonExpectId", 0x1234);
    HwmonStart(&device);
    CHECK(!device.Hwmon.Online && device.Hwmon.Reason == BC250_HWMON_REASON_CUSTOMER);
    CHECK(native_log_has("HwmonExpectId 0x1234"));
    HwmonSample(&device);
    Read(&h);
    CHECK(h.Flags == 0 && h.Reason == BC250_HWMON_REASON_CUSTOMER && h.Samples == 0);

    /* HwmonDutyProven only opens the flag the control application needs to show a percentage. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonDutyProven", 1);
    HwmonStart(&device);
    HwmonSample(&device);
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_DUTY_PROVEN) != 0);
    CHECK(h.DutyPermille[0] == 961);                /* 245 of 255, rounded to nearest */

    /* The firmware is not monitoring. Upstream nct6683 sets HWM_CFG bit 7 at that point; we refuse instead,
     * because writing a configuration register of a chip the BIOS owns is not a fan reader's business. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    ec_put8(&native_ec, BC250_HWMON_REG_CFG, 0x00);
    HwmonStart(&device);
    CHECK(!device.Hwmon.Online && device.Hwmon.Reason == BC250_HWMON_REASON_MONITORING);
    CHECK(ec_peek8(&native_ec, BC250_HWMON_REG_CFG) == 0x00);   /* and it stayed clear */
    CHECK(NoWriteOutsideTheLatch());
    Read(&h);
    CHECK(h.Flags == 0 && h.Reason == BC250_HWMON_REASON_MONITORING);

    /* Nothing on the window: every byte reads 0xFF, which is what an absent or busy chip looks like. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    native_ec.answer_fixed = 1;
    native_ec.fixed = 0xFF;
    HwmonStart(&device);
    CHECK(!device.Hwmon.Online && device.Hwmon.Reason == BC250_HWMON_REASON_IDENTITY);
    CHECK(NoWriteOutsideTheLatch());

    /* A window that only echoes its own index latch. Every byte it returns is plausible on its own, so the
     * start must refuse it on the version, the build year and the absent voltage channel - or the tools would
     * show four temperature channels of 0.0 C from a window with no chip behind it. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    native_ec.answer_index = 1;
    HwmonStart(&device);
    CHECK(!device.Hwmon.Online && device.Hwmon.Reason == BC250_HWMON_REASON_IDENTITY);
    HwmonSample(&device);
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_VALID) == 0 && h.Samples == 0);
    CHECK(h.TemperatureSource[0] == 0 && h.TemperatureMc[0] == 0);
    CHECK(NoWriteOutsideTheLatch());
    native_ec.answer_index = 0;

    /* An offline reader writes ONE log line per reason, not one per telemetry tick: the ring's tail is 768
     * lines, and a 20-minute session would otherwise spend 240 of them on the same sentence. */
    native_lines = 0;
    for (i = 0; i < 20; i++) HwmonLogLine(&device, "telemetry");
    CHECK(native_lines == 1 && native_log_has("no reading, reason identity"));

    /* Every byte reads 0x00: the other half of a dead window. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    native_ec.answer_fixed = 1;
    native_ec.fixed = 0x00;
    HwmonStart(&device);
    CHECK(!device.Hwmon.Online && device.Hwmon.Reason == BC250_HWMON_REASON_IDENTITY);
    CHECK(NoWriteOutsideTheLatch());
}

/* ---- the sampler --------------------------------------------------------------------------------------- */

static void sampler(void)
{
    BC250_ESCAPE_HWMON h;
    ULONG i;

    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    NativeSetSetting(L"HwmonDutyProven", 1);
    HwmonStart(&device);
    CHECK(device.Hwmon.Online);

    HwmonSample(&device);
    CHECK(device.Hwmon.Samples == 1 && device.Hwmon.Errors == 0 && device.Hwmon.LastValid);
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_VALID) != 0 && (h.Flags & BC250_HWMON_FLAG_FRESH) != 0);
    CHECK(h.AgeMs == 0);                            /* the same tick as the sample is as fresh as it gets */
    CHECK(h.Rpm[1] == 1589 && Fastest(&h) == 1589);
    CHECK(h.Rpm[0] == 0 && h.Rpm[5] == 0);          /* four channels present with no fan, three absent */
    for (i = 0; i < 5; i++) CHECK(h.DutyPermille[i] == 961);
    CHECK(h.DutyPermille[5] == 0);
    CHECK(h.TemperatureSource[0] == BC250_HWMON_SOURCE_APU && h.TemperatureMc[0] == 83000);
    CHECK(h.TemperatureSource[1] == BC250_HWMON_SOURCE_THERMISTOR14 && h.TemperatureMc[1] == 59500);
    CHECK(h.TemperatureSource[2] == BC250_HWMON_SOURCE_THERMISTOR15 && h.TemperatureMc[2] == 59500);
    CHECK(h.TemperatureSource[3] == 0 && h.TemperatureMc[3] == 0);
    CHECK(h.ModeMask == 0 && h.Samples == 1 && h.Errors == 0 && h.Generation == 7);
    CHECK(h.Engine == 0 && h.Refusals == 0);
    /* Every present tachometer and every present duty output answered this sample. A reader tells a refused
     * value from a channel that reads 0 by these two masks and by nothing else. */
    CHECK(h.RpmValidMask == 0x1F && h.DutyValidMask == 0x1F);
    CHECK((h.Flags & BC250_HWMON_FLAG_STOPPED) == 0);
    CHECK(NoWriteOutsideTheLatch());

    /* The telemetry lines, the shape an operator reads in the driver log. THREE lines since KMD 0.7.212: one
     * line was 332 characters at its widest and BC250_LOG_TEXT holds 159, so it lost its counters without
     * saying so (the guardlog-width gate, BD-070). The reading first, then the two registers nothing decides
     * on with the duty verdict, then the counters. */
    native_lines = 0;
    HwmonLogLine(&device, "telemetry");
    CHECK(native_lines == 3);
    CHECK(native_log_has("fan2 1589 rpm (1/5 turn)") && native_log_has("duty 961 permille"));
    CHECK(native_log_has("apu 83.0 C") && !native_log_has("unproven"));
    CHECK(native_log_has("duty read-back proven") && native_log_has("samples"));

    /* The fan speeds up. The jump rule admits it, because 1589 to 2400 is not a factor of four. */
    ec_put16(&native_ec, BC250_HWMON_REG_FAN(1), 2400);
    native_time += 10000ull * BC250_HWMON_PERIOD_MS;
    HwmonSample(&device);
    Read(&h);
    CHECK(h.Rpm[1] == 2400 && h.Samples == 2 && h.AgeMs == 0 && h.Errors == 0);

    /* Ageing. The sampler stops; the published values stay, and the age grows until FRESH drops. The reader
     * sees an old reading and knows it is old, which is the whole purpose of publishing the time. */
    native_time += 10000ull * 2000ull;
    Read(&h);
    CHECK(h.AgeMs == 2000 && (h.Flags & BC250_HWMON_FLAG_FRESH) != 0 && h.Rpm[1] == 2400);
    native_time += 10000ull * 1001ull;
    Read(&h);
    CHECK(h.AgeMs == 3001 && (h.Flags & BC250_HWMON_FLAG_FRESH) == 0);
    CHECK((h.Flags & BC250_HWMON_FLAG_VALID) != 0 && h.Rpm[1] == 2400);   /* stale, not absent */

    /* A duty output runs and nothing turns: the one case the owner must see at a glance. It is shown in red
     * and the owner reacts to it, so it needs BC250_HWMON_STOPPED_SAMPLES samples in a row, and the two before
     * that must NOT raise it. */
    ec_put16(&native_ec, BC250_HWMON_REG_FAN(1), 0);
    for (i = 0; i < BC250_HWMON_STOPPED_SAMPLES - 1u; i++) {
        native_time += 10000ull * BC250_HWMON_PERIOD_MS;
        HwmonSample(&device);
        Read(&h);
        CHECK((h.Flags & BC250_HWMON_FLAG_STOPPED) == 0 && h.RpmValidMask == 0x1F && Fastest(&h) == 0);
    }
    native_time += 10000ull * BC250_HWMON_PERIOD_MS;
    HwmonSample(&device);
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_STOPPED) != 0 && Fastest(&h) == 0 && h.DutyPermille[0] == 961);
    native_lines = 0;
    HwmonLogLine(&device, "summary");
    CHECK(native_log_has("(0/5 turn)"));

    /* ONE refused tachometer reading is not a stopped fan. The duty read-backs stay at 961, the one turning
     * channel answers 0xFFFF and is refused, and a red "not turning" row over that would send the owner to the
     * case over a collision on a window that has no arbiter - which is the very reason the gate exists. */
    ec_put16(&native_ec, BC250_HWMON_REG_FAN(1), BC250_HWMON_RPM_NONE);
    native_time += 10000ull * BC250_HWMON_PERIOD_MS;
    HwmonSample(&device);
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_STOPPED) == 0);
    CHECK((h.Flags & (BC250_HWMON_FLAG_VALID | BC250_HWMON_FLAG_FRESH)) ==
          (BC250_HWMON_FLAG_VALID | BC250_HWMON_FLAG_FRESH));
    CHECK(h.RpmValidMask == 0x1D && h.Rpm[1] == 0 && h.DutyPermille[1] == 961);  /* channel 1 refused */
    CHECK(h.Refusals >= 1 && h.Errors == 0);                /* and the refusal is counted, not silent */
    /* And the count starts again from there: the refused sample is no evidence either way. */
    ec_put16(&native_ec, BC250_HWMON_REG_FAN(1), 0);
    for (i = 0; i < BC250_HWMON_STOPPED_SAMPLES - 1u; i++) {
        native_time += 10000ull * BC250_HWMON_PERIOD_MS;
        HwmonSample(&device);
        Read(&h);
        CHECK((h.Flags & BC250_HWMON_FLAG_STOPPED) == 0);
    }
    native_time += 10000ull * BC250_HWMON_PERIOD_MS;
    HwmonSample(&device);
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_STOPPED) != 0);

    /* A stopped fan with no duty either is an idle board, not a fault. */
    for (i = 0; i < 5; i++) ec_put8(&native_ec, BC250_HWMON_REG_DUTY(i), 0);
    native_time += 10000ull * BC250_HWMON_PERIOD_MS;
    HwmonSample(&device);
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_STOPPED) == 0 && Fastest(&h) == 0);
    CHECK(NoWriteOutsideTheLatch());

    /* The chip's own "no reading" is not a speed. It is refused, and the retry is counted. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    HwmonStart(&device);
    ec_put16(&native_ec, BC250_HWMON_REG_FAN(1), BC250_HWMON_RPM_NONE);
    HwmonSample(&device);
    Read(&h);
    CHECK(h.Rpm[1] == 0 && h.Retries > 0 && h.Samples == 1);    /* the rest of the sample was still accepted */
    CHECK(h.TemperatureMc[0] == 83000);
    CHECK(h.RpmValidMask == 0x1D && h.Refusals == 1);

    /* An impossible speed on this board. Same answer, and the duty read-back beside it survives. */
    ec_put16(&native_ec, BC250_HWMON_REG_FAN(1), 20000);
    native_time += 10000ull * BC250_HWMON_PERIOD_MS;
    HwmonSample(&device);
    Read(&h);
    CHECK(h.Rpm[1] == 0 && h.DutyPermille[1] == 961);
    CHECK(NoWriteOutsideTheLatch());
}

/* ---- the missing governor thread ------------------------------------------------------------------------ */

/* EnableHwmon is 1 and the identity passed, but nothing ever samples: the sampler is the DPM governor thread,
 * and a start without the native SMU owner never creates it. The reader published VALID and reason "ok", so
 * every tool said "no reading" and named the healthy reason, which told the operator nothing. After one
 * freshness window with no sample at all the answer is NO_THREAD. */
static void no_thread(void)
{
    BC250_ESCAPE_HWMON h;
    ULONG i;

    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    HwmonStart(&device);
    CHECK(device.Hwmon.Online);
    Read(&h);
    /* Inside the window the first sample is still due, so the answer is still "ok". */
    CHECK((h.Flags & BC250_HWMON_FLAG_VALID) != 0 && h.Samples == 0);
    CHECK(h.Reason == BC250_HWMON_REASON_OK && (h.Flags & BC250_HWMON_FLAG_FRESH) == 0);
    native_time += 10000ull * (BC250_HWMON_NO_THREAD_MS + 1u);
    Read(&h);
    CHECK(h.Reason == BC250_HWMON_REASON_NO_THREAD && h.Samples == 0);
    CHECK((h.Flags & BC250_HWMON_FLAG_VALID) != 0 && (h.Flags & BC250_HWMON_FLAG_FRESH) == 0);
    native_lines = 0;
    for (i = 0; i < 10; i++) HwmonLogLine(&device, "telemetry");
    CHECK(native_lines == 1 && native_log_has("no reading, reason no-thread"));

    /* One sample settles it: the reason goes back to the published one and stays there. */
    HwmonSample(&device);
    Read(&h);
    CHECK(h.Reason == BC250_HWMON_REASON_OK && h.Samples == 1);
    native_time += 10000ull * (BC250_HWMON_NO_THREAD_MS + 10000u);
    Read(&h);
    CHECK(h.Reason == BC250_HWMON_REASON_OK);                   /* stale, with an age, but not thread-less */
    CHECK(NoWriteOutsideTheLatch());
}

/* ---- giving up ----------------------------------------------------------------------------------------- */

static void give_up(void)
{
    BC250_ESCAPE_HWMON h;
    ULONG i;

    /* The window has no arbiter. When the reads stop making sense the reader goes quiet for the rest of this
     * start instead of restarting itself: a loop that keeps writing the latch of a chip somebody else is
     * talking to is the one behaviour that could make another program's reading wrong. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    HwmonStart(&device);
    HwmonSample(&device);
    CHECK(device.Hwmon.Samples == 1);
    Read(&h);
    CHECK(Fastest(&h) == 1589);

    native_ec.answer_fixed = 1;
    native_ec.fixed = 0xFF;                         /* every register refuses now */
    for (i = 0; i < BC250_HWMON_FAIL_LIMIT - 1u; i++) {
        native_time += 10000ull * BC250_HWMON_PERIOD_MS;
        HwmonSample(&device);
    }
    CHECK(device.Hwmon.Online && device.Hwmon.Errors == BC250_HWMON_FAIL_LIMIT - 1u);
    CHECK(device.Hwmon.FailuresInRow == BC250_HWMON_FAIL_LIMIT - 1u);
    Read(&h);
    /* The last accepted sample is still published, with an age that grew while it was failing. */
    CHECK((h.Flags & BC250_HWMON_FLAG_VALID) != 0 && Fastest(&h) == 1589);
    CHECK(h.AgeMs == (BC250_HWMON_FAIL_LIMIT - 1u) * BC250_HWMON_PERIOD_MS);
    CHECK((h.Flags & BC250_HWMON_FLAG_FRESH) == 0 && h.Samples == 1 && h.Errors == h.AgeMs / 1000u);

    native_lines = 0;
    native_time += 10000ull * BC250_HWMON_PERIOD_MS;
    HwmonSample(&device);
    CHECK(!device.Hwmon.Online && device.Hwmon.Reason == BC250_HWMON_REASON_PORT);
    CHECK(native_lines == 1 && native_log_has("in a row were refused"));
    Read(&h);
    CHECK((h.Flags & BC250_HWMON_FLAG_VALID) == 0 && h.Reason == BC250_HWMON_REASON_PORT);

    /* And it stays quiet: no further port access, no further log line, for this start. */
    {
        unsigned int reads = native_ec.reads, writes = native_ec.writes_latch;

        native_lines = 0;
        for (i = 0; i < 5; i++) {
            native_time += 10000ull * BC250_HWMON_PERIOD_MS;
            HwmonSample(&device);
        }
        CHECK(native_ec.reads == reads && native_ec.writes_latch == writes);
        CHECK(native_lines == 0 && device.Hwmon.Errors == BC250_HWMON_FAIL_LIMIT);
    }
    CHECK(NoWriteOutsideTheLatch());

    /* A single bad sample between good ones is not a give-up: the counter resets. */
    Fresh();
    NativeSetSetting(L"EnableHwmon", 1);
    HwmonStart(&device);
    for (i = 0; i < 3u * BC250_HWMON_FAIL_LIMIT; i++) {
        native_ec.answer_fixed = (i % 3u) == 0 ? 1 : 0;
        native_ec.fixed = 0xFF;
        native_time += 10000ull * BC250_HWMON_PERIOD_MS;
        HwmonSample(&device);
        CHECK(device.Hwmon.Online);
    }
    CHECK(device.Hwmon.FailuresInRow == 0 && device.Hwmon.Samples == 2u * BC250_HWMON_FAIL_LIMIT);
    CHECK(device.Hwmon.Errors == BC250_HWMON_FAIL_LIMIT);

    /* Stop. The reading goes away with the start, and the counters are logged once. */
    native_lines = 0;
    HwmonStop(&device.Hwmon);
    CHECK(native_lines == 1 && native_log_has("samples,") && native_log_has("retries"));
    Read(&h);
    CHECK((h.Flags & (BC250_HWMON_FLAG_VALID | BC250_HWMON_FLAG_FRESH)) == 0);
    CHECK(NoWriteOutsideTheLatch());

    /* A second start on the same device begins from nothing: no inherited sample, no inherited counters. */
    device.StartHealth.Generation = 8;
    HwmonStart(&device);
    CHECK(device.Hwmon.Online && device.Hwmon.Samples == 0 && device.Hwmon.Errors == 0);
    CHECK(!device.Hwmon.LastValid && device.Hwmon.FailuresInRow == 0);
    Read(&h);
    CHECK(h.Generation == 8 && Fastest(&h) == 0 && h.Samples == 0);
    HwmonSample(&device);
    Read(&h);
    CHECK(Fastest(&h) == 1589 && h.Generation == 8);
    CHECK(NoWriteOutsideTheLatch());
}

int main(void)
{
    wire();
    gate();
    start();
    sampler();
    no_thread();
    give_up();
    printf("hardware monitor binding: %ld checks, %ld failures\n", native_checks, native_failures);
    return native_failures ? 1 : 0;
}
