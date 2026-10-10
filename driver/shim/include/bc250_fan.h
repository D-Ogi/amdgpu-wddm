/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 case fan control (Part B of bc250-win docs/design/fan.md). Pure policy: no OS call, no lock of its own,
 * no time source. The miniport (driver/kmd/fan.c) owns the gate, the timers, the escape and the exit paths, and
 * calls in here once a second from the governor thread.
 *
 * The owner's rule (2026-10-06): the driver may take the fan from the BIOS curve while it runs, the BIOS "Fan
 * Setting" stays unchanged, and the driver gives the fan back to the chip's automatic mode on every exit path.
 * This module is the one place that decides a duty and the one place that writes the chip, through
 * bc250_hwmon_write8() and its three-register allowlist.
 *
 * What the chip does, measured on unit A (fact M803):
 *   - open:   0x80 -> 0x0A01, then the engine status 0x0CF8 goes from 0x60 to 0x08 (PHASE) within one 1 ms poll.
 *   - write:  set bit 1 of the mode mask 0x0A00, then the duty target 0x0A29 (0..255). Both inside the phase.
 *   - close:  0x40 -> 0x0A01, then CHECK_DONE and LOCK return within three or four 1 ms polls. INVALID never set.
 *   - result: duty 255 drives the fan to 1699..1749 RPM, duty 102 to 771..774 RPM, and the duty read-back 0x161
 *             follows the written target within one second.
 *   - restore: target 128 and mode 0xE0 give the fan back to the EC curve (1357 RPM after 3 s).
 *
 * Rules this module keeps, each one a host test:
 *   1. Only three registers are ever written (bc250_hwmon_write_allowed), each inside an open phase.
 *   2. The restore record is taken once, inside the first open phase, before the first change. A mode bit that is
 *      already ours at that moment (a previous driver died while it held the fan) is not recorded as the board's:
 *      the record then holds the M803 rest values instead.
 *   3. The restore writes the target first and the mode second, and clears our bit only.
 *   4. On doubt, full speed: a stale or invalid temperature, two temperature sources more than 10 C apart, a stale
 *      reader. Doubt held for 5 s gives the fan back to the board. A failed handshake, a target that does not read
 *      back, a mode bit that does not stick, a duty read-back that does not follow and a stopped fan give the fan
 *      back at once and latch a fault: no further write in this start.
 *   5. The duty never goes below 20 %, and 0 % is never written.
 *   6. Emergency: a guard temperature at or above 87 C forces 100 % until it stays at or below 82 C for 10 s.
 *   7. The curve output rises at once and falls only after 10 s below, by 10 points at most every 2 s.
 *   8. A leased mode (a fixed duty, or a curve that was not stored) ends with the fan given back to the board.
 *   9. A sustained heavy load drives the fan to full speed before the curve gets there (the load feed-forward,
 *      rule 10 in docs/design/fan.md, whose list counts a refusal by the chip as a rule of its own,
 *      owner 2026-10-10: "Robiąc takie testy sterownik powinien sam ogarnąć, że trzeba wiać z maksymalną mocą!",
 *      "doing such tests the driver should work out by itself that it has to blow at full power"). The
 *      feed-forward only ever RAISES the duty above the curve, it needs the governor's load feed, and the
 *      FanLoadBoost setting switches it off.
 *
 * Register facts: Linux mainline drivers/hwmon/nct6683.c and the out-of-tree nct6687d (both GPL-2.0, facts only, no
 * code taken), confirmed on unit A by M803.
 */
#ifndef BC250_FAN_H
#define BC250_FAN_H

#include "bc250_hwmon.h"

/* ---- bounds -------------------------------------------------------------------------------------------------- */

#define BC250_FAN_POINTS_MIN		2u
#define BC250_FAN_POINTS_MAX		8u
#define BC250_FAN_FLOOR_PCT		20u	/* a three-wire fan can stall below this and not restart by itself */
#define BC250_FAN_FULL_PCT		100u
#define BC250_FAN_TEMP_MIN_C		20u	/* curve point temperatures, the range of the BIOS Customize page */
#define BC250_FAN_TEMP_MAX_C		95u

