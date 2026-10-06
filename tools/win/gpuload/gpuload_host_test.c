// gpuload_host_test - development-PC checks of gpuload_logic.h, no Vulkan, no GPU.
//
//   1. GlExpected and GlSeed against vectors computed by an independent implementation (Python integers masked to
//      32/64 bits, 2026-10-02), so the CPU reference is the kernel shaders/spin.comp states and not a copy of a typo;
//   2. GlCheckIndex stays inside the grid, puts one check in each eighth, and over consecutive batches reaches every
//      position, the first and the last invocation included;
//   3. consecutive seeds differ for every in-flight depth, so a stale slot cannot pass;
//   4. GlBusyAdd: back-to-back batches are 100 %, gaps are idle, overlaps and contained batches are never counted
//      twice, and a counter of fewer than 64 bits wraps through GlTicks;
//   5. the work controller converges on --batch-ms for a GPU whose rate changes under it (the clock rising 1000 ->
//      2000 MHz), never moves more than 2x per batch, and stays within its bounds;
//   6. the report lines carry exactly the keys and the order dpm-lib.ps1 parses;
//   7. the exit codes are distinct.
// Exit 0 when all pass, else the number of failed checks.

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "gpuload_logic.h"

static int g_Failed, g_Checks;

static void Check(int ok, const char *what, unsigned long long got, unsigned long long want)
{
    g_Checks++;
    if (ok) return;
    g_Failed++;
    printf("FAIL %s: got 0x%llX (%llu), want 0x%llX (%llu)\n", what, got, got, want, want);
}
#define EQ(what, got, want) Check((unsigned long long)(got) == (unsigned long long)(want), what, \
                                  (unsigned long long)(got), (unsigned long long)(want))
#define TRUE_(what, cond) Check((cond) != 0, what, 0, 1)

static void TestReference(void)
{
    EQ("expected iters 0", GlExpected(0, 0, 0), 0x7F4A7C15u);
    EQ("expected 1 iteration", GlExpected(0x12345678u, 0, 1), 0x38D5A78Bu);
    EQ("expected 1000 iterations", GlExpected(0x12345678u, 255, 1000), 0xFF29619Cu);
    EQ("expected last invocation of 1024 groups", GlExpected(0xDEADBEEFu, 262143u, 48211u), 0x93795877u);
    EQ("expected with batch 0 seed", GlExpected(GlSeed(0), 7, 3), 0x6223F408u);
    EQ("seed 0", GlSeed(0), 0xA511E9B3u);
    EQ("seed 1", GlSeed(1), 0x3B26900Au);
    EQ("seed 2", GlSeed(2), 0x997F1AC1u);
    EQ("seed 1000", GlSeed(1000), 0xADA2952Au);
    // One iteration more must change the word, or the negative control would prove nothing.
    TRUE_("one more iteration differs", GlExpected(GlSeed(5), 100, 4000) != GlExpected(GlSeed(5), 100, 4001));
}

static void TestCheckIndex(void)
{
    static const uint32_t grids[] = { 8u, 256u, 1024u * 256u, 3u * 256u, 65535u * 256u };
    for (size_t g = 0; g < sizeof(grids) / sizeof(grids[0]); g++) {
        uint32_t n = grids[g], part = n / GL_CHECKS_PER_BATCH, inside = 1, eighths = 1;
        uint32_t sawFirst = 0, sawLast = 0;
        uint64_t batches = part < 4096u ? part : 4096u;
        for (uint64_t b = 0; b < batches; b++)
            for (uint32_t j = 0; j < GL_CHECKS_PER_BATCH; j++) {
                uint32_t i = GlCheckIndex(n, b, j);
                if (i >= n) inside = 0;
                if (j < GL_CHECKS_PER_BATCH - 1u && i / part != j) eighths = 0;
                if (i == 0) sawFirst = 1;
                if (i == n - 1u) sawLast = 1;
            }
        TRUE_("check index inside the grid", inside);
        TRUE_("one check per eighth", eighths);
        if (part <= 4096u) {
            // part consecutive batches visit every offset of an eighth (2654435761 is odd and prime, part < 2^24).
            uint8_t seen[4096];
            int all = 1;
            memset(seen, 0, sizeof(seen));
            for (uint64_t b = 0; b < part; b++) seen[GlCheckIndex(n, b, 0)] = 1;
            for (uint32_t o = 0; o < part; o++) if (!seen[o]) all = 0;
            TRUE_("every offset of the first eighth over part batches", all);
            TRUE_("first invocation checked", sawFirst);
            TRUE_("last invocation checked", sawLast);
        }
    }
}

static void TestSeeds(void)
{
    int distinct = 1;
    for (uint64_t b = 0; b < 200000u; b++)
        for (uint64_t k = 1; k <= 4; k++)
            if (GlSeed(b) == GlSeed(b + k)) distinct = 0;
    TRUE_("seeds of batches up to 4 apart differ", distinct);
}

