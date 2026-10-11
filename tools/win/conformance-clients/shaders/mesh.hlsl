// SPDX-License-Identifier: MIT
// Mesh shader witness: the same eight triangles drawn three times, from a vertex program, from a mesh program, and
// from an amplification program that dispatches the mesh program. The three images must be identical.
//
// Every triangle sits in its own 16x16 cell of the 64x64 target, so a wrong triangle is a wrong cell. The pixel
// program writes two tags and a constant: the vertex tag, which the vertex or mesh program gives per vertex, and the
// primitive tag, which the vertex program gives per vertex and the mesh program gives per primitive. Both tags are
// nointerpolation, so a pixel takes them from the first vertex of its triangle, whichever program wrote them. A
// mesh pipeline whose primitive output does not reach the pixel input writes the wrong primitive tag, and the
// comparison with the vertex reference fails on every covered pixel.
//
// Entry points, all shader model 6.5: vs_main, ps_main, ms_main (WITNESS_PAYLOAD for the amplification variant),
// as_main.

#define WITNESS_TRIS 8

struct PsIn {
    float4 pos : SV_Position;
    nointerpolation uint vtag : TAGV;
    nointerpolation uint ptag : TAGP;
};

struct MsVert {
    float4 pos : SV_Position;
    nointerpolation uint vtag : TAGV;
};

struct MsPrim {
    nointerpolation uint ptag : TAGP;
};

struct Payload {
    uint tri[WITNESS_TRIS];
};

uint vtag_of(uint tri) { return 0x10u + tri; }
uint ptag_of(uint tri) { return 0x80u + (7u - tri); }

// Corner c of triangle tri in pixels of the 64x64 target: cell (tri & 3, tri >> 2) of 16x16, the triangle inset by
// two pixels. The three corners are (2,2), (14,4) and (4,14) of the cell, which covers 70 pixel centres.
float2 corner(uint tri, uint c) {
    float2 p = float2(float(tri & 3u) * 16.0 + 2.0, float(tri >> 2) * 16.0 + 2.0);
    if (c == 1u) p += float2(12.0, 2.0);
    else if (c == 2u) p += float2(2.0, 12.0);
    return p;
}

// Pixel coordinates to clip space. The target is 64x64 and the triangles are flat, so z is 0 and w is 1.
float4 clip_of(float2 px) { return float4(px.x / 32.0 - 1.0, 1.0 - px.y / 32.0, 0.0, 1.0); }

PsIn vs_main(uint vid : SV_VertexID) {
    const uint tri = vid / 3u;
    PsIn o;
    o.pos = clip_of(corner(tri, vid % 3u));
    o.vtag = vtag_of(tri);
    o.ptag = ptag_of(tri);
    return o;
}

float4 ps_main(PsIn i) : SV_Target0 {
    return float4(float(i.vtag) / 255.0, float(i.ptag) / 255.0, 165.0 / 255.0, 1.0);
}

[outputtopology("triangle")]
[numthreads(1, 1, 1)]
void ms_main(uint3 gid : SV_GroupID,
#ifdef WITNESS_PAYLOAD
             in payload Payload pay,
#endif
             out vertices MsVert verts[3], out indices uint3 prims[1], out primitives MsPrim pdata[1]) {
    SetMeshOutputCounts(3, 1);
#ifdef WITNESS_PAYLOAD
    const uint tri = pay.tri[gid.x];
#else
    const uint tri = gid.x;
#endif
    for (uint c = 0; c < 3u; ++c) {
        verts[c].pos = clip_of(corner(tri, c));
        verts[c].vtag = vtag_of(tri);
    }
    prims[0] = uint3(0, 1, 2);
    pdata[0].ptag = ptag_of(tri);
}

// One group of eight threads, one triangle each, then eight mesh groups. A payload that does not arrive draws the
// wrong triangles.
groupshared Payload s_pay;

[numthreads(WITNESS_TRIS, 1, 1)]
void as_main(uint3 tid : SV_GroupThreadID) {
    s_pay.tri[tid.x] = tid.x;
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(WITNESS_TRIS, 1, 1, s_pay);
}
