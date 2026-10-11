// hipbench.hip - what one kernel launch of the HIP path costs the caller, measured as the wall
// time of the enqueue loop.
//
// Why it exists. Build 1 of M16 route B is correctness first: every hipLaunchKernel builds its
// own indirect buffer and calls D3DKMTSubmitCommand, so one kernel costs one user-to-kernel
// transition and one kernel driver submission. A decode step of a language model issues
// hundreds of small kernels, and llama.cpp is the named consumer of this route, so that cost is
// the one to measure before anything is tuned. This program measures it and prints the ratio
// the route is judged by: submissions per launch.
//
// It is a normal HIP program, built by clang against our own hip/hip_runtime.h and our own
// import library, exactly as samples/vadd.hip is. It therefore runs
//   * on the development PC against the mock build of amdhip64.dll, where it proves the harness
//     and the counters and measures the host half of the cost (the mock runs no instruction),
//   * on the lab against the product DLL, where the numbers are the real ones.
//
// What the timed loop of measurements 1 and 2 is, exactly. The interval is the wall time of the
// loop of HIP launches: enqueue-loop wall time with backpressure. It is not the time one
// processor core spends before any shader runs, because a launch inside the loop can wait for
// the GPU: `hipLaunchKernel` waits for a free entry of the 64-buffer kernel argument pool
// (runtime/hip_launch.cpp) and a submission waits for a free command slot of the ring
// (bc250hsa.h section 8: dispatch submission "may wait for a free ring slot"). The counters
// printed beside the number say how often that happened: `waits` and `waits_fast`. A claim about
// processor cost alone needs the active and blocked time of the thread measured separately, and
// this program does not measure them (independent audit of 2026-10-10, finding HIP-F3).
//
// Five measurements:
//   1. launch   N empty kernels back to back, then one synchronisation. The per-launch figure is
//               the enqueue-loop wall time divided by N, and the wall is the whole thing. This
//               is the number a program sees when it does not wait per kernel.
//   2. chain    C tiny kernels, each reading what the one before it wrote. This is the shape of
//               a decode step, and the one the barrier between two dispatches belongs to.
//   3a. verify  the copy routes, byte for byte and untimed, before any rate is measured: the
//               staging ring, the direct route, device to device and pitched copies, at their
//               edges (measurement 3a below). --verify-fault is its positive control.
//   3b.         the same for hipHostMalloc buffers at the first byte of their host view, where a
//               view can start right after another (DEFECTS BD-123): the direct route,
//               hipMemset and hipHostGetDevicePointer (measurement 3b below).
//   3. copy     hipMemcpy host to device and device to host at 4 KB, 64 KB, 1 MB and 64 MB,
//               into pageable memory and into memory this program asked hipHostMalloc for.
//               Build 1 copied through the host mapping, so the line was a memory bandwidth
//               and not a copy engine; with the GPU copy route of
//               scratch/hip-gpu-copy/CONTRACT.md the two device-to-host arms are the copy
//               engine, and the counter line below each rate says which route each one took.
//   4. sync     hipStreamSynchronize on a stream whose work has already retired. It should read
//               the fence mapping and return, and the counters say whether it did (waits_fast).
//   5. event    hipEventRecord and hipEventSynchronize on retired work, the same way.
//
// Each measurement prints one line of plain text and, with --json, one JSON line as well, which
// is what the lab record keeps. A second line, "time:", says where the wall time of the interval
// went, from the duration counters of the DLL: the time inside D3DKMTSubmitCommand, inside the
// waits (fast ones and the others), and inside each copy route, in microseconds. The longest
// wait is the longest since the process started, not of the interval, because a maximum has no
// difference; the JSON line carries the same fields.
//
// Bounds. A lab trial of this program must stay inside three minutes. The program keeps its own
// budget (--budget-ms, 150000 by default), checks it between measurements and stops when it is
// spent. Every wait of the runtime is bounded as well (--wait-total).
//
// Switches of the backend. Batching and the barrier are policy of the caller, and the runtime
// reads them from the environment at its first call (section 8.1 of bc250hsa.h). This program
// sets them before its first HIP call, so one trial can run all four arms:
//   hipbench.exe --batch 0                     one submission per launch, the build-1 behaviour
//   hipbench.exe --batch 1 --barrier full      one buffer per batch, the full acquire between
//   hipbench.exe --batch 1 --barrier light     the same with the level-0 and level-1 invalidate
//
// Exit code 0 means every HIP call answered hipSuccess and, with --expect-compute, every result
// was exact. A measurement is never a verdict by itself: the numbers go into the lab record.

#include "hip/hip_runtime.h"

#include <chrono>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAIN_ITEMS 1024
#define CHAIN_BLOCK 256

// The empty kernel of measurement 1. It must not be optimised away, and a kernel with no
// argument and no body is exactly what the launch path has to carry.
__global__ void nothing() {}

// The tiny dependent kernel of measurement 2: every element reads what the dispatch before it
// wrote into the same place. A batch that leaves out the barrier between two dispatches, or that
// runs them out of order, shows up here as a wrong sum.
__global__ void __launch_bounds__(CHAIN_BLOCK) bump(int* values, int n) {
    const int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < n) {
        values[i] = values[i] + 1;
    }
}

static int g_failures = 0;
static int g_expect_compute = 0;
static int g_json = 0;

static void check(hipError_t err, const char* what) {
    if (err != hipSuccess) {
        printf("hipbench: %s failed: %s (%s)\n", what, hipGetErrorName(err),
               hipGetErrorString(err));
        g_failures++;
    }
}

static double now_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration<double, std::milli>(now).count();
}

static double g_started_ms = 0.0;
static double g_budget_ms = 150000.0;

// True while there is time left for another measurement. A trial that runs out says so and
// stops, instead of running past the bound of the lab.
static int budget_left(const char* next) {
    const double spent = now_ms() - g_started_ms;
    if (spent < g_budget_ms) {
        return 1;
    }
    printf("hipbench: the budget of %.0f ms is spent after %.0f ms; '%s' did not run\n",
           g_budget_ms, spent, next);
    return 0;
}

