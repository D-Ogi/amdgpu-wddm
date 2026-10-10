/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 case fan control. See include/bc250_fan.h for the contract and the rules, and bc250-win
 * docs/design/fan.md for the design. Pure: the only outside world this file knows is the port vtable it is
 * given, so driver/shim/test/fan_test.c drives exactly what the miniport drives.
 *
 * The handshake follows the sequence that M803 measured on unit A, which is the out-of-tree nct6687d sequence
 * (GPL-2.0, facts only, no code taken). Where this file differs from it, a comment says why.
 */
#include <string.h>
#include "bc250_fan.h"

/* ---- profiles ------------------------------------------------------------------------------------------------ */

/* STANDARD is the default, and it must never run the fan slower than the BIOS Standard Mode does. What unit A
 * measured under that mode: 65 C -> 65 %, 69 C -> 77 % (b20 read trial), 70 C -> 80 % (M803 at rest, duty 204),
 * 83 C -> 96 % (E01, duty 245). The line below gives 78 %, 84 %, 85 % and 100 % at those four points, 95 % (about
 * 1650 RPM) at 76 C, and 100 % at 80 C. That is under 82 C, the temperature below which the DPM releases a
 * thermal cap, and 7 C under its 87 C warm zone, so the fan is at full speed before the heat starts to cost clock
 * (the curve gave 100 % only at 85 C until 0.7.216, which the owner found late). Below 60 C no measurement exists;
 * the curve then holds 50 % or more, which is a floor the idle board (55 C and up) never reaches.
 * QUIET is quieter than the board below 80 C and is a user's choice. PERFORMANCE is louder everywhere. */
static const struct bc250_fan_curve g_fan_profiles[BC250_FAN_PROFILE_COUNT] = {
	{ 0u, { { 0u, 0u } } },							/* CUSTOM: from the request */
	{ 5u, { { 40u, 50u }, { 60u, 70u }, { 70u, 85u }, { 76u, 95u }, { 80u, 100u } } },
	{ 5u, { { 40u, 30u }, { 60u, 45u }, { 70u, 60u }, { 80u, 80u }, { 85u, 100u } } },
	{ 4u, { { 40u, 60u }, { 55u, 75u }, { 65u, 90u }, { 75u, 100u } } },
};

int bc250_fan_profile_curve(unsigned int profile, struct bc250_fan_curve *out)
{
	memset(out, 0, sizeof(*out));
	if (profile == BC250_FAN_PROFILE_CUSTOM || profile >= BC250_FAN_PROFILE_COUNT)
		return BC250_FAN_ERROR_PROFILE;
	*out = g_fan_profiles[profile];
	return BC250_FAN_ERROR_OK;
}

int bc250_fan_curve_check(const struct bc250_fan_curve *curve)
{
	unsigned int i;

	if (curve->points < BC250_FAN_POINTS_MIN || curve->points > BC250_FAN_POINTS_MAX)
		return BC250_FAN_ERROR_POINTS;
	for (i = 0; i < curve->points; i++) {
		const struct bc250_fan_point *p = &curve->p[i];

		if (p->c < BC250_FAN_TEMP_MIN_C || p->c > BC250_FAN_TEMP_MAX_C)
			return BC250_FAN_ERROR_TEMPERATURE;
		if (p->pct < BC250_FAN_FLOOR_PCT || p->pct > BC250_FAN_FULL_PCT)
			return BC250_FAN_ERROR_DUTY;
		if (i > 0u && p->c <= curve->p[i - 1u].c)
			return BC250_FAN_ERROR_TEMPERATURE;
		if (i > 0u && p->pct < curve->p[i - 1u].pct)
			return BC250_FAN_ERROR_DUTY;
	}
	return BC250_FAN_ERROR_OK;
}

unsigned int bc250_fan_curve_eval(const struct bc250_fan_curve *curve, int mc)
{
	unsigned int i, pct;

	if (curve->points == 0u)
		return BC250_FAN_FULL_PCT;	/* no curve is a doubt, and doubt is full speed */
	if (mc <= (int)curve->p[0].c * 1000) {
		pct = curve->p[0].pct;
	} else if (mc >= (int)curve->p[curve->points - 1u].c * 1000) {
		pct = curve->p[curve->points - 1u].pct;
	} else {
		pct = curve->p[curve->points - 1u].pct;
		for (i = 1; i < curve->points; i++) {
			const struct bc250_fan_point *a = &curve->p[i - 1u], *b = &curve->p[i];
			unsigned int span_mc, into_mc, rise;

			if (mc > (int)b->c * 1000)
				continue;
			span_mc = (b->c - a->c) * 1000u;
			into_mc = (unsigned int)(mc - (int)a->c * 1000);
			rise = b->pct - a->pct;
			/* Rounded up: between two points the fan never runs slower than the line. */
			pct = a->pct + (rise * into_mc + span_mc - 1u) / span_mc;
			break;
		}
	}
	if (pct < BC250_FAN_FLOOR_PCT)
		pct = BC250_FAN_FLOOR_PCT;
	if (pct > BC250_FAN_FULL_PCT)
		pct = BC250_FAN_FULL_PCT;
	return pct;
}

