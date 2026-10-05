/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 DPM: a load-driven GFX clock governor between the lab floor (1000 MHz / 820 mV) and at
 * most 2000 MHz, with thermal clamps and a boot guard. The load never goes under the lab floor. Two
 * other rules do, both at the floor's own 820 mV: the thermal clamp, down to 800 MHz (0.7.205, owner
 * decision 2026-10-05), and the idle state, 500 MHz while the GPU has no work (0.7.206, owner decision
 * 2026-10-05).
 * Not amdgpu: amdgpu does no DPM on this part
 * under load (facts M90), the community governors run in user space. docs/design/dpm.md in
 * bc250-win is the design; this header is the part without Windows in it, so the host test
 * (test/dpm_test.c) runs exactly what the miniport runs.
 *
 * Four pure pieces:
 *   bc250_dpm_decide()        the settings and the boot guard: fixed-lab or DPM for this start,
 *                             and what the caller must persist before and after
 *   bc250_dpm_step()          one governor tick: GPU busy share and temperature in, a level out
 *   bc250_dpm_tune_check()    the thresholds and floor an administrator may set at run time (0.7.185)
 *   bc250_dpm_session_step()  the "running above the floor" marker that turns a crash at a high
 *                             clock into a fixed-lab next start
 * The idle state (0.7.206) is part of bc250_dpm_step(); bc250_dpm_idle_config() turns it on.
 * The operating points themselves are bc250_clock.h's table; a level is an index into it.
 */
#ifndef BC250_DPM_H
#define BC250_DPM_H

#include "bc250_clock.h"

#define BC250_DPM_MODE_FIXED	0u	/* the lab point, BC250_DPM_FLOOR_LEVEL, set once at start: today's behaviour */
#define BC250_DPM_MODE_DPM	1u
/* Absent DpmMode means this. Fixed until the lab accepts DPM (docs/design/dpm.md, lab plan). */
#define BC250_DPM_DEFAULT_MODE	BC250_DPM_MODE_FIXED
/* Absent DpmMaxMHz means this (owner, 2026-09-30: start at 1500). DpmMaxMHz may raise it to the table's
 * BC250_CLOCK_CEILING_MHZ, the hard ceiling; nothing goes above that. */
#define BC250_DPM_DEFAULT_MAX_MHZ	1500u

/* The lab floor, 1000 MHz: the level a fixed start runs at, the lowest the load may ask for, and where every
 * rule that needs one clock this part is known to run puts it (a missing sensor, SetStablePowerState, the
 * fixed mode, stop, power down, giving up, an unknown readback). Index 5 since 0.7.206, where the table
 * reaches down to 500 MHz (index 2 in 0.7.205, with 900 and 800 MHz under it). */
#define BC250_DPM_FLOOR_LEVEL	5u
/* 800 MHz: the lowest level the thermal cap may reach (0.7.205, owner decision 2026-10-05, after RotTR scene
 * 2 held 88 C with the governor already at its 1000 MHz floor and the GPU 97 % busy). The firmware has never
 * run below 1000 MHz (facts M47), so the KMD may withdraw it for a start: bc250_dpm_subfloor_refused(). */
#define BC250_DPM_THERMAL_FLOOR_LEVEL	3u
/* 500 MHz: the idle point (0.7.206). Only the idle state goes there, and only while the GPU has no work; the
 * levels between it and the thermal floor (700 and 600 MHz) exist to keep the 100 MHz grid whole and no rule
 * selects them. The KMD may withdraw the point for a start: bc250_dpm_idle_refused(). */
#define BC250_DPM_IDLE_LEVEL	0u
#define BC250_DPM_TOP_LEVEL	(BC250_CLOCK_LEVELS - 1u)

