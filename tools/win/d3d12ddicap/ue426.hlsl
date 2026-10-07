// SPDX-License-Identifier: MIT
// Ray tracing libraries in the shape we infer for Unreal Engine 4.26 (one library per shader collection; INFERENCE,
// the engine source was not available): one library for the ray generation shader, one for the miss shader, one for
// the hit group's closest hit and any hit shaders. Compiled three times with -D RGS, -D MS or -D HIT (build.ps1).
// The payload is 24 bytes and the attributes 8, as the trial 465 minidump shows ({24, 8} on the failing thread's
// stack). The local root signature's constant is at b0 space1.
struct Payload {
    float4 color;
    float2 extra;
};

RaytracingAccelerationStructure scene : register(t0);
RWStructuredBuffer<uint> output : register(u0);
cbuffer Local : register(b0, space1) {
    uint local_value;
};

#ifdef RGS
[shader("raygeneration")] void MainRGS() {
    Payload payload = (Payload)0;
    RayDesc ray;
    ray.Origin = float3(DispatchRaysIndex().xy, -1.0f);
    ray.Direction = float3(0.0f, 0.0f, 1.0f);
    ray.TMin = 0.0f;
    ray.TMax = 4.0f;
    TraceRay(scene, RAY_FLAG_NONE, 0xFF, 0, 1, 0, ray, payload);
    output[DispatchRaysIndex().x] = asuint(payload.color.x);
}
#endif

#ifdef MS
[shader("miss")] void MainMS(inout Payload payload) {
    payload.color = float4(2.0f, 0.0f, 0.0f, 0.0f);
}
#endif

#ifdef HIT
[shader("closesthit")] void MainCHS(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.color = float4(float(local_value), attributes.barycentrics, 0.0f);
}

[shader("anyhit")] void MainAHS(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    if (attributes.barycentrics.x > 2.0f)
        IgnoreHit();
}
#endif
