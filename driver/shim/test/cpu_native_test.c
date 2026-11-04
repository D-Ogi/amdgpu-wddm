/* Compiles the actual miniport binding of the CPU surface (driver/kmd/cpu.c) on the host, replacing only the
 * Windows kernel primitives and the mailbox owner, and drives its read stage against the firmware model of
 * cpu_native_mock.h.
 *
 * What is under test here is the part the pure-shim test (cpu_test.c) cannot reach: the BOOST PROBE'S SWEEP, which
 * is a loop inside CpuBoostProbe (audit finding F3, 2026-10-10). The shim holds the sweep's stop rule as a pure
 * function, bc250_cpu_boost_probe_more, and cpu_test.c checks it; but with BC250_CPU_BOOST_PROBE_ROUNDS = 2 the
 * loop's only call is bc250_cpu_boost_probe_more(0, ...), which the bound alone answers, so that check cannot tell
 * the shipped sweep from the one it replaced. The audit said so, and it is right: what the sweep does with the
 * firmware's replies is only visible against a firmware that answers DIFFERENT clocks in the two windows.
 *
 * So the arms below are firmware schedules, and each one states what the stage must end with:
 *
 *   ramp       2900 MHz in the first window, 3500 in the second. The sweep must read both windows and record
 *              3500, because the recorded number is what a restore gives back (the BD-094 cut). Up to 0.7.216.24
 *              the sweep stopped at the first reply inside the band and recorded 2900.
 *   thermal    3200 MHz in every window: the observed maximum is 3200, and it is a usable restore target.
 *   delayed    nothing inside the band in the first window, 3500 in the second: the sweep must still be running.
 *   hot        the part is at or above BC250_CLOCK_HOT_MC: no probe at all, and the clock control stays refused.
 *   refused    message 0x43 answers nothing: the stage records no ceiling and says so.
 *
 * The BD-295 arms drive whole driver loads of one boot (CpuStart, one pass of the worker, the escape, CpuStop)
 * against a chip whose clock limit, curve scale and cap outlive the load, and a volatile store that does too:
 *
 *   boot record    the first start of a boot writes its read; a later start adopts it, so a kept 3000 MHz limit in
 *                  the chip does not become the baseline, and a reset gives 3500 MHz back.
 *   leftover       a revert refused at the stop writes the mark; the next start sends every control of the boot
 *                  baseline before its own settings and clears the mark. Refused again, every setter is refused
 *                  with BC250_CPU_ERROR_LEFTOVER until the worker's retry goes through.
 *   foreign        a record of another version, a partial one and an unreadable store are not a baseline.
 *   resume         the D0 after a power transition adopts the record; dropped stored settings count as a leftover.
 *
 * Every arm of the probe also asserts the probe's discipline: it pins itself to the processor it runs on and reverts, it sends
 * no setter, and its stalls stay inside the bound its own comment claims.
 */
#define WIN32_NO_STATUS
#include "cpu_native_mock.h"
#include "cpu-native.inc"

static BC250_DEVICE device;

/* The sweep's bound, in stall microseconds: rounds * (window + cores * getter gap). The probe's comment calls it
 * "one core of six busy for about a fifth of a second", and this is that number. */
#define PROBE_STALL_US ((ULONG64)BC250_CPU_BOOST_PROBE_ROUNDS * \
    (BC250_CPU_BOOST_PROBE_MS * 1000ull + (ULONG64)BC250_CPU_CORES * BC250_CPU_GETTER_GAP_MS * 1000ull))
/* One window of the sweep ends after its busy wait and the getter gap of every core. A schedule that changes
 * between the rounds closes its first window a millisecond into the second round's busy wait. */
#define ROUND_STALL_US (BC250_CPU_BOOST_PROBE_MS * 1000ull + \
    (ULONG64)BC250_CPU_CORES * BC250_CPU_GETTER_GAP_MS * 1000ull)
#define FIRST_ROUND_WINDOW_US (ROUND_STALL_US + 1000ull)
/* The core reads of one full stage: one per core, and then the sweep's rounds. */
#define STAGE_CORE_READS BC250_CPU_CORES
#define ROUND_CORE_READS ((unsigned int)BC250_CPU_CORES)
#define SWEEP_CORE_READS ((unsigned int)(BC250_CPU_BOOST_PROBE_ROUNDS * BC250_CPU_CORES))

static void Fill(unsigned int *out, unsigned int mhz)
{
    unsigned int i;

    for (i = 0; i < BC250_CPU_CORES; i++) out[i] = mhz;
}

/* The model's window 0 is the one the READ STAGE itself reads in: an idle machine, which is the only start that
 * owes a sweep at all. It closes on the sweep's first stall, because the stage sleeps between its messages and
 * never stalls, so no arithmetic ties the stage's length to the schedule. */
static void StageWindow(unsigned int mhz)
{
    Fill(native_firmware.window[0].core_mhz, mhz);
    native_firmware.window[0].us = 1u;
    native_firmware.windows = 1;
}

/* One window of the sweep, in order. `us` of 0 makes it the last one, which never closes, so a sweep of more
 * rounds than the schedule describes still gets an answer. */
static void ProbeWindow(unsigned int mhz, ULONG64 us)
{
    CHECK(native_firmware.windows < NATIVE_WINDOWS);
    Fill(native_firmware.window[native_firmware.windows].core_mhz, mhz);
    native_firmware.window[native_firmware.windows].us = us;
    native_firmware.windows++;
}

/* A part that has answered nothing yet: an idle machine, a 3200 MHz P-state table (unit A's), 1300 mV and a
 * temperature well under the hot gate. The model's windows are filled by each arm. */
