/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "gdi_batch.h"
#include "gdi_emit.h"
#include "gdi_dispatch.h"
#include "gfx_copy.h"

/*
 * The capacity contract of gdi_batch.h, and the proof that it holds.
 *
 * Write I(k) for the state before the caller offers item k, with r sub-rectangles still to come,
 * item k included:
 *
 *     Used + r * dispatchMax + (CodeVa == 0 ? codeMax : 0) + pad <= Capacity
 *
 * where pad is the ring padding the tail may need. Bc250GdiBatchBegin establishes I(1) for r = N,
 * because that is exactly the sum it compares with Capacity. Each of the three things that can
 * happen to item k preserves it:
 *
 *   - the dispatch path. Its cost is at most codeMax + dispatchMax when the code block is not yet
 *     in the buffer and at most dispatchMax after, and the term for the code block is gone from the
 *     next state, so I(k+1) follows from I(k) by arithmetic.
 *   - the row path. It is offered only the dwords left over after the whole right-hand side of
 *     I(k+1), so whatever it writes, I(k+1) holds by construction.
 *   - a refusal. Used does not move and r falls.
 *
 * I(k) with r >= 1 is more than one dispatch needs, so after Begin has said yes no admitted
 * sub-rectangle can be refused for want of space, and BC250_GDI_BATCH_NO_SPACE is unreachable.
 * It is still returned rather than ignored, because an unreachable state that corrupts a command
 * stream is worse than one that stops it, and because the host test asserts that it never happens.
 *
 * Capacity itself is bounded before any of this starts (rule 3 of gdi_batch.h): Begin clamps it to
 * BC250_GDI_IB_DWORDS_MAX, the length one indirect buffer can carry. Every term of the induction is
 * inside Capacity, the row path included, because the row path is offered the dwords left over
 * after the whole right-hand side of I(k+1) and nothing else. So a DMA buffer larger than one
 * indirect buffer cannot turn into a stream the submitter would have to refuse, and the sentence
 * "an item takes the dispatch instead" covers that case too: the rows of a tall rectangle stop at
 * the capacity like everything else.
 */

unsigned int Bc250GdiBatchDwords(unsigned int subRects)
{
    unsigned long long need = (unsigned long long)BC250_GFX_ACQUIRE_DWORDS +
                              (unsigned long long)Bc250GdiCodeDwordsMax() +
                              (unsigned long long)subRects * Bc250GdiDispatchDwordsMax() +
                              (unsigned long long)(BC250_GDI_RING_GRANULE - 1u);
    return need > 0xFFFFFFFFull ? 0u : (unsigned int)need;
}

/* The input bound of gdi_batch.h, and the size that follows from it. Division, not multiplication,
 * so nothing here can overflow: the largest CommandBytes an unsigned int can hold gives a
 * sub-rectangle count under 2^28, and Bc250GdiBatchDwords saturates to 0 above 2^32 dwords. */
unsigned int Bc250GdiMaxSubRects(unsigned int commandBytes)
{
    if (commandBytes <= BC250_GDI_RECT_FLOOR_BYTES) return 0u;
    return (commandBytes - BC250_GDI_RECT_FLOOR_BYTES) / BC250_GDI_SUBRECT_BYTES;
}

unsigned int Bc250GdiCallDwords(unsigned int commandBytes)
{
    return Bc250GdiBatchDwords(Bc250GdiMaxSubRects(commandBytes));
}

/* What one call may write: the space this call was given, never more than one indirect buffer can
 * carry, and never more than the part of that length this DMA buffer has left. dxgkrnl may fill one
 * buffer with several calls before it submits it, and the submitter encodes the whole buffer as one
 * indirect buffer, so the second term is what keeps a sequence of guaranteed calls inside the one
 * length the packet can name. */
unsigned int Bc250GdiCallCapacity(unsigned int dmaDwords, unsigned int aheadDwords)
{
    unsigned int room;
    if (aheadDwords >= BC250_GDI_IB_DWORDS_MAX) return 0u;
    room = BC250_GDI_IB_DWORDS_MAX - aheadDwords;
    return dmaDwords < room ? dmaDwords : room;
}

/* The DMA buffers of one GDI context (rule 3 of gdi_batch.h). Three plain functions over the
 * structure, so the host test drives every ordering of calls and submissions the DDI does not
 * exclude. The caller serialises them; nothing here reads a clock, a lock or an address order.
 *
 * A slot is free when Base is 0. A slot with Base set and Dwords == 0 is a buffer this context was
 * offered and wrote nothing into, which is a place to start from and not output to protect, so
 * another buffer may take that slot. A slot with Dwords != 0 is output of ours that no submission
 * has carried away yet, and nothing takes that slot until Bc250GdiDmaSubmitted releases it. */
