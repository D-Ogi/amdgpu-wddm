// SPDX-License-Identifier: MIT
RWByteAddressBuffer output : register(u0);
float4 generic_vs(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}
void generic_ps(float4 position : SV_Position) {
    uint index = uint(position.y) * 16 + uint(position.x);
    output.Store(index * 4, 0x10000000u + index);
}
void generic_ps_added(float4 position : SV_Position) {
    uint index = uint(position.y) * 16 + uint(position.x);
    output.Store(index * 4, 0x20000000u + index);
}
