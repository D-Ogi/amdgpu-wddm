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

/* A level counted from the lab floor, which is where the load lives: L(0) is 1000 MHz, L(10) the 2000 MHz
 * ceiling, the two thermal-only points of 0.7.205 are L(-1) (900 MHz) and L(-2) (800 MHz,
 * BC250_DPM_THERMAL_FLOOR_LEVEL), and L(-5) == 0 is the idle point of 0.7.207 (500 MHz,
 * BC250_DPM_IDLE_LEVEL). Up to 0.7.204 the lab floor was index 0 and these tests wrote the index
 * itself; L() keeps every expectation below written in the same numbers. */
#define L(n) ((unsigned int)((int)BC250_DPM_FLOOR_LEVEL + (n)))

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
		CHECK(!bc250_clock_point_allowed(p->mhz, p->mv - 1u));
		CHECK(!bc250_clock_point_allowed(p->mhz, BC250_CLOCK_CEILING_MV + 1u));
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(-1) && g.thermal_cap == L(-1));
	CHECK(bc250_dpm_level_mhz(L(-1)) == 900u && bc250_dpm_level_mv(L(-1)) == 820u);
	for (i = 1; i < BC250_DPM_HOT_STEP_MS / 25u; i++) CHECK(run(&g, 1000, 89, 25) == L(-1));
	CHECK(run(&g, 1000, 89, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(bc250_dpm_level_mhz(BC250_DPM_THERMAL_FLOOR_LEVEL) == 800u);
	for (i = 0; i < 100; i++) CHECK(run(&g, 1000, 89, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(g.want == L(0));		/* the load never asks below the lab floor */

	/* No reading is treated as critical. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(7);
	in = tick(1000, 60, 25); in.temperature_valid = 0;
	level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	CHECK(level == L(0) && g.throttle == BC250_DPM_THROTTLE_SENSOR);

	/* Thermal wins over a max setting, the max setting over the load. */
	bc250_dpm_init(&g, L(6));
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

static void test_tune_check(void)
{
	struct bc250_dpm_tune t, d;
	struct bc250_dpm_governor g;
	unsigned int up, target, down, compared = 0;

	bc250_dpm_tune_default(&d);
	CHECK(d.up_permille == 900u && d.target_permille == 800u && d.down_permille == 650u && d.down_hold_ms == 200u &&
	      d.floor_level == L(0));
	CHECK(d.hot_step_ms == 500u && d.soft_delta_mc == 0u && d.soft_step_ms == 3000u);
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
	t = d; t.soft_delta_mc = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 1; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 499; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 500; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 4500; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_delta_mc = 4501; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_delta_mc = 0x80000000u; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_step_ms = 1999; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	t = d; t.soft_step_ms = 2000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_step_ms = 30000; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_OK);
	t = d; t.soft_step_ms = 30001; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	/* The soft step is checked while the release is off, so that turning it on later is the delta alone. */
	t = d; t.soft_delta_mc = 0; t.soft_step_ms = 0; CHECK(bc250_dpm_tune_check(&t, L(10)) == BC250_DPM_TUNE_THERMAL);
	/* The soft threshold stays strictly inside RELEASE..HOT at both edges of the delta (compile time below). */

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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, L(5));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run(&g, 0, 60, 25) == L(5));
	CHECK(run(&g, 1000, 60, 25) == L(9));     /* 1500 x 1000 / 800 = 1875 -> 1900 */
	for (i = 0; i < 400; i++) level = run(&g, 0, 60, 25);
	CHECK(level == L(5) && g.want <= L(5));

	/* The max setting: a floor above it is refused; one written past the check is clamped, never above the ceiling. */
	bc250_dpm_init(&g, L(6));
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	CHECK(run_mc(&g, 1000, 85000, 25) == L(6) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP && g.warm_holds == 0);
	CHECK(g.want == L(9) && g.raises == 1);		/* want stays the load's own answer */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	CHECK(run_mc(&g, 1000, 86999, 25) == L(6) && g.warm_holds == 0 && g.thermal_events == 0);
	/* Held at 86 C, the clock climbs one level per ramp interval (3.82 s at 86 C) up to the top, no thermal event. */
	for (i = 0; i < 20000u / 25u; i++) level = run_mc(&g, 1000, 86000, 25);
	CHECK(level == L(10) && g.warm_holds == 0 && g.thermal_events == 0 && g.raises == 5);

	/* 87 C is HOT: one step down at once, as before; the warm rule counts nothing, the cap holds the level. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(9) && g.thermal_cap == L(9));
	g.avg_permille = 0;
	for (i = 1; i < BC250_DPM_DOWN_HOLD_MS / 25u; i++) CHECK(run_mc(&g, 0, 87500, 25) == L(9));
	CHECK(run_mc(&g, 0, 87500, 25) == L(8) && g.thermal_cap == L(9));
	CHECK(run_mc(&g, 1000, 87500, 25) == L(8) && g.throttle == BC250_DPM_THROTTLE_THERMAL_WARM && g.warm_holds == 1);

	/* A lowering still happens at 85-86.999 C, after the same hold as at 60 C, one step at a time. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(8);
	for (i = 0; i < 7; i++) CHECK(run_mc(&g, 0, 85000, 25) == L(8));
	CHECK(run_mc(&g, 0, 86999, 25) == L(7) && g.lowers == 1 && g.warm_holds == 0);
	CHECK(g.throttle == BC250_DPM_THROTTLE_NONE);

	/* The runtime floor is a raise too: one level at 86.5 C, none at 87 C (the hot cap). */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(3);
	t = tune(900, 800, 650, 200, L(10));
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
	CHECK(run_mc(&g, 0, 86500, 25) == L(4) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP && g.warm_holds == 0);
	CHECK(run_mc(&g, 0, 87000, 25) == L(3) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	CHECK(run_mc(&g, 0, 60000, 25) == L(3));	/* one tick under 82 C releases nothing */

	/* At the max setting there is nothing to refuse: the setting names the reason. */
	bc250_dpm_init(&g, L(6)); g.level = L(6);
	CHECK(run_mc(&g, 1000, 86000, 25) == L(6) && g.throttle == BC250_DPM_THROTTLE_MAX_SETTING && g.warm_holds == 0);

	/* SetStablePowerState and a missing sensor go to the floor, never a hold. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(5); g.stable = 1;
	CHECK(run_mc(&g, 1000, 87000, 25) == L(0) && g.throttle == BC250_DPM_THROTTLE_STABLE && g.warm_holds == 0);
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	in = tick(1000, 0, 25); in.temperature_mc = 87000; in.temperature_valid = 0;
	CHECK(bc250_dpm_step(&g, &in) == L(0) && g.throttle == BC250_DPM_THROTTLE_SENSOR && g.warm_holds == 0);
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
	bc250_dpm_init(&g, L(6));
	{
		struct bc250_dpm_tune t;
		bc250_dpm_tune_default(&t); t.soft_delta_mc = 1500;
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 69999, 25) == L(3) && run_mc(&g, 1000, 69999, 25) == L(7) && run_mc(&g, 1000, 69999, 25) == L(10));
	CHECK(g.ramp_holds == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* At the knee: the first raise (none before) goes one level, the next waits a full interval. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(9);
	CHECK(run_mc(&g, 915, 80000, 25) == L(10) && g.ramp_holds == 0 && g.throttle == BC250_DPM_THROTTLE_NONE);

	/* A raise below the knee starts the interval too: a reading at 70 C 25 ms later waits for the rest of it. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 69000, 25) == L(3));
	for (i = 1; i < BC250_DPM_RAMP_MIN_MS / 25u; i++) CHECK(run_mc(&g, 1000, 70000, 25) == L(3));
	CHECK(run_mc(&g, 1000, 70000, 25) == L(4));

	/* Lowering is never held: idle at 80 C steps down after the usual hold. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(8); g.raise_ms = 0;
	for (i = 0; i < 7; i++) CHECK(run_mc(&g, 0, 80000, 25) == L(8));
	CHECK(run_mc(&g, 0, 80000, 25) == L(7) && g.ramp_holds == 0);

	/* The hot band (and the warm zone with it, 87 C from 0.7.204) names its own reason; the ramp adds no hold there. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(5);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(4) && g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT && g.ramp_holds == 0);
	CHECK(run_mc(&g, 1000, 87000, 25) == L(4) && g.ramp_holds == 0 && g.warm_holds == 0);

	/* The runtime floor is a raise too: one level per interval above the knee. */
	{
		struct bc250_dpm_tune t = tune(900, 800, 650, 200, L(10));
		bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
		CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);
		CHECK(run_mc(&g, 0, 78000, 25) == L(1) && g.throttle == BC250_DPM_THROTTLE_THERMAL_RAMP);
		CHECK(run_mc(&g, 0, 60000, 25) == L(10));	/* below the knee: at once */
	}

	/* A stalled tick counts as MAX_DT_MS, so one stall does not open a raise. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(10);
	CHECK(run(&g, 1000, 90, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL && g.throttle == BC250_DPM_THROTTLE_THERMAL_HARD);
	CHECK(g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL);

	/* The load never goes below the lab floor, at any temperature and any busy share: only the cap does, and
	 * what the step returns is below the floor only while the cap is. */
	for (temp = 40000; temp <= 95000; temp += 500)
		for (busy = 0; busy <= 1000u; busy += 100u) {
			bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 90000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	in = tick(1000, 60, 25); in.temperature_valid = 0;
	for (i = 0; i < 400u; i++) {
		level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
		CHECK(level == BC250_DPM_THERMAL_FLOOR_LEVEL && g.thermal_cap == BC250_DPM_THERMAL_FLOOR_LEVEL);
		CHECK(g.throttle == BC250_DPM_THROTTLE_SENSOR);
	}
	/* From the floor or above it the rule is the lowering it has always been (0.7.204 behaviour). */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.level = L(5); g.thermal_cap = L(5);
	level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level);
	CHECK(level == L(0) && g.thermal_cap == L(0) && g.throttle == BC250_DPM_THROTTLE_SENSOR);
	/* Only the release brings a sub-floor cap back up, and it does so one level at a time. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 90000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	for (i = 0; i < 4u; i++) { level = bc250_dpm_step(&g, &in); bc250_dpm_commit(&g, level); }
	for (i = 1; i < BC250_DPM_RELEASE_STEP_MS / 25u; i++)
		CHECK(run_mc(&g, 1000, 60000, 25) == BC250_DPM_THERMAL_FLOOR_LEVEL);
	CHECK(run_mc(&g, 1000, 60000, 25) == L(-1) && g.thermal_cap == L(-1));
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL); g.stable = 1;
	CHECK(run_mc(&g, 1000, 88000, 25) == L(-1));		/* the cap is lower than the floor it asks for */
	CHECK(g.throttle == BC250_DPM_THROTTLE_THERMAL_SOFT);
	g.stable = 0;
	for (i = 0; i < 20u * BC250_DPM_RELEASE_STEP_MS / 25u; i++) run_mc(&g, 0, 60000, 25);
	g.stable = 1;
	CHECK(run_mc(&g, 1000, 60000, 25) == L(0) && g.throttle == BC250_DPM_THROTTLE_STABLE);

	/* A runtime floor is never admitted below the lab floor (the points below it are the cap's). */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	t = tune(900, 800, 650, 200, L(-1));
	CHECK(bc250_dpm_tune_check(&t, BC250_DPM_TOP_LEVEL) == BC250_DPM_TUNE_FLOOR);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && g.tune.floor_level == L(0));
	t.floor_level = BC250_DPM_THERMAL_FLOOR_LEVEL;
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_FLOOR && g.tune.floor_level == L(0));
	t.floor_level = L(0);
	CHECK(bc250_dpm_set_tune(&g, &t) == BC250_DPM_TUNE_OK);

	/* The refusal: the KMD could not put the hardware at a sub-floor point. From then on the cap stops at the
	 * lab floor for the rest of the start, whatever the temperature, and a cap already below it is raised. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(run_mc(&g, 1000, 88000, 25) == L(-1) && g.thermal_cap == L(-1));
	bc250_dpm_subfloor_refused(&g);
	CHECK(!g.subfloor_ok && g.subfloor_refusals == 1 && g.thermal_cap == L(0));
	g.level = L(0);					/* the KMD applied the lab floor instead */
	for (i = 0; i < 4000u; i++) {
		level = run_mc(&g, 1000, (i & 1u) ? 88000 : 91000, 25);
		CHECK(level == L(0) && g.thermal_cap == L(0));
	}
	/* And a refusal before any hot tick keeps the whole start at or above the lab floor. */
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(g, max_level);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	/* Between the two thresholds the trailing window decides: a share over the admitted mean that never
	 * reaches the exit share leaves after one hold time, not at once and not never. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	for (i = 1; i < hold; i++)
		CHECK(run_ring(&g, BC250_DPM_IDLE_BUSY_PERMILLE + 50u, 60000, 25, 0) == BC250_DPM_IDLE_LEVEL);
	CHECK(run_ring(&g, BC250_DPM_IDLE_BUSY_PERMILLE + 50u, 60000, 25, 0) == L(0));
	CHECK(!g.idle && g.idle_exits == 1 && g.idle_entries == 1);
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

	/* A submission with no hardware busy share of its own (a tick waiting on a fence) exits as well, and
	 * keeps the window from starting while the ring has work. */
	idle_init(&g, BC250_DPM_TOP_LEVEL);
	CHECK(quiet_until_idle(&g, hold, 60000) == hold);
	CHECK(run_ring(&g, 0, 60000, 25, 1) == L(0) && !g.idle && g.idle_exits == 1);
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
		bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, BC250_DPM_TOP_LEVEL);
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
	bc250_dpm_init(&g, L(5));
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
	test_tune_check();
	test_tune_no_oscillation();
	test_floor();
	test_warm();
	test_reentry();
	test_soft_release();
	test_plant();
	test_ramp();
	test_plant367();
	test_subfloor();
	printf("dpm policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
