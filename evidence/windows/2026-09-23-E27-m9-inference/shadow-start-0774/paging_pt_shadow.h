// Logical page-table state for construction of ordered paging commands.
// Caller owns storage and serialization. This never writes GPU-visible tables.
#pragma once
#define PAGING_PT_ENTRIES 512u
typedef unsigned long long PAGING_PT_U64;
typedef struct {
    PAGING_PT_U64 Physical;
    PAGING_PT_U64 Known[8];
    PAGING_PT_U64 Entries[PAGING_PT_ENTRIES];
    unsigned Occupied;
} PAGING_PT_SHADOW_SLOT;
typedef struct {
    PAGING_PT_SHADOW_SLOT* Slots;
    unsigned Capacity;
    unsigned Used;
} PAGING_PT_SHADOW;
enum { PAGING_PT_OK=0, PAGING_PT_MISSING=1, PAGING_PT_FULL=2, PAGING_PT_INVALID=3 };
// Initialization/reset needs exclusive ownership; no pointer may survive reset.
int PagingPtShadowInit(PAGING_PT_SHADOW* State, PAGING_PT_SHADOW_SLOT* Slots, unsigned Capacity);
// Entries must already be completely validated/encoded and not alias shadow storage.
// Register=1 only for tables whose full lifetime is owned by the paging process.
// Missing updates never silently register a table when Register=0.
int PagingPtShadowApply(PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                       unsigned First, unsigned Count, const PAGING_PT_U64* Entries, int Register);
// Missing/uninitialized entries are different from an explicitly initialized zero PTE.
int PagingPtShadowRead(const PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                      unsigned Index, PAGING_PT_U64* Entry);

// Number of table pages needed for a fixed VA hierarchy (4KiB pages,9bits/level).
unsigned PagingPtShadowTableCount(PAGING_PT_U64 VirtualBytes, unsigned Levels);
// Non-mutating preflight under the same caller lock as the subsequent Apply.
int PagingPtShadowCanApply(const PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                          unsigned First, unsigned Count, int Register);