/* Emergency: the fan tries before the clock pays. The DPM's hot step is 87 C as well, and the curve of every
 * preset reaches 100 % at or below 85 C, so this is the backstop for a quiet custom curve. */
#define BC250_FAN_EMERGENCY_ON_MC	87000
#define BC250_FAN_EMERGENCY_OFF_MC	82000	/* the DPM's own release temperature */
#define BC250_FAN_EMERGENCY_HOLD_MS	10000u

#define BC250_FAN_HYSTERESIS_MC		3000	/* the curve's input falls only once it is 3 C under its peak */
#define BC250_FAN_FALL_HOLD_MS		10000u	/* the output falls only after this long below */
#define BC250_FAN_FALL_STEP_PCT		10u	/* and by at most this much */
#define BC250_FAN_FALL_GAP_MS		2000u	/* per this long */

#define BC250_FAN_DISAGREE_MC		10000	/* Tctl and the EC's own SB-TSI reading: unit A reads them within 0.2 C */
#define BC250_FAN_DOUBT_HANDBACK_MS	5000u	/* doubt held this long: the board gets the fan back */
#define BC250_FAN_RETAKE_MS		30000u	/* clean inputs this long after such a handback: the driver takes again */
#define BC250_FAN_RETAKES_MAX		3u	/* per start, then the board keeps it */
#define BC250_FAN_FAULT_RETRY_MS	10000u	/* a restore that failed is tried again this often */

#define BC250_FAN_READBACK_TOLERANCE	8u	/* duty read-back against the written target, counts of 255 */
#define BC250_FAN_SETTLE_MS		2000u	/* after a write, before the read-back and the tachometer are judged */
#define BC250_FAN_STOPPED_SAMPLES	3u

/* ---- the load feed-forward (rule 10) -------------------------------------------------------------------------
 *
 * The curve alone always answers late: it reads the heat that the load has already made. On 2026-10-10 an LLM
 * benchmark arm on unit A held the GPU at 93 to 100 % busy and 107 to 122 W of SMU socket power, and Tctl walked
 * from 62.8 C to 76.9 C in 14 s. The Standard curve answered with 95 % duty, and the owner asked for the obvious:
 * the driver sees the load, so it must blow at full power without waiting for the temperature.
 *
 * A step is HEAVY when the load feed says any of these:
 *   - the GPU busy share of the whole step is at or above BC250_FAN_BOOST_BUSY_PERMILLE, and the GFX clock is at
 *     or above BC250_FAN_BOOST_MHZ (a busy GPU at the 500 MHz idle point is not a heavy load);
 *   - the SMU socket power is at or above BC250_FAN_BOOST_POWER_MW (unit A idles at 41 to 56 W);
 *   - the guard temperature rose by BC250_FAN_BOOST_RISE_MC or more over the last BC250_FAN_BOOST_RISE_MS.
 *
 * Heavy time is counted in elapsed milliseconds, never in steps, so the rule does not depend on the governor's
 * cadence: a heavy step adds its own dt and any other step takes its dt away. The account stops at
 * BC250_FAN_BOOST_LOAD_MAX_MS, which is the margin that lets a single quiet step inside a load (the LLM arm has
 * them) pass without disarming the boost. The boost engages at BC250_FAN_BOOST_ARM_MS, so a one-second spike
 * every ten seconds (a menu, one compile) never reaches it.
 *
 * The way down: once the load is no longer heavy the boost holds for BC250_FAN_BOOST_HOLD_MS, and after that it
 * ends only when the curve itself asks for less than the duty in force. The ordinary slope rule (rule 7) then
 * takes the duty down, 10 points at most every 2 s, so the fan never drops in one step.
 */
