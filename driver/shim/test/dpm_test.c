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
#include "dpm_test_traces.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* A level counted from the lab floor, which is where the load lives: L(0) is 1000 MHz, L(10) the 2000 MHz
 * ceiling, the two thermal-only points of 0.7.205 are L(-1) (900 MHz) and L(-2) (800 MHz,
 * BC250_DPM_THERMAL_FLOOR_LEVEL), and L(-5) == 0 is the idle point of 0.7.207 (500 MHz,
 * BC250_DPM_IDLE_LEVEL). Up to 0.7.204 the lab floor was index 0 and these tests wrote the index
 * itself; L() keeps every expectation below written in the same numbers. */
#define L(n) ((unsigned int)((int)BC250_DPM_FLOOR_LEVEL + (n)))

/* A governor with the thermal rules of 0.7.212: the hot cap at 87 C alone, no soft zone, no lead, no soft release.
 * Every case written before 0.7.213 asserts that governor and starts from here, so that the hot cap, the release, the
 * warm zone, the ramp and the sub-floor keep being tested on their own; test_zone covers the new default, which is the
 * zone on (bc250_dpm_tune_default). The 3000 ms soft step is the 0.7.212 default of that field: the release is off, so
 * it only has to stay inside its range, but a case that turns the release on by hand gets the old timing. */
static void init_old(struct bc250_dpm_governor *g, unsigned int max_level)
{
	struct bc250_dpm_tune t;
	bc250_dpm_init(g, max_level);
	t = g->tune;
	bc250_dpm_tune_zone_off(&t);
	t.soft_step_ms = 3000u;
	CHECK(bc250_dpm_set_tune(g, &t) == BC250_DPM_TUNE_OK);
}

/* ---- the table ---------------------------------------------------------------------------------- */

/* The table's shape since 0.7.207: 16 levels, 500 MHz first (the idle point, index 0), the thermal floor at
 * index 3 (800 MHz) and the lab floor at index 5 (1000 MHz).
 * A constant CHECK would trip C4127 under /WX, so these fail the build instead. */
typedef char dpm_table_is_16_levels[(BC250_CLOCK_LEVELS == 16u && BC250_DPM_TOP_LEVEL == 15u &&
				     BC250_DPM_IDLE_LEVEL == 0u && BC250_DPM_THERMAL_FLOOR_LEVEL == 3u &&
				     BC250_DPM_FLOOR_LEVEL == 5u &&
				     BC250_CLOCK_MIN_MHZ == 500u && BC250_CLOCK_FLOOR_MHZ == 1000u) ? 1 : -1];

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
		/* From the lab floor up, the anchors' line rounded up. The two thermal-only points below it
		 * (0.7.205) keep the floor's own voltage: a lower clock at 820 mV, and no extrapolation of the
		 * line under its lowest anchor. */
		unsigned int want_mv = i < BC250_DPM_FLOOR_LEVEL ? BC250_CLOCK_FLOOR_MV
								: (anchor_uv(p->mhz) + 999u) / 1000u;
		CHECK(p->mhz == BC250_CLOCK_MIN_MHZ + i * BC250_CLOCK_STEP_MHZ);
		CHECK(p->mv == want_mv);
		CHECK(p->vid == bc250_clock_vid(p->mv));
		/* Truncating encoding: the voltage the VID stands for is never below the table's. */
		CHECK(bc250_clock_vid_uv(p->vid) >= p->mv * 1000u);
		CHECK(bc250_clock_vid_uv(p->vid) < p->mv * 1000u + 6250u);
		CHECK(p->mv >= BC250_CLOCK_FLOOR_MV && p->mv <= BC250_CLOCK_CEILING_MV);
		/* AMD's overdrive voltage range (cyan_skillfish_ppt.c) contains every point; its clock range starts
		 * at the lab floor, which the two thermal-only points are below on purpose. */
		CHECK(p->mhz >= BC250_CLOCK_MIN_MHZ && p->mhz <= 2000u && p->mv >= 700u && p->mv <= 1129u);
		if (i > BC250_DPM_FLOOR_LEVEL) {
			CHECK(p->mv > bc250_clock_points[i - 1].mv);
			CHECK(p->vid < bc250_clock_points[i - 1].vid);
		} else if (i) {
			/* 500 to 1000 MHz all share 820 mV and VID 116. */
			CHECK(p->mv == bc250_clock_points[i - 1].mv && p->vid == bc250_clock_points[i - 1].vid);
		}
		CHECK(bc250_dpm_level_of(p->mhz) == (int)i);
		CHECK(bc250_dpm_level_mhz(i) == p->mhz && bc250_dpm_level_mv(i) == p->mv);
		CHECK(bc250_clock_min_mv(p->mhz) == p->mv);
		CHECK(bc250_clock_point_allowed(p->mhz, p->mv));
		CHECK(bc250_clock_point_allowed(p->mhz, BC250_CLOCK_CEILING_MV));
		CHECK(!bc250_clock_point_allowed(p->mhz, BC250_CLOCK_CEILING_MV + 1u));
		/* The undervolt band of 0.7.210: from the lab floor up, a point may stand BC250_CURVE_UNDERVOLT_MV
		 * under the table's line but never under BC250_CLOCK_FLOOR_MV, and the floor itself and every point
		 * below it have nothing left to give. */
		CHECK(bc250_clock_floor_mv(p->mhz) ==
		      (i <= BC250_DPM_FLOOR_LEVEL ? BC250_CLOCK_FLOOR_MV
						  : (p->mv > BC250_CLOCK_FLOOR_MV + BC250_CURVE_UNDERVOLT_MV
						     ? p->mv - BC250_CURVE_UNDERVOLT_MV : BC250_CLOCK_FLOOR_MV)));
		CHECK(bc250_clock_point_allowed(p->mhz, bc250_clock_floor_mv(p->mhz)));
		CHECK(!bc250_clock_point_allowed(p->mhz, bc250_clock_floor_mv(p->mhz) - 1u));
		if (i <= BC250_DPM_FLOOR_LEVEL) CHECK(!bc250_clock_point_allowed(p->mhz, p->mv - 1u));
		else CHECK(bc250_clock_point_allowed(p->mhz, p->mv - 1u));
	}
	/* The shape of the table is checked at compile time (dpm_table_is_16_levels below). */
	CHECK(bc250_dpm_level_mhz(BC250_DPM_THERMAL_FLOOR_LEVEL) == 800u);
	CHECK(bc250_dpm_level_mv(BC250_DPM_THERMAL_FLOOR_LEVEL) == BC250_CLOCK_FLOOR_MV);
	CHECK(bc250_dpm_level_mhz(BC250_DPM_FLOOR_LEVEL) == BC250_CLOCK_FLOOR_MHZ);
	CHECK(bc250_dpm_level_mv(BC250_DPM_FLOOR_LEVEL) == BC250_CLOCK_FLOOR_MV);
	CHECK(bc250_dpm_level_of(500) == 0 && bc250_dpm_level_of(800) == 3 && bc250_dpm_level_of(1000) == 5);
	CHECK(bc250_dpm_level_mhz(BC250_DPM_IDLE_LEVEL) == BC250_DPM_IDLE_MHZ);
	CHECK(bc250_dpm_level_mv(BC250_DPM_IDLE_LEVEL) == BC250_CLOCK_FLOOR_MV);
	/* Nothing below 500 MHz, and no undervolt at any point under the lab floor. */
	CHECK(bc250_clock_min_mv(500) == 820u && bc250_clock_min_mv(800) == 820u && bc250_clock_min_mv(900) == 820u);
	CHECK(bc250_clock_min_mv(400) == 0u && bc250_dpm_level_of(400) == -1);
	CHECK(!bc250_clock_point_allowed(400, 820u) && !bc250_clock_point_allowed(500, 819u));
	/* The measured anchors, by value: lab point VID 116 (facts M22), ceiling 1000 mV = VID 88. */
	CHECK(bc250_clock_points[L(0)].mhz == 1000u && bc250_clock_points[L(0)].mv == 820u &&
	      bc250_clock_points[L(0)].vid == 116u);
	CHECK(bc250_clock_points[L(5)].mhz == 1500u && bc250_clock_vid_uv(bc250_clock_points[L(5)].vid) >= 918750u);
	CHECK(bc250_clock_points[L(10)].mhz == 2000u && bc250_clock_points[L(10)].mv == 1000u &&
	      bc250_clock_points[L(10)].vid == 88u);
	CHECK(bc250_clock_vid(918u) == 101u && bc250_clock_vid_uv(101u) == 918750u);
	for (mhz = 0; mhz <= 2600u; mhz += 25u) {
		int on = mhz >= BC250_CLOCK_MIN_MHZ && mhz <= 2000u && mhz % 100u == 0;
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

static void test_decide_closed_record(void);

static void test_decide(void)
{
	struct bc250_dpm_request r;
	struct bc250_dpm_decision d;

	/* Absent: the default, which is fixed until the lab accepts DPM. */
	r = req();
	bc250_dpm_decide(&r, &d);
	CHECK(d.requested == BC250_DPM_MODE_FIXED);   /* BC250_DPM_DEFAULT_MODE, pinned until lab acceptance */
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_NOT_REQUESTED && d.max_level == L(0));
	CHECK(!d.force_fixed && !d.mark_pending && !d.clear_pending && !d.clear_session && !d.encoded);

	/* Fixed with stale marks: both go, nothing else is written. */
	r = req(); r.mode_present = 1; r.mode = 0; r.pending_present = 1; r.pending = bc250_dpm_encode(2000); r.session_present = 1;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.clear_pending && d.clear_session && !d.force_fixed);

	/* DPM, first start without DpmMaxMHz: pending mark, the default 1500 ceiling. */
	r = req(); r.mode_present = 1; r.mode = 1;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.reason == BC250_DPM_REASON_NONE);
	CHECK(d.max_mhz == BC250_DPM_DEFAULT_MAX_MHZ && d.max_mhz == 1500u && d.max_level == L(5) && d.encoded == 0xD00005DCu);
	CHECK(d.mark_pending && !d.confirmed && !d.force_fixed);
	/* A confirmation of the default covers the default only. */
	r.confirmed_present = 1; r.confirmed = bc250_dpm_encode(1500);
	bc250_dpm_decide(&r, &d);
	CHECK(d.confirmed && !d.mark_pending);

	/* The override up to the hard ceiling: 2000. */
	r = req(); r.mode_present = 1; r.mode = 1; r.max_present = 1; r.max_mhz = 2000;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.max_mhz == 2000u && d.max_level == L(10) && d.encoded == 0xD00007D0u);
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
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.max_mhz == 1600u && d.max_level == L(6) && d.encoded == bc250_dpm_encode(1600));
	r.max_mhz = 1000;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.max_level == L(0));
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
	CHECK(!d.mark_pending && d.max_level == L(0));

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

	test_decide_closed_record();
}

/* The durable record of a fallback (0.7.208, BD-069): DpmClosedReason tells the release installer, one boot
 * later, that the driver wrote DpmMode 0 and the tester did not. force_fixed writes it, a start with a DpmMode
 * other than 0 deletes it, and a start with DpmMode 0 leaves it where it is. */
static void test_decide_closed_record(void)
{
	struct bc250_dpm_request r;
	struct bc250_dpm_decision d;

	/* Both fallbacks record their own reason, and neither clears the record it writes. */
	r = req(); r.mode_present = 1; r.mode = 1; r.pending_present = 1; r.pending = bc250_dpm_encode(1500);
	bc250_dpm_decide(&r, &d);
	CHECK(d.force_fixed && d.closed_reason == BC250_DPM_REASON_UNCONFIRMED && d.closed_reason == 3u && !d.clear_closed);
	r = req(); r.mode_present = 1; r.mode = 1; r.session_present = 1;
	bc250_dpm_decide(&r, &d);
	CHECK(d.force_fixed && d.closed_reason == BC250_DPM_REASON_UNCLEAN && d.closed_reason == 4u && !d.clear_closed);
	/* A fallback over an older record writes the new reason. */
	r.closed_present = 1; r.closed = BC250_DPM_REASON_UNCONFIRMED;
	bc250_dpm_decide(&r, &d);
	CHECK(d.force_fixed && d.closed_reason == BC250_DPM_REASON_UNCLEAN && !d.clear_closed);

	/* The next start of the fallback's own boot, and every start after it: DpmMode 0, reason NOT_REQUESTED
	 * over DpmLastReason, and the record untouched. This is the start that made the installer blind before
	 * the record existed (BD-069). */
	r = req(); r.mode_present = 1; r.mode = 0; r.closed_present = 1; r.closed = BC250_DPM_REASON_UNCLEAN;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_FIXED && d.reason == BC250_DPM_REASON_NOT_REQUESTED);
	CHECK(d.closed_reason == BC250_DPM_REASON_UNCLEAN && !d.clear_closed && !d.force_fixed);
	/* The default is the fixed mode, so an absent DpmMode keeps the record too. */
	r = req(); r.closed_present = 1; r.closed = BC250_DPM_REASON_SMU_ERROR;
	bc250_dpm_decide(&r, &d);
	CHECK(d.closed_reason == BC250_DPM_REASON_SMU_ERROR && !d.clear_closed);

	/* DpmMode 1 again (the tester, the control application or install.cmd -Repair): the record goes. */
	r = req(); r.mode_present = 1; r.mode = 1; r.closed_present = 1; r.closed = BC250_DPM_REASON_SMU_ERROR;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && d.clear_closed && d.closed_reason == 0u);
	/* Nothing to delete when there is no record. */
	r.closed_present = 0; r.closed = 0;
	bc250_dpm_decide(&r, &d);
	CHECK(d.mode == BC250_DPM_MODE_DPM && !d.clear_closed && d.closed_reason == 0u);
	/* An unknown mode is not the fixed mode either: the record describes a 0 that is gone. */
	r = req(); r.mode_present = 1; r.mode = 2; r.closed_present = 1; r.closed = BC250_DPM_REASON_UNCLEAN;
	bc250_dpm_decide(&r, &d);
	CHECK(d.reason == BC250_DPM_REASON_INVALID_SETTING && d.clear_closed && d.closed_reason == 0u);
	/* So is a DPM request with a ceiling out of range. */
	r = req(); r.mode_present = 1; r.mode = 1; r.max_present = 1; r.max_mhz = 2300;
	r.closed_present = 1; r.closed = BC250_DPM_REASON_UNCLEAN;
	bc250_dpm_decide(&r, &d);
	CHECK(d.reason == BC250_DPM_REASON_INVALID_SETTING && d.clear_closed && d.closed_reason == 0u);

	/* No SMU owner: this start writes nothing at all, so the record stays for the start that has one. */
	r = req(); r.smu_online = 0; r.mode_present = 1; r.mode = 1;
	r.closed_present = 1; r.closed = BC250_DPM_REASON_UNCLEAN;
	bc250_dpm_decide(&r, &d);
	CHECK(d.reason == BC250_DPM_REASON_NO_SMU && !d.clear_closed && d.closed_reason == BC250_DPM_REASON_UNCLEAN);
}

/* ---- the governor ------------------------------------------------------------------------------- */

static struct bc250_dpm_input tick(unsigned int busy, int temp_c, unsigned int dt)
{
	struct bc250_dpm_input in;
	in.busy_permille = busy;
	in.temperature_mc = temp_c * 1000;
	in.temperature_valid = 1;
	in.dt_ms = dt;
	in.ring_busy = 0;		/* the ring is empty: only the idle state of 0.7.207 reads this */
	in.sdma_permille = 0;		/* the paging node is idle: the idle state alone reads this too */
	return in;
}

/* The lowest level the thermal cap may hold, as bc250_dpm.c's thermal_floor() computes it: the thermal floor,
 * or the lab floor once a point under it has been refused. The cap never goes below this, whatever the clock
 * was when the part went hot - the idle point is not a load level (0.7.207). Every tick below checks it, so a
 * cap of 500 MHz (which would pin a loaded GPU there) fails the gate instead of passing `level <= cap`. */
static unsigned int cap_bottom(const struct bc250_dpm_governor *g)
{
	return g->subfloor_ok ? BC250_DPM_THERMAL_FLOOR_LEVEL : BC250_DPM_FLOOR_LEVEL;
}

