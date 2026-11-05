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
        /* One allocation as source and destination. BITBLT is admitted (BD-306): wddm.c clears
         * NoSameBitmapBitBlt and NoSameBitmapOverlappedBitBlt, so CDD sends a same-surface BitBlt
         * to us, overlapping or not, instead of copying through a staging surface of its own
         * (d3dkmddi.h lines 1949-1950: with the bit set, the OS will not ask). Bc250GdiPlanItem
         * marks the items whose blocks meet, and the emitters do those in bands (gdi_pixel.h).
         * ALPHABLEND, STRETCHBLT and TRANSPARENTBLT keep their No*SameBitmap* caps set, so a
         * command of theirs that names one allocation twice is still refused rather than run. The
         * two pitch members must then agree: one allocation has one row stride. */
        if (Op->SrcAllocationIndex == Op->DstAllocationIndex) {
            if (Op->OpCode != BC250_GDIOP_BITBLT) return Bc250GdiBadSameSurface;
            if (srcPitch != dstPitch) return Bc250GdiBadSurface;
        }
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

/* A BITBLT inside one allocation (BD-306), once the source block of this item is known: SrcX and
 * SrcY are its top-left pixel and the block has the destination's extent. Three cases.
 *
 *   - The same block (Ddx = Ddy = 0): SRCCOPY, SRCAND and SRCOR leave the destination as it is, so
 *     the item becomes Bc250GdiWorkSkip. SRCINVERT stays the ordinary kernel: each work item reads
 *     and writes only its own pixel, so no order is needed.
 *   - Blocks that do not meet: no pixel of this item is read and written, so the item stays what
 *     ClassifyWork made it. The order between items is the order of the sub-rectangles, which the
 *     CDD gives for an overlapping copy, and gdi_batch.c ends every item with a wait.
 *   - Blocks that meet: Ordered. A copy stays an engine copy, which gdi_emit.c then writes in
 *     bands; any other ROP becomes gdi_bitblt_ordered.
 *
 * Two blocks of one surface that do not meet in pixels do not meet in bytes either: a row of a
 * block lies inside its own row of the surface, because the block lies inside the surface width
 * and the pitch is at least that width (Bc250GdiEffectivePitch). */
static void ClassifySameSurface(const BC250_GDI_OP* Op, unsigned int SrcX, unsigned int SrcY,
                                BC250_GDI_ITEM* Item)
{
    unsigned int w = Item->DstWidth, h = Item->DstHeight;
    if (Item->Ddx == 0 && Item->Ddy == 0) {
        if (Op->Rop != BC250_GDI_ROP_SRCINVERT) {
            Item->Work = (unsigned int)Bc250GdiWorkSkip;
            Item->Kernel = (unsigned int)Bc250GdiKernelNone;
        }
        return;
    }
    if (SrcX >= Item->DstLeft + w || Item->DstLeft >= SrcX + w ||
        SrcY >= Item->DstTop + h || Item->DstTop >= SrcY + h)
        return;
    Item->Ordered = 1u;
    if (Item->Work == (unsigned int)Bc250GdiWorkKernel)
        Item->Kernel = (unsigned int)Bc250GdiKernelBitBltOrdered;
}

/* A sub-rectangle clipped to DstRect, as Bc250GdiPlanItem clips it. */
static BC250_GDI_RECT ClipToDst(const BC250_GDI_OP* Op, const BC250_GDI_RECT* R)
{
    BC250_GDI_RECT c;
    c.Left = R->Left > Op->DstRect.Left ? R->Left : Op->DstRect.Left;
    c.Top = R->Top > Op->DstRect.Top ? R->Top : Op->DstRect.Top;
    c.Right = R->Right < Op->DstRect.Right ? R->Right : Op->DstRect.Right;
    c.Bottom = R->Bottom < Op->DstRect.Bottom ? R->Bottom : Op->DstRect.Bottom;
    return c;
}

/* Item i writes pixels that item j reads, when Di and Dj are their clipped destination blocks: Di
 * meets Dj moved back by the translation (Dj - d is item j's source block). Every term is inside
 * CoordSane, so the sums cannot overflow. An empty block meets nothing. */
