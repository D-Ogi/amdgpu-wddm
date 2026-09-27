/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "gfx_blt.h"
#include "gfx_copy.h"

BC250_GFX_BLIT_RESULT Bc250EmitGfxBlt(const BC250_BLIT_PLAN* plan,
    unsigned long long sourceBase, unsigned long long destinationBase,
    const BC250_BLIT_CURSOR* cursor, BC250_BLIT_CURSOR* next,
    unsigned int* buffer, unsigned int capacity, unsigned int* written)
{
    BC250_BLIT_CURSOR at;
    unsigned long long source, destination, sourceSpan, destinationSpan;
    unsigned int available = capacity / 7u, count, emitted = 0, maximum;
    if (!written || !next || !cursor) return Bc250GfxBltInvalid;
    at = *cursor; *next = at; *written = 0;
    if (!plan || at.Row > plan->Rows || (at.ByteInRow & 3u)) return Bc250GfxBltInvalid;
    if (!plan->Rows)
        return (!plan->RowBytes && !at.Row && !at.ByteInRow) ? Bc250GfxBltDone : Bc250GfxBltInvalid;
    if (!plan->RowBytes || (plan->RowBytes & 3u) ||
        (plan->SourcePitch & 3u) || (plan->DestinationPitch & 3u) ||
        (plan->SourceOffset & 3u) || (plan->DestinationOffset & 3u) ||
        plan->SourcePitch < plan->RowBytes || plan->DestinationPitch < plan->RowBytes ||
        (at.Row == plan->Rows ? at.ByteInRow != 0 : at.ByteInRow >= plan->RowBytes))
        return Bc250GfxBltInvalid;
    sourceSpan = (unsigned long long)(plan->Rows - 1u) * plan->SourcePitch + plan->RowBytes;
    destinationSpan = (unsigned long long)(plan->Rows - 1u) * plan->DestinationPitch + plan->RowBytes;
    if (sourceBase > ~0ull - plan->SourceOffset || destinationBase > ~0ull - plan->DestinationOffset)
        return Bc250GfxBltInvalid;
    source = sourceBase + plan->SourceOffset;
    destination = destinationBase + plan->DestinationOffset;
    if (source > ~0ull - (sourceSpan - 1u) || destination > ~0ull - (destinationSpan - 1u))
        return Bc250GfxBltInvalid;
    if (source <= destination ? destination - source < sourceSpan : source - destination < destinationSpan)
        return Bc250GfxBltInvalid;
    if (at.Row == plan->Rows) return Bc250GfxBltDone;
    if (!available) return Bc250GfxBltNoSpace;
    if (!buffer) return Bc250GfxBltInvalid;
    maximum = Bc250GfxCopyMaxBytes();
    while (available && at.Row < plan->Rows) {
        int last;
        count = plan->RowBytes - at.ByteInRow;
        if (count > maximum) count = maximum;
        last = available == 1u || (at.Row == plan->Rows - 1u && count == plan->RowBytes - at.ByteInRow);
        /* Every condition checked by the span emitter is proved above. */
        (void)Bc250EmitGfxCopySpan(buffer + emitted, 7u,
            source + (unsigned long long)at.Row * plan->SourcePitch + at.ByteInRow,
            destination + (unsigned long long)at.Row * plan->DestinationPitch + at.ByteInRow,
            count, emitted == 0, last);
        emitted += 7u; available--;
        at.ByteInRow += count;
        if (at.ByteInRow == plan->RowBytes) { at.ByteInRow = 0; at.Row++; }
    }
    *written = emitted; *next = at;
    return at.Row == plan->Rows ? Bc250GfxBltDone : Bc250GfxBltMore;
}
