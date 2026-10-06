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
 * 800 MHz at the floor's own 820 mV (owner decision 2026-10-05). Since 0.7.207 the idle state goes lower
 * still, to 500 MHz, but only while the GPU has no work at all. Since 0.7.213 the cap starts stepping in the soft
 * zone, 86 C, with the reading extrapolated 15 s along its own slope, and the clock stops rising at 83 C
 * (bc250_dpm_warm_mc); the 87 C hot cap and the 90 C critical rule stay behind both as backstops on the raw reading.
 * From the warm threshold up no raise happens at all, and from RAMP_KNEE_MC
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

/* Saturating sum, for the millisecond and permille-millisecond counters. */
static unsigned int add_ms(unsigned int a, unsigned int b)
{
	return a > BC250_DPM_CAP_MS_MAX - b ? BC250_DPM_CAP_MS_MAX : a + b;
}

/* The lowest level at or above a clock, over the whole table (the points under the lab floor included). */
static unsigned int ceil_level(unsigned int mhz)
{
	unsigned int level;
	if (mhz <= BC250_CLOCK_MIN_MHZ) return BC250_DPM_IDLE_LEVEL;
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
	/* The record of an earlier fallback stays as it is, unless this start closes DPM or opens it again. */
	d->closed_reason = r->closed_present ? r->closed : 0u;
	/* Without the owner nothing below runs, so nothing is persisted either. */
	if (!r->smu_online) {
		d->reason = BC250_DPM_REASON_NO_SMU;
		return;
	}
	if (d->requested == BC250_DPM_MODE_FIXED) {
		/* A fixed start never leaves the floor: both marks describe a request no longer made. The record
		 * of a fallback is not a mark of a request. It describes who wrote this 0, so it stays: the
		 * installer reads it one boot later and offers the repair (BD-069). */
		d->reason = BC250_DPM_REASON_NOT_REQUESTED;
		d->clear_pending = r->pending_present;
		d->clear_session = r->session_present;
		return;
	}
	/* DpmMode is not the fixed mode any more, so somebody wrote over the fallback: the record goes, the way
	 * interop_policy.c clears InteropClosedReason when a switch is open again. */
	d->clear_closed = r->closed_present;
	d->closed_reason = 0u;
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
		d->clear_closed = 0;
		d->closed_reason = d->reason;		/* the driver wrote this 0: DpmClosedReason records it */
		return;
	}
	if (r->session_present) {
		d->reason = BC250_DPM_REASON_UNCLEAN;
		d->force_fixed = 1;
		d->clear_closed = 0;
		d->closed_reason = d->reason;
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
	t->zone_delta_mc = BC250_DPM_ZONE_DELTA_MC;
	t->zone_step_ms = BC250_DPM_ZONE_STEP_MS;
	t->zone_lead_ms = BC250_DPM_ZONE_LEAD_MS;
}

void bc250_dpm_tune_zone_off(struct bc250_dpm_tune *t)
{
	/* The zone, the lead and the soft release together: with the zone off the only rule that reads the temperature is
	 * the hot cap at HOT_MC, which is 0.7.212 exactly. zone_step_ms keeps a value inside its range, because
	 * bc250_dpm_tune_check checks it even while the zone is off, so turning the zone back on is one field. */
	t->zone_delta_mc = 0u;
	t->zone_step_ms = BC250_DPM_ZONE_STEP_MS;
	t->zone_lead_ms = 0u;
	t->soft_delta_mc = 0u;
}

int bc250_dpm_zone_mc(const struct bc250_dpm_tune *t)
{
	return t->zone_delta_mc ? (int)BC250_DPM_HOT_MC - (int)t->zone_delta_mc : 0;
}

