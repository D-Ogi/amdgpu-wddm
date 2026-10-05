// SPDX-License-Identifier: MIT
// hs_5_0/ds_5_0 with patch-constant signatures, compiled three times: /D TESS_TRI, /D TESS_QUAD, /D TESS_ISOLINE.
// The patch constants carry SV_TessFactor and SV_InsideTessFactor (none for isolines) next to user data, and
// the hull program's output control points use other registers than its input control points.

#if defined(TESS_TRI)
#define DOMAIN "tri"
#define NCP 3
#define NEDGE 3
#define NINSIDE 1
#define TOPO "triangle_cw"
#elif defined(TESS_QUAD)
#define DOMAIN "quad"
#define NCP 4
#define NEDGE 4
#define NINSIDE 2
#define TOPO "triangle_ccw"
#elif defined(TESS_ISOLINE)
#define DOMAIN "isoline"
#define NCP 2
#define NEDGE 2
#define NINSIDE 0
#define TOPO "line"
#else
#error define TESS_TRI, TESS_QUAD or TESS_ISOLINE
#endif

struct VSIn
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
};

struct CP
{
    float3 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float  w   : TEXCOORD1;
};

CP VSMain(VSIn i, uint vid : SV_VertexID)
{
    CP o;
    o.pos = i.pos;
    o.uv = i.uv;
    o.w = float(vid) * 0.25 + 0.5;
    return o;
}

struct HSCP
{
    float4 col : COLOR5;
    float3 pos : BEZIERPOS;
};

struct PC
{
    float e[NEDGE] : SV_TessFactor;
#if NINSIDE == 2
    float i[2] : SV_InsideTessFactor;
#elif NINSIDE == 1
    float i : SV_InsideTessFactor;
#endif
    float3 center : CENTER;
    nointerpolation uint tag : PATCHTAG;
};

PC PCMain(InputPatch<CP, NCP> ip, uint pid : SV_PrimitiveID)
{
    PC o;
    [unroll] for (uint k = 0; k < NEDGE; k++)
        o.e[k] = 2.0 + float(k) + float(pid);
#if NINSIDE == 2
    o.i[0] = 3.0;
    o.i[1] = 4.0 + float(pid);
#elif NINSIDE == 1
    o.i = 3.0 + float(pid);
#endif
    float3 c = 0.0;
    [unroll] for (uint n = 0; n < NCP; n++)
        c += ip[n].pos;
    o.center = c / float(NCP);
    o.tag = pid * 11u + 3u;
    return o;
}

[domain(DOMAIN)]
[partitioning("integer")]
[outputtopology(TOPO)]
[outputcontrolpoints(NCP)]
[patchconstantfunc("PCMain")]
[maxtessfactor(16.0)]
HSCP HSMain(InputPatch<CP, NCP> ip, uint i : SV_OutputControlPointID, uint pid : SV_PrimitiveID)
{
    HSCP o;
    o.pos = ip[i].pos;
    o.col = float4(ip[i].uv, ip[i].w, float(pid) + float(i) * 0.25);
    return o;
}

struct DSOut
{
    float4 pos : SV_Position;
    float4 col : TEXCOORD0;
    nointerpolation uint tag : TEXCOORD1;
};

[domain(DOMAIN)]
DSOut DSMain(PC pc,
#if defined(TESS_TRI)
             float3 loc : SV_DomainLocation,
#else
             float2 loc : SV_DomainLocation,
#endif
             const OutputPatch<HSCP, NCP> cp)
{
#if defined(TESS_TRI)
    float3 p = cp[0].pos * loc.x + cp[1].pos * loc.y + cp[2].pos * loc.z;
    float4 col = cp[0].col * loc.x + cp[1].col * loc.y + cp[2].col * loc.z;
    float inner = pc.i;
#elif defined(TESS_QUAD)
    float3 p = lerp(lerp(cp[0].pos, cp[1].pos, loc.x), lerp(cp[3].pos, cp[2].pos, loc.x), loc.y);
    float4 col = lerp(lerp(cp[0].col, cp[1].col, loc.x), lerp(cp[3].col, cp[2].col, loc.x), loc.y);
    float inner = pc.i[0] + pc.i[1] * 0.5;
#else
    float3 p = lerp(cp[0].pos, cp[1].pos, loc.x) + float3(0.0, loc.y * 0.5, 0.0);
    float4 col = lerp(cp[0].col, cp[1].col, loc.x);
    float inner = 0.0;
#endif
    DSOut o;
    o.pos = float4(p.xy + (p.xy - pc.center.xy) * 0.1, 0.5, 1.0);
    o.col = col + float4(pc.e[0] * 0.01, pc.e[1] * 0.02, inner * 0.03, pc.center.z);
    o.tag = pc.tag;
    return o;
}

struct PSOut
{
    float4 col : SV_Target0;
    uint   tag : SV_Target1;
};

PSOut PSMain(DSOut i)
{
    PSOut o;
    o.col = i.col;
    o.tag = i.tag;
    return o;
}
