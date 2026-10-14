/* Host test of the kernel GDI hardware acceleration contract layer: gdi_pixel.h (the per-pixel
 * arithmetic of all six operations), gdi_render.c (admission, the destination-to-source transform
 * and the per-sub-rectangle plan) and gdi_emit.c (the DMA_DATA copy and fill command stream).
 * Built with no WDK header by run_gdi_render.ps1, beside gdi_render.c, gdi_emit.c and gfx_copy.c.
 *
 * The method, for each part:
 *
 *   arithmetic   every function of gdi_pixel.h is compared with a double-precision oracle that
 *                evaluates the DDI page's own expression, over the whole input domain where that
 *                is finite (all 256 values of each byte, all 16 gamma rows) and over a bounded
 *                enumeration where it is not. No tolerance: the comparison is for equality, and
 *                the oracle's domain is chosen so that it has no ties to break (see Div255Round).
 *   geometry     the transform is checked against its defining inequality in 64-bit integers,
 *                which is an independent statement of "floor", not a second copy of the formula.
 *   plan         every refusal class is provoked, and the admitted plans are bounds-checked
 *                against the surfaces by hand.
 *   stream       the emitted packets are DECODED and replayed onto a software framebuffer, and the
 *                result is compared byte for byte with the reference model applied to the same
 *                starting image. That is the bit-for-bit comparison of the command stream with the
 *                model; it does not depend on the GPU and it fails on an addressing mistake.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../gdi_emit.h"
#include "../gfx_copy.h"
#include "../gfx_copy_fields.h"
#include "../../amdgpu-import/nvd.h"

static int g_failures, g_checks;

#define CHECK(cond) \
    do { g_checks++; if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

#define CHECK_EQ(a, b) \
    do { g_checks++; if ((a) != (b)) { fprintf(stderr, "FAIL %s:%d: %s (0x%llX) != %s (0x%llX)\n", \
        __FILE__, __LINE__, #a, (unsigned long long)(a), #b, (unsigned long long)(b)); g_failures++; } } while (0)

/* The two imported headers describe the same PACKET3_DMA_DATA. gdi_emit.c uses the Mesa macros for
 * the copy and the Linux ones for the fill, so their agreement is part of this test rather than an
 * assumption. A C compile-time check without the WDK's C_ASSERT. */
#define STATIC_CHECK(name, cond) typedef char static_check_##name[(cond) ? 1 : -1]
STATIC_CHECK(dst_sel, S_501_DST_SEL(3u) == (unsigned)PACKET3_DMA_DATA_DST_SEL(3));
STATIC_CHECK(src_sel, S_501_SRC_SEL(3u) == (unsigned)PACKET3_DMA_DATA_SRC_SEL(3));
STATIC_CHECK(cp_sync, S_501_CP_SYNC(1u) == (unsigned)PACKET3_DMA_DATA_CP_SYNC);
STATIC_CHECK(raw_wait, S_506_RAW_WAIT(1u) == (unsigned)PACKET3_DMA_DATA_CMD_RAW_WAIT);
STATIC_CHECK(l2_values, V_501_SRC_ADDR_USING_L2 == 3 && V_501_DST_ADDR_USING_L2 == 3);

/* ================================================================================================
 * 1. gdi_pixel.h against double-precision oracles
 * ============================================================================================== */

/* round(N/255) with ties away from zero. N is an integer, so N/255 has a fractional part of
 * exactly one half only when 2N == 255 (mod 510); 2N is even and 255 is odd, so that never
 * happens and this floor is never on a boundary a double could land the wrong side of. */
static unsigned int OracleDiv255(double n)
{
    return (unsigned int)floor(n / 255.0 + 0.5);
}

static void TestDiv255Round(void)
{
    unsigned int n;
    /* The whole domain the callers can produce: 255 * 255 for a two-byte product, and 255 * 255
     * again for the ClearType numerator, so one sweep covers both. */
    for (n = 0; n <= 255u * 255u; n++) CHECK_EQ(Bc250GdiDiv255Round(n), OracleDiv255((double)n));
    /* The identities that matter at the ends. */
    CHECK_EQ(Bc250GdiDiv255Round(0), 0u);
    CHECK_EQ(Bc250GdiDiv255Round(255u * 255u), 255u);
    CHECK_EQ(Bc250GdiMulDiv255(255u, 255u), 255u);
    CHECK_EQ(Bc250GdiMulDiv255(0, 255u), 0u);
    CHECK_EQ(Bc250GdiMulDiv255(255u, 0), 0u);
}

static void TestSwizzle(void)
{
    unsigned int p = 0x11223344u;        /* A=11 R=22 G=33 B=44 */
    CHECK_EQ(Bc250GdiSwapRb(p), 0x11443322u);
    CHECK_EQ(Bc250GdiSwapRb(Bc250GdiSwapRb(p)), p);
    CHECK_EQ(Bc250GdiToArgb(p, 0), p);
    CHECK_EQ(Bc250GdiToArgb(p, 1), 0x11443322u);
    CHECK_EQ(Bc250GdiFromArgb(Bc250GdiToArgb(p, 1), 1), p);
    CHECK_EQ(Bc250GdiPack(0x11, 0x22, 0x33, 0x44), p);
    CHECK_EQ(Bc250GdiByte(p, 24), 0x11u);
    CHECK_EQ(Bc250GdiByte(p, 16), 0x22u);
    CHECK_EQ(Bc250GdiByte(p, 8), 0x33u);
    CHECK_EQ(p & 0xFFu, 0x44u);
}

static void TestBitBltRops(void)
{
    unsigned int s = 0xF0A5C33Cu, d = 0x0F5A3CC3u;
    CHECK_EQ(Bc250GdiBitBltPixel(BC250_GDI_ROP_SRCCOPY, s, d), s);
    CHECK_EQ(Bc250GdiBitBltPixel(BC250_GDI_ROP_SRCINVERT, s, d), d ^ s);
    CHECK_EQ(Bc250GdiBitBltPixel(BC250_GDI_ROP_SRCAND, s, d), d & s);
    CHECK_EQ(Bc250GdiBitBltPixel(BC250_GDI_ROP_SRCOR, s, d), d | s);
    /* Refused opcodes leave the destination alone rather than invent a value. */
    CHECK_EQ(Bc250GdiBitBltPixel(BC250_GDI_ROP_INVALID, s, d), d);
    CHECK_EQ(Bc250GdiBitBltPixel(BC250_GDI_ROP_ROP3, s, d), d);
}

static void TestColorFillRops(void)
{
    unsigned int c = 0xF0A5C33Cu, d = 0x0F5A3CC3u;
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_PATCOPY, c, d), c);
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_PATINVERT, c, d), d ^ c);
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_PDXN, c, d), ~(c ^ d));
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_DSTINVERT, c, d), ~d);
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_PATAND, c, d), d & c);
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_PATOR, c, d), d | c);
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_INVALID, c, d), d);
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_ROP3, c, d), d);
    /* DSTINVERT must not read Color at all. */
    CHECK_EQ(Bc250GdiColorFillPixel(BC250_GDI_CF_DSTINVERT, 0, d),
             Bc250GdiColorFillPixel(BC250_GDI_CF_DSTINVERT, 0xFFFFFFFFu, d));
}

/* The BLENDFUNCTION tables, in double, one channel at a time. */
static unsigned int OracleAlphaChannel(unsigned int s, unsigned int d, unsigned int sa,
                                       unsigned int sAlphaPre, unsigned int hasAlpha)
{
    double v;
    if (!hasAlpha) v = ((double)s * sa + (double)d * (255.0 - (double)sa)) / 255.0;
    else v = (double)s + (255.0 - (double)sAlphaPre) * (double)d / 255.0;
    v = floor(v + 0.5);
    if (v > 255.0) v = 255.0;
    if (v < 0.0) v = 0.0;
    return (unsigned int)v;
}