int bc250_dpm_warm_mc(const struct bc250_dpm_tune *t)
{
	/* With the zone on, the clock stops rising exactly where the cap's up side is: the soft-release threshold. The two
	 * must not disagree, or a released cap would be followed by a clock the zone is about to take back one tick later.
	 * bc250_dpm_tune_check refuses a zone without a soft release, so the second test is the "zone off" case alone. */
	if (t->zone_delta_mc && t->soft_delta_mc) return (int)BC250_DPM_HOT_MC - (int)t->soft_delta_mc;
	return BC250_DPM_WARM_MC;
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
	/* The soft zone (0.7.213) last, so that an older caller's refusal keeps its old error number. The deadband is the
	 * point of the third test: the zone steps the cap down from HOT_MC - zone_delta_mc and the soft release raises it
	 * below HOT_MC - soft_delta_mc, so with no half degree between them one reading would ask for both. A zone with the
	 * soft release off is refused by the same inequality (soft_delta_mc 0), and that is deliberate: the cap would then
	 * have a way down at 86 C and no way back up until 82 C. */
	if (t->zone_step_ms < BC250_DPM_TUNE_MIN_ZONE_STEP_MS || t->zone_step_ms > BC250_DPM_TUNE_MAX_ZONE_STEP_MS ||
	    t->zone_lead_ms > BC250_DPM_TUNE_MAX_ZONE_LEAD_MS ||
	    (t->zone_delta_mc != 0u &&
	     (t->zone_delta_mc < BC250_DPM_TUNE_MIN_ZONE_DELTA_MC || t->zone_delta_mc > BC250_DPM_TUNE_MAX_ZONE_DELTA_MC ||
	      t->zone_delta_mc + 500u > t->soft_delta_mc)))
		return BC250_DPM_TUNE_ZONE;
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
	/* The idle state is off until a caller configures it (bc250_dpm_idle_config), so a governor built here
	 * and stepped behaves exactly as 0.7.205. The values below are only what the state would run with. */
	g->idle_level = BC250_DPM_IDLE_LEVEL;
	g->idle_hold_ms = BC250_DPM_IDLE_HOLD_MS;
	g->idle_busy_permille = BC250_DPM_IDLE_BUSY_PERMILLE;
	bc250_dpm_tune_default(&g->tune);
}

enum bc250_dpm_idle_error bc250_dpm_idle_config(struct bc250_dpm_governor *g, unsigned int idle_mhz,
						unsigned int hold_ms, unsigned int busy_permille)
{
	int level = idle_mhz ? bc250_dpm_level_of(idle_mhz) : -1;

	g->idle_on = 0;
	g->idle = 0;
	g->idle_ms = 0;
	g->idle_acc = 0;
	if (!idle_mhz) return BC250_DPM_IDLE_OK;			/* the state is off, as asked */
	if (level < 0 || (unsigned int)level >= BC250_DPM_FLOOR_LEVEL) return BC250_DPM_IDLE_CLOCK;
	if (hold_ms < BC250_DPM_IDLE_MIN_HOLD_MS || hold_ms > BC250_DPM_IDLE_MAX_HOLD_MS)
		return BC250_DPM_IDLE_HOLD;
	if (busy_permille > BC250_DPM_IDLE_MAX_BUSY_PERMILLE) return BC250_DPM_IDLE_BUSY;
	g->idle_level = (unsigned int)level;
	g->idle_hold_ms = hold_ms;
	g->idle_busy_permille = busy_permille;
	g->idle_on = 1;
	return BC250_DPM_IDLE_OK;
}

void bc250_dpm_idle_refused(struct bc250_dpm_governor *g)
{
	g->idle_refusals++;
	g->idle = 0;
	g->idle_ms = 0;
	g->idle_acc = 0;
	/* One step up the fallback: the idle point, then the thermal floor, then nothing. A refused point at or
	 * above the thermal floor is the thermal cap's own lowest point, asked for with the same two messages, so
	 * the cap loses it too and nothing below the lab floor is asked for again (0.7.207). */
	if (g->idle_level < BC250_DPM_THERMAL_FLOOR_LEVEL) g->idle_level = BC250_DPM_THERMAL_FLOOR_LEVEL;
	else {
		g->idle_on = 0;
		bc250_dpm_subfloor_refused(g);
	}
}

