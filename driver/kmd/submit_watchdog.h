/* BD-114: the private submit watchdog of node 0, as pure decisions (scratch\bd114\ANALYSIS.md, sections 7.1,
 * 7.2, 7.3 and 7.7; docs/design/hang-recovery.md, "The private submit watchdog"). No kernel types and no I/O, so
 * wddm.c and the host test (test/submit_watchdog_test.c, run_submit_watchdog.ps1) compile one definition.
 *
 * Why this file exists at all. Until 0.7.216.27 the watchdog was one constant, 500 ms, measured from the moment
 * the packet was written to the ring. Two kernel dumps of 2026-10-10 (q27 02:55Z, g12 03:14Z) show what that
 * costs: a dense 512-token prefill submits packets whose own execution time is 400 ms or more, the watchdog
 * fired on one of them, the node closed, the next submission was refused, the refusal latched
 * RefusalPending[0], that flag blocked the DXGK_INTERRUPT_DMA_PREEMPTED acknowledgement for ever, and the OS
 * waited out its whole TdrDelay before DxgkDdiResetEngine was asked to recover a packet that had meanwhile
 * completed. The end of that chain is bugcheck 0x116, because this part has no working GPU reset (facts M53).
 * A false trip can therefore lead to a bugcheck when recovery cannot reopen the node.
 *
 * BD-152 removes that failure path: elapsed time permits diagnostics only. Windows times a scheduler
 * preemption request; this observer times sampled inactivity of a software queue head. Those clocks have
 * different origins. Activity can postpone diagnostics indefinitely, and inactivity is not proof that shader
 * execution has stopped. */
#ifndef BC250_SUBMIT_WATCHDOG_H
#define BC250_SUBMIT_WATCHDOG_H

/* BD-152: a diagnostic clock, independent of Windows' preemption/TDR clock.
 * Reaching this threshold authorizes a bounded snapshot, never a gate closure,
 * completion, preemption acknowledgement or engine reset. */
#define BC250_SUBMIT_DIAGNOSTIC_VERSION 2u
#define BC250_SUBMIT_DIAGNOSTIC_DEFAULT_MS 12000ul
#define BC250_SUBMIT_DIAGNOSTIC_MIN_MS 1000ul
#define BC250_SUBMIT_DIAGNOSTIC_MAX_MS 300000ul

static __inline unsigned long Bc250SubmitDiagnosticMs(unsigned long Requested, int* Invalid)
{
    *Invalid = Requested != 0ul &&
        (Requested < BC250_SUBMIT_DIAGNOSTIC_MIN_MS || Requested > BC250_SUBMIT_DIAGNOSTIC_MAX_MS);
    return Requested == 0ul || *Invalid ? BC250_SUBMIT_DIAGNOSTIC_DEFAULT_MS : Requested;
}

/* ---- 7.2: re-arm while progress is observed --------------------------------------------------------------- */

/* How often the watchdog looks for progress. A quarter of the budget, capped: the gap between two checks must
 * stay BELOW the budget, or the staleness window below would restart itself for ever and the watchdog would
 * never fire at all. 250 ms is the cap because a check is five register reads and a fence read in a DPC. Software
 * head changes can also restart the cadence (the tick is re-armed by the
 * DPC itself, and a head change re-arms it from the stamp).
 *
 * A budget of 3 ms or less yields a tick equal to the budget and the watchdog then never fires. That is the
 * conservative direction and it cannot be reached from the registry, because the diagnostic threshold is at least 1 s. */
#define BC250_SUBMIT_TICK_MAX_MS 250u
#define BC250_SUBMIT_TICK_DIVISOR 4u

static __inline unsigned long Bc250SubmitTickMs(unsigned long BudgetMs)
{
    unsigned long tick = BudgetMs / (unsigned long)BC250_SUBMIT_TICK_DIVISOR;

    if (tick > (unsigned long)BC250_SUBMIT_TICK_MAX_MS) tick = (unsigned long)BC250_SUBMIT_TICK_MAX_MS;
    if (tick == 0ul) tick = 1ul;
    return tick;
}

/* The progress token. One 64-bit value mixed out of the counters that move while a node-0 packet is healthy:
 * the submission fence slot the CP writes (completion progress) and the command processor's own live fetch
 * registers (CP_RB0_RPTR, the CP_IB1/CP_IB2 base and size pairs), which wddm.c already reads in its timeout
 * snapshot. Only equality matters; the value is never read as a number.
 *
 * Two honest limits, the same two the snapshot's comment states. The CP_IB* family is banked by GRBM_GFX_INDEX,
 * whose bank the wave reader can change. The sampler excludes that reader and otherwise uses the shim's bank.
 * Those registers describe
 * where the CP is now, not necessarily the head job. These limits can make the token change when the head
 * made no progress. A/B/A/B changes can postpone expiration forever without useful forward progress. A
 * mix collision or a long shader with stationary fetch registers can hide useful work and permit expiration. */
#define BC250_SUBMIT_PROGRESS_SEED 0xCBF29CE484222325ull        /* FNV-1a's 64-bit offset basis */

static __inline unsigned long long Bc250SubmitProgressMix(unsigned long long Token, unsigned long Value)
{
    Token ^= (unsigned long long)Value;
    Token *= 0x100000001B3ull;                                  /* FNV-1a's 64-bit prime */
    return Token;
}

