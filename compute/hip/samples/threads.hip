// threads.hip - a multithreaded HIP program, built with clang against our own hip_runtime.h and
// our own import library amdhip64.lib.
//
// The owner's instruction of 2026-09-29: multithreading is tested with our own clients before a
// real application is asked to exercise it. The measurement lives in
// compute/hip/tests/host/hip_threads_client.h, which the host test test_hip_threads shares with
// this program: one thread waits for the device while the others launch kernels and allocate
// memory, and the client counts the work the others finish inside that wait.
//
// Two places run it:
//   * the development PC, against the mock build of amdhip64.dll. BC250_HIP_MOCK_HOLD_MS makes
//     each dispatch retire after a measured delay, so that the wait of thread 0 is a real wait.
//     build-runtime.ps1 does this.
//   * the lab, against the product amdhip64.dll over bc250hsa, where the delay is the kernel
//     itself. --spin sets how long the kernel runs (step 3 of the design, section 6).
//
// Exit code 0 asks of every HIP call that it answered hipSuccess, and of every verdict of the
// client that it held.
//
// --wait-total <ms> shortens every wait of the runtime, as vadd.hip does: the runtime reads the
// bound one time, at its first call, from BC250_HIP_WAIT_TOTAL_MS. A lab trial stays inside its
// bound this way.

#include "hip/hip_runtime.h"

// The device pass compiles this file too, and the host part of this program uses the C++ thread
// library. Those headers are not for a GPU target, so they are out of the device pass.
#if !defined(__HIP_DEVICE_COMPILE__)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hip_threads_client.h"
#endif

#define BLOCK 64

// The kernel of the measurement. The loop is what makes a wait on the real device long enough to
// measure: a dispatch of a few microseconds would be over before another thread looked.
__global__ void __launch_bounds__(BLOCK) spin(float* out, int n, int iterations) {
    int   i = blockIdx.x * blockDim.x + threadIdx.x;
    float value = 1.0f;
    for (int k = 0; k < iterations; ++k) {
        value = value * 1.0000001f + 1.0f;
    }
    if (i < n) {
        out[i] = value;
    }
}

#if !defined(__HIP_DEVICE_COMPILE__)

static int g_spin_iterations = 20000;
static int g_grid = 64;

// One launch, from any thread. Every argument is on this thread's stack.
static hipError_t launch_spin(hipStream_t stream, void* buffer, void* ctx) {
    (void)ctx;
    float* out = (float*)buffer;
    int    n = g_grid * BLOCK;
    spin<<<g_grid, BLOCK, 0, stream>>>(out, n, g_spin_iterations);
    return hipGetLastError();
}

static int set_env_ms(const char* name, const char* ms) {
    if (ms == NULL || ms[0] == '\0') {
        return 0;
    }
#if defined(_WIN32)
    return _putenv_s(name, ms) == 0;
#else
    return setenv(name, ms, 1) == 0;
#endif
}

int main(int argc, char** argv) {
    int i;
    int threads = 4;
    int iterations = 4;
    int overlaps = 0;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--wait-total") == 0 && i + 1 < argc) {
            ++i;
            if (!set_env_ms("BC250_HIP_WAIT_TOTAL_MS", argv[i])) {
                printf("hipthreads: cannot set the wait bound to '%s'\n", argv[i]);
                return 2;
            }
            printf("hipthreads: every wait of the runtime is bounded to %s ms in all\n", argv[i]);
        } else if (strcmp(argv[i], "--mock-hold") == 0 && i + 1 < argc) {
            ++i;
            if (!set_env_ms("BC250_HIP_MOCK_HOLD_MS", argv[i])) {
                printf("hipthreads: cannot set the mock hold time to '%s'\n", argv[i]);
                return 2;
            }
            printf("hipthreads: a mock backend holds every dispatch for %s ms\n", argv[i]);
        } else if (strcmp(argv[i], "--spin") == 0 && i + 1 < argc) {
            ++i;
            g_spin_iterations = (int)strtol(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--grid") == 0 && i + 1 < argc) {
            ++i;
            g_grid = (int)strtol(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            ++i;
            threads = (int)strtol(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--iterations") == 0 && i + 1 < argc) {
            ++i;
            iterations = (int)strtol(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "--device-overlaps") == 0) {
            /* The backend behind this runtime can have several dispatches in flight at once,
             * so phase B may beat the serialised time. The mock backend can; the BC-250
             * cannot, because the ring runs one indirect buffer at a time (bc250hsa.h rule 5),
             * and the default is therefore off (defect BD-111). */
            overlaps = 1;
        } else {
            printf("hipthreads: unknown argument '%s'\n", argv[i]);
            printf("hipthreads: usage: hipthreads.exe [--wait-total <ms>] [--mock-hold <ms>] "
                   "[--spin <n>] [--grid <n>] [--threads <n>] [--iterations <n>] "
                   "[--device-overlaps]\n");
            return 2;
        }
    }
    if (g_spin_iterations < 1) {
        g_spin_iterations = 1;
    }
    if (g_grid < 1) {
        g_grid = 1;
    }

    {
        hipDeviceProp_t prop;
        int             count = 0;
        hipError_t      err = hipInit(0);
        if (err != hipSuccess) {
            printf("hipthreads: hipInit failed: %s\n", hipGetErrorName(err));
            return 1;
        }
        err = hipGetDeviceCount(&count);
        if (err != hipSuccess || count < 1) {
            printf("hipthreads: no device: %s\n", hipGetErrorName(err));
            return 1;
        }
        memset(&prop, 0, sizeof(prop));
        (void)hipGetDeviceProperties(&prop, 0);
        printf("hipthreads: device '%s' arch '%s', %d threads, %d iterations, spin %d, grid %d\n",
               prop.name, prop.gcnArchName, threads, iterations, g_spin_iterations, g_grid);
    }

    {
        bc250hipthreads::Options options =
            bc250hipthreads::default_options(launch_spin, NULL);
        bc250hipthreads::Result result;
        int                    failures;
        options.threads = threads;
        options.iterations = iterations;
        options.device_overlaps_dispatches = overlaps;
        options.buffer_bytes = (size_t)g_grid * BLOCK * sizeof(float);
        failures = bc250hipthreads::run(options, &result);
        printf("hipthreads: %s\n", failures == 0 ? "ok" : "failed");
        return failures == 0 ? 0 : 1;
    }
}

#endif  // !__HIP_DEVICE_COMPILE__
