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
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE && g.thermal_events == 0 && g.thermal_cap == 10);
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

	/* A new episode after a cool spell clamps at once again, at exactly the limit. */
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
	printf("dpm policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
