/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* The walk of one GDI command buffer, in the order that makes every read bounded.
 *
 * This is the seam between the operating system's command buffer and the plain records of
 * gdi_render.c. Both passes of DxgkDdiRenderGdi, the one that counts sub-rectangles and the one
 * that translates them, call Bc250GdiWalkCommand and nothing else, so the two cannot disagree.
 *
 * The order of the four steps is the contract, and it is the reason this code is not inside
 * wddm.c any more. A command buffer may end in the middle of a command, so a field may not be
 * read before the bytes that hold it are known to be inside the buffer:
 *
 *   1. the caller's loop admits the command only while the eight-byte header fits;
 *   2. Bc250GdiOpMinBytes gives the fixed size of the opcode, from OpCode alone;
 *   3. Bc250GdiCommandSize checks that size against CommandSize and the rest of the buffer;
 *   4. only then does Bc250GdiDecodeCommand read the opcode's own member.
 *
 * Review r1 of 2026-10-14 found step 4 happening before step 3, which read up to 40 bytes past
 * CommandLength for a truncated final command. run_gdi_walk.ps1 is the test that holds the order:
 * it puts an inaccessible page immediately after the command buffer.
 */
#ifndef BC250_GDI_WALK_H
#define BC250_GDI_WALK_H

/* The includer of this header has the WDK declarations of the command buffer:
 * DXGK_RENDERKM_COMMAND, its opcode table and RECT. Nothing here restates them. gdi_walk.c gets
 * them from gdi_walk_ddi.h, and wddm.c from the display interface it already includes. */
#include "gdi_render.h"

/* BC250_GDI_CMD_STOP, _SKIP and _DRAW of gdi_render.h, plus the one answer this layer adds. An
 * opcode the table does not describe stops the walk, like a malformed size, because a command of
 * unknown length leaves no way to reach the next one. */
#define BC250_GDI_WHY_OPCODE 5u

/* The smallest CommandSize an opcode may have: the fixed part of DXGK_RENDERKM_COMMAND plus the
 * union member this opcode names. 0 for an opcode outside the table. It reads no memory, so it is
 * safe before the body of the command is known to be inside the buffer. */
unsigned int Bc250GdiOpMinBytes(unsigned int OpCode);

/* One RECT of the OS, in the plain record of gdi_render.h. The translation pass copies each
 * sub-rectangle with it, after Bc250GdiWalkCommand has said the array lies inside the command. */
void Bc250GdiWalkRect(const RECT* From, BC250_GDI_RECT* To);

/* One command's structure: the opcode table, the size step and the containment of the
 * sub-rectangle array inside the command. Buffer is the command buffer, Bytes its CommandLength
 * and Offset the start of this command, which the caller has already admitted as holding at least
 * the header. Returns BC250_GDI_CMD_STOP, _SKIP or _DRAW, with Why in BC250_GDI_WHY_*, and Next
 * set to the offset of the command behind this one whenever the walk may continue. */
int Bc250GdiWalkCommand(const unsigned char* Buffer, unsigned int Bytes, unsigned int Offset,
                        BC250_GDI_OP* Op, const RECT** SubRects, unsigned int* SubCount,
                        unsigned int* Next, unsigned int* Why);

#endif /* BC250_GDI_WALK_H */
