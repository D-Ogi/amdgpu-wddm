/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_PIXEL_H
#define BC250_GDI_PIXEL_H
/*
 * The per-pixel arithmetic of the six GDI hardware-accelerated operations, as one body of integer
 * code that compiles BOTH as kernel/host C and as OpenCL C for gfx1013. That is the whole point of
 * this file: the GPU kernel and the host reference model are the same source text, so a host test
 * that proves the arithmetic proves the arithmetic the GPU runs, and the two cannot drift.
 *
 * Contract (local copies, revisions in docs/design/gdi-hw-accel.md):
 *   ref/ddi-display/d3dkmddi.md  DXGK_GDIROP_BITBLT, DXGK_GDIROP_COLORFILL, DXGK_GDIARG_ALPHABLEND,
 *                                DXGK_GDIARG_TRANSPARENTBLT, DXGK_GDIARG_CLEARTYPEBLEND Remarks
 *   The ClearType and TransparentBlt formulas below are the Remarks verbatim, in integers.
 *   The DDI pages give NO formula for AlphaBlend; the Win32 BLENDFUNCTION rule is used and named
 *   where it is applied.
 *
 * Everything here is pure: no memory of its own, no pointers except the two gamma rows, no float.
 * Float is avoided on purpose. The contract's formulas are written with `/ 255.0` and `round()`,
 * and a float pipeline would make the result depend on the rounding mode of whichever unit ran it;
 * the integer forms below are exact for the whole input domain and are proved so by the host test,
 * which enumerates all 256 alpha values against a double-precision oracle.
 */

#ifdef __OPENCL_VERSION__
typedef uint  bc250_u32;
typedef int   bc250_i32;
#define BC250_GDI_FN static inline
#define BC250_GDI_U8PTR const __global uchar*
#else
typedef unsigned int bc250_u32;
typedef int          bc250_i32;
#define BC250_GDI_FN static __inline
#define BC250_GDI_U8PTR const unsigned char*
#endif

/* DXGK_GDIROP_BITBLT, d3dkmddi.h line 480. ROP3 (5) is admitted only by a driver that set
 * SupportAllBltRops; ours does not, so it is refused before it reaches this file. */
#define BC250_GDI_ROP_INVALID   0u
#define BC250_GDI_ROP_SRCCOPY   1u
#define BC250_GDI_ROP_SRCINVERT 2u
#define BC250_GDI_ROP_SRCAND    3u
#define BC250_GDI_ROP_SRCOR     4u
#define BC250_GDI_ROP_ROP3      5u

/* DXGK_GDIROP_COLORFILL, d3dkmddi.h line 504. Same note about ROP3 (7). */
#define BC250_GDI_CF_INVALID    0u
#define BC250_GDI_CF_PATCOPY    1u
#define BC250_GDI_CF_PATINVERT  2u
#define BC250_GDI_CF_PDXN       3u
#define BC250_GDI_CF_DSTINVERT  4u
#define BC250_GDI_CF_PATAND     5u
#define BC250_GDI_CF_PATOR      6u
#define BC250_GDI_CF_ROP3       7u

/* D3DKM_INVALID_GAMMA_INDEX, d3dkmddi.h line 586. */
#define BC250_GDI_INVALID_GAMMA 0xFFFFFFFFu

/* ---- channel order ------------------------------------------------------------------------------
 * Canonical here is ARGB: A<<24 | R<<16 | G<<8 | B, which is exactly D3DDDIFMT_A8R8G8B8, the format
 * every colour in the DDI structures is documented in (Color, Color2, the TransparentBlt key).
 * A8B8G8R8/X8B8G8R8 surfaces hold the same channels with R and B exchanged - wddm.c calls that
 * WddmRedFirst, because red is the first byte in memory - so one swap converts either way.
 */
BC250_GDI_FN bc250_u32 Bc250GdiSwapRb(bc250_u32 Pixel)
{
    return (Pixel & 0xFF00FF00u) | ((Pixel & 0x00FF0000u) >> 16) | ((Pixel & 0x000000FFu) << 16);
}

