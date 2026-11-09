/* Stage 2 of the hang recovery, the KMD half (docs/design/queue-reset.md section 8): the pure decisions that gfx.c
 * and wddm.c take around the shim's queue resets. The shim's packets and step order are checked by
 * driver/shim/test/queue_reset.c. This test fixes:
 *   - when stage 2 runs: HangRecoveryMode on, node 0 or 2, after a kill loop that did not drain, and only where the
 *     device has that queue reset (queue_reset_caps.h, also fixed here with amdgpu's firmware gates).
 *   - which failed SDMA0 queue-reset steps leave the engine unproven (GpuStopUnconfirmed).
 *   - the wave proof: a complete capture with no wave of the hung VMID, and nothing less.
 *   - the record: QUEUE_RESET counts as recovered, QUEUE_RESET_FAILED as not drained, and both keep the
 *     invariant Attempts == Recovered + NotDrained + Refused after the PENDING write.
 *   - the VMID: the hung tenancy goes to the history and the VMID holds no root afterwards.
 * run_queue_reset.ps1 also reads wddm.c for the place of the stage-2 call. */
#include <stdio.h>
#include <string.h>
/* These two first, so that run_queue_reset.ps1's copies next to this file are the ones every later header gets
 * (their include guards keep the copies in driver\kmd out). */
#include "queue_reset_caps.h"
#include "hang_recovery.h"
#include "queue_reset.h"
#include "vmid_pool.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static BC250_WAVE_SNAPSHOT g_snap;

static void snap_complete(unsigned count)
{
    memset(&g_snap, 0, sizeof(g_snap));
    g_snap.Header.Flags = BC250_WAVE_COMPLETE;
    g_snap.Header.WaveCount = count;
}

static void wave_vmid(unsigned i, unsigned vmid)
{
    g_snap.Waves[i].Raw[Bc250WaveHwId2] = (vmid << SQ_WAVE_HW_ID2__VM_ID__SHIFT) | 0x00A5005Au;
}

#define ALL_CAPS (BC250_QRESET_CAP_COMPUTE | BC250_QRESET_CAP_GFX | BC250_QRESET_CAP_SDMA)