void bc250_dpm_idle_leave(struct bc250_dpm_governor *g)
{
	if (g->idle) g->idle_exits++;
	g->idle = 0;
	g->idle_ms = 0;
	g->idle_acc = 0;
}

/* ---- the operator's V/F curve and its trial (0.7.210, docs/design/tuner.md, ADR 0020) ------------------- */

void bc250_dpm_curve_init(struct bc250_dpm_curve_state *s, const struct bc250_clock_curve *stored)
{
	memset(s, 0, sizeof(*s));
	if (stored) s->stored = *stored;
	else bc250_clock_curve_default(&s->stored);
	s->active = s->stored;
	/* The start applies the floor itself through the same transaction, so no forced re-apply is owed here. */
}

enum bc250_clock_curve_error bc250_dpm_curve_set(struct bc250_dpm_curve_state *s, const struct bc250_clock_curve *c,
						 unsigned int trial_ms, unsigned int *level)
{
	enum bc250_clock_curve_error error = bc250_clock_curve_check(c, level);
	if (error != BC250_CLOCK_CURVE_OK) return error;
	if (trial_ms < BC250_DPM_CURVE_TRIAL_MIN_MS) trial_ms = BC250_DPM_CURVE_TRIAL_MIN_MS;
	if (trial_ms > BC250_DPM_CURVE_TRIAL_MAX_MS) trial_ms = BC250_DPM_CURVE_TRIAL_MAX_MS;
	/* A second SET replaces the candidate and restarts the window. The revert target stays s->stored: a chain
	 * of trials never leaves a candidate behind as the thing a deadline would fall back to. */
	s->candidate = *c;
	s->active = *c;
	s->trial = 1;
	s->trial_ms = trial_ms;
	s->elapsed_ms = 0;
	s->serial++;
	s->sets++;
	s->apply = 1;
	return BC250_CLOCK_CURVE_OK;
}

int bc250_dpm_curve_keep(struct bc250_dpm_curve_state *s)
{
	if (!s->trial) return 0;
	/* A curve is kept because it ran, not because it was asked for. Two states look like a running trial and
	 * are not one: a KEEP in the same governor tick as the SET, where the level has not been re-applied yet,
	 * and a SET whose apply the clock gate refused as too hot, where the resync drops the level to the floor.
	 * Both stored a candidate the hardware never carried, and a start that merely resyncs to the floor is a
	 * healthy start, so no boot guard caught it either (0.7.211). */
	if (s->applied != s->serial || s->elapsed_ms < BC250_DPM_CURVE_KEEP_MIN_MS) return -1;
	s->stored = s->candidate;
	s->trial = 0;
	s->elapsed_ms = 0;
	s->keeps++;
	/* The candidate is already active, so the hardware needs nothing: only the disk does. */
	return 1;
}

/* The one revert. Both the deadline and a cancel go through it, so there is exactly one way back. */
static int curve_revert(struct bc250_dpm_curve_state *s)
{
	if (!s->trial) return 0;
	s->active = s->stored;
	s->trial = 0;
	s->elapsed_ms = 0;
	s->serial++;
	s->apply = 1;
	return 1;
}

int bc250_dpm_curve_cancel(struct bc250_dpm_curve_state *s)
{
	if (!curve_revert(s)) return 0;
	s->cancels++;
	return 1;
}

int bc250_dpm_curve_reset(struct bc250_dpm_curve_state *s)
{
	struct bc250_clock_curve line;
	int changed;
	bc250_clock_curve_default(&line);
	changed = s->trial || !bc250_clock_curve_is_default(&s->stored) || !bc250_clock_curve_is_default(&s->active);
	if (s->trial) s->cancels++;
	s->trial = 0;
	s->elapsed_ms = 0;
	s->stored = line;
	s->active = line;
	if (changed) {
		s->serial++;
		s->apply = 1;
	}
	return changed;
}