#define BC250_FAN_BOOST_BUSY_PERMILLE	850u	/* the GPU busy share of the whole step */
#define BC250_FAN_BOOST_MHZ		1000u	/* and the GFX clock at or above the lab floor */
#define BC250_FAN_BOOST_POWER_MW	85000u	/* or the socket power: idle 41 to 56 W, the LLM arm 107 to 122 W */
#define BC250_FAN_BOOST_RISE_MS		3000u	/* or the guard temperature over this window */
#define BC250_FAN_BOOST_RISE_MC		3000	/* rising by this much, which is 1 C a second */
#define BC250_FAN_BOOST_ARM_MS		2000u	/* heavy for this long: the boost engages */
#define BC250_FAN_BOOST_LOAD_MAX_MS	4000u	/* and the heavy-time account stops here */
#define BC250_FAN_BOOST_HOLD_MS		30000u	/* after the load: the boost holds at least this long */

/* Why the boost is on, the bits of the last heavy step. */
#define BC250_FAN_BOOST_WHY_BUSY	1u
#define BC250_FAN_BOOST_WHY_POWER	2u
#define BC250_FAN_BOOST_WHY_RISE	4u
#define BC250_FAN_BOOST_WHY_ALL		7u

#define BC250_FAN_LEASE_MIN_MS		5000u
#define BC250_FAN_LEASE_MAX_MS		300000u
#define BC250_FAN_LEASE_DEFAULT_MS	30000u

/* The handshake polls. One poll is one register read, then delay_us. M803 saw the open answer within one 1 ms poll
 * and the close within four, so 50 ms is a wide bound that a working chip never reaches. */
#define BC250_FAN_POLL_US		250u
#define BC250_FAN_POLL_MAX		200u
/* The blind restore of the bugcheck path: no lock, no verification, a 2 ms bound. */
#define BC250_FAN_BLIND_POLL_US		100u
#define BC250_FAN_BLIND_POLL_MAX	20u

/* The miniport's own watchdog gives the fan back when the control step has not run for this long. */
#define BC250_FAN_WATCHDOG_MS		3000u

/* ---- enums, shared with the escape (BC250_ESCAPE_FAN) ------------------------------------------------------- */

enum bc250_fan_mode {
	BC250_FAN_MODE_BOARD = 0,	/* the EC curve of the BIOS setting runs the fan; the driver writes nothing */
	BC250_FAN_MODE_CURVE = 1,	/* the driver's curve */
	BC250_FAN_MODE_FIXED = 2,	/* one duty, for a bounded lease */
	BC250_FAN_MODE_COUNT
};

enum bc250_fan_profile {
	BC250_FAN_PROFILE_CUSTOM = 0,		/* the curve points come with the request */
	BC250_FAN_PROFILE_STANDARD = 1,		/* the default: at least as aggressive as the BIOS Standard Mode */
	BC250_FAN_PROFILE_QUIET = 2,
	BC250_FAN_PROFILE_PERFORMANCE = 3,
	BC250_FAN_PROFILE_COUNT
};

enum bc250_fan_state {
	BC250_FAN_STATE_OFF = 0,	/* EnableFanControl 0, or no chip this start can drive */
	BC250_FAN_STATE_BOARD = 1,	/* the EC curve runs the fan */
	BC250_FAN_STATE_CURVE = 2,	/* the driver's curve runs it */
	BC250_FAN_STATE_FIXED = 3,	/* a fixed duty runs it, under a lease */
	BC250_FAN_STATE_EMERGENCY = 4,	/* 100 %, the guard temperature reached 87 C */
	BC250_FAN_STATE_DOUBT = 5,	/* 100 %, an input is not believed */
	BC250_FAN_STATE_FAULT = 6,	/* the chip refused something: given back, no further write in this start */
	BC250_FAN_STATE_COUNT
};

