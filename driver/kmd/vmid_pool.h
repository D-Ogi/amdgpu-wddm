/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* vmid_pool.h - which hardware VMID a graphics job runs at (docs/design/gfx-submit-root-serialization.md).
 *
 * Up to 0.7.213.1 every application job ran at VMID 1, and gfx.c refused a job of another page-table root while a
 * job was in flight, because the root of VMID 1 is changed by MMIO from the CPU and a root change must not redirect
 * a job that still uses the VMID. The game and DWM have different roots, so every frame waited for the other
 * process's job to retire (sessions 313/314; K138 after KMD 196's spin and event wake: about 0.24 ms a frame).
 *
 * With the pool, a root gets a VMID of its own and keeps it while it keeps submitting:
 *
 *   1. A pool VMID that already holds the job's root is used again, whatever is in flight on it. The root does not
 *      change, so nothing can be redirected. (The per-submit invalidation still runs, as it always did: it is what
 *      picks up a leaf-PTE change made under an unchanged root.)
 *   2. Otherwise the least recently used pool VMID whose last job has retired is claimed and given the root.
 *   3. Otherwise there is no VMID to give, and the caller gets STATUS_DEVICE_BUSY, which is the old behaviour.
 *
 * THE RULE: the root of a VMID changes only after the last job submitted at that VMID has retired. LiveSeq holds
 * that job's sequence, the fence is global and in order, so one read of the fence slot answers it for every VMID.
 * Bc250VmidMayProgram is the same rule restated as a check the claim path makes before every register write.
 *
 * Least recently USED, not least recently claimed (the design note said claimed): a VMID's order is bumped by every
 * job, so a process that keeps submitting keeps its VMID, and a recycled VMID is one nobody has used for longest.
 * By claim order DWM's VMID, claimed at boot, would be the first one recycled once the pool had gone round once.
 *
 * Pure data and integer arithmetic, no kernel call and no lock: gfx.c holds GartLock for every change of
 * LiveSeq/Order and its VMID spin lock as well for every change of what a reader at DISPATCH_LEVEL looks at (Root,
 * the tenant fields, the history). The host test (driver/kmd/test/vmid_pool_test.c) drives it with a model fence.
 */
#ifndef BC250_VMID_POOL_H
#define BC250_VMID_POOL_H

#include "bc250_fence_order.h"

#define BC250_VMID_COUNT 16u                /* AMDGPU_NUM_VMID, driver/shim/include/amdgpu.h */
#define BC250_VMID_GART 0u                  /* the system domain, the GART aperture: never ours to re-point */
#define BC250_VMID_LEGACY 1u                /* the one application VMID up to 0.7.213.1, and EnableVmidPool 0 */
#define BC250_VMID_SDMA_PAGING 2u           /* BC250_SDMA_PAGING_VMID: node 1 programs it per paging buffer */
#define BC250_VMID_RESERVED ((1u << BC250_VMID_GART) | (1u << BC250_VMID_SDMA_PAGING))
#define BC250_VMID_CANDIDATES (0xFFFFu & ~BC250_VMID_RESERVED)     /* 1 and 3..15 */
#define BC250_VMID_NONE 0xFFFFFFFFu
#define BC250_VMID_HISTORY 16u              /* retired tenancies kept for fault attribution; a power of two */

/* The live table. Index = VMID. Root 0 = this VMID was given no root in this engine instance. */
typedef struct BC250_VMID_TABLE {
    unsigned long long Root[BC250_VMID_COUNT];   /* the page directory root the VMID was last given */
    unsigned long long Order[BC250_VMID_COUNT];  /* Clock at the VMID's last claim or job; 0 = never used */
    unsigned long long Clock;
    unsigned long LiveSeq[BC250_VMID_COUNT];     /* newest job at this VMID not yet seen retired; 0 = none */
    unsigned long FirstSeq[BC250_VMID_COUNT];    /* the current tenant's first job; 0 = none submitted yet */
    unsigned long LastSeq[BC250_VMID_COUNT];     /* the current tenant's newest job, kept after it retired */
    unsigned long Process[BC250_VMID_COUNT];     /* the process of the current tenant's newest job */
} BC250_VMID_TABLE;

/* One tenancy that ended: a VMID given a different root, or the table reset by a teardown. */
typedef struct BC250_VMID_TENANT {
    unsigned long long Root;
    unsigned long Vmid, Process, FirstSeq, LastSeq;
} BC250_VMID_TENANT;

typedef struct BC250_VMID_HISTORY_RING {
    BC250_VMID_TENANT Entry[BC250_VMID_HISTORY];
    unsigned long Next;                          /* tenancies recorded; the newest is Entry[(Next - 1) % 16] */
} BC250_VMID_HISTORY_RING;

