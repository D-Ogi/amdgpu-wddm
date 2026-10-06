/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Host test of driver/kmd/vmid_pool.h, the VMID pool of the graphics submit path.
 *
 * The property everything else rests on: while a job is in flight, the VMID it runs at holds the root it was
 * submitted with. A VMID whose root changed under a live job walks another process's page tables, which on this
 * part is a 0x116 and no GPU reset. The model below runs the chooser and the bookkeeping exactly as gfx.c's
 * SubmitIbLocked does (sweep, admit, claim, submitted) against an in-order fence, and checks that property after
 * every step: exhaustively over every interleaving of submits and retirements up to a depth, and over seeded
 * random runs with the sequence counter crossing its wrap. Then the parts one at a time: the bring-up read, reuse,
 * least recently used among retired VMIDs, BUSY when none is free, the FLUSH_TLB fan-out, the history, and the
 * EnableVmidPool 0 decision against 0.7.213.1's predicate, case by case. */
#include <stdio.h>
#include <string.h>
#include "vmid_pool.h"

static unsigned checks, bad;
#define CHECK(x) do { ++checks; if (!(x)) { ++bad; printf("FAIL line %u: %s\n", __LINE__, #x); } } while (0)

/* ---- the model --------------------------------------------------------------------------------------------- */

#define MODEL_PENDING_MAX 7u                /* BC250_GFX_PENDING_MAX: WddmSubmitHardware's queue bound */
#define MODEL_DEPTH_MAX 64u

typedef struct {
    unsigned long Seq;
    unsigned Vmid;
    unsigned long long Root;
} MODEL_JOB;

typedef struct {
    BC250_VMID_TABLE Table;
    BC250_VMID_HISTORY_RING History;
    unsigned Members;
    unsigned long NextSeq;                  /* the last sequence handed out; the next is +1, skipping 0 */
    unsigned long Completed;                /* the fence slot: the newest retired sequence, 0 before any */
    MODEL_JOB Pending[MODEL_DEPTH_MAX];
    unsigned PendingCount;
    unsigned LastVmid;
    unsigned long Claims, Reuses, Busy;
} MODEL;

static void ModelInit(MODEL* m, unsigned members, unsigned long firstSeq)
{
    memset(m, 0, sizeof(*m));
    m->Members = members;
    m->NextSeq = firstSeq - 1u;
    m->Completed = 0;
}

/* The safety property: every job in flight still has its root at its VMID. */
static int ModelSafe(const MODEL* m)
{
    unsigned i;
    for (i = 0; i < m->PendingCount; i++)
        if (m->Table.Root[m->Pending[i].Vmid] != m->Pending[i].Root) return 0;
    return 1;
}

/* One submission at AUTO with the pool open, as SubmitIbLocked runs it. Returns the verdict. */
static int ModelSubmit(MODEL* m, unsigned long long root, unsigned* vmidOut)
{
    BC250_VMID_DECISION d;
    unsigned i;

    if (m->PendingCount >= MODEL_PENDING_MAX) return -1;    /* refused above gfx.c; not a VMID question */
    Bc250VmidSweep(&m->Table, m->Completed);
    d = Bc250VmidAdmit(&m->Table, m->Members, 1, BC250_VMID_AUTO, root, m->PendingCount != 0, m->LastVmid,
                       m->Completed);
    CHECK(d.Verdict != BC250_VMID_REFUSE_RULE);            /* the chooser never proposes a live VMID */
    CHECK(d.Verdict != BC250_VMID_REFUSE_PARAM);
    if (d.Verdict == BC250_VMID_REFUSE_BUSY) {
        /* BUSY only when it has to be: no member holds the root, and every member has a job in flight. */
        unsigned v;
        m->Busy++;
        CHECK(m->PendingCount != 0);
        for (v = 0; v < BC250_VMID_COUNT; v++) {
            int live = 0;
            if (!(m->Members & BC250_VMID_CANDIDATES & (1u << v))) continue;
            CHECK(m->Table.Root[v] != root);
            for (i = 0; i < m->PendingCount; i++) if (m->Pending[i].Vmid == v) live = 1;
            CHECK(live);
        }
        return d.Verdict;
    }
    CHECK(d.Vmid < BC250_VMID_COUNT && (m->Members & BC250_VMID_CANDIDATES & (1u << d.Vmid)));
    CHECK(d.Vmid != BC250_VMID_GART && d.Vmid != BC250_VMID_SDMA_PAGING);
    if (d.Claim) {
        /* Independent of the table's own LiveSeq: the model's queue says no job is live at this VMID. */
        for (i = 0; i < m->PendingCount; i++) CHECK(m->Pending[i].Vmid != d.Vmid || m->Pending[i].Root == root);
        Bc250VmidClaim(&m->Table, &m->History, d.Vmid, root);
        m->Claims++;
    } else {
        CHECK(m->Table.Root[d.Vmid] == root);
        m->Reuses++;
    }
    if (++m->NextSeq == 0) m->NextSeq = 1;                  /* SubmitIbLocked: 0 means nothing in flight */
    Bc250VmidSubmitted(&m->Table, d.Vmid, m->NextSeq, (unsigned long)(root & 0xFFFFu));
    m->Pending[m->PendingCount].Seq = m->NextSeq;
    m->Pending[m->PendingCount].Vmid = d.Vmid;
    m->Pending[m->PendingCount].Root = root;
    m->PendingCount++;
    m->LastVmid = d.Vmid;
    if (vmidOut) *vmidOut = d.Vmid;
    return d.Verdict;
}

