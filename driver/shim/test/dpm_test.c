/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * DPM policy host test (driver/shim/bc250_dpm.c, bc250_clock.c's table; docs/design/dpm.md):
 * the operating-point table against its anchors, the SMU message allowlist, the settings and
 * boot guard, the governor's load steps, hysteresis and thermal clamps, and the session marker.
 * Host-side only. Build and run: driver/shim/test/run_dpm.ps1.
 */
#include <stdio.h>
#include <string.h>
#include "bc250_dpm.h"
#include "smu_v11_8_ppsmc.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* ---- the table ---------------------------------------------------------------------------------- */

/* The anchors of bc250_clock.h, in microvolts: the table's mV is this line rounded up to a whole mV. */
static unsigned int anchor_uv(unsigned int mhz)
{
	if (mhz <= 1500u) return 820000u + (918750u - 820000u) * (mhz - 1000u) / 500u;
	return 918750u + (1000000u - 918750u) * (mhz - 1500u) / 500u;
}

static void test_table(void)
{
	unsigned int i, mhz;
	for (i = 0; i < BC250_CLOCK_LEVELS; i++) {
		const struct bc250_clock_point *p = &bc250_clock_points[i];
		unsigned int want_mv = (anchor_uv(p->mhz) + 999u) / 1000u;
		CHECK(p->mhz == BC250_CLOCK_FLOOR_MHZ + i * BC250_CLOCK_STEP_MHZ);
		CHECK(p->mv == want_mv);
		CHECK(p->vid == bc250_clock_vid(p->mv));
		/* Truncating encoding: the voltage the VID stands for is never below the table's. */
		CHECK(bc250_clock_vid_uv(p->vid) >= p->mv * 1000u);
		CHECK(bc250_clock_vid_uv(p->vid) < p->mv * 1000u + 6250u);
		CHECK(p->mv >= BC250_CLOCK_FLOOR_MV && p->mv <= BC250_CLOCK_CEILING_MV);
		/* AMD's overdrive range (cyan_skillfish_ppt.c) contains every point. */
		CHECK(p->mhz >= 1000u && p->mhz <= 2000u && p->mv >= 700u && p->mv <= 1129u);
		if (i) {
			CHECK(p->mv > bc250_clock_points[i - 1].mv);
			CHECK(p->vid < bc250_clock_points[i - 1].vid);
		}
		CHECK(bc250_dpm_level_of(p->mhz) == (int)i);
		CHECK(bc250_dpm_level_mhz(i) == p->mhz && bc250_dpm_level_mv(i) == p->mv);
		CHECK(bc250_clock_min_mv(p->mhz) == p->mv);
		CHECK(bc250_clock_point_allowed(p->mhz, p->mv));
		CHECK(bc250_clock_point_allowed(p->mhz, BC250_CLOCK_CEILING_MV));
		CHECK(!bc250_clock_point_allowed(p->mhz, p->mv - 1u));
		CHECK(!bc250_clock_point_allowed(p->mhz, BC250_CLOCK_CEILING_MV + 1u));
	}
	/* The measured anchors, by value: lab point VID 116 (facts M22), ceiling 1000 mV = VID 88. */
	CHECK(bc250_clock_points[0].mhz == 1000u && bc250_clock_points[0].mv == 820u && bc250_clock_points[0].vid == 116u);
	CHECK(bc250_clock_points[5].mhz == 1500u && bc250_clock_vid_uv(bc250_clock_points[5].vid) >= 918750u);
	CHECK(bc250_clock_points[10].mhz == 2000u && bc250_clock_points[10].mv == 1000u && bc250_clock_points[10].vid == 88u);
	CHECK(bc250_clock_vid(918u) == 101u && bc250_clock_vid_uv(101u) == 918750u);
	for (mhz = 0; mhz <= 2600u; mhz += 25u) {
		int on = mhz >= 1000u && mhz <= 2000u && mhz % 100u == 0;
		CHECK((bc250_clock_min_mv(mhz) != 0) == on);
		CHECK((bc250_dpm_level_of(mhz) >= 0) == on);
		if (!on) CHECK(!bc250_clock_point_allowed(mhz, 950u));
	}
	CHECK(bc250_dpm_level_mhz(BC250_DPM_TOP_LEVEL) == 2000u && bc250_dpm_level_mhz(99u) == 2000u);
}

static void test_allowlist(void)
{
	unsigned int m, allowed = 0;
	for (m = 0; m < 0x100u; m++) if (bc250_clock_message_allowed(m)) allowed++;
	CHECK(allowed == 5u);
	CHECK(bc250_clock_message_allowed(PPSMC_MSG_GetSmuVersion));
	CHECK(bc250_clock_message_allowed(PPSMC_MSG_RequestGfxclk));
	CHECK(bc250_clock_message_allowed(PPSMC_MSG_ForceGfxVid));
	CHECK(bc250_clock_message_allowed(PPSMC_MSG_GetGfxFrequency));
	CHECK(bc250_clock_message_allowed(PPSMC_MSG_GetGfxVid));
	/* Not sent by this driver: the firmware's own voltage choice, frequency forcing, tables, resets. */
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_UnforceGfxVid));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_ForceGfxFreq));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_UnForceGfxFreq));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_TransferTableSmu2Dram));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_SetDriverTableDramAddrLow));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_InitiateGcRsmuSoftReset));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_SetCoreEnableMask));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_RequestActiveWgp));
	CHECK(!bc250_clock_message_allowed(PPSMC_MSG_TestMessage));
}

/* ---- the settings and the boot guard ------------------------------------------------------------ */

static struct bc250_dpm_request req(void)
{
	struct bc250_dpm_request r;
	memset(&r, 0, sizeof(r));
	r.smu_online = 1;
	return r;
}

