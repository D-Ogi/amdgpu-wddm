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
