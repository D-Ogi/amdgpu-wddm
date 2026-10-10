#include "bc250_board_envelope.h"
/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 DPM: a load-driven GFX clock governor between the lab floor (1000 MHz / 820 mV) and at
 * most 2000 MHz, with thermal clamps and a boot guard. The load never goes under the lab floor. Two
 * other rules do, both at the floor's own 820 mV: the thermal clamp, down to 800 MHz (0.7.205, owner
 * decision 2026-10-05), and the idle state, 500 MHz while the GPU has no work (0.7.207, owner decision
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
 * The idle state (0.7.207) is part of bc250_dpm_step(); bc250_dpm_idle_config() turns it on. So is the soft thermal
 * zone (0.7.213, BD-087): the cap starts stepping down at 86 C instead of 87 C, judged on the reading plus its own
 * slope extrapolated 15 s, and the 87 C hot cap stays behind it as the backstop on the raw reading.
 * The operating points themselves are bc250_clock.h's table; a level is an index into it.
 */
#ifndef BC250_DPM_H
#define BC250_DPM_H

#include "bc250_clock.h"

/* Absent DpmMode means this. Fixed until the lab accepts DPM (docs/design/dpm.md, lab plan). */
/* Absent DpmMaxMHz means this (owner, 2026-09-30: start at 1500). DpmMaxMHz may raise it to the table's
 * BC250_CLOCK_CEILING_MHZ, the hard ceiling; nothing goes above that. */

/* The lab floor, 1000 MHz: the level a fixed start runs at, the lowest the load may ask for, and where every
 * rule that needs one clock this part is known to run puts it (a missing sensor, SetStablePowerState, the
 * fixed mode, stop, power down, giving up, an unknown readback). Index 5 since 0.7.207, where the table
 * reaches down to 500 MHz (index 2 in 0.7.205, with 900 and 800 MHz under it). */
/* 800 MHz: the lowest level the thermal cap may reach (0.7.205, owner decision 2026-10-05, after RotTR scene
 * 2 held 88 C with the governor already at its 1000 MHz floor and the GPU 97 % busy). The tables publish no
 * level below 1000 MHz (facts M47) although unit A's firmware accepts 800 and 900 MHz (facts M785), so the
 * KMD may still withdraw it for a start: bc250_dpm_subfloor_refused(). */
/* 500 MHz: the idle point (0.7.207). Only the idle state goes there, and only while the GPU has no work; the
 * levels between it and the thermal floor (700 and 600 MHz) exist to keep the 100 MHz grid whole and no rule
 * selects them. The KMD may withdraw the point for a start: bc250_dpm_idle_refused(). */

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
	int		closed_present;		/* DpmClosedReason: the driver itself wrote the fixed mode (0.7.208) */
	unsigned int	closed;
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
	/* The durable record of a fallback (0.7.208), the way interop_policy.c keeps InteropClosedReason.
	 * force_fixed writes closed_reason into DpmClosedReason, so the next boot still knows that the driver
	 * wrote DpmMode 0 and the release installer does not read the 0 as a setting of the tester (BD-069).
	 * DpmLastReason cannot carry that: every start overwrites it, and a start that reads DpmMode 0 writes
	 * NOT_REQUESTED over the fallback. */
	unsigned int	closed_reason;		/* the record after this start: force_fixed writes this value into
						 * DpmClosedReason (driver/kmd/dpm.c PersistFallback), every other
						 * start reports here what the key keeps. 0 for no record */
	int		clear_closed;		/* delete DpmClosedReason: DpmMode is not the fixed mode any more */
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
	BC250_DPM_THROTTLE_IDLE = 10,		/* the idle state holds the clock at the idle point, or is being left (0.7.207) */
	BC250_DPM_THROTTLE_THERMAL_ZONE = 11,	/* the soft zone (0.7.213): the cap stepped down below HOT_MC */
	BC250_DPM_THROTTLE_COUNT
};

/* Thresholds. Busy is in permille of the tick's wall time. These are the defaults every start begins
 * with; an administrator may change them at run time (struct bc250_dpm_tune below). */
/* The warm zone (0.7.200, owner after session 344: 1500 MHz held while Tctl rose 83.5 -> 85.3 C): from here up no
 * raise of clock or voltage; the level holds, a lowering still happens. 85 C (HOT_MC - 2 C) in 0.7.200-203; at HOT_MC
 * from 0.7.204 (owner, 2026-10-04: "próg na 87", the threshold at 87). The rule stays as a backstop at and above
 * HOT_MC, where the hot cap already holds the clock at or below the running level, and the clock gate refuses a
 * raise. Never above HOT_MC: the hot cap's step down and this rule's hold then cover the same readings.
 * Since 0.7.213 this constant is only what the rule falls back to with the soft zone off: with the zone on the
 * threshold in force is the soft-release threshold, HOT_MC - soft_delta_mc (83.0 C by default), and it is read with the
 * zone's lead. bc250_dpm_warm_mc() is the one place that decides. The two belong together: the cap's up side and the
 * clock's up side must not disagree, or a released cap would be followed by a clock the zone is about to take back.
 * The thermal ramp's table still runs to BC250_DPM_WARM_MC (bc250_dpm_ramp_interval_ms), on the raw reading, because
 * it spaces raises below the knee as well and its shape was measured against 87 C (session 367). */
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
/* 90 C: the thermal floor at once (800 MHz since 0.7.205; the lab floor when a sub-floor transition was
 * refused, bc250_dpm_subfloor_refused). A missing reading clamps to the lab floor and never raises the cap. */