static void test_decide(void)
{
	struct bc250_dpm_request r;
	struct bc250_dpm_decision d;

	/* Absent: the default, which is fixed until the lab accepts DPM. */
	r = req();
	bc250_dpm_decide(&r, &d);
	CHECK(d.requested == BC250_DPM_MODE_FIXED);   /* BC250_DPM_DEFAULT_MODE, pinned until lab acceptance */
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_NOT_REQUESTED && d.max_level == 0);
	CHECK(!d.force_fixed && !d.mark_pending && !d.clear_pending && !d.clear_session && !d.encoded);

	/* Fixed with stale marks: both go, nothing else is written. */
	r = req(); r.mode_present = 1; r.mode = 0; r.pending_present = 1; r.pending = bc250_dpm_encode(2000); r.session_present = 1;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.clear_pending && d.clear_session && !d.force_fixed);

	/* DPM, first start without DpmMaxMHz: pending mark, the default 1500 ceiling. */
	r = req(); r.mode_present = 1; r.mode = 1;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.reason == BC250_DPM_REASON_NONE);
	CHECK(d.max_mhz == BC250_DPM_DEFAULT_MAX_MHZ && d.max_mhz == 1500u && d.max_level == 5u && d.encoded == 0xD00005DCu);
	CHECK(d.mark_pending && !d.confirmed && !d.force_fixed);
	/* A confirmation of the default covers the default only. */
	r.confirmed_present = 1; r.confirmed = bc250_dpm_encode(1500);
	bc250_dpm_decide(&r, &d);
	CHECK(d.confirmed && !d.mark_pending);

	/* The override up to the hard ceiling: 2000. */
	r = req(); r.mode_present = 1; r.mode = 1; r.max_present = 1; r.max_mhz = 2000;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.max_mhz == 2000u && d.max_level == 10u && d.encoded == 0xD00007D0u);
	CHECK(d.mark_pending && !d.confirmed && !d.force_fixed);

	/* Confirmed earlier with the same request: no mark. */
	r.confirmed_present = 1; r.confirmed = bc250_dpm_encode(2000);
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.confirmed && !d.mark_pending);
	/* ...but a confirmation of another ceiling does not cover this one. */
	r.confirmed = bc250_dpm_encode(1600);
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && !d.confirmed && d.mark_pending);

	/* The ceiling override: grid-rounded down, range-checked. */
	r = req(); r.mode_present = 1; r.mode = 1; r.max_present = 1; r.max_mhz = 1650;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.max_mhz == 1600u && d.max_level == 6u && d.encoded == bc250_dpm_encode(1600));
	r.max_mhz = 1000;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.max_level == 0);
	r.max_mhz = 999;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_INVALID_SETTING && !d.force_fixed && !d.mark_pending);
	r.max_mhz = 2001;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_INVALID_SETTING);
	r.max_mhz = 2300;   /* the community's patched-kernel range: not ours */
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_INVALID_SETTING);

	/* An unknown mode is kept as written and runs fixed. */
	r = req(); r.mode_present = 1; r.mode = 2; r.pending_present = 1;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_INVALID_SETTING);
	CHECK(!d.force_fixed && !d.clear_pending && !d.mark_pending);

	/* The guard: an unconfirmed earlier start, whatever it asked for. */
	r = req(); r.mode_present = 1; r.mode = 1; r.pending_present = 1; r.pending = bc250_dpm_encode(1500);
	r.confirmed_present = 1; r.confirmed = bc250_dpm_encode(2000);
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_UNCONFIRMED && d.force_fixed);
	CHECK(!d.mark_pending && d.max_level == 0);

	/* The guard: an earlier start ended above the floor, even a confirmed one. */
	r = req(); r.mode_present = 1; r.mode = 1; r.session_present = 1;
	r.confirmed_present = 1; r.confirmed = bc250_dpm_encode(2000);
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_UNCLEAN && d.force_fixed);

	/* No SMU owner: nothing runs and nothing is persisted, whatever the registry says. */
	r = req(); r.smu_online = 0; r.mode_present = 1; r.mode = 1; r.pending_present = 1; r.session_present = 1;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_NO_SMU);
	CHECK(!d.force_fixed && !d.clear_pending && !d.clear_session && !d.mark_pending);
}

/* ---- the governor ------------------------------------------------------------------------------- */

static struct bc250_dpm_input tick(unsigned int busy, int temp_c, unsigned int dt)
{
	struct bc250_dpm_input in;
	in.busy_permille = busy;
	in.temperature_mc = temp_c * 1000;
	in.temperature_valid = 1;
	in.dt_ms = dt;
	return in;
}

/* One tick applied without failure, the temperature in millidegrees (the thresholds' edges); returns the level. */
static unsigned int run_mc(struct bc250_dpm_governor *g, unsigned int busy, int temp_mc, unsigned int dt)
{
	struct bc250_dpm_input in = tick(busy, 0, dt);
	unsigned int level;
	in.temperature_mc = temp_mc;
	level = bc250_dpm_step(g, &in);
	CHECK(level <= g->max_level && level <= g->thermal_cap);
	bc250_dpm_commit(g, level);
	return level;
}

/* The same in whole degrees. */
static unsigned int run(struct bc250_dpm_governor *g, unsigned int busy, int temp_c, unsigned int dt)
{
	return run_mc(g, busy, temp_c * 1000, dt);
}