static __inline unsigned int bc250_dpm_level_mhz(unsigned int level)
{
	return bc250_clock_points[level < BC250_CLOCK_LEVELS ? level : BC250_DPM_TOP_LEVEL].mhz;
}
static __inline unsigned int bc250_dpm_level_mv(unsigned int level)
{
	return bc250_clock_points[level < BC250_CLOCK_LEVELS ? level : BC250_DPM_TOP_LEVEL].mv;
}
/* The level of an exact grid clock, or -1 (a clock somebody else set, a failed readback). */
int bc250_dpm_level_of(unsigned int mhz);

/* ---- the settings and the boot guard -------------------------------------------------------- */

/* What the guard did, or why this start is fixed. Shared with the escape and the CLI. */
enum bc250_dpm_reason {
	BC250_DPM_REASON_NONE = 0,
	BC250_DPM_REASON_NOT_REQUESTED = 1,	/* DpmMode is 0, or absent with a fixed default */
	BC250_DPM_REASON_INVALID_SETTING = 2,	/* DpmMode or DpmMaxMHz out of range: fixed, kept */
	BC250_DPM_REASON_UNCONFIRMED = 3,	/* an earlier DPM start was never confirmed */
	BC250_DPM_REASON_UNCLEAN = 4,		/* an earlier start ended while above the floor */
	BC250_DPM_REASON_REGISTRY = 5,		/* the pending mark could not be made durable */
	BC250_DPM_REASON_NO_SMU = 6,		/* no native SMU owner (EnableNativeSmu closed) */
	BC250_DPM_REASON_NOT_RUN = 7,		/* the device start did not reach the governor */
	BC250_DPM_REASON_SMU_ERROR = 8,		/* the governor gave up after repeated SMU failures */
	BC250_DPM_REASON_COUNT
};

/* The encoded request: a tag and the ceiling clock. Pending and Confirmed hold it, so a confirmed
 * 1600 MHz ceiling does not cover a later 2000. */
#define BC250_DPM_ENCODED_TAG	0xD0000000u
static __inline unsigned int bc250_dpm_encode(unsigned int max_mhz)
{
	return BC250_DPM_ENCODED_TAG | (max_mhz & 0xFFFFu);
}

struct bc250_dpm_request {
	int		smu_online;		/* the native SMU owner started */
	int		mode_present;		/* DpmMode exists as a REG_DWORD */
	unsigned int	mode;
	int		max_present;		/* DpmMaxMHz */
	unsigned int	max_mhz;
	int		pending_present;	/* DpmPending */
	unsigned int	pending;
	int		confirmed_present;	/* DpmConfirmed */
	unsigned int	confirmed;
	int		session_present;	/* DpmSession: a start ran above the floor and did not end */
	unsigned int	session;
};

struct bc250_dpm_decision {
	unsigned int	requested;		/* the mode asked for, the default when absent */
	unsigned int	mode;			/* what this start runs */
	unsigned int	max_mhz, max_level;	/* the ceiling, grid-rounded down; the floor when fixed */
	unsigned int	encoded;		/* bc250_dpm_encode(max_mhz) of a DPM request, else 0 */
	unsigned int	reason;
	int		force_fixed;		/* persist: DpmMode = 0, delete Pending, Confirmed, Session */
	int		mark_pending;		/* persist DpmPending = encoded before the first raise */
	int		clear_pending;		/* a stale mark of a request that is no longer made */
	int		clear_session;		/* a stale marker of a fixed start (nothing ran above the floor) */
	int		confirmed;		/* Confirmed == encoded: no pending mark for this start */
};

void bc250_dpm_decide(const struct bc250_dpm_request *r, struct bc250_dpm_decision *d);

/* ---- the governor ---------------------------------------------------------------------------- */