/* One tick applied without failure, the temperature in millidegrees (the thresholds' edges); returns the level. */
static unsigned int run_mc(struct bc250_dpm_governor *g, unsigned int busy, int temp_mc, unsigned int dt)
{
	struct bc250_dpm_input in = tick(busy, 0, dt);
	unsigned int level;
	in.temperature_mc = temp_mc;
	level = bc250_dpm_step(g, &in);
	CHECK(level <= g->max_level && level <= g->thermal_cap);
	CHECK(g->thermal_cap >= cap_bottom(g));
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
	CHECK(g.level == L(0) && g.thermal_cap == BC250_DPM_TOP_LEVEL && g.max_level == BC250_DPM_TOP_LEVEL);
	/* Idle stays at the floor. */
	for (i = 0; i < 100; i++) CHECK(run(&g, 0, 60, 25) == L(0));
	CHECK(g.raises == 0 && g.lowers == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* Saturated: 1000 -> 1300 (1250 needed) -> 1700 (1625) -> 2000 (2125, top). Three ticks. */
	CHECK(run(&g, 1000, 60, 25) == L(3));
	CHECK(run(&g, 1000, 60, 25) == L(7));
	CHECK(run(&g, 1000, 60, 25) == L(10));
	CHECK(run(&g, 1000, 60, 25) == L(10));
	CHECK(g.raises == 3);

	/* The band holds: 650..899 at 2000 MHz neither raises nor lowers. */
	for (i = 0; i < 200; i++) CHECK(run(&g, 650 + (i * 7) % 250, 60, 25) == L(10));

	/* 90 % at 1500 MHz: demand 1687 -> 1700. The observed game at 1000 MHz (91.5 %, ETW): 1144 -> 1200. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	CHECK(run(&g, 900, 60, 25) == L(7));
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run(&g, 915, 60, 25) == L(2));
	/* Just under UP: no raise. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(4); g.avg_permille = 899;
	CHECK(run(&g, 899, 60, 25) == L(4));

	/* Down: only after DOWN_HOLD_MS with the average below DOWN, one step at a time. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	for (i = 0; i < 7; i++) CHECK(run(&g, 0, 60, 25) == L(10));   /* average falls, hold accumulates */
	level = run(&g, 0, 60, 25);
	CHECK(level == L(9));
	/* From the top to the floor at idle: ten steps, each after a full hold. */
	for (i = 0; i < 400 && level > L(0); i++) level = run(&g, 0, 60, 25);
	CHECK(level == L(0) && g.lowers == 10);
	CHECK(i >= 9u * (BC250_DPM_DOWN_HOLD_MS / 25u) - 1u);

	/* A single idle tick in a busy stream does not lower (the average, not the tick, decides). */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(8);
	for (i = 0; i < 40; i++) run(&g, 850, 60, 25);
	CHECK(run(&g, 0, 60, 25) == L(8));
	CHECK(run(&g, 850, 60, 25) == L(8));

	/* A long tick (resume, stall) counts as MAX_DT_MS, not as minutes of hold. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(6); g.avg_permille = 0;
	CHECK(run(&g, 0, 60, 3600000u) == L(5));
	CHECK(g.down_ms == 0);

	/* Busy above 1000 permille is clamped, not trusted. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run(&g, 5000, 60, 25) == L(3));
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
			if (level > L(0)) CHECK(busy >= BC250_DPM_DOWN_PERMILLE - 1u);
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
/* The warm zone starts at HOT (0.7.204, owner: "próg na 87"; 2 C under HOT in 0.7.200-203), never above it, above
 * RELEASE and above the ramp's knee, and above every admitted soft threshold. */
typedef char dpm_warm_is_87c[(BC250_DPM_WARM_MC == 87000 && BC250_DPM_WARM_MC <= BC250_DPM_HOT_MC &&
			      BC250_DPM_WARM_MC > BC250_DPM_RELEASE_MC && BC250_DPM_WARM_MC > BC250_DPM_RAMP_KNEE_MC &&
			      BC250_DPM_HOT_MC - (int)BC250_DPM_TUNE_MIN_SOFT_DELTA_MC < BC250_DPM_WARM_MC) ? 1 : -1];
/* A soft raise is never faster than the hot step that undoes it. */
typedef char dpm_soft_slower_than_hot[(BC250_DPM_TUNE_MIN_SOFT_STEP_MS >= BC250_DPM_TUNE_MIN_HOT_STEP_MS) ? 1 : -1];

static void test_thermal(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_input in;
	unsigned int i, level;

	/* The limits themselves are checked at compile time (dpm_hot_is_87c and the two after it). */

	/* Under full load at the top, 85 C (the old limit) and 86.999 C do nothing thermal. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	CHECK(run(&g, 1000, 85, 25) == L(10));
	CHECK(run_mc(&g, 1000, 86999, 25) == L(10));
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE && g.thermal_events == 0 && g.thermal_cap == L(10) && g.warm_holds == 0);
	/* 87 C at the top: one step down at once, under full load. */
	CHECK(run_mc(&g, 1000, 87000, 25) == L(9));
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.thermal_events == 1 && g.thermal_cap == L(9));
	/* Still hot: another step every HOT_STEP_MS, none in between. */
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 1000, 88, 25) == L(9));
	CHECK(run(&g, 1000, 88, 25) == L(8));
	CHECK(g.thermal_events == 1);
	/* 82..86.999: the cap holds, no raise, the reason stays thermal. Both edges, then the middle. */
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 86999, 25) == L(8));
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 82000, 25) == L(8));
	for (i = 0; i < 100; i++) CHECK(run(&g, 1000, 84, 25) == L(8));
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.thermal_events == 1);
	/* Below 82 (81.999 is enough): one level per RELEASE_STEP_MS, back to the top. The last level at 65 C, under the
	 * thermal ramp's knee (test_ramp covers the clock that follows a released cap above it). */
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++) CHECK(run_mc(&g, 1000, 81999, 25) == L(8));
	CHECK(run_mc(&g, 1000, 81999, 25) == L(9));
	for (i = 0; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 1000, 65, 25);
	CHECK(level == L(10) && g.thermal_cap == L(10));
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE);

	/* A new episode after a cool spell clamps at once again, at exactly the limit: the spell has held the cap for
	 * a hot step since its last raise (0.7.197: a re-entry inside the hot step does not step, test_reentry). */
	for (i = 0; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 1000, 65, 25) == L(10));
	CHECK(run_mc(&g, 1000, 87000, 25) == L(9) && g.thermal_events == 2);

	/* 90 C: the thermal floor at once (800 MHz since 0.7.205), whatever the load; recovery goes through
	 * release, step by step. 89.999 C is hot, not critical. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	CHECK(run_mc(&g, 1000, 89999, 25) == L(9) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	CHECK(run(&g, 1000, 90, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_HARD && g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL);
	/* Hot, not critical: the cap is at the bottom already and does not underflow. */
	for (i = 0; i < 100; i++) CHECK(run(&g, 1000, 88, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 82000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 1000, 70, 25);
	CHECK(level == L(10));

	/* 87 C at the lab floor: the hot step goes under it, one level per hot step, down to 800 MHz and no
	 * further (0.7.205; before it the cap stopped at 1000 MHz). */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(-1) && g.thermal_cap == L(-1));
	CHECK(bc250_dpm_level_mhz(L(-1)) == 900u && bc250_dpm_level_mv(L(-1)) == 820u);
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 1000, 89, 25) == L(-1));
	CHECK(run(&g, 1000, 89, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(bc250_dpm_level_mhz(BC250_DPM_THERMAL_FLOOR_LEVEL) == 800u);
	for (i = 0; i < 100; i++) CHECK(run(&g, 1000, 89, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(g.want == L(0));		/* the load never asks below the lab floor */

	/* No reading is treated as critical. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(7);
	in = tick(1000, 60, 25); in.temperature_valid = 0;
	level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	CHECK(level == L(0) && g.throttle == BC250_DPM_THROTTLE_SENSOR);

	/* Thermal wins over a max setting, the max setting over the load. */
	init_old(&g, L(6));
	for (i = 0; i < 10; i++) level = run(&g, 1000, 60, 25);
	CHECK(level == L(6) && g.throttle == BC250_DPM_THROTTLE_MAX_SETTING);
	CHECK(run(&g, 1000, 87, 25) == L(5) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 1000, 70, 25);
	CHECK(level == L(6) && g.thermal_cap == L(6));   /* the cap is released only up to the setting */
}

static void test_stable_and_failure(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_input in;
	unsigned int level;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(9);
	g.stable = 1;
	CHECK(run(&g, 1000, 60, 25) == L(0) && g.throttle == BC250_DPM_THROTTLE_STABLE);
	CHECK(run(&g, 1000, 60, 25) == L(0));
	g.stable = 0;
	CHECK(run(&g, 1000, 60, 25) == L(3));

	/* A failed apply: the caller does not commit, the governor asks again from where it is. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	in = tick(1000, 60, 25);
	level = bc250_dpm_step(&g, &in);
	CHECK(level == L(3) && g.level == L(0));
	level = bc250_dpm_step(&g, &in);
	CHECK(level == L(3) && g.level == L(0) && g.raises == 0);
}

static void test_session(void)
{
	struct bc250_dpm_session s;
	unsigned int i;
	memset(&s, 0, sizeof(s));
	CHECK(bc250_dpm_session_step(&s, L(0), 25) == BC250_DPM_SESSION_NONE);
	CHECK(bc250_dpm_session_step(&s, L(3), 0) == BC250_DPM_SESSION_SET);
	/* Until the caller has made it durable, it keeps asking. */
	CHECK(bc250_dpm_session_step(&s, L(3), 25) == BC250_DPM_SESSION_SET);
	s.marked = 1;
	CHECK(bc250_dpm_session_step(&s, L(5), 25) == BC250_DPM_SESSION_NONE);
	/* At the floor: cleared only after SESSION_CLEAR_MS in a row. */
	for (i = 1; i < BC250_DPM_SESSION_CLEAR_MS / 25u; i++) CHECK(bc250_dpm_session_step(&s, L(0), 25) == BC250_DPM_SESSION_NONE);
	CHECK(bc250_dpm_session_step(&s, L(0), 25) == BC250_DPM_SESSION_CLEAR);
	/* A raise in between restarts the count. */
	memset(&s, 0, sizeof(s)); s.marked = 1;
	for (i = 0; i < 300; i++) bc250_dpm_session_step(&s, L(0), 25);
	CHECK(bc250_dpm_session_step(&s, L(1), 25) == BC250_DPM_SESSION_NONE && s.floor_ms == 0);
	for (i = 1; i < BC250_DPM_SESSION_CLEAR_MS / 25u; i++) CHECK(bc250_dpm_session_step(&s, L(0), 25) == BC250_DPM_SESSION_NONE);
	/* A stalled tick does not clear it in one go. */
	memset(&s, 0, sizeof(s)); s.marked = 1;
	CHECK(bc250_dpm_session_step(&s, L(0), 60000u) == BC250_DPM_SESSION_NONE);
	/* The thermal-only levels under the lab floor (0.7.205) are not "above the floor": a start that ends at
	 * 900 or 800 MHz is no reason to make the next one fixed, so nothing is marked and a marker still clears. */
	memset(&s, 0, sizeof(s));
	CHECK(bc250_dpm_session_step(&s, L(-1), 25) == BC250_DPM_SESSION_NONE);
	CHECK(bc250_dpm_session_step(&s, BC250_DPM_THERMAL_FLOOR_LEVEL, 25) == BC250_DPM_SESSION_NONE);
	CHECK(!s.marked);
	s.marked = 1; s.floor_ms = 0;
	for (i = 1; i < BC250_DPM_SESSION_CLEAR_MS / 25u; i++)
		CHECK(bc250_dpm_session_step(&s, (i & 1u) ? L(-1) : BC250_DPM_THERMAL_FLOOR_LEVEL, 25) == BC250_DPM_SESSION_NONE);
	CHECK(bc250_dpm_session_step(&s, L(-1), 25) == BC250_DPM_SESSION_CLEAR);
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
	bc250_dpm_tune_default(&t);
	/* The thermal timing at its defaults, with the soft zone of 0.7.213 off: every case that builds a tune this way
	 * was written for the 0.7.212 thermal rules and asserts the clock they give. test_zone builds its own. */
	bc250_dpm_tune_zone_off(&t);
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
	for (l = BC250_DPM_FLOOR_LEVEL + 1u; l < BC250_CLOCK_LEVELS; l++)
		if ((t->down_permille + 1u) * bc250_clock_points[l].mhz > t->up_permille * bc250_clock_points[l - 1].mhz)
			return 0;
	return 1;
}

static int ref_raise(const struct bc250_dpm_tune *t)
{
	unsigned int l, busy;
	for (l = BC250_DPM_FLOOR_LEVEL; l < BC250_DPM_TOP_LEVEL; l++)
		for (busy = t->up_permille; busy <= 1000u; busy++) {
			struct bc250_dpm_governor g;
			struct bc250_dpm_input in;
			unsigned int next;
			bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
			g.tune = *t;
			g.tune.floor_level = L(0);
			g.level = l;
			in.busy_permille = busy; in.temperature_mc = 60000; in.temperature_valid = 1; in.dt_ms = 25;
			in.ring_busy = 0; in.sdma_permille = 0;
			next = bc250_dpm_step(&g, &in);
			if (next <= l) return -1;      /* not a raise: the reference itself is wrong */
			if (t->down_permille * bc250_clock_points[next].mhz > busy * bc250_clock_points[l].mhz) return 0;
		}
	return 1;
}

/* ---- the operator's V/F curve and its trial (0.7.210) ------------------------------------------- */

/* The default curve is the table's own line, so a start that nobody tuned behaves exactly as 0.7.207 did; a
 * candidate is checked whole, and a trial ends by itself. */
static void test_curve(void)
{
	struct bc250_clock_curve line, c;
	struct bc250_dpm_curve_state s;
	unsigned int i, level = 99u;

	bc250_clock_curve_default(&line);
	for (i = 0; i < BC250_CURVE_POINTS; i++)
		CHECK(line.mv[i] == bc250_clock_points[BC250_CURVE_FIRST_LEVEL + i].mv);
	CHECK(bc250_clock_curve_is_default(&line));
	CHECK(bc250_clock_curve_check(&line, &level) == BC250_CLOCK_CURVE_OK);
	/* Below the first level the curve has no say: the table's own voltage, as every point under the lab floor
	 * is the floor's 820 mV. Above the table the last point answers, as bc250_dpm_level_mhz does. */
	for (i = 0; i < BC250_CLOCK_LEVELS; i++)
		CHECK(bc250_clock_curve_mv(&line, i) == bc250_clock_points[i].mv);
	CHECK(bc250_clock_curve_mv(&line, 99u) == bc250_clock_points[BC250_CLOCK_LEVELS - 1].mv);
	CHECK(bc250_clock_curve_checksum(&line) == bc250_clock_curve_checksum(&line));

	/* A real undervolt: every editable point at its own floor. It passes, and it is not the default. */
	c = line;
	for (i = 0; i < BC250_CURVE_POINTS; i++)
		c.mv[i] = bc250_clock_floor_mv(bc250_dpm_level_mhz(BC250_CURVE_FIRST_LEVEL + i));
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_OK);
	CHECK(!bc250_clock_curve_is_default(&c));
	CHECK(bc250_clock_curve_checksum(&c) != bc250_clock_curve_checksum(&line));
	CHECK(c.mv[0] == BC250_CLOCK_FLOOR_MV);

	/* Every refusal, one at a time, with the level it happened at. */
	c = line; c.mv[0] = BC250_CLOCK_FLOOR_MV + 1u;
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_FLOOR && level == BC250_CURVE_FIRST_LEVEL);
	c = line; c.mv[0] = BC250_CLOCK_FLOOR_MV - 1u;
	CHECK(bc250_clock_curve_check(&c, &level) != BC250_CLOCK_CURVE_OK);
	c = line; c.mv[BC250_CURVE_POINTS - 1] = BC250_CLOCK_CEILING_MV + 1u;
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_RANGE &&
	      level == BC250_CLOCK_LEVELS - 1u);
	c = line; c.mv[BC250_CURVE_POINTS - 1] = BC250_CLOCK_FLOOR_MV - 1u;
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_RANGE);
	/* Deeper than the band admits at that clock. */
	c = line; c.mv[5] = bc250_clock_floor_mv(bc250_dpm_level_mhz(BC250_CURVE_FIRST_LEVEL + 5u)) - 1u;
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_DEPTH &&
	      level == BC250_CURVE_FIRST_LEVEL + 5u);
	/* A falling voltage with a rising clock: the one shape the governor's own transitions could not carry. */
	c = line; c.mv[3] = c.mv[2] - 1u;
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_ORDER &&
	      level == BC250_CURVE_FIRST_LEVEL + 3u);
	CHECK(bc250_clock_curve_check(NULL, &level) == BC250_CLOCK_CURVE_NULL);
	/* A flat step is legal: two neighbours at the same voltage is what the table itself does below the floor,
	 * and an operator who flattens the top of the line is not asking for anything new. */
	c = line; c.mv[BC250_CURVE_POINTS - 2] = c.mv[BC250_CURVE_POINTS - 1];
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_OK);
	/* The whole line at the lab point is not a curve, it is a 155 mV undervolt at 2000 MHz: the band refuses it
	 * at the first level where the line is further away than BC250_CURVE_UNDERVOLT_MV. */
	c = line;
	for (i = 1; i < BC250_CURVE_POINTS; i++) c.mv[i] = c.mv[0];
	CHECK(bc250_clock_curve_check(&c, &level) == BC250_CLOCK_CURVE_DEPTH);
	CHECK(level > BC250_CURVE_FIRST_LEVEL && level <= BC250_DPM_TOP_LEVEL);

	/* The trial's whole life. Nothing here writes to disk: that is the caller's, and only on a keep. */
	bc250_dpm_curve_init(&s, NULL);
	CHECK(bc250_clock_curve_is_default(&s.stored) && bc250_clock_curve_is_default(&s.active));
	CHECK(!s.trial && !s.apply && s.serial == 0 && bc250_dpm_curve_remaining_ms(&s) == 0);
	CHECK(bc250_dpm_curve_level_mv(&s, BC250_CURVE_FIRST_LEVEL) == BC250_CLOCK_FLOOR_MV);
	CHECK(bc250_dpm_curve_level_vid(&s, BC250_CURVE_FIRST_LEVEL) == bc250_clock_vid(BC250_CLOCK_FLOOR_MV));
	/* Nothing on trial: a keep and a cancel both have nothing to do, and neither invents a change. 0 means
	 * "no trial", which the caller shows differently from -1, "not applied yet" (0.7.211). */
	CHECK(bc250_dpm_curve_keep(&s) == 0 && !bc250_dpm_curve_cancel(&s) && s.serial == 0);
	CHECK(!bc250_dpm_curve_reset(&s) && s.serial == 0);

	/* A candidate: active at once, a window of its own, and one forced re-apply owed to the governor. */
	c = line;
	for (i = 0; i < BC250_CURVE_POINTS; i++)
		c.mv[i] = bc250_clock_floor_mv(bc250_dpm_level_mhz(BC250_CURVE_FIRST_LEVEL + i));
	CHECK(bc250_dpm_curve_set(&s, &c, 20000u, &level) == BC250_CLOCK_CURVE_OK);
	CHECK(s.trial && s.serial == 1 && s.sets == 1 && s.trial_ms == 20000u);
	CHECK(bc250_dpm_curve_remaining_ms(&s) == 20000u);
	CHECK(bc250_dpm_curve_level_mv(&s, BC250_DPM_TOP_LEVEL) == c.mv[BC250_CURVE_POINTS - 1]);
	/* Under the lab floor the candidate changes nothing at all. */
	CHECK(bc250_dpm_curve_level_mv(&s, BC250_DPM_IDLE_LEVEL) == BC250_CLOCK_FLOOR_MV);
	/* take() says the governor owes a re-apply; it does NOT say the hardware has the curve. Only a
	 * transaction that went through records that, which is what a KEEP is then allowed to rely on. */
	CHECK(bc250_dpm_curve_take(&s) && s.applied != s.serial);
	CHECK(!bc250_dpm_curve_take(&s));               /* once per change, not once per tick */
	bc250_dpm_curve_applied(&s, s.serial);
	CHECK(s.applied == s.serial);
	/* A serial that is not the current one is ignored, so a candidate nobody applied cannot count as
	 * applied because a later apply of an older one happened to land. */
	bc250_dpm_curve_applied(&s, s.serial + 7u);
	CHECK(s.applied == s.serial);
	/* The window runs out and the stored curve comes back by itself, with a re-apply owed again. */
	for (i = 0; i < 19u; i++) CHECK(!bc250_dpm_curve_tick(&s, 1000u));
	CHECK(bc250_dpm_curve_remaining_ms(&s) == 1000u);
	CHECK(bc250_dpm_curve_tick(&s, 1000u));
	CHECK(!s.trial && s.reverts == 1 && s.serial == 2 && bc250_clock_curve_is_default(&s.active));
	CHECK(bc250_dpm_curve_take(&s));
	bc250_dpm_curve_applied(&s, s.serial);
	CHECK(s.applied == 2u);
	CHECK(!bc250_dpm_curve_tick(&s, 100000u));      /* no window, no revert, no serial */
	CHECK(s.serial == 2u);

	/* A keep inside the window: the candidate becomes the stored curve and the hardware needs nothing. The
	 * candidate must have run first (0.7.211): a KEEP in the same governor tick as the SET, and a KEEP after
	 * an apply the clock gate refused, both answer -1 and store nothing. */
	CHECK(bc250_dpm_curve_set(&s, &c, 0u, &level) == BC250_CLOCK_CURVE_OK);
	CHECK(s.trial_ms == BC250_DPM_CURVE_TRIAL_MIN_MS);      /* 0 is clamped up, not taken literally */
	CHECK(bc250_dpm_curve_keep(&s) == -1 && s.trial && s.keeps == 0);   /* nothing applied, no time passed */
	CHECK(bc250_dpm_curve_take(&s));
	CHECK(bc250_dpm_curve_keep(&s) == -1 && s.trial);        /* the apply has not gone through yet */
	bc250_dpm_curve_applied(&s, s.serial);
	CHECK(bc250_dpm_curve_keep(&s) == -1 && s.trial);        /* applied, but inside one governor tick */
	CHECK(!bc250_dpm_curve_tick(&s, BC250_DPM_CURVE_KEEP_MIN_MS));
	CHECK(bc250_dpm_curve_keep(&s) == 1 && !s.trial && s.keeps == 1);
	CHECK(!bc250_dpm_curve_take(&s));               /* a keep changes no voltage: nothing to re-apply */
	for (i = 0; i < BC250_CURVE_POINTS; i++) CHECK(s.stored.mv[i] == c.mv[i] && s.active.mv[i] == c.mv[i]);
	CHECK(!bc250_clock_curve_is_default(&s.stored));
	CHECK(!bc250_dpm_curve_tick(&s, 1000000u));     /* a kept curve has no deadline */

	/* A second trial on top of a kept curve, then a cancel: the revert target is the kept curve and never
	 * the first candidate. */
	{
		struct bc250_clock_curve deeper = line;
		unsigned int serial = s.serial;
		deeper.mv[BC250_CURVE_POINTS - 1] = BC250_CLOCK_CEILING_MV - 1u;
		CHECK(bc250_dpm_curve_set(&s, &line, 30000u, &level) == BC250_CLOCK_CURVE_OK);
		CHECK(s.serial == serial + 1u && s.trial);
		CHECK(bc250_dpm_curve_set(&s, &deeper, 30000u, &level) == BC250_CLOCK_CURVE_OK);
		CHECK(s.serial == serial + 2u && s.trial_ms == 30000u && bc250_dpm_curve_remaining_ms(&s) == 30000u);
		/* The second candidate was never applied, so a KEEP here would store a curve the hardware never
		 * carried: that is refused, and the cancel is the way out. */
		CHECK(bc250_dpm_curve_keep(&s) == -1 && s.trial);
		CHECK(bc250_dpm_curve_cancel(&s) && s.cancels == 1);
		for (i = 0; i < BC250_CURVE_POINTS; i++) CHECK(s.active.mv[i] == c.mv[i]);
		CHECK(bc250_dpm_curve_take(&s));
		bc250_dpm_curve_applied(&s, s.serial);
	}
	/* The apply the clock gate refused: the governor takes the re-apply, the transaction fails, so nothing
	 * records the serial and the KEEP stays refused however long the window runs (0.7.211). */
	{
		unsigned int serial;
		CHECK(bc250_dpm_curve_set(&s, &line, 30000u, &level) == BC250_CLOCK_CURVE_OK);
		serial = s.serial;
		CHECK(bc250_dpm_curve_take(&s));        /* the caller tried and the hardware refused */
		CHECK(!bc250_dpm_curve_tick(&s, 5000u));
		CHECK(bc250_dpm_curve_keep(&s) == -1 && s.trial && s.applied != serial);
		CHECK(bc250_dpm_curve_cancel(&s));
		CHECK(bc250_dpm_curve_take(&s));
		bc250_dpm_curve_applied(&s, s.serial);
	}
	/* A candidate the checks refuse never becomes active and never moves the serial. */
	{
		struct bc250_clock_curve bad = c;
		unsigned int serial = s.serial;
		bad.mv[0] = BC250_CLOCK_FLOOR_MV + 10u;
		CHECK(bc250_dpm_curve_set(&s, &bad, 20000u, &level) == BC250_CLOCK_CURVE_FLOOR);
		CHECK(s.serial == serial && !s.trial && s.active.mv[0] == c.mv[0]);
	}
	/* The window is bounded at both ends, whatever a caller asks for. */
	CHECK(bc250_dpm_curve_set(&s, &line, 1u, &level) == BC250_CLOCK_CURVE_OK);
	CHECK(s.trial_ms == BC250_DPM_CURVE_TRIAL_MIN_MS);
	CHECK(bc250_dpm_curve_set(&s, &line, 10u * 60u * 1000u, &level) == BC250_CLOCK_CURVE_OK);
	CHECK(s.trial_ms == BC250_DPM_CURVE_TRIAL_MAX_MS);
	/* The reset ends a trial too, and leaves the table's own line stored. */
	CHECK(bc250_dpm_curve_reset(&s));
	CHECK(!s.trial && bc250_clock_curve_is_default(&s.stored) && bc250_clock_curve_is_default(&s.active));
	CHECK(bc250_dpm_curve_take(&s));
	bc250_dpm_curve_applied(&s, s.serial);
	CHECK(!bc250_dpm_curve_reset(&s));              /* already the line: nothing to change, nothing to apply */

	/* A start that reads a stored curve runs it from its first level change, with nothing owed. */
	bc250_dpm_curve_init(&s, &c);
	for (i = 0; i < BC250_CURVE_POINTS; i++) CHECK(s.active.mv[i] == c.mv[i] && s.stored.mv[i] == c.mv[i]);
	CHECK(!s.apply && !s.trial && s.serial == 0);
	CHECK(bc250_dpm_curve_level_vid(&s, BC250_DPM_TOP_LEVEL) ==
	      bc250_clock_vid(c.mv[BC250_CURVE_POINTS - 1]));
}

