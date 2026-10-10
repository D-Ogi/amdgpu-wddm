/* BD-114 host control: the private submit watchdog's decisions and the aborted-fence report
 * (submit_watchdog.h, hang_recovery.h; scratch\bd114\ANALYSIS.md sections 7.1, 7.2, 7.3, 7.4 and 7.7;
 * docs/design/hang-recovery.md, "The private submit watchdog"). Run by run_submit_watchdog.ps1, which also reads
 * wddm.c by text, because this test cannot compile it.
 *
 * What this file is for. Until 0.7.216.27 the watchdog was one constant, 500 ms, counted from the moment the
 * packet was written to the ring. Two kernel dumps of 2026-10-10 show the price: a dense 512-token LLM prefill
 * submits packets whose own execution time is 400 ms or more, the watchdog fired on one of them, the node closed,
 * the next submission was refused, the refusal latched RefusalPending[0], that flag blocked the
 * DXGK_INTERRUPT_DMA_PREEMPTED acknowledgement for ever, and the OS waited out its whole TdrDelay before
 * DxgkDdiResetEngine was asked to recover a packet that had meanwhile completed. The end of that chain is
 * bugcheck 0x116, because this part has no working GPU reset (facts M53). A false trip of this watchdog is
 * therefore not a lost frame; it is a guaranteed bugcheck.
 *
 * One thing this test establishes that the analysis did not: with 7.2's window armed when a job becomes the head,
 * 7.3's stamp no longer changes WHEN the watchdog fires - the staleness window restarts at every head change
 * either way. What 7.3 changes is the deadline a queued job carries and the two numbers the timeout line reports,
 * so that is what the cases below hold it to. The ranked order of the analysis is preserved; the claim is not. */
#include <stdio.h>
#include <string.h>
#include "submit_watchdog.h"
#include "hang_recovery.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

#define MS 10000ull                     /* KeQueryInterruptTime units in a millisecond */
#define MODEL_PENDING_MAX 7u            /* BC250_GFX_PENDING_MAX of gfx_completion_queue.h */
#define MODEL_START_MS 1000ull          /* interrupt time is never 0 when a driver submits */

/* ---- the model ------------------------------------------------------------------------------------------------
 *
 * One node-0 completion queue, one watchdog, one clock, and the two sites of wddm.c that decide: the head stamp
 * (WddmGfxHeadLocked) and the check (WddmSubmitDpcCheck). The hardware is a progress token the case moves, which
 * is what the driver mixes out of the fence slot and the command processor's own fetch registers. */
typedef struct {
    unsigned Seq;
    unsigned long long Submitted, Deadline, HeadSince;
} MODEL_JOB;

typedef struct {
    MODEL_JOB Job[MODEL_PENDING_MAX];
    unsigned Count;
    BC250_SUBMIT_WATCHDOG Watch;
    unsigned long BudgetMs, TickMs;
    unsigned long long Now, Token;
    int Fired;                                  /* the watchdog closed the node in this run */
    unsigned FiredSeq;
    unsigned long FiredHeadMs, FiredQueuedMs, FiredStaleMs;
    unsigned long HeadMaxMs, QueueMaxMs;        /* the two high-water marks of the wddm summary */
    unsigned Checks, Rearms;
} MODEL;

static void ModelStart(MODEL* m, unsigned long requested, unsigned long tdrSeconds)
{
    int defaulted = 0, raised = 0;
    memset(m, 0, sizeof(*m));
    m->BudgetMs = Bc250SubmitBudgetMs(requested, tdrSeconds, &defaulted, &raised);
    m->TickMs = Bc250SubmitTickMs(m->BudgetMs);
    m->Now = MODEL_START_MS * MS;
    m->Token = 1ull;                            /* the hardware is never all zero in a healthy run */
    Bc250SubmitWatchdogIdle(&m->Watch);
}

static MODEL_JOB* ModelHeadJob(MODEL* m) { return m->Count != 0 ? &m->Job[0] : NULL; }

/* WddmGfxHeadLocked. A new head is stamped here and nowhere else; a head that already carries a deadline keeps
 * it, so appending work can never extend a running job's watchdog. */