int Bc250GdiDmaClaim(BC250_GDI_DMA_MAP* map, unsigned long long windowVa, unsigned int pageBytes,
                     unsigned int* ahead)
{
    unsigned int i, empty = BC250_GDI_DMA_SLOTS, spare = BC250_GDI_DMA_SLOTS;

    if (ahead) *ahead = 0u;
    if (!map || !ahead || windowVa == 0 || pageBytes == 0u) return BC250_GDI_DMA_UNKNOWN;

    for (i = 0; i < BC250_GDI_DMA_SLOTS; i++)
    {
        BC250_GDI_DMA_SLOT* slot = &map->Slots[i];
        unsigned long long end;

        if (slot->Base == 0)
        {
            if (empty == BC250_GDI_DMA_SLOTS) empty = i;
            continue;
        }
        if (slot->Base == windowVa)
        {
            /* The same buffer from its start again. Whatever we wrote into it before is behind us:
             * either a submission this driver did not see took it away, or dxgkrnl offers the
             * buffer again from the start, and in both readings nothing of ours is ahead of this
             * call. The slot starts again with it. */
            slot->Dwords = 0u;
            return BC250_GDI_DMA_RESTART;
        }
        end = slot->Base + (unsigned long long)slot->Dwords * 4u;
        if (slot->Dwords != 0u && windowVa == end)
        {
            *ahead = slot->Dwords;
            return BC250_GDI_DMA_APPEND;
        }
        if (windowVa > slot->Base && windowVa < end)
        {
            /* A window inside output of ours that nothing has submitted. No reading of the DDI
             * produces it, and the dwords ahead of it are not the ones we recorded, so this call
             * is not translated at all. */
            return BC250_GDI_DMA_UNKNOWN;
        }
        if (slot->Dwords == 0u && spare == BC250_GDI_DMA_SLOTS) spare = i;
    }
    /* No buffer of this context explains the window. The DDI gives the start of a DMA buffer as 4
     * KB aligned, so a window on that boundary is the start of a buffer this driver has not written
     * into, and a window off it is a position this driver cannot place. */
    if ((windowVa & (unsigned long long)(pageBytes - 1u)) != 0) return BC250_GDI_DMA_UNKNOWN;
    if (empty == BC250_GDI_DMA_SLOTS) empty = spare;
    if (empty == BC250_GDI_DMA_SLOTS) return BC250_GDI_DMA_FULL;
    map->Slots[empty].Base = windowVa;
    map->Slots[empty].Dwords = 0u;
    return BC250_GDI_DMA_FRESH;
}

int Bc250GdiDmaWrote(BC250_GDI_DMA_MAP* map, unsigned long long windowVa, unsigned int ahead,
                     unsigned int dwords)
{
    unsigned int i;

    if (!map || windowVa == 0 || dwords == 0u) return 0;
    for (i = 0; i < BC250_GDI_DMA_SLOTS; i++)
    {
        BC250_GDI_DMA_SLOT* slot = &map->Slots[i];
        if (slot->Base == 0) continue;
        if (slot->Base + (unsigned long long)ahead * 4u != windowVa) continue;
        if (slot->Dwords != ahead) continue;
        /* The length of this buffer stays inside what one indirect buffer can carry, which is what
         * the capacity of every call into it was computed against. */
        if ((unsigned long long)ahead + (unsigned long long)dwords > BC250_GDI_IB_DWORDS_MAX) return 0;
        slot->Dwords = ahead + dwords;
        return 1;
    }
    return 0;
}

unsigned int Bc250GdiDmaSubmitted(BC250_GDI_DMA_MAP* map, unsigned long long bufferVa,
                                  unsigned int bytes)
{
    unsigned int i, freed = 0;

    if (!map || bufferVa == 0) return 0u;
    for (i = 0; i < BC250_GDI_DMA_SLOTS; i++)
    {
        BC250_GDI_DMA_SLOT* slot = &map->Slots[i];
        if (slot->Base == 0) continue;
        /* Only the buffer this submission carried away: its own address range. A submission of one
         * buffer says nothing about the content of another, which is why no submission clears the
         * whole map. */
        if (slot->Base != bufferVa &&
            !(slot->Base > bufferVa && slot->Base - bufferVa < (unsigned long long)bytes)) continue;
        slot->Base = 0;
        slot->Dwords = 0u;
        freed++;
    }
    return freed;
}

