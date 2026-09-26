// Portable page-wise paging builder. Callback emits existing AMD packets; no opcodes here.
#pragma once
typedef unsigned long long PAGING_U64;
typedef int (*PAGING_RESOLVE)(void*, PAGING_U64, unsigned, PAGING_U64*);
// Emit returns 0 on success, 1 for insufficient room, 2 on invalid input.
typedef int (*PAGING_EMIT)(void*, unsigned*, unsigned, PAGING_U64, PAGING_U64, unsigned, unsigned*);
enum { PagingStreamDone=0, PagingStreamMore=1, PagingStreamAddress=2, PagingStreamInvalid=3 };
// Independent endpoint resolvers allow the same offset to name different MDLs.
// Source resolver may be NULL for fill, since fill never reads the source.
int PagingStreamBuildEndpoints64(void* context, PAGING_RESOLVE sourceResolve,
                      PAGING_RESOLVE destinationResolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, PAGING_U64 total, PAGING_U64 start,
                      unsigned* buffer, unsigned capacity, unsigned* written, PAGING_U64* next);
// Byte progress is independent of the bounded DWORD packet buffer. Callers must
// explicitly encode it when their external resume token has a narrower type.
int PagingStreamBuild64(void* context, PAGING_RESOLVE resolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, PAGING_U64 total, PAGING_U64 start,
                      unsigned* buffer, unsigned capacity, unsigned* written, PAGING_U64* next);
int PagingStreamBuild(void* context, PAGING_RESOLVE resolve, PAGING_EMIT emit,
                      int fill, PAGING_U64 src, PAGING_U64 dst, unsigned total, unsigned start,
                      unsigned* buffer, unsigned capacity, unsigned* written, unsigned* next);
// Limit the entire accumulated DMA buffer to one live-ring reservation, including
// the submission fence and final alignment padding. Byte offset is independent of remaining bytes.
unsigned PagingStreamCapacity(unsigned remaining, unsigned offset, unsigned shadow,
                              unsigned ringMax, unsigned alignMask, unsigned fence);

// Resolve a byte slice within one page of an MDL PFN array or ADL page list.
// Output is a device-programmable address, not permission to map it on the CPU.
// FirstPage is MdlOffset/AdlOffset; transfer progress is a separate byte offset.
// Contiguous selects BasePage; otherwise Pages has exactly PageCount elements.
int PagingPageListAddress(const PAGING_U64* Pages, unsigned PageCount,
                          PAGING_U64 BasePage, int Contiguous, unsigned FirstPage,
                          PAGING_U64 ByteOffset, unsigned Bytes, PAGING_U64* Address);

// Stateless UINT token = number of completed page slices (not bytes/pages).
// Source and destination may have different page offsets. The final token also
// represents a partial last slice. Refuse ranges needing more than UINT32_MAX
// slices before any work is emitted. Outputs are cleared on refusal.
int PagingStreamTokenDecode(int Fill, PAGING_U64 Source, PAGING_U64 Destination,
    PAGING_U64 Total, unsigned Token, PAGING_U64* Progress);
int PagingStreamTokenEncode(int Fill, PAGING_U64 Source, PAGING_U64 Destination,
    PAGING_U64 Total, PAGING_U64 Progress, unsigned* Token);