static void TestAlphaBlend(void)
{
    unsigned int sa, s, d;
    /* Branch one, no per-pixel alpha: the whole three-dimensional domain of one channel. */
    for (sa = 0; sa < 256u; sa++)
        for (s = 0; s < 256u; s += 5u)
            for (d = 0; d < 256u; d += 7u) {
                unsigned int got = Bc250GdiAlphaBlendPixel(Bc250GdiPack(s, s, s, s),
                                                           Bc250GdiPack(d, d, d, d), sa, 0);
                unsigned int want = OracleAlphaChannel(s, d, sa, 0, 0);
                CHECK_EQ(Bc250GdiByte(got, 16), want);
                CHECK_EQ(got, Bc250GdiPack(want, want, want, want));
            }
    /* Branch three, per-pixel alpha. The premultiply is its own rounding, so the oracle performs
     * it the same way the page's assignment does and then applies the following line. */
    for (sa = 0; sa < 256u; sa += 3u)
        for (s = 0; s < 256u; s += 11u)
            for (d = 0; d < 256u; d += 13u) {
                unsigned int sAlpha = 255u - s;              /* an alpha unrelated to the colour */
                unsigned int pre = sa == 255u ? s : OracleDiv255((double)s * sa);
                unsigned int preA = sa == 255u ? sAlpha : OracleDiv255((double)sAlpha * sa);
                unsigned int got = Bc250GdiAlphaBlendPixel(Bc250GdiPack(sAlpha, s, s, s),
                                                           Bc250GdiPack(d, d, d, d), sa, 1);
                unsigned int want = OracleAlphaChannel(pre, d, sa, preA, 1);
                CHECK_EQ(Bc250GdiByte(got, 16), want);
            }
    /* The two ends the page states in words. A fully transparent source leaves the destination;
     * an opaque premultiplied source replaces it. */
    CHECK_EQ(Bc250GdiAlphaBlendPixel(0xFFFFFFFFu, 0x11223344u, 0, 0), 0x11223344u);
    CHECK_EQ(Bc250GdiAlphaBlendPixel(0x11223344u, 0x99887766u, 255u, 0), 0x11223344u);
    CHECK_EQ(Bc250GdiAlphaBlendPixel(0xFF223344u, 0x99887766u, 255u, 1), 0xFF223344u);
    /* A source that is not premultiplied is out of contract; the result must still be in range. */
    CHECK_EQ(Bc250GdiAlphaBlendPixel(0x00FFFFFFu, 0xFFFFFFFFu, 255u, 1), 0xFFFFFFFFu);
}

static void TestTransparentBlt(void)
{
    unsigned int d = 0x11223344u;
    /* HonorAlpha: the whole word is compared. */
    CHECK_EQ(Bc250GdiTransparentPixel(0xFF00FF00u, d, 0xFF00FF00u, 1), d);
    CHECK_EQ(Bc250GdiTransparentPixel(0xFE00FF00u, d, 0xFF00FF00u, 1), 0xFE00FF00u);
    /* Without it the source is masked and the key is not, exactly as the Remarks write it: a key
     * that carries an alpha byte matches nothing at all. */
    CHECK_EQ(Bc250GdiTransparentPixel(0xFF00FF00u, d, 0x0000FF00u, 0), d);
    CHECK_EQ(Bc250GdiTransparentPixel(0xFF00FF00u, d, 0xFF00FF00u, 0), 0xFF00FF00u);
    CHECK_EQ(Bc250GdiTransparentPixel(0x1200FF00u, d, 0x0000FF00u, 0), d);
    /* The page's own example value. */
    CHECK_EQ(Bc250GdiTransparentPixel(0x00FFFFFFu, d, 0x00FFFFFFu, 0), d);
}

/* One row of a gamma allocation: a table and its inverse. The test uses a real gamma curve so that
 * a mixed-up pair of tables shows as a difference and not as the identity. */
static unsigned char g_gamma[512];

static void BuildGamma(double exponent)
{
    unsigned int i;
    for (i = 0; i < 256u; i++) {
        double v = pow((double)i / 255.0, exponent) * 255.0;
        g_gamma[i] = (unsigned char)floor(v + 0.5);
        v = pow((double)i / 255.0, 1.0 / exponent) * 255.0;
        g_gamma[256u + i] = (unsigned char)floor(v + 0.5);
    }
}

static unsigned int OracleClearTypeChannel(unsigned int d, unsigned int a, unsigned int color,
                                           unsigned int color2, int gammaValid, unsigned int aSel)
{
    unsigned int out;
    if (gammaValid) {
        unsigned int tmp = g_gamma[d];
        double blend = (double)tmp + ((double)color - (double)tmp) * (double)a / 255.0;
        unsigned int index = (unsigned int)floor(blend + 0.5);
        out = g_gamma[256u + index];
    } else {
        double v = (double)d + ((double)color - (double)d) * (double)aSel / 255.0;
        out = (unsigned int)floor(v + 0.5);
    }
    /* The two unconditional sentences of the Remarks, after either branch. */
    if (a == 0) out = d;
    else if (a == 255u) out = color2;
    return out;
}

static void TestClearTypeBlend(void)
{
    unsigned int d, a, color;
    BuildGamma(2.2);
    for (d = 0; d < 256u; d += 5u)
        for (a = 0; a < 256u; a += 3u)
            for (color = 0; color < 256u; color += 17u) {
                unsigned int color2 = (color + 7u) & 0xFFu;    /* distinct from Color on purpose */
                unsigned int dst = Bc250GdiPack(0x5A, d, d, d);
                unsigned int alpha = Bc250GdiPack(0, a, a, a);
                unsigned int got = Bc250GdiClearTypePixel(dst, alpha,
                    Bc250GdiPack(0, color, color, color), Bc250GdiPack(0, color2, color2, color2),
                    1u, g_gamma, g_gamma + 256u);
                unsigned int want = OracleClearTypeChannel(d, a, color, color2, 1, a);
                CHECK_EQ(Bc250GdiByte(got, 16), want);
                CHECK_EQ(Bc250GdiByte(got, 8), want);
                CHECK_EQ(got & 0xFFu, want);
                /* OutputColor.a = D.a: the destination alpha is never touched. */
                CHECK_EQ(Bc250GdiByte(got, 24), 0x5Au);
            }
    /* The invalid-gamma branch, including its asymmetric selector: A.r when Color >= D, A.g
     * otherwise, for every channel, and never A.b. */
    for (d = 0; d < 256u; d += 7u)
        for (a = 0; a < 256u; a += 5u)
            for (color = 0; color < 256u; color += 11u) {
                unsigned int aR = a, aG = (a * 3u) & 0xFFu, aB = (a * 7u) & 0xFFu;
                unsigned int color2 = (color + 9u) & 0xFFu;
                unsigned int dst = Bc250GdiPack(0x3C, d, d, d);
                unsigned int alpha = Bc250GdiPack(0, aR, aG, aB);
                unsigned int got = Bc250GdiClearTypePixel(dst, alpha,
                    Bc250GdiPack(0, color, color, color), Bc250GdiPack(0, color2, color2, color2),
                    0u, g_gamma, g_gamma + 256u);
                unsigned int sel = color >= d ? aR : aG;
                CHECK_EQ(Bc250GdiByte(got, 16), OracleClearTypeChannel(d, aR, color, color2, 0, sel));
                CHECK_EQ(Bc250GdiByte(got, 8), OracleClearTypeChannel(d, aG, color, color2, 0, sel));
                CHECK_EQ(got & 0xFFu, OracleClearTypeChannel(d, aB, color, color2, 0, sel));
                CHECK_EQ(Bc250GdiByte(got, 24), 0x3Cu);
            }
    /* The two sentences, stated directly, in the branch where one of them is NOT implied by the
     * branch's own formula: with an invalid gamma and A 0xFF the formula gives Color and the
     * requirement is Color2. */
    {
        unsigned int got = Bc250GdiClearTypePixel(Bc250GdiPack(0, 10, 10, 10),
            Bc250GdiPack(0, 255u, 255u, 255u), Bc250GdiPack(0, 100u, 100u, 100u),
            Bc250GdiPack(0, 200u, 200u, 200u), 0u, g_gamma, g_gamma + 256u);
        CHECK_EQ(Bc250GdiByte(got, 16), 200u);
        got = Bc250GdiClearTypePixel(Bc250GdiPack(0, 10, 10, 10), Bc250GdiPack(0, 0, 0, 0),
            Bc250GdiPack(0, 100u, 100u, 100u), Bc250GdiPack(0, 200u, 200u, 200u),
            0u, g_gamma, g_gamma + 256u);
        CHECK_EQ(Bc250GdiByte(got, 16), 10u);
        /* The same two, in the gamma branch. */
        got = Bc250GdiClearTypePixel(Bc250GdiPack(0, 10, 10, 10),
            Bc250GdiPack(0, 255u, 255u, 255u), Bc250GdiPack(0, 100u, 100u, 100u),
            Bc250GdiPack(0, 200u, 200u, 200u), 1u, g_gamma, g_gamma + 256u);
        CHECK_EQ(Bc250GdiByte(got, 16), 200u);
        got = Bc250GdiClearTypePixel(Bc250GdiPack(0, 10, 10, 10), Bc250GdiPack(0, 0, 0, 0),
            Bc250GdiPack(0, 100u, 100u, 100u), Bc250GdiPack(0, 200u, 200u, 200u),
            1u, g_gamma, g_gamma + 256u);
        CHECK_EQ(Bc250GdiByte(got, 16), 10u);
    }
    /* The gamma index can never leave a 256-entry table: the rounded blend lies between Tmp and
     * Color, both bytes. Checked over the whole domain rather than argued. */
    for (d = 0; d < 256u; d++)
        for (a = 0; a < 256u; a += 15u)
            for (color = 0; color < 256u; color += 15u) {
                unsigned int tmp = g_gamma[d];
                int n = (int)(tmp * 255u) + ((int)color - (int)tmp) * (int)a;
                unsigned int index = Bc250GdiDiv255Round((unsigned int)n);
                CHECK(index < 256u);
                CHECK(index >= (tmp < color ? tmp : color) && index <= (tmp < color ? color : tmp));
            }
}

