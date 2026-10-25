// SPDX-License-Identifier: MIT
// Compile limits_vs as vs_6_0, limits_ps as ps_6_0, limits_cs as cs_6_0.
RWByteAddressBuffer output : register(u0);
float4 limits_vs(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}
void limits_ps(float4 position : SV_Position, uint sample : SV_SampleIndex) {
    uint ignored;
    output.InterlockedOr((uint(position.y) * 16 + uint(position.x)) * 4, 1u << sample, ignored);
}
Texture2D<float> source : register(t0);
SamplerState dynamic_sampler : register(s0);
SamplerState static_sampler : register(s2032);
[numthreads(1, 1, 1)]
void limits_cs() {
    output.Store(0, asuint(source.SampleLevel(dynamic_sampler, float2(1.25, 0.5), 0)));
    output.Store(4, asuint(source.SampleLevel(static_sampler, float2(1.25, 0.5), 0)));
}