static void Reset(void)
{
    unsigned int i;

    memset(&native_firmware, 0, sizeof(native_firmware));
    memset(&device, 0, sizeof(device));
    native_time = 0;
    native_stall_us = 0;
    native_sleep_ms = 0;
    native_stalls = 0;
    native_sleeps = 0;
    native_lines = 0;
    native_affinity_depth = 0;
    native_affinity_sets = 0;
    native_affinity_reverts = 0;
    native_processor = 3;
    NativeClearSettings();
    NativeBoot();                           /* a cold boot: no record of an earlier start (BD-295) */
    for (i = 0; i < BC250_CPU_PSTATES; i++) native_firmware.pstate_mhz[i] = i == 0 ? 3200u : 1600u;
    native_firmware.cpu_mv = 1300u;
    native_firmware.gpu_mv = 820u;
    native_firmware.cap_c = 95u;
    native_firmware.features = 0x12345678u;
    native_firmware.temperature_mc = 55000;
    native_firmware.temperature_valid = TRUE;
    CpuInitialize(&device);
    device.FullWddm = TRUE;
    device.Smu.Online = TRUE;
    device.Smu.CpuOnline = TRUE;
}

/* One full read stage, as the worker runs it at the start of a start: under Lock, with the slow first-traffic gap. */
static NTSTATUS Stage(void)
{
    NTSTATUS status;

    CpuLock(&device.Cpu);
    status = CpuReadStage(&device, TRUE, BC250_CPU_MESSAGE_GAP_MS);
    CpuUnlock(&device.Cpu);
    return status;
}

/* One line per arm, so the run itself is the evidence of what the firmware answered and what the stage recorded. */
static void Report(const char *arm)
{
    printf("  %-31s core reads %2u (stage %u, rounds %u + %u), boost %u MHz, table %u MHz, baseline %u MHz,"
           " stalls %llu us\n", arm, native_firmware.core_reads, native_firmware.core_reads_per_window[0],
           native_firmware.core_reads_per_window[1], native_firmware.core_reads_per_window[2],
           device.Cpu.BaselineRead.boost_mhz, device.Cpu.BaselineRead.table_mhz,
           device.Cpu.Baseline.max_given ? device.Cpu.Baseline.max_mhz : 0u,
           (unsigned long long)native_stall_us);
}

/* What every arm must hold, whether the sweep ran or not. */
static void CheckDiscipline(void)
{
    CHECK(native_firmware.writes == 0);                     /* a read stage sends no setter */
    CHECK(native_firmware.sequence_open == 0);              /* SmuCpuBegin was matched by SmuCpuEnd */
    CHECK(native_firmware.sequences == 1);
    CHECK(native_affinity_depth == 0);                      /* the probe gave the thread back */
    CHECK(native_affinity_sets == native_affinity_reverts);
    CHECK(native_stall_us <= PROBE_STALL_US);               /* the sweep's own bound, nothing added to it */
}

/* ---- the arms ------------------------------------------------------------------------------------------- */

/* A ramping firmware. THIS IS THE REGRESSION: the first reply inside the band is not the maximum, and the sweep
 * that stopped there recorded 2900 MHz as "the boost" and gave that back at a restore. */
static void test_ramp(void)
{
    Reset();
    StageWindow(1200u);                                     /* the stage reads an idle machine, which owes a sweep */
    ProbeWindow(2900u, FIRST_ROUND_WINDOW_US);              /* the first round: already inside the band, but climbing */
    ProbeWindow(3500u, 0);                                  /* the second round: where the part actually settles */

    CHECK(Stage() == STATUS_SUCCESS);
    /* The whole sweep ran: every core of every round was read, and the first in-band reply did not end it. */
    CHECK(native_firmware.core_reads == STAGE_CORE_READS + SWEEP_CORE_READS);
    CHECK(native_firmware.core_reads_per_window[0] == STAGE_CORE_READS);
    CHECK(native_firmware.core_reads_per_window[1] == ROUND_CORE_READS);
    CHECK(native_firmware.core_reads_per_window[2] == ROUND_CORE_READS);
    /* And the record is the HIGHEST reply, not the first one inside the band. */
    CHECK(device.Cpu.BaselineRead.boost_given == 1);
    CHECK(device.Cpu.BaselineRead.boost_mhz == 3500u);
    CHECK(device.Cpu.BaselineRead.table_mhz == 3200u);      /* the P-state table is untouched by the sweep */
    CHECK(device.Cpu.BaselineRead.mhz == 3500u);
    /* What a restore of this start would give back. */
    CHECK(device.Cpu.BaselineValid == TRUE);
    CHECK(device.Cpu.Baseline.max_given == 1 && device.Cpu.Baseline.max_mhz == 3500u);
    /* Every core's cached reading is the highest that core answered, so the snapshot cannot show a core at its
     * idle clock after a window in which it ran at 3500 MHz. */
    CHECK(device.Cpu.Snap.CoreMHz[0] == 3500u);
    CHECK(device.Cpu.Snap.CoreMHz[BC250_CPU_CORES - 1] == 3500u);
    /* The log calls the quantity what it is. */
    CHECK(native_log_has("the highest observed core clock is 3500 MHz"));
    CHECK(!native_log_has("the firmware's own boost"));
    CheckDiscipline();
    Report(__func__ + 5);
}

/* A thermally limited part: every window answers the same clock. That is the observed maximum and the usable
 * restore target. It says nothing about what the firmware would allow on a cold part, and the driver does not
 * claim it does. */