/* What the bring-up read found, and so which VMIDs the pool may use. Value[v] is VMID v's page-table base pair as
 * the hardware held it before this driver wrote any of them (bc250_gmc_get_vmid_pd). A non-zero value in 3..15
 * means something else programmed that context: the VMID stays out of the pool, and *Excluded says which. VMID 1
 * stays in whatever it holds: every driver up to 0.7.213.1 rewrote it on every job, so a value there is this
 * driver's own from an earlier start in the same boot, and leaving it out would only turn the pool into the old
 * single-VMID path with one VMID fewer. 0 and 2 are never in the pool. */
static __inline unsigned Bc250VmidPoolFromProbe(const unsigned long long Value[BC250_VMID_COUNT], unsigned* Excluded)
{
    unsigned members = 1u << BC250_VMID_LEGACY, excluded = 0, v;

    for (v = 0; v < BC250_VMID_COUNT; v++) {
        if (!(BC250_VMID_CANDIDATES & (1u << v)) || v == BC250_VMID_LEGACY) continue;
        if (Value[v] != 0) excluded |= 1u << v;
        else members |= 1u << v;
    }
    if (Excluded) *Excluded = excluded;
    return members;
}

static __inline unsigned Bc250VmidCount(unsigned Mask)
{
    unsigned n = 0;
    for (; Mask; Mask &= Mask - 1u) n++;
    return n;
}

/* Has the last job submitted at Vmid retired? Observed is the fence slot as read once by the caller. */
static __inline int Bc250VmidRetired(const BC250_VMID_TABLE* Table, unsigned Vmid, unsigned long Observed)
{
    return Table->LiveSeq[Vmid] == 0 || bc250_fence_reached((unsigned)Observed, (unsigned)Table->LiveSeq[Vmid]);
}

/* Forget every live sequence the fence has passed. Every submit calls it, so a sequence of a VMID nobody uses any
 * more is cleared long before it could age 2^31 submissions into the wrap window of bc250_fence_reached, where an
 * old retired job would read as live again (safe, but a VMID lost to the pool). */
static __inline void Bc250VmidSweep(BC250_VMID_TABLE* Table, unsigned long Observed)
{
    unsigned v;
    for (v = 0; v < BC250_VMID_COUNT; v++)
        if (Table->LiveSeq[v] != 0 && bc250_fence_reached((unsigned)Observed, (unsigned)Table->LiveSeq[v]))
            Table->LiveSeq[v] = 0;
}

/* THE RULE as a check: may Vmid be given Root now? Yes when it already holds Root (no change, nothing to
 * redirect) or when its last job has retired. The claim path asks this before every register write. */
static __inline int Bc250VmidMayProgram(const BC250_VMID_TABLE* Table, unsigned Vmid, unsigned long long Root,
                                        unsigned long Observed)
{
    if (Vmid >= BC250_VMID_COUNT) return 0;
    return Table->Root[Vmid] == Root || Bc250VmidRetired(Table, Vmid, Observed);
}

/* The chooser. Members is the pool (reserved VMIDs are masked off here whatever the caller passes). Returns the
 * VMID and sets *Claim to 1 when the VMID must be given Root (a different root, or none yet), 0 when it holds Root
 * already; BC250_VMID_NONE when no member is free, which the caller answers with STATUS_DEVICE_BUSY. A root of 0 is
 * never "resident": an unused VMID has Root 0 too. */
static __inline unsigned Bc250VmidChoose(const BC250_VMID_TABLE* Table, unsigned Members, unsigned long long Root,
                                         unsigned long Observed, int* Claim)
{
    unsigned v, best = BC250_VMID_NONE;

    *Claim = 0;
    Members &= BC250_VMID_CANDIDATES;
    if (Root != 0)
        for (v = 0; v < BC250_VMID_COUNT; v++)
            if ((Members & (1u << v)) && Table->Root[v] == Root) return v;
    for (v = 0; v < BC250_VMID_COUNT; v++) {
        if (!(Members & (1u << v)) || !Bc250VmidRetired(Table, v, Observed)) continue;
        if (best == BC250_VMID_NONE || Table->Order[v] < Table->Order[best]) best = v;
    }
    if (best != BC250_VMID_NONE) *Claim = 1;
    return best;
}

