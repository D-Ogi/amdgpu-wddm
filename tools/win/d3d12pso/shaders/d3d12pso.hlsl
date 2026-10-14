// SPDX-License-Identifier: MIT
// The programs of amdgpu_wddm_d3d12pso. One triangle covers a 64x64 R32_UINT target and the pixel program writes a
// word the client also computes on the CPU, so a draw is checked against our own arithmetic and not against another
// renderer's picture.
//
// ROUNDS is the only difference between the variants (build.ps1 compiles one pixel program per variant with its own
// -D ROUNDS=...). The chain is unrolled, so every variant is a different program of a different length: the engine
// has to convert and compile each of them, which is the work this client measures. The arithmetic is exact 32-bit
// integer work, so neither dxc nor the engine may reassociate it into a different result.
#ifndef ROUNDS
#define ROUNDS 4
#endif

cbuffer Constants : register(b0) {
    uint g_seed;        // a root 32-bit constant, new on every run of the client
};

struct Vertex {
    float2 position : POSITION;
    uint tag : TAG;
};

struct Fragment {
    float4 position : SV_Position;
    nointerpolation uint tag : TAG;
};

Fragment vs_main(Vertex input) {
    Fragment output;
    output.position = float4(input.position, 0.0, 1.0);
    output.tag = input.tag;
    return output;
}

uint mix_round(uint value, uint round) {
    value += round;
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

uint ps_main(Fragment input) : SV_Target0 {
    const uint x = (uint)input.position.x;
    const uint y = (uint)input.position.y;
    uint value = g_seed ^ input.tag ^ ((y << 16) | x);
    [unroll] for (uint round = 0; round < ROUNDS; ++round) value = mix_round(value, round);
    return value;
}
