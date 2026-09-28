// SPDX-License-Identifier: MIT
// VS/PS pair for the container reconstruction test. The vertex inputs and the varyings sit on different
// registers with different masks; fxc packs several varyings into shared registers. System values:
// SV_VertexID, SV_InstanceID, SV_Position (not the first varying), SV_IsFrontFace, SV_Target0, 1 and 3.
// One varying is min16float. COLOR3 and TEXCOORD2 have the same format, so a swapped input register is
// visible in the output (the mismatch control).

struct VSIn
{
    float3 pos   : POSITION;
    float4 color : COLOR3;
    float2 uv    : TEXCOORD5;
    float4 extra : TEXCOORD2;
    uint   tag   : BLENDINDICES;    // per instance
    uint   vid   : SV_VertexID;
    uint   iid   : SV_InstanceID;
};

struct Varyings
{
    float2 a : TEXCOORD7;
    float  b : TEXCOORD1;
    float4 pos : SV_Position;
    nointerpolation uint t : BLENDWEIGHT0;
    float3 c : COLOR0;
    float  d : FOG;
    float4 e : TEXCOORD2;
    nointerpolation int2 g : PSIZE1;
    min16float2 h : TEXCOORD6;
    float4 f : TEXCOORD3;
};

Varyings VSMain(VSIn i)
{
    Varyings o;
    float2 ofs = float2(float(i.iid) * 0.5 - 0.25, float(i.iid) * 0.125);
    o.pos = float4(i.pos.xy * 0.75 + ofs, i.pos.z, 1.0);
    o.a = i.uv * 3.0 + float(i.vid) * 0.125;
    o.b = i.color.w * 0.5 + float(i.iid);
    o.t = i.tag * 3u + i.vid;
    o.c = i.color.rgb;
    o.d = i.extra.w;
    o.e = i.extra * 1.5 - 0.25;
    o.g = int2(int(i.vid), -int(i.iid) - 3);
    o.h = min16float2(i.uv.yx);
    o.f = float4(i.color.zyx, i.extra.x);
    return o;
}

struct PSOut
{
    float4 c0 : SV_Target0;
    uint   c1 : SV_Target1;
    float2 c3 : SV_Target3;
};

PSOut PSMain(Varyings v, bool ff : SV_IsFrontFace)
{
    PSOut o;
    o.c0 = float4(frac(v.a.x + v.b), saturate(v.c.y * v.d), frac(v.e.z * v.f.w) * 0.5 + (ff ? 0.0 : 0.5),
                  frac(v.pos.x / 64.0));
    o.c1 = v.t * 7u + (ff ? 1u : 2u) + uint(v.g.x) * 13u + uint(v.g.y) * 17u + (asuint(v.e.x) >> 16);
    o.c3 = float2(v.e.x + v.f.y + float(v.h.x), dot(v.f, v.e) + v.a.y + float(v.h.y) + v.c.x + v.c.z);
    return o;
}