unsigned int bc250_fan_pct_to_raw(unsigned int pct)
{
	if (pct > BC250_FAN_FULL_PCT)
		pct = BC250_FAN_FULL_PCT;
	return (pct * 255u + 50u) / 100u;
}

unsigned int bc250_fan_raw_to_pct(unsigned int raw)
{
	if (raw > 255u)
		raw = 255u;
	return (raw * 100u + 127u) / 255u;
}

/* ---- requests ------------------------------------------------------------------------------------------------ */

int bc250_fan_request_check(const struct bc250_fan_request *request, struct bc250_fan_curve *resolved)
{
	int error;

	memset(resolved, 0, sizeof(*resolved));
	switch (request->mode) {
	case BC250_FAN_MODE_BOARD:
		return BC250_FAN_ERROR_OK;
	case BC250_FAN_MODE_FIXED:
		if (request->fixed_pct < BC250_FAN_FLOOR_PCT || request->fixed_pct > BC250_FAN_FULL_PCT)
			return BC250_FAN_ERROR_DUTY;
		/* A fixed duty does not follow the temperature, so it is never durable. */
		if (request->lease_ms < BC250_FAN_LEASE_MIN_MS || request->lease_ms > BC250_FAN_LEASE_MAX_MS)
			return BC250_FAN_ERROR_LEASE;
		return BC250_FAN_ERROR_OK;
	case BC250_FAN_MODE_CURVE:
		if (request->lease_ms != 0u &&
		    (request->lease_ms < BC250_FAN_LEASE_MIN_MS || request->lease_ms > BC250_FAN_LEASE_MAX_MS))
			return BC250_FAN_ERROR_LEASE;
		if (request->profile >= BC250_FAN_PROFILE_COUNT)
			return BC250_FAN_ERROR_PROFILE;
		if (request->profile == BC250_FAN_PROFILE_CUSTOM)
			*resolved = request->curve;
		else
			(void)bc250_fan_profile_curve(request->profile, resolved);
		error = bc250_fan_curve_check(resolved);
		if (error != BC250_FAN_ERROR_OK)
			memset(resolved, 0, sizeof(*resolved));
		return error;
	default:
		return BC250_FAN_ERROR_MODE;
	}
}

static void apply_request(struct bc250_fan_ctl *ctl, const struct bc250_fan_request *request,
			  const struct bc250_fan_curve *resolved)
{
	ctl->mode = request->mode;
	ctl->profile = request->mode == BC250_FAN_MODE_CURVE ? request->profile : 0u;
	ctl->curve = *resolved;
	ctl->fixed_pct = request->mode == BC250_FAN_MODE_FIXED ? request->fixed_pct : 0u;
	ctl->lease_ms = request->mode == BC250_FAN_MODE_BOARD ? 0u : request->lease_ms;
	/* A durable request is also what a later lease ends with. A FIXED duty always has a lease. */
	if (ctl->lease_ms == 0u) {
		ctl->durable_mode = ctl->mode;
		ctl->durable_profile = ctl->profile;
		ctl->durable_curve = ctl->curve;
	}
}

void bc250_fan_init(struct bc250_fan_ctl *ctl, int enabled, const struct bc250_fan_request *start)
{
	struct bc250_fan_restore restore = ctl->restore;
	struct bc250_fan_request standard;
	struct bc250_fan_curve resolved;

	memset(ctl, 0, sizeof(*ctl));
	ctl->restore = restore;			/* rule 2: once per power-on, so a stop and a start keep it */
	ctl->enabled = enabled ? 1u : 0u;
	ctl->boost_enabled = 1u;		/* rule 10: on unless FanLoadBoost says otherwise */
	memset(&standard, 0, sizeof(standard));
	standard.mode = BC250_FAN_MODE_CURVE;
	standard.profile = BC250_FAN_PROFILE_STANDARD;
	/* A start never runs a leased mode: whatever the stored choice says about a lease, the start drops it. */
	if (start != NULL && start->lease_ms == 0u && start->mode != BC250_FAN_MODE_FIXED &&
	    bc250_fan_request_check(start, &resolved) == BC250_FAN_ERROR_OK)
		apply_request(ctl, start, &resolved);
	else {
		(void)bc250_fan_request_check(&standard, &resolved);
		apply_request(ctl, &standard, &resolved);
	}
	ctl->state = ctl->enabled ? BC250_FAN_STATE_BOARD : BC250_FAN_STATE_OFF;
}

int bc250_fan_set(struct bc250_fan_ctl *ctl, const struct bc250_fan_request *request)
{
	struct bc250_fan_curve resolved;
	int error = bc250_fan_request_check(request, &resolved);

	if (error != BC250_FAN_ERROR_OK)
		return error;
	apply_request(ctl, request, &resolved);
	ctl->held_back = 0;
	ctl->clean_ms = 0;
	return BC250_FAN_ERROR_OK;
}

void bc250_fan_load_boost(struct bc250_fan_ctl *ctl, int enabled)
{
	ctl->boost_enabled = enabled ? 1u : 0u;
	if (!ctl->boost_enabled) {
		/* The duty itself is not touched here: the next step reads the curve again and the slope rule (rule 7)
		 * walks the fan down, as it does at the end of a boost that ran its course. */
		ctl->boost = 0;
		ctl->boost_why = 0;
		ctl->boost_load_ms = 0;
		ctl->boost_hold_ms = 0;
	}
}