static int WritesWhatReads(const BC250_GDI_RECT* Di, const BC250_GDI_RECT* Dj, int Ddx, int Ddy)
{
    return RectNonEmpty(Di) && RectNonEmpty(Dj) &&
        Di->Left < Dj->Right - Ddx && Dj->Left - Ddx < Di->Right &&
        Di->Top < Dj->Bottom - Ddy && Dj->Top - Ddy < Di->Bottom;
}

/* The lowest set bit of a nonzero word. */
static unsigned int LowestBit(unsigned long long Word)
{
    unsigned int b = 0u;
    if ((Word & 0xFFFFFFFFull) == 0u) { Word >>= 32; b += 32u; }
    if ((Word & 0xFFFFull) == 0u) { Word >>= 16; b += 16u; }
    if ((Word & 0xFFull) == 0u) { Word >>= 8; b += 8u; }
    if ((Word & 0xFull) == 0u) { Word >>= 4; b += 4u; }
    if ((Word & 0x3ull) == 0u) { Word >>= 2; b += 2u; }
    if ((Word & 0x1ull) == 0u) b += 1u;
    return b;
}

/* The work space of Bc250GdiSameSurfaceOrder, as BC250_GDI_ORDER_BYTES lays it out. */
typedef struct GDI_ORDER_WORK {
    BC250_GDI_RECT* Dst;        /* [Count] the clipped destination blocks; empty for a refused one */
    unsigned short* Waiting;    /* [Count] the items that must still run before this one */
    unsigned short* Sorted;     /* [Rows] the items with a block, by the low edge on the axis */
    unsigned long long* Ready;  /* [Words] one bit per item that waits for nothing and is not placed */
    unsigned int Rows, Words;
    int Vertical;               /* the axis: 1 sorts by Top, 0 by Left */
    int Longest;                /* the greatest extent of a block on the axis */
} GDI_ORDER_WORK;

static int AxisLo(const GDI_ORDER_WORK* W, const BC250_GDI_RECT* R) { return W->Vertical ? R->Top : R->Left; }
static int AxisHi(const GDI_ORDER_WORK* W, const BC250_GDI_RECT* R) { return W->Vertical ? R->Bottom : R->Right; }

static void SiftDown(const GDI_ORDER_WORK* W, unsigned int Root, unsigned int Size)
{
    unsigned short* heap = W->Sorted;
    for (;;) {
        unsigned int child = Root * 2u + 1u, top = Root;
        unsigned short swap;
        if (child < Size && AxisLo(W, &W->Dst[heap[child]]) > AxisLo(W, &W->Dst[heap[top]])) top = child;
        if (child + 1u < Size && AxisLo(W, &W->Dst[heap[child + 1u]]) > AxisLo(W, &W->Dst[heap[top]]))
            top = child + 1u;
        if (top == Root) return;
        swap = heap[Root]; heap[Root] = heap[top]; heap[top] = swap;
        Root = top;
    }
}

/* Sorted in place, by heap sort: no recursion and no allocation, Rows * log(Rows) steps. Items with
 * one low edge come in any order, which changes nothing: every query below tests each candidate
 * whole and only counts. */
static void SortOnAxis(GDI_ORDER_WORK* W)
{
    unsigned int k;
    if (W->Rows < 2u) return;
    for (k = W->Rows / 2u; k-- > 0u;) SiftDown(W, k, W->Rows);
    for (k = W->Rows - 1u; k > 0u; k--) {
        unsigned short swap = W->Sorted[0];
        W->Sorted[0] = W->Sorted[k]; W->Sorted[k] = swap;
        SiftDown(W, 0u, k);
    }
}

/* The first position in Sorted whose block can meet a block with low edge Lo on the axis: a block
 * whose low edge is at or below Lo - Longest ends at or below Lo. Both terms are inside CoordSane. */
