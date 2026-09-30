// SPDX-License-Identifier: MIT
// Programs of the reset churn variant (interactive-resetchurn.h). Compiled with fxc into resetchurn-programs.h; the
// commands are recorded at the top of that file.
//
// One draw covers the single pixel of a 1x1 target. Its pixel program copies what it read, the 32 root constants
// and the 16 words of the root CBV, into slot root[0].x of the root UAV: 48 words, 192 bytes per slot. The slot
// index is itself a root constant, so a wrong constant also writes the wrong slot, and the comparison sees both.
// u1, not u0: in ps_5_0 UAVs share the output register space with SV_Target0.
cbuffer Root : register(b0) { uint4 root[8]; };
cbuffer Ring : register(b1) { uint4 ring[4]; };
RWByteAddressBuffer slots : register(u1);

float4 vs_main(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3.0f : -1.0f, id == 1 ? 3.0f : -1.0f, 0.5f, 1.0f);
}

uint ps_main(float4 position : SV_Position) : SV_Target0 {
    const uint base = root[0].x * 192;
    [unroll] for (uint i = 0; i < 8; ++i) slots.Store4(base + 16 * i, root[i]);
    [unroll] for (uint j = 0; j < 4; ++j) slots.Store4(base + 128 + 16 * j, ring[j]);
    return root[0].x;
}
