/* Deterministic interleavings over production recovery helpers. No kernel or GPU calls. */
#include <stdio.h>
#include <string.h>
#include "hang_recovery.h"


static unsigned checks;
static unsigned failures;
#define CHECK(x) do { checks++; if (!(x)) { printf("FAIL CHECK line %u: %s\n", (unsigned)__LINE__, #x); failures++; } } while (0)

#include "timeout_latch.inc"

static void complete(BC250_HANG_NODE_STATE *s, unsigned fence)
{
    Bc250HangObserveCompleted(s, fence);
}

static int abort_at(BC250_HANG_NODE_STATE *s, unsigned notified, unsigned submitted, unsigned *out)
{
    return Bc250HangAbortCompletedFence(s, 0, 0, 1, notified, 1, submitted, out);
}

static void publication(void)
{
    BC250_HANG_NODE_STATE s = {0};
    unsigned out = 0xdeadbeefu;
    CHECK(!abort_at(&s, 7, 9, &out)); /* A notification alone is not a hardware witness. */
    CHECK(out == 0xdeadbeefu);
    complete(&s, 7);
    CHECK(abort_at(&s, 7, 9, &out) && out == 7);
    complete(&s, 8);
    CHECK(!abort_at(&s, 7, 9, &out)); /* Completion pending was cleared before publication. */
    Bc250HangReportBegin(&s);
    CHECK(!abort_at(&s, 8, 9, &out)); /* even if notified store raced ahead */
    CHECK(!Bc250HangResetBegin(&s, 0));
    Bc250HangReportEnd(&s, 8, 1, 0);
    CHECK(abort_at(&s, 8, 9, &out) && out == 8);
    complete(&s, 9);
    Bc250HangReportBegin(&s);
    Bc250HangReportEnd(&s, 9, 0, 1); /* retry exhaustion */
    CHECK(!abort_at(&s, 8, 10, &out));
    CHECK(!abort_at(&s, 9, 10, &out)); /* Lost is independent of numeric coincidence. */
    CHECK(!Bc250HangAbortCompletedFence(&s, 1, 0, 1, 9, 1, 10, &out));
    CHECK(!Bc250HangAbortCompletedFence(&s, 0, 1, 1, 9, 1, 10, &out));
    CHECK(Bc250HangReportBegin(&s));
    Bc250HangReportEnd(&s, 8, 1, 0);
    CHECK(s.ReportLost); /* An older report cannot erase the newer dropped report. */
    CHECK(Bc250HangReportBegin(&s));
    Bc250HangReportEnd(&s, 9, 1, 0);
    CHECK(!s.ReportLost && abort_at(&s, 9, 10, &out) && out == 9);
}

static void node_and_wrap(void)
{
    BC250_HANG_NODE_STATE nodes[2] = {{0}};
    unsigned out;
    complete(&nodes[0], 0xfffffffeu);
    complete(&nodes[1], 9000);
#ifdef SHARE_NODE_WATERMARK
    nodes[0] = nodes[1];
#endif
    CHECK(abort_at(&nodes[0], 0xfffffffeu, 1, &out) && out == 0xfffffffeu);
    complete(&nodes[0], 0xffffffffu);
    complete(&nodes[0], 0);
    CHECK(abort_at(&nodes[0], 0, 1, &out) && out == 0);
    CHECK(nodes[1].CompletedFence == 9000);
    CHECK(!abort_at(&nodes[0], 0, 0xffffffffu, &out));
    CHECK(!Bc250HangAbortCompletedFence(&nodes[0], 0, 0, 0, 0, 1, 1, &out));
    CHECK(!Bc250HangAbortCompletedFence(&nodes[0], 0, 0, 1, 0, 0, 1, &out));
}

static void reset_protocol(void)
{
    BC250_HANG_NODE_STATE s = {0};
    unsigned long long old_epoch;
    unsigned out;
    complete(&s, 10);
    CHECK(!Bc250HangResetBegin(&s, 1));
    old_epoch = s.Epoch;
    CHECK(Bc250HangTimeoutCurrent(&s, old_epoch));
    CHECK(Bc250HangResetBegin(&s, 0));
    CHECK(!Bc250HangResetBegin(&s, 0));
    CHECK(!Bc250HangReportBegin(&s));
    CHECK(!Bc250HangTimeoutCurrent(&s, old_epoch));
    CHECK(!Bc250HangTimeoutCurrent(&s, s.Epoch));
    Bc250HangResetEnd(&s, 1, 12);
    CHECK(!s.ResetActive);
    CHECK(s.CompletedFence == 10); /* Reset boundary is NOT a hardware completion. */
    CHECK(s.BoundaryKnown && s.BoundaryFence == 12);
    CHECK(!Bc250HangTimeoutCurrent(&s, old_epoch));
    CHECK(Bc250HangTimeoutCurrent(&s, s.Epoch));
    CHECK(!abort_at(&s, 10, 13, &out)); /* Historical completion precedes reset boundary. */
    complete(&s, 13);
    CHECK(abort_at(&s, 13, 14, &out) && out == 13);
    CHECK(Bc250HangResetBegin(&s, 0));
    Bc250HangResetEnd(&s, 0, 15);
    CHECK(s.CompletedFence == 13 && s.BoundaryFence == 12);
    CHECK(Bc250HangReportBegin(&s));
    Bc250HangReportEnd(&s, 13, 1, 0);
    CHECK(!s.BoundaryKnown); /* A covered historical boundary must leave modular comparisons. */
    complete(&s, 0x8000000eu);
    CHECK(abort_at(&s, 0x8000000eu, 0x8000000fu, &out) && out == 0x8000000eu);
}

