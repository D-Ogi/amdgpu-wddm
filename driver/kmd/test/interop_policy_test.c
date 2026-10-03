/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
// The interop switches' start policy (interop_policy.c) against an independent oracle over every input
// combination, then the life of the switches across boots on a simulated Parameters key, then the session
// marker's transitions within a boot (BD-059: a clean restart ends the session at the system power transition).
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

// ---- BD-059: the session marker's life within a boot (bc250_interop_session_step) ---------------------------------

// The caller's part, as interop.c InteropEvent does it: the step's answer carried out on the simulated registry,
// marked set only when the write or delete reached it.
static struct bc250_interop_step Event(REGISTRY* r, struct bc250_interop_session* s, unsigned int ev, unsigned int* lastEnd)
{
    struct bc250_interop_step st;
    bc250_interop_session_step(s, ev, &st);
    if (st.unmark && !r->failWrites) { *lastEnd = st.unmark; r->session.present = 0; s->marked = 0; }
    if (st.mark && !r->failWrites) { r->bootRecord = 1; r->session.present = 1; r->session.value = 42; s->marked = 1; }
    return st;
}

// Every sequence of up to five events, each with the registry write succeeding or failing, against the rules
// stated independently of the step's code.
static unsigned int SessionSequences(void)
{
    static const unsigned int events[] = {BC250_INTEROP_EVENT_BEGIN, BC250_INTEROP_EVENT_END, BC250_INTEROP_EVENT_STOP,
                                          BC250_INTEROP_EVENT_SYSTEM_DOWN, BC250_INTEROP_EVENT_ADAPTER_DOWN,
                                          BC250_INTEROP_EVENT_UP};
    unsigned int len, code, total, i, n = 0;
    for (len = 1; len <= 5; len++) {
        for (total = 1, i = 0; i < len; i++) total *= 12;
        for (code = 0; code < total; code++) {
            struct bc250_interop_session s;
            unsigned int c = code, users = 0, down = 0;
            memset(&s, 0, sizeof(s));
            for (i = 0; i < len; i++, c /= 12) {
                const unsigned int ev = events[c % 12 / 2];
                const int ok = (c % 2) == 0;
                const int wasMarked = s.marked;
                struct bc250_interop_step st;
                bc250_interop_session_step(&s, ev, &st);
                // The model.
                if (ev == BC250_INTEROP_EVENT_BEGIN) users++;
                if (ev == BC250_INTEROP_EVENT_END && users) users--;
                if (ev == BC250_INTEROP_EVENT_STOP) users = 0;
                if (ev == BC250_INTEROP_EVENT_SYSTEM_DOWN || ev == BC250_INTEROP_EVENT_ADAPTER_DOWN) down = 1;
                if (ev == BC250_INTEROP_EVENT_UP) down = 0;
                CHECK(s.users == users && s.down == (int)down);
                // Never both, never a mark while down or over a marker, an unmark only of a marker.
                CHECK(!(st.mark && st.unmark));
                CHECK(!st.mark || (!wasMarked && !down));
                CHECK(!st.unmark || wasMarked);
                // What must happen.
                CHECK(st.mark == (!wasMarked && !down &&
                                  (ev == BC250_INTEROP_EVENT_BEGIN || (ev == BC250_INTEROP_EVENT_UP && users))));
                CHECK(st.unmark == (!wasMarked ? 0u
                    : ev == BC250_INTEROP_EVENT_STOP ? BC250_INTEROP_SESSION_END_STOP
                    : ev == BC250_INTEROP_EVENT_END && users == 0 ? BC250_INTEROP_SESSION_END_USERS
                    : ev == BC250_INTEROP_EVENT_SYSTEM_DOWN ? BC250_INTEROP_SESSION_END_SYSTEM_POWER
                    : ev == BC250_INTEROP_EVENT_ADAPTER_DOWN ? BC250_INTEROP_SESSION_END_ADAPTER_D3 : 0u));
                if (ok && st.mark) s.marked = 1;
                if (ok && st.unmark) s.marked = 0;
                n++;
            }
        }
    }
    return n;
}

