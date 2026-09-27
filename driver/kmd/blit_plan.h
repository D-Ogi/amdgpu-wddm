/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#pragma once

/* Geometry only. The caller must prove linear layout, format mapping, residency,
 * allocation lifetime and non-overlap of the actual backing stores. These offsets
 * are relative to allocations; they are neither GPU VAs nor CPU physical addresses.
 * No caller is wired to the engine until its submission/fence path is validated. */
typedef enum BC250_BLIT_FORMAT {
    Bc250BltBgra8 = 1,
    Bc250BltRgba8 = 2
} BC250_BLIT_FORMAT;

typedef struct BC250_BLIT_SURFACE {
    unsigned int Width, Height, Pitch;
    unsigned long long Bytes;
    BC250_BLIT_FORMAT Format;
} BC250_BLIT_SURFACE;

typedef struct BC250_BLIT_RECT {
    int Left, Top, Right, Bottom;
} BC250_BLIT_RECT;

typedef struct BC250_BLIT_PLAN {
    unsigned long long SourceOffset, DestinationOffset;
    unsigned int SourcePitch, DestinationPitch, RowBytes, Rows;
} BC250_BLIT_PLAN;

typedef enum BC250_BLIT_RESULT {
    Bc250BltInvalid = 0,
    Bc250BltEmpty = 1,
    Bc250BltCopy = 2
} BC250_BLIT_RESULT;

/* Dirty is in destination coordinates; NULL means the whole destination rect.
 * Intersect it with DestinationRect, retaining the original source translation.
 * Invalid geometry, scaling or conversion rejects the plan without partial work.
 * Plan is cleared for both Invalid and Empty. All source/destination rectangles
 * must be within their allocations, including Dirty before intersection. */
BC250_BLIT_RESULT Bc250PlanBlt(const BC250_BLIT_SURFACE* Source,
    const BC250_BLIT_SURFACE* Destination, const BC250_BLIT_RECT* SourceRect,
    const BC250_BLIT_RECT* DestinationRect, const BC250_BLIT_RECT* Dirty,
    BC250_BLIT_PLAN* Plan);
