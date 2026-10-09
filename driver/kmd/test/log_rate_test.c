/* Host control of the hot-path log rate limit (log_rate.h, BD-097). The rule is a pure decision over a
 * millisecond clock, so the whole mechanism runs here on a made-up clock, with no driver, no lock and no lab.
 *
 * The numbers the cases are built from are the b23 lab read of 2026-10-08
 * (scratch/train/b23/validation/bd097/ring-mix.txt): the idle desktop submits SDMA paging work about twice a
 * second, 708 of the ring's 768 rotating lines were that one line, and the ring held 354 s. The cases measure
 * what the same traffic writes now, that the first work of a device start still reads line for line, and that
 * no shape of traffic can write more than BC250_LOG_RATE_INTERVAL_LINES lines in one interval.
 */
#include <stdio.h>
#include <string.h>
#include "log_rate.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static unsigned long Decide(BC250_LOG_RATE* rate, unsigned long long ms, BC250_LOG_RATE_NOTE* note)
{
    memset(note, 0, sizeof(*note));
    return Bc250LogRateDecide(rate, ms, note);
}

/* Every call of one pattern: `count` calls `step` ms apart. Returns the lines they wrote. */
static unsigned long Lines(BC250_LOG_RATE* rate, unsigned long long step, unsigned long count)
{
    BC250_LOG_RATE_NOTE note;
    unsigned long long ms = 0;
    unsigned long i;

    Bc250LogRateReset(rate);
    for (i = 0; i < count; i++, ms += step) (void)Decide(rate, ms, &note);
    return rate->Written + rate->Summaries;
}