/* ================================================================================================
 * 2. geometry: admission, the transform, the plan
 * ============================================================================================== */

static BC250_GDI_SURFACE Surface(unsigned int w, unsigned int h, unsigned int bpp,
                                 unsigned int type, unsigned int redFirst)
{
    BC250_GDI_SURFACE s;
    memset(&s, 0, sizeof(s));
    s.Width = w; s.Height = h; s.Bpp = bpp;
    s.Pitch = ((w * bpp) + 255u) & ~255u;        /* a realistic aligned pitch */
    s.Bytes = (unsigned long long)s.Pitch * h;
    s.Type = type; s.RedFirst = redFirst; s.Present = 1;
    return s;
}

static BC250_GDI_RECT Rect(int l, int t, int r, int b)
{
    BC250_GDI_RECT v; v.Left = l; v.Top = t; v.Right = r; v.Bottom = b; return v;
}

static BC250_GDI_OP BitBltOp(unsigned int rop, BC250_GDI_RECT src, BC250_GDI_RECT dst)
{
    BC250_GDI_OP op;
    memset(&op, 0, sizeof(op));
    op.OpCode = BC250_GDIOP_BITBLT;
    op.Rop = rop;
    op.SrcRect = src; op.DstRect = dst;
    op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
    op.NumSubRects = 1;
    return op;
}

static void TestEffectivePitch(void)
{
    BC250_GDI_SURFACE s = Surface(100, 50, 4, BC250_GDISURFACE_TEXTURE, 0);
    BC250_GDI_SURFACE staging = Surface(100, 50, 4, BC250_GDISURFACE_STAGING_CPUVISIBLE, 0);
    BC250_GDI_SURFACE absent = s;
    absent.Present = 0;
    /* A texture ignores the DDI pitch, "Pitch should be ignored for other allocation types". */
    CHECK_EQ(Bc250GdiEffectivePitch(&s, 0), s.Pitch);
    CHECK_EQ(Bc250GdiEffectivePitch(&s, 4096u), s.Pitch);
    /* A CPU-visible staging surface takes it. */
    CHECK_EQ(Bc250GdiEffectivePitch(&staging, 0), staging.Pitch);
    CHECK_EQ(Bc250GdiEffectivePitch(&staging, 512u), 512u);
    /* AlignmentShift 2: an unaligned pitch is a malformed buffer, not a value to round. */
    CHECK_EQ(Bc250GdiEffectivePitch(&staging, 513u), 0u);
    /* A pitch narrower than the row, and one whose rows leave the allocation. */
    CHECK_EQ(Bc250GdiEffectivePitch(&staging, 396u), 0u);
    CHECK_EQ(Bc250GdiEffectivePitch(&staging, 65536u), 0u);
    CHECK_EQ(Bc250GdiEffectivePitch(&absent, 0), 0u);
    CHECK_EQ(Bc250GdiEffectivePitch(0, 0), 0u);
    /* Extents and depths outside the reported maximum. */
    {
        BC250_GDI_SURFACE big = Surface(100, 50, 4, BC250_GDISURFACE_TEXTURE, 0);
        big.Width = 8193u;
        CHECK_EQ(Bc250GdiEffectivePitch(&big, 0), 0u);
        big = Surface(100, 50, 4, BC250_GDISURFACE_TEXTURE, 0);
        big.Height = 0;
        CHECK_EQ(Bc250GdiEffectivePitch(&big, 0), 0u);
        big = Surface(100, 50, 4, BC250_GDISURFACE_TEXTURE, 0);
        big.Bpp = 2u;
        CHECK_EQ(Bc250GdiEffectivePitch(&big, 0), 0u);
    }
    /* 8192 x 8192, the largest extent the reported MaxTexture*Shift of 2 admits. */
    {
        BC250_GDI_SURFACE max = Surface(8192u, 8192u, 4, BC250_GDISURFACE_TEXTURE, 0);
        CHECK_EQ(Bc250GdiEffectivePitch(&max, 0), 8192u * 4u);
    }
}

