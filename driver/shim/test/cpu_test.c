/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * CPU policy host test (driver/shim/bc250_cpu.c; docs/design/tuner.md, docs/hardware.md, ADR 0020): the two
 * queues' message allowlists and their per-message argument ranges, the admitted settings, the order a change is
 * sent in, the signed packing of the curve scale, the failure signs and the guided undervolt search.
 *
 * Why this test carries so much weight: every range in bc250_cpu.h is REPORTED by two community projects and
 * measured by nobody on this part, and one of those projects destroyed a board by letting the CPU voltage scale
 * freely. The driver's answer is that nothing outside this file's expectations can reach the mailbox at all, so
 * these expectations are the safety argument and not a formality.
 *
 * Host-side only; nothing here touches the lab machine. Build and run: driver/shim/test/run_cpu.ps1.
 */
#include <stdio.h>
#include <string.h>
#include "bc250_cpu.h"
#include "bc250_dpm.h"

static int checks, failures;
#define CHECK(x) do { checks++; if (!(x)) { failures++; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

/* The shape the rest of the file assumes. A constant CHECK would trip C4127 under /WX, so these fail the build. */
typedef char cpu_shape[(BC250_CPU_CORES == 8u && BC250_CPU_PSTATES == 8u && BC250_CPU_QUEUE_CPU == 3u &&
			BC250_CPU_MASK_STOCK == 0x77u && BC250_CPU_MASK_FULL == 0xFFu &&
			BC250_CPU_REFUSE_MV == 1300u && BC250_CPU_MAX_MHZ < BC250_CPU_MAX_MHZ_LAB &&
			/* every step the search ever asks for is inside the admitted undervolt range */
			BC250_CPU_SEARCH_MAX_STEPS <= BC250_CPU_UV_MAX_STEPS) ? 1 : -1];

/* ---- the transport ------------------------------------------------------------------------------- */

/* The three registers of queue 3, as the firmware's own descriptor table gives them. bc250_cpu.c asserts the same
 * three values at compile time against the generated queue 0 constants; this is the readable copy of that, and the
 * one place a reader can see which register is which. */
static void test_queue(void)
{
	struct bc250_cpu_queue q;
	memset(&q, 0, sizeof(q));
	bc250_cpu_queue3(&q);
	CHECK(q.msg_reg == 0x58A20u);       /* C2PMSG_72, MP1 SMN 0x03B10A20 */
	CHECK(q.resp_reg == 0x58A80u);      /* C2PMSG_96, MP1 SMN 0x03B10A80 */
	CHECK(q.param_reg == 0x58A88u);     /* C2PMSG_98, MP1 SMN 0x03B10A88 */
	/* All three distinct, and none of them queue 0's three (0x58A08, 0x58A48, 0x58A68). */
	CHECK(q.msg_reg != q.resp_reg && q.msg_reg != q.param_reg && q.resp_reg != q.param_reg);
	CHECK(q.msg_reg != 0x58A08u && q.resp_reg != 0x58A48u && q.param_reg != 0x58A68u);
	/* The hazard not to copy: the community helper writes its argument at command + 4, which is queue 4's. */
	CHECK(q.param_reg != q.msg_reg + 4u);
}

/* ---- the allowlists ----------------------------------------------------------------------------- */