static void test_thermally_limited(void)
{
    Reset();
    StageWindow(1200u);
    ProbeWindow(3200u, 0);                                  /* every round of the sweep reads the same clock */

    CHECK(Stage() == STATUS_SUCCESS);
    CHECK(native_firmware.core_reads == STAGE_CORE_READS + SWEEP_CORE_READS);
    CHECK(native_firmware.core_reads_per_window[1] == SWEEP_CORE_READS);
    CHECK(device.Cpu.BaselineRead.boost_given == 1 && device.Cpu.BaselineRead.boost_mhz == 3200u);
    CHECK(device.Cpu.BaselineRead.mhz == 3200u);
    CHECK(device.Cpu.Baseline.max_given == 1 && device.Cpu.Baseline.max_mhz == 3200u);
    CHECK(native_log_has("the highest observed core clock is 3200 MHz"));
    CheckDiscipline();
    Report(__func__ + 5);
}

/* A delayed answer: the first window has nothing inside the band. The sweep must still be running in the second,
 * which is why its bound is two rounds and not one. */
static void test_delayed_answer(void)
{
    Reset();
    StageWindow(1200u);
    ProbeWindow(1200u, FIRST_ROUND_WINDOW_US);              /* the first round answers nothing inside the band */
    ProbeWindow(3500u, 0);

    CHECK(Stage() == STATUS_SUCCESS);
    CHECK(native_firmware.core_reads == STAGE_CORE_READS + SWEEP_CORE_READS);
    CHECK(native_firmware.core_reads_per_window[2] == ROUND_CORE_READS);
    CHECK(device.Cpu.BaselineRead.boost_given == 1 && device.Cpu.BaselineRead.boost_mhz == 3500u);
    CHECK(device.Cpu.Snap.CoreMHz[0] == 3500u);
    CheckDiscipline();
    Report(__func__ + 5);
}

/* A hot part owes no probe: the sweep makes heat, and the only thing it would buy is the clock control, which the
 * start can refuse. The stage must read its cores once and stop there. */
static void test_hot_part_runs_no_probe(void)
{
    Reset();
    native_firmware.temperature_mc = BC250_CLOCK_HOT_MC + 1500;
    StageWindow(1200u);
    ProbeWindow(3500u, 0);                                  /* the model would answer, if the sweep ever ran */

    CHECK(Stage() == STATUS_SUCCESS);
    CHECK(native_firmware.core_reads == STAGE_CORE_READS);  /* the sweep did not run */
    CHECK(native_stalls == 0);                              /* and nothing kept a core busy */
    /* No ceiling, so the worker leaves a stored clock limit out of its plan (BD-094). The baseline's own clock is
     * the P-state table's top, which is not a ceiling and is not treated as one. */
    CHECK(device.Cpu.BaselineRead.boost_given == 0 && device.Cpu.BaselineRead.boost_mhz == 0u);
    CHECK(device.Cpu.BaselineRead.table_mhz == 3200u && device.Cpu.Baseline.max_mhz == 3200u);
    CHECK(native_log_has("the boost probe is not run"));
    CHECK(native_affinity_sets == 0);
    CheckDiscipline();
    Report(__func__ + 5);
}

/* The firmware refuses message 0x43. The stage records no ceiling and the log says the band was never reached:
 * an unanswered sweep must not leave a number behind that a restore would give back. */
static void test_refused_core_reads(void)
{
    Reset();
    native_firmware.refuse_core_reads = 1;
    StageWindow(3500u);                                     /* the model would answer, if it were asked */
    ProbeWindow(3500u, 0);

    CHECK(Stage() == STATUS_SUCCESS);                       /* the stage's own first getter answered */
    CHECK(native_firmware.core_reads == 0);                 /* the model answered none of them */
    CHECK(native_firmware.core_requests == STAGE_CORE_READS + SWEEP_CORE_READS);   /* all of them were asked for */
    CHECK(device.Cpu.BaselineRead.boost_given == 0);
    CHECK(device.Cpu.BaselineRead.boost_mhz == 0u);
    CHECK(device.Cpu.Baseline.max_mhz == 3200u);            /* the table alone, and no ceiling to give back */
    CHECK(native_log_has("still no clock inside the band"));
    CheckDiscipline();
    Report(__func__ + 5);
}

/* A part that is already inside the band when the stage reads it owes no sweep at all: the firmware has answered
 * the question the sweep exists to ask. */
static void test_an_answered_part_owes_no_probe(void)
{
    Reset();
    StageWindow(3500u);
    ProbeWindow(3500u, 0);

    CHECK(Stage() == STATUS_SUCCESS);
    CHECK(native_firmware.core_reads == STAGE_CORE_READS);
    CHECK(native_stalls == 0);
    CHECK(device.Cpu.BaselineRead.boost_mhz == 3500u);
    CHECK(device.Cpu.Baseline.max_mhz == 3500u);
    CheckDiscipline();
    Report(__func__ + 5);
}

/* ---- the default temperature cap (2026-11-04, BD-180) ----------------------------------------------- */
/* The start and the reset of the binding itself: CpuApplyStart and the RESET escape. The shim's own test pins the
 * rule (bc250_cpu_start_target, bc250_cpu_restore_target); these arms pin that cpu.c sends what the rule says. */

/* One full read stage of an idle part that is already inside the band, then the start's settings, as the worker
 * runs them. The firmware takes every setter. */
static void StartWith(void)
{
    StageWindow(3500u);
    ProbeWindow(3500u, 0);
    CHECK(Stage() == STATUS_SUCCESS);
    CHECK(native_firmware.writes == 0);
    native_firmware.accept_writes = 1;
    CpuLock(&device.Cpu);
    CpuApplyStart(&device);
    CpuUnlock(&device.Cpu);
}

