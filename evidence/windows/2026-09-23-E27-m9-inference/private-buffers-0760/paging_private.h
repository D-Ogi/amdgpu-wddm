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
