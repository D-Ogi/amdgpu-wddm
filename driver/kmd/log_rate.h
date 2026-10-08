/* log_rate.h - how often one line of a hot path may reach the driver's log ring (BD-097).
 *
 * Why it exists. The log ring keeps BC250_LOG_RING_LINES lines, of which only the tail rotates, so the ring holds
 * a number of SECONDS that depends on how fast the driver writes. The b23 lab read of 2026-10-08 measured it: at
 * an idle desktop 708 of the 768 rotating lines were "gfx: paging submit ...", one per SDMA paging submit, about
 * two a second, and the 768 lines held 354 s. The two mode-set marks of an hour before were gone, which is the
 * defect BD-097 is about: the evidence of a rare event is pushed out by a line that says the same thing every
 * time. The driver already has one answer to this, a gate that turns a hot line off (gfx.c HotSubmitLog, D5),
 * and one more, a cap on the first calls (BC250_WDDM_LOG_CALLS). Neither keeps a cadence: the gate loses the
 * line until an operator reads the registry, and the cap loses it after the first seconds of a device start.
 *
 * What this object decides. For one call of one hot line, with the time in milliseconds, it answers
 *   LINE     - write the line as it is;
 *   SUMMARY  - write one line that carries the count of the calls left out, instead of this call's own line;
 *   DROP     - write nothing, and count this call.
 * The rule:
 *   - the first BC250_LOG_RATE_BURST calls of the device start are written in full, so the first work of a start
 *     reads exactly as it did before this header existed;
 *   - after them one line per BC250_LOG_RATE_INTERVAL_MS, for as long as the path stays busy, and that line is
 *     a SUMMARY whenever calls were left out;
 *   - a call that follows BC250_LOG_RATE_GAP_MS of quiet also writes, but only while the interval still has
 *     room (BC250_LOG_RATE_INTERVAL_LINES lines), so that work after an idle desktop is in the log and a caller
 *     whose own period is longer than the gap cannot write a line per call.
 * A caller that must never lose a line (every error path) does not come here at all.
 *
 * The interval cap is what bounds the rate, whatever shape the traffic has. Two lines a minute is 120 lines an
 * hour of the ring's 768 rotating lines, and the measured idle traffic gives 67 lines an hour. A rule without
 * that cap gave one line per call for every caller whose period was longer than the gap.
 *
 * Everything is integer arithmetic on a millisecond clock the caller reads, with no kernel call, no lock and no
 * floating point, so the caller may hold its own lock across it and the host test can drive it with a made-up
 * clock (driver/kmd/test/log_rate_test.c). A clock that goes backwards counts as no time passing, never as a
 * huge interval: a wrong decision would be one line too many, and a negative interval in a summary is fiction.
 *
 * It decides nothing about the device. Every field is evidence or a counter; nothing here is control flow.
 */
#ifndef BC250_LOG_RATE_H
#define BC250_LOG_RATE_H

/* Eight lines, the same number as BC250_WDDM_LOG_CALLS, so one reader's rule of thumb covers both: "the first
 * eight of anything are in the log in full". Sixty seconds, the cadence the b23 fix gave the telemetry block
 * (BD-097), because the lines this governs are a cadence and not an event: at the measured two submits a second
 * it is 67 lines an hour instead of 7200. Two seconds of quiet is longer than a frame interval, so a call after
 * it is new work and not the same stream. Two lines an interval, which bounds the whole rule at 120 lines an
 * hour of the ring's 768 rotating lines. */
#define BC250_LOG_RATE_BURST 8u
#define BC250_LOG_RATE_INTERVAL_MS 60000u
#define BC250_LOG_RATE_GAP_MS 2000u
#define BC250_LOG_RATE_INTERVAL_LINES 2u

#define BC250_LOG_RATE_DROP 0u
#define BC250_LOG_RATE_LINE 1u
#define BC250_LOG_RATE_SUMMARY 2u

/* One governed line's state. The caller owns it and serializes its calls (gfx.c holds a spin lock of its own
 * across the decision, which costs a few instructions and no log call). Written is the lines that reached the
 * ring, Skipped the calls that did not, both since the state was reset: they are what the log summary reports,
 * so a quiet log is never read as a quiet path. */
