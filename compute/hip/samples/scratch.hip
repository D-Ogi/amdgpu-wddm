// Fixed-private numeric probe. Run only on the lab with an independent watchdog.
#include "hip/hip_runtime.h"
#if !defined(__HIP_DEVICE_COMPILE__)
#include <stdio.h>
#include <stdlib.h>
#endif

template<unsigned N>
__global__ void scratch_probe(unsigned* output, unsigned count, unsigned seed) {
    unsigned id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= count) return;
    volatile unsigned local[N];
    unsigned base = id * 17u + seed;
    for (unsigned j = 0; j < N; ++j) local[j] = base + 3u * j;
    unsigned index = (id ^ seed) & (N - 1u);
    local[index] += 7u;
    unsigned sum = 0;
    for (unsigned j = 0; j < N; ++j) sum += local[(j + index) & (N - 1u)];
    output[id + 1u] = sum;
}

template __global__ void scratch_probe<64u>(unsigned*, unsigned, unsigned);
template __global__ void scratch_probe<1024u>(unsigned*, unsigned, unsigned);

#if !defined(__HIP_DEVICE_COMPILE__)
static bool check(hipError_t status, const char* operation) {
    if (status == hipSuccess) return true;
    fprintf(stderr, "FAIL operation=%s status=%s\n", operation, hipGetErrorName(status));
    fflush(stderr);
    return false;
}
int main() {
    // Registration precedes main; operator must also set this before process creation.
    if (_putenv_s("BC250_HIP_WAIT_TOTAL_MS", "10000") != 0) return 2;
    const unsigned count = 2051u, sentinel = 0xA55A1234u;
    const unsigned blocks[] = {31u, 32u, 33u, 64u, 256u, 1024u};
    unsigned host[count + 2u];
    unsigned* device = nullptr;
    if (!check(hipMalloc((void**)&device, sizeof(host)), "allocate")) return 2;
    // 36 submissions exceed the default ring slot count; grow then reuse both frame sizes.
    for (unsigned pass = 0; pass < 36u; ++pass) {
        unsigned n = ((pass / 8u) & 1u) ? 1024u : 64u;
        unsigned block = blocks[pass % 6u];
        unsigned seed = 0x12340000u + pass * 101u;
        for (unsigned j = 0; j < count + 2u; ++j) host[j] = sentinel;
        if (!check(hipMemcpy(device, host, sizeof(host), hipMemcpyHostToDevice), "initialize")) return 2;
        dim3 grid((count + block - 1u) / block);
        if (n == 64u) scratch_probe<64u><<<grid, block, 0, nullptr>>>(device, count, seed);
        else scratch_probe<1024u><<<grid, block, 0, nullptr>>>(device, count, seed);
        if (!check(hipGetLastError(), "launch") || !check(hipDeviceSynchronize(), "retire")) return 2;
        if (!check(hipMemcpy(host, device, sizeof(host), hipMemcpyDeviceToHost), "readback")) return 2;
        unsigned errors = (host[0] != sentinel) + (host[count + 1u] != sentinel);
        for (unsigned id = 0; id < count; ++id) {
            // Independent closed-form sum modulo 2^32, not a copy of the device loop.
            unsigned expected = n * (id * 17u + seed) + 3u * n * (n - 1u) / 2u + 7u;
            if (host[id + 1u] != expected) {
                if (errors < 4u) fprintf(stderr, "mismatch pass=%u id=%u got=%08x expected=%08x\n", pass, id, host[id+1u], expected);
                ++errors;
            }
        }
        printf("pass=%u array_bytes=%u block=%u count=%u mismatches=%u guards=%s\n", pass, n*4u, block, count, errors, host[0]==sentinel && host[count+1u]==sentinel ? "ok":"bad");
        fflush(stdout);
        if (errors) return 3;
    }
    if (!check(hipFree(device), "free")) return 2;
    puts("PASS fixed-private numeric probe: 36 dispatches, 73836 thread results, 72 guards");
    return 0;
}
#endif