int bc250_fan_renew(struct bc250_fan_ctl *ctl, unsigned int lease_ms)
{
	if (ctl->lease_ms == 0u)
		return -1;
	if (lease_ms < BC250_FAN_LEASE_MIN_MS || lease_ms > BC250_FAN_LEASE_MAX_MS)
		return -1;
	ctl->lease_ms = lease_ms;
	return 0;
}

/* ---- the handshake ------------------------------------------------------------------------------------------- */

static void poll_wait(const struct bc250_hwmon_io *io, unsigned int us)
{
	if (io->delay_us != 0)
		io->delay_us(io->context, us);
}

/* A register the handshake must read. The allowlist admits every one of them, so a refusal here is a bug, and a
 * bug in the handshake must look like a busy chip rather than like a reading of 0. */
static unsigned int engine(const struct bc250_hwmon_io *io)
{
	unsigned int value = 0;

	if (bc250_hwmon_read8(io, BC250_HWMON_REG_ENGINE, &value) != 0)
		return BC250_HWMON_ENGINE_LOCK;
	return value;
}

static int phase_open(unsigned int status)
{
	return (status & BC250_HWMON_ENGINE_LOCK) == 0u && (status & BC250_HWMON_ENGINE_PHASE) != 0u;
}

int bc250_fan_open(const struct bc250_hwmon_io *io)
{
	unsigned int status = engine(io), request = 0, polls;

	/* A phase that is open already (our own close timed out last time) is used as it is, as nct6687d does. */
	if (phase_open(status))
		return 0;
	/* The engine must be out of any phase and no request may be pending before ours goes in. */
	for (polls = 0;; polls++) {
		if (bc250_hwmon_read8(io, BC250_HWMON_REG_FAN_CTRL, &request) != 0)
			return BC250_FAN_E_BUSY;
		if ((status & BC250_HWMON_ENGINE_PHASE) == 0u && (request & BC250_HWMON_FAN_CFG_REQUEST) == 0u)
			break;
		if (polls >= BC250_FAN_POLL_MAX)
			return BC250_FAN_E_BUSY;
		poll_wait(io, BC250_FAN_POLL_US);
		status = engine(io);
	}
	if (bc250_hwmon_write8(io, BC250_HWMON_REG_FAN_CTRL, BC250_HWMON_FAN_CFG_REQUEST) != 0)
		return BC250_FAN_E_BUSY;
	for (polls = 0;; polls++) {
		status = engine(io);
		if (phase_open(status))
			return 0;
		if (polls >= BC250_FAN_POLL_MAX)
			return BC250_FAN_E_OPEN;
		poll_wait(io, BC250_FAN_POLL_US);
	}
}

int bc250_fan_close(const struct bc250_hwmon_io *io)
{
	unsigned int status, polls;

	if (bc250_hwmon_write8(io, BC250_HWMON_REG_FAN_CTRL, BC250_HWMON_FAN_CFG_DONE) != 0)
		return BC250_FAN_E_CLOSE;
	for (polls = 0;; polls++) {
		status = engine(io);
		if ((status & BC250_HWMON_ENGINE_CHECK_DONE) != 0u)
			break;
		if (polls >= BC250_FAN_POLL_MAX)
			return BC250_FAN_E_CLOSE;
		poll_wait(io, BC250_FAN_POLL_US);
	}
	if ((status & BC250_HWMON_ENGINE_INVALID) != 0u)
		return BC250_FAN_E_INVALID;
	if ((status & BC250_HWMON_ENGINE_LOCK) == 0u)
		return BC250_FAN_E_UNLOCKED;
	return 0;
}

#define FAN_BIT (1u << BC250_FAN_CHANNEL)
#define FAN_TARGET BC250_HWMON_REG_DUTY_WRITE(BC250_FAN_CHANNEL)

/* Close after a failure inside the phase. The first failure is the answer: a close that also fails changes
 * nothing about what the caller must do. */
static int close_after(const struct bc250_hwmon_io *io, int status)
{
	int closed = bc250_fan_close(io);

	return status != 0 ? status : closed;
}

/* The first takeover: open, take the record if there is none, set our mode bit, write the target, read both back,
 * close, and read the mode once more after the close (the chip applies the set at the close). */
static int chip_take(const struct bc250_hwmon_io *io, struct bc250_fan_restore *restore, unsigned int raw)
{
	unsigned int mode = 0, target = 0;
	int status = bc250_fan_open(io);

	if (status != 0)
		return status;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &mode) != 0 ||
	    bc250_hwmon_read8(io, FAN_TARGET, &target) != 0)
		return close_after(io, BC250_FAN_E_VERIFY);
	if (!restore->valid) {
		if ((mode & FAN_BIT) != 0u) {
			/* Our bit is set before we ever wrote it: a previous driver held the fan when it died. The target
			 * in the chip is that driver's duty, not the board's, so it is not recorded as the board's. */
			restore->mode = (mode & ~FAN_BIT) & 0xFFu;
			restore->target = BC250_HWMON_TARGET_REST;
			restore->substituted = 1;
		} else {
			restore->mode = mode & 0xFFu;
			restore->target = target & 0xFFu;
			restore->substituted = 0;
		}
		restore->valid = 1;
	}
	if (bc250_hwmon_write8(io, BC250_HWMON_REG_MODE, (mode | FAN_BIT) & 0xFFu) != 0 ||
	    bc250_hwmon_write8(io, FAN_TARGET, raw) != 0)
		return close_after(io, BC250_FAN_E_VERIFY);
	if (bc250_hwmon_read8(io, FAN_TARGET, &target) != 0 || target != raw)
		return close_after(io, BC250_FAN_E_VERIFY);
	status = bc250_fan_close(io);
	if (status != 0)
		return status;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &mode) != 0 || (mode & FAN_BIT) == 0u)
		return BC250_FAN_E_MODE;
	return 0;
}