static unsigned int FirstOnAxis(const GDI_ORDER_WORK* W, int Lo)
{
    unsigned int lo = 0u, hi = W->Rows;
    int below = Lo - W->Longest;
    while (lo < hi) {
        unsigned int mid = lo + (hi - lo) / 2u;
        if (AxisLo(W, &W->Dst[W->Sorted[mid]]) <= below) lo = mid + 1u; else hi = mid;
    }
    return lo;
}

int Bc250GdiSameSurfaceOrder(const BC250_GDI_OP* Op, const BC250_GDI_RECT* Subs, unsigned int Count,
                             unsigned short* Order, void* Work, unsigned int WorkBytes)
{
    GDI_ORDER_WORK w;
    unsigned int done = 0u, i, j, k, moved = 0u;
    int ddx, ddy, d, minX = 0, maxX = 0, minY = 0, maxY = 0, wide = 0, tall = 0;

    if (!Op || !Subs || !Order || Count > BC250_GDI_ORDER_LIMIT) return 0;
    for (i = 0; i < Count; i++) Order[i] = (unsigned short)i;
    if (Op->OpCode != BC250_GDIOP_BITBLT || Op->SrcAllocationIndex != Op->DstAllocationIndex)
        return 1;
    if (!RectSane(&Op->DstRect) || !RectSane(&Op->SrcRect)) return 0;
    ddx = Op->DstRect.Left - Op->SrcRect.Left;
    ddy = Op->DstRect.Top - Op->SrcRect.Top;
    if (ddx == 0 && ddy == 0) return 1;     /* each item reads only its own pixels */
    /* The alignment is the caller's (gdi_render.h); this file uses no standard header to test it. */
    if (!Work || WorkBytes < BC250_GDI_ORDER_BYTES(Count)) return 0;
    w.Dst = (BC250_GDI_RECT*)Work;
    w.Waiting = (unsigned short*)(w.Dst + Count);
    w.Sorted = (unsigned short*)((unsigned char*)w.Waiting + ((Count * 2u + 7u) & ~7u));
    w.Ready = (unsigned long long*)((unsigned char*)w.Sorted + ((Count * 2u + 7u) & ~7u));
    w.Words = (Count + 63u) / 64u;
    w.Rows = 0u;
    for (i = 0; i < Count; i++) {
        BC250_GDI_RECT* r = &w.Dst[i];
        /* A rectangle Bc250GdiPlanItem will refuse draws nothing and needs no place: it becomes
         * the empty block, which meets nothing and is not in Sorted. */
        if (RectSane(&Subs[i])) {
            *r = ClipToDst(Op, &Subs[i]);
        } else {
            r->Left = r->Top = r->Right = r->Bottom = 0;
        }
        if (!RectNonEmpty(r)) continue;
        if (w.Rows == 0u || r->Left < minX) minX = r->Left;
        if (w.Rows == 0u || r->Right > maxX) maxX = r->Right;
        if (w.Rows == 0u || r->Top < minY) minY = r->Top;
        if (w.Rows == 0u || r->Bottom > maxY) maxY = r->Bottom;
        if (r->Right - r->Left > wide) wide = r->Right - r->Left;
        if (r->Bottom - r->Top > tall) tall = r->Bottom - r->Top;
        w.Sorted[w.Rows++] = (unsigned short)i;
    }
    /* The axis on which the longest block is the smaller part of the span, so that a query window
     * holds fewer blocks: rows of one band sort by Left, strips stacked on each other by Top. The
     * products are of extents inside CoordSane, so they fit 64 bits. */
    w.Vertical = (long long)tall * (long long)(maxX - minX) <= (long long)wide * (long long)(maxY - minY);
    w.Longest = w.Vertical ? tall : wide;
    d = w.Vertical ? ddy : ddx;
    SortOnAxis(&w);
    for (i = 0; i < w.Words; i++) w.Ready[i] = 0u;
    /* Waiting[i]: the items j that item i writes for (Dst[i] meets Dst[j] - d, so Dst[j] meets
     * Dst[i] + d). Only the blocks whose low edge is inside the window of that block can meet it.
     * Count - 1 fits, because Count is at most BC250_GDI_ORDER_LIMIT. */
    for (i = 0; i < Count; i++) {
        unsigned int n = 0u;
        if (RectNonEmpty(&w.Dst[i])) {
            int hi = AxisHi(&w, &w.Dst[i]) + d;
            for (k = FirstOnAxis(&w, AxisLo(&w, &w.Dst[i]) + d);
                 k < w.Rows && AxisLo(&w, &w.Dst[w.Sorted[k]]) < hi; k++) {
                j = w.Sorted[k];
                if (j != i && WritesWhatReads(&w.Dst[i], &w.Dst[j], ddx, ddy)) n++;
            }
        }
        w.Waiting[i] = (unsigned short)n;
        if (n == 0u) w.Ready[i / 64u] |= 1ull << (i % 64u);
    }
    /* Kahn's order, always taking the lowest index that is free. An order that is already safe is
     * then given back unchanged, item for item. When item i is placed, each item j that waits for
     * it (Dst[j] meets Dst[i] - d) waits for one fewer; a ready or placed item waits for nothing
     * that is left. */
    while (done < Count) {
        for (j = 0; j < w.Words && w.Ready[j] == 0u; j++) {}
        if (j == w.Words) {
            /* A cycle: no order of whole items is safe. A clip region of GDI cannot make one, so
             * this is a malformed command; the given order stays and the caller counts it. */
            for (j = 0; j < Count; j++) Order[j] = (unsigned short)j;
            return 0;
        }
        i = j * 64u + LowestBit(w.Ready[j]);
        w.Ready[j] &= w.Ready[j] - 1u;
        if (i != done) moved = 1u;
        Order[done++] = (unsigned short)i;
        if (RectNonEmpty(&w.Dst[i])) {
            int hi = AxisHi(&w, &w.Dst[i]) - d;
            for (k = FirstOnAxis(&w, AxisLo(&w, &w.Dst[i]) - d);
                 k < w.Rows && AxisLo(&w, &w.Dst[w.Sorted[k]]) < hi; k++) {
                j = w.Sorted[k];
                if (w.Waiting[j] != 0u && WritesWhatReads(&w.Dst[j], &w.Dst[i], ddx, ddy) &&
                    --w.Waiting[j] == 0u)
                    w.Ready[j / 64u] |= 1ull << (j % 64u);
            }
        }
    }
    return moved ? 2 : 1;
}