static void test_allowlist(void)
{
	unsigned int m, getters = 0, setters = 0, gfxGetters = 0, gfxSetters = 0;
	for (m = 0; m < 0x100u; m++) {
		if (bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, m, 0)) getters++;
		if (bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, m, 1)) setters++;
		if (bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, m, 0)) gfxGetters++;
		if (bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, m, 1)) gfxSetters++;
		/* No other queue exists for this driver: 1, 2 and 4 and up are refused outright. */
		CHECK(!bc250_cpu_message_allowed(1u, m, 0) && !bc250_cpu_message_allowed(1u, m, 1));
		CHECK(!bc250_cpu_message_allowed(4u, m, 0) && !bc250_cpu_message_allowed(4u, m, 1));
	}
	/* Queue 3: six getters and the three setters that are the whole write surface of the CPU rail. */
	CHECK(getters == 6u && setters == 3u);
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CPU_MV, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_GPU_MV, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_PSTATE_MHZ, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CAP_C, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_SOC_MHZ, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CORE_MHZ, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE, 1));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CAP_C, 1));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, 1));
	/* A getter sent as a setter and a setter sent as a getter are both refused: a caller cannot mislabel a
	 * message to get past the gate. */
	CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CPU_MV, 1));
	CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, 0));
	/* Queue 0: the two reads, the mask, and the two soft CCLK limits. Nothing of the clock transaction. */
	CHECK(gfxGetters == 2u && gfxSetters == 3u);
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_QUERY_CORE_PSTATE, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_GET_ENABLED_FEATURES, 0));
	CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_SET_CORE_ENABLE_MASK, 1));
	/* The GFX clock transaction's own messages are not CPU messages on queue 0: GetSmuVersion 0x02,
	 * RequestGfxclk 0x0E, GetGfxFrequency 0x37, GetGfxVid 0x38, ForceGfxVid 0x3B
	 * (driver/amdgpu-import/smu_v11_8_ppsmc.h). Two of those numbers are queue 3's own getters in the other
	 * queue's namespace, which is why the allowlist takes the queue as well as the message, and why the
	 * clock list and this list must refuse each other's numbers. */
	{
		static const unsigned int clock[] = { 0x02u, 0x0Eu, 0x37u, 0x38u, 0x3Bu };
		unsigned int i;
		for (i = 0; i < sizeof(clock) / sizeof(clock[0]); i++) {
			CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, clock[i], 0));
			CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, clock[i], 1));
		}
		CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x37u, 0));	/* READ_GPU_MV */
		CHECK(bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x3Bu, 0));	/* READ_PSTATE_MHZ */
		CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x02u, 0) &&
		      !bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x02u, 1));
		CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x0Eu, 0) &&
		      !bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x0Eu, 1));
		CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x38u, 0) &&
		      !bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, 0x38u, 1));
		/* And the other way: no CPU message of queue 0 is admitted by the clock list. */
		CHECK(!bc250_clock_message_allowed(BC250_CPU_MSG_SET_CORE_ENABLE_MASK));
		CHECK(!bc250_clock_message_allowed(BC250_CPU_MSG_SET_SOFT_MIN_CCLK));
		CHECK(!bc250_clock_message_allowed(BC250_CPU_MSG_SET_SOFT_MAX_CCLK));
		CHECK(!bc250_clock_message_allowed(BC250_CPU_MSG_QUERY_CORE_PSTATE));
		CHECK(!bc250_clock_message_allowed(BC250_CPU_MSG_GET_ENABLED_FEATURES));
	}
	/* The rejected messages of docs/design/rejected-options.md, every one of them, both directions:
	 * the absolute CPU VID writes, the two unknown pairs, the SoC voltage pair, the LCLK pair, 0x77, 0x8E,
	 * the generic SMN write that hangs on argument 0, and the guarded block 0x28..0x30. */
	{
		static const unsigned int rejected[] = { 0x0Fu, 0x10u, 0x4Du, 0x4Eu, 0x49u, 0x4Au, 0x52u, 0x53u,
							 0x6Du, 0x25u, 0x26u, 0x77u, 0x8Eu, 0x8Cu, 0x20u, 0x9Au, 0x98u };
		unsigned int i;
		for (i = 0; i < sizeof(rejected) / sizeof(rejected[0]); i++) {
			CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, rejected[i], 0));
			CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, rejected[i], 1));
			CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, rejected[i], 0));
			CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_GFX, rejected[i], 1));
		}
		for (m = 0x28u; m <= 0x30u; m++)
			if (m != BC250_CPU_MSG_SET_CORE_ENABLE_MASK) {
				CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, m, 0));
				CHECK(!bc250_cpu_message_allowed(BC250_CPU_QUEUE_CPU, m, 1));
			}
	}
}

