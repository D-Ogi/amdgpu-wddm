// SPDX-License-Identifier: MIT
// ps_5_0 with class linkage (interfaces and classes): fxc emits interface declarations and fcall. The
// reconstruction must refuse it. D3D12 has no class linkage; this is the unsupported-case control.

interface ILight
{
    float3 Shade(float3 n);
};

class Directional : ILight
{
    float3 dir;
    float3 Shade(float3 n) { return saturate(dot(n, dir)).xxx; }
};

class Ambient : ILight
{
    float3 color;
    float3 Shade(float3 n) { return color; }
};

ILight g_light;

cbuffer Lights : register(b0)
{
    Directional g_dir;
    Ambient g_amb;
};

float4 PSMain(float3 n : NORMAL) : SV_Target
{
    return float4(g_light.Shade(n), 1.0);
}