/* The whole admission decision of one graphics job, as SubmitIbLocked makes it (gfx.c).
 *
 *   Requested  BC250_VMID_AUTO from the WDDM path, or an explicit VMID 0..15 from the IB_AT escape.
 *   PoolGate   EnableVmidPool as read at GfxStart. With it closed, AUTO means VMID 1 and every job takes the
 *              single-VMID path of 0.7.213.1 exactly: the refusal below is that driver's predicate, unchanged.
 *   InFlight   a job is in flight and its fence has not arrived (Gfx->SubmitInFlight, GfxFenceArrived).
 *   LastVmid   the VMID of the newest job submitted (Gfx->SubmitVmid).
 *
 * An explicit VMID always takes the single-VMID predicate: the escape is a diagnostic and stays exclusive. With the
 * pool, a VMID 0 job in flight still holds everything off (VMID 0 diagnostics stay exclusive, as before). The rule
 * is checked last for every non-zero VMID, whichever way it was chosen: a decision that would rewrite the root of
 * a VMID with a live job comes back BC250_VMID_REFUSE_RULE, which the caller logs once and answers BUSY. */
#define BC250_VMID_AUTO 0xFFFFFFFEu
#define BC250_VMID_ADMIT 0
#define BC250_VMID_REFUSE_BUSY 1            /* wait for a retirement, as before */
#define BC250_VMID_REFUSE_PARAM 2           /* not a VMID */
#define BC250_VMID_REFUSE_RULE 3            /* the invariant: a live VMID's root would change */
typedef struct BC250_VMID_DECISION {
    unsigned Vmid;                          /* the VMID the job runs at; valid with ADMIT and RULE */
    int Claim;                              /* the VMID is given Root (it holds another root, or none) */
    int Verdict;                            /* BC250_VMID_ADMIT or BC250_VMID_REFUSE_* */
    int Pool;                               /* the chooser decided, not the caller */
} BC250_VMID_DECISION;

static __inline BC250_VMID_DECISION Bc250VmidAdmit(const BC250_VMID_TABLE* Table, unsigned Members, int PoolGate,
                                                   unsigned Requested, unsigned long long Root, int InFlight,
                                                   unsigned LastVmid, unsigned long Observed)
{
    BC250_VMID_DECISION d;

    d.Pool = Requested == BC250_VMID_AUTO && PoolGate;
    d.Vmid = Requested == BC250_VMID_AUTO ? (PoolGate ? BC250_VMID_NONE : BC250_VMID_LEGACY) : Requested;
    d.Claim = 0;
    d.Verdict = BC250_VMID_ADMIT;
    if (!d.Pool) {
        if (d.Vmid >= BC250_VMID_COUNT) { d.Verdict = BC250_VMID_REFUSE_PARAM; return d; }
        /* 0.7.213.1, SubmitIbLocked: queue only jobs sharing the current VMID 1 root; VMID 0 stays exclusive. */
        if (InFlight && (d.Vmid != 1 || LastVmid != 1 || Table->Root[d.Vmid] != Root)) {
            d.Verdict = BC250_VMID_REFUSE_BUSY;
            return d;
        }
        d.Claim = d.Vmid != 0 && Table->Root[d.Vmid] != Root;
    } else {
        if (InFlight && LastVmid == 0) { d.Verdict = BC250_VMID_REFUSE_BUSY; return d; }
        d.Vmid = Bc250VmidChoose(Table, Members, Root, Observed, &d.Claim);
        if (d.Vmid == BC250_VMID_NONE) { d.Verdict = BC250_VMID_REFUSE_BUSY; return d; }
    }
    if (d.Vmid != 0 && !Bc250VmidMayProgram(Table, d.Vmid, Root, Observed)) d.Verdict = BC250_VMID_REFUSE_RULE;
    return d;
}

static __inline void Bc250VmidHistoryPush(BC250_VMID_HISTORY_RING* History, unsigned Vmid,
                                          const BC250_VMID_TABLE* Table)
{
    BC250_VMID_TENANT* e = &History->Entry[History->Next % BC250_VMID_HISTORY];
    e->Root = Table->Root[Vmid];
    e->Vmid = Vmid;
    e->Process = Table->Process[Vmid];
    e->FirstSeq = Table->FirstSeq[Vmid];
    e->LastSeq = Table->LastSeq[Vmid];
    History->Next++;
}

/* Vmid now holds Root, written to the hardware by the caller. The previous tenant, if it ran a job, goes into the
 * history first, so a fault latched on this VMID before anybody read the latch can still be put to its process. */
static __inline void Bc250VmidClaim(BC250_VMID_TABLE* Table, BC250_VMID_HISTORY_RING* History, unsigned Vmid,
                                    unsigned long long Root)
{
    if (Table->Root[Vmid] != 0 && Table->FirstSeq[Vmid] != 0) Bc250VmidHistoryPush(History, Vmid, Table);
    Table->Root[Vmid] = Root;
    Table->FirstSeq[Vmid] = 0;
    Table->LastSeq[Vmid] = 0;
    Table->Process[Vmid] = 0;
    Table->Order[Vmid] = ++Table->Clock;
}