BC250_GDI_FN bc250_u32 Bc250GdiToArgb(bc250_u32 Raw, bc250_u32 RedFirst)
{
    return RedFirst ? Bc250GdiSwapRb(Raw) : Raw;
}

BC250_GDI_FN bc250_u32 Bc250GdiFromArgb(bc250_u32 Argb, bc250_u32 RedFirst)
{
    return RedFirst ? Bc250GdiSwapRb(Argb) : Argb;
}

/* ---- exact rational arithmetic ------------------------------------------------------------------
 * round(N / 255) for an unsigned N whose quotient fits a byte, with ties away from zero, which is
 * what C's round() does and what the ClearType Remarks write. floor((2N + 255) / 510) is that
 * function exactly: the quotient increases at N = 255k + 128, which is the first N whose remainder
 * is at least half. No division survives in the generated code for the uses below, because the
 * compiler turns a constant divisor into a multiply-shift; it is written as a division so the host
 * test and the kernel share one text.
 */
BC250_GDI_FN bc250_u32 Bc250GdiDiv255Round(bc250_u32 N)
{
    return (2u * N + 255u) / 510u;
}

/* round(A * B / 255) for two bytes: the "premultiply by SourceConstantAlpha" step of
 * BLENDFUNCTION, and the alpha product of its third table. */
BC250_GDI_FN bc250_u32 Bc250GdiMulDiv255(bc250_u32 A, bc250_u32 B)
{
    return Bc250GdiDiv255Round(A * B);
}

BC250_GDI_FN bc250_u32 Bc250GdiByte(bc250_u32 Pixel, bc250_u32 Shift)
{
    return (Pixel >> Shift) & 0xFFu;
}

BC250_GDI_FN bc250_u32 Bc250GdiPack(bc250_u32 A, bc250_u32 R, bc250_u32 G, bc250_u32 B)
{
    return (A << 24) | (R << 16) | (G << 8) | B;
}

/* ---- BitBlt -------------------------------------------------------------------------------------
 * DXGK_GDIROP_BITBLT, the whole enumeration, on the raw surface words. These four are bitwise and
 * channel order does not enter them, so no conversion is done: a bitwise operation on two words in
 * the same channel order gives the same answer in that order. The caller guarantees the two
 * surfaces have the same order, which is a condition of admission (a BitBlt between an A8R8G8B8 and
 * an A8B8G8R8 surface would need the source converted first, and the DDI has no way to ask for it;
 * such a pair is refused with STATUS_GRAPHICS_CANNOTCOLORCONVERT).
 */
BC250_GDI_FN bc250_u32 Bc250GdiBitBltPixel(bc250_u32 Rop, bc250_u32 Src, bc250_u32 Dst)
{
    switch (Rop) {
    case BC250_GDI_ROP_SRCCOPY:   return Src;
    case BC250_GDI_ROP_SRCINVERT: return Dst ^ Src;
    case BC250_GDI_ROP_SRCAND:    return Dst & Src;
    case BC250_GDI_ROP_SRCOR:     return Dst | Src;
    default:                      return Dst;      /* refused before here; leave the pixel alone */
    }
}

/* ---- ColorFill ----------------------------------------------------------------------------------
 * DXGK_GDIROP_COLORFILL. Color arrives in A8R8G8B8 ("the fill color, in 32-bit ARGB"), so for a
 * red-first destination the CALLER converts the colour once per command, not per pixel; this
 * function is then bitwise in the destination's own order, like BitBlt above.
 */
BC250_GDI_FN bc250_u32 Bc250GdiColorFillPixel(bc250_u32 Rop, bc250_u32 Color, bc250_u32 Dst)
{
    switch (Rop) {
    case BC250_GDI_CF_PATCOPY:   return Color;
    case BC250_GDI_CF_PATINVERT: return Dst ^ Color;
    case BC250_GDI_CF_PDXN:      return ~(Color ^ Dst);
    case BC250_GDI_CF_DSTINVERT: return ~Dst;
    case BC250_GDI_CF_PATAND:    return Dst & Color;
    case BC250_GDI_CF_PATOR:     return Dst | Color;
    default:                     return Dst;
    }
}

