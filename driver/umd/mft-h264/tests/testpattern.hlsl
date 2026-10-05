// SPDX-License-Identifier: MIT
// The test source picture, drawn on the test's own Direct3D 11 device so that the encoder is fed a
// real GPU surface rather than an upload. Deterministic in the frame index: the same frame always
// produces the same pixels, so a bit-exactness result is reproducible.
//
// The content is chosen to exercise the parts of the encoder that matter:
//   - a smooth moving gradient: low frequency, large flat areas, where P_Skip and the DC prediction
//     modes have to work;
//   - hard-edged bars that move one pixel per frame: the quarter-sample interpolation and the
//     deblocking filter both show up here;
//   - a static one-pixel checkerboard: the worst case for the transform and quantisation, and the
//     place where a wrong zig-zag or scaling factor becomes visible first;
//   - a block that only appears every sixteenth frame, to make the key-frame decision observable.

cbuffer PatternConstants : register(b0)
{
    uint gFrame;
    uint gWidth;
    uint gHeight;
    uint gPad;
};

struct VsOut {
    float4 pos : SV_Position;
};

// One full-screen triangle, no vertex buffer.
VsOut VSFullscreen(uint vid : SV_VertexID)
{
    VsOut o;
    const float2 p = float2((vid == 2u) ? 3.0 : -1.0, (vid == 1u) ? 3.0 : -1.0);
    o.pos = float4(p, 0.0, 1.0);
    return o;
}

float4 PSTestPattern(VsOut inp) : SV_Target
{
    const int x = int(inp.pos.x);
    const int y = int(inp.pos.y);
    const int f = int(gFrame);

    // Moving diagonal gradient, 8 bit steps, wrapping.
    float3 c;
    c.r = float((x + y + f * 3) & 255) / 255.0;
    c.g = float((x - y + f * 2) & 255) / 255.0;
    c.b = float((x * 2 - f * 5) & 255) / 255.0;

    // Hard-edged vertical bars, moving one pixel per frame: text-like high contrast edges.
    const uint barX = uint(x + f) % 64u;
    if (y > 80 && y < 200 && barX < 24u) {
        c = float3(0.94, 0.94, 0.94);
    }
    // Horizontal bars of varying width, moving the other way.
    const uint barY = uint(y - f + 1024) % 48u;
    if (x > 300 && x < 700 && barY < 6u) {
        c = float3(0.06, 0.06, 0.06);
    }

    // Static one-pixel checkerboard, the quantiser's worst case.
    if (x >= 800 && x < 960 && y >= 300 && y < 460) {
        c = (((x + y) & 1) != 0) ? float3(1.0, 1.0, 1.0) : float3(0.0, 0.0, 0.0);
    }

    // A block that appears for one frame in sixteen: a scene-change-like event.
    if ((uint(f) % 16u) == 0u && x >= 100 && x < 260 && y >= 420 && y < 560) {
        c = float3(0.0, 0.75, 0.25);
    }

    // Keep the picture inside the requested size even if the viewport is larger.
    if (uint(x) >= gWidth || uint(y) >= gHeight) {
        c = float3(0.0, 0.0, 0.0);
    }
    return float4(c, 1.0);
}
