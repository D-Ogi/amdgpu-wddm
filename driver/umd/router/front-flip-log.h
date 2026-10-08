// SPDX-License-Identifier: MIT
//
// M15.14 increment 3: the log of `pfnCheckDirectFlipSupport`, by change and by count.
//
// Increment 2 wrote one line per call, up to 256 lines per device. The compositor asks the question at
// every present of a chain it may flip, so a line per call is a line per frame: trial 478 spent the 256 lines
// in the first seconds of The Witcher 3, and the later segments of the session (the exclusive 1920x1080 mode
// among them) had no line at all. A log that stops when the interesting part starts is a log nobody can use.
//
// This log writes what changes, and counts the rest:
//   - a CHANGE LINE when the key of the answer differs from the key of the last line this device wrote. The
//     key is every field the line prints except the call number and the two handles: a flip chain's buffers
//     have one shape and the compositor alternates them, so a chain that keeps its answer writes one line.
//     The change lines have a budget (kFlipChangeLines) against an answer that alternates forever, and the
//     changes past it are counted, not lost;
//   - a COUNTER per rule of front-direct-flip.h, per answer and per flag (IMMEDIATE), for every call;
//   - a SUMMARY LINE with all the counters, every kFlipSummaryMs of wall time while the compositor asks, and
//     once more when the device is destroyed. A trial therefore reads the whole session: which answers were
//     given, how many times, and which clause refused the rest.
//
// The functions below are pure apart from the interlocked operations, so the host gate drives them with its
// own clock (tests/test-router.cpp, front-rule).
#ifndef BC250_FRONT_FLIP_LOG_H
#define BC250_FRONT_FLIP_LOG_H

#include <windows.h>

namespace bc250front {

// One counter per FlipRefusal value (front-direct-flip.h asserts that the count matches the enum).
static const unsigned kFlipRules = 14;
static const LONG kFlipChangeLines = 256;
static const ULONGLONG kFlipSummaryMs = 30000;

struct FlipLog {
    volatile LONG64 last_key;            // the key of the last change line; 0 before the first one
    volatile LONG changes;               // answers whose key differed from the last line's
    volatile LONG unchanged;             // answers whose key was the last line's: no line
    volatile LONG immediate;             // calls with D3D11_1DDI_CHECK_DIRECT_FLIP_IMMEDIATE
    volatile LONG rules[kFlipRules];     // calls per rule; rules[0] (FlipRefusal::none) is the TRUE count
    volatile LONG64 summary_tick;        // the clock of the last summary, or of the first call; 0 before it
    volatile LONG summaries;
};

// The key: FNV-1a over 64-bit words, never 0 (0 is "no line yet").
inline unsigned long long FlipKeyStart() { return 1469598103934665603ull; }
inline unsigned long long FlipKeyAdd(unsigned long long key, unsigned long long word)
{
    for (unsigned i = 0; i < 8; ++i) key = (key ^ ((word >> (i * 8)) & 0xFFu)) * 1099511628211ull;
    return key;
}

enum class FlipLine { none, change, suppressed };

// Counts one answer and says whether its change line must be written. `rule` is the FlipRefusal value.
inline FlipLine FlipLogAnswer(FlipLog *log, unsigned rule, bool immediate, unsigned long long key)
{
    if (rule < kFlipRules) InterlockedIncrement(&log->rules[rule]);
    if (immediate) InterlockedIncrement(&log->immediate);
    if (!key) key = 1;
    if (InterlockedExchange64(&log->last_key, (LONG64)key) == (LONG64)key) {
        InterlockedIncrement(&log->unchanged);
        return FlipLine::none;
    }
    return InterlockedIncrement(&log->changes) > kFlipChangeLines ? FlipLine::suppressed : FlipLine::change;
}

// Whether a periodic summary is due at `now` (milliseconds of a monotonic clock that is never 0). The first
// call only starts the period. One caller wins a period when several threads ask at once.
inline bool FlipLogSummaryDue(FlipLog *log, ULONGLONG now, ULONGLONG interval)
{
    const LONG64 last = log->summary_tick;
    if (!last) {
        InterlockedCompareExchange64(&log->summary_tick, (LONG64)now, 0);
        return false;
    }
    if (now - (ULONGLONG)last < interval) return false;
    if (InterlockedCompareExchange64(&log->summary_tick, (LONG64)now, last) != last) return false;
    InterlockedIncrement(&log->summaries);
    return true;
}

// The totals a summary line prints.
struct FlipTotals {
    long calls, supported, refused, lines, suppressed;
};
inline FlipTotals FlipLogTotals(const FlipLog *log)
{
    FlipTotals t = {};
    for (unsigned i = 0; i < kFlipRules; ++i) t.calls += log->rules[i];
    t.supported = log->rules[0];
    t.refused = t.calls - t.supported;
    const long changes = log->changes;
    t.lines = changes < kFlipChangeLines ? changes : kFlipChangeLines;
    t.suppressed = changes - t.lines;
    return t;
}

}  // namespace bc250front

#endif