static void test_load_steps(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, level = 0;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(g.level == 0 && g.thermal_cap == 10u && g.max_level == 10u);
	/* Idle stays at the floor. */
	for (i = 0; i < 100; i++) CHECK(run(&g, 0, 60, 25) == 0);
	CHECK(g.raises == 0 && g.lowers == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* Saturated: 1000 -> 1300 (1250 needed) -> 1700 (1625) -> 2000 (2125, top). Three ticks. */
	CHECK(run(&g, 1000, 60, 25) == 3);
	CHECK(run(&g, 1000, 60, 25) == 7);
	CHECK(run(&g, 1000, 60, 25) == 10);
	CHECK(run(&g, 1000, 60, 25) == 10);
	CHECK(g.raises == 3);

	/* The band holds: 650..899 at 2000 MHz neither raises nor lowers. */
	for (i = 0; i < 200; i++) CHECK(run(&g, 650 + (i * 7) % 250, 60, 25) == 10);

	/* 90 % at 1500 MHz: demand 1687 -> 1700. The observed game at 1000 MHz (91.5 %, ETW): 1144 -> 1200. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 5;
	CHECK(run(&g, 900, 60, 25) == 7);
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run(&g, 915, 60, 25) == 2);
	/* Just under UP: no raise. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 4; g.avg_permille = 899;
	CHECK(run(&g, 899, 60, 25) == 4);

	/* Down: only after DOWN_HOLD_MS with the average below DOWN, one step at a time. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 10;
	for (i = 0; i < 7; i++) CHECK(run(&g, 0, 60, 25) == 10);   /* average falls, hold accumulates */
	level = run(&g, 0, 60, 25);
	CHECK(level == 9);
	/* From the top to the floor at idle: ten steps, each after a full hold. */
	for (i = 0; i < 400 && level; i++) level = run(&g, 0, 60, 25);
	CHECK(level == 0 && g.lowers == 10);
	CHECK(i >= 9u * (BC250_DPM_DOWN_HOLD_MS / 25u) - 1u);

	/* A single idle tick in a busy stream does not lower (the average, not the tick, decides). */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 8;
	for (i = 0; i < 40; i++) run(&g, 850, 60, 25);
	CHECK(run(&g, 0, 60, 25) == 8);
	CHECK(run(&g, 850, 60, 25) == 8);

	/* A long tick (resume, stall) counts as MAX_DT_MS, not as minutes of hold. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 6; g.avg_permille = 0;
	CHECK(run(&g, 0, 60, 3600000u) == 5);
	CHECK(g.down_ms == 0);

	/* Busy above 1000 permille is clamped, not trusted. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run(&g, 5000, 60, 25) == 3);
}

/* A workload that needs a fixed amount of work per second: busy = demand / clock, saturated at 1. The
 * governor must settle, and must not oscillate between two levels, for every demand on a fine grid. */
static void test_no_oscillation(void)
{
	unsigned int demand, i;
	for (demand = 100; demand <= 2400; demand += 10) {
		struct bc250_dpm_governor g;
		unsigned int changes = 0, last = 0, level = 0;
		bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
		for (i = 0; i < 2000; i++) {
			unsigned int mhz = bc250_dpm_level_mhz(g.level);
			unsigned int busy = demand >= mhz ? 1000u : demand * 1000u / mhz;
			level = run(&g, busy, 60, 25);
			if (i >= 1000 && level != last) changes++;
			last = level;
		}
		CHECK(changes == 0);
		/* Settled where the load fits: not saturated below the top, not idling far above it. */
		{
			unsigned int mhz = bc250_dpm_level_mhz(level);
			unsigned int busy = demand >= mhz ? 1000u : demand * 1000u / mhz;
			if (level < BC250_DPM_TOP_LEVEL) CHECK(busy < BC250_DPM_UP_PERMILLE);
			if (level > 0) CHECK(busy >= BC250_DPM_DOWN_PERMILLE - 1u);
		}
	}
}

/* The limits: hot at 87 C (owner decision 2026-10-01; 85 C before), the cap released 5 C lower, the floor at
 * 90 C. A constant CHECK would trip C4127 under /WX, so these fail the build instead. */
typedef char dpm_hot_is_87c[(BC250_DPM_HOT_MC == 87000 && BC250_CLOCK_HOT_MC == 87000) ? 1 : -1];
typedef char dpm_release_is_82c[(BC250_DPM_RELEASE_MC == 82000 && BC250_DPM_HOT_MC - BC250_DPM_RELEASE_MC == 5000) ? 1 : -1];
typedef char dpm_critical_is_90c[(BC250_DPM_CRITICAL_MC == 90000) ? 1 : -1];
/* The soft threshold (HOT - delta) lies strictly between RELEASE and HOT for every admitted delta. */
typedef char dpm_soft_threshold_inside[(BC250_DPM_TUNE_MIN_SOFT_DELTA_MC > 0u &&
					BC250_DPM_HOT_MC - (int)BC250_DPM_TUNE_MAX_SOFT_DELTA_MC > BC250_DPM_RELEASE_MC) ? 1 : -1];
/* The warm zone starts 2 C under HOT, above RELEASE (0.7.200). */
typedef char dpm_warm_is_85c[(BC250_DPM_WARM_MC == 85000 && BC250_DPM_HOT_MC - BC250_DPM_WARM_MC == 2000 &&
			      BC250_DPM_WARM_MC > BC250_DPM_RELEASE_MC) ? 1 : -1];
/* A soft raise is never faster than the hot step that undoes it. */
typedef char dpm_soft_slower_than_hot[(BC250_DPM_TUNE_MIN_SOFT_STEP_MS >= BC250_DPM_TUNE_MIN_HOT_STEP_MS) ? 1 : -1];

static void test_thermal(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_input in;
	unsigned int i, level;

	/* The limits themselves are checked at compile time (dpm_hot_is_87c and the two after it). */

	/* Under full load at the top, 85 C (the old limit) and 86.999 C do nothing thermal. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 10;
	CHECK(run(&g, 1000, 85, 25) == 10);
	CHECK(run_mc(&g, 1000, 86999, 25) == 10);
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE && g.thermal_events == 0 && g.thermal_cap == 10 && g.warm_holds == 0);
	/* 87 C at the top: one step down at once, under full load. */
	CHECK(run_mc(&g, 1000, 87000, 25) == 9);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.thermal_events == 1 && g.thermal_cap == 9);
	/* Still hot: another step every HOT_STEP_MS, none in between. */
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 1000, 88, 25) == 9);
	CHECK(run(&g, 1000, 88, 25) == 8);
	CHECK(g.thermal_events == 1);
	/* 82..86.999: the cap holds, no raise, the reason stays thermal. Both edges, then the middle. */
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 86999, 25) == 8);
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 82000, 25) == 8);
	for (i = 0; i < 100; i++) CHECK(run(&g, 1000, 84, 25) == 8);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.thermal_events == 1);
	/* Below 82 (81.999 is enough): one level per RELEASE_STEP_MS, back to the top. */
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++) CHECK(run_mc(&g, 1000, 81999, 25) == 8);
	CHECK(run_mc(&g, 1000, 81999, 25) == 9);
	for (i = 0; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 1000, 75, 25);
	CHECK(level == 10 && g.thermal_cap == 10);
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE);

	/* A new episode after a cool spell clamps at once again, at exactly the limit: the spell has held the cap for
	 * a hot step since its last raise (0.7.197: a re-entry inside the hot step does not step, test_reentry). */
	for (i = 0; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 1000, 75, 25) == 10);
	CHECK(run_mc(&g, 1000, 87000, 25) == 9 && g.thermal_events == 2);

	/* 90 C: the floor at once, whatever the load; recovery goes through release, step by step. 89.999 C is
	 * hot, not critical. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 10;
	CHECK(run_mc(&g, 1000, 89999, 25) == 9 && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	CHECK(run(&g, 1000, 90, 25) == 0);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_HARD && g.thermal_cap == 0);
	for (i = 0; i < 100; i++) CHECK(run(&g, 1000, 88, 25) == 0);    /* hot, not critical: cap stays 0 */
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 82000, 25) == 0);    /* not yet below release */
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 1000, 70, 25);
	CHECK(level == 10);

	/* 87 C at the floor: nothing to lower, no underflow, no raise. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 87000, 25) == 0 && g.thermal_cap == 0);
	for (i = 0; i < 100; i++) CHECK(run(&g, 1000, 89, 25) == 0);

	/* No reading is treated as critical. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 7;
	in = tick(1000, 60, 25); in.temperature_valid = 0;
	level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	CHECK(level == 0 && g.throttle == BC250_DPM_THROTTLE_SENSOR);

	/* Thermal wins over a max setting, the max setting over the load. */
	bc250_dpm_init(&g, 6);
	for (i = 0; i < 10; i++) level = run(&g, 1000, 60, 25);
	CHECK(level == 6 && g.throttle == BC250_DPM_THROTTLE_MAX_SETTING);
	CHECK(run(&g, 1000, 87, 25) == 5 && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 1000, 70, 25);
	CHECK(level == 6 && g.thermal_cap == 6);   /* the cap is released only up to the setting */
}

static void test_stable_and_failure(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_input in;
	unsigned int level;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 9;
	g.stable = 1;
	CHECK(run(&g, 1000, 60, 25) == 0 && g.throttle == BC250_DPM_THROTTLE_STABLE);
	CHECK(run(&g, 1000, 60, 25) == 0);
	g.stable = 0;
	CHECK(run(&g, 1000, 60, 25) == 3);

	/* A failed apply: the caller does not commit, the governor asks again from where it is. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	in = tick(1000, 60, 25);
	level = bc250_dpm_step(&g, &in);
	CHECK(level == 3 && g.level == 0);
	level = bc250_dpm_step(&g, &in);
	CHECK(level == 3 && g.level == 0 && g.raises == 0);
}

static void test_session(void)
{
	struct bc250_dpm_session s;
	unsigned int i;
	memset(&s, 0, sizeof(s));
	CHECK(bc250_dpm_session_step(&s, 0, 25) == BC250_DPM_SESSION_NONE);
	CHECK(bc250_dpm_session_step(&s, 3, 0) == BC250_DPM_SESSION_SET);
	/* Until the caller has made it durable, it keeps asking. */
	CHECK(bc250_dpm_session_step(&s, 3, 25) == BC250_DPM_SESSION_SET);
	s.marked = 1;
	CHECK(bc250_dpm_session_step(&s, 5, 25) == BC250_DPM_SESSION_NONE);
	/* At the floor: cleared only after SESSION_CLEAR_MS in a row. */
	for (i = 1; i < BC250_DPM_SESSION_CLEAR_MS / 25u; i++) CHECK(bc250_dpm_session_step(&s, 0, 25) == BC250_DPM_SESSION_NONE);
	CHECK(bc250_dpm_session_step(&s, 0, 25) == BC250_DPM_SESSION_CLEAR);
	/* A raise in between restarts the count. */
	memset(&s, 0, sizeof(s)); s.marked = 1;
	for (i = 0; i < 300; i++) bc250_dpm_session_step(&s, 0, 25);
	CHECK(bc250_dpm_session_step(&s, 1, 25) == BC250_DPM_SESSION_NONE && s.floor_ms == 0);
	for (i = 1; i < BC250_DPM_SESSION_CLEAR_MS / 25u; i++) CHECK(bc250_dpm_session_step(&s, 0, 25) == BC250_DPM_SESSION_NONE);
	/* A stalled tick does not clear it in one go. */
	memset(&s, 0, sizeof(s)); s.marked = 1;
	CHECK(bc250_dpm_session_step(&s, 0, 60000u) == BC250_DPM_SESSION_NONE);
}