/* What holds the level below what the load asks for. Shared with the escape and the CLI. */
enum bc250_dpm_throttle {
	BC250_DPM_THROTTLE_NONE = 0,
	BC250_DPM_THROTTLE_THERMAL_SOFT = 1,	/* 87 C: stepped down, no raise */
	BC250_DPM_THROTTLE_THERMAL_HARD = 2,	/* 90 C: at the thermal floor (800 MHz since 0.7.205) */
	BC250_DPM_THROTTLE_SENSOR = 3,		/* no temperature reading: at the floor */
	BC250_DPM_THROTTLE_MAX_SETTING = 4,	/* DpmMaxMHz */
	BC250_DPM_THROTTLE_STABLE = 5,		/* D3D12 SetStablePowerState: pinned to the floor */
	BC250_DPM_THROTTLE_SMU = 6,		/* the governor stopped after SMU failures */
	BC250_DPM_THROTTLE_FIXED = 7,		/* this start is fixed-lab */
	BC250_DPM_THROTTLE_THERMAL_WARM = 8,	/* WARM_MC (85 C in 0.7.200, 87 C from 0.7.204): a raise refused */
	BC250_DPM_THROTTLE_THERMAL_RAMP = 9,	/* 70 C to WARM_MC (0.7.203): a raise cut to one level, or held */
	BC250_DPM_THROTTLE_IDLE = 10,		/* the idle state holds the clock at the idle point, or is being left (0.7.206) */
	BC250_DPM_THROTTLE_COUNT
};

/* Thresholds. Busy is in permille of the tick's wall time. These are the defaults every start begins
 * with; an administrator may change them at run time (struct bc250_dpm_tune below). */
#define BC250_DPM_UP_PERMILLE		900u	/* at or above: raise now */
#define BC250_DPM_TARGET_PERMILLE	800u	/* a raise aims at this share at the new clock */
#define BC250_DPM_DOWN_PERMILLE		650u	/* the average below this for DOWN_HOLD_MS: one step down */
#define BC250_DPM_DOWN_HOLD_MS		200u
#define BC250_DPM_HOT_MC		BC250_CLOCK_HOT_MC	/* 87 C: one step down, no raise */
/* The warm zone (0.7.200, owner after session 344: 1500 MHz held while Tctl rose 83.5 -> 85.3 C): from here up no
 * raise of clock or voltage; the level holds, a lowering still happens. 85 C (HOT_MC - 2 C) in 0.7.200-203; at HOT_MC
 * from 0.7.204 (owner, 2026-10-04: "próg na 87", the threshold at 87). The rule stays as a backstop at and above
 * HOT_MC, where the hot cap already holds the clock at or below the running level, and the clock gate refuses a
 * raise. Never above HOT_MC: the hot cap's step down and this rule's hold then cover the same readings. */
#define BC250_DPM_WARM_MC		BC250_DPM_HOT_MC
/* The thermal ramp (0.7.203, session 367): from RAMP_KNEE_MC up to WARM_MC a raise goes one level at most, and only
 * when the last raise is at least the ramp interval ago. The interval grows linearly from RAMP_MIN_MS at the knee to
 * RAMP_MAX_MS at WARM_MC (bc250_dpm_ramp_interval_ms). In session 367 the load raised 1000 -> 2000 MHz within one
 * telemetry interval at 75.7 C; at 2000 MHz / 1000 mV the hot spot gained some 12 C in 5 s and read 87.1 C, and the
 * 85 C warm zone of that KMD could not act, because it looks only at the reading of the tick and the raise was already
 * done. One level (100 MHz, at most 20 mV) per interval lets each raise show in the reading before the next one: the
 * hot spot's fast time constant is about 3 s (session 367: some 12 C of a 15 C rise in 5 s), and from about 81 C up
 * the interval is longer than that (0.7.204: the interval runs to WARM_MC, now 87 C, so it is 3.5 s at 85 C). Below the knee the load raises as before: the knee plus the fast rise of a jump to 2000 MHz (about
 * 15 C) stays under 87 C. test_plant367 in dpm_test.c holds the evidence: on its plant the old rule puts the hot spot
 * past 87 C for every start from 74 C, the ramp for none up to 83 C, and both settle at the same clock from a cold
 * start. Fixed values, not in struct bc250_dpm_tune: the RUN_DPM_TUNE escape (ABI 2, 152 bytes) has no field for them,
 * and a new field is an ABI change. */
