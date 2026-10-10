// BC-250 board operating envelope. Not an AMD-wide chip capability.
#ifndef BC250_BOARD_ENVELOPE_H
#define BC250_BOARD_ENVELOPE_H
#define BC250_CLOCK_MIN_MHZ 500u        /* the table's lowest clock: the idle point, below the lab floor */
#define BC250_CLOCK_FLOOR_MHZ 1000u     /* the lab point, and the lowest clock the load may ask for */
#define BC250_CLOCK_CEILING_MHZ 2000u
#define BC250_CLOCK_STEP_MHZ 100u
#define BC250_CLOCK_LEVELS 16u
#define BC250_CLOCK_FLOOR_MV 820u
#define BC250_CLOCK_CEILING_MV 1000u
#define BC250_CLOCK_HOT_MC 87000     /* no raise of clock or voltage at or above this (owner, 2026-10-01; was 85000) */
#define BC250_CURVE_UNDERVOLT_MV 25u    /* how far under the table's line one level may go */
#define BC250_CPU_MIN_MHZ		2800u
#define BC250_CPU_MAX_MHZ		3500u
#define BC250_CPU_MAX_MHZ_LAB		4000u
#define BC250_CPU_LAB_MIN_UV_STEPS	4u	/* about 13 to 17 mV: what the lab bound needs first */
#define BC250_CPU_UV_MAX_STEPS		16u	/* about 50 to 115 mV; the reference tool's own GUI stops at 40 */
#define BC250_CPU_TEMP_MIN_C		85u
#define BC250_CPU_TEMP_MAX_C		100u	/* the firmware default, and the value a restore puts back */
#define BC250_CPU_REFUSE_MV		1300u
#define BC250_CPU_PLAUSIBLE_MIN_MV	700u	/* a readback outside this band is not a voltage we understand */
#define BC250_CPU_PLAUSIBLE_MAX_MV	1600u
#define BC250_CPU_STRETCH_MHZ		200u
#define BC250_CPU_GPU_BUSY_PERMILLE	500u
#define BC250_CPU_MASK_STOCK	0x77u
#define BC250_CPU_MASK_FULL	0xFFu
#define BC250_FAN_POINTS_MIN		2u
#define BC250_FAN_POINTS_MAX		8u
#define BC250_FAN_FLOOR_PCT		20u	/* a three-wire fan can stall below this and not restart by itself */
#define BC250_FAN_FULL_PCT		100u
#define BC250_FAN_TEMP_MIN_C		20u	/* curve point temperatures, the range of the BIOS Customize page */
#define BC250_FAN_TEMP_MAX_C		95u
#define BC250_FAN_EMERGENCY_ON_MC	87000
#define BC250_FAN_EMERGENCY_OFF_MC	82000	/* the DPM's own release temperature */
#define BC250_FAN_EMERGENCY_HOLD_MS	10000u
#define BC250_FAN_BOOST_BUSY_PERMILLE	850u	/* the GPU busy share of the whole step */
#define BC250_FAN_BOOST_MHZ		1000u	/* and the GFX clock at or above the lab floor */
#define BC250_FAN_BOOST_POWER_MW	85000u	/* or the socket power: idle 41 to 56 W, the LLM arm 107 to 122 W */
#define BC250_FAN_BOOST_RISE_MS		3000u	/* or the guard temperature over this window */
#define BC250_FAN_BOOST_RISE_MC		3000	/* rising by this much, which is 1 C a second */
#define BC250_FAN_BOOST_ARM_MS		2000u	/* heavy for this long: the boost engages */
#define BC250_FAN_BOOST_LOAD_MAX_MS	4000u	/* and the heavy-time account stops here */
#define BC250_FAN_BOOST_STEP_MAX_MS	1500u	/* one step pays at most this much in, so one late step arms nothing */
#define BC250_FAN_BOOST_HOLD_MS		30000u	/* after the load: the boost holds at least this long */
#define BC250_FAN_LEASE_MIN_MS		5000u
#define BC250_FAN_LEASE_MAX_MS		300000u
#define BC250_FAN_LEASE_DEFAULT_MS	30000u
#define BC250_DPM_FLOOR_LEVEL	5u
#define BC250_DPM_THERMAL_FLOOR_LEVEL	3u
#define BC250_DPM_IDLE_LEVEL	0u
#define BC250_DPM_HOT_MC		BC250_CLOCK_HOT_MC	/* 87 C: one step down, no raise */
#define BC250_DPM_WARM_MC		BC250_DPM_HOT_MC
#define BC250_DPM_HOT_STEP_MS		500u	/* the default hot step: at most one step down per this */

