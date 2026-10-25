// SPDX-License-Identifier: MIT
cbuffer Draw : register(b0) { float depth; uint shape; uint value; uint unused; }
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 p;
    if (shape == 2) {
        float2 positions[6] = { float2(-1,-1), float2(-1,1), float2(0,-1),
                                float2(0,1), float2(1,-1), float2(1,1) };
        p = positions[id % 6];
    } else if (shape == 3) {
        p = float2(id ? 0.75 : -0.75, 0.0625);
    } else {
        uint i = shape == 1 ? (id == 1 ? 2 : id == 2 ? 1 : 0) : id;
        p = float2(i == 2 ? 3 : -1, i == 1 ? 3 : -1);
    }
    return float4(p, depth, 1);
}
uint ps() : SV_Target { return value; }
