/* TDR recovery S1 host control: the pure decisions behind DxgkDdiResetFromTimeout and DxgkDdiRestartFromTimeout
 * (adapter_reset.h, docs/design/tdr-recovery-s1.md). The register reads, the fence reads and the wave kill touch
 * hardware and run on the lab behind AdapterRecoveryMode; this test fixes the switch, the idle witnesses (every
 * sample, every register, the SDMA0 pointers only while the paging ring is in use), the all-or-nothing begin of the
 * reset on every node, the fence advance of an adapter-wide reset, the window outcome at the restart, the record's
 * counters on the adapter path, and that every witness register is on the read list that MmioRead admits. A
 * refactor cannot quietly make ResetFromTimeout return success on a busy engine. */
#include <stdio.h>
#include <string.h>
#include "adapter_reset.h"
/* The generated tables, as mmio.c compiles them (the pattern of hang_recovery_test.c). */
typedef struct _BC250_DCN_REG_INFO { const char* Name; unsigned long Offset; } BC250_DCN_REG_INFO;
#define BC250_REGS_WITH_TABLES
#include "regs.generated.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define NODES 3u

/* The witness table of wddm.c, in the order of the BC250_ADAPTER_REG_* indexes. run_adapter_reset.ps1 checks that
 * wddm.c's g_AdapterWitnessRegisters lists the same names in the same order. */
static const unsigned long g_Witness[BC250_ADAPTER_SAMPLE_REGISTERS] = {
    BC250_REG_GC_GRBM_STATUS, BC250_REG_GC_CP_STAT, BC250_REG_GC_CP_CPC_STATUS, BC250_REG_GC_SDMA0_STATUS_REG,
    BC250_REG_GC_SDMA0_GFX_RB_RPTR, BC250_REG_GC_SDMA0_GFX_RB_RPTR_HI, BC250_REG_GC_SDMA0_GFX_RB_WPTR,
    BC250_REG_GC_SDMA0_GFX_RB_WPTR_HI,
};

static int readable(unsigned long offset)
{
    unsigned i;
    for (i = 0; i < BC250_MMIO_READ_ALLOW_COUNT; i++)
        if (g_MmioReadAllow[i] == offset) return 1;
    return 0;
}

/* An idle sample: every register read, GUI_ACTIVE, CP_BUSY and CPC_BUSY clear, SDMA0 IDLE set, the SDMA0 ring's
 * read pointer equal to its write pointer. */
static BC250_ADAPTER_SAMPLE idle_sample(void)
{
    BC250_ADAPTER_SAMPLE s;
    memset(&s, 0, sizeof(s));
    s.Read = (1u << BC250_ADAPTER_SAMPLE_REGISTERS) - 1u;
    s.Value[BC250_ADAPTER_REG_SDMA0_STATUS] = SDMA0_STATUS_REG__IDLE_MASK;
    s.Value[BC250_ADAPTER_REG_SDMA0_RPTR] = s.Value[BC250_ADAPTER_REG_SDMA0_WPTR] = 0x1230u;
    s.Value[BC250_ADAPTER_REG_SDMA0_RPTR_HI] = s.Value[BC250_ADAPTER_REG_SDMA0_WPTR_HI] = 0x1u;
    return s;
}

/* The record's counters as guard.c keeps them: one write adds Bc250HangVerdictCounts (hang_recovery_test.c). */
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

static int balanced(const struct record* r)
{
    return r->attempts == r->recovered + r->notDrained + r->refused;
}

