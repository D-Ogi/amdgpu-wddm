// SPDX-License-Identifier: MIT
// Hull shader control point phase that indexes per-control-point data with SV_OutputControlPointID. Edge case from
// Michael Moroz's FXC2 (https://github.com/MichaelMoroz/FXC2), patch 64: FXC2 once read such data from an
// indexable temp indexed by vOutputControlPointID itself (x0[vOutputControlPointID]), WARP drew it, and an NVIDIA
// card removed the D3D11 device at the first draw. FXC copies the index to a temp register first.
//
// The draw: one patch of three control points that covers the whole 64x64 target, tessellated with factor 4. The
// vertex program gives control point k the tag 7k + 3. The hull program stores tag[id] * (id + 1) for control point
// id, so the three stored values are 3, 20 and 51. The domain program combines them into one value that does not
// depend on the domain location, cp0 + 1000 cp1 + 1000000 cp2 = 51020003, and passes it without interpolation.
// Every covered pixel must hold it (d3d11shaderedge.cpp, case tess-*). A wrong index in the control point phase
// changes the value.
//
// HS_DIRECT 0: the hull program copies the inputs to local arrays and indexes the arrays (an indexable temp in DXBC).
// HS_DIRECT 1: the hull program indexes the input patch directly (vicp[...] in DXBC).
cbuffer Control : register(b0)
{
    uint neg;        // 1: the negative control, the value must then be wrong
    uint3 pad;
};

struct VsOut {
    float4 pos : POS;
    uint tag : TAG;
};

VsOut vs_main(uint id : SV_VertexID)
{
    VsOut o;
    o.pos = float4(id == 1 ? 3.0f : -1.0f, id == 2 ? 3.0f : -1.0f, 0.5f, 1.0f);
    o.tag = 7 * id + 3;
    return o;
}

struct ControlPoint {
    float4 pos : POS;
    uint value : VALUE;
};

struct PatchConstants {
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

PatchConstants hs_constants(InputPatch<VsOut, 3> ip)
{
    PatchConstants c;
    c.edges[0] = 4.0f;
    c.edges[1] = 4.0f;
    c.edges[2] = 4.0f;
    c.inside = 4.0f;
    return c;
}

#ifndef HS_DIRECT
#define HS_DIRECT 0
#endif

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_constants")]
ControlPoint hs_main(InputPatch<VsOut, 3> ip, uint id : SV_OutputControlPointID)
{
    ControlPoint o;
#if HS_DIRECT
    o.pos = ip[id].pos;
    o.value = ip[id].tag * (id + 1);
#else
    float4 positions[3] = { ip[0].pos, ip[1].pos, ip[2].pos };
    uint tags[3] = { ip[0].tag, ip[1].tag, ip[2].tag };
    o.pos = positions[id];
    o.value = tags[id] * (id + 1);
#endif
    return o;
}

struct DsOut {
    float4 pos : SV_Position;
    nointerpolation uint check : CHECK;
};

[domain("tri")]
DsOut ds_main(PatchConstants c, float3 uvw : SV_DomainLocation, const OutputPatch<ControlPoint, 3> cp)
{
    DsOut o;
    o.pos = uvw.x * cp[0].pos + uvw.y * cp[1].pos + uvw.z * cp[2].pos;
    o.check = cp[0].value + 1000 * cp[1].value + 1000000 * cp[2].value + neg;
    return o;
}

uint4 ps_main(DsOut i) : SV_Target
{
    return uint4(i.check, 0xC0FFEEu, 0, 1);
}
