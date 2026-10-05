// SPDX-License-Identifier: MIT
// Shared declarations for the BC-250 H.264 encoder compute passes.
//
// The GPU side owns everything that is per-block independent: colour conversion, motion estimation,
// intra and inter prediction, the forward 4x4 transform and the DC Hadamard transforms, quantisation,
// the normative dequantisation and inverse transform, reconstruction and (in cs_deblock.hlsl) the
// deblocking filter. It never touches a bit of the bitstream.
//
// Pictures live in ByteAddressBuffers of 8-bit samples, three planes per picture, luma stride gPadW
// and chroma stride gPadW/2. Buffers rather than textures because a codec needs exact integer
// arithmetic and byte addressing, not filtering: no texture unit is of any use here, and typed UAV
// stores to R8_UNORM are not a guaranteed Direct3D 11 capability.
//
// The MbInfo and levels layouts mirror ../src/mb_layout.h word for word. Change both together.

#ifndef BC250_H264_COMMON_HLSLI
#define BC250_H264_COMMON_HLSLI

cbuffer EncodeConstants : register(b0)
{
    uint gWidthMb;
    uint gHeightMb;
    uint gPadW;          // coded luma width, a multiple of 16
    uint gPadH;          // coded luma height, a multiple of 16
    uint gQpY;           // luma QP for this picture, 0..51
    uint gQpC;           // chroma QP derived through Table 8-15
    uint gIsIntra;       // 1: I picture
    uint gDiagonalBase;  // first macroblock column (intra) or row (deblock) on the wave
    uint gDiagonal;      // wave index: mbx + mby for the intra pass, mbx + 2 * mby for deblocking
    uint gLambda;        // motion cost weight, in SAD units per motion vector component step
    uint gSkipBias;      // SAD slack for snapping a motion vector back to (0,0)
    uint gSrcWidth;      // visible luma width (the import pass pads out to gPadW by replication)
    uint gSrcHeight;
    int  gAlphaOffsetDiv2;
    int  gBetaOffsetDiv2;
    uint gDeblockIdc;    // disable_deblocking_filter_idc for this picture
};

// ---------------------------------------------------------------------------------------------------
// Resources. One map for every pass; a pass that does not use a slot simply never reads it.
//   t0..t5  input picture, in whichever form the client handed it over
//   t6..t8  the encoder's own source planes (written by the import pass)
//   t9..t11 the reference picture planes (the previous reconstruction, after deblocking)
//   t12     macroblock info, read-only (deblocking needs the modes of neighbours)
//   u0..u2  the import pass writes the source planes here; later passes write the reconstruction
//   u3      quantised levels
//   u4      macroblock info
// ---------------------------------------------------------------------------------------------------
Texture2D<float>  texNv12Y   : register(t0);
Texture2D<float2> texNv12UV  : register(t1);
Texture2D<float4> texBgra    : register(t2);
ByteAddressBuffer bufInY     : register(t3);
ByteAddressBuffer bufInCb    : register(t4);
ByteAddressBuffer bufInCr    : register(t5);
ByteAddressBuffer bufSrcY    : register(t6);
ByteAddressBuffer bufSrcCb   : register(t7);
ByteAddressBuffer bufSrcCr   : register(t8);
ByteAddressBuffer bufRefY    : register(t9);
ByteAddressBuffer bufRefCb   : register(t10);
ByteAddressBuffer bufRefCr   : register(t11);
ByteAddressBuffer bufMbInfoRO : register(t12);

RWByteAddressBuffer rwY       : register(u0);
RWByteAddressBuffer rwCb      : register(u1);
RWByteAddressBuffer rwCr      : register(u2);
RWByteAddressBuffer rwLevels  : register(u3);
RWByteAddressBuffer rwMbInfo  : register(u4);

// ---------------------------------------------------------------------------------------------------
// Tables of the standard. Mirrors of ../src/h264_tables.cpp.
// ---------------------------------------------------------------------------------------------------
static const uint kNormAdjust[6][3] = {
    { 10, 16, 13 }, { 11, 18, 14 }, { 13, 20, 16 },
    { 14, 23, 18 }, { 16, 25, 20 }, { 18, 29, 23 },
};
static const uint kQuantCoef[6][3] = {
    { 13107, 5243, 8066 }, { 11916, 4660, 7490 }, { 10082, 4194, 6554 },
    {  9362, 3647, 5825 }, {  8192, 3355, 5243 }, {  7282, 2893, 4559 },
};
// clause 8.5.6 zig-zag scan: kZigZag[scanPos] = raster index inside the 4x4 block.
static const uint kZigZag[16] = { 0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15 };

