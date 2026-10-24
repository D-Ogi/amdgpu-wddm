// The statuses that DxgkDdiBuildPagingBuffer may give to the OS. The DDI page (d3dkmddi.h,
// DXGKDDI_BUILDPAGINGBUFFER, "Return value") permits STATUS_SUCCESS, STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER and
// STATUS_GRAPHICS_ALLOCATION_BUSY, and says that the function can fail only if it runs out of space. Its own example
// loop treats every other status as a critical failure and stops the system: dxgmms2 does that with bug check 0x10E,
// parameter 1 = 0xB (BD-235, a virtual fill that returned STATUS_INVALID_PARAMETER during an engine timeout). Header
// only, so that the host test and the kernel use the same text and the build lists need no new file.
#pragma once

#define PAGING_DDI_SUCCESS                 ((long)0x00000000L)
#define PAGING_DDI_INSUFFICIENT_DMA_BUFFER ((long)0xC01E0001L)  // STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER
#define PAGING_DDI_ALLOCATION_BUSY         ((long)0xC01E0102L)  // STATUS_GRAPHICS_ALLOCATION_BUSY
#define PAGING_DDI_NOT_BUILT               ((long)0xC0000001L)  // STATUS_UNSUCCESSFUL

// The status for the OS, from the status that a builder returned. No status says "built" unless the builder said
// STATUS_SUCCESS: SUCCESS tells VidMm that the whole requested operation is in the buffer, and a partial fill or
// transfer reported that way would leave memory that VidMm takes as initialised.
//   - The three permitted statuses pass unchanged; *Outside is 0.
//   - A failure outside the DDI passes unchanged; *Outside is 1. The OS then stops the system, as the DDI says. That
//     is the outcome before BD-235. The DDI has no status for "this operation cannot be built", and the two permitted
//     failures have their own conditions (no space left; the allocation is in use), so neither is used for it.
//   - A nonnegative status other than STATUS_SUCCESS (for example STATUS_PENDING) says nothing about a completed
//     operation; the callback has no asynchronous completion. It becomes STATUS_UNSUCCESSFUL, the same outcome as
//     any other failure outside the DDI; *Outside is 1. No builder returns one today.
// The caller counts and logs every *Outside status.
static __inline long PagingDdiStatus(long Status, int* Outside)
{
    *Outside = 0;
    if (Status == PAGING_DDI_SUCCESS || Status == PAGING_DDI_INSUFFICIENT_DMA_BUFFER ||
        Status == PAGING_DDI_ALLOCATION_BUSY) return Status;
    *Outside = 1;
    if (Status >= 0) return PAGING_DDI_NOT_BUILT;
    return Status;
}
