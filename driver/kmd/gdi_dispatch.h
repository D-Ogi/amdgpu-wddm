/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#ifndef BC250_GDI_DISPATCH_H
#define BC250_GDI_DISPATCH_H
#include "gdi_render.h"

/*
 * The command stream of the four GDI operations the command processor cannot perform: a compute
 * dispatch of one of the six kernels of gdi_kernels.cl, which the driver image carries as machine
 * code (gdi_kernels.generated.h). Increment 2 of docs/design/gdi-hw-accel.md.
 *
 * Plain C over plain integers, like gdi_emit.h, so that the whole thing is host-tested on this PC:
 * the test decodes the packets this file writes, runs the kernels' own source over the grid they
 * describe and compares the result with the reference model, byte for byte.
 *
 * Three things go into the DMA buffer, and nothing anywhere else:
 *
 *   1. the code, once per DMA buffer, behind a PACKET3_NOP whose payload it is. The driver has no
 *      allocation of its own to keep a shader in (a kernel-mode GDI context owns nothing but the
 *      buffer dxgkrnl hands it), and this adapter reports no no-execute support for its page
 *      tables, so the buffer's own pages are executable: the design page records the evidence.
 *   2. the kernel arguments, once per sub-rectangle, behind another NOP.
 *   3. the register writes and the dispatch itself.
 *
 * Address obligations are the caller's, as in gdi_emit.h and gfx_blt.h: the bases are GPU virtual
 * addresses of resident allocations and the buffer is neither a source nor a destination of the
 * work it carries. This file adds the arithmetic.
 */

/* The kernel's argument list, which is gdi_kernels.cl's GDI_ARGS field for field. gdi_dispatch.c
 * holds it against the compiler's own metadata of the code object (BC250_GDI_KERNARG_BYTES and the
 * per-argument offsets the build gate reads), so a reordered argument breaks the build.
 *
 * The four addresses are GPU virtual addresses: the destination pixel of this sub-rectangle, the
 * SOURCE SURFACE'S ORIGIN (the transform gives absolute source coordinates), the alpha pixel under
 * the same destination pixel, and the selected gamma row.
 */
typedef struct BC250_GDI_KERNARG {
    unsigned long long Dst, Src, Alpha, Gamma;
    unsigned int DstPitch, SrcPitch, AlphaPitch, Width, Height;
    unsigned int Color, Color2, Rop, Flags, Sca;
    int SrcLeft, SrcTop;
    unsigned int Dx0, Dy0, Ws, Wd, Hs, Hd;
} BC250_GDI_KERNARG;

/* The argument block of one admitted sub-rectangle, from the command, its allocation list and the
 * plan. Vas is indexed by allocation-list slot exactly as Surfaces is, and Count is the number of
 * slots; the indices in Op are the slot numbers the caller rewrote them to.
 *
 * This is also where the one per-command colour conversion of a kernel operation happens, because
 * each kernel reads Color in one channel order and only this function knows which. Every Color
 * field of the DDI is in its surface's own format, which the WDK header states for each of them
 * ("Color in the format of the destination surface" on DXGK_GDIARG_COLORFILL, "Foreground color in
 * a 32-bit surface format" on DXGK_GDIARG_CLEARTYPEBLEND) and which the header's priority over a
 * documentation page settles (CLAUDE.md, References). So a COLORFILL's colour is passed through
 * untouched, because that kernel is bitwise in the destination's own order, while the
 * TRANSPARENTBLT key and the two ClearType colours are converted to A8R8G8B8 once per command,
 * because those kernels work per channel and convert their pixels the same way.
 *
 * Returns 0 and writes nothing when the item is not a kernel item, when a slot is out of range or
 * absent, or when an address would wrap. A returned 1 means every field is set.
 */
int Bc250GdiKernarg(const BC250_GDI_OP* Op, const BC250_GDI_SURFACE* Surfaces,
                    const unsigned long long* Vas, unsigned int Count,
                    const BC250_GDI_ITEM* Item, BC250_GDI_KERNARG* Arg);

/* The dwords the code block needs at a given place in the DMA buffer: a NOP header, the padding
 * that puts the code at a BC250_GDI_CODE_ALIGN boundary, and the code. BufferVa is the GPU virtual
 * address of Buffer[0] of the emitter below, so the padding can be computed before anything is
 * written. 0 means the code cannot be placed there at all (an unaligned or wrapping address). */
unsigned int Bc250GdiCodeDwords(unsigned long long BufferVa);

/* The code block, once per DMA buffer. *CodeVa receives the GPU virtual address the kernels then
 * run from, which the dispatches below take as their Code argument. Returns the dwords written, or
 * 0 on invalid input or short capacity, having written nothing. */
unsigned int Bc250EmitGdiCode(unsigned int* Buffer, unsigned int CapacityDwords,
                              unsigned long long BufferVa, unsigned long long* CodeVa);

/* The dwords one dispatch needs at a given place: the kernarg NOP and its padding, the argument
 * block, the register writes and the dispatch. The only thing that varies with the address is the
 * padding, so the answer is one of two numbers. */
unsigned int Bc250GdiDispatchDwords(unsigned long long BufferVa);

/* The largest answer the two functions above can give, for any address: what one sub-rectangle and
 * one code block cost in the worst case. gdi_batch.c budgets a whole command buffer with these, so
 * that the capacity of the call is settled before the first packet is written. */
unsigned int Bc250GdiCodeDwordsMax(void);
unsigned int Bc250GdiDispatchDwordsMax(void);

/* One sub-rectangle: the whole dispatch of Item->Kernel over Arg. Code is the address
 * Bc250EmitGdiCode returned for this DMA buffer. Returns the dwords written, or 0 on invalid input
 * or short capacity, having written nothing.
 *
 * The grid is ceil(Width / BC250_GDI_GROUP_X) by ceil(Height / BC250_GDI_GROUP_Y) work groups, so
 * the last group of a row or column can overshoot the rectangle; the kernels drop those work items
 * themselves, which is the one bound they check.
 */
unsigned int Bc250EmitGdiDispatch(unsigned int Kernel, const BC250_GDI_KERNARG* Arg,
                                  unsigned long long Code, unsigned int* Buffer,
                                  unsigned int CapacityDwords, unsigned long long BufferVa);

/* What the image carries, for the capability gate and the log line: the number of kernels, their
 * total code size in bytes, and the SHA256 of the code as a string. Bc250GdiKernelsPresent of
 * gdi_render.h answers from these. */
unsigned int Bc250GdiCodeKernelCount(void);
unsigned int Bc250GdiCodeBytes(void);
const char* Bc250GdiCodeHash(void);

#endif