// Scaling class: 0 both indices even, 1 both odd, 2 otherwise.
uint PosClass(uint i, uint j)
{
    const uint ie = i & 1u;
    const uint je = j & 1u;
    if (ie == 0u && je == 0u) return 0u;
    if (ie == 1u && je == 1u) return 1u;
    return 2u;
}

// ---------------------------------------------------------------------------------------------------
// Byte addressing helpers
// ---------------------------------------------------------------------------------------------------
uint LoadByteRO(ByteAddressBuffer b, uint addr)
{
    return (b.Load(addr & ~3u) >> ((addr & 3u) << 3u)) & 0xFFu;
}
uint LoadByteRW(RWByteAddressBuffer b, uint addr)
{
    return (b.Load(addr & ~3u) >> ((addr & 3u) << 3u)) & 0xFFu;
}
uint PackBytes(uint b0, uint b1, uint b2, uint b3)
{
    return (b0 & 0xFFu) | ((b1 & 0xFFu) << 8u) | ((b2 & 0xFFu) << 16u) | ((b3 & 0xFFu) << 24u);
}

uint Clip255(int v) { return uint(clamp(v, 0, 255)); }

// Reference and reconstruction sample access, clamped to the coded picture as clause 8.4.2.2.1 requires.
uint RefY(int x, int y)
{
    const int cx = clamp(x, 0, int(gPadW) - 1);
    const int cy = clamp(y, 0, int(gPadH) - 1);
    return LoadByteRO(bufRefY, uint(cy) * gPadW + uint(cx));
}
uint RefCb(int x, int y)
{
    const int cx = clamp(x, 0, int(gPadW >> 1) - 1);
    const int cy = clamp(y, 0, int(gPadH >> 1) - 1);
    return LoadByteRO(bufRefCb, uint(cy) * (gPadW >> 1) + uint(cx));
}
uint RefCr(int x, int y)
{
    const int cx = clamp(x, 0, int(gPadW >> 1) - 1);
    const int cy = clamp(y, 0, int(gPadH >> 1) - 1);
    return LoadByteRO(bufRefCr, uint(cy) * (gPadW >> 1) + uint(cx));
}

// ---------------------------------------------------------------------------------------------------
// Luma quarter sample interpolation, clause 8.4.2.2.1 and Table 8-12.
// mv is in quarter luma sample units; (px,py) is the position in the current picture.
// ---------------------------------------------------------------------------------------------------
int Tap6(int a, int b, int c, int d, int e, int f)
{
    return a - 5 * b + 20 * c + 20 * d - 5 * e + f;
}
// Half sample "b": horizontal, between (x,y) and (x+1,y).
int B1(int x, int y)
{
    return Tap6(int(RefY(x - 2, y)), int(RefY(x - 1, y)), int(RefY(x, y)),
                int(RefY(x + 1, y)), int(RefY(x + 2, y)), int(RefY(x + 3, y)));
}
// Half sample "h": vertical, between (x,y) and (x,y+1).
int H1(int x, int y)
{
    return Tap6(int(RefY(x, y - 2)), int(RefY(x, y - 1)), int(RefY(x, y)),
                int(RefY(x, y + 1)), int(RefY(x, y + 2)), int(RefY(x, y + 3)));
}
int J1(int x, int y)
{
    return Tap6(B1(x, y - 2), B1(x, y - 1), B1(x, y), B1(x, y + 1), B1(x, y + 2), B1(x, y + 3));
}

// The named sample positions of Table 8-12, computed on demand:
//   b: half sample horizontally between (xi,yi) and (xi+1,yi)
//   h: half sample vertically   between (xi,yi) and (xi,yi+1)
//   m: like h but at column xi+1        s: like b but at row yi+1
//   j: the centre, the vertical 6-tap of the b intermediates
// One of the four half-sample grid positions at integer position (x,y): (0,0) is the integer sample
// G, (1,0) is b, (0,1) is h, (1,1) is the centre j.
uint HalfSample(int x, int y, uint hx, uint hy)
{
    uint v;
    if (hy == 0u) {
        v = (hx == 0u) ? RefY(x, y) : Clip255((B1(x, y) + 16) >> 5);
    } else {
        v = (hx == 0u) ? Clip255((H1(x, y) + 16) >> 5) : Clip255((J1(x, y) + 512) >> 10);
    }
    return v;
}