/* A new duty while the driver holds the fan: open, the target, its read-back, close, our bit still set. */
static int chip_write(const struct bc250_hwmon_io *io, unsigned int raw)
{
	unsigned int target = 0, mode = 0;
	int status = bc250_fan_open(io);

	if (status != 0)
		return status;
	if (bc250_hwmon_write8(io, FAN_TARGET, raw) != 0 ||
	    bc250_hwmon_read8(io, FAN_TARGET, &target) != 0 || target != raw)
		return close_after(io, BC250_FAN_E_VERIFY);
	status = bc250_fan_close(io);
	if (status != 0)
		return status;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &mode) != 0 || (mode & FAN_BIT) == 0u)
		return BC250_FAN_E_MODE;
	return 0;
}

/* The way back: the target first, then the mode with our bit clear (rule 3), both read back inside the phase,
 * and the mode once more after the close. Only our bit is touched: the other bits are whatever the chip holds now,
 * not what the record held, because the record is a minute or a day old and the chip owns those bits. */
static int chip_restore(const struct bc250_hwmon_io *io, const struct bc250_fan_restore *restore)
{
	unsigned int target = restore->valid ? restore->target : BC250_HWMON_TARGET_REST, mode = 0, check = 0;
	int status = bc250_fan_open(io);

	if (status != 0)
		return status;
	if (bc250_hwmon_write8(io, FAN_TARGET, target) != 0 ||
	    bc250_hwmon_read8(io, FAN_TARGET, &check) != 0 || check != target)
		return close_after(io, BC250_FAN_E_VERIFY);
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &mode) != 0 ||
	    bc250_hwmon_write8(io, BC250_HWMON_REG_MODE, mode & ~FAN_BIT & 0xFFu) != 0 ||
	    bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &check) != 0 || (check & FAN_BIT) != 0u)
		return close_after(io, BC250_FAN_E_MODE);
	status = bc250_fan_close(io);
	if (status != 0)
		return status;
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &mode) != 0 || (mode & FAN_BIT) != 0u)
		return BC250_FAN_E_MODE;
	return 0;
}

static unsigned int reason_of(int status)
{
	if (status == BC250_FAN_E_VERIFY)
		return BC250_FAN_REASON_READBACK;
	if (status == BC250_FAN_E_MODE)
		return BC250_FAN_REASON_MODE;
	return BC250_FAN_REASON_HANDSHAKE;
}

/* ---- the exit paths ------------------------------------------------------------------------------------------ */

int bc250_fan_handback(const struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl, unsigned int reason)
{
	int status = 0;

	ctl->reason = reason;
	if (ctl->controlling) {
		status = chip_restore(io, &ctl->restore);
		ctl->last_error = status;
		if (status != 0) {
			/* The fan may still be ours. Keep saying so: the next exit path, the bugcheck callback and the
			 * fault retry all key on `controlling`, and a false "given back" would switch all three off. */
			ctl->failures++;
			ctl->fault = 1;
			ctl->fault_retry_ms = 0;
			ctl->state = BC250_FAN_STATE_FAULT;
			return status;
		}
		ctl->controlling = 0;
		ctl->handbacks++;
	}
	ctl->written_raw = 0;
	ctl->applied_pct = 0;
	ctl->since_write_ms = 0;
	ctl->stopped_samples = 0;
	/* Rule 10: a boost is a duty, and the duty is now the board's. The heavy-time account goes with it, so a
	 * driver that takes the fan again has to see the load again before it blows at full speed. */
	ctl->boost = 0;
	ctl->boost_why = 0;
	ctl->boost_load_ms = 0;
	ctl->boost_hold_ms = 0;
	ctl->state = ctl->fault ? BC250_FAN_STATE_FAULT : ctl->enabled ? BC250_FAN_STATE_BOARD : BC250_FAN_STATE_OFF;
	return 0;
}

