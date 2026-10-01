#pragma once
#include "paging_window.h"

// Logical state of accepted aperture mapping commands, not a read of live GART.
// Caller owns storage and serializes publication/lookup with the paging builder.
// Publish only after the matching DMA/private record is accepted. Keep entries
// immutable with respect to an in-progress lookup; reset only after lifecycle drain.
#define PAGING_APERTURE_BATCH_PAGES 256u

typedef struct {
    PAGING_APERTURE aperture;
    unsigned long long* entries; // physical page | bit0 valid; page0 is representable
    unsigned count;
} PAGING_APERTURE_STATE;

int PagingApertureStateInit(PAGING_APERTURE_STATE* state, const PAGING_APERTURE* aperture,
                           unsigned long long* storage, unsigned storagePages);
// PhysicalPages is a disjoint immutable array of page-aligned physical addresses.
// No caller MDL or pointer to the input array is retained.
int PagingApertureStateMap(PAGING_APERTURE_STATE* state, unsigned first, unsigned count,
                          const unsigned long long* physicalPages, unsigned long long addressMask);
int PagingApertureStateUnmap(PAGING_APERTURE_STATE* state, unsigned first, unsigned count);
// Resolves one page slice. Distinct aperture slots alias iff physical slices overlap.
int PagingApertureStateResolve(const PAGING_APERTURE_STATE* state, unsigned long long mc,
                              unsigned bytes, unsigned long long* physical);