/* A job of the current tenant reached the ring as Seq. */
static __inline void Bc250VmidSubmitted(BC250_VMID_TABLE* Table, unsigned Vmid, unsigned long Seq,
                                        unsigned long Process)
{
    Table->LiveSeq[Vmid] = Seq;
    Table->LastSeq[Vmid] = Seq;
    if (Table->FirstSeq[Vmid] == 0) Table->FirstSeq[Vmid] = Seq;
    Table->Process[Vmid] = Process;
    Table->Order[Vmid] = ++Table->Clock;
}

/* The engines are torn down or power-cycled: no root this instance programmed describes anything the next one will
 * submit. Every tenancy that ran a job goes to the history, then the table is empty. */
static __inline void Bc250VmidResetAll(BC250_VMID_TABLE* Table, BC250_VMID_HISTORY_RING* History)
{
    unsigned v;
    unsigned char* p = (unsigned char*)Table;
    unsigned long i;
    for (v = 0; v < BC250_VMID_COUNT; v++)
        if (Table->Root[v] != 0 && Table->FirstSeq[v] != 0) Bc250VmidHistoryPush(History, v, Table);
    for (i = 0; i < (unsigned long)sizeof(*Table); i++) p[i] = 0;
}

/* The VMIDs that hold Root (any candidate, not only pool members: the IB_AT escape may program another one). */
static __inline unsigned Bc250VmidHolders(const BC250_VMID_TABLE* Table, unsigned long long Root)
{
    unsigned v, mask = 0;
    if (Root == 0) return 0;
    for (v = 0; v < BC250_VMID_COUNT; v++)
        if ((BC250_VMID_CANDIDATES & (1u << v)) && Table->Root[v] == Root) mask |= 1u << v;
    return mask;
}

/* DXGK_OPERATION_FLUSH_TLB with the pool open. The OS asks for the TLB of one root; the VMIDs that hold that root
 * when the paging buffer is BUILT are not necessarily the ones that hold it when the SDMA engine EXECUTES it: in
 * between, the root's VMID can be recycled to another root and the root claimed again on a different VMID, whose
 * TLB could then hold a translation the buffer's own page-table writes change. So the buffer flushes a superset
 * that does not depend on timing: the holders of the root, every pool member and every other candidate VMID that
 * holds a root at all. That is at most fourteen invalidations per FLUSH_TLB; the one logged rate on file is 47863
 * FLUSH_TLB in 3909 s, about twelve a second (evidence/linux/2026-09-24-E29-sdma-reset/windows-before.log, wddm
 * summary). With nothing to flush, VMID 1, which is the single-VMID driver's record shape. */
static __inline unsigned Bc250VmidFlushMask(const BC250_VMID_TABLE* Table, unsigned Members, unsigned long long Root)
{
    unsigned v, mask = Bc250VmidHolders(Table, Root) | (Members & BC250_VMID_CANDIDATES);
    for (v = 0; v < BC250_VMID_COUNT; v++)
        if ((BC250_VMID_CANDIDATES & (1u << v)) && Table->Root[v] != 0) mask |= 1u << v;
    return mask ? mask : (1u << BC250_VMID_LEGACY);
}

/* Who ran at Vmid: the current tenant (*Live, from the table) and the newest earlier tenancy of the same VMID
 * (*Before, from the history). Each flag says whether its record exists. A fault latch names a VMID, not a job,
 * and the job that faulted may belong to either: a latch read late can describe a VMID that was since recycled. */
static __inline void Bc250VmidDescribe(const BC250_VMID_TABLE* Table, const BC250_VMID_HISTORY_RING* History,
                                       unsigned Vmid, BC250_VMID_TENANT* Live, int* HaveLive,
                                       BC250_VMID_TENANT* Before, int* HaveBefore)
{
    unsigned long n, kept;

    *HaveLive = 0;
    *HaveBefore = 0;
    if (Vmid >= BC250_VMID_COUNT) return;
    if (Table->Root[Vmid] != 0) {
        Live->Root = Table->Root[Vmid];
        Live->Vmid = Vmid;
        Live->Process = Table->Process[Vmid];
        Live->FirstSeq = Table->FirstSeq[Vmid];
        Live->LastSeq = Table->LastSeq[Vmid];
        *HaveLive = 1;
    }
    kept = History->Next < BC250_VMID_HISTORY ? History->Next : BC250_VMID_HISTORY;
    for (n = 0; n < kept; n++) {
        const BC250_VMID_TENANT* e = &History->Entry[(History->Next - 1u - n) % BC250_VMID_HISTORY];
        if (e->Vmid != Vmid) continue;
        *Before = *e;
        *HaveBefore = 1;
        return;
    }
}

#endif
