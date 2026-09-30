/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 DPM policy (include/bc250_dpm.h, docs/design/dpm.md). Pure: no locks, no time source, no
 * registry, no SMU. The miniport (driver/kmd/dpm.c) samples, persists and executes.
 *
 * The governor is the classic utilisation governor, not an import: the demand of a tick is
 * busy x clock, and a raise picks the lowest level at which that demand would fill TARGET of the
 * time. Raises happen at once, lowerings one step at a time after DOWN_HOLD_MS below DOWN on the
 * average, so a step down never lands above UP (worst ratio 1100/1000: 650 x 1.1 = 715 < 900) and
 * the two cannot chase each other. The thermal cap sits on top and wins over everything but the
 * floor. Kto wysoko lata, ten nisko upada - who flies high falls low; here it is the clock, on purpose.
 */
#include <string.h>
#include "bc250_dpm.h"

int bc250_dpm_level_of(unsigned int mhz)
{
	if (mhz < BC250_CLOCK_FLOOR_MHZ || mhz > BC250_CLOCK_CEILING_MHZ ||
	    (mhz - BC250_CLOCK_FLOOR_MHZ) % BC250_CLOCK_STEP_MHZ)
		return -1;
	return (int)((mhz - BC250_CLOCK_FLOOR_MHZ) / BC250_CLOCK_STEP_MHZ);
}

/* The lowest level at or above a clock. */
static unsigned int ceil_level(unsigned int mhz)
{
	unsigned int level;
	if (mhz <= BC250_CLOCK_FLOOR_MHZ) return BC250_DPM_FLOOR_LEVEL;
	level = (mhz - BC250_CLOCK_FLOOR_MHZ + BC250_CLOCK_STEP_MHZ - 1u) / BC250_CLOCK_STEP_MHZ;
	return level > BC250_DPM_TOP_LEVEL ? BC250_DPM_TOP_LEVEL : level;
}

void bc250_dpm_decide(const struct bc250_dpm_request *r, struct bc250_dpm_decision *d)
{
	unsigned int max;

	memset(d, 0, sizeof(*d));
	d->requested = r->mode_present ? r->mode : BC250_DPM_DEFAULT_MODE;
	d->mode = BC250_DPM_MODE_FIXED;
	d->max_mhz = BC250_CLOCK_FLOOR_MHZ;
	d->max_level = BC250_DPM_FLOOR_LEVEL;
	/* Without the owner nothing below runs, so nothing is persisted either. */
	if (!r->smu_online) {
		d->reason = BC250_DPM_REASON_NO_SMU;
		return;
	}
	if (d->requested == BC250_DPM_MODE_FIXED) {
		/* A fixed start never leaves the floor: both marks describe a request no longer made. */
		d->reason = BC250_DPM_REASON_NOT_REQUESTED;
		d->clear_pending = r->pending_present;
		d->clear_session = r->session_present;
		return;
	}
	if (d->requested != BC250_DPM_MODE_DPM) {
		/* Kept as written: the owner's typo is the owner's to fix, the marks stay for the next try. */
		d->reason = BC250_DPM_REASON_INVALID_SETTING;
		return;
	}
	max = r->max_present ? r->max_mhz : BC250_CLOCK_CEILING_MHZ;
	if (max < BC250_CLOCK_FLOOR_MHZ || max > BC250_CLOCK_CEILING_MHZ) {
		d->reason = BC250_DPM_REASON_INVALID_SETTING;
		return;
	}
	max -= (max - BC250_CLOCK_FLOOR_MHZ) % BC250_CLOCK_STEP_MHZ;
	d->encoded = bc250_dpm_encode(max);
	if (r->pending_present) {
		/* Whatever happened to the start that set it, DPM is not tried twice unasked. */
		d->reason = BC250_DPM_REASON_UNCONFIRMED;
		d->force_fixed = 1;
		return;
	}
	if (r->session_present) {
		d->reason = BC250_DPM_REASON_UNCLEAN;
		d->force_fixed = 1;
		return;
	}
	d->mode = BC250_DPM_MODE_DPM;
	d->max_mhz = max;
	d->max_level = (unsigned int)bc250_dpm_level_of(max);
	d->reason = BC250_DPM_REASON_NONE;
	if (r->confirmed_present && r->confirmed == d->encoded) d->confirmed = 1;
	else d->mark_pending = 1;
}

void bc250_dpm_init(struct bc250_dpm_governor *g, unsigned int max_level)
{
	memset(g, 0, sizeof(*g));
	g->max_level = max_level > BC250_DPM_TOP_LEVEL ? BC250_DPM_TOP_LEVEL : max_level;
	g->thermal_cap = g->max_level;
	g->level = BC250_DPM_FLOOR_LEVEL;
}