#define BC250_DPM_RAMP_KNEE_MC		70000
#define BC250_DPM_RAMP_MIN_MS		1000u
#define BC250_DPM_RAMP_MAX_MS		4000u
#define BC250_DPM_HOT_STEP_MS		500u	/* the default hot step: at most one step down per this */
/* 90 C: the thermal floor at once (800 MHz since 0.7.205; the lab floor when a sub-floor transition was
 * refused, bc250_dpm_subfloor_refused). A missing reading clamps to the lab floor and never raises the cap. */
#define BC250_DPM_CRITICAL_MC		90000
#define BC250_DPM_RELEASE_MC		82000	/* below: the thermal cap rises again (HOT_MC - 5 C) */
#define BC250_DPM_RELEASE_STEP_MS	1000u	/* one level per this, while below RELEASE_MC */
/* The soft release (BD-055), off by default: below HOT_MC - delta for a whole step, the cap rises one level. Without
 * it the cap holds anywhere between RELEASE_MC and HOT_MC, so under a sustained load one excursion past 87 C costs
 * levels for the rest of the load (sessions 318, 320, 321: 2000 -> 1500..1600 MHz, frozen at 85.6-86.2 C). */
#define BC250_DPM_SOFT_DELTA_MC		0u	/* 0: no soft release */
#define BC250_DPM_SOFT_STEP_MS		3000u
/* The idle state (0.7.206, owner decision 2026-10-05: "jak lab nie pracuje, to ustawiaj mu zegar gpu na
 * 500 MHz" - when the lab does not work, set its GPU clock to 500 MHz). While the GPU has no work the
 * governor holds the idle point, below the lab floor, at the floor's own 820 mV. The three settings are the
 * KMD's registry values DpmIdleMHz, DpmIdleHoldMs and DpmIdleBusyPermille (driver/kmd/dpm.c), checked and
 * taken by bc250_dpm_idle_config(); they are not part of struct bc250_dpm_tune, so the RUN_DPM_TUNE escape
 * and its ABI do not change. DpmIdleMHz 0 turns the whole state off, which is exactly 0.7.205 behaviour. */
#define BC250_DPM_IDLE_MHZ		500u	/* absent DpmIdleMHz: the idle point, 0 for no idle state */
/* The GPU must have had no work for this long before the clock goes to the idle point. The desktop on the
 * GPU (DWM) gives short bursts, so the rule is the busy share over the whole window, not a strict zero. */
#define BC250_DPM_IDLE_HOLD_MS		3000u
#define BC250_DPM_IDLE_BUSY_PERMILLE	2u	/* the window's mean busy share must stay under this */
#define BC250_DPM_IDLE_MIN_HOLD_MS	250u	/* ten governor ticks */
#define BC250_DPM_IDLE_MAX_HOLD_MS	60000u
#define BC250_DPM_IDLE_MAX_BUSY_PERMILLE 100u	/* 10 %: anything higher is not an idle GPU */
/* The share at which one tick of its own leaves the state. It is not the entry threshold on purpose. Entry
 * admits a mean of 2 permille because a static desktop wakes for single frames, and one such frame is
 * 1000 / BC250_DPM_TICK_MS = 40 permille of its own tick (one active GRBM sample of the 25 a 25 ms tick holds),
 * twenty times that mean. Comparing a tick's own share with the entry threshold therefore left the state at the
 * very frame the mean rule was written to tolerate: on the policy itself, one wake per second gave 15 entries and
 * 15 exits a minute and held the point for a quarter of the time. A tick at or above half its own wall time is
 * work no desktop frame reaches, and work on either ring leaves the state in that tick anyway (ring_busy), so
 * this threshold only has to catch heavy work that the ring accounting cannot see. Anything under it that lasts
 * leaves through the window rule: a whole window whose mean is above the admitted share. */