/* The oldest job's fence arrives: in order, as the gfx ring retires them. */
static int ModelRetire(MODEL* m)
{
    if (m->PendingCount == 0) return 0;
    m->Completed = m->Pending[0].Seq;
    memmove(&m->Pending[0], &m->Pending[1], (m->PendingCount - 1u) * sizeof(m->Pending[0]));
    m->PendingCount--;
    return 1;
}

/* ---- exhaustive interleavings -------------------------------------------------------------------------------- */

static unsigned long g_leaves;
static const unsigned long long g_roots[] = { 0x1000ull, 0x2000ull, 0x3000ull, 0x4000ull };

static void Explore(const MODEL* m, unsigned depth, unsigned roots)
{
    unsigned op;
    if (depth == 0) { g_leaves++; return; }
    for (op = 0; op <= roots; op++) {
        MODEL next = *m;
        if (op == roots) { if (!ModelRetire(&next)) continue; }
        else if (ModelSubmit(&next, g_roots[op], NULL) < 0) continue;
        CHECK(ModelSafe(&next));
        if (bad > 20) return;
        Explore(&next, depth - 1u, roots);
    }
}

static void TestExhaustive(void)
{
    static const struct { unsigned Members, Roots, Depth; } cases[] = {
        { (1u << 1), 3, 9 },                        /* a pool of one: the old single-VMID shape */
        { (1u << 1) | (1u << 3), 3, 10 },
        { (1u << 1) | (1u << 3) | (1u << 4), 4, 9 },
        { (1u << 1) | (1u << 15), 2, 12 },
    };
    unsigned i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        MODEL m;
        unsigned long before = checks;
        ModelInit(&m, cases[i].Members, 1);
        g_leaves = 0;
        Explore(&m, cases[i].Depth, cases[i].Roots);
        printf("exhaustive: pool 0x%04X, %u roots, depth %u: %lu interleavings, %lu checks\n", cases[i].Members,
               cases[i].Roots, cases[i].Depth, g_leaves, checks - before);
        CHECK(g_leaves > 1000);
    }
}

/* ---- seeded random runs ------------------------------------------------------------------------------------ */

static unsigned long g_rng;
static unsigned Rand(unsigned n)
{
    g_rng = g_rng * 1103515245ul + 12345ul;
    return (unsigned)((g_rng >> 8) % n);
}

static void TestRandom(void)
{
    static const unsigned pools[] = { BC250_VMID_CANDIDATES, (1u << 1) | (1u << 3), (1u << 1) | (1u << 3) | (1u << 9) | (1u << 10),
                                      (1u << 1) };
    unsigned p, seed;
    unsigned long steps = 0, claims = 0, reuses = 0, busy = 0;

    for (p = 0; p < sizeof(pools) / sizeof(pools[0]); p++)
        for (seed = 1; seed <= 8; seed++) {
            MODEL m;
            unsigned long i;
            /* Start 1000 short of the wrap: the sequence crosses 0xFFFFFFFF -> 1 inside every run. */
            ModelInit(&m, pools[p], 0xFFFFFFFFul - 1000ul);
            g_rng = seed * 2654435761ul + p;
            for (i = 0; i < 40000ul; i++) {
                unsigned roots = 2u + Rand(16);
                if (Rand(3) == 0) (void)ModelRetire(&m);
                else (void)ModelSubmit(&m, 0x10000ull * (1u + Rand(roots)), NULL);
                CHECK(ModelSafe(&m));
                if (bad > 20) return;
            }
            steps += i; claims += m.Claims; reuses += m.Reuses; busy += m.Busy;
            /* Drain: with nothing in flight every root must be admitted at once. */
            while (ModelRetire(&m)) {}
            CHECK(ModelSubmit(&m, 0x77770000ull, NULL) == BC250_VMID_ADMIT);
        }
    printf("random: %lu steps, %lu claims, %lu reuses, %lu busy\n", steps, claims, reuses, busy);
    CHECK(claims > 0 && reuses > 0 && busy > 0);
}

