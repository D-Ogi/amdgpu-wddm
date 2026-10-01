/* M15.12 hang recovery (DESIGN.md): the pure decisions behind DxgkDdiResetEngine's stage-1 soft recovery, in a
 * header so wddm.c and the host test (test/hang_recovery_test.c, run_hang_recovery.ps1) share one definition.
 * No kernel types, no I/O: just the gate and the fence-range guard. The wave kill itself (SQ_CMD) and the
 * bounded fence poll are in driver/shim/bc250_gfx.c and driver/kmd/gfx.c (they touch hardware) and are
 * exercised only on the lab, behind the HangRecoveryMode switch - never here. */
#ifndef BC250_HANG_RECOVERY_H
#define BC250_HANG_RECOVERY_H

/* Attempt a soft recovery only with the switch on and only for the 3D/compute node (ordinal 0). The paging node
 * (ordinal 1) is never soft-recovered: a successful reset of a paging packet makes dxgkrnl follow with an
 * adapter-wide reset (TDR changes in Windows 8, step 9), which on this part is the 0x116 path anyway, and the
 * hang class never involves node 1. */
static __inline int Bc250SoftRecoveryWanted(int hangRecoveryMode, unsigned nodeOrdinal)
{
    return hangRecoveryMode != 0 && nodeOrdinal == 0u;
}

/* The engine-reset contract: the LastAbortedFenceId returned from DxgkDdiResetEngine must be in
 * [last completed fence, last submitted fence] or dxgkrnl bugchecks 0x119 (parameter 0xA, "invalid aborted
 * fence id"). Submission fence ids are 32-bit and wrap, so the comparison is a signed difference, the same
 * ordering bc250_fence_reached uses: aborted is valid iff lastCompleted <= aborted <= lastSubmitted in that
 * wrap-aware order. This is a last-line guard - the fence we abort is the in-flight job's own fence, which is by
 * construction above the last completed and no newer than the last submitted - but a guard that costs nothing
 * and keeps a future caller from ever handing dxgkrnl an out-of-range value. */
static __inline int Bc250AbortedFenceValid(unsigned aborted, unsigned lastCompleted, unsigned lastSubmitted)
{
    return (int)(aborted - lastCompleted) >= 0 && (int)(lastSubmitted - aborted) >= 0;
}

#endif /* BC250_HANG_RECOVERY_H */
