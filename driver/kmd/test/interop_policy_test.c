/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// The interop switches' start policy (interop_policy.c) against an independent oracle over every input
// combination, then the life of the switches across boots on a simulated Parameters key.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../interop_policy.h"
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

// Switch states: absent, 0, 1, 2 (invalid), 0xFFFFFFFF (invalid), unreadable.
static const struct bc250_interop_value g_Switch[] = {
    {BC250_INTEROP_ABSENT, 0}, {BC250_INTEROP_PRESENT, 0}, {BC250_INTEROP_PRESENT, 1},
    {BC250_INTEROP_PRESENT, 2}, {BC250_INTEROP_PRESENT, 0xFFFFFFFFu}, {BC250_INTEROP_UNREADABLE, 0}
};
static const struct bc250_interop_value g_Marker[] = {
    {BC250_INTEROP_ABSENT, 0}, {BC250_INTEROP_PRESENT, 7}, {BC250_INTEROP_UNREADABLE, 0}
};
static const struct bc250_interop_value g_Closed[] = {
    {BC250_INTEROP_ABSENT, 0}, {BC250_INTEROP_PRESENT, 4}, {BC250_INTEROP_PRESENT, 2}, {BC250_INTEROP_UNREADABLE, 0}
};

static void Oracle(const struct bc250_interop_inputs* in, struct bc250_interop_decision* o)
{
    const struct bc250_interop_value* s[2] = {&in->blit, &in->cdd};
    unsigned int req = 0, i;
    int zero = 1, badRead = 0, badValue = 0;
    int stale = in->session.state == BC250_INTEROP_PRESENT && in->boot_record;
    int dirty = in->session.state != BC250_INTEROP_ABSENT && !stale;
    unsigned int previous = in->closed.state == BC250_INTEROP_PRESENT ? in->closed.value : 0;
    memset(o, 0, sizeof(*o));
    for (i = 0; i < 2; i++) {
        unsigned int bit = i ? BC250_INTEROP_CDD : BC250_INTEROP_BLIT;
        if (!(s[i]->state == BC250_INTEROP_PRESENT && s[i]->value == 0)) zero = 0;
        if (s[i]->state == BC250_INTEROP_ABSENT || (s[i]->state == BC250_INTEROP_PRESENT && s[i]->value == 1)) req |= bit;
        if (s[i]->state == BC250_INTEROP_UNREADABLE) badRead = 1;
        if (s[i]->state == BC250_INTEROP_PRESENT && s[i]->value > 1) badValue = 1;
    }
    o->requested = req;
    o->flags = (stale ? BC250_INTEROP_DECISION_STALE : 0) | (dirty ? BC250_INTEROP_DECISION_UNCLEAN : 0);
    o->clear_session = stale || dirty;
    o->closed_reason = previous;
    if (dirty && !zero) {
        o->reason = 4; o->closed_reason = 4; o->persist_close = 1;
        o->flags |= BC250_INTEROP_DECISION_CLOSED_BY_DRIVER;
    } else if (badRead) o->reason = 5;
    else if (badValue) o->reason = 2;
    else if (!req) { o->reason = 1; if (previous) o->flags |= BC250_INTEROP_DECISION_CLOSED_BY_DRIVER; }
    else {
        o->effective = req; o->reason = 0;
        if (in->closed.state != BC250_INTEROP_ABSENT) { o->clear_closed = 1; o->closed_reason = 0; }
    }
}

// ---- a simulated Parameters key and the start/stop sequence interop.c runs ----------------------------------------

typedef struct { int present; unsigned int value; } VALUE;
typedef struct { VALUE blit, cdd, session, closed; int bootRecord; int failWrites; } REGISTRY;

static struct bc250_interop_value Read(const VALUE* v)
{
    struct bc250_interop_value r;
    r.state = v->present ? BC250_INTEROP_PRESENT : BC250_INTEROP_ABSENT;
    r.value = v->present ? v->value : 0;
    return r;
}

// What InteropStart does with the decision (interop.c InteropStart), with the same ordering: the durable
// close first, the marker is deleted only after the close reached the disk.
static struct bc250_interop_decision Start(REGISTRY* r)
{
    struct bc250_interop_inputs in;
    struct bc250_interop_decision d;
    in.blit = Read(&r->blit); in.cdd = Read(&r->cdd); in.session = Read(&r->session); in.closed = Read(&r->closed);
    in.boot_record = r->bootRecord;
    bc250_interop_decide(&in, &d);
    if (d.persist_close) {
        if (r->failWrites) return d;
        r->blit.present = r->cdd.present = r->closed.present = 1;
        r->blit.value = r->cdd.value = 0;
        r->closed.value = d.reason;
    }
    if (d.clear_closed && !r->failWrites) r->closed.present = 0;
    if (d.clear_session && !r->failWrites) r->session.present = 0;
    return d;
}
static void Use(REGISTRY* r) { r->bootRecord = 1; r->session.present = 1; r->session.value = 42; }  // InteropUserBegin
static void End(REGISTRY* r) { r->session.present = 0; }                                             // last user, stop
static void Reboot(REGISTRY* r) { r->bootRecord = 0; }