void bc250_fan_handback_blind(const struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl)
{
	unsigned int polls, mode = BC250_HWMON_MODE_REST, target;

	/* Rule 10, as in bc250_fan_handback(): a boost is a duty, and this path gives the duty back. FanResetDevice
	 * takes it on the way to a hibernation, so a start that came back with the account still armed would drive
	 * 100 % at once instead of the curve. Cleared before the hold below, so the two give-backs are symmetric
	 * whether this one holds the fan or not. */
	ctl->boost = 0;
	ctl->boost_why = 0;
	ctl->boost_load_ms = 0;
	ctl->boost_hold_ms = 0;
	if (!ctl->controlling)
		return;
	target = ctl->restore.valid ? ctl->restore.target : BC250_HWMON_TARGET_REST;
	(void)bc250_hwmon_write8(io, BC250_HWMON_REG_FAN_CTRL, BC250_HWMON_FAN_CFG_REQUEST);
	for (polls = 0; polls < BC250_FAN_BLIND_POLL_MAX; polls++) {
		if (phase_open(engine(io)))
			break;
		poll_wait(io, BC250_FAN_BLIND_POLL_US);
	}
	/* Written whether the phase came or not: there is no second chance on this path, and a write outside the
	 * phase is one the chip ignores, which leaves the state this path found. */
	(void)bc250_hwmon_write8(io, FAN_TARGET, target);
	if (bc250_hwmon_read8(io, BC250_HWMON_REG_MODE, &mode) != 0)
		mode = BC250_HWMON_MODE_REST;
	(void)bc250_hwmon_write8(io, BC250_HWMON_REG_MODE, mode & ~FAN_BIT & 0xFFu);
	(void)bc250_hwmon_write8(io, BC250_HWMON_REG_FAN_CTRL, BC250_HWMON_FAN_CFG_DONE);
	ctl->controlling = 0;
	ctl->reason = BC250_FAN_REASON_BUGCHECK;
}

/* ---- the control step ---------------------------------------------------------------------------------------- */

/* The guard temperature and the doubt about it. Tctl is the primary input; the EC's own SB-TSI reading of the same
 * die is the cross-check, and the guard is the hotter of the two. */
static unsigned int read_guard(struct bc250_fan_ctl *ctl, const struct bc250_fan_input *in)
{
	int guard;

	ctl->guard_valid = 0;
	if (!in->tctl_valid)
		return BC250_FAN_REASON_TEMPERATURE;
	if (!in->reader_valid)
		return BC250_FAN_REASON_READER;
	guard = in->tctl_mc;
	if (in->tsi_mapped && in->tsi_valid) {
		int apart = in->tsi_mc > in->tctl_mc ? in->tsi_mc - in->tctl_mc : in->tctl_mc - in->tsi_mc;

		if (apart > BC250_FAN_DISAGREE_MC)
			return BC250_FAN_REASON_TEMPERATURE;
		if (in->tsi_mc > guard)
			guard = in->tsi_mc;
	}
	/* A refused SB-TSI value in one sample is a collision on an unarbitrated window; Tctl carries the tick. A reader
	 * that stays refused goes stale, and that is a doubt above. */
	ctl->guard_mc = guard;
	ctl->guard_valid = 1;
	return BC250_FAN_REASON_NONE;
}

static void track_emergency(struct bc250_fan_ctl *ctl, unsigned int dt)
{
	if (!ctl->guard_valid)
		return;
	if (ctl->guard_mc >= BC250_FAN_EMERGENCY_ON_MC) {
		if (!ctl->emergency)
			ctl->emergencies++;
		ctl->emergency = 1;
		ctl->emergency_cool_ms = 0;
		return;
	}
	if (!ctl->emergency)
		return;
	if (ctl->guard_mc > BC250_FAN_EMERGENCY_OFF_MC) {
		ctl->emergency_cool_ms = 0;
		return;
	}
	ctl->emergency_cool_ms += dt;
	if (ctl->emergency_cool_ms >= BC250_FAN_EMERGENCY_HOLD_MS) {
		ctl->emergency = 0;
		ctl->emergency_cool_ms = 0;
	}
}

static void track_effective(struct bc250_fan_ctl *ctl)
{
	if (!ctl->guard_valid)
		return;
	if (!ctl->effective_valid || ctl->guard_mc > ctl->effective_mc) {
		ctl->effective_mc = ctl->guard_mc;
		ctl->effective_valid = 1;
	} else if (ctl->guard_mc < ctl->effective_mc - BC250_FAN_HYSTERESIS_MC) {
		ctl->effective_mc = ctl->guard_mc + BC250_FAN_HYSTERESIS_MC;
	}
}

/* The guard temperature's rise, over windows of BC250_FAN_BOOST_RISE_MS. The window is closed by elapsed time,
 * so the figure means the same whatever the governor's cadence is, and the rise of the window that closed last
 * is what the feed-forward reads. The windows follow each other: at the 1 s cadence one closes every three
 * steps, because the step that closes a window opens the next one at its own reading. */
static void track_rise(struct bc250_fan_ctl *ctl, unsigned int dt)
{
	if (!ctl->guard_valid)
		return;
	/* The window opens at this step's guard reading: at the first step that has a guard, and at a step that took
	 * longer than one step may pay (a starved governor thread, a resume). Such a step says nothing about a 3 s
	 * window, and closing one over a gap of unknown length is what would make the threshold more sensitive the
	 * later the step is. The 1 ms is the marker of an open window, so that the next step adds its own dt instead
	 * of opening a second one; a window therefore measures a whole BC250_FAN_BOOST_RISE_MS. */
	if (ctl->rise_ms == 0u || dt > BC250_FAN_BOOST_STEP_MAX_MS) {
		ctl->rise_ref_mc = ctl->guard_mc;
		ctl->rise_ms = 1u;
		return;
	}
	ctl->rise_ms += dt;
	if (ctl->rise_ms < BC250_FAN_BOOST_RISE_MS)
		return;
	ctl->rise_mc = ctl->guard_mc - ctl->rise_ref_mc;
	ctl->rise_valid = 1;
	ctl->rise_ref_mc = ctl->guard_mc;
	ctl->rise_ms = 1u;
}

