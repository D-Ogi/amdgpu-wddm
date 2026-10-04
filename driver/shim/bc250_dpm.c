/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 DPM policy (include/bc250_dpm.h, docs/design/dpm.md). Pure: no locks, no time source, no
 * registry, no SMU. The miniport (driver/kmd/dpm.c) samples, persists and executes.
 *
 * The governor is the classic utilisation governor, not an import: the demand of a tick is
 * busy x clock, and a raise picks the lowest level at which that demand would fill TARGET of the
 * time. Raises happen at once, lowerings one step at a time after DOWN_HOLD_MS below DOWN on the
 * average, so a step down never lands at or above UP (the invariant at bc250_dpm_tune_check; the
 * defaults: 651 x 1.1 = 716.1 <= 900) and the two cannot chase each other. The thermal cap sits on
 * top and wins over everything, the lab floor included: since 0.7.205 it alone may go under it, to 900 or
 * 800 MHz at the floor's own 820 mV (owner decision 2026-10-05). From WARM_MC (87 C since 0.7.204) no raise happens at all, and from RAMP_KNEE_MC
 * (70 C) a raise goes one level per ramp interval (0.7.203). The thresholds and a runtime floor can change at run
 * time (struct bc250_dpm_tune, 0.7.185); the runtime floor lifts only what the load asks for, below
 * every limit. Kto wysoko lata, ten nisko upada - who flies high falls low; here it is the clock, on purpose.
 */
#include <string.h>
#include "bc250_dpm.h"

int bc250_dpm_level_of(unsigned int mhz)
{
	if (mhz < BC250_CLOCK_MIN_MHZ || mhz > BC250_CLOCK_CEILING_MHZ ||
	    (mhz - BC250_CLOCK_MIN_MHZ) % BC250_CLOCK_STEP_MHZ)
		return -1;
	return (int)((mhz - BC250_CLOCK_MIN_MHZ) / BC250_CLOCK_STEP_MHZ);
}

/* The lowest level at or above a clock, over the whole table (the thermal-only points included). */
static unsigned int ceil_level(unsigned int mhz)
{
	unsigned int level;
	if (mhz <= BC250_CLOCK_MIN_MHZ) return BC250_DPM_THERMAL_FLOOR_LEVEL;
	level = (mhz - BC250_CLOCK_MIN_MHZ + BC250_CLOCK_STEP_MHZ - 1u) / BC250_CLOCK_STEP_MHZ;
	return level > BC250_DPM_TOP_LEVEL ? BC250_DPM_TOP_LEVEL : level;
}

/* The lowest level the thermal cap may use now: the thermal floor, or the lab floor once a sub-floor
 * transition has been refused for this start. */
static unsigned int thermal_floor(const struct bc250_dpm_governor *g)
{
	return g->subfloor_ok ? BC250_DPM_THERMAL_FLOOR_LEVEL : BC250_DPM_FLOOR_LEVEL;
}

/* Where a raise from cur goes: the lowest level at which the tick's work (busy x clock) would fill target
 * permille, at least one level up, and never below the lab floor (the load's own lowest level).
 * bc250_dpm_step and invariant 2 of bc250_dpm_tune_check share it. */
static unsigned int raise_level(unsigned int cur, unsigned int busy, unsigned int target_permille)
{
	unsigned int want = ceil_level(bc250_dpm_level_mhz(cur) * busy / target_permille);
	if (want <= cur && cur < BC250_DPM_TOP_LEVEL) want = cur + 1u;
	return want < BC250_DPM_FLOOR_LEVEL ? BC250_DPM_FLOOR_LEVEL : want;
}

unsigned int bc250_dpm_ramp_interval_ms(int temperature_mc)
{
	if (temperature_mc < BC250_DPM_RAMP_KNEE_MC) return 0u;
	if (temperature_mc >= BC250_DPM_WARM_MC) return BC250_DPM_RAMP_MAX_MS;
	return BC250_DPM_RAMP_MIN_MS + (unsigned int)((unsigned long long)(BC250_DPM_RAMP_MAX_MS - BC250_DPM_RAMP_MIN_MS) *
		(unsigned int)(temperature_mc - BC250_DPM_RAMP_KNEE_MC) / (unsigned int)(BC250_DPM_WARM_MC - BC250_DPM_RAMP_KNEE_MC));
}