// One environment variable, set before the first HIP call of the process. The runtime reads its
// policy one time, at its first call, and _putenv_s writes the one environment block that the
// DLL's GetEnvironmentVariableA reads (the MEASURED note in runtime/hip_device.cpp).
static int set_env(const char* name, const char* value) {
#if defined(_WIN32)
    return _putenv_s(name, value) == 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

typedef struct counters_delta {
    unsigned long long dispatches;
    unsigned long long submissions;
    unsigned long long batches;
    unsigned long long dispatches_batched;
    unsigned long long waits;
    unsigned long long waits_fast;
    unsigned long long gpu_copy_d2h;
    unsigned long long gpu_copy_d2h_staged;
    unsigned long long gpu_copy_d2d;
    unsigned long long gpu_fill;
    unsigned long long gpu_fill_bytes;
    unsigned long long cpu_copy_fallback;
    unsigned long long cpu_copy_fallback_bytes;
    unsigned long long gpu_copy_packets;
    unsigned long long gpu_copy_forced_flushes;
    unsigned long long gpu_copy_control_failed;
    // The durations, in ticks of ticks_per_second. The two maxima are the values after the
    // interval, since a maximum has no difference.
    unsigned long long ticks_per_second;
    unsigned long long waits_fast_ticks;
    unsigned long long waits_fast_ticks_max;
    unsigned long long waits_other_ticks;
    unsigned long long waits_other_ticks_max;
    unsigned long long submit_calls;
    unsigned long long submit_ticks;
    unsigned long long gpu_copy_d2h_ticks;
    unsigned long long gpu_copy_d2h_staged_ticks;
    unsigned long long gpu_copy_d2d_ticks;
    unsigned long long gpu_fill_ticks;
    unsigned long long cpu_copy_fallback_ticks;
} counters_delta;

static bc250hipCounters read_counters() {
    bc250hipCounters counters;
    memset(&counters, 0, sizeof(counters));
    counters.struct_bytes = (unsigned)sizeof(counters);
    check(bc250hipGetCounters(&counters), "bc250hipGetCounters");
    return counters;
}

static counters_delta counters_since(const bc250hipCounters& before) {
    const bc250hipCounters after = read_counters();
    counters_delta d;
    d.dispatches = after.dispatches - before.dispatches;
    d.submissions = after.submissions - before.submissions;
    d.batches = after.batches - before.batches;
    d.dispatches_batched = after.dispatches_batched - before.dispatches_batched;
    d.waits = after.waits - before.waits;
    d.waits_fast = after.waits_fast - before.waits_fast;
    d.gpu_copy_d2h = after.gpu_copy_d2h - before.gpu_copy_d2h;
    d.gpu_copy_d2h_staged = after.gpu_copy_d2h_staged - before.gpu_copy_d2h_staged;
    d.gpu_copy_d2d = after.gpu_copy_d2d - before.gpu_copy_d2d;
    d.gpu_fill = after.gpu_fill - before.gpu_fill;
    d.gpu_fill_bytes = after.gpu_fill_bytes - before.gpu_fill_bytes;
    d.cpu_copy_fallback = after.cpu_copy_fallback - before.cpu_copy_fallback;
    d.cpu_copy_fallback_bytes = after.cpu_copy_fallback_bytes - before.cpu_copy_fallback_bytes;
    d.gpu_copy_packets = after.gpu_copy_packets - before.gpu_copy_packets;
    d.gpu_copy_forced_flushes = after.gpu_copy_forced_flushes - before.gpu_copy_forced_flushes;
    d.gpu_copy_control_failed = after.gpu_copy_control_failed;
    d.ticks_per_second = after.ticks_per_second;
    d.waits_fast_ticks = after.waits_fast_ticks - before.waits_fast_ticks;
    d.waits_fast_ticks_max = after.waits_fast_ticks_max;
    d.waits_other_ticks = after.waits_other_ticks - before.waits_other_ticks;
    d.waits_other_ticks_max = after.waits_other_ticks_max;
    d.submit_calls = after.submit_calls - before.submit_calls;
    d.submit_ticks = after.submit_ticks - before.submit_ticks;
    d.gpu_copy_d2h_ticks = after.gpu_copy_d2h_ticks - before.gpu_copy_d2h_ticks;
    d.gpu_copy_d2h_staged_ticks =
        after.gpu_copy_d2h_staged_ticks - before.gpu_copy_d2h_staged_ticks;
    d.gpu_copy_d2d_ticks = after.gpu_copy_d2d_ticks - before.gpu_copy_d2d_ticks;
    d.gpu_fill_ticks = after.gpu_fill_ticks - before.gpu_fill_ticks;
    d.cpu_copy_fallback_ticks = after.cpu_copy_fallback_ticks - before.cpu_copy_fallback_ticks;
    return d;
}

// Ticks of the duration counters in microseconds.
static double ticks_us(const counters_delta* d, unsigned long long ticks) {
    return d->ticks_per_second != 0 ? (double)ticks * 1000000.0 / (double)d->ticks_per_second
                                     : 0.0;
}

// The "time:" line of one measurement. The same fields for all of them, so that one pattern
// reads every line of a lab record.
static void report_time(const char* name, const counters_delta* d) {
    printf("hipbench: %-8s time: submit %llu calls %.3f us (%.3f us each), waits fast %.3f us,"
           " other %.3f us, longest wait since start fast %.3f us other %.3f us, gpu d2h"
           " %.3f us, staged %.3f us, d2d %.3f us, fill %.3f us, cpu fallback %.3f us\n",
           name, d->submit_calls, ticks_us(d, d->submit_ticks),
           d->submit_calls != 0 ? ticks_us(d, d->submit_ticks) / (double)d->submit_calls : 0.0,
           ticks_us(d, d->waits_fast_ticks), ticks_us(d, d->waits_other_ticks),
           ticks_us(d, d->waits_fast_ticks_max), ticks_us(d, d->waits_other_ticks_max),
           ticks_us(d, d->gpu_copy_d2h_ticks), ticks_us(d, d->gpu_copy_d2h_staged_ticks),
           ticks_us(d, d->gpu_copy_d2d_ticks), ticks_us(d, d->gpu_fill_ticks),
           ticks_us(d, d->cpu_copy_fallback_ticks));
}

// The same durations as JSON fields, written inside the object a measurement has open.
static void json_time_fields(const counters_delta* d) {
    printf(",\"submit_calls\":%llu,\"submit_us\":%.3f,\"waits_fast_us\":%.3f,"
           "\"waits_other_us\":%.3f,\"waits_fast_max_us\":%.3f,\"waits_other_max_us\":%.3f,"
           "\"gpu_copy_d2h_us\":%.3f,\"gpu_copy_d2h_staged_us\":%.3f,"
           "\"gpu_copy_d2d_us\":%.3f,\"gpu_fill_us\":%.3f,\"cpu_copy_fallback_us\":%.3f",
           d->submit_calls, ticks_us(d, d->submit_ticks), ticks_us(d, d->waits_fast_ticks),
           ticks_us(d, d->waits_other_ticks), ticks_us(d, d->waits_fast_ticks_max),
           ticks_us(d, d->waits_other_ticks_max), ticks_us(d, d->gpu_copy_d2h_ticks),
           ticks_us(d, d->gpu_copy_d2h_staged_ticks), ticks_us(d, d->gpu_copy_d2d_ticks),
           ticks_us(d, d->gpu_fill_ticks), ticks_us(d, d->cpu_copy_fallback_ticks));
}

// What the timed interval of this measurement is. The two kernel measurements time a loop of
// launches and the three others time one call each, so the name is not the same for all five.
#define HIPBENCH_ENQUEUE_METRIC "enqueue-loop wall time with backpressure"
#define HIPBENCH_CALL_METRIC "wall time of one call"

static void report_json(const char* name, double us_each, const char* metric, double wall_ms,
                        double rate, const char* rate_unit, const counters_delta* d,
                        long long count) {
    if (!g_json) {
        return;
    }
    // us_each keeps its name, because the lab records of 2026-10-09 are matched against it row
    // for row. What it means is stated beside it, so a reader of the record cannot take the
    // kernel measurements for processor-only cost: for launch and chain the interval is the
    // enqueue loop with its pool and ring backpressure inside it.
    printf("{\"tool\":\"hipbench\",\"measurement\":\"%s\",\"count\":%lld,\"us_each\":%.3f,"
           "\"us_each_metric\":\"%s\",\"wall_ms\":%.3f,\"rate\":%.3f,\"rate_unit\":\"%s\","
           "\"dispatches\":%llu,\"submissions\":%llu,\"batches\":%llu,"
           "\"dispatches_batched\":%llu,\"waits\":%llu,\"waits_fast\":%llu",
           name, count, us_each, metric, wall_ms, rate, rate_unit, d->dispatches, d->submissions,
           d->batches, d->dispatches_batched, d->waits, d->waits_fast);
    json_time_fields(d);
    printf("}\n");
}

// Measurement 1: the launch rate of an empty kernel.
static void measure_launch(hipStream_t stream, long long launches) {
    const bc250hipCounters before = read_counters();
    const double t0 = now_ms();
    for (long long i = 0; i < launches; ++i) {
        nothing<<<1, 64, 0, stream>>>();
    }
    const double t1 = now_ms();
    check(hipStreamSynchronize(stream), "launch: hipStreamSynchronize");
    const double t2 = now_ms();
    const counters_delta d = counters_since(before);
    const double us_each = (t1 - t0) * 1000.0 / (double)launches;
    const double per_launch_submissions =
        d.dispatches != 0 ? (double)d.submissions / (double)d.dispatches : 0.0;
    check(hipGetLastError(), "launch: the error state after the launches");
    printf("hipbench: launch   %lld empty kernels: %.3f us each of enqueue-loop wall time with"
           " backpressure (%llu waits, %llu fast), wall %.1f ms, %.3f submissions per launch\n",
           launches, us_each, d.waits, d.waits_fast, t2 - t0, per_launch_submissions);
    report_time("launch", &d);
    report_json("launch", us_each, HIPBENCH_ENQUEUE_METRIC, t2 - t0, per_launch_submissions,
                "submissions/launch", &d, launches);
}

// Measurement 2: a dependent chain of tiny kernels, the shape of a decode step.
static void measure_chain(hipStream_t stream, int* values, long long links) {
    check(hipMemset(values, 0, sizeof(int) * CHAIN_ITEMS), "chain: hipMemset");
    const bc250hipCounters before = read_counters();
    const double t0 = now_ms();
    for (long long i = 0; i < links; ++i) {
        bump<<<CHAIN_ITEMS / CHAIN_BLOCK, CHAIN_BLOCK, 0, stream>>>(values, CHAIN_ITEMS);
    }
    const double t1 = now_ms();
    check(hipStreamSynchronize(stream), "chain: hipStreamSynchronize");
    const double t2 = now_ms();
    const counters_delta d = counters_since(before);
    check(hipGetLastError(), "chain: the error state after the launches");

    int mismatches = 0;
    if (g_expect_compute) {
        int* host = (int*)malloc(sizeof(int) * CHAIN_ITEMS);
        if (host == NULL) {
            printf("hipbench: chain: out of host memory\n");
            g_failures++;
            return;
        }
        memset(host, 0, sizeof(int) * CHAIN_ITEMS);
        check(hipMemcpy(host, values, sizeof(int) * CHAIN_ITEMS, hipMemcpyDeviceToHost),
              "chain: hipMemcpy back");
        for (int i = 0; i < CHAIN_ITEMS; ++i) {
            if (host[i] != (int)links) {
                if (mismatches < 3) {
                    printf("hipbench: chain: item %d is %d, the chain of %lld says %lld\n", i,
                           host[i], links, links);
                }
                mismatches++;
            }
        }
        free(host);
        if (mismatches != 0) {
            g_failures++;
        }
    }
    const double us_each = (t1 - t0) * 1000.0 / (double)links;
    const double per_link_ms = (t2 - t0) / (double)links;
    printf("hipbench: chain    %lld dependent kernels: %.3f us each of enqueue-loop wall time"
           " with backpressure (%llu waits, %llu fast), %.4f ms each end to end, wall %.1f ms,"
           " %llu submissions, exact %s\n",
           links, us_each, d.waits, d.waits_fast, per_link_ms, t2 - t0, d.submissions,
           g_expect_compute ? (mismatches == 0 ? "yes" : "NO") : "not checked");
    report_time("chain", &d);
    report_json("chain", us_each, HIPBENCH_ENQUEUE_METRIC, t2 - t0,
                d.dispatches != 0 ? (double)d.submissions / (double)d.dispatches : 0.0,
                "submissions/launch", &d, links);
}

// Measurement 3a: the copy routes, checked byte for byte and not timed.
//
// The timed copy loop below measures rate only: it copies back into the buffer it copied from,
// so a device-to-host copy that wrote nothing would still leave the expected bytes there. This
// pass is the oracle of the routes the rates are about (the review of 2026-10-11). Every
// case writes a source whose bytes depend on their position, so a chunk that lands at the
// wrong offset is wrong and not merely present; makes the whole destination buffer dirty
// first; copies; and compares every byte of that buffer: the range must hold the source and
// every byte around it must still be dirty.
//
// The cases are the edges of the new code: more than four staging rounds with a ragged last
// round, the 64 MiB copy whose last 32 bytes travel in a second DMA_DATA packet (67,108,832
// bytes is one packet), unaligned source and destination offsets, a device-to-device copy, and
// a pitched two-dimensional copy, once with rows above the threshold and once with short rows.
// A destination this program could not get from hipHostMalloc is SKIP, and a SKIP cannot
// accept the direct route: the lab plan counts it as not run.
//
// --verify-fault is the positive control of this oracle: omit-tail leaves the final chunk of
// every case out (32 bytes, or the last row), corrupt-tail changes the last byte of every
// destination after the copy. Either must turn every case that runs into WRONG and the exit
// code into 1; a control that passes would mean the oracle cannot see what it is for.
enum VerifyFault { kFaultNone = 0, kFaultOmitTail, kFaultCorruptTail };

static unsigned char pattern_at(size_t i) {
    unsigned long long x = (unsigned long long)i * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    return (unsigned char)(x >> 32);
}

#define VERIFY_DIRTY 0xD5
#define VERIFY_GUARD 256u
#define VERIFY_BIG (64u * 1024u * 1024u)

typedef struct verify_tally {
    int passed;
    int failed;
    int skipped;
} verify_tally;

// One verdict line. `got` is the whole destination buffer as it reads back; `want` says what
// each byte must be.
typedef unsigned char (*verify_want_fn)(size_t i, const void* ctx);

static void verify_report(verify_tally* tally, const char* name, const unsigned char* got,
                          size_t size, verify_want_fn want, const void* ctx) {
    size_t wrong = 0;
    size_t first = 0;
    for (size_t i = 0; i < size; ++i) {
        if (got[i] != want(i, ctx)) {
            if (wrong == 0) {
                first = i;
            }
            ++wrong;
        }
    }
    if (wrong == 0) {
        tally->passed++;
        printf("hipbench: verify   %-44s ok, %zu bytes compared\n", name, size);
        return;
    }
    tally->failed++;
    g_failures++;
    printf("hipbench: verify   %-44s WRONG: %zu of %zu bytes, the first at offset %zu"
           " (0x%02X, expected 0x%02X)\n",
           name, wrong, size, first, got[first], want(first, ctx));
}

static void verify_skip(verify_tally* tally, const char* name, const char* why) {
    tally->skipped++;
    printf("hipbench: verify   %-44s SKIP: %s\n", name, why);
}

// A one-dimensional copy of n bytes from source offset src_off into destination offset dst_off.
typedef struct linear_case {
    size_t dst_off;
    size_t src_off;
    size_t n;
} linear_case;

static unsigned char want_linear(size_t i, const void* ctx) {
    const linear_case* c = (const linear_case*)ctx;
    if (i >= c->dst_off && i - c->dst_off < c->n) {
        return pattern_at(c->src_off + (i - c->dst_off));
    }
    return VERIFY_DIRTY;
}

// A pitched copy of `height` rows of `width` bytes, read at spitch and written at dpitch.
typedef struct pitched_case {
    size_t width;
    size_t height;
    size_t spitch;
    size_t dpitch;
} pitched_case;

static unsigned char want_pitched(size_t i, const void* ctx) {
    const pitched_case* c = (const pitched_case*)ctx;
    const size_t row = i / c->dpitch;
    const size_t column = i % c->dpitch;
    if (row < c->height && column < c->width) {
        return pattern_at(row * c->spitch + column);
    }
    return VERIFY_DIRTY;
}

// The device destination of a device-to-device case, read back through a route that the cases
// before it have already checked.
static int read_device(void* host, const void* device, size_t bytes) {
    const hipError_t err = hipMemcpy(host, device, bytes, hipMemcpyDeviceToHost);
    check(err, "verify: read the device destination back");
    return err == hipSuccess;
}

static void verify_copies(int fault) {
    const size_t size = (size_t)VERIFY_BIG + 2u * VERIFY_GUARD;
    const size_t tail = fault == kFaultOmitTail ? 32u : 0u;
    verify_tally tally = { 0, 0, 0 };
    unsigned char* source = (unsigned char*)malloc(size);
    unsigned char* pageable = (unsigned char*)malloc(size);
    unsigned char* pinned = NULL;
    unsigned char* device_src = NULL;
    unsigned char* device_dst = NULL;
    if (source == NULL || pageable == NULL) {
        printf("hipbench: verify: out of host memory for %zu bytes\n", size);
        g_failures++;
        free(source);
        free(pageable);
        return;
    }
    for (size_t i = 0; i < size; ++i) {
        source[i] = pattern_at(i);
    }
    check(hipMalloc((void**)&device_src, size), "verify: hipMalloc the source");
    check(hipMalloc((void**)&device_dst, size), "verify: hipMalloc the destination");
    if (device_src == NULL || device_dst == NULL) {
        free(source);
        free(pageable);
        return;
    }
    if (hipHostMalloc((void**)&pinned, size, hipHostMallocDefault) != hipSuccess) {
        pinned = NULL;
        (void)hipGetLastError();
    }
    check(hipMemcpy(device_src, source, size, hipMemcpyHostToDevice), "verify: the source");
    const bc250hipCounters before = read_counters();

    // Device to host into the program's own memory: the staging ring.
    {
        const char* name = "staged, 6 rounds, unaligned";
        const linear_case c = { 5u, 3u, 5u * 1024u * 1024u + 123u };
        memset(pageable, VERIFY_DIRTY, size);
        check(hipMemcpy(pageable + c.dst_off, device_src + c.src_off, c.n - tail,
                        hipMemcpyDeviceToHost),
              "verify: staged copy");
        if (fault == kFaultCorruptTail) {
            pageable[c.dst_off + c.n - 1] ^= 0xFF;
        }
        verify_report(&tally, name, pageable, size, want_linear, &c);
    }

    // Device to host into memory hipHostMalloc returned: one packet, or two at 64 MiB.
    const linear_case direct_cases[2] = { { VERIFY_GUARD, 0u, VERIFY_BIG },
                                          { 3u, 1u, 1024u * 1024u + 7u } };
    const char* const direct_names[2] = { "direct, 64 MiB, split tail",
                                          "direct, unaligned" };
    for (int k = 0; k < 2; ++k) {
        const linear_case* c = &direct_cases[k];
        if (pinned == NULL) {
            verify_skip(&tally, direct_names[k], "hipHostMalloc gave no destination");
            continue;
        }
        memset(pinned, VERIFY_DIRTY, size);
        check(hipMemcpy(pinned + c->dst_off, device_src + c->src_off, c->n - tail,
                        hipMemcpyDeviceToHost),
              "verify: direct copy");
        if (fault == kFaultCorruptTail) {
            pinned[c->dst_off + c->n - 1] ^= 0xFF;
        }
        verify_report(&tally, direct_names[k], pinned, size, want_linear, c);
    }

    // Device to device, read back through the direct route when there is one and through the
    // staging ring otherwise.
    const linear_case d2d_cases[2] = { { VERIFY_GUARD, 0u, VERIFY_BIG },
                                       { 2u, 5u, 1024u * 1024u + 13u } };
    const char* const d2d_names[2] = { "device to device, 64 MiB, split tail",
                                       "device to device, unaligned" };
    for (int k = 0; k < 2; ++k) {
        const linear_case* c = &d2d_cases[k];
        unsigned char* readback = pinned != NULL ? pinned : pageable;
        check(hipMemset(device_dst, VERIFY_DIRTY, size), "verify: a dirty device destination");
        check(hipMemcpy(device_dst + c->dst_off, device_src + c->src_off, c->n - tail,
                        hipMemcpyDeviceToDevice),
              "verify: device-to-device copy");
        if (fault == kFaultCorruptTail) {
            const unsigned char bad = (unsigned char)(pattern_at(c->src_off + c->n - 1) ^ 0xFF);
            check(hipMemcpy(device_dst + c->dst_off + c->n - 1, &bad, 1, hipMemcpyHostToDevice),
                  "verify: corrupt the device tail");
        }
        if (read_device(readback, device_dst, size)) {
            verify_report(&tally, d2d_names[k], readback, size, want_linear, c);
        } else {
            tally.failed++;
        }
    }

    // A pitched copy into the program's own memory, rows above the threshold and short rows.
    const pitched_case pitched[2] = { { 4100u, 5u, 8192u, 4608u }, { 100u, 4u, 256u, 160u } };
    const char* const pitched_names[2] = { "pitched 2D, rows above the threshold",
                                           "pitched 2D, short rows" };
    for (int k = 0; k < 2; ++k) {
        const pitched_case* c = &pitched[k];
        const size_t region = c->dpitch * (c->height + 1u);
        const size_t rows = fault == kFaultOmitTail ? c->height - 1u : c->height;
        memset(pageable, VERIFY_DIRTY, region);
        check(hipMemcpy2DAsync(pageable, c->dpitch, device_src, c->spitch, c->width, rows,
                               hipMemcpyDeviceToHost, NULL),
              "verify: pitched copy");
        check(hipStreamSynchronize(NULL), "verify: the pitched copy retires");
        if (fault == kFaultCorruptTail) {
            pageable[(c->height - 1u) * c->dpitch + c->width - 1u] ^= 0xFF;
        }
        verify_report(&tally, pitched_names[k], pageable, region, want_pitched, c);
    }

    const counters_delta d = counters_since(before);
    printf("hipbench: verify   %d cases: %d passed, %d failed, %d skipped%s\n",
           tally.passed + tally.failed + tally.skipped, tally.passed, tally.failed, tally.skipped,
           fault == kFaultNone ? ""
                               : (fault == kFaultOmitTail ? " (fault control: omit-tail)"
                                                          : " (fault control: corrupt-tail)"));
    printf("hipbench: verify   routes: gpu d2h %llu, staged %llu, d2d %llu, cpu fallback %llu"
           " (%llu bytes), packets %llu, control failed %llu\n",
           d.gpu_copy_d2h, d.gpu_copy_d2h_staged, d.gpu_copy_d2d, d.cpu_copy_fallback,
           d.cpu_copy_fallback_bytes, d.gpu_copy_packets, d.gpu_copy_control_failed);
    if (g_json) {
        printf("{\"tool\":\"hipbench\",\"measurement\":\"verify\",\"fault\":\"%s\","
               "\"cases\":%d,\"passed\":%d,\"failed\":%d,\"skipped\":%d,"
               "\"gpu_copy_d2h\":%llu,\"gpu_copy_d2h_staged\":%llu,\"gpu_copy_d2d\":%llu,"
               "\"cpu_copy_fallback\":%llu,\"cpu_copy_fallback_bytes\":%llu,"
               "\"gpu_copy_packets\":%llu,\"gpu_copy_control_failed\":%llu}\n",
               fault == kFaultNone ? "none"
                                   : (fault == kFaultOmitTail ? "omit-tail" : "corrupt-tail"),
               tally.passed + tally.failed + tally.skipped, tally.passed, tally.failed,
               tally.skipped, d.gpu_copy_d2h, d.gpu_copy_d2h_staged, d.gpu_copy_d2d,
               d.cpu_copy_fallback, d.cpu_copy_fallback_bytes, d.gpu_copy_packets,
               d.gpu_copy_control_failed);
    }

    if (pinned != NULL) {
        check(hipHostFree(pinned), "verify: hipHostFree");
    }
    check(hipFree(device_dst), "verify: hipFree the destination");
    check(hipFree(device_src), "verify: hipFree the source");
    free(pageable);
    free(source);
}

// Measurement 3b: host views back to back (DEFECTS BD-123).
//
// The operating system can place the host view of a hipHostMalloc buffer at the first byte after
// the view of another allocation. Up to e8be31bf the runtime looked up such a pointer with a host
// range closed at its end, so it named the allocation whose view ends there: in lab round 2 the
// two read-backs of measurement 3a into hipHostMalloc memory at offset 0 took the staging ring.
// The same lookup serves hipMemset and hipHostGetDevicePointer. This pass allocates one hipMalloc
// buffer and then ADJACENT_VIEWS hipHostMalloc buffers of one size, says which of them start at
// the end of another of them (the view of the hipMalloc buffer is the runtime's own and cannot
// be seen here), and checks each one at its first byte:
//   - a device-to-host copy into it lands, and takes the route that a copy into byte 4096 of the
//     same buffer took just before it: the direct route when the runtime has one, and the same
//     route as an interior pointer otherwise, so the pass also runs with the route off or
//     against a runtime without it;
//   - hipMemset of it fills it, and a buffer whose view ends where it starts keeps its bytes;
//   - hipHostGetDevicePointer of it is the address of byte 4096 less 4096, and a read through
//     that address returns the buffer's bytes.
// The routes are reported on their own line, so the routes line of measurement 3a stays
// comparable with earlier rounds. --verify-fault applies here as well.
#define ADJACENT_VIEWS 4
#define ADJACENT_BYTES (1024u * 1024u)
#define ADJACENT_FILL 0x5A
#define ADJACENT_KEEP 0x3C

static unsigned char want_byte(size_t i, const void* ctx) {
    (void)i;
    return *(const unsigned char*)ctx;
}

static void verify_adjacent(int fault) {
    const size_t size = ADJACENT_BYTES;
    const size_t tail = fault == kFaultOmitTail ? 32u : 0u;
    verify_tally tally = { 0, 0, 0 };
    unsigned char* views[ADJACENT_VIEWS] = { NULL };
    int after[ADJACENT_VIEWS];   // the view whose host range ends where this one starts, or -1
    unsigned char* device = NULL;
    unsigned char* pattern = (unsigned char*)malloc(size);
    unsigned char* readback = (unsigned char*)malloc(size);
    if (pattern == NULL || readback == NULL) {
        printf("hipbench: verify: out of host memory for %zu bytes\n", size);
        g_failures++;
        free(pattern);
        free(readback);
        return;
    }
    for (size_t i = 0; i < size; ++i) {
        pattern[i] = pattern_at(i);
    }
    check(hipMalloc((void**)&device, size), "verify: adjacent hipMalloc");
    for (int k = 0; k < ADJACENT_VIEWS; ++k) {
        if (hipHostMalloc((void**)&views[k], size, hipHostMallocDefault) != hipSuccess) {
            views[k] = NULL;
            (void)hipGetLastError();
        }
    }
    if (device != NULL) {
        check(hipMemcpy(device, pattern, size, hipMemcpyHostToDevice), "verify: adjacent source");
    }
    int pairs = 0;
    for (int k = 0; k < ADJACENT_VIEWS; ++k) {
        after[k] = -1;
        for (int j = 0; j < ADJACENT_VIEWS; ++j) {
            if (j != k && views[j] != NULL && views[k] != NULL && views[j] + size == views[k]) {
                after[k] = j;
                ++pairs;
            }
        }
    }
    printf("hipbench: verify   adjacent: %d hipHostMalloc views of %zu bytes after one hipMalloc"
           " view; %d start at the end of another of them",
           ADJACENT_VIEWS, size, pairs);
    for (int k = 0; k < ADJACENT_VIEWS; ++k) {
        if (after[k] >= 0) {
            printf(", view %d after view %d", k, after[k]);
        }
    }
    printf("\n");
    const bc250hipCounters before = read_counters();

    for (int k = 0; k < ADJACENT_VIEWS; ++k) {
        char name[64];
        unsigned char* view = views[k];
        snprintf(name, sizeof(name), "adjacent view %d, direct at byte 0", k);
        if (view == NULL || device == NULL) {
            verify_skip(&tally, name, "no hipHostMalloc or hipMalloc buffer");
            snprintf(name, sizeof(name), "adjacent view %d, hipMemset", k);
            verify_skip(&tally, name, "no hipHostMalloc or hipMalloc buffer");
            snprintf(name, sizeof(name), "adjacent view %d, device pointer", k);
            verify_skip(&tally, name, "no hipHostMalloc or hipMalloc buffer");
            continue;
        }

        // The device-to-host copy: an interior pointer first, as the route reference.
        memset(view, VERIFY_DIRTY, size);
        const bc250hipCounters ref0 = read_counters();
        check(hipMemcpy(view + 4096u, device, size - 4096u, hipMemcpyDeviceToHost),
              "verify: adjacent copy into byte 4096");
        const counters_delta ref = counters_since(ref0);
        memset(view, VERIFY_DIRTY, size);
        const bc250hipCounters at0 = read_counters();
        check(hipMemcpy(view, device, size - tail, hipMemcpyDeviceToHost),
              "verify: adjacent copy into byte 0");
        const counters_delta got = counters_since(at0);
        if (fault == kFaultCorruptTail) {
            view[size - 1u] ^= 0xFF;
        }
        if (got.gpu_copy_d2h != ref.gpu_copy_d2h ||
            got.gpu_copy_d2h_staged != ref.gpu_copy_d2h_staged ||
            got.cpu_copy_fallback != ref.cpu_copy_fallback) {
            tally.failed++;
            g_failures++;
            printf("hipbench: verify   %-44s WRONG route: gpu d2h %llu, staged %llu, cpu fallback"
                   " %llu; byte 4096 took %llu, %llu, %llu\n",
                   name, got.gpu_copy_d2h, got.gpu_copy_d2h_staged, got.cpu_copy_fallback,
                   ref.gpu_copy_d2h, ref.gpu_copy_d2h_staged, ref.cpu_copy_fallback);
        } else {
            const linear_case c = { 0u, 0u, size };
            verify_report(&tally, name, view, size, want_linear, &c);
        }

        // hipMemset of the whole buffer from its first byte.
        snprintf(name, sizeof(name), "adjacent view %d, hipMemset", k);
        unsigned char* before_view = after[k] >= 0 ? views[after[k]] : NULL;
        if (before_view != NULL) {
            memset(before_view, ADJACENT_KEEP, size);
        }
        memset(view, VERIFY_DIRTY, size);
        check(hipMemset(view, ADJACENT_FILL, size - tail), "verify: adjacent hipMemset");
        if (fault == kFaultCorruptTail) {
            view[size - 1u] ^= 0xFF;
        }
        const unsigned char fill = ADJACENT_FILL;
        verify_report(&tally, name, view, size, want_byte, &fill);
        if (before_view != NULL) {
            const unsigned char keep = ADJACENT_KEEP;
            // Neither fault touches this buffer, so both change its last byte here: a fault
            // control must turn every case that runs into WRONG.
            if (fault != kFaultNone) {
                before_view[size - 1u] ^= 0xFF;
            }
            snprintf(name, sizeof(name), "adjacent view %d, the view before it", after[k]);
            verify_report(&tally, name, before_view, size, want_byte, &keep);
        }

        // hipHostGetDevicePointer of the first byte, and a read through it.
        snprintf(name, sizeof(name), "adjacent view %d, device pointer", k);
        void* first = NULL;
        void* interior = NULL;
        check(hipHostGetDevicePointer(&first, view, 0), "verify: adjacent device pointer");
        check(hipHostGetDevicePointer(&interior, view + 4096u, 0),
              "verify: adjacent device pointer of byte 4096");
        if (first == NULL || (unsigned char*)interior - (unsigned char*)first != 4096) {
            tally.failed++;
            g_failures++;
            printf("hipbench: verify   %-44s WRONG: byte 0 at 0x%llx, byte 4096 at 0x%llx\n", name,
                   (unsigned long long)(uintptr_t)first, (unsigned long long)(uintptr_t)interior);
            continue;
        }
        memcpy(view, pattern, size);
        memset(readback, VERIFY_DIRTY, size);
        check(hipMemcpy(readback, first, size - tail, hipMemcpyDeviceToHost),
              "verify: adjacent read through the device pointer");
        if (fault == kFaultCorruptTail) {
            readback[size - 1u] ^= 0xFF;
        }
        const linear_case c = { 0u, 0u, size };
        verify_report(&tally, name, readback, size, want_linear, &c);
    }

    const counters_delta d = counters_since(before);
    printf("hipbench: verify   adjacent %d cases: %d passed, %d failed, %d skipped%s\n",
           tally.passed + tally.failed + tally.skipped, tally.passed, tally.failed, tally.skipped,
           fault == kFaultNone ? ""
                               : (fault == kFaultOmitTail ? " (fault control: omit-tail)"
                                                          : " (fault control: corrupt-tail)"));
    printf("hipbench: verify   adjacent routes: gpu d2h %llu, staged %llu, d2d %llu, fill %llu,"
           " cpu fallback %llu\n",
           d.gpu_copy_d2h, d.gpu_copy_d2h_staged, d.gpu_copy_d2d, d.gpu_fill, d.cpu_copy_fallback);
    if (g_json) {
        printf("{\"tool\":\"hipbench\",\"measurement\":\"verify-adjacent\",\"fault\":\"%s\","
               "\"views\":%d,\"pairs\":%d,\"cases\":%d,\"passed\":%d,\"failed\":%d,\"skipped\":%d,"
               "\"gpu_copy_d2h\":%llu,\"gpu_copy_d2h_staged\":%llu,\"gpu_copy_d2d\":%llu,"
               "\"gpu_fill\":%llu,\"cpu_copy_fallback\":%llu}\n",
               fault == kFaultNone ? "none"
                                   : (fault == kFaultOmitTail ? "omit-tail" : "corrupt-tail"),
               ADJACENT_VIEWS, pairs, tally.passed + tally.failed + tally.skipped, tally.passed,
               tally.failed, tally.skipped, d.gpu_copy_d2h, d.gpu_copy_d2h_staged, d.gpu_copy_d2d,
               d.gpu_fill, d.cpu_copy_fallback);
    }

    for (int k = ADJACENT_VIEWS - 1; k >= 0; --k) {
        if (views[k] != NULL) {
            check(hipHostFree(views[k]), "verify: adjacent hipHostFree");
        }
    }
    if (device != NULL) {
        check(hipFree(device), "verify: adjacent hipFree");
    }
    free(readback);
    free(pattern);
}

// Measurement 3: one copy size, both directions.
// The copy arms. Three destinations, because the three routes of the GPU copy are chosen by
// what the destination is (scratch/hip-gpu-copy/CONTRACT.md sections 5a and 5b):
//
//   to device        a host-processor write of the mapping, MEASURED fast, unchanged;
//   from device      into memory this program owns, which goes through a staging slot;
//   from device, own into memory hipHostMalloc returned, which the copy packet writes
//                    straight into. This is the route llama.cpp takes for its logits, and
//                    it is the one the Gemma arithmetic of CONTRACT.md section 10 is about.
//
// The copy counters are printed beside the rates, so that a line whose rate did not move can
// be told from a line that never took the GPU route at all.
static void measure_copy(size_t bytes, int iterations) {
    void* device = NULL;
    void* pinned = NULL;
    void* host = malloc(bytes);
    if (host == NULL) {
        printf("hipbench: copy: out of host memory for %zu bytes\n", bytes);
        g_failures++;
        return;
    }
    memset(host, 0x5A, bytes);
    check(hipMalloc(&device, bytes), "copy: hipMalloc");
    if (device == NULL) {
        free(host);
        return;
    }
    // A failed hipHostMalloc is not a failure of this measurement: the arm is left out and
    // the two others still run.
    if (hipHostMalloc(&pinned, bytes, hipHostMallocDefault) != hipSuccess) {
        pinned = NULL;
    }
    // One copy each way first, so that the first-touch cost of the pages is not inside the
    // measurement.
    check(hipMemcpy(device, host, bytes, hipMemcpyHostToDevice), "copy: warm up to device");
    check(hipMemcpy(host, device, bytes, hipMemcpyDeviceToHost), "copy: warm up from device");
    if (pinned != NULL) {
        check(hipMemcpy(pinned, device, bytes, hipMemcpyDeviceToHost),
              "copy: warm up from device into our own memory");
    }

    const bc250hipCounters before = read_counters();
    double t0 = now_ms();
    for (int i = 0; i < iterations; ++i) {
        check(hipMemcpy(device, host, bytes, hipMemcpyHostToDevice), "copy: to device");
    }
    const double h2d_ms = now_ms() - t0;
    t0 = now_ms();
    for (int i = 0; i < iterations; ++i) {
        check(hipMemcpy(host, device, bytes, hipMemcpyDeviceToHost), "copy: from device");
    }
    const double d2h_ms = now_ms() - t0;
    double d2h_own_ms = 0.0;
    if (pinned != NULL) {
        t0 = now_ms();
        for (int i = 0; i < iterations; ++i) {
            check(hipMemcpy(pinned, device, bytes, hipMemcpyDeviceToHost),
                  "copy: from device into our own memory");
        }
        d2h_own_ms = now_ms() - t0;
    }
    const counters_delta d = counters_since(before);

    const double mib = (double)bytes * (double)iterations / (1024.0 * 1024.0);
    const double h2d = h2d_ms > 0.0 ? mib / (h2d_ms / 1000.0) : 0.0;
    const double d2h = d2h_ms > 0.0 ? mib / (d2h_ms / 1000.0) : 0.0;
    const double d2h_own = d2h_own_ms > 0.0 ? mib / (d2h_own_ms / 1000.0) : 0.0;
    printf("hipbench: copy     %8zu bytes x %d: to device %.1f MiB/s (%.3f us each), from"
           " device %.1f MiB/s (%.3f us each), from device into our own memory %.1f MiB/s"
           " (%.3f us each)\n",
           bytes, iterations, h2d, h2d_ms * 1000.0 / iterations, d2h,
           d2h_ms * 1000.0 / iterations, d2h_own,
           iterations > 0 ? d2h_own_ms * 1000.0 / iterations : 0.0);
    printf("hipbench: copy     %8zu bytes: gpu d2h %llu, staged %llu, d2d %llu, fill %llu,"
           " cpu fallback %llu (%llu bytes), packets %llu, forced flushes %llu,"
           " control failed %llu\n",
           bytes, d.gpu_copy_d2h, d.gpu_copy_d2h_staged, d.gpu_copy_d2d, d.gpu_fill,
           d.cpu_copy_fallback, d.cpu_copy_fallback_bytes, d.gpu_copy_packets,
           d.gpu_copy_forced_flushes, d.gpu_copy_control_failed);
    {
        char name[48];
        snprintf(name, sizeof(name), "copy     %8zu bytes", bytes);
        report_time(name, &d);
    }
    if (g_json) {
        printf("{\"tool\":\"hipbench\",\"measurement\":\"copy\",\"bytes\":%zu,"
               "\"iterations\":%d,\"h2d_mib_s\":%.3f,\"d2h_mib_s\":%.3f,"
               "\"d2h_own_mib_s\":%.3f,\"h2d_us\":%.3f,"
               "\"d2h_us\":%.3f,\"d2h_own_us\":%.3f,\"waits\":%llu,\"waits_fast\":%llu,"
               "\"gpu_copy_d2h\":%llu,\"gpu_copy_d2h_staged\":%llu,\"gpu_copy_d2d\":%llu,"
               "\"gpu_fill\":%llu,\"cpu_copy_fallback\":%llu,"
               "\"cpu_copy_fallback_bytes\":%llu,\"gpu_copy_packets\":%llu,"
               "\"gpu_copy_forced_flushes\":%llu,\"gpu_copy_control_failed\":%llu",
               bytes, iterations, h2d, d2h, d2h_own, h2d_ms * 1000.0 / iterations,
               d2h_ms * 1000.0 / iterations,
               iterations > 0 ? d2h_own_ms * 1000.0 / iterations : 0.0, d.waits, d.waits_fast,
               d.gpu_copy_d2h, d.gpu_copy_d2h_staged, d.gpu_copy_d2d, d.gpu_fill,
               d.cpu_copy_fallback, d.cpu_copy_fallback_bytes, d.gpu_copy_packets,
               d.gpu_copy_forced_flushes, d.gpu_copy_control_failed);
        json_time_fields(&d);
        printf("}\n");
    }
    if (pinned != NULL) {
        check(hipHostFree(pinned), "copy: hipHostFree");
    }
    check(hipFree(device), "copy: hipFree");
    free(host);
}

// Measurement 4: the cost of a synchronisation that has nothing to wait for.
static void measure_sync(hipStream_t stream, long long calls) {
    check(hipStreamSynchronize(stream), "sync: the first synchronisation");
    const bc250hipCounters before = read_counters();
    const double t0 = now_ms();
    for (long long i = 0; i < calls; ++i) {
        check(hipStreamSynchronize(stream), "sync: hipStreamSynchronize");
    }
    const double wall = now_ms() - t0;
    const counters_delta d = counters_since(before);
    const double us_each = wall * 1000.0 / (double)calls;
    printf("hipbench: sync     %lld synchronisations of a retired stream: %.3f us each,"
           " %llu of %llu waits answered by the fence mapping\n",
           calls, us_each, d.waits_fast, d.waits);
    report_time("sync", &d);
    report_json("sync", us_each, HIPBENCH_CALL_METRIC, wall, (double)d.waits_fast, "fast waits",
                &d, calls);
}

// Measurement 6: hipMemset, with its bytes checked.
//
// Two things are wanted here. The rate answers CONTRACT.md deviation 13.3, which left the GPU
// fill off for an allocation that has a host mapping on the argument that a host-processor
// write of that mapping is MEASURED at 15.6 GB/s; with BC250_HIP_GPU_FILL=1 this arm measures
// the engine against that number instead of arguing about it.
//
// The correctness part is the unaligned fill. A fill packet needs a 4-byte aligned address and
// size, so a range that starts or ends between two dword boundaries has its one to three edge
// bytes carried by copy packets out of a staging slot and its interior by a fill packet
// (CONTRACT.md section 5c). Nothing else in this program, in vadd or in llama.cpp asks for an
// unaligned hipMemset, and no host test can reach that code, so this is where the edges are
// proved on silicon: one byte before the range and two after it must keep the frame value, and
// every byte inside it must hold the fill value.
static void measure_fill(size_t bytes, int iterations) {
    void* device = NULL;
    unsigned char* host = (unsigned char*)malloc(bytes);
    if (host == NULL) {
        printf("hipbench: fill: out of host memory for %zu bytes\n", bytes);
        g_failures++;
        return;
    }
    check(hipMalloc(&device, bytes), "fill: hipMalloc");
    if (device == NULL) {
        free(host);
        return;
    }

    // The frame, then the unaligned range inside it: three head bytes, an interior, and a
    // tail, whatever the allocation's own alignment is.
    check(hipMemset(device, 0x11, bytes), "fill: the frame");
    check(hipMemset((unsigned char*)device + 1, 0xA7, bytes - 3), "fill: an unaligned range");
    check(hipMemcpy(host, device, bytes, hipMemcpyDeviceToHost), "fill: read the frame back");
    size_t wrong = 0;
    size_t first_wrong = 0;
    for (size_t i = 0; i < bytes; ++i) {
        const unsigned char want = (i == 0 || i + 2 >= bytes) ? 0x11 : 0xA7;
        if (host[i] != want) {
            if (wrong == 0) {
                first_wrong = i;
            }
            ++wrong;
        }
    }
    if (wrong != 0) {
        printf("hipbench: fill     an unaligned hipMemset of %zu bytes left %zu bytes wrong,"
               " the first at offset %zu (0x%02X, expected 0x%02X)\n",
               bytes - 3, wrong, first_wrong, host[first_wrong],
               (first_wrong == 0 || first_wrong + 2 >= bytes) ? 0x11 : 0xA7);
        g_failures++;
    }

    const bc250hipCounters before = read_counters();
    const double t0 = now_ms();
    for (int i = 0; i < iterations; ++i) {
        check(hipMemset(device, 0x5A, bytes), "fill: aligned");
    }
    const double wall = now_ms() - t0;
    const counters_delta d = counters_since(before);
    const double mib = (double)bytes * (double)iterations / (1024.0 * 1024.0);
    const double rate = wall > 0.0 ? mib / (wall / 1000.0) : 0.0;
    printf("hipbench: fill     %8zu bytes x %d: %.1f MiB/s (%.3f us each), unaligned bytes"
           " wrong %zu\n",
           bytes, iterations, rate, wall * 1000.0 / iterations, wrong);
    printf("hipbench: fill     %8zu bytes: gpu fill %llu (%llu bytes), gpu d2h %llu, staged"
           " %llu, cpu fallback %llu (%llu bytes), packets %llu, control failed %llu\n",
           bytes, d.gpu_fill, d.gpu_fill_bytes, d.gpu_copy_d2h, d.gpu_copy_d2h_staged,
           d.cpu_copy_fallback, d.cpu_copy_fallback_bytes, d.gpu_copy_packets,
           d.gpu_copy_control_failed);
    {
        char name[48];
        snprintf(name, sizeof(name), "fill     %8zu bytes", bytes);
        report_time(name, &d);
    }
    if (g_json) {
        printf("{\"tool\":\"hipbench\",\"measurement\":\"fill\",\"bytes\":%zu,"
               "\"iterations\":%d,\"mib_s\":%.3f,\"us\":%.3f,\"unaligned_wrong\":%zu,"
               "\"gpu_fill\":%llu,\"gpu_fill_bytes\":%llu,\"cpu_copy_fallback\":%llu,"
               "\"cpu_copy_fallback_bytes\":%llu,\"gpu_copy_packets\":%llu,"
               "\"gpu_copy_control_failed\":%llu",
               bytes, iterations, rate, wall * 1000.0 / iterations, wrong, d.gpu_fill,
               d.gpu_fill_bytes, d.cpu_copy_fallback, d.cpu_copy_fallback_bytes,
               d.gpu_copy_packets, d.gpu_copy_control_failed);
        json_time_fields(&d);
        printf("}\n");
    }

    check(hipFree(device), "fill: hipFree");
    free(host);
}

// Measurement 7: an event recorded on retired work, and the wait for it.
static void measure_event(hipStream_t stream, long long rounds) {
    hipEvent_t mark = NULL;
    check(hipEventCreate(&mark), "event: hipEventCreate");
    check(hipStreamSynchronize(stream), "event: the first synchronisation");
    const bc250hipCounters before = read_counters();
    const double t0 = now_ms();
    for (long long i = 0; i < rounds; ++i) {
        check(hipEventRecord(mark, stream), "event: hipEventRecord");
        check(hipEventSynchronize(mark), "event: hipEventSynchronize");
    }
    const double wall = now_ms() - t0;
    const counters_delta d = counters_since(before);
    const double us_each = wall * 1000.0 / (double)rounds;
    printf("hipbench: event    %lld record and wait rounds on retired work: %.3f us each\n",
           rounds, us_each);
    report_time("event", &d);
    report_json("event", us_each, HIPBENCH_CALL_METRIC, wall, 0.0, "none", &d, rounds);
    check(hipEventDestroy(mark), "event: hipEventDestroy");
}

static void usage() {
    printf("hipbench: usage: hipbench.exe [--launches <n>] [--chain <n>] [--sync <n>]\n");
    printf("                             [--event <n>] [--copy-iterations <n>]\n");
    printf("                             [--batch 0|1] [--batch-max <n>] [--batch-hold-us <us>]\n");
    printf("                             [--barrier full|light] [--wait-total <ms>]\n");
    printf("                             [--budget-ms <ms>] [--expect-compute] [--json]\n");
    printf("                             [--verify 0|1] [--verify-fault omit-tail|corrupt-tail]\n");
    printf("                             [--verify-only]\n");
}

int main(int argc, char** argv) {
    long long launches = 2000;
    long long chain = 1000;
    long long syncs = 2000;
    long long events = 500;
    int copy_iterations = 8;
    const char* batch = NULL;
    const char* barrier = NULL;
    int verify = 1;
    int verify_only = 0;
    int fault = kFaultNone;

    g_started_ms = now_ms();
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const int has_value = i + 1 < argc;
        if (strcmp(arg, "--expect-compute") == 0) {
            g_expect_compute = 1;
        } else if (strcmp(arg, "--json") == 0) {
            g_json = 1;
        } else if (strcmp(arg, "--verify-only") == 0) {
            verify_only = 1;
        } else if (strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else if (!has_value) {
            printf("hipbench: '%s' needs a value\n", arg);
            usage();
            return 2;
        } else if (strcmp(arg, "--verify") == 0) {
            verify = atoi(argv[++i]) != 0;
        } else if (strcmp(arg, "--verify-fault") == 0) {
            const char* kind = argv[++i];
            if (strcmp(kind, "omit-tail") == 0) {
                fault = kFaultOmitTail;
            } else if (strcmp(kind, "corrupt-tail") == 0) {
                fault = kFaultCorruptTail;
            } else {
                printf("hipbench: --verify-fault takes 'omit-tail' or 'corrupt-tail'\n");
                return 2;
            }
        } else if (strcmp(arg, "--launches") == 0) {
            launches = atoll(argv[++i]);
        } else if (strcmp(arg, "--chain") == 0) {
            chain = atoll(argv[++i]);
        } else if (strcmp(arg, "--sync") == 0) {
            syncs = atoll(argv[++i]);
        } else if (strcmp(arg, "--event") == 0) {
            events = atoll(argv[++i]);
        } else if (strcmp(arg, "--copy-iterations") == 0) {
            copy_iterations = atoi(argv[++i]);
        } else if (strcmp(arg, "--budget-ms") == 0) {
            g_budget_ms = atof(argv[++i]);
        } else if (strcmp(arg, "--wait-total") == 0) {
            if (!set_env("BC250_HIP_WAIT_TOTAL_MS", argv[++i])) {
                printf("hipbench: cannot set the wait bound\n");
                return 2;
            }
        } else if (strcmp(arg, "--batch") == 0) {
            batch = argv[++i];
            if (!set_env("BC250_HIP_BATCH", batch)) {
                printf("hipbench: cannot set the batching switch\n");
                return 2;
            }
        } else if (strcmp(arg, "--batch-max") == 0) {
            if (!set_env("BC250_HIP_BATCH_MAX", argv[++i])) {
                printf("hipbench: cannot set the batch size cap\n");
                return 2;
            }
        } else if (strcmp(arg, "--batch-hold-us") == 0) {
            if (!set_env("BC250_HIP_BATCH_HOLD_US", argv[++i])) {
                printf("hipbench: cannot set the batch time cap\n");
                return 2;
            }
        } else if (strcmp(arg, "--barrier") == 0) {
            barrier = argv[++i];
            if (strcmp(barrier, "full") != 0 && strcmp(barrier, "light") != 0) {
                printf("hipbench: --barrier takes 'full' or 'light'\n");
                return 2;
            }
            if (!set_env("BC250_HIP_BARRIER", barrier)) {
                printf("hipbench: cannot set the barrier switch\n");
                return 2;
            }
        } else {
            printf("hipbench: unknown argument '%s'\n", arg);
            usage();
            return 2;
        }
    }
    if (launches < 1 || chain < 1 || syncs < 1 || events < 1 || copy_iterations < 1) {
        printf("hipbench: every count must be one or more\n");
        return 2;
    }

    printf("hipbench: batch %s, barrier %s, budget %.0f ms, results %s\n",
           batch != NULL ? batch : "1 (default)", barrier != NULL ? barrier : "light (default)",
           g_budget_ms, g_expect_compute ? "checked" : "not checked");

    check(hipInit(0), "hipInit");
    int count = 0;
    check(hipGetDeviceCount(&count), "hipGetDeviceCount");
    hipDeviceProp_t prop;
    memset(&prop, 0, sizeof(prop));
    check(hipGetDeviceProperties(&prop, 0), "hipGetDeviceProperties");
    printf("hipbench: %d device(s), '%s' arch '%s', %d multiprocessors at %d kHz\n", count,
           prop.name, prop.gcnArchName, prop.multiProcessorCount, prop.clockRate);
    if (g_failures != 0) {
        printf("hipbench: the device did not open; nothing was measured\n");
        return 1;
    }

    hipStream_t stream = NULL;
    int* values = NULL;
    check(hipStreamCreate(&stream), "hipStreamCreate");
    check(hipMalloc((void**)&values, sizeof(int) * CHAIN_ITEMS), "hipMalloc chain");
    // The first launch of the process loads the code object, so it is not part of any
    // measurement. The route document asks for its cost, and this is where it is paid.
    {
        const double t0 = now_ms();
        nothing<<<1, 64, 0, stream>>>();
        check(hipGetLastError(), "the first launch");
        check(hipStreamSynchronize(stream), "the first synchronisation");
        printf("hipbench: the first launch and its synchronisation took %.3f ms (the code object"
               " load is inside it)\n",
               now_ms() - t0);
    }

    // The fault control checks the oracle and measures nothing else: a rate taken with a fault
    // injected would be a rate of a copy this program broke on purpose.
    if (fault != kFaultNone) {
        verify_copies(fault);
        verify_adjacent(fault);
        check(hipFree(values), "hipFree chain");
        check(hipStreamDestroy(stream), "hipStreamDestroy");
        printf("hipbench: the fault control ran alone; %d failures, which must not be 0\n",
               g_failures);
        printf("hipbench: %s\n", g_failures == 0 ? "ok" : "FAILED");
        return g_failures == 0 ? 0 : 1;
    }
    if (verify_only) {
        // Measurements 3a and 3b and nothing timed: the copy routes alone, for a build gate or
        // a lab control that needs the routes and not the rates.
        verify_copies(kFaultNone);
        verify_adjacent(kFaultNone);
        check(hipFree(values), "hipFree chain");
        check(hipStreamDestroy(stream), "hipStreamDestroy");
        printf("hipbench: %.0f ms in all, api failures %d\n", now_ms() - g_started_ms, g_failures);
        printf("hipbench: %s\n", g_failures == 0 ? "ok" : "FAILED");
        return g_failures == 0 ? 0 : 1;
    }
    if (verify && budget_left("verify")) {
        verify_copies(kFaultNone);
        verify_adjacent(kFaultNone);
    }
    if (budget_left("launch")) {
        measure_launch(stream, launches);
    }
    if (budget_left("chain")) {
        measure_chain(stream, values, chain);
    }
    if (budget_left("copy")) {
        measure_copy(4096u, copy_iterations * 32);
        // 64 KB is the third size the copy question of scratch/hip-gpu-copy/CONTRACT.md
        // section 11 asks for: it is above the 4096-byte threshold and small enough that the
        // fixed cost of one submission is still a visible part of the time.
        measure_copy(65536u, copy_iterations * 8);
        measure_copy(1024u * 1024u, copy_iterations);
        measure_copy(64u * 1024u * 1024u, copy_iterations);
    }
    if (budget_left("fill")) {
        measure_fill(1024u * 1024u, copy_iterations);
    }
    if (budget_left("sync")) {
        measure_sync(stream, syncs);
    }
    if (budget_left("event")) {
        measure_event(stream, events);
    }

    check(hipFree(values), "hipFree chain");
    check(hipStreamDestroy(stream), "hipStreamDestroy");
    printf("hipbench: %.0f ms in all, api failures %d\n", now_ms() - g_started_ms, g_failures);
    printf("hipbench: %s\n", g_failures == 0 ? "ok" : "FAILED");
    return g_failures == 0 ? 0 : 1;
}