int bc250_dpm_curve_tick(struct bc250_dpm_curve_state *s, unsigned int dt_ms)
{
	if (!s->trial) return 0;
	s->elapsed_ms = add_ms(s->elapsed_ms, dt_ms);
	if (s->elapsed_ms < s->trial_ms) return 0;
	if (!curve_revert(s)) return 0;
	s->reverts++;
	return 1;
}

int bc250_dpm_curve_take(struct bc250_dpm_curve_state *s)
{
	if (!s->apply) return 0;
	s->apply = 0;
	return 1;
}

void bc250_dpm_curve_applied(struct bc250_dpm_curve_state *s, unsigned int serial)
{
	if (serial == s->serial) s->applied = serial;
}

unsigned int bc250_dpm_curve_level_mv(const struct bc250_dpm_curve_state *s, unsigned int level)
{
	return bc250_clock_curve_mv(&s->active, level);
}

unsigned int bc250_dpm_curve_level_vid(const struct bc250_dpm_curve_state *s, unsigned int level)
{
	return bc250_clock_vid(bc250_clock_curve_mv(&s->active, level));
}

unsigned int bc250_dpm_curve_remaining_ms(const struct bc250_dpm_curve_state *s)
{
	if (!s->trial) return 0;
	return s->elapsed_ms >= s->trial_ms ? 0u : s->trial_ms - s->elapsed_ms;
}

unsigned int bc250_dpm_idle_mhz(const struct bc250_dpm_governor *g)
{
	/* The thermal sub-floor and the idle state stand or fall together below the lab floor: a point the
	 * firmware refused for one of them is refused for the other (bc250_dpm_subfloor_refused). */
	return g->idle_on && g->subfloor_ok ? bc250_dpm_level_mhz(g->idle_level) : 0u;
}

void bc250_dpm_subfloor_refused(struct bc250_dpm_governor *g)
{
	g->subfloor_refusals++;
	g->subfloor_ok = 0;
	if (g->thermal_cap < BC250_DPM_FLOOR_LEVEL) {
		g->thermal_cap = BC250_DPM_FLOOR_LEVEL;
		g->cap_ms = 0;
	}
	/* Nothing under the lab floor works on this part, so the idle state goes with the sub-floor (0.7.207);
	 * bc250_dpm_idle_mhz then reads 0 and the step below never asks for the point again. */
	g->idle = 0;
	g->idle_ms = 0;
	g->idle_acc = 0;
}

enum bc250_dpm_tune_error bc250_dpm_set_tune(struct bc250_dpm_governor *g, const struct bc250_dpm_tune *t)
{
	enum bc250_dpm_tune_error e = bc250_dpm_tune_check(t, g->max_level);
	if (e == BC250_DPM_TUNE_OK) g->tune = *t;
	return e;
}

/* The busy share the idle state reads: the graphics engine's, or the paging node's when that is higher
 * (0.7.207). busy_permille is GRBM GUI_ACTIVE and ring_busy is the GFX ring, so an eviction or an upload on
 * the paging queue shows in neither; without this the clock would drop to the idle point in the middle of such
 * a transfer and nothing would end the state. The load governor keeps the GFX-only share it always had. */
static unsigned int idle_busy_of(const struct bc250_dpm_input *in, unsigned int busy)
{
	unsigned int sdma = in->sdma_permille > 1000u ? 1000u : in->sdma_permille;
	return sdma > busy ? sdma : busy;
}

/* The share at which one tick of its own leaves the state (BC250_DPM_IDLE_EXIT_PERMILLE and why), never at or
 * below the mean the entry window admits, however that is configured. */
static unsigned int idle_exit_permille(const struct bc250_dpm_governor *g)
{
	return g->idle_busy_permille < BC250_DPM_IDLE_EXIT_PERMILLE ? BC250_DPM_IDLE_EXIT_PERMILLE
								    : g->idle_busy_permille + 1u;
}

