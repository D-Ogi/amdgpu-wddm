// Node 1's FIFO drain (wddm.c WddmGpuFencePaging): the per-invocation retirement quota.
#pragma once
// A single caller retires at most one hardware packet per invocation: after a retirement the next pass submits
// the successor and returns. More than one needs another caller publishing and the GPU finishing
// between two passes, which has no bound of its own; 8 leaves that interleaving room to make progress while
// keeping one invocation to a handful of lock holds. Hitting it is counted, so a lab run says whether it happens.
#define PAGING_DRAIN_QUOTA 8u

// Why the most recent invocation returned, for the progress record (hang.c). Zero is "never returned".
typedef enum PAGING_DRAIN_EXIT {
    PagingDrainExitNone = 0,
    PagingDrainExitStopping,            // the stop has begun
    PagingDrainExitIdle,                // no completion observed: GPU busy, queue empty, or the head just submitted
    PagingDrainExitRefused,             // the head was refused by the ring and the node closed
    PagingDrainExitQuota,               // PAGING_DRAIN_QUOTA retirements; the drain was requeued
} PAGING_DRAIN_EXIT;

typedef enum PAGING_DRAIN_NEXT {
    PagingDrainReturn = 0,              // nothing retired in this pass: whoever retires next brings the next pass
    PagingDrainContinue,                // retired one, below the quota: look again
    PagingDrainYield,                   // retired the quota: requeue and return
} PAGING_DRAIN_NEXT;

// After one pass: Completed says whether it retired a job; Retired counts this invocation's retirements.
static __inline PAGING_DRAIN_NEXT PagingDrainNext(unsigned* Retired, int Completed, unsigned Quota)
{
    if (!Completed) return PagingDrainReturn;
    return ++*Retired >= Quota ? PagingDrainYield : PagingDrainContinue;
}