static void test_tune_check(void)
{
	struct bc250_dpm_tune t, d;
	struct bc250_dpm_governor g;
	unsigned int up, target, down, compared = 0;

	bc250_dpm_tune_default(&d);
	CHECK(d.up_permille == 900u && d.target_permille == 800u && d.down_permille == 650u && d.down_hold_ms == 200u &&
	      d.floor_level == L(0));
	/* 0.7.213: the soft release is on by default, 4.0 C under HOT_MC (83.0 C) with a 4 s step, and it is the up side
	 * of the soft zone, whose threshold is 1.0 C under HOT_MC (86.0 C) with a 1.5 s step and a 15 s lead. */
	CHECK(d.hot_step_ms == 500u && d.soft_delta_mc == 4000u && d.soft_step_ms == 4000u);
	CHECK(d.zone_delta_mc == 1000u && d.zone_step_ms == 1500u && d.zone_lead_ms == 15000u);
	CHECK(bc250_dpm_zone_mc(&d) == 86000 && bc250_dpm_warm_mc(&d) == 83000);
	CHECK(bc250_dpm_tune_check(&d, BC250_DPM_TOP_LEVEL) == BC250_DPM_TUNE_OK);
	CHECK(bc250_dpm_tune_check(&d, L(0)) == BC250_DPM_TUNE_OK);        /* a fixed-lab ceiling: no runtime floor is fine */
	CHECK(ref_lowering(&d) == 1 && ref_raise(&d) == 1);
	bc250_dpm_init(&g, L(7));
	CHECK(memcmp(&g.tune, &d, sizeof(d)) == 0 && g.floor_ticks == 0);

	/* Ranges. */
	t = tune(1001, 800, 650, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_RANGE);
	t = tune(900, 800, 99, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_RANGE);
	t = tune(900, 0, 650, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_RANGE);
	t = tune(1000, 999, 100, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	/* Order: down < target < up, strictly. */
	t = tune(900, 650, 650, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ORDER);
	t = tune(900, 900, 650, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ORDER);
	t = tune(600, 800, 650, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ORDER);
	/* Invariant 1 at its exact edge for up 900: 11 x 818 = 8998 <= 9000 admits down 817, 11 x 819 = 9009 does not
	 * admit 818. (target 899 keeps invariant 2 out of the way.) */
	t = tune(900, 899, 817, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = tune(900, 899, 818, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_LOWERING);
	/* The strict form without the +1 would admit 818 (dpm_strict_form_admits_818): at 1100 MHz a load that reads
	 * 818 permille (the work of 899.8 to 900.9 permille at 1000 MHz) can hold the average at 817, one under it, and
	 * lower; at 1000 MHz the same work reads 899 or 900, and 900 is up. */
	/* Invariant 2: a target just above down lets a raise land below down. */
	t = tune(370, 336, 335, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_RAISE);
	CHECK(ref_lowering(&t) == 1 && ref_raise(&t) == 0);
	/* The A/B tune of the 0.7.185 lab plan. */
	t = tune(750, 650, 500, 200, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	/* The hold. */
	t = tune(900, 800, 650, 99, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_HOLD);
	t = tune(900, 800, 650, 100, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = tune(900, 800, 650, 5000, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = tune(900, 800, 650, 5001, L(0)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_HOLD);
	/* The floor: a level at or below the start's ceiling. */
	t = tune(900, 800, 650, 200, L(10)); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	CHECK(bc250_dpm_tune_check(&t, L(9)) == BC250_DPM_TUNE_FLOOR);
	CHECK(bc250_dpm_tune_check(&t, L(0)) == BC250_DPM_TUNE_FLOOR);
	t = tune(900, 800, 650, 200, L(11)); CHECK(bc250_dpm_tune_check(&t, L(99)) == BC250_DPM_TUNE_FLOOR);
	t = tune(900, 800, 650, 200, L(6)); CHECK(bc250_dpm_tune_check(&t, L(6)) == BC250_DPM_TUNE_OK);
	/* The thermal timing (0.7.197): each field at both edges and one past each, the soft delta's 0 (off) admitted. */
	t = d; t.hot_step_ms = 249; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.hot_step_ms = 250; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.hot_step_ms = 10000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.hot_step_ms = 10001; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.hot_step_ms = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	/* The soft delta's own range, with the zone off so that the deadband rule is not what answers (it has its own
	 * cases below): 0 is the release off, then 500..4500 mC. */
	t = d; bc250_dpm_tune_zone_off(&t); CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 1; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 499; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 500; t.zone_delta_mc = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 4500; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 4501; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 0x80000000u; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_step_ms = 1999; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_step_ms = 2000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_step_ms = 30000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_step_ms = 30001; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	/* The soft step is checked while the release is off, so that turning it on later is the delta alone. */
	t = d; bc250_dpm_tune_zone_off(&t); t.soft_step_ms = 0;
	CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	/* The soft threshold stays strictly inside RELEASE..HOT at both edges of the delta (compile time below). */

	/* The soft zone (0.7.213), each field at both edges and one past each, and the deadband. */
	t = d; t.zone_delta_mc = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);   /* the zone off */
	t = d; t.zone_delta_mc = 499; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	t = d; t.zone_delta_mc = 500; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.zone_delta_mc = 3500; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.zone_delta_mc = 4000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE); /* no deadband */
	t = d; t.zone_delta_mc = 4001; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE); /* past the max */
	t = d; t.zone_delta_mc = 0x80000000u; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	t = d; t.zone_step_ms = 249; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	t = d; t.zone_step_ms = 250; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.zone_step_ms = 30000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.zone_step_ms = 30001; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	/* The zone's step and lead are checked while the zone is off, so that turning it on is the delta alone. */
	t = d; t.zone_delta_mc = 0; t.zone_step_ms = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	t = d; t.zone_lead_ms = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);     /* the lead off */
	t = d; t.zone_lead_ms = 60000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.zone_lead_ms = 60001; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	/* A zone with no soft release above it would have a way down and no way back up until RELEASE_MC: refused by the
	 * same inequality, and the deadband at its exact edge. */
	t = d; t.soft_delta_mc = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	t = d; t.zone_delta_mc = 1000; t.soft_delta_mc = 1499; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_ZONE);
	t = d; t.zone_delta_mc = 1000; t.soft_delta_mc = 1500; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	/* bc250_dpm_tune_zone_off is the 0.7.212 rules and must be admitted whatever else a tune holds. */
	t = d; bc250_dpm_tune_zone_off(&t);
	CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK && bc250_dpm_zone_mc(&t) == 0 &&
	      bc250_dpm_warm_mc(&t) == BC250_DPM_WARM_MC && t.soft_delta_mc == 0u);

	/* set_tune: a refused tune leaves the governor's as it was, an admitted one replaces it. */
	bc250_dpm_init(&g, L(6));
	t = tune(750, 650, 500, 300, L(7));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && memcmp(&g.tune, &d, sizeof(d)) == 0);
	t.floor_level = L(6);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK && memcmp(&g.tune, &t, sizeof(t)) == 0);

	/* The check against the references: invariant 1 on a fine grid, invariant 2 (and 1) on a coarser one. */
	for (up = 110; up <= 1000; up += 7)
		for (down = 100; down < up; down++) {
			t = tune(up, up - 1u, down, 200, L(0));
			if (down + 1u >= up) continue;
			{
				enum bc250_dpm_tune_error e = bc250_dpm_tune_check(&t, L(10));
				CHECK((e != BC250_DPM_TUNE_LOWERING) == ref_lowering(&t));
				compared++;
			}
		}
	for (up = 150; up <= 1000; up += 50)
		for (down = 100; down < up; down += 45)
			for (target = down + 1u; target < up; target += 40) {
				enum bc250_dpm_tune_error e;
				int r1, r2;
				t = tune(up, target, down, 200, L(0));
				e = bc250_dpm_tune_check(&t, L(10));
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
				t = tune(up, target, down, 200, L(0));
				if (bc250_dpm_tune_check(&t, L(10)) != BC250_DPM_TUNE_OK) continue;
				admitted++;
				for (h = 0; h < sizeof(holds) / sizeof(holds[0]); h++) {
					t.down_hold_ms = holds[h];
					for (demand = 100; demand <= 2400; demand += 50) {
						unsigned int mhz, busy;
						CHECK(sweep_changes(&t, BC250_DPM_TOP_LEVEL, demand, &settled) == 0);
						mhz = bc250_dpm_level_mhz(settled);
						busy = demand >= mhz ? 1000u : demand * 1000u / mhz;
						if (settled < BC250_DPM_TOP_LEVEL) CHECK(busy < t.up_permille);
						if (settled > L(0)) CHECK(busy + 2u >= t.down_permille);
					}
				}
			}
	CHECK(admitted > 40u);

	/* The lab plan's tune, densely. */
	t = tune(750, 650, 500, 200, L(0));
	for (demand = 100; demand <= 2400; demand += 10) CHECK(sweep_changes(&t, BC250_DPM_TOP_LEVEL, demand, &settled) == 0);

	/* Negative control: tunes that keep invariant 1 but break invariant 2 do cycle. The check refuses them. */
	t = tune(370, 336, 335, 200, L(0));
	CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_RAISE);
	for (demand = 300; demand <= 450; demand += 5) if (sweep_changes(&t, BC250_DPM_TOP_LEVEL, demand, &settled)) seen_cycle++;
	CHECK(seen_cycle > 0);
	seen_cycle = 0;
	t = tune(200, 181, 180, 100, L(0));
	CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_RAISE);
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
	init_old(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, L(10));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 0, 60, 25) == L(10));
	CHECK(g.want == L(0) && g.throttle == BC250_DPM_THROTTLE_NONE && g.floor_ticks == 1 && g.raises == 1);
	for (i = 0; i < 400; i++) CHECK(run(&g, 0, 60, 25) == L(10));
	CHECK(g.lowers == 0 && g.floor_ticks > 1);
	/* Full load at the floor: nothing to raise, nothing lifted. */
	i = g.floor_ticks;
	CHECK(run(&g, 1000, 60, 25) == L(10) && g.floor_ticks == i);

	/* Thermal soft beats the floor: one step at 87 C, another every HOT_STEP_MS, back only below 82 C. */
	CHECK(run_mc(&g, 0, 87000, 25) == L(9) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 0, 88, 25) == L(9));
	CHECK(run(&g, 0, 88, 25) == L(8));
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 0, 82000, 25) == L(8));
	for (i = 0; i < 4 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 0, 70, 25);
	CHECK(level == L(10) && g.throttle == BC250_DPM_THROTTLE_NONE);
	/* Critical: the thermal floor at once, whatever the runtime floor. */
	CHECK(run(&g, 0, 90, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL && g.throttle == BC250_DPM_THROTTLE_THERMAL_HARD);
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 0, 70, 25);
	CHECK(level == L(10));
	/* No sensor: the lab floor, not the thermal floor (nothing is known about the temperature). */
	in = tick(0, 60, 25); in.temperature_valid = 0;
	level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	CHECK(level == L(0) && g.throttle == BC250_DPM_THROTTLE_SENSOR);
	for (i = 0; i < 20 * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run(&g, 0, 70, 25);
	CHECK(level == L(10));
	/* SetStablePowerState pins the table's floor over the runtime floor. */
	g.stable = 1;
	CHECK(run(&g, 0, 60, 25) == L(0) && g.throttle == BC250_DPM_THROTTLE_STABLE);
	g.stable = 0;
	CHECK(run(&g, 0, 60, 25) == L(10));

	/* Floor off: from 2000 at idle one step per hold, as before 0.7.185 (the hold counted from here). */
	t.floor_level = L(0);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	g.down_ms = 0;
	for (i = 0; i < 7; i++) CHECK(run(&g, 0, 60, 25) == L(10));
	CHECK(run(&g, 0, 60, 25) == L(9));
	for (i = 0; i < 400 && level > L(0); i++) level = run(&g, 0, 60, 25);
	CHECK(level == L(0));

	/* A floor in the middle: the load still raises above it, lowerings stop at it. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, L(5));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 0, 60, 25) == L(5));
	CHECK(run(&g, 1000, 60, 25) == L(9));     /* 1500 x 1000 / 800 = 1875 -> 1900 */
	for (i = 0; i < 400; i++) level = run(&g, 0, 60, 25);
	CHECK(level == L(5) && g.want <= L(5));

	/* The max setting: a floor above it is refused; one written past the check is clamped, never above the ceiling. */
	init_old(&g, L(6));
	t = tune(900, 800, 650, 200, L(10));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && g.tune.floor_level == L(0));
	t.floor_level = L(6);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 0, 60, 25) == L(6) && g.throttle == BC250_DPM_THROTTLE_NONE);
	CHECK(run(&g, 1000, 60, 25) == L(6) && g.throttle == BC250_DPM_THROTTLE_MAX_SETTING);
	g.tune.floor_level = L(10);
	/* The limit alone would also hold it at 6, but name MAX_SETTING; the clamp keeps the floor a floor. */
	for (i = 0; i < 50; i++) CHECK(run(&g, 0, 60, 25) == L(6) && g.throttle == BC250_DPM_THROTTLE_NONE);
	/* Thermal soft with the floor at the ceiling: below both. */
	CHECK(run(&g, 0, 87, 25) == L(5) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);

	/* Tuned thresholds act: up 750 raises where 900 would not; down 500 holds where 650 would lower. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run(&g, 800, 60, 25) == L(0));
	t = tune(750, 650, 500, 200, L(0));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 800, 60, 25) == L(3));      /* 1000 x 800 / 650 = 1230 -> 1300 */
	for (i = 0; i < 100; i++) CHECK(run(&g, 600, 60, 25) == L(3));
	t = tune(900, 800, 650, 200, L(0));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	for (i = 0; i < 100; i++) level = run(&g, 600, 60, 25);
	CHECK(level < L(3));
}

/* ---- the warm zone (0.7.200; at 87 C from 0.7.204) ------------------------------------------------ */

/* After session 344 (1500 MHz held while Tctl rose 83.5 -> 85.3 C) 0.7.200 refused every raise from 85 C up to 87 C.
 * 0.7.204 (owner, 2026-10-04: "próg na 87", the threshold at 87) moves the zone to 87 C: from 87 C up no raise, the
 * hot cap lowers one level per hot step. Below 87 C a raise goes, one level per ramp interval (0.7.203). */
static void test_warm(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_tune t;
	struct bc250_dpm_input in;
	unsigned int i, level;

	/* Full load at 1500 MHz at 85.0 C and 86.999 C: the raise goes, one level (the ramp), no warm hold, no hot entry. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	CHECK(run_mc(&g, 1000, 85000, 25) == L(6) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP && g.warm_holds == 0);
	CHECK(g.want == L(9) && g.raises == 1);		/* want stays the load's own answer */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	CHECK(run_mc(&g, 1000, 86999, 25) == L(6) && g.warm_holds == 0 && g.thermal_events == 0);
	/* Held at 86 C, the clock climbs one level per ramp interval (3.82 s at 86 C) up to the top, no thermal event. */
	for (i = 0; i < 20000u / 25u; i++) level = run_mc(&g, 1000, 86000, 25);
	CHECK(level == L(10) && g.warm_holds == 0 && g.thermal_events == 0 && g.raises == 5);

	/* 87 C is HOT: one step down at once, as before; the warm rule counts nothing, the cap holds the level. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(9) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	CHECK(g.thermal_events == 1 && g.thermal_cap == L(9) && g.warm_holds == 0);
	/* Still at 87 C or more: one more level per hot step, never a raise. */
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run_mc(&g, 1000, 87000, 25) == L(9));
	CHECK(run_mc(&g, 1000, 88000, 25) == L(8));
	for (i = 0; i < 2000u / 25u; i++) level = run_mc(&g, 1000, 87500, 25);
	CHECK(level == L(4) && g.raises == 0 && g.warm_holds == 0);
	/* Back under 87 C, above 82 C: the cap holds (no release above 82 C), and so does the clock. */
	for (i = 0; i < 100; i++) CHECK(run_mc(&g, 1000, 86000, 25) == L(4));
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.thermal_cap == L(4));
	/* The release below 82 C is unchanged: one level per RELEASE_STEP_MS; the clock follows at the ramp's pace. */
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++) CHECK(run_mc(&g, 1000, 81999, 25) == L(4));
	CHECK(run_mc(&g, 1000, 81999, 25) == L(5) && g.thermal_cap == L(5));

	/* The backstop: the load lowered the clock under the hot cap inside a hot episode, then asks again at 87.5 C. The cap
	 * would allow it; the warm rule holds the level (before 0.7.204 the step asked for it and the clock gate refused it,
	 * "refused by the clock gate" in the driver log). */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(9) && g.thermal_cap == L(9));
	g.avg_permille = 0;
	for (i = 1; i < BC250_DPM_DOWN_HOLD_MS / 25u; i++) CHECK(run_mc(&g, 0, 87500, 25) == L(9));
	CHECK(run_mc(&g, 0, 87500, 25) == L(8) && g.thermal_cap == L(9));
	CHECK(run_mc(&g, 1000, 87500, 25) == L(8) && g.throttle == BC250_DPM_THROTTLE_THERMAL_WARM && g.warm_holds == 1);

	/* A lowering still happens at 85-86.999 C, after the same hold as at 60 C, one step at a time. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(8);
	for (i = 0; i < 7; i++) CHECK(run_mc(&g, 0, 85000, 25) == L(8));
	CHECK(run_mc(&g, 0, 86999, 25) == L(7) && g.lowers == 1 && g.warm_holds == 0);
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE);

	/* The runtime floor is a raise too: one level at 86.5 C, none at 87 C (the hot cap). */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(3);
	t = tune(900, 800, 650, 200, L(10));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run_mc(&g, 0, 86500, 25) == L(4) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP && g.warm_holds == 0);
	CHECK(run_mc(&g, 0, 87000, 25) == L(3) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	CHECK(run_mc(&g, 0, 60000, 25) == L(3));	/* one tick under 82 C releases nothing */

	/* At the max setting there is nothing to refuse: the setting names the reason. */
	init_old(&g, L(6)); g.level = L(6);
	CHECK(run_mc(&g, 1000, 86000, 25) == L(6) && g.throttle == BC250_DPM_THROTTLE_MAX_SETTING && g.warm_holds == 0);

	/* SetStablePowerState and a missing sensor go to the floor, never a hold. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(5); g.stable = 1;
	CHECK(run_mc(&g, 1000, 87000, 25) == L(0) && g.throttle == BC250_DPM_THROTTLE_STABLE && g.warm_holds == 0);
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	in = tick(1000, 0, 25); in.temperature_mc = 87000; in.temperature_valid = 0;
	CHECK(bc250_dpm_step(&g, &in) == L(0) && g.throttle == BC250_DPM_THROTTLE_SENSOR && g.warm_holds == 0);
}

/* ---- the thermal timing (0.7.197, BD-055) ------------------------------------------------------- */

/* A governor at the top under full load with a thermal tune; the tune must be admitted. The soft zone of 0.7.213 is off
 * here: these cases are about the hot cap's timing and the soft release on their own, and a zone at 86 C would take the
 * cap down before either of them acted. */
static void init_thermal(struct bc250_dpm_governor *g, unsigned int hot_step, unsigned int soft_delta, unsigned int soft_step)
{
	struct bc250_dpm_tune t;
	init_old(g, BC250_DPM_TOP_LEVEL);
	bc250_dpm_tune_default(&t);
	bc250_dpm_tune_zone_off(&t);
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
		/* ... bounded by the thermal floor, which is TOP_LEVEL - THERMAL_FLOOR_LEVEL levels down: twelve
		 * since 0.7.205 (ten up to 0.7.204). Up to 0.7.205 that was BC250_DPM_TOP_LEVEL itself, because the
		 * thermal floor was index 0; since 0.7.207 the table has three idle-only levels under it. */
		{
			unsigned int span = BC250_DPM_TOP_LEVEL - BC250_DPM_THERMAL_FLOOR_LEVEL;
			CHECK(changes == (7975u / steps[s] + 1u < span ? 7975u / steps[s] + 1u : span));
		}
		CHECK(min_gap >= steps[s]);
		CHECK(level == g.thermal_cap && g.thermal_events == 8000u / 25u / 2u);
	}

	/* A steady 87 C: the same spacing, from the entry. */
	init_thermal(&g, 2000, 0, BC250_DPM_SOFT_STEP_MS);
	CHECK(run(&g, 1000, 87, 25) == L(9));
	for (i = 1; i < 2000u / 25u; i++) CHECK(run(&g, 1000, 87, 25) == L(9));
	CHECK(run(&g, 1000, 87, 25) == L(8));

	/* A re-entry after a short cool spell inside the hot step: the cap is clamped to the running clock (nothing
	 * lowered, no raise), and the step follows a hot step after the last change, not after the entry. */
	init_thermal(&g, 2000, 0, BC250_DPM_SOFT_STEP_MS);
	CHECK(run(&g, 1000, 87, 25) == L(9));                              /* t = 0: step */
	for (i = 0; i < 1000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == L(9));   /* out for 1 s */
	CHECK(run(&g, 1000, 87, 25) == L(9) && g.thermal_cap == L(9) && g.thermal_events == 2);
	for (i = 1; i < 1000u / 25u - 1u; i++) CHECK(run(&g, 1000, 87, 25) == L(9));
	CHECK(run(&g, 1000, 87, 25) == L(8));                              /* 2 s after the first step */

	/* The clamp to the running clock: a load below the cap is not raised past the clock it ran at on entry. */
	init_thermal(&g, 2000, 0, BC250_DPM_SOFT_STEP_MS);
	g.level = L(6); g.cap_ms = 100;                                    /* a raise 100 ms ago */
	CHECK(run(&g, 1000, 87, 25) == L(6) && g.thermal_cap == L(6));       /* full load asks more; the clamp holds 6 */
}

