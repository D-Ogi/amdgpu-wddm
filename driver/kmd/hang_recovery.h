/* M15.12 hang recovery (docs/design/hang-recovery.md): the pure decisions behind DxgkDdiResetEngine's stage-1
 * soft recovery, in a header so wddm.c, guard.c, gfx.c and the host test (test/hang_recovery_test.c,
 * run_hang_recovery.ps1) share one definition. No kernel types, no I/O: the gate, the fence-range guard, the
 * VMID guard, the kill loop and the verdicts of the sticky record. The wave kill itself (SQ_CMD), the register
 * path it takes and the fence reads are in driver/shim/bc250_gfx.c and driver/kmd/gfx.c (they touch hardware) and
 * reach the hardware only on the lab, behind the HangRecoveryMode switch - never here. */
#ifndef BC250_HANG_RECOVERY_H
#define BC250_HANG_RECOVERY_H

/* vmid_pool.h is pure data and integer arithmetic (it says so itself), and it owns the VMID numbers this file's
 * kill guard must agree with: 0 is the GART/system domain and 2 is SDMA paging's, so neither may ever be the
 * target of a wave kill. Naming them here instead would be a second copy of those constants. */
#include "vmid_pool.h"

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
 * fence id"; tdr-changes-in-windows-8.md step 8). Submission fence ids are 32-bit and wrap, so the comparison is
 * a signed difference, the same ordering bc250_fence_reached uses: aborted is valid iff
 * lastCompleted <= aborted <= lastSubmitted in that wrap-aware order. This is a last-line guard - the fence we
 * abort is the in-flight job's own fence, which is by construction above the last completed and no newer than the
 * last submitted - but a guard that costs nothing and keeps a future caller from ever handing dxgkrnl an
 * out-of-range value. */
static __inline int Bc250AbortedFenceValid(unsigned aborted, unsigned lastCompleted, unsigned lastSubmitted)
{
    return (int)(aborted - lastCompleted) >= 0 && (int)(lastSubmitted - aborted) >= 0;
}

/* The lower bound of that range is the last completed fence OF THE NODE BEING RESET. Submission fence ids are
 * counted per node: node 0 (3D) and node 1 (paging) each have their own sequence, and on the lab node 1's ids run
 * far ahead of node 0's (trial D, 2026-10-07: node 0 had reported 1185 and hung at 1186, node 1 had reported
 * 9228). Up to 0.7.216.13 ResetEngine read the adapter-wide LastCompletedFence, which holds the newest report of
 * EITHER node, so whenever paging work completed after the hang the guard compared node 0's 1186 with node 1's
 * 9228 and refused a valid recovery (verdict 4, then 0x116). The value to compare with is what the report DPC last
 * told dxgkrnl for this node: LastReportedFence[node], valid once LastReportedValid[node] is set. Returns 0 when
 * the node has no reported fence yet (or is out of range): the range cannot be proven then, and the caller
 * refuses. The arrays are wddm.c's own (volatile LONG and BOOLEAN there, long and unsigned char here). */
static __inline int Bc250HangNodeLastCompleted(const volatile long* lastReportedFence,
                                               const unsigned char* lastReportedValid, unsigned nodeCount,
                                               unsigned node, unsigned* lastCompleted)
{
    if (node >= nodeCount || !lastReportedValid[node]) return 0;
    *lastCompleted = (unsigned)lastReportedFence[node];
    return 1;
}

/* Which VMID a wave kill may name. Up to 0.7.213.1 every WDDM job ran at VMID 1 and the kill could name that
 * constant; with the VMID pool (0.7.214, vmid_pool.h) the hung job's VMID comes out of the completion-queue
 * entry, so it is a value the kill has to check. VMID 0 is the GART/system domain and VMID 2 is SDMA paging's:
 * a broadcast CHECK_VMID kill on either would terminate waves that are not the hung application's. A 0 also
 * means "this entry recorded no VMID", which is not something to kill on either. */
