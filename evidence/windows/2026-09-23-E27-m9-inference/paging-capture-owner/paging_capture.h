#pragma once
// CPU metadata only. Caller stores captured identities in the same allocation
// after this header; no OS MDL, VA pointer or GPU command references this storage.
// Serialize all operations with the owning adapter's PagingBuildLock.
typedef struct PAGING_CAPTURE {
    struct PAGING_CAPTURE* Next;
    unsigned Token;
    unsigned Band,Action;
    unsigned long long Root,Source,Destination,Bytes,AllocationOffset;
    const void* Allocation;
} PAGING_CAPTURE;
typedef struct {
    PAGING_CAPTURE* Head;
    unsigned LastId;
} PAGING_CAPTURE_OWNER;
#define PAGING_CAPTURE_TAG 0x80000000u
// Tokens never repeat during one owner's lifetime. Attach fails before mutation
// on exhaustion; caller retains the allocation. Zero-initialize a fresh owner.
int PagingCaptureAttach(PAGING_CAPTURE_OWNER* Owner,PAGING_CAPTURE* Capture);
PAGING_CAPTURE* PagingCaptureFind(const PAGING_CAPTURE_OWNER* Owner,unsigned Token);
// Detach transfers sole metadata ownership back to the caller for freeing.
PAGING_CAPTURE* PagingCaptureDetach(PAGING_CAPTURE_OWNER* Owner,unsigned Token);
PAGING_CAPTURE* PagingCaptureTakeAll(PAGING_CAPTURE_OWNER* Owner);
