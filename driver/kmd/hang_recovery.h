/* M15.12 hang recovery (docs/design/hang-recovery.md): the pure decisions behind DxgkDdiResetEngine's stage-1
 * soft recovery, in a header so wddm.c, guard.c, gfx.c and the host test (test/hang_recovery_test.c,
 * run_hang_recovery.ps1) share one definition. No kernel types, no I/O: the gate, the fence-range guard and the
 * verdicts of the sticky record. The wave kill itself (SQ_CMD) and the bounded fence poll are in
 * driver/shim/bc250_gfx.c and driver/kmd/gfx.c (they touch hardware) and are exercised only on the lab, behind the
 * HangRecoveryMode switch - never here. */
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

/* The verdict of one ResetEngine call that reached stage 1 (the record, docs/design/hang-recovery.md). guard.c
 * GuardRecordHangRecovery writes it, flushed, to the non-volatile subkey Parameters\HangRecovery, because the
 * KMD log ring (about 1024 lines, in memory) does not outlive the 0x116 and the reboot that follow a refusal. */
#define BC250_HANG_VERDICT_PENDING          0u  /* on disk before the first kill; still 0 after a reboot = the
                                                   machine went down between the kill and the verdict */
#define BC250_HANG_VERDICT_DRAINED          1u  /* the kills retired the newest sequence: reset reported, node 0 open */
#define BC250_HANG_VERDICT_NOT_DRAINED      2u  /* the bound ran out (or there was no ring to kill on): refused */
#define BC250_HANG_VERDICT_NOTHING_ON_RING  3u  /* no node-0 job on the ring after late fences retired: refused */
#define BC250_HANG_VERDICT_FENCE_GUARD      4u  /* the abort fence fails the 0x119 range guard: refused */
#define BC250_HANG_VERDICT_ALREADY_RETIRED  5u  /* retired before the first kill: the ring is idle, reset reported */

/* Before any kill: stage 1 needs a job of node 0 on the ring (the WDDM queue's view, after late fences have been
 * retired) and an abort fence the engine-reset contract accepts. PENDING = go ahead: record, then kill. The other
 * two are refusals decided without touching the hardware. */
static __inline unsigned Bc250HangPreKillVerdict(int jobOnRing, unsigned abortFence, unsigned lastCompleted)
{
    if (!jobOnRing) return BC250_HANG_VERDICT_NOTHING_ON_RING;
    if (!Bc250AbortedFenceValid(abortFence, lastCompleted, abortFence)) return BC250_HANG_VERDICT_FENCE_GUARD;
    return BC250_HANG_VERDICT_PENDING;
}

/* The verdicts after which ResetEngine reports success: either way the newest sequence retired and the ring is idle. */
static __inline int Bc250HangVerdictRecovered(unsigned verdict)
{
    return verdict == BC250_HANG_VERDICT_DRAINED || verdict == BC250_HANG_VERDICT_ALREADY_RETIRED;
}

/* Which counters of the record one write adds 1 to. Every attempt counts one Attempt in its first write (the
 * pending record before a kill, or a refusal decided before any kill) and one outcome in its last, so after every
 * finished attempt Attempts == Recovered + NotDrained + Refused; an attempt the machine did not survive leaves
 * Attempts one ahead with LastVerdict 0. */
#define BC250_HANG_COUNT_ATTEMPT     0x1u
#define BC250_HANG_COUNT_RECOVERED   0x2u
#define BC250_HANG_COUNT_NOT_DRAINED 0x4u
#define BC250_HANG_COUNT_REFUSED     0x8u

static __inline unsigned Bc250HangVerdictCounts(unsigned verdict)
{
    switch (verdict) {
    case BC250_HANG_VERDICT_PENDING:          return BC250_HANG_COUNT_ATTEMPT;
    case BC250_HANG_VERDICT_DRAINED:
    case BC250_HANG_VERDICT_ALREADY_RETIRED:  return BC250_HANG_COUNT_RECOVERED;
    case BC250_HANG_VERDICT_NOT_DRAINED:      return BC250_HANG_COUNT_NOT_DRAINED;
    default:                                  return BC250_HANG_COUNT_ATTEMPT | BC250_HANG_COUNT_REFUSED;
    }
}

#endif /* BC250_HANG_RECOVERY_H */
