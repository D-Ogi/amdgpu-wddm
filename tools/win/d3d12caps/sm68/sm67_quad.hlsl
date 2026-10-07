// SPDX-License-Identifier: MIT
// d3d12sm68, shader model 6.7 test: QuadAny and QuadAll, which SM 6.7 requires of every driver that reports it
// (no optional capability bit). A 1D group of 64 threads forms quads of four consecutive SV_GroupIndex values.
// Expected word for thread i: (quad i/4 even ? 3 : 0) | (i << 8). build.ps1 compiles this with dxc -T cs_6_7.
RWStructuredBuffer<uint> output : register(u0);

[RootSignature("UAV(u0)")]
[numthreads(64, 1, 1)]
void main(uint index : SV_GroupIndex, uint3 group : SV_GroupID)
{
    bool one_lane = (index % 8) == 1;       // one lane in every even quad, none in the odd ones
    bool all_lanes = (index % 8) < 4;       // all four lanes of every even quad, none of the odd ones
    uint v = (QuadAny(one_lane) ? 1u : 0u) | (QuadAll(all_lanes) ? 2u : 0u);
    output[group.x * 64 + index] = v | (index << 8);
}
