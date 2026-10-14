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

unsigned int Bc250GdiContextDmaBytes(unsigned int commandBytesBound)
{
    unsigned int dwords = Bc250GdiCallDwords(commandBytesBound);
    unsigned long long bytes;
    if (dwords == 0u) return 0u;
    bytes = (unsigned long long)dwords * 4u;
    /* A whole number of 4 KB pages, which is what the video memory manager allocates anyway. */
    bytes = (bytes + 4095u) & ~4095ull;
    if (bytes > BC250_GDI_DMA_BYTES_MAX) return 0u;
    return (unsigned int)bytes;
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
