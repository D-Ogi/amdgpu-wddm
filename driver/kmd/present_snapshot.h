#pragma once

// BD-065: why WddmSnapshotPresentAllocations (wddm_allocation_identity.inc) refused the two allocations of a GPU
// Present, first failing check wins. The order is the order of its checks. wddm.c counts each reason; slot 0 counts
// all refusals and bounds the detailed log, Count sizes the counters.
typedef enum _BC250_SNAPSHOT_REFUSAL {
    Bc250SnapshotAdmitted=0,
    Bc250SnapshotStopping,      // the adapter is stopping
    Bc250SnapshotNotOpened,     // the handle is no opened object of ours
    Bc250SnapshotOtherOwner,    // opened by another device than the context's
    Bc250SnapshotUmdOpened,     // a BC2A open: GPU Present takes LB7A only
    Bc250SnapshotUnbound,       // the open found no backing allocation
    Bc250SnapshotBackingGone,   // the backing allocation was destroyed since
    Bc250SnapshotUmdBacking,    // the backing allocation is BC2A
    Bc250SnapshotDescriptor,    // opened and backing descriptors differ
    Bc250SnapshotSameBacking,   // both sides import one allocation
    Bc250SnapshotFormat,        // the formats differ: CP DMA moves bytes and cannot convert
    Bc250SnapshotRefusalCount
} BC250_SNAPSHOT_REFUSAL;

// Which check of WddmBuildGpuPresent (wddm.c) returned STATUS_INVALID_PARAMETER, one value for each return.
// wddm.c counts each step; slot 0 counts all of them and bounds the detailed log, Count sizes the counters. The
// snapshot refusals (STATUS_INVALID_HANDLE) and a format the copy cannot take (CANNOTCOLORCONVERT) are not here.
typedef enum _BC250_GPU_PRESENT_STEP {
    Bc250GpuPresentRefusals=0,
    Bc250GpuPresentUmdContext,      // a context of a user mode driver: Present builds only for a system context
    Bc250GpuPresentFlags,           // the flags are not Blt alone
    Bc250GpuPresentNode,            // the context is not on the 3D node
    Bc250GpuPresentAllocationList,  // no allocation information, or a list too short for source and destination
    Bc250GpuPresentSubRectArray,    // sub-rectangles counted but not given
    Bc250GpuPresentDmaBuffer,       // the DMA buffer or its private data is absent or misaligned
    Bc250GpuPresentDmaAlignment,    // the DMA buffer size is not a multiple of 32 bytes
    Bc250GpuPresentRecord,          // the Present record cannot describe the DMA buffer
    Bc250GpuPresentPhysicalAdapter, // an allocation on another physical adapter
    Bc250GpuPresentVirtualAddress,  // a GPU virtual address of 0, not page aligned or past the end of the space
    Bc250GpuPresentIbOverlap,       // an allocation overlaps the DMA buffer
    Bc250GpuPresentSubRectCount,    // too many sub-rectangles
    Bc250GpuPresentEmitter,         // the blit emitter refused the rectangles or the surfaces
    Bc250GpuPresentWritten,         // the emitter did not fill the whole DMA buffer
    Bc250GpuPresentStepCount
} BC250_GPU_PRESENT_STEP;