static void SessionLifecycles(void)
{
    struct bc250_interop_session s;
    struct bc250_interop_decision got;
    struct bc250_interop_step st;
    unsigned int lastEnd;
    REGISTRY r;

#define BOOT() do { Reboot(&r); got = Start(&r); memset(&s, 0, sizeof(s)); lastEnd = 0; } while (0)
#define FRESH() do { memset(&r, 0, sizeof(r)); got = Start(&r); memset(&s, 0, sizeof(s)); lastEnd = 0; } while (0)

    // The BD-059 lab case before the fix: DWM presented, a clean restart, no hook ran (end none). Unclean.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    BOOT();
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_UNCLEAN && got.persist_close);

    // A clean restart: the power callback, then the adapter's D3. Open, nothing seen, ended by the callback.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    CHECK(r.session.present && s.marked);
    st = Event(&r, &s, BC250_INTEROP_EVENT_SYSTEM_DOWN, &lastEnd);
    CHECK(st.unmark == BC250_INTEROP_SESSION_END_SYSTEM_POWER && !r.session.present && s.users == 1);
    st = Event(&r, &s, BC250_INTEROP_EVENT_ADAPTER_DOWN, &lastEnd);
    CHECK(!st.unmark && !st.mark && lastEnd == BC250_INTEROP_SESSION_END_SYSTEM_POWER);
    BOOT();
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == 0 && got.reason == BC250_INTEROP_REASON_NONE);

    // Only the adapter's D3 came (no callback): still clean, ended by the adapter.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_ADAPTER_DOWN, &lastEnd);
    CHECK(lastEnd == BC250_INTEROP_SESSION_END_ADAPTER_D3 && !r.session.present);
    BOOT();
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == 0);

    // The callback's delete failed, the adapter's D3 retries it: clean.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    r.failWrites = 1;
    Event(&r, &s, BC250_INTEROP_EVENT_SYSTEM_DOWN, &lastEnd);
    CHECK(r.session.present && s.marked);
    r.failWrites = 0;
    st = Event(&r, &s, BC250_INTEROP_EVENT_ADAPTER_DOWN, &lastEnd);
    CHECK(st.unmark == BC250_INTEROP_SESSION_END_ADAPTER_D3 && !r.session.present);
    BOOT();
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == 0);

    // A device that begins during the shutdown marks nothing: clean.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_SYSTEM_DOWN, &lastEnd);
    st = Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    CHECK(!st.mark && !r.session.present && s.users == 2);
    BOOT();
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == 0);

    // A hang, a 0x116 or an AC cut with DWM on the path: no power event before the death. Closed, durably.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_END, &lastEnd);
    CHECK(r.session.present);                                   // one user left
    BOOT();
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_UNCLEAN && r.closed.value == 4);

    // Sleep, resume (adapter D0 first, then the callback), then a death: marked again on the way back, so closed.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_SYSTEM_DOWN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_ADAPTER_DOWN, &lastEnd);
    CHECK(!r.session.present && s.down);
    st = Event(&r, &s, BC250_INTEROP_EVENT_UP, &lastEnd);
    CHECK(st.mark && r.session.present && !s.down);
    st = Event(&r, &s, BC250_INTEROP_EVENT_UP, &lastEnd);
    CHECK(!st.mark && !st.unmark);
    BOOT();
    CHECK(got.effective == 0 && got.reason == BC250_INTEROP_REASON_UNCLEAN);

    // Sleep with DWM gone before it, resume: nothing to mark again; a death after that is not in a session.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_END, &lastEnd);
    CHECK(lastEnd == BC250_INTEROP_SESSION_END_USERS && !r.session.present);
    Event(&r, &s, BC250_INTEROP_EVENT_SYSTEM_DOWN, &lastEnd);
    st = Event(&r, &s, BC250_INTEROP_EVENT_UP, &lastEnd);
    CHECK(!st.mark && !r.session.present);
    BOOT();
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == 0);

    // A power loss while asleep (S3) with DWM alive: the marker went at the sleep, so it reads as clean. Recorded
    // in docs/design/gpu-dwm-interop-switches.md: the path is not in use in S3/S4.
    FRESH();
    Event(&r, &s, BC250_INTEROP_EVENT_BEGIN, &lastEnd);
    Event(&r, &s, BC250_INTEROP_EVENT_SYSTEM_DOWN, &lastEnd);
    BOOT();
    CHECK(got.effective == BC250_INTEROP_ALL && got.flags == 0);

    // A device-level D3 (PowerActionNone) or a display-off is not a system action; the five system ones are.
    CHECK(!bc250_interop_system_action(0) && !bc250_interop_system_action(1));
    CHECK(bc250_interop_system_action(2) && bc250_interop_system_action(3) && bc250_interop_system_action(4) &&
          bc250_interop_system_action(5) && bc250_interop_system_action(6));
    CHECK(!bc250_interop_system_action(7) && !bc250_interop_system_action(8) && !bc250_interop_system_action(99));

    // Null session, unknown event: nothing.
    bc250_interop_session_step(0, BC250_INTEROP_EVENT_BEGIN, &st);
    CHECK(!st.mark && !st.unmark);
    memset(&s, 0, sizeof(s));
    s.marked = 1; s.users = 3;
    bc250_interop_session_step(&s, 99, &st);
    CHECK(!st.mark && !st.unmark && s.users == 3 && s.marked && !s.down);
#undef BOOT
#undef FRESH
}

int main(void)
{
    unsigned int a, b, m, c, k, cases = 0, steps;
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

    steps = SessionSequences();
    SessionLifecycles();
    printf("interop policy: %u combinations and 8 lifecycles PASS; session: %u sequence steps and 9 lifecycles PASS\n",
           cases, steps);
    return 0;
}