/* ---- runtime tuning (0.7.185) ------------------------------------------------------------------- */

/* The defaults keep invariant 1 at the table's widest step (1100/1000) and the order, at compile time. */
typedef char dpm_default_lowering[(11u * (BC250_DPM_DOWN_PERMILLE + 1u) <= 10u * BC250_DPM_UP_PERMILLE) ? 1 : -1];
typedef char dpm_default_order[(BC250_DPM_DOWN_PERMILLE < BC250_DPM_TARGET_PERMILLE &&
				BC250_DPM_TARGET_PERMILLE < BC250_DPM_UP_PERMILLE) ? 1 : -1];
typedef char dpm_strict_form_admits_818[(818u * 1100u < 900u * 1000u) ? 1 : -1];
typedef char dpm_default_hold[(BC250_DPM_DOWN_HOLD_MS >= BC250_DPM_TUNE_MIN_HOLD_MS &&
			       BC250_DPM_DOWN_HOLD_MS <= BC250_DPM_TUNE_MAX_HOLD_MS) ? 1 : -1];

static struct bc250_dpm_tune tune(unsigned int up, unsigned int target, unsigned int down, unsigned int hold,
				  unsigned int floor_level)
{
	struct bc250_dpm_tune t;
	bc250_dpm_tune_default(&t);	/* the thermal timing at its defaults */
	t.up_permille = up;
	t.target_permille = target;
	t.down_permille = down;
	t.down_hold_ms = hold;
	t.floor_level = floor_level;
	return t;
}

/* Independent references for the two invariants. Invariant 1 straight from its inequality; invariant 2 by asking
 * bc250_dpm_step itself where a raise from each level goes, so the check is held to what the governor does. */
static int ref_lowering(const struct bc250_dpm_tune *t)
{
	unsigned int l;
	for (l = 1; l < BC250_CLOCK_LEVELS; l++)
		if ((t->down_permille + 1u) * bc250_clock_points[l].mhz > t->up_permille * bc250_clock_points[l - 1].mhz)
			return 0;
	return 1;
}

static int ref_raise(const struct bc250_dpm_tune *t)
{
	unsigned int l, busy;
	for (l = 0; l < BC250_DPM_TOP_LEVEL; l++)
		for (busy = t->up_permille; busy <= 1000u; busy++) {
			struct bc250_dpm_governor g;
			struct bc250_dpm_input in;
			unsigned int next;
			bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
			g.tune = *t;
			g.tune.floor_level = 0;
			g.level = l;
			in.busy_permille = busy; in.temperature_mc = 60000; in.temperature_valid = 1; in.dt_ms = 25;
			next = bc250_dpm_step(&g, &in);
			if (next <= l) return -1;      /* not a raise: the reference itself is wrong */
			if (t->down_permille * bc250_clock_points[next].mhz > busy * bc250_clock_points[l].mhz) return 0;
		}
	return 1;
}

static void test_tune_check(void)
{
	struct bc250_dpm_tune t, d;
	struct bc250_dpm_governor g;
	unsigned int up, target, down, compared = 0;

	bc250_dpm_tune_default(&d);
	CHECK(d.up_permille == 900u && d.target_permille == 800u && d.down_permille == 650u && d.down_hold_ms == 200u &&
	      d.floor_level == 0u);
	CHECK(d.hot_step_ms == 500u && d.soft_delta_mc == 0u && d.soft_step_ms == 3000u);
	CHECK(bc250_dpm_tune_check(&d, BC250_DPM_TOP_LEVEL) == BC250_DPM_TUNE_OK);
	CHECK(bc250_dpm_tune_check(&d, 0) == BC250_DPM_TUNE_OK);        /* a fixed-lab ceiling: no runtime floor is fine */
	CHECK(ref_lowering(&d) == 1 && ref_raise(&d) == 1);
	bc250_dpm_init(&g, 7);
	CHECK(memcmp(&g.tune, &d, sizeof(d)) == 0 && g.floor_ticks == 0);

	/* Ranges. */
	t = tune(1001, 800, 650, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_RANGE);
	t = tune(900, 800, 99, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_RANGE);
	t = tune(900, 0, 650, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_RANGE);
	t = tune(1000, 999, 100, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	/* Order: down < target < up, strictly. */
	t = tune(900, 650, 650, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_ORDER);
	t = tune(900, 900, 650, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_ORDER);
	t = tune(600, 800, 650, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_ORDER);
	/* Invariant 1 at its exact edge for up 900: 11 x 818 = 8998 <= 9000 admits down 817, 11 x 819 = 9009 does not
	 * admit 818. (target 899 keeps invariant 2 out of the way.) */
	t = tune(900, 899, 817, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = tune(900, 899, 818, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_LOWERING);
	/* The strict form without the +1 would admit 818 (dpm_strict_form_admits_818): at 1100 MHz a load that reads
	 * 818 permille (the work of 899.8 to 900.9 permille at 1000 MHz) can hold the average at 817, one under it, and
	 * lower; at 1000 MHz the same work reads 899 or 900, and 900 is up. */
	/* Invariant 2: a target just above down lets a raise land below down. */
	t = tune(370, 336, 335, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_RAISE);
	CHECK(ref_lowering(&t) == 1 && ref_raise(&t) == 0);
	/* The A/B tune of the 0.7.185 lab plan. */
	t = tune(750, 650, 500, 200, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	/* The hold. */
	t = tune(900, 800, 650, 99, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_HOLD);
	t = tune(900, 800, 650, 100, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = tune(900, 800, 650, 5000, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = tune(900, 800, 650, 5001, 0); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_HOLD);
	/* The floor: a level at or below the start's ceiling. */
	t = tune(900, 800, 650, 200, 10); CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	CHECK(bc250_dpm_tune_check(&t, 9) == BC250_DPM_TUNE_FLOOR);
	CHECK(bc250_dpm_tune_check(&t, 0) == BC250_DPM_TUNE_FLOOR);
	t = tune(900, 800, 650, 200, 11); CHECK(bc250_dpm_tune_check(&t, 99) == BC250_DPM_TUNE_FLOOR);
	t = tune(900, 800, 650, 200, 6); CHECK(bc250_dpm_tune_check(&t, 6) == BC250_DPM_TUNE_OK);
	/* The thermal timing (0.7.197): each field at both edges and one past each, the soft delta's 0 (off) admitted. */
	t = d; t.hot_step_ms = 249; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.hot_step_ms = 250; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = d; t.hot_step_ms = 10000; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = d; t.hot_step_ms = 10001; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.hot_step_ms = 0; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 0; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 1; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 499; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 500; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 4500; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 4501; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 0x80000000u; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_step_ms = 1999; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_step_ms = 2000; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = d; t.soft_step_ms = 30000; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_OK);
	t = d; t.soft_step_ms = 30001; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	/* The soft step is checked while the release is off, so that turning it on later is the delta alone. */
	t = d; t.soft_delta_mc = 0; t.soft_step_ms = 0; CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_THERMAL);
	/* The soft threshold stays strictly inside RELEASE..HOT at both edges of the delta (compile time below). */

	/* set_tune: a refused tune leaves the governor's as it was, an admitted one replaces it. */
	bc250_dpm_init(&g, 6);
	t = tune(750, 650, 500, 300, 7);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && memcmp(&g.tune, &d, sizeof(d)) == 0);
	t.floor_level = 6;
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK && memcmp(&g.tune, &t, sizeof(t)) == 0);

	/* The check against the references: invariant 1 on a fine grid, invariant 2 (and 1) on a coarser one. */
	for (up = 110; up <= 1000; up += 7)
		for (down = 100; down < up; down++) {
			t = tune(up, up - 1u, down, 200, 0);
			if (down + 1u >= up) continue;
			{
				enum bc250_dpm_tune_error e = bc250_dpm_tune_check(&t, 10);
				CHECK((e != BC250_DPM_TUNE_LOWERING) == ref_lowering(&t));
				compared++;
			}
		}
	for (up = 150; up <= 1000; up += 50)
		for (down = 100; down < up; down += 45)
			for (target = down + 1u; target < up; target += 40) {
				enum bc250_dpm_tune_error e;
				int r1, r2;
				t = tune(up, target, down, 200, 0);
				e = bc250_dpm_tune_check(&t, 10);
				r1 = ref_lowering(&t);
				r2 = r1 ? ref_raise(&t) : 1;
				CHECK(r2 >= 0);
				CHECK((e == BC250_DPM_TUNE_OK) == (r1 && r2 == 1));
				if (!r1) CHECK(e == BC250_DPM_TUNE_LOWERING);
				else if (r2 == 0) CHECK(e == BC250_DPM_TUNE_RAISE);
				compared++;
			}
	CHECK(compared > 50000u);
}

