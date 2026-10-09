/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 CPU control policy (include/bc250_cpu.h, docs/design/tuner.md, ADR 0020). Pure: no locks, no time
 * source, no registry, no MMIO. driver/kmd/cpu.c samples, persists and executes; driver/kmd/smu.c owns the
 * mailbox. driver/shim/test/cpu_test.c runs exactly what the miniport runs.
 *
 * Nothing in this file is a guess about the firmware: every message it admits has a handler in an image that
 * reports unit A's own version, and every argument form and range is a community report, cited in
 * docs/design/tuner.md with its class. The gate that keeps a report from reaching the hardware as a guess is in
 * driver/kmd/cpu.c, which sends no setter until this start's read stage has answered once.
 *
 * Nie wkladaj palcow miedzy drzwi - do not put your fingers between the door and the frame. One community board
 * died of a CPU clock raised with the voltage left to scale; this driver limits before it raises, and reads back.
 */
#include <string.h>
#include "bc250_cpu.h"
#include "generated/smu_registers.h"

/* The queue 3 mailbox, as BAR5 byte offsets. Written as offsets from the generated queue 0 constants so that a
 * regenerated register header moves both queues together, and asserted against the values the firmware's own
 * descriptor table gives (0x03B10A20 / 0x03B10A80 / 0x03B10A88 less the MP1 constant 0x03AB8000). */
#define BC250_CPU_Q3_MSG   (BC250_SMU_mmMP1_SMN_C2PMSG_66 + 6u * 4u)	/* C2PMSG_72, 0x58A20 */
#define BC250_CPU_Q3_RESP  (BC250_SMU_mmMP1_SMN_C2PMSG_90 + 6u * 4u)	/* C2PMSG_96, 0x58A80 */
#define BC250_CPU_Q3_PARAM (BC250_SMU_mmMP1_SMN_C2PMSG_90 + 8u * 4u)	/* C2PMSG_98, 0x58A88 */
typedef char bc250_cpu_queue3_offsets[(BC250_CPU_Q3_MSG == 0x58A20u && BC250_CPU_Q3_RESP == 0x58A80u &&
				       BC250_CPU_Q3_PARAM == 0x58A88u) ? 1 : -1];

void bc250_cpu_queue3(struct bc250_cpu_queue *queue)
{
	queue->msg_reg = BC250_CPU_Q3_MSG;
	queue->param_reg = BC250_CPU_Q3_PARAM;
	queue->resp_reg = BC250_CPU_Q3_RESP;
}

/* ---- the allowlist ----------------------------------------------------------------------------- */

int bc250_cpu_message_allowed(unsigned int queue, unsigned int message, int write)
{
	if (queue == BC250_CPU_QUEUE_GFX) {
		if (!write)
			switch (message) {
			case BC250_CPU_MSG_QUERY_CORE_PSTATE:
			case BC250_CPU_MSG_GET_ENABLED_FEATURES:
				return 1;
			default:
				return 0;
			}
		switch (message) {
		/* The core-enable mask alone. RequestCorePstate 0x0B is deliberately absent: a per-core P-state
		 * request on a part whose core topology Windows cannot even report is a control nothing asks for.
		 * SetSoftMinCclk 0x35 and SetSoftMaxCclk 0x36 are absent as well (0.7.211): no code path sends
		 * either, and an entry with no caller that admits a CPU minimum of 4000 MHz under no undervolt
		 * rule is the bricking shape through the one message bc250_cpu_settings_check does not cover. */
		case BC250_CPU_MSG_SET_CORE_ENABLE_MASK:
			return 1;
		default:
			return 0;
		}
	}
	if (queue != BC250_CPU_QUEUE_CPU) return 0;
	if (!write)
		switch (message) {
		case BC250_CPU_MSG_READ_CPU_MV:
		case BC250_CPU_MSG_READ_GPU_MV:
		case BC250_CPU_MSG_READ_PSTATE_MHZ:
		case BC250_CPU_MSG_READ_CAP_C:
		case BC250_CPU_MSG_READ_SOC_MHZ:
		case BC250_CPU_MSG_READ_CORE_MHZ:
			return 1;
		default:
			return 0;
		}
	/* The whole write surface on the CPU rail: a temperature cap, a clock limit and a curve scale, each with a
	 * readback or a recorded baseline. Everything that forces an absolute voltage identifier (0x0F, 0x10, 0x4D,
	 * 0x4E), every voltage offset without a getter (0x49, 0x4A), clock stretching (0x52, 0x53, 0x6D), the
	 * per-core clock (0x25, 0x26), the protection limits (0x77, 0x8E), the GPU temperature cap (0x8C, 0x20),
	 * the extra-voltage flag (0x9A) and the generic SMN write (0x98) stay out, each for the reason recorded in
	 * docs/design/rejected-options.md. The guarded block 0x28..0x30 is out as well: its nine entries carry a
	 * different configuration word in the firmware's own table, and the community reads that as a boot flag
	 * nobody has. */
	switch (message) {
	case BC250_CPU_MSG_SET_CURVE_SCALE:
	case BC250_CPU_MSG_SET_CAP_C:
	case BC250_CPU_MSG_SET_MAX_MHZ:
		return 1;
	default:
		return 0;
	}
}

