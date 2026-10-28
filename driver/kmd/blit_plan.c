/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "blit_plan.h"

static int SurfaceValid(const BC250_BLIT_SURFACE* s)
{
    return s && s->Width && s->Height && s->Width <= (~0u / 4u) &&
        s->Pitch >= s->Width * 4u && (s->Pitch & 3u) == 0 &&
        (s->Format == Bc250BltBgra8 || s->Format == Bc250BltRgba8) &&
        (unsigned long long)s->Pitch * s->Height <= s->Bytes;
}

static int RectValid(const BC250_BLIT_RECT* r, const BC250_BLIT_SURFACE* s)
{
    return r && r->Left >= 0 && r->Top >= 0 && r->Right >= r->Left &&
        r->Bottom >= r->Top && (unsigned int)r->Right <= s->Width &&
        (unsigned int)r->Bottom <= s->Height;
}

BC250_BLIT_RESULT Bc250PlanBlt(const BC250_BLIT_SURFACE* source,
    const BC250_BLIT_SURFACE* destination, const BC250_BLIT_RECT* sourceRect,
    const BC250_BLIT_RECT* destinationRect, const BC250_BLIT_RECT* dirty,
    BC250_BLIT_PLAN* plan)
{
    BC250_BLIT_RECT clipped;
    unsigned int sourceX, sourceY, rows, rowBytes;
    unsigned long long sourceOffset, destinationOffset;
    if (!plan) return Bc250BltInvalid;
    plan->SourceOffset = plan->DestinationOffset = 0;
    plan->SourcePitch = plan->DestinationPitch = plan->RowBytes = plan->Rows = 0;
    if (!SurfaceValid(source) || !SurfaceValid(destination) ||
        source->Format != destination->Format ||
        !RectValid(sourceRect, source) || !RectValid(destinationRect, destination))
        return Bc250BltInvalid;
    /* After RectValid, all differences fit in a positive signed int. */
    if (sourceRect->Right - sourceRect->Left != destinationRect->Right - destinationRect->Left ||
        sourceRect->Bottom - sourceRect->Top != destinationRect->Bottom - destinationRect->Top)
        return Bc250BltInvalid;
    if (!dirty) dirty = destinationRect;
    if (!RectValid(dirty, destination)) return Bc250BltInvalid;
    clipped.Left = dirty->Left > destinationRect->Left ? dirty->Left : destinationRect->Left;
    clipped.Top = dirty->Top > destinationRect->Top ? dirty->Top : destinationRect->Top;
    clipped.Right = dirty->Right < destinationRect->Right ? dirty->Right : destinationRect->Right;
    clipped.Bottom = dirty->Bottom < destinationRect->Bottom ? dirty->Bottom : destinationRect->Bottom;
    if (clipped.Right <= clipped.Left || clipped.Bottom <= clipped.Top) return Bc250BltEmpty;
    sourceX = (unsigned int)sourceRect->Left + (unsigned int)(clipped.Left - destinationRect->Left);
    sourceY = (unsigned int)sourceRect->Top + (unsigned int)(clipped.Top - destinationRect->Top);
    rowBytes = (unsigned int)(clipped.Right - clipped.Left) * 4u;
    rows = (unsigned int)(clipped.Bottom - clipped.Top);
    sourceOffset = (unsigned long long)sourceY * source->Pitch + (unsigned long long)sourceX * 4u;
    destinationOffset = (unsigned long long)(unsigned int)clipped.Top * destination->Pitch +
        (unsigned long long)(unsigned int)clipped.Left * 4u;
    /* Subtraction form protects the last row, even with large pitched extents. */
    if (sourceOffset > source->Bytes || rowBytes > source->Bytes - sourceOffset ||
        (unsigned long long)(rows - 1u) * source->Pitch > source->Bytes - sourceOffset - rowBytes ||
        destinationOffset > destination->Bytes || rowBytes > destination->Bytes - destinationOffset ||
        (unsigned long long)(rows - 1u) * destination->Pitch > destination->Bytes - destinationOffset - rowBytes)
        return Bc250BltInvalid;
    plan->SourceOffset = sourceOffset;
    plan->DestinationOffset = destinationOffset;
    plan->SourcePitch = source->Pitch;
    plan->DestinationPitch = destination->Pitch;
    plan->RowBytes = rowBytes;
    plan->Rows = rows;
    return Bc250BltCopy;
}

/* The source column (or row) of destination position d, point sampled at the pixel
 * centre. d is inside [d0, d0 + dLength), so every term is not negative, and
 * (2 * 2^32 + 1) * 2^31 fits in 64 bits. */
static long long PresentMap(long long d, long long d0, long long dLength, long long s0, long long sLength)
{
    return s0 + (long long)(((unsigned long long)(2 * (d - d0) + 1) * (unsigned long long)sLength) /
        (2ull * (unsigned long long)dLength));
}

/* The end of the run that starts at a: the first position before end that does not
 * map to the source position after that of its left (or upper) neighbour. */