int main(void)
{
    unsigned int a, b, m, c, k, cases = 0;
    struct bc250_interop_decision got, want;
    REGISTRY r;

    for (a = 0; a < 6; a++) for (b = 0; b < 6; b++) for (m = 0; m < 3; m++) for (c = 0; c < 4; c++) for (k = 0; k < 2; k++) {
        struct bc250_interop_inputs in;
        in.blit = g_Switch[a]; in.cdd = g_Switch[b]; in.session = g_Marker[m]; in.closed = g_Closed[c];
        in.boot_record = (int)k;
        memset(&got, 0xA5, sizeof(got));
        bc250_interop_decide(&in, &got);
        Oracle(&in, &want);
        CHECK(got.requested == want.requested);
        CHECK(got.effective == want.effective);
        CHECK(got.reason == want.reason);
        CHECK(got.closed_reason == want.closed_reason);
        CHECK(got.flags == want.flags);
        CHECK(got.persist_close == want.persist_close);
        CHECK(got.clear_closed == want.clear_closed);
        CHECK(got.clear_session == want.clear_session);
        // Invariants, independent of the oracle's shape.
        CHECK((got.effective & ~got.requested) == 0);
        CHECK(got.effective == 0 || got.reason == BC250_INTEROP_REASON_NONE);
        CHECK(!(got.flags & BC250_INTEROP_DECISION_UNCLEAN) || got.effective == 0);
        CHECK(!got.persist_close || (got.effective == 0 && got.reason == BC250_INTEROP_REASON_UNCLEAN));
        CHECK(!(got.flags & BC250_INTEROP_DECISION_STALE) || !got.persist_close);
        CHECK(!got.clear_closed || got.effective != 0);
        cases++;
    }
    CHECK(cases == 6u * 6u * 3u * 4u * 2u);

    // Named cases. Nothing set: the new default is on, both.
    memset(&r, 0, sizeof(r));
    got = Start(&r);
    CHECK(got.effective == BC250_INTEROP_ALL && got.reason == 0 && !got.persist_close);
    // The lab's Parameters today (kit Configure): both explicit 0, CPU desktop.
    memset(&r, 0, sizeof(r));
    r.blit.present = r.cdd.present = 1;
    got = Start(&r);
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_NOT_REQUESTED && got.flags == 0);
    // One switch off, the other absent: only the absent one opens.
    memset(&r, 0, sizeof(r));
    r.blit.present = 1;
    got = Start(&r);
    CHECK(got.requested == BC250_INTEROP_CDD && got.effective == BC250_INTEROP_CDD);
    CHECK(strcmp(bc250_interop_reason_name(4), "unclean") == 0 && strcmp(bc250_interop_reason_name(99), "unknown") == 0);
    CHECK(strcmp(bc250_interop_reason_name(BC250_INTEROP_REASON_NOT_RUN), "not-run") == 0);

    // A clean life: used, last user gone before the reboot, open again after it.
    memset(&r, 0, sizeof(r));
    got = Start(&r); Use(&r); End(&r); Reboot(&r);
    got = Start(&r);
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == 0);

    // A driver restart within one boot with the marker left behind: stale, nothing closes.
    memset(&r, 0, sizeof(r));
    got = Start(&r); Use(&r);
    got = Start(&r);
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == BC250_INTEROP_DECISION_STALE && !r.session.present);

    // Death in a session: the next start closes both, durably, and says why.
    memset(&r, 0, sizeof(r));
    got = Start(&r); Use(&r); Reboot(&r);
    got = Start(&r);
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_UNCLEAN && got.persist_close);
    CHECK(r.blit.present && r.blit.value == 0 && r.cdd.present && r.cdd.value == 0);
    CHECK(r.closed.present && r.closed.value == 4 && !r.session.present);
    // The boot after: still closed, now by the switches themselves; the driver's reason stays visible.
    Reboot(&r);
    got = Start(&r);
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_NOT_REQUESTED);
    CHECK(got.closed_reason == 4 && (got.flags & BC250_INTEROP_DECISION_CLOSED_BY_DRIVER));
    // The operator opens them again (a 1 each, or deleting both): the driver's mark goes.
    r.blit.value = r.cdd.value = 1; Reboot(&r);
    got = Start(&r);
    CHECK(got.effective == BC250_INTEROP_ALL && got.clear_closed && !r.closed.present && got.closed_reason == 0);

    // The durable close did not reach the disk: the marker stays, so the next start closes again.
    memset(&r, 0, sizeof(r));
    got = Start(&r); Use(&r); Reboot(&r);
    r.failWrites = 1;
    got = Start(&r);
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_UNCLEAN && r.session.present && !r.blit.present);
    r.failWrites = 0; Reboot(&r);
    got = Start(&r);
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_UNCLEAN && !r.session.present && r.closed.value == 4);

    // A death with both already off: nothing to close, but the start still says it saw one.
    memset(&r, 0, sizeof(r));
    r.blit.present = r.cdd.present = 1; r.session.present = 1;
    got = Start(&r);
    CHECK(got.effective == 0 && got.reason == 1 && !got.persist_close && (got.flags & BC250_INTEROP_DECISION_UNCLEAN));
    CHECK(!r.closed.present);

    // Null input: not run, nothing to do.
    bc250_interop_decide(0, &got);
    CHECK(got.reason == BC250_INTEROP_REASON_NOT_RUN && got.effective == 0 && !got.persist_close);

    printf("interop policy: %u combinations and 8 lifecycles PASS\n", cases);
    return 0;
}
