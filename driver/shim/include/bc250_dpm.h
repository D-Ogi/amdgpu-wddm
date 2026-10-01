/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 DPM: a load-driven GFX clock governor between the lab floor (1000 MHz / 820 mV) and at
 * most 2000 MHz, with thermal clamps and a boot guard. Not amdgpu: amdgpu does no DPM on this part
 * under load (facts M90), the community governors run in user space. docs/design/dpm.md in
 * bc250-win is the design; this header is the part without Windows in it, so the host test
 * (test/dpm_test.c) runs exactly what the miniport runs.
 *
 * Three pure pieces:
 *   bc250_dpm_decide()        the settings and the boot guard: fixed-lab or DPM for this start,
 *                             and what the caller must persist before and after
 *   bc250_dpm_step()          one governor tick: GPU busy share and temperature in, a level out
 *   bc250_dpm_session_step()  the "running above the floor" marker that turns a crash at a high
 *                             clock into a fixed-lab next start
 * The operating points themselves are bc250_clock.h's table; a level is an index into it.
 */
#ifndef BC250_DPM_H
#define BC250_DPM_H

#include "bc250_clock.h"

#define BC250_DPM_MODE_FIXED	0u	/* the lab point, level 0, set once at start: today's behaviour */
#define BC250_DPM_MODE_DPM	1u
/* Absent DpmMode means this. Fixed until the lab accepts DPM (docs/design/dpm.md, lab plan). */
#define BC250_DPM_DEFAULT_MODE	BC250_DPM_MODE_FIXED
/* Absent DpmMaxMHz means this (owner, 2026-09-30: start at 1500). DpmMaxMHz may raise it to the table's
 * BC250_CLOCK_CEILING_MHZ, the hard ceiling; nothing goes above that. */
#define BC250_DPM_DEFAULT_MAX_MHZ	1500u

#define BC250_DPM_FLOOR_LEVEL	0u
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
	BC250_DPM_THROTTLE_THERMAL_HARD = 2,	/* 90 C: at the floor */
	BC250_DPM_THROTTLE_SENSOR = 3,		/* no temperature reading: at the floor */
	BC250_DPM_THROTTLE_MAX_SETTING = 4,	/* DpmMaxMHz */
	BC250_DPM_THROTTLE_STABLE = 5,		/* D3D12 SetStablePowerState: pinned to the floor */
	BC250_DPM_THROTTLE_SMU = 6,		/* the governor stopped after SMU failures */
	BC250_DPM_THROTTLE_FIXED = 7,		/* this start is fixed-lab */
	BC250_DPM_THROTTLE_COUNT
};

/* Thresholds. Busy is in permille of the tick's wall time. */
#define BC250_DPM_UP_PERMILLE		900u	/* at or above: raise now */
#define BC250_DPM_TARGET_PERMILLE	800u	/* a raise aims at this share at the new clock */
#define BC250_DPM_DOWN_PERMILLE		650u	/* the average below this for DOWN_HOLD_MS: one step down */
#define BC250_DPM_DOWN_HOLD_MS		200u
#define BC250_DPM_HOT_MC		BC250_CLOCK_HOT_MC	/* 87 C: one step down at once, no raise */
#define BC250_DPM_HOT_STEP_MS		500u	/* still hot after this: another step down */
#define BC250_DPM_CRITICAL_MC		90000	/* the floor at once */
#define BC250_DPM_RELEASE_MC		82000	/* below: the thermal cap rises again (HOT_MC - 5 C) */
#define BC250_DPM_RELEASE_STEP_MS	1000u	/* one level per this, while below RELEASE_MC */
#define BC250_DPM_MAX_DT_MS		1000u	/* a longer tick (a stall, a resume) counts as this */

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

struct bc250_dpm_input {
	unsigned int	busy_permille;		/* 0..1000 over this tick */
	int		temperature_mc;
	int		temperature_valid;
	unsigned int	dt_ms;			/* since the previous tick */
};

struct bc250_dpm_governor {
	unsigned int	max_level;		/* the setting's ceiling */
	unsigned int	level;			/* the level the hardware is at, as the caller committed it */
	unsigned int	thermal_cap;		/* the thermal clamp, max_level when released */
	unsigned int	avg_permille;		/* busy, exponential average, 1/4 per tick */
	unsigned int	down_ms, hot_ms, release_ms;
	int		hot;			/* inside a >= 87 C episode */
	int		stable;			/* SetStablePowerState(TRUE) */
	unsigned int	throttle;		/* enum bc250_dpm_throttle of the last step */
	unsigned int	want;			/* what the load asked for in the last step */
	unsigned int	raises, lowers, thermal_events;
};

void bc250_dpm_init(struct bc250_dpm_governor *g, unsigned int max_level);
/* One tick. Returns the level to apply; the caller applies it and reports with bc250_dpm_commit()
 * (success) or leaves g->level as it was (failure). Never returns above min(max_level, thermal_cap). */
unsigned int bc250_dpm_step(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in);
void bc250_dpm_commit(struct bc250_dpm_governor *g, unsigned int level);

/* ---- the session marker ---------------------------------------------------------------------- */

/* DpmSession is written (and flushed) when the governor first leaves the floor, and deleted after
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