unsigned int bc250_dpm_busy_permille(unsigned int samples, unsigned int active, unsigned int submit_permille,
				     enum bc250_dpm_busy_source *source)
{
	if (samples < BC250_DPM_HW_MIN_SAMPLES) {
		*source = BC250_DPM_BUSY_SUBMIT;
		return submit_permille > 1000u ? 1000u : submit_permille;
	}
	*source = BC250_DPM_BUSY_GRBM;
	if (active > samples) active = samples;
	return (unsigned int)(((unsigned long long)active * 1000u + samples / 2u) / samples);
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
	max = r->max_present ? r->max_mhz : BC250_DPM_DEFAULT_MAX_MHZ;
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

void bc250_dpm_tune_default(struct bc250_dpm_tune *t)
{
	t->up_permille = BC250_DPM_UP_PERMILLE;
	t->target_permille = BC250_DPM_TARGET_PERMILLE;
	t->down_permille = BC250_DPM_DOWN_PERMILLE;
	t->down_hold_ms = BC250_DPM_DOWN_HOLD_MS;
	t->floor_level = BC250_DPM_FLOOR_LEVEL;
	t->hot_step_ms = BC250_DPM_HOT_STEP_MS;
	t->soft_delta_mc = BC250_DPM_SOFT_DELTA_MC;
	t->soft_step_ms = BC250_DPM_SOFT_STEP_MS;
}

enum bc250_dpm_tune_error bc250_dpm_tune_check(const struct bc250_dpm_tune *t, unsigned int max_level)
{
	unsigned int level;

