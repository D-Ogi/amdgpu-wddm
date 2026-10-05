#pragma once
#include "paging_graph_batch.h"
// CPU metadata only. Caller stores captured identities in the same allocation
// after this header; no OS MDL, VA pointer or GPU command references this storage.
// Serialize all operations with the owning adapter's PagingBuildLock.
typedef struct PAGING_CAPTURE {
    struct PAGING_CAPTURE* Next;
    unsigned Token,PoolTag;
    unsigned Band,Action;
    unsigned long long Root,Source,Destination,Bytes,AllocationOffset;
    unsigned long long ReservationBytes; // Aligned arena span; zero for heap metadata.
    const void* Allocation;
} PAGING_CAPTURE;
typedef struct {
    PAGING_CAPTURE* Head;
    unsigned LastId;
    unsigned ActivePlans, PeakPlans; // Unfinished construction, not pending GPU fences.
    unsigned long long ReservedBytes, PeakReservedBytes; // Attached arena spans only.
    unsigned ReservedCaptures,HeapCaptures; // Bounded per-context diagnostic witnesses.
    void* Storage;                    // Optional context-owned nonpaged reservation.
    unsigned long long StorageBytes;
} PAGING_CAPTURE_OWNER;
// Find an aligned span not occupied by an attached reserved capture. Caller
// holds PagingBuildLock until initialization/attach; failure takes no ownership.
void* PagingCaptureStorage(const PAGING_CAPTURE_OWNER* Owner,unsigned long long Bytes);
// Whole-arena availability, retained for lifecycle checks.
void* PagingCaptureIdleStorage(const PAGING_CAPTURE_OWNER* Owner);
#define PAGING_CAPTURE_TAG 0x80000000u
#define PAGING_CAPTURE_COMPLETE 0x7fffffffu
// Tokens never repeat during one owner's lifetime. Attach fails before mutation
// on exhaustion; caller retains the allocation. Zero-initialize a fresh owner.
int PagingCaptureAttach(PAGING_CAPTURE_OWNER* Owner,PAGING_CAPTURE* Capture);
PAGING_CAPTURE* PagingCaptureFind(const PAGING_CAPTURE_OWNER* Owner,unsigned Token);
// Detach transfers sole metadata ownership back to the caller for freeing.
PAGING_CAPTURE* PagingCaptureDetach(PAGING_CAPTURE_OWNER* Owner,unsigned Token);
PAGING_CAPTURE* PagingCaptureTakeAll(PAGING_CAPTURE_OWNER* Owner);

// All pointer fields refer inside this one allocation. Identities, flags and
// source/destination indexes stay immutable; only planning workspace is reused.
typedef struct {
    PAGING_CAPTURE Owner;
    unsigned PageCount,Identities,BandCount,RequiredDwords,MaxAtomicMoves;
    unsigned Linear,Backward,SourcePageCount,DestinationPageCount;
    unsigned long long Progress;
    unsigned PlannedBand,MoveCount; // Cached immutable plan for the current byte band.
    unsigned long long ScratchMc,VramMcBase,VramPhysical;
    PAGING_PAGE_BAND Bands[3];
    unsigned long long* Pages;
    unsigned char* SystemPages;
    unsigned *SourceIndex,*DestinationIndex,*Readers,*Writer,*Queue,*Forward;
    PAGING_PAGE_MOVE* Moves;
} PAGING_GRAPH_CAPTURE;