/* The inverse of the two engine arms of ClassifyWork: the kernel that performs the same operation.
 * The ROP checks are the classification's own conditions read back, so a mismatch is a refusal and
 * not a silently different drawing. */
int Bc250GdiItemToKernel(const BC250_GDI_OP* Op, BC250_GDI_ITEM* Item)
{
    unsigned int kernel;
    if (!Op || !Item) return 0;
    if (Item->Work == (unsigned int)Bc250GdiWorkKernel)
        return Item->Kernel != (unsigned int)Bc250GdiKernelNone &&
               Item->Kernel < (unsigned int)Bc250GdiKernels;
    if (Item->Work != (unsigned int)Bc250GdiWorkCopy && Item->Work != (unsigned int)Bc250GdiWorkFill)
        return 0;
    switch (Op->OpCode) {
    case BC250_GDIOP_BITBLT:
        if (Op->Rop != BC250_GDI_ROP_SRCCOPY) return 0;
        /* A copy whose source and destination blocks meet in one allocation keeps its band order
         * in the kernel too (gdi_render.h, Bc250GdiItemToKernel). */
        kernel = Item->Ordered ? (unsigned int)Bc250GdiKernelBitBltOrdered
                               : (unsigned int)Bc250GdiKernelBitBltRop;
        break;
    case BC250_GDIOP_STRETCHBLT:
        if (Item->Stretch) return 0;            /* a stretching StretchBlt is never an engine item */
        kernel = (unsigned int)Bc250GdiKernelStretchBlt;
        break;
    case BC250_GDIOP_COLORFILL:
        if (Op->Rop != BC250_GDI_CF_PATCOPY) return 0;
        kernel = (unsigned int)Bc250GdiKernelColorFillRop;
        break;
    default:
        return 0;
    }
    Item->Work = (unsigned int)Bc250GdiWorkKernel;
    Item->Kernel = kernel;
    return 1;
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
    if (Op->OpCode == BC250_GDIOP_BITBLT && Op->SrcAllocationIndex == Op->DstAllocationIndex) {
        /* Equal extents (admission), so the translation is one vector for the whole command. Both
         * terms are inside CoordSane, so the differences cannot overflow. */
        Item->Same = 1u;
        Item->Ddx = Op->DstRect.Left - Op->SrcRect.Left;
        Item->Ddy = Op->DstRect.Top - Op->SrcRect.Top;
    }

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
        if (Item->Same) ClassifySameSurface(Op, (unsigned int)x0, (unsigned int)y0, Item);
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

int Bc250GdiHeaderFits(unsigned int BufferBytes, unsigned int Offset, unsigned int HeaderBytes)
{
    if (!HeaderBytes || (HeaderBytes & 3u)) return 0;
    if (Offset > BufferBytes) return 0;
    return HeaderBytes <= BufferBytes - Offset;
}

int Bc250GdiCommandSize(const BC250_GDI_CMD_VIEW* View, unsigned int* Next, unsigned int* Why)
{
    if (!Next || !Why) return 0;
    *Next = 0;
    *Why = BC250_GDI_WHY_MALFORMED;
    if (!View) return 0;
    /* The fixed part of this opcode's command is at least the header, and the header is what the
     * caller has already read. A MinBytes below it would admit a command whose own opcode and size
     * are not inside it. */
    if (View->MinBytes < View->HeaderBytes) return 0;
    if (!Bc250GdiHeaderFits(View->BufferBytes, View->Offset, View->HeaderBytes)) return 0;
    if (!Bc250GdiWalkStep(View->BufferBytes, View->Offset, View->CommandBytes, View->MinBytes, Next))
        return 0;
    return 1;
}

int Bc250GdiCommandRects(const BC250_GDI_CMD_VIEW* View, unsigned int* Why)
{
    unsigned long long lowest, limit;

    if (!Why) return BC250_GDI_CMD_SKIP;
    *Why = BC250_GDI_WHY_OUTSIDE;
    if (!View) return BC250_GDI_CMD_SKIP;
    /* The order of the three answers below is the order the translation pass applies them, because
     * a count and a translation that disagree are the defect this function exists to remove. */
    if (View->Escape) { *Why = BC250_GDI_WHY_ESCAPE; return BC250_GDI_CMD_SKIP; }
    if (!View->SubCount || !View->HaveRects) { *Why = BC250_GDI_WHY_EMPTY; return BC250_GDI_CMD_SKIP; }
    *Why = BC250_GDI_WHY_OUTSIDE;
    if (!View->RectBytes || View->MinBytes < View->HeaderBytes) return BC250_GDI_CMD_SKIP;
    if (View->CommandBytes < View->MinBytes) return BC250_GDI_CMD_SKIP;
    /* Addresses, in 64 bits, so that an array below the command cannot become a large offset. The
     * two wrap guards cover a command buffer at the very top of the address space. The floor is the
     * header, not the fixed part: the array may overlap the fixed part, as the diagnostic producer
     * inside dxgkrnl builds it, and such a command is still read inside its own bytes. */
    lowest = View->Base + (unsigned long long)View->Offset + View->HeaderBytes;
    limit = View->Base + (unsigned long long)View->Offset + View->CommandBytes;
    if (lowest < View->Base || limit < View->Base) return BC250_GDI_CMD_SKIP;
    if (View->First < lowest || View->First > limit) return BC250_GDI_CMD_SKIP;
    if ((limit - View->First) / View->RectBytes < View->SubCount) return BC250_GDI_CMD_SKIP;
    *Why = BC250_GDI_WHY_DRAW;
    return BC250_GDI_CMD_DRAW;
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