#define BC250_DPM_IDLE_EXIT_PERMILLE	500u
#define BC250_DPM_MAX_DT_MS		1000u	/* a longer tick (a stall, a resume) counts as this */
#define BC250_DPM_CAP_MS_MAX		0x7FFFFFFFu	/* where the time since the last cap change saturates */

/* The busy signal (docs/design/dpm.md, "Busy"). The miniport samples GRBM_STATUS.GUI_ACTIVE every
 * HW_SAMPLE_US and counts the samples and the active ones per tick: the graphics engine's own
 * activity, whichever path fed it. KMD 0.7.175 used the GFX ring's submit-to-fence time instead;
 * that stays as the fallback for a tick with too few samples (a sampler that could not start, a
 * power transition). */
#define BC250_DPM_HW_SAMPLE_US		1000u
#define BC250_DPM_HW_MIN_SAMPLES	8u
enum bc250_dpm_busy_source { BC250_DPM_BUSY_SUBMIT = 0, BC250_DPM_BUSY_GRBM = 1 };
/* The tick's busy share in permille: active/samples rounded, or submit_permille (clamped to 1000)
 * when samples < HW_MIN_SAMPLES. An active count above samples (a sample that landed between the
 * caller's two reads) counts as samples. */
unsigned int bc250_dpm_busy_permille(unsigned int samples, unsigned int active, unsigned int submit_permille,
				     enum bc250_dpm_busy_source *source);
/* The least time between two raises at a temperature (the thermal ramp, 0.7.203): 0 below RAMP_KNEE_MC, RAMP_MIN_MS at
 * the knee, linear up to RAMP_MAX_MS at WARM_MC (87 C from 0.7.204) and above, where no raise happens anyway. */
unsigned int bc250_dpm_ramp_interval_ms(int temperature_mc);

/* ---- runtime tuning (0.7.185) --------------------------------------------------------------- */

/* The four thresholds above and a runtime floor, set by an administrator through the driver's escape
 * (BC250_ESCAPE_RUN_DPM_TUNE) while a DPM start runs, for A/B experiments. Never persisted: every start
 * begins with bc250_dpm_tune_default(). The floor is a level of the clock table the governor does not go
 * below on its own; it never beats the thermal cap, the critical rule, a missing sensor or
 * SetStablePowerState, and it never exceeds the start's ceiling (max_level, DpmMaxMHz). It never goes under
 * BC250_DPM_FLOOR_LEVEL either (0.7.205): the points below it belong to the thermal cap and the idle state
 * alone, and a runtime floor there would say nothing, because the load never asks for them. A runtime floor
 * also turns the idle state off while it is set (0.7.206): the operator asked for a clock, not for 500 MHz. */
