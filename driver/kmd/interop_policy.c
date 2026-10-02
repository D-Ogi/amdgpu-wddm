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
