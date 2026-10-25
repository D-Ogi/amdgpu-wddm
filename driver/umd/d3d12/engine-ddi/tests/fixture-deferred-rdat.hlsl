// SPDX-License-Identifier: MIT
// Deliberately omits pipeline/shader configuration to force collection deferral.
// The local root and association payloads must survive two imported generations.
RWByteAddressBuffer output : register(u0);
cbuffer Record : register(b0, space1) { uint value; };
LocalRootSignature Local = { "RootConstants(num32BitConstants=1, b0, space=1)" };
SubobjectToExportsAssociation LocalAssociation = { "Local", "deferred_raygen;deferred_closest" };
TriangleHitGroup DeferredHit = { "", "deferred_closest" };
struct Payload { uint value; };
[shader("raygeneration")]
void deferred_raygen() { output.Store(DispatchRaysIndex().x * 4, value + DispatchRaysIndex().x); }
[shader("closesthit")]
void deferred_closest(inout Payload payload, in BuiltInTriangleIntersectionAttributes attributes) {
    payload.value = value;
}