struct bc250_dpm_tune {
	unsigned int	up_permille;
	unsigned int	target_permille;
	unsigned int	down_permille;
	unsigned int	down_hold_ms;
	unsigned int	floor_level;		/* BC250_DPM_FLOOR_LEVEL: no runtime floor; never below it */
	/* The thermal cap's timing (0.7.197, BD-055). Not the limits: HOT, RELEASE and CRITICAL stay fixed. */
	unsigned int	hot_step_ms;		/* at most one thermal step down per this */
	unsigned int	soft_delta_mc;		/* soft release below HOT_MC - this; 0: none */
	unsigned int	soft_step_ms;		/* held below that for this: the cap rises one level */
};
#define BC250_DPM_TUNE_MIN_PERMILLE	100u
#define BC250_DPM_TUNE_MAX_PERMILLE	1000u
#define BC250_DPM_TUNE_MIN_HOLD_MS	100u	/* at most one lowering per four ticks of the 25 ms governor */
#define BC250_DPM_TUNE_MAX_HOLD_MS	5000u
#define BC250_DPM_TUNE_MIN_HOT_STEP_MS	250u	/* ten governor ticks: the part's thermal response is slower still */
#define BC250_DPM_TUNE_MAX_HOT_STEP_MS	10000u
/* The soft threshold lies strictly between RELEASE_MC and HOT_MC, at least half a degree from each. */
#define BC250_DPM_TUNE_MIN_SOFT_DELTA_MC 500u
#define BC250_DPM_TUNE_MAX_SOFT_DELTA_MC 4500u
#define BC250_DPM_TUNE_MIN_SOFT_STEP_MS	2000u	/* a soft raise never follows a thermal change by less than this */
#define BC250_DPM_TUNE_MAX_SOFT_STEP_MS	30000u
/* Why a tune was refused. Shared with the escape and the CLI. */
enum bc250_dpm_tune_error {
	BC250_DPM_TUNE_OK = 0,
	BC250_DPM_TUNE_RANGE = 1,		/* a threshold outside MIN..MAX_PERMILLE */
	BC250_DPM_TUNE_ORDER = 2,		/* not down < target < up */
	BC250_DPM_TUNE_LOWERING = 3,		/* invariant 1: a one-step lowering could land at or above up */
	BC250_DPM_TUNE_RAISE = 4,		/* invariant 2: a raise could land below down */
	BC250_DPM_TUNE_HOLD = 5,		/* down_hold_ms outside MIN..MAX_HOLD_MS */
	BC250_DPM_TUNE_FLOOR = 6,		/* the floor is not a table level from the lab floor to the start's ceiling */
	BC250_DPM_TUNE_THERMAL = 7,		/* hot step, soft delta or soft step outside its range */
	BC250_DPM_TUNE_COUNT
};
void bc250_dpm_tune_default(struct bc250_dpm_tune *t);
/* Ranges, order, then two invariants over the clock table, both checked exactly (every level, every busy
 * share the governor can see), not by a rule of thumb about the table's shape:
 *
 * 1. A one-step lowering never lands at or above up. For every level L in 1..TOP:
 *	(down + 1) x mhz(L) <= up x mhz(L - 1)
 *    A lowering from L happens once the average stays below down. With the average settled on the load (it
 *    settles at most one permille under the tick's busy share, and that share is truncated to a whole
 *    permille) the load at L is below down + 1 permille, so at L - 1 the same work is below
 *    (down + 1) x mhz(L) / mhz(L - 1) <= up permille: no raise answers the step down. mhz(L) / mhz(L - 1) is
 *    largest at the bottom of this table (1100 / 1000), where the inequality reads 11 x (down + 1) <= 10 x up;
 *    the defaults: 11 x 651 = 7161 <= 9000.
 * 2. A raise never lands below down. For every level L in 0..TOP-1 and every busy share b in up..1000, with
 *    N the level bc250_dpm_step raises to (the lowest whose clock covers mhz(L) x b / target, at least L + 1):
 *	down x mhz(N) <= b x mhz(L)
 *    The same work then fills at least down permille at N, the average comes down to it from above up and
 *    never crosses down, so no lowering follows a raise. Together with the order (target < up) a constant
 *    load settles after at most one raise. Invariant 1 alone does not give that: the average lags a step
 *    down by several ticks, so a short hold can take a second step during that lag, and a raise landing
 *    below down then starts the cycle again (the host test sweeps both). The defaults: the lowest landing
 *    is 739.2 permille (961 at 1000 MHz raised to 1300 MHz), above 650.
 *
 * The hold's lower bound is about SMU traffic (each lowering is a transaction), not stability: with both
 * invariants the host test finds no oscillation down to a one-tick hold.
 *
 * The thermal timing last: hot step MIN..MAX_HOT_STEP_MS, soft delta 0 or MIN..MAX_SOFT_DELTA_MC, soft step
 * MIN..MAX_SOFT_STEP_MS (checked even while the soft release is off, so that turning it on later is one field). */
enum bc250_dpm_tune_error bc250_dpm_tune_check(const struct bc250_dpm_tune *t, unsigned int max_level);