static int CapSent(int at, unsigned int c)
{
    return native_firmware.set[at].queue == BC250_CPU_QUEUE_CPU &&
           native_firmware.set[at].message == BC250_CPU_MSG_SET_CAP_C && native_firmware.set[at].parameter == c;
}

/* Nothing stored: the start sends the default cap, one setter, and stores nothing. */
static void test_start_applies_the_default_cap(void)
{
    ULONG value = 0;

    Reset();
    native_firmware.cap_c = 100u;                           /* the firmware's own cap after a cold boot */
    StartWith();
    CHECK(native_firmware.writes == 1 && CapSent(0, BC250_CPU_TEMP_DEFAULT_C));
    CHECK(native_firmware.cap_c == BC250_CPU_TEMP_DEFAULT_C);
    CHECK(device.Cpu.Applied.temp_given && device.Cpu.Applied.temp_c == BC250_CPU_TEMP_DEFAULT_C);
    CHECK(!device.Cpu.Applied.max_given && !device.Cpu.Applied.uv_given);
    CHECK(!device.Cpu.Stored.temp_given);                   /* the default is not a stored value */
    CHECK(device.Cpu.Baseline.temp_given && device.Cpu.Baseline.temp_c == 100u);   /* the firmware's, unchanged */
    CHECK(GuardQuerySetting(L"CpuTempC", &value) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(GuardQuerySetting(L"CpuLastReason", &value) == STATUS_OBJECT_NAME_NOT_FOUND);
    CHECK(native_log_has("the default of 87 C goes in"));
    printf("  %-31s cap %lu C sent, baseline cap %u C\n", "start_default_cap", native_firmware.set[0].parameter,
           device.Cpu.Baseline.temp_c);
}

/* A stored cap wins: 100 C (the firmware's own) is sent as 100 C, and the default does not replace it. */
static void test_start_keeps_a_stored_cap(void)
{
    ULONG value = 0;

    Reset();
    native_firmware.cap_c = 100u;
    device.Cpu.Stored.temp_given = 1;
    device.Cpu.Stored.temp_c = BC250_CPU_TEMP_MAX_C;
    StartWith();
    CHECK(native_firmware.writes == 1 && CapSent(0, BC250_CPU_TEMP_MAX_C));
    CHECK(device.Cpu.Applied.temp_given && device.Cpu.Applied.temp_c == BC250_CPU_TEMP_MAX_C);
    CHECK(!native_log_has("goes in"));
    CHECK(GuardQuerySetting(L"CpuLastReason", &value) == STATUS_SUCCESS && value == 1u);    /* CPU_REASON_OK */
}

/* Stored settings without a cap: the undervolt goes in with the default cap, and the cap goes first (it cools). */
static void test_start_adds_the_cap_to_stored_settings(void)
{
    Reset();
    device.Cpu.Stored.uv_given = 1;
    device.Cpu.Stored.uv_steps = 4u;
    StartWith();
    CHECK(native_firmware.writes == 2 && CapSent(0, BC250_CPU_TEMP_DEFAULT_C));
    CHECK(native_firmware.set[1].message == BC250_CPU_MSG_SET_CURVE_SCALE &&
          native_firmware.set[1].parameter == bc250_cpu_scale_argument(4u));
    CHECK(device.Cpu.Applied.temp_c == BC250_CPU_TEMP_DEFAULT_C && device.Cpu.Applied.uv_steps == 4u);
    CHECK(!device.Cpu.Stored.temp_given);
}

/* One RESET escape, as the CLI and the control application send it. */
static void ResetEscape(BC250_ESCAPE_CPU *d)
{
    D3DDDI_ESCAPEFLAGS flags;

    memset(d, 0, sizeof(*d));
    memset(&flags, 0, sizeof(flags));
    flags.HardwareAccess = 1;
    d->AbiVersion = BC250_CPU_ABI;
    d->Op = BC250_CPU_OP_RESET;
    d->ExpectedGeneration = device.Cpu.Generation;
    CpuRequest(&device, d, sizeof(*d), TRUE, flags.Value);
}

/* The RESET escape after a kept 100 C: the stored values go off the disk and the cap comes back to the default,
 * not to the 100 C of the baseline. The reply still names the baseline as the firmware's own value. */
static void test_reset_returns_to_the_default_cap(void)
{
    BC250_ESCAPE_CPU d;
    ULONG value = 0;

    Reset();
    native_firmware.cap_c = 100u;
    device.Cpu.Stored.temp_given = 1;
    device.Cpu.Stored.temp_c = BC250_CPU_TEMP_MAX_C;
    NativeSetSetting(L"CpuTempC", BC250_CPU_TEMP_MAX_C);
    StartWith();
    CHECK(device.Cpu.Applied.temp_c == BC250_CPU_TEMP_MAX_C);
    device.Cpu.Enabled = TRUE;
    device.Cpu.Created = TRUE;
    device.Cpu.Proven = TRUE;                               /* the worker sets it after the stage answered */
    ResetEscape(&d);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE);
    CHECK(native_firmware.writes == 2 && CapSent(1, BC250_CPU_TEMP_DEFAULT_C));
    CHECK(native_firmware.cap_c == BC250_CPU_TEMP_DEFAULT_C);
    CHECK(d.AppliedTempC == BC250_CPU_TEMP_DEFAULT_C && d.StoredTempC == 0u && d.BaselineTempC == 100u);
    CHECK(!(d.Flags & BC250_CPU_FLAG_STORED));
    CHECK(GuardQuerySetting(L"CpuTempC", &value) == STATUS_OBJECT_NAME_NOT_FOUND);
    /* A second reset finds the default in force and sends nothing. */
    ResetEscape(&d);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && native_firmware.writes == 2);
    printf("  %-31s cap %u C -> %lu C, baseline cap %lu C\n", "reset_default_cap", BC250_CPU_TEMP_MAX_C,
           native_firmware.set[1].parameter, d.BaselineTempC);
}