static int test_wanted(void)
{
    CHECK(BC250_QRESET_GFX_DEFAULT == 1);   /* tester.38: node 0 in both flavours (BD-180 point 3) */
    CHECK(Bc250QueueResetWanted(ALL_CAPS, 1, 1, 0, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(Bc250QueueResetWanted(ALL_CAPS, 1, 1, 2, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    /* Node 0 without the lab selector: the release flavour, since tester.38. */
    CHECK(Bc250QueueResetWanted(ALL_CAPS, 0, 1, 0, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(Bc250QueueResetWanted(ALL_CAPS, 2, 1, 0, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 1, 0, 0, BC250_HANG_VERDICT_NOT_DRAINED, 1));   /* HangRecoveryMode 0 */
    /* Node 2 in both flavours (Q2 of 2026-11-04): the selector (0 in the release flavour) does not matter. */
    CHECK(Bc250QueueResetWanted(ALL_CAPS, 0, 1, 2, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(Bc250QueueResetWanted(ALL_CAPS, 2, 1, 2, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 0, 0, 2, BC250_HANG_VERDICT_NOT_DRAINED, 1));   /* HangRecoveryMode 0 */
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 0, 1, 2, BC250_HANG_VERDICT_NOT_DRAINED, 0));   /* no kill loop ran */
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 0, 1, 2, BC250_HANG_VERDICT_DRAINED, 1));
    /* Node 1 never in ResetEngine: its reset is ResetFromTimeout's (queue-reset.md section 13). */
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 1, 1, 1, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 1, 1, 0, BC250_HANG_VERDICT_NOT_DRAINED, 0));   /* no kill loop ran */
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 1, 1, 0, BC250_HANG_VERDICT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 1, 1, 0, BC250_HANG_VERDICT_ALREADY_RETIRED, 1));
    CHECK(!Bc250QueueResetWanted(ALL_CAPS, 1, 1, 0, BC250_HANG_VERDICT_VMID_GUARD, 1));
    /* The device decides: no capability, no reset, whatever the selectors say. */
    CHECK(!Bc250QueueResetWanted(0, 1, 1, 0, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(0, 1, 1, 2, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(BC250_QRESET_CAP_COMPUTE, 1, 1, 0, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(Bc250QueueResetWanted(BC250_QRESET_CAP_COMPUTE, 1, 1, 2, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(BC250_QRESET_CAP_GFX, 1, 1, 2, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(Bc250QueueResetWanted(BC250_QRESET_CAP_GFX, 1, 1, 0, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    CHECK(!Bc250QueueResetWanted(BC250_QRESET_CAP_SDMA, 1, 1, 1, BC250_HANG_VERDICT_NOT_DRAINED, 1));
    return 0;
}

/* queue_reset_caps.h: amdgpu's declarations, what the shim implements, and the one measured deviation. */
static int test_caps(void)
{
    const unsigned gc1013 = BC250_IP_VERSION(10, 1, 3), sdma501 = BC250_IP_VERSION(5, 0, 1);
    BC250_ASIC_IP ip;

    CHECK(BC250_IP_VERSION(10, 1, 3) == 0x0A0103u && BC250_IP_MAJOR(gc1013) == 10u && BC250_IP_MINOR(gc1013) == 1u);
    /* amdgpu: GC 10 has both KIQ resets; SDMA 5.0.1 is not in sdma_v5_0.c's list at any firmware. */
    CHECK(Bc250QueueResetUpstreamCaps(gc1013, sdma501, 999u) == (BC250_QRESET_CAP_COMPUTE | BC250_QRESET_CAP_GFX));
    /* The firmware gates of sdma_v5_0.c and sdma_v5_2.c, both sides of each threshold. */
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 0, 0), 35u) == BC250_QRESET_CAP_SDMA);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 0, 0), 34u) == 0);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 0, 2), 35u) == BC250_QRESET_CAP_SDMA);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 0, 5), 35u) == BC250_QRESET_CAP_SDMA);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 0, 5), BC250_SDMA_FW_UNKNOWN) == 0);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 2, 0), 76u) == BC250_QRESET_CAP_SDMA);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 2, 4), 75u) == 0);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 2, 5), 34u) == BC250_QRESET_CAP_SDMA);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 2, 5), 33u) == 0);
    CHECK(Bc250QueueResetUpstreamCaps(0, BC250_IP_VERSION(5, 2, 1), 999u) == 0);
    CHECK(Bc250QueueResetUpstreamCaps(BC250_IP_VERSION(11, 0, 0), 0, 0) == 0);   /* GFX11: MES, not this path */
    /* The shim implements GC 10.1 and 10.3 and SDMA 5.0 only: an SDMA 5.2 part gets no SDMA reset here. */
    CHECK(Bc250QueueResetImplementedCaps(BC250_IP_VERSION(10, 3, 0), BC250_IP_VERSION(5, 2, 0)) ==
          (BC250_QRESET_CAP_COMPUTE | BC250_QRESET_CAP_GFX));
    CHECK(Bc250QueueResetImplementedCaps(gc1013, sdma501) == ALL_CAPS);
    CHECK(Bc250QueueResetImplementedCaps(BC250_IP_VERSION(10, 2, 0), 0) == 0);
    /* The table: the BC-250 by its PCI id, nothing for another vendor or device. */
    CHECK(Bc250AsicIpLookup(0x1002u, 0x13FEu, &ip) && ip.Gc == gc1013 && ip.Sdma == sdma501);
    CHECK(!Bc250AsicIpLookup(0x1022u, 0x13FEu, &ip));
    CHECK(!Bc250AsicIpLookup(0x1002u, 0x15BFu, &ip));
    /* M943 adds SDMA on this device only, without any firmware gate. */
    CHECK(Bc250QueueResetCaps(0x1002u, 0x13FEu, BC250_SDMA_FW_UNKNOWN) == ALL_CAPS);
    CHECK(Bc250QueueResetCaps(0x1002u, 0x15BFu, 999u) == 0);
    CHECK(Bc250QueueResetCaps(0, 0, 0) == 0);
    return 0;
}

/* The SDMA0 queue reset: which failed steps leave the engine unproven (queue_reset.h). */
static int test_sdma_steps(void)
{
    unsigned stage;

    CHECK(BC250_SDMA_QRESET_STEP_BASE > BC250_QRESET_STEP_SEQUENCE);   /* no overlap with the KIQ steps */
    CHECK(!Bc250SdmaQueueResetUnconfirmed(BC250_SDMA_QRESET_STEP_BASE + 0u));   /* argument check: no access */
    for (stage = 1; stage <= 6; stage++) CHECK(Bc250SdmaQueueResetUnconfirmed(BC250_SDMA_QRESET_STEP_BASE + stage));
    CHECK(Bc250SdmaQueueResetUnconfirmed(BC250_SDMA_QRESET_STEP_RING_TEST));
    CHECK(Bc250SdmaQueueResetUnconfirmed(BC250_SDMA_QRESET_STEP_SEQUENCE));
    CHECK(!Bc250SdmaQueueResetUnconfirmed(BC250_SDMA_QRESET_STEP_ARGS));
    CHECK(!Bc250SdmaQueueResetUnconfirmed(BC250_SDMA_QRESET_STEP_DONE));
    CHECK(!Bc250QueueResetKiqStep(BC250_SDMA_QRESET_STEP_BASE + 2u));
    return 0;
}