/* One tick of the idle state (0.7.207). Returns 1 while the clock belongs at the idle point, and sets *left
 * when this tick ended an episode (the caller's exit rule reads it).
 *
 * Entry needs a quiet window: no work outstanding on either ring at any tick of it, and a mean busy share
 * under idle_busy_permille over the whole hold time. The mean, not a strict zero, because the desktop on
 * the GPU wakes for a cursor or a frame now and then: one active GRBM sample in a 3 s window at the
 * 1 ms sample rate is 0.3 permille, and the default admits 2. A window whose mean is too high starts
 * again, so a busy GPU never enters, and the worst case from "the GPU went quiet" to the idle point is
 * two hold times (the burst lands at the end of a window that then has to run again).
 *
 * Two rules end an episode, and the fast one is the ring: work outstanding on the GFX ring or activity on the
 * paging node leaves the state in that tick, which is every submission this driver makes. A tick whose own busy
 * share reaches idle_exit_permille (half its wall time) leaves as well, for work the ring accounting cannot see.
 * Below that share the trailing window decides: the same window, the same admitted mean as the entry, so the
 * single desktop frame the entry rule tolerates does not leave the state, and work that keeps the GPU busier
 * than the admitted mean leaves within one hold time. The clock is back at the lab floor after the governor's
 * detection (one tick) plus the SMU transaction the caller runs; bc250_dpm_step names the latency.
 *
 * The state does not run at all while another rule owns the clock: a runtime floor (an operator asked for
 * a clock), SetStablePowerState (a profiler asked for one steady clock), a temperature at or above HOT_MC
 * or a hot episode (the thermal cap is stepping and must not be undercut by a state with its own timing),
 * and no reading at all (blind, the driver holds the one point this part is known to run at). */
static int idle_step(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in, unsigned int busy,
		     unsigned int dt, int *left)
{
	int allowed = g->idle_on && g->subfloor_ok && !g->stable && !g->hot &&
		      g->idle_level < BC250_DPM_FLOOR_LEVEL &&
		      g->tune.floor_level == BC250_DPM_FLOOR_LEVEL &&
		      in->temperature_valid && in->temperature_mc < BC250_DPM_HOT_MC;
	unsigned int idle_busy = idle_busy_of(in, busy);

	*left = 0;
	if (!allowed || in->ring_busy || (g->idle && idle_busy >= idle_exit_permille(g))) {
		if (g->idle) {
			g->idle = 0;
			g->idle_exits++;
			*left = 1;
		}
		g->idle_ms = 0;
		g->idle_acc = 0;
		return 0;
	}
	g->idle_ms = add_ms(g->idle_ms, dt);
	g->idle_acc = add_ms(g->idle_acc, idle_busy * dt);
	if (g->idle_ms >= g->idle_hold_ms) {
		int too_busy = (unsigned long long)g->idle_acc >
			       (unsigned long long)g->idle_busy_permille * g->idle_ms;
		g->idle_ms = 0;		/* entered, left, or simply too busy: either way the window starts again */
		g->idle_acc = 0;
		if (too_busy) {
			/* Before the state: the candidate window failed and the next one decides. In the state: a
			 * whole window above the admitted mean is the slow way out, for work that stays under the
			 * exit share and that the ring accounting does not show. */
			if (g->idle) {
				g->idle = 0;
				g->idle_exits++;
				*left = 1;
			}
			return 0;
		}
		if (!g->idle) {
			g->idle = 1;
			g->idle_entries++;
			return 1;	/* the caller's apply takes the clock there: no time at the point yet */
		}
	}
	if (!g->idle) return 0;
	g->idle_total_ms = add_ms(g->idle_total_ms, dt);
	return 1;
}

/* ---- the soft zone's lead (0.7.213, BD-087) ---------------------------------------------------------- */

/* The lead in mC: the rise of the last BC250_DPM_ZONE_SLOPE_MS, scaled to lead_ms. 0 for a falling or flat die, for a
 * ring that is not full yet, and with the lead turned off - every one of those cases reads the raw sensor, which is the
 * conservative direction for the release rules and the only honest one for the step-down rules.
 *
 * The oldest slot of a full ring is slope_head, the slot about to be overwritten. Ages come from unsigned differences
 * of a wrapping millisecond clock, so they stay exact over a start of any length; rise x lead_ms needs 64 bits because
 * a reading that jumps the whole sensor range would overflow 32 (70000 mC x 60000 ms). */
