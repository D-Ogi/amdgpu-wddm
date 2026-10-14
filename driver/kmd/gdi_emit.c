/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "gdi_emit.h"
#include "gfx_copy.h"
#include "gfx_copy_fields.h"
#include "../amdgpu-import/nvd.h"

/* The fill is the same PACKET3_DMA_DATA as gfx_copy.c's copy with one field changed: SRC_SEL
 * selects DATA instead of SRC_ADDR, and the dword that would have been SRC_ADDR_LO is then the
 * pattern. nvd.h's own comment on PACKET3_DMA_DATA_SRC_SEL lists the four values and names 2 as
 * DATA, and its layout comment names dword 3 "SRC_ADDR_LO or DATA [31:0]"; that is where both
 * numbers below come from, and no offset or shift is written out here.
 *
 * gfx_copy_fields.h (Mesa) and nvd.h (Linux amdgpu) describe the same packet, so the two sets of
 * macros agree field for field; gdi_render_test.c checks that at compile time rather than trusting
 * it. The copy keeps the Mesa macros it was validated with and the fill uses nvd.h, which is the
 * header that documents the DATA selector at all. */
#define GDI_DMA_DATA_SRC_DATA 2
#define GDI_DMA_DATA_DST_L2   3

#define FILL_DWORDS 7u

unsigned int Bc250GdiFillDwords(unsigned int bytes)
{
    /* The same chunking as the copy, because it is the same byte-count field. */
    return Bc250GfxCopyDwords(bytes);
}

unsigned int Bc250EmitGdiFillSpan(unsigned int* buffer, unsigned int capacity,
    unsigned long long destination, unsigned int bytes, unsigned int pattern,
    int waitBefore, int syncAfter)
{
    unsigned int required = Bc250GdiFillDwords(bytes), written = 0, remaining = bytes;
    unsigned int chunk = Bc250GfxCopyMaxBytes();
    if (!buffer || !required || capacity < required) return 0;
    if (bytes & 3u) return 0;                                   /* the engine repeats a dword */
    if (destination > ~0ull - (bytes - 1u)) return 0;
    while (remaining) {
        unsigned int count = remaining > chunk ? chunk : remaining;
        unsigned int control = (unsigned int)(PACKET3_DMA_DATA_DST_SEL(GDI_DMA_DATA_DST_L2) |
            PACKET3_DMA_DATA_SRC_SEL(GDI_DMA_DATA_SRC_DATA));
        unsigned int command = S_506_BYTE_COUNT(count);
        if (count == remaining && syncAfter) control |= (unsigned int)PACKET3_DMA_DATA_CP_SYNC;
        if (!written && waitBefore) command |= (unsigned int)PACKET3_DMA_DATA_CMD_RAW_WAIT;
        buffer[written++] = (unsigned int)PACKET3(PACKET3_DMA_DATA, 5);
        buffer[written++] = control;
        buffer[written++] = pattern;                            /* dword 3: DATA, not SRC_ADDR_LO */
        buffer[written++] = 0;                                   /* dword 4: unused in DATA mode */
        buffer[written++] = (unsigned int)destination;
        buffer[written++] = (unsigned int)(destination >> 32);
        buffer[written++] = command;
        remaining -= count;
        if (remaining) destination += count;
    }
    return written;
}

/* Rows of one item, resumable at a row boundary. The whole footprint is validated before any dword
 * is written, so a no-space answer leaves the buffer untouched and the caller may retry with a
 * larger one; that is the DDI's STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER contract. */
int Bc250EmitGdiItem(const BC250_GDI_ITEM* item, unsigned long long sourceBase,
    unsigned long long destinationBase, unsigned int pattern, unsigned int offset,
    unsigned int* next, unsigned int* buffer, unsigned int capacity, unsigned int* written)
{
    unsigned int rows, row, used = 0, perRow, copy;
    unsigned long long srcRow, dstRow;

    if (!item || !next || !written) return BC250_GDI_EMIT_INVALID;
    *next = offset;
    *written = 0;
    if (!buffer) return BC250_GDI_EMIT_INVALID;
    copy = item->Work == Bc250GdiWorkCopy;
    if (!copy && item->Work != Bc250GdiWorkFill) return BC250_GDI_EMIT_INVALID;
    if (item->Stretch) return BC250_GDI_EMIT_INVALID;            /* not an engine case */
    rows = item->DstHeight;
    if (!rows || !item->RowBytes) return BC250_GDI_EMIT_DONE;    /* an empty item emits nothing */
    if (offset > rows) return BC250_GDI_EMIT_INVALID;
    if (offset == rows) return BC250_GDI_EMIT_DONE;
    if (!item->DstPitch || (copy && !item->SrcPitch)) return BC250_GDI_EMIT_INVALID;
    if (item->RowBytes & 3u) {
        /* A one-byte-per-pixel row - the lookup-table initialisation - need not be a multiple of
         * four, and a fill of it has no dword meaning. A copy of it is fine: DMA_DATA copies bytes.
         */
        if (!copy) return BC250_GDI_EMIT_INVALID;
    }
    perRow = copy ? Bc250GfxCopyDwords(item->RowBytes) : Bc250GdiFillDwords(item->RowBytes);
    if (!perRow) return BC250_GDI_EMIT_INVALID;

    /* Validate every remaining row's addresses before writing anything. */
    for (row = offset; row < rows; row++) {
        dstRow = destinationBase + item->DstOffset + (unsigned long long)row * item->DstPitch;
        if (destinationBase > ~0ull - item->DstOffset) return BC250_GDI_EMIT_INVALID;
        if (dstRow > ~0ull - (item->RowBytes - 1u)) return BC250_GDI_EMIT_INVALID;
        if (copy) {
            srcRow = sourceBase + item->SrcOffset + (unsigned long long)row * item->SrcPitch;
            if (sourceBase > ~0ull - item->SrcOffset) return BC250_GDI_EMIT_INVALID;
            if (srcRow > ~0ull - (item->RowBytes - 1u)) return BC250_GDI_EMIT_INVALID;
            /* The two rows must not overlap. The surfaces are different allocations (admission
             * refuses a command naming one twice), but their virtual addresses are the caller's
             * word, so the row pair is checked here as gfx_copy.c checks its span. */
            if (srcRow <= dstRow ? dstRow - srcRow < item->RowBytes : srcRow - dstRow < item->RowBytes)
                return BC250_GDI_EMIT_INVALID;
        }
    }
    if (capacity < perRow) return BC250_GDI_EMIT_NO_SPACE;       /* not even one row fits */

    for (row = offset; row < rows; row++) {
        unsigned int got;
        int last;
        if (capacity - used < perRow) break;
        dstRow = destinationBase + item->DstOffset + (unsigned long long)row * item->DstPitch;
        last = (row + 1u == rows) || (capacity - used - perRow < perRow);
        if (copy) {
            srcRow = sourceBase + item->SrcOffset + (unsigned long long)row * item->SrcPitch;
            got = Bc250EmitGfxCopySpan(buffer + used, capacity - used, srcRow, dstRow,
                                       item->RowBytes, row == offset, last);
        } else {
            got = Bc250EmitGdiFillSpan(buffer + used, capacity - used, dstRow, item->RowBytes,
                                       pattern, row == offset, last);
        }
        if (!got) break;
        used += got;
    }
    if (!used) return BC250_GDI_EMIT_NO_SPACE;
    *written = used;
    *next = offset + used / perRow;
    return *next < rows ? BC250_GDI_EMIT_MORE : BC250_GDI_EMIT_DONE;
}
