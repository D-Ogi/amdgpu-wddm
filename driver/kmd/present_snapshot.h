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