struct bc250_dpm_input {
	unsigned int	busy_permille;		/* 0..1000 over this tick */
	int		temperature_mc;
	int		temperature_valid;
	unsigned int	dt_ms;			/* since the previous tick */
	/* Work outstanding on the GFX ring at this tick (the KMD's GfxSubmitBusy): submitted and not yet
	 * retired, whatever the hardware's busy samples say. The idle state alone reads it (0.7.206): a
	 * submission waiting on a fence keeps the clock at the lab floor. Zero is "the ring is empty". */
	int		ring_busy;
	/* The paging node's busy share over this tick, from the same hardware samples as busy_permille
	 * (SDMA0_STATUS_REG.IDLE), 0 for a tick with too few samples. The idle state alone reads it (0.7.206):
	 * busy_permille is GRBM GUI_ACTIVE and ring_busy is the GFX ring, so both are blind to a transfer on the
	 * paging queue, and without this field an eviction or an upload with the GFX ring empty would run at the
	 * idle point and nothing would end the state. The load governor keeps its GFX-only accounting. */
	unsigned int	sdma_permille;
};

struct bc250_dpm_governor {
	unsigned int	max_level;		/* the setting's ceiling */
	unsigned int	level;			/* the level the hardware is at, as the caller committed it */
	unsigned int	thermal_cap;		/* the thermal clamp, max_level when released */
	unsigned int	avg_permille;		/* busy, exponential average, 1/4 per tick */
	unsigned int	down_ms, hot_ms, release_ms;
	unsigned int	soft_ms;		/* held below the soft threshold */
	unsigned int	cap_ms;			/* since the thermal cap last changed, saturating */
	unsigned int	soft_releases;		/* cap raises by the soft release */
	int		hot;			/* inside a >= 87 C episode */
	int		stable;			/* SetStablePowerState(TRUE) */
	unsigned int	throttle;		/* enum bc250_dpm_throttle of the last step */
	unsigned int	want;			/* what the load asked for in the last step */
	unsigned int	raises, lowers, thermal_events;
	struct bc250_dpm_tune tune;		/* the thresholds and floor in force; bc250_dpm_set_tune changes them */
	unsigned int	floor_ticks;		/* steps in which the runtime floor lifted the request above want */
	unsigned int	warm_holds;		/* steps in which the warm zone refused a raise (0.7.200) */
	unsigned int	raise_ms;		/* since the last step that returned a raise, saturating (0.7.203) */
	unsigned int	ramp_holds;		/* steps in which the thermal ramp cut or held a raise (0.7.203) */
	int		subfloor_ok;		/* the thermal cap may use the points under the lab floor (0.7.205) */
	unsigned int	subfloor_refusals;	/* bc250_dpm_subfloor_refused() calls of this start */
	/* The idle state (0.7.206). idle_on is the setting of this start, idle the state now. */
	int		idle_on, idle;
	unsigned int	idle_level;		/* the point idle holds; raised to the thermal floor after a refusal */
	unsigned int	idle_hold_ms;		/* the window the GPU must be quiet for */
	unsigned int	idle_busy_permille;	/* the window's admitted mean busy share, and the exit threshold */
	unsigned int	idle_ms;		/* the window so far: the candidate one before entry, the trailing one in idle */
	unsigned int	idle_acc;		/* busy permille x ms over that window, saturating */
	unsigned int	idle_entries, idle_exits, idle_refusals;
	unsigned int	idle_total_ms;		/* time held at the idle point, saturating */
};

void bc250_dpm_init(struct bc250_dpm_governor *g, unsigned int max_level);
/* A checked tune replaces g->tune (the next step uses it); a refused one leaves it as it was. */
enum bc250_dpm_tune_error bc250_dpm_set_tune(struct bc250_dpm_governor *g, const struct bc250_dpm_tune *t);
/* One tick. Returns the level to apply; the caller applies it and reports with bc250_dpm_commit()
 * (success) or leaves g->level as it was (failure). Never returns above min(max_level, thermal_cap). */