/* ---- the parts ----------------------------------------------------------------------------------------------- */

static void TestProbe(void)
{
    unsigned long long value[BC250_VMID_COUNT];
    unsigned excluded = 0xDEAD, members;

    memset(value, 0, sizeof(value));
    members = Bc250VmidPoolFromProbe(value, &excluded);
    CHECK(members == BC250_VMID_CANDIDATES && members == 0xFFFAu && excluded == 0);
    CHECK(Bc250VmidCount(members) == 14);
    /* The GART aperture and SDMA paging always read non-zero once they run; neither is ever a member. */
    value[0] = 0x123001ull; value[2] = 0x456001ull;
    members = Bc250VmidPoolFromProbe(value, &excluded);
    CHECK(members == 0xFFFAu && excluded == 0);
    /* VMID 1 holding a root (an earlier start of this driver) stays in; 5 and 15 holding one stay out. */
    value[1] = 0x789001ull; value[5] = 0xABC001ull; value[15] = 1ull;
    members = Bc250VmidPoolFromProbe(value, &excluded);
    CHECK((members & (1u << 1)) && !(members & (1u << 5)) && !(members & (1u << 15)));
    CHECK(excluded == ((1u << 5) | (1u << 15)) && Bc250VmidCount(members) == 12);
    /* Everything 3..15 in use: the pool is VMID 1 alone, which is the single-VMID driver. */
    { unsigned v; for (v = 3; v < 16; v++) value[v] = 0x1000ull * v; }
    members = Bc250VmidPoolFromProbe(value, &excluded);
    CHECK(members == (1u << 1) && Bc250VmidCount(excluded) == 13);
}

static void TestChooser(void)
{
    MODEL m;
    unsigned v1 = 99, v2 = 99, v3 = 99, v4 = 99;

    ModelInit(&m, BC250_VMID_CANDIDATES, 1);
    /* The first root lands on VMID 1: a one-process system runs exactly where the old driver ran. */
    CHECK(ModelSubmit(&m, 0xA000, &v1) == BC250_VMID_ADMIT && v1 == 1);
    /* A second root while the first is in flight: its own VMID, no waiting. This is the whole point. */
    CHECK(ModelSubmit(&m, 0xB000, &v2) == BC250_VMID_ADMIT && v2 == 3);
    /* Same root again with its job live: reuse, no claim. */
    CHECK(ModelSubmit(&m, 0xA000, &v3) == BC250_VMID_ADMIT && v3 == 1 && m.Claims == 2 && m.Reuses == 1);
    CHECK(m.PendingCount == 3 && ModelSafe(&m));
    /* Least recently used among retired: retire everything, use A (VMID 1) once more, then a new root C must take
     * an unused VMID (4) first, then D the oldest used one, which is B's VMID 3 - not A's VMID 1, used later. */
    while (ModelRetire(&m)) {}
    CHECK(ModelSubmit(&m, 0xA000, &v3) == BC250_VMID_ADMIT && v3 == 1);
    CHECK(ModelSubmit(&m, 0xC000, &v4) == BC250_VMID_ADMIT && v4 == 4);
    {
        MODEL n = m;
        unsigned v;
        for (v = 5; v < 16; v++) {
            unsigned got = 0;
            while (ModelRetire(&n)) {}          /* the queue holds seven; unused VMIDs still go first */
            CHECK(ModelSubmit(&n, 0x100000ull * v, &got) == 0 && got == v);
        }
        while (ModelRetire(&n)) {}
        CHECK(ModelSubmit(&n, 0xA000, &v) == 0 && v == 1);          /* still resident: reuse */
        CHECK(ModelSubmit(&n, 0xD000, &v) == 0 && v == 3);          /* B's VMID: the least recently used */
        CHECK(n.History.Next == 1 && n.History.Entry[0].Vmid == 3 && n.History.Entry[0].Root == 0xB000);
    }
    /* BUSY when no member is free: a pool of two, both with live jobs, a third root. */
    ModelInit(&m, (1u << 1) | (1u << 3), 1);
    CHECK(ModelSubmit(&m, 0xA000, NULL) == 0 && ModelSubmit(&m, 0xB000, NULL) == 0);
    CHECK(ModelSubmit(&m, 0xC000, NULL) == BC250_VMID_REFUSE_BUSY && m.Busy == 1);
    CHECK(ModelSubmit(&m, 0xB000, &v1) == 0 && v1 == 3);            /* a resident root is never BUSY */
    CHECK(ModelRetire(&m));                                         /* A's job retires: VMID 1 frees */
    CHECK(ModelSubmit(&m, 0xC000, &v1) == 0 && v1 == 1 && ModelSafe(&m));
    /* A root of 0 never matches an unused VMID's empty slot. */
    {
        BC250_VMID_TABLE t;
        int claim = 7;
        memset(&t, 0, sizeof(t));
        CHECK(Bc250VmidChoose(&t, BC250_VMID_CANDIDATES, 0, 0, &claim) == 1 && claim == 1);
        /* Reserved VMIDs are masked off whatever the caller passes. */
        CHECK(Bc250VmidChoose(&t, (1u << 0) | (1u << 2), 0x1000, 0, &claim) == BC250_VMID_NONE && claim == 0);
    }
}

