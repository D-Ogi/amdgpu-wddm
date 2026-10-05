// Logical page-table state for construction of ordered paging commands.
// Caller owns storage and serialization. This never writes GPU-visible tables.
#pragma once
#define PAGING_PT_ENTRIES 512u
typedef unsigned long long PAGING_PT_U64;
typedef struct {
    PAGING_PT_U64 Physical;
    // One bit per byte; Read requires all eight bytes of the PTE.
    PAGING_PT_U64 Known[64];
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

// Copy logical metadata after GPU-copy publication. Inputs are resolved physical
// table identities, NOT the virtual addresses in DXGK_COPY_RANGE. Same-table
// overlap has snapshot/memmove semantics; GPU commands must match this ordering.
// No registration/allocation. Missing source data clears destination Known bits;
// callers must not interpret an unknown value as an invalid (zero) PTE.
int PagingPtShadowCopy(PAGING_PT_SHADOW* State, PAGING_PT_U64 Source, unsigned SourceFirst,
                      PAGING_PT_U64 Destination, unsigned DestinationFirst, unsigned Count);

// Apply a DWORD-pattern fill to registered tables intersecting a physical range.
// Each written byte becomes known independently; Read exposes only complete
// PTEs. Copy preserves partial knowledge as well as complete entries.
int PagingPtShadowFill(PAGING_PT_SHADOW* State, PAGING_PT_U64 Physical,
                       PAGING_PT_U64 Bytes, unsigned Pattern);

// Copy a slice contained in one source and one destination page. Byte-granular
// snapshot/memmove semantics; caller must match hardware overlap ordering.
// SourceTracked=0 means unknown external bytes; no CPU read or source registration.
int PagingPtShadowCopyBytes(PAGING_PT_SHADOW* State, PAGING_PT_U64 Source,
    PAGING_PT_U64 Destination, unsigned Bytes, int SourceTracked);

// Logical SAVE/RESTORE for graph cycles. Scratch is a separate caller-owned slot,
// never an element of State->Slots, never registered as a page table, and must
// not be allocated as a large kernel-stack local. Caller serializes the whole
// accepted command group under the shadow lock. SAVE resets its knowledge and
// stores [Source,Source+Bytes) at scratch offset0. RESTORE copies from offset0.
// Untracked/missing source bytes remain unknown. These helpers do not publish
// commands or establish GPU retirement/ownership; complete groups must match
// hardware scratch lifetime. No allocation, retained pointer or table registration.
int PagingPtShadowSaveBytes(PAGING_PT_SHADOW* State,PAGING_PT_U64 Source,
    unsigned Bytes,int SourceTracked,PAGING_PT_SHADOW_SLOT* Scratch);
int PagingPtShadowRestoreBytes(PAGING_PT_SHADOW* State,const PAGING_PT_SHADOW_SLOT* Scratch,
    PAGING_PT_U64 Destination,unsigned Bytes);