static void ModelHead(MODEL* m)
{
    MODEL_JOB* job = ModelHeadJob(m);
    if (job == NULL) { Bc250SubmitWatchdogIdle(&m->Watch); return; }
    if (job->Deadline == 0ull)
    {
        job->HeadSince = m->Now;
        job->Deadline = Bc250SubmitHeadDeadline(job->Deadline, m->Now, m->BudgetMs); /* STAMP SITE */
        if (job->Submitted != 0ull && m->Now > job->Submitted)
        {
            unsigned long queued = Bc250SubmitElapsedMs(job->Submitted, m->Now);
            if (queued > m->QueueMaxMs) m->QueueMaxMs = queued;
        }
        Bc250SubmitWatchdogArm(&m->Watch, m->Now);
    }
}

static int ModelSubmit(MODEL* m, unsigned seq)
{
    MODEL_JOB job;
    if (m->Count >= MODEL_PENDING_MAX) return 0;        /* the ring is full: WddmSubmitHardware refuses */
    job.Seq = seq;
    job.Submitted = m->Now;
    job.Deadline = 0ull;
    job.HeadSince = 0ull;
    m->Job[m->Count++] = job;
    ModelHead(m);
    return 1;
}

static void ModelRetire(MODEL* m)
{
    unsigned i;
    if (m->Count == 0) return;
    for (i = 1; i < m->Count; i++) m->Job[i - 1] = m->Job[i];
    m->Count--;
    m->Token++;                                         /* a retirement moves the fence slot */
    ModelHead(m);
}

/* WddmSubmitDpcCheck. Both conditions, as the driver has them: the staleness window AND the head's own deadline. */
static void ModelCheck(MODEL* m)
{
    MODEL_JOB* head = ModelHeadJob(m);
    unsigned long long age = 0;
    int stale;

    m->Checks++;
    if (head == NULL || m->Fired) return;
    stale = Bc250SubmitWatchdogCheck(&m->Watch, m->Token, m->Now, MS * (unsigned long long)m->BudgetMs, &age);
    if (stale && m->Now >= head->Deadline)
    {
        m->Fired = 1;
        m->FiredSeq = head->Seq;
        m->FiredHeadMs = Bc250SubmitElapsedMs(head->HeadSince, m->Now);
        m->FiredQueuedMs = head->Submitted != 0ull ? Bc250SubmitElapsedMs(head->Submitted, head->HeadSince) : 0ul;
        m->FiredStaleMs = (unsigned long)(age / MS);
        return;
    }
    m->Rearms++;
    if (head->HeadSince != 0ull)
    {
        unsigned long held = Bc250SubmitElapsedMs(head->HeadSince, m->Now);
        if (held > m->HeadMaxMs) m->HeadMaxMs = held;
    }
}

/* Time passes in whole ticks, with a check at each one, which is what the self-re-arming DPC does; the last step
 * of a run is cut short so that a case's durations are exact. MoveEveryMs is how often the hardware's progress
 * token changes inside a packet; 0 is a ring that shows no progress at all. */
static void ModelRun(MODEL* m, unsigned long forMs, unsigned long moveEveryMs)
{
    unsigned long long end = m->Now + MS * (unsigned long long)forMs;
    unsigned long long nextMove = moveEveryMs != 0ul ? m->Now + MS * (unsigned long long)moveEveryMs : 0ull;
    while (m->Now < end)
    {
        unsigned long long step = MS * (unsigned long long)m->TickMs;
        if (m->Now + step > end) step = end - m->Now;
        m->Now += step;
        if (nextMove != 0ull && m->Now >= nextMove)
        {
            m->Token++;
            nextMove = m->Now + MS * (unsigned long long)moveEveryMs;
        }
        ModelCheck(m);
    }
}