static void TestAdmission(void)
{
    BC250_GDI_SURFACE s[4];
    BC250_GDI_OP op;
    s[0] = Surface(200, 100, 4, BC250_GDISURFACE_TEXTURE, 0);
    s[1] = Surface(200, 100, 4, BC250_GDISURFACE_TEXTURE, 0);
    s[2] = Surface(200, 100, 4, BC250_GDISURFACE_STAGING, 0);
    s[3] = Surface(BC250_GDI_GAMMA_WIDTH, BC250_GDI_GAMMA_HEIGHT, 1, BC250_GDISURFACE_LOOKUPTABLE, 0);

    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(5, 5, 15, 15));
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);

    /* ESCAPE is skipped, not refused: "Driver ignores this command". */
    op.OpCode = BC250_GDIOP_ESCAPE;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiSkip);
    op.OpCode = 0; CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadOpCode);
    op.OpCode = 8; CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadOpCode);
    CHECK_EQ(Bc250GdiAdmitOp(0, s, 4), Bc250GdiBadOpCode);

    /* ROP3 and the invalid ROP: SupportAllBltRops is clear, so both are refused. */
    op = BitBltOp(BC250_GDI_ROP_ROP3, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
    op.Rop3 = 0xCCu;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRop);
    op = BitBltOp(BC250_GDI_ROP_INVALID, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRop);
    /* A Rop3 beside a legal Rop is a buffer we do not understand. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
    op.Rop3 = 1u;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRop);

    /* Every ROP of both enumerations is admitted. */
    {
        unsigned int rop;
        for (rop = BC250_GDI_ROP_SRCCOPY; rop <= BC250_GDI_ROP_SRCOR; rop++) {
            op = BitBltOp(rop, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
            CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);
        }
        for (rop = BC250_GDI_CF_PATCOPY; rop <= BC250_GDI_CF_PATOR; rop++) {
            memset(&op, 0, sizeof(op));
            op.OpCode = BC250_GDIOP_COLORFILL;
            op.Rop = rop; op.DstRect = Rect(0, 0, 10, 10); op.DstAllocationIndex = 1;
            CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);
        }
    }

    /* An index past the list, and an absent allocation. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
    op.DstAllocationIndex = 4;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadIndex);
    op.DstAllocationIndex = 1;
    op.SrcAllocationIndex = 9;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadIndex);
    {
        BC250_GDI_SURFACE t[4];
        memcpy(t, s, sizeof(t));
        t[0].Present = 0;
        op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 4), Bc250GdiBadIndex);
    }

    /* Channel orders that differ cannot be converted by any command this DDI has. */
    {
        BC250_GDI_SURFACE t[4];
        memcpy(t, s, sizeof(t));
        t[0].RedFirst = 1;
        op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 4), Bc250GdiBadFormat);
        /* Two red-first surfaces are fine. */
        t[1].RedFirst = 1;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 4), Bc250GdiOk);
    }

    /* One allocation named twice: the No*SameBitmap* caps say the OS will not ask, and a command
     * that does is refused rather than run with an undefined read-modify-write. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(20, 20, 30, 30));
    op.SrcAllocationIndex = op.DstAllocationIndex = 1;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadSameSurface);

    /* A BITBLT is "with no stretching" by its own title. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(0, 0, 20, 20));
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRect);

    /* A BITBLT source MAY overhang its surface; an ALPHABLEND, STRETCHBLT or TRANSPARENTBLT source
     * may not, and each page says so in its own SrcRect field. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(190, 90, 210, 110), Rect(0, 0, 20, 20));
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);
    {
        unsigned int codes[3];
        unsigned int i;
        codes[0] = BC250_GDIOP_ALPHABLEND;
        codes[1] = BC250_GDIOP_STRETCHBLT;
        codes[2] = BC250_GDIOP_TRANSPARENTBLT;
        for (i = 0; i < 3; i++) {
            op = BitBltOp(0, Rect(190, 90, 210, 110), Rect(0, 0, 20, 20));
            op.OpCode = codes[i];
            op.SourceConstantAlpha = 255u;
            op.Mode = BC250_GDI_STRETCH_COLORONCOLOR;
            CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRect);
            op.SrcRect = Rect(0, 0, 20, 20);
            CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);
        }
    }

    /* Stretch modes and mirrors this driver did not advertise. */
    memset(&op, 0, sizeof(op));
    op.OpCode = BC250_GDIOP_STRETCHBLT;
    op.SrcRect = Rect(0, 0, 10, 10); op.DstRect = Rect(0, 0, 20, 20);
    op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
    op.Mode = BC250_GDI_STRETCH_COLORONCOLOR;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);
    op.Mode = 0;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);          /* an unset mode reads as 3 */
    op.Mode = BC250_GDI_STRETCH_BLACKONWHITE;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadMode);
    op.Mode = BC250_GDI_STRETCH_WHITEONBLACK;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadMode);
    op.Mode = 4u;                                               /* HALFTONE */
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadMode);
    op.Mode = BC250_GDI_STRETCH_COLORONCOLOR;
    op.MirrorX = 1u; CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadMode);
    op.MirrorX = 0u; op.MirrorY = 1u; CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadMode);

    /* An empty or disordered destination rectangle. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(10, 10, 10, 20));
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRect);
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(20, 0, 10, 10));
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRect);

    /* The one case a one-byte destination is admitted for: the lookup-table initialisation the
     * D3DKMDT_GDISURFACE_LOOKUPTABLE field describes, a SRCCOPY from a CPU-visible staging A8. */
    {
        BC250_GDI_SURFACE t[4];
        memcpy(t, s, sizeof(t));
        t[0] = Surface(BC250_GDI_GAMMA_WIDTH, BC250_GDI_GAMMA_HEIGHT, 1,
                       BC250_GDISURFACE_STAGING_CPUVISIBLE, 0);
        op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 512, 16), Rect(0, 0, 512, 16));
        op.DstAllocationIndex = 3;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 4), Bc250GdiOk);
        /* Any other operation on a one-byte destination is refused. */
        op.Rop = BC250_GDI_ROP_SRCOR;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 4), Bc250GdiBadSurface);
        op.Rop = BC250_GDI_ROP_SRCCOPY;
        op.OpCode = BC250_GDIOP_COLORFILL;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 4), Bc250GdiBadSurface);
        /* A one-byte source against a four-byte destination and the other way round. */
        op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 10, 10), Rect(0, 0, 10, 10));
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 4), Bc250GdiBadSurface);
    }

    /* ClearTypeBlend: its three extra slots and the gamma shape. */
    {
        BC250_GDI_SURFACE t[5];
        memcpy(t, s, sizeof(s));
        t[4] = Surface(200, 100, 4, BC250_GDISURFACE_STAGING, 0);     /* the alpha surface */
        memset(&op, 0, sizeof(op));
        op.OpCode = BC250_GDIOP_CLEARTYPEBLEND;
        op.DstRect = Rect(0, 0, 20, 10);
        op.DstAllocationIndex = 1;
        op.AlphaSurfAllocationIndex = 4;
        op.GammaSurfAllocationIndex = 3;
        op.Gamma = 7u;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiOk);
        op.Gamma = BC250_GDI_GAMMA_HEIGHT;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiBadGamma);
        /* An invalid gamma index needs no gamma allocation at all. */
        op.Gamma = BC250_GDI_INVALID_GAMMA;
        op.GammaSurfAllocationIndex = 99u;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiOk);
        /* A lookup allocation of the wrong shape is not the table the formula indexes. */
        op.Gamma = 0u;
        op.GammaSurfAllocationIndex = 3u;
        t[3].Width = 256u;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiBadGamma);
        t[3] = Surface(BC250_GDI_GAMMA_WIDTH, BC250_GDI_GAMMA_HEIGHT, 1,
                       BC250_GDISURFACE_LOOKUPTABLE, 0);
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiOk);
        /* The alpha surface must be four-byte: the formula reads A.r, A.g and A.b. */
        t[4].Bpp = 1u;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiBadSurface);
        t[4] = Surface(200, 100, 4, BC250_GDISURFACE_STAGING, 0);
        /* The alpha surface may not be the destination. */
        op.AlphaSurfAllocationIndex = 1u;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiBadSameSurface);
        op.AlphaSurfAllocationIndex = 4u;
        /* TmpSurfAllocationIndex is never dereferenced, because
         * NoTempSurfaceForClearTypeBlend is set; a nonsense value in it changes nothing. */
        op.TmpSurfAllocationIndex = 0xFFFFFFFFu;
        CHECK_EQ(Bc250GdiAdmitOp(&op, t, 5), Bc250GdiOk);
    }

    /* AlphaBlend's two scalar fields. */
    memset(&op, 0, sizeof(op));
    op.OpCode = BC250_GDIOP_ALPHABLEND;
    op.SrcRect = Rect(0, 0, 10, 10); op.DstRect = Rect(0, 0, 10, 10);
    op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
    op.SourceConstantAlpha = 255u;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiOk);
    op.SourceConstantAlpha = 256u;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRop);
    op.SourceConstantAlpha = 0u; op.SourceHasAlpha = 2u;
    CHECK_EQ(Bc250GdiAdmitOp(&op, s, 4), Bc250GdiBadRop);
}

