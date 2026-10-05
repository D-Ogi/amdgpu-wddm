// Per-DMA-buffer records stored in dxgkrnl-owned nonpaged private data.
#pragma once
#define PAGING_PRIVATE_HEADER_BYTES 24u
#define PAGING_PRIVATE_BUFFER_BYTES (2u * 65536u)
typedef unsigned long long PAGING_ADDRESS;
typedef void (*PAGING_PRIVATE_VISITOR)(void*, const unsigned*, unsigned);
int PagingPrivateHeader(unsigned* record, unsigned capacity, unsigned offset,
                        PAGING_ADDRESS base, unsigned bytes);
// Two passes: validate the ENTIRE requested range, then visit packet spans.
// No global address lookup, range clamping, or callbacks on malformed data.
int PagingPrivateVisit(const void* data, unsigned capacity, PAGING_ADDRESS start,
                       unsigned bytes, int virtualAddress, PAGING_PRIVATE_VISITOR visit, void* context);

// Native records describe retained OS DMA storage, not an inline word copy.
// The system paging root is pinned by VidMm until its power/lifecycle transition.
// CSA and IB must be disjoint subranges of the selected whole DMA record.
#define PAGING_PRIVATE_NATIVE_BYTES 64u
#define PAGING_PRIVATE_DIRECT 0u
#define PAGING_PRIVATE_NATIVE 1u
#define PAGING_PRIVATE_CSA_BYTES 64u

typedef struct PAGING_PRIVATE_SPAN {
    unsigned Kind;
    const unsigned* Words;       // direct only; valid during the visitor call
    unsigned Dwords;             // direct words or native IB length
    PAGING_ADDRESS RootPhysical, IbAddress, CsaAddress;
} PAGING_PRIVATE_SPAN;
typedef int (*PAGING_PRIVATE_MIXED_VISITOR)(void*, const PAGING_PRIVATE_SPAN*);
int PagingPrivateNativeHeader(unsigned* record, unsigned capacity, unsigned offset,
    PAGING_ADDRESS base, unsigned bytes, PAGING_ADDRESS rootPhysical,
    unsigned ibOffset, unsigned ibDwords, unsigned csaOffset);
// Validate the entire selected range before any callback. Native spans require
// virtual delivery and whole-record boundaries; physical spans retain slicing.
// Callback refusal propagates without claiming execution or completion.
int PagingPrivateVisitMixed(const void* data, unsigned capacity, PAGING_ADDRESS start,
    unsigned bytes, int virtualAddress, PAGING_PRIVATE_MIXED_VISITOR visit, void* context);

// Byte ownership returned by the native builder; no allocated capture state.
typedef struct PAGING_NATIVE_RESULT {
    unsigned Bytes, IbOffset, IbDwords, CsaOffset, NextToken;
    PAGING_ADDRESS Moved;
} PAGING_NATIVE_RESULT;