static void TestRule(void)
{
    BC250_VMID_TABLE t;
    BC250_VMID_DECISION d;

    memset(&t, 0, sizeof(t));
    t.Root[3] = 0xB000; t.LiveSeq[3] = 50;
    /* VMID 3 holds B with job 50 live, fence at 49. Giving it another root is the bug. */
    CHECK(!Bc250VmidMayProgram(&t, 3, 0xC000, 49));
    CHECK(Bc250VmidMayProgram(&t, 3, 0xB000, 49));                  /* same root: nothing changes */
    CHECK(Bc250VmidMayProgram(&t, 3, 0xC000, 50));                  /* retired */
    CHECK(Bc250VmidMayProgram(&t, 3, 0xC000, 0x80000040ul) == 0);   /* more than half a wrap ahead reads as behind */
    CHECK(!Bc250VmidMayProgram(&t, 16, 0xB000, 50));
    /* An explicit request (the IB_AT escape) for that VMID with another root, with nothing in flight according to
     * the caller: the predicate admits it, the rule does not. */
    d = Bc250VmidAdmit(&t, BC250_VMID_CANDIDATES, 1, 3, 0xC000, 0, 3, 49);
    CHECK(d.Verdict == BC250_VMID_REFUSE_RULE && d.Vmid == 3 && d.Claim == 1 && !d.Pool);
    /* The sweep clears what the fence passed and nothing else, across the wrap as well. */
    t.LiveSeq[4] = 0xFFFFFFF0ul; t.LiveSeq[5] = 5;
    Bc250VmidSweep(&t, 2);
    CHECK(t.LiveSeq[4] == 0 && t.LiveSeq[5] == 5 && t.LiveSeq[3] == 50);
    Bc250VmidSweep(&t, 0);                                          /* no fence yet: nothing has retired */
    CHECK(t.LiveSeq[5] == 5);
    /* With the pool, a VMID 0 diagnostic in flight still holds every job off. */
    memset(&t, 0, sizeof(t));
    d = Bc250VmidAdmit(&t, BC250_VMID_CANDIDATES, 1, BC250_VMID_AUTO, 0xA000, 1, 0, 0);
    CHECK(d.Verdict == BC250_VMID_REFUSE_BUSY);
    d = Bc250VmidAdmit(&t, BC250_VMID_CANDIDATES, 1, 17, 0xA000, 0, 1, 0);
    CHECK(d.Verdict == BC250_VMID_REFUSE_PARAM);
}

