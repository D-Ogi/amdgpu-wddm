/* dpm_log_cadence.h - how often the periodic telemetry block reaches the driver log (BD-097).
 *
 * Why it exists. The KMD log ring keeps 1024 lines of which 768 rotate (docs/design/kmd-log-ring.md), so the
 * ring holds a number of SECONDS that depends on how fast the driver writes. The periodic telemetry block is
 * twelve lines of one governor tick (dpm, the idle line, the joint lines, hwmon, smu metrics, fan, curve), and
 * at an idle desktop nothing else writes: the lab read of 2026-10-09 measured 0.256 lines/s of which 78 % were
 * that block, one block every 60 s, and the 768 rotating lines then hold about 3000 s
 * (scratch/b26/lab-followups/bd097-ring-longidle.txt). The plan of BD-097 asks for an hour. A block every
 * 120 s at the idle point leaves about 4900 s, which is the only change this header makes to the numbers.
 *
 * What this object decides. For one check of the periodic block it answers whether the block is due now. The
 * rule:
 *   - under load (not the governor's idle state) the period is BC250_DPM_LOG_MS, 5 s, unchanged, because a
 *     game trial's kernel-log stream is read at that cadence;
 *   - at the governor's own idle state (BC250_DPM_FLAG_IDLE, the state of 0.7.207, not a new heuristic of this
 *     header) the period is the one in force from BC250_DPM_IDLE_LOG_MS or Parameters\TelemetryIdleLogMs;
 *   - the first block of a device start is always due, so a start writes its telemetry at once;
 *   - an idle period at or below BC250_DPM_LOG_MS means "no idle cadence", the behaviour of a driver before
 *     the b23 fix.
 * The caller still checks every BC250_DPM_LOG_MS, so the first block after the governor leaves the idle state
 * comes within 5 s. Nothing else passes through here: every state change (a clock level, a thermal step, a fan
 * mode, a fault, a refusal) writes its own line where it happens and this decision never sees it.
 *
 * Everything is integer arithmetic on the clock the caller reads (KeQueryInterruptTime units of 100 ns), with
 * no kernel call, no lock and no floating point, so the host test drives the whole rule with a made-up clock
 * (driver/kmd/test/dpm_log_cadence_test.c). A clock that goes backwards counts as no time passing, as in
 * log_rate.h: the block then waits, which costs one late block and never a 49-day interval.
 *
 * It decides nothing about the device. The answer only moves lines of evidence.
 */
#ifndef BC250_DPM_LOG_CADENCE_H
#define BC250_DPM_LOG_CADENCE_H

/* The two cadences and the bound of the registry value. 5 s is what a trial's stream expects. 120 s at the
 * idle point is the measured answer to the hour-long row of BD-097: 33 blocks in 1994 s were 78 % of the idle
 * traffic, so half of them is about 4900 s of ring at the same desktop. One hour is the largest value an
 * operator may ask for: a period longer than the ring's own span would make the block useless as a sign that
 * the governor still runs. */
#define BC250_DPM_LOG_MS 5000u              /* one telemetry block in the driver log under load */
#define BC250_DPM_IDLE_LOG_MS 120000u       /* the block's period at the idle point (BD-097), TelemetryIdleLogMs */
#define BC250_DPM_IDLE_LOG_MAX_MS 3600000u  /* the largest TelemetryIdleLogMs this driver accepts */

/* Units of KeQueryInterruptTime (100 ns) in one millisecond. The caller hands its own clock in these units, so
 * nothing here has to know which clock it is. */
#define BC250_DPM_LOG_UNITS_PER_MS 10000ull

/* The period in force from the value an operator stored, in milliseconds. Out of range takes the default, and
 * the caller says so in the log; 0 or a value at or below BC250_DPM_LOG_MS asks for no idle cadence at all and
 * comes back unchanged. */
static __inline unsigned long Bc250DpmIdleLogMs(unsigned long Setting)
{
    return (Setting > (unsigned long)BC250_DPM_IDLE_LOG_MAX_MS) ? (unsigned long)BC250_DPM_IDLE_LOG_MS : Setting;
}

/* The period of the periodic block for this check, in milliseconds. Idle is the governor's own idle state. */
static __inline unsigned long Bc250DpmTelemetryPeriodMs(int Idle, unsigned long IdleLogMs)
{
    if (!Idle || IdleLogMs <= (unsigned long)BC250_DPM_LOG_MS) return (unsigned long)BC250_DPM_LOG_MS;
    return IdleLogMs;
}

/* Is the periodic block due? LastLog is the clock of the last block and 0 when this start has written none;
 * Now is the same clock, read in this tick. */
static __inline int Bc250DpmTelemetryDue(int Idle, unsigned long IdleLogMs, unsigned long long LastLog,
                                         unsigned long long Now)
{
    unsigned long period = Bc250DpmTelemetryPeriodMs(Idle, IdleLogMs);
    unsigned long long elapsed;

    /* The first block of a device start, and every check whose period is the caller's own 5 s check. */
    if (LastLog == 0ull || period <= (unsigned long)BC250_DPM_LOG_MS) return 1;
    elapsed = (Now > LastLog) ? (Now - LastLog) : 0ull;
    return (elapsed >= (unsigned long long)period * BC250_DPM_LOG_UNITS_PER_MS) ? 1 : 0;
}

#endif /* BC250_DPM_LOG_CADENCE_H */
