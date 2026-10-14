/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_BATCH_H
#define BC250_GDI_BATCH_H
#include "gdi_render.h"

/*
 * The capacity contract of one DxgkDdiRenderGdi call, and the choice of path for each admitted
 * sub-rectangle.
 *
 * The guaranteed contract DMA buffer model gives one DMA buffer for one command buffer and no
 * second pass: "The display miniport driver must be able to translate one command buffer into
 * exactly one DMA buffer and one patch-location list. If this translation is not possible, the
 * user-mode command buffer is, by definition, invalid. The display miniport driver cannot return
 * status that indicates it is out of DMA buffer space"
 * (display/using-the-guaranteed-contract-dma-buffer-model.md, ref revision 110f60ea). RenderGdi
 * also has no status for it: its return table is STATUS_SUCCESS, and "All other return values will
 * lead to the OS bugcheck" (ref/ddi-display/d3dkmddi.md, DXGKDDI_RENDERGDI).
 *
 * So capacity cannot be a per-item question. If it were, an early item with a tall rectangle would
 * spend the buffer on its own rows and a later item would be dropped from a call that reported
 * success, which is what a review of increment 1 reproduced on this host: a 2 by 8192 ColorFill
 * with two 1 by 8192 sub-rectangles filled the first column in 57344 dwords and lost the second.
 *
 * Two rules remove that class:
 *
 *   1. Every admitted sub-rectangle has a bounded cost, whatever its size: one compute dispatch of
 *      the kernel of its operation, which is a fixed number of dwords. The per-row DMA_DATA path
 *      is cheaper for a short rectangle and unbounded for a tall one, so it is an optimisation and
 *      never an obligation. An item whose rows do not fit the dwords left over after the bounded
 *      cost of everything behind it takes the dispatch instead.
 *   2. The whole batch is checked before anything is written. Bc250GdiBatchBegin takes the number
 *      of sub-rectangles the command buffer holds and refuses the call if their bounded cost, the
 *      cache operation, the code block and the ring padding do not fit. The caller then writes an
 *      empty DMA buffer, which draws nothing and leaves nothing half drawn.
 *
 * Rule 1 is what makes rule 2 a contract rather than a hope: after Begin has said yes, no admitted
 * sub-rectangle can be refused for want of space, and the host test asserts exactly that over a
 * set of batches that includes the counterexample above.
 *
 * Rule 3 is what makes rule 2 harmless: the DMA buffer this driver asks for is large enough that
 * Begin cannot refuse any command buffer the producer is able to build. That needs a bound on the
 * input, and the DDI gives one:
 *
 *   "CommandSize: The size of the current command, in bytes. This is equal to the number of bytes
 *    from the beginning of DXGK_RENDERKM_COMMAND up to the next command."
 *   "Each command varies in length depending on the value of the OpCode member and the number of
 *    sub-rectangles in the command."
 *      (ref/ddi-display/d3dkmddi.md:27128 and :27169, DXGK_RENDERKM_COMMAND, WDK 10.0.26100)
 *
 * A command's length can depend on how many sub-rectangles it has only if they are inside it, and
 * CommandSize reaches to the next command, so the array of one command lies inside that command and
 * the arrays of different commands do not overlap. wddm.c enforces exactly that reading and drops a
 * pSubRects that points outside the command it belongs to. Therefore, writing L for CommandLength
 * and taking ColorFill as the smallest command that can carry a sub-rectangle at all:
 *
 *     sum over commands of 16 * NumSubRects <= L - 48,  so  N <= (L - 48) / 16
 *
 * and with the bounded cost of rule 1 the whole call needs at most
 *
 *     acquire + codeMax + ((L - 48) / 16) * dispatchMax + granule - 1   dwords,
 *
 * which is Bc250GdiCallDwords(L). It is a constant factor of the input: 324 bytes of DMA buffer per
 * 16 bytes of command buffer, 20.25 to one. So a driver that asks dxgkrnl for
 * Bc250GdiContextDmaBytes(bound) bytes can translate every command buffer up to bound, and the
 * refusal in Begin is then unreachable for the whole admitted input domain, not merely for the
 * inputs anyone has tried.
 *
 * What the DDI does not publish is the size of the command buffer dxgkrnl gives a GDI context, so
 * that bound is this driver's choice (BC250_GDI_CMDBUF_BOUND in wddm.c) and not the OS's promise.
 * A call above it is counted and logged, every GDI context created afterwards asks for a DMA buffer
 * large enough for it, and docs/design/gdi-hw-accel.md says what such a call shows on screen. The
 * DMA buffer "can grow and shrink after the context is created; however, the DMA buffer can never
 * shrink smaller than the starting size that DmaBufferSize specifies" (DXGK_CONTEXTINFO), so the
 * size asked for here is a floor and the capacity each call reports is used as it comes.
 *
 * This file is plain C over plain integers, like gdi_render.c, so the contract is tested on this PC
 * and not on the lab. Residency, cache state and the addresses themselves stay the caller's
 * obligation.
 */

/* The ring takes a 32-byte aligned indirect buffer, which is eight dwords. */
#define BC250_GDI_RING_GRANULE 8u