/* The settle-and-hold sweep of test_no_oscillation, for any tune; returns the level changes after the settle. */
static unsigned int sweep_changes(const struct bc250_dpm_tune *t, unsigned int max_level, unsigned int demand,
				  unsigned int *settled)
{
	struct bc250_dpm_governor g;
	unsigned int i, last = 0, changes = 0, level = 0;
	bc250_dpm_init(&g, max_level);
	g.tune = *t;
	for (i = 0; i < 1000; i++) {
		unsigned int mhz = bc250_dpm_level_mhz(g.level);
		unsigned int busy = demand >= mhz ? 1000u : demand * 1000u / mhz;
		level = run(&g, busy, 60, 25);
		if (i >= 700 && level != last) changes++;
		last = level;
	}
	*settled = level;
	return changes;
}

static void test_tune_no_oscillation(void)
{
	static const unsigned int holds[] = { 100, 200, 5000 };
	struct bc250_dpm_tune t;
	unsigned int up, target, down, demand, h, settled, admitted = 0, seen_cycle = 0;

	/* Every admitted tune on a grid, three holds, every constant demand: settles, no cycle, and where the load fits. */
	for (up = 150; up <= 1000; up += 50)
		for (down = 100; down < up; down += 50)
			for (target = down + 10u; target < up; target += 50) {
				t = tune(up, target, down, 200, 0);
				if (bc250_dpm_tune_check(&t, 10) != BC250_DPM_TUNE_OK) continue;
				admitted++;
				for (h = 0; h < sizeof(holds) / sizeof(holds[0]); h++) {
					t.down_hold_ms = holds[h];
					for (demand = 100; demand <= 2400; demand += 50) {
						unsigned int mhz, busy;
						CHECK(sweep_changes(&t, BC250_DPM_TOP_LEVEL, demand, &settled) == 0);
						mhz = bc250_dpm_level_mhz(settled);
						busy = demand >= mhz ? 1000u : demand * 1000u / mhz;
						if (settled < BC250_DPM_TOP_LEVEL) CHECK(busy < t.up_permille);
						if (settled > 0) CHECK(busy + 2u >= t.down_permille);
					}
				}
			}
	CHECK(admitted > 40u);

	/* The lab plan's tune, densely. */
	t = tune(750, 650, 500, 200, 0);
	for (demand = 100; demand <= 2400; demand += 10) CHECK(sweep_changes(&t, BC250_DPM_TOP_LEVEL, demand, &settled) == 0);

	/* Negative control: tunes that keep invariant 1 but break invariant 2 do cycle. The check refuses them. */
	t = tune(370, 336, 335, 200, 0);
	CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_RAISE);
	for (demand = 300; demand <= 450; demand += 5) if (sweep_changes(&t, BC250_DPM_TOP_LEVEL, demand, &settled)) seen_cycle++;
	CHECK(seen_cycle > 0);
	seen_cycle = 0;
	t = tune(200, 181, 180, 100, 0);
	CHECK(bc250_dpm_tune_check(&t, 10) == BC250_DPM_TUNE_RAISE);
	for (demand = 180; demand <= 260; demand += 5) if (sweep_changes(&t, BC250_DPM_TOP_LEVEL, demand, &settled)) seen_cycle++;
	CHECK(seen_cycle > 0);
}

/* The runtime floor: lifts the load's want, never the limits. */
static void test_floor(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_tune t;
	struct bc250_dpm_input in;
	unsigned int i, level;

	/* Floor 2000 at idle: the first tick goes there, want stays the load's (the floor). */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, 10);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 0, 60, 25) == 10);
	CHECK(g.want == 0 && g.throttle == BC250_DPM_THROTTLE_NONE && g.floor_ticks == 1 && g.raises == 1);
	for (i = 0; i < 400; i++) CHECK(run(&g, 0, 60, 25) == 10);
	CHECK(g.lowers == 0 && g.floor_ticks > 1);
	/* Full load at the floor: nothing to raise, nothing lifted. */
	i = g.floor_ticks;
	CHECK(run(&g, 1000, 60, 25) == 10 && g.floor_ticks == i);

	/* Thermal soft beats the floor: one step at 87 C, another every HOT_STEP_MS, back only below 82 C. */
	CHECK(run_mc(&g, 0, 87000, 25) == 9 && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 0, 88, 25) == 9);
	CHECK(run(&g, 0, 88, 25) == 8);
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 0, 82000, 25) == 8);
	for (i = 0; i < 4 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 0, 70, 25);
	CHECK(level == 10 && g.throttle == BC250_DPM_THROTTLE_NONE);
	/* Critical: the floor of the table at once, whatever the runtime floor. */
	CHECK(run(&g, 0, 90, 25) == 0 && g.throttle == BC250_DPM_THROTTLE_THERMAL_HARD);
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 0, 70, 25);
	CHECK(level == 10);
	/* No sensor: the table's floor. */
	in = tick(0, 60, 25); in.temperature_valid = 0;
	level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	CHECK(level == 0 && g.throttle == BC250_DPM_THROTTLE_SENSOR);
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 0, 70, 25);
	CHECK(level == 10);
	/* SetStablePowerState pins the table's floor over the runtime floor. */
	g.stable = 1;
	CHECK(run(&g, 0, 60, 25) == 0 && g.throttle == BC250_DPM_THROTTLE_STABLE);
	g.stable = 0;
	CHECK(run(&g, 0, 60, 25) == 10);

	/* Floor off: from 2000 at idle one step per hold, as before 0.7.185 (the hold counted from here). */
	t.floor_level = 0;
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	g.down_ms = 0;
	for (i = 0; i < 7; i++) CHECK(run(&g, 0, 60, 25) == 10);
	CHECK(run(&g, 0, 60, 25) == 9);
	for (i = 0; i < 400 && level; i++) level = run(&g, 0, 60, 25);
	CHECK(level == 0);

	/* A floor in the middle: the load still raises above it, lowerings stop at it. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, 5);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 0, 60, 25) == 5);
	CHECK(run(&g, 1000, 60, 25) == 9);     /* 1500 x 1000 / 800 = 1875 -> 1900 */
	for (i = 0; i < 400; i++) level = run(&g, 0, 60, 25);
	CHECK(level == 5 && g.want <= 5);

	/* The max setting: a floor above it is refused; one written past the check is clamped, never above the ceiling. */
	bc250_dpm_init(&g, 6);
	t = tune(900, 800, 650, 200, 10);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && g.tune.floor_level == 0);
	t.floor_level = 6;
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 0, 60, 25) == 6 && g.throttle == BC250_DPM_THROTTLE_NONE);
	CHECK(run(&g, 1000, 60, 25) == 6 && g.throttle == BC250_DPM_THROTTLE_MAX_SETTING);
	g.tune.floor_level = 10;
	/* The limit alone would also hold it at 6, but name MAX_SETTING; the clamp keeps the floor a floor. */
	for (i = 0; i < 50; i++) CHECK(run(&g, 0, 60, 25) == 6 && g.throttle == BC250_DPM_THROTTLE_NONE);
	/* Thermal soft with the floor at the ceiling: below both. */
	CHECK(run(&g, 0, 87, 25) == 5 && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);

	/* Tuned thresholds act: up 750 raises where 900 would not; down 500 holds where 650 would lower. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run(&g, 800, 60, 25) == 0);
	t = tune(750, 650, 500, 200, 0);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 800, 60, 25) == 3);      /* 1000 x 800 / 650 = 1230 -> 1300 */
	for (i = 0; i < 100; i++) CHECK(run(&g, 600, 60, 25) == 3);
	t = tune(900, 800, 650, 200, 0);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	for (i = 0; i < 100; i++) level = run(&g, 600, 60, 25);
	CHECK(level < 3);
}

