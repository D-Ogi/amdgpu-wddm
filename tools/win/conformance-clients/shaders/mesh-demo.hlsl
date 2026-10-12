// SPDX-License-Identifier: MIT
// mesh-demo: a field of rotating torus knots drawn by mesh programs only. There is no vertex buffer, no index buffer
// and no input layout: the mesh program makes every vertex and every triangle of a knot from its parameters.
//
// One amplification thread is one object. It culls the object's bounding sphere against the six frustum planes,
// picks a level of detail from the distance to the eye and writes one payload entry per meshlet of that level. One
// mesh group is one meshlet: a tile of 8 x 8 quads of the knot's (u, v) surface grid, 81 vertices and 128 triangles.
// The meshlet colour is a per-primitive output, so the per-primitive path to the pixel program is part of every
// picture.
//
// Entry points, all shader model 6.5: as_main, ms_main, ps_main.

#define AS_GROUP 32u              // objects per amplification group
#define MS_THREADS 128u           // one thread per triangle; the first 81 also make one vertex each
#define TILE 8u                   // quads per meshlet side
#define TILE_VERTS 81u            // (TILE + 1) * (TILE + 1)
#define TILE_TRIS 128u            // TILE * TILE * 2
#define MAX_MESHLETS 64u          // level 0: (256 / TILE) * (16 / TILE)
#define FLAG_MESHLET_COLOURS 1u
#define FLAG_STATS 2u
#define TWO_PI 6.28318530718

// The C++ side mirrors this layout in FrameConstants (mesh-demo.cpp) and checks its size.
cbuffer Frame : register(b0) {
    row_major float4x4 g_view_proj;   // row vectors: clip = mul(float4(world, 1), g_view_proj)
    float4 g_planes[6];               // inward frustum planes, xyz normalized
    float3 g_eye;        float g_time;
    float3 g_sun;        uint  g_object_count;
    float3 g_fog;        uint  g_grid_side;
    float4 g_lod;                     // x, y, z: the distances where levels 1, 2 and 3 start; w: fog start
    float  g_spacing;    float g_fog_end;    uint g_flags;    float g_pad;
};

// Counters of the culling result, zeroed before every frame: [0] objects drawn, [1] meshlets, [2..5] objects per
// level of detail.
RWByteAddressBuffer g_stats : register(u0);

// Surface grid of each level of detail: segments along the knot (u) and around the tube (v). Both are multiples of
// TILE, so a level is a whole number of meshlets: 64, 16, 8 and 5.
static const uint kSegmentsU[4] = {256u, 128u, 64u, 40u};
static const uint kSegmentsV[4] = {16u, 8u, 8u, 8u};
// (p, q) torus knots, every pair coprime.
static const uint2 kKnots[8] = {uint2(2, 3), uint2(3, 2), uint2(2, 5), uint2(3, 4),
                                uint2(3, 5), uint2(5, 2), uint2(4, 3), uint2(5, 3)};
static const float kTube = 0.38;
static const float kBound = 3.55;  // the curve reaches sqrt(3 * 3 + 1) = 3.17 from the centre, plus the tube

struct Payload {
    uint meshlet[AS_GROUP * MAX_MESHLETS];   // object << 8 | level << 6 | meshlet
};

struct VertexOut {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float3 world : WORLDPOS;
};

struct PrimitiveOut {
    nointerpolation float3 colour : COLOUR;
};

struct PsIn {
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float3 world : WORLDPOS;
    nointerpolation float3 colour : COLOUR;
};

uint hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
float unit(uint h) { return float(h >> 8) * (1.0 / 16777216.0); }

float3 hue(float h) { return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0); }

struct Object {
    float3 centre;
    float3 axis;
    float angle;
    float2 pq;
    float3 colour;
};

float3 object_centre(uint i) {
    const float half_side = (float(g_grid_side) - 1.0) * 0.5;
    const float x = (float(i % g_grid_side) - half_side) * g_spacing;
    const float z = (float(i / g_grid_side) - half_side) * g_spacing;
    const float y = 3.0 * sin(0.11 * x + 0.7 * g_time) * cos(0.09 * z + 0.5 * g_time);
    return float3(x, y, z);
}

Object object_of(uint i) {
    static const float3 kPaint[6] = {float3(1.00, 0.71, 0.29), float3(0.95, 0.64, 0.54), float3(0.91, 0.92, 0.92),
                                     float3(0.10, 0.62, 0.60), float3(0.72, 0.08, 0.12), float3(0.14, 0.27, 0.80)};
    const uint h0 = hash(i * 3u + 1u), h1 = hash(i * 3u + 2u), h2 = hash(i * 3u + 3u);
    Object o;
    o.centre = object_centre(i);
    o.axis = normalize(float3(unit(h0), unit(h1), unit(h2)) - 0.5 + float3(0.0, 0.2, 0.0));
    o.angle = g_time * (0.4 + 1.2 * unit(hash(h0))) + TWO_PI * unit(h1);
    o.pq = float2(kKnots[h2 & 7u]);
    o.colour = kPaint[(h2 >> 3) % 6u];
    return o;
}

