/* TDR recovery stage S1 (docs/design/tdr-recovery-s1.md): the pure decisions behind DxgkDdiResetFromTimeout, in a
 * header so wddm.c and the host test (test/adapter_reset_test.c, run_adapter_reset.ps1) share one definition. No
 * kernel types, no I/O: the switch, the busy reasons of the idle witnesses, the begin of the reset on every node
 * and the fence advance of an adapter-wide reset. The register reads, the fence reads and the wave kill are in
 * wddm.c and gfx.c, and reach the hardware only on the lab.
 *
 * The contract (windows-driver-docs 110f60ea, windows-driver-docs-ddi 7515063):
 *   - DxgkDdiResetFromTimeout "guarantees that the GPU is not writing or reading any memory by the time that
 *     DxgkDdiResetFromTimeout returns". A failure return is a bugcheck (nc-d3dkmddi-dxgkddi_resetfromtimeout.md),
 *     which is 0x116 with the status in parameter 3 (bug-check-0x116---video-tdr-failure.md).
 *   - "For adapter-wide reset, the last completed fence ID on all nodes should be advanced to the last submitted
 *     fence ID at the time of the reset" (tdr-changes-in-windows-8.md).
 *   - No other thread runs in the driver during the call, but interrupts and DPCs can
 *     (thread-synchronization-and-tdr.md).
 *
 * This part has no adapter reset that is measured to work (facts M53, M351). S1 therefore resets nothing: it
 * returns success only when the witnesses below show that no engine of this driver has work and no engine reports
 * busy. Then the contract holds without a reset, because nothing reads or writes memory. In every other case S1
 * returns the old failure, so S1 never makes a hang worse than before. */
#ifndef BC250_ADAPTER_RESET_H
#define BC250_ADAPTER_RESET_H

#include "hang_recovery.h"
#include "gc_10_1_0_sh_mask.h"      /* GRBM_STATUS, CP_STAT, CP_CPC_STATUS, SDMA0_STATUS_REG field masks */

/* The switch AdapterRecoveryMode (Parameters, REG_DWORD, start-latched like HangRecoveryMode).
 *   0     off: ResetFromTimeout fails every time, as before S1 (0x116).
 *   1     witnesses only: success when every engine is already idle.
 *   2     (absent, or any other value) a bounded wave kill on the IB nodes 0 and 2 first, the kill of
 *         hang-recovery.md stage 1, then the same witnesses.
 * HangRecoveryMode 0 also turns S1 off, so that HangRecoveryMode 0 keeps every TDR DDI as it was before 0.7.216.13. */
#define BC250_ADAPTER_RECOVERY_OFF  0u
#define BC250_ADAPTER_RECOVERY_IDLE 1u
#define BC250_ADAPTER_RECOVERY_KILL 2u

static __inline unsigned Bc250AdapterRecoveryMode(int hangRecoveryMode, unsigned long setting)
{
    if (!hangRecoveryMode || setting == BC250_ADAPTER_RECOVERY_OFF) return BC250_ADAPTER_RECOVERY_OFF;
    if (setting == BC250_ADAPTER_RECOVERY_IDLE) return BC250_ADAPTER_RECOVERY_IDLE;
    return BC250_ADAPTER_RECOVERY_KILL;
}

/* Why ResetFromTimeout refused. Stable on-disk bits: HangRecovery\LastSeq of the state store, verdict
 * BC250_HANG_VERDICT_ADAPTER_BUSY. More than one bit can be set. */
