// Import pass: brings one source picture into the encoder's own planar 8-bit buffers, padding the
// right and bottom edges out to the coded macroblock multiple by replication, and converting BGRA to
// BT.709 studio-range Y'CbCr 4:2:0 when the input is RGB. This is the colour-convert compute pass.
//
// One thread owns an 8x2 luma block, which makes every store a single aligned 32-bit word: four luma
// words and one word in each chroma plane. rwY/rwCb/rwCr are bound to the source planes for this pass.

#include "h264_common.hlsli"

uint2 ClampSrc(int x, int y)
{
    return uint2(uint(clamp(x, 0, int(gSrcWidth) - 1)), uint(clamp(y, 0, int(gSrcHeight) - 1)));
}

[numthreads(8, 8, 1)]
void CSImportNV12(uint3 tid : SV_DispatchThreadID)
{
    const uint bx = tid.x * 8u;        // luma x, a multiple of 8
    const uint by = tid.y * 2u;        // luma y, a multiple of 2
    if (bx >= gPadW || by >= gPadH) {
        return;
    }

    uint row;
    for (row = 0; row < 2; ++row) {
        uint s[8];
        [unroll] for (uint i = 0; i < 8; ++i) {
            const uint2 p = ClampSrc(int(bx + i), int(by + row));
            s[i] = uint(round(texNv12Y[p] * 255.0f));
        }
        const uint base = (by + row) * gPadW + bx;
        rwY.Store(base, PackBytes(s[0], s[1], s[2], s[3]));
        rwY.Store(base + 4u, PackBytes(s[4], s[5], s[6], s[7]));
    }

    const uint cw = gSrcWidth >> 1;
    const uint ch = gSrcHeight >> 1;
    uint cb[4];
    uint cr[4];
    [unroll] for (uint i = 0; i < 4; ++i) {
        const uint px = min((bx >> 1) + i, cw - 1u);
        const uint py = min(by >> 1, ch - 1u);
        const float2 uv = texNv12UV[uint2(px, py)];
        cb[i] = uint(round(uv.x * 255.0f));
        cr[i] = uint(round(uv.y * 255.0f));
    }
    const uint cbase = (by >> 1) * (gPadW >> 1) + (bx >> 1);
    rwCb.Store(cbase, PackBytes(cb[0], cb[1], cb[2], cb[3]));
    rwCr.Store(cbase, PackBytes(cr[0], cr[1], cr[2], cr[3]));
}

// BT.709 studio range, the matrix the sequence parameter set advertises.
void RgbToYuv(float3 rgb, out int y, out int u, out int v)
{
    const int r = int(round(rgb.r * 255.0f));
    const int g = int(round(rgb.g * 255.0f));
    const int b = int(round(rgb.b * 255.0f));
    y = clamp((( 47 * r + 157 * g +  16 * b + 128) >> 8) +  16,  16, 235);
    u = clamp(((-26 * r -  87 * g + 112 * b + 128) >> 8) + 128,  16, 240);
    v = clamp(((112 * r - 102 * g -  10 * b + 128) >> 8) + 128,  16, 240);
}

[numthreads(8, 8, 1)]
void CSImportBGRA(uint3 tid : SV_DispatchThreadID)
{
    const uint bx = tid.x * 8u;
    const uint by = tid.y * 2u;
    if (bx >= gPadW || by >= gPadH) {
        return;
    }

    int yv[2][8];
    int uu[2][8];
    int vv[2][8];
    uint row;
    for (row = 0; row < 2; ++row) {
        [unroll] for (uint i = 0; i < 8; ++i) {
            const uint2 p = ClampSrc(int(bx + i), int(by + row));
            const float4 c = texBgra[p];
            RgbToYuv(c.rgb, yv[row][i], uu[row][i], vv[row][i]);
        }
        const uint base = (by + row) * gPadW + bx;
        rwY.Store(base, PackBytes(uint(yv[row][0]), uint(yv[row][1]),
                                  uint(yv[row][2]), uint(yv[row][3])));
        rwY.Store(base + 4u, PackBytes(uint(yv[row][4]), uint(yv[row][5]),
                                       uint(yv[row][6]), uint(yv[row][7])));
    }

    uint cb[4];
    uint cr[4];
    [unroll] for (uint i = 0; i < 4; ++i) {
        const uint a = i * 2u;
        const int su = uu[0][a] + uu[0][a + 1] + uu[1][a] + uu[1][a + 1];
        const int sv = vv[0][a] + vv[0][a + 1] + vv[1][a] + vv[1][a + 1];
        cb[i] = uint(clamp((su + 2) >> 2, 0, 255));
        cr[i] = uint(clamp((sv + 2) >> 2, 0, 255));
    }
    const uint cbase = (by >> 1) * (gPadW >> 1) + (bx >> 1);
    rwCb.Store(cbase, PackBytes(cb[0], cb[1], cb[2], cb[3]));
    rwCr.Store(cbase, PackBytes(cr[0], cr[1], cr[2], cr[3]));
}