/* ---- the warm zone (0.7.200) --------------------------------------------------------------------- */

/* After session 344 (1500 MHz held while Tctl rose 83.5 -> 85.3 C): from 85 C up to 87 C no raise, the level holds;
 * lowerings and the thermal paths act as below it. */
static void test_warm(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_tune t;
	struct bc250_dpm_input in;
	unsigned int i;

	/* Full load at 1500 MHz: refused at 85.0 C and at 86.9 C, the cap untouched, no hot entry. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 5;
	CHECK(run_mc(&g, 1000, 85000, 25) == 5 && g.throttle == BC250_DPM_THROTTLE_THERMAL_WARM && g.warm_holds == 1);
	CHECK(g.want == 9 && g.raises == 0);		/* want stays the load's own answer */
	CHECK(run_mc(&g, 1000, 86900, 25) == 5 && g.warm_holds == 2);
	CHECK(run_mc(&g, 1000, 86999, 25) == 5 && g.warm_holds == 3);
	for (i = 0; i < 400; i++) CHECK(run_mc(&g, 1000, 86000, 25) == 5);
	CHECK(g.warm_holds == 403u && g.thermal_events == 0 && g.thermal_cap == 10 && g.raises == 0);
	/* 84.999 C and 84.9 C: the raise goes through, as before 0.7.200 (1500 x 1000 / 800 = 1875 -> 1900). */
	CHECK(run_mc(&g, 1000, 84999, 25) == 9 && g.throttle == BC250_DPM_THROTTLE_NONE && g.warm_holds == 403u);
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 5;
	CHECK(run_mc(&g, 1000, 84900, 25) == 9 && g.warm_holds == 0);
	/* A busy share in the band that asks for no raise is no hold. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 5; g.avg_permille = 800;
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 800, 86000, 25) == 5);
	CHECK(g.warm_holds == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* A lowering still happens in the band, after the same hold as at 60 C, one step at a time. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 8;
	for (i = 0; i < 7; i++) CHECK(run_mc(&g, 0, 85000, 25) == 8);
	CHECK(run_mc(&g, 0, 86999, 25) == 7 && g.lowers == 1 && g.warm_holds == 0);
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE);

	/* 87 C is HOT, not warm: one step down at once, as before. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 10;
	CHECK(run_mc(&g, 1000, 87000, 25) == 9 && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	CHECK(g.thermal_events == 1 && g.thermal_cap == 9 && g.warm_holds == 0);
	/* Back in the band at the cap: the cap holds (no release above 82 C) and so does the clock. The cap, not the zone,
	 * holds the level, so the reason stays thermal-soft and no hold is counted. */
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 85000, 25) == 9);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.warm_holds == 0 && g.thermal_cap == 9);
	/* Below the cap the zone is what holds the level. */
	g.level = 6;
	CHECK(run_mc(&g, 1000, 85000, 25) == 6 && g.throttle == BC250_DPM_THROTTLE_THERMAL_WARM && g.warm_holds == 1);
	/* The release below 82 C is unchanged: one level per RELEASE_STEP_MS. */
	g.level = 9;
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++) CHECK(run_mc(&g, 1000, 81999, 25) == 9);
	CHECK(run_mc(&g, 1000, 81999, 25) == 10 && g.thermal_cap == 10);

	/* The runtime floor is a raise too: refused in the band, taken below it. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 3;
	t = tune(900, 800, 650, 200, 10);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run_mc(&g, 0, 85500, 25) == 3 && g.throttle == BC250_DPM_THROTTLE_THERMAL_WARM && g.warm_holds == 1);
	CHECK(run_mc(&g, 0, 84000, 25) == 10 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* At the max setting there is nothing to refuse: the setting names the reason. */
	bc250_dpm_init(&g, 6); g.level = 6;
	CHECK(run_mc(&g, 1000, 86000, 25) == 6 && g.throttle == BC250_DPM_THROTTLE_MAX_SETTING && g.warm_holds == 0);

	/* SetStablePowerState and a missing sensor go to the floor, never a hold. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 5; g.stable = 1;
	CHECK(run_mc(&g, 1000, 86000, 25) == 0 && g.throttle == BC250_DPM_THROTTLE_STABLE && g.warm_holds == 0);
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = 5;
	in = tick(1000, 0, 25); in.temperature_mc = 86000; in.temperature_valid = 0;
	CHECK(bc250_dpm_step(&g, &in) == 0 && g.throttle == BC250_DPM_THROTTLE_SENSOR && g.warm_holds == 0);
}

/* ---- the thermal timing (0.7.197, BD-055) ------------------------------------------------------- */

/* A governor at the top under full load with a thermal tune; the tune must be admitted. */
static void init_thermal(struct bc250_dpm_governor *g, unsigned int hot_step, unsigned int soft_delta, unsigned int soft_step)
{
	struct bc250_dpm_tune t;
	bc250_dpm_init(g, BC250_DPM_TOP_LEVEL);
	bc250_dpm_tune_default(&t);
	t.hot_step_ms = hot_step;
	t.soft_delta_mc = soft_delta;
	t.soft_step_ms = soft_step;
	CHECK(bc250_dpm_set_tune(g, &t) == BC250_DPM_TUNE_OK);
	g->level = BC250_DPM_TOP_LEVEL;
}

/* A temperature hovering at the limit: in and out of the hot band every other tick. Before 0.7.197 each entry stepped
 * at once (20 steps in a second here); now steps are at least a hot step apart, as under a steady 87 C. */
