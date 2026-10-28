// SPDX-License-Identifier: MIT
// OpReadClockKHR (SPV_KHR_shader_clock) emitted with DXC's inline SPIR-V attributes. Edge case from Erfan Ahmadi's
// "Inline SPIR-V" (https://erfan-ahmadi.github.io/blog/inline-spirv/), which reads the Subgroup clock this way.
//
// SCOPE is 3 (Subgroup, the shaderSubgroupClock feature) or 1 (Device, the shaderDeviceClock feature). Each thread
// reads the clock, runs pc.spin steps of dependent integer work, and reads it again. Words per thread: the first
// value (low, high), the second value (low, high), and the result of the work, which keeps the loop alive.
// vkshaderedge.c (CheckClock) requires every value to be non-zero, the second value of every thread to be not less
// than its first one, and the second value to be greater than the first in at least one thread. For the Device scope
// it runs the program twice, one submission after the other, and requires every value of the second run to be not
// less than every value of the first run.
//
// pc.neg = 1 is the negative control: the two values change places, so a working clock must fail the check.
#include "inline_spirv.hlsli"

#ifndef SCOPE
#define SCOPE 3
#endif

RWStructuredBuffer<uint> mem : register(u0);

[numthreads(64, 1, 1)]
void clock_main(uint t : SV_DispatchThreadID)
{
    uint2 first = spvReadClock(SCOPE);
    uint x = t ^ mem[5 * pc.n];
    for (uint i = 0; i < pc.spin; ++i)
        x = x * 1664525u + 1013904223u;
    uint2 second = spvReadClock(SCOPE);
    if (pc.neg) {
        const uint2 keep = first;
        first = second;
        second = keep;
    }
    mem[5 * t + 0] = first.x;
    mem[5 * t + 1] = first.y;
    mem[5 * t + 2] = second.x;
    mem[5 * t + 3] = second.y;
    mem[5 * t + 4] = x;
}