typedef struct _BC250_LOG_RATE {
    unsigned long Burst;                /* lines of the device start already written in full */
    unsigned long IntervalLines;        /* lines written in the current interval */
    unsigned long Pending;              /* calls left out since the last line that was written */
    unsigned long long IntervalMs;      /* when the current interval started */
    unsigned long long LastMs;          /* the last call, written or not */
    unsigned long Calls;                /* calls, including this one */
    unsigned long Written;              /* of them, the ones that got a line of their own */
    unsigned long Summaries;            /* of them, the ones that got a summary line */
    unsigned long Skipped;              /* calls that wrote nothing at all */
    unsigned long Started;              /* 1 once the first call has been seen: a zeroed state is "never called" */
} BC250_LOG_RATE;

/* What a SUMMARY line says. Pending is the calls this summary line accounts for, its own call included (so a
 * summary with Pending 13 stands for 13 submits, 12 of which wrote nothing), ElapsedMs how long they took, and
 * Calls/Skipped the totals since the state was reset. */
typedef struct _BC250_LOG_RATE_NOTE {
    unsigned long Pending;
    unsigned long long ElapsedMs;
    unsigned long Calls;
    unsigned long Skipped;
} BC250_LOG_RATE_NOTE;

static __inline void Bc250LogRateReset(BC250_LOG_RATE* Rate)
{
    Rate->Burst = 0; Rate->IntervalLines = 0; Rate->Pending = 0;
    Rate->IntervalMs = 0; Rate->LastMs = 0;
    Rate->Calls = 0; Rate->Written = 0; Rate->Summaries = 0; Rate->Skipped = 0;
    Rate->Started = 0;
}

/* Milliseconds from From to To, and 0 for a clock that did not move or went backwards. */
static __inline unsigned long long Bc250LogRateSince(unsigned long long From, unsigned long long To)
{
    return (To > From) ? (To - From) : 0ull;
}

/* The decision for one call. Note is filled on every outcome, so a caller may log the totals from it, but only
 * SUMMARY asks for them to be printed. */
static __inline unsigned long Bc250LogRateDecide(BC250_LOG_RATE* Rate, unsigned long long Ms,
                                                 BC250_LOG_RATE_NOTE* Note)
{
    unsigned long long since, elapsed;
    unsigned long outcome;

    Rate->Calls++;
    since = Rate->Started ? Bc250LogRateSince(Rate->LastMs, Ms) : 0ull;
    elapsed = Rate->Started ? Bc250LogRateSince(Rate->IntervalMs, Ms) : 0ull;

    if (!Rate->Started)
    {
        /* The first call of this state's life: the device start's first line. */
        outcome = BC250_LOG_RATE_LINE;
        Rate->Burst = 1;
        Rate->IntervalMs = Ms;
        Rate->IntervalLines = 1;
    }
    else if (Rate->Burst < (unsigned long)BC250_LOG_RATE_BURST)
    {
        /* Still inside the first lines of the device start, whenever they come. */
        outcome = BC250_LOG_RATE_LINE;
        Rate->Burst++;
        Rate->IntervalLines++;
    }
    else if (elapsed >= (unsigned long long)BC250_LOG_RATE_INTERVAL_MS)
    {
        /* The interval's own line. It carries the count whenever calls were left out. */
        outcome = (Rate->Pending != 0) ? BC250_LOG_RATE_SUMMARY : BC250_LOG_RATE_LINE;
        Rate->IntervalMs = Ms;
        Rate->IntervalLines = 1;
    }
    else if (since >= (unsigned long long)BC250_LOG_RATE_GAP_MS &&
             Rate->IntervalLines < (unsigned long)BC250_LOG_RATE_INTERVAL_LINES)
    {
        /* Work after a quiet gap, while the interval still has room: the line a reader needs to see that the
         * path started again. What the gap ended is said here, because nobody would know of it otherwise. */
        outcome = (Rate->Pending != 0) ? BC250_LOG_RATE_SUMMARY : BC250_LOG_RATE_LINE;
        Rate->IntervalLines++;
    }
    else outcome = BC250_LOG_RATE_DROP;

    Rate->LastMs = Ms;
    Rate->Started = 1;

    if (outcome == BC250_LOG_RATE_DROP)
    {
        Rate->Pending++;
        Rate->Skipped++;
    }
    Note->Pending = (outcome == BC250_LOG_RATE_SUMMARY) ? Rate->Pending + 1u : Rate->Pending;
    Note->ElapsedMs = elapsed;
    if (outcome == BC250_LOG_RATE_LINE) Rate->Written++;
    if (outcome == BC250_LOG_RATE_SUMMARY)
    {
        Rate->Summaries++;
        Rate->Pending = 0;                  /* the calls it accounts for are counted in Skipped already */
    }
    Note->Calls = Rate->Calls;
    Note->Skipped = Rate->Skipped;
    return outcome;
}

#endif /* BC250_LOG_RATE_H */