/* Is this step's load heavy (rule 10)? The bits say which signal called it heavy, and zero means it is not. */
static unsigned int heavy_load(const struct bc250_fan_ctl *ctl, const struct bc250_fan_input *in)
{
	unsigned int why = 0;

	if (!in->load_valid)
		return 0;
	/* A busy GPU at the idle point is a desktop that composes, not a load that heats the board. An unknown
	 * clock passes: a missing reading is no reason to run the fan slower. */
	if (in->busy_permille >= BC250_FAN_BOOST_BUSY_PERMILLE &&
	    (in->gfx_mhz == 0u || in->gfx_mhz >= BC250_FAN_BOOST_MHZ))
		why |= BC250_FAN_BOOST_WHY_BUSY;
	if (in->power_valid && in->socket_mw >= BC250_FAN_BOOST_POWER_MW)
		why |= BC250_FAN_BOOST_WHY_POWER;
	if (ctl->rise_valid && ctl->rise_mc >= BC250_FAN_BOOST_RISE_MC)
		why |= BC250_FAN_BOOST_WHY_RISE;
	return why;
}

/* The feed-forward itself: the heavy-time account, the engagement and the way out of it. It decides nothing about
 * the duty; it only says whether the step's target is raised to full speed below. */
static void track_boost(struct bc250_fan_ctl *ctl, const struct bc250_fan_input *in, unsigned int dt)
{
	unsigned int why;

	track_rise(ctl, dt);
	/* The board's own curve and a latched fault are states in which this file writes no duty at all, so there is
	 * nothing for a feed-forward to raise. A fixed duty under a lease keeps the account running: the raise waits
	 * for the curve the lease ends with, and a load that outlives the lease is then already known. */
	if (!ctl->boost_enabled || ctl->mode == BC250_FAN_MODE_BOARD || ctl->fault) {
		ctl->boost = 0;
		ctl->boost_why = 0;
		ctl->boost_load_ms = 0;
		ctl->boost_hold_ms = 0;
		return;
	}
	why = heavy_load(ctl, in);
	if (why != 0u) {
		/* One step pays at most BC250_FAN_BOOST_STEP_MAX_MS in, which is under the arming time: a single late
		 * step cannot arm the rule, and a sustained load is still two steps away from it. A step that is not
		 * heavy takes its whole length away, because a long gap is a reason to let the boost go. */
		unsigned int paid = dt > BC250_FAN_BOOST_STEP_MAX_MS ? BC250_FAN_BOOST_STEP_MAX_MS : dt;

		ctl->boost_why = why;
		ctl->boost_load_ms = ctl->boost_load_ms + paid > BC250_FAN_BOOST_LOAD_MAX_MS ?
				     BC250_FAN_BOOST_LOAD_MAX_MS : ctl->boost_load_ms + paid;
	} else {
		ctl->boost_load_ms = ctl->boost_load_ms > dt ? ctl->boost_load_ms - dt : 0u;
	}
	/* The engage needs a duty of ours to raise. Before the first take-over, and in the 30 s after a doubt gave
	 * the fan back, the board's own curve drives the fan: the account above keeps running, so a load that
	 * outlives such a wait is already known when the driver takes the fan again, but the flag, the count, the
	 * time and the log line stay off until the fan is ours. */
	if (!ctl->controlling) {
		ctl->boost = 0;
		ctl->boost_hold_ms = 0;
		return;
	}
	if (ctl->boost)
		ctl->boost_ms += dt;
	if (ctl->boost_load_ms >= BC250_FAN_BOOST_ARM_MS) {
		if (!ctl->boost)
			ctl->boosts++;
		ctl->boost = 1;
		ctl->boost_hold_ms = 0;
		return;
	}
	if (!ctl->boost)
		return;
	/* The load has stopped being heavy. The boost holds for BC250_FAN_BOOST_HOLD_MS, and after that until the
	 * curve itself asks for less than the duty in force: the board must be back under the curve's own point for
	 * that duty before the fan is allowed to slow down at all. */
	ctl->boost_hold_ms += dt;
	if (ctl->boost_hold_ms < BC250_FAN_BOOST_HOLD_MS)
		return;
	if (ctl->mode == BC250_FAN_MODE_CURVE && ctl->applied_pct != 0u &&
	    bc250_fan_curve_eval(&ctl->curve, ctl->effective_mc) >= ctl->applied_pct)
		return;
	ctl->boost = 0;
	ctl->boost_why = 0;
	ctl->boost_hold_ms = 0;
}

