/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "gdi_render.h"

/* Every arithmetic step below is written so that it cannot overflow on the admitted domain, and
 * the domain is established first: a surface is at most 8192 x 8192 (the MaxTextureWidthShift and
 * MaxTextureHeightShift this driver reports are 2, so 2^(2+11)) and its pitch is at most that
 * width's bytes plus padding, bounded here. A rectangle coordinate is a signed int from the OS and
 * is bounded before it is multiplied. */
#define GDI_MAX_EXTENT 8192u
/* A pitch no wider than a maximum row plus one maximum row of padding. The real pitches come from
 * our own layout functions and are far below this; the bound exists so that pitch * height and
 * the row arithmetic cannot leave 64 bits, whatever the DDI hands us. */
#define GDI_MAX_PITCH  (GDI_MAX_EXTENT * 4u * 2u)

static int RectOrdered(const BC250_GDI_RECT* R)
{
    return R->Right >= R->Left && R->Bottom >= R->Top;
}

static int RectNonEmpty(const BC250_GDI_RECT* R)
{
    return R->Right > R->Left && R->Bottom > R->Top;
}

/* A rectangle entirely inside a surface, with both edges non-negative. The lower-right edge is
 * exclusive, as every DstRect member of the DDI says. */
static int RectInside(const BC250_GDI_RECT* R, unsigned int Width, unsigned int Height)
{
    return R->Left >= 0 && R->Top >= 0 && R->Right >= R->Left && R->Bottom >= R->Top &&
        (unsigned int)R->Right <= Width && (unsigned int)R->Bottom <= Height;
}

/* A coordinate that the transform may multiply. Both the destination and the source extents are
 * bounded by GDI_MAX_EXTENT after this, so (2*(Xd-L)+1) * Ws stays below 2^31. */
static int CoordSane(int V)
{
    return V > -(int)(GDI_MAX_EXTENT * 4u) && V < (int)(GDI_MAX_EXTENT * 4u);
}

static int RectSane(const BC250_GDI_RECT* R)
{
    return CoordSane(R->Left) && CoordSane(R->Top) && CoordSane(R->Right) && CoordSane(R->Bottom) &&
        RectOrdered(R);
}

unsigned int Bc250GdiEffectivePitch(const BC250_GDI_SURFACE* Surface, unsigned int DdiPitch)
{
    unsigned int pitch;
    unsigned long long rows;
    if (!Surface || !Surface->Present) return 0;
    if (!Surface->Width || !Surface->Height || Surface->Width > GDI_MAX_EXTENT ||
        Surface->Height > GDI_MAX_EXTENT) return 0;
    if (Surface->Bpp != 1u && Surface->Bpp != 4u) return 0;
    /* The two CPU-visible types are the only ones whose pitch the DDI supplies; for every other
     * type "pitch should be ignored" and the allocation's own is the addressing pitch. This driver
     * refuses D3DKMDT_GDISURFACE_EXISTINGSYSMEM at CreateAllocation, so type 2 is the only one
     * that can reach here with a DDI pitch. */
    pitch = Surface->Type == BC250_GDISURFACE_STAGING_CPUVISIBLE && DdiPitch ? DdiPitch : Surface->Pitch;
    if (!pitch || pitch > GDI_MAX_PITCH) return 0;
    /* AlignmentShift 2: this driver requires four-byte pitch alignment and the OS guarantees it
     * ("Pitch is guaranteed to be aligned ... according to the AlignmentShift member"). A pitch
     * that is not aligned is a malformed buffer, not a case to round. */
    if (pitch & 3u) return 0;
    if (pitch < Surface->Width * Surface->Bpp) return 0;
    rows = (unsigned long long)pitch * Surface->Height;
    if (rows > Surface->Bytes) return 0;
    return pitch;
}

/* The byte offset of a pixel, and the proof that the row it starts fits. Offset is the first byte;
 * the caller adds RowBytes and checks the span. */
static int PixelOffset(unsigned int Pitch, unsigned int Bpp, unsigned long long Bytes,
                       unsigned int X, unsigned int Y, unsigned long long* Offset)
{
    unsigned long long off = (unsigned long long)Y * Pitch + (unsigned long long)X * Bpp;
    if (off >= Bytes) return 0;
    *Offset = off;
    return 1;
}

/* The whole footprint of a Width x Height block at (X, Y): first byte, and the last byte of the
 * last row. Written as a subtraction so a large pitch cannot wrap, the same form blit_plan.c uses. */
