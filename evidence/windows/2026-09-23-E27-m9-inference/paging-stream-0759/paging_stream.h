// Portable page-wise paging builder. Callback emits existing AMD packets; no opcodes here.
#pragma once
typedef unsigned long long PAGING_U64;
typedef int (*PAGING_RESOLVE)(void*, PAGING_U64, unsigned, PAGING_U64*);
// Emit returns 0 on success, 1 for insufficient room, 2 on invalid input.
typedef int (*PAGING_EMIT)(void*, unsigned*, unsigned, PAGING_U64, PAGING_U64, unsigned, unsigned*);
enum { PagingStreamDone=0, PagingStreamMore=1, PagingStreamAddress=2, PagingStreamInvalid=3 };
int PagingStreamBuild(void* context, PAGING_RESOLVE resolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, unsigned total, unsigned start,
                      unsigned* buffer, unsigned capacity, unsigned* written, unsigned* next);
// Limit the entire accumulated DMA buffer to one live-ring reservation, including
// the submission fence and final alignment padding. Byte offset is independent of remaining bytes.
unsigned PagingStreamCapacity(unsigned remaining, unsigned offset, unsigned shadow,
                              unsigned ringMax, unsigned alignMask, unsigned fence);