static void test_arguments(void)
{
	unsigned int p;
	/* A message that is not on the list has no admitted argument at all. */
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, 0x98u, 0));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, 0x0Fu, 1200u));
	/* The single-value getters take 0 and nothing else: a stray argument is a different question than the one
	 * the driver means to ask. */
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CPU_MV, 0));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CPU_MV, 1));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CAP_C, 0));
	/* Per-core and per-P-state indices stop at the eighth. */
	for (p = 0; p < 16u; p++) {
		CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_CORE_MHZ, p) ==
		      (p < BC250_CPU_CORES));
		CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_PSTATE_MHZ, p) ==
		      (p < BC250_CPU_PSTATES));
		CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_QUERY_CORE_PSTATE, p) ==
		      (p < BC250_CPU_CORES));
	}
	/* The SoC clock index lives in the high half, and the low half must be clean. */
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_SOC_MHZ, 0u));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_SOC_MHZ, 19u << 16));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_SOC_MHZ, 20u << 16));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_READ_SOC_MHZ, (1u << 16) | 1u));
	/* The temperature cap: whole Celsius inside the band, and the firmware's own 100 C is the top. */
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CAP_C, BC250_CPU_TEMP_MIN_C - 1u));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CAP_C, BC250_CPU_TEMP_MIN_C));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CAP_C, BC250_CPU_TEMP_MAX_C));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CAP_C, BC250_CPU_TEMP_MAX_C + 1u));
	/* The clock limit: the lab bound is the widest the argument check admits, and bc250_cpu_settings_check is
	 * what refuses the band between the release bound and it without an undervolt. */
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, BC250_CPU_MIN_MHZ - 1u));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, BC250_CPU_MIN_MHZ));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, BC250_CPU_MAX_MHZ_LAB));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, BC250_CPU_MAX_MHZ_LAB + 1u));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, 5000u));
	/* The core mask: two values and no others. */
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_SET_CORE_ENABLE_MASK, BC250_CPU_MASK_STOCK));
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_SET_CORE_ENABLE_MASK, BC250_CPU_MASK_FULL));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_SET_CORE_ENABLE_MASK, 0u));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_GFX, BC250_CPU_MSG_SET_CORE_ENABLE_MASK, 0x7Fu));
	/* The curve scale, the one argument whose form is the safety rule: 0 or a negative 16-bit value whose
	 * magnitude is an admitted step count. Every positive scale is refused here, before the mailbox. */
	CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE, 0u));
	for (p = 1; p <= BC250_CPU_UV_MAX_STEPS; p++)
		CHECK(bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE,
						bc250_cpu_scale_argument(p)));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE,
					  bc250_cpu_scale_argument(BC250_CPU_UV_MAX_STEPS + 1u)));
	for (p = 1; p < 0x8000u; p += 97u)
		CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE, p));
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE, 0x10000u));
	/* The packing itself: a step count of n is sent as -n in the low 16 bits. */
	CHECK(bc250_cpu_scale_argument(0) == 0u);
	CHECK(bc250_cpu_scale_argument(1) == 0xFFFFu);
	CHECK(bc250_cpu_scale_argument(16) == 0xFFF0u);
	/* Nothing clamps here: a step count the ranges refuse packs into an argument the allowlist refuses too, so
	 * a caller that skipped bc250_cpu_settings_check still cannot reach the mailbox. */
	CHECK(!bc250_cpu_argument_allowed(BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE,
					  bc250_cpu_scale_argument(BC250_CPU_UV_MAX_STEPS + 100u)));
}

/* ---- the admitted settings ---------------------------------------------------------------------- */

static struct bc250_cpu_settings set(unsigned int mhz, unsigned int uv, unsigned int temp)
{
	struct bc250_cpu_settings s;
	memset(&s, 0, sizeof(s));
	if (mhz) { s.max_given = 1; s.max_mhz = mhz; }
	if (uv) { s.uv_given = 1; s.uv_steps = uv; }
	if (temp) { s.temp_given = 1; s.temp_c = temp; }
	return s;
}

