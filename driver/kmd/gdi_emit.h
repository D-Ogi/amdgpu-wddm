/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_EMIT_H
#define BC250_GDI_EMIT_H
#include "gdi_render.h"

/* The command stream of the two GDI operations the CP's DMA_DATA engine performs exactly: an
 * equal-extent SRCCOPY bit-block transfer and a PATCOPY colour fill. Both are byte operations with
 * no arithmetic, so there is nothing here for a kernel to round differently and nothing for a host
 * test to approximate: the packets either address the right bytes or they do not.
 *
 * Everything else that gdi_render.c classifies as Bc250GdiWorkKernel is a compute dispatch, which
 * this file does not emit; docs/design/gdi-hw-accel.md increment 2 holds that part.
 *
 * Address obligations are the caller's, as in gfx_blt.h: the bases are GPU virtual addresses of
 * resident allocations, the two spans do not overlap, the command allocation is neither source nor
 * destination, and producer cache ordering is already established. This file adds the rows.
 */

/* Dwords a fill of Bytes needs, and the largest byte count one packet carries (the same chunking
 * as Bc250GfxCopyDwords, because it is the same packet). 0 for zero bytes. */
unsigned int Bc250GdiFillDwords(unsigned int Bytes);

/* One DMA_DATA fill of Bytes from the immediate dword Pattern. Bytes must be a multiple of four:
 * the engine repeats a dword, so a partial dword has no defined meaning here and is refused.
 * Returns the dwords written, or 0 on invalid input or short capacity, having written nothing. */
unsigned int Bc250EmitGdiFillSpan(unsigned int* Buffer, unsigned int CapacityDwords,
    unsigned long long Destination, unsigned int Bytes, unsigned int Pattern,
    int WaitBefore, int SyncAfter);

/* The whole item: every row of a Bc250GdiWorkCopy or Bc250GdiWorkFill plan, in destination row
 * order, with one RAW_WAIT on the first packet and one CP_SYNC on the last. SourceBase and
 * DestinationBase are the allocations' GPU virtual addresses; Item->SrcOffset and Item->DstOffset
 * are added to them.
 *
 * Pattern is the fill dword already in the destination surface's own channel order, which is the
 * caller's one conversion of DXGK_GDIARG_COLORFILL.Color: the DDI documents Color in A8R8G8B8 and
 * a red-first destination needs it swapped once per command, never per row.
 *
 * Offset and Next are row ordinals, so a command that does not fit one DMA buffer resumes at a row
 * boundary and the DDI's MultipassOffset can carry it. Written is the dwords emitted.
 */
#define BC250_GDI_EMIT_INVALID  0
#define BC250_GDI_EMIT_DONE     1
#define BC250_GDI_EMIT_MORE     2
#define BC250_GDI_EMIT_NO_SPACE 3
int Bc250EmitGdiItem(const BC250_GDI_ITEM* Item, unsigned long long SourceBase,
    unsigned long long DestinationBase, unsigned int Pattern, unsigned int Offset,
    unsigned int* Next, unsigned int* Buffer, unsigned int CapacityDwords, unsigned int* Written);

#endif