int main(void)
{
    BC250_LOG_RATE r;
    BC250_LOG_RATE_NOTE n;
    unsigned long long ms;
    unsigned long i, lines;

    /* ---- the constants: a reader's rule of thumb, and a cap that bounds the whole rule ------------------- */
    CHECK(BC250_LOG_RATE_BURST >= 1u);
    CHECK(BC250_LOG_RATE_INTERVAL_LINES >= 1u);
    CHECK(BC250_LOG_RATE_GAP_MS < BC250_LOG_RATE_INTERVAL_MS);
    CHECK(BC250_LOG_RATE_DROP == 0u && BC250_LOG_RATE_LINE == 1u && BC250_LOG_RATE_SUMMARY == 2u);
    /* The cap is the number that matters: at two lines a minute the rule costs the 768 rotating lines of the
     * ring 120 lines an hour, whatever shape the traffic has. */
    CHECK(BC250_LOG_RATE_INTERVAL_LINES * (3600000u / BC250_LOG_RATE_INTERVAL_MS) == 120u);

    /* ---- a zeroed state is "never called", and its first call is a line ---------------------------------- */
    Bc250LogRateReset(&r);
    CHECK(r.Started == 0 && r.Calls == 0 && r.Written == 0 && r.Skipped == 0);
    CHECK(Decide(&r, 0ull, &n) == BC250_LOG_RATE_LINE);
    CHECK(r.Started == 1 && r.Calls == 1 && r.Written == 1 && r.Burst == 1 && r.IntervalLines == 1);
    CHECK(n.Pending == 0 && n.ElapsedMs == 0 && n.Calls == 1 && n.Skipped == 0);

    /* A state zeroed by the caller (a device start does that with RtlZeroMemory, not with the reset) behaves
     * the same: Started is the one field that makes "never called" different from "called at ms 0". */
    memset(&r, 0, sizeof(r));
    CHECK(Decide(&r, 0ull, &n) == BC250_LOG_RATE_LINE);

    /* ---- the measured traffic: two submits a second ------------------------------------------------------ */
    Bc250LogRateReset(&r);
    /* The first eight are written in full, so the first work of a device start reads as it did before. */
    for (i = 0; i < BC250_LOG_RATE_BURST; i++)
    {
        CHECK(Decide(&r, (unsigned long long)i * 500ull, &n) == BC250_LOG_RATE_LINE);
        CHECK(n.Pending == 0);
    }
    CHECK(r.Written == BC250_LOG_RATE_BURST && r.Skipped == 0 && r.Summaries == 0);
    /* The ninth is dropped and counted. 500 ms between calls is under BC250_LOG_RATE_GAP_MS, so this is one
     * stream and not new work. */
    CHECK(Decide(&r, 4000ull, &n) == BC250_LOG_RATE_DROP);
    CHECK(r.Skipped == 1 && r.Pending == 1 && n.Pending == 1);
    /* Everything up to the interval is dropped: 4500..59500 is 111 more calls. */
    for (ms = 4500ull; ms < 60000ull; ms += 500ull)
        CHECK(Decide(&r, ms, &n) == BC250_LOG_RATE_DROP);
    CHECK(r.Skipped == 112 && r.Written == BC250_LOG_RATE_BURST && r.Summaries == 0);
    /* The first call at or after the interval carries them: 112 silent calls and its own, 60 s of them. */
    CHECK(Decide(&r, 60000ull, &n) == BC250_LOG_RATE_SUMMARY);
    CHECK(n.Pending == 113 && n.ElapsedMs == 60000ull && n.Calls == 121 && n.Skipped == 112);
    CHECK(r.Pending == 0 && r.Summaries == 1 && r.Skipped == 112 && r.IntervalLines == 1);
    /* The next interval starts at that line, not at the device start. */
    for (ms = 60500ull; ms < 120000ull; ms += 500ull)
        CHECK(Decide(&r, ms, &n) == BC250_LOG_RATE_DROP);
    CHECK(Decide(&r, 120000ull, &n) == BC250_LOG_RATE_SUMMARY);
    CHECK(n.Pending == 120 && n.ElapsedMs == 60000ull);

    /* ---- an hour of it: what the log ring is asked to hold ----------------------------------------------- */
    lines = Lines(&r, 500ull, 7200);
    CHECK(r.Calls == 7200);
    /* Eight full lines and one summary a minute, 59 of them (the first minute opens with the burst): 67 lines
     * of 7200 calls. The same hour wrote 7200 lines before this header, nine times the whole ring. */
    CHECK(r.Written == BC250_LOG_RATE_BURST && r.Summaries == 59 && lines == 67);
    CHECK(r.Skipped == r.Calls - lines);
    /* An hour of the measured traffic now costs under a tenth of the ring's 768 rotating lines. */
    CHECK(lines * 10u < 768u);

    /* ---- the interval cap: no shape of traffic writes more than two lines in one interval ---------------- */
    /* A caller whose own period is longer than the gap would take the gap clause at every call. The cap is
     * what stops it: 720 polls an hour, 5 s apart, write 126 lines and not 720. */
    lines = Lines(&r, 5000ull, 720);
    CHECK(r.Calls == 720 && lines == 126 && r.Written == 67 && r.Summaries == 59);
    CHECK(lines <= BC250_LOG_RATE_BURST + 2u * (3600000u / BC250_LOG_RATE_INTERVAL_MS));
    /* The same holds for work that comes in pairs 3 s apart: 2400 calls an hour, 126 lines. */
    Bc250LogRateReset(&r);
    for (i = 0; i < 1200; i++)
    {
        (void)Decide(&r, (unsigned long long)i * 3000ull, &n);
        (void)Decide(&r, (unsigned long long)i * 3000ull + 20ull, &n);
    }
    CHECK(r.Calls == 2400 && r.Written + r.Summaries == 126);

    /* ---- work after a quiet gap, while the interval has room --------------------------------------------- */
    Bc250LogRateReset(&r);
    for (i = 0; i < BC250_LOG_RATE_BURST; i++) (void)Decide(&r, (unsigned long long)i * 100ull, &n);
    /* The burst spent this interval's room, so a gap in it writes nothing. */
    CHECK(Decide(&r, 700ull + BC250_LOG_RATE_GAP_MS, &n) == BC250_LOG_RATE_DROP);
    /* The interval's own line gives the room back. A gap of exactly BC250_LOG_RATE_GAP_MS is a gap (the names
     * read as "at least"), and the line after it says what the quiet period ended. */
    CHECK(Decide(&r, 60000ull, &n) == BC250_LOG_RATE_SUMMARY);
    CHECK(r.IntervalLines == 1);
    CHECK(Decide(&r, 60000ull + BC250_LOG_RATE_GAP_MS, &n) == BC250_LOG_RATE_LINE);
    CHECK(r.IntervalLines == 2 && n.Pending == 0);
    /* And the room is then gone until the next interval. */
    CHECK(Decide(&r, 60000ull + 2u * BC250_LOG_RATE_GAP_MS, &n) == BC250_LOG_RATE_DROP);

    /* ---- a clock that does not move, and one that goes backwards ----------------------------------------- */
    CHECK(Bc250LogRateSince(5ull, 5ull) == 0ull);
    CHECK(Bc250LogRateSince(5ull, 3ull) == 0ull);    /* never a huge unsigned difference */
    Bc250LogRateReset(&r);
    for (i = 0; i < BC250_LOG_RATE_BURST; i++) (void)Decide(&r, 1000ull, &n);
    /* The same millisecond for every call: the burst, then drops, and no summary, because no interval passed. */
    CHECK(Decide(&r, 1000ull, &n) == BC250_LOG_RATE_DROP);
    CHECK(r.Summaries == 0 && r.Written == BC250_LOG_RATE_BURST);
    /* A clock that jumps back has no time passing: still a drop, and ElapsedMs is 0, never a figure that would
     * read as a 49-day interval in the log. */
    CHECK(Decide(&r, 400ull, &n) == BC250_LOG_RATE_DROP);
    CHECK(n.ElapsedMs == 0ull);
    /* A jump cannot silence the line for good: the next call past the interval writes the count. */
    CHECK(Decide(&r, 1000ull + BC250_LOG_RATE_INTERVAL_MS, &n) == BC250_LOG_RATE_SUMMARY);
    CHECK(n.Pending == 3);

    /* ---- the counters are evidence: Calls = Written + Summaries + Skipped, always ------------------------ */
    Bc250LogRateReset(&r);
    for (ms = 0ull; ms < 120000ull; ms += 37ull)
    {
        (void)Decide(&r, ms, &n);
        CHECK(r.Calls == r.Written + r.Summaries + r.Skipped);
        CHECK(n.Calls == r.Calls && n.Skipped == r.Skipped);
    }
    CHECK(r.Calls == 3244 && r.Written == BC250_LOG_RATE_BURST && r.Summaries == 1);

    printf("PASS log_rate: %lu calls in the last case, %lu lines, %lu summaries, %lu silent\n",
           r.Calls, r.Written, r.Summaries, r.Skipped);
    return 0;
}