int main(void)
{
    BC250_ADAPTER_SAMPLE s, samples[BC250_ADAPTER_SAMPLES];
    BC250_ADAPTER_RINGS rings = { 1, 1, 1 };
    BC250_HANG_NODE_STATE states[NODES];
    volatile long active[NODES] = { 0, 0, 0 };
    struct record r = { 0, 0, 0, 0, 0 };
    unsigned fence = 0, n, i;

    /* The switch. HangRecoveryMode 0 turns S1 off whatever AdapterRecoveryMode says; absent (the code default 2)
     * and any value other than 0 and 1 is the kill mode, as for the other finished features. */
    CHECK(Bc250AdapterRecoveryMode(0, 2) == BC250_ADAPTER_RECOVERY_OFF);
    CHECK(Bc250AdapterRecoveryMode(0, 1) == BC250_ADAPTER_RECOVERY_OFF);
    CHECK(Bc250AdapterRecoveryMode(1, 0) == BC250_ADAPTER_RECOVERY_OFF);
    CHECK(Bc250AdapterRecoveryMode(1, 1) == BC250_ADAPTER_RECOVERY_IDLE);
    CHECK(Bc250AdapterRecoveryMode(1, 2) == BC250_ADAPTER_RECOVERY_KILL);
    CHECK(Bc250AdapterRecoveryMode(1, 7) == BC250_ADAPTER_RECOVERY_KILL);

    /* Every witness register is on the read list. MmioRead refuses any other offset, and an unread witness is a
     * refusal, so a register missing there would make S1 refuse every time without a word in the build. */
    for (i = 0; i < BC250_ADAPTER_SAMPLE_REGISTERS; i++) CHECK(readable(g_Witness[i]));

    /* One sample. Idle is 0 with the paging ring in use and without it. */
    s = idle_sample();
    CHECK(Bc250AdapterSampleBusy(&s, 1) == 0);
    CHECK(Bc250AdapterSampleBusy(&s, 0) == 0);
    s.Value[BC250_ADAPTER_REG_GRBM_STATUS] |= GRBM_STATUS__GUI_ACTIVE_MASK;
    CHECK(Bc250AdapterSampleBusy(&s, 1) == BC250_ADAPTER_BUSY_GUI_ACTIVE);
    s = idle_sample();
    s.Value[BC250_ADAPTER_REG_CP_STAT] |= CP_STAT__CP_BUSY_MASK;
    CHECK(Bc250AdapterSampleBusy(&s, 1) == BC250_ADAPTER_BUSY_CP);
    s = idle_sample();
    s.Value[BC250_ADAPTER_REG_CP_CPC_STATUS] |= CP_CPC_STATUS__CPC_BUSY_MASK;
    CHECK(Bc250AdapterSampleBusy(&s, 1) == BC250_ADAPTER_BUSY_CPC);
    s = idle_sample();
    s.Value[BC250_ADAPTER_REG_SDMA0_STATUS] = 0;    /* IDLE clear */
    CHECK(Bc250AdapterSampleBusy(&s, 1) == BC250_ADAPTER_BUSY_SDMA0);
    CHECK(Bc250AdapterSampleBusy(&s, 0) == BC250_ADAPTER_BUSY_SDMA0);
    /* A device that answers every read with all ones (a dead link) is busy on every test. */
    for (i = 0; i < BC250_ADAPTER_SAMPLE_REGISTERS; i++) s.Value[i] = 0xFFFFFFFFu;
    CHECK((Bc250AdapterSampleBusy(&s, 1) & (BC250_ADAPTER_BUSY_GUI_ACTIVE | BC250_ADAPTER_BUSY_CP |
                                            BC250_ADAPTER_BUSY_CPC)) ==
          (BC250_ADAPTER_BUSY_GUI_ACTIVE | BC250_ADAPTER_BUSY_CP | BC250_ADAPTER_BUSY_CPC));
    /* The SDMA0 ring pointers: a witness only while the paging node runs on that ring, both halves compared. */
    s = idle_sample();
    s.Value[BC250_ADAPTER_REG_SDMA0_WPTR] += 0x20u;
    CHECK(Bc250AdapterSampleBusy(&s, 1) == BC250_ADAPTER_BUSY_SDMA0_POINTERS);
    CHECK(Bc250AdapterSampleBusy(&s, 0) == 0);
    s = idle_sample();
    s.Value[BC250_ADAPTER_REG_SDMA0_WPTR_HI] += 1u;
    CHECK(Bc250AdapterSampleBusy(&s, 1) == BC250_ADAPTER_BUSY_SDMA0_POINTERS);
    /* An unread register refuses. The pointers are needed only while the ring is in use. */
    s = idle_sample();
    s.Read &= ~(1u << BC250_ADAPTER_REG_CP_CPC_STATUS);
    CHECK(Bc250AdapterSampleBusy(&s, 0) == BC250_ADAPTER_BUSY_UNREAD);
    s = idle_sample();
    s.Read &= ~(1u << BC250_ADAPTER_REG_SDMA0_WPTR_HI);
    CHECK(Bc250AdapterSampleBusy(&s, 1) == BC250_ADAPTER_BUSY_UNREAD);
    CHECK(Bc250AdapterSampleBusy(&s, 0) == 0);

    /* All witnesses. Every sample must be idle, not only the last, and every ring must have retired. */
    for (i = 0; i < BC250_ADAPTER_SAMPLES; i++) samples[i] = idle_sample();
    CHECK(Bc250AdapterBusyReasons(&rings, samples, BC250_ADAPTER_SAMPLES, 1) == 0);
    CHECK(Bc250AdapterBusyReasons(&rings, samples, 0, 1) == BC250_ADAPTER_BUSY_UNREAD);
    samples[0].Value[BC250_ADAPTER_REG_GRBM_STATUS] |= GRBM_STATUS__GUI_ACTIVE_MASK;
    CHECK(Bc250AdapterBusyReasons(&rings, samples, BC250_ADAPTER_SAMPLES, 1) == BC250_ADAPTER_BUSY_GUI_ACTIVE);
    samples[0] = idle_sample();
    samples[1].Value[BC250_ADAPTER_REG_SDMA0_STATUS] = 0;
    CHECK(Bc250AdapterBusyReasons(&rings, samples, BC250_ADAPTER_SAMPLES, 1) == BC250_ADAPTER_BUSY_SDMA0);
    samples[1] = idle_sample();
    rings.GfxRetired = 0;
    CHECK(Bc250AdapterBusyReasons(&rings, samples, BC250_ADAPTER_SAMPLES, 1) == BC250_ADAPTER_BUSY_GFX_RING);
    rings.GfxRetired = 1; rings.ComputeRetired = 0;
    CHECK(Bc250AdapterBusyReasons(&rings, samples, BC250_ADAPTER_SAMPLES, 1) == BC250_ADAPTER_BUSY_COMPUTE_RING);
    rings.ComputeRetired = 1; rings.PagingRetired = 0;
    CHECK(Bc250AdapterBusyReasons(&rings, samples, BC250_ADAPTER_SAMPLES, 1) == BC250_ADAPTER_BUSY_PAGING_RING);
    rings.PagingRetired = 1;

    /* The begin on every node: all or nothing. */
    memset(states, 0, sizeof(states));
    states[1].ReportInFlight = 1;
    CHECK(Bc250AdapterResetBeginAll(states, active, NODES) == BC250_ADAPTER_BUSY_PUBLICATION);
    for (n = 0; n < NODES; n++) { CHECK(!states[n].ResetActive); CHECK(states[n].Epoch == 0); }
    states[1].ReportInFlight = 0;
    active[2] = 1;
    CHECK(Bc250AdapterResetBeginAll(states, active, NODES) == BC250_ADAPTER_BUSY_SUBMISSIONS);
    for (n = 0; n < NODES; n++) CHECK(!states[n].ResetActive);
    active[2] = 0;
    states[0].ResetActive = 1;
    CHECK(Bc250AdapterResetBeginAll(states, active, NODES) == BC250_ADAPTER_BUSY_NO_STATE);
    CHECK(!states[1].ResetActive && !states[2].ResetActive);
    states[0].ResetActive = 0;
    CHECK(Bc250AdapterResetBeginAll(states, active, NODES) == 0);
    for (n = 0; n < NODES; n++) { CHECK(states[n].ResetActive); CHECK(states[n].Epoch == 1); }
    /* A begun reset stops publication and admission on every node (hang_recovery.h). */
    for (n = 0; n < NODES; n++) CHECK(!Bc250HangReportBegin(&states[n]));
    /* The undo of a refusal advances nothing. */
    Bc250AdapterResetAbortAll(states, NODES);
    for (n = 0; n < NODES; n++) { CHECK(!states[n].ResetActive); CHECK(!states[n].BoundaryKnown); }

    /* The fence advance: the last submitted fence, else the last reported, else nothing. 32-bit ids. */
    CHECK(Bc250AdapterResetFence(1, 0x10005u, 1, 0x10001u, &fence) && fence == 0x10005u);
    CHECK(Bc250AdapterResetFence(0, 0, 1, 0x77u, &fence) && fence == 0x77u);
    fence = 0xABCDu;
    CHECK(!Bc250AdapterResetFence(0, 0, 0, 0, &fence) && fence == 0xABCDu);

    /* The commit: completion, notification boundary and scheduler boundary are the advanced fence; a lost report
     * is resolved; the node stays in its reset until the restart. */
    memset(states, 0, sizeof(states));
    CHECK(Bc250AdapterResetBeginAll(states, active, NODES) == 0);
    states[0].ReportLost = 1; states[0].ReportLostFence = 40;
    Bc250AdapterResetCommitNode(&states[0], 1, 41u);
    CHECK(states[0].CompletedKnown && states[0].CompletedFence == 41u);
    CHECK(states[0].BoundaryKnown && states[0].BoundaryFence == 41u);
    CHECK(!states[0].ReportLost && !states[0].ReportInFlight);
    CHECK(states[0].ResetActive);
    Bc250AdapterResetCommitNode(&states[2], 0, 0);
    CHECK(!states[2].CompletedKnown && !states[2].BoundaryKnown && states[2].ResetActive);
    /* After the restart, a later engine reset of the node is admitted again and names the advanced fence as the
     * scheduler's boundary (the 0x119 range of hang-recovery.md). */
    Bc250AdapterRestartNode(&states[0]);
    CHECK(!states[0].ResetActive);
    CHECK(Bc250HangAdmissionReasons(&states[0], 0, 0, 0) == 0);
    CHECK(Bc250HangSchedulerBoundary(&states[0], 1, 41u, &fence) && fence == 41u);
    CHECK(Bc250HangReportBegin(&states[0]));

    /* The window at the restart. */
    CHECK(Bc250AdapterWindowOutcome(0, 0) == BC250_ADAPTER_WINDOW_NONE);
    CHECK(Bc250AdapterWindowOutcome(3, 0) == BC250_ADAPTER_WINDOW_COMPLETE);
    CHECK(Bc250AdapterWindowOutcome(3, 1) == BC250_ADAPTER_WINDOW_CLOSE);
    CHECK(Bc250AdapterWindowOutcome(1, 1) == BC250_ADAPTER_WINDOW_CLOSE);

    /* The record on the adapter path: PENDING first, then 9 or 10. Every finished call stays balanced; a call the
     * machine did not survive leaves Attempts one ahead and LastVerdict 0, as on the engine path. */
    record_write(&r, BC250_HANG_VERDICT_PENDING);
    record_write(&r, BC250_HANG_VERDICT_ADAPTER_RESET);
    CHECK(balanced(&r) && r.recovered == 1);
    record_write(&r, BC250_HANG_VERDICT_PENDING);
    record_write(&r, BC250_HANG_VERDICT_ADAPTER_BUSY);
    CHECK(balanced(&r) && r.refused == 1 && r.lastVerdict == BC250_HANG_VERDICT_ADAPTER_BUSY);
    record_write(&r, BC250_HANG_VERDICT_PENDING);
    CHECK(r.attempts == r.recovered + r.notDrained + r.refused + 1 && r.lastVerdict == 0);
    CHECK(BC250_HANG_PATH_ENGINE != BC250_HANG_PATH_ADAPTER);

    printf("PASS: adapter recovery switch, idle witnesses per sample and register (SDMA0 pointers only with the paging "
           "ring in use, all-ones busy), read-list admission of every witness, all-or-nothing begin, fence advance, "
           "commit and restart, window outcome, and the adapter path's record counters\n");
    return 0;
}
