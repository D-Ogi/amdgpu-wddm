// SPDX-License-Identifier: MIT
struct Varyings {
    noperspective centroid float4 position : SV_Position;
    float2 clip : SV_ClipDistance;
    float2 cull : SV_CullDistance;
};
Varyings VSMain(uint vertex : SV_VertexID) {
    Varyings o; o.position = float4(vertex, 0, 0.5, 1);
    o.clip = float2(vertex, 1); o.cull = float2(1, vertex); return o;
}
struct Output {
    float4 target : SV_Target;
    uint coverage : SV_Coverage;
#if defined(LESS_EQUAL)
    float depth : SV_DepthLessEqual;
#elif defined(GREATER_EQUAL)
    float depth : SV_DepthGreaterEqual;
#else
    float depth : SV_Depth;
#endif
};
Output PSMain(Varyings i, uint coverage : SV_Coverage) {
    Output o; o.target = float4(i.clip, i.cull); o.coverage = coverage;
    o.depth = i.position.z; return o;
}