/* The soft release (BD-055): below HOT_MC - delta for a whole step, the cap rises one level. Without it the cap holds
 * anywhere between RELEASE_MC and HOT_MC, so under a sustained load one excursion past 87 C costs levels for the rest
 * of the load (sessions 318, 320, 321: 2000 -> 1500..1600 MHz, frozen at 85.6-86.2 C). It was off by default until
 * 0.7.213, where it became the up side of the soft zone below: 4.0 C under HOT_MC, which is 83.0 C, held for 4 s.
 * The soft zone's own search picked both numbers (scratch/thermal-zone/sim, best.txt "What the recommendation needs"),
 * and this threshold is also where the warm zone now refuses a raise (bc250_dpm_warm_mc). */
/* The soft thermal zone (0.7.213, BD-087, GRAPH.md C56, after session 436). The hot cap first acts at HOT_MC, which is
 * also where the lab runner ends a game (87 C held 10 s, or 89 C at once), and the die's time constant is about 25 s:
 * in 436 the benchmark read 80.2 C at 1500 MHz, 84.0 C at 1200 MHz 32 s later and 88.8 C at 800 MHz 31 s after that,
 * and the runner stopped the game while the governor was still stepping down. A controller that first acts at 87 C
 * cannot hold 87 C, so the cap now starts stepping at ZONE_MC and the clock stops rising at the soft-release
 * threshold, both of them read with a lead.
 *
 * The numbers are the result of an exhaustive search over 6393 threshold/timer/trigger variants, replayed on 21
 * recorded game sessions with each session's own measured disturbance over a fitted die model
 * (P:\BC-250\scratch\thermal-zone, sim/best.txt; the model reproduces each session's Tctl to 0.13-0.23 C rms). The
 * recommendation is the highest-clock variant that never reaches 87 C on any trace AND keeps the runner clear under
 * every perturbation a fixed 800 MHz itself survives: peak 86.7 C, 0 s at or above 87 C, mean 1073 MHz against the
 * shipped governor's 1131 MHz, which peaks 89.0 C and loses four of the traces. On 436 it peaks 85.9 C instead of
 * 88.9 C and does not stop.
 *
 * Those are the numbers of a search whose plant has no sensor noise in it, and the lead they were scored with had no
 * cap on it. The safety review of this change measured what the recorded noise does to such a lead (LEAD_MAX_MC below)
 * and the thresholds were scored again with it, by replaying the same 21 sessions through this code over the same plant
 * with each session's own recorded noise added to the die (scratch/thermal-zone/review-dpm, validation.txt). What ships
 * is that second set of numbers: peak 87.3 C, 9.4 s at or above 87 C in some two hours of play, no runner stop on any
 * trace, mean 1048 MHz, and 436 at 85.4 C with no reading at 87 C. The figures of the paragraph above stand as what the
 * search found on a noise-free plant, not as a claim about the driver.
 *
 * ZONE_MC is HOT_MC - ZONE_DELTA_MC = 86.0 C. It is not "87 C minus a margin": the overshoot the controller allows
 * grows with the clock it is allowed to reach, which is why a cooler die wants a LOWER threshold, not a higher one. */
/* One level off the cap per this, while in the zone. The noise-free search picked 3000 ms, with a lead that had no cap
 * on it; the safety review's cap (BC250_DPM_ZONE_LEAD_MAX_MC) means the zone acts at a raw 84.0 C instead of the 78 to
 * 83 C that the noise itself was producing, so it has less warning and has to step faster. 1500 ms is where both
 * validations come out clean: on the fitted plant every arm of test_zone436 stays under 87 C with no runner stop, and
 * over the 21 recorded sessions replayed closed-loop with their own sensor noise the die peaks at 87.3 C with 9.4 s at
 * or above 87 C in some two hours of play and no stop, against 89.3 C, 149.3 s and five stops for the 0.7.212 rules, for
 * a mean clock of 1048 MHz against 1099 (scratch/thermal-zone/review-dpm). */
/* The lead. Every threshold of the zone and of the soft release is judged on Tctl plus the slope of the last
 * ZONE_SLOPE_MS seconds times this, when that is higher than Tctl itself. It is the whole gain of the search: at
 * identical thresholds, turning it off gives peak 87.58 C and 47.5 s at or above 87 C over the 21 traces, and turning
 * it on gives 86.71 C and no second at all, for 2.7 % of the mean clock. A plain threshold, however low, cannot do
 * this - it is the 25 s lag that beats it. In the driver it is a least-squares fit over a short ring of past readings
 * (bc250_dpm_governor::slope_mc), a deadband, and a cap: SLOPE_MIN_MC and LEAD_MAX_MC below say why each of the three
 * is there. The fit's span is whatever the ring really covers, never a figure assumed from the slot spacing, because a
 * skipped slot widens it. HOT_MC and CRITICAL_MC keep reading the raw sensor: the backstop must never be a guess. */