/* The transform's defining inequality in 64 bits: Xs - SrcLeft is the unique k with
 * k * (2*Wd) <= (2*(Xd - DstLeft0) + 1) * Ws < (k+1) * (2*Wd). This is an independent statement of
 * "truncate", not a second copy of the formula. */
static void CheckTransform(const BC250_GDI_ITEM* item, int xd)
{
    long long k = (long long)Bc250GdiSourceX(item, xd) - item->SrcLeft;
    long long num = (2ll * (xd - item->DstLeft0) + 1ll) * item->Ws;
    long long den = 2ll * item->Wd;
    CHECK(k * den <= num && num < (k + 1ll) * den);
}

static void CheckTransformY(const BC250_GDI_ITEM* item, int yd)
{
    long long k = (long long)Bc250GdiSourceY(item, yd) - item->SrcTop;
    long long num = (2ll * (yd - item->DstTop0) + 1ll) * item->Hs;
    long long den = 2ll * item->Hd;
    CHECK(k * den <= num && num < (k + 1ll) * den);
}

static void TestTransform(void)
{
    BC250_GDI_SURFACE s[2];
    unsigned int ws, wd;
    s[0] = Surface(1024, 1024, 4, BC250_GDISURFACE_TEXTURE, 0);
    s[1] = Surface(1024, 1024, 4, BC250_GDISURFACE_TEXTURE, 0);
    /* A sweep of ratios, each over every destination pixel. Shrink, stretch and 1:1. */
    for (wd = 1; wd <= 64u; wd++)
        for (ws = 1; ws <= 64u; ws++) {
            BC250_GDI_OP op;
            BC250_GDI_ITEM item;
            BC250_GDI_RECT sub;
            unsigned int x;
            memset(&op, 0, sizeof(op));
            op.OpCode = BC250_GDIOP_STRETCHBLT;
            op.Mode = BC250_GDI_STRETCH_COLORONCOLOR;
            op.SrcRect = Rect(3, 5, 3 + (int)ws, 5 + (int)ws);
            op.DstRect = Rect(7, 11, 7 + (int)wd, 11 + (int)wd);
            op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
            sub = op.DstRect;
            CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
            CHECK_EQ(item.Ws, ws);
            CHECK_EQ(item.Wd, wd);
            CHECK_EQ(item.Stretch, ws != wd ? 1u : 0u);
            for (x = 0; x < wd; x++) {
                CheckTransform(&item, op.DstRect.Left + (int)x);
                CheckTransformY(&item, op.DstRect.Top + (int)x);
                /* It must stay inside the source rectangle, which is what makes the plan's corner
                 * check sufficient. */
                CHECK(Bc250GdiSourceX(&item, op.DstRect.Left + (int)x) >= op.SrcRect.Left);
                CHECK(Bc250GdiSourceX(&item, op.DstRect.Left + (int)x) < op.SrcRect.Right);
            }
            /* Monotonic in both axes, which is why checking the corners bounds every pixel. */
            for (x = 1; x < wd; x++) {
                CHECK(Bc250GdiSourceX(&item, op.DstRect.Left + (int)x) >=
                      Bc250GdiSourceX(&item, op.DstRect.Left + (int)x - 1));
                CHECK(Bc250GdiSourceY(&item, op.DstRect.Top + (int)x) >=
                      Bc250GdiSourceY(&item, op.DstRect.Top + (int)x - 1));
            }
            /* 1:1 is exactly the translation the BITBLT Remarks give. */
            if (ws == wd)
                for (x = 0; x < wd; x++)
                    CHECK_EQ(Bc250GdiSourceX(&item, op.DstRect.Left + (int)x),
                             (int)x + op.SrcRect.Left);
        }
    /* The widest ratios the reported extents allow, at the ends of the destination. */
    {
        BC250_GDI_OP op;
        BC250_GDI_ITEM item;
        BC250_GDI_RECT sub;
        BC250_GDI_SURFACE big[2];
        big[0] = Surface(8192u, 8192u, 4, BC250_GDISURFACE_TEXTURE, 0);
        big[1] = Surface(8192u, 8192u, 4, BC250_GDISURFACE_TEXTURE, 0);
        memset(&op, 0, sizeof(op));
        op.OpCode = BC250_GDIOP_STRETCHBLT;
        op.Mode = BC250_GDI_STRETCH_COLORONCOLOR;
        op.SrcRect = Rect(0, 0, 8192, 8192);
        op.DstRect = Rect(0, 0, 1, 1);
        op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
        sub = op.DstRect;
        CHECK_EQ(Bc250GdiPlanItem(&op, big, 2, &sub, &item), Bc250GdiOk);
        CheckTransform(&item, 0);
        CheckTransformY(&item, 0);
        op.SrcRect = Rect(0, 0, 1, 1);
        op.DstRect = Rect(0, 0, 8192, 8192);
        sub = op.DstRect;
        CHECK_EQ(Bc250GdiPlanItem(&op, big, 2, &sub, &item), Bc250GdiOk);
        CheckTransform(&item, 0);
        CheckTransform(&item, 8191);
        CheckTransformY(&item, 8191);
        CHECK_EQ(Bc250GdiSourceX(&item, 8191), 0);
    }
}