static void test_soft_release(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, level;

	/* Off by default: anywhere in 82..86.999 the cap holds, as before. */
	init_thermal(&g, 500, 0, 3000);
	CHECK(run(&g, 1000, 87, 25) == L(9));
	for (i = 0; i < 60000u / 25u; i++) CHECK(run_mc(&g, 1000, 82500, 25) == L(9));
	CHECK(g.soft_releases == 0);

	/* Delta 1.5 C (85.5 C), step 3 s: held below for a whole step without a break, one level. */
	init_thermal(&g, 500, 1500, 3000);
	CHECK(run(&g, 1000, 87, 25) == L(9));
	for (i = 1; i < 3000u / 25u; i++) CHECK(run_mc(&g, 1000, 85499, 25) == L(9));
	/* 85.499 C is under the warm zone (87 C from 0.7.204; 85 C before, where only the cap rose): the clock follows the
	 * cap at once, one level, the first raise since the start. */
	CHECK(run_mc(&g, 1000, 85499, 25) == L(10) && g.soft_releases == 1 && g.thermal_cap == L(10));
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE && g.warm_holds == 0);
	/* At the threshold itself: no raise. */
	init_thermal(&g, 500, 1500, 3000);
	CHECK(run(&g, 1000, 87, 25) == L(9));
	for (i = 0; i < 30000u / 25u; i++) CHECK(run_mc(&g, 1000, 85500, 25) == L(9));
	CHECK(g.soft_releases == 0 && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	/* A break restarts the hold: one tick at 85.6 C just before the step. */
	init_thermal(&g, 500, 1500, 3000);
	CHECK(run(&g, 1000, 87, 25) == L(9));
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == L(9));
	CHECK(run_mc(&g, 1000, 85600, 25) == L(9));
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == L(9));
	CHECK(run(&g, 1000, 85, 25) == L(10) && g.soft_releases == 1 && g.thermal_cap == L(10));
	/* A hot tick restarts it too, and it steps (cap_ms is past the hot step). */
	init_thermal(&g, 500, 1500, 3000);
	g.thermal_cap = L(8); g.level = L(8);
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == L(8));
	CHECK(run(&g, 1000, 87, 25) == L(7) && g.soft_releases == 0);
	for (i = 1; i < 3000u / 25u; i++) CHECK(run(&g, 1000, 85, 25) == L(7));
	CHECK(run(&g, 1000, 85, 25) == L(8) && g.soft_releases == 1 && g.thermal_cap == L(8));
	/* The soft path climbs one level per step, all the way up; below 82 C the fast path (1 s) takes over. Since 0.7.203
	 * the clock follows the cap one level per ramp interval (3.47 s at 84 C, 2.94 s at 81 C from 0.7.204): the cap is at
	 * the top after 18 s (6 s), the clock a few seconds later. */
	init_thermal(&g, 500, 1500, 3000);
	g.thermal_cap = L(4); g.level = L(4);
	for (i = 0; i < 6u * 3000u / 25u; i++) level = run(&g, 1000, 84, 25);
	CHECK(level < L(10) && g.thermal_cap == L(10) && g.soft_releases == 6);
	for (i = 0; i < 8000u / 25u; i++) level = run(&g, 1000, 84, 25);
	CHECK(level == L(10));
	g.thermal_cap = L(4); g.level = L(4); g.soft_ms = 0;
	for (i = 0; i < 6u * 1000u / 25u; i++) level = run(&g, 1000, 81, 25);
	CHECK(g.thermal_cap == L(10) && level < L(10));
	for (i = 0; i < 12000u / 25u; i++) level = run(&g, 1000, 81, 25);
	CHECK(level == L(10) && g.soft_releases == 6);
	/* The widest delta (82.5 C) and the narrowest (86.5 C). Both thresholds lie under the warm zone (87 C, 0.7.204): the
	 * clock follows the raised cap. */
	init_thermal(&g, 500, 4500, 2000);
	g.thermal_cap = L(8); g.level = L(8);
	for (i = 0; i < 2000u / 25u; i++) level = run_mc(&g, 1000, 82499, 25);
	CHECK(level == L(9));
	init_thermal(&g, 500, 500, 2000);
	g.thermal_cap = L(8); g.level = L(8);
	for (i = 0; i < 2000u / 25u; i++) level = run_mc(&g, 1000, 86499, 25);
	CHECK(level == L(9) && g.thermal_cap == L(9) && g.soft_releases == 1 && g.warm_holds == 0);
	/* Critical goes to the floor; the soft release brings it back, a level per step, never past the setting. The clock
	 * follows each raised cap within a ramp interval (3.65 s at 85 C). */
	init_old(&g, L(6));
	{
		struct bc250_dpm_tune t;
		bc250_dpm_tune_default(&t); bc250_dpm_tune_zone_off(&t); t.soft_delta_mc = 1500;
		CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	}
	CHECK(run(&g, 1000, 90, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 20u * 3000u / 25u; i++) level = run(&g, 1000, 85, 25);
	/* Eight levels back since 0.7.205 (critical goes to 800 MHz, two under the lab floor), six before it. */
	CHECK(level == L(6) && g.thermal_cap == L(6) && g.soft_releases == 8 && g.raises == 8 && g.warm_holds == 0);
	/* A missing sensor resets the hold as critical does. */
	init_thermal(&g, 500, 1500, 3000);
	g.thermal_cap = L(8); g.level = L(8);
	for (i = 1; i < 3000u / 25u; i++) run(&g, 1000, 85, 25);
	{
		struct bc250_dpm_input in = tick(1000, 60, 25);
		in.temperature_valid = 0;
		level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	}
	CHECK(level == L(0) && g.soft_ms == 0 && g.soft_releases == 0);
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

/* ---- the soft thermal zone (0.7.213, BD-087, GRAPH.md C56) --------------------------------------- */

/* The rules, one at a time, on the defaults of bc250_dpm_tune_default: the zone's threshold and step, the hold band
 * between the zone and the soft release, the release, the lead, and the two backstops that keep reading the raw
 * sensor. The plant below then runs the whole thing against session 436. */
static void test_zone(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, level, first;
	int mc;

	/* The thresholds the defaults put in force: 86.0 C for the zone, 83.0 C for the cap's and the clock's up side. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(bc250_dpm_zone_mc(&g.tune) == BC250_DPM_HOT_MC - 1000 && bc250_dpm_zone_mc(&g.tune) == 86000);
	CHECK(bc250_dpm_warm_mc(&g.tune) == BC250_DPM_HOT_MC - 4000 && bc250_dpm_warm_mc(&g.tune) == 83000);

	/* A steady 85.999 C is under the zone, and a steady reading has no slope, so there is no lead either: nothing
	 * thermal happens at all, three degrees under the hot cap's own limit. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	for (i = 0; i < 400u; i++) CHECK(run_mc(&g, 1000, 85999, 25) == BC250_DPM_TOP_LEVEL);
	CHECK(g.thermal_cap == BC250_DPM_TOP_LEVEL && g.zone_steps == 0 && g.zone_ticks == 0);
	CHECK(g.zone_lead_mc == 0 && !g.hot && g.thermal_events == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* 86.0 C: the cap comes down to the clock the GPU is running at in that tick (which lowers nothing yet), then one
	 * level per zone step. The governor is not hot: the hot cap has not been reached. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	CHECK(run_mc(&g, 1000, 86000, 25) == BC250_DPM_TOP_LEVEL && g.thermal_cap == BC250_DPM_TOP_LEVEL);
	CHECK(g.zone_ticks == 1 && g.zone_steps == 0 && !g.hot && g.thermal_events == 0);
	for (i = 2; i <= BC250_DPM_ZONE_STEP_MS / 25u; i++) CHECK(run_mc(&g, 1000, 86000, 25) == BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 86000, 25) == BC250_DPM_TOP_LEVEL - 1u && g.zone_steps == 1);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_ZONE && g.thermal_cap == BC250_DPM_TOP_LEVEL - 1u);
	/* Down to the thermal floor (800 MHz) and no further, one level per zone step, still without going hot. */
	for (i = 0; i < 20u * BC250_DPM_ZONE_STEP_MS / 25u; i++) level = run_mc(&g, 1000, 86000, 25);
	CHECK(level == BC250_DPM_THERMAL_FLOOR_LEVEL && g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(g.zone_steps == BC250_DPM_TOP_LEVEL - BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(!g.hot && g.thermal_events == 0 && g.throttle == BC250_DPM_THROTTLE_THERMAL_ZONE);

	/* The band between the two thresholds, 83.0 to 85.999 C: the cap holds. No step down (the zone is above it) and
	 * no raise (the soft release is below it), however long the die stays there. */
	for (i = 0; i < 10u * BC250_DPM_SOFT_STEP_MS / 25u; i++)
		CHECK(run_mc(&g, 1000, 85999, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 10u * BC250_DPM_SOFT_STEP_MS / 25u; i++)
		CHECK(run_mc(&g, 1000, 83000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL && g.soft_releases == 0 && g.zone_steps ==
	      BC250_DPM_TOP_LEVEL - BC250_DPM_THERMAL_FLOOR_LEVEL);

	/* 82.999 C: the soft release, one level per soft step. The clock follows the cap at the ramp's pace. */
	for (i = 1; i < BC250_DPM_SOFT_STEP_MS / 25u; i++)
		CHECK(run_mc(&g, 1000, 82999, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(run_mc(&g, 1000, 82999, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL + 1u && g.soft_releases == 1);
	/* Below RELEASE_MC the cap rises per release step instead; under the ramp's knee the clock follows at once, and
	 * the cap goes back to the ceiling. */
	for (i = 0; i < 20u * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run_mc(&g, 1000, 65000, 25);
	CHECK(level == BC250_DPM_TOP_LEVEL && g.thermal_cap == BC250_DPM_TOP_LEVEL);
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE);

	/* No raise of the clock from the soft-release threshold up (bc250_dpm_warm_mc): the clock's up side and the cap's
	 * are the same temperature, so a released cap is never followed into the zone. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(0);
	CHECK(run_mc(&g, 1000, 83000, 25) == L(0) && g.throttle == BC250_DPM_THROTTLE_THERMAL_WARM && g.warm_holds == 1);
	CHECK(run_mc(&g, 1000, 82999, 25) == L(1) && g.warm_holds == 1);

	/* The lead, which is where the whole gain of the search sits (BC250_DPM_ZONE_LEAD_MS and the comment there).
	 * A whole slope window has to pass before there is a slope at all. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	mc = 60000;
	for (i = 0; i < (BC250_DPM_ZONE_SLOPE_SLOTS - 1u) * BC250_DPM_ZONE_SLOPE_SLOT_MS / 25u; i++, mc += 5) {
		run_mc(&g, 1000, mc, 25);
		CHECK(g.zone_lead_mc == 0 && g.thermal_cap == BC250_DPM_TOP_LEVEL);
	}
	/* Then it is the fitted rise of that window scaled to BC250_DPM_ZONE_LEAD_MS, and never more than
	 * BC250_DPM_ZONE_LEAD_MAX_MC. 5 mC a 25 ms tick is 0.2 C/s, which extrapolates to 0.2 x 15 = 3.0 C, so what this
	 * ramp reports is the cap (0.7.213 safety review, finding 4: the recorded sessions' own noise reached 7.8 C of
	 * lead on a die that was going nowhere, and no estimator over a 5 s window can tell the two apart). */
	for (i = 0; i < 400u; i++, mc += 5) run_mc(&g, 1000, mc, 25);
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC && g.zone_lead_ok);

	/* So on that ramp the zone acts while the sensor still reads two degrees under its threshold, and the cap is what
	 * makes that bound provable: the zone can never engage below zone_mc minus the cap, a raw 84.0 C by default. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	mc = 80000;
	first = 0;
	for (i = 0; i < 1000u; i++, mc += 5) {
		run_mc(&g, 1000, mc, 25);
		if (!first && g.zone_ticks) first = (unsigned int)mc;
	}
	CHECK(first >= 83950u && first <= 84100u);
	CHECK(first >= (unsigned int)(BC250_DPM_HOT_MC - (int)BC250_DPM_ZONE_DELTA_MC - BC250_DPM_ZONE_LEAD_MAX_MC));
	CHECK(!g.hot && g.thermal_events == 0 && g.zone_steps > 0 && g.thermal_cap < BC250_DPM_TOP_LEVEL);

	/* The two backstops keep reading the raw sensor, so a lead can never invent a hot or a critical episode. A die
	 * rising 2.68 C/s would extrapolate to some 40 C of lead and reports the cap; at 86.8 C the governor is still only
	 * in the zone. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	mc = 60000;
	for (i = 0; i < 400u; i++, mc += 67) run_mc(&g, 1000, mc, 25);
	CHECK(mc - 67 == 86733 && g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC);
	CHECK(!g.hot && g.thermal_events == 0 && g.thermal_cap > BC250_DPM_THERMAL_FLOOR_LEVEL);
	/* 87.0 C on the sensor itself is the hot cap, as it always was. */
	CHECK(run_mc(&g, 1000, 87000, 25) <= g.thermal_cap && g.hot && g.thermal_events == 1);

	/* A falling die is read at its present temperature: no lead, so nothing the zone does depends on a guess about a
	 * die that is already on its way down. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	mc = 86500;
	for (i = 0; i < 400u; i++, mc -= 20) {
		run_mc(&g, 1000, mc, 25);
		CHECK(g.zone_lead_mc == 0);
	}

	/* A tick whose dt_ms had to be clamped empties the ring: the governor's own clock advanced by the clamp and not by
	 * the time that really passed, so every age in it is wrong (test_zone_stall has the case the review found). A tick
	 * that only brings no reading keeps the window and skips its slot (test_zone_sensor_gap). */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	mc = 60000;
	for (i = 0; i < 400u; i++, mc += 5) run_mc(&g, 1000, mc, 25);
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC && g.slope_count == BC250_DPM_ZONE_SLOPE_SLOTS);
	{
		struct bc250_dpm_input in = tick(1000, 0, 25 + BC250_DPM_MAX_DT_MS);
		in.temperature_mc = mc;
		bc250_dpm_commit(&g, bc250_dpm_step(&g, &in));
		CHECK(g.slope_count == 0);
	}
	for (i = 0; i < (BC250_DPM_ZONE_SLOPE_SLOTS - 1u) * BC250_DPM_ZONE_SLOPE_SLOT_MS / 25u; i++, mc += 5) {
		run_mc(&g, 1000, mc, 25);
		CHECK(g.zone_lead_mc == 0);
	}
	for (i = 0; i < 40u; i++, mc += 5) run_mc(&g, 1000, mc, 25);
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC);

	/* The off switch (DpmThermalZone 0, bc250_dpm_tune_zone_off) is 0.7.212 exactly: a die climbing from 80 C to
	 * 86 C moves nothing at all, which is the behaviour BD-087 is about. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = BC250_DPM_TOP_LEVEL;
	mc = 80000;
	for (i = 0; i < 1200u; i++, mc += 5) run_mc(&g, 1000, mc, 25);
	CHECK(mc == 86000 && g.zone_ticks == 0 && g.zone_steps == 0 && g.zone_lead_mc == 0);
	CHECK(g.thermal_cap == BC250_DPM_TOP_LEVEL && g.soft_releases == 0 && g.thermal_events == 0);
}

/* The die model fitted to the recorded lab sessions (P:\BC-250\scratch\thermal-zone\model, fit.json "primary"):
 *
 *	dTj/dt = P - B (Tj - Tamb),   P = kdyn busy (f/1000) (mv/820)^2 + kf (f/1000) + dist
 *
 *	P += kcpu	while a game runs
 *
 * B = 0.0394934 (a 25.3 s time constant), kdyn = 0.37745, kf = 0.0240122, kcpu = 0.16834, Tamb pinned at 40 C, and
 * dist everything else unmeasured, 1.3256 tu at the median of the recorded game sessions. Power is in thermal units:
 * 1 tu raises the die 1 C/s with no heat leaving it, and the smart plug puts 1 tu at about 130 W at the wall. Replaying
 * each session's own clock reproduces its Tctl to 0.13-0.23 C rms. The model's steady points under a running game are
 * the whole problem in three numbers: 1500 MHz at 95 % busy settles at 95.8 C, 1000 MHz at 87.5 C, and 800 MHz - the
 * cap's own bottom - at 85.6 C. So the clock that holds this part is 800 MHz, and the only question is whether the die
 * passes 87 C on the way there.
 *
 * Session 436 is the case of BD-087: RotTR's benchmark at a 1500 MHz ceiling read 80.2 C at the start, 84.0 C at
 * 1200 MHz 32 s later and 88.8 C at 800 MHz 31 s after that, and the lab runner ended the game (Tctl >= 87 C held
 * 10 s). The governor was stepping the clock down the whole time; the die was one time constant behind it. Busy is
 * flat at 95 % whatever the clock, which is what the recorded sessions show (median busy 0.93-0.97 at every clock from
 * 800 to 1500 MHz): these games are GPU bound and render more frames at a higher clock instead of idling more.
 */
#define ZONE_PLANT_B		0.0394934
#define ZONE_PLANT_KDYN		0.37745
#define ZONE_PLANT_KF		0.0240122
#define ZONE_PLANT_KCPU		0.16834
#define ZONE_PLANT_DIST		1.3256		/* the median of the recorded game sessions */
/* The arm calibrated to 436's own outcome. The sessions' disturbance spreads from 1.173 tu (p10) to 1.443 (p90), and
 * 436 ran at the hot end of it: this value puts the steady point at the cap's own bottom, 800 MHz, at 86.4 C, which is
 * what the session showed - it was at 800 MHz and still over 87 C. It is the one arm on which the 0.7.212 rules meet the
 * lab runner's stop, so it is the one that reproduces the defect. */
#define ZONE_PLANT_DIST_436	1.3581
#define ZONE_PLANT_TAMB		40.0

struct zone_result {
	unsigned int peak_mc, hold87_ms, sec87_ms, mean_mhz, final_mhz, changes, zone_steps, hot_entries;
	int stop;			/* the lab runner's rule: 87 C held 10 s, or 89 C at once */
};

/* 436's shape under one set of rules: the governor at a 1500 MHz ceiling, 95 % busy, for three minutes of 25 ms ticks.
 * Two starts matter and they say different things.
 *   at_ceiling      the state 436 was in when the benchmark began: 80.2 C with the clock already at 1500 MHz, which is
 *                   where the 0.7.212 rules leave a part, because nothing under 87 C refuses a raise. The die holds
 *                   heat that has not arrived yet, so this arm asks what a governor can still save after the fact.
 *   from the floor  where a game really starts, and the only fair A/B: both sets of rules see the same die and raise
 *                   the clock under their own rules. */
#define ZONE_RULES_OLD		0	/* 0.7.212: the hot cap at 87 C alone */
#define ZONE_RULES_ZONE		1	/* 0.7.213 as shipped: the zone with its lead */
#define ZONE_RULES_NO_LEAD	2	/* the same thresholds with the lead off: a plain 86 C threshold */

static struct zone_result zone_plant(int rules, int start_mc, int at_ceiling, double dist, unsigned int seconds)
{
	struct bc250_dpm_governor g;
	struct zone_result r;
	double die = start_mc / 1000.0;
	unsigned long long mhz_sum = 0;
	unsigned int i, last, hot_run_ms = 0;
	const unsigned int ticks = seconds * 1000u / 25u, ceiling = (unsigned int)bc250_dpm_level_of(1500);

	memset(&r, 0, sizeof(r));
	if (rules == ZONE_RULES_OLD) init_old(&g, ceiling);
	else bc250_dpm_init(&g, ceiling);
	if (rules == ZONE_RULES_NO_LEAD) {
		struct bc250_dpm_tune t = g.tune;
		t.zone_lead_ms = 0;
		CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	}
	if (at_ceiling) g.level = ceiling;	/* 436's own state: the 0.7.212 rules had already raised the clock this far */
	last = g.level;
	for (i = 0; i < ticks; i++) {
		unsigned int mhz = bc250_dpm_level_mhz(g.level);
		double v = bc250_dpm_level_mv(g.level) / 820.0;
		double p = ZONE_PLANT_KDYN * 0.95 * (mhz / 1000.0) * v * v + ZONE_PLANT_KF * (mhz / 1000.0) +
			   ZONE_PLANT_KCPU + dist;
		int sensor;
		die += (p - ZONE_PLANT_B * (die - ZONE_PLANT_TAMB)) * 0.025;
		sensor = (int)(die * 1000.0);
		if (sensor > (int)r.peak_mc) r.peak_mc = (unsigned int)sensor;
		if (sensor >= 87000) {
			r.sec87_ms += 25u;
			hot_run_ms += 25u;
			if (hot_run_ms > r.hold87_ms) r.hold87_ms = hot_run_ms;
		} else hot_run_ms = 0;
		if (sensor >= 89000 || hot_run_ms >= 10000u) r.stop = 1;
		run_mc(&g, 950, sensor, 25);
		if (g.level != last) { r.changes++; last = g.level; }
		mhz_sum += bc250_dpm_level_mhz(g.level);
	}
	r.mean_mhz = (unsigned int)(mhz_sum / ticks);
	r.final_mhz = bc250_dpm_level_mhz(g.level);
	r.zone_steps = g.zone_steps;
	r.hot_entries = g.thermal_events;
	return r;
}

static void zone_print(const char *name, const struct zone_result *r)
{
	printf("zone436 %-14s peak %u mC, %u ms >= 87 C (longest %u ms), runner %-4s mean %u MHz (final %u), "
	       "%u level changes, %u zone steps, %u hot entries\n", name, r->peak_mc, r->sec87_ms, r->hold87_ms,
	       r->stop ? "STOP" : "ok", r->mean_mhz, r->final_mhz, r->changes, r->zone_steps, r->hot_entries);
}

static void test_zone436(void)
{
	struct zone_result old = zone_plant(ZONE_RULES_OLD, 80200, 1, ZONE_PLANT_DIST_436, 180u);
	struct zone_result now = zone_plant(ZONE_RULES_ZONE, 80200, 1, ZONE_PLANT_DIST_436, 180u);
	struct zone_result cold_old = zone_plant(ZONE_RULES_OLD, 70000, 0, ZONE_PLANT_DIST_436, 180u);
	struct zone_result cold_now = zone_plant(ZONE_RULES_ZONE, 70000, 0, ZONE_PLANT_DIST_436, 180u);
	struct zone_result flat = zone_plant(ZONE_RULES_NO_LEAD, 70000, 0, ZONE_PLANT_DIST_436, 180u);
	struct zone_result med_old = zone_plant(ZONE_RULES_OLD, 70000, 0, ZONE_PLANT_DIST, 180u);
	struct zone_result med_now = zone_plant(ZONE_RULES_ZONE, 70000, 0, ZONE_PLANT_DIST, 180u);
	zone_print("436 state, 0.7.212", &old);
	zone_print("436 state, zone", &now);
	zone_print("from floor, 0.7.212", &cold_old);
	zone_print("from floor, zone", &cold_now);
	zone_print("from floor, lead off", &flat);
	zone_print("median, 0.7.212", &med_old);
	zone_print("median, zone", &med_now);

	/* What BD-087 reported: with the hot cap as the only rule the die goes past 87 C and the runner ends the game. It
	 * happens from either start, because the rule is the same in both - nothing under 87 C refuses a raise, so the clock
	 * is already at the ceiling by the time the die is anywhere near the limit. */
	CHECK(old.peak_mc >= 87000u && old.stop);
	CHECK(cold_old.peak_mc >= 87000u && cold_old.stop);
	/* With the zone, from the start a game really has: the cap comes down before the die arrives, so 87 C is never
	 * reached at all and the hot cap behind it never fires once. */
	CHECK(cold_now.peak_mc < 87000u && !cold_now.stop && cold_now.sec87_ms == 0u && cold_now.hold87_ms == 0u);
	CHECK(cold_now.zone_steps > 0 && cold_now.hot_entries == 0);
	/* From 436's own state the heat is already in the die and no governor can take it back out: the zone does not keep
	 * the reading under 87 C there. What it does is keep the excursion short enough that the runner does not end the
	 * game, which is the difference between a benchmark that finishes and one that does not. */
	CHECK(!now.stop && now.hold87_ms < 10000u && now.peak_mc < old.peak_mc && now.sec87_ms * 2u < old.sec87_ms);
	/* And it does not pay for it in clock: the cap stops where the die balances (800 MHz on this plant), which is where
	 * the hot cap ends up anyway after the overshoot that costs the game. */
	CHECK(now.mean_mhz >= 800u && now.final_mhz >= 800u && cold_now.mean_mhz >= 800u);
	CHECK(cold_now.mean_mhz * 10u >= cold_old.mean_mhz * 9u);
	/* At the sessions' median disturbance the 0.7.212 rules do not quite meet the stop rule, and they still park the die
	 * above 87 C for seconds at a time - the state the shipped governor was measured in on every sustained load. The zone
	 * keeps the same final clock and never reaches 87 C at all. */
	CHECK(med_old.sec87_ms >= 5000u && med_now.sec87_ms == 0u && med_now.peak_mc < 87000u);
	CHECK(med_now.final_mhz == med_old.final_mhz);
	/* It is the lead that does this and not the threshold, which is the one finding of the search worth carrying: the
	 * same thresholds with the lead off are a plain 86 C threshold, and a plain threshold on a die 25 s behind the clock
	 * still reaches 87 C. Over the 21 recorded sessions the same comparison was peak 86.71 C and no second at or above
	 * 87 C with the lead, against 87.58 C and 47.5 s without it. */
	CHECK(flat.peak_mc > cold_now.peak_mc && flat.sec87_ms > cold_now.sec87_ms);
}

/* ---- the soft zone after the safety review (0.7.213) --------------------------------------------- */

/* One tick with the GPU doing work: the zone's gate (zone_may_act) only acts while something is on the ring or the
 * engines, so every case that is about the zone under load has to say so. tick() leaves the ring empty, which is an
 * idle GPU. */
static unsigned int run_busy_mc(struct bc250_dpm_governor *g, unsigned int busy, int temp_mc, unsigned int dt)
{
	struct bc250_dpm_input in = tick(busy, 0, dt);
	unsigned int level;
	in.temperature_mc = temp_mc;
	in.ring_busy = 1;
	level = bc250_dpm_step(g, &in);
	CHECK(level <= g->max_level && level <= g->thermal_cap);
	CHECK(g->thermal_cap >= cap_bottom(g));
	CHECK(g->zone_lead_mc <= BC250_DPM_ZONE_LEAD_MAX_MC);
	bc250_dpm_commit(g, level);
	return level;
}

/* A load lull inside the zone (0.7.213 safety review, finding 1). The GPU still has work outstanding, the die holds
 * 86.2 C, and the load's own answer falls to the lab floor because the frames stopped: a loading screen. The cap has to
 * follow the die at its own pace and never the load. The reviewed code wrote the clamp every tick, so cur took the cap
 * from 1500 to 1000 MHz in one tick and the next three seconds took it to 800, where it stayed for the rest of the
 * session - nothing between the soft-release threshold and the zone's own raises the cap again. */
static void test_zone_dip(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, steps;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = L(5);					/* 1500 MHz, a game start's ceiling on the lab */
	g.thermal_cap = L(5);
	for (i = 0; i < 40u; i++) CHECK(run_busy_mc(&g, 950, 85500, 25) == L(5));
	CHECK(g.zone_ticks == 0 && g.zone_steps == 0 && g.thermal_cap == L(5));

	/* Ten seconds of the lull. The load takes the clock to the lab floor within a second, the zone takes the cap down
	 * one level per zone step from the cap it had, and the two are independent. */
	for (i = 0; i < 400u; i++) run_busy_mc(&g, 10, 86200, 25);
	steps = 10000u / BC250_DPM_ZONE_STEP_MS;
	CHECK(g.zone_steps == steps && g.thermal_cap == L(5) - steps);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_ZONE && !g.hot && g.thermal_events == 0);
	/* The clamp of the entry is all cur ever gave the cap, and that clamp lowered nothing: the cap was at the clock. */
	CHECK(g.want == BC250_DPM_FLOOR_LEVEL && g.thermal_cap > BC250_DPM_THERMAL_FLOOR_LEVEL);

	/* Full load returns and the die, at the lower clock, settles at 84.0 C: the hold band. The cap holds there - no
	 * step down (the zone is above it) and no raise (the soft release is below it) - and the clock follows the cap. */
	for (i = 0; i < 2400u; i++) run_busy_mc(&g, 950, 84000, 25);
	CHECK(g.zone_steps == steps && g.thermal_cap == L(5) - steps && g.soft_releases == 0);
	/* And the episode is still open at 84.0 C, so a return to 86.2 C steps the cap instead of clamping it to the
	 * clock the lull left. */
	CHECK(g.zone == 1);
	for (i = 0; i < 60u; i++) run_busy_mc(&g, 950, 86200, 25);
	CHECK(g.zone_steps == steps + 1u && g.thermal_cap == L(5) - steps - 1u);
	/* Under the soft-release threshold the episode ends and the cap rises again, one level per soft step. */
	for (i = 0; i < 4u * BC250_DPM_SOFT_STEP_MS / 25u; i++) run_busy_mc(&g, 950, 82900, 25);
	CHECK(g.zone == 0 && g.soft_releases >= 3u && g.thermal_cap > L(5) - steps);
}

/* An idle GPU on a die the CPU is holding at 86.0 C (0.7.213 safety review, finding 2). The zone has nothing to take
 * out of it: the clock is at the idle point, a cap stepped down to 800 MHz lowers nothing, and the cap would then stay
 * there until the die fell under 83 C, so the first work of the next burst would run at 800 MHz. The reviewed code
 * pinned the cap at the thermal floor here, in a steady state, with no GPU load at all. */
static void test_zone_idle(void)
{
	struct bc250_dpm_governor g;
	unsigned int i;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(bc250_dpm_idle_config(&g, 500, 3000, 2) == BC250_DPM_IDLE_OK);
	g.level = L(5);
	g.thermal_cap = L(5);
	for (i = 0; i < 4000u; i++) run_mc(&g, 0, 86000, 25);		/* 100 s, the ring empty, 0 permille busy */
	/* Not one step, and not one in the first three seconds either: the quiet timer starts saturated, so a governor
	 * that has seen no work has no window at all (test_zone_quiet_gate has that case on its own). */
	CHECK(g.idle == 1 && g.zone_steps == 0 && g.zone_ticks == 0 && g.thermal_cap == L(5));
	CHECK(g.thermal_cap > BC250_DPM_THERMAL_FLOOR_LEVEL && g.zone_idle_holds > 3000u);
	CHECK(bc250_dpm_level_mhz(g.level) == 500u && g.throttle == BC250_DPM_THROTTLE_IDLE);
	/* Work comes back while the die is still at 86 C. The clock leaves the idle point for the lab floor and no raise
	 * above it is admitted anyway, which is why refusing to act above cost nothing. */
	for (i = 0; i < 40u; i++) run_busy_mc(&g, 1000, 86000, 25);
	CHECK(g.level == BC250_DPM_FLOOR_LEVEL && g.thermal_cap >= L(0));

	/* The same with the idle state off for this start (DpmIdleMHz 0): the gate is the quiet window itself, not the
	 * state, so a start that never configured the idle point is covered too. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = L(5);
	g.thermal_cap = L(5);
	for (i = 0; i < 4000u; i++) run_mc(&g, 0, 86000, 25);
	CHECK(g.idle == 0 && g.zone_steps == 0 && g.zone_ticks == 0 && g.thermal_cap == L(5));
	CHECK(g.thermal_cap > BC250_DPM_THERMAL_FLOOR_LEVEL);
}

/* The same idle GPU on a die the CPU is warming at 0.5 C/s, the shape the recorded sessions show while a game loads.
 * The lead is real here, so the reviewed code entered the zone early, collapsed the cap to 800 MHz and reported a
 * throttle at a raw reading in the seventies. */
static void test_zone_idle_rise(void)
{
	struct bc250_dpm_governor g;
	unsigned int i;
	int mc = 60000;

	/* The ceiling of a game start on the lab, so that the cool half of the ramp has no cap to raise: what this case
	 * watches is the cap coming down, and the release rule's way up is test_zone's business. */
	bc250_dpm_init(&g, L(5));
	(void)bc250_dpm_idle_config(&g, 500, 3000, 2);
	g.level = L(5);
	g.thermal_cap = L(5);
	for (i = 0; i < 2000u; i++, mc += 13) {				/* 50 s, 60.0 to 86.0 C */
		run_mc(&g, 0, mc, 25);
		CHECK(g.thermal_cap > BC250_DPM_THERMAL_FLOOR_LEVEL);
	}
	CHECK(g.idle == 1 && bc250_dpm_level_mhz(g.level) == 500u);
	CHECK(g.zone_steps == 0 && g.thermal_cap == L(5));
	/* And the zone did meet its threshold on the way: 86.0 C with the lead at its cap is 84.0 C of sensor, so the
	 * last two degrees of the ramp are ticks the zone would have acted on. */
	CHECK(g.zone_idle_holds > 0u && g.zone_ticks == 0u);
}

/* The two edges of the zone's work gate: the governor's first ticks, and the far side of a tick whose dt_ms had to be
 * clamped (0.7.213 safety review, second round). A quiet timer counted from zero read "work just now" in both
 * places, so the zone acted on a GPU that nobody had given work to - the symptom of finding 2 by another path. On a die
 * the CPU holds at 86 C the start window took a level off the cap, and with the clock DpmResyncLevel reads from an idle
 * SMU (500 MHz, under the thermal floor) the entry clamp took the cap straight to 800 MHz and held it there through the
 * whole 83 to 86 C band. A 60 s stall with no work bought the same credit again. */
static void test_zone_quiet_gate(void)
{
	struct bc250_dpm_governor g;
	const unsigned int ceiling = (unsigned int)bc250_dpm_level_of(1500);
	unsigned int i, cap0;

	/* A hot die at the first tick this governor ever takes, with no work on either engine. */
	bc250_dpm_init(&g, ceiling);
	CHECK(bc250_dpm_idle_config(&g, 500, 3000, 2) == BC250_DPM_IDLE_OK);
	CHECK(g.zone_quiet_ms >= BC250_DPM_ZONE_QUIET_MS);	/* no tick yet, so no work yet */
	for (i = 0; i < 200u; i++) run_mc(&g, 0, 86000, 25);	/* 5 s at 86.0 C: twice the quiet window */
	CHECK(g.zone_ticks == 0 && g.zone_steps == 0 && g.thermal_cap == ceiling && g.zone_idle_holds > 0u);
	for (i = 0; i < 2400u; i++) run_mc(&g, 0, 84000, 25);	/* and a minute in the hold band changes nothing */
	CHECK(g.thermal_cap == ceiling);
	/* So the first burst of work runs at the lab floor, which is what the die at 86 C admits, and not under it. */
	for (i = 0; i < 40u; i++) run_busy_mc(&g, 1000, 84000, 25);
	CHECK(g.level == BC250_DPM_FLOOR_LEVEL && g.thermal_cap == ceiling && g.zone_quiet_ms == 0);

	/* The worst case of the same window: DpmResyncLevel read 500 MHz from the SMU, so cur is under the thermal floor
	 * and an entry clamp to the running clock would take the cap to the thermal floor in one tick. */
	bc250_dpm_init(&g, ceiling);
	CHECK(bc250_dpm_idle_config(&g, 500, 3000, 2) == BC250_DPM_IDLE_OK);
	g.level = 0u;						/* what DpmResyncLevel writes after reading 500 MHz */
	for (i = 0; i < 200u; i++) run_mc(&g, 0, 86000, 25);
	CHECK(g.zone_ticks == 0 && g.zone_steps == 0 && g.thermal_cap == ceiling);
	for (i = 0; i < 2400u; i++) run_mc(&g, 0, 84000, 25);
	for (i = 0; i < 40u; i++) run_busy_mc(&g, 1000, 84000, 25);
	CHECK(g.level == BC250_DPM_FLOOR_LEVEL && g.thermal_cap == ceiling);

	/* A stall of a minute in which the GPU had no work: the clamped tick counts as a whole quiet window, not as the
	 * clamp, so the far side of it is no window for the zone either. */
	bc250_dpm_init(&g, ceiling);
	g.level = ceiling;
	for (i = 0; i < 400u; i++) run_busy_mc(&g, 950, 70000, 25);
	CHECK(g.zone_quiet_ms == 0);
	cap0 = g.thermal_cap;
	{
		struct bc250_dpm_input in = tick(0, 0, 60000);	/* one clamped tick, 60 s, nothing on either engine */
		in.temperature_mc = 86000;
		bc250_dpm_commit(&g, bc250_dpm_step(&g, &in));
	}
	CHECK(g.zone_quiet_ms >= BC250_DPM_ZONE_QUIET_MS && g.zone_ticks == 0);
	for (i = 0; i < 200u; i++) run_mc(&g, 0, 86000, 25);
	CHECK(g.zone_ticks == 0 && g.zone_steps == 0 && g.thermal_cap == cap0);

	/* Work in the stalled tick itself still says the GPU is busy: the gate is about work, not about the stall. */
	bc250_dpm_init(&g, ceiling);
	g.level = ceiling;
	for (i = 0; i < 400u; i++) run_busy_mc(&g, 950, 70000, 25);
	{
		struct bc250_dpm_input in = tick(1000, 0, 60000);
		in.temperature_mc = 86000;
		in.ring_busy = 1;
		bc250_dpm_commit(&g, bc250_dpm_step(&g, &in));
	}
	CHECK(g.zone_quiet_ms == 0 && g.zone_ticks == 1u && g.zone == 1);
}

/* A tick whose dt_ms had to be clamped (0.7.213 safety review, finding 3). The governor's own clock advances by
 * BC250_DPM_MAX_DT_MS while the die moved on for 30 s, so every age in the ring is short and a slope taken from it is
 * several times the real one: the reviewed code reported a lead of 48 C and put a 78 C die in the zone. */
static void test_zone_stall(void)
{
	struct bc250_dpm_governor g;
	unsigned int i;
	int mc = 60000;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = BC250_DPM_TOP_LEVEL;
	for (i = 0; i < 400u; i++, mc += 5) run_busy_mc(&g, 1000, mc, 25);	/* a full, honest window at 0.2 C/s */
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC && g.slope_count == BC250_DPM_ZONE_SLOPE_SLOTS);
	{
		/* The stalled tick itself reads the window it has, which is the pre-stall slope and nothing to do with the
		 * 30 s: the fit takes the ring's own readings and times, never this tick's reading against a stale slot. */
		struct bc250_dpm_input in = tick(1000, 0, 30000);
		in.temperature_mc = 78000;
		in.ring_busy = 1;
		bc250_dpm_commit(&g, bc250_dpm_step(&g, &in));
		CHECK(g.zone_lead_mc <= BC250_DPM_ZONE_LEAD_MAX_MC && g.zone_ticks == 0 && g.thermal_cap == BC250_DPM_TOP_LEVEL);
		CHECK(g.slope_count == 0 && !g.zone);			/* and the ring is empty afterwards */
	}
	/* No lead at all until a whole fresh window has been measured, so the stall cannot leave an inflated one behind. */
	for (i = 0; i < (BC250_DPM_ZONE_SLOPE_SLOTS - 1u) * BC250_DPM_ZONE_SLOPE_SLOT_MS / 25u; i++) {
		run_busy_mc(&g, 1000, 78000, 25);
		CHECK(g.zone_lead_mc == 0 && g.zone_ticks == 0 && g.thermal_cap == BC250_DPM_TOP_LEVEL);
	}
	CHECK(g.zone_lead_gaps > 0u);

	/* The same stall with the GPU quiet and the cap under the ceiling, which is where the review found it: a quiet,
	 * slowly warming machine, one 30 s stall, and a 78 C die must not be throttled by either rule. */
	bc250_dpm_init(&g, (unsigned int)bc250_dpm_level_of(1500));
	(void)bc250_dpm_idle_config(&g, 500, 3000, 2);
	g.level = (unsigned int)bc250_dpm_level_of(1500);
	g.thermal_cap = (unsigned int)bc250_dpm_level_of(1500);
	mc = 60000;
	for (i = 0; i < 600u; i++, mc += 5) run_mc(&g, 0, mc, 25);
	{
		struct bc250_dpm_input in = tick(0, 0, 30000);
		in.temperature_mc = 78000;
		bc250_dpm_commit(&g, bc250_dpm_step(&g, &in));
	}
	for (i = 0; i < 400u; i++) run_mc(&g, 0, 78000, 25);
	CHECK(g.zone_ticks == 0 && g.zone_steps == 0 && g.thermal_cap == (unsigned int)bc250_dpm_level_of(1500));
}

/* The lead's deadband and its cap (0.7.213 safety review, finding 4), and the noise they exist for. */
static void test_zone_lead_bound(void)
{
	struct bc250_dpm_governor g;
	unsigned int i;
	int mc, dir = 1;

	/* A die rising at 2.68 C/s would extrapolate to some 40 C of lead. The cap holds it at 2.0 C, so the zone's
	 * threshold can never come down by more than that from a raw reading. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = BC250_DPM_TOP_LEVEL;
	mc = 60000;
	for (i = 0; i < 300u; i++, mc += 67) run_busy_mc(&g, 1000, mc, 25);
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC && g.zone_lead_ok);

	/* A die rising under the deadband (0.04 C/s over the window) carries no lead at all: a trend that small is the
	 * sensor's own 0.1 C grid, not a die on its way to 87 C. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = BC250_DPM_TOP_LEVEL;
	mc = 80000;
	for (i = 0; i < 2000u; i++) {
		if (i % 50u == 0u) mc += 1;				/* 0.8 mC/s: far under the deadband */
		run_busy_mc(&g, 1000, mc, 25);
		CHECK(g.zone_lead_mc == 0);
	}
	CHECK(g.zone_lead_ok && g.zone_ticks == 0 && g.thermal_cap == BC250_DPM_TOP_LEVEL);

	/* A flat die whose reading jitters by the sensor's own grid, slot by slot: no lead, so no zone two degrees under
	 * the threshold. The two-point estimator the review measured reported up to 7.8 C here. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = BC250_DPM_TOP_LEVEL;
	for (i = 0; i < 4000u; i++) {
		if (i % 10u == 0u) dir = -dir;
		run_busy_mc(&g, 1000, 84000 + (dir > 0 ? 100 : -100), 25);
		CHECK(g.zone_lead_mc == 0);
	}
	CHECK(g.zone_ticks == 0 && g.zone_steps == 0 && g.thermal_cap == BC250_DPM_TOP_LEVEL);
}

/* A reading that fails now and then (0.7.213 safety review, finding 6). The reviewed code emptied the ring on every
 * failed SmuReadTemperature, so one failure disabled the lead for a whole 5 s window and a failure every 5 s disabled
 * it for good, with nothing in the log but a lead of 0 - which the header itself says means 47.5 s at or above 87 C. */
static void test_zone_sensor_gap(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, gaps;
	int mc = 60000;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = BC250_DPM_TOP_LEVEL;
	for (i = 0; i < 400u; i++, mc += 5) run_busy_mc(&g, 1000, mc, 25);
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC && g.slope_count == BC250_DPM_ZONE_SLOPE_SLOTS);
	gaps = g.zone_lead_gaps;
	{
		/* One failed reading: the slot is skipped, the window stays, and the tick itself reads the raw sensor
		 * (which is the floor rule's own business, not the lead's). */
		struct bc250_dpm_input in = tick(1000, 0, 25);
		in.temperature_valid = 0;
		in.ring_busy = 1;
		bc250_dpm_commit(&g, bc250_dpm_step(&g, &in));
		CHECK(g.slope_count == BC250_DPM_ZONE_SLOPE_SLOTS && g.throttle == BC250_DPM_THROTTLE_SENSOR);
	}
	mc += 5;
	run_busy_mc(&g, 1000, mc, 25);
	/* The failed tick itself counts as a gap - it had no reading, so it had no lead - and the next one has the window
	 * back, which is the whole difference from the reviewed code. */
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC && g.zone_lead_ok && g.zone_lead_gaps == gaps + 1u);

	/* A reading that fails for longer than the ring may span: the lead reports unavailable instead of quietly
	 * extrapolating from readings nobody has refreshed, and the log can count those ticks. */
	for (i = 0; i < BC250_DPM_ZONE_SLOPE_MAX_SPAN_MS / 25u + 40u; i++) {
		struct bc250_dpm_input in = tick(1000, 0, 25);
		in.temperature_valid = 0;
		in.ring_busy = 1;
		bc250_dpm_commit(&g, bc250_dpm_step(&g, &in));
	}
	mc += 5;
	run_busy_mc(&g, 1000, mc, 25);
	CHECK(!g.zone_lead_ok && g.zone_lead_mc == 0 && g.zone_lead_gaps > gaps + 1u);
	/* And a fresh window brings it back. */
	for (i = 0; i < 400u; i++, mc += 5) run_busy_mc(&g, 1000, mc, 25);
	CHECK(g.zone_lead_mc == BC250_DPM_ZONE_LEAD_MAX_MC && g.zone_lead_ok);
}

/* The 87 C backstop is never delayed by a zone step (0.7.213 safety review, finding 7). The zone used to reset cap_ms,
 * which is what the hot entry measures its own first step from, so a zone step 100 ms before the die crossed 87 C
 * deferred that step by a whole hot step. */
static void test_zone_backstop_timing(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, cap_after_zone;

	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	g.level = BC250_DPM_TOP_LEVEL;
	/* In the zone long enough for a step, with the step as recent as a tick. */
	for (i = 0; i < BC250_DPM_ZONE_STEP_MS / 25u + 1u; i++) run_busy_mc(&g, 1000, 86000, 25);
	CHECK(g.zone_steps == 1 && g.zone == 1);
	cap_after_zone = g.thermal_cap;
	CHECK(g.cap_ms >= BC250_DPM_HOT_STEP_MS);	/* the zone left the hot step's own timer alone */
	/* 87.0 C on the sensor: the hot entry steps at once, in the same tick, not a hot step later. */
	run_busy_mc(&g, 1000, 87000, 25);
	CHECK(g.hot && g.thermal_events == 1 && g.thermal_cap == cap_after_zone - 1u);
}

/* The recorded sessions themselves, replayed through the governor (0.7.213 safety review, finding 4). Each sample is
 * held for 40 ticks of 25 ms, which is how the driver sees a 1 s telemetry sample, so the ring carries the sensor's own
 * noise. Open loop: the clock is pinned, because what these cases check is where the zone engages and not where the
 * clock ends up - the closed loop is test_zone436 and the harness in scratch/thermal-zone/review-dpm.
 *
 * The requirement of the review: on the recorded readings the zone never engages below a raw 84.0 C. The reviewed code
 * engaged at 74.8 to 82.9 C on these same traces, on noise alone. */
static void zone_replay(const char *name, const short *deci, unsigned int n, unsigned int *ticks, int *lowest)
{
	struct bc250_dpm_governor g;
	const unsigned int ceiling = (unsigned int)bc250_dpm_level_of(1500);
	unsigned int i, j;

	*lowest = 1000000;
	bc250_dpm_init(&g, ceiling);
	g.level = ceiling;
	g.thermal_cap = ceiling;
	for (i = 0; i < n; i++) {
		int mc = deci[i] * 100;
		for (j = 0; j < 40u; j++) {
			struct bc250_dpm_input in = tick(950, 0, 25);
			unsigned int before = g.zone_ticks;
			in.temperature_mc = mc;
			in.ring_busy = 1;
			(void)bc250_dpm_step(&g, &in);
			bc250_dpm_commit(&g, ceiling);		/* open loop: hold the clock at the ceiling */
			CHECK(g.zone_lead_mc <= BC250_DPM_ZONE_LEAD_MAX_MC);
			if (g.zone_ticks > before && mc < *lowest) *lowest = mc;
		}
	}
	*ticks = g.zone_ticks;
	printf("zone replay %-16s %4u samples, %6u zone ticks, %u steps, lowest raw reading in the zone %d mC\n",
	       name, n, g.zone_ticks, g.zone_steps, g.zone_ticks ? *lowest : 0);
}

static void test_zone_traces(void)
{
	unsigned int ticks;
	int lowest;
	const int bound = BC250_DPM_HOT_MC - (int)BC250_DPM_ZONE_DELTA_MC - BC250_DPM_ZONE_LEAD_MAX_MC;

	CHECK(bound == 84000);		/* the review's requirement, as the three constants put it */

	/* 436, the session of BD-087: the zone does engage, and not one tick of it under the bound. */
	zone_replay("436", zone_trace_436, (unsigned int)(sizeof(zone_trace_436) / sizeof(zone_trace_436[0])),
		    &ticks, &lowest);
	CHECK(ticks > 0u && lowest >= bound);

	/* The four longest segments of recorded readings that go nowhere, from the sessions whose two-point lead was the
	 * largest. The warm, noisy ones must not engage under the bound; the cool one must not engage at all. */
	zone_replay("430 flat", zone_flat_430, (unsigned int)(sizeof(zone_flat_430) / sizeof(zone_flat_430[0])),
		    &ticks, &lowest);
	CHECK(!ticks || lowest >= bound);
	zone_replay("418 flat", zone_flat_418, (unsigned int)(sizeof(zone_flat_418) / sizeof(zone_flat_418[0])),
		    &ticks, &lowest);
	CHECK(!ticks || lowest >= bound);
	zone_replay("412 flat", zone_flat_412, (unsigned int)(sizeof(zone_flat_412) / sizeof(zone_flat_412[0])),
		    &ticks, &lowest);
	CHECK(!ticks || lowest >= bound);
	zone_replay("436 cool", zone_flat_436, (unsigned int)(sizeof(zone_flat_436) / sizeof(zone_flat_436[0])),
		    &ticks, &lowest);
	CHECK(ticks == 0u);
}

/* ---- the thermal ramp (0.7.203, session 367) ----------------------------------------------------- */

/* One step of the governor before 0.7.203, for the A/B below. The ramp is the step's last transform, it only lowers the
 * returned level, and it touches nothing but its own fields (raise_ms, ramp_holds, throttle); without a runtime floor
 * and SetStablePowerState the level it cut is min(want, cap, ceiling), with the throttle the limits named. */
static unsigned int step_without_ramp(struct bc250_dpm_governor *g, const struct bc250_dpm_input *in)
{
	unsigned int level = bc250_dpm_step(g, in);
	if (g->throttle == BC250_DPM_THROTTLE_THERMAL_RAMP) {
		unsigned int limit = g->thermal_cap < g->max_level ? g->thermal_cap : g->max_level;
		level = g->want < limit ? g->want : limit;
		g->throttle = g->want > limit ? (g->thermal_cap < g->max_level ? BC250_DPM_THROTTLE_THERMAL_SOFT :
						 BC250_DPM_THROTTLE_MAX_SETTING) : BC250_DPM_THROTTLE_NONE;
	}
	return level;
}

static void test_ramp(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, level;

	/* The interval: 0 below the knee, 1 s at 70 C, linear to 4 s at the warm zone (87 C from 0.7.204), 4 s above. */
	CHECK(bc250_dpm_ramp_interval_ms(69999) == 0u && bc250_dpm_ramp_interval_ms(-5000) == 0u);
	CHECK(bc250_dpm_ramp_interval_ms(70000) == BC250_DPM_RAMP_MIN_MS);
	CHECK(bc250_dpm_ramp_interval_ms(78500) == (BC250_DPM_RAMP_MIN_MS + BC250_DPM_RAMP_MAX_MS) / 2u);
	CHECK(bc250_dpm_ramp_interval_ms(80000) == 2764u && bc250_dpm_ramp_interval_ms(84000) == 3470u);
	CHECK(bc250_dpm_ramp_interval_ms(85000) == 3647u && bc250_dpm_ramp_interval_ms(86999) < BC250_DPM_RAMP_MAX_MS);
	CHECK(bc250_dpm_ramp_interval_ms(87000) == BC250_DPM_RAMP_MAX_MS && bc250_dpm_ramp_interval_ms(99000) == BC250_DPM_RAMP_MAX_MS);
	for (i = 70000; i < 87000; i += 7) CHECK(bc250_dpm_ramp_interval_ms((int)i) <= bc250_dpm_ramp_interval_ms((int)i + 7));

	/* Below the knee the load raises as before: 1000 -> 1300 -> 1700 -> 2000 in three ticks at 69.999 C. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 69999, 25) == L(3) && run_mc(&g, 1000, 69999, 25) == L(7) && run_mc(&g, 1000, 69999, 25) == L(10));
	CHECK(g.ramp_holds == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* At the knee: the first raise (none before) goes one level, the next waits a full interval. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 70000, 25) == L(1) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP && g.ramp_holds == 1);
	CHECK(g.want == L(3));					/* want stays the load's own answer */
	for (i = 1; i < BC250_DPM_RAMP_MIN_MS / 25u; i++) CHECK(run_mc(&g, 1000, 70000, 25) == L(1));
	CHECK(g.ramp_holds == BC250_DPM_RAMP_MIN_MS / 25u && g.raises == 1);
	CHECK(run_mc(&g, 1000, 70000, 25) == L(2) && g.raises == 2);
	/* At 84.9 C the interval is 3629 ms: 145 ticks hold (3625 ms), the 146th raises. */
	for (i = 1; i < bc250_dpm_ramp_interval_ms(84900) / 25u; i++) CHECK(run_mc(&g, 1000, 84900, 25) == L(2));
	CHECK(run_mc(&g, 1000, 84900, 25) == L(2));
	CHECK(run_mc(&g, 1000, 84900, 25) == L(3) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP);
	/* A raise of one level is no hold: the load asks 2000 at 1900 MHz, the ramp lets it through. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(9);
	CHECK(run_mc(&g, 915, 80000, 25) == L(10) && g.ramp_holds == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* A raise below the knee starts the interval too: a reading at 70 C 25 ms later waits for the rest of it. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 69000, 25) == L(3));
	for (i = 1; i < BC250_DPM_RAMP_MIN_MS / 25u; i++) CHECK(run_mc(&g, 1000, 70000, 25) == L(3));
	CHECK(run_mc(&g, 1000, 70000, 25) == L(4));

	/* Lowering is never held: idle at 80 C steps down after the usual hold. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(8); g.raise_ms = 0;
	for (i = 0; i < 7; i++) CHECK(run_mc(&g, 0, 80000, 25) == L(8));
	CHECK(run_mc(&g, 0, 80000, 25) == L(7) && g.ramp_holds == 0);

	/* The hot band (and the warm zone with it, 87 C from 0.7.204) names its own reason; the ramp adds no hold there. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(4) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.ramp_holds == 0);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(4) && g.ramp_holds == 0 && g.warm_holds == 0);

	/* The runtime floor is a raise too: one level per interval above the knee. */
	{
		struct bc250_dpm_tune t = tune(900, 800, 650, 200, L(10));
		init_old(&g, BC250_DPM_TOP_LEVEL);
		CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
		CHECK(run_mc(&g, 0, 78000, 25) == L(1) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP);
		CHECK(run_mc(&g, 0, 60000, 25) == L(10));	/* below the knee: at once */
	}

	/* A stalled tick counts as MAX_DT_MS, so one stall does not open a raise. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 84000, 25) == L(1));
	CHECK(run_mc(&g, 1000, 84000, 3600000u) == L(1));
	for (i = 0; i < 400; i++) level = run_mc(&g, 1000, 84000, 25);
	CHECK(level == L(4));	/* 1 s of the stall + 10 s at a 3.47 s interval: three more raises */
}

/* Session 367 on a plant: the governor at 1000 MHz with the hot spot at 75.7 C after an earlier load (the sink still
 * warm), ceiling 2000 MHz, then a game whose work fills 93.1 % of 2000 MHz for ten minutes. The plant has the same two
 * nodes as plant_run with constants fitted to the lab instead of to the scene-change shape: the hot spot's fast node
 * (3 s, 16 C of the full-power rise) gains 12 C in 5 s at 2000 MHz / 1000 mV from 75.7 C, as in session 367, and the
 * steady state at full load is 82.8 C at 1300 MHz, 86.0 C at 1400 and 89.5 C at 1500, so 1300-1400 MHz at 84-86 C, as
 * in sessions 361-365 with the 1500 MHz ceiling. Ambient 56.6 C, sink 90 s and 36 C, sensor noise +-0.4 C. */
struct ramp_result {
	unsigned int peak_mc, die_peak_mc, hot_ms, longest_hot_ms, first_hot_ms, final_mhz, reach_ms, settle_ms, mean_mhz;
	unsigned int raises, lowers;
	unsigned int ramp_holds, warm_holds, thermal_events;
	unsigned int stops, stop_phases, longest_1s;	/* the lab runner's 1 s samples at 87 C or more (plant367_run) */
};

static struct ramp_result plant367_run(int ramp, int start_mc)
{
	struct bc250_dpm_governor g;
	struct ramp_result r;
	static unsigned char trace[600000u / 25u];	/* the level after each tick */
	double die = start_mc / 1000.0, sink = die - 1.1;	/* 1.1 C: the fast node at 1000 MHz, 7 % busy */
	unsigned long long mhz_sum = 0;
	unsigned int i, n = 0, seed = 367u, last, hot_run = 0, run1s[40] = { 0 };
	unsigned long long stop_phases = 0;
	const unsigned int ticks = 600000u / 25u, demand = 1862u;

	memset(&r, 0, sizeof(r));
	r.first_hot_ms = ~0u;
	init_old(&g, BC250_DPM_TOP_LEVEL);	/* session 367 is the ramp against the 0.7.212 thermal rules */
	last = g.level;
	for (i = 0; i < ticks; i++) {
		unsigned int mhz = bc250_dpm_level_mhz(g.level), busy = demand >= mhz ? 1000u : demand * 1000u / mhz, level;
		double v = bc250_dpm_level_mv(g.level) / 1000.0;
		double p = mhz / 2000.0 * v * v * (0.15 + 0.85 * busy / 1000.0);
		struct bc250_dpm_input in = tick(busy, 0, 25);
		seed = seed * 1103515245u + 12345u;
		sink += (56.6 + 36.0 * p - sink) * 0.025 / 90.0;
		die += (sink + 16.0 * p - die) * 0.025 / 3.0;
		in.temperature_mc = (int)(die * 1000.0) + (int)((seed >> 16) % 801u) - 400;
		level = ramp ? bc250_dpm_step(&g, &in) : step_without_ramp(&g, &in);
		CHECK(level <= g.max_level && level <= g.thermal_cap);
		bc250_dpm_commit(&g, level);
		if (g.level != last) { r.settle_ms = i * 25u; last = g.level; }
		if (in.temperature_mc >= BC250_DPM_HOT_MC) {
			if (r.first_hot_ms == ~0u) r.first_hot_ms = i * 25u;
			r.hot_ms += 25u;
			hot_run += 25u;
			if (hot_run > r.longest_hot_ms) r.longest_hot_ms = hot_run;
		} else hot_run = 0;
		/* The lab runner samples once a second, at a phase it does not choose: every one of the 40 phases of the
		 * 25 ms tick counts. A stop is three samples in a row at 87 C or more, counted once per run; stops sums them
		 * over the phases, stop_phases counts the phases with at least one. */
		{
			unsigned int *c = &run1s[i % 40u];
			*c = in.temperature_mc >= BC250_DPM_HOT_MC ? *c + 1u : 0u;
			if (*c == 3u) {
				r.stops++;
				stop_phases |= 1ull << (i % 40u);
			}
			if (*c > r.longest_1s) r.longest_1s = *c;
		}
		if (in.temperature_mc > (int)r.peak_mc) r.peak_mc = (unsigned int)in.temperature_mc;
		if (die * 1000.0 > r.die_peak_mc) r.die_peak_mc = (unsigned int)(die * 1000.0);
		trace[i] = (unsigned char)g.level;
		if (i >= 300000u / 25u) { mhz_sum += bc250_dpm_level_mhz(g.level); n++; }
	}
	for (i = 0; i < 40u; i++) if (stop_phases >> i & 1u) r.stop_phases++;
	r.final_mhz = bc250_dpm_level_mhz(g.level);
	for (i = 0; trace[i] != g.level; i++) {}
	r.reach_ms = i * 25u;
	r.mean_mhz = (unsigned int)(mhz_sum / n);
	r.raises = g.raises;
	r.lowers = g.lowers;
	r.ramp_holds = ramp ? g.ramp_holds : 0u;	/* the old rule's cuts were undone */
	r.warm_holds = g.warm_holds;
	r.thermal_events = g.thermal_events;
	return r;
}

static void ramp_print(const char *name, const struct ramp_result *r)
{
	printf("plant367 %-9s peak %u mC (hot spot %u mC), first >= 87 C at %d ms, %u ms >= 87 C (longest %u ms), 1 s "
	       "samples: longest run %u, stops %u in %u of 40 phases, final %u MHz (first at %u ms, last change at %u ms), "
	       "mean %u MHz (min 5-10), %u raises %u lowers, %u hot entries, %u warm holds, %u ramp holds\n",
	       name, r->peak_mc, r->die_peak_mc, r->first_hot_ms == ~0u ? -1 : (int)r->first_hot_ms, r->hot_ms,
	       r->longest_hot_ms, r->longest_1s, r->stops, r->stop_phases, r->final_mhz, r->reach_ms, r->settle_ms,
	       r->mean_mhz, r->raises, r->lowers, r->thermal_events, r->warm_holds, r->ramp_holds);
}

static void test_plant367(void)
{
	struct ramp_result old = plant367_run(0, 75700), now = plant367_run(1, 75700);
	ramp_print("old rule:", &old);
	ramp_print("ramp:", &now);
	/* Without the ramp: 2000 MHz in three ticks, the hot spot past 87 C within 5 s and the reading at 87 C or more for
	 * about 2 s without a break. The lab runner (87 C in three samples 1 s apart) stops it at 18 of the 40 sample
	 * phases. The hot steps then latch the cap at 1300 MHz: no reading falls under 82 C again. */
	CHECK(old.first_hot_ms < 10000u && old.longest_hot_ms >= 1000u && old.die_peak_mc >= (unsigned int)BC250_DPM_HOT_MC);
	CHECK(old.stop_phases >= 10u && old.final_mhz == 1300u);
	/* The ramp: the hot spot stays under 87 C, no reading reaches 87 C in the climb (the first 20 s), no two 1 s samples
	 * in a row reach it, and the clock settles at 1400 MHz, the highest level whose steady state is under 87 C. The sink
	 * is still under its steady state at 1400 MHz when a reading under the warm zone lets one more level through:
	 * 1500 MHz then drifts to 87 C on the sink's 90 s scale, a reading touches 87.0 C for single ticks (sensor noise on
	 * a hot spot under 87 C) and the hot rule takes the level back at once. No rule that reads only the temperature of
	 * the tick sees that drift coming. With the warm zone at 87 C (0.7.204) the drift gets one more level than at 85 C
	 * more often; the hot rule, not the warm zone, then ends the climb. */
	CHECK(now.die_peak_mc < (unsigned int)BC250_DPM_HOT_MC && now.peak_mc < (unsigned int)BC250_DPM_HOT_MC + 500u);
	CHECK(now.first_hot_ms >= 20000u && now.longest_hot_ms <= 50u && now.longest_1s <= 1u && now.stops == 0u);
	CHECK(now.final_mhz == 1400u && now.mean_mhz == 1400u && now.reach_ms <= 15000u && now.settle_ms <= 120000u);
	/* Every start from 50 C (cold) to 83 C. Cold starts reach 2000 MHz below the knee and meet the band on the sink's
	 * slow drift, which the hot rule answers one level at a time: there the two rules behave the same. From 74 C the
	 * old rule's jump puts the hot spot past 87 C, from 75 C the runner stops it. The ramp keeps the hot spot under
	 * 87 C and gives the runner no stop for every start. From 79 C a start ends at 1300 MHz: the overshoot to 1500 MHz
	 * comes with a sink still hot from the earlier load, and the cap it costs is released only under 82 C. At 84 C the
	 * sink alone (from the earlier load) puts the hot spot over 87 C at 1000 MHz, so no clock rule decides there. */
	{
		int start;
		unsigned int old_hot = 0, old_stop = 0, at1300 = 0, worst = 0, worst_1s = 0, worst_ms = 0;
		for (start = 50000; start <= 83000; start += 1000) {
			struct ramp_result a = plant367_run(0, start), b = plant367_run(1, start);
			if (a.die_peak_mc >= (unsigned int)BC250_DPM_HOT_MC) old_hot++;
			if (a.stops) old_stop++;
			if (b.final_mhz == 1300u) at1300++;
			if (b.die_peak_mc > worst) worst = b.die_peak_mc;
			if (b.longest_1s > worst_1s) worst_1s = b.longest_1s;
			if (b.longest_hot_ms > worst_ms) worst_ms = b.longest_hot_ms;
			CHECK(b.die_peak_mc < (unsigned int)BC250_DPM_HOT_MC && b.stops == 0u && b.longest_1s <= 2u);
			CHECK(b.longest_hot_ms <= 125u && b.final_mhz >= 1300u && b.mean_mhz >= a.mean_mhz);
			if (start >= 74000) CHECK(a.die_peak_mc >= (unsigned int)BC250_DPM_HOT_MC);
			if (start >= 75000) CHECK(a.stops > 0u);
			if (start < BC250_DPM_RAMP_KNEE_MC - 1000) CHECK(a.die_peak_mc == b.die_peak_mc && a.mean_mhz == b.mean_mhz);
		}
		printf("plant367 starts 50-83 C: old rule hot spot >= 87 C in %u of 34, runner stop in %u; ramp: peak hot spot "
		       "%u mC, longest run of readings >= 87 C %u ms, of 1 s samples %u, no runner stop, %u starts end at "
		       "1300 MHz, the others at 1400\n", old_hot, old_stop, worst, worst_ms, worst_1s, at1300);
	}
}

/* ---- the thermal sub-floor (0.7.205, owner decision 2026-10-05) --------------------------------- */

/* Two levels below the lab floor, and no more (a constant CHECK would trip C4127 under /WX). */
typedef char dpm_subfloor_is_two_levels[(L(-2) == BC250_DPM_THERMAL_FLOOR_LEVEL) ? 1 : -1];

/* RotTR scene 2 held 88 C with the governor already at its 1000 MHz floor and the GPU 97 % busy, so the
 * thermal cap gained two levels below the lab floor: 900 and 800 MHz, both at the floor's own 820 mV. The
 * load never asks for them, SetStablePowerState and a missing sensor stay at the lab floor, and the KMD can
 * withdraw them for a start (bc250_dpm_subfloor_refused) because the firmware has never been seen there
 * (facts M47). Nie ma dymu bez ognia - there is no smoke without fire; here we would rather have neither. */
static void test_subfloor(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_input in;
	struct bc250_dpm_tune t;
	unsigned int i, level, temp, busy;

	/* The two points themselves: a lower clock at the floor's voltage, no undervolt. */
	CHECK(bc250_dpm_level_mhz(L(-1)) == 900u && bc250_dpm_level_mhz(L(-2)) == 800u);
	CHECK(bc250_dpm_level_mv(L(-1)) == bc250_dpm_level_mv(L(0)));
	CHECK(bc250_dpm_level_mv(L(-2)) == bc250_dpm_level_mv(L(0)));

	/* A sustained 88 C at the lab floor under full load: the hot entry steps to 900 MHz at once, the next hot
	 * step to 800 MHz, and there it stays (no underflow, however long it lasts). */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(g.level == L(0) && g.subfloor_ok);
	CHECK(run_mc(&g, 1000, 88000, 25) == L(-1) && g.thermal_cap == L(-1) && g.thermal_events == 1);
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run_mc(&g, 1000, 88000, 25) == L(-1));
	CHECK(run_mc(&g, 1000, 88000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 2000u; i++) CHECK(run_mc(&g, 1000, 88000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL && g.thermal_events == 1 && g.lowers == 2);
	/* The load's own answer stays at the lab floor: the telemetry shows what holds the clock, not a demand
	 * nobody made. */
	CHECK(g.want == L(0));

	/* The release comes back the same way: one level per RELEASE_STEP_MS, 800 -> 900 -> 1000 -> up. Below the
	 * ramp's knee, so that the clock follows the cap at once (above it the ramp spaces the raises, test_ramp). */
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++)
		CHECK(run_mc(&g, 0, 60000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(run_mc(&g, 0, 60000, 25) == L(-1) && g.thermal_cap == L(-1));
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++) CHECK(run_mc(&g, 0, 60000, 25) == L(-1));
	CHECK(run_mc(&g, 0, 60000, 25) == L(0) && g.thermal_cap == L(0));
	for (i = 0; i < 20u * BC250_DPM_RELEASE_STEP_MS / 25u; i++) level = run_mc(&g, 0, 60000, 25);
	CHECK(level == L(0) && g.thermal_cap == BC250_DPM_TOP_LEVEL);	/* idle: the cap is free, the load is not */

	/* 90 C: 800 MHz at once, from anywhere. */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	CHECK(run(&g, 1000, 90, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL && g.throttle == BC250_DPM_THROTTLE_THERMAL_HARD);
	CHECK(g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL);

	/* The load never goes below the lab floor, at any temperature and any busy share: only the cap does, and
	 * what the step returns is below the floor only while the cap is. */
	for (temp = 40000; temp <= 95000; temp += 500)
		for (busy = 0; busy <= 1000u; busy += 100u) {
			init_old(&g, BC250_DPM_TOP_LEVEL);
			for (i = 0; i < 400u; i++) {
				level = run_mc(&g, busy, (int)temp, 25);
				CHECK(g.want >= L(0));
				CHECK(level >= L(0) || g.thermal_cap < L(0));
				CHECK(level >= BC250_DPM_THERMAL_FLOOR_LEVEL);
			}
			/* Under the hot threshold nothing drops below the lab floor. */
			if (temp < (unsigned int)BC250_DPM_HOT_MC) CHECK(g.level >= L(0));
		}

	/* A missing sensor and SetStablePowerState ask for the lab floor, but the missing-sensor rule is a clamp and
	 * never a raise: blind, the driver owns the point this part is known to run at, and a cap already below the
	 * floor stays where it is. The reading the KMD leaves in the tick when SmuReadTemperature fails is the last
	 * good one, so a bare "go to the floor" would let one blind tick discard both hot steps and walk the clock
	 * back up through the warm zone (the release below RELEASE_MC is the only way up). */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 90000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	in = tick(1000, 60, 25); in.temperature_valid = 0;
	for (i = 0; i < 400u; i++) {
		level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
		CHECK(level == BC250_DPM_THERMAL_FLOOR_LEVEL && g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL);
		CHECK(g.throttle == BC250_DPM_THROTTLE_SENSOR);
	}
	/* From the floor or above it the rule is the lowering it has always been (0.7.204 behaviour). */
	init_old(&g, BC250_DPM_TOP_LEVEL); g.level = L(5); g.thermal_cap = L(5);
	level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	CHECK(level == L(0) && g.thermal_cap == L(0) && g.throttle == BC250_DPM_THROTTLE_SENSOR);
	/* Only the release brings a sub-floor cap back up, and it does so one level at a time. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 90000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 4u; i++) { level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level); }
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++)
		CHECK(run_mc(&g, 1000, 60000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(run_mc(&g, 1000, 60000, 25) == L(-1) && g.thermal_cap == L(-1));
	init_old(&g, BC250_DPM_TOP_LEVEL); g.stable = 1;
	CHECK(run_mc(&g, 1000, 88000, 25) == L(-1));		/* the cap is lower than the floor it asks for */
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	g.stable = 0;
	for (i = 0; i < 20u * BC250_DPM_RELEASE_STEP_MS / 25u; i++) run_mc(&g, 0, 60000, 25);
	g.stable = 1;
	CHECK(run_mc(&g, 1000, 60000, 25) == L(0) && g.throttle == BC250_DPM_THROTTLE_STABLE);

	/* A runtime floor is never admitted below the lab floor (the points below it are the cap's). */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, L(-1));
	CHECK(bc250_dpm_tune_check(&t, BC250_DPM_TOP_LEVEL) == BC250_DPM_TUNE_FLOOR);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && g.tune.floor_level == L(0));
	t.floor_level = BC250_DPM_THERMAL_FLOOR_LEVEL;
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && g.tune.floor_level == L(0));
	t.floor_level = L(0);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);

	/* The refusal: the KMD could not put the hardware at a sub-floor point. From then on the cap stops at the
	 * lab floor for the rest of the start, whatever the temperature, and a cap already below it is raised. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 88000, 25) == L(-1) && g.thermal_cap == L(-1));
	bc250_dpm_subfloor_refused(&g);
	CHECK(!g.subfloor_ok && g.subfloor_refusals == 1 && g.thermal_cap == L(0));
	g.level = L(0);					/* the KMD applied the lab floor instead */
	for (i = 0; i < 4000u; i++) {
		level = run_mc(&g, 1000, (i & 1u) ? 88000 : 91000, 25);
		CHECK(level == L(0) && g.thermal_cap == L(0));
	}
	/* And a refusal before any hot tick keeps the whole start at or above the lab floor. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	bc250_dpm_subfloor_refused(&g);
	CHECK(g.thermal_cap == BC250_DPM_TOP_LEVEL);		/* nothing to raise: the cap was not below the floor */
	for (i = 0; i < 4000u; i++) CHECK(run_mc(&g, 1000, 89000, 25) >= L(0));
	CHECK(g.thermal_cap == L(0) && g.level == L(0));
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

/* ---- the idle state (0.7.207, owner decision 2026-10-05) ---------------------------------------- */

/* The idle point is the table's first level, five below the lab floor (a constant CHECK would trip C4127). */
typedef char dpm_idle_is_level_zero[(L(-5) == BC250_DPM_IDLE_LEVEL && BC250_DPM_IDLE_LEVEL == 0u) ? 1 : -1];

/* One tick with the ring's state and the paging node's share named: the idle state is the only rule that reads
 * either of them. */
static unsigned int run_nodes(struct bc250_dpm_governor *g, unsigned int busy, int temp_mc, unsigned int dt,
			      int ring_busy, unsigned int sdma_permille)
{
	struct bc250_dpm_input in = tick(busy, 0, dt);
	unsigned int level;
	in.temperature_mc = temp_mc;
	in.ring_busy = ring_busy;
	in.sdma_permille = sdma_permille;
	level = bc250_dpm_step(g, &in);
	CHECK(level <= g->max_level && level <= g->thermal_cap);
	CHECK(g->thermal_cap >= cap_bottom(g));
	bc250_dpm_commit(g, level);
	return level;
}

/* The same with an idle paging node, which is what most of the cases below describe. */
static unsigned int run_ring(struct bc250_dpm_governor *g, unsigned int busy, int temp_mc, unsigned int dt,
			     int ring_busy)
{
	return run_nodes(g, busy, temp_mc, dt, ring_busy, 0u);
}

/* One tick with no temperature reading at all. */
static unsigned int run_blind(struct bc250_dpm_governor *g, unsigned int busy, unsigned int dt)
{
	struct bc250_dpm_input in = tick(busy, 60, dt);
	unsigned int level;
	in.temperature_valid = 0;
	level = bc250_dpm_step(g, &in);
	bc250_dpm_commit(g, level);
	return level;
}

/* A governor with the idle state on, as the KMD configures it from the registry defaults. */
static void idle_init(struct bc250_dpm_governor *g, unsigned int max_level)
{
	init_old(g, max_level);		/* the idle state against the 0.7.212 thermal rules; test_zone has the zone */
	CHECK(bc250_dpm_idle_config(g, BC250_DPM_IDLE_MHZ, BC250_DPM_IDLE_HOLD_MS,
				    BC250_DPM_IDLE_BUSY_PERMILLE) == BC250_DPM_IDLE_OK);
	CHECK(g->idle_on && !g->idle && bc250_dpm_idle_mhz(g) == 500u);
}

/* Quiet ticks; returns the tick (1-based) the state entered on, 0 when it did not. */
static unsigned int quiet_until_idle(struct bc250_dpm_governor *g, unsigned int ticks, int temp_mc)
{
	unsigned int i, entered = 0;
	for (i = 1; i <= ticks; i++) {
		unsigned int level = run_ring(g, 0, temp_mc, 25, 0);
		if (level == g->idle_level && !entered) entered = i;
		/* Until the state enters, nothing may go under the lab floor (the load's own lowest level); the
		 * clock may still be coming down from where the caller left it. */
		if (!entered) CHECK(level >= L(0));
	}
	return entered;
}

static void test_idle(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_tune t;
	unsigned int i, level, wake, hold = BC250_DPM_IDLE_HOLD_MS / 25u;

	/* The setting itself. 0 is "off" and not an error; anything that is not a table clock under the lab
	 * floor, and any hold or share out of range, is refused and leaves the state off. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(bc250_dpm_idle_config(&g, 0, 0, 0) == BC250_DPM_IDLE_OK && !g.idle_on);
	CHECK(bc250_dpm_idle_mhz(&g) == 0u);
	CHECK(bc250_dpm_idle_config(&g, 1000, 3000, 2) == BC250_DPM_IDLE_CLOCK && !g.idle_on);
	CHECK(bc250_dpm_idle_config(&g, 2000, 3000, 2) == BC250_DPM_IDLE_CLOCK);
	CHECK(bc250_dpm_idle_config(&g, 450, 3000, 2) == BC250_DPM_IDLE_CLOCK);
	CHECK(bc250_dpm_idle_config(&g, 550, 3000, 2) == BC250_DPM_IDLE_CLOCK);	/* not on the 100 MHz grid */
	CHECK(bc250_dpm_idle_config(&g, 500, BC250_DPM_IDLE_MIN_HOLD_MS - 1u, 2) == BC250_DPM_IDLE_HOLD);
	CHECK(bc250_dpm_idle_config(&g, 500, BC250_DPM_IDLE_MAX_HOLD_MS + 1u, 2) == BC250_DPM_IDLE_HOLD);
	CHECK(bc250_dpm_idle_config(&g, 500, 3000, BC250_DPM_IDLE_MAX_BUSY_PERMILLE + 1u) == BC250_DPM_IDLE_BUSY);
	CHECK(!g.idle_on && bc250_dpm_idle_mhz(&g) == 0u);
	/* Every point under the lab floor is admitted as the idle point, the thermal cap's two included. */
	CHECK(bc250_dpm_idle_config(&g, 900, 3000, 2) == BC250_DPM_IDLE_OK && bc250_dpm_idle_mhz(&g) == 900u);
	CHECK(bc250_dpm_idle_config(&g, 500, BC250_DPM_IDLE_MAX_HOLD_MS, 0) == BC250_DPM_IDLE_OK);

	/* The state off (DpmIdleMHz 0) is 0.7.205 behaviour exactly: ten seconds of a quiet GPU stay at the
	 * lab floor, and nothing of the state moves. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	for (i = 0; i < 400u; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == L(0));
	CHECK(!g.idle && g.idle_entries == 0 && g.idle_exits == 0 && g.idle_total_ms == 0 && g.lowers == 0);

	/* Entry: a quiet window of the hold time, then the idle point, one lowering, the throttle names it. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	CHECK(g.idle && g.idle_entries == 1 && g.idle_exits == 0 && g.lowers == 1);
	CHECK(g.throttle == BC250_DPM_THROTTLE_IDLE && g.level == BC250_DPM_IDLE_LEVEL);
	CHECK(bc250_dpm_level_mhz(g.level) == 500u && bc250_dpm_level_mv(g.level) == BC250_CLOCK_FLOOR_MV);
	/* It stays there, and the time at the point is counted from the tick after the entry (the caller's apply
	 * takes the clock there, so the entry tick itself was not spent at the point). */
	for (i = 0; i < 400u; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL);
	CHECK(g.idle_total_ms == 400u * 25u && g.idle_entries == 1 && g.lowers == 1);

	/* Exit: one busy tick returns to the lab floor at once, whatever the load would ask for; the governor
	 * goes on from the floor at the next tick (1000 -> 1300 at full load). */
	CHECK(run_ring(&g, 1000, 60000, 25, 0) == L(0));
	CHECK(!g.idle && g.idle_exits == 1 && g.raises == 1);
	CHECK(run_ring(&g, 1000, 60000, 25, 0) == L(3));

	/* The exit threshold is the tick's own share and not the window's admitted mean (0.7.207, review): one
	 * active GRBM sample inside a 25 ms tick is 40 permille, twenty times the 2 permille mean the entry
	 * window admits, so a per-tick exit at the entry threshold left the state at the very frame the mean rule
	 * tolerates. A minute of a static desktop that wakes once per second must hold the point, with one entry,
	 * no exit and one transition of the clock. */
	for (wake = 500u; wake <= 3000u; wake += 500u) {
		unsigned int at_point = 0, ticks = 60000u / 25u, every = wake / 25u;
		idle_init(&g, BC250_DPM_TOP_LEVEL);
		for (i = 1; i <= ticks; i++)
			if (run_ring(&g, (i % every) == 0u ? 1000u / 25u : 0u, 45000, 25, 0) == BC250_DPM_IDLE_LEVEL)
				at_point++;
		CHECK(g.idle && g.idle_entries == 1 && g.idle_exits == 0);
		CHECK(g.raises + g.lowers == 1 && at_point > ticks - 2u * hold);
	}
	/* A tick at the exit share leaves in that tick, even with both rings empty: that is work the ring
	 * accounting cannot see, and half a tick of it is no desktop frame. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	CHECK(run_ring(&g, BC250_DPM_IDLE_EXIT_PERMILLE - 1u, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL && g.idle);
	CHECK(run_ring(&g, BC250_DPM_IDLE_EXIT_PERMILLE, 60000, 25, 0) == L(0) && !g.idle && g.idle_exits == 1);
	/* Between the two thresholds the trailing window decides (0.7.216.6): a share over the admitted entry mean but
	 * under the leave share is work the point serves, and it holds for as long as it lasts. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	for (i = 0; i < 10u * hold; i++)
		CHECK(run_ring(&g, BC250_DPM_IDLE_BUSY_PERMILLE + 50u, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL);
	for (i = 0; i < 10u * hold; i++)
		CHECK(run_ring(&g, BC250_DPM_IDLE_LEAVE_PERMILLE - 1u, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL);
	CHECK(g.idle && g.idle_exits == 0 && g.idle_entries == 1);
	/* A share at the leave share leaves when the window's work reaches it, which for exactly that share is the
	 * window's last tick: not at once and not never. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	for (i = 1; i < hold; i++)
		CHECK(run_ring(&g, BC250_DPM_IDLE_LEAVE_PERMILLE, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL);
	CHECK(run_ring(&g, BC250_DPM_IDLE_LEAVE_PERMILLE, 60000, 25, 0) == L(0));
	CHECK(!g.idle && g.idle_exits == 1 && g.idle_slow_exits == 1 && g.idle_fast_exits == 0);
	/* A higher share spends the window's budget sooner: 200 permille leaves at 150 / 200 of the window. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	for (i = 1; i < hold * BC250_DPM_IDLE_LEAVE_PERMILLE / 200u; i++)
		CHECK(run_ring(&g, 200u, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL);
	CHECK(run_ring(&g, 200u, 60000, 25, 0) == L(0) && g.idle_slow_exits == 1);
	/* The paging node (SDMA0) is work as well, and the GFX ring is empty throughout a transfer on it: no
	 * entry while it runs, and an exit at the first tick that sees it. A single sample of it is tolerated,
	 * exactly as a GFX one is. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 0; i < 4u * hold; i++) CHECK(run_nodes(&g, 0, 60000, 25, 0, 1000u) == L(0));
	CHECK(!g.idle && g.idle_entries == 0);
	CHECK(quiet_until_idle(&g, 2u * hold, 60000) > 0);
	CHECK(run_nodes(&g, 0, 60000, 25, 0, 1000u) == L(0) && !g.idle && g.idle_exits == 1);
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 1; i < hold; i++) CHECK(run_nodes(&g, 0, 60000, 25, 0, i == 5u ? 40u : 0u) == L(0));
	CHECK(run_ring(&g, 0, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL && g.idle_entries == 1);

	/* A submission with no hardware busy share of its own (a tick waiting on a fence) no longer leaves the state
	 * (0.7.216.6): the point holds while its work fits it, and a submission is not work by itself. Outside the
	 * state the ring still keeps the window from starting, which is the entry rule of 0.7.207, unchanged. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	for (i = 0; i < 400u; i++) CHECK(run_ring(&g, 0, 60000, 25, 1) == BC250_DPM_IDLE_LEVEL);
	CHECK(g.idle && g.idle_exits == 0);
	CHECK(run_ring(&g, 1000, 60000, 25, 1) == L(0) && !g.idle && g.idle_exits == 1 && g.idle_fast_exits == 1);
	for (i = 0; i < 400u; i++) CHECK(run_ring(&g, 0, 60000, 25, 1) == L(0));
	CHECK(!g.idle && g.idle_entries == 1 && g.idle_ms == 0);
	/* With the ring empty again the next full window enters. */
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);

	/* The window is a mean, not a strict zero: one tick of a 40 permille sample (one active GRBM sample of
	 * 25) inside a window still enters, because 40 x 25 / 3000 = 0.33 permille is under the admitted 2. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 1; i < hold; i++) CHECK(run_ring(&g, i == 5u ? 40u : 0u, 60000, 25, 0) == L(0));
	CHECK(run_ring(&g, 0, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL && g.idle_entries == 1);

	/* A window whose mean is too high starts again: a full busy tick every hold time keeps the clock at the
	 * lab floor for as long as that goes on. Once it stops, the state enters within two hold times, which
	 * is the worst case of the rule. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 0; i < 10u * hold; i++) {
		level = run_ring(&g, (i % hold) == 0u ? 1000u : 0u, 60000, 25, 0);
		CHECK(level >= L(0));
	}
	CHECK(!g.idle && g.idle_entries == 0);
	CHECK(quiet_until_idle(&g, 2u * hold, 60000) > 0);
	CHECK(g.idle && g.idle_entries == 1);

	/* A runtime floor owns the clock while it is set: no idle. Clearing it lets the state run again. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, L(5));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	for (i = 0; i < 4u * hold; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == L(5));
	CHECK(!g.idle && g.idle_entries == 0);
	t.floor_level = BC250_DPM_FLOOR_LEVEL;
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(quiet_until_idle(&g, 2u * hold, 60000) > 0 && g.idle);

	/* SetStablePowerState: one steady clock, so no idle, and it leaves the state at once. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	g.stable = 1;
	CHECK(run_ring(&g, 0, 60000, 25, 0) == L(0) && !g.idle && g.throttle == BC250_DPM_THROTTLE_STABLE);
	for (i = 0; i < 4u * hold; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == L(0));
	CHECK(g.idle_entries == 1 && g.idle_exits == 1);
	g.stable = 0;
	CHECK(quiet_until_idle(&g, 2u * hold, 60000) > 0);

	/* The thermal rules win. At 87 C and above no entry (the hot cap is stepping); a critical reading and a
	 * missing reading keep the clock where those rules put it. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 0; i < 4u * hold; i++) CHECK(run_ring(&g, 0, 87000, 25, 0) >= BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(!g.idle && g.idle_entries == 0);
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 0; i < 4u * hold; i++) CHECK(run_ring(&g, 0, 90000, 25, 0) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(!g.idle && g.idle_entries == 0);
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 0; i < 4u * hold; i++) CHECK(run_blind(&g, 0, 25) == L(0));
	CHECK(!g.idle && g.idle_entries == 0);
	/* A sensor that fails while the clock is at the idle point: out of the state, back to the lab floor,
	 * which is the point the blind rule owns. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	CHECK(run_blind(&g, 0, 25) == L(0) && !g.idle && g.idle_exits == 1);

	/* The exit is not cut by the warm zone or the thermal ramp: at 86 C, above the ramp knee, the clock goes
	 * from 500 MHz to the lab floor in one step, not to 600 MHz. It does not start the ramp's interval
	 * again either, so the first real raise is spaced by the ramp's own timing from the lab floor. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	CHECK(run_ring(&g, 1000, 86000, 25, 0) == L(0) && g.ramp_holds == 0 && g.warm_holds == 0);
	CHECK(g.raise_ms >= BC250_DPM_RAMP_MAX_MS);
	CHECK(run_ring(&g, 1000, 86000, 25, 0) == L(1));

	/* The same for an idle point that is not 500 MHz (0.7.207, review). The exit is immediate because the
	 * state held the clock, not because the clock is under the thermal floor: a configured 800 MHz point, and
	 * the 800 MHz the fallback picks after the firmware refuses 500 MHz, both return to the lab floor in one
	 * tick at 80 C, where the ramp interval is 2.8 s and would otherwise give 900 MHz. */
	for (i = 0; i < 2u; i++) {
		init_old(&g, BC250_DPM_TOP_LEVEL);
		if (i == 0) {
			CHECK(bc250_dpm_idle_config(&g, 800, BC250_DPM_IDLE_HOLD_MS,
						    BC250_DPM_IDLE_BUSY_PERMILLE) == BC250_DPM_IDLE_OK);
		} else {
			CHECK(bc250_dpm_idle_config(&g, 500, BC250_DPM_IDLE_HOLD_MS,
						    BC250_DPM_IDLE_BUSY_PERMILLE) == BC250_DPM_IDLE_OK);
			bc250_dpm_idle_refused(&g);
		}
		CHECK(g.idle_level == BC250_DPM_THERMAL_FLOOR_LEVEL && bc250_dpm_idle_mhz(&g) == 800u);
		CHECK(quiet_until_idle(&g, 2u * hold, 80000) > 0);
		CHECK(g.level == BC250_DPM_THERMAL_FLOOR_LEVEL && g.ramp_holds == 0);
		CHECK(run_ring(&g, 1000, 80000, 25, 1) == L(0));
		CHECK(!g.idle && g.ramp_holds == 0 && g.raise_ms >= BC250_DPM_RAMP_MAX_MS);
	}
	/* The cap's own sub-floor keeps the ramp, though: a cap that walked down to 800 MHz under load comes back
	 * level by level, as it did in 0.7.205, and not in the one tick an idle exit takes (the idle state must
	 * not change that). */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	for (i = 0; i < 400u; i++) run_ring(&g, 1000, 88000, 25, 1);
	CHECK(g.level == BC250_DPM_THERMAL_FLOOR_LEVEL && g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL && !g.idle);
	CHECK(run_ring(&g, 1000, 80000, 25, 1) == BC250_DPM_THERMAL_FLOOR_LEVEL);	/* the cap still holds it */
	level = 0;
	for (i = 1; i <= 400u && level < L(0); i++) level = run_ring(&g, 1000, 80000, 25, 1);
	CHECK(level == L(0) && i * 25u > BC250_DPM_RELEASE_STEP_MS && g.ramp_holds > 0);

	/* A reading of 87 C or more while the clock is at the idle point: the thermal cap steps from the lab
	 * floor, never from the idle point (0.7.207, review). A cap at the idle point would hold a loaded GPU at
	 * 500 MHz, a point nothing has measured under load, until the release below 82 C. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	g.cap_ms = 0;					/* the cap moved just now: the hot entry takes its clamp */
	CHECK(run_ring(&g, 0, 87500, 25, 0) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL && g.hot && !g.idle);
	for (i = 0; i < 4000u; i++) level = run_ring(&g, 1000, 85000, 25, 1);
	CHECK(level == BC250_DPM_THERMAL_FLOOR_LEVEL && g.want == L(0));
	/* And with the sub-floor already refused the same entry steps to the lab floor. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	bc250_dpm_subfloor_refused(&g);
	g.cap_ms = 0;
	CHECK(run_ring(&g, 0, 87500, 25, 0) == L(0) && g.thermal_cap == L(0));

	/* The idle point is never above the limits (0.7.207, review): a cap holding a part at 800 MHz wins over a
	 * configured 900 MHz point, so the state lowers the clock and never raises it. */
	init_old(&g, BC250_DPM_TOP_LEVEL);
	CHECK(bc250_dpm_idle_config(&g, 900, BC250_DPM_IDLE_HOLD_MS, BC250_DPM_IDLE_BUSY_PERMILLE) == BC250_DPM_IDLE_OK);
	for (i = 0; i < 400u; i++) run_ring(&g, 1000, 88000, 25, 1);	/* the cap walks down to 800 MHz */
	CHECK(g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 8u * hold; i++) {
		level = run_ring(&g, 0, 84000, 25, 0);		/* above RELEASE_MC: the cap holds */
		CHECK(level <= g.thermal_cap);
	}
	CHECK(g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL && level == BC250_DPM_THERMAL_FLOOR_LEVEL);

	/* An exit while the part is critical goes to the thermal floor, not above it. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	CHECK(run_ring(&g, 1000, 90000, 25, 0) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(!g.idle && g.throttle == BC250_DPM_THROTTLE_THERMAL_HARD);

	/* The firmware refuses the point: the fallback is 500 MHz, then the thermal floor (800 MHz), then off.
	 * A refused point is not asked for again. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	bc250_dpm_idle_refused(&g);				/* the caller could not apply 500 MHz */
	CHECK(!g.idle && g.idle_refusals == 1 && bc250_dpm_idle_mhz(&g) == 800u);
	CHECK(g.idle_level == BC250_DPM_THERMAL_FLOOR_LEVEL);
	g.level = BC250_DPM_FLOOR_LEVEL;			/* the KMD put the lab floor back */
	CHECK(quiet_until_idle(&g, 2u * hold, 60000) > 0);
	CHECK(g.level == BC250_DPM_THERMAL_FLOOR_LEVEL && g.idle_entries == 2);
	bc250_dpm_idle_refused(&g);				/* 800 MHz refused too: nothing below the floor */
	CHECK(!g.idle && !g.idle_on && g.idle_refusals == 2 && bc250_dpm_idle_mhz(&g) == 0u);
	/* The second step is the thermal floor itself, which the cap asks for with the same two messages: the cap
	 * loses it as well, so no rule tries a point under the lab floor again for this start. */
	CHECK(!g.subfloor_ok && g.subfloor_refusals == 1 && g.thermal_cap >= BC250_DPM_FLOOR_LEVEL);
	for (i = 0; i < 400u; i++) CHECK(run_ring(&g, 1000, 91000, 25, 1) == L(0));
	g.level = BC250_DPM_FLOOR_LEVEL;
	for (i = 0; i < 4u * hold; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == L(0));
	CHECK(g.idle_entries == 2);

	/* A refused thermal sub-floor takes the idle state with it: the same firmware, the same refusal. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	bc250_dpm_subfloor_refused(&g);
	CHECK(!g.idle && bc250_dpm_idle_mhz(&g) == 0u);
	g.level = BC250_DPM_FLOOR_LEVEL;
	for (i = 0; i < 4u * hold; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == L(0));
	CHECK(g.idle_entries == 1);

	/* A ceiling below the default changes nothing: the idle point is below every ceiling. The hold and the
	 * admitted share are the setting's, so a 500 ms hold enters after 500 ms. */
	init_old(&g, L(5));
	CHECK(bc250_dpm_idle_config(&g, 500, 500, 2) == BC250_DPM_IDLE_OK);
	for (i = 0; i < 500u / 25u - 1u; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == L(0));
	CHECK(run_ring(&g, 0, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL && g.max_level == L(5));
	/* A stop or a power transition takes the hardware off the point outside a tick: it counts as an exit, and
	 * the next entry needs a whole quiet window again (the KMD calls this from DpmStop and DpmPause). */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	bc250_dpm_idle_leave(&g);
	CHECK(!g.idle && g.idle_exits == 1 && g.idle_ms == 0 && g.idle_on);
	bc250_dpm_idle_leave(&g);			/* again: no second exit counted */
	CHECK(g.idle_exits == 1);
	g.level = BC250_DPM_FLOOR_LEVEL;		/* the KMD put the lab floor back */
	for (i = 1; i < hold; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == L(0));
	CHECK(run_ring(&g, 0, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL && g.idle_entries == 2);

	/* The session marker reads the idle point as "not above the floor": nothing to carry into a next start,
	 * and a stop from there is clean. */
	{
		struct bc250_dpm_session s;
		memset(&s, 0, sizeof(s));
		CHECK(bc250_dpm_session_step(&s, BC250_DPM_IDLE_LEVEL, 25) == BC250_DPM_SESSION_NONE);
		s.marked = 1;
		for (i = 0; i < BC250_DPM_SESSION_CLEAR_MS / 25u - 1u; i++)
			CHECK(bc250_dpm_session_step(&s, BC250_DPM_IDLE_LEVEL, 25) == BC250_DPM_SESSION_NONE);
		CHECK(bc250_dpm_session_step(&s, BC250_DPM_IDLE_LEVEL, 25) == BC250_DPM_SESSION_CLEAR);
	}
}

/* The leave share lies above the default entry mean and every admitted leave share under the fast exit's share. */
typedef char dpm_idle_leave_order[(BC250_DPM_IDLE_BUSY_PERMILLE < BC250_DPM_IDLE_LEAVE_PERMILLE &&
				   BC250_DPM_IDLE_MIN_LEAVE_PERMILLE <= BC250_DPM_IDLE_LEAVE_PERMILLE &&
				   BC250_DPM_IDLE_LEAVE_PERMILLE <= BC250_DPM_IDLE_MAX_LEAVE_PERMILLE &&
				   BC250_DPM_IDLE_MAX_LEAVE_PERMILLE < BC250_DPM_IDLE_EXIT_PERMILLE) ? 1 : -1];

/* A small deterministic generator for the traces below (Numerical Recipes' LCG): the same trace on every run. */
static unsigned int trace_rand(unsigned int *seed)
{
	*seed = *seed * 1664525u + 1013904223u;
	return *seed >> 8;
}

/* The idle point as a DPM level with hysteresis (0.7.216.6). The lab record that asked for it: with the desktop
 * composed on our GPU, every DWM frame put work on the GFX ring, the ring rule left the idle point at each one, and
 * the owner watched 500 -> 1000 -> 500 MHz every two seconds at an idle desktop (29 exits in 6.5 minutes when quiet).
 *   1. A desktop: one short frame every 0.5 to 2 s, 1 to 2 ticks of 40 to 160 permille with the ring busy in those
 *      ticks, for 6.5 minutes. Entered once, never left, one transition of the clock.
 *   2. The same desktop from the lab floor with the ring seen in one frame of eight: the entry rule (unchanged) still
 *      finds its quiet window, and from then on the point holds.
 *   3. A game: the first busy tick leaves, whatever the ring says, and the next tick is the load's own raise.
 *   4. A steady 30 % load leaves within one hold time, wherever in the trailing window it starts, ring or not.
 *   5. The setting DpmIdleLeavePermille: its bounds, and that it must be above the admitted entry mean. */
static void test_idle_desktop(void)
{
	struct bc250_dpm_governor g;
	unsigned int i, offset, ticks, frames, at_point, hold = BC250_DPM_IDLE_HOLD_MS / 25u;
	unsigned int seed, next, burst, share;
	const unsigned int minutes65 = 390000u / 25u;	/* 6.5 minutes of ticks */

	/* 1. From the idle point, 6.5 minutes of a waking desktop. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 45000) == hold);
	seed = 1u;
	next = (500u + trace_rand(&seed) % 1501u) / 25u;
	burst = 0;
	share = 0;
	frames = at_point = 0;
	for (i = 0; i < minutes65; i++) {
		int ring = 0;
		unsigned int busy = 0;
		if (burst) {
			ring = 1;
			busy = share;
			burst--;
		} else if (next-- == 0u) {
			frames++;
			burst = 1u + trace_rand(&seed) % 2u;		/* 1 or 2 ticks */
			share = 40u + trace_rand(&seed) % 121u;		/* 40 to 160 permille */
			ring = 1;
			busy = share;
			burst--;
			next = (500u + trace_rand(&seed) % 1501u) / 25u;
		}
		if (run_ring(&g, busy, 45000, 25, ring) == BC250_DPM_IDLE_LEVEL) at_point++;
	}
	CHECK(frames > 190u);					/* about one frame every 1.25 s */
	CHECK(g.idle && g.idle_entries == 1 && g.idle_exits == 0 && at_point == minutes65);
	CHECK(g.raises == 0 && g.lowers == 1);

	/* 2. From the lab floor. The ring is seen in one frame of eight, which is what a 25 ms tick sampling a 2 ms
	 * frame sees: the entry needs one window with no ring sample and a mean under 2 permille, and then holds. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	seed = 7u;
	next = (500u + trace_rand(&seed) % 1501u) / 25u;
	frames = at_point = 0;
	for (i = 0; i < minutes65; i++) {
		int ring = 0;
		unsigned int busy = 0;
		if (next-- == 0u) {
			frames++;
			busy = 40u;					/* one active sample of the 25 a tick holds */
			ring = (trace_rand(&seed) % 8u) == 0u;
			next = (500u + trace_rand(&seed) % 1501u) / 25u;
		}
		if (run_ring(&g, busy, 45000, 25, ring) == BC250_DPM_IDLE_LEVEL) at_point++;
	}
	CHECK(g.idle && g.idle_entries == 1 && g.idle_exits == 0);
	CHECK(at_point > minutes65 * 9u / 10u && g.raises + g.lowers == 1);

	/* 3. A game from the idle point: the first busy tick returns to the lab floor in that tick, with the ring busy
	 * or not, and the load raises from there at the next tick. A burst on the paging node alone does the same. */
	for (i = 0; i < 3u; i++) {
		idle_init(&g, BC250_DPM_TOP_LEVEL);
		CHECK(quiet_until_idle(&g, hold, 60000) == hold);
		if (i < 2u) CHECK(run_ring(&g, 950, 60000, 25, (int)i) == L(0));
		else CHECK(run_nodes(&g, 0, 60000, 25, 0, 950u) == L(0));
		CHECK(!g.idle && g.idle_exits == 1 && g.idle_fast_exits == 1);
		if (i < 2u) CHECK(run_ring(&g, 950, 60000, 25, 1) > L(0));
	}

	/* 4. A steady 30 % load, started at every tick of the trailing window: it leaves within one hold time, and
	 * through the window rule, not the fast share. */
	for (offset = 0; offset < hold; offset++) {
		int ring = (int)(offset & 1u);
		idle_init(&g, BC250_DPM_TOP_LEVEL);
		CHECK(quiet_until_idle(&g, hold, 60000) == hold);
		for (i = 0; i < offset; i++) CHECK(run_ring(&g, 0, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL);
		for (ticks = 1; ticks <= 2u * hold; ticks++)
			if (run_ring(&g, 300, 60000, 25, ring) != BC250_DPM_IDLE_LEVEL) break;
		CHECK(ticks <= hold && !g.idle && g.idle_slow_exits == 1 && g.idle_fast_exits == 0);
		CHECK(g.level == L(0));
	}

	/* 5. The setting. The default is in place after bc250_dpm_idle_config; the bounds and the entry mean refuse, and a
	 * refusal turns the state off, as every refused idle setting does. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(g.idle_leave_permille == BC250_DPM_IDLE_LEAVE_PERMILLE);
	CHECK(bc250_dpm_idle_set_leave(&g, BC250_DPM_IDLE_MIN_LEAVE_PERMILLE) == BC250_DPM_IDLE_OK && g.idle_on);
	CHECK(bc250_dpm_idle_set_leave(&g, BC250_DPM_IDLE_MAX_LEAVE_PERMILLE) == BC250_DPM_IDLE_OK && g.idle_on);
	CHECK(bc250_dpm_idle_set_leave(&g, BC250_DPM_IDLE_MAX_LEAVE_PERMILLE + 1u) == BC250_DPM_IDLE_LEAVE && !g.idle_on);
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(bc250_dpm_idle_set_leave(&g, BC250_DPM_IDLE_MIN_LEAVE_PERMILLE - 1u) == BC250_DPM_IDLE_LEAVE && !g.idle_on);
	CHECK(bc250_dpm_idle_config(&g, 500, 3000, 50) == BC250_DPM_IDLE_OK);
	CHECK(bc250_dpm_idle_set_leave(&g, 50) == BC250_DPM_IDLE_LEAVE && !g.idle_on);	/* not above the mean */
	CHECK(bc250_dpm_idle_config(&g, 500, 3000, 50) == BC250_DPM_IDLE_OK);
	CHECK(bc250_dpm_idle_set_leave(&g, 51) == BC250_DPM_IDLE_OK && g.idle_on && g.idle_leave_permille == 51u);
	/* A configured share is the one in force: 250 permille holds a 200 permille load that the default leaves. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(bc250_dpm_idle_set_leave(&g, 250) == BC250_DPM_IDLE_OK);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	for (i = 0; i < 10u * hold; i++) CHECK(run_ring(&g, 200, 60000, 25, 1) == BC250_DPM_IDLE_LEVEL);
	CHECK(g.idle && g.idle_exits == 0);
}

/* ---- the joint power arm (C62, 0.7.216.7) ------------------------------------------------------- */

typedef char joint_min_is_the_cpu_floor[(BC250_JOINT_MIN_MHZ == 2800u && BC250_JOINT_STEP_MHZ == 200u &&
					 BC250_JOINT_FREE_PERMILLE < BC250_JOINT_BOUND_PERMILLE &&
					 BC250_JOINT_ENGAGE_MS >= 2000u && BC250_JOINT_FREE_MS >= 2000u &&
					 BC250_JOINT_STEP_MS >= BC250_JOINT_ENGAGE_MS &&
					 BC250_JOINT_COOL_MS > BC250_JOINT_STEP_MS) ? 1 : -1];

static struct bc250_joint_input jin(unsigned int busy, int heat, int cool, unsigned int base)
{
	struct bc250_joint_input in;
	memset(&in, 0, sizeof(in));
	in.busy_permille = busy;
	in.heat = heat;
	in.cool = cool;
	in.cpu_ready = 1;
	in.base_mhz = base;
	in.dt_ms = 25;
	return in;
}

/* Ticks of one input until the cap changes, or limit ticks; returns the ticks taken (limit when it did not change). */
static unsigned int joint_until_change(struct bc250_joint *j, const struct bc250_joint_input *in, unsigned int limit)
{
	unsigned int i, before = j->cap_mhz;
	for (i = 1; i <= limit; i++)
		if (bc250_joint_step(j, in) != before) return i;
	return limit;
}

static void test_joint(void)
{
	struct bc250_joint j;
	struct bc250_joint_input in;
	const unsigned int base = 3200u;	/* unit A's recorded baseline (facts M817) */
	const unsigned int engage = BC250_JOINT_ENGAGE_MS / 25u, step = BC250_JOINT_STEP_MS / 25u;
	const unsigned int freeTicks = BC250_JOINT_FREE_MS / 25u, cool = BC250_JOINT_COOL_MS / 25u;
	unsigned int i;

	bc250_joint_init(&j);
	CHECK(j.cap_mhz == 0 && j.reason == BC250_JOINT_FREE);

	/* Bound and hot: nothing for ENGAGE_MS, then base - one step, in the tick that completes it. */
	in = jin(900, 1, 0, base);
	for (i = 1; i < engage; i++) CHECK(bc250_joint_step(&j, &in) == 0 && j.reason == BC250_JOINT_WAIT);
	CHECK(bc250_joint_step(&j, &in) == 3000u && j.reason == BC250_JOINT_CAPPING && j.engages == 1);
	/* Then one step per STEP_MS, down to MIN_MHZ and no further. */
	CHECK(joint_until_change(&j, &in, 10u * step) == step && j.cap_mhz == 2800u);
	CHECK(joint_until_change(&j, &in, 10u * step) == 10u * step && j.cap_mhz == 2800u);
	CHECK(j.reason == BC250_JOINT_HOLD && j.steps_down == 2);

	/* Not bound any more: the whole cap goes after FREE_MS, and a dip shorter than that changes nothing. */
	in = jin(500, 0, 0, base);
	for (i = 1; i < freeTicks; i++) CHECK(bc250_joint_step(&j, &in) == 2800u);
	in = jin(900, 1, 0, base);
	CHECK(bc250_joint_step(&j, &in) == 2800u && j.free_ms == 0);
	in = jin(500, 0, 0, base);
	CHECK(joint_until_change(&j, &in, 10u * freeTicks) == freeTicks && j.cap_mhz == 0);
	CHECK(j.releases == 1 && j.reason == BC250_JOINT_FREE);

	/* Bound without heat never engages, and neither does heat without a bound GPU (the bound share is exact). */
	in = jin(1000, 0, 0, base);
	CHECK(joint_until_change(&j, &in, 100u * engage) == 100u * engage && j.cap_mhz == 0);
	in = jin(BC250_JOINT_BOUND_PERMILLE - 1u, 1, 0, base);
	CHECK(joint_until_change(&j, &in, 100u * engage) == 100u * engage && j.cap_mhz == 0);
	in = jin(BC250_JOINT_BOUND_PERMILLE, 1, 0, base);
	CHECK(joint_until_change(&j, &in, 100u * engage) == engage && j.cap_mhz == 3000u);
	/* An interrupted bound window starts again. */
	bc250_joint_reset(&j);
	CHECK(j.cap_mhz == 0 && j.releases == 2);
	in = jin(900, 1, 0, base);
	for (i = 1; i < engage; i++) (void)bc250_joint_step(&j, &in);
	in = jin(700, 1, 0, base);
	CHECK(bc250_joint_step(&j, &in) == 0 && j.bound_ms == 0);
	in = jin(900, 1, 0, base);
	CHECK(joint_until_change(&j, &in, 10u * engage) == engage);

	/* Cool: one step back up per COOL_MS, and the step that reaches base is the release. Bound and neither hot nor
	 * cool holds without a timer. */
	CHECK(joint_until_change(&j, &in, 10u * step) == step && j.cap_mhz == 2800u);
	in = jin(900, 0, 0, base);
	CHECK(joint_until_change(&j, &in, 100u * cool) == 100u * cool && j.cap_mhz == 2800u);
	CHECK(j.step_ms == 0 && j.cool_ms == 0 && j.reason == BC250_JOINT_HOLD);
	in = jin(900, 0, 1, base);
	CHECK(joint_until_change(&j, &in, 10u * cool) == cool && j.cap_mhz == 3000u && j.reason == BC250_JOINT_RAISING);
	CHECK(joint_until_change(&j, &in, 10u * cool) == cool && j.cap_mhz == 0 && j.steps_up == 1);
	CHECK(j.releases == 3);

	/* Blind: everything holds, the cap and the timers alike. */
	in = jin(900, 1, 0, base);
	for (i = 0; i < engage / 2u; i++) (void)bc250_joint_step(&j, &in);
	in.blind = 1;
	in.heat = 0;
	for (i = 0; i < 10u * engage; i++) CHECK(bc250_joint_step(&j, &in) == 0 && j.reason == BC250_JOINT_BLIND);
	in = jin(900, 1, 0, base);
	CHECK(joint_until_change(&j, &in, 10u * engage) == engage - engage / 2u);
	in.blind = 1;
	for (i = 0; i < 10u * step; i++) CHECK(bc250_joint_step(&j, &in) == 3000u);

	/* The CPU surface not ready: the cap goes at once, and coming back needs a whole window. */
	in = jin(900, 1, 0, base);
	in.cpu_ready = 0;
	CHECK(bc250_joint_step(&j, &in) == 0 && j.reason == BC250_JOINT_NO_CPU && j.releases == 4);
	in.cpu_ready = 1;
	CHECK(joint_until_change(&j, &in, 10u * engage) == engage);

	/* A base under the cap releases it; a base at MIN_MHZ has no room; 2900 gets the floor as its first cap. */
	in = jin(900, 1, 0, 3000u);
	CHECK(bc250_joint_step(&j, &in) == 0 && j.reason == BC250_JOINT_FREE);
	in = jin(900, 1, 0, BC250_JOINT_MIN_MHZ);
	CHECK(joint_until_change(&j, &in, 10u * engage) == 10u * engage && j.cap_mhz == 0);
	CHECK(j.reason == BC250_JOINT_NO_ROOM);
	in = jin(900, 1, 0, 2900u);
	CHECK(joint_until_change(&j, &in, 10u * engage) == engage && j.cap_mhz == 2800u);
	CHECK(joint_until_change(&j, &in, 10u * step) == 10u * step);
	/* A long tick counts as BC250_DPM_MAX_DT_MS, so a stall cannot jump the windows. */
	bc250_joint_reset(&j);
	in = jin(900, 1, 0, base);
	in.dt_ms = 60000u;
	CHECK(bc250_joint_step(&j, &in) == 0 && bc250_joint_step(&j, &in) == 3000u);

	/* Random inputs, ready and a fixed base: the cap is 0 or inside MIN..base - STEP, a lowering only ever follows a
	 * bound and hot tick, a raise a cool one, a release a free one or a cool one, and two changes are never closer
	 * than the shortest window (ENGAGE_MS and FREE_MS). */
	{
		unsigned int seed = 12345u, last = 0, changes = 0, sinceChange = 1000000u, minGap = 1000000u;
		bc250_joint_init(&j);
		for (i = 0; i < 2000000u; i++) {
			unsigned int r, cap;
			seed = seed * 1103515245u + 12345u;
			r = seed >> 8;
			in = jin((r % 4u == 0u) ? 400u : (r % 4u == 1u) ? 700u : 920u, (r >> 3) % 3u == 0u, 0, base);
			in.cool = !in.heat && (r >> 5) % 2u == 0u;
			/* Inputs stay put for a while, as a scene does: a fresh draw every 1..8 s. */
			{
				unsigned int k, hold = 40u + (r >> 9) % 280u;
				for (k = 0; k < hold && i < 2000000u; k++, i++) {
					cap = bc250_joint_step(&j, &in);
					sinceChange += 25u;
					CHECK(cap == 0 || (cap >= BC250_JOINT_MIN_MHZ && cap <= base - BC250_JOINT_STEP_MHZ));
					if (cap != last) {
						if (cap && (cap < last || !last)) CHECK(in.busy_permille >= 850u && in.heat);
						if (cap > last && last) CHECK(in.cool);
						if (!cap) CHECK(in.busy_permille < 600u || in.cool);
						if (sinceChange < minGap) minGap = sinceChange;
						sinceChange = 0;
						changes++;
						last = cap;
					}
				}
			}
		}
		CHECK(changes > 1000u && minGap >= BC250_JOINT_ENGAGE_MS);
		printf("joint arm fuzz: %u cap changes, the closest two %u ms apart, %u engages %u releases\n", changes,
		       minGap, j.engages, j.releases);
	}
}

/* The arm's inputs out of the governor: heat, cool and blind as bc250_joint_read derives them. */
static void test_joint_read(void)
{
	struct bc250_dpm_governor g;
	struct bc250_dpm_input in;
	struct bc250_joint_input out;
	const unsigned int ceiling = (unsigned int)bc250_dpm_level_of(1500);

	bc250_dpm_init(&g, ceiling);
	g.avg_permille = 900;
	in = tick(900, 80, 25);
	bc250_joint_read(&g, &in, &out);
	CHECK(out.busy_permille == 900 && !out.heat && out.cool && !out.blind && out.dt_ms == 25);
	in.temperature_mc = 81000;		/* at the cool line: not cool, and not hot either */
	bc250_joint_read(&g, &in, &out);
	CHECK(!out.heat && !out.cool);
	in.temperature_mc = 80999;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.cool);
	/* The soft zone and an 87 C episode are heat at any reading. */
	g.zone = 1;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.heat && !out.cool);
	g.zone = 0;
	g.hot = 1;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.heat && !out.cool);
	g.hot = 0;
	/* A lowered cap: heat at or above the soft-release threshold (83 C by default), the release at work under it. */
	g.thermal_cap = ceiling - 1u;
	in.temperature_mc = (int)BC250_DPM_HOT_MC - (int)g.tune.soft_delta_mc;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.heat && !out.cool);
	in.temperature_mc -= 1;
	bc250_joint_read(&g, &in, &out);
	CHECK(!out.heat && !out.cool);
	in.temperature_mc = 70000;
	bc250_joint_read(&g, &in, &out);
	CHECK(!out.heat && !out.cool);		/* a lowered cap is never cool */
	/* With the soft release off, the hot cap's own release threshold. */
	g.tune.soft_delta_mc = 0;
	in.temperature_mc = BC250_DPM_RELEASE_MC;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.heat);
	in.temperature_mc = BC250_DPM_RELEASE_MC - 1;
	bc250_joint_read(&g, &in, &out);
	CHECK(!out.heat);
	g.thermal_cap = ceiling;
	/* A thermal throttle is heat; the other throttles are not. */
	g.throttle = BC250_DPM_THROTTLE_THERMAL_WARM;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.heat);
	g.throttle = BC250_DPM_THROTTLE_THERMAL_ZONE;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.heat);
	g.throttle = BC250_DPM_THROTTLE_MAX_SETTING;
	bc250_joint_read(&g, &in, &out);
	CHECK(!out.heat);
	g.throttle = BC250_DPM_THROTTLE_THERMAL_RAMP;	/* the ramp slows raises from 70 C: not a held clock */
	bc250_joint_read(&g, &in, &out);
	CHECK(!out.heat);
	/* No reading: blind, never hot and never cool, whatever the governor's state says. */
	g.zone = 1;
	in.temperature_valid = 0;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.blind && !out.heat && !out.cool);
	in.temperature_valid = 1;
	in.dt_ms = 60000;
	bc250_joint_read(&g, &in, &out);
	CHECK(out.dt_ms == BC250_DPM_MAX_DT_MS);
}