static void test_reentry(void)
{
	static const unsigned int steps[] = { 250, 500, 2000 };
	struct bc250_dpm_governor g;
	unsigned int s, i, level = 0, last_change = 0, changes, min_gap;

	for (s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
		init_thermal(&g, steps[s], 0, BC250_DPM_SOFT_STEP_MS);
		changes = 0; min_gap = ~0u; last_change = 0;
		for (i = 1; i <= 8000u / 25u; i++) {
			unsigned int before = g.thermal_cap;
			level = run_mc(&g, 1000, (i & 1u) ? 87000 : 86900, 25);
			if (g.thermal_cap != before) {
				CHECK(g.thermal_cap + 1u == before);
				if (changes && i * 25u - last_change < min_gap) min_gap = i * 25u - last_change;
				last_change = i * 25u;
				changes++;
			}
		}
		/* The first entry (t = 0) steps at once, no change since the start; then one per hot step until the last
		 * tick (t = 7975 ms), bounded by the floor ten levels down. */
		CHECK(changes == (7975u / steps[s] + 1u < 10u ? 7975u / steps[s] + 1u : 10u));
		CHECK(min_gap >= steps[s]);
		CHECK(level == g.thermal_cap && g.thermal_events == 8000u / 25u / 2u);
	}

	/* A steady 87 C: the same spacing, from the entry. */
	init_thermal(&g, 2000, 0, BC250_DPM_SOFT_STEP_MS);
	CHECK(run(&g, 1000, 87, 25) == 9);
	for (i = 1; i < 2000u / 25u; i++) CHECK(run(&g, 1000, 87, 25) == 9);
	CHECK(run(&g, 1000, 87, 25) == 8);

	/* A re-entry after a short cool spell inside the hot step: the cap is clamped to the running clock (nothing
	 * lowered, no raise), and the step follows a hot step after the last change, not after the entry. */
	init_thermal(&g, 2000, 0, BC250_DPM_SOFT_STEP_MS);
	CHECK(run(&g, 1000, 87, 25) == 9);                              /* t = 0: step */
	for (i = 0; i < 1000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == 9);   /* out for 1 s */
	CHECK(run(&g, 1000, 87, 25) == 9 && g.thermal_cap == 9 && g.thermal_events == 2);
	for (i = 1; i < 1000u / 25u - 1u; i++) CHECK(run(&g, 1000, 87, 25) == 9);
	CHECK(run(&g, 1000, 87, 25) == 8);                              /* 2 s after the first step */

	/* The clamp to the running clock: a load below the cap is not raised past the clock it ran at on entry. */
	init_thermal(&g, 2000, 0, BC250_DPM_SOFT_STEP_MS);
	g.level = 6; g.cap_ms = 100;                                    /* a raise 100 ms ago */
	CHECK(run(&g, 1000, 87, 25) == 6 && g.thermal_cap == 6);       /* full load asks more; the clamp holds 6 */
}

static void test_soft_release(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, level;

	/* Off by default: anywhere in 82..86.999 the cap holds, as before. */
	init_thermal(&g, 500, 0, 3000);
	CHECK(run(&g, 1000, 87, 25) == 9);
	for (i = 0; i < 60000u / 25u; i++) CHECK(run_mc(&g, 1000, 82500, 25) == 9);
	CHECK(g.soft_releases == 0);

	/* Delta 1.5 C (85.5 C), step 3 s: held below for a whole step without a break, one level. */
	init_thermal(&g, 500, 1500, 3000);
	CHECK(run(&g, 1000, 87, 25) == 9);
	for (i = 1; i < 3000u / 25u; i++) CHECK(run_mc(&g, 1000, 85499, 25) == 9);
	/* 85.499 C is in the warm zone (0.7.200): the cap rises as before, the clock follows it only below 85 C. */
	CHECK(run_mc(&g, 1000, 85499, 25) == 9 && g.soft_releases == 1 && g.thermal_cap == 10);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_WARM && g.warm_holds == 1);
	CHECK(run_mc(&g, 1000, 84999, 25) == 10 && g.throttle == BC250_DPM_THROTTLE_NONE && g.warm_holds == 1);
	/* At the threshold itself: no raise. */
	init_thermal(&g, 500, 1500, 3000);
	CHECK(run(&g, 1000, 87, 25) == 9);
	for (i = 0; i < 30000u / 25u; i++) CHECK(run_mc(&g, 1000, 85500, 25) == 9);
	CHECK(g.soft_releases == 0 && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	/* A break restarts the hold: one tick at 85.6 C just before the step. */
	init_thermal(&g, 500, 1500, 3000);
	CHECK(run(&g, 1000, 87, 25) == 9);
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == 9);
	CHECK(run_mc(&g, 1000, 85600, 25) == 9);
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == 9);
	CHECK(run(&g, 1000, 85, 25) == 9 && g.soft_releases == 1 && g.thermal_cap == 10);
	CHECK(run(&g, 1000, 84, 25) == 10);
	/* A hot tick restarts it too, and it steps (cap_ms is past the hot step). */
	init_thermal(&g, 500, 1500, 3000);
	g.thermal_cap = 8; g.level = 8;
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == 8);
	CHECK(run(&g, 1000, 87, 25) == 7 && g.soft_releases == 0);
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == 7);
	CHECK(run(&g, 1000, 85, 25) == 7 && g.soft_releases == 1 && g.thermal_cap == 8);
	CHECK(run(&g, 1000, 84, 25) == 8);
	/* The soft path climbs one level per step, all the way up; below 82 C the fast path (1 s) takes over. */
	init_thermal(&g, 500, 1500, 3000);
	g.thermal_cap = 4; g.level = 4;
	for (i = 0; i < 6u * 3000u / 25u; i++) level = run(&g, 1000, 84, 25);
	CHECK(level == 10 && g.soft_releases == 6);
	g.thermal_cap = 4; g.level = 4; g.soft_ms = 0;
	for (i = 0; i < 6u * 1000u / 25u; i++) level = run(&g, 1000, 81, 25);
	CHECK(level == 10 && g.soft_releases == 6);
	/* The widest delta (82.5 C) and the narrowest (86.5 C). */
	init_thermal(&g, 500, 4500, 2000);
	g.thermal_cap = 8; g.level = 8;
	for (i = 0; i < 2000u / 25u; i++) level = run_mc(&g, 1000, 82499, 25);
	CHECK(level == 9);
	init_thermal(&g, 500, 500, 2000);
	g.thermal_cap = 8; g.level = 8;
	for (i = 0; i < 2000u / 25u; i++) level = run_mc(&g, 1000, 86499, 25);
	CHECK(level == 8 && g.thermal_cap == 9 && g.soft_releases == 1);	/* warm: the cap only */
	CHECK(run(&g, 1000, 84, 25) == 9);
	/* Critical goes to the floor; the soft release brings it back, a level per step, never past the setting. */
	bc250_dpm_init(&g, 6);
	{
		struct bc250_dpm_tune t;
		bc250_dpm_tune_default(&t); t.soft_delta_mc = 1500;
		CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	}
	CHECK(run(&g, 1000, 90, 25) == 0);
	for (i = 0; i < 20u * 3000u / 25u; i++) level = run(&g, 1000, 85, 25);
	CHECK(level == 0 && g.thermal_cap == 6 && g.soft_releases == 6);	/* 85 C is warm: the cap only */
	CHECK(run(&g, 1000, 84, 25) == 3);
	CHECK(run(&g, 1000, 84, 25) == 6);
	/* A missing sensor resets the hold as critical does. */
	init_thermal(&g, 500, 1500, 3000);
	g.thermal_cap = 8; g.level = 8;
	for (i = 1; i < 3000u / 25u; i++) run(&g, 1000, 85, 25);
	{
		struct bc250_dpm_input in = tick(1000, 60, 25);
		in.temperature_valid = 0;
		level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	}
	CHECK(level == 0 && g.soft_ms == 0 && g.soft_releases == 0);
}