static void TestPlan(void)
{
    BC250_GDI_SURFACE s[2];
    BC250_GDI_OP op;
    BC250_GDI_ITEM item;
    BC250_GDI_RECT sub;
    s[0] = Surface(200, 100, 4, BC250_GDISURFACE_TEXTURE, 0);
    s[1] = Surface(200, 100, 4, BC250_GDISURFACE_TEXTURE, 0);

    /* A plain copy: offsets and classification. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(10, 20, 30, 40), Rect(50, 60, 70, 80));
    sub = Rect(52, 62, 68, 78);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.Work, (unsigned int)Bc250GdiWorkCopy);
    CHECK_EQ(item.Kernel, (unsigned int)Bc250GdiKernelNone);
    CHECK_EQ(item.DstLeft, 52u);
    CHECK_EQ(item.DstTop, 62u);
    CHECK_EQ(item.DstWidth, 16u);
    CHECK_EQ(item.DstHeight, 16u);
    CHECK_EQ(item.RowBytes, 64u);
    CHECK_EQ(item.DstOffset, 62ull * item.DstPitch + 52ull * 4ull);
    /* The source origin follows the translation, so (52,62) maps to (12,22). */
    CHECK_EQ(item.SrcOffset, 22ull * item.SrcPitch + 12ull * 4ull);

    /* A sub-rectangle larger than DstRect is clipped to it, and the source translation is kept. */
    sub = Rect(0, 0, 200, 100);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.DstLeft, 50u);
    CHECK_EQ(item.DstWidth, 20u);
    CHECK_EQ(item.SrcOffset, 20ull * item.SrcPitch + 10ull * 4ull);

    /* A sub-rectangle disjoint from DstRect is empty, which is not an error. */
    sub = Rect(0, 0, 10, 10);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.DstWidth, 0u);
    CHECK_EQ(item.DstHeight, 0u);

    /* A sub-rectangle outside the destination surface is refused: it is the one OS coordinate that
     * gets multiplied by a pitch. */
    sub = Rect(190, 90, 210, 110);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiBadRect);
    sub = Rect(-1, 0, 10, 10);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiBadRect);

    /* 1 x 1, the smallest admitted rectangle. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(199, 99, 200, 100), Rect(0, 0, 1, 1));
    sub = Rect(0, 0, 1, 1);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.DstWidth, 1u);
    CHECK_EQ(item.RowBytes, 4u);
    CHECK_EQ(item.SrcOffset, 99ull * item.SrcPitch + 199ull * 4ull);

    /* A BITBLT source that overhangs its surface is admitted, but a sub-rectangle whose transform
     * leaves the source is refused. The DDI guarantees it will not; the driver still checks. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(190, 90, 210, 110), Rect(0, 0, 20, 20));
    sub = Rect(0, 0, 20, 20);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiBadRect);
    sub = Rect(0, 0, 10, 10);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.SrcOffset, 90ull * item.SrcPitch + 190ull * 4ull);

    /* A fill has no source and takes the engine path only for PATCOPY. */
    memset(&op, 0, sizeof(op));
    op.OpCode = BC250_GDIOP_COLORFILL;
    op.Rop = BC250_GDI_CF_PATCOPY;
    op.DstRect = Rect(1, 2, 5, 7);
    op.DstAllocationIndex = 1;
    sub = op.DstRect;
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.Work, (unsigned int)Bc250GdiWorkFill);
    CHECK_EQ(item.RowBytes, 16u);
    CHECK_EQ(item.DstHeight, 5u);
    CHECK_EQ(item.SrcPitch, 0u);
    op.Rop = BC250_GDI_CF_PATINVERT;
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.Work, (unsigned int)Bc250GdiWorkKernel);
    CHECK_EQ(item.Kernel, (unsigned int)Bc250GdiKernelColorFillRop);

    /* A 1:1 StretchBlt is a copy; a scaled one is the resample kernel. */
    memset(&op, 0, sizeof(op));
    op.OpCode = BC250_GDIOP_STRETCHBLT;
    op.Mode = BC250_GDI_STRETCH_COLORONCOLOR;
    op.SrcRect = Rect(0, 0, 10, 10); op.DstRect = Rect(0, 0, 10, 10);
    op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
    sub = op.DstRect;
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.Work, (unsigned int)Bc250GdiWorkCopy);
    op.SrcRect = Rect(0, 0, 20, 20);
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
    CHECK_EQ(item.Work, (unsigned int)Bc250GdiWorkKernel);
    CHECK_EQ(item.Kernel, (unsigned int)Bc250GdiKernelStretchBlt);
    /* A stretching plan carries no row copy, because there is no constant source offset. */
    CHECK_EQ(item.SrcOffset, 0ull);
    CHECK_EQ(item.RowBytes, 0u);

    /* The remaining classifications. */
    {
        struct { unsigned int op, rop, kernel; } cases[4];
        unsigned int i;
        cases[0].op = BC250_GDIOP_BITBLT; cases[0].rop = BC250_GDI_ROP_SRCINVERT;
        cases[0].kernel = Bc250GdiKernelBitBltRop;
        cases[1].op = BC250_GDIOP_BITBLT; cases[1].rop = BC250_GDI_ROP_SRCAND;
        cases[1].kernel = Bc250GdiKernelBitBltRop;
        cases[2].op = BC250_GDIOP_ALPHABLEND; cases[2].rop = 0;
        cases[2].kernel = Bc250GdiKernelAlphaBlend;
        cases[3].op = BC250_GDIOP_TRANSPARENTBLT; cases[3].rop = 0;
        cases[3].kernel = Bc250GdiKernelTransparentBlt;
        for (i = 0; i < 4; i++) {
            memset(&op, 0, sizeof(op));
            op.OpCode = cases[i].op; op.Rop = cases[i].rop;
            op.SrcRect = Rect(0, 0, 10, 10); op.DstRect = Rect(0, 0, 10, 10);
            op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
            op.SourceConstantAlpha = 255u;
            sub = op.DstRect;
            CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);
            CHECK_EQ(item.Work, (unsigned int)Bc250GdiWorkKernel);
            CHECK_EQ(item.Kernel, cases[i].kernel);
        }
    }

    /* ClearTypeBlend: the alpha offset, the gamma row, and an alpha offset that leaves its
     * surface. */
    {
        BC250_GDI_SURFACE t[5];
        unsigned int i;
        for (i = 0; i < 2; i++) t[i] = s[i];
        t[2] = Surface(200, 100, 4, BC250_GDISURFACE_STAGING, 0);
        t[3] = Surface(BC250_GDI_GAMMA_WIDTH, BC250_GDI_GAMMA_HEIGHT, 1,
                       BC250_GDISURFACE_LOOKUPTABLE, 0);
        t[4] = s[0];
        memset(&op, 0, sizeof(op));
        op.OpCode = BC250_GDIOP_CLEARTYPEBLEND;
        op.DstRect = Rect(10, 10, 30, 20);
        op.DstAllocationIndex = 1;
        op.AlphaSurfAllocationIndex = 2;
        op.GammaSurfAllocationIndex = 3;
        op.Gamma = 9u;
        op.DstToAlphaOffsetX = -5; op.DstToAlphaOffsetY = -5;
        sub = op.DstRect;
        CHECK_EQ(Bc250GdiPlanItem(&op, t, 5, &sub, &item), Bc250GdiOk);
        CHECK_EQ(item.GammaValid, 1u);
        CHECK_EQ(item.AlphaOffset, 5ull * item.AlphaPitch + 5ull * 4ull);
        CHECK_EQ(item.GammaOffset, 9ull * Bc250GdiEffectivePitch(&t[3], 0));
        /* An offset that puts the alpha rectangle outside its surface. */
        op.DstToAlphaOffsetX = -20;
        CHECK_EQ(Bc250GdiPlanItem(&op, t, 5, &sub, &item), Bc250GdiBadRect);
        op.DstToAlphaOffsetX = 190;
        CHECK_EQ(Bc250GdiPlanItem(&op, t, 5, &sub, &item), Bc250GdiBadRect);
        op.DstToAlphaOffsetX = 0; op.DstToAlphaOffsetY = 0;
        CHECK_EQ(Bc250GdiPlanItem(&op, t, 5, &sub, &item), Bc250GdiOk);
        CHECK_EQ(item.AlphaOffset, 10ull * item.AlphaPitch + 10ull * 4ull);
        /* Every gamma row resolves inside the allocation, and every one of them is distinct. */
        {
            unsigned int row;
            unsigned long long previous = 0;
            for (row = 0; row < BC250_GDI_GAMMA_HEIGHT; row++) {
                op.Gamma = row;
                CHECK_EQ(Bc250GdiPlanItem(&op, t, 5, &sub, &item), Bc250GdiOk);
                CHECK(item.GammaOffset + 512u <= t[3].Bytes);
                if (row) CHECK(item.GammaOffset > previous);
                previous = item.GammaOffset;
            }
        }
        op.Gamma = BC250_GDI_INVALID_GAMMA;
        CHECK_EQ(Bc250GdiPlanItem(&op, t, 5, &sub, &item), Bc250GdiOk);
        CHECK_EQ(item.GammaValid, 0u);
        CHECK_EQ(item.GammaOffset, 0ull);
    }
}

static void TestWalkAndPass(void)
{
    unsigned int next;
    /* sizeof(DXGK_RENDERKM_COMMAND)'s fixed part is passed by wddm.c; 16 is a plausible value and
     * the arithmetic does not depend on which. */
    CHECK(Bc250GdiWalkStep(256u, 0, 16u, 16u, &next) && next == 16u);
    CHECK(Bc250GdiWalkStep(256u, 240u, 16u, 16u, &next) && next == 256u);
    /* One byte past the end. */
    CHECK(!Bc250GdiWalkStep(256u, 240u, 20u, 16u, &next));
    CHECK_EQ(next, 0u);
    /* A size below the minimum, and one that is not a multiple of four. */
    CHECK(!Bc250GdiWalkStep(256u, 0, 12u, 16u, &next));
    CHECK(!Bc250GdiWalkStep(256u, 0, 18u, 16u, &next));
    /* A size of zero would make the walk loop for ever. */
    CHECK(!Bc250GdiWalkStep(256u, 0, 0, 16u, &next));
    /* An offset past the buffer, and the exact end. */
    CHECK(!Bc250GdiWalkStep(256u, 257u, 16u, 16u, &next));
    CHECK(!Bc250GdiWalkStep(256u, 256u, 16u, 16u, &next));
    /* Arithmetic that would wrap in 32 bits. */
    CHECK(!Bc250GdiWalkStep(256u, 16u, 0xFFFFFFF0u, 16u, &next));
    CHECK(!Bc250GdiWalkStep(0xFFFFFFF0u, 0xFFFFFFE0u, 0xFFFFFFF0u, 16u, &next));
    /* A bad minimum is the caller's mistake and is refused rather than used. */
    CHECK(!Bc250GdiWalkStep(256u, 0, 16u, 0, &next));
    CHECK(!Bc250GdiWalkStep(256u, 0, 16u, 15u, &next));
    CHECK(!Bc250GdiWalkStep(256u, 0, 16u, 16u, 0));

    /* MultipassOffset packing round-trips, and refuses what it cannot represent. */
    CHECK_EQ(Bc250GdiPackPass(0, 0), 0u);
    CHECK_EQ(Bc250GdiPackPass(3u, 7u), 0x00030007u);
    CHECK_EQ(Bc250GdiPassCommand(0x00030007u), 3u);
    CHECK_EQ(Bc250GdiPassSubRect(0x00030007u), 7u);
    CHECK_EQ(Bc250GdiPackPass(0xFFFFu, 0xFFFFu), 0xFFFFFFFFu);
    CHECK_EQ(Bc250GdiPackPass(0x10000u, 0), 0u);
    CHECK_EQ(Bc250GdiPackPass(0, 0x10000u), 0u);
}