static int BlockFits(unsigned int Pitch, unsigned int Bpp, unsigned long long Bytes,
                     unsigned int X, unsigned int Y, unsigned int Width, unsigned int Height,
                     unsigned long long* Offset)
{
    unsigned long long off, rowBytes;
    if (!Width || !Height) return 0;
    if (!PixelOffset(Pitch, Bpp, Bytes, X, Y, &off)) return 0;
    rowBytes = (unsigned long long)Width * Bpp;
    if (rowBytes > Bytes - off) return 0;
    if ((unsigned long long)(Height - 1u) * Pitch > Bytes - off - rowBytes) return 0;
    *Offset = off;
    return 1;
}

static const BC250_GDI_SURFACE* SurfaceAt(const BC250_GDI_SURFACE* Surfaces, unsigned int Count,
                                          unsigned int Index)
{
    if (!Surfaces || Index >= Count) return 0;
    if (!Surfaces[Index].Present) return 0;
    return &Surfaces[Index];
}

/* Bc250GdiKernelsPresent answers from the code object itself and so lives beside it, in
 * gdi_dispatch.c. Nothing else in the driver may decide it: the capability bit, the start log and
 * the operator's gate all read that one function. */

/* Which allocation-list slots an opcode reads, and the arithmetic class it belongs to. Kept in one
 * place so admission, planning and the caller's own slot binding cannot disagree about what a
 * command means. */
int Bc250GdiOpHasSource(unsigned int OpCode)
{
    return OpCode == BC250_GDIOP_BITBLT || OpCode == BC250_GDIOP_ALPHABLEND ||
        OpCode == BC250_GDIOP_STRETCHBLT || OpCode == BC250_GDIOP_TRANSPARENTBLT;
}

/* The source rectangle of an operation that may not overhang its source. Only BITBLT may: "The
 * source rectangle can exceed the bounds of the source surface" (BITBLT SrcRect), against
 * "will never exceed the bounds of the source surface, so it will never overhang" (ALPHABLEND and
 * STRETCHBLT SrcRect). TRANSPARENTBLT says neither, and its Remarks say SrcRect bounds all source
 * sub-rectangles, so it is held to the stricter rule. */
static int OpSourceMustFit(unsigned int OpCode)
{
    return OpCode == BC250_GDIOP_ALPHABLEND || OpCode == BC250_GDIOP_STRETCHBLT ||
        OpCode == BC250_GDIOP_TRANSPARENTBLT;
}

static int AdmitRop(unsigned int OpCode, unsigned int Rop)
{
    if (OpCode == BC250_GDIOP_BITBLT)
        return Rop >= BC250_GDI_ROP_SRCCOPY && Rop <= BC250_GDI_ROP_SRCOR;
    return Rop >= BC250_GDI_CF_PATCOPY && Rop <= BC250_GDI_CF_PATOR;
}

