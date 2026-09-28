// SPDX-License-Identifier: MIT
// Registerless and generated system values: the pixel program writes SV_Depth, reads SV_PrimitiveID (which the
// vertex program does not write) and the vertex program writes SV_ClipDistance0. No input layout.

struct Varyings
{
    float4 pos  : SV_Position;
    float2 uv   : TEXCOORD0;
    float  clip : SV_ClipDistance0;
};

Varyings VSMain(uint vid : SV_VertexID)
{
    // Two triangles covering the target: 0 1 2, 2 1 3 of a 2x2 grid.
    static const uint corner[6] = { 0u, 1u, 2u, 2u, 1u, 3u };
    uint c = corner[vid % 6u];
    float2 uv = float2(float(c & 1u), float(c >> 1));
    Varyings o;
    o.pos = float4(uv * 2.0 - 1.0, 0.5, 1.0);
    o.uv = uv;
    o.clip = 0.8 - uv.x * uv.y;     // clips the far corner
    return o;
}

struct PSOut
{
    float4 color : SV_Target0;
    float  depth : SV_Depth;
};

PSOut PSMain(Varyings v, uint prim : SV_PrimitiveID)
{
    PSOut o;
    o.color = float4(v.uv, float(prim) + 0.25, v.pos.y / 64.0);
    o.depth = saturate(v.uv.x * 0.5 + v.uv.y * 0.25 + float(prim) * 0.125);
    return o;
}