/* ---- AlphaBlend ---------------------------------------------------------------------------------
 * The DXGK_GDIARG_ALPHABLEND page documents the fields and the stretch transform and gives no
 * blending formula at all. The formula is the Win32 one, BLENDFUNCTION with BlendOp AC_SRC_OVER
 * (https://learn.microsoft.com/windows/win32/api/wingdi/ns-wingdi-blendfunction), whose three
 * tables map onto the two fields this DDI has:
 *
 *   SourceHasAlpha 0 (no AC_SRC_ALPHA), SourceConstantAlpha = SCA:
 *       Dst.x = Src.x * (SCA/255) + Dst.x * (1 - SCA/255)
 *   SourceHasAlpha 1, SCA = 255:
 *       Dst.x = Src.x + (1 - Src.a) * Dst.x          (source premultiplied by its own alpha)
 *   SourceHasAlpha 1, SCA < 255:
 *       Src.x = Src.x * SCA/255 (alpha too), then the previous line.
 *
 * The third form subsumes the second (SCA 255 leaves the source unchanged), so there are two
 * branches here, not three.
 *
 * Rounding. The tables are real-valued and the page does not say how the result becomes a byte.
 * Each table line is therefore rounded ONCE, over the whole right-hand side, which is the most
 * faithful reading: rounding the two products separately can differ from it by one count (Src 3,
 * Dst 1, SCA 64 is such a pair) and nothing in the page asks for that. The one place where a
 * second rounding does belong is the premultiply of the third table, because the page writes it as
 * an assignment to Src.x - the premultiplied source is itself a pixel - and the lines that follow
 * then consume that byte. The host test holds both branches against a double-precision oracle of
 * the page's own expressions over the whole 256^3 domain of each channel.
 *
 * The alpha channel is blended by the same rule. On an X8 destination the byte is "don't care" by
 * format, and writing the blended value into it is what the alpha tables say to do; the display
 * path ignores it.
 */
BC250_GDI_FN bc250_u32 Bc250GdiAlphaBlendPixel(bc250_u32 SrcArgb, bc250_u32 DstArgb,
                                               bc250_u32 SourceConstantAlpha, bc250_u32 SourceHasAlpha)
{
    bc250_u32 sa, ia, a, r, g, b;
    bc250_u32 sA = Bc250GdiByte(SrcArgb, 24), sR = Bc250GdiByte(SrcArgb, 16);
    bc250_u32 sG = Bc250GdiByte(SrcArgb, 8),  sB = SrcArgb & 0xFFu;
    bc250_u32 dA = Bc250GdiByte(DstArgb, 24), dR = Bc250GdiByte(DstArgb, 16);
    bc250_u32 dG = Bc250GdiByte(DstArgb, 8),  dB = DstArgb & 0xFFu;

    if (!SourceHasAlpha) {
        /* First table: one constant factor, applied to every channel including alpha, and one
         * rounding of the sum. The numerator is at most 255 * 255, so it cannot overflow. */
        sa = SourceConstantAlpha;
        ia = 255u - sa;
        a = Bc250GdiDiv255Round(sA * sa + dA * ia);
        r = Bc250GdiDiv255Round(sR * sa + dR * ia);
        g = Bc250GdiDiv255Round(sG * sa + dG * ia);
        b = Bc250GdiDiv255Round(sB * sa + dB * ia);
    } else {
        /* Third table. The premultiply by SCA comes first and changes the alpha that the second
         * step then uses; with SCA 255 every one of these products is the identity. */
        if (SourceConstantAlpha != 255u) {
            sA = Bc250GdiMulDiv255(sA, SourceConstantAlpha);
            sR = Bc250GdiMulDiv255(sR, SourceConstantAlpha);
            sG = Bc250GdiMulDiv255(sG, SourceConstantAlpha);
            sB = Bc250GdiMulDiv255(sB, SourceConstantAlpha);
        }
        ia = 255u - sA;
        a = sA + Bc250GdiMulDiv255(dA, ia);
        r = sR + Bc250GdiMulDiv255(dR, ia);
        g = sG + Bc250GdiMulDiv255(dG, ia);
        b = sB + Bc250GdiMulDiv255(dB, ia);
    }
    /* A source that is not premultiplied (Src.x > Src.a) can exceed 255 in the second branch. The
     * Win32 page's own example multiplies the colour channels by alpha "prior to the call", so such
     * a source is out of contract; clamping is what every implementation does with it and it keeps
     * the kernel's store in range whatever the compositor hands us. */
    if (a > 255u) a = 255u;
    if (r > 255u) r = 255u;
    if (g > 255u) g = 255u;
    if (b > 255u) b = 255u;
    return Bc250GdiPack(a, r, g, b);
}

