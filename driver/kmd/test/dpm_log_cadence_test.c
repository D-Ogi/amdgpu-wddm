/* Host control of the periodic telemetry block's cadence (dpm_log_cadence.h, BD-097). The rule is a pure
 * decision over a clock, so the whole mechanism runs here on a made-up clock, with no driver and no lab.
 *
 * The numbers the cases are built from are the lab read of 2026-10-09 on tester.23, KMD 0.7.216.24
 * (scratch/b26/lab-followups/bd097-ring-longidle.txt): an idle desktop wrote 510 lines in 1994 s, 396 of them
 * the twelve-line telemetry block, one block every 60 s, so the 768 rotating lines of the ring held about
 * 3000 s where the plan of BD-097 asks for 3600 s. The cases measure the three decisions the governor makes
 * (idle, under load, the first block of a start), what an hour of each cadence costs the ring, and that an
 * operator's value cannot ask for a period the driver does not accept.
 */
#include <stdio.h>
#include "dpm_log_cadence.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

#define MS(x) ((unsigned long long)(x) * BC250_DPM_LOG_UNITS_PER_MS)

/* The blocks that one span of an unchanging state writes: the governor checks every BC250_DPM_LOG_MS and
 * writes whenever the decision says the block is due. The first check of the span is at BC250_DPM_LOG_MS, as
 * it is in the thread (NextLog starts one period after the thread's own start), so the clock of a written block
 * is never 0 and 0 keeps its meaning of "no block yet". Returns the blocks, and leaves the clock of the last
 * one in LastLog, exactly as the governor thread keeps it. */
static unsigned long Blocks(int idle, unsigned long idleLogMs, unsigned long long spanMs,
                            unsigned long long* lastLog)
{
    unsigned long long now;
    unsigned long blocks = 0;

    for (now = MS(BC250_DPM_LOG_MS); now <= MS(BC250_DPM_LOG_MS) + MS(spanMs); now += MS(BC250_DPM_LOG_MS))
    {
        if (!Bc250DpmTelemetryDue(idle, idleLogMs, *lastLog, now)) continue;
        *lastLog = now;
        blocks++;
    }
    return blocks;
}