/* The stop rule as the loop asks it, next to the sweep that uses it: only the bound ends the sweep. */
static void test_the_stop_rule(void)
{
    unsigned int round;

    CHECK(bc250_cpu_boost_probe_more(0u, 0u) == 1);
    CHECK(bc250_cpu_boost_probe_more(0u, 2800u) == 1);
    CHECK(bc250_cpu_boost_probe_more(0u, 3500u) == 1);
    CHECK(bc250_cpu_boost_probe_more(0u, 4000u) == 1);
    for (round = 0; round + 1u < BC250_CPU_BOOST_PROBE_ROUNDS; round++)
        CHECK(bc250_cpu_boost_probe_more(round, 3500u) == 1);
    CHECK(bc250_cpu_boost_probe_more(BC250_CPU_BOOST_PROBE_ROUNDS - 1u, 3500u) == 0);
    CHECK(bc250_cpu_boost_probe_more(BC250_CPU_BOOST_PROBE_ROUNDS, 0u) == 0);
}

/* BD-293: a failed power transaction keeps the governor paused until the next start. The CPU surface then needs only
 * its owner: the paused governor gates it in every other phase, and a missing owner gates it in FAILED too. */
static void test_failed_transaction_admits_the_cpu(void)
{
    Reset();
    CHECK(!CpuAdapterDown(&device));
    device.Dpm.Paused = 1;
    CHECK(CpuAdapterDown(&device));                         /* ACTIVE with the governor paused: on the way down */
    device.RetainedPowerPhase = BC250_POWER_SUSPENDING;
    CHECK(CpuAdapterDown(&device));
    device.RetainedPowerPhase = BC250_POWER_RESTORING;
    CHECK(CpuAdapterDown(&device));
    device.RetainedPowerPhase = BC250_POWER_FAILED;
    CHECK(!CpuAdapterDown(&device));                        /* the owner alone gates it now */
    device.Smu.Online = FALSE;
    CHECK(CpuAdapterDown(&device));
    device.Smu.Online = TRUE;
    device.Smu.CpuOnline = FALSE;
    CHECK(CpuAdapterDown(&device));
    printf("  %-31s the paused governor gates the CPU except in FAILED\n", "failed_transaction");
}

/* ---- BD-295: the boot record and the leftover mark ----------------------------------------------------------- */
/* A driver stop and start inside one boot: the device structure and every cached value of the surface go, and the
 * chip (native_firmware's limit, scale and cap) and the volatile store stay. The worker runs as the real one does,
 * one pass of CpuThread per call: the mock's waits time out and its stop event ends the loop after the pass. */

/* The firmware's answers for the next read stage: an idle stage, then the probe's window. The clock limit of the
 * chip holds every reply under it (cpu_native_mock.h). */
static void Chip(unsigned int stage_mhz, unsigned int probe_mhz)
{
    memset(native_firmware.window, 0, sizeof(native_firmware.window));
    native_firmware.windows = 0;
    native_firmware.at = 0;
    native_firmware.spent_us = 0;
    StageWindow(stage_mhz);
    ProbeWindow(probe_mhz, 0);
}

/* A cold boot: the chip is at its own values (cap 100 C, no limit), the volatile store is empty, CpuTune is 1. */
static void Boot(void)
{
    Reset();
    NativeBoot();
    NativeSetSetting(L"CpuTune", 1u);
    native_firmware.cap_c = 100u;
    native_firmware.accept_writes = 1;
}

/* One driver load of the same boot: a new device, CpuStart, and the worker's first pass. */
static void Load(void)
{
    memset(&device, 0, sizeof(device));
    CpuInitialize(&device);
    device.FullWddm = TRUE;
    device.Smu.Online = TRUE;
    device.Smu.CpuOnline = TRUE;
    native_lines = 0;
    CpuStart(&device);
    CHECK(device.Cpu.Created);
    CpuThread(&device);
    CHECK(native_firmware.sequence_open == 0);
}

static void CpuEscape(BC250_ESCAPE_CPU *d, ULONG op, ULONG given, ULONG max_mhz, ULONG uv_steps)
{
    D3DDDI_ESCAPEFLAGS flags;

    memset(d, 0, sizeof(*d));
    memset(&flags, 0, sizeof(flags));
    if (op == BC250_CPU_OP_READ) flags.NoAdapterSynchronization = 1;
    else flags.HardwareAccess = 1;
    d->AbiVersion = BC250_CPU_ABI;
    d->Op = op;
    d->Given = given;
    d->MaxMHz = max_mhz;
    d->UvSteps = uv_steps;
    d->ExpectedGeneration = device.Cpu.Generation;
    CpuRequest(&device, d, sizeof(*d), TRUE, flags.Value);
}

/* A value of the volatile store, or MAXULONG when it is not there. */
static ULONG Volatile(const WCHAR *key, const WCHAR *name)
{
    ULONG value = 0;

    return NT_SUCCESS(GuardVolatileQuery(key, name, &value)) ? value : MAXULONG;
}