/* Raise fast, fall slowly. A first duty after a takeover, an emergency and a fixed duty skip the rule. */
static unsigned int slope(struct bc250_fan_ctl *ctl, unsigned int target, unsigned int dt)
{
	unsigned int applied = ctl->applied_pct, next;

	if (!ctl->controlling || applied == 0u || target >= applied) {
		ctl->below_ms = 0;
		ctl->fall_wait_ms = 0;
		return target;
	}
	ctl->below_ms += dt;
	if (ctl->below_ms < BC250_FAN_FALL_HOLD_MS)
		return applied;
	if (ctl->fall_wait_ms > dt) {
		ctl->fall_wait_ms -= dt;
		return applied;
	}
	ctl->fall_wait_ms = BC250_FAN_FALL_GAP_MS;
	next = applied > target + BC250_FAN_FALL_STEP_PCT ? applied - BC250_FAN_FALL_STEP_PCT : target;
	return next;
}

/* Puts `pct` into the chip: the takeover the first time, a write when the byte changes, nothing otherwise. A chip
 * that refuses gets the fan back at once and the start latches the fault. */
static int drive(const struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl, unsigned int pct)
{
	unsigned int raw;
	int status;

	if (pct < BC250_FAN_FLOOR_PCT)
		pct = BC250_FAN_FLOOR_PCT;
	if (pct > BC250_FAN_FULL_PCT)
		pct = BC250_FAN_FULL_PCT;
	raw = bc250_fan_pct_to_raw(pct);
	ctl->applied_pct = pct;
	if (ctl->controlling && raw == ctl->written_raw)
		return 0;
	if (!ctl->controlling) {
		status = chip_take(io, &ctl->restore, raw);
		/* Once the phase was open, the mode bit may be set even when a later step failed, so such a take is
		 * handed back as if it had succeeded: the restore clears only our bit and writes the board's target.
		 * A take that never got the phase wrote no configuration register and holds nothing. */
		ctl->controlling = status != BC250_FAN_E_BUSY && status != BC250_FAN_E_OPEN ? 1u : 0u;
		if (status == 0)
			ctl->takeovers++;
	} else {
		status = chip_write(io, raw);
	}
	ctl->last_error = status;
	if (status != 0) {
		ctl->failures++;
		ctl->fault = 1;
		(void)bc250_fan_handback(io, ctl, reason_of(status));
		ctl->state = BC250_FAN_STATE_FAULT;
		return status;
	}
	ctl->written_raw = raw;
	ctl->since_write_ms = 0;
	ctl->stopped_samples = 0;
	ctl->writes++;
	return 0;
}

