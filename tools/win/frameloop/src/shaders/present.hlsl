// SPDX-License-Identifier: MIT
// The frame's visible work: one fullscreen triangle whose colour is read from the buffer the frame's compute
// dispatches wrote. A presented frame therefore cannot be produced without that frame's GPU work having
// completed, which is what makes the frame-loop timings a measurement of a dependent pipeline and not of two
// unrelated streams.
//
// The 0..255 unpacking below reads the 32-bit words the compute shader stored. It is not a statement about the
// back buffer: the back buffer format lives in exactly one constant in the client (kBackBufferFormat) and the
// shader writes float4 in [0,1], which is correct for an 8-bit, a 10-bit or a float render target alike.

ByteAddressBuffer source : register(t0);

cbuffer Frame : register(b0)
{
    uint frame_index;
    uint slots;
    uint reserved0;
    uint reserved1;
};

void vs_main(uint vertex : SV_VertexID, out float4 position : SV_Position)
{
    const float2 xy = float2(float((vertex << 1) & 2u), float(vertex & 2u));
    position = float4(xy * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float4 ps_main(float4 position : SV_Position) : SV_Target
{
    const uint index = (uint(position.x) + uint(position.y) * 7u + frame_index) % max(slots, 1u);
    const uint value = source.Load(index * 4u);
    return float4(float((value >> 16) & 0xffu) / 255.0,
                  float((value >> 8) & 0xffu) / 255.0,
                  float(value & 0xffu) / 255.0,
                  1.0);
}
