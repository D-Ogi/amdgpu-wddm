// SPDX-License-Identifier: MIT
// d3d12sm68, shader model 6.8 test: SampleCmpGrad, the expanded comparison sampling of SM 6.8 behind
// D3D12_FEATURE_D3D12_OPTIONS21.SampleCmpGradientAndBiasSupported (the driver's DDI type 1091). The client runs it
// only when that bit is set. A 4x4 R32_FLOAT texture holds texel i = i / 16 + 1 / 32; point filtering, zero
// gradients (LOD 0) and LESS_EQUAL against the reference 0.5 give 0 for texels 0-7 and 1 for texels 8-15.
// Word for thread i: round(result * 255) | (i << 8). build.ps1 compiles this with dxc -T cs_6_8.
#define RS "UAV(u0), DescriptorTable(SRV(t0)), " \
           "StaticSampler(s0, filter = FILTER_COMPARISON_MIN_MAG_MIP_POINT, addressU = TEXTURE_ADDRESS_CLAMP, " \
           "addressV = TEXTURE_ADDRESS_CLAMP, comparisonFunc = COMPARISON_LESS_EQUAL)"
Texture2D<float> depth : register(t0);
SamplerComparisonState compare : register(s0);
RWStructuredBuffer<uint> output : register(u0);

[RootSignature(RS)]
[numthreads(16, 1, 1)]
void main(uint index : SV_GroupIndex)
{
    float2 uv = (float2(index % 4, index / 4) + 0.5) / 4.0;
    float r = depth.SampleCmpGrad(compare, uv, 0.5, float2(0.0, 0.0), float2(0.0, 0.0));
    output[index] = uint(r * 255.0 + 0.5) | (index << 8);
}