/* Admission tests run the extracted production loop. The fake clock advances only at waits;
 * a 600-wait safety trip makes a broken deadline terminate, then fail the 500 ms CHECK. */
static void admission_setup(BC250_DEVICE *d, BC250_WDDM *w, BC250_GFX *g)
{
    memset(d, 0, sizeof(*d));
    memset(w, 0, sizeof(*w));
    memset(g, 0, sizeof(*g));
    d->Gfx = g;
    g->SubmitAdev = g;
    MockWddm = w;
    MockDevice = d;
    MockNow = 10000000ull;
    MockStart = MockNow;
    MockDrainMs = ~0u;
    MockSecondaryMs = ~0u;
    MockFaultMs = ~0u;
    MockWaits = MockLockDepth = MockSafetyTrips = MockPoisonRelease = 0;
}

static void admission_reasons(void)
{
    unsigned bits;
    for (bits = 0; bits < 64; bits++) {
        BC250_HANG_NODE_STATE s = {0};
        s.ReportLost = !!(bits & 4);
        s.ReportInFlight = !!(bits & 8);
        s.ResetActive = !!(bits & 32);
        CHECK(Bc250HangAdmissionReasons(&s, !!(bits & 1), !!(bits & 2), !!(bits & 16)) == bits);
    }
    CHECK(BC250_HANG_VERDICT_ADMISSION_GUARD == 8u);
    CHECK(!Bc250HangVerdictRecovered(BC250_HANG_VERDICT_ADMISSION_GUARD));
    CHECK(Bc250HangVerdictCounts(BC250_HANG_VERDICT_ADMISSION_GUARD) ==
          (BC250_HANG_COUNT_ATTEMPT | BC250_HANG_COUNT_REFUSED));
}

/* Success deliberately returns the production lock held so the caller can snapshot atomically. */
static unsigned TestResetAdmit(BC250_DEVICE *d, BC250_WDDM *w, unsigned node)
{
    KIRQL irql = 0;
    unsigned reasons = WddmResetAdmit(d, w, node, &irql);
    CHECK(MockLockDepth == (reasons ? 0u : 1u));
    if (!reasons) KeReleaseSpinLock(&w->Lock, irql);
    return reasons;
}