/* The first start of a boot keeps its read in the record, and a clean stop leaves no mark. */
static void test_first_start_writes_the_boot_record(void)
{
    Boot();
    Chip(1200u, 3500u);
    Load();
    CHECK(device.Cpu.Proven);
    CHECK(device.Cpu.Baseline.max_given && device.Cpu.Baseline.max_mhz == 3500u);
    CHECK(Volatile(L"CpuBoot", L"Valid") == 1u && Volatile(L"CpuBoot", L"Version") == 1u);
    CHECK(Volatile(L"CpuBoot", L"MaxMHz") == 3500u && Volatile(L"CpuBoot", L"CapC") == 100u);
    CHECK(Volatile(L"CpuBoot", L"TableMHz") == 3200u && Volatile(L"CpuBoot", L"BoostMHz") == 3500u);
    CHECK(native_log_has("the boot record is written"));
    CHECK(native_firmware.cap_c == BC250_CPU_TEMP_DEFAULT_C);      /* the start went on as before */
    CpuStop(&device);
    CHECK(Volatile(L"CpuLeftover", L"Owed") == MAXULONG);
    printf("  %-31s record clock %lu MHz, cap %lu C, boost %lu MHz\n", "boot_record_written",
           Volatile(L"CpuBoot", L"MaxMHz"), Volatile(L"CpuBoot", L"CapC"), Volatile(L"CpuBoot", L"BoostMHz"));
}

/* THE REGRESSION, case 2 of BD-295: a kept clock limit stays in the chip across a driver restart. The probe of the
 * next start then sees the limit, and up to BD-295 that start recorded the limit (or the P-state top over it) as the
 * firmware baseline, so a reset went back there and not to the boost. */
static void test_a_kept_limit_does_not_become_the_baseline(void)
{
    BC250_ESCAPE_CPU d;

    Boot();
    Chip(1200u, 3500u);
    Load();
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3000u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && native_firmware.limit_mhz == 3000u);
    CpuEscape(&d, BC250_CPU_OP_KEEP, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE);
    CpuStop(&device);
    CHECK(Volatile(L"CpuLeftover", L"Owed") == MAXULONG);         /* kept settings are not a leftover */

    Chip(1200u, 3500u);                                             /* the chip keeps its 3000 MHz limit */
    Load();
    CHECK(native_log_has("adopts the boot record"));
    CHECK(device.Cpu.Baseline.max_mhz == 3500u);                    /* the boot's, not the limit's 3000 or 3200 */
    CHECK(device.Cpu.BaselineRead.boost_given == 1 && device.Cpu.BaselineRead.boost_mhz == 3500u);
    CHECK(native_firmware.limit_mhz == 3000u);                      /* the stored limit went in again */
    CHECK(!native_log_has("the boost probe kept one core busy"));  /* the record answered the boost */
    CpuEscape(&d, BC250_CPU_OP_RESET, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE);
    CHECK(native_firmware.limit_mhz == 3500u);                      /* reset gives the boost back */
    CHECK(d.BaselineMaxMHz == 3500u);
    printf("  %-31s baseline %u MHz after a kept 3000 MHz, reset sent %u MHz\n", "kept_limit_adopts_record",
           device.Cpu.Baseline.max_mhz, native_firmware.limit_mhz);
}

/* One load that ends with a trial the firmware will not take back: the stop leaves the trial in the chip. */
static void LoadAndStopWithRevertRefused(void)
{
    BC250_ESCAPE_CPU d;

    Boot();
    Chip(1200u, 3500u);
    Load();
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX | BC250_CPU_GIVEN_UV, 3000u, 2u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && (d.Flags & BC250_CPU_FLAG_ON_TRIAL));
    CHECK(native_firmware.limit_mhz == 3000u && native_firmware.scale == bc250_cpu_scale_argument(2u));
    native_firmware.accept_writes = 0;
    CpuStop(&device);
    CHECK(native_log_has("a revert is still owed at the stop"));
    CHECK(native_log_has("the leftover mark is written"));
    CHECK(Volatile(L"CpuLeftover", L"Owed") == 1u && Volatile(L"CpuLeftover", L"What") == 1u);
    CHECK(Volatile(L"CpuLeftover", L"MaxMHz") == 3000u && Volatile(L"CpuLeftover", L"UvSteps") == 2u);
    CHECK(native_firmware.limit_mhz == 3000u);                      /* the trial is still in the chip */
}

/* Case 1 of BD-295: a revert refused at the stop. The next start sends the boot baseline back, every control of it,
 * before its own settings, and clears the mark. */
static void test_a_refused_revert_is_restored_at_the_next_start(void)
{
    BC250_ESCAPE_CPU d;
    int before;

    LoadAndStopWithRevertRefused();
    native_firmware.accept_writes = 1;
    before = native_firmware.writes;
    Chip(1200u, 3500u);
    Load();
    CHECK(native_log_has("an earlier stop of this boot left settings in the chip"));
    CHECK(native_log_has("is back at the boot baseline"));
    CHECK(!device.Cpu.LeftoverOwed);
    CHECK(Volatile(L"CpuLeftover", L"Owed") == 0u);
    CHECK(native_firmware.limit_mhz == 3500u);
    CHECK(native_firmware.scale == bc250_cpu_scale_argument(0u));
    CHECK(native_firmware.cap_c == BC250_CPU_TEMP_DEFAULT_C);
    CHECK(native_firmware.writes - before == 3);                    /* all three, and the default cap sends nothing */
    CHECK(device.Cpu.Applied.max_mhz == 3500u && device.Cpu.Applied.uv_steps == 0u);
    printf("  %-31s %d setters at the start, chip at %u MHz, scale 0x%X, cap %u C\n", "leftover_restored",
           native_firmware.writes - before, native_firmware.limit_mhz, native_firmware.scale, native_firmware.cap_c);
    CpuEscape(&d, BC250_CPU_OP_READ, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && !(d.Flags & BC250_CPU_FLAG_REVERT_OWED));
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3200u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && native_firmware.limit_mhz == 3200u);
}

