// SPDX-License-Identifier: MIT
float4 vs_main(uint id : SV_VertexID) : SV_Position {
    float2 vertices[4] = { float2(-1,-1), float2(-1,1), float2(1,1), float2(1,-1) };
    return float4(vertices[id], 0, 1);
}
struct Outputs { float4 first : SV_Target0; float4 second : SV_Target1; };
Outputs ps_main() {
    Outputs result;
    result.first = float4(1,0,0,1);
    result.second = float4(0,0,1,1);
    return result;
}
