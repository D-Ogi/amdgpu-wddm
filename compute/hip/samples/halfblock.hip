// halfblock.hip - the two device-header contracts that no earlier sample covered: the directed
// float-to-half conversions of hip/hip_fp16.h and the block collectives of hip/hip_runtime.h.
//
// Why it exists. The independent audit of 2026-10-10 (findings HS-1 and HS-2) read both sets
// out of the headers: `__float2half_rz`, `_rd` and `_ru` all returned the nearest value, and
// `__syncthreads_count`, `_or` and `_and` reduced over one wave of 32 instead of the whole
// workgroup. Both are answered by arithmetic and by a 64-thread block, so both get a test.
//
// Build: compute/hip/build-runtime.ps1, exactly as samples/vadd.hip is built.
//
// Two halves, and they are separate on purpose:
//   * the host half runs the conversion table on the processor and needs no device at all. The
//     build on the development PC runs it, so a regression in the integer conversion is caught
//     offline.
//   * the device half runs the same table and the block collectives on the GPU. Its results are
//     only checked with --expect-compute, because the mock backend of the offline build records
//     a dispatch and runs no instruction. A lab arm passes that argument.
//
// Exit code 0 means every HIP call answered hipSuccess and every check that ran passed.
//
//   halfblock.exe                                  the host table only (offline)
//   halfblock.exe --expect-compute --wait-total 20000    the lab arm
//
// The expected bit patterns are not computed by the header's own arithmetic. They come from an
// independent oracle that enumerates all 65536 binary16 values and compares each with the exact
// rational value of the float input (exhaustive search, Fraction arithmetic). The sign of a zero
// result follows IEEE 754-2019 section 6.3: the sign of the result is the sign of the operand,
// so -1e-10f toward zero is -0 and not +0.

#include "hip/hip_runtime.h"

#include "hip/hip_fp16.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// The conversion table
// ---------------------------------------------------------------------------

typedef struct conv_case {
    float          x;
    unsigned short rn;
    unsigned short rz;
    unsigned short rd;
    unsigned short ru;
    const char*    name;
} conv_case;

// Exact, halfway (both parities), overflow, the largest finite value, subnormal, the smallest
// subnormal and half of it, a value under every subnormal, and the negative of each shape.
static const conv_case g_cases[] = {
    {1.0f, 0x3c00u, 0x3c00u, 0x3c00u, 0x3c00u, "1.0 exact"},
    {0.5f, 0x3800u, 0x3800u, 0x3800u, 0x3800u, "0.5 exact"},
    {1.0001f, 0x3c00u, 0x3c00u, 0x3c00u, 0x3c01u, "1.0001 (ru alone goes up)"},
    {1.00075f, 0x3c01u, 0x3c00u, 0x3c00u, 0x3c01u, "1.00075 (rn and ru go up)"},
    {-1.0001f, 0xbc00u, 0xbc00u, 0xbc01u, 0xbc00u, "-1.0001 (rd alone grows)"},
    {-1.00075f, 0xbc01u, 0xbc00u, 0xbc01u, 0xbc00u, "-1.00075"},
    {1.0f + 0x1p-11f, 0x3c00u, 0x3c00u, 0x3c00u, 0x3c01u, "halfway, ties to even down"},
    {1.0f + 0x3p-11f, 0x3c02u, 0x3c01u, 0x3c01u, 0x3c02u, "halfway, ties to even up"},
    {-(1.0f + 0x1p-11f), 0xbc00u, 0xbc00u, 0xbc01u, 0xbc00u, "halfway negative"},
    {65504.0f, 0x7bffu, 0x7bffu, 0x7bffu, 0x7bffu, "the largest finite value"},
    {65510.0f, 0x7bffu, 0x7bffu, 0x7bffu, 0x7c00u, "over the largest, under the halfway"},
    {65520.0f, 0x7c00u, 0x7bffu, 0x7bffu, 0x7c00u, "the overflow halfway point"},
    {100000.0f, 0x7c00u, 0x7bffu, 0x7bffu, 0x7c00u, "overflow"},
    {-65510.0f, 0xfbffu, 0xfbffu, 0xfc00u, 0xfbffu, "negative, over the largest finite"},
    {-100000.0f, 0xfc00u, 0xfbffu, 0xfc00u, 0xfbffu, "negative overflow"},
    {0x1p-24f, 0x0001u, 0x0001u, 0x0001u, 0x0001u, "the smallest subnormal, exact"},
    {0x3p-25f, 0x0002u, 0x0001u, 0x0001u, 0x0002u, "1.5 of the smallest subnormal"},
    {0x1p-25f, 0x0000u, 0x0000u, 0x0000u, 0x0001u, "half of the smallest subnormal"},
    {-0x1p-25f, 0x8000u, 0x8000u, 0x8001u, 0x8000u, "the same, negative (-0 keeps its sign)"},
    {1e-10f, 0x0000u, 0x0000u, 0x0000u, 0x0001u, "under every subnormal"},
    {-1e-10f, 0x8000u, 0x8000u, 0x8001u, 0x8000u, "under every subnormal, negative"},
    {0x1p-14f, 0x0400u, 0x0400u, 0x0400u, 0x0400u, "the smallest normal, exact"},
    {0x1.004p-14f, 0x0401u, 0x0401u, 0x0401u, 0x0401u, "the next normal, exact"},
    {0.0f, 0x0000u, 0x0000u, 0x0000u, 0x0000u, "zero"},
};