/* Why the fan was given back, or why it stays with the board. */
enum bc250_fan_reason {
	BC250_FAN_REASON_NONE = 0,
	BC250_FAN_REASON_USER = 1,		/* a request for the board's own curve */
	BC250_FAN_REASON_STOP = 2,		/* device stop or remove */
	BC250_FAN_REASON_POWER = 3,		/* a transition out of D0 */
	BC250_FAN_REASON_UNLOAD = 4,		/* driver unload */
	BC250_FAN_REASON_WATCHDOG = 5,		/* the control step stopped running */
	BC250_FAN_REASON_LEASE = 6,		/* a leased mode was not renewed */
	BC250_FAN_REASON_TEMPERATURE = 7,	/* no Tctl, or Tctl and the EC disagree */
	BC250_FAN_REASON_READER = 8,		/* the hardware monitor's reading is stale */
	BC250_FAN_REASON_HANDSHAKE = 9,		/* the configuration phase did not open or close */
	BC250_FAN_REASON_READBACK = 10,		/* the duty target or the duty read-back did not follow */
	BC250_FAN_REASON_MODE = 11,		/* the mode bit did not stick, or did not clear */
	BC250_FAN_REASON_STOPPED = 12,		/* duty at or above the floor and the fan does not turn */
	BC250_FAN_REASON_DISABLED = 13,		/* the control was switched off while the driver held the fan */
	BC250_FAN_REASON_BUGCHECK = 14,		/* the blind restore of the bugcheck path ran */
	BC250_FAN_REASON_COUNT
};

/* Why a request was refused. */
enum bc250_fan_error {
	BC250_FAN_ERROR_OK = 0,
	BC250_FAN_ERROR_MODE = 1,	/* not a mode */
	BC250_FAN_ERROR_PROFILE = 2,	/* not a profile */
	BC250_FAN_ERROR_POINTS = 3,	/* fewer than 2 or more than 8 points */
	BC250_FAN_ERROR_TEMPERATURE = 4,	/* a point outside 20..95 C, or temperatures that do not rise */
	BC250_FAN_ERROR_DUTY = 5,	/* a duty outside 20..100 %, or duties that fall as the temperature rises */
	BC250_FAN_ERROR_LEASE = 6,	/* a lease outside 5..300 s, or a fixed duty without one */
	BC250_FAN_ERROR_COUNT
};

/* The chip operations' answers. Zero is success. */
#define BC250_FAN_E_BUSY	(-1)	/* the engine stayed in a phase, or a request stayed pending, before the open */
#define BC250_FAN_E_OPEN	(-2)	/* the phase was not granted */
#define BC250_FAN_E_CLOSE	(-3)	/* CHECK_DONE did not come back */
#define BC250_FAN_E_INVALID	(-4)	/* the chip rejected the configuration */
#define BC250_FAN_E_UNLOCKED	(-5)	/* LOCK stayed clear after the close */
#define BC250_FAN_E_VERIFY	(-6)	/* the duty target did not read back as written */
#define BC250_FAN_E_MODE	(-7)	/* the mode bit did not stick, or did not clear */

/* ---- curves -------------------------------------------------------------------------------------------------- */

struct bc250_fan_point {
	unsigned int	c;	/* degrees C */
	unsigned int	pct;	/* duty, percent */
};

struct bc250_fan_curve {
	unsigned int		points;
	struct bc250_fan_point	p[BC250_FAN_POINTS_MAX];
};

/* The curve of a profile. Zero on success; BC250_FAN_ERROR_PROFILE for CUSTOM or an unknown value. */
int bc250_fan_profile_curve(unsigned int profile, struct bc250_fan_curve *out);

/* enum bc250_fan_error: 2..8 points, temperatures in 20..95 C that rise strictly, duties in 20..100 % that never
 * fall as the temperature rises. */
int bc250_fan_curve_check(const struct bc250_fan_curve *curve);

/* The duty a checked curve asks for at `mc` millidegrees: the first point's duty below it, the last point's above
 * it, and the straight line between two points, rounded UP to a whole percent. Never below the floor. */
unsigned int bc250_fan_curve_eval(const struct bc250_fan_curve *curve, int mc);

unsigned int bc250_fan_pct_to_raw(unsigned int pct);	/* (pct * 255 + 50) / 100, at most 255 */
unsigned int bc250_fan_raw_to_pct(unsigned int raw);	/* rounded to nearest */

