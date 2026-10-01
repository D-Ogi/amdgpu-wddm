/* M15.12 host control: the pure decisions behind DxgkDdiResetEngine's stage-1 soft recovery (hang_recovery.h).
 * The wave kill (SQ_CMD) and the bounded fence poll touch hardware and are exercised on the lab behind the
 * HangRecoveryMode switch (DESIGN.md, LAB-PLAN.md); this test fixes the gate and the 0x119 fence-range guard,
 * including 32-bit fence wrap, so a refactor cannot quietly change when we attempt a recovery or what we report. */
#include <stdio.h>
#include "hang_recovery.h"
#include "bc250_fence_order.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void)
{
    /* The gate: attempt only with the switch on AND only for node 0 (3D/compute). Node 1 (paging) and any
     * higher ordinal are never soft-recovered; the switch off is today's behaviour for every node. */
    CHECK(!Bc250SoftRecoveryWanted(0, 0));      /* switch off: never, even on node 0 */
    CHECK(!Bc250SoftRecoveryWanted(0, 1));
    CHECK(Bc250SoftRecoveryWanted(1, 0));       /* switch on, node 0: the one case we act on */
    CHECK(!Bc250SoftRecoveryWanted(1, 1));      /* switch on, node 1 (paging): refuse */
    CHECK(!Bc250SoftRecoveryWanted(1, 2));      /* switch on, any other node: refuse */

    /* The 0x119 guard: LastAbortedFenceId must be in [lastCompleted, lastSubmitted]. */
    CHECK(Bc250AbortedFenceValid(100, 100, 100));   /* the empty-queue special case: all three equal */
    CHECK(Bc250AbortedFenceValid(105, 100, 110));   /* strictly inside */
    CHECK(Bc250AbortedFenceValid(100, 100, 110));   /* at the lower bound */
    CHECK(Bc250AbortedFenceValid(110, 100, 110));   /* at the upper bound */
    CHECK(!Bc250AbortedFenceValid(99, 100, 110));   /* below last completed -> would bugcheck 0x119 */
    CHECK(!Bc250AbortedFenceValid(111, 100, 110));  /* above last submitted -> would bugcheck 0x119 */

    /* 32-bit wrap: a window straddling 0xFFFFFFFF must still order correctly (the same signed-difference order
     * bc250_fence_reached uses), or a recovery near the wrap would be refused or, worse, pass an out-of-range id. */
    CHECK(Bc250AbortedFenceValid(0x00000002u, 0xFFFFFFFEu, 0x00000004u));   /* inside a wrapped window */
    CHECK(Bc250AbortedFenceValid(0xFFFFFFFEu, 0xFFFFFFFEu, 0x00000004u));   /* at the wrapped lower bound */
    CHECK(Bc250AbortedFenceValid(0x00000004u, 0xFFFFFFFEu, 0x00000004u));   /* at the wrapped upper bound */
    CHECK(!Bc250AbortedFenceValid(0xFFFFFFFDu, 0xFFFFFFFEu, 0x00000004u));  /* just below the wrapped window */
    CHECK(!Bc250AbortedFenceValid(0x00000005u, 0xFFFFFFFEu, 0x00000004u));  /* just above the wrapped window */

    /* The guard agrees with the fence-order primitive it mirrors: aborted >= lastCompleted is exactly
     * bc250_fence_reached(aborted, lastCompleted). */
    CHECK(bc250_fence_reached(105u, 100u) && (int)(105u - 100u) >= 0);
    CHECK(!bc250_fence_reached(99u, 100u) && (int)(99u - 100u) < 0);

    printf("PASS: soft-recovery gate (switch/node) and the 0x119 fence-range guard, including 32-bit wrap\n");
    return 0;
}
