// SPDX-License-Identifier: MIT
// Ray tracing pipeline library of engine-ddi-harness (test-raytracing.cpp), compiled into fixture-raylib.h. The same
// orthographic rays as fixture-rayquery.hlsl, one per texel of an 8x8 grid; a hit writes the hit group's local root
// constant, a miss 2, word (y * 8 + x) of the output. One hit group "hitgroup" (triangles, closest hit "closest") is
// declared by the harness in the state object, not here.
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);
cbuffer Record : register(b0, space1) {        // the local root signature: one 32-bit constant in the hit group record
    uint hit_value;
};

struct Payload {
    uint value;
};

[shader("raygeneration")]
void raygen() {
    const uint2 id = DispatchRaysIndex().xy;
    RayDesc ray;
    ray.Origin = float3((id.x + 0.5f) / 4.0f - 1.0f, (id.y + 0.5f) / 4.0f - 1.0f, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 4.0f;
    Payload payload = {0u};
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, payload);
    output.Store((id.y * 8 + id.x) * 4, payload.value);
}

[shader("miss")]
void miss(inout Payload payload) {
    payload.value = 2u;
}

[shader("closesthit")]
void closest(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.value = hit_value;
}