#define CASE_COUNT ((int)(sizeof(g_cases) / sizeof(g_cases[0])))
#define MODE_COUNT 4

// The block sizes the collectives are checked at: one wave, two waves and eight waves of the
// wave32 mode this part runs in. One wave is the shape the old wave-only code happened to be
// right for; the other two are the shapes it was wrong for.
static const int g_block_sizes[] = {32, 64, 256};
#define BLOCK_SIZE_COUNT ((int)(sizeof(g_block_sizes) / sizeof(g_block_sizes[0])))
#define BLOCK_SLOTS 7

static int g_failures = 0;
static int g_host_checks = 0;
static int g_device_checks = 0;
static int g_expect_compute = 0;

static void check(hipError_t err, const char* what) {
    if (err != hipSuccess) {
        printf("halfblock: %s failed: %s (%s)\n", what, hipGetErrorName(err),
               hipGetErrorString(err));
        g_failures++;
    }
}

static const char* mode_name(int mode) {
    switch (mode) {
        case 0: return "rn";
        case 1: return "rz";
        case 2: return "rd";
        default: return "ru";
    }
}

static unsigned short expected_bits(const conv_case* c, int mode) {
    switch (mode) {
        case 0: return c->rn;
        case 1: return c->rz;
        case 2: return c->rd;
        default: return c->ru;
    }
}

// ---------------------------------------------------------------------------
// The device side
// ---------------------------------------------------------------------------

__global__ void convert_cases(const float* in, unsigned short* out, int n) {
    const int i = (int)(blockIdx.x * blockDim.x + threadIdx.x);
    if (i < n) {
        const float x = in[i];
        out[i * MODE_COUNT + 0] = __half_as_ushort(__float2half_rn(x));
        out[i * MODE_COUNT + 1] = __half_as_ushort(__float2half_rz(x));
        out[i * MODE_COUNT + 2] = __half_as_ushort(__float2half_rd(x));
        out[i * MODE_COUNT + 3] = __half_as_ushort(__float2half_ru(x));
    }
}

// The decisive predicate is in the last thread of the block, which for every size above 32 is
// in a wave other than the first one. A wave-only reduction answers 0 for it.
__global__ void block_collectives(int* out) {
    const int tid = (int)threadIdx.x;
    const int threads = (int)blockDim.x;
    const int last = (tid == threads - 1) ? 1 : 0;
    const int count_all = __syncthreads_count(1);
    const int count_none = __syncthreads_count(0);
    const int count_last = __syncthreads_count(last);
    const int or_last = __syncthreads_or(last) ? 1 : 0;
    const int or_none = __syncthreads_or(0) ? 1 : 0;
    const int and_all = __syncthreads_and(1) ? 1 : 0;
    const int and_last = __syncthreads_and(last) ? 1 : 0;
    if (tid == 0) {
        out[0] = count_all;
        out[1] = count_none;
        out[2] = count_last;
        out[3] = or_last;
        out[4] = or_none;
        out[5] = and_all;
        out[6] = and_last;
    }
}