unsigned int bc250_cpu_scale_argument(unsigned int uv_steps)
{
	/* Undervolt only: a step count of n goes as -n in the low 16 bits, which is what both community
	 * implementations send. Zero is the firmware default and a valid argument (the restore). */
	return (0u - uv_steps) & 0xFFFFu;
}

int bc250_cpu_argument_allowed(unsigned int queue, unsigned int message, unsigned int parameter)
{
	if (!bc250_cpu_message_allowed(queue, message, 0) && !bc250_cpu_message_allowed(queue, message, 1)) return 0;
	if (queue == BC250_CPU_QUEUE_GFX)
		switch (message) {
		case BC250_CPU_MSG_GET_ENABLED_FEATURES:
			return parameter == 0u;
		case BC250_CPU_MSG_QUERY_CORE_PSTATE:
			return parameter < BC250_CPU_CORES;
		case BC250_CPU_MSG_SET_CORE_ENABLE_MASK:
			return bc250_cpu_mask_allowed(parameter);
		default:
			return 0;
		}
	switch (message) {
	case BC250_CPU_MSG_READ_CPU_MV:
	case BC250_CPU_MSG_READ_GPU_MV:
	case BC250_CPU_MSG_READ_CAP_C:
		return parameter == 0u;
	case BC250_CPU_MSG_READ_PSTATE_MHZ:
		return parameter < BC250_CPU_PSTATES;
	case BC250_CPU_MSG_READ_CORE_MHZ:
		return parameter < BC250_CPU_CORES;
	case BC250_CPU_MSG_READ_SOC_MHZ:
		return (parameter & 0xFFFFu) == 0u && (parameter >> 16) < BC250_CPU_SOC_CLOCKS;
	case BC250_CPU_MSG_SET_CAP_C:
		return parameter >= BC250_CPU_TEMP_MIN_C && parameter <= BC250_CPU_TEMP_MAX_C;
	case BC250_CPU_MSG_SET_MAX_MHZ:
		return parameter >= BC250_CPU_MIN_MHZ && parameter <= BC250_CPU_MAX_MHZ_LAB;
	case BC250_CPU_MSG_SET_CURVE_SCALE: {
		unsigned int steps;
		if (parameter > 0xFFFFu) return 0;
		if (parameter == 0u) return 1;
		/* Undervolt only: the argument must be a negative 16-bit value whose magnitude is an admitted step
		 * count. A positive scale would raise the voltage and is refused here, not in the firmware. */
		if (parameter < 0x8000u) return 0;
		steps = (0x10000u - parameter) & 0xFFFFu;
		return steps <= BC250_CPU_UV_MAX_STEPS;
	}
	default:
		return 0;
	}
}

/* ---- the settings ------------------------------------------------------------------------------- */