static int zone_lead_mc(const struct bc250_dpm_governor *g, int now_mc, unsigned int lead_ms)
{
	unsigned int age;
	int rise;

	if (!lead_ms || g->slope_count < BC250_DPM_ZONE_SLOPE_SLOTS) return 0;
	age = g->zone_now_ms - g->slope_at_ms[g->slope_head];
	if (!age) return 0;
	rise = now_mc - g->slope_mc[g->slope_head];
	if (rise <= 0) return 0;
	return (int)((long long)rise * (long long)lead_ms / (long long)age);
}

/* One reading into the ring, at most one per slot. A tick without a reading empties it: the next lead waits for a whole
 * fresh window, because a slope measured across a gap would be a guess about what happened in the gap. */
static void zone_slope_sample(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in, unsigned int dt)
{
	g->zone_now_ms += dt;			/* wraps; only differences of it are ever read */
	if (!in->temperature_valid) {
		g->slope_count = 0;
		g->slope_head = 0;
		g->slope_push_ms = 0;
		return;
	}
	g->slope_push_ms += dt;
	if (g->slope_push_ms < BC250_DPM_ZONE_SLOPE_SLOT_MS && g->slope_count) return;
	g->slope_push_ms = 0;
	g->slope_mc[g->slope_head] = in->temperature_mc;
	g->slope_at_ms[g->slope_head] = g->zone_now_ms;
	g->slope_head = (g->slope_head + 1u) % BC250_DPM_ZONE_SLOPE_SLOTS;
	if (g->slope_count < BC250_DPM_ZONE_SLOPE_SLOTS) g->slope_count++;
}