/* The ring the slope is measured over: one reading per slot, a slot every SLOPE_SLOT_MS. With 20 slots a full ring
 * spans 4.75 to 5.0 s, which is SLOPE_MS within one slot, and the fit below divides by the span it measures, so the
 * estimate is exact whatever the governor's real tick period turned out to be. 20 slots is 160 bytes of the governor;
 * a per-tick ring at BC250_DPM_TICK_MS would be ten times that for no better slope. A tick that brings no reading
 * skips its slot and the span grows, so the span is checked against SLOPE_MAX_SPAN_MS instead of assumed. */
/* The widest ring the lead is taken from. A reading that fails now and then skips its slot instead of emptying the
 * ring (0.7.213 review), so the span may exceed SLOPE_MS; past this the readings are too old to extrapolate from and
 * the lead reports unavailable. Twice the window: a slope over 10 s still says something about a die whose time
 * constant is 25 s. */
/* The deadband and the cap on the lead (0.7.213 safety review, finding 4). The slope is the least-squares fit over the
 * ring, not the difference of its two ends, because both ends carry the full noise of one reading each: replayed on the
 * recorded sessions' thermally flat segments the two-point estimator reported a lead of up to 7.8 C, which moved the
 * zone's 86.0 C threshold down to a raw 78 C.
 *   The deadband is on the fitted rise over the ring's own span: under it the die counts as flat and there is no lead at
 *   all. 200 mC over 5 s is 0.04 C/s, well under the 0.12 to 0.15 C/s of session 436, so it costs no signal.
 *   The cap is what makes the bound provable, and the fit alone does not: the recorded 1 s samples jump by up to 1 C on
 *   a die whose time constant is 25 s, and no estimator over a 5 s window can tell one such jump from the start of a
 *   real rise. With the lead capped the zone cannot engage below bc250_dpm_zone_mc() minus the cap, a raw 84.0 C at the
 *   default threshold. It costs almost nothing: over the 21 recorded sessions the median lead on a rising die is 1.2 to
 *   1.5 C, and 436's own rise carries 1.8 to 2.25 C, so the signal sits at or under the cap already. */
/* The zone acts only while the GPU is doing work (0.7.213 safety review, finding 2). An idle GPU is not the heat source
 * the zone addresses: the clock is already at the idle point or the lab floor, a cap stepped down to 800 MHz lowers
 * nothing, and the cap then stays there until the die falls under the soft-release threshold, so the first work of the
 * next burst runs at 800 MHz on a die the CPU was heating. Refusing to act costs no safety, because from the
 * soft-release threshold up the clock gate already refuses every raise: a GPU that gets work at 86 C cannot go above the
 * lab floor whatever the cap says.
 * "Work" is the GFX ring, the graphics engine or the paging engine; the share has to beat a single desktop frame, which
 * is 1000 / BC250_DPM_TICK_MS = 40 permille of its own tick, and QUIET_MS without any of the three is the idle state's
 * own definition of an idle GPU (BC250_DPM_IDLE_HOLD_MS). The gate holds from the governor's first tick and across a
 * stall: zone_quiet_ms starts saturated and saturates again on a clamped tick with no work, because a window counted
 * from a start or from a resume is a window in which the zone acts on a GPU nobody has given work to. */
/* The idle state (0.7.207, owner decision 2026-10-05: "jak lab nie pracuje, to ustawiaj mu zegar gpu na
 * 500 MHz" - when the lab does not work, set its GPU clock to 500 MHz). While the GPU has no work the
 * governor holds the idle point, below the lab floor, at the floor's own 820 mV. The three settings are the
 * KMD's registry values DpmIdleMHz, DpmIdleHoldMs and DpmIdleBusyPermille (driver/kmd/dpm.c), checked and
 * taken by bc250_dpm_idle_config(); they are not part of struct bc250_dpm_tune, so the RUN_DPM_TUNE escape
 * and its ABI do not change. DpmIdleMHz 0 turns the whole state off, which is exactly 0.7.205 behaviour. */
/* The GPU must have had no work for this long before the clock goes to the idle point. The desktop on the
 * GPU (DWM) gives short bursts, so the rule is the busy share over the whole window, not a strict zero. */
