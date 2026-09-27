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

BC250_GFX_BLIT_RESULT Bc250EmitGfxBltList(
    const BC250_BLIT_SURFACE* source, const BC250_BLIT_SURFACE* destination,
    const BC250_BLIT_RECT* sourceRect, const BC250_BLIT_RECT* destinationRect,
    const BC250_BLIT_RECT* dirty, unsigned int count,
    unsigned long long sourceBase, unsigned long long destinationBase,
    unsigned int offset, unsigned int* next, unsigned int* buffer,
    unsigned int capacity, unsigned int* written)
{
    BC250_BLIT_PLAN plan;
    BC250_BLIT_CURSOR cursor, end;
    unsigned int i, n = count ? count : 1u, ignored, maximum = Bc250GfxCopyMaxBytes();
    unsigned int perRow, packets, skip = offset, used = 0;
    unsigned long long total = 0;
    BC250_GFX_BLIT_RESULT result;
    if (!next || !written) return Bc250GfxBltInvalid;
    *next = offset; *written = 0;
    if (!source || !destination || !sourceRect || !destinationRect ||
        (count && !dirty) || !source->Bytes || !destination->Bytes ||
        (sourceBase & 3u) || (destinationBase & 3u) ||
        sourceBase > ~0ull - (source->Bytes - 1u) ||
        destinationBase > ~0ull - (destination->Bytes - 1u)) return Bc250GfxBltInvalid;
    if (sourceBase <= destinationBase ? destinationBase - sourceBase < source->Bytes :
        sourceBase - destinationBase < destination->Bytes) return Bc250GfxBltInvalid;
    /* Full-list preflight must finish before even the first output word. */
    for (i = 0; i < n; i++) {
        if (Bc250PlanBlt(source, destination, sourceRect, destinationRect,
            count ? dirty + i : 0, &plan) == Bc250BltInvalid) return Bc250GfxBltInvalid;
        cursor.Row = cursor.ByteInRow = 0;
        result = Bc250EmitGfxBlt(&plan, sourceBase, destinationBase, &cursor, &end, 0, 0, &ignored);
        if (result != Bc250GfxBltNoSpace && result != Bc250GfxBltDone) return Bc250GfxBltInvalid;
        perRow = plan.RowBytes / maximum + (plan.RowBytes % maximum != 0);
        total += (unsigned long long)perRow * plan.Rows;
        if (total > ~0u) return Bc250GfxBltInvalid;
    }
    if (offset > total) return Bc250GfxBltInvalid;
    if (offset == total) return Bc250GfxBltDone;
    if (capacity < 7u) return Bc250GfxBltNoSpace;
    if (!buffer) return Bc250GfxBltInvalid;
    for (i = 0; i < n; i++) {
        /* Immutable input and preflight above prove this plan and packet count. */
        (void)Bc250PlanBlt(source, destination, sourceRect, destinationRect,
            count ? dirty + i : 0, &plan);
        perRow = plan.RowBytes / maximum + (plan.RowBytes % maximum != 0);
        packets = perRow * plan.Rows;
        if (skip >= packets) { skip -= packets; continue; }
        cursor.Row = skip / perRow;
        cursor.ByteInRow = (skip % perRow) * maximum;
        result = Bc250EmitGfxBlt(&plan, sourceBase, destinationBase, &cursor, &end,
            buffer + used, capacity - used, &ignored);
        used += ignored;
        skip = 0;
        if (result == Bc250GfxBltMore || capacity - used < 7u) break;
    }
    *written = used; *next = offset + used / 7u;
    return *next == (unsigned int)total ? Bc250GfxBltDone : Bc250GfxBltMore;
}

static unsigned int PresentWord(const unsigned char* p)
{
    return (unsigned int)p[0] | ((unsigned int)p[1]<<8) |
        ((unsigned int)p[2]<<16) | ((unsigned int)p[3]<<24);
}
static int PresentRange(unsigned long long address, unsigned int bytes)
{
    /* GFX IB start is DWORD aligned; its length is padded to8dwords. */
    return address && !(address&3u) && bytes && !(bytes&31u) &&
        address<=~0ull-(bytes-1u);
}
int Bc250GfxPresentRecord(void* record, unsigned int capacity,
    unsigned long long address, unsigned int bytes)
{
    unsigned int words[6],i,j;unsigned char* p=(unsigned char*)record;
    if(!record || capacity<BC250_GFX_PRESENT_RECORD_BYTES || !PresentRange(address,bytes))return 0;
    words[0]=BC250_GFX_PRESENT_MAGIC;words[1]=1;words[2]=BC250_GFX_PRESENT_RECORD_BYTES;
    words[3]=bytes;words[4]=(unsigned int)address;words[5]=(unsigned int)(address>>32);
    for(i=0;i<6;i++)for(j=0;j<4;j++)p[i*4+j]=(unsigned char)(words[i]>>(8*j));
    return 1;
}
int Bc250GfxPresentMatches(const void* record, unsigned int capacity,
    unsigned long long address, unsigned int bytes)
{
    const unsigned char* p=(const unsigned char*)record;
    if(!record || capacity<BC250_GFX_PRESENT_RECORD_BYTES || !PresentRange(address,bytes))return 0;
    return PresentWord(p)==BC250_GFX_PRESENT_MAGIC && PresentWord(p+4)==1 &&
        PresentWord(p+8)==BC250_GFX_PRESENT_RECORD_BYTES && PresentWord(p+12)==bytes &&
        (((unsigned long long)PresentWord(p+20)<<32)|PresentWord(p+16))==address;
}

BC250_GFX_BLIT_RESULT Bc250EmitGfxPresentBltList(
    const BC250_BLIT_SURFACE* source, const BC250_BLIT_SURFACE* destination,
    const BC250_BLIT_RECT* sourceRect, const BC250_BLIT_RECT* destinationRect,
    const BC250_BLIT_RECT* dirty, unsigned int count,
    unsigned long long sourceBase, unsigned long long destinationBase,
    unsigned int offset, unsigned int* next, unsigned int* buffer,
    unsigned int capacity, unsigned int* written)
{
    BC250_GFX_BLIT_RESULT result;
    unsigned int copied,i;
    if (!next || !written) return Bc250GfxBltInvalid;
    *next=offset;*written=0;
    if (capacity<16u || (capacity&7u)) return Bc250GfxBltNoSpace;
    if (!buffer) return Bc250GfxBltInvalid;
    result=Bc250EmitGfxBltList(source,destination,sourceRect,destinationRect,
        dirty,count,sourceBase,destinationBase,offset,next,
        buffer+BC250_GFX_ACQUIRE_DWORDS,capacity-BC250_GFX_ACQUIRE_DWORDS,&copied);
    if (result!=Bc250GfxBltDone && result!=Bc250GfxBltMore) return result;
    (void)Bc250EmitGfxAcquire(buffer,capacity);
    for(i=BC250_GFX_ACQUIRE_DWORDS+copied;i<capacity;i++)buffer[i]=Bc250GfxCopyNop();
    *written=capacity;
    return result;
}

void Bc250GfxPresentInvalidate(void* record, unsigned int capacity)
{
    unsigned char* p=(unsigned char*)record;
    if (p && capacity>=4) p[0]=p[1]=p[2]=p[3]=0;
}