int main(void)
{
    unsigned long long last;
    unsigned long blocks;
    unsigned long long now;
    unsigned long i;

    /* ---- the constants: the two cadences, and the bound of the operator's value --------------------------- */
    CHECK(BC250_DPM_LOG_MS == 5000u);                       /* a trial's kernel-log stream reads this cadence */
    CHECK(BC250_DPM_IDLE_LOG_MS > BC250_DPM_LOG_MS);
    CHECK(BC250_DPM_IDLE_LOG_MS % BC250_DPM_LOG_MS == 0u);  /* a whole number of checks, so no block drifts */
    CHECK(BC250_DPM_IDLE_LOG_MAX_MS == 3600000u);
    CHECK(BC250_DPM_IDLE_LOG_MS < BC250_DPM_IDLE_LOG_MAX_MS);
    CHECK(BC250_DPM_LOG_UNITS_PER_MS == 10000ull);          /* KeQueryInterruptTime units in a millisecond */

    /* ---- the period in force -------------------------------------------------------------------------------- */
    /* Under load the idle value changes nothing at all. */
    CHECK(Bc250DpmTelemetryPeriodMs(0, BC250_DPM_IDLE_LOG_MS) == BC250_DPM_LOG_MS);
    CHECK(Bc250DpmTelemetryPeriodMs(0, 0u) == BC250_DPM_LOG_MS);
    CHECK(Bc250DpmTelemetryPeriodMs(0, 600000u) == BC250_DPM_LOG_MS);
    /* At the idle state it is the value in force, whatever the default is. */
    CHECK(Bc250DpmTelemetryPeriodMs(1, BC250_DPM_IDLE_LOG_MS) == BC250_DPM_IDLE_LOG_MS);
    CHECK(Bc250DpmTelemetryPeriodMs(1, 60000u) == 60000u);
    /* 0, or a value the 5 s check already covers, asks for no idle cadence: the driver before the b23 fix. */
    CHECK(Bc250DpmTelemetryPeriodMs(1, 0u) == BC250_DPM_LOG_MS);
    CHECK(Bc250DpmTelemetryPeriodMs(1, BC250_DPM_LOG_MS) == BC250_DPM_LOG_MS);
    CHECK(Bc250DpmTelemetryPeriodMs(1, BC250_DPM_LOG_MS + 1u) == BC250_DPM_LOG_MS + 1u);

    /* ---- the operator's value: out of range takes the default ---------------------------------------------- */
    CHECK(Bc250DpmIdleLogMs(0u) == 0u);
    CHECK(Bc250DpmIdleLogMs(60000u) == 60000u);
    CHECK(Bc250DpmIdleLogMs(BC250_DPM_IDLE_LOG_MAX_MS) == BC250_DPM_IDLE_LOG_MAX_MS);
    CHECK(Bc250DpmIdleLogMs(BC250_DPM_IDLE_LOG_MAX_MS + 1u) == BC250_DPM_IDLE_LOG_MS);
    CHECK(Bc250DpmIdleLogMs(0xFFFFFFFFu) == BC250_DPM_IDLE_LOG_MS);
    /* Whatever an operator stores, the period in force is never above the bound. */
    for (i = 0; i < 64u; i++)
    {
        unsigned long stored = (i < 32u) ? (i * 250000u) : (0xFFFFFFFFu - i);
        CHECK(Bc250DpmTelemetryPeriodMs(1, Bc250DpmIdleLogMs(stored)) <= BC250_DPM_IDLE_LOG_MAX_MS);
    }

    /* ---- the first block of a device start is always due --------------------------------------------------- */
    /* The governor thread zeroes its tick state, so LastLog 0 is "no block yet this start". An idle start then
     * writes its telemetry at the first check and does not wait two minutes for it. */
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, 0ull, 0ull) == 1);
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, 0ull, MS(5000)) == 1);
    CHECK(Bc250DpmTelemetryDue(0, BC250_DPM_IDLE_LOG_MS, 0ull, 0ull) == 1);

    /* ---- idle: one block every 120 s ----------------------------------------------------------------------- */
    last = MS(1000);
    /* Every check inside the period is refused, the check at the period writes. 24 checks of 5 s make 120 s. */
    for (i = 1; i < 24u; i++)
        CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last + MS(i * 5000u)) == 0);
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last + MS(120000)) == 1);
    /* One tick before the period is not the period (the clock may stretch a tick, never shorten the cadence). */
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last + MS(119999)) == 0);
    /* An hour of an idle desktop with the default period: 30 blocks, where 60 s gave 60 and 5 s gave 720. This
     * is what the default has to deliver, so the behaviour is the case and the number follows it. */
    last = 0ull;
    blocks = Blocks(1, BC250_DPM_IDLE_LOG_MS, 3600000ull, &last);
    CHECK(blocks == 31u);                                   /* the start's own block plus one every 120 s */
    CHECK(last == MS(3605000));
    CHECK(BC250_DPM_IDLE_LOG_MS == 120000u);                /* BD-097: 60 s held 3000 s of ring, 120 s holds ~4900 */
    /* What that costs the ring. The lab measured twelve lines in the block, 0.256 lines/s in all of which 78 %
     * were the block: an hour of 120 s blocks is 360 lines of the 768 that rotate, where 60 s wrote 720 and
     * filled them. */
    CHECK((blocks - 1u) * 12u == 360u);
    CHECK((blocks - 1u) * 12u < 768u);
    last = 0ull;
    CHECK(Blocks(1, 60000u, 3600000ull, &last) == 61u);     /* the b23 cadence, for the comparison */
    CHECK((61u - 1u) * 12u == 720u);

    /* ---- under load: one block every 5 s, whatever the idle value says ------------------------------------- */
    last = 0ull;
    blocks = Blocks(0, BC250_DPM_IDLE_LOG_MS, 60000ull, &last);
    CHECK(blocks == 13u && last == MS(65000));              /* every 5 s check of the span writes */
    last = MS(4000);
    CHECK(Bc250DpmTelemetryDue(0, BC250_DPM_IDLE_LOG_MS, last, last + MS(5000)) == 1);
    /* A busy governor's own check is the cadence: the decision never refuses a block under load, not even one
     * that comes early because the timer stretched a tick. */
    CHECK(Bc250DpmTelemetryDue(0, BC250_DPM_IDLE_LOG_MS, last, last + MS(1)) == 1);
    CHECK(Bc250DpmTelemetryDue(0, 0u, last, last) == 1);

    /* ---- a state change: the block comes back within one check --------------------------------------------- */
    /* This is the half of BD-097 that matters for a reader. A governor at the idle point has just written a
     * block; a clock level, a thermal step, a fan mode or a fault writes its own line where it happens (dpm.c
     * DpmStep, the zone, fan.c, the fault paths), never through this decision, and the governor then leaves the
     * idle state. The next 5 s check must write the block, so the state behind the event line is in the log
     * beside it, and not two minutes later. */
    last = MS(10000);
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last + MS(5000)) == 0);      /* still idle */
    CHECK(Bc250DpmTelemetryDue(0, BC250_DPM_IDLE_LOG_MS, last, last + MS(5000)) == 1);      /* work arrived */
    /* And the way back: the first idle check after work keeps the block, then the 120 s cadence starts from it. */
    last = MS(5000);
    CHECK(Bc250DpmTelemetryDue(0, BC250_DPM_IDLE_LOG_MS, last, last + MS(5000)) == 1);
    last = last + MS(5000);
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last + MS(5000)) == 0);
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last + MS(120000)) == 1);

    /* ---- a desktop that wakes up now and then -------------------------------------------------------------- */
    /* Ten minutes of idle with one second of work every two and a half minutes: the work's own blocks come at
     * 5 s and the idle periods start again from them, so the log carries every piece of work and the block
     * count stays near the idle cadence. */
    last = 0ull;
    blocks = 0;
    for (now = MS(BC250_DPM_LOG_MS); now <= MS(605000); now += MS(BC250_DPM_LOG_MS))
    {
        int idle = ((now / MS(150000)) * MS(150000) == now) ? 0 : 1;   /* busy on every 150 s mark */
        if (!Bc250DpmTelemetryDue(idle, BC250_DPM_IDLE_LOG_MS, last, now)) continue;
        last = now;
        blocks++;
    }
    CHECK(blocks == 9u);
    CHECK(blocks * 12u < 768u / 4u);

    /* ---- a clock that does not move, and one that goes backwards ------------------------------------------- */
    last = MS(100000);
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last) == 0);
    /* A clock that jumps back has no time passing: the block waits, and nothing reads as a 49-day interval. */
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, MS(1)) == 0);
    /* A jump cannot silence the block for good: the check past the period from the stored clock writes. */
    CHECK(Bc250DpmTelemetryDue(1, BC250_DPM_IDLE_LOG_MS, last, last + MS(BC250_DPM_IDLE_LOG_MS)) == 1);
    /* Under load a backwards clock changes nothing, because the 5 s check is the cadence. */
    CHECK(Bc250DpmTelemetryDue(0, BC250_DPM_IDLE_LOG_MS, last, MS(1)) == 1);

    /* ---- the largest period an operator may ask for -------------------------------------------------------- */
    last = 0ull;
    blocks = Blocks(1, BC250_DPM_IDLE_LOG_MAX_MS, 3600000ull, &last);
    CHECK(blocks == 2u);                                    /* the start's block and one an hour later */
    CHECK(last == MS(3605000));

    printf("PASS dpm_log_cadence: idle %lu ms, load %lu ms, %lu blocks an hour idle (%lu lines of the ring's 768)\n",
           (unsigned long)BC250_DPM_IDLE_LOG_MS, (unsigned long)BC250_DPM_LOG_MS, 30ul, 360ul);
    return 0;
}
