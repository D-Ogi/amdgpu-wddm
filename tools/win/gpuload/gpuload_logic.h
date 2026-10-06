/*
 * gpuload_logic.h - the parts of gpuload that need no GPU: the kernel's CPU reference, the checked invocations, the
 * busy accounting over GPU timestamps, the per-batch work controller and the report lines. gpuload.c and
 * gpuload_host_test.c include the same code, so the host test exercises what the client runs.
 */
#ifndef GPULOAD_LOGIC_H
#define GPULOAD_LOGIC_H

#include <stdint.h>
#include <stdio.h>

/* The exit code contract (README.md): the DPM trial's dpm-step.ps1 tells these apart. */
enum {
    GL_EXIT_OK = 0,      /* ran for --seconds, every checked word matched */
    GL_EXIT_CHECK = 1,   /* a checked word differed from the CPU reference: the GPU did not do the work */
    GL_EXIT_USAGE = 2,
    GL_EXIT_ERROR = 3,   /* Vulkan or system error, including device loss and a fence wait timeout */
    GL_EXIT_STOPPED = 4  /* the stop file appeared: ended early on request, the checks so far matched */
};

#define GL_GROUP_SIZE 256u          /* local_size_x of shaders/spin.comp */
#define GL_CHECKS_PER_BATCH 8u      /* invocations of every batch compared with GlExpected */
#define GL_MIN_ITERS 16u
#define GL_MAX_ITERS (1u << 24)

/* shaders/spin.comp, to the bit: uint32 arithmetic wraps the same way in GLSL and in C. */
static inline uint32_t GlExpected(uint32_t seed, uint32_t i, uint32_t iters)
{
    uint32_t x = seed ^ (i * 0x9E3779B9u), y = i + 0x7F4A7C15u, k;
    for (k = 0; k < iters; k++) {
        x = x * 1664525u + 1013904223u;
        y = (y ^ (x >> 13)) * 0x5BD1E995u;
    }
    return x ^ y;
}

/* The seed of batch b. Consecutive batches differ, so a slot whose buffer still holds an older batch's words (the GPU
 * never ran the new one) fails the check. */
static inline uint32_t GlSeed(uint64_t batch)
{
    return (uint32_t)((batch * 0x9E3779B97F4A7C15ull) >> 32) ^ 0xA511E9B3u;
}

/* Checked invocation j of batch b for a grid of n invocations (n >= GL_CHECKS_PER_BATCH): one in each eighth of the
 * grid, at an offset that moves with the batch, so over consecutive batches every position of an eighth is checked,
 * the first and the last invocation of the grid included. */
static inline uint32_t GlCheckIndex(uint32_t n, uint64_t batch, uint32_t j)
{
    uint32_t part = n / GL_CHECKS_PER_BATCH;
    return j * part + (uint32_t)((batch * 2654435761ull) % part) + (j == GL_CHECKS_PER_BATCH - 1u ? n % GL_CHECKS_PER_BATCH : 0u);
}

/*
 * Busy accounting in GPU time. Batches arrive in completion order with their top-of-pipe and bottom-of-pipe
 * timestamps (ticks, already made relative to the first one). The new work of a batch is the part of [start, end]
 * after the previous batch's end; the span it adds runs from the previous end to its end. Overlapping batches are
 * therefore never counted twice, and a gap between them counts as idle. work / span is the share of GPU time with a
 * batch in flight, independent of the timestamp period.
 */
typedef struct {
    int have;
    uint64_t prev_end;
} GlBusy;

static inline void GlBusyAdd(GlBusy *b, uint64_t start, uint64_t end, uint64_t *work, uint64_t *span)
{
    uint64_t from;
    *work = 0;
    *span = 0;
    if (end < start) end = start;
    if (!b->have) {
        b->have = 1;
        b->prev_end = end;
        *work = end - start;
        *span = end - start;
        return;
    }
    if (end <= b->prev_end) return;  /* inside the previous batch's interval: nothing new */
    from = start > b->prev_end ? start : b->prev_end;
    *work = end - from;
    *span = end - b->prev_end;
    b->prev_end = end;
}

/* Ticks between two raw timestamps of a counter with valid_bits bits (wraps once at most). */
static inline uint64_t GlTicks(uint64_t from, uint64_t to, uint32_t valid_bits)
{
    uint64_t mask = valid_bits >= 64 ? ~0ull : ((1ull << valid_bits) - 1ull);
    return (to - from) & mask;
}

static inline uint32_t GlClampIters(double v)
{
    if (v < (double)GL_MIN_ITERS) return GL_MIN_ITERS;
    if (v > (double)GL_MAX_ITERS) return GL_MAX_ITERS;
    return (uint32_t)v;
}

/* Calibration step (one batch at a time, nothing else in flight): straight to the count that would take target_ms at
 * the measured rate, at most 8x up or down per step. */
static inline uint32_t GlCalibrateIters(uint32_t iters, double batch_ms, double target_ms)
{
    double want = batch_ms > 0.0 ? (double)iters * target_ms / batch_ms : 8.0 * iters;
    if (want > 8.0 * iters) want = 8.0 * iters;
    if (want < iters / 8.0) want = iters / 8.0;
    return GlClampIters(want);
}

/* Steady state: from the count in use (iters), halfway toward the count that would take target_ms at the rate a
 * retired batch of measured_iters showed in work_ms of new GPU time, never more than doubled or halved per batch.
 * Batches still in flight were recorded with older counts, hence the two arguments. The clock moves under the load
 * (that is the point), so a batch keeps its length in time, not in iterations. */
static inline uint32_t GlNextIters(uint32_t iters, uint32_t measured_iters, double work_ms, double target_ms)
{
    double want, next;
    if (work_ms <= 0.0) return iters;  /* a batch that added no GPU time tells nothing about the rate */
    want = (double)measured_iters * target_ms / work_ms;
    next = 0.5 * ((double)iters + want);
    if (next > 2.0 * iters) next = 2.0 * iters;
    if (next < 0.5 * iters) next = 0.5 * iters;
    return GlClampIters(next);
}

/* The report lines dpm-step.ps1 parses (ConvertFrom-GpuLoadReport in dpm-lib.ps1); `gpuload --format-sample` prints
 * one of each so that parser is tested against this code, not against a copy of it. */
static inline int GlFormatTick(char *buf, size_t size, double t, uint64_t batches, uint32_t iters, double batch_ms,
                        double busy_pct)
{
    return snprintf(buf, size, "gpuload t=%.1f batches=%llu iters=%u batch_ms=%.2f busy_pct=%.1f", t,
                    (unsigned long long)batches, iters, batch_ms, busy_pct);
}

static inline int GlFormatResult(char *buf, size_t size, const char *result, const char *stop, double seconds,
                          uint64_t batches, uint64_t checked, uint64_t mismatches, double busy_pct,
                          double busy_min_pct, double gpu_ms, uint32_t iters)
{
    return snprintf(buf, size,
                    "gpuload result=%s stop=%s seconds=%.1f batches=%llu checked=%llu mismatches=%llu busy_pct=%.1f "
                    "busy_min_1s_pct=%.1f gpu_ms=%.1f iters=%u",
                    result, stop, seconds, (unsigned long long)batches, (unsigned long long)checked,
                    (unsigned long long)mismatches, busy_pct, busy_min_pct, gpu_ms, iters);
}

#endif
