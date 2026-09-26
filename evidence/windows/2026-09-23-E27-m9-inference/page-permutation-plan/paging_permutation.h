#pragma once
/* A permutation of distinct physical page identities, normalized by the caller.
 * Destination[i] is the destination identity for original source page i.
 * This is not a classifier for unaligned ranges or duplicate physical pages.
 * Plans use one scratch page; execution must retain it across all DMA buffers
 * until the final fence retires. Planning never accesses page contents. */
#define PAGING_PERMUTATION_SCRATCH 0xffffffffu
typedef struct { unsigned source, destination; } PAGING_PAGE_MOVE;
/* Caller supplies Count inverse entries and Count visited bytes. Moves needs
 * Count + Count/2 entries (worst case: disjoint two-cycles). No retained pointers.
 * Identity edges emit nothing. Returns1 on success,0 without a published plan. */
int PagingPermutationPlan(const unsigned* Destination, unsigned Count,
    unsigned* Inverse, unsigned char* Visited, PAGING_PAGE_MOVE* Moves,
    unsigned Capacity, unsigned* Written);
