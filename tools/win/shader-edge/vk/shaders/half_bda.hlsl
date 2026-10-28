// SPDX-License-Identifier: MIT
// Known-good reference for a structured buffer of vector<half, N> elements read and written through a physical
// pointer (buffer device address), N = 3, 5 and 7: element strides of 6, 10 and 14 bytes, which are not a multiple
// of 4. This is the access class of defect BD-248, where our DXIL to SPIR-V translation gave thread t the element
// N * t instead of the element t. This program does by hand what the translation must emit: the address of
// component c of element t is base + t * 2N + 2c, every access 2-byte aligned, written in HLSL so that DXC, not
// our translation, makes the SPIR-V. Compare a DXIL program of the same shape with it (vkshaderedge.c,
// CheckHalfStride, has the expected values).
//
// N            3, 5 or 7
// VECTOR       0: one 16-bit load and store per component (vk::RawBufferLoad<half>)
//              1: vector accesses, half3 for N = 3, half4 + half for N = 5, half4 + half3 for N = 7, at alignment 2
// pc.a0        input elements, pc.a1 output elements, pc.n the number of elements (one thread per element)
// out[t][c] = in[t][c] * 2 + c, exact in binary16 for the values the client writes.
//
// pc.neg = 1 is the negative control: every thread uses element N * t, the BD-248 model. The client makes both
// buffers N times larger for it, and the check must fail.
#include "inline_spirv.hlsli"

#ifndef N
#define N 3
#endif
#ifndef VECTOR
#define VECTOR 0
#endif

[numthreads(64, 1, 1)]
void half_main(uint t : SV_DispatchThreadID)
{
    if (t >= pc.n)
        return;
    const uint element = pc.neg ? N * t : t;
    const uint64_t offset = (uint64_t)element * (2 * N);
    const uint64_t src = pc.a0 + offset, dst = pc.a1 + offset;
#if VECTOR
#if N == 3
    half3 v = vk::RawBufferLoad<half3>(src, 2);
    vk::RawBufferStore<half3>(dst, v * (half)2 + half3(0, 1, 2), 2);
#elif N == 5
    half4 v = vk::RawBufferLoad<half4>(src, 2);
    half w = vk::RawBufferLoad<half>(src + 8, 2);
    vk::RawBufferStore<half4>(dst, v * (half)2 + half4(0, 1, 2, 3), 2);
    vk::RawBufferStore<half>(dst + 8, w * (half)2 + (half)4, 2);
#elif N == 7
    half4 v = vk::RawBufferLoad<half4>(src, 2);
    half3 w = vk::RawBufferLoad<half3>(src + 8, 2);
    vk::RawBufferStore<half4>(dst, v * (half)2 + half4(0, 1, 2, 3), 2);
    vk::RawBufferStore<half3>(dst + 8, w * (half)2 + half3(4, 5, 6), 2);
#else
#error N must be 3, 5 or 7
#endif
#else
    [unroll] for (uint c = 0; c < N; ++c) {
        const half x = vk::RawBufferLoad<half>(src + 2 * c, 2);
        vk::RawBufferStore<half>(dst + 2 * c, x * (half)2 + (half)c, 2);
    }
#endif
}