unsigned int bc250_dpm_step(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in)
{
	unsigned int dt = in->dt_ms > BC250_DPM_MAX_DT_MS ? BC250_DPM_MAX_DT_MS : in->dt_ms;
	unsigned int busy = in->busy_permille > 1000u ? 1000u : in->busy_permille;
	unsigned int cur = g->level > BC250_DPM_TOP_LEVEL ? BC250_DPM_TOP_LEVEL : g->level;
	unsigned int want = cur, limit, target;
	unsigned int thermal = BC250_DPM_THROTTLE_NONE;
	int left = 0, idle_exit = 0;	/* the idle state ended in this tick / this tick returns from under the floor */
	const struct bc250_dpm_tune *t = &g->tune;
	/* bc250_dpm_set_tune admits no floor above the ceiling; a caller that wrote g->tune itself gets it clamped. */
	unsigned int floor = t->floor_level < g->max_level ? t->floor_level : g->max_level;
	/* The reading the soft rules are judged on (0.7.213): this tick's, or its straight-line extrapolation over
	 * zone_lead_ms when the die is rising, whichever is higher. HOT_MC and CRITICAL_MC below read in->temperature_mc
	 * itself. The lead is computed before this tick's reading enters the ring, so it measures a whole window. */
	int lead_mc = in->temperature_valid ? zone_lead_mc(g, in->temperature_mc, t->zone_lead_ms) : 0;
	int teff = in->temperature_mc + lead_mc;

	g->zone_lead_mc = lead_mc;
	zone_slope_sample(g, in, dt);
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
		g->zone_ms = 0;
		thermal = in->temperature_valid ? BC250_DPM_THROTTLE_THERMAL_HARD : BC250_DPM_THROTTLE_SENSOR;
	} else if (in->temperature_mc >= BC250_DPM_HOT_MC) {
		if (!g->hot) {
			/* One step below where the clock is now, whatever the load: at once when the cap last changed at
			 * least a hot step ago. A temperature hovering at 87 C enters again and again, and stepped once per
			 * crossing before 0.7.197 (BD-055); a re-entry inside the hot step only stops raises (the cap down
			 * to the clock, which lowers nothing) and its step follows a hot step after the last change.
			 *
			 * The step is measured from the clock a load could be running at, never from below the cap's own
			 * bottom (0.7.207): the idle point is not a load level, and a cap set to it would hold a loaded
			 * GPU at 500 MHz until the release below 82 C - a point nothing has measured under load. With the
			 * clock at or above the thermal floor this is cur, exactly as in 0.7.205. */
			unsigned int base = cur > thermal_floor(g) ? cur : thermal_floor(g);
			g->hot = 1;
			g->hot_ms = 0;
			g->thermal_events++;
			if (g->thermal_cap >= base) {
				if (g->cap_ms >= t->hot_step_ms) {
					g->thermal_cap = base > thermal_floor(g) ? base - 1u : thermal_floor(g);
					g->cap_ms = 0;
				} else {
					g->thermal_cap = base;
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
		g->zone_ms = 0;
		thermal = BC250_DPM_THROTTLE_THERMAL_SOFT;
	} else if (t->zone_delta_mc && teff >= bc250_dpm_zone_mc(t)) {
		/* The soft zone (0.7.213, BD-087, after session 436): the same shape as the hot branch, at a lower threshold,
		 * on its own timer, and on the lead reading. The cap first comes down to the clock the GPU is running at, then
		 * one level per zone_step_ms for as long as the zone holds. Unlike the hot branch there is no entry/continue
		 * split: the clamp is written every tick, because the zone is where the load is still free to raise the clock
		 * under the cap and the cap has to follow it down again. The first step is therefore a whole zone_step_ms
		 * after entry, which is what the search scored (scratch/thermal-zone/sim).
		 *
		 * The step is measured from a clock a load could run at, never from under the cap's own bottom, for the same
		 * reason as the hot branch (0.7.207): the idle point is not a load level. */
		unsigned int base = cur > thermal_floor(g) ? cur : thermal_floor(g);
		g->hot = 0;
		g->hot_ms = 0;
		g->zone_ticks++;
		if (g->thermal_cap > base) {
			g->thermal_cap = base;
			g->cap_ms = 0;
		}
		g->zone_ms += dt;
		if (g->zone_ms >= t->zone_step_ms) {
			g->zone_ms = 0;
			if (g->thermal_cap > thermal_floor(g)) {
				g->thermal_cap--;
				g->cap_ms = 0;
				g->zone_steps++;
			}
		}
		g->release_ms = 0;
		g->soft_ms = 0;
		thermal = BC250_DPM_THROTTLE_THERMAL_ZONE;
	} else {
		g->hot = 0;
		g->hot_ms = 0;
		g->zone_ms = 0;
		/* The release rules read the lead temperature too (0.7.213): a cap that rises while the die is already climbing
		 * towards the zone is the one thing the search found no threshold can repair afterwards. */
		if (teff < BC250_DPM_RELEASE_MC && g->thermal_cap < g->max_level) {
			g->soft_ms = 0;
			g->release_ms += dt;
			if (g->release_ms >= BC250_DPM_RELEASE_STEP_MS) {
				g->release_ms = 0;
				g->thermal_cap++;
				g->cap_ms = 0;
			}
		} else if (t->soft_delta_mc && g->thermal_cap < g->max_level &&
			   teff < BC250_DPM_HOT_MC - (int)t->soft_delta_mc) {
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
	/* The idle state (0.7.207, owner decision 2026-10-05: 500 MHz while the lab does not work). It is a
	 * lowering below the lab floor, so it comes after the limits and replaces the warm zone and the ramp,
	 * which bound raises only. Three outcomes:
	 *   in idle          the idle point, whatever the load wanted, and never above the limits
	 *   leaving idle     at least the lab floor (bounded by the limits), this tick, not one level at a time
	 *   neither          the warm zone and the ramp as before
	 * The exit is not bounded by the warm zone or the ramp on purpose: every point under the lab floor is
	 * 820 mV, so the return raises no voltage, and the lab floor is the one operating point this part is known
	 * to run at, so the rules that keep a hot part from gaining voltage have nothing to refuse here. The
	 * condition is "the state held this clock", not "the clock is under the thermal floor": the idle point can
	 * be the thermal floor itself (a configured DpmIdleMHz, or the fallback after the firmware refused 500 MHz),
	 * and the thermal cap can hold the clock at the same level, where the ramp's bound must stay. cur under the
	 * cap's own bottom belongs to no cap, so the floor goes in there whether or not an episode just ended: an
	 * administrator's own request, or a point the cap lost (bc250_dpm_subfloor_refused), does not walk back up
	 * through 600 and 700 MHz one ramp interval at a time.
	 *
	 * Exit latency: this tick's detection plus the caller's SMU transaction. The tick is
	 * BC250_DPM_TICK_MS (25 ms) and the transaction's two messages measure in single milliseconds, so the first
	 * work of a burst runs some 25 to 35 ms at the idle point. Neither figure is a bound: the governor's thread
	 * is an ordinary system thread, the period is relative to the end of the previous tick, DpmPause holds its
	 * lock across a power transition, and a raise re-reads the clock up to BC250_CLOCK_SETTLE_READS times with
	 * BC250_CLOCK_SETTLE_US between the reads (50 ms if the firmware reports the clock on its way). Nothing has
	 * measured the figure on the hardware yet; BC250_DPM_MAX_DT_MS is the clamp this policy puts on dt_ms and
	 * says nothing about the real period. */
	if (idle_step(g, in, busy, dt, &left)) {
		/* Never above the limits: the thermal cap's lowest level is the thermal floor, so a cap holding a hot
		 * part at 800 MHz wins over a configured idle point of 900. */
		target = g->idle_level < limit ? g->idle_level : limit;
		g->throttle = BC250_DPM_THROTTLE_IDLE;
	} else if (cur < BC250_DPM_FLOOR_LEVEL && (left || cur < thermal_floor(g))) {
		/* The hardware sits below the lab floor and no rule claims the point any more: the lab floor, or the
		 * limits when they are lower (a critical reading caps at 800 MHz). The governor goes on from there at
		 * the next tick, where cur is the floor and every rule reads as it always did. */
		idle_exit = 1;
		target = BC250_DPM_FLOOR_LEVEL < limit ? BC250_DPM_FLOOR_LEVEL : limit;
		if (target < g->want && g->throttle == BC250_DPM_THROTTLE_NONE)
			g->throttle = BC250_DPM_THROTTLE_IDLE;
	} else {
		/* The warm zone (from WARM_MC, 0.7.200; 87 C = HOT_MC since 0.7.204): no raise of clock or voltage, the level holds;
		 * a lowering, and the paths above, act as below it. It comes last, so it bounds a raise from the load and from the
		 * runtime floor alike, and it names the reason when it is what holds the level. From HOT_MC up the hot cap already
		 * holds the target at or below the running level (a step down, or the clamp of a re-entry), so there the rule is a
		 * backstop that never fires; before 0.7.204 it ended at HOT_MC. The soft release threshold (HOT_MC minus 0.5 to
		 * 4.5 C) now always lies under WARM_MC, so a released cap is followed at the ramp's pace. The cap's timing (cap_ms,
		 * soft_ms) does not depend on this zone.
		 * Since 0.7.213 the threshold is bc250_dpm_warm_mc(): HOT_MC with the soft zone off, and the soft-release
		 * threshold (83.0 C by default) with it on, read with the zone's lead. So the clock stops rising exactly where
		 * the cap stops rising, and from the zone's own threshold up it has been held for 3 C already. */
		if (teff >= bc250_dpm_warm_mc(t) && target > cur) {
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
	}
	/* A return from a point below the lab floor to the floor does not start the ramp's interval again: it adds
	 * no voltage, and treating it as a raise would make the first real raise after every idle episode wait a
	 * ramp interval above the knee. Same condition as the branch above, so a cap-driven 800 or 900 MHz keeps
	 * the ramp's timing. */
	if (target > cur && !(idle_exit && target <= BC250_DPM_FLOOR_LEVEL))
		g->raise_ms = 0;
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
