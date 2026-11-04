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
 * Every arm also asserts the probe's discipline: it pins itself to the processor it runs on and reverts, it sends
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
    printf("cpu native (the boost probe's sweep, the default cap): %ld checks, %ld failures\n", native_checks,
           native_failures);
    return native_failures != 0 ? 1 : 0;
}