static int test_wave_proof(void)
{
    snap_complete(0);
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == 0);
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 0, 13) == BC250_QRESET_STEP_WAVES);
    snap_complete(3);
    wave_vmid(0, 4); wave_vmid(1, 7); wave_vmid(2, 12);
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == 0);         /* other processes */
    wave_vmid(2, 13);
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == BC250_QRESET_STEP_WAVES);
    snap_complete(1);
    wave_vmid(0, 15);
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 15) == BC250_QRESET_STEP_WAVES);
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 14) == 0);
    snap_complete(0);
    g_snap.Header.Flags = 0;
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == BC250_QRESET_STEP_WAVES);
    g_snap.Header.Flags = BC250_WAVE_COMPLETE | BC250_WAVE_TRUNCATED;
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == BC250_QRESET_STEP_WAVES);
    g_snap.Header.Flags = BC250_WAVE_COMPLETE | BC250_WAVE_BUDGET_EXPIRED;
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == BC250_QRESET_STEP_WAVES);
    g_snap.Header.Flags = BC250_WAVE_COMPLETE | BC250_WAVE_READ_ERROR;
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == BC250_QRESET_STEP_WAVES);
    snap_complete(BC250_WAVE_MAX_RECORDS + 1u);
    CHECK(Bc250QueueResetWaveProof(&g_snap.Header, g_snap.Waves, 1, 13) == BC250_QRESET_STEP_WAVES);
    CHECK(Bc250QueueResetWaveProof(0, g_snap.Waves, 1, 13) == BC250_QRESET_STEP_WAVES);
    return 0;
}

static int test_record(void)
{
    unsigned pending = Bc250HangVerdictCounts(BC250_HANG_VERDICT_PENDING);
    unsigned ok = Bc250HangVerdictCounts(BC250_HANG_VERDICT_QUEUE_RESET);
    unsigned bad = Bc250HangVerdictCounts(BC250_HANG_VERDICT_QUEUE_RESET_FAILED);
    CHECK(pending == BC250_HANG_COUNT_ATTEMPT);
    CHECK(ok == BC250_HANG_COUNT_RECOVERED);         /* the attempt was counted by PENDING */
    CHECK(bad == BC250_HANG_COUNT_NOT_DRAINED);
    CHECK(Bc250HangVerdictRecovered(BC250_HANG_VERDICT_QUEUE_RESET));
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_QUEUE_RESET_FAILED));
    CHECK(Bc250QueueResetKiqStep(2) && Bc250QueueResetKiqStep(4) && Bc250QueueResetKiqStep(6));
    CHECK(!Bc250QueueResetKiqStep(3) && !Bc250QueueResetKiqStep(7) && !Bc250QueueResetKiqStep(BC250_QRESET_STEP_WAVES));
    return 0;
}

static BC250_VMID_TABLE g_table;
static BC250_VMID_HISTORY_RING g_history;

static int test_vmid(void)
{
    memset(&g_table, 0, sizeof(g_table));
    memset(&g_history, 0, sizeof(g_history));
    Bc250VmidClaim(&g_table, &g_history, 13, 0x12345000ull);
    Bc250VmidSubmitted(&g_table, 13, 500, 77);
    Bc250VmidClaim(&g_table, &g_history, 4, 0x55555000ull);
    Bc250VmidSubmittedCompute(&g_table, 4, 501, 78);
    Bc250VmidForget(&g_table, &g_history, 13);
    CHECK(g_table.Root[13] == 0 && g_table.LiveSeq[13] == 0 && g_table.LiveSeqCompute[13] == 0);
    CHECK(g_history.Next == 1 && g_history.Entry[0].Vmid == 13 && g_history.Entry[0].Root == 0x12345000ull &&
          g_history.Entry[0].LastSeq == 500 && g_history.Entry[0].Process == 77);
    CHECK(g_table.Root[4] == 0x55555000ull && g_table.LiveSeqCompute[4] == 501);   /* another process: untouched */
    Bc250VmidForget(&g_table, &g_history, 13);                                    /* again: nothing to push */
    CHECK(g_history.Next == 1);
    Bc250VmidForget(&g_table, &g_history, BC250_VMID_COUNT);                      /* out of range: no write */
    CHECK(g_table.Root[4] == 0x55555000ull);
    return 0;
}

int main(void)
{
    if (test_wanted() || test_caps() || test_sdma_steps() || test_wave_proof() || test_record() || test_vmid())
        return 1;
    printf("queue_reset_test: all checks passed\n");
    return 0;
}
