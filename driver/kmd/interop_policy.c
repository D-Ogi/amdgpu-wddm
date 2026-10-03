/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
#include "interop_policy.h"

static const char* const g_InteropReason[BC250_INTEROP_REASON_COUNT] = {
    "none", "not-requested", "invalid-setting", "unused", "unclean", "registry", "unused", "not-run"
};

const char* bc250_interop_reason_name(unsigned int reason)
{
    return reason < BC250_INTEROP_REASON_COUNT ? g_InteropReason[reason] : "unknown";
}

static void Switch(const struct bc250_interop_value* v, unsigned int bit, unsigned int* requested,
                   int* invalid, int* unreadable, int* explicit_zero)
{
    if (v->state == BC250_INTEROP_ABSENT) { *requested |= bit; *explicit_zero = 0; return; }   // default on
    if (v->state != BC250_INTEROP_PRESENT) { *unreadable = 1; *explicit_zero = 0; return; }
    if (v->value == 1u) { *requested |= bit; *explicit_zero = 0; return; }
    if (v->value != 0u) { *invalid = 1; *explicit_zero = 0; }
}

void bc250_interop_decide(const struct bc250_interop_inputs* in, struct bc250_interop_decision* out)
{
    unsigned int requested = 0, previous = 0;
    int invalid = 0, unreadable = 0, both_zero = 1, unclean = 0;

    out->requested = out->effective = out->closed_reason = out->flags = 0;
    out->reason = BC250_INTEROP_REASON_NOT_RUN;
    out->persist_close = out->clear_closed = out->clear_session = 0;
    if (in == 0) return;

    Switch(&in->blit, BC250_INTEROP_BLIT, &requested, &invalid, &unreadable, &both_zero);
    Switch(&in->cdd, BC250_INTEROP_CDD, &requested, &invalid, &unreadable, &both_zero);
    out->requested = requested;

    // The marker. A record of this boot means an earlier start of this boot marked it and ended without the
    // unmark (a failed stop, a surprise removal): the machine did not die, so nothing closes. Without that
    // record the marker outlived a boot. An unreadable marker counts as unclean: the safe side.
    if (in->session.state == BC250_INTEROP_PRESENT && in->boot_record) {
        out->flags |= BC250_INTEROP_DECISION_STALE;
        out->clear_session = 1;
    } else if (in->session.state != BC250_INTEROP_ABSENT) {
        out->flags |= BC250_INTEROP_DECISION_UNCLEAN;
        out->clear_session = 1;
        unclean = 1;
    }
    if (in->closed.state == BC250_INTEROP_PRESENT) previous = in->closed.value;

    if (unclean && !both_zero) {
        // Everything not explicitly 0 closes, durably: an unreadable or invalid value is overwritten as well.
        out->reason = BC250_INTEROP_REASON_UNCLEAN;
        out->closed_reason = BC250_INTEROP_REASON_UNCLEAN;
        out->flags |= BC250_INTEROP_DECISION_CLOSED_BY_DRIVER;
        out->persist_close = 1;
        return;
    }
    out->closed_reason = previous;
    if (unreadable) { out->reason = BC250_INTEROP_REASON_REGISTRY; return; }
    if (invalid) { out->reason = BC250_INTEROP_REASON_INVALID_SETTING; return; }
    if (requested == 0) {
        out->reason = BC250_INTEROP_REASON_NOT_REQUESTED;
        if (previous) out->flags |= BC250_INTEROP_DECISION_CLOSED_BY_DRIVER;
        return;
    }
    out->effective = requested;
    out->reason = BC250_INTEROP_REASON_NONE;
    if (in->closed.state != BC250_INTEROP_ABSENT) {
        // The driver had closed them and the operator wrote a 1 (or deleted the values) since: forget it.
        out->clear_closed = 1;
        out->closed_reason = 0;
    }
}

void bc250_interop_session_step(struct bc250_interop_session* s, unsigned int event, struct bc250_interop_step* out)
{
    out->mark = 0;
    out->unmark = BC250_INTEROP_SESSION_END_NONE;
    if (s == 0) return;
    switch (event) {
    case BC250_INTEROP_EVENT_BEGIN:
        s->users++;
        out->mark = !s->marked && !s->down;     // also retries a mark that failed for an earlier device
        break;
    case BC250_INTEROP_EVENT_END:
        if (s->users) s->users--;
        if (s->users == 0 && s->marked) out->unmark = BC250_INTEROP_SESSION_END_USERS;
        break;
    case BC250_INTEROP_EVENT_STOP:
        s->users = 0;
        if (s->marked) out->unmark = BC250_INTEROP_SESSION_END_STOP;
        break;
    case BC250_INTEROP_EVENT_SYSTEM_DOWN:
    case BC250_INTEROP_EVENT_ADAPTER_DOWN:
        // The users stay counted: after a sleep they present again without a new BEGIN.
        s->down = 1;
        if (s->marked)
            out->unmark = event == BC250_INTEROP_EVENT_SYSTEM_DOWN ? BC250_INTEROP_SESSION_END_SYSTEM_POWER
                                                                  : BC250_INTEROP_SESSION_END_ADAPTER_D3;
        break;
    case BC250_INTEROP_EVENT_UP:
        s->down = 0;
        out->mark = !s->marked && s->users != 0;
        break;
    default:
        break;
    }
}

int bc250_interop_system_action(unsigned int action)
{
    // POWER_ACTION (wdm.h): 2 sleep, 3 hibernate, 4 shutdown, 5 shutdown-reset, 6 shutdown-off. Not 0 (none: a
    // device-level transition) or 7/8 (warm eject, display off): the system keeps running through those.
    return action >= 2u && action <= 6u;
}