/* EnableVmidPool 0: every decision is 0.7.213.1's, and AUTO is VMID 1. */
static void TestGateClosedIdentity(void)
{
    static const unsigned requested[] = { 0, 1, 3, 15, BC250_VMID_AUTO };
    static const unsigned last[] = { 0, 1, 3 };
    static const unsigned long long roots[] = { 0xA000, 0xB000 };
    unsigned r, f, l, k, h, cases = 0;

    for (r = 0; r < 5; r++) for (f = 0; f < 2; f++) for (l = 0; l < 3; l++) for (k = 0; k < 2; k++)
    for (h = 0; h < 2; h++) {
        BC250_VMID_TABLE t;
        BC250_VMID_DECISION d;
        unsigned vmid = requested[r] == BC250_VMID_AUTO ? 1u : requested[r];
        int oldBusy;
        memset(&t, 0, sizeof(t));
        if (h) { t.Root[1] = roots[0]; t.Root[3] = roots[0]; t.Root[15] = roots[0]; }
        /* A consistent state: when nothing is in flight, nothing is live. In flight, the newest job is live at
         * LastVmid. */
        if (f && last[l] != 0) t.LiveSeq[last[l]] = 9;
        d = Bc250VmidAdmit(&t, BC250_VMID_CANDIDATES, 0, requested[r], roots[k], f, last[l], f ? 8 : 9);
        oldBusy = f && (vmid != 1 || last[l] != 1 || t.Root[vmid] != roots[k]);
        CHECK(!d.Pool && d.Vmid == vmid);
        CHECK(d.Verdict == (oldBusy ? BC250_VMID_REFUSE_BUSY : BC250_VMID_ADMIT));
        if (!oldBusy) CHECK(d.Claim == (vmid != 0 && t.Root[vmid] != roots[k]));
        cases++;
    }
    CHECK(cases == 120);
    /* And the same requests with the gate open: an explicit VMID still takes the old predicate. */
    for (r = 0; r < 4; r++) for (f = 0; f < 2; f++) for (l = 0; l < 3; l++) {
        BC250_VMID_TABLE t;
        BC250_VMID_DECISION d;
        int oldBusy = f && (requested[r] != 1 || last[l] != 1 || 0 != 0xA000);
        memset(&t, 0, sizeof(t));
        d = Bc250VmidAdmit(&t, BC250_VMID_CANDIDATES, 1, requested[r], 0xA000, f, last[l], 9);
        CHECK(!d.Pool && d.Vmid == requested[r] && d.Verdict == (oldBusy ? BC250_VMID_REFUSE_BUSY : BC250_VMID_ADMIT));
    }
}

static void TestFlushMask(void)
{
    BC250_VMID_TABLE t;
    unsigned mask;

    memset(&t, 0, sizeof(t));
    /* Nothing programmed and no pool yet (no submit has run): VMID 1, the single-VMID record. */
    CHECK(Bc250VmidFlushMask(&t, 0, 0xA000) == (1u << 1));
    /* A root on two VMIDs (an IB_AT diagnostic put it on 7 as well): both holders are flushed. */
    t.Root[3] = 0xA000; t.Root[7] = 0xA000; t.Root[4] = 0xB000;
    CHECK(Bc250VmidHolders(&t, 0xA000) == ((1u << 3) | (1u << 7)));
    CHECK(Bc250VmidHolders(&t, 0) == 0);
    mask = Bc250VmidFlushMask(&t, (1u << 1) | (1u << 3) | (1u << 4), 0xA000);
    CHECK((mask & ((1u << 3) | (1u << 7))) == ((1u << 3) | (1u << 7)));
    /* The superset: every member, and every candidate that holds any root, whatever the requested root. */
    CHECK(mask == ((1u << 1) | (1u << 3) | (1u << 4) | (1u << 7)));
    CHECK(Bc250VmidFlushMask(&t, (1u << 1) | (1u << 3) | (1u << 4), 0) == mask);
    /* Never the reserved two, even if the caller's pool or a stray table entry names them. */
    t.Root[2] = 0xA000; t.Root[0] = 0xA000;
    mask = Bc250VmidFlushMask(&t, 0xFFFFu, 0xA000);
    CHECK(!(mask & BC250_VMID_RESERVED) && mask == BC250_VMID_CANDIDATES);
    /* The race this superset exists for: A on VMID 3 when the buffer is built; before it executes, VMID 3 is
     * recycled to B and A is claimed on VMID 5. The built mask must already contain 5. */
    {
        MODEL m;
        unsigned built, heldAtBuild, va = 0, vb = 0, vlater = 0, v;
        ModelInit(&m, (1u << 1) | (1u << 3) | (1u << 5), 1);
        CHECK(ModelSubmit(&m, 0xC000, &v) == 0 && v == 1);
        CHECK(ModelSubmit(&m, 0xA000, &va) == 0 && va == 3);
        while (ModelRetire(&m)) {}
        CHECK(ModelSubmit(&m, 0xC000, &v) == 0 && v == 1);          /* C stays the most recently used */
        built = Bc250VmidFlushMask(&m.Table, m.Members, 0xA000);   /* FLUSH_TLB for A built here */
        heldAtBuild = Bc250VmidHolders(&m.Table, 0xA000);
        CHECK(ModelSubmit(&m, 0xD000, &vb) == 0 && vb == 5);
        while (ModelRetire(&m)) {}
        CHECK(ModelSubmit(&m, 0xC000, &v) == 0);
        CHECK(ModelSubmit(&m, 0xD000, &v) == 0);
        CHECK(ModelSubmit(&m, 0xB000, &vb) == 0 && vb == 3);        /* A's VMID recycled */
        while (ModelRetire(&m)) {}
        CHECK(ModelSubmit(&m, 0xA000, &vlater) == 0 && vlater != 3);
        CHECK(heldAtBuild == (1u << 3) && !(heldAtBuild & (1u << vlater)));    /* holders alone would miss it */
        CHECK(built & (1u << vlater));
    }
}