/* ---- TransparentBlt -----------------------------------------------------------------------------
 * DXGK_GDIARG_TRANSPARENTBLT Remarks, verbatim:
 *
 *   if (tblt.Flags.HonorAlpha) { if (SourceColor != tblt.Color) DestinationColor = SourceColor; }
 *   else { if ((SourceColor & 0x00FFFFFF) != tblt.Color) DestinationColor = SourceColor; }
 *
 * Note the asymmetry of the second branch, which is the page's and is kept: the source is masked
 * and the key is NOT, so a key with a nonzero alpha byte matches nothing. The page says as much -
 * "a value of 0x00FFFFFF in ARGB format indicates alpha = 0, hence alpha-blending is not
 * implemented". Both words are in A8R8G8B8 here, so the caller converts a red-first source word
 * before the comparison; it cannot mask in the surface's own order, because the mask is channel
 * specific.
 */
BC250_GDI_FN bc250_u32 Bc250GdiTransparentPixel(bc250_u32 SrcArgb, bc250_u32 DstArgb,
                                                bc250_u32 KeyArgb, bc250_u32 HonorAlpha)
{
    if (HonorAlpha) return SrcArgb != KeyArgb ? SrcArgb : DstArgb;
    return (SrcArgb & 0x00FFFFFFu) != KeyArgb ? SrcArgb : DstArgb;
}

/* ---- ClearTypeBlend -----------------------------------------------------------------------------
 * DXGK_GDIARG_CLEARTYPEBLEND Remarks. Two branches, picked by whether Gamma is a row index in
 * [0,15] or D3DKM_INVALID_GAMMA_INDEX, and then the two unconditional sentences that follow them:
 *
 *   "The display miniport driver must ensure that when a component of A is zero, the corresponding
 *    output component is the same as the background color component (D)."
 *   "The driver must also ensure that when a component of A is 0xFF, the corresponding output
 *    component is the same as the foreground color component (Color2)."
 *
 * Those two sentences are outside either branch and are applied to both. In the gamma branch they
 * are also written into the code the page shows. In the invalid-gamma branch they are not, and
 * there they are not redundant: A 0xFF gives Color (gamma corrected) by that branch's own formula,
 * while the sentence demands Color2. The sentence wins, because it is the requirement and the code
 * block is an illustration; the design page records the reading.
 *
 * Gamma branch, per channel x of r, g, b:
 *   Tmp.x        = GammaTable[D.x]
 *   BlendColor.x = InverseGammaTable[round(Tmp.x + (Color.x - Tmp.x) * A.x / 255.0)]
 * The index is a byte, and the rounded value is between Tmp.x and Color.x, so it cannot leave
 * [0,255] and the table read cannot leave its 256-entry row. In integers, with a numerator that is
 * 255 times a value in [0,255] and therefore never negative:
 *   N = Tmp.x * 255 + (Color.x - Tmp.x) * A.x,  index = round(N / 255) = Bc250GdiDiv255Round(N)
 *
 * Invalid-gamma branch:
 *   Out.x = D.x + (Color.x - D.x) * (Color.x >= D.x ? A.r : A.g) / 255.0
 * The page writes no round() here. It is a colour component and the value has to become a byte, so
 * some rounding happens; the same round-half-away-from-zero as everywhere else in the page is used,
 * and the design page records that this is an interpretation of a silent contract. The same
 * numerator trick applies and is again non-negative.
 *
 * OutputColor.a = D.a in both branches: the alpha byte of the destination is never touched.
 *
 * Gamma and InverseGamma are the two halves of one row of the lookup allocation: the row is
 * Gamma * GammaPitch bytes into it, the gamma table is its first 256 bytes and the inverse gamma
 * table the next 256 (the allocation is 512 x 16, A8). The caller resolves the row and hands both
 * pointers, so this function does no address arithmetic on an OS-supplied index.
 */