/* The restore refused as well: every setter is refused with the reason the CLI names, a readback still runs, and the
 * worker's next pass sends the restore again. A stop in that state writes the mark again. */
static void test_a_refused_restore_keeps_the_surface_shut(void)
{
    BC250_ESCAPE_CPU d;

    LoadAndStopWithRevertRefused();
    Chip(1200u, 3500u);
    Load();                                                         /* the firmware still refuses every setter */
    CHECK(device.Cpu.LeftoverOwed && !device.Cpu.Applied.temp_given);   /* the start settings wait too */
    CHECK(native_log_has("the leftover restore is refused"));
    CHECK(native_firmware.limit_mhz == 3000u);
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3200u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_REFUSED && d.Error == BC250_CPU_REQUEST_ERROR_LEFTOVER);
    CHECK(d.NtStatus == (ULONG)STATUS_INVALID_DEVICE_STATE && (d.Flags & BC250_CPU_FLAG_REVERT_OWED));
    CpuEscape(&d, BC250_CPU_OP_RESET, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_REFUSED && d.Error == BC250_CPU_REQUEST_ERROR_LEFTOVER);
    CpuEscape(&d, BC250_CPU_OP_READBACK, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE);
    CHECK(device.Cpu.Baseline.max_mhz == 3500u);                    /* the readback did not move the baseline */
    CHECK(Volatile(L"CpuLeftover", L"Owed") == 1u);

    native_firmware.accept_writes = 1;
    CpuThread(&device);                                             /* the worker's next pass: the retry */
    CHECK(!device.Cpu.LeftoverOwed);
    CHECK(Volatile(L"CpuLeftover", L"Owed") == 0u);
    CHECK(native_firmware.limit_mhz == 3500u && native_firmware.scale == bc250_cpu_scale_argument(0u));
    CHECK(device.Cpu.Applied.temp_given && device.Cpu.Applied.temp_c == BC250_CPU_TEMP_DEFAULT_C);
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3200u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && native_firmware.limit_mhz == 3200u);

    /* A stop while the restore is still owed carries the mark on. */
    LoadAndStopWithRevertRefused();
    Chip(1200u, 3500u);
    Load();
    CHECK(device.Cpu.LeftoverOwed);
    CpuStop(&device);
    CHECK(Volatile(L"CpuLeftover", L"Owed") == 1u && (Volatile(L"CpuLeftover", L"What") & 4u));
    printf("  %-31s setters refused with error %lu, the retry restored %u MHz\n", "leftover_restore_refused",
           (ULONG)BC250_CPU_REQUEST_ERROR_LEFTOVER, 3500u);
}

/* A record this build does not know, or a partial one, is not a baseline: the start reads the chip, as before
 * BD-295, and leaves the record as it is. */
static void test_a_foreign_record_is_ignored(void)
{
    Boot();
    (void)GuardVolatileStore(L"CpuBoot", L"Version", 2u);
    (void)GuardVolatileStore(L"CpuBoot", L"MaxMHz", 3500u);
    (void)GuardVolatileStore(L"CpuBoot", L"CapC", 100u);
    (void)GuardVolatileStore(L"CpuBoot", L"TableMHz", 3200u);
    (void)GuardVolatileStore(L"CpuBoot", L"BoostMHz", 3500u);
    (void)GuardVolatileStore(L"CpuBoot", L"Valid", 1u);
    native_firmware.limit_mhz = 3000u;
    Chip(1200u, 3500u);
    Load();
    CHECK(native_log_has("the boot record is version 2, not 1"));
    CHECK(!native_log_has("adopts the boot record"));
    CHECK(device.Cpu.Baseline.max_mhz == 3200u);                    /* this start's own read */
    CHECK(Volatile(L"CpuBoot", L"Version") == 2u && Volatile(L"CpuBoot", L"MaxMHz") == 3500u);

    Boot();
    (void)GuardVolatileStore(L"CpuBoot", L"Version", 1u);
    (void)GuardVolatileStore(L"CpuBoot", L"MaxMHz", 3000u);
    (void)GuardVolatileStore(L"CpuBoot", L"Valid", 0u);             /* a writer stopped before Valid */
    Chip(1200u, 3500u);
    Load();
    CHECK(native_log_has("the boot record is partial"));
    CHECK(device.Cpu.Baseline.max_mhz == 3500u && Volatile(L"CpuBoot", L"Valid") == 0u);

    Boot();
    native_volatile_fail = STATUS_INVALID_DEVICE_STATE;              /* a store that is not ready */
    Chip(1200u, 3500u);
    Load();
    native_volatile_fail = STATUS_SUCCESS;
    CHECK(device.Cpu.Proven && device.Cpu.Baseline.max_mhz == 3500u);
    CHECK(native_log_has("the boot record is not readable"));
    printf("  %-31s version 2, partial and unreadable records: the start keeps its own read\n", "foreign_record");
}

/* The D0 after a power transition takes the record too, and so does a start whose stored settings were dropped
 * after an earlier start of the boot may have sent them. */
