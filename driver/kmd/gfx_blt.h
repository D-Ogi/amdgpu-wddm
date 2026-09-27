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

/* Stateless Present-list adapter. Offset/Next are packet ordinals, suitable for
 * a UINT MultipassOffset. Count0 selects the complete destination rectangle.
 * Every rectangle and address footprint is validated before any packet write,
 * including rectangles preceding Offset. Total packet counts exceeding UINT are
 * rejected. Allocation VA spans must be disjoint; caller still proves backing
 * non-aliasing, lifetime, residency and producer ordering. Inputs must be immutable
 * across passes and distinct from Buffer/Written/Next. No IB padding is emitted.
 * Intermediate rectangle boundaries retain conservative CP_SYNC/RAW_WAIT pairs.
 * This is command construction, not the WDDM allocation-list/submit integration. */
BC250_GFX_BLIT_RESULT Bc250EmitGfxBltList(
    const BC250_BLIT_SURFACE* Source, const BC250_BLIT_SURFACE* Destination,
    const BC250_BLIT_RECT* SourceRect, const BC250_BLIT_RECT* DestinationRect,
    const BC250_BLIT_RECT* Dirty, unsigned int Count,
    unsigned long long SourceBase, unsigned long long DestinationBase,
    unsigned int Offset, unsigned int* Next, unsigned int* Buffer,
    unsigned int CapacityDwords, unsigned int* Written);

/* OS-owned private record for a driver-generated Present IB. This binds the
 * entire padded command buffer; it never describes a user BC2S submission. */
#define BC250_GFX_PRESENT_MAGIC 0x31504742u /* BGP1 */
#define BC250_GFX_PRESENT_RECORD_BYTES 24u
int Bc250GfxPresentRecord(void* Record, unsigned int Capacity,
    unsigned long long Address, unsigned int Bytes);
int Bc250GfxPresentMatches(const void* Record, unsigned int Capacity,
    unsigned long long Address, unsigned int Bytes);

/* Non-UMD Present submission: dxgkrnl reports the producer's consumed private
 * span in DmaBufferUmdPrivateDataSize (M656). Require exactly one BGP1 record,
 * independently of the larger OS-owned buffer capacity. */
int Bc250GfxPresentSubmitMatches(const void* Record, unsigned int Capacity,
    unsigned int UsedPrivateBytes, unsigned long long Address, unsigned int Bytes);

/* Complete Present IB: acquire before every pass, copies, full NOP padding.
 * Capacity must be a multiple of eight DWORDs and at least16. Offset counts
 * copy packets only. Invalid/no-space never writes Buffer. Residency and
 * cross-queue producer completion remain caller obligations. */
BC250_GFX_BLIT_RESULT Bc250EmitGfxPresentBltList(
    const BC250_BLIT_SURFACE* Source, const BC250_BLIT_SURFACE* Destination,
    const BC250_BLIT_RECT* SourceRect, const BC250_BLIT_RECT* DestinationRect,
    const BC250_BLIT_RECT* Dirty, unsigned int Count,
    unsigned long long SourceBase, unsigned long long DestinationBase,
    unsigned int Offset, unsigned int* Next, unsigned int* Buffer,
    unsigned int CapacityDwords, unsigned int* Written);

/* Called by the Present producer on every new call, never by submit/re-submit.
 * Clear recognition before any early return or alternate command path. */
void Bc250GfxPresentInvalidate(void* Record, unsigned int Capacity);
