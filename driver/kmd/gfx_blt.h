/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once
#include "blit_plan.h"

typedef struct BC250_BLIT_CURSOR { unsigned int Row, ByteInRow; } BC250_BLIT_CURSOR;
typedef enum BC250_GFX_BLIT_RESULT {
    Bc250GfxBltInvalid = 0, Bc250GfxBltDone = 1,
    Bc250GfxBltMore = 2, Bc250GfxBltNoSpace = 3
} BC250_GFX_BLIT_RESULT;

/* Build one bounded DMA_DATA batch. Cursor can resume within a very wide row.
 * Validate the whole plan/address footprint before any output; reject overlapping
 * bounding spans. Caller separately proves backing-store non-aliasing, residency
 * and visibility, and keeps all input/output storage distinct except Cursor/Next
 * which may alias. Each nonempty batch has one first RAW_WAIT and last CP_SYNC.
 * Written=0 and Next=Cursor on invalid/no-space; Done may also write0 when empty.
 * This cursor is internal. DDI MultipassOffset still needs its own adapter. */
BC250_GFX_BLIT_RESULT Bc250EmitGfxBlt(const BC250_BLIT_PLAN* Plan,
    unsigned long long SourceBase, unsigned long long DestinationBase,
    const BC250_BLIT_CURSOR* Cursor, BC250_BLIT_CURSOR* Next,
    unsigned int* Buffer, unsigned int CapacityDwords, unsigned int* Written);
