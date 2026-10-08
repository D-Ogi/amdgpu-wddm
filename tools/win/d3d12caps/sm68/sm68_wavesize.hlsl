// SPDX-License-Identifier: MIT
// d3d12sm68, shader model 6.8 test: the WaveSize range attribute (min 32, max 64), which SM 6.8 requires. The driver
// picks a wave size inside the range; the test reads it back and checks every lane against it.
// Word for thread i: lanes | (active lanes << 8) | (lane index << 16) | (i << 24). Expected: lanes 32 or 64 and the
// same in the whole dispatch, active lanes = lanes (64 threads fill whole waves), lane index = i % lanes.
// build.ps1 compiles this with dxc -T cs_6_8.
RWStructuredBuffer<uint> output : register(u0);

[RootSignature("UAV(u0)")]
[WaveSize(32, 64)]
[numthreads(64, 1, 1)]
void main(uint index : SV_GroupIndex, uint3 group : SV_GroupID)
{
    uint lanes = WaveGetLaneCount();
    uint active = WaveActiveCountBits(true);
    uint lane = WaveGetLaneIndex();
    output[group.x * 64 + index] = lanes | (active << 8) | (lane << 16) | (index << 24);
}