// Table 8-12 without sixteen separate expressions. Write xf = 2*hx + ox and yf = 2*hy + oy: a
// position with ox == oy == 0 is one of the four half-sample positions; one odd component is the
// average of two neighbouring half samples along that axis; both odd is the average of b below and h
// to the right, which is exactly what the table gives for e, g, p and r.
//
// Sixteen explicit cases (one per fractional position) made fxc 10.1 fail code generation with
// "internal error: unexpected input register type" at every optimisation level above /Od, because
// this function is called from inside the 256-sample loops of motion estimation and inter
// prediction. This form is also far smaller.
uint InterpLuma(int px, int py, int mvx, int mvy)
{
    const int xi = px + (mvx >> 2);
    const int yi = py + (mvy >> 2);
    const uint xf = uint(mvx) & 3u;
    const uint yf = uint(mvy) & 3u;
    const uint hx = xf >> 1u;
    const uint hy = yf >> 1u;
    const uint ox = xf & 1u;
    const uint oy = yf & 1u;
    const int nx = (xf == 3u) ? 1 : 0;
    const int ny = (yf == 3u) ? 1 : 0;

    uint result;
    if (ox == 0u && oy == 0u) {
        result = HalfSample(xi, yi, hx, hy);
    } else if (oy == 0u) {
        result = (HalfSample(xi, yi, hx, hy) + HalfSample(xi + nx, yi, 1u - hx, hy) + 1u) >> 1u;
    } else if (ox == 0u) {
        result = (HalfSample(xi, yi, hx, hy) + HalfSample(xi, yi + ny, hx, 1u - hy) + 1u) >> 1u;
    } else {
        result = (HalfSample(xi, yi + ny, 1u, 0u) + HalfSample(xi + nx, yi, 0u, 1u) + 1u) >> 1u;
    }
    return result;
}

// Chroma eighth sample interpolation, clause 8.4.2.2.2. For 4:2:0 mvC equals mvL and its fractional
// part is in eighths of a chroma sample. comp 0 is Cb, comp 1 is Cr.
uint InterpChroma(uint comp, int px, int py, int mvx, int mvy)
{
    const int xi = px + (mvx >> 3);
    const int yi = py + (mvy >> 3);
    const int xf = int(uint(mvx) & 7u);
    const int yf = int(uint(mvy) & 7u);
    int A, B, C, D;
    if (comp == 0u) {
        A = int(RefCb(xi, yi));     B = int(RefCb(xi + 1, yi));
        C = int(RefCb(xi, yi + 1)); D = int(RefCb(xi + 1, yi + 1));
    } else {
        A = int(RefCr(xi, yi));     B = int(RefCr(xi + 1, yi));
        C = int(RefCr(xi, yi + 1)); D = int(RefCr(xi + 1, yi + 1));
    }
    return uint(((8 - xf) * (8 - yf) * A + xf * (8 - yf) * B +
                 (8 - xf) * yf * C + xf * yf * D + 32) >> 6);
}