/* The arm on the governor, closed loop on the GPU clock, over recorded Tctl readings at a bound GPU: session 436
 * (BD-087, 89.2 C) must engage it, and only once the governor's clock was held by heat; the cool segment of the same
 * session must never engage it. The readings are replayed as recorded, so the CPU cap has no effect on them: this
 * shows when the arm acts, not what it buys. */
static void joint_replay(const char *name, const short *deci, unsigned int n, unsigned int *engages, int *first_mc)
{
	struct bc250_dpm_governor g;
	struct bc250_joint j;
	const unsigned int ceiling = (unsigned int)bc250_dpm_level_of(1500);
	unsigned int i, k, steps = 0;

	*first_mc = 0;
	bc250_dpm_init(&g, ceiling);
	g.level = ceiling;
	g.thermal_cap = ceiling;
	bc250_joint_init(&j);
	for (i = 0; i < n; i++) {
		for (k = 0; k < 40u; k++) {
			struct bc250_dpm_input in = tick(950, 0, 25);
			struct bc250_joint_input ji;
			unsigned int before = j.cap_mhz, level;
			in.temperature_mc = deci[i] * 100;
			in.ring_busy = 1;
			level = bc250_dpm_step(&g, &in);
			bc250_dpm_commit(&g, level);
			bc250_joint_read(&g, &in, &ji);
			ji.cpu_ready = 1;
			ji.base_mhz = 3200u;
			(void)bc250_joint_step(&j, &ji);
			if (j.cap_mhz != before) steps++;
			if (!before && j.cap_mhz && !*first_mc) *first_mc = deci[i] * 100;
		}
	}
	*engages = j.engages;
	printf("joint replay %-10s %4u samples: %u engages, %u cap changes, %u down %u up %u releases, first at %d mC\n",
	       name, n, j.engages, steps, j.steps_down, j.steps_up, j.releases, *first_mc);
}

static void test_joint_traces(void)
{
	unsigned int engages;
	int first;
	joint_replay("436", zone_trace_436, (unsigned int)(sizeof(zone_trace_436) / sizeof(zone_trace_436[0])),
		     &engages, &first);
	CHECK(engages > 0u && first >= 83000);
	joint_replay("436 cool", zone_flat_436, (unsigned int)(sizeof(zone_flat_436) / sizeof(zone_flat_436[0])),
		     &engages, &first);
	CHECK(engages == 0u);
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
	test_idle();
	test_idle_desktop();
	test_joint();
	test_joint_read();
	test_joint_traces();
	test_curve();
	test_tune_check();
	test_tune_no_oscillation();
	test_floor();
	test_warm();
	test_reentry();
	test_soft_release();
	test_plant();
	test_zone();
	test_zone436();
	test_zone_dip();
	test_zone_idle();
	test_zone_idle_rise();
	test_zone_quiet_gate();
	test_zone_stall();
	test_zone_lead_bound();
	test_zone_sensor_gap();
	test_zone_backstop_timing();
	test_zone_traces();
	test_ramp();
	test_plant367();
	test_subfloor();
	printf("dpm policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