BC250_GDI_FN bc250_u32 Bc250GdiClearTypeChannel(bc250_u32 D, bc250_u32 A, bc250_u32 Color,
                                                BC250_GDI_U8PTR GammaTable,
                                                BC250_GDI_U8PTR InverseGammaTable)
{
    bc250_u32 tmp = GammaTable[D];
    bc250_i32 n = (bc250_i32)(tmp * 255u) + ((bc250_i32)Color - (bc250_i32)tmp) * (bc250_i32)A;
    return InverseGammaTable[Bc250GdiDiv255Round((bc250_u32)n)];
}

BC250_GDI_FN bc250_u32 Bc250GdiClearTypeChannelNoGamma(bc250_u32 D, bc250_u32 A, bc250_u32 Color)
{
    bc250_i32 n = (bc250_i32)(D * 255u) + ((bc250_i32)Color - (bc250_i32)D) * (bc250_i32)A;
    return Bc250GdiDiv255Round((bc250_u32)n);
}

BC250_GDI_FN bc250_u32 Bc250GdiClearTypePixel(bc250_u32 DstArgb, bc250_u32 AlphaArgb,
                                              bc250_u32 ColorArgb, bc250_u32 Color2Argb,
                                              bc250_u32 GammaValid,
                                              BC250_GDI_U8PTR GammaTable,
                                              BC250_GDI_U8PTR InverseGammaTable)
{
    bc250_u32 dR = Bc250GdiByte(DstArgb, 16), dG = Bc250GdiByte(DstArgb, 8), dB = DstArgb & 0xFFu;
    bc250_u32 aR = Bc250GdiByte(AlphaArgb, 16), aG = Bc250GdiByte(AlphaArgb, 8), aB = AlphaArgb & 0xFFu;
    bc250_u32 cR = Bc250GdiByte(ColorArgb, 16), cG = Bc250GdiByte(ColorArgb, 8), cB = ColorArgb & 0xFFu;
    bc250_u32 r, g, b;

    if (GammaValid) {
        r = Bc250GdiClearTypeChannel(dR, aR, cR, GammaTable, InverseGammaTable);
        g = Bc250GdiClearTypeChannel(dG, aG, cG, GammaTable, InverseGammaTable);
        b = Bc250GdiClearTypeChannel(dB, aB, cB, GammaTable, InverseGammaTable);
    } else {
        /* The selector is A.r or A.g for every channel, never A.b. That is the page's formula. */
        r = Bc250GdiClearTypeChannelNoGamma(dR, cR >= dR ? aR : aG, cR);
        g = Bc250GdiClearTypeChannelNoGamma(dG, cG >= dG ? aR : aG, cG);
        b = Bc250GdiClearTypeChannelNoGamma(dB, cB >= dB ? aR : aG, cB);
    }
    /* The two unconditional sentences, per channel, after either branch. */
    if (aR == 0u) r = dR; else if (aR == 255u) r = Bc250GdiByte(Color2Argb, 16);
    if (aG == 0u) g = dG; else if (aG == 255u) g = Bc250GdiByte(Color2Argb, 8);
    if (aB == 0u) b = dB; else if (aB == 255u) b = Color2Argb & 0xFFu;
    return Bc250GdiPack(Bc250GdiByte(DstArgb, 24), r, g, b);
}

#endif