static void test_settings(void)
{
	struct bc250_cpu_settings s;
	s = set(0, 0, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_ERROR_NOTHING);
	/* The release bound: a limit, never a raise past BC250_CPU_MAX_MHZ. */
	s = set(BC250_CPU_MIN_MHZ, 0, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_OK);
	s = set(BC250_CPU_MAX_MHZ, 0, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_OK);
	s = set(BC250_CPU_MIN_MHZ - 100u, 0, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_ERROR_CLOCK);
	s = set(BC250_CPU_MAX_MHZ + 100u, 0, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_ERROR_CLOCK);
	/* The lab bound, under the owner's pre-approval: it needs an undervolt already in the same request, which
	 * is the whole point - a higher clock at the firmware's own voltage is the shape that destroyed a board. */
	s = set(BC250_CPU_MAX_MHZ_LAB, 0, 0);
	CHECK(bc250_cpu_settings_check(&s, 1) == BC250_CPU_ERROR_CLOCK);
	s = set(BC250_CPU_MAX_MHZ_LAB, 1, 0);
	CHECK(bc250_cpu_settings_check(&s, 1) == BC250_CPU_OK);
	s = set(BC250_CPU_MAX_MHZ_LAB, 1, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_ERROR_CLOCK);
	s = set(BC250_CPU_MAX_MHZ_LAB + 100u, 8, 0);
	CHECK(bc250_cpu_settings_check(&s, 1) == BC250_CPU_ERROR_CLOCK);
	/* The undervolt: a step count, bounded, and 0 is "no undervolt" and not "undervolt by nothing". */
	s = set(0, BC250_CPU_UV_MAX_STEPS, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_OK);
	s = set(0, BC250_CPU_UV_MAX_STEPS + 1u, 0);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_ERROR_UV);
	/* The temperature cap. */
	s = set(0, 0, BC250_CPU_TEMP_MIN_C);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_OK);
	s = set(0, 0, BC250_CPU_TEMP_MAX_C);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_OK);
	s = set(0, 0, BC250_CPU_TEMP_MIN_C - 1u);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_ERROR_TEMP);
	s = set(0, 0, BC250_CPU_TEMP_MAX_C + 1u);
	CHECK(bc250_cpu_settings_check(&s, 0) == BC250_CPU_ERROR_TEMP);
}

/* ---- the order a change is sent in --------------------------------------------------------------- */