int bc250_fan_tick(const struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl, const struct bc250_fan_input *in)
{
	unsigned int dt = in->dt_ms, doubt, target;
	int status;

	if (ctl->controlling)
		ctl->since_write_ms += dt;
	doubt = read_guard(ctl, in);
	ctl->doubt = doubt;
	track_emergency(ctl, dt);
	track_effective(ctl);
	/* Rule 10, before every branch below: the heavy-time account must keep running while a doubt forces full
	 * speed, so that a load which outlasts the doubt does not have to be learned twice. */
	track_boost(ctl, in, dt);

	/* A fault: no write in this start, except the way back for as long as the fan may still be ours. */
	if (ctl->fault) {
		ctl->state = BC250_FAN_STATE_FAULT;
		if (!ctl->controlling)
			return 0;
		ctl->fault_retry_ms += dt;
		if (ctl->fault_retry_ms < BC250_FAN_FAULT_RETRY_MS)
			return 0;
		ctl->fault_retry_ms = 0;
		status = chip_restore(io, &ctl->restore);
		ctl->last_error = status;
		if (status == 0) {
			ctl->controlling = 0;
			ctl->handbacks++;
		}
		return status;
	}
	if (!ctl->enabled)
		return bc250_fan_handback(io, ctl, BC250_FAN_REASON_DISABLED);

	/* A lease that ran out: the durable mode from before the lease comes back (rule 9). That is the driver's
	 * curve, which follows the temperature, or the board, which gets the fan back and keeps it until somebody
	 * asks again. */
	if (ctl->lease_ms != 0u) {
		if (dt >= ctl->lease_ms) {
			ctl->lease_ms = 0;
			ctl->lease_expiries++;
			ctl->fixed_pct = 0;
			if (ctl->durable_mode != BC250_FAN_MODE_CURVE) {
				ctl->mode = BC250_FAN_MODE_BOARD;
				ctl->profile = 0;
				return bc250_fan_handback(io, ctl, BC250_FAN_REASON_LEASE);
			}
			ctl->mode = BC250_FAN_MODE_CURVE;
			ctl->profile = ctl->durable_profile;
			ctl->curve = ctl->durable_curve;
		} else
			ctl->lease_ms -= dt;
	}
	if (ctl->mode == BC250_FAN_MODE_BOARD) {
		if (ctl->controlling)
			return bc250_fan_handback(io, ctl, BC250_FAN_REASON_USER);
		ctl->state = BC250_FAN_STATE_BOARD;
		return 0;
	}

	/* Doubt: full speed now, the board after BC250_FAN_DOUBT_HANDBACK_MS. A driver that does not hold the fan does
	 * not take it on a doubt: the board's own curve is the safe state. */
	if (doubt != BC250_FAN_REASON_NONE) {
		ctl->clean_ms = 0;
		if (!ctl->controlling) {
			ctl->state = BC250_FAN_STATE_BOARD;
			return 0;
		}
		if (ctl->doubt_ms == 0u)
			ctl->doubts++;
		ctl->doubt_ms += dt;
		if (ctl->doubt_ms >= BC250_FAN_DOUBT_HANDBACK_MS) {
			ctl->doubt_ms = 0;
			ctl->held_back = 1;
			return bc250_fan_handback(io, ctl, doubt);
		}
		ctl->below_ms = 0;
		ctl->fall_wait_ms = 0;
		status = drive(io, ctl, BC250_FAN_FULL_PCT);
		if (status == 0)
			ctl->state = BC250_FAN_STATE_DOUBT;
		return status;
	}
	ctl->doubt_ms = 0;
	if (ctl->held_back) {
		ctl->clean_ms += dt;
		if (ctl->clean_ms < BC250_FAN_RETAKE_MS || ctl->retakes >= BC250_FAN_RETAKES_MAX) {
			ctl->state = BC250_FAN_STATE_BOARD;
			return 0;
		}
		ctl->held_back = 0;
		ctl->clean_ms = 0;
		ctl->retakes++;
	}

	/* The fan itself, once a write has had time to land. */
	if (ctl->controlling && ctl->since_write_ms >= BC250_FAN_SETTLE_MS) {
		if (in->readback_valid) {
			unsigned int got = in->readback_raw, want = ctl->written_raw;
			unsigned int apart = got > want ? got - want : want - got;

			if (apart > BC250_FAN_READBACK_TOLERANCE) {
				ctl->failures++;
				ctl->fault = 1;
				(void)bc250_fan_handback(io, ctl, BC250_FAN_REASON_READBACK);
				ctl->state = BC250_FAN_STATE_FAULT;
				return BC250_FAN_E_VERIFY;
			}
		}
		if (in->rpm_valid && in->rpm == 0u && ctl->written_raw >= bc250_fan_pct_to_raw(BC250_FAN_FLOOR_PCT)) {
			ctl->stopped_samples++;
			if (ctl->stopped_samples >= BC250_FAN_STOPPED_SAMPLES) {
				/* Full speed first: if the fan only stalled at a low duty, 100 % may start it, and the board's
				 * curve then finds it turning. Then the board, and no further write in this start. */
				(void)drive(io, ctl, BC250_FAN_FULL_PCT);
				ctl->fault = 1;
				(void)bc250_fan_handback(io, ctl, BC250_FAN_REASON_STOPPED);
				ctl->state = BC250_FAN_STATE_FAULT;
				return 0;
			}
		} else {
			ctl->stopped_samples = 0;
		}
	}

	if (ctl->emergency) {
		target = BC250_FAN_FULL_PCT;
		ctl->below_ms = 0;
		ctl->fall_wait_ms = 0;
	} else if (ctl->mode == BC250_FAN_MODE_FIXED) {
		target = ctl->fixed_pct;
		ctl->below_ms = 0;
		ctl->fall_wait_ms = 0;
	} else {
		target = bc250_fan_curve_eval(&ctl->curve, ctl->effective_mc);
		/* Rule 10. The feed-forward only ever raises: the curve's own answer stands wherever it is the higher
		 * one, and every rule above this line (the emergency, a fixed duty, doubt, a fault) keeps its place. */
		if (ctl->boost && target < BC250_FAN_FULL_PCT)
			target = BC250_FAN_FULL_PCT;
	}
	ctl->target_pct = target;
	if (ctl->mode == BC250_FAN_MODE_CURVE && !ctl->emergency)
		target = slope(ctl, target, dt);
	status = drive(io, ctl, target);
	if (status == 0)
		ctl->state = ctl->emergency ? BC250_FAN_STATE_EMERGENCY :
			     ctl->mode == BC250_FAN_MODE_FIXED ? BC250_FAN_STATE_FIXED : BC250_FAN_STATE_CURVE;
	return status;
}

/* ---- names --------------------------------------------------------------------------------------------------- */

const char *bc250_fan_state_name(unsigned int state)
{
	static const char *const names[] = { "off", "board", "curve", "fixed", "emergency", "doubt", "fault" };

	return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

const char *bc250_fan_reason_name(unsigned int reason)
{
	static const char *const names[] = { "none", "user", "stop", "power", "unload", "watchdog", "lease",
					     "temperature", "reader", "handshake", "readback", "mode", "stopped",
					     "disabled", "bugcheck" };

	return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "?";
}

const char *bc250_fan_mode_name(unsigned int mode)
{
	static const char *const names[] = { "board", "curve", "fixed" };

	return mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "?";
}

const char *bc250_fan_profile_name(unsigned int profile)
{
	static const char *const names[] = { "custom", "standard", "quiet", "performance" };

	return profile < sizeof(names) / sizeof(names[0]) ? names[profile] : "?";
}

const char *bc250_fan_boost_name(unsigned int why)
{
	/* Indexed by the three BC250_FAN_BOOST_WHY_* bits, so one word says every signal that called the load heavy. */
	static const char *const names[] = { "none", "busy", "power", "busy+power", "rise", "busy+rise",
					     "power+rise", "busy+power+rise" };

	return why <= BC250_FAN_BOOST_WHY_ALL ? names[why] : "?";
}