int Bc250GdiAdmitOp(const BC250_GDI_OP* Op, const BC250_GDI_SURFACE* Surfaces, unsigned int Count)
{
    const BC250_GDI_SURFACE* src;
    const BC250_GDI_SURFACE* dst;
    const BC250_GDI_SURFACE* alpha;
    const BC250_GDI_SURFACE* gamma;
    unsigned int srcPitch, dstPitch;

    if (!Op) return Bc250GdiBadOpCode;
    if (Op->OpCode == BC250_GDIOP_ESCAPE) return Bc250GdiSkip;
    if (Op->OpCode < BC250_GDIOP_BITBLT || Op->OpCode > BC250_GDIOP_CLEARTYPEBLEND)
        return Bc250GdiBadOpCode;

    if (!RectSane(&Op->DstRect)) return Bc250GdiBadRect;
    if (!RectNonEmpty(&Op->DstRect)) return Bc250GdiBadRect;
    if ((unsigned int)(Op->DstRect.Right - Op->DstRect.Left) > GDI_MAX_EXTENT ||
        (unsigned int)(Op->DstRect.Bottom - Op->DstRect.Top) > GDI_MAX_EXTENT) return Bc250GdiBadRect;

    dst = SurfaceAt(Surfaces, Count, Op->DstAllocationIndex);
    if (!dst) return Bc250GdiBadIndex;
    dstPitch = Bc250GdiEffectivePitch(dst, Op->DstPitch);
    if (!dstPitch) return Bc250GdiBadSurface;
    /* Every colour operation addresses four-byte pixels. A one-byte destination is admitted for
     * one case only, and it is the case the LOOKUPTABLE field describes: "The allocation is
     * initialized in D3DDDIFMT_A8 format by issuing a ... DXGK_GDIOP_BITBLT command from a
     * D3DKMDT_GDISURFACE_STAGING_CPUVISIBLE surface." That is a byte copy, nothing else. */
    if (dst->Bpp != 4u) {
        if (Op->OpCode != BC250_GDIOP_BITBLT || Op->Rop != BC250_GDI_ROP_SRCCOPY ||
            dst->Type != BC250_GDISURFACE_LOOKUPTABLE) return Bc250GdiBadSurface;
    }

    if (Op->OpCode == BC250_GDIOP_BITBLT || Op->OpCode == BC250_GDIOP_COLORFILL) {
        if (Op->Rop == BC250_GDI_ROP_ROP3 && Op->OpCode == BC250_GDIOP_BITBLT) return Bc250GdiBadRop;
        if (Op->Rop == BC250_GDI_CF_ROP3 && Op->OpCode == BC250_GDIOP_COLORFILL) return Bc250GdiBadRop;
        if (!AdmitRop(Op->OpCode, Op->Rop)) return Bc250GdiBadRop;
        /* SupportAllBltRops is clear, so Rop3 is never consulted and must be zero. A nonzero Rop3
         * beside a non-ROP3 Rop is a buffer this driver does not understand. */
        if (Op->Rop3) return Bc250GdiBadRop;
    }

    if (Op->OpCode == BC250_GDIOP_STRETCHBLT) {
        if (Op->Mode != 0u && Op->Mode != BC250_GDI_STRETCH_COLORONCOLOR) return Bc250GdiBadMode;
        if (Op->MirrorX || Op->MirrorY) return Bc250GdiBadMode;
    }

    if (Bc250GdiOpHasSource(Op->OpCode)) {
        if (!RectSane(&Op->SrcRect) || !RectNonEmpty(&Op->SrcRect)) return Bc250GdiBadRect;
        if ((unsigned int)(Op->SrcRect.Right - Op->SrcRect.Left) > GDI_MAX_EXTENT ||
            (unsigned int)(Op->SrcRect.Bottom - Op->SrcRect.Top) > GDI_MAX_EXTENT) return Bc250GdiBadRect;
        src = SurfaceAt(Surfaces, Count, Op->SrcAllocationIndex);
        if (!src) return Bc250GdiBadIndex;
        srcPitch = Bc250GdiEffectivePitch(src, Op->SrcPitch);
        if (!srcPitch) return Bc250GdiBadSurface;
        if (src->Bpp != dst->Bpp) return Bc250GdiBadSurface;
        /* A blit between two different channel orders would need a conversion the DDI has no way
         * to request, and the arithmetic of gdi_pixel.h is in one order per command. The Present
         * producer answers the same case the same way. A one-byte pair has no channel order. */
        if (dst->Bpp == 4u && src->RedFirst != dst->RedFirst) return Bc250GdiBadFormat;
        if (OpSourceMustFit(Op->OpCode) && !RectInside(&Op->SrcRect, src->Width, src->Height))
            return Bc250GdiBadRect;
        /* The No*SameBitmap* caps are all set, so dxgkrnl will not ask for any of these. A command
         * that does arrive naming one allocation twice is refused rather than run, because the
         * read-modify-write of a kernel over an overlapping pair has no defined result. */
        if (Op->SrcAllocationIndex == Op->DstAllocationIndex) return Bc250GdiBadSameSurface;
        /* A stretch that is not 1:1 is only admitted for the three operations whose Remarks define
         * one. BITBLT is "with no stretching" by its own title: unequal extents there is a
         * malformed command. */
        if (Op->OpCode == BC250_GDIOP_BITBLT &&
            (Op->SrcRect.Right - Op->SrcRect.Left != Op->DstRect.Right - Op->DstRect.Left ||
             Op->SrcRect.Bottom - Op->SrcRect.Top != Op->DstRect.Bottom - Op->DstRect.Top))
            return Bc250GdiBadRect;
    }

    if (Op->OpCode == BC250_GDIOP_ALPHABLEND) {
        if (Op->SourceConstantAlpha > 255u) return Bc250GdiBadRop;
        if (Op->SourceHasAlpha > 1u) return Bc250GdiBadRop;
    }

    if (Op->OpCode == BC250_GDIOP_CLEARTYPEBLEND) {
        alpha = SurfaceAt(Surfaces, Count, Op->AlphaSurfAllocationIndex);
        if (!alpha) return Bc250GdiBadIndex;
        if (alpha->Bpp != 4u) return Bc250GdiBadSurface;
        if (!Bc250GdiEffectivePitch(alpha, Op->AlphaSurfPitch)) return Bc250GdiBadSurface;
        if (Op->AlphaSurfAllocationIndex == Op->DstAllocationIndex) return Bc250GdiBadSameSurface;
        if (!CoordSane(Op->DstToAlphaOffsetX) || !CoordSane(Op->DstToAlphaOffsetY))
            return Bc250GdiBadRect;
        if (Op->Gamma != BC250_GDI_INVALID_GAMMA) {
            if (Op->Gamma >= BC250_GDI_GAMMA_HEIGHT) return Bc250GdiBadGamma;
            gamma = SurfaceAt(Surfaces, Count, Op->GammaSurfAllocationIndex);
            if (!gamma) return Bc250GdiBadIndex;
            /* The shape is fixed by the contract; a lookup allocation of any other shape is not
             * the table the formula indexes. */
            if (gamma->Bpp != 1u || gamma->Width != BC250_GDI_GAMMA_WIDTH ||
                gamma->Height != BC250_GDI_GAMMA_HEIGHT ||
                gamma->Type != BC250_GDISURFACE_LOOKUPTABLE) return Bc250GdiBadGamma;
            if (!Bc250GdiEffectivePitch(gamma, 0)) return Bc250GdiBadSurface;
            if (Op->GammaSurfAllocationIndex == Op->DstAllocationIndex) return Bc250GdiBadSameSurface;
        }
        /* NoTempSurfaceForClearTypeBlend is set: the kernel reads the destination itself, so
         * TmpSurfAllocationIndex is never dereferenced and is not validated. */
    }
    return Bc250GdiOk;
}