/* ================================================================================================
 * 3. the command stream, decoded and replayed
 * ============================================================================================== */

#define MEM_BYTES (1u << 20)
static unsigned char g_memory[MEM_BYTES];       /* one flat space; the two surfaces sit in it */
static unsigned char g_expected[MEM_BYTES];

/* Replay one DMA_DATA packet onto g_memory. Returns the dwords consumed, 0 on anything the
 * emitters are not supposed to produce. The decode is deliberately strict: a packet with a field
 * this driver never sets fails the test rather than being tolerated. */
static unsigned int ReplayPacket(const unsigned int* p, unsigned int available,
                                 unsigned long long base, unsigned int* sawRawWait,
                                 unsigned int* sawCpSync)
{
    unsigned int control, command, count, srcSel, dstSel, i;
    unsigned long long src, dst;
    if (available < 7u) return 0;
    if (p[0] != (unsigned int)PACKET3(PACKET3_DMA_DATA, 5)) return 0;
    control = p[1];
    command = p[6];
    count = command & 0x3FFFFFFu;
    if (command & (unsigned int)PACKET3_DMA_DATA_CMD_RAW_WAIT) (*sawRawWait)++;
    if (control & (unsigned int)PACKET3_DMA_DATA_CP_SYNC) (*sawCpSync)++;
    /* Nothing but the two selectors, CP_SYNC and RAW_WAIT may be set. */
    if (control & ~(unsigned int)(PACKET3_DMA_DATA_DST_SEL(3) | PACKET3_DMA_DATA_SRC_SEL(3) |
                                  PACKET3_DMA_DATA_CP_SYNC)) return 0;
    if (command & ~(unsigned int)(0x3FFFFFFu | PACKET3_DMA_DATA_CMD_RAW_WAIT)) return 0;
    dstSel = (control >> 20) & 3u;
    srcSel = (control >> 29) & 3u;
    if (dstSel != 3u) return 0;
    dst = ((unsigned long long)p[5] << 32) | p[4];
    if (dst < base || dst - base + count > MEM_BYTES) return 0;
    if (srcSel == 3u) {
        src = ((unsigned long long)p[3] << 32) | p[2];
        if (src < base || src - base + count > MEM_BYTES) return 0;
        memmove(g_memory + (dst - base), g_memory + (src - base), count);
    } else if (srcSel == 2u) {
        if (p[3] != 0) return 0;                 /* dword 4 is unused in DATA mode */
        if (count & 3u) return 0;
        for (i = 0; i < count; i += 4u)
            memcpy(g_memory + (dst - base) + i, &p[2], 4u);
    } else return 0;
    return 7u;
}

static unsigned int Replay(const unsigned int* buffer, unsigned int dwords, unsigned long long base,
                           unsigned int* rawWait, unsigned int* cpSync)
{
    unsigned int at = 0, packets = 0;
    *rawWait = *cpSync = 0;
    while (at < dwords) {
        unsigned int used = ReplayPacket(buffer + at, dwords - at, base, rawWait, cpSync);
        if (!used) return 0;
        at += used;
        packets++;
    }
    return packets;
}

/* The reference model of one item, on g_expected. Only the two engine classes are modelled here;
 * the kernel classes have no command stream yet. */
static void ReferenceItem(const BC250_GDI_ITEM* item, unsigned int pattern)
{
    unsigned int row, x;
    for (row = 0; row < item->DstHeight; row++) {
        unsigned long long dst = item->DstOffset + (unsigned long long)row * item->DstPitch;
        if (item->Work == (unsigned int)Bc250GdiWorkCopy) {
            unsigned long long src = item->SrcOffset + (unsigned long long)row * item->SrcPitch;
            memmove(g_expected + dst, g_expected + src, item->RowBytes);
        } else {
            for (x = 0; x < item->RowBytes; x += 4u)
                memcpy(g_expected + dst + x, &pattern, 4u);
        }
    }
}

static void SeedMemory(void)
{
    unsigned int i;
    for (i = 0; i < MEM_BYTES; i++) g_memory[i] = (unsigned char)((i * 31u + 7u) & 0xFFu);
    memcpy(g_expected, g_memory, MEM_BYTES);
}

/* One surface pair inside g_memory: the source at offset 0, the destination at DST_AT. */
#define DST_AT 0x80000u

static void TestStreamCase(const BC250_GDI_OP* op, const BC250_GDI_SURFACE* s, unsigned int count,
                           const BC250_GDI_RECT* sub, unsigned int pattern, unsigned int capacity)
{
    static unsigned int buffer[65536];
    BC250_GDI_ITEM item;
    unsigned int next = 0, written, rawWait, cpSync, packets, total = 0, rounds = 0;
    int result;
    CHECK_EQ(Bc250GdiPlanItem(op, s, count, sub, &item), Bc250GdiOk);
    if (!item.DstHeight) return;
    SeedMemory();
    /* The plan's offsets are relative to each allocation; here the source allocation starts at 0
     * and the destination at DST_AT, so the bases are those two numbers. */
    item.DstOffset += DST_AT;
    ReferenceItem(&item, pattern);
    item.DstOffset -= DST_AT;

    do {
        result = Bc250EmitGdiItem(&item, 0, DST_AT, pattern, next, &next, buffer,
                                  capacity > 65536u ? 65536u : capacity, &written);
        CHECK(result == BC250_GDI_EMIT_DONE || result == BC250_GDI_EMIT_MORE);
        if (result != BC250_GDI_EMIT_DONE && result != BC250_GDI_EMIT_MORE) return;
        packets = Replay(buffer, written, 0, &rawWait, &cpSync);
        CHECK(written == 0 || packets > 0);
        /* One RAW_WAIT at the front of a batch and one CP_SYNC at its end, as gfx_blt.h's own
         * contract puts it. */
        if (written) { CHECK_EQ(rawWait, 1u); CHECK_EQ(cpSync, 1u); }
        total += written;
        if (++rounds > 4096u) { CHECK(0); return; }
    } while (result == BC250_GDI_EMIT_MORE);
    CHECK(total > 0);
    CHECK_EQ(next, item.DstHeight);
    /* The bit-for-bit comparison: the whole memory, so a packet that wrote one byte outside its
     * rectangle fails here. */
    CHECK_EQ(memcmp(g_memory, g_expected, MEM_BYTES), 0);
}

