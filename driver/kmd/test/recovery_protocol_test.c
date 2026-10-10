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

int main(void)
{
    publication();
    node_and_wrap();
    reset_protocol();
    test_timeout_latch();
    printf("recovery protocol: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
