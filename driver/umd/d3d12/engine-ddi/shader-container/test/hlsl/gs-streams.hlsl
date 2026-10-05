// SPDX-License-Identifier: MIT
// gs_5_0 with two streams. fxc numbers the output registers of each stream from o0, so both streams use o0 and
// o1 with different elements; stream output captures both streams into two buffers. No rasterized stream.

struct VSOut
{
    float4 p : TEXCOORD0;
    nointerpolation uint id : BLENDINDICES;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    VSOut o;
    o.p = float4(float(vid) * 0.25, 1.0 - float(vid) * 0.125, float(vid & 1u), float(vid) + 0.5);
    o.id = vid * 3u + 1u;
    return o;
}

struct S0
{
    float4 pos : SV_Position;
    float3 v   : TEXCOORD0;
    float  s   : TEXCOORD4;
};

struct S1
{
    float2 w : TEXCOORD1;
    nointerpolation uint k : TEXCOORD2;
    float z : TEXCOORD3;
};

[maxvertexcount(3)]
void GSMain(point VSOut i[1], uint pid : SV_PrimitiveID, inout PointStream<S0> s0, inout PointStream<S1> s1)
{
    S0 a;
    a.pos = i[0].p;
    a.v = i[0].p.zyx * 2.0;
    a.s = float(pid);
    s0.Append(a);
    a.v += 1.0;
    a.s += 0.5;
    s0.Append(a);

    S1 b;
    b.w = i[0].p.xy - 0.5;
    b.k = i[0].id * 5u + pid;
    b.z = i[0].p.w;
    s1.Append(b);
}