/* ---- requests ------------------------------------------------------------------------------------------------ */

struct bc250_fan_request {
	unsigned int		mode;		/* enum bc250_fan_mode */
	unsigned int		profile;	/* CURVE: enum bc250_fan_profile */
	struct bc250_fan_curve	curve;		/* CURVE with PROFILE_CUSTOM */
	unsigned int		fixed_pct;	/* FIXED: 20..100 */
	unsigned int		lease_ms;	/* FIXED: 5..300 s, required. CURVE: 0 is durable, else 5..300 s */
};

/* ---- the controller ------------------------------------------------------------------------------------------ */

/* The board's own values, read once inside the first open phase before the first change (rule 2). */
struct bc250_fan_restore {
	unsigned int	valid;
	unsigned int	mode;		/* 0x0A00 as found, with our bit clear */
	unsigned int	target;		/* 0x0A29 as found */
	unsigned int	substituted;	/* our bit was already set: mode and target are the M803 rest values */
};

/* One second of inputs, gathered by the miniport. */
struct bc250_fan_input {
	unsigned int	dt_ms;		/* since the previous tick */
	int		tctl_mc;	/* the SMU's Tctl, this tick */
	int		tctl_valid;
	int		tsi_mc;		/* the EC's own SB-TSI channel, this sample */
	int		tsi_valid;
	int		tsi_mapped;	/* the identity found an APU channel at all */
	int		reader_valid;	/* the hardware monitor is online and its sample is fresh */
	unsigned int	rpm;		/* tachometer BC250_FAN_CHANNEL */
	int		rpm_valid;
	unsigned int	readback_raw;	/* duty read-back 0x160 + BC250_FAN_CHANNEL */
	int		readback_valid;
	/* The governor's load feed (rule 10). A step without it runs on the curve alone: the feed-forward then never
	 * engages, because a temperature that rises without a load reading is the curve's and the emergency's work. */
	int		load_valid;	/* the feed below belongs to this step */
	unsigned int	busy_permille;	/* GPU busy, the mean over the whole step, 0..1000 */
	unsigned int	gfx_mhz;	/* the GFX clock the governor holds; 0 means unknown, which passes the test */
	unsigned int	socket_mw;	/* the SMU socket power, when power_valid */
	int		power_valid;
};

struct bc250_fan_ctl {
	/* set by bc250_fan_init and bc250_fan_set */
	unsigned int		enabled;	/* EnableFanControl and a chip this start may drive */
	unsigned int		mode;		/* enum bc250_fan_mode in force */
	unsigned int		profile;
	struct bc250_fan_curve	curve;
	unsigned int		fixed_pct;
	unsigned int		lease_ms;	/* left of a leased mode, 0 for a durable one */
	/* the durable mode a lease ends with (rule 9): the last request without a lease, or the start's mode */
	unsigned int		durable_mode;	/* BOARD or CURVE */
	unsigned int		durable_profile;
	struct bc250_fan_curve	durable_curve;
	/* the chip */
	unsigned int		controlling;	/* our mode bit is set in the chip */
	struct bc250_fan_restore restore;	/* kept across a device stop and start */
	unsigned int		written_raw;	/* the duty target we wrote last */
	unsigned int		since_write_ms;
	/* the loop */
	int			guard_mc;	/* max(Tctl, EC SB-TSI) this tick */
	unsigned int		guard_valid;
	int			effective_mc;	/* the curve's input after the hysteresis */
	unsigned int		effective_valid;
	unsigned int		target_pct;	/* what the curve, the fixed duty or the emergency asks for */
	unsigned int		applied_pct;	/* what the slope rule let through */
	unsigned int		below_ms, fall_wait_ms;
	unsigned int		emergency, emergency_cool_ms;
	/* the load feed-forward (rule 10) */
	unsigned int		boost_enabled;	/* FanLoadBoost: 1 unless the user switched the feed-forward off */
	unsigned int		boost;		/* the feed-forward holds the fan at full speed now */
	unsigned int		boost_why;	/* BC250_FAN_BOOST_WHY_* of the last heavy step */
	unsigned int		boost_load_ms;	/* the heavy-time account, 0..BC250_FAN_BOOST_LOAD_MAX_MS */
	unsigned int		boost_hold_ms;	/* since the load stopped being heavy */
	int			rise_ref_mc;	/* the guard temperature the open rise window started at */
	unsigned int		rise_ms;	/* into that window */
	int			rise_mc;	/* the rise of the window that closed last */
	unsigned int		rise_valid;	/* a window has closed since the start */
	unsigned int		doubt_ms, held_back, clean_ms, retakes;
	unsigned int		stopped_samples;
	unsigned int		fault, fault_retry_ms;
	unsigned int		state;		/* enum bc250_fan_state */
	unsigned int		doubt;		/* enum bc250_fan_reason, 0 without doubt */
	unsigned int		reason;		/* the last handback, enum bc250_fan_reason */
	int			last_error;	/* the last chip operation's answer */
	/* counters */
	unsigned long long	takeovers, handbacks, writes, failures, emergencies, doubts, lease_expiries;
	unsigned long long	boosts, boost_ms;	/* engagements of the feed-forward, and the time it held */
};

