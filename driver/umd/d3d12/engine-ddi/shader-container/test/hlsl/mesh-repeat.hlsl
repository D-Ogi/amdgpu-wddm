// SPDX-License-Identifier: MIT
// BD-286 fixture: a mesh program whose vertex output is an array without a struct, with a primitive output, and a
// pixel program that reads the vertex and primitive values. dxc 1.8.2502 writes one SV_Position entry for MSMain;
// an older dxc wrote one OSG1 row per array element. mesh-repeat-test adds such rows to the DDI form.

struct Primitive
{
    float4 color : PRIM_COLOR;
};

[numthreads(4, 1, 1)]
[outputtopology("triangle")]
void MSMain(in uint tid : SV_GroupIndex, out vertices float4 v[4] : SV_POSITION, out indices uint3 i[2],
        out primitives Primitive p[2])
{
    SetMeshOutputCounts(4, 2);
    v[tid] = float4(float(tid & 1) * 2.0f - 1.0f, float(tid & 2) - 1.0f, 0.0f, 1.0f);
    if (tid < 2)
    {
        i[tid] = uint3(tid, 1 + 2 * tid, 2);
        p[tid].color = float4(0.0f, 1.0f, float(tid), 1.0f);
    }
}

float4 PSMain(float4 position : SV_POSITION, float4 color : PRIM_COLOR) : SV_TARGET
{
    return color + position.xxxx * 0.0f;
}

// The control that must not merge: two geometry streams with one layout give rows that are equal in everything but
// the stream, and the runtime may give every entry stream 0.
struct GsOut
{
    float4 value : TEXCOORD0;
};

[maxvertexcount(1)]
void GSMain(point float4 p[1] : TEXCOORD0, inout PointStream<GsOut> s0, inout PointStream<GsOut> s1)
{
    GsOut o;
    o.value = p[0];
    s0.Append(o);
    o.value = p[0] * 2.0f;
    s1.Append(o);
}
