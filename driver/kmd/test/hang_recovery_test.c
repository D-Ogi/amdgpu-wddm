/* M15.12 host control: the pure decisions behind DxgkDdiResetEngine's stage-1 soft recovery (hang_recovery.h).
 * The wave kill (SQ_CMD) and the bounded fence poll touch hardware and are exercised on the lab behind the
 * HangRecoveryMode switch (docs/design/hang-recovery.md); this test fixes the gate, the 0x119 fence-range guard
 * (including 32-bit fence wrap, and its lower bound read per node with node 0 and node 1 fence ids interleaved, the
 * defect of lab trial D), the VMID guard the VMID pool made necessary, the pre-kill verdict and the sticky
 * record's counter arithmetic, so a refactor cannot quietly change when we attempt a recovery, what we report, or
 * how the lab reads the record. */
#include <stdio.h>
#include "hang_recovery.h"
#include "bc250_fence_order.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

/* The record's counters as guard.c GuardRecordHangRecovery keeps them: one write adds Bc250HangVerdictCounts. */
struct record { unsigned attempts, recovered, notDrained, refused, lastVerdict; };

static void record_write(struct record* r, unsigned verdict)
{
    unsigned counts = Bc250HangVerdictCounts(verdict);
    if (counts & BC250_HANG_COUNT_ATTEMPT) r->attempts++;
    if (counts & BC250_HANG_COUNT_RECOVERED) r->recovered++;
    if (counts & BC250_HANG_COUNT_NOT_DRAINED) r->notDrained++;
    if (counts & BC250_HANG_COUNT_REFUSED) r->refused++;
    r->lastVerdict = verdict;
}

/* What wddm.c writes for one ResetEngine call: the pre-kill verdict, and if that is PENDING, the pending record
 * and then the kill's verdict; otherwise the refusal alone. */
static void record_call(struct record* r, unsigned preKill, unsigned killVerdict)
{
    if (preKill == BC250_HANG_VERDICT_PENDING) {
        record_write(r, BC250_HANG_VERDICT_PENDING);
        record_write(r, killVerdict);
    } else {
        record_write(r, preKill);
    }
}

static int balanced(const struct record* r)
{
    return r->attempts == r->recovered + r->notDrained + r->refused;
}

/* The fence state of wddm.c's BC250_WDDM that ResetEngine reads, with the same names and types (volatile LONG
 * and BOOLEAN there): one pair per node, and the adapter-wide LastCompletedFence that every delivered completion
 * report also writes (WddmReportDpcPublish). Fence ids are per node; node 1 (paging) runs far ahead of node 0. */
#define MODEL_NODES 2u
struct fences {
    volatile long LastReportedFence[MODEL_NODES];
    unsigned char LastReportedValid[MODEL_NODES];
    volatile long LastCompletedFence;
};

/* One delivered DMA_COMPLETED report, written as the report DPC writes it. */
static void model_report(struct fences* f, unsigned node, long fence)
{
    f->LastCompletedFence = fence;
    f->LastReportedFence[node] = fence;
    f->LastReportedValid[node] = 1;
}

/* ResetEngine's read site and pre-kill decision, as in wddm.c: the lower bound of the 0x119 range for the reset
 * node. run_hang_recovery.ps1 -SharedLastCompleted replaces the marked line with the 0.7.216.13 read of the
 * adapter-wide field, and the two-node checks below must then fail. */
static unsigned model_pre_kill(const struct fences* f, unsigned node, unsigned hungFence, unsigned vmid)
{
    unsigned lastCompleted = 0;
    int known;
    known = Bc250HangNodeLastCompleted(f->LastReportedFence, f->LastReportedValid, MODEL_NODES, node, &lastCompleted); /* READ SITE */
    return Bc250HangPreKillVerdict(1, hungFence, known, lastCompleted, vmid);
}