// ---------------------------------------------------------------------------
// The host half: the same table, on the processor
// ---------------------------------------------------------------------------

static unsigned short host_convert(float x, int mode) {
    switch (mode) {
        case 0: return __half_as_ushort(__float2half_rn(x));
        case 1: return __half_as_ushort(__float2half_rz(x));
        case 2: return __half_as_ushort(__float2half_rd(x));
        default: return __half_as_ushort(__float2half_ru(x));
    }
}

static void run_host_table(void) {
    int i;
    int mode;
    for (i = 0; i < CASE_COUNT; ++i) {
        for (mode = 0; mode < MODE_COUNT; ++mode) {
            const unsigned short want = expected_bits(&g_cases[i], mode);
            const unsigned short got = host_convert(g_cases[i].x, mode);
            g_host_checks++;
            if (got != want) {
                printf("halfblock: host __float2half_%s(%s) is 0x%04x, expected 0x%04x\n",
                       mode_name(mode), g_cases[i].name, (unsigned)got, (unsigned)want);
                g_failures++;
            }
        }
    }
    // Infinity and NaN carry no rounding mode: every mode gives the same answer.
    for (mode = 0; mode < MODE_COUNT; ++mode) {
        const unsigned short inf = host_convert(__builtin_inff(), mode);
        const unsigned short ninf = host_convert(-__builtin_inff(), mode);
        const unsigned short nan = host_convert(__builtin_nanf(""), mode);
        g_host_checks += 3;
        if (inf != 0x7c00u || ninf != 0xfc00u || (nan & 0x7c00u) != 0x7c00u ||
            (nan & 0x03ffu) == 0u) {
            printf("halfblock: host mode %s gives inf 0x%04x, -inf 0x%04x, nan 0x%04x\n",
                   mode_name(mode), (unsigned)inf, (unsigned)ninf, (unsigned)nan);
            g_failures++;
        }
    }
    printf("halfblock: host checks %d, failed %d\n", g_host_checks, g_failures);
}

// ---------------------------------------------------------------------------
// The device half
// ---------------------------------------------------------------------------

static void run_device_table(void) {
    float*          inputs = NULL;
    unsigned short* results = NULL;
    float*          host_in = (float*)malloc(sizeof(float) * (size_t)CASE_COUNT);
    unsigned short* host_out =
        (unsigned short*)malloc(sizeof(unsigned short) * (size_t)CASE_COUNT * MODE_COUNT);
    int i;
    int mode;

    if (host_in == NULL || host_out == NULL) {
        printf("halfblock: out of host memory\n");
        g_failures++;
        free(host_in);
        free(host_out);
        return;
    }
    for (i = 0; i < CASE_COUNT; ++i) {
        host_in[i] = g_cases[i].x;
    }
    memset(host_out, 0, sizeof(unsigned short) * (size_t)CASE_COUNT * MODE_COUNT);

    check(hipMalloc((void**)&inputs, sizeof(float) * (size_t)CASE_COUNT), "hipMalloc inputs");
    check(hipMalloc((void**)&results,
                    sizeof(unsigned short) * (size_t)CASE_COUNT * MODE_COUNT),
          "hipMalloc results");
    check(hipMemcpy(inputs, host_in, sizeof(float) * (size_t)CASE_COUNT, hipMemcpyHostToDevice),
          "copy inputs");
    check(hipMemset(results, 0, sizeof(unsigned short) * (size_t)CASE_COUNT * MODE_COUNT),
          "clear results");
    (void)hipGetLastError();
    convert_cases<<<1, 64, 0, 0>>>(inputs, results, CASE_COUNT);
    check(hipGetLastError(), "convert_cases launch");
    check(hipDeviceSynchronize(), "convert_cases synchronise");
    check(hipMemcpy(host_out, results,
                    sizeof(unsigned short) * (size_t)CASE_COUNT * MODE_COUNT,
                    hipMemcpyDeviceToHost),
          "copy results");

    if (g_expect_compute) {
        for (i = 0; i < CASE_COUNT; ++i) {
            for (mode = 0; mode < MODE_COUNT; ++mode) {
                const unsigned short want = expected_bits(&g_cases[i], mode);
                const unsigned short got = host_out[i * MODE_COUNT + mode];
                g_device_checks++;
                if (got != want) {
                    printf("halfblock: device __float2half_%s(%s) is 0x%04x, expected 0x%04x\n",
                           mode_name(mode), g_cases[i].name, (unsigned)got, (unsigned)want);
                    g_failures++;
                }
            }
        }
    }

    check(hipFree(results), "hipFree results");
    check(hipFree(inputs), "hipFree inputs");
    free(host_out);
    free(host_in);
}