// System memory NV12: bufInY is the luma plane, bufInCb the interleaved CbCr plane. Deinterleaving on
// the GPU keeps the CPU out of the per-sample path; the upload is a single memcpy of each plane.
[numthreads(8, 8, 1)]
void CSImportNV12Sys(uint3 tid : SV_DispatchThreadID)
{
    const uint bx = tid.x * 8u;
    const uint by = tid.y * 2u;
    if (bx >= gPadW || by >= gPadH) {
        return;
    }
    uint row;
    for (row = 0; row < 2; ++row) {
        uint s[8];
        [unroll] for (uint i = 0; i < 8; ++i) {
            const uint2 p = ClampSrc(int(bx + i), int(by + row));
            s[i] = LoadByteRO(bufInY, p.y * gSrcWidth + p.x);
        }
        const uint base = (by + row) * gPadW + bx;
        rwY.Store(base, PackBytes(s[0], s[1], s[2], s[3]));
        rwY.Store(base + 4u, PackBytes(s[4], s[5], s[6], s[7]));
    }
    const uint cw = gSrcWidth >> 1;
    const uint ch = gSrcHeight >> 1;
    uint cb[4];
    uint cr[4];
    [unroll] for (uint i = 0; i < 4; ++i) {
        const uint px = min((bx >> 1) + i, cw - 1u);
        const uint py = min(by >> 1, ch - 1u);
        const uint off = (py * cw + px) * 2u;
        cb[i] = LoadByteRO(bufInCb, off);
        cr[i] = LoadByteRO(bufInCb, off + 1u);
    }
    const uint cbase = (by >> 1) * (gPadW >> 1) + (bx >> 1);
    rwCb.Store(cbase, PackBytes(cb[0], cb[1], cb[2], cb[3]));
    rwCr.Store(cbase, PackBytes(cr[0], cr[1], cr[2], cr[3]));
}

// Planar input uploaded as Y, Cb, Cr byte planes of the visible size. This pass only replicates the
// edges out to the coded size, so a system memory I420 frame is never colour converted.
[numthreads(8, 8, 1)]
void CSImportPlanar(uint3 tid : SV_DispatchThreadID)
{
    const uint bx = tid.x * 8u;
    const uint by = tid.y * 2u;
    if (bx >= gPadW || by >= gPadH) {
        return;
    }
    uint row;
    for (row = 0; row < 2; ++row) {
        uint s[8];
        [unroll] for (uint i = 0; i < 8; ++i) {
            const uint2 p = ClampSrc(int(bx + i), int(by + row));
            s[i] = LoadByteRO(bufInY, p.y * gSrcWidth + p.x);
        }
        const uint base = (by + row) * gPadW + bx;
        rwY.Store(base, PackBytes(s[0], s[1], s[2], s[3]));
        rwY.Store(base + 4u, PackBytes(s[4], s[5], s[6], s[7]));
    }
    const uint cw = gSrcWidth >> 1;
    const uint ch = gSrcHeight >> 1;
    uint cb[4];
    uint cr[4];
    [unroll] for (uint i = 0; i < 4; ++i) {
        const uint px = min((bx >> 1) + i, cw - 1u);
        const uint py = min(by >> 1, ch - 1u);
        cb[i] = LoadByteRO(bufInCb, py * cw + px);
        cr[i] = LoadByteRO(bufInCr, py * cw + px);
    }
    const uint cbase = (by >> 1) * (gPadW >> 1) + (bx >> 1);
    rwCb.Store(cbase, PackBytes(cb[0], cb[1], cb[2], cb[3]));
    rwCr.Store(cbase, PackBytes(cr[0], cr[1], cr[2], cr[3]));
}