static void test_plan(void)
{
	struct bc250_cpu_plan p;
	struct bc250_cpu_settings from, to;

	/* Nothing to do is its own answer and sends no message. */
	from = set(3400u, 4u, 95u);
	CHECK(bc250_cpu_plan(&from, &from, 0, &p) == BC250_CPU_ERROR_NOTHING && p.count == 0);
	to = set(0, 0, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_ERROR_NOTHING && p.count == 0);

	/* A deeper undervolt: the voltage-lowering step first. */
	to = set(0, 6u, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_OK);
	CHECK(p.count == 1 && p.step[0].kind == BC250_CPU_STEP_UV);
	CHECK(p.step[0].queue == BC250_CPU_QUEUE_CPU && p.step[0].message == BC250_CPU_MSG_SET_CURVE_SCALE);
	CHECK(p.step[0].parameter == bc250_cpu_scale_argument(6u));

	/* A deeper undervolt and a higher clock together: the undervolt goes first, because the predicted voltage
	 * must never transiently exceed the ceiling. */
	to = set(3600u, 8u, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_OK);
	CHECK(p.count == 2 && p.step[0].kind == BC250_CPU_STEP_UV && p.step[1].kind == BC250_CPU_STEP_CLOCK);
	CHECK(p.step[1].message == BC250_CPU_MSG_SET_MAX_MHZ && p.step[1].parameter == 3600u);

	/* A shallower undervolt with the clock unchanged: the clock step is absent and the undervolt is the only
	 * step, so the order rule has nothing to decide. */
	to = set(0, 2u, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_OK);
	CHECK(p.count == 1 && p.step[0].kind == BC250_CPU_STEP_UV);

	/* A shallower undervolt and a lower clock: the clock comes down first, then the voltage goes back up. */
	to = set(3000u, 1u, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_OK);
	CHECK(p.count == 2 && p.step[0].kind == BC250_CPU_STEP_CLOCK && p.step[1].kind == BC250_CPU_STEP_UV);
	CHECK(p.step[0].parameter == 3000u && p.step[1].parameter == bc250_cpu_scale_argument(1u));

	/* The full restore: the clock down, then the undervolt away, and the cap back up last because a looser
	 * cap is less protection. */
	to = set(3000u, 0, BC250_CPU_TEMP_MAX_C);
	to.uv_given = 1; to.uv_steps = 0;
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_OK);
	CHECK(p.count == 3 && p.step[0].kind == BC250_CPU_STEP_CLOCK && p.step[1].kind == BC250_CPU_STEP_UV &&
	      p.step[2].kind == BC250_CPU_STEP_TEMP);
	CHECK(p.step[1].parameter == 0u && p.step[2].parameter == BC250_CPU_TEMP_MAX_C);

	/* A tightening cap goes first, before anything that makes heat. */
	to = set(3600u, 8u, 88u);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_OK);
	CHECK(p.count == 3 && p.step[0].kind == BC250_CPU_STEP_TEMP && p.step[0].parameter == 88u);
	CHECK(p.step[1].kind == BC250_CPU_STEP_UV && p.step[2].kind == BC250_CPU_STEP_CLOCK);

	/* From nothing applied at all: the first clock limit is a lowering of the firmware's own ceiling. */
	from = set(0, 0, 0);
	to = set(3200u, 0, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_OK);
	CHECK(p.count == 1 && p.step[0].kind == BC250_CPU_STEP_CLOCK);

	/* Every refusal of the ranges is a refusal of the plan, and then nothing is sent at all. */
	to = set(5000u, 0, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_ERROR_CLOCK && p.count == 0);
	to = set(0, 99u, 0);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_ERROR_UV && p.count == 0);
	to = set(0, 0, 70u);
	CHECK(bc250_cpu_plan(&from, &to, 0, &p) == BC250_CPU_ERROR_TEMP && p.count == 0);

	/* Every step of every plan is a message the allowlist admits with the argument it carries. That is the
	 * invariant the mailbox owner relies on, so it is checked over a wide sweep of plans and not by hand. */
	{
		unsigned int a, b, uvA, uvB, count = 0;
		for (a = BC250_CPU_MIN_MHZ; a <= BC250_CPU_MAX_MHZ; a += 100u)
			for (b = BC250_CPU_MIN_MHZ; b <= BC250_CPU_MAX_MHZ; b += 100u)
				for (uvA = 0; uvA <= BC250_CPU_UV_MAX_STEPS; uvA += 4u)
					for (uvB = 0; uvB <= BC250_CPU_UV_MAX_STEPS; uvB += 4u) {
						unsigned int i;
						struct bc250_cpu_settings f = set(a, uvA, 95u);
						struct bc250_cpu_settings t = set(b, uvB, 90u);
						enum bc250_cpu_error e;
						f.uv_given = 1; f.uv_steps = uvA;
						t.uv_given = 1; t.uv_steps = uvB;
						e = bc250_cpu_plan(&f, &t, 0, &p);
						CHECK(e == BC250_CPU_OK || e == BC250_CPU_ERROR_NOTHING);
						for (i = 0; i < p.count; i++) {
							CHECK(bc250_cpu_message_allowed(p.step[i].queue,
											p.step[i].message, 1));
							CHECK(bc250_cpu_argument_allowed(p.step[i].queue,
											 p.step[i].message,
											 p.step[i].parameter));
							count++;
						}
						CHECK(p.count <= BC250_CPU_PLAN_MAX);
					}
		CHECK(count > 1000u);
	}
}

/* ---- the failure signs -------------------------------------------------------------------------- */

