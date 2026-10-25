// SPDX-License-Identifier: MIT
Texture2D<float4> packed : register(t0);
Texture2D<float> boundary : register(t1);
Texture2D<float> mips : register(t2);
SamplerState point_sampler : register(s0);
SamplerState anisotropic_point_mip : register(s1);
SamplerState static_anisotropic_point_mip : register(s2);
SamplerState min_point_mag_linear : register(s3);
SamplerState min_linear_mag_point : register(s4);
SamplerState min_anisotropic_point_mip : register(s7);
SamplerState max_anisotropic_point_mip : register(s8);
SamplerComparisonState cmp_anisotropic_point_mip : register(s9);
SamplerState static_min_anisotropic_point_mip : register(s10);
SamplerState static_max_anisotropic_point_mip : register(s11);
SamplerComparisonState static_cmp_anisotropic_point_mip : register(s12);
#ifndef NO_UNNORMALIZED
SamplerState unnormalized_dynamic : register(s5);
SamplerState unnormalized_static : register(s6);
#endif
RWByteAddressBuffer output : register(u0);
[numthreads(1, 1, 1)]
void main() {
    for (uint y = 0; y < 2; ++y) for (uint x = 0; x < 2; ++x) {
        uint at = (y * 2 + x) * 16;
        output.Store4(at, (uint4)round(packed.Load(int3(x, y, 0)) * 15));
        output.Store4(64 + at, (uint4)round(packed.SampleLevel(point_sampler, (float2(x, y) + 0.5) / 2, 0) * 15));
    }
    output.Store4(128, (uint4)round(packed.GatherRed(point_sampler, float2(0.5, 0.5)) * 15));
    float coordinates[8] = { (1 - exp2(-10.0)) / 4, (1 - exp2(-12.0)) / 4,
        asfloat(0x3e7fffff), 0.25, (2 - exp2(-10.0)) / 4, asfloat(0x3effffff), 0.5, 0.875 };
    for (uint i = 0; i < 8; ++i)
        output.Store(144 + i * 4, (uint)round(boundary.SampleLevel(point_sampler, float2(coordinates[i], 0.5), 0)));
    float lods[4] = { 0.49, 0.51, 1.49, 1.51 };
    for (uint j = 0; j < 4; ++j) {
        output.Store(176 + j * 4, (uint)round(mips.SampleLevel(anisotropic_point_mip, float2(0.5, 0.5), lods[j])));
        output.Store(192 + j * 4, (uint)round(mips.SampleLevel(static_anisotropic_point_mip, float2(0.5, 0.5), lods[j])));
        output.Store(256 + j * 4, (uint)round(mips.SampleLevel(min_anisotropic_point_mip, float2(0.5, 0.5), lods[j])));
        output.Store(272 + j * 4, (uint)round(mips.SampleLevel(max_anisotropic_point_mip, float2(0.5, 0.5), lods[j])));
        output.Store(288 + j * 4, (uint)round(mips.SampleCmpLevel(cmp_anisotropic_point_mip, float2(0.5, 0.5), 16, lods[j])));
        output.Store(304 + j * 4, (uint)round(mips.SampleLevel(static_min_anisotropic_point_mip, float2(0.5, 0.5), lods[j])));
        output.Store(320 + j * 4, (uint)round(mips.SampleLevel(static_max_anisotropic_point_mip, float2(0.5, 0.5), lods[j])));
        output.Store(336 + j * 4, (uint)round(mips.SampleCmpLevel(static_cmp_anisotropic_point_mip, float2(0.5, 0.5), 16, lods[j])));
    }
    // Each mixed sampler has a point-boundary witness and a linear control.
    output.Store(208, (uint)round(boundary.SampleLevel(min_point_mag_linear, float2((2-exp2(-12.0))/4, 0.5), 1)));
    output.Store(212, (uint)round(boundary.SampleLevel(min_point_mag_linear, float2((1-exp2(-12.0))/4, 0.5), -1)));
    output.Store(216, (uint)round(boundary.SampleLevel(min_linear_mag_point, float2((1-exp2(-12.0))/4, 0.5), -1)));
    output.Store(220, (uint)round(boundary.SampleLevel(min_linear_mag_point, float2((2-exp2(-12.0))/4, 0.5), 1)));
#ifndef NO_UNNORMALIZED
    output.Store4(224, (uint4)round(packed.SampleLevel(unnormalized_dynamic, float2(1.5, 0.5), 0) * 15));
    output.Store4(240, (uint4)round(packed.SampleLevel(unnormalized_static, float2(0.5, 1.5), 0) * 15));
#endif
}