enum bc250_cpu_error bc250_cpu_settings_check(const struct bc250_cpu_settings *s, int lab)
{
	unsigned int ceiling = lab ? BC250_CPU_MAX_MHZ_LAB : BC250_CPU_MAX_MHZ;
	if (s->uv_given && s->uv_steps > BC250_CPU_UV_MAX_STEPS) return BC250_CPU_ERROR_UV;
	if (s->max_given) {
		if (s->max_mhz < BC250_CPU_MIN_MHZ || s->max_mhz > ceiling) return BC250_CPU_ERROR_CLOCK;
		/* A clock above the release bound is a lab experiment, and it never runs without a real undervolt
		 * in the same request: that is the one rule between us and the reported bricking path. One step is
		 * about 5 mV at 3500 MHz, which is not an undervolt, so the bar is BC250_CPU_LAB_MIN_UV_STEPS. */
		if (s->max_mhz > BC250_CPU_MAX_MHZ &&
		    !(s->uv_given && s->uv_steps >= BC250_CPU_LAB_MIN_UV_STEPS)) return BC250_CPU_ERROR_CLOCK;
	}
	if (s->temp_given && (s->temp_c < BC250_CPU_TEMP_MIN_C || s->temp_c > BC250_CPU_TEMP_MAX_C))
		return BC250_CPU_ERROR_TEMP;
	if (!s->max_given && !s->uv_given && !s->temp_given) return BC250_CPU_ERROR_NOTHING;
	return BC250_CPU_OK;
}

static void add_step(struct bc250_cpu_plan *p, unsigned int queue, unsigned int message, unsigned int parameter,
		     unsigned int kind, unsigned int cools)
{
	if (p->count >= BC250_CPU_PLAN_MAX) return;
	p->step[p->count].queue = queue;
	p->step[p->count].message = message;
	p->step[p->count].parameter = parameter;
	p->step[p->count].kind = kind;
	p->step[p->count].cools = cools;
	p->count++;
}

static void add_uv(struct bc250_cpu_plan *p, unsigned int steps, unsigned int cools)
{
	add_step(p, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CURVE_SCALE, bc250_cpu_scale_argument(steps),
		 BC250_CPU_STEP_UV, cools);
}

static void add_clock(struct bc250_cpu_plan *p, unsigned int mhz, unsigned int cools)
{
	add_step(p, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_MAX_MHZ, mhz, BC250_CPU_STEP_CLOCK, cools);
}

enum bc250_cpu_error bc250_cpu_plan(const struct bc250_cpu_settings *from, const struct bc250_cpu_settings *to,
				    int lab, struct bc250_cpu_plan *plan)
{
	struct bc250_cpu_settings want;
	enum bc250_cpu_error error;
	int uvChange, clockChange, tempChange, deeper, lower;

	memset(plan, 0, sizeof(*plan));
	/* A value *to does not give keeps whatever *from has, so one control can change alone. */
	want = *to;
	if (!want.max_given) { want.max_given = from->max_given; want.max_mhz = from->max_mhz; }
	if (!want.uv_given) { want.uv_given = from->uv_given; want.uv_steps = from->uv_steps; }
	if (!want.temp_given) { want.temp_given = from->temp_given; want.temp_c = from->temp_c; }
	error = bc250_cpu_settings_check(&want, lab);
	if (error != BC250_CPU_OK) return error;

	uvChange = want.uv_given && (!from->uv_given || from->uv_steps != want.uv_steps);
	clockChange = want.max_given && (!from->max_given || from->max_mhz != want.max_mhz);
	tempChange = want.temp_given && (!from->temp_given || from->temp_c != want.temp_c);
	if (!uvChange && !clockChange && !tempChange) return BC250_CPU_ERROR_NOTHING;

	/* The cap first when it tightens (more protection before anything that makes heat), last when it loosens. */
	if (tempChange && (!from->temp_given || want.temp_c <= from->temp_c))
		add_step(plan, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CAP_C, want.temp_c, BC250_CPU_STEP_TEMP, 1u);
	/* The direction rule: the predicted voltage must never transiently exceed the ceiling, so every step that
	 * lowers the voltage goes before every step that raises it. A deeper undervolt lowers it; so does a lower
	 * clock limit, including the first limit of all, which is a lowering of the firmware's own ceiling. A
	 * shallower undervolt and a higher limit raise it. Each of the two controls is judged on its own, because one
	 * request can lower one and raise the other (a lower clock with less undervolt is the usual way back), and a
	 * single flag for both would then send the raising step first. Of the two raising steps the undervolt's
	 * goes first: a higher clock over a deeper undervolt is the combination that stretches. */
	deeper = uvChange && want.uv_steps > (from->uv_given ? from->uv_steps : 0u);
	lower = clockChange && (!from->max_given || want.max_mhz < from->max_mhz);
	/* A clock limit that comes down goes first of the two: it lowers the voltage the firmware chooses, and
	 * after it the undervolt may go either way. Otherwise the undervolt goes first, deeper or shallower, so
	 * that a higher clock never goes out over an undervolt deeper than the one in force at the lower clock
	 * (0.7.211; up to 0.7.210 the way back from a lab clock raised it before shallowing the undervolt). */
	if (lower) {
		add_clock(plan, want.max_mhz, 1u);
		if (uvChange) add_uv(plan, want.uv_steps, deeper ? 1u : 0u);
	} else {
		if (uvChange) add_uv(plan, want.uv_steps, deeper ? 1u : 0u);
		if (clockChange) add_clock(plan, want.max_mhz, 0u);
	}
	if (tempChange && from->temp_given && want.temp_c > from->temp_c)
		add_step(plan, BC250_CPU_QUEUE_CPU, BC250_CPU_MSG_SET_CAP_C, want.temp_c, BC250_CPU_STEP_TEMP, 0u);
	if (plan->count != (unsigned int)(uvChange + clockChange + tempChange)) return BC250_CPU_ERROR_PLAN;
	return BC250_CPU_OK;
}