int main(void)
{
    MODEL m;
    unsigned long tdr, n;
    unsigned long long age;
    unsigned abortFence;
    BC250_SUBMIT_WATCHDOG w;

    /* ---- 7.1: the budget is a setting, defaulted FROM TdrDelay, never shorter than it ------------------------
     *
     * "TdrDelay ... specifies the number of seconds that the GPU can delay the preempt request from the GPU
     * scheduler ... 2 seconds is the default value" (tdr-registry-keys.md:53-55). The OS measures execution time
     * itself and owns recovery (timeout-detection-and-recovery.md:43). A private watchdog shorter than the OS's
     * budget replaces a mechanism that works with one that bugchecks. */
    CHECK(Bc250SubmitTdrMs(0) == 1000ul * BC250_SUBMIT_TDR_DEFAULT_S);  /* absent reads as Windows' own default */
    CHECK(Bc250SubmitTdrMs(2) == 2000ul && Bc250SubmitTdrMs(10) == 10000ul);
    CHECK(Bc250SubmitTdrMs(1000000ul) == BC250_SUBMIT_BUDGET_MAX_MS);   /* a typo cannot overflow the arithmetic */
    {
        int defaulted = 0, raised = 0;
        /* The lab's own configuration: TdrDelay 10 s (the GUI's default, BD-079), no SubmitWatchdogMs. */
        CHECK(Bc250SubmitBudgetMs(0, 10, &defaulted, &raised) == 10000ul + BC250_SUBMIT_MARGIN_MS);
        CHECK(defaulted == 1 && raised == 0);
        /* No TdrDelay either: Windows' own 2 s plus the margin, never a literal of ours. */
        CHECK(Bc250SubmitBudgetMs(0, 0, &defaulted, &raised) == 2000ul + BC250_SUBMIT_MARGIN_MS);
        CHECK(defaulted == 1 && raised == 0);
        /* THE DEFECT, as a number: an operator (or a value left over from an old build) asks for the 500 ms that
         * bugchecked unit A twice. It is raised to the OS's budget, and the start says that it was. */
        CHECK(Bc250SubmitBudgetMs(500, 10, &defaulted, &raised) == 10000ul);
        CHECK(defaulted == 0 && raised == 1);
        /* A value above TdrDelay is taken as asked, and an absurd one is clamped. */
        CHECK(Bc250SubmitBudgetMs(20000, 10, &defaulted, &raised) == 20000ul && raised == 0);
        CHECK(Bc250SubmitBudgetMs(1000000ul, 10, &defaulted, &raised) == BC250_SUBMIT_BUDGET_MAX_MS);
        /* THE FLOOR, over the whole range this driver can be configured with: whatever anybody writes anywhere,
         * our watchdog never fires before the OS scheduler's own timeout has started. */
        for (tdr = 0; tdr <= 300ul; tdr++)
        {
            static const unsigned long asked[] = { 0ul, 1ul, 100ul, 500ul, 2000ul, 11999ul, 60000ul, 400000ul };
            for (n = 0; n < sizeof(asked) / sizeof(asked[0]); n++)
                CHECK(Bc250SubmitBudgetMs(asked[n], tdr, &defaulted, &raised) >= Bc250SubmitTdrMs(tdr));
        }
    }
    /* The look-for-progress cadence stays strictly inside the budget, or a window would restart for ever. */
    for (n = 1; n <= BC250_SUBMIT_BUDGET_MAX_MS; n += 7ul)
        CHECK(Bc250SubmitTickMs(n) >= 1ul && (n < 2ul || Bc250SubmitTickMs(n) < n));
    CHECK(Bc250SubmitTickMs(12000ul) == BC250_SUBMIT_TICK_MAX_MS);
    CHECK(Bc250SubmitTickMs(400ul) == 100ul);

    /* ---- 7.2: re-arm while progress is observed --------------------------------------------------------------
     *
     * The design defect, not the number 500. The watchdog of every build before 0.7.216.27 had one input, wall
     * clock since the ring write, so it could not tell a long healthy job from a dead one at all. */

    /* The g12 arm of the two dumps: ONE packet alone on the ring (its queue depth was 1), 3.2 s of work, the
     * token moving as the command processor walks the indirect buffers. It must not fire - and it did. */
    ModelStart(&m, 0, 10);
    CHECK(ModelSubmit(&m, 1));
    ModelRun(&m, 3200, 100);
    CHECK(!m.Fired);
    ModelRetire(&m);
    CHECK(m.Count == 0);

    /* The same packet with the measured per-packet bound as the cadence: a token that moves once every 400 ms,
     * for twice the budget. Progress is progress even when it is slow. */
    ModelStart(&m, 0, 10);
    CHECK(ModelSubmit(&m, 1));
    ModelRun(&m, 2ul * m.BudgetMs, 400);
    CHECK(!m.Fired && m.Rearms > 0);

    /* A ring that shows no progress at all: the watchdog still catches it, and only after the whole budget. */
    ModelStart(&m, 0, 10);
    CHECK(ModelSubmit(&m, 1));
    ModelRun(&m, m.BudgetMs - m.TickMs, 0);
    CHECK(!m.Fired);                                    /* not one tick early */
    ModelRun(&m, m.BudgetMs + 2ul * m.TickMs, 0);
    CHECK(m.Fired && m.FiredSeq == 1);
    CHECK(m.FiredStaleMs >= m.BudgetMs);

    /* A token that moves once, late, inside a budget's worth of standing still, re-arms the whole budget. */
    ModelStart(&m, 0, 10);
    CHECK(ModelSubmit(&m, 1));
    ModelRun(&m, m.BudgetMs - m.TickMs, 0);
    CHECK(!m.Fired);
    m.Token++;
    ModelRun(&m, m.BudgetMs - m.TickMs, 0);
    CHECK(!m.Fired);

    /* Nobody was looking: a gap of a whole budget between two checks starts a new window instead of firing. A
     * debugger break, a DPC storm or a timer that fired late is not evidence of a hang (the rule
     * Bc250HangWatchCheck already uses in progress.h). */
    memset(&w, 0, sizeof(w));
    Bc250SubmitWatchdogArm(&w, 1000ull * MS);
    CHECK(!Bc250SubmitWatchdogCheck(&w, 7ull, 1250ull * MS, 12000ull * MS, &age));
    CHECK(!Bc250SubmitWatchdogCheck(&w, 7ull, 90000ull * MS, 12000ull * MS, &age) && age == 0ull);
    /* and the new window, once the checks are watching again, is a whole budget long */
    for (n = 1; n < 48ul; n++)
        CHECK(!Bc250SubmitWatchdogCheck(&w, 7ull, (90000ull + 250ull * n) * MS, 12000ull * MS, &age));
    CHECK(Bc250SubmitWatchdogCheck(&w, 7ull, (90000ull + 250ull * 48ul) * MS, 12000ull * MS, &age));
    CHECK(age == 12000ull * MS);

    /* A clock that went backwards is not stale time either. */
    memset(&w, 0, sizeof(w));
    Bc250SubmitWatchdogArm(&w, 90000ull * MS);
    CHECK(!Bc250SubmitWatchdogCheck(&w, 7ull, 1000ull * MS, 12000ull * MS, &age) && age == 0ull);

    /* An unprimed window never fires on its first check: no job was the head when it was opened. */
    memset(&w, 0, sizeof(w));
    Bc250SubmitWatchdogIdle(&w);
    CHECK(!Bc250SubmitWatchdogCheck(&w, 0ull, 500000ull * MS, 1000ull * MS, &age));

    /* ---- 7.3: the deadline is stamped when the job becomes the HEAD ------------------------------------------
     *
     * The q27 arm: a seven-deep queue, every packet 400 ms. The budget here is deliberately small (TdrDelay 1 s)
     * so that the queue wait is a large fraction of it, which is the shape of that dump. What is held below is
     * the identity of the deadline and of the two measured numbers, because the staleness window of 7.2 makes
     * the firing decision the same either way (the window restarts at every head change). Under the ring-write
     * rule of 0.7.216.26 the fourth packet's whole budget was gone before it began. */
    ModelStart(&m, 0, 1);               /* TdrDelay 1 s: budget 3000 ms, tick 250 ms */
    CHECK(m.BudgetMs == 1000ul + BC250_SUBMIT_MARGIN_MS);
    for (n = 1; n <= MODEL_PENDING_MAX; n++) CHECK(ModelSubmit(&m, (unsigned)n));
    CHECK(m.Count == MODEL_PENDING_MAX);
    CHECK(!ModelSubmit(&m, 99));        /* the ring is full; the eighth packet waits in software */
    /* Every job was written to the ring at the same moment, and only the head carries a deadline. */
    for (n = 1; n < MODEL_PENDING_MAX; n++)
        CHECK(m.Job[n].Submitted == MODEL_START_MS * MS && m.Job[n].Deadline == 0ull &&
              m.Job[n].HeadSince == 0ull);
    for (n = 1; n <= MODEL_PENDING_MAX; n++)
    {
        MODEL_JOB head = m.Job[0];
        CHECK(head.Seq == (unsigned)n);
        CHECK(head.HeadSince == m.Now);                                 /* stamped on arrival at the head */
        CHECK(head.Deadline == head.HeadSince + MS * m.BudgetMs);       /* THE WHOLE budget, from here */
        /* and the point of it: a job that waited gets a later deadline than the ring write would have given. */
        if (n > 1) CHECK(head.Deadline > head.Submitted + MS * m.BudgetMs);
        ModelRun(&m, 400, 100);         /* this packet executes, making progress */
        CHECK(!m.Fired);
        ModelRetire(&m);
    }
    CHECK(m.Count == 0 && !m.Fired);
    /* The queue wait is now a measured number of its own: the last packet waited six packets for its turn. */
    CHECK(m.QueueMaxMs == 6ul * 400ul);
    CHECK(m.HeadMaxMs == 400ul);

    /* Appending work must never extend a running job's watchdog. The head's deadline is stamped once. */
    ModelStart(&m, 0, 10);
    CHECK(ModelSubmit(&m, 1));
    {
        unsigned long long stamped = m.Job[0].Deadline;
        CHECK(stamped != 0ull);
        ModelRun(&m, 1000, 100);
        CHECK(ModelSubmit(&m, 2) && ModelSubmit(&m, 3));
        CHECK(m.Job[0].Deadline == stamped);                            /* NO EXTEND */
        CHECK(m.Job[1].HeadSince == 0ull && m.Job[1].Deadline == 0ull);
    }
    /* A job that has never been the head gets its budget from the moment it becomes one, and one that has a
     * deadline keeps it, whatever the caller passes. */
    CHECK(Bc250SubmitHeadDeadline(0ull, 5000ull * MS, 12000ul) == (5000ull + 12000ull) * MS);
    CHECK(Bc250SubmitHeadDeadline(77ull, 5000ull * MS, 12000ul) == 77ull);

    /* ---- 7.4: report the last completed fence as aborted when nothing is on the ring -------------------------
     *
     * "A special situation can occur when a packet is completed on the GPU between steps 3 and 7. In this case,
     * the driver should set LastAbortedFenceId to the fence ID of the last completed packet if there are no
     * packets in the hardware queue from the driver's point of view. From the scheduler's point of view, it
     * appears that such a packet was aborted" (tdr-changes-in-windows-8.md:100). Today that situation is verdict
     * 3, which is a refusal, after which ResetFromTimeout fails and the machine bugchecks.
     *
     * The design document's objection is answered by the guards, not waved away: "our view of 'last completed'
     * can lag a pending report, and a wrong choice re-executes a packet" (hang-recovery.md). */
    CHECK(Bc250HangPreKillVerdict(0, 0, 1, 100, 1) == BC250_HANG_VERDICT_NOTHING_ON_RING);
    /* The q27 numbers: last reported 232226, last submitted 232228. */
    abortFence = 0xFFFFFFFFu;
    CHECK(Bc250HangAbortReportedFence(0, 0, 1, 232226u, 1, 232228u, &abortFence) && abortFence == 232226u);
    /* Each guard refuses on its own, and a refusal leaves verdict 3 and today's behaviour. */
    CHECK(!Bc250HangAbortReportedFence(1, 0, 1, 232226u, 1, 232228u, &abortFence));  /* a job IS on the ring */
    CHECK(!Bc250HangAbortReportedFence(0, 1, 1, 232226u, 1, 232228u, &abortFence));  /* a completion is pending */
    CHECK(!Bc250HangAbortReportedFence(0, 0, 0, 232226u, 1, 232228u, &abortFence));  /* nothing reported yet */
    CHECK(!Bc250HangAbortReportedFence(0, 0, 1, 232226u, 0, 232228u, &abortFence));  /* nothing submitted yet */
    /* Out of the engine-reset contract's range is bugcheck 0x119, so it is refused: a reported fence NEWER than
     * the last submitted one cannot be named. */
    CHECK(!Bc250HangAbortReportedFence(0, 0, 1, 232229u, 1, 232228u, &abortFence));
    /* Equal is in range: the last submitted packet is also the last reported one. */
    CHECK(Bc250HangAbortReportedFence(0, 0, 1, 232228u, 1, 232228u, &abortFence) && abortFence == 232228u);
    /* Fence ids are 32 bits and wrap, and the comparison is the wrap-aware one. */
    CHECK(Bc250HangAbortReportedFence(0, 0, 1, 0xFFFFFFFEu, 1, 1u, &abortFence) && abortFence == 0xFFFFFFFEu);
    CHECK(!Bc250HangAbortReportedFence(0, 0, 1, 1u, 1, 0xFFFFFFFEu, &abortFence));
    /* A guard that refuses must not have written an answer. */
    abortFence = 0x5A5Au;
    CHECK(!Bc250HangAbortReportedFence(0, 1, 1, 232226u, 1, 232228u, &abortFence) && abortFence == 0x5A5Au);

    /* The verdict is a recovery, it is a verdict of its own, and the record's arithmetic still closes: an
     * outcome decided before any kill counts its own attempt, because no PENDING record counted one for it. */
    CHECK(BC250_HANG_VERDICT_ABORT_REPORTED == 7u);
    CHECK(Bc250HangVerdictRecovered(BC250_HANG_VERDICT_ABORT_REPORTED));
    CHECK(Bc250HangVerdictRecovered(BC250_HANG_VERDICT_DRAINED));
    CHECK(Bc250HangVerdictRecovered(BC250_HANG_VERDICT_ALREADY_RETIRED));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_NOTHING_ON_RING));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_NOT_DRAINED));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_FENCE_GUARD));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_VMID_GUARD));
    CHECK(Bc250HangVerdictCounts(BC250_HANG_VERDICT_ABORT_REPORTED) ==
          (BC250_HANG_COUNT_ATTEMPT | BC250_HANG_COUNT_RECOVERED));
    {
        /* Attempts == Recovered + NotDrained + Refused after every finished attempt, with the new verdict in the
         * mix: one PENDING-then-DRAINED attempt, one ABORT_REPORTED and one pre-kill refusal. */
        static const unsigned writes[] = {
            BC250_HANG_VERDICT_PENDING, BC250_HANG_VERDICT_DRAINED,
            BC250_HANG_VERDICT_ABORT_REPORTED,
            BC250_HANG_VERDICT_NOTHING_ON_RING
        };
        unsigned attempts = 0, recovered = 0, notDrained = 0, refused = 0;
        for (n = 0; n < sizeof(writes) / sizeof(writes[0]); n++)
        {
            unsigned bits = Bc250HangVerdictCounts(writes[n]);
            if (bits & BC250_HANG_COUNT_ATTEMPT) attempts++;
            if (bits & BC250_HANG_COUNT_RECOVERED) recovered++;
            if (bits & BC250_HANG_COUNT_NOT_DRAINED) notDrained++;
            if (bits & BC250_HANG_COUNT_REFUSED) refused++;
        }
        CHECK(attempts == 3u && recovered == 2u && notDrained == 0u && refused == 1u);
        CHECK(attempts == recovered + notDrained + refused);
    }

    /* ---- 7.7: log what was measured, not the constant --------------------------------------------------------
     *
     * The timeout line printed the budget itself until 0.7.216.27, so "after 500 ms" meant "after at least
     * 500 ms, by an unknown amount", and the offline analysis of two bugchecks could bound a packet's duration
     * but never measure one. */
    CHECK(Bc250SubmitElapsedMs(0ull, 400ull * MS) == 400ul);
    CHECK(Bc250SubmitElapsedMs(1000ull * MS, 1400ull * MS) == 400ul);
    CHECK(Bc250SubmitElapsedMs(1000ull * MS, 1000ull * MS) == 0ul);      /* no time has passed */
    CHECK(Bc250SubmitElapsedMs(1000ull * MS, 999ull * MS) == 0ul);       /* and no negative durations */
    CHECK(Bc250SubmitElapsedMs(0ull, MS - 1ull) == 0ul);                 /* truncated, never rounded up */

    /* A fire reports the three numbers it judged on, and they are MEASURED: the seventh packet of the q27 shape
     * waited 2.4 s for its turn and then stopped making progress. Neither number is the budget. */
    ModelStart(&m, 0, 1);
    for (n = 1; n <= MODEL_PENDING_MAX; n++) CHECK(ModelSubmit(&m, (unsigned)n));
    for (n = 1; n < MODEL_PENDING_MAX; n++)
    {
        ModelRun(&m, 400, 100);
        CHECK(!m.Fired);
        ModelRetire(&m);
    }
    CHECK(m.Count == 1 && m.Job[0].Seq == MODEL_PENDING_MAX);
    ModelRun(&m, 2ul * m.BudgetMs, 0);                                  /* and now it stops */
    CHECK(m.Fired && m.FiredSeq == MODEL_PENDING_MAX);
    CHECK(m.FiredQueuedMs == 6ul * 400ul);                              /* the term section 4.2 could not split */
    CHECK(m.FiredHeadMs > m.BudgetMs);                                  /* longer than the budget, not equal to it */
    CHECK(m.FiredStaleMs >= m.BudgetMs);
    CHECK(m.FiredHeadMs >= m.FiredStaleMs);                             /* the stall is inside the head's time */

    printf("PASS: budget floor over 301 TdrDelay values, progress re-arm (g12 3.2 s, 400 ms packets, no progress,"
           " unwatched gap, clock back), head stamp (q27 7-deep, no extend), aborted fence (q27 232226/232228,"
           " six guards, wrap, record arithmetic), measured elapsed (head %lu ms, queued %lu ms, stale %lu ms,"
           " budget %lu ms)\n",
           m.FiredHeadMs, m.FiredQueuedMs, m.FiredStaleMs, m.BudgetMs);
    return 0;
}
