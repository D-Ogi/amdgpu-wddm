// SPDX-License-Identifier: MIT
// Programs of the record bench variant (interactive-recordbench.h). Compiled with fxc into recordbench-programs.h;
// the commands are recorded at the top of that file.
//
// One draw covers the single pixel of a 1x1 target. Its pixel program writes what it read into its own 128-byte slot
// of the root UAV, slot index root[0].x: the 8 root constants, the 16 words of the root CBV, the 4 words of the
// descriptor table's CBV, the pipeline's mark with the slot index, the vertex buffer's tag, a cross word of all four
// sources and a fixed seal. ps_a and ps_b differ only in the mark, so the slot shows which pipeline drew. The target
// gets root word 1, which differs per frame and draw, so the target's final value names the last draw that rendered
// to it in that frame.
// u1, not u0: in ps_5_0 UAVs share the output register space with SV_Target0.
cbuffer Root : register(b0) { uint4 root[2]; };
cbuffer Ring : register(b1) { uint4 ring[4]; };
cbuffer Table : register(b2) { uint4 table; };
RWByteAddressBuffer slots : register(u1);

struct Vertex {
    float4 position : SV_Position;
    nointerpolation uint tag : TAG;
};

Vertex vs_main(uint tag : TAG, uint id : SV_VertexID) {
    Vertex v;
    v.position = float4(id == 2 ? 3.0f : -1.0f, id == 1 ? 3.0f : -1.0f, 0.5f, 1.0f);
    v.tag = tag;
    return v;
}

uint write_slot(Vertex v, uint mark) {
    const uint base = root[0].x * 128;
    slots.Store4(base, root[0]);
    slots.Store4(base + 16, root[1]);
    [unroll] for (uint j = 0; j < 4; ++j) slots.Store4(base + 32 + 16 * j, ring[j]);
    slots.Store4(base + 96, table);
    slots.Store4(base + 112, uint4(root[0].x ^ (mark << 24), v.tag, root[0].y ^ ring[0].x ^ table.x ^ v.tag, 0x600DF00Du));
    return root[0].y;
}

uint ps_a(Vertex v) : SV_Target0 { return write_slot(v, 0xA5u); }
uint ps_b(Vertex v) : SV_Target0 { return write_slot(v, 0x5Au); }