// ---------------------------------------------------------------------------------------------------
// Quantisation and the normative inverse steps (clauses 8.5.9 to 8.5.12)
// ---------------------------------------------------------------------------------------------------
// Forward quantisation. Not normative: only the inverse is. Levels are clamped to +-2047 so that
// level_prefix never needs the clause 9.2.2 escape beyond 15, and the clamp happens here so that the
// reconstruction below uses exactly the levels the entropy coder will write.
int Quant4x4(int coef, uint qp, uint cls, bool intra)
{
    const uint qbits = 15u + qp / 6u;
    const uint f = (1u << qbits) / (intra ? 3u : 6u);
    const int a = abs(coef);
    int lev = int((uint(a) * kQuantCoef[qp % 6u][cls] + f) >> qbits);
    lev = min(lev, 2047);
    return coef < 0 ? -lev : lev;
}
// Same for the Intra_16x16 luma DC block, after the 4x4 Hadamard of clause 8.5.10's inverse.
//
// The shift is derived from the normative inverse, not copied from a reference encoder. Write H for
// the plain +-1 4x4 Hadamard, so H H = 4 I. Let D be the array of the sixteen block DC coefficients
// (each one the sum of the sixteen residual samples of its 4x4 block, the forward core transform's
// DC gain being 16), F = H D H the forward Hadamard of that array, and c = F / g the levels for some
// scale g. The decoder then computes f = H c H = 16 D / g (8.5.10), dcY = f * 16N << q6 >> 6 with
// N = normAdjust4x4(qp%6, 0, 0) and q6 = qp/6, and finally the 4x4 inverse of a DC-only block, whose
// last step is (h + 32) >> 6, so each reconstructed sample is dcY / 64 and the block DC it carries is
// 16 * dcY / 64 = dcY / 4. Reproducing D therefore needs dcY = 4 D, that is
//   4 D = (16 D / g) * 16 N * 2^q6 / 2^6   =>   g = N * 2^q6.
// With N * kQuantCoef = 2^17 that is a multiply by kQuantCoef and a shift of 17 + q6 = qbits + 2.
// A shift of qbits + 1 (which is what the chroma DC below needs, and what one gets by analogy with
// it) halves every level and doubles the reconstructed DC: at qp 26 that drove a flat 16x16 block to
// -99 before clipping, so every macroblock of a smooth ramp reconstructed as 0.
int QuantLumaDc(int coef, uint qp, bool intra)
{
    const uint qbits = 15u + qp / 6u;
    const uint f = (1u << qbits) / (intra ? 3u : 6u);
    const int a = abs(coef);
    int lev = int((uint(a) * kQuantCoef[qp % 6u][0] + 4u * f) >> (qbits + 2u));
    lev = min(lev, 2047);
    return coef < 0 ? -lev : lev;
}
// The chroma DC, after the 2x2 Hadamard of clause 8.5.11. The same derivation with the 2x2 Hadamard
// (gain 4, H2 H2 = 2 I) and clause 8.5.11.2's dcC = f * 16N << q6 >> 5 gives g = N * 2^q6 / 2, one
// bit less than the luma DC above.
int QuantChromaDc(int coef, uint qp, bool intra)
{
    const uint qbits = 15u + qp / 6u;
    const uint f = (1u << qbits) / (intra ? 3u : 6u);
    const int a = abs(coef);
    int lev = int((uint(a) * kQuantCoef[qp % 6u][0] + 2u * f) >> (qbits + 1u));
    lev = min(lev, 2047);
    return coef < 0 ? -lev : lev;
}

// clause 8.5.12.1, flat scaling list: LevelScale4x4 = 16 * normAdjust4x4.
int Dequant4x4(int level, uint qp, uint cls)
{
    const int ls = int(16u * kNormAdjust[qp % 6u][cls]);
    const uint q6 = qp / 6u;
    if (q6 >= 4u) {
        return (level * ls) << (q6 - 4u);
    }
    return (level * ls + (1 << (3u - q6))) >> (4u - q6);
}
// clause 8.5.10 scaling of the Intra_16x16 luma DC, after the inverse Hadamard transform.
int DequantLumaDc(int f, uint qp)
{
    const int ls = int(16u * kNormAdjust[qp % 6u][0]);
    const uint q6 = qp / 6u;
    if (q6 >= 6u) {
        return (f * ls) << (q6 - 6u);
    }
    return (f * ls + (1 << (5u - q6))) >> (6u - q6);
}
// clause 8.5.11.2 scaling of the chroma DC, after the 2x2 inverse Hadamard transform.
int DequantChromaDc(int f, uint qp)
{
    const int ls = int(16u * kNormAdjust[qp % 6u][0]);
    return ((f * ls) << (qp / 6u)) >> 5;
}

// Forward 4x4 core transform, Cf X Cf^T.
void Forward4x4(inout int b[16])
{
    int i;
    for (i = 0; i < 4; ++i) {
        const int x0 = b[i * 4 + 0], x1 = b[i * 4 + 1], x2 = b[i * 4 + 2], x3 = b[i * 4 + 3];
        const int s0 = x0 + x3, s1 = x1 + x2, s2 = x1 - x2, s3 = x0 - x3;
        b[i * 4 + 0] = s0 + s1;
        b[i * 4 + 1] = 2 * s3 + s2;
        b[i * 4 + 2] = s0 - s1;
        b[i * 4 + 3] = s3 - 2 * s2;
    }
    for (i = 0; i < 4; ++i) {
        const int x0 = b[0 * 4 + i], x1 = b[1 * 4 + i], x2 = b[2 * 4 + i], x3 = b[3 * 4 + i];
        const int s0 = x0 + x3, s1 = x1 + x2, s2 = x1 - x2, s3 = x0 - x3;
        b[0 * 4 + i] = s0 + s1;
        b[1 * 4 + i] = 2 * s3 + s2;
        b[2 * 4 + i] = s0 - s1;
        b[3 * 4 + i] = s3 - 2 * s2;
    }
}