#define BC250_ADAPTER_BUSY_GFX_RING       0x0001u /* the newest sequence on the GFX ring (node 0) did not retire */
#define BC250_ADAPTER_BUSY_COMPUTE_RING   0x0002u /* the newest sequence on the compute ring (node 2) did not retire */
#define BC250_ADAPTER_BUSY_PAGING_RING    0x0004u /* the paging packet in flight on SDMA0 (node 1) did not retire */
#define BC250_ADAPTER_BUSY_GUI_ACTIVE     0x0008u /* GRBM_STATUS.GUI_ACTIVE: a GFX block is busy */
#define BC250_ADAPTER_BUSY_CP             0x0010u /* CP_STAT.CP_BUSY */
#define BC250_ADAPTER_BUSY_CPC            0x0020u /* CP_CPC_STATUS.CPC_BUSY: the compute micro engines */
#define BC250_ADAPTER_BUSY_SDMA0          0x0040u /* SDMA0_STATUS_REG.IDLE is clear */
#define BC250_ADAPTER_BUSY_SDMA0_POINTERS 0x0080u /* SDMA0 GFX ring: read pointer is not the write pointer */
#define BC250_ADAPTER_BUSY_UNREAD         0x0100u /* a witness register was not read */
#define BC250_ADAPTER_BUSY_PUBLICATION    0x0200u /* a completion or preemption report stayed in flight */
#define BC250_ADAPTER_BUSY_SUBMISSIONS    0x0400u /* a submission was still inside the driver */
#define BC250_ADAPTER_BUSY_STOPPING       0x0800u /* the device stop has begun */
#define BC250_ADAPTER_BUSY_NO_STATE       0x1000u /* no WDDM state, or a node reset was already active */
#define BC250_ADAPTER_BUSY_WINDOW         0x2000u /* an earlier adapter reset has not had its restart yet */
#define BC250_ADAPTER_BUSY_PAGING_QUEUED  0x4000u /* a paging job was queued but never ran: VidMm may still need it */

/* One sample of the witness registers. Read is a mask: bit n is set when register n was read. The order is the
 * order of wddm.c's register table, which a C_ASSERT there binds to BC250_ADAPTER_SAMPLE_REGISTERS. */
#define BC250_ADAPTER_REG_GRBM_STATUS    0u
#define BC250_ADAPTER_REG_CP_STAT        1u
#define BC250_ADAPTER_REG_CP_CPC_STATUS  2u
#define BC250_ADAPTER_REG_SDMA0_STATUS   3u
#define BC250_ADAPTER_REG_SDMA0_RPTR     4u
#define BC250_ADAPTER_REG_SDMA0_RPTR_HI  5u
#define BC250_ADAPTER_REG_SDMA0_WPTR     6u
#define BC250_ADAPTER_REG_SDMA0_WPTR_HI  7u
#define BC250_ADAPTER_SAMPLE_REGISTERS   8u
#define BC250_ADAPTER_SAMPLES            3u     /* three samples, about 1 ms apart (wddm.c) */

typedef struct bc250_adapter_sample {
    unsigned Read;
    unsigned Value[BC250_ADAPTER_SAMPLE_REGISTERS];
} BC250_ADAPTER_SAMPLE;

/* The busy bits of one sample. The GFX and SDMA tests are amdgpu's own idle tests (gfx_v10_0_is_idle:
 * GRBM_STATUS.GUI_ACTIVE; sdma_v5_0_is_idle: SDMA0_STATUS_REG.IDLE), with two more: CP_STAT.CP_BUSY and
 * CP_CPC_STATUS.CPC_BUSY. The SDMA0 ring pointers are a witness only while node 1 runs on that ring
 * (sdmaRingInUse); before the paging node has its ring, the pointers are not ours to read meaning into. The GFX
 * ring pointers are not a witness: node 0 is a kernel queue that the KIQ maps, and what CP_RB0_RPTR and
 * CP_RB0_WPTR say about such a queue is not measured on this part. The fence of the newest sequence is the GFX
 * ring's witness instead (Bc250AdapterBusyReasons). */