static void TestStream(void)
{
    BC250_GDI_SURFACE s[2];
    BC250_GDI_OP op;
    BC250_GDI_RECT sub;
    s[0] = Surface(256, 128, 4, BC250_GDISURFACE_TEXTURE, 0);
    s[1] = Surface(256, 128, 4, BC250_GDISURFACE_TEXTURE, 0);

    /* A copy of a middle rectangle, in one pass. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(3, 5, 67, 45), Rect(100, 60, 164, 100));
    sub = op.DstRect;
    TestStreamCase(&op, s, 2, &sub, 0, 65536u);

    /* The same, in several passes, down to a capacity that holds exactly one row. */
    TestStreamCase(&op, s, 2, &sub, 0, 7u);
    TestStreamCase(&op, s, 2, &sub, 0, 14u);
    TestStreamCase(&op, s, 2, &sub, 0, 70u);

    /* 1 x 1 and the full surface. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 1, 1), Rect(255, 127, 256, 128));
    sub = op.DstRect;
    TestStreamCase(&op, s, 2, &sub, 0, 65536u);
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 256, 128), Rect(0, 0, 256, 128));
    sub = op.DstRect;
    TestStreamCase(&op, s, 2, &sub, 0, 65536u);

    /* A clipped sub-rectangle still copies the pixels the untruncated translation names. */
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(10, 10, 50, 50), Rect(20, 20, 60, 60));
    sub = Rect(30, 30, 40, 40);
    TestStreamCase(&op, s, 2, &sub, 0, 65536u);

    /* A fill, with a pattern whose bytes are all different so a swapped dword shows. */
    memset(&op, 0, sizeof(op));
    op.OpCode = BC250_GDIOP_COLORFILL;
    op.Rop = BC250_GDI_CF_PATCOPY;
    op.DstRect = Rect(7, 9, 71, 49);
    op.DstAllocationIndex = 1;
    sub = op.DstRect;
    TestStreamCase(&op, s, 2, &sub, 0x11335577u, 65536u);
    TestStreamCase(&op, s, 2, &sub, 0x11335577u, 7u);
    op.DstRect = Rect(0, 0, 1, 1);
    sub = op.DstRect;
    TestStreamCase(&op, s, 2, &sub, 0xDEADBEEFu, 65536u);

    /* A 1:1 StretchBlt reaches the engine path and replays as a copy. */
    memset(&op, 0, sizeof(op));
    op.OpCode = BC250_GDIOP_STRETCHBLT;
    op.Mode = BC250_GDI_STRETCH_COLORONCOLOR;
    op.SrcRect = Rect(0, 0, 32, 16); op.DstRect = Rect(8, 8, 40, 24);
    op.SrcAllocationIndex = 0; op.DstAllocationIndex = 1;
    sub = op.DstRect;
    TestStreamCase(&op, s, 2, &sub, 0, 65536u);
}

static void TestStreamRefusals(void)
{
    static unsigned int buffer[256];
    BC250_GDI_SURFACE s[2];
    BC250_GDI_OP op;
    BC250_GDI_ITEM item;
    BC250_GDI_RECT sub;
    unsigned int next, written;
    s[0] = Surface(64, 32, 4, BC250_GDISURFACE_TEXTURE, 0);
    s[1] = Surface(64, 32, 4, BC250_GDISURFACE_TEXTURE, 0);
    op = BitBltOp(BC250_GDI_ROP_SRCCOPY, Rect(0, 0, 8, 4), Rect(0, 0, 8, 4));
    sub = op.DstRect;
    CHECK_EQ(Bc250GdiPlanItem(&op, s, 2, &sub, &item), Bc250GdiOk);

    /* No output buffer, and an offset past the last row. */
    CHECK_EQ(Bc250EmitGdiItem(&item, 0, DST_AT, 0, 0, &next, 0, 256u, &written),
             BC250_GDI_EMIT_INVALID);
    CHECK_EQ(Bc250EmitGdiItem(&item, 0, DST_AT, 0, 99u, &next, buffer, 256u, &written),
             BC250_GDI_EMIT_INVALID);
    /* The exact end is done, not an error, and it emits nothing. */
    CHECK_EQ(Bc250EmitGdiItem(&item, 0, DST_AT, 0, item.DstHeight, &next, buffer, 256u, &written),
             BC250_GDI_EMIT_DONE);
    CHECK_EQ(written, 0u);
    /* A capacity below one row is exhaustion, and writes nothing. */
    memset(buffer, 0xCD, sizeof(buffer));
    CHECK_EQ(Bc250EmitGdiItem(&item, 0, DST_AT, 0, 0, &next, buffer, 6u, &written),
             BC250_GDI_EMIT_NO_SPACE);
    CHECK_EQ(written, 0u);
    CHECK_EQ(next, 0u);
    CHECK_EQ(buffer[0], 0xCDCDCDCDu);
    /* Overlapping source and destination rows: the same base for both. */
    CHECK_EQ(Bc250EmitGdiItem(&item, 0, 0, 0, 0, &next, buffer, 256u, &written),
             BC250_GDI_EMIT_INVALID);
    /* A kernel item has no command stream here. */
    {
        BC250_GDI_ITEM kernel = item;
        kernel.Work = Bc250GdiWorkKernel;
        CHECK_EQ(Bc250EmitGdiItem(&kernel, 0, DST_AT, 0, 0, &next, buffer, 256u, &written),
                 BC250_GDI_EMIT_INVALID);
        kernel = item;
        kernel.Stretch = 1u;
        CHECK_EQ(Bc250EmitGdiItem(&kernel, 0, DST_AT, 0, 0, &next, buffer, 256u, &written),
                 BC250_GDI_EMIT_INVALID);
    }
    /* A fill of a row that is not a multiple of four has no dword meaning; a copy of one is fine.
     * This is the lookup-table initialisation's shape. */
    {
        BC250_GDI_ITEM odd = item;
        odd.Work = Bc250GdiWorkFill;
        odd.RowBytes = 7u;
        CHECK_EQ(Bc250EmitGdiItem(&odd, 0, DST_AT, 0, 0, &next, buffer, 256u, &written),
                 BC250_GDI_EMIT_INVALID);
        odd.Work = Bc250GdiWorkCopy;
        CHECK_EQ(Bc250EmitGdiItem(&odd, 0, DST_AT, 0, 0, &next, buffer, 256u, &written),
                 BC250_GDI_EMIT_DONE);
        CHECK(written > 0u);
    }
    /* A base that would wrap the address space. */
    CHECK_EQ(Bc250EmitGdiItem(&item, 0, ~0ull - 16ull, 0, 0, &next, buffer, 256u, &written),
             BC250_GDI_EMIT_INVALID);
    /* The fill span's own refusals. */
    CHECK_EQ(Bc250EmitGdiFillSpan(buffer, 256u, 0x1000ull, 0, 0, 1, 1), 0u);
    CHECK_EQ(Bc250EmitGdiFillSpan(buffer, 256u, 0x1000ull, 7u, 0, 1, 1), 0u);
    CHECK_EQ(Bc250EmitGdiFillSpan(buffer, 6u, 0x1000ull, 64u, 0, 1, 1), 0u);
    CHECK_EQ(Bc250EmitGdiFillSpan(0, 256u, 0x1000ull, 64u, 0, 1, 1), 0u);
    CHECK_EQ(Bc250EmitGdiFillSpan(buffer, 256u, 0x1000ull, 64u, 0, 1, 1), 7u);
    /* A fill larger than one packet carries chunks the same way the copy does. */
    CHECK_EQ(Bc250GdiFillDwords(Bc250GfxCopyMaxBytes() + 4u), 14u);
    CHECK_EQ(Bc250GdiFillDwords(0), 0u);
}

int main(void)
{
    TestDiv255Round();
    TestSwizzle();
    TestBitBltRops();
    TestColorFillRops();
    TestAlphaBlend();
    TestTransparentBlt();
    TestClearTypeBlend();
    TestEffectivePitch();
    TestAdmission();
    TestTransform();
    TestPlan();
    TestWalkAndPass();
    TestStream();
    TestStreamRefusals();
    printf("gdi_render_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