/* Both axes are Bc250GdiSourceCoord of gdi_pixel.h, which is the one text the kernels run too. */
int Bc250GdiSourceX(const BC250_GDI_ITEM* Item, int Xd)
{
    if (!Item || !Item->Wd) return 0;
    return Bc250GdiSourceCoord(Item->SrcLeft, (unsigned int)(Xd - Item->DstLeft0), Item->Ws, Item->Wd);
}

int Bc250GdiSourceY(const BC250_GDI_ITEM* Item, int Yd)
{
    if (!Item || !Item->Hd) return 0;
    return Bc250GdiSourceCoord(Item->SrcTop, (unsigned int)(Yd - Item->DstTop0), Item->Hs, Item->Hd);
}

/* The work and kernel an admitted command maps to. One function so the classification is the same
 * for the emitter and for the host test. */
static void ClassifyWork(const BC250_GDI_OP* Op, unsigned int Stretch, BC250_GDI_ITEM* Item)
{
    Item->Work = Bc250GdiWorkKernel;
    Item->Kernel = Bc250GdiKernelNone;
    switch (Op->OpCode) {
    case BC250_GDIOP_BITBLT:
        if (Op->Rop == BC250_GDI_ROP_SRCCOPY) {
            /* An equal-extent SRCCOPY is a byte move of whole rows and the CP's DMA_DATA engine
             * does it exactly; Stretch is always 0 here, because admission refused a BITBLT with
             * unequal extents. */
            Item->Work = Bc250GdiWorkCopy;
        } else {
            Item->Kernel = Bc250GdiKernelBitBltRop;
        }
        break;
    case BC250_GDIOP_COLORFILL:
        if (Op->Rop == BC250_GDI_CF_PATCOPY) Item->Work = Bc250GdiWorkFill;
        else Item->Kernel = Bc250GdiKernelColorFillRop;
        break;
    case BC250_GDIOP_ALPHABLEND:
        Item->Kernel = Bc250GdiKernelAlphaBlend;
        break;
    case BC250_GDIOP_STRETCHBLT:
        /* A 1:1 StretchBlt is a copy, and the OS does send them; take the engine path. */
        if (!Stretch) Item->Work = Bc250GdiWorkCopy;
        else Item->Kernel = Bc250GdiKernelStretchBlt;
        break;
    case BC250_GDIOP_TRANSPARENTBLT:
        Item->Kernel = Bc250GdiKernelTransparentBlt;
        break;
    case BC250_GDIOP_CLEARTYPEBLEND:
        Item->Kernel = Bc250GdiKernelClearTypeBlend;
        break;
    default:
        Item->Work = Bc250GdiWorkNone;
        break;
    }
}