// clause 8.5.12.2 inverse transform, before the final (x + 32) >> 6.
void Inverse4x4(inout int d[16])
{
    int i;
    for (i = 0; i < 4; ++i) {
        const int d0 = d[i * 4 + 0], d1 = d[i * 4 + 1], d2 = d[i * 4 + 2], d3 = d[i * 4 + 3];
        const int e0 = d0 + d2, e1 = d0 - d2, e2 = (d1 >> 1) - d3, e3 = d1 + (d3 >> 1);
        d[i * 4 + 0] = e0 + e3;
        d[i * 4 + 1] = e1 + e2;
        d[i * 4 + 2] = e1 - e2;
        d[i * 4 + 3] = e0 - e3;
    }
    for (i = 0; i < 4; ++i) {
        const int f0 = d[0 * 4 + i], f1 = d[1 * 4 + i], f2 = d[2 * 4 + i], f3 = d[3 * 4 + i];
        const int g0 = f0 + f2, g1 = f0 - f2, g2 = (f1 >> 1) - f3, g3 = f1 + (f3 >> 1);
        d[0 * 4 + i] = g0 + g3;
        d[1 * 4 + i] = g1 + g2;
        d[2 * 4 + i] = g1 - g2;
        d[3 * 4 + i] = g0 - g3;
    }
}

// clause 8.5.10: f = H c H with H = {{1,1,1,1},{1,1,-1,-1},{1,-1,-1,1},{1,-1,1,-1}}. The forward
// transform uses the same matrix; the factor of 16 is absorbed by the quantiser.
// The 4x4 Hadamard of clause 8.5.10, used for the Intra_16x16 luma DC block in both directions
// (the matrix is its own transpose and the forward and inverse differ only by a scale factor the
// quantiser carries). The output row order matters: the matrix is
//     1  1  1  1
//     1  1 -1 -1
//     1 -1 -1  1
//     1 -1  1 -1
// and the butterfly below produces exactly those four rows in that order. A butterfly that emits
// them as rows 0, 2, 3, 1 - which is what the core transform's butterfly looks like - still gives a
// self-consistent encoder, but its reconstruction disagrees with every decoder, because the
// bitstream carries the coefficients in the array positions the standard defines.
void Hadamard4x4(inout int b[16])
{
    int i;
    for (i = 0; i < 4; ++i) {
        const int x0 = b[i * 4 + 0], x1 = b[i * 4 + 1], x2 = b[i * 4 + 2], x3 = b[i * 4 + 3];
        const int a0 = x0 + x3, a1 = x1 + x2, a2 = x1 - x2, a3 = x0 - x3;
        b[i * 4 + 0] = a0 + a1;
        b[i * 4 + 1] = a3 + a2;
        b[i * 4 + 2] = a0 - a1;
        b[i * 4 + 3] = a3 - a2;
    }
    for (i = 0; i < 4; ++i) {
        const int x0 = b[0 * 4 + i], x1 = b[1 * 4 + i], x2 = b[2 * 4 + i], x3 = b[3 * 4 + i];
        const int a0 = x0 + x3, a1 = x1 + x2, a2 = x1 - x2, a3 = x0 - x3;
        b[0 * 4 + i] = a0 + a1;
        b[1 * 4 + i] = a3 + a2;
        b[2 * 4 + i] = a0 - a1;
        b[3 * 4 + i] = a3 - a2;
    }
}

// ---------------------------------------------------------------------------------------------------
// MbInfo and levels layout (../src/mb_layout.h)
// ---------------------------------------------------------------------------------------------------
static const uint kMbInfoWords = 16u;
static const uint kLevelsWordsPerMb = 204u;
static const uint kLevelsOffLumaDc = 0u;
static const uint kLevelsOffLumaAc = 8u;
static const uint kLevelsOffChromaDc = 136u;
static const uint kLevelsOffChromaAc = 140u;

uint PackLevels(int lo, int hi)
{
    return (uint(lo) & 0xFFFFu) | ((uint(hi) & 0xFFFFu) << 16u);
}

#endif // BC250_H264_COMMON_HLSLI
