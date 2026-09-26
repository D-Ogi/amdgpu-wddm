#pragma once
/* A permutation of distinct physical page identities, normalized by the caller.
 * Destination[i] is the destination identity for original source page i.
 * This is not a classifier for unaligned ranges or duplicate physical pages.
 * Plans use one scratch page, retained from SAVE through matching RESTORE.
 * Complete-cycle batch boundaries allow scratch reuse after retirement. Split
 * cycles would require per-transfer scratch ownership. No page contents read. */
#define PAGING_PERMUTATION_SCRATCH 0xffffffffu
typedef struct { unsigned source, destination; } PAGING_PAGE_MOVE;
/* Caller supplies Count inverse entries and Count visited bytes. Moves needs
 * Count + Count/2 entries (worst case: disjoint two-cycles). No retained pointers.
 * Identity edges emit nothing. Returns1 on success,0 without a published plan. */
int PagingPermutationPlan(const unsigned* Destination, unsigned Count,
    unsigned* Inverse, unsigned char* Visited, PAGING_PAGE_MOVE* Moves,
    unsigned Capacity, unsigned* Written);

/* Pack complete cycles only. MaxMoves must be derived from a proven worst-case
 * packet cost per move and remaining DMA/private capacity. No split SAVE/RESTORE
 * pair is published. A separate queued job may overwrite scratch between batches.
 * NeedCycle is not an instruction to retry an unchanged buffer indefinitely:
 * the caller must supply enough capacity or choose retained per-transfer scratch. */
enum { PagingPermutationDone=0, PagingPermutationMore=1,
       PagingPermutationNeedCycle=2, PagingPermutationInvalid=3 };
int PagingPermutationBatch(const PAGING_PAGE_MOVE* Moves,unsigned Count,
    unsigned Start,unsigned MaxMoves,unsigned* Next,unsigned* Required);

/* Normalize complete, page-aligned physical identities. Source and destination
 * arrays describe corresponding logical slices. Work/Seen/Permutation each have
 * Count entries and are scratch on failure; no MDL or allocation is retained.
 * Returns1 only for a bijection of the same distinct physical pages. */
typedef struct { unsigned long long address; unsigned index; } PAGING_PAGE_IDENTITY;
int PagingPermutationNormalize(const unsigned long long* SourcePages,
    const unsigned long long* DestinationPages,unsigned Count,
    PAGING_PAGE_IDENTITY* Work,unsigned char* Seen,unsigned* Permutation);

/* Bound atomic groups without splitting scratch lifetime. Cycles longer than
 * MaxAtomicMoves use pivot transpositions (three moves per swap). Other cycles
 * keep the minimal SAVE/inverse-copy/RESTORE plan. Capacity must hold 3*Count.
 * The emitted groups can use PagingPermutationBatch unchanged. */
int PagingPermutationPlanBounded(const unsigned* Destination,unsigned Count,
    unsigned* Inverse,unsigned char* Visited,PAGING_PAGE_MOVE* Moves,
    unsigned Capacity,unsigned MaxAtomicMoves,unsigned* Written);

#define PAGING_PERMUTATION_RESUME 0x80000000u