static inline int bc250_board_supported(unsigned int provider_id)
{ return provider_id == 1u; }
#define BC250_DPM_DEFAULT_MAX_MHZ	1500u
#define BC250_BOARD_CPU_STOCK_CORES 6u
#define BC250_DPM_MODE_FIXED	0u	/* the lab point, BC250_DPM_FLOOR_LEVEL, set once at start: today's behaviour */
#define BC250_DPM_MODE_DPM	1u
#define BC250_DPM_DEFAULT_MODE	BC250_DPM_MODE_FIXED
#define BC250_DPM_TOP_LEVEL	(BC250_CLOCK_LEVELS - 1u)
#define BC250_DPM_ENCODED_TAG	0xD0000000u
#define BC250_DPM_UP_PERMILLE		900u	/* at or above: raise now */
#define BC250_DPM_TARGET_PERMILLE	800u	/* a raise aims at this share at the new clock */
#define BC250_DPM_DOWN_PERMILLE		650u	/* the average below this for DOWN_HOLD_MS: one step down */
#define BC250_DPM_DOWN_HOLD_MS		200u
#define BC250_DPM_RAMP_KNEE_MC		70000
#define BC250_DPM_RAMP_MIN_MS		1000u
#define BC250_DPM_RAMP_MAX_MS		4000u
#define BC250_DPM_CRITICAL_MC		90000
#define BC250_DPM_RELEASE_MC		82000	/* below: the thermal cap rises again (HOT_MC - 5 C) */
#define BC250_DPM_RELEASE_STEP_MS	1000u	/* one level per this, while below RELEASE_MC */
#define BC250_DPM_SOFT_DELTA_MC		4000u	/* HOT_MC - this = 83.0 C; 0: no soft release */
#define BC250_DPM_SOFT_STEP_MS		4000u
#define BC250_DPM_ZONE_DELTA_MC		1000u	/* HOT_MC - this = 86.0 C: the zone's step-down threshold; 0: no zone */
#define BC250_DPM_ZONE_STEP_MS		1500u
#define BC250_DPM_ZONE_LEAD_MS		15000u	/* 0: no lead, every threshold on the raw reading */
#define BC250_DPM_ZONE_SLOPE_MS		5000u	/* the window the slope is measured over; fixed, not a tune field */
#define BC250_DPM_ZONE_SLOPE_SLOTS	20u
#define BC250_DPM_ZONE_SLOPE_SLOT_MS	(BC250_DPM_ZONE_SLOPE_MS / BC250_DPM_ZONE_SLOPE_SLOTS)
#define BC250_DPM_ZONE_SLOPE_MAX_SPAN_MS (2u * BC250_DPM_ZONE_SLOPE_MS)
#define BC250_DPM_ZONE_SLOPE_MIN_MC	200	/* the fitted rise over the ring's span must reach this */
#define BC250_DPM_ZONE_LEAD_MAX_MC	2000	/* and the lead itself never exceeds this */
#define BC250_DPM_ZONE_WORK_PERMILLE	50u
#define BC250_DPM_ZONE_QUIET_MS		BC250_DPM_IDLE_HOLD_MS
#define BC250_DPM_IDLE_MHZ		500u	/* absent DpmIdleMHz: the idle point, 0 for no idle state */
#define BC250_DPM_IDLE_HOLD_MS		3000u
#define BC250_DPM_IDLE_BUSY_PERMILLE	2u	/* the window's mean busy share must stay under this */
#define BC250_DPM_IDLE_MIN_HOLD_MS	250u	/* ten governor ticks */
#define BC250_DPM_IDLE_MAX_HOLD_MS	60000u
#define BC250_DPM_IDLE_MAX_BUSY_PERMILLE 100u	/* 10 %: anything higher is not an idle GPU */
#define BC250_DPM_IDLE_EXIT_PERMILLE	500u
#define BC250_DPM_IDLE_LEAVE_PERMILLE	150u
#define BC250_DPM_IDLE_MIN_LEAVE_PERMILLE 10u	/* and always above the window's admitted entry mean */
#define BC250_DPM_IDLE_MAX_LEAVE_PERMILLE 400u	/* and always under BC250_DPM_IDLE_EXIT_PERMILLE */
#define BC250_DPM_MAX_DT_MS		1000u	/* a longer tick (a stall, a resume) counts as this */
#define BC250_DPM_CAP_MS_MAX		0x7FFFFFFFu	/* where the time since the last cap change saturates */
#define BC250_DPM_HW_SAMPLE_US		1000u
#define BC250_DPM_HW_MIN_SAMPLES	8u
#define BC250_DPM_TUNE_MIN_PERMILLE	100u
#define BC250_DPM_TUNE_MAX_PERMILLE	1000u
#define BC250_DPM_TUNE_MIN_HOLD_MS	100u	/* at most one lowering per four ticks of the 25 ms governor */
#define BC250_DPM_TUNE_MAX_HOLD_MS	5000u
#define BC250_DPM_TUNE_MIN_HOT_STEP_MS	250u	/* ten governor ticks: the part's thermal response is slower still */
#define BC250_DPM_TUNE_MAX_HOT_STEP_MS	10000u
#define BC250_DPM_TUNE_MIN_SOFT_DELTA_MC 500u
#define BC250_DPM_TUNE_MAX_SOFT_DELTA_MC 4500u
#define BC250_DPM_TUNE_MIN_SOFT_STEP_MS	2000u	/* a soft raise never follows a thermal change by less than this */
#define BC250_DPM_TUNE_MAX_SOFT_STEP_MS	30000u
#define BC250_DPM_TUNE_MIN_ZONE_DELTA_MC 500u
#define BC250_DPM_TUNE_MAX_ZONE_DELTA_MC (BC250_DPM_TUNE_MAX_SOFT_DELTA_MC - 500u)
#define BC250_DPM_TUNE_MIN_ZONE_STEP_MS	250u
#define BC250_DPM_TUNE_MAX_ZONE_STEP_MS	30000u
#define BC250_DPM_TUNE_MAX_ZONE_LEAD_MS	60000u
#define BC250_DPM_CURVE_TRIAL_MS	25000u	/* the release default: the reference Control Center's own 25 s */
#define BC250_DPM_CURVE_TRIAL_MIN_MS	10000u
#define BC250_DPM_CURVE_TRIAL_MAX_MS	180000u	/* the three-minute lab bound */
#define BC250_DPM_CURVE_KEEP_MIN_MS	100u
#define BC250_DPM_SESSION_CLEAR_MS	10000u

#endif
