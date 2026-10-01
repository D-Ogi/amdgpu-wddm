#pragma once
#include "paging_stream.h"
typedef struct {PAGING_U64 begin,end;} PAGING_INTERVAL;
unsigned PagingIntervalCapacity(PAGING_U64 address,PAGING_U64 bytes);
// Resolve callbacks return system physical addresses, not MC or tagged addresses.
// Caller-owned source workspace is sorted/merged in place. No retained pointers.
int PagingIntervalsDisjoint(void* context,PAGING_RESOLVE source,PAGING_RESOLVE destination,
    PAGING_U64 src,PAGING_U64 dst,PAGING_U64 bytes,PAGING_INTERVAL* work,unsigned capacity,int* disjoint);