static struct bc250_cpu_sample sample_ok(void)
{
	struct bc250_cpu_sample s;
	unsigned int i;
	memset(&s, 0, sizeof(s));
	s.voltage_mv = 1050u;
	s.cores = BC250_CPU_CORES;
	for (i = 0; i < BC250_CPU_CORES; i++) s.core_mhz[i] = 3400u;
	s.target_mhz = 3400u;
	s.temperature_mc = 70000;
	s.temperature_valid = 1;
	return s;
}

static void test_sample(void)
{
	struct bc250_cpu_sample s = sample_ok();
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_NONE);
	/* A machine check comes before everything else: the part has already computed something wrong. */
	s = sample_ok(); s.whea_events = 1; s.voltage_mv = 2000u;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_WHEA);
	s = sample_ok(); s.checksum_errors = 1;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_CHECKSUM);
	/* The voltage rule, on both sides of the plausible band and at the refusal line. */
	s = sample_ok(); s.voltage_mv = BC250_CPU_REFUSE_MV;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_NONE);
	s = sample_ok(); s.voltage_mv = BC250_CPU_REFUSE_MV + 1u;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_VOLTAGE);
	s = sample_ok(); s.voltage_mv = BC250_CPU_PLAUSIBLE_MIN_MV - 1u;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_VOLTAGE);
	/* A voltage of 0 is "the firmware did not answer", not "0 mV". */
	s = sample_ok(); s.voltage_mv = 0;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_NONE);
	/* The lab's own 87 C stops a trial as well. */
	s = sample_ok(); s.temperature_mc = BC250_CLOCK_HOT_MC;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_HOT);
	s = sample_ok(); s.temperature_mc = BC250_CLOCK_HOT_MC; s.temperature_valid = 0;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_NONE);
	/* Clock stretching: one core far enough under the limit that is applied. */
	s = sample_ok(); s.core_mhz[3] = s.target_mhz - BC250_CPU_STRETCH_MHZ;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_STRETCH);
	s = sample_ok(); s.core_mhz[3] = s.target_mhz - BC250_CPU_STRETCH_MHZ + 1u;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_NONE);
	/* A core that did not answer reads 0 and is not evidence either way. */
	s = sample_ok(); s.core_mhz[3] = 0;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_NONE);
	/* With no clock limit applied there is nothing to stretch against. */
	s = sample_ok(); s.target_mhz = 0; s.core_mhz[0] = 500u;
	CHECK(bc250_cpu_check_sample(&s) == BC250_CPU_FAIL_NONE);
}

/* ---- the guided search -------------------------------------------------------------------------- */

/* The walk a real run makes: the first call asks for step 1, every later call judges the step just loaded, and the
 * search never reports a step it saw fail. */