/* A synthetic plant: the hot spot is a fast node (3 s, 20 C of the full-power rise) over a heat sink (60 s, 29.5 C)
 * at 40 C, the power is f x V^2 of the level times a scene factor, and the sensor carries +-0.4 C of noise. Its
 * constants are not the lab's: they put 2000 MHz above the limit (89.5 C) and 1900 MHz under it (85.5 C) at the base
 * scene, with a heavier scene (+12 % power, 20 s of every 60 s) that needs 1800 MHz (86.8 C), so that a scene change
 * pushes the hot spot past 87 C and leaves it in the 82..87 band afterwards (the shape of sessions 318, 320 and 321).
 * Returns the mean clock over minutes 2-10, the peak reading and the change counts. */
struct plant_result { unsigned int mean_mhz, peak_mc, changes, soft_releases, thermal_events, cap_changes, min_cap_gap_ms; };

static struct plant_result plant_run(unsigned int hot_step, unsigned int soft_delta, unsigned int soft_step)
{
	struct bc250_dpm_governor g;
	struct plant_result r;
	double sink = 40.0, die = 40.0;
	unsigned long long mhz_sum = 0;
	unsigned int i, n = 0, seed = 12345u, last, cap_change_ms = 0;
	const unsigned int ticks = 600000u / 25u;

	memset(&r, 0, sizeof(r));
	init_thermal(&g, hot_step, soft_delta, soft_step);
	last = g.level;
	r.min_cap_gap_ms = ~0u;
	for (i = 0; i < ticks; i++) {
		unsigned int cap_before = g.thermal_cap;
		double v = bc250_dpm_level_mv(g.level) / 1000.0;
		double p = bc250_dpm_level_mhz(g.level) / 2000.0 * v * v * ((i * 25u) % 60000u < 20000u ? 1.12 : 1.0);
		int sensor;
		seed = seed * 1103515245u + 12345u;
		sink += (40.0 + 29.5 * p - sink) * 0.025 / 60.0;
		die += (sink + 20.0 * p - die) * 0.025 / 3.0;
		sensor = (int)(die * 1000.0) + (int)((seed >> 16) % 801u) - 400;
		run_mc(&g, 1000, sensor, 25);
		if (g.level != last) { r.changes++; last = g.level; }
		if (g.thermal_cap != cap_before) {
			if (r.cap_changes && i * 25u - cap_change_ms < r.min_cap_gap_ms) r.min_cap_gap_ms = i * 25u - cap_change_ms;
			cap_change_ms = i * 25u;
			r.cap_changes++;
		}
		if (sensor > (int)r.peak_mc) r.peak_mc = (unsigned int)sensor;
		if (i >= 120000u / 25u) { mhz_sum += bc250_dpm_level_mhz(g.level); n++; }
	}
	r.mean_mhz = (unsigned int)(mhz_sum / n);
	r.soft_releases = g.soft_releases;
	r.thermal_events = g.thermal_events;
	return r;
}

static void plant_print(const char *name, const struct plant_result *r)
{
	printf("plant %-26s mean %u MHz, peak %u mC, %u level changes, %u cap changes (min gap %u ms), %u hot entries, "
	       "%u soft raises\n", name, r->mean_mhz, r->peak_mc, r->changes, r->cap_changes,
	       r->cap_changes > 1u ? r->min_cap_gap_ms : 0u, r->thermal_events, r->soft_releases);
}

static void test_plant(void)
{
	struct plant_result legacy = plant_run(500, 0, 3000), slow = plant_run(2000, 0, 3000),
			    soft = plant_run(500, 1500, 3000), both = plant_run(2000, 1500, 3000);
	plant_print("hot 500, soft off:", &legacy);
	plant_print("hot 2000, soft off:", &slow);
	plant_print("hot 500, soft 1.5 C/3 s:", &soft);
	plant_print("hot 2000, soft 1.5 C/3 s:", &both);
	/* The legacy rule latches: no soft raise, so the clock the first scene change left stays. */
	CHECK(legacy.soft_releases == 0 && legacy.mean_mhz <= 1820u);
	/* The proposed rule recovers between heavy scenes, without critical, within a degree of the limit, and with
	 * cap changes at least two seconds apart (the direction's "at most one cap change per 2 s"). */
	CHECK(both.soft_releases > 0 && both.mean_mhz >= legacy.mean_mhz + 50u);
	CHECK(both.peak_mc < 88000u && legacy.peak_mc < 88000u);
	CHECK(both.min_cap_gap_ms >= 2000u);
	CHECK(both.changes <= 30u * 10u);
	/* The 2 s hot step is what spaces them: with the soft release on a 500 ms hot step the cap churns more. */
	CHECK(soft.min_cap_gap_ms < 2000u && both.cap_changes < soft.cap_changes);
	CHECK(slow.min_cap_gap_ms >= 2000u);
}

/* The busy source: GRBM samples when there are enough, the submit accounting otherwise. */
static void test_busy_source(void)
{
	enum bc250_dpm_busy_source src = BC250_DPM_BUSY_GRBM;
	unsigned int s, a;

	CHECK(bc250_dpm_busy_permille(0, 0, 420, &src) == 420 && src == BC250_DPM_BUSY_SUBMIT);
	CHECK(bc250_dpm_busy_permille(BC250_DPM_HW_MIN_SAMPLES - 1u, 7, 1500, &src) == 1000 && src == BC250_DPM_BUSY_SUBMIT);
	CHECK(bc250_dpm_busy_permille(25, 0, 900, &src) == 0 && src == BC250_DPM_BUSY_GRBM);
	CHECK(bc250_dpm_busy_permille(25, 25, 0, &src) == 1000 && src == BC250_DPM_BUSY_GRBM);
	CHECK(bc250_dpm_busy_permille(25, 23, 0, &src) == 920);		/* 23 of 25 reaches UP */
	CHECK(bc250_dpm_busy_permille(25, 22, 0, &src) == 880);
	CHECK(bc250_dpm_busy_permille(3, 2, 0, &src) == 0 && src == BC250_DPM_BUSY_SUBMIT);	/* too few: no 667 */
	CHECK(bc250_dpm_busy_permille(24, 30, 0, &src) == 1000);	/* a late active sample: clamped */
	/* Every count from the minimum up: in range, monotonic in active, exact at the ends. */
	for (s = BC250_DPM_HW_MIN_SAMPLES; s <= 64u; s++) {
		unsigned int prev = 0;
		for (a = 0; a <= s; a++) {
			unsigned int p = bc250_dpm_busy_permille(s, a, 0, &src);
			CHECK(src == BC250_DPM_BUSY_GRBM && p <= 1000u && p >= prev);
			CHECK(a != 0 || p == 0);
			CHECK(a != s || p == 1000);
			prev = p;
		}
	}
}

int main(void)
{
	test_table();
	test_allowlist();
	test_decide();
	test_load_steps();
	test_no_oscillation();
	test_thermal();
	test_stable_and_failure();
	test_session();
	test_busy_source();
	test_tune_check();
	test_tune_no_oscillation();
	test_floor();
	test_warm();
	test_reentry();
	test_soft_release();
	test_plant();
	printf("dpm policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