/* The share at which one tick of its own leaves the state: the fast exit. It is not the entry threshold on purpose.
 * Entry admits a mean of 2 permille because a static desktop wakes for single frames, and one such frame is
 * 1000 / BC250_DPM_TICK_MS = 40 permille of its own tick (one active GRBM sample of the 25 a 25 ms tick holds),
 * twenty times that mean. Comparing a tick's own share with the entry threshold therefore left the state at the
 * very frame the mean rule was written to tolerate: on the policy itself, one wake per second gave 15 entries and
 * 15 exits a minute and held the point for a quarter of the time. A tick at or above half its own wall time is
 * work no desktop frame reaches: 12.5 ms of GPU work inside one 25 ms tick at 500 MHz is a frame that comes near a
 * 60 Hz deadline, and a game's first busy tick reads 900 permille and more. Anything under it that lasts leaves
 * through the window rule: the work of the trailing window reaches BC250_DPM_IDLE_LEAVE_PERMILLE.
 *
 * Since 0.7.216.6 work on the GFX ring (ring_busy) no longer leaves the state by itself. With the desktop composed
 * on our GPU every DWM frame - the cursor, the clock, an overlay redraw - is a submission, and the ring rule left the
 * idle point at each one: 500 -> 1000 MHz, three seconds at the lab floor for the next quiet window, back to 500, and
 * again, every two seconds while the owner watched and 29 exits in 6.5 minutes at a quiet desktop (2026-10-07). The
 * idle point is now a DPM level with hysteresis: it holds while its own work fits it and is left by work that does
 * not. The ring still gates the entry, which is unchanged. */
/* The slow exit: the trailing window's mean busy share at the idle point, in permille, at which the state is left
 * (DpmIdleLeavePermille). The window is the hold time; its work is counted as the ticks come, and the state is left
 * at the tick at which the window's work reaches this share of the whole window, not at the window's end, so a
 * steady load of at least twice the share leaves within one hold time wherever in a window it starts.
 *
 * Why 150. The measured desktop is far under it: the idle phases of the C62 identification (R120, 24 samples at
 * 1.75 s, 500 MHz, quiet desktop on the GPU) read a tick share of 0 and a busy average of 2 to 6 permille, and the
 * DWM frames that caused the flapping are about 2 ms of GPU work per 2 s, some 1 permille at the lab floor and 2 at
 * the idle point. A desktop that animates at 60 Hz with 2 ms of work a frame is 120 permille here and still fits.
 * And what stays under it fits the point: 150 permille at 500 MHz is about 75 at the lab floor, an order of magnitude
 * under the load governor's own lowering threshold (BC250_DPM_DOWN_PERMILLE, 650), so the lab floor would serve the
 * same work at the same throughput, only each frame's 2.5 ms becomes 1.25 ms, both far inside a 16.7 ms frame. It is
 * the middle of the 100 to 250 permille band the review asked for: below it a video or an animated page starts to
 * flap again, above it a sustained 30 % load (the test's case) would need more than one hold time. */

/* The busy signal (docs/design/dpm.md, "Busy"). The miniport samples GRBM_STATUS.GUI_ACTIVE every
 * HW_SAMPLE_US and counts the samples and the active ones per tick: the graphics engine's own
 * activity, whichever path fed it. KMD 0.7.175 used the GFX ring's submit-to-fence time instead;
 * that stays as the fallback for a tick with too few samples (a sampler that could not start, a
 * power transition). */
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
 * also turns the idle state off while it is set (0.7.207): the operator asked for a clock, not for 500 MHz. */
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
	/* The soft zone (0.7.213, BD-087). zone_delta_mc 0 turns the whole zone off, the lead with it. */
	unsigned int	zone_delta_mc;		/* the zone steps the cap down from HOT_MC - this; 0: no zone */
	unsigned int	zone_step_ms;		/* at most one zone step down per this */
	unsigned int	zone_lead_ms;		/* the lead the zone's thresholds are judged with; 0: raw readings */
};
/* The soft threshold lies strictly between RELEASE_MC and HOT_MC, at least half a degree from each. */
/* The soft zone (0.7.213). Its threshold lies strictly between the soft-release threshold and HOT_MC, at least half a
 * degree from each, so the two never read the same temperature: zone_delta_mc + 500 <= soft_delta_mc, and a zone with
 * no soft release above it is refused (the cap could then only ever fall). The upper bound on zone_delta_mc is the
 * soft-release bound less that half degree; the arithmetic, not a second number, keeps them consistent. The step's
 * lower bound is the hot step's, ten governor ticks. The lead has no lower bound (0 turns it off) and a minute is as
 * far as an extrapolation of a 5 s slope can be worth anything on a die whose time constant is 25 s. */
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
	BC250_DPM_TUNE_ZONE = 8,		/* the soft zone: a value out of range, or no deadband under HOT_MC (0.7.213) */
	BC250_DPM_TUNE_COUNT
};
void bc250_dpm_tune_default(struct bc250_dpm_tune *t);
/* The 0.7.212 thermal rules in a tune: no soft zone, no lead, no soft release, so the hot cap at HOT_MC is the only
 * rule that reads the temperature and the warm zone is back at HOT_MC. The KMD calls it for a start whose
 * DpmThermalZone is 0, which is the one switch that turns the zone off; a run-time RUN_DPM_TUNE reset goes back to
 * bc250_dpm_tune_default() and therefore turns the zone on again. The thresholds, the floor and the hot step are left
 * exactly as they are, so this composes with any other tune. */