static __inline unsigned Bc250AdapterSampleBusy(const BC250_ADAPTER_SAMPLE* s, int sdmaRingInUse)
{
    unsigned need = (1u << BC250_ADAPTER_REG_GRBM_STATUS) | (1u << BC250_ADAPTER_REG_CP_STAT) |
                    (1u << BC250_ADAPTER_REG_CP_CPC_STATUS) | (1u << BC250_ADAPTER_REG_SDMA0_STATUS);
    unsigned pointers = (1u << BC250_ADAPTER_REG_SDMA0_RPTR) | (1u << BC250_ADAPTER_REG_SDMA0_RPTR_HI) |
                        (1u << BC250_ADAPTER_REG_SDMA0_WPTR) | (1u << BC250_ADAPTER_REG_SDMA0_WPTR_HI);
    unsigned busy = 0;

    if (sdmaRingInUse) need |= pointers;
    if ((s->Read & need) != need) busy |= BC250_ADAPTER_BUSY_UNREAD;
    if ((s->Read & (1u << BC250_ADAPTER_REG_GRBM_STATUS)) &&
        (s->Value[BC250_ADAPTER_REG_GRBM_STATUS] & GRBM_STATUS__GUI_ACTIVE_MASK))
        busy |= BC250_ADAPTER_BUSY_GUI_ACTIVE;
    if ((s->Read & (1u << BC250_ADAPTER_REG_CP_STAT)) && (s->Value[BC250_ADAPTER_REG_CP_STAT] & CP_STAT__CP_BUSY_MASK))
        busy |= BC250_ADAPTER_BUSY_CP;
    if ((s->Read & (1u << BC250_ADAPTER_REG_CP_CPC_STATUS)) &&
        (s->Value[BC250_ADAPTER_REG_CP_CPC_STATUS] & CP_CPC_STATUS__CPC_BUSY_MASK))
        busy |= BC250_ADAPTER_BUSY_CPC;
    if ((s->Read & (1u << BC250_ADAPTER_REG_SDMA0_STATUS)) &&
        !(s->Value[BC250_ADAPTER_REG_SDMA0_STATUS] & SDMA0_STATUS_REG__IDLE_MASK))
        busy |= BC250_ADAPTER_BUSY_SDMA0;
    if (sdmaRingInUse && (s->Read & pointers) == pointers &&
        (s->Value[BC250_ADAPTER_REG_SDMA0_RPTR] != s->Value[BC250_ADAPTER_REG_SDMA0_WPTR] ||
         s->Value[BC250_ADAPTER_REG_SDMA0_RPTR_HI] != s->Value[BC250_ADAPTER_REG_SDMA0_WPTR_HI]))
        busy |= BC250_ADAPTER_BUSY_SDMA0_POINTERS;
    return busy;
}

/* The software witnesses, from gfx.c: for each ring, the fence slot reached the newest sequence that this driver
 * wrote to it (a ring that never ran counts as retired). A ring runs in order, so its newest sequence retires only
 * after every packet before it retired, and then the ring has no work. */
typedef struct bc250_adapter_rings {
    int GfxRetired;
    int ComputeRetired;
    int PagingRetired;
} BC250_ADAPTER_RINGS;

/* All witnesses together. Every sample must be idle: a busy bit in any of them refuses. No sample at all is
 * UNREAD. 0 = idle, and only 0 lets ResetFromTimeout return success. */
static __inline unsigned Bc250AdapterBusyReasons(const BC250_ADAPTER_RINGS* rings, const BC250_ADAPTER_SAMPLE* samples,
                                                 unsigned count, int sdmaRingInUse)
{
    unsigned busy = 0, i;

    if (!rings->GfxRetired) busy |= BC250_ADAPTER_BUSY_GFX_RING;
    if (!rings->ComputeRetired) busy |= BC250_ADAPTER_BUSY_COMPUTE_RING;
    if (!rings->PagingRetired) busy |= BC250_ADAPTER_BUSY_PAGING_RING;
    if (count == 0) busy |= BC250_ADAPTER_BUSY_UNREAD;
    for (i = 0; i < count; i++) busy |= Bc250AdapterSampleBusy(&samples[i], sdmaRingInUse);
    return busy;
}

/* The begin of an adapter reset, for every node in one critical section (the caller holds the WDDM lock).
 * ResetActive on a node stops the report DPC from publishing on it and stops new submissions from reaching the
 * implementation (hang-recovery.md, "Recovery is an exclusive per-node transaction"). It is all or nothing: when
 * one node refuses, no node begins, and the reasons say why. reportInFlight is not waited for here; the caller
 * gives a report in flight a bounded time to end before it calls this again. */