int bc250_cpu_restore_target(const struct bc250_cpu_settings *from, const struct bc250_cpu_settings *before,
			     const struct bc250_cpu_settings *baseline, struct bc250_cpu_settings *out)
{
	int complete = 1;
	*out = *before;
	/* The cap: the state before the trial, then what the read stage answered, then the firmware's own. */
	if (from->temp_given && !out->temp_given) {
		out->temp_given = 1;
		out->temp_c = (baseline != NULL && baseline->temp_given) ? baseline->temp_c : BC250_CPU_TEMP_MAX_C;
	}
	/* The undervolt: zero is the firmware's own curve and a valid argument, so this is always nameable. */
	if (from->uv_given && !out->uv_given) {
		out->uv_given = 1;
		out->uv_steps = 0u;
	}
	/* The clock limit: only the recorded baseline can name it. Inventing a constant here would turn a
	 * restore into a raise, which is what 0.7.210 did with BC250_CPU_MAX_MHZ as "the baseline". */
	if (from->max_given && !out->max_given) {
		if (baseline != NULL && baseline->max_given) {
			out->max_given = 1;
			out->max_mhz = baseline->max_mhz;
		} else complete = 0;
	}
	return complete;
}

/* One firmware answer into the running maximum, when it is a clock of this part (bc250_cpu.h). */
static unsigned int cpu_baseline_take(unsigned int top, unsigned int mhz)
{
	if (mhz < BC250_CPU_MIN_MHZ || mhz > BC250_CPU_MAX_MHZ_LAB) return top;
	return mhz > top ? mhz : top;
}

void bc250_cpu_baseline_read(const unsigned int *pstate_mhz, unsigned int pstates,
			     const unsigned int *core_mhz, unsigned int cores,
			     const struct bc250_cpu_baseline *previous, struct bc250_cpu_baseline *out)
{
	unsigned int i, table = 0u, boost = 0u;
	if (out == NULL) return;
	for (i = 0u; pstate_mhz != NULL && i < pstates; i++) table = cpu_baseline_take(table, pstate_mhz[i]);
	for (i = 0u; core_mhz != NULL && i < cores; i++) boost = cpu_baseline_take(boost, core_mhz[i]);
	/* Every top of the same start is a floor: a stage that read nothing new cannot lower what another one saw.
	 * The tops of *previous came out of this function, so they are inside the band already. */
	if (previous != NULL) {
		if (previous->table_mhz > table) table = previous->table_mhz;
		if (previous->boost_mhz > boost) boost = previous->boost_mhz;
	}
	if (table > BC250_CPU_MAX_MHZ) table = BC250_CPU_MAX_MHZ;
	if (boost > BC250_CPU_MAX_MHZ) boost = BC250_CPU_MAX_MHZ;
	out->table_mhz = table;
	out->boost_mhz = boost;
	out->boost_given = boost != 0u ? 1 : 0;
	out->mhz = boost > table ? boost : table;
}

int bc250_cpu_boost_probe_needed(const struct bc250_cpu_baseline *baseline)
{
	if (baseline == NULL) return 1;
	return baseline->boost_given ? 0 : 1;
}

/* ---- the failure signs -------------------------------------------------------------------------- */