static int PresentRunEnd(int a, int end, long long d0, long long dLength, long long s0, long long sLength)
{
    long long previous = PresentMap(a, d0, dLength, s0, sLength);
    int e = a + 1;
    while (e < end) {
        long long current = PresentMap(e, d0, dLength, s0, sLength);
        if (current != previous + 1) break;
        previous = current;
        e++;
    }
    return e;
}

BC250_BLIT_RESULT Bc250PlanPresentRun(const BC250_BLIT_SURFACE* source,
    const BC250_BLIT_SURFACE* destination, const BC250_BLIT_RECT* sourceRect,
    const BC250_BLIT_RECT* destinationRect, const BC250_BLIT_RECT* dirty,
    BC250_PRESENT_RUN* run, BC250_BLIT_PLAN* plan)
{
    BC250_BLIT_RECT whole, clipped, runSource, runDestination;
    long long dw, dh, sw, sh;
    int xEnd;
    if (!plan) return Bc250BltInvalid;
    plan->SourceOffset = plan->DestinationOffset = 0;
    plan->SourcePitch = plan->DestinationPitch = plan->RowBytes = plan->Rows = 0;
    if (!run || !SurfaceValid(source) || !SurfaceValid(destination) ||
        source->Format != destination->Format || !destinationRect ||
        !RectValid(sourceRect, source) ||
        destinationRect->Right < destinationRect->Left || destinationRect->Bottom < destinationRect->Top)
        return Bc250BltInvalid;
    if (!dirty) {
        whole.Left = whole.Top = 0;
        /* SurfaceValid bounds Width under 2^30; Height must fit an int as well. */
        if (destination->Height > 0x7fffffffu) return Bc250BltInvalid;
        whole.Right = (int)destination->Width; whole.Bottom = (int)destination->Height;
        dirty = &whole;
    }
    if (!RectValid(dirty, destination)) return Bc250BltInvalid;
    /* An empty rectangle maps nothing: no copy, as in Bc250PlanBlt. */
    if (sourceRect->Right == sourceRect->Left || sourceRect->Bottom == sourceRect->Top ||
        destinationRect->Right == destinationRect->Left || destinationRect->Bottom == destinationRect->Top)
        return Bc250BltEmpty;
    dw = (long long)destinationRect->Right - destinationRect->Left;
    dh = (long long)destinationRect->Bottom - destinationRect->Top;
    sw = (long long)sourceRect->Right - sourceRect->Left;
    sh = (long long)sourceRect->Bottom - sourceRect->Top;
    clipped.Left = dirty->Left > destinationRect->Left ? dirty->Left : destinationRect->Left;
    clipped.Top = dirty->Top > destinationRect->Top ? dirty->Top : destinationRect->Top;
    clipped.Right = dirty->Right < destinationRect->Right ? dirty->Right : destinationRect->Right;
    clipped.Bottom = dirty->Bottom < destinationRect->Bottom ? dirty->Bottom : destinationRect->Bottom;
    if (clipped.Right <= clipped.Left || clipped.Bottom <= clipped.Top) return Bc250BltEmpty;
    if (!run->Started) {
        run->Started = 1; run->X = clipped.Left; run->Y = clipped.Top;
        run->YEnd = PresentRunEnd(clipped.Top, clipped.Bottom, destinationRect->Top, dh, sourceRect->Top, sh);
    }
    if (run->Y == clipped.Bottom) return Bc250BltEmpty;
    /* A cursor that is not on a run start of this geometry is a caller defect. */
    if (run->X < clipped.Left || run->X >= clipped.Right || run->Y < clipped.Top || run->Y > clipped.Bottom ||
        run->YEnd <= run->Y || run->YEnd > clipped.Bottom) return Bc250BltInvalid;
    xEnd = PresentRunEnd(run->X, clipped.Right, destinationRect->Left, dw, sourceRect->Left, sw);
    runDestination.Left = run->X; runDestination.Right = xEnd;
    runDestination.Top = run->Y; runDestination.Bottom = run->YEnd;
    runSource.Left = (int)PresentMap(run->X, destinationRect->Left, dw, sourceRect->Left, sw);
    runSource.Right = runSource.Left + (xEnd - run->X);
    runSource.Top = (int)PresentMap(run->Y, destinationRect->Top, dh, sourceRect->Top, sh);
    runSource.Bottom = runSource.Top + (run->YEnd - run->Y);
    /* The run is a 1:1 copy inside both surfaces; Bc250PlanBlt checks that again. */
    if (Bc250PlanBlt(source, destination, &runSource, &runDestination, 0, plan) != Bc250BltCopy)
        return Bc250BltInvalid;
    run->X = xEnd;
    if (run->X == clipped.Right) {
        run->X = clipped.Left; run->Y = run->YEnd;
        if (run->Y < clipped.Bottom)
            run->YEnd = PresentRunEnd(run->Y, clipped.Bottom, destinationRect->Top, dh, sourceRect->Top, sh);
    }
    return Bc250BltCopy;
}