static void run_block_collectives(void) {
    int* device = NULL;
    int  host[BLOCK_SLOTS];
    int  size_index;

    check(hipMalloc((void**)&device, sizeof(int) * BLOCK_SLOTS), "hipMalloc block slots");
    for (size_index = 0; size_index < BLOCK_SIZE_COUNT; ++size_index) {
        const int threads = g_block_sizes[size_index];
        int       want[BLOCK_SLOTS];
        int       slot;
        memset(host, 0, sizeof(host));
        check(hipMemset(device, 0, sizeof(int) * BLOCK_SLOTS), "clear block slots");
        (void)hipGetLastError();
        block_collectives<<<1, threads, 0, 0>>>(device);
        check(hipGetLastError(), "block_collectives launch");
        check(hipDeviceSynchronize(), "block_collectives synchronise");
        check(hipMemcpy(host, device, sizeof(int) * BLOCK_SLOTS, hipMemcpyDeviceToHost),
              "copy block slots");
        want[0] = threads;             // count over every thread of the block
        want[1] = 0;                   // count of a predicate no thread holds
        want[2] = 1;                   // the last thread alone
        want[3] = 1;                   // or over the last thread alone
        want[4] = 0;                   // or over nothing
        want[5] = 1;                   // and over every thread
        want[6] = (threads == 1) ? 1 : 0;  // and over the last thread alone
        printf("halfblock: block %d collectives count_all %d count_none %d count_last %d"
               " or_last %d or_none %d and_all %d and_last %d\n",
               threads, host[0], host[1], host[2], host[3], host[4], host[5], host[6]);
        if (g_expect_compute) {
            for (slot = 0; slot < BLOCK_SLOTS; ++slot) {
                g_device_checks++;
                if (host[slot] != want[slot]) {
                    printf("halfblock: block %d slot %d is %d, expected %d\n", threads, slot,
                           host[slot], want[slot]);
                    g_failures++;
                }
            }
        }
    }
    check(hipFree(device), "hipFree block slots");
}

static int set_wait_total(const char* ms) {
    if (ms == NULL || ms[0] == '\0') {
        return 0;
    }
#if defined(_WIN32)
    return _putenv_s("BC250_HIP_WAIT_TOTAL_MS", ms) == 0;
#else
    return setenv("BC250_HIP_WAIT_TOTAL_MS", ms, 1) == 0;
#endif
}

int main(int argc, char** argv) {
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--expect-compute") == 0) {
            g_expect_compute = 1;
        } else if (strcmp(argv[i], "--wait-total") == 0 && i + 1 < argc) {
            ++i;
            if (!set_wait_total(argv[i])) {
                printf("halfblock: cannot set the wait bound to '%s'\n", argv[i]);
                return 2;
            }
            printf("halfblock: every wait of the runtime is bounded to %s ms in all\n", argv[i]);
        } else {
            printf("halfblock: unknown argument '%s'\n", argv[i]);
            printf("halfblock: usage: halfblock.exe [--expect-compute] [--wait-total <ms>]\n");
            return 2;
        }
    }

    // The host half first: it needs no device, so it runs even where there is none.
    run_host_table();

    printf("halfblock: device results are %s\n",
           g_expect_compute ? "checked" : "not checked (no --expect-compute)");
    check(hipInit(0), "hipInit");
    run_device_table();
    run_block_collectives();
    check(hipDeviceSynchronize(), "hipDeviceSynchronize");

    printf("halfblock: host checks %d, device checks %d, failures %d\n", g_host_checks,
           g_device_checks, g_failures);
    printf("halfblock: %s\n", g_failures == 0 ? "ok" : "failed");
    return g_failures == 0 ? 0 : 1;
}