static void TestHistory(void)
{
    BC250_VMID_TABLE t;
    BC250_VMID_HISTORY_RING h;
    BC250_VMID_TENANT live, before;
    int haveLive, haveBefore;
    unsigned i;

    memset(&t, 0, sizeof(t));
    memset(&h, 0, sizeof(h));
    Bc250VmidDescribe(&t, &h, 3, &live, &haveLive, &before, &haveBefore);
    CHECK(!haveLive && !haveBefore);
    Bc250VmidClaim(&t, &h, 3, 0xA000);
    CHECK(h.Next == 0);                                             /* an empty VMID has no tenant to record */
    Bc250VmidSubmitted(&t, 3, 100, 4242);
    Bc250VmidSubmitted(&t, 3, 105, 4242);
    Bc250VmidClaim(&t, &h, 3, 0xB000);                              /* the game's VMID goes to DWM */
    Bc250VmidSubmitted(&t, 3, 106, 777);
    Bc250VmidDescribe(&t, &h, 3, &live, &haveLive, &before, &haveBefore);
    CHECK(haveLive && live.Root == 0xB000 && live.Process == 777 && live.FirstSeq == 106 && live.LastSeq == 106);
    CHECK(haveBefore && before.Root == 0xA000 && before.Process == 4242 && before.FirstSeq == 100 &&
          before.LastSeq == 105 && before.Vmid == 3);
    /* A claimed VMID that never ran a job leaves no history behind when it is claimed again. */
    Bc250VmidClaim(&t, &h, 9, 0xC000);
    Bc250VmidClaim(&t, &h, 9, 0xD000);
    CHECK(h.Next == 1);
    /* The ring keeps the newest sixteen; Describe finds the newest entry of the VMID asked for. */
    for (i = 0; i < 40; i++) {
        Bc250VmidSubmitted(&t, 4, 200 + i, i);
        Bc250VmidClaim(&t, &h, 4, 0x10000ull + i);
    }
    Bc250VmidDescribe(&t, &h, 4, &live, &haveLive, &before, &haveBefore);
    CHECK(haveBefore && before.Process == 39 && before.LastSeq == 239 && before.Root == 0x10000ull + 38);
    Bc250VmidDescribe(&t, &h, 3, &live, &haveLive, &before, &haveBefore);
    CHECK(!haveBefore);                                             /* pushed out of the ring by now */
    /* Reset (GfxTearDown, power): every tenant that ran a job goes to the history, the table is empty. */
    i = h.Next;
    Bc250VmidResetAll(&t, &h);
    CHECK(h.Next == i + 1);                     /* VMID 3; 4 and 9 ran no job since their last claim */
    { unsigned v; for (v = 0; v < 16; v++) CHECK(t.Root[v] == 0 && t.LiveSeq[v] == 0 && t.Order[v] == 0); }
    CHECK(t.Clock == 0);
    Bc250VmidDescribe(&t, &h, 3, &live, &haveLive, &before, &haveBefore);
    CHECK(!haveLive && haveBefore && before.Root == 0xB000 && before.Process == 777);
}

int main(void)
{
    TestProbe();
    TestChooser();
    TestRule();
    TestGateClosedIdentity();
    TestFlushMask();
    TestHistory();
    TestExhaustive();
    TestRandom();
    printf("vmid pool: %u checks, %u failed\n", checks, bad);
    return bad ? 1 : 0;
}