void bc250_dpm_tune_zone_off(struct bc250_dpm_tune *t);
/* The zone's step-down threshold in mC, 0 when the zone is off, and the highest reading at which a raise of the clock
 * is still admitted (BC250_DPM_WARM_MC, or the soft-release threshold with the zone on). The KMD's log line, the escape
 * and the host test read both instead of recomputing them. */
int bc250_dpm_zone_mc(const struct bc250_dpm_tune *t);
int bc250_dpm_warm_mc(const struct bc250_dpm_tune *t);
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
 * MIN..MAX_SOFT_STEP_MS (checked even while the soft release is off, so that turning it on later is one field), and
 * then the soft zone (0.7.213): zone delta 0 or MIN..MAX_ZONE_DELTA_MC, zone step MIN..MAX_ZONE_STEP_MS, zone lead at
 * most MAX_ZONE_LEAD_MS, and the deadband zone_delta_mc + 500 <= soft_delta_mc, which also refuses a zone with the soft
 * release off. */
enum bc250_dpm_tune_error bc250_dpm_tune_check(const struct bc250_dpm_tune *t, unsigned int max_level);

struct bc250_dpm_input {
	unsigned int	busy_permille;		/* 0..1000 over this tick */
	int		temperature_mc;
	int		temperature_valid;
	unsigned int	dt_ms;			/* since the previous tick */
	/* Work outstanding on the GFX ring at this tick (the KMD's GfxSubmitBusy): submitted and not yet
	 * retired, whatever the hardware's busy samples say. The idle state reads it for its entry (0.7.207): a
	 * submission waiting on a fence keeps the clock at the lab floor. Since 0.7.216.6 it no longer ends an idle
	 * episode by itself (BC250_DPM_IDLE_EXIT_PERMILLE says why); the soft zone's work gate reads it as well.
	 * Zero is "the ring is empty". */
	int		ring_busy;
	/* The paging node's busy share over this tick, from the same hardware samples as busy_permille
	 * (SDMA0_STATUS_REG.IDLE), 0 for a tick with too few samples. The idle state alone reads it (0.7.207):
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
	/* The idle state (0.7.207). idle_on is the setting of this start, idle the state now. */
	int		idle_on, idle;
	unsigned int	idle_level;		/* the point idle holds; raised to the thermal floor after a refusal */
	unsigned int	idle_hold_ms;		/* the window the GPU must be quiet for */
	unsigned int	idle_busy_permille;	/* the entry window's admitted mean busy share */
	unsigned int	idle_ms;		/* the window so far: the candidate one before entry, the trailing one in idle */
	unsigned int	idle_acc;		/* busy permille x ms over that window, saturating */
	unsigned int	idle_entries, idle_exits, idle_refusals;
	unsigned int	idle_total_ms;		/* time held at the idle point, saturating */
	unsigned int	idle_leave_permille;	/* the slow exit's share (0.7.216.6, BC250_DPM_IDLE_LEAVE_PERMILLE) */
	unsigned int	idle_fast_exits;	/* exits by one tick at BC250_DPM_IDLE_EXIT_PERMILLE (0.7.216.6) */
	unsigned int	idle_slow_exits;	/* exits by the window's work reaching idle_leave_permille (0.7.216.6) */
	/* The soft zone (0.7.213, BD-087). */
	unsigned int	zone_ms;		/* time in the zone since its last step down */
	unsigned int	zone_steps;		/* cap lowerings by the zone */
	unsigned int	zone_ticks;		/* steps spent in the zone */
	int		zone;			/* inside a zone episode: entered at the zone's threshold, left under the
						 * soft-release threshold, so the cap is clamped to the running clock once
						 * per episode and never follows a load lull (0.7.213 review) */
	int		zone_lead_mc;		/* the lead of the last step, 0 without one (the log and the escape print it) */
	int		zone_lead_ok;		/* the ring could measure a slope in the last step (the log prints it): 0
						 * means every soft threshold read the raw sensor, which the zone's own
						 * numbers were not chosen for */
	unsigned int	zone_lead_gaps;		/* steps in which the lead was wanted and unavailable */
	unsigned int	zone_quiet_ms;		/* since the GPU last had work, saturating: the zone acts only under
						 * BC250_DPM_ZONE_QUIET_MS of it. It starts saturated (a governor that has
						 * seen no tick has seen no work) and saturates again on a clamped tick that
						 * brought no work, so no start and no resume gives the zone a window on a
						 * GPU nobody has given work to (0.7.213 safety review, second round) */
	unsigned int	zone_idle_holds;	/* steps in which the zone's threshold was met and the GPU was idle */
	/* The slope ring the lead is measured from: one reading per BC250_DPM_ZONE_SLOPE_SLOT_MS. slope_at_ms and
	 * zone_now_ms are the governor's own millisecond clock and WRAP on purpose - only unsigned differences of them
	 * are ever read, which stay exact across the wrap, so no saturation can turn an age into a zero. A tick whose
	 * dt_ms had to be clamped empties the ring: zone_now_ms then advanced by less than the time that really passed,
	 * so every age in the ring is wrong. A tick that brings no reading skips its slot and keeps the window. */
	int		slope_mc[BC250_DPM_ZONE_SLOPE_SLOTS];
	unsigned int	slope_at_ms[BC250_DPM_ZONE_SLOPE_SLOTS];
	unsigned int	slope_head;		/* the next slot to write, and the oldest one once the ring is full */
	unsigned int	slope_count;		/* slots written, up to BC250_DPM_ZONE_SLOPE_SLOTS */
	unsigned int	slope_push_ms;		/* since the last slot was written */
	unsigned int	zone_now_ms;		/* the governor's millisecond clock, wrapping */
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
 * lab floor again, and a cap already below it is raised to it. The two sub-floor points are outside the
 * published tables (facts M47), so one refusal is enough: the governor does not try the same point again and
 * again. Unit A's own firmware accepts them (facts M785). */
void bc250_dpm_subfloor_refused(struct bc250_dpm_governor *g);

/* ---- the idle state (0.7.207) ----------------------------------------------------------------- */

/* Why an idle setting was refused; the KMD logs it and runs without the idle state. */
enum bc250_dpm_idle_error {
	BC250_DPM_IDLE_OK = 0,
	BC250_DPM_IDLE_CLOCK = 1,	/* DpmIdleMHz is not a table clock below the lab floor */
	BC250_DPM_IDLE_HOLD = 2,	/* DpmIdleHoldMs outside MIN..MAX_HOLD_MS */
	BC250_DPM_IDLE_BUSY = 3,	/* DpmIdleBusyPermille above MAX_BUSY_PERMILLE */
	BC250_DPM_IDLE_LEAVE = 4,	/* DpmIdleLeavePermille outside MIN..MAX_LEAVE_PERMILLE, or not above the entry mean */
	BC250_DPM_IDLE_ERROR_COUNT
};
/* The start's idle setting, after bc250_dpm_init (which leaves the state off, so a caller that does not
 * configure it keeps 0.7.205 behaviour exactly). idle_mhz 0 turns it off and is not an error; any other
 * value must be a clock of the table below BC250_CLOCK_FLOOR_MHZ. A refused setting leaves the state off.
 * Not a run-time operation: the KMD calls it once per start, before the governor thread exists. */
enum bc250_dpm_idle_error bc250_dpm_idle_config(struct bc250_dpm_governor *g, unsigned int idle_mhz,
						unsigned int hold_ms, unsigned int busy_permille);
/* The slow exit's share (DpmIdleLeavePermille, 0.7.216.6), after bc250_dpm_idle_config, which leaves the default in
 * place. MIN..MAX_LEAVE_PERMILLE and strictly above the entry window's admitted mean: a share at or under that mean
 * would leave the state on the very work that entered it. A refused value turns the idle state off, as every refused
 * idle setting does, and the KMD logs it. */
enum bc250_dpm_idle_error bc250_dpm_idle_set_leave(struct bc250_dpm_governor *g, unsigned int leave_permille);
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

/* ---- the operator's V/F curve and its trial (0.7.210) ---------------------------------------- */

/* The curve the governor's levels are applied with, and the trial that lets an operator try one without putting
 * it on disk. Pure policy: the caller (driver/kmd/dpm.c) owns the lock, the time source, the registry and the
 * one SMU transaction. docs/design/tuner.md and ADR 0020 are the design; the host test drives this struct
 * directly, so the whole lifecycle is tested with no kernel in the loop.
 *
 * Three curves live here:
 *   stored     what the registry holds, and what every revert goes back to. The start reads it.
 *   active     what the governor applies now. Equal to stored outside a trial.
 *   candidate  the curve under trial.
 *
 * A SET stores a candidate, makes it active, starts the window and raises apply. The caller re-applies the
 * current level once at its next tick, which is the only way a curve reaches the hardware. A KEEP inside the
 * window makes the candidate the stored curve and tells the caller to persist it. A CANCEL, the deadline, a
 * stop or a power transition put the stored curve back and raise apply again. Nothing here writes to disk, so a
 * killed tool, a hung tool, a lost remote session and a bugcheck all end at the stored curve. */
/* A KEEP needs a candidate that has run: this much of the window must have passed, and the governor must have
 * applied the candidate's own serial (0.7.211). SET and KEEP inside one governor tick stored a curve that never
 * reached the hardware, and a SET whose apply the clock gate refused as too hot stored the candidate all the
 * same. Four governor ticks, which is the shortest window in which a level is re-applied and read back. */

struct bc250_dpm_curve_state {
	struct bc250_clock_curve stored, active, candidate;
	int		trial;			/* a candidate is on trial */
	unsigned int	trial_ms, elapsed_ms;	/* the window, and how much of it has passed */
	unsigned int	serial;			/* one more for every SET: the caller's "apply" bookkeeping */
	unsigned int	applied;		/* the serial the governor has applied */
	unsigned int	sets, keeps, cancels, reverts;
	int		apply;			/* the caller must re-apply the current level once */
};

/* The start's curve: stored and active both become *stored (the default line when it is 0). */
void bc250_dpm_curve_init(struct bc250_dpm_curve_state *s, const struct bc250_clock_curve *stored);
/* A checked candidate becomes active and starts a window of trial_ms (clamped to MIN..MAX_MS); a refused one
 * changes nothing. *level (optional) names the level a refusal broke. A second SET replaces the first candidate
 * and restarts the window; the revert target stays the stored curve, never the first candidate. */
enum bc250_clock_curve_error bc250_dpm_curve_set(struct bc250_dpm_curve_state *s, const struct bc250_clock_curve *c,
						 unsigned int trial_ms, unsigned int *level);
/* 1 when a trial was kept: the candidate is now stored and the caller must persist it. 0 when none ran, and -1
 * when one runs but the governor has not carried it for BC250_DPM_CURVE_KEEP_MIN_MS yet, which is a different
 * answer for the caller to show (BC250_CLOCK_CURVE_UNTRIED). Nothing is stored in either refusal. */
int bc250_dpm_curve_keep(struct bc250_dpm_curve_state *s);
/* 1 when a trial was ended and the stored curve put back (apply raised). 0 when none ran. */
int bc250_dpm_curve_cancel(struct bc250_dpm_curve_state *s);
/* The table's own line becomes stored and active, any trial ends. 1 when something changed, so the caller knows
 * whether to delete the registry values. */
int bc250_dpm_curve_reset(struct bc250_dpm_curve_state *s);
/* One tick's worth of the window. 1 when the deadline passed and the stored curve was put back. */
int bc250_dpm_curve_tick(struct bc250_dpm_curve_state *s, unsigned int dt_ms);
/* 1 once after every change of the active curve: the caller re-applies the current level through its checked
 * transaction. Clears the flag; it does NOT record the serial as applied, because at that point the apply has
 * not happened yet - bc250_dpm_curve_applied() records it once the transaction went through (0.7.211). */
int bc250_dpm_curve_take(struct bc250_dpm_curve_state *s);
/* The serial the caller has just applied to the hardware, after a transaction that went through. Ignored when a
 * newer SET arrived in the meantime, so a candidate nobody applied can never count as applied. */
void bc250_dpm_curve_applied(struct bc250_dpm_curve_state *s, unsigned int serial);
/* The active curve's voltage and VID at a level of the whole table (below BC250_CURVE_FIRST_LEVEL: the table's
 * own, where no curve may act). Every reader of the voltage column goes through these two. */
unsigned int bc250_dpm_curve_level_mv(const struct bc250_dpm_curve_state *s, unsigned int level);
unsigned int bc250_dpm_curve_level_vid(const struct bc250_dpm_curve_state *s, unsigned int level);
/* What is left of the window, 0 outside a trial. */
unsigned int bc250_dpm_curve_remaining_ms(const struct bc250_dpm_curve_state *s);

/* ---- the session marker ---------------------------------------------------------------------- */

/* DpmSession is written (and flushed) when the governor first goes above the floor (a thermal-only level
 * under it counts as "not above the floor": it is no risk to carry into the next start), and deleted after
 * SESSION_CLEAR_MS at the floor or at a clean stop. A start that finds it knows the previous one
 * ended above the floor without a stop: a bugcheck, a hang, a power cut at a high clock. */
enum bc250_dpm_session_action { BC250_DPM_SESSION_NONE = 0, BC250_DPM_SESSION_SET = 1, BC250_DPM_SESSION_CLEAR = 2 };
struct bc250_dpm_session {
	int		marked;			/* the caller sets it after a durable write, clears after a delete */
	unsigned int	floor_ms;
};
/* Call with the level about to be applied (SET must be durable before it) and after each tick. */
enum bc250_dpm_session_action bc250_dpm_session_step(struct bc250_dpm_session *s, unsigned int level,
						     unsigned int dt_ms);

/* ---- the joint power arm (C62, 0.7.216.7) ------------------------------------------------------ */

/* The GPU clock governor above, the CPU surface (bc250_cpu.h) and the fan control each run their own loop, and none
 * of them knows what the others cost in heat. In a GPU-bound game the package's heat is shared: the CPU's part of it
 * takes clock from the GPU through the thermal cap and the soft zone, while the CPU itself waits for the GPU. The
 * joint arm closes that loop in one direction only. When the GPU is the bottleneck AND its clock is held by heat, it
 * lowers the CPU's maximum boost clock (queue 3 BC250_CPU_MSG_SET_MAX_MHZ, the one CPU control that is a pure
 * lowering) one step at a time; the GPU gets the headroom back through the governor's own soft release, whose rules
 * stay exactly as they are. When the GPU is no longer the bottleneck, or the heat is gone, the CPU limit comes back.
 *
 * Pure policy, like the rest of this file: the KMD (driver/kmd/dpm.c) feeds one tick of the governor's own state and
 * the CPU surface's readiness, and publishes the cap it returns; driver/kmd/cpu.c owns every mailbox message. The
 * arm never names a GPU clock, never touches a thermal rule and never sends anything itself.
 *
 *   bound      the governor's busy average at or above BOUND_PERMILLE: the GPU is the bottleneck
 *   free       the average under FREE_PERMILLE: it is not
 *   heat       the governor's clock is held by a thermal rule: an 87 C episode, the soft zone, a throttle that names a
 *              thermal rule (soft, hard, warm, zone), or a lowered thermal cap with the reading still at or above the
 *              soft-release threshold. A lowered cap under that threshold is the soft release at work, and the arm
 *              waits for it instead of capping the CPU further.
 *   cool       a valid reading under BC250_DPM_HOT_MC - COOL_DELTA_MC, the thermal cap released, no episode, no zone
 *   blind      no temperature reading this tick: the arm holds whatever it has (the governor itself goes to the floor)
 *
 * The states, by enum bc250_joint_reason:
 *   no cap     bound and heat for ENGAGE_MS -> the first cap, base - STEP_MHZ (never under MIN_MHZ)
 *   capped     bound and heat for another STEP_MS -> one step lower, down to MIN_MHZ
 *              free for FREE_MS -> released at once, the whole cap
 *              cool for COOL_MS -> one step higher; reaching base is the release
 *   any        the CPU surface not ready (CpuTune 0, queue 3 not proven, a trial, a search, an owed revert, a fault),
 *              or a base that leaves no room under it -> no cap, every timer cleared
 * The cap never goes above base, and a base that falls to the cap or below releases it. */
#define BC250_JOINT_BOUND_PERMILLE	850u	/* the governor raises from 900: a bound GPU averages above this */
#define BC250_JOINT_FREE_PERMILLE	600u	/* well under the governor's own 651 target: a menu or a loading screen */
#define BC250_JOINT_ENGAGE_MS		2000u	/* bound and hot this long before the first CPU message */
#define BC250_JOINT_FREE_MS		2000u
#define BC250_JOINT_STEP_MS		4000u	/* one CPU step per this, at most: the package's heat answers in seconds */
#define BC250_JOINT_COOL_MS		8000u	/* a step back up waits twice as long as a step down */
#define BC250_JOINT_STEP_MHZ		200u
#define BC250_JOINT_MIN_MHZ		2800u	/* BC250_CPU_MIN_MHZ; driver/kmd/cpu.c asserts that they are equal */
#define BC250_JOINT_COOL_DELTA_MC	6000	/* 81 C: two degrees under the default soft-release threshold of 83 C */

enum bc250_joint_reason {
	BC250_JOINT_OFF = 0,		/* the arm is off for this start (DpmJointGovernor 0) */
	BC250_JOINT_NO_CPU = 1,		/* the CPU surface cannot take a cap now */
	BC250_JOINT_NO_ROOM = 2,	/* the CPU's base clock is at MIN_MHZ already: nothing to lower */
	BC250_JOINT_FREE = 3,		/* the GPU is not bound, or not held by heat: no cap */
	BC250_JOINT_WAIT = 4,		/* bound and hot, ENGAGE_MS not yet over */
	BC250_JOINT_CAPPING = 5,	/* this tick lowered the cap */
	BC250_JOINT_HOLD = 6,		/* capped, waiting (for a step, for the cool time, at MIN_MHZ) */
	BC250_JOINT_RAISING = 7,	/* this tick raised the cap one step */
	BC250_JOINT_BLIND = 8,		/* no temperature reading: everything held */
	BC250_JOINT_REASON_COUNT
};

struct bc250_joint_input {
	unsigned int	busy_permille;		/* the governor's busy average (bc250_dpm_governor.avg_permille) */
	int		heat, cool, blind;
	int		cpu_ready;		/* the CPU surface can take a cap now (driver/kmd/cpu.c) */
	unsigned int	base_mhz;		/* the CPU's limit without the arm: the operator's, or the recorded baseline */
	unsigned int	dt_ms;
};

struct bc250_joint {
	unsigned int	cap_mhz;		/* the CPU limit the arm asks for, 0 for none */
	unsigned int	reason;			/* enum bc250_joint_reason of the last step */
	unsigned int	bound_ms, free_ms, step_ms, cool_ms;
	unsigned int	engages, steps_down, steps_up, releases;
};

void bc250_joint_init(struct bc250_joint *j);
/* The tick's heat, cool, blind and busy average out of the governor right after bc250_dpm_step(), and the tick's
 * input. The caller adds cpu_ready and base_mhz. */
void bc250_joint_read(const struct bc250_dpm_governor *g, const struct bc250_dpm_input *in,
		      struct bc250_joint_input *out);
/* One tick. Returns the CPU limit to ask for, 0 for none; j->reason says why. */
unsigned int bc250_joint_step(struct bc250_joint *j, const struct bc250_joint_input *in);
/* The cap is gone outside a tick (a stop, a power transition, the governor gave up): no cap, timers cleared, and a
 * cap that was in force counts as a release. */
void bc250_joint_reset(struct bc250_joint *j);

#endif
