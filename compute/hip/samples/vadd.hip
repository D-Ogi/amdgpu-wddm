// vadd.hip - a normal HIP program, built with clang against our own hip_runtime.h and our own
// import library amdhip64.lib. It is the end-to-end test of layer 2: the fat binary of this
// program registers itself with our DLL before main() runs, and the kernel starts through
// hipLaunchKernel.
//
// Build: compute/hip/build-runtime.ps1 (clang -x hip --offload-arch=gfx1013
//        --target=x86_64-pc-windows-msvc -nogpuinc -nogpulib -I<include> -L<lib>).
//
// It runs the same work twice, so that a second run in one process can be compared with the
// first: the allocation count of the runtime must not grow.
//
// Exit code 0 asks of every HIP call that it answered hipSuccess. The sums are checked only
// with the argument --expect-compute, because the mock backend of the host tests records a
// dispatch and runs no instruction. On the real device the lab runner passes that argument
// (the step-2 section of the lab README).

#include "hip/hip_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define N 65536
#define BLOCK 256

__global__ void vadd(const float* a, const float* b, float* c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        c[i] = a[i] + b[i];
    }
}

__global__ void scale(float* c, float factor, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        c[i] = c[i] * factor;
    }
}

static int failures = 0;

static void check(hipError_t err, const char* what) {
    if (err != hipSuccess) {
        printf("vadd: %s failed: %s (%s)\n", what, hipGetErrorName(err), hipGetErrorString(err));
        failures++;
    }
}

static int g_expect_compute = 0;
static clock_t g_start = 0;
static int g_first_launch_reported = 0;

// Milliseconds since main() started. The route document asks for the start time of the process
// and the time to the first kernel, because the code object load path is new.
static double elapsed_ms(void) {
    return 1000.0 * (double)(clock() - g_start) / (double)CLOCKS_PER_SEC;
}

static int run_once(int pass) {
    float* host_a = (float*)malloc(sizeof(float) * N);
    float* host_b = (float*)malloc(sizeof(float) * N);
    float* host_c = (float*)malloc(sizeof(float) * N);
    float* device_a = NULL;
    float* device_b = NULL;
    float* device_c = NULL;
    hipStream_t stream = NULL;
    hipStream_t second = NULL;
    hipEvent_t done = NULL;
    int i;
    int mismatches = 0;

    if (host_a == NULL || host_b == NULL || host_c == NULL) {
        printf("vadd: out of host memory\n");
        return 1;
    }
    for (i = 0; i < N; ++i) {
        host_a[i] = (float)i;
        host_b[i] = (float)(2 * i);
        host_c[i] = -1.0f;
    }

    check(hipMalloc((void**)&device_a, sizeof(float) * N), "hipMalloc a");
    check(hipMalloc((void**)&device_b, sizeof(float) * N), "hipMalloc b");
    check(hipMalloc((void**)&device_c, sizeof(float) * N), "hipMalloc c");
    check(hipStreamCreate(&stream), "hipStreamCreate");
    check(hipStreamCreate(&second), "hipStreamCreate 2");
    check(hipEventCreate(&done), "hipEventCreate");

    check(hipMemcpy(device_a, host_a, sizeof(float) * N, hipMemcpyHostToDevice), "copy a");
    check(hipMemcpy(device_b, host_b, sizeof(float) * N, hipMemcpyHostToDevice), "copy b");
    check(hipMemsetAsync(device_c, 0, sizeof(float) * N, stream), "memset c");

    vadd<<<N / BLOCK, BLOCK, 0, stream>>>(device_a, device_b, device_c, N);
    check(hipGetLastError(), "vadd launch");
    if (!g_first_launch_reported) {
        g_first_launch_reported = 1;
        printf("vadd: the first launch returned at %.1f ms after start\n", elapsed_ms());
    }

    // The event and the second stream: the scale kernel must run after the vadd kernel, on
    // another stream, through an event wait.
    check(hipEventRecord(done, stream), "hipEventRecord");
    check(hipStreamWaitEvent(second, done, 0), "hipStreamWaitEvent");
    scale<<<N / BLOCK, BLOCK, 0, second>>>(device_c, 1.0f, N);
    check(hipGetLastError(), "scale launch");

    check(hipStreamSynchronize(second), "hipStreamSynchronize");
    check(hipMemcpy(host_c, device_c, sizeof(float) * N, hipMemcpyDeviceToHost), "copy c");

    for (i = 0; i < N; ++i) {
        const float want = host_a[i] + host_b[i];
        if (host_c[i] != want) {
            if (mismatches < 3) {
                printf("vadd: pass %d index %d is %f, expected %f\n", pass, i, (double)host_c[i],
                       (double)want);
            }
            mismatches++;
        }
    }
    printf("vadd: pass %d mismatches %d of %d\n", pass, mismatches, N);
    if (g_expect_compute && mismatches != 0) {
        failures++;
    }

    check(hipEventDestroy(done), "hipEventDestroy");
    check(hipStreamDestroy(second), "hipStreamDestroy 2");
    check(hipStreamDestroy(stream), "hipStreamDestroy");
    check(hipFree(device_c), "hipFree c");
    check(hipFree(device_b), "hipFree b");
    check(hipFree(device_a), "hipFree a");
    free(host_c);
    free(host_b);
    free(host_a);
    {
        // The free bytes after the teardown of this pass. A later pass that reports fewer free
        // bytes than the first one leaked an allocation.
        size_t free_bytes = 0;
        size_t total_bytes = 0;
        check(hipMemGetInfo(&free_bytes, &total_bytes), "hipMemGetInfo");
        printf("vadd: pass %d memory free %zu of %zu after the teardown\n", pass, free_bytes,
               total_bytes);
    }
    return mismatches;
}

int main(int argc, char** argv) {
    hipDeviceProp_t prop;
    int count = 0;
    size_t free_bytes = 0;
    size_t total_bytes = 0;
    int i;

    g_start = clock();
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--expect-compute") == 0) {
            g_expect_compute = 1;
        } else {
            printf("vadd: unknown argument '%s'\n", argv[i]);
            return 2;
        }
    }
    printf("vadd: sums are %s\n", g_expect_compute ? "checked" : "not checked (no --expect-compute)");

    check(hipInit(0), "hipInit");
    check(hipGetDeviceCount(&count), "hipGetDeviceCount");
    printf("vadd: devices %d\n", count);
    memset(&prop, 0, sizeof(prop));
    check(hipGetDeviceProperties(&prop, 0), "hipGetDeviceProperties");
    printf("vadd: props name '%s' arch '%s' warp %d shared %zu cu %d\n", prop.name,
           prop.gcnArchName, prop.warpSize, prop.sharedMemPerBlock, prop.multiProcessorCount);
    check(hipMemGetInfo(&free_bytes, &total_bytes), "hipMemGetInfo");
    printf("vadd: memory free %zu total %zu\n", free_bytes, total_bytes);

    (void)run_once(1);
    (void)run_once(2);

    check(hipDeviceSynchronize(), "hipDeviceSynchronize");
    printf("vadd: api failures %d\n", failures);
    printf("vadd: %s\n", failures == 0 ? "ok" : "failed");
    return failures == 0 ? 0 : 1;
}
