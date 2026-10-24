// The statuses that DxgkDdiBuildPagingBuffer may give to the OS. The DDI page (d3dkmddi.h,
// DXGKDDI_BUILDPAGINGBUFFER, "Return value") permits STATUS_SUCCESS, STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER and
// STATUS_GRAPHICS_ALLOCATION_BUSY, and says that the function can fail only if it runs out of space. dxgmms2 stops
// the machine on every other status: bug check 0x10E, parameter 1 = 0xB (BD-235, a virtual fill that returned
// STATUS_INVALID_PARAMETER during an engine timeout). Header only, so that the host test and the kernel use the
// same text and the build lists need no new file.
#pragma once

#define PAGING_DDI_SUCCESS                 ((long)0x00000000L)
#define PAGING_DDI_INSUFFICIENT_DMA_BUFFER ((long)0xC01E0001L)  // STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER
#define PAGING_DDI_ALLOCATION_BUSY         ((long)0xC01E0102L)  // STATUS_GRAPHICS_ALLOCATION_BUSY

// The status for the OS, from the status that a builder returned. *Refused is 1 when the builder status is not one
// that the DDI permits. The caller counts and logs it. The OS gets success for the part of the operation that the
// builder published before it stopped: the published records are complete, and the rest of the operation is not
// in the buffer.
// Residual risk: a fill reported as success may leave part of the allocation not zeroed if VidMm submits that
// buffer.
static __inline long PagingDdiStatus(long Status, int* Refused)
{
    *Refused = 0;
    if (Status == PAGING_DDI_INSUFFICIENT_DMA_BUFFER || Status == PAGING_DDI_ALLOCATION_BUSY) return Status;
    if (Status >= 0) return PAGING_DDI_SUCCESS;     // an informational status is a success
    *Refused = 1;
    return PAGING_DDI_SUCCESS;
}