static __inline int Bc250KillVmidValid(unsigned vmid)
{
    return vmid < BC250_VMID_COUNT && vmid != BC250_VMID_GART && vmid != BC250_VMID_SDMA_PAGING;
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
#define BC250_HANG_VERDICT_VMID_GUARD       6u  /* the hung job named no VMID of the pool: refused, nothing killed */
#define BC250_HANG_VERDICT_ABORT_REPORTED   7u  /* BD-114: nothing on the ring, so this node's last reported fence
                                                   is named as aborted. Nothing was killed: the packet completed
                                                   between the watchdog and the TDR, and the ring is already idle */

/* Before any kill: stage 1 needs a job of the reset's own node on the ring (the WDDM queue's view, after late
 * fences have been retired), an abort fence the engine-reset contract accepts, and a VMID a kill may name.
 * lastCompletedKnown and lastCompleted are Bc250HangNodeLastCompleted's answer for the reset node; a node with no
 * reported fence fails the fence guard, because nothing proves the abort fence is in range.
 * PENDING = go ahead: record, then kill. The others are refusals decided without touching the hardware. */
static __inline unsigned Bc250HangPreKillVerdict(int jobOnRing, unsigned abortFence, int lastCompletedKnown,
                                                 unsigned lastCompleted, unsigned vmid)
{
    if (!jobOnRing) return BC250_HANG_VERDICT_NOTHING_ON_RING;
    if (!lastCompletedKnown) return BC250_HANG_VERDICT_FENCE_GUARD;
    if (!Bc250AbortedFenceValid(abortFence, lastCompleted, abortFence)) return BC250_HANG_VERDICT_FENCE_GUARD;
    if (!Bc250KillVmidValid(vmid)) return BC250_HANG_VERDICT_VMID_GUARD;
    return BC250_HANG_VERDICT_PENDING;
}

/* BD-114, ANALYSIS.md 7.4: verdict 3 is what a packet that ran past our private watchdog and then completed
 * before the TDR gets today, and verdict 3 is a refusal, after which ResetFromTimeout fails and the machine
 * bugchecks 0x116. The contract sanctions the other answer, tdr-changes-in-windows-8.md:100: "A special
 * situation can occur when a packet is completed on the GPU between steps 3 and 7. In this case, the driver
 * should set LastAbortedFenceId to the fence ID of the last completed packet if there are no packets in the
 * hardware queue from the driver's point of view. From the scheduler's point of view, it appears that such a
 * packet was aborted."
 *
 * The design document's objection has to be answered rather than waved away (hang-recovery.md, "Limits"): "our
 * view of 'last completed' can lag a pending report, and a wrong choice re-executes a packet". The answer is in
 * the three guards below, and every one of them is a condition the report DPC already tracks:
 *   - nothing of this node on the ring. Only then is there no packet whose completion we would be pre-empting.
 *   - no completion pending for this node (CompletionPending[node] == 0). A pending completion is exactly the
 *     lag the objection names: a fence the hardware has produced and dxgkrnl has not been told about. With one
 *     outstanding, "last reported" is not the last completed and the fence named would be wrong.
 *   - the node has a reported fence at all (LastReportedValid[node]), and that fence is inside the engine-reset
 *     contract's range against the last fence dxgkrnl submitted on this node. Out of range is bugcheck 0x119.
 * The fence named is the node's own LastReportedFence, so the lower bound of the range holds by construction and
 * only the upper bound can refuse. A refusal leaves verdict 3 and today's behaviour exactly as it was. */
static __inline int Bc250HangAbortReportedFence(int jobOnRing, int completionPending, int lastReportedKnown,
                                                unsigned lastReported, int lastSubmittedKnown,
                                                unsigned lastSubmitted, unsigned* abortFence)
{
    if (jobOnRing || completionPending || !lastReportedKnown || !lastSubmittedKnown) return 0;
    if (!Bc250AbortedFenceValid(lastReported, lastReported, lastSubmitted)) return 0;
    *abortFence = lastReported;
    return 1;
}

/* The kill loop of amdgpu_ring_soft_recovery, with its hardware behind four callbacks so that gfx.c and the host
 * test run the same loop. Retired: the newest sequence's fence has been reached. Expired: the 10 ms budget ran
 * out. Kill: one SQ_CMD wave kill; it returns 1 when the write reached the register and 0 when the register path
 * refused it. Stall: the 100 us between kills.
 *
 * Only a kill that reached the register is counted, and a refused kill ends the loop at once: the refusal is
 * sticky (a stopped sequence drops every later write), so repeating it would only count kills that never
 * happened. 0.7.216.16 (lab trial D1, 2026-10-07) issued the kill through the GART sequence, whose table has no
 * SQ_CMD: the first write was refused, the next 94 were dropped, and the record still said 95 kills. The loop
 * leaves only from a look at the fence, so a sequence that retired on its own is still found. *refused tells a
 * refused kill from a budget that ran out: both are NOT_DRAINED. */
typedef struct bc250_hang_kill_ops {
    int (*Retired)(void* context);
    int (*Expired)(void* context);
    int (*Kill)(void* context);
    void (*Stall)(void* context);
} BC250_HANG_KILL_OPS;

static __inline unsigned Bc250HangKillLoop(const BC250_HANG_KILL_OPS* ops, void* context, unsigned* kills,
                                           int* refused)
{
    *kills = 0;
    *refused = 0;
    for (;;) {
        if (ops->Retired(context))
            return *kills == 0 ? BC250_HANG_VERDICT_ALREADY_RETIRED : BC250_HANG_VERDICT_DRAINED;
        if (*refused || ops->Expired(context)) return BC250_HANG_VERDICT_NOT_DRAINED;
        if (!ops->Kill(context)) { *refused = 1; continue; }     /* one last look, then out */
        (*kills)++;
        ops->Stall(context);
    }
}

/* The verdicts after which ResetEngine reports success: in each the newest sequence retired and the ring is
 * idle. ABORT_REPORTED (BD-114) is the third: no kill ran there, because the packet had already completed, and
 * the fence named as aborted is the one dxgkrnl was last told about. */
static __inline int Bc250HangVerdictRecovered(unsigned verdict)
{
    return verdict == BC250_HANG_VERDICT_DRAINED || verdict == BC250_HANG_VERDICT_ALREADY_RETIRED ||
           verdict == BC250_HANG_VERDICT_ABORT_REPORTED;
}

/* Which counters of the record one write adds 1 to. Every attempt counts one Attempt in its first write (the
 * pending record before a kill, or an outcome decided before any kill - a refusal, or BD-114's ABORT_REPORTED)
 * and one outcome in its last, so after every finished attempt Attempts == Recovered + NotDrained + Refused; an
 * attempt the machine did not survive leaves Attempts one ahead with LastVerdict 0. */
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
    /* BD-114: decided before any kill, like the refusals, so it counts its own attempt - there is no PENDING
     * write ahead of it to have counted one. Without this the record's invariant would break the other way. */
    case BC250_HANG_VERDICT_ABORT_REPORTED:   return BC250_HANG_COUNT_ATTEMPT | BC250_HANG_COUNT_RECOVERED;
    case BC250_HANG_VERDICT_NOT_DRAINED:      return BC250_HANG_COUNT_NOT_DRAINED;
    default:                                  return BC250_HANG_COUNT_ATTEMPT | BC250_HANG_COUNT_REFUSED;
    }
}

#endif /* BC250_HANG_RECOVERY_H */