static void admission_paths(void)
{
    BC250_DEVICE d;
    BC250_WDDM w;
    BC250_GFX g;
    unsigned bits, reasons, i;
    static const unsigned drains[] = {100, 499};
    admission_setup(&d, &w, &g);
    CHECK(TestResetAdmit(&d, &w, 0) == 0);
    CHECK(w.Recovery[0].ResetActive && w.WatchdogFaulted[0] && g.SubmitFailed);
    CHECK(!d.HealthFault && d.Calls == 0 && MockWaits == 0 && MockLockDepth == 0);
    /* Model the successful locked commit after the actual admission. It does not invoke admission again. */
    w.WatchdogFaulted[0] = 0;
    g.SubmitFailed = 0;
    d.GfxClosed = 0;
    Bc250HangResetEnd(&w.Recovery[0], 1, 5);
    CHECK(!d.HealthFault && !w.Recovery[0].ResetActive && !w.WatchdogFaulted[0] && !g.SubmitFailed);
    admission_setup(&d, &w, &g);
    CHECK(!TestResetAdmit(&d, &w, 0));
    /* A later refusal ends the transaction but leaves both submission gates closed. */
    Bc250HangResetEnd(&w.Recovery[0], 0, 0);
    CHECK(!w.Recovery[0].ResetActive && w.WatchdogFaulted[0] && g.SubmitFailed && !d.HealthFault);
    CHECK(run_refusal_tail(&d) == STATUS_NOT_SUPPORTED);
    CHECK(w.WatchdogFaulted[0] && g.SubmitFailed && d.HealthFault);
    /* A successful recovery does not invent a health fault, nor erase an earlier real fault. */
    admission_setup(&d, &w, &g);
    d.HealthFault = 1;
    CHECK(!TestResetAdmit(&d, &w, 0));
    Bc250HangResetEnd(&w.Recovery[0], 1, 5);
    CHECK(d.HealthFault && d.Calls == 0);
    admission_setup(&d, &w, &g);
    GfxSubmitFail(&d);
    CHECK(d.HealthFault && d.Calls == 1 && g.SubmitFailed);

    /* Every early refusal preserves gates/epoch and records all simultaneous reasons. */
    for (bits = 1; bits < 64; bits++) {
        if (bits == 16) continue; /* sole active submit has a bounded drain path */
        admission_setup(&d, &w, &g);
        w.Stopping = !!(bits & 1);
        w.CompletionPending[0] = !!(bits & 2);
        w.Recovery[0].ReportLost = !!(bits & 4);
        w.Recovery[0].ReportInFlight = !!(bits & 8);
        w.ActiveSubmissions[0] = !!(bits & 16);
        w.Recovery[0].ResetActive = !!(bits & 32);
        reasons = TestResetAdmit(&d, &w, 0);
        CHECK(reasons == bits && MockWaits == 0);
        CHECK(!w.WatchdogFaulted[0] && !g.SubmitFailed && !d.HealthFault && w.Recovery[0].Epoch == 0);
        CHECK(w.Recovery[0].ResetActive == !!(bits & 32) && MockLockDepth == 0);
        CHECK(run_refusal_tail(&d) == STATUS_NOT_SUPPORTED);
        CHECK(!w.WatchdogFaulted[0] && !g.SubmitFailed && d.HealthFault);
    }
    for (i = 0; i < sizeof(drains)/sizeof(drains[0]); i++) {
        admission_setup(&d, &w, &g);
        w.ActiveSubmissions[0] = 1;
        MockDrainMs = drains[i];
        CHECK(TestResetAdmit(&d, &w, 0) == 0);
        CHECK((MockNow - MockStart)/10000ull == drains[i] && MockWaits == drains[i]);
        CHECK(w.Recovery[0].ResetActive && w.WatchdogFaulted[0] && g.SubmitFailed);
        CHECK(!d.HealthFault && MockSafetyTrips == 0 && MockLockDepth == 0 && d.DrainWakes == 1);
    }
    admission_setup(&d, &w, &g);
    w.ActiveSubmissions[0] = 1;
    CHECK(TestResetAdmit(&d, &w, 0) == 16u);
    CHECK(d.DrainWakes == 1);
    CHECK(MockWaits == 500 && MockNow - MockStart == 500ull * 10000ull && MockSafetyTrips == 0);
    CHECK(!w.Recovery[0].ResetActive && !w.WatchdogFaulted[0] && !g.SubmitFailed && !d.HealthFault);
    admission_setup(&d, &w, &g);
    w.ActiveSubmissions[0] = 1;
    MockSecondaryMs = 100;
    CHECK(TestResetAdmit(&d, &w, 0) == (16u | 4u));
    CHECK(MockWaits == 100 && !w.Recovery[0].ResetActive && !g.SubmitFailed && MockSafetyTrips == 0);
    CHECK(MockLockDepth == 0);
    /* A real hold-path failure during the wait stays faulted after admission succeeds. */
    admission_setup(&d, &w, &g);
    w.ActiveSubmissions[0] = 1;
    MockFaultMs = 300;
    MockDrainMs = 301;
    CHECK(!TestResetAdmit(&d, &w, 0));
    CHECK(MockWaits == 301 && d.HealthFault && d.Calls == 1);
    CHECK(w.Recovery[0].ResetActive && w.WatchdogFaulted[0] && g.SubmitFailed);
}

static void submission_freeze(void)
{
    BC250_DEVICE d;
    BC250_WDDM w;
    BC250_GFX g;
    admission_setup(&d, &w, &g);
    CHECK(WddmBeginSubmissionLocked(&w, 0, 17));
    CHECK(w.ActiveSubmissions[0] == 1 && w.NoteCalls == 1 && w.NotedFence[0] == 17);
    w.Recovery[0].ResetActive = 1;
    CHECK(!WddmBeginSubmissionLocked(&w, 0, 18));
    CHECK(w.ActiveSubmissions[0] == 1 && w.NoteCalls == 2 && w.NotedFence[0] == 18);
    CHECK(WddmBeginSubmissionLocked(&w, 1, 23));
    CHECK(w.ActiveSubmissions[1] == 1 && w.NoteCalls == 3 && w.NotedFence[1] == 23);
    CHECK(PRODUCTION_ADMISSION_PROTOCOL);
}

int main(void)
{
    publication();
    node_and_wrap();
    reset_protocol();
    test_timeout_latch();
    admission_reasons();
    admission_paths();
    submission_freeze();
    printf("recovery protocol: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