unsigned int Bc250GdiContextDmaBytes(unsigned int commandBytesBound)
{
    unsigned int dwords = Bc250GdiCallDwords(commandBytesBound);
    unsigned long long bytes;
    if (dwords == 0u) return 0u;
    /* A bound whose worst case one indirect buffer cannot carry is not a bound this driver can
     * promise, whatever size of DMA buffer it asked for. */
    if (dwords > BC250_GDI_IB_DWORDS_MAX) return 0u;
    bytes = (unsigned long long)dwords * 4u;
    /* A whole number of 4 KB pages, which is what the video memory manager allocates anyway. */
    bytes = (bytes + 4095u) & ~4095ull;
    if (bytes > BC250_GDI_DMA_BYTES_MAX) return 0u;
    return (unsigned int)bytes;
}

/* The bound this context is created with. The candidates are the bound and its halves down to the
 * floor, so the step down is coarse on purpose: a GDI context that cannot have the whole bound
 * should still have a large one, and the number has to be the same for every context of the same
 * start. Nothing here reads hardware, so the host test drives every aperture the driver accepts. */
unsigned int Bc250GdiContextBound(unsigned long long apertureBytes, unsigned int wanted)
{
    unsigned long long share;
    unsigned int bound;

    if (apertureBytes == 0ull) return BC250_GDI_CMDBUF_FLOOR;
    share = apertureBytes / BC250_GDI_DMA_APERTURE_SHARE;

    /* What an earlier short call asked for, if it is larger than the bound and the share still
     * admits it. This implementation has no on-demand enlargement of a context's DMA buffer and no
     * RenderGdi continuation to ask for one, so it only ever helps the contexts created after that
     * call. */
    if (wanted > BC250_GDI_CMDBUF_BOUND)
    {
        unsigned int bytes = Bc250GdiContextDmaBytes(wanted);
        if (bytes != 0u && (unsigned long long)bytes <= share) return wanted;
    }

    for (bound = BC250_GDI_CMDBUF_BOUND; bound > BC250_GDI_CMDBUF_FLOOR; bound /= 2u)
    {
        unsigned int bytes = Bc250GdiContextDmaBytes(bound);
        if (bytes != 0u && (unsigned long long)bytes <= share) return bound;
    }
    return BC250_GDI_CMDBUF_FLOOR;
}

/* The right-hand side of I(k+1): what the items behind this one are owed. */
static unsigned int BatchReserve(const BC250_GDI_BATCH* batch, unsigned int remaining)
{
    unsigned int behind = remaining > 1u ? remaining - 1u : 0u;
    unsigned long long reserve = (unsigned long long)behind * Bc250GdiDispatchDwordsMax() +
                                 (batch->CodeVa == 0 ? Bc250GdiCodeDwordsMax() : 0u) +
                                 (BC250_GDI_RING_GRANULE - 1u);
    return reserve > 0xFFFFFFFFull ? 0xFFFFFFFFu : (unsigned int)reserve;
}

int Bc250GdiBatchBegin(BC250_GDI_BATCH* batch, unsigned int* out, unsigned int capacity,
                       unsigned long long ib, unsigned int subRects)
{
    unsigned int need, i;
    if (!batch) return 0;
    for (i = 0; i < sizeof(*batch); i++) ((unsigned char*)batch)[i] = 0;
    if (!out || capacity == 0u || ib == 0 || (ib & 3u)) return 0;
    /* Rule 3, held here and not only in the caller: whatever the caller believes it has, the batch
     * works inside the length one indirect buffer can carry. */
    if (capacity > BC250_GDI_IB_DWORDS_MAX) capacity = BC250_GDI_IB_DWORDS_MAX;
    need = Bc250GdiBatchDwords(subRects);
    if (need == 0u || need > capacity) return 0;
    batch->Out = out;
    batch->Capacity = capacity;
    batch->Ib = ib;
    batch->SubRects = subRects;
    return 1;
}