static void test_search(void)
{
	struct bc250_cpu_search s;
	struct bc250_cpu_sample ok;
	unsigned int i;

	/* Eight good steps: the search stops at its own bound with the last step as the answer. */
	bc250_cpu_search_begin(&s, 0, 0, 1100u, 3400u);
	CHECK(s.running && s.max_steps == BC250_CPU_SEARCH_MAX_STEPS && s.load_ms == BC250_CPU_SEARCH_LOAD_MS);
	CHECK(bc250_cpu_search_next(&s, NULL) == BC250_CPU_SEARCH_APPLY && s.step == 1u);
	for (i = 1; i <= BC250_CPU_SEARCH_MAX_STEPS; i++) {
		enum bc250_cpu_search_action action;
		ok = sample_ok();
		ok.voltage_mv = 1100u - 5u * i;          /* the firmware is taking the scale */
		action = bc250_cpu_search_next(&s, &ok);
		CHECK(s.best == i);
		CHECK(action == (i == BC250_CPU_SEARCH_MAX_STEPS ? BC250_CPU_SEARCH_DONE : BC250_CPU_SEARCH_APPLY));
		if (action == BC250_CPU_SEARCH_APPLY) CHECK(s.step == i + 1u);
	}
	CHECK(!s.running && s.best == BC250_CPU_SEARCH_MAX_STEPS && s.fail == BC250_CPU_FAIL_NONE &&
	      s.tested == BC250_CPU_SEARCH_MAX_STEPS);
	CHECK(bc250_cpu_search_next(&s, &ok) == BC250_CPU_SEARCH_DONE);      /* a finished search stays finished */

	/* The third step stretches the clock: the answer is the second, and the sign is recorded. */
	bc250_cpu_search_begin(&s, 6u, 12000u, 1100u, 3400u);
	CHECK(s.max_steps == 6u && s.load_ms == 12000u);
	CHECK(bc250_cpu_search_next(&s, NULL) == BC250_CPU_SEARCH_APPLY);
	ok = sample_ok(); ok.voltage_mv = 1090u;
	CHECK(bc250_cpu_search_next(&s, &ok) == BC250_CPU_SEARCH_APPLY && s.step == 2u && s.best == 1u);
	ok = sample_ok(); ok.voltage_mv = 1080u;
	CHECK(bc250_cpu_search_next(&s, &ok) == BC250_CPU_SEARCH_APPLY && s.step == 3u && s.best == 2u);
	ok = sample_ok(); ok.voltage_mv = 1070u; ok.core_mhz[5] = ok.target_mhz - BC250_CPU_STRETCH_MHZ;
	CHECK(bc250_cpu_search_next(&s, &ok) == BC250_CPU_SEARCH_DONE);
	CHECK(!s.running && s.best == 2u && s.fail == (unsigned int)BC250_CPU_FAIL_STRETCH && s.tested == 3u);

	/* A voltage that does not move means the firmware is not taking the scale at all, and a deeper step would
	 * say nothing: the search stops with the step that held as the answer. */
	bc250_cpu_search_begin(&s, 8u, 0, 1100u, 3400u);
	CHECK(bc250_cpu_search_next(&s, NULL) == BC250_CPU_SEARCH_APPLY);
	ok = sample_ok(); ok.voltage_mv = 1100u;
	CHECK(bc250_cpu_search_next(&s, &ok) == BC250_CPU_SEARCH_DONE);
	CHECK(!s.running && s.best == 1u && s.fail == BC250_CPU_FAIL_NONE);

	/* The first step fails: nothing is on offer, and the baseline is what the caller puts back. */
	bc250_cpu_search_begin(&s, 8u, 0, 1100u, 3400u);
	CHECK(bc250_cpu_search_next(&s, NULL) == BC250_CPU_SEARCH_APPLY);
	ok = sample_ok(); ok.whea_events = 2u;
	CHECK(bc250_cpu_search_next(&s, &ok) == BC250_CPU_SEARCH_DONE);
	CHECK(!s.running && s.best == 0u && s.fail == (unsigned int)BC250_CPU_FAIL_WHEA);
}

/* ---- the core mask ------------------------------------------------------------------------------ */

static void test_mask(void)
{
	unsigned int m, allowed = 0;
	for (m = 0; m < 0x200u; m++) if (bc250_cpu_mask_allowed(m)) allowed++;
	CHECK(allowed == 2u);
	CHECK(bc250_cpu_mask_allowed(BC250_CPU_MASK_STOCK) && bc250_cpu_mask_allowed(BC250_CPU_MASK_FULL));
	CHECK(!bc250_cpu_mask_allowed(0u) && !bc250_cpu_mask_allowed(0x7Fu) && !bc250_cpu_mask_allowed(0x100u));
	/* The stock mask is 6 of 8 cores, the fourth of each complex off; the full mask is 8. */
	CHECK(bc250_cpu_mask_cores(BC250_CPU_MASK_STOCK) == 6u);
	CHECK(bc250_cpu_mask_cores(BC250_CPU_MASK_FULL) == BC250_CPU_CORES);
	CHECK(bc250_cpu_mask_cores(0u) == 0u);
	CHECK(bc250_cpu_mask_cores(1u) == 1u);
}

int main(void)
{
	test_queue();
	test_allowlist();
	test_arguments();
	test_settings();
	test_plan();
	test_sample();
	test_search();
	test_mask();
	printf("cpu policy: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
