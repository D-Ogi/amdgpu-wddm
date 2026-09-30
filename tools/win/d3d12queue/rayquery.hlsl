// SPDX-License-Identifier: MIT
// Program of the ray query variant (interactive-rayquery.h), compiled with dxc into rayquery-program.h; the command
// is recorded at the top of that file. The body is that of engine-ddi's fixture-rayquery.hlsl, so client and harness
// trace the same rays. One orthographic ray per texel of an 8x8 grid, from z = -1 toward +z; 1 for a hit, 2 for a
// miss, word (y * 8 + x) of the output.
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    RayDesc ray;
    ray.Origin = float3((id.x + 0.5f) / 4.0f - 1.0f, (id.y + 0.5f) / 4.0f - 1.0f, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 4.0f;
    RayQuery<RAY_FLAG_FORCE_OPAQUE> query;
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0xFF, ray);
    while (query.Proceed()) {
    }
    output.Store((id.y * 8 + id.x) * 4, query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 1u : 2u);
}