int Bc250GdiBatchItem(BC250_GDI_BATCH* batch, const BC250_GDI_OP* op,
                      const BC250_GDI_SURFACE* surfaces, const unsigned long long* vas,
                      unsigned int count, const BC250_GDI_ITEM* item, unsigned int pattern,
                      unsigned int remaining)
{
    BC250_GDI_ITEM local;
    BC250_GDI_KERNARG arg;
    unsigned long long source = 0;
    unsigned int reserve, budget, got, rows = 0, written = 0;
    int fallback, result;

    if (!batch || !batch->Out || !op || !surfaces || !vas || !item) return BC250_GDI_BATCH_REFUSED;
    if (item->DstWidth == 0u || item->DstHeight == 0u) return BC250_GDI_BATCH_REFUSED;
    if (op->DstAllocationIndex >= count) return BC250_GDI_BATCH_REFUSED;
    if (!surfaces[op->DstAllocationIndex].Present || vas[op->DstAllocationIndex] == 0)
        return BC250_GDI_BATCH_REFUSED;
    if (Bc250GdiOpHasSource(op->OpCode))
    {
        if (op->SrcAllocationIndex >= count) return BC250_GDI_BATCH_REFUSED;
        if (!surfaces[op->SrcAllocationIndex].Present || vas[op->SrcAllocationIndex] == 0)
            return BC250_GDI_BATCH_REFUSED;
        source = vas[op->SrcAllocationIndex];
    }

    local = *item;
    fallback = local.Work == (unsigned int)Bc250GdiWorkCopy ||
               local.Work == (unsigned int)Bc250GdiWorkFill;
    if (!fallback && local.Work != (unsigned int)Bc250GdiWorkKernel)
        return BC250_GDI_BATCH_REFUSED;
    /* The argument block of the bounded path is built first, so an item the dispatch could not
     * describe is refused before anything is written and the buffer keeps its shape. */
    if (fallback)
    {
        BC250_GDI_ITEM asKernel = local;
        if (!Bc250GdiItemToKernel(op, &asKernel) ||
            !Bc250GdiKernarg(op, surfaces, vas, count, &asKernel, &arg))
            return BC250_GDI_BATCH_REFUSED;
        local = asKernel;
    }
    else if (!Bc250GdiKernarg(op, surfaces, vas, count, &local, &arg))
    {
        return BC250_GDI_BATCH_REFUSED;
    }

    /* One cache operation for the whole buffer, before the first work of it: the CDD wrote its
     * bitmaps with the processor, and this is what makes those writes visible to the engine's level
     * two read, its instruction reads and its scalar reads. A buffer whose every item is refused
     * after this point carries the cache operation alone, which draws nothing and costs nothing. */
    if (batch->Used == 0u)
    {
        got = Bc250EmitGfxAcquire(batch->Out, batch->Capacity);
        if (got == 0u) return BC250_GDI_BATCH_NO_SPACE;
        batch->Used = got;
    }

    /* The engine path, if it fits beside everything the items behind this one are owed. */
    if (fallback)
    {
        reserve = BatchReserve(batch, remaining);
        budget = batch->Capacity - batch->Used > reserve ? batch->Capacity - batch->Used - reserve : 0u;
        result = Bc250EmitGdiItem(item, source, vas[op->DstAllocationIndex], pattern, 0u, &rows,
                                  batch->Out + batch->Used, budget, &written);
        if (result == BC250_GDI_EMIT_DONE)
        {
            batch->Used += written;
            batch->Dwords += written;
            batch->Items++;
            batch->RowItems++;
            return BC250_GDI_BATCH_ROWS;
        }
        /* MORE and NO_SPACE mean the rows did not fit the budget, which is a reason to dispatch and
         * not to drop: whatever those rows wrote is inside the budget and is overwritten below.
         * INVALID is not about capacity at all, so the item is refused instead. */
        if (result == BC250_GDI_EMIT_INVALID) return BC250_GDI_BATCH_REFUSED;
        batch->Fallbacks++;
    }

    if (batch->CodeVa == 0)
    {
        got = Bc250EmitGdiCode(batch->Out + batch->Used, batch->Capacity - batch->Used,
                               batch->Ib + (unsigned long long)batch->Used * 4u, &batch->CodeVa);
        if (got == 0u) return BC250_GDI_BATCH_NO_SPACE;
        batch->Used += got;
        batch->Dwords += got;
    }
    got = Bc250EmitGdiDispatch(local.Kernel, &arg, batch->CodeVa, batch->Out + batch->Used,
                               batch->Capacity - batch->Used,
                               batch->Ib + (unsigned long long)batch->Used * 4u);
    if (got == 0u) return BC250_GDI_BATCH_NO_SPACE;
    batch->Used += got;
    batch->Dwords += got;
    batch->Items++;
    batch->Dispatches++;
    return BC250_GDI_BATCH_DISPATCH;
}

unsigned int Bc250GdiBatchEnd(BC250_GDI_BATCH* batch)
{
    if (!batch || !batch->Out || batch->Used == 0u) return 0u;
    /* The command processor's own no-operation word, the same one the Present indirect buffer pads
     * with. The padding fits because every state above kept the granule in its reserve. */
    while ((batch->Used & (BC250_GDI_RING_GRANULE - 1u)) != 0u && batch->Used < batch->Capacity)
        batch->Out[batch->Used++] = Bc250GfxCopyNop();
    return batch->Used;
}
