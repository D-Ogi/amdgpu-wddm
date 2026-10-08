// hip_test_kernels.hip - the device side of the layer-2 host test (M16 route B).
//
// The kernels have C linkage, so the device names in the metadata are 'vadd', 'scale' and
// 'nothing'. A test that calls __hipRegisterFunction by hand needs a name it can type, and a
// mangled C++ name would move with every change of a parameter type.
//
// vadd reads blockIdx and blockDim, so its kernel argument buffer carries the hidden (implicit)
// argument block as well as its four explicit arguments. That is what makes it the test of
// argument packing.
//
// The committed code object comes from this file. The command and the clang revision are in
// PROVENANCE-runtime.txt next to it.

#include "hip/hip_runtime.h"

extern "C" __global__ void vadd(const float* a, const float* b, float* c, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        c[i] = a[i] + b[i];
    }
}

extern "C" __global__ void scale(float* c, float factor, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) {
        c[i] = c[i] * factor;
    }
}

// A kernel with local memory and a barrier, for a dispatch whose local memory size the host has
// to compute.
extern "C" __global__ void reduce256(const float* in, float* out) {
    __shared__ float tile[256];
    unsigned t = threadIdx.x;
    tile[t] = in[blockIdx.x * 256u + t];
    __syncthreads();
    for (unsigned s = 128; s > 0; s >>= 1) {
        if (t < s) {
            tile[t] += tile[t + s];
        }
        __syncthreads();
    }
    if (t == 0) {
        out[blockIdx.x] = tile[0];
    }
}
