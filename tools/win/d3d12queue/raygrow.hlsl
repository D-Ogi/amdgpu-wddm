// SPDX-License-Identifier: MIT
// First library of the grow variant (interactive-raypipeline.h, build.ps1 -RayGrow), compiled with dxc into
// raygrow-program.h; the command is recorded at the top of that file. The rays are those of raypipeline.hlsl. The ray
// generation shader picks miss shader 0 for even columns and 1 for odd ones: 1 is the miss shader that the addition
// in raygrow-miss.hlsl brings. The closest hit shader writes the constant of its hit group's local root signature.
// The payload starts at 0, so a ray that ran neither shader fails the comparison.
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);
cbuffer HitRecord : register(b0, space1) {
    uint hit_value;
};

struct Payload {
    uint value;
};

[shader("raygeneration")]
void raygen() {
    uint2 id = DispatchRaysIndex().xy;
    RayDesc ray;
    ray.Origin = float3((id.x + 0.5f) / 4.0f - 1.0f, (id.y + 0.5f) / 4.0f - 1.0f, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 4.0f;
    Payload payload;
    payload.value = 0;
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, id.x & 1, ray, payload);
    output.Store((id.y * 8 + id.x) * 4, payload.value);
}

[shader("miss")]
void miss(inout Payload payload) {
    payload.value = 2;
}

[shader("closesthit")]
void closest(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.value = hit_value;
}