/* The staleness window of one node. Times are KeQueryInterruptTime units (100 ns), the clock the deadlines and
 * the log's own ticks use. */
typedef enum BC250_SUBMIT_OBSERVATION {
    BC250_SUBMIT_OBSERVATION_NONE = 0,
    BC250_SUBMIT_OBSERVATION_PRIME,
    BC250_SUBMIT_OBSERVATION_ACTIVITY,
    BC250_SUBMIT_OBSERVATION_GAP_RESET,
    BC250_SUBMIT_OBSERVATION_UNCHANGED
} BC250_SUBMIT_OBSERVATION;

typedef struct BC250_SUBMIT_WATCHDOG {
    unsigned long long Progress;        /* last sampled token, not a monotonic progress measure */
    unsigned long long Advanced;        /* beginning of the current observed inactivity window */
    unsigned long long Checked;         /* previous check */
    int Primed;                         /* a window is open; 0 while no job is the head */
    int Observed;                       /* a token has actually been sampled in this window */
    int SnapshotTaken;                  /* one diagnostic per head, reset on a new head */
    BC250_SUBMIT_OBSERVATION LastObservation;
} BC250_SUBMIT_WATCHDOG;

/* The first DPC sample primes the window even when its token is zero. Opening a window or
 * restarting observation after a gap is not an observed activity change. */
static __inline void Bc250SubmitWatchdogArm(BC250_SUBMIT_WATCHDOG* Watchdog, unsigned long long Now)
{
    Watchdog->Progress = 0ull;
    Watchdog->Advanced = Now;
    Watchdog->Checked = Now;
    Watchdog->Primed = 1;
    Watchdog->Observed = 0;
    Watchdog->SnapshotTaken = 0;
    Watchdog->LastObservation = BC250_SUBMIT_OBSERVATION_NONE;
}

static __inline void Bc250SubmitWatchdogIdle(BC250_SUBMIT_WATCHDOG* Watchdog)
{
    Watchdog->Primed = 0;
    Watchdog->Observed = 0;
    Watchdog->SnapshotTaken = 0;
    Watchdog->LastObservation = BC250_SUBMIT_OBSERVATION_NONE;
}

/* Nonzero when sampled inactivity reaches Budget. A full-budget observation gap or a backwards
 * clock resets the window. Classify exactly once per check, independently of any caller timer rearm.
 * GAP_RESET takes precedence over a changed token because continuity of observation was lost. */
static __inline int Bc250SubmitWatchdogCheck(BC250_SUBMIT_WATCHDOG* Watchdog, unsigned long long Progress,
                                             unsigned long long Now, unsigned long long Budget,
                                             unsigned long long* Age)
{
    int watched = Watchdog->Primed && Now >= Watchdog->Checked && Now - Watchdog->Checked < Budget;

    *Age = 0ull;
    if (!Watchdog->Primed || !Watchdog->Observed)
        Watchdog->LastObservation = BC250_SUBMIT_OBSERVATION_PRIME;
    else if (!watched || Now < Watchdog->Advanced)
        Watchdog->LastObservation = BC250_SUBMIT_OBSERVATION_GAP_RESET;
    else if (Progress != Watchdog->Progress)
        Watchdog->LastObservation = BC250_SUBMIT_OBSERVATION_ACTIVITY;
    else
        Watchdog->LastObservation = BC250_SUBMIT_OBSERVATION_UNCHANGED;
    Watchdog->Checked = Now;
    if (Watchdog->LastObservation != BC250_SUBMIT_OBSERVATION_UNCHANGED)
    {
        Watchdog->Primed = 1;
        Watchdog->Observed = 1;
        Watchdog->Progress = Progress;
        Watchdog->Advanced = Now;
        return 0;
    }
    *Age = Now - Watchdog->Advanced;
    return *Age >= Budget;
}

/* ---- 7.3: the deadline belongs to the head ---------------------------------------------------------------- */

/* The deadline of the job at the head of the completion queue. Deadline 0 means "has never been the head": the
 * job gets its budget from Now. A job that already carries a deadline keeps it, which is the rule
 * WddmGfxHeadLocked has always stated ("appending work must not extend a hung job's watchdog") - only now it is
 * also the rule that stops a queued job from being charged for the time it waited. */
static __inline unsigned long long Bc250SubmitHeadDeadline(unsigned long long Deadline, unsigned long long Now,
                                                           unsigned long BudgetMs)
{
    return Deadline != 0ull ? Deadline : Now + 10000ull * (unsigned long long)BudgetMs;
}

/* ---- 7.7: say what was measured ---------------------------------------------------------------------------- */

/* Milliseconds between two interrupt-time stamps, 0 if the second is not later. The timeout line printed the
 * CONSTANT until 0.7.216.27 ("after 500 ms" meant "after at least 500 ms, by an unknown amount"), which is why
 * the analysis of BD-114 could bound the packet duration but never measure it. */
static __inline unsigned long Bc250SubmitElapsedMs(unsigned long long Start, unsigned long long Now)
{
    return Now > Start ? (unsigned long)((Now - Start) / 10000ull) : 0ul;
}

#endif /* BC250_SUBMIT_WATCHDOG_H */
