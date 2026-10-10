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
//   3. copy     hipMemcpy host to device and device to host at 4 KB, 1 MB and 64 MB. Build 1
//               copies through the host mapping, so this is a memory bandwidth and not a copy
//               engine.
//   4. sync     hipStreamSynchronize on a stream whose work has already retired. It should read
//               the fence mapping and return, and the counters say whether it did (waits_fast).
//   5. event    hipEventRecord and hipEventSynchronize on retired work, the same way.
//
// Each measurement prints one line of plain text and, with --json, one JSON line as well, which
// is what the lab record keeps.
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
    return d;
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
           "\"dispatches_batched\":%llu,\"waits\":%llu,\"waits_fast\":%llu}\n",
           name, count, us_each, metric, wall_ms, rate, rate_unit, d->dispatches, d->submissions,
           d->batches, d->dispatches_batched, d->waits, d->waits_fast);
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
    report_json("chain", us_each, HIPBENCH_ENQUEUE_METRIC, t2 - t0,
                d.dispatches != 0 ? (double)d.submissions / (double)d.dispatches : 0.0,
                "submissions/launch", &d, links);
}

// Measurement 3: one copy size, both directions.
static void measure_copy(size_t bytes, int iterations) {
    void* device = NULL;
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
    // One copy each way first, so that the first-touch cost of the pages is not inside the
    // measurement.
    check(hipMemcpy(device, host, bytes, hipMemcpyHostToDevice), "copy: warm up to device");
    check(hipMemcpy(host, device, bytes, hipMemcpyDeviceToHost), "copy: warm up from device");

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
    const counters_delta d = counters_since(before);

    const double mib = (double)bytes * (double)iterations / (1024.0 * 1024.0);
    const double h2d = h2d_ms > 0.0 ? mib / (h2d_ms / 1000.0) : 0.0;
    const double d2h = d2h_ms > 0.0 ? mib / (d2h_ms / 1000.0) : 0.0;
    printf("hipbench: copy     %8zu bytes x %d: to device %.1f MiB/s (%.3f us each), from"
           " device %.1f MiB/s (%.3f us each)\n",
           bytes, iterations, h2d, h2d_ms * 1000.0 / iterations, d2h,
           d2h_ms * 1000.0 / iterations);
    if (g_json) {
        printf("{\"tool\":\"hipbench\",\"measurement\":\"copy\",\"bytes\":%zu,"
               "\"iterations\":%d,\"h2d_mib_s\":%.3f,\"d2h_mib_s\":%.3f,\"h2d_us\":%.3f,"
               "\"d2h_us\":%.3f,\"waits\":%llu,\"waits_fast\":%llu}\n",
               bytes, iterations, h2d, d2h, h2d_ms * 1000.0 / iterations,
               d2h_ms * 1000.0 / iterations, d.waits, d.waits_fast);
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
    report_json("sync", us_each, HIPBENCH_CALL_METRIC, wall, (double)d.waits_fast, "fast waits",
                &d, calls);
}

// Measurement 5: an event recorded on retired work, and the wait for it.
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
    report_json("event", us_each, HIPBENCH_CALL_METRIC, wall, 0.0, "none", &d, rounds);
    check(hipEventDestroy(mark), "event: hipEventDestroy");
}

static void usage() {
    printf("hipbench: usage: hipbench.exe [--launches <n>] [--chain <n>] [--sync <n>]\n");
    printf("                             [--event <n>] [--copy-iterations <n>]\n");
    printf("                             [--batch 0|1] [--batch-max <n>] [--batch-hold-us <us>]\n");
    printf("                             [--barrier full|light] [--wait-total <ms>]\n");
    printf("                             [--budget-ms <ms>] [--expect-compute] [--json]\n");
}

int main(int argc, char** argv) {
    long long launches = 2000;
    long long chain = 1000;
    long long syncs = 2000;
    long long events = 500;
    int copy_iterations = 8;
    const char* batch = NULL;
    const char* barrier = NULL;

    g_started_ms = now_ms();
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const int has_value = i + 1 < argc;
        if (strcmp(arg, "--expect-compute") == 0) {
            g_expect_compute = 1;
        } else if (strcmp(arg, "--json") == 0) {
            g_json = 1;
        } else if (strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        } else if (!has_value) {
            printf("hipbench: '%s' needs a value\n", arg);
            usage();
            return 2;
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

    if (budget_left("launch")) {
        measure_launch(stream, launches);
    }
    if (budget_left("chain")) {
        measure_chain(stream, values, chain);
    }
    if (budget_left("copy")) {
        measure_copy(4096u, copy_iterations * 32);
        measure_copy(1024u * 1024u, copy_iterations);
        measure_copy(64u * 1024u * 1024u, copy_iterations);
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
