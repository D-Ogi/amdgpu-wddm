// SPDX-License-Identifier: MIT
// Library of the ray pipeline variant (interactive-raypipeline.h), compiled with dxc into raypipeline-program.h;
// the command is recorded at the top of that file. The rays are those of rayquery.hlsl, traced with TraceRay
// instead of an inline query: one orthographic ray per dispatch index of an 8x8 grid, from z = -1 toward +z. The
// closest hit shader writes 1 into the payload, the miss shader 2, and the ray generation shader stores the payload
// as word (y * 8 + x) of the output. The payload starts at 0, so a ray that ran neither shader fails the comparison.
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);

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
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, payload);
    output.Store((id.y * 8 + id.x) * 4, payload.value);
}

[shader("miss")]
void miss(inout Payload payload) {
    payload.value = 2;
}

[shader("closesthit")]
void closest(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.value = 1;
}