unsigned int bc250_dpm_step(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in)
{
	unsigned int dt = in->dt_ms > BC250_DPM_MAX_DT_MS ? BC250_DPM_MAX_DT_MS : in->dt_ms;
	unsigned int busy = in->busy_permille > 1000u ? 1000u : in->busy_permille;
	unsigned int cur = g->level > BC250_DPM_TOP_LEVEL ? BC250_DPM_TOP_LEVEL : g->level;
	unsigned int want = cur, limit, target;
	unsigned int thermal = BC250_DPM_THROTTLE_NONE;

	g->avg_permille = (g->avg_permille * 3u + busy + 2u) / 4u;

	/* Thermal first: it bounds whatever the load asks for. */
	if (!in->temperature_valid || in->temperature_mc >= BC250_DPM_CRITICAL_MC) {
		if (g->thermal_cap != BC250_DPM_FLOOR_LEVEL || !g->hot) g->thermal_events++;
		g->thermal_cap = BC250_DPM_FLOOR_LEVEL;
		g->hot = 1;
		g->hot_ms = 0;
		g->release_ms = 0;
		thermal = in->temperature_valid ? BC250_DPM_THROTTLE_THERMAL_HARD : BC250_DPM_THROTTLE_SENSOR;
	} else if (in->temperature_mc >= BC250_DPM_HOT_MC) {
		if (!g->hot) {
			/* At once: one step below where the clock is now, whatever the load. */
			g->hot = 1;
			g->hot_ms = 0;
			g->thermal_events++;
			if (g->thermal_cap >= cur) g->thermal_cap = cur ? cur - 1u : BC250_DPM_FLOOR_LEVEL;
		} else {
			g->hot_ms += dt;
			if (g->hot_ms >= BC250_DPM_HOT_STEP_MS) {
				g->hot_ms = 0;
				if (g->thermal_cap) g->thermal_cap--;
			}
		}
		g->release_ms = 0;
		thermal = BC250_DPM_THROTTLE_THERMAL_SOFT;
	} else {
		g->hot = 0;
		g->hot_ms = 0;
		if (in->temperature_mc < BC250_DPM_RELEASE_MC && g->thermal_cap < g->max_level) {
			g->release_ms += dt;
			if (g->release_ms >= BC250_DPM_RELEASE_STEP_MS) {
				g->release_ms = 0;
				g->thermal_cap++;
			}
		} else g->release_ms = 0;
		/* Between RELEASE and HOT the cap holds; it still names what holds the clock. */
		if (g->thermal_cap < g->max_level) thermal = BC250_DPM_THROTTLE_THERMAL_SOFT;
	}

	/* The load. */
	if (busy >= BC250_DPM_UP_PERMILLE) {
		unsigned int demand = bc250_dpm_level_mhz(cur) * busy / BC250_DPM_TARGET_PERMILLE;
		want = ceil_level(demand);
		if (want <= cur && cur < BC250_DPM_TOP_LEVEL) want = cur + 1u;
		g->down_ms = 0;
	} else if (g->avg_permille < BC250_DPM_DOWN_PERMILLE) {
		g->down_ms += dt;
		if (g->down_ms >= BC250_DPM_DOWN_HOLD_MS) {
			g->down_ms = 0;
			if (cur > BC250_DPM_FLOOR_LEVEL) want = cur - 1u;
		}
	} else g->down_ms = 0;
	g->want = want;

	/* The limits, in the order that names the reason. */
	target = want;
	g->throttle = BC250_DPM_THROTTLE_NONE;
	limit = g->thermal_cap < g->max_level ? g->thermal_cap : g->max_level;
	if (g->stable) {
		target = BC250_DPM_FLOOR_LEVEL;
		g->throttle = BC250_DPM_THROTTLE_STABLE;
	} else if (target > limit) {
		/* The cap only names the reason while it is below the setting (or the sensor is gone). */
		if (g->thermal_cap < g->max_level || thermal == BC250_DPM_THROTTLE_THERMAL_HARD ||
		    thermal == BC250_DPM_THROTTLE_SENSOR)
			g->throttle = thermal != BC250_DPM_THROTTLE_NONE ? thermal : BC250_DPM_THROTTLE_THERMAL_SOFT;
		else g->throttle = BC250_DPM_THROTTLE_MAX_SETTING;
		target = limit;
	}
	return target;
}

void bc250_dpm_commit(struct bc250_dpm_governor *g, unsigned int level)
{
	if (level > BC250_DPM_TOP_LEVEL) level = BC250_DPM_TOP_LEVEL;
	if (level > g->level) g->raises++;
	else if (level < g->level) g->lowers++;
	g->level = level;
}

enum bc250_dpm_session_action bc250_dpm_session_step(struct bc250_dpm_session *s, unsigned int level,
						     unsigned int dt_ms)
{
	if (level > BC250_DPM_FLOOR_LEVEL) {
		s->floor_ms = 0;
		return s->marked ? BC250_DPM_SESSION_NONE : BC250_DPM_SESSION_SET;
	}
	if (!s->marked) return BC250_DPM_SESSION_NONE;
	s->floor_ms += dt_ms > BC250_DPM_MAX_DT_MS ? BC250_DPM_MAX_DT_MS : dt_ms;
	return s->floor_ms >= BC250_DPM_SESSION_CLEAR_MS ? BC250_DPM_SESSION_CLEAR : BC250_DPM_SESSION_NONE;
}