int Bc250GdiPlanItem(const BC250_GDI_OP* Op, const BC250_GDI_SURFACE* Surfaces, unsigned int Count,
                     const BC250_GDI_RECT* SubRect, BC250_GDI_ITEM* Item)
{
    const BC250_GDI_SURFACE* dst;
    const BC250_GDI_SURFACE* src = 0;
    const BC250_GDI_SURFACE* alpha = 0;
    const BC250_GDI_SURFACE* gamma = 0;
    BC250_GDI_RECT clipped;
    unsigned int i;
    int status;

    if (!Item || !SubRect) return Bc250GdiBadRect;
    for (i = 0; i < sizeof(*Item); i++) ((unsigned char*)Item)[i] = 0;
    status = Bc250GdiAdmitOp(Op, Surfaces, Count);
    if (status != Bc250GdiOk) return status;

    dst = SurfaceAt(Surfaces, Count, Op->DstAllocationIndex);
    Item->DstPitch = Bc250GdiEffectivePitch(dst, Op->DstPitch);
    /* "all sub-rectangles are guaranteed to fit inside the destination surface". Checked, because
     * this is the one coordinate the OS gives that is multiplied by a pitch. */
    if (!RectSane(SubRect)) return Bc250GdiBadRect;
    if (!RectInside(SubRect, dst->Width, dst->Height)) return Bc250GdiBadRect;

    /* "Sub-rectangles can be constrained further by a bounding rectangle that is smaller than the
     * destination rectangle", and DstRect itself may overhang the destination. Intersect, keeping
     * the original DstRect origin for the source transform. */
    clipped.Left = SubRect->Left > Op->DstRect.Left ? SubRect->Left : Op->DstRect.Left;
    clipped.Top = SubRect->Top > Op->DstRect.Top ? SubRect->Top : Op->DstRect.Top;
    clipped.Right = SubRect->Right < Op->DstRect.Right ? SubRect->Right : Op->DstRect.Right;
    clipped.Bottom = SubRect->Bottom < Op->DstRect.Bottom ? SubRect->Bottom : Op->DstRect.Bottom;

    Item->DstLeft0 = Op->DstRect.Left;
    Item->DstTop0 = Op->DstRect.Top;
    Item->Wd = (unsigned int)(Op->DstRect.Right - Op->DstRect.Left);
    Item->Hd = (unsigned int)(Op->DstRect.Bottom - Op->DstRect.Top);
    if (Bc250GdiOpHasSource(Op->OpCode)) {
        src = SurfaceAt(Surfaces, Count, Op->SrcAllocationIndex);
        Item->SrcPitch = Bc250GdiEffectivePitch(src, Op->SrcPitch);
        Item->SrcLeft = Op->SrcRect.Left;
        Item->SrcTop = Op->SrcRect.Top;
        Item->Ws = (unsigned int)(Op->SrcRect.Right - Op->SrcRect.Left);
        Item->Hs = (unsigned int)(Op->SrcRect.Bottom - Op->SrcRect.Top);
        Item->Stretch = (Item->Ws != Item->Wd || Item->Hs != Item->Hd) ? 1u : 0u;
    } else {
        Item->Ws = Item->Wd;
        Item->Hs = Item->Hd;
    }
    ClassifyWork(Op, Item->Stretch, Item);

    if (!RectNonEmpty(&clipped)) return Bc250GdiOk;      /* nothing to do, not an error */

    Item->DstLeft = (unsigned int)clipped.Left;
    Item->DstTop = (unsigned int)clipped.Top;
    Item->DstWidth = (unsigned int)(clipped.Right - clipped.Left);
    Item->DstHeight = (unsigned int)(clipped.Bottom - clipped.Top);
    if (!BlockFits(Item->DstPitch, dst->Bpp, dst->Bytes, Item->DstLeft, Item->DstTop,
                   Item->DstWidth, Item->DstHeight, &Item->DstOffset)) {
        Item->DstWidth = Item->DstHeight = 0;
        return Bc250GdiBadRect;
    }

    if (src) {
        /* The source origin of this sub-rectangle, and the source block the whole sub-rectangle
         * reads. "When sub-rectangles are transformed to the source surface space, the result is
         * guaranteed to be within the source surface" - checked, for the corner pixels, which
         * bound every pixel between them because the transform is monotonic in both axes. */
        int x0 = Bc250GdiSourceX(Item, clipped.Left);
        int y0 = Bc250GdiSourceY(Item, clipped.Top);
        int x1 = Bc250GdiSourceX(Item, clipped.Right - 1);
        int y1 = Bc250GdiSourceY(Item, clipped.Bottom - 1);
        unsigned long long base;
        if (x0 < 0 || y0 < 0 || x1 < x0 || y1 < y0) return Bc250GdiBadRect;
        if ((unsigned int)x1 >= src->Width || (unsigned int)y1 >= src->Height) return Bc250GdiBadRect;
        if (!BlockFits(Item->SrcPitch, src->Bpp, src->Bytes, (unsigned int)x0, (unsigned int)y0,
                       (unsigned int)(x1 - x0) + 1u, (unsigned int)(y1 - y0) + 1u, &base))
            return Bc250GdiBadRect;
        Item->SrcBase = 0;
        Item->SrcOffset = base;
        Item->RowBytes = Item->DstWidth * dst->Bpp;
        /* A copy addresses whole rows at a constant offset; the engine path needs that offset and
         * nothing else. A kernel re-evaluates the transform per pixel and uses SrcBase 0. */
        if (Item->Stretch) { Item->SrcOffset = 0; Item->RowBytes = 0; }
    } else {
        Item->RowBytes = Item->DstWidth * dst->Bpp;
    }

    if (Op->OpCode == BC250_GDIOP_CLEARTYPEBLEND) {
        int ax = clipped.Left + Op->DstToAlphaOffsetX;
        int ay = clipped.Top + Op->DstToAlphaOffsetY;
        alpha = SurfaceAt(Surfaces, Count, Op->AlphaSurfAllocationIndex);
        Item->AlphaPitch = Bc250GdiEffectivePitch(alpha, Op->AlphaSurfPitch);
        if (ax < 0 || ay < 0) return Bc250GdiBadRect;
        /* The extents, before the bytes: a block whose right edge passes the alpha surface's own
         * width still lies inside the allocation (the pitch has padding, and the rows below it
         * follow), so BlockFits alone would admit it and the kernel would read the wrong pixels. */
        if ((unsigned int)ax + Item->DstWidth > alpha->Width ||
            (unsigned int)ay + Item->DstHeight > alpha->Height)
            return Bc250GdiBadRect;
        if (!BlockFits(Item->AlphaPitch, alpha->Bpp, alpha->Bytes, (unsigned int)ax,
                       (unsigned int)ay, Item->DstWidth, Item->DstHeight, &Item->AlphaOffset))
            return Bc250GdiBadRect;
        Item->GammaValid = Op->Gamma != BC250_GDI_INVALID_GAMMA ? 1u : 0u;
        if (Item->GammaValid) {
            unsigned int pitch;
            gamma = SurfaceAt(Surfaces, Count, Op->GammaSurfAllocationIndex);
            pitch = Bc250GdiEffectivePitch(gamma, 0);
            /* The row holds both tables back to back, so the row must be at least 512 bytes wide;
             * the shape check in admission already fixes Width at 512 and Bpp at 1. */
            if (!BlockFits(pitch, gamma->Bpp, gamma->Bytes, 0, Op->Gamma,
                           BC250_GDI_GAMMA_ENTRIES * 2u, 1u, &Item->GammaOffset))
                return Bc250GdiBadGamma;
        }
    }
    return Bc250GdiOk;
}

int Bc250GdiWalkStep(unsigned int BufferBytes, unsigned int Offset, unsigned int CommandBytes,
                     unsigned int MinBytes, unsigned int* Next)
{
    if (!Next) return 0;
    *Next = 0;
    if (!MinBytes || (MinBytes & 3u)) return 0;
    if (Offset > BufferBytes) return 0;
    /* The command must hold at least an opcode and a size, and the OS writes DWORD-aligned
     * structures; a size that is not a multiple of four would leave the next command misaligned. */
    if (CommandBytes < MinBytes || (CommandBytes & 3u)) return 0;
    if (CommandBytes > BufferBytes - Offset) return 0;
    *Next = Offset + CommandBytes;
    return 1;
}

unsigned int Bc250GdiPackPass(unsigned int Command, unsigned int SubRect)
{
    if (Command > BC250_GDI_PASS_MAX || SubRect > BC250_GDI_PASS_MAX) return 0;
    return (Command << 16) | SubRect;
}

unsigned int Bc250GdiPassCommand(unsigned int Pass)
{
    return (Pass >> 16) & BC250_GDI_PASS_MAX;
}

unsigned int Bc250GdiPassSubRect(unsigned int Pass)
{
    return Pass & BC250_GDI_PASS_MAX;
}