static void test_resume_and_dropped_settings(void)
{
    BC250_ESCAPE_CPU d;

    Boot();
    Chip(1200u, 3500u);
    Load();
    CpuPause(&device);
    CpuResume(&device);
    native_lines = 0;
    native_firmware.limit_mhz = 3000u;
    Chip(1200u, 3500u);
    CpuThread(&device);
    CHECK(native_log_has("adopts the boot record") && device.Cpu.Baseline.max_mhz == 3500u);

    Boot();
    Chip(1200u, 3500u);
    Load();
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3000u, 0u);
    CpuEscape(&d, BC250_CPU_OP_KEEP, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE);
    CpuStop(&device);
    NativeSetSetting(L"CpuPending", 0x1234u);                       /* the kept set never became healthy */
    Chip(1200u, 3500u);
    Load();
    CHECK(native_log_has("may have sent the dropped settings"));
    CHECK(!device.Cpu.Stored.max_given && !device.Cpu.LeftoverOwed);
    CHECK(native_firmware.limit_mhz == 3500u);                      /* the dropped limit is out of the chip */
    printf("  %-31s resume adopts the record; a dropped kept limit goes back to %u MHz\n", "resume_dropped",
           native_firmware.limit_mhz);
}

/* BD-297: a start whose read stage does not answer keeps its start work. The next pass tries again and the default
 * cap goes in; setters wait for it. After BC250_CPU_START_TRIES attempts the worker gives up, and a readback that
 * answers gives the start back to it. */
static void test_a_refused_start_read_is_tried_again(void)
{
    BC250_ESCAPE_CPU d;
    unsigned int writes;

    Boot();
    Chip(1200u, 3500u);
    native_firmware.refuse_getters = 1;
    Load();                                                         /* attempt 1 refused */
    CHECK(!device.Cpu.Proven && device.Cpu.StartWork && !device.Cpu.StartMissed);
    CHECK(native_firmware.cap_c == 100u && native_firmware.writes == 0);
    CHECK(native_log_has("tried again every"));
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3200u, 0u);
    CHECK(d.Status != BC250_ESCAPE_STATUS_DONE && d.NtStatus == (ULONG)STATUS_DEVICE_NOT_READY);
    native_firmware.refuse_getters = 0;
    /* A readback that answers proves queue 3 while the start is owed: setters still wait for the start apply. */
    CpuEscape(&d, BC250_CPU_OP_READBACK, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && device.Cpu.Proven && device.Cpu.StartWork);
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3200u, 0u);
    CHECK(d.Status != BC250_ESCAPE_STATUS_DONE && d.NtStatus == (ULONG)STATUS_DEVICE_NOT_READY);
    CHECK(native_firmware.writes == 0);
    Chip(1200u, 3500u);
    CpuThread(&device);                                             /* the next pass: the stage answers */
    CHECK(device.Cpu.Proven && !device.Cpu.StartWork && !device.Cpu.StartMissed);
    CHECK(native_firmware.cap_c == BC250_CPU_TEMP_DEFAULT_C);
    CHECK(device.Cpu.Applied.temp_given && device.Cpu.Applied.temp_c == BC250_CPU_TEMP_DEFAULT_C);
    CpuEscape(&d, BC250_CPU_OP_SET, BC250_CPU_GIVEN_MAX, 3200u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && native_firmware.limit_mhz == 3200u);
    CpuStop(&device);

    /* The worker's own retries, in one thread: they run out after BC250_CPU_START_TRIES attempts. */
    Boot();
    Chip(1200u, 3500u);
    native_firmware.refuse_getters = 1;
    native_wait_passes = 20u * BC250_CPU_START_TRIES;               /* 100 ms waits, 1000 ms between attempts */
    Load();
    native_wait_passes = 0u;
    CHECK(!device.Cpu.Proven && !device.Cpu.StartWork && device.Cpu.StartMissed);
    CHECK(native_log_has("did not answer in 30 attempts"));
    CHECK(native_firmware.cap_c == 100u && native_firmware.writes == 0);
    writes = (unsigned int)native_firmware.writes;
    native_firmware.refuse_getters = 0;
    Chip(1200u, 3500u);
    CpuEscape(&d, BC250_CPU_OP_READBACK, 0u, 0u, 0u);
    CHECK(d.Status == BC250_ESCAPE_STATUS_DONE && device.Cpu.StartWork && !device.Cpu.StartMissed);
    CHECK(native_log_has("the start apply runs now"));
    CHECK((unsigned int)native_firmware.writes == writes);         /* the readback itself sends no setter */
    Chip(1200u, 3500u);
    CpuThread(&device);
    CHECK(native_firmware.cap_c == BC250_CPU_TEMP_DEFAULT_C && !device.Cpu.StartWork);
    CpuStop(&device);
    printf("  %-31s a refused start read is tried again; after %u attempts a readback restarts it\n",
           "start_read_retry", (unsigned int)BC250_CPU_START_TRIES);
}

int main(void)
{
    test_ramp();
    test_thermally_limited();
    test_delayed_answer();
    test_hot_part_runs_no_probe();
    test_refused_core_reads();
    test_an_answered_part_owes_no_probe();
    test_the_stop_rule();
    test_start_applies_the_default_cap();
    test_start_keeps_a_stored_cap();
    test_start_adds_the_cap_to_stored_settings();
    test_reset_returns_to_the_default_cap();
    test_failed_transaction_admits_the_cpu();
    test_first_start_writes_the_boot_record();
    test_a_kept_limit_does_not_become_the_baseline();
    test_a_refused_revert_is_restored_at_the_next_start();
    test_a_refused_restore_keeps_the_surface_shut();
    test_a_foreign_record_is_ignored();
    test_resume_and_dropped_settings();
    test_a_refused_start_read_is_tried_again();
    printf("cpu native (the boost probe's sweep, the default cap, the boot record): %ld checks, %ld failures\n",
           native_checks, native_failures);
    return native_failures != 0 ? 1 : 0;
}