int main(void)
{
    struct record r = { 0, 0, 0, 0, 0 };
    unsigned vmid;

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

    /* The VMID guard (0.7.216.13, after the VMID pool of 0.7.214): a broadcast CHECK_VMID kill may name only a
     * VMID the pool can give an application. VMID 0 is the GART/system domain, VMID 2 is SDMA paging's, and a
     * recorded 0 also means "this queue entry named no VMID". Every member of BC250_VMID_CANDIDATES is allowed. */
    CHECK(!Bc250KillVmidValid(BC250_VMID_GART));            /* 0: never */
    CHECK(!Bc250KillVmidValid(BC250_VMID_SDMA_PAGING));     /* 2: node 1's own VMID, never */
    CHECK(Bc250KillVmidValid(BC250_VMID_LEGACY));           /* 1: the pre-pool application VMID */
    CHECK(Bc250KillVmidValid(BC250_VMID_COUNT - 1u));       /* 15: the last pool member */
    CHECK(!Bc250KillVmidValid(BC250_VMID_COUNT));           /* 16: not a VMID of this part */
    CHECK(!Bc250KillVmidValid(BC250_VMID_NONE));            /* the pool's "no VMID" marker */
    for (vmid = 0; vmid < BC250_VMID_COUNT; vmid++)
        CHECK(Bc250KillVmidValid(vmid) == (int)(((unsigned)BC250_VMID_CANDIDATES >> vmid) & 1u));

    /* Pre-kill verdict: nothing on the ring wins over everything; then the fence guard, then the VMID guard;
     * only then a kill. VMID 1 below is a valid pool VMID, so it is never what refuses these cases. */
    CHECK(Bc250HangPreKillVerdict(0, 105, 1, 100, 1) == BC250_HANG_VERDICT_NOTHING_ON_RING);
    CHECK(Bc250HangPreKillVerdict(0, 99, 1, 100, 1) == BC250_HANG_VERDICT_NOTHING_ON_RING);
    CHECK(Bc250HangPreKillVerdict(0, 105, 1, 100, 0) == BC250_HANG_VERDICT_NOTHING_ON_RING);  /* an empty ring first */
    CHECK(Bc250HangPreKillVerdict(1, 105, 1, 100, 1) == BC250_HANG_VERDICT_PENDING);    /* the hang: head above completed */
    CHECK(Bc250HangPreKillVerdict(1, 100, 1, 100, 1) == BC250_HANG_VERDICT_PENDING);    /* at the bound is still valid */
    CHECK(Bc250HangPreKillVerdict(1, 99, 1, 100, 1) == BC250_HANG_VERDICT_FENCE_GUARD); /* would be 0x119: no kill */
    CHECK(Bc250HangPreKillVerdict(1, 99, 1, 100, 0) == BC250_HANG_VERDICT_FENCE_GUARD); /* the fence guard goes first */
    CHECK(Bc250HangPreKillVerdict(1, 105, 1, 100, 0) == BC250_HANG_VERDICT_VMID_GUARD); /* no VMID recorded: no kill */
    CHECK(Bc250HangPreKillVerdict(1, 105, 1, 100, 2) == BC250_HANG_VERDICT_VMID_GUARD); /* SDMA paging's: no kill */
    CHECK(Bc250HangPreKillVerdict(1, 105, 1, 100, 7) == BC250_HANG_VERDICT_PENDING);    /* a pool VMID of 3..15 */
    CHECK(Bc250HangPreKillVerdict(1, 0x00000002u, 1, 0xFFFFFFFEu, 1) == BC250_HANG_VERDICT_PENDING);     /* across wrap */
    CHECK(Bc250HangPreKillVerdict(1, 0xFFFFFFFDu, 1, 0xFFFFFFFEu, 1) == BC250_HANG_VERDICT_FENCE_GUARD);

    /* No reported fence on the node: the range cannot be proven, so the fence guard refuses before any kill. */
    CHECK(Bc250HangPreKillVerdict(1, 105, 0, 0, 1) == BC250_HANG_VERDICT_FENCE_GUARD);
    CHECK(Bc250HangPreKillVerdict(0, 105, 0, 0, 1) == BC250_HANG_VERDICT_NOTHING_ON_RING);  /* an empty ring first */

    /* Two nodes, interleaved fence ids (0.7.216.16, lab trial D of 2026-10-07). Node 0 reports 1180..1185 and
     * hangs at 1186; node 1's paging fences run around 9220 and keep completing after the hang, the last report
     * being node 1's 9228 - exactly the dump of trial D. The guard must compare 1186 with node 0's 1185. With the
     * adapter-wide field (9228) it refused a valid recovery with verdict 4 and the machine went to 0x116. */
    {
        struct fences f = { { 0, 0 }, { 0, 0 }, 0 };
        unsigned last = 0;
        long i;

        for (i = 0; i < 6; i++) {
            model_report(&f, 0, 1180 + i);
            model_report(&f, 1, 9220 + i);
        }
        model_report(&f, 1, 9226);      /* paging work completes after node 0 hung at 1186 */
        model_report(&f, 1, 9227);
        model_report(&f, 1, 9228);
        CHECK(f.LastReportedFence[0] == 1185 && f.LastReportedFence[1] == 9228 && f.LastCompletedFence == 9228);
        CHECK(Bc250HangNodeLastCompleted(f.LastReportedFence, f.LastReportedValid, MODEL_NODES, 0, &last));
        CHECK(last == 1185u);
        CHECK(Bc250HangNodeLastCompleted(f.LastReportedFence, f.LastReportedValid, MODEL_NODES, 1, &last));
        CHECK(last == 9228u);
        CHECK(!Bc250HangNodeLastCompleted(f.LastReportedFence, f.LastReportedValid, MODEL_NODES, 2, &last));
        /* The trial-D case: node 0, hung fence 1186, VMID 12 - a kill goes ahead. */
        CHECK(model_pre_kill(&f, 0, 1186, 12) == BC250_HANG_VERDICT_PENDING);
        /* Node 0's own bound still guards: a fence below node 0's last report is refused. */
        CHECK(model_pre_kill(&f, 0, 1184, 12) == BC250_HANG_VERDICT_FENCE_GUARD);
        /* The other order: node 0 reported last and node 1 is still far ahead. The decision must not depend on
         * which node happened to report last. */
        model_report(&f, 0, 1186);
        CHECK(model_pre_kill(&f, 0, 1187, 12) == BC250_HANG_VERDICT_PENDING);
        model_report(&f, 1, 9229);
        CHECK(model_pre_kill(&f, 0, 1187, 12) == BC250_HANG_VERDICT_PENDING);
        /* Node 1 behind node 0 (ids wrap independently): node 0's guard still reads node 0. */
        model_report(&f, 1, 5);
        CHECK(model_pre_kill(&f, 0, 1187, 12) == BC250_HANG_VERDICT_PENDING);
        CHECK(model_pre_kill(&f, 0, 1185, 12) == BC250_HANG_VERDICT_FENCE_GUARD);
    }
    /* A node that has reported nothing yet has no proven lower bound: refused, even though node 1 has one. */
    {
        struct fences f = { { 0, 0 }, { 0, 0 }, 0 };
        unsigned last = 0;

        CHECK(!Bc250HangNodeLastCompleted(f.LastReportedFence, f.LastReportedValid, MODEL_NODES, 0, &last));
        model_report(&f, 1, 40);
        CHECK(model_pre_kill(&f, 0, 41, 12) == BC250_HANG_VERDICT_FENCE_GUARD);
    }

    /* Only DRAINED and ALREADY_RETIRED report a reset; every other verdict keeps today's refusal. */
    CHECK(Bc250HangVerdictRecovered(BC250_HANG_VERDICT_DRAINED));
    CHECK(Bc250HangVerdictRecovered(BC250_HANG_VERDICT_ALREADY_RETIRED));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_PENDING));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_NOT_DRAINED));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_NOTHING_ON_RING));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_FENCE_GUARD));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_VMID_GUARD));
    CHECK(!Bc250HangVerdictRecovered(7u));

    /* The values the LAB-PLAN tells the operator to read must not move. */
    CHECK(BC250_HANG_VERDICT_PENDING == 0u && BC250_HANG_VERDICT_DRAINED == 1u && BC250_HANG_VERDICT_NOT_DRAINED == 2u);
    CHECK(BC250_HANG_VERDICT_NOTHING_ON_RING == 3u && BC250_HANG_VERDICT_FENCE_GUARD == 4u);
    CHECK(BC250_HANG_VERDICT_ALREADY_RETIRED == 5u && BC250_HANG_VERDICT_VMID_GUARD == 6u);

    /* The record: every finished call leaves Attempts == Recovered + NotDrained + Refused. */
    record_call(&r, BC250_HANG_VERDICT_PENDING, BC250_HANG_VERDICT_DRAINED);
    CHECK(balanced(&r) && r.attempts == 1 && r.recovered == 1 && r.lastVerdict == BC250_HANG_VERDICT_DRAINED);
    record_call(&r, BC250_HANG_VERDICT_PENDING, BC250_HANG_VERDICT_ALREADY_RETIRED);
    CHECK(balanced(&r) && r.attempts == 2 && r.recovered == 2);
    record_call(&r, BC250_HANG_VERDICT_PENDING, BC250_HANG_VERDICT_NOT_DRAINED);
    CHECK(balanced(&r) && r.attempts == 3 && r.notDrained == 1 && r.lastVerdict == BC250_HANG_VERDICT_NOT_DRAINED);
    record_call(&r, BC250_HANG_VERDICT_NOTHING_ON_RING, 0);
    CHECK(balanced(&r) && r.attempts == 4 && r.refused == 1 && r.lastVerdict == BC250_HANG_VERDICT_NOTHING_ON_RING);
    record_call(&r, BC250_HANG_VERDICT_FENCE_GUARD, 0);
    CHECK(balanced(&r) && r.attempts == 5 && r.refused == 2 && r.lastVerdict == BC250_HANG_VERDICT_FENCE_GUARD);
    record_call(&r, BC250_HANG_VERDICT_VMID_GUARD, 0);
    CHECK(balanced(&r) && r.attempts == 6 && r.refused == 3 && r.lastVerdict == BC250_HANG_VERDICT_VMID_GUARD);
    /* A call the machine did not survive: only the pending write landed. Attempts runs one ahead, LastVerdict 0 -
     * exactly what the LAB-PLAN reads as "the kill itself took the machine down". */
    record_write(&r, BC250_HANG_VERDICT_PENDING);
    CHECK(!balanced(&r) && r.attempts == r.recovered + r.notDrained + r.refused + 1 && r.lastVerdict == 0u);

    printf("PASS: soft-recovery gate (switch/node), the 0x119 fence-range guard including 32-bit wrap and its "
           "per-node lower bound with interleaved node 0/node 1 fences, the VMID guard, the pre-kill verdict and the "
           "sticky record's counters\n");
    return 0;
}