/* A start: `enabled` says whether this start may drive the chip at all, `start` is the mode the start runs
 * (the stored choice, already checked; null means the STANDARD curve). The restore record survives. */
void bc250_fan_init(struct bc250_fan_ctl *ctl, int enabled, const struct bc250_fan_request *start);

/* The FanLoadBoost setting (rule 10), at the start and when the operator changes it. The feed-forward is on
 * unless this says otherwise; switching it off also ends a boost that holds now, and the slope rule then takes
 * the duty down. */
void bc250_fan_load_boost(struct bc250_fan_ctl *ctl, int enabled);

/* Checks a request and resolves its curve. enum bc250_fan_error. */
int bc250_fan_request_check(const struct bc250_fan_request *request, struct bc250_fan_curve *resolved);

/* A request from the operator, applied at the next tick. enum bc250_fan_error; nothing changes on a refusal.
 * A request clears a doubt hold-back (the operator asked for the fan), never a fault. */
int bc250_fan_set(struct bc250_fan_ctl *ctl, const struct bc250_fan_request *request);

/* A renewal of a leased mode. Zero when a lease runs and was renewed. */
int bc250_fan_renew(struct bc250_fan_ctl *ctl, unsigned int lease_ms);

/* One control step: the inputs, the decision and the chip writes it needs. The answer of the last chip operation,
 * 0 when there was none or it succeeded. */
int bc250_fan_tick(const struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl, const struct bc250_fan_input *in);

/* An exit path: the fan goes back to the board now, with the full handshake and verification. Zero when the fan is
 * the board's afterwards (also when the driver did not hold it). A failure latches the fault and keeps
 * `controlling`, so the next exit path, the bugcheck callback and the fault retry still try. */
int bc250_fan_handback(const struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl, unsigned int reason);

/* The bugcheck path: HIGH_LEVEL, other processors stopped. Port writes only: no lock (the io must have none), at
 * most BC250_FAN_BLIND_POLL_MAX polls, no verification, no retry. Nothing happens unless the driver holds the fan. */
void bc250_fan_handback_blind(const struct bc250_hwmon_io *io, struct bc250_fan_ctl *ctl);

/* The chip operations, exposed for the host test. Each returns 0 or a BC250_FAN_E_* value. */
int bc250_fan_open(const struct bc250_hwmon_io *io);
int bc250_fan_close(const struct bc250_hwmon_io *io);

/* Names for logs and tools. */
const char *bc250_fan_state_name(unsigned int state);
const char *bc250_fan_reason_name(unsigned int reason);
const char *bc250_fan_mode_name(unsigned int mode);
const char *bc250_fan_profile_name(unsigned int profile);
const char *bc250_fan_boost_name(unsigned int why);	/* the BC250_FAN_BOOST_WHY_* bits as one word */

#endif