// Rodrigues: v turned by angle about the unit axis.
float3 rotate(float3 v, float3 axis, float angle) {
    float s, c;
    sincos(angle, s, c);
    return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

float3 knot_curve(float phi, float2 pq) {
    const float r = 2.0 + cos(pq.y * phi);
    return float3(r * cos(pq.x * phi), r * sin(pq.x * phi), sin(pq.y * phi));
}

// The tube around the knot at (s, t) in [0, 1)^2, in object space. The frame is tangent, normal and binormal with
// tangent x normal = binormal, and the surface offset is -cos(theta) normal + sin(theta) binormal. With that frame
// d/du x d/dv points outward, so the triangles of ms_main face outward when u runs along the first edge.
float3 knot_surface(float s, float t, float2 pq, out float3 normal) {
    const float phi = s * TWO_PI, theta = t * TWO_PI;
    const float3 p1 = knot_curve(phi, pq);
    const float3 p2 = knot_curve(phi + 0.01, pq);
    const float3 tangent = p2 - p1;
    const float3 b = normalize(cross(tangent, p1 + p2));
    const float3 n = normalize(cross(b, tangent));
    float st, ct;
    sincos(theta, st, ct);
    normal = -ct * n + st * b;
    return p1 + kTube * normal;
}

float3 meshlet_colour(uint object, uint meshlet) {
    return lerp(float3(0.06, 0.06, 0.06), hue(unit(hash(object * MAX_MESHLETS + meshlet))), 0.9);
}

// ------------------------------------------------------------------------------------------------- amplification
groupshared Payload s_payload;
groupshared uint s_count;

[numthreads(AS_GROUP, 1, 1)]
void as_main(uint gtid : SV_GroupThreadID, uint dtid : SV_DispatchThreadID) {
    if (gtid == 0u) s_count = 0u;
    GroupMemoryBarrierWithGroupSync();
    if (dtid < g_object_count) {
        const float3 c = object_centre(dtid);
        bool visible = true;
        [unroll] for (uint p = 0; p < 6u; ++p) visible = visible && dot(g_planes[p].xyz, c) + g_planes[p].w > -kBound;
        if (visible) {
            const float d = distance(c, g_eye);
            const uint lod = d < g_lod.x ? 0u : d < g_lod.y ? 1u : d < g_lod.z ? 2u : 3u;
            const uint n = (kSegmentsU[lod] / TILE) * (kSegmentsV[lod] / TILE);
            uint base;
            InterlockedAdd(s_count, n, base);
            for (uint m = 0; m < n; ++m) s_payload.meshlet[base + m] = (dtid << 8) | (lod << 6) | m;
            if (g_flags & FLAG_STATS) {
                uint ignored;
                g_stats.InterlockedAdd(0, 1u, ignored);
                g_stats.InterlockedAdd(4, n, ignored);
                g_stats.InterlockedAdd(8 + 4 * lod, 1u, ignored);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(s_count, 1, 1, s_payload);
}

// ------------------------------------------------------------------------------------------------- mesh
[outputtopology("triangle")]
[numthreads(MS_THREADS, 1, 1)]
void ms_main(uint gtid : SV_GroupThreadID, uint3 gid : SV_GroupID, in payload Payload pay,
             out vertices VertexOut verts[TILE_VERTS], out indices uint3 tris[TILE_TRIS],
             out primitives PrimitiveOut prims[TILE_TRIS]) {
    const uint packed = pay.meshlet[gid.x];
    const uint index = packed >> 8, lod = (packed >> 6) & 3u, meshlet = packed & 63u;
    const uint segments_u = kSegmentsU[lod], segments_v = kSegmentsV[lod];
    const uint tiles_u = segments_u / TILE;
    const uint u0 = (meshlet % tiles_u) * TILE, v0 = (meshlet / tiles_u) * TILE;
    const Object o = object_of(index);
    SetMeshOutputCounts(TILE_VERTS, TILE_TRIS);

    if (gtid < TILE_VERTS) {
        // The last row and column of a tile are the first of the next one; modulo makes the seam vertices
        // bit-identical, so the tube is closed without cracks.
        const uint u = (u0 + gtid % (TILE + 1u)) % segments_u;
        const uint v = (v0 + gtid / (TILE + 1u)) % segments_v;
        float3 normal;
        const float3 local = knot_surface(float(u) / float(segments_u), float(v) / float(segments_v), o.pq, normal);
        const float3 world = o.centre + rotate(local, o.axis, o.angle);
        verts[gtid].pos = mul(float4(world, 1.0), g_view_proj);
        verts[gtid].normal = rotate(normal, o.axis, o.angle);
        verts[gtid].world = world;
    }

    // Quad q of the tile is two triangles; both take u along their first edge, so both face outward.
    const uint quad = gtid >> 1;
    const uint i0 = (quad / TILE) * (TILE + 1u) + quad % TILE;
    tris[gtid] = (gtid & 1u) ? uint3(i0 + 1u, i0 + TILE + 2u, i0 + TILE + 1u) : uint3(i0, i0 + 1u, i0 + TILE + 1u);
    prims[gtid].colour = (g_flags & FLAG_MESHLET_COLOURS) ? meshlet_colour(index, meshlet) : o.colour;
}

// ------------------------------------------------------------------------------------------------- pixel
float4 ps_main(PsIn i) : SV_Target0 {
    const float3 n = normalize(i.normal);
    const float3 v = normalize(g_eye - i.world);
    const float3 h = normalize(g_sun + v);
    const float3 sky = float3(0.32, 0.42, 0.62), ground = float3(0.07, 0.05, 0.04);
    const float3 ambient = lerp(ground, sky, n.y * 0.5 + 0.5) * 0.55;
    const float diffuse = saturate(dot(n, g_sun));
    const float specular = pow(saturate(dot(n, h)), 48.0) * 0.55 * step(0.0, diffuse);
    const float rim = pow(1.0 - saturate(dot(n, v)), 3.0) * 0.3;
    float3 c = i.colour * (ambient + diffuse * float3(1.0, 0.94, 0.84)) + specular + rim * sky;
    const float fog = saturate((distance(g_eye, i.world) - g_lod.w) / (g_fog_end - g_lod.w));
    c = lerp(c, g_fog, fog * fog);
    return float4(pow(saturate(c), 1.0 / 2.2), 1.0);   // the target is UNORM, so the encoding is done here
}
