#pragma once
#include "paging_permutation.h"
// Callback-scoped description of exactly one emitted equal-offset graph batch.
// Storage is immutable through publication; no pointer is copied into OS DMA.
typedef struct {
    const unsigned long long* Pages;
    const unsigned char* SystemPages;
    unsigned Identities;
    const PAGING_PAGE_MOVE* Moves;
    unsigned Count,Offset,Bytes;
} PAGING_GRAPH_BATCH;