static __inline unsigned Bc250AdapterResetBeginAll(BC250_HANG_NODE_STATE* states, const volatile long* activeSubmissions,
                                                   unsigned nodeCount)
{
    unsigned reasons = 0, n;

    for (n = 0; n < nodeCount; n++) {
        if (states[n].ResetActive) reasons |= BC250_ADAPTER_BUSY_NO_STATE;
        if (states[n].ReportInFlight) reasons |= BC250_ADAPTER_BUSY_PUBLICATION;
        if (activeSubmissions[n] != 0) reasons |= BC250_ADAPTER_BUSY_SUBMISSIONS;
    }
    if (reasons != 0) return reasons;
    for (n = 0; n < nodeCount; n++) (void)Bc250HangResetBegin(&states[n], 0);
    return 0;
}

/* The undo of Bc250AdapterResetBeginAll after a refusal: nothing advances. */
static __inline void Bc250AdapterResetAbortAll(BC250_HANG_NODE_STATE* states, unsigned nodeCount)
{
    unsigned n;

    for (n = 0; n < nodeCount; n++) Bc250HangResetEnd(&states[n], 0, 0);
}

/* The fence that a node's "last completed" takes after an adapter-wide reset: the last submitted fence at the
 * time of the reset (tdr-changes-in-windows-8.md). A node with no submitted fence keeps the fence it last
 * reported. A node with neither has nothing to advance: 0. */
static __inline int Bc250AdapterResetFence(int submittedKnown, unsigned submitted, int reportedKnown, unsigned reported,
                                           unsigned* fence)
{
    if (submittedKnown) { *fence = submitted; return 1; }
    if (reportedKnown) { *fence = reported; return 1; }
    return 0;
}

/* The per-node state after a successful adapter reset, under the WDDM lock in the commit. The completion that the
 * node has seen, the notification watermark and the scheduler boundary are all the advanced fence now, so that a
 * later empty-queue engine reset (verdict 7) and a later preemption report name a fence the scheduler accepts. A
 * lost report is resolved: the scheduler counts its fence as completed. ResetActive stays set: the commit ends the
 * reset only at DxgkDdiRestartFromTimeout (Bc250AdapterRestartNode). */
static __inline void Bc250AdapterResetCommitNode(BC250_HANG_NODE_STATE* state, int known, unsigned fence)
{
    if (known) {
        state->CompletedFence = fence;
        state->CompletedKnown = 1;
        state->BoundaryFence = fence;
        state->BoundaryKnown = 1;
    }
    state->ReportLost = 0;
    state->ReportInFlight = 0;
}

/* What a node does at DxgkDdiRestartFromTimeout with the submissions that arrived between the two calls (the
 * window). While ResetActive is set, WddmBeginSubmissionLocked refuses them, so none of them ran and none of them
 * has a completion. The window is not measured on this part: tdr-changes-in-windows-8.md says only that VidMm
 * purges allocations in it, with paging operations that need no GPU work.
 *   NONE      no submission in the window: the node reopens.
 *   COMPLETE  only empty submissions (no command bytes): the node reopens, and the newest window fence is
 *             completed in software, because an empty packet has nothing to run.
 *   CLOSE     a submission with commands did not run: completing its fence would tell the scheduler that work
 *             ran which did not, so the node stays closed. The next timeout of that node is the old failure. */
#define BC250_ADAPTER_WINDOW_NONE     0u
#define BC250_ADAPTER_WINDOW_COMPLETE 1u
#define BC250_ADAPTER_WINDOW_CLOSE    2u

static __inline unsigned Bc250AdapterWindowOutcome(unsigned submits, unsigned nonEmpty)
{
    if (nonEmpty != 0) return BC250_ADAPTER_WINDOW_CLOSE;
    if (submits != 0) return BC250_ADAPTER_WINDOW_COMPLETE;
    return BC250_ADAPTER_WINDOW_NONE;
}

/* DxgkDdiRestartFromTimeout: the reset of this node ends, with the boundary that the commit wrote. */
static __inline void Bc250AdapterRestartNode(BC250_HANG_NODE_STATE* state)
{
    state->ResetActive = 0;
}

#endif /* BC250_ADAPTER_RESET_H */