enum bc250_cpu_fail bc250_cpu_check_sample(const struct bc250_cpu_sample *s)
{
	unsigned int i;
	/* A machine check and a wrong answer come first: both say the part computed something wrong, which no
	 * temperature or clock reading would catch. */
	if (s->whea_events) return BC250_CPU_FAIL_WHEA;
	if (s->checksum_errors) return BC250_CPU_FAIL_CHECKSUM;
	if (s->voltage_mv) {
		if (s->voltage_mv > BC250_CPU_REFUSE_MV) return BC250_CPU_FAIL_VOLTAGE;
		if (s->voltage_mv < BC250_CPU_PLAUSIBLE_MIN_MV || s->voltage_mv > BC250_CPU_PLAUSIBLE_MAX_MV)
			return BC250_CPU_FAIL_VOLTAGE;
	}
	if (s->temperature_valid && s->temperature_mc >= BC250_CLOCK_HOT_MC) return BC250_CPU_FAIL_HOT;
	/* Clock stretching, against the clock the cores should reach. A core that did not answer reads 0 and is
	 * not evidence either way, and neither is any core of a sample nobody loaded: an idle core sits a whole
	 * gigahertz under the limit, so an unloaded sample would fail every step of the search. */
	if (s->loaded && s->target_mhz >= BC250_CPU_STRETCH_MHZ)
		for (i = 0; i < s->cores && i < BC250_CPU_CORES; i++)
			if (s->core_mhz[i] && s->core_mhz[i] + BC250_CPU_STRETCH_MHZ <= s->target_mhz)
				return BC250_CPU_FAIL_STRETCH;
	return BC250_CPU_FAIL_NONE;
}

/* ---- the guided search -------------------------------------------------------------------------- */

void bc250_cpu_search_begin(struct bc250_cpu_search *s, unsigned int max_steps, unsigned int load_ms,
			    unsigned int baseline_mv, unsigned int baseline_mhz)
{
	memset(s, 0, sizeof(*s));
	s->max_steps = max_steps == 0u || max_steps > BC250_CPU_SEARCH_MAX_STEPS ? BC250_CPU_SEARCH_MAX_STEPS : max_steps;
	s->load_ms = load_ms == 0u ? BC250_CPU_SEARCH_LOAD_MS : load_ms;
	s->baseline_mv = baseline_mv;
	s->baseline_mhz = baseline_mhz;
	s->running = 1;
}

enum bc250_cpu_search_action bc250_cpu_search_next(struct bc250_cpu_search *s, const struct bc250_cpu_sample *sample)
{
	enum bc250_cpu_fail fail;
	if (!s->running) return BC250_CPU_SEARCH_DONE;
	if (sample == NULL) {
		s->step = 1u;			/* the first step to try */
		return BC250_CPU_SEARCH_APPLY;
	}
	s->tested++;
	fail = bc250_cpu_check_sample(sample);
	if (fail != BC250_CPU_FAIL_NONE) {
		/* One step back from the first failure, and stop. The search never reports a step it saw fail. */
		s->fail = (unsigned int)fail;
		s->running = 0;
		return BC250_CPU_SEARCH_DONE;
	}
	/* The step held: it becomes the answer so far. A voltage that did not move at all is also a stop, because
	 * the firmware is not taking the scale and a deeper step would say nothing. */
	s->best = s->step;
	if (s->baseline_mv && sample->voltage_mv && sample->voltage_mv >= s->baseline_mv) {
		s->running = 0;
		return BC250_CPU_SEARCH_DONE;
	}
	if (s->step >= s->max_steps) {
		s->running = 0;
		return BC250_CPU_SEARCH_DONE;
	}
	s->step++;
	return BC250_CPU_SEARCH_APPLY;
}

/* ---- the core-enable mask ----------------------------------------------------------------------- */

int bc250_cpu_mask_allowed(unsigned int mask)
{
	/* Two masks and no others: the stock pattern this part ships with, and all eight cores. A different
	 * pattern suggests a real harvest of defective cores, which both community implementations refuse as
	 * well, and nothing in the window or the command line can ask for one. */
	return mask == BC250_CPU_MASK_STOCK || mask == BC250_CPU_MASK_FULL;
}

unsigned int bc250_cpu_mask_cores(unsigned int mask)
{
	unsigned int i, cores = 0;
	for (i = 0; i < BC250_CPU_CORES; i++) if (mask & (1u << i)) cores++;
	return cores;
}
