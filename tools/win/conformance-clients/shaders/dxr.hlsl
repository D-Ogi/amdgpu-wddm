// SPDX-License-Identifier: MIT
// Ray pipeline of the indirect DispatchRays subtest (lib_6_3). The scene and the rays are those of the d3d12queue
// ray pipeline variant: one orthographic ray per dispatch index from z = -1 toward +z over a triangle at z = 0.5;
// the closest hit shader writes 1 into the payload, the miss shader 2. Two ray generation shaders store
//   payload | tag << 8 | DispatchRaysDimensions().x << 16 | DispatchRaysDimensions().y << 24
// raygen_a (tag 0x0A) at word y * 8 + x and raygen_b (tag 0x0B) at word 64 + y * 8 + x of the output, so every
// word names the shader record that wrote it and the dimensions it was dispatched with.
RaytracingAccelerationStructure scene : register(t0);
RWByteAddressBuffer output : register(u0);

struct Payload {
    uint value;
};

void trace_grid(uint base, uint tag) {
    uint2 id = DispatchRaysIndex().xy;
    uint2 dim = DispatchRaysDimensions().xy;
    RayDesc ray;
    ray.Origin = float3((id.x + 0.5f) / 4.0f - 1.0f, (id.y + 0.5f) / 4.0f - 1.0f, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 4.0f;
    Payload payload;
    payload.value = 0;
    TraceRay(scene, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 1, 0, ray, payload);
    output.Store((base + id.y * 8 + id.x) * 4, payload.value | (tag << 8) | (dim.x << 16) | (dim.y << 24));
}

[shader("raygeneration")]
void raygen_a() {
    trace_grid(0, 0x0A);
}

[shader("raygeneration")]
void raygen_b() {
    trace_grid(64, 0x0B);
}

[shader("miss")]
void miss(inout Payload payload) {
    payload.value = 2;
}

[shader("closesthit")]
void closest(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.value = 1;
}