unsigned int bc250_dpm_step(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in);
void bc250_dpm_commit(struct bc250_dpm_governor *g, unsigned int level);
/* The caller could not put the hardware at a level below BC250_DPM_FLOOR_LEVEL (the SMU refused it, the
 * readback did not match). From here on, for the rest of this start, the thermal cap's lowest level is the
 * lab floor again, and a cap already below it is raised to it. The firmware has never been seen below
 * 1000 MHz (facts M47), so one refusal is enough: the governor does not try the same point again and again. */
void bc250_dpm_subfloor_refused(struct bc250_dpm_governor *g);

/* ---- the idle state (0.7.206) ----------------------------------------------------------------- */

/* Why an idle setting was refused; the KMD logs it and runs without the idle state. */
enum bc250_dpm_idle_error {
	BC250_DPM_IDLE_OK = 0,
	BC250_DPM_IDLE_CLOCK = 1,	/* DpmIdleMHz is not a table clock below the lab floor */
	BC250_DPM_IDLE_HOLD = 2,	/* DpmIdleHoldMs outside MIN..MAX_HOLD_MS */
	BC250_DPM_IDLE_BUSY = 3,	/* DpmIdleBusyPermille above MAX_BUSY_PERMILLE */
	BC250_DPM_IDLE_ERROR_COUNT
};
/* The start's idle setting, after bc250_dpm_init (which leaves the state off, so a caller that does not
 * configure it keeps 0.7.205 behaviour exactly). idle_mhz 0 turns it off and is not an error; any other
 * value must be a clock of the table below BC250_CLOCK_FLOOR_MHZ. A refused setting leaves the state off.
 * Not a run-time operation: the KMD calls it once per start, before the governor thread exists. */
enum bc250_dpm_idle_error bc250_dpm_idle_config(struct bc250_dpm_governor *g, unsigned int idle_mhz,
						unsigned int hold_ms, unsigned int busy_permille);
/* The caller could not put the hardware at the idle point (the SMU refused it, the readback did not match).
 * The idle point falls back one step at a time, as the owner asked: 500 MHz, then the thermal floor
 * (800 MHz), then off, which is the lab floor. One refusal is enough for each step: the firmware has never
 * been seen below 1000 MHz (facts M47), so the governor does not try the same point every tick. A refusal of a
 * point at or above the thermal floor is the thermal cap's answer as well - the same clock, the same two
 * messages - so it calls bc250_dpm_subfloor_refused() and nothing under the lab floor is asked for again. */
void bc250_dpm_idle_refused(struct bc250_dpm_governor *g);
/* The idle point in force, 0 when the idle state is off for this start (the escape and the log print it). */
unsigned int bc250_dpm_idle_mhz(const struct bc250_dpm_governor *g);
/* The caller took the hardware off the idle point outside a governing tick (a stop, a power transition). It
 * counts as an exit, and the next entry needs a whole quiet window again. The setting itself stays. */
void bc250_dpm_idle_leave(struct bc250_dpm_governor *g);

/* ---- the session marker ---------------------------------------------------------------------- */

/* DpmSession is written (and flushed) when the governor first goes above the floor (a thermal-only level
 * under it counts as "not above the floor": it is no risk to carry into the next start), and deleted after
 * SESSION_CLEAR_MS at the floor or at a clean stop. A start that finds it knows the previous one
 * ended above the floor without a stop: a bugcheck, a hang, a power cut at a high clock. */
#define BC250_DPM_SESSION_CLEAR_MS	10000u
enum bc250_dpm_session_action { BC250_DPM_SESSION_NONE = 0, BC250_DPM_SESSION_SET = 1, BC250_DPM_SESSION_CLEAR = 2 };
struct bc250_dpm_session {
	int		marked;			/* the caller sets it after a durable write, clears after a delete */
	unsigned int	floor_ms;
};
/* Call with the level about to be applied (SET must be durable before it) and after each tick. */
enum bc250_dpm_session_action bc250_dpm_session_step(struct bc250_dpm_session *s, unsigned int level,
						     unsigned int dt_ms);

#endif