static void TestBusy(void)
{
    GlBusy b = { 0, 0 };
    uint64_t w, s, tw = 0, ts = 0;
    // Back to back: 100 %.
    GlBusyAdd(&b, 0, 1000, &w, &s); EQ("first work", w, 1000); EQ("first span", s, 1000);
    GlBusyAdd(&b, 1000, 2000, &w, &s); EQ("back-to-back work", w, 1000); EQ("back-to-back span", s, 1000);
    // A 250-tick gap: idle, so work 1000 of span 1250.
    GlBusyAdd(&b, 2250, 3250, &w, &s); EQ("gap work", w, 1000); EQ("gap span", s, 1250);
    // Overlap: started (top of pipe) before the previous ended; only the part after it is new.
    GlBusyAdd(&b, 3000, 4000, &w, &s); EQ("overlap work", w, 750); EQ("overlap span", s, 750);
    // Contained in the previous interval: nothing new.
    GlBusyAdd(&b, 3500, 3900, &w, &s); EQ("contained work", w, 0); EQ("contained span", s, 0);
    // End before start (a broken pair) counts as an empty batch at its start.
    GlBusyAdd(&b, 5000, 4500, &w, &s); EQ("inverted work", w, 0); EQ("inverted span", s, 1000);
    // A saturated pipeline of 1000 overlapping batches stays at 100 % and never above.
    {
        GlBusy p = { 0, 0 };
        for (uint64_t i = 0; i < 1000; i++) {
            GlBusyAdd(&p, i * 100u, i * 100u + 180u, &w, &s);
            tw += w;
            ts += s;
        }
        EQ("saturated work equals span", tw, ts);
    }
    // A 36-bit counter wrapping between base and end.
    {
        uint64_t mask = (1ull << 36) - 1ull, base = mask - 99u;
        EQ("wrapped ticks", GlTicks(base, 50u, 36), 150u);
        EQ("unwrapped ticks", GlTicks(100u, 400u, 64), 300u);
        EQ("full-width counter", GlTicks(~0ull - 9u, 5u, 64), 15u);
        (void)mask;
    }
}

static void TestController(void)
{
    // A GPU that does `rate` iterations per ms, doubling halfway (the governor raising the clock). The controller is
    // fed with what the retired batch showed, as gpuload does with two batches in flight.
    double rate = 2000.0, target = 20.0;
    uint32_t iters = 256, inflight[2] = { 256, 256 }, maxStep = 1;
    for (int step = 0; step < 12; step++) {
        double ms = iters / rate;
        if (ms >= 0.5 * target && ms <= 2.0 * target) break;
        iters = GlCalibrateIters(iters, ms, target);
    }
    TRUE_("calibration lands within 2x of the target", iters / rate >= 0.5 * target && iters / rate <= 2.0 * target);
    inflight[0] = inflight[1] = iters;
    for (int batch = 0; batch < 400; batch++) {
        uint32_t retired = inflight[batch % 2], before = iters;
        if (batch == 200) rate *= 2.0;
        iters = GlNextIters(iters, retired, retired / rate, target);
        if (iters > 2u * before || 2u * iters < before) maxStep = 0;
        inflight[batch % 2] = iters;
        if (batch == 199) TRUE_("settled at 1000 MHz within 5 %", iters / rate > 0.95 * target && iters / rate < 1.05 * target);
    }
    TRUE_("settled at 2000 MHz within 5 %", iters / rate > 0.95 * target && iters / rate < 1.05 * target);
    TRUE_("never more than 2x per batch", maxStep);
    EQ("upper bound", GlNextIters(GL_MAX_ITERS, GL_MAX_ITERS, 0.001, 20.0), GL_MAX_ITERS);
    EQ("lower bound", GlNextIters(GL_MIN_ITERS, GL_MIN_ITERS, 1000.0, 20.0), GL_MIN_ITERS);
    EQ("no measurement keeps the count", GlNextIters(1000, 1000, 0.0, 20.0), 1000);
    EQ("calibrate bounded at 8x", GlCalibrateIters(256, 0.0001, 20.0), 2048);
    EQ("calibrate bounded at 1/8", GlCalibrateIters(4096, 1000.0, 20.0), 512);
}

static void TestLines(void)
{
    char line[512];
    GlFormatTick(line, sizeof(line), 3.0, 151, 48211, 19.87, 99.4);
    TRUE_("tick line", !strcmp(line, "gpuload t=3.0 batches=151 iters=48211 batch_ms=19.87 busy_pct=99.4"));
    GlFormatResult(line, sizeof(line), "ok", "duration", 30.0, 1502, 1502, 0, 99.1, 97.8, 29811.5, 51034);
    TRUE_("result line", !strcmp(line, "gpuload result=ok stop=duration seconds=30.0 batches=1502 checked=1502 "
                                       "mismatches=0 busy_pct=99.1 busy_min_1s_pct=97.8 gpu_ms=29811.5 iters=51034"));
    if (g_Failed) printf("  last line: %s\n", line);
}

static void TestExitCodes(void)
{
    const int codes[] = { GL_EXIT_OK, GL_EXIT_CHECK, GL_EXIT_USAGE, GL_EXIT_ERROR, GL_EXIT_STOPPED };
    int distinct = 1;
    for (size_t i = 0; i < 5; i++)
        for (size_t j = i + 1; j < 5; j++)
            if (codes[i] == codes[j]) distinct = 0;
    TRUE_("exit codes distinct", distinct);
    EQ("ok is 0", GL_EXIT_OK, 0);
}

int main(void)
{
    TestReference();
    TestCheckIndex();
    TestSeeds();
    TestBusy();
    TestController();
    TestLines();
    TestExitCodes();
    printf("gpuload_host_test: %d checks, %d failed\n", g_Checks, g_Failed);
    return g_Failed;
}
