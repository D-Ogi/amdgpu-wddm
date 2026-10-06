// SPDX-License-Identifier: MIT
// Raster programs of the conformance client (ROV and conservative rasterization subtests), compiled by build.ps1
// with dxc into gen\*.h. Vertices are given in pixels of a 64x64 target; the vertex program maps them to clip space
// with exact power-of-two arithmetic, so the rasterizer sees the CPU oracle's coordinates without rounding.
struct VertexIn {
    float2 position : POSITION;
    uint tag : TAG;
};
struct VertexOut {
    float4 position : SV_Position;
    nointerpolation uint tag : TAG;
};

VertexOut vs_main(VertexIn v) {
    VertexOut o;
    o.position = float4(v.position.x / 32.0f - 1.0f, 1.0f - v.position.y / 32.0f, 0.5f, 1.0f);
    o.tag = v.tag;
    return o;
}

// ROV subtest: an order-dependent read-modify-write per pixel, v = v * 0x01000193 + (tag ^ (y * 64 + x)).
// Swapping any two different tags changes the result, so the stored word depends on the order in which the
// primitives covering the pixel were applied. ORDERED declares the target as a rasterizer ordered view (accesses
// in primitive order); without it the same program uses a plain RWTexture2D, whose order is undefined.
#ifdef ORDERED
RasterizerOrderedTexture2D<uint> target : register(u0);
#else
RWTexture2D<uint> target : register(u0);
#endif

uint ps_fold(VertexOut i) : SV_Target {
    uint2 p = uint2(i.position.xy);
    uint v = target[p];
    target[p] = v * 0x01000193u + (i.tag ^ (p.y * 64u + p.x));
    return i.tag;
}

// Conservative rasterization subtest: the tag of the primitive, and with SV_InnerCoverage (tier 3 only) the tag
// shifted left by one with bit 0 of the inner coverage in bit 0.
uint ps_tag(VertexOut i) : SV_Target {
    return i.tag;
}

uint ps_inner(VertexOut i, uint inner : SV_InnerCoverage) : SV_Target {
    return (i.tag << 1) | (inner & 1u);
}