/* sizeof(RECT) and the smallest DXGK_RENDERKM_COMMAND that can carry a sub-rectangle, which is
 * FIELD_OFFSET(DXGK_RENDERKM_COMMAND, Command) + sizeof(DXGK_GDIARG_COLORFILL). Both are held
 * against the WDK headers by C_ASSERT in wddm.c, so this file stays plain C over plain integers. */
#define BC250_GDI_SUBRECT_BYTES 16u
#define BC250_GDI_MIN_COMMAND_BYTES 48u

/* The largest command buffer this driver undertakes to translate in one call. Nothing in the DDI
 * publishes the size dxgkrnl actually uses for a GDI context, so this is a choice, and it is the
 * number the GDI context's DMA buffer size is computed from: 96 KB of command buffer holds at most
 * 6141 sub-rectangles and costs about 1.9 MB of DMA buffer. The field's real figure is measured
 * (GdiRenderPeakCommandLength), a call above the bound is counted, and the next GDI context asks
 * for a buffer that fits it. */
#define BC250_GDI_CMDBUF_BOUND (96u * 1024u)

/* A ceiling on what any adaptation may ask dxgkrnl for, so a nonsense CommandLength cannot turn
 * into a nonsense allocation. It covers a command buffer of about 1.6 MB. */
#define BC250_GDI_DMA_BYTES_MAX (32u * 1024u * 1024u)

/* What Bc250GdiBatchItem did with an item. */
#define BC250_GDI_BATCH_REFUSED  0   /* the item is not addressable; nothing was written */
#define BC250_GDI_BATCH_ROWS     1   /* the DMA_DATA rows of the engine path */
#define BC250_GDI_BATCH_DISPATCH 2   /* a compute dispatch */
#define BC250_GDI_BATCH_NO_SPACE 3   /* unreachable after Begin said yes; nothing was written */

/* The state of one call's translation. The caller sets nothing by hand: Begin fills it and the
 * other two functions own it. Out is the DMA buffer in dwords and Ib its GPU virtual address. */
typedef struct BC250_GDI_BATCH {
    unsigned int* Out;
    unsigned int Capacity;              /* dwords of Out */
    unsigned long long Ib;
    unsigned int Used;                  /* dwords written so far */
    unsigned long long CodeVa;          /* 0 until the code block is in this buffer */
    unsigned int SubRects;              /* what Begin was promised */
    /* What happened, for the caller's counters. */
    unsigned int Items, RowItems, Dispatches, Fallbacks, Dwords;
} BC250_GDI_BATCH;

/* The worst case a batch of SubRects sub-rectangles can cost, in dwords, including the cache
 * operation, the code block, one bounded dispatch for each sub-rectangle and the ring padding.
 * 0 when the number cannot be expressed in an unsigned int, which refuses the call. */
unsigned int Bc250GdiBatchDwords(unsigned int SubRects);

/* The most sub-rectangles a command buffer of CommandBytes bytes can hold, by the bound above. */
unsigned int Bc250GdiMaxSubRects(unsigned int CommandBytes);

/* The worst case any command buffer of CommandBytes bytes can cost, in dwords: the capacity a call
 * must have for Bc250GdiBatchBegin to be unable to refuse it. 0 when it does not fit an unsigned
 * int. */
unsigned int Bc250GdiCallDwords(unsigned int CommandBytes);

/* The DMA buffer size to ask dxgkrnl for, in bytes, so that every command buffer up to
 * CommandBytesBound translates: Bc250GdiCallDwords rounded up to a page and clamped to
 * BC250_GDI_DMA_BYTES_MAX. 0 when the bound cannot be met inside that ceiling. */
unsigned int Bc250GdiContextDmaBytes(unsigned int CommandBytesBound);

/* Begin a translation into Out. Returns 0 when this batch cannot be guaranteed, having written
 * nothing: the caller must then emit nothing at all and count the refusal. */
int Bc250GdiBatchBegin(BC250_GDI_BATCH* Batch, unsigned int* Out, unsigned int Capacity,
                       unsigned long long Ib, unsigned int SubRects);

/* One admitted sub-rectangle. Remaining is how many sub-rectangles the caller may still offer,
 * this one included, so that this item cannot spend the capacity the others are owed. Pattern is
 * the fill dword of a Bc250GdiWorkFill item, in the destination surface's own channel order.
 *
 * Returns a BC250_GDI_BATCH_* result. The cache operation is emitted before the first item, and
 * the code block before the first dispatch, so a batch whose items are all refused writes nothing.
 */
int Bc250GdiBatchItem(BC250_GDI_BATCH* Batch, const BC250_GDI_OP* Op,
                      const BC250_GDI_SURFACE* Surfaces, const unsigned long long* Vas,
                      unsigned int Count, const BC250_GDI_ITEM* Item, unsigned int Pattern,
                      unsigned int Remaining);

/* Pad the tail to the ring granule and return the dwords the caller must advance pDmaBuffer by. 0
 * when nothing was emitted. */
unsigned int Bc250GdiBatchEnd(BC250_GDI_BATCH* Batch);

#endif
