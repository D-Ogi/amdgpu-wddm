// SPDX-License-Identifier: MIT
// Calibrated GPU work for the frame-loop client. Every thread folds one 32-bit accumulator `iterations` times
// and stores the result, so neither the loop nor the store can be removed: the trip count arrives in root
// constants, the fold is a dependent chain, and the stored value depends on every iteration.
//
// The dispatch shape (thread group count) stays fixed for a whole run; only `iterations` is calibrated, so the
// per-dispatch duration is close to linear in `iterations` with a small fixed intercept.

RWByteAddressBuffer sink : register(u0);

cbuffer Spin : register(b0)
{
    uint iterations;
    uint seed;
    uint slots;     // 32-bit slots in sink; threads beyond it do not store
    uint reserved;
};

[numthreads(64, 1, 1)]
void cs_main(uint3 thread : SV_DispatchThreadID)
{
    uint acc = thread.x * 2654435761u + seed + 1u;
    [loop]
    for (uint i = 0; i < iterations; ++i)
    {
        acc = acc * 1664525u + 1013904223u;
        acc ^= acc >> 13;
        acc += i ^ seed;
    }
    if (thread.x < slots)
    {
        sink.Store(thread.x * 4u, acc);
    }
}