	if (t->up_permille < BC250_DPM_TUNE_MIN_PERMILLE || t->up_permille > BC250_DPM_TUNE_MAX_PERMILLE ||
	    t->target_permille < BC250_DPM_TUNE_MIN_PERMILLE || t->target_permille > BC250_DPM_TUNE_MAX_PERMILLE ||
	    t->down_permille < BC250_DPM_TUNE_MIN_PERMILLE || t->down_permille > BC250_DPM_TUNE_MAX_PERMILLE)
		return BC250_DPM_TUNE_RANGE;
	if (!(t->down_permille < t->target_permille && t->target_permille < t->up_permille))
		return BC250_DPM_TUNE_ORDER;
	/* Invariant 1, every adjacent pair of the load's levels (the lab floor up; the two thermal-only points
	 * below it are never a load decision), not only the bottom one: the inequality is the contract, the
	 * table's shape (where the ratio peaks) is not. */
	for (level = BC250_DPM_FLOOR_LEVEL + 1u; level < BC250_CLOCK_LEVELS; level++)
		if ((t->down_permille + 1u) * bc250_dpm_level_mhz(level) > t->up_permille * bc250_dpm_level_mhz(level - 1u))
			return BC250_DPM_TUNE_LOWERING;
	/* Invariant 2, with bc250_dpm_step's own arithmetic. At most 10 x 901 cases; run only when a tune changes. */
	for (level = BC250_DPM_FLOOR_LEVEL; level < BC250_DPM_TOP_LEVEL; level++) {
		unsigned int busy, mhz = bc250_dpm_level_mhz(level);
		for (busy = t->up_permille; busy <= 1000u; busy++) {
			unsigned int next = raise_level(level, busy, t->target_permille);
			if (t->down_permille * bc250_dpm_level_mhz(next) > busy * mhz) return BC250_DPM_TUNE_RAISE;
		}
	}
	if (t->down_hold_ms < BC250_DPM_TUNE_MIN_HOLD_MS || t->down_hold_ms > BC250_DPM_TUNE_MAX_HOLD_MS)
		return BC250_DPM_TUNE_HOLD;
	/* The runtime floor stays between the lab floor and the start's ceiling: the thermal-only points below
	 * BC250_DPM_FLOOR_LEVEL are the cap's, and the load never lands there (0.7.205). */
	if (t->floor_level < BC250_DPM_FLOOR_LEVEL || t->floor_level > max_level ||
	    t->floor_level > BC250_DPM_TOP_LEVEL)
		return BC250_DPM_TUNE_FLOOR;
	if (t->hot_step_ms < BC250_DPM_TUNE_MIN_HOT_STEP_MS || t->hot_step_ms > BC250_DPM_TUNE_MAX_HOT_STEP_MS ||
	    (t->soft_delta_mc != 0u &&
	     (t->soft_delta_mc < BC250_DPM_TUNE_MIN_SOFT_DELTA_MC || t->soft_delta_mc > BC250_DPM_TUNE_MAX_SOFT_DELTA_MC)) ||
	    t->soft_step_ms < BC250_DPM_TUNE_MIN_SOFT_STEP_MS || t->soft_step_ms > BC250_DPM_TUNE_MAX_SOFT_STEP_MS)
		return BC250_DPM_TUNE_THERMAL;
	return BC250_DPM_TUNE_OK;
}

void bc250_dpm_init(struct bc250_dpm_governor *g, unsigned int max_level)
{
	memset(g, 0, sizeof(*g));
	g->max_level = max_level > BC250_DPM_TOP_LEVEL ? BC250_DPM_TOP_LEVEL : max_level;
	g->thermal_cap = g->max_level;
	g->level = BC250_DPM_FLOOR_LEVEL;
	g->cap_ms = BC250_DPM_CAP_MS_MAX;	/* no change yet: the first hot tick steps at once */
	g->raise_ms = BC250_DPM_CAP_MS_MAX;	/* no raise yet: the first one is not held */
	g->subfloor_ok = 1;			/* until the hardware refuses a point under the lab floor */
	bc250_dpm_tune_default(&g->tune);
}

void bc250_dpm_subfloor_refused(struct bc250_dpm_governor *g)
{
	g->subfloor_refusals++;
	g->subfloor_ok = 0;
	if (g->thermal_cap < BC250_DPM_FLOOR_LEVEL) {
		g->thermal_cap = BC250_DPM_FLOOR_LEVEL;
		g->cap_ms = 0;
	}
}

enum bc250_dpm_tune_error bc250_dpm_set_tune(struct bc250_dpm_governor *g, const struct bc250_dpm_tune *t)
{
	enum bc250_dpm_tune_error e = bc250_dpm_tune_check(t, g->max_level);
	if (e == BC250_DPM_TUNE_OK) g->tune = *t;
	return e;
}

unsigned int bc250_dpm_step(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in)
{
	unsigned int dt = in->dt_ms > BC250_DPM_MAX_DT_MS ? BC250_DPM_MAX_DT_MS : in->dt_ms;
	unsigned int busy = in->busy_permille > 1000u ? 1000u : in->busy_permille;
	unsigned int cur = g->level > BC250_DPM_TOP_LEVEL ? BC250_DPM_TOP_LEVEL : g->level;
	unsigned int want = cur, limit, target;
	unsigned int thermal = BC250_DPM_THROTTLE_NONE;
	const struct bc250_dpm_tune *t = &g->tune;
	/* bc250_dpm_set_tune admits no floor above the ceiling; a caller that wrote g->tune itself gets it clamped. */
	unsigned int floor = t->floor_level < g->max_level ? t->floor_level : g->max_level;

	g->avg_permille = (g->avg_permille * 3u + busy + 2u) / 4u;

	/* Thermal first: it bounds whatever the load asks for. cap_ms is the time since the cap last moved the clock's
	 * bound (a step down or a raise; the hot entry's clamp to the running clock lowers nothing and does not count):
	 * the spacing of a re-entry's step is measured from it. */
	g->cap_ms = g->cap_ms > BC250_DPM_CAP_MS_MAX - dt ? BC250_DPM_CAP_MS_MAX : g->cap_ms + dt;
	g->raise_ms = g->raise_ms > BC250_DPM_CAP_MS_MAX - dt ? BC250_DPM_CAP_MS_MAX : g->raise_ms + dt;
	if (!in->temperature_valid || in->temperature_mc >= BC250_DPM_CRITICAL_MC) {
		/* Critical goes to the lowest level the cap may use (800 MHz since 0.7.205). A missing reading goes
		 * to the lab floor instead: blind, the driver owns the one point this part is known to run at. That
		 * rule is a clamp and never a raise: a cap already below the floor stays where it is. Up to 0.7.204
		 * the cap could not be below the floor, so this was always a lowering; with the two thermal-only
		 * points a bare "go to the floor" would let one blind tick undo every hot step the governor had
		 * taken (the release rule, RELEASE_MC, is the only sanctioned way back up), and a flaky sensor would
		 * walk the clock up and down through the warm zone. The stale reading the KMD leaves in the tick
		 * when SmuReadTemperature fails may sit between RELEASE_MC and HOT_MC, where neither the warm zone
		 * nor the clock transaction's own gate refuses a raise. */
		unsigned int bottom = in->temperature_valid ? thermal_floor(g)
				    : (g->thermal_cap < BC250_DPM_FLOOR_LEVEL ? g->thermal_cap : BC250_DPM_FLOOR_LEVEL);
		if (g->thermal_cap != bottom || !g->hot) g->thermal_events++;
		if (g->thermal_cap != bottom) g->cap_ms = 0;
		g->thermal_cap = bottom;
		g->hot = 1;
		g->hot_ms = 0;
		g->release_ms = 0;
		g->soft_ms = 0;
		thermal = in->temperature_valid ? BC250_DPM_THROTTLE_THERMAL_HARD : BC250_DPM_THROTTLE_SENSOR;
	} else if (in->temperature_mc >= BC250_DPM_HOT_MC) {
		if (!g->hot) {
			/* One step below where the clock is now, whatever the load: at once when the cap last changed at
			 * least a hot step ago. A temperature hovering at 87 C enters again and again, and stepped once per
			 * crossing before 0.7.197 (BD-055); a re-entry inside the hot step only stops raises (the cap down
			 * to the clock, which lowers nothing) and its step follows a hot step after the last change. */
			g->hot = 1;
			g->hot_ms = 0;
			g->thermal_events++;
			if (g->thermal_cap >= cur) {
				if (g->cap_ms >= t->hot_step_ms) {
					g->thermal_cap = cur > thermal_floor(g) ? cur - 1u : thermal_floor(g);
					g->cap_ms = 0;
				} else {
					g->thermal_cap = cur;
					g->hot_ms = g->cap_ms;
				}
			}
		} else {
			g->hot_ms += dt;
			if (g->hot_ms >= t->hot_step_ms) {
				g->hot_ms = 0;
				if (g->thermal_cap > thermal_floor(g)) {
					/* One level per hot step, down to 800 MHz since 0.7.205 (the owner's decision of
					 * 2026-10-05: the last two levels are below the lab floor). */
					g->thermal_cap--;
					g->cap_ms = 0;
				}
			}
		}
		g->release_ms = 0;
		g->soft_ms = 0;
		thermal = BC250_DPM_THROTTLE_THERMAL_SOFT;
	} else {
		g->hot = 0;
		g->hot_ms = 0;
		if (in->temperature_mc < BC250_DPM_RELEASE_MC && g->thermal_cap < g->max_level) {
			g->soft_ms = 0;
			g->release_ms += dt;
			if (g->release_ms >= BC250_DPM_RELEASE_STEP_MS) {
				g->release_ms = 0;
				g->thermal_cap++;
				g->cap_ms = 0;
			}
		} else if (t->soft_delta_mc && g->thermal_cap < g->max_level &&
			   in->temperature_mc < BC250_DPM_HOT_MC - (int)t->soft_delta_mc) {
			/* The soft release: held below the soft threshold for a whole step without a break, one level. A
			 * thermal change resets the hold (no step down happens below HOT_MC, and the paths above clear
			 * soft_ms), so a soft raise is at least soft_step_ms after any change of the cap. */
			g->release_ms = 0;
			g->soft_ms += dt;
			if (g->soft_ms >= t->soft_step_ms) {
				g->soft_ms = 0;
				g->thermal_cap++;
				g->cap_ms = 0;
				g->soft_releases++;
			}
		} else {
			g->release_ms = 0;
			g->soft_ms = 0;
		}
		/* Between the release threshold in force and HOT the cap holds; it still names what holds the clock. */
		if (g->thermal_cap < g->max_level) thermal = BC250_DPM_THROTTLE_THERMAL_SOFT;
	}

	/* The load. */
	if (busy >= t->up_permille) {
		want = raise_level(cur, busy, t->target_permille);
		g->down_ms = 0;
	} else if (g->avg_permille < t->down_permille) {
		g->down_ms += dt;
		if (g->down_ms >= t->down_hold_ms) {
			g->down_ms = 0;
			if (cur > BC250_DPM_FLOOR_LEVEL) want = cur - 1u;
		}
	} else g->down_ms = 0;
	/* The load's own answer never goes under the lab floor: the two levels below it are the thermal cap's
	 * alone (0.7.205). cur is below the floor only while that cap holds it there, and the cap is applied
	 * further down anyway, so this changes nothing but what the telemetry calls the load's demand. */
	if (want < BC250_DPM_FLOOR_LEVEL) want = BC250_DPM_FLOOR_LEVEL;
	g->want = want;

	/* The runtime floor lifts what the load asks for; the limits below still bound it. want stays the
	 * load's own answer, so the telemetry shows what the governor would do without the floor. */
	target = want;
	if (target < floor) {
		target = floor;
		g->floor_ticks++;
	}

	/* The limits, in the order that names the reason. SetStablePowerState asks for the lab floor, and since
	 * 0.7.205 a thermal cap below it still wins (a profiler's request for one steady clock is no reason to
	 * raise a hot part); up to 0.7.204 the cap could not be lower, so this changes nothing else. */
	g->throttle = BC250_DPM_THROTTLE_NONE;
	limit = g->thermal_cap < g->max_level ? g->thermal_cap : g->max_level;
	if (g->stable) {
		target = BC250_DPM_FLOOR_LEVEL;
		g->throttle = BC250_DPM_THROTTLE_STABLE;
	}
	if (target > limit) {
		/* The cap only names the reason while it is below the setting (or the sensor is gone). */
		if (g->thermal_cap < g->max_level || thermal == BC250_DPM_THROTTLE_THERMAL_HARD ||
		    thermal == BC250_DPM_THROTTLE_SENSOR)
			g->throttle = thermal != BC250_DPM_THROTTLE_NONE ? thermal : BC250_DPM_THROTTLE_THERMAL_SOFT;
		else g->throttle = BC250_DPM_THROTTLE_MAX_SETTING;
		target = limit;
	}
	/* The warm zone (from WARM_MC, 0.7.200; 87 C = HOT_MC since 0.7.204): no raise of clock or voltage, the level holds;
	 * a lowering, and the paths above, act as below it. It comes last, so it bounds a raise from the load and from the
	 * runtime floor alike, and it names the reason when it is what holds the level. From HOT_MC up the hot cap already
	 * holds the target at or below the running level (a step down, or the clamp of a re-entry), so there the rule is a
	 * backstop that never fires; before 0.7.204 it ended at HOT_MC. The soft release threshold (HOT_MC minus 0.5 to
	 * 4.5 C) now always lies under WARM_MC, so a released cap is followed at the ramp's pace. The cap's timing (cap_ms,
	 * soft_ms) does not depend on this zone. */
	if (in->temperature_mc >= BC250_DPM_WARM_MC && target > cur) {
		target = cur;
		g->throttle = BC250_DPM_THROTTLE_THERMAL_WARM;
		g->warm_holds++;
	}
	/* The thermal ramp (RAMP_KNEE_MC up to WARM_MC, 0.7.203): a raise goes one level at most, and only a ramp interval
	 * after the last raise. Like the warm zone it bounds every raise (load, runtime floor, the clock following a released
	 * cap) and lowers nothing. A step that returned a raise starts the interval again, also when the caller's
	 * transaction then failed: the retry waits, which is the safe side. Below the knee raise_ms still counts, so a raise
	 * at 74.9 C spaces the next one at 75 C. A stalled tick counts as MAX_DT_MS, as everywhere. */
	if (target > cur && in->temperature_mc >= BC250_DPM_RAMP_KNEE_MC) {
		unsigned int ramp = g->raise_ms >= bc250_dpm_ramp_interval_ms(in->temperature_mc) ? cur + 1u : cur;
		if (target > ramp) {
			target = ramp;
			g->throttle = BC250_DPM_THROTTLE_THERMAL_RAMP;
			g->ramp_holds++;
		}
	}
	if (target > cur) g->raise_ms = 0;
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
