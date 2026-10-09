/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 CPU control policy (0.7.210; docs/design/tuner.md, docs/hardware.md, ADR 0020). Pure: no locks, no time
 * source, no registry, no MMIO. driver/kmd/cpu.c is the miniport around it, driver/kmd/smu.c the one serialized
 * mailbox owner, and driver/shim/test/cpu_test.c runs exactly what the miniport runs.
 *
 * What this file is for: the CPU rail of this part is reached through a second SMU queue, the firmware's queue 3,
 * whose three mailbox registers sit in the same BAR5 aperture the driver already maps for queue 0 (the three
 * offsets are in bc250_cpu_queue3() below, each derived from the generated queue 0 constant and checked against
 * the firmware's own descriptor table). So the CPU surface needs no new window into the chip and no
 * configuration-space SMN access, which docs/design/rejected-options.md forbids. It needs a second transport
 * instance, a second message allowlist and a per-message argument check. This file holds the last two, plus the
 * order a change must be sent in, the admitted ranges and the guided undervolt search.
 *
 * Classes of fact (docs/design/tuner.md, section 9 of the design note):
 *   PROVEN    queue 3's three registers, and that every message below has a handler in an image reporting unit
 *             A's firmware version 0.58.6.0 (our own read of both shipped images).
 *   REPORTED  what each message does, its argument form, and every range below. Two independent community
 *             implementations agree on the numbers; nobody has measured one on unit A.
 *
 * Therefore: no setter may be sent before the read stage of this start has answered once. driver/kmd/cpu.c holds
 * that gate (BC250_CPU_FLAG_QUEUE3_PROVEN); this file refuses every message that is not on its allowlist, and
 * every argument outside its range, so an inferred fact can never reach the hardware as a guess.
 *
 * The one hazard worth naming in the header: a community project permanently destroyed a BC-250 by raising the
 * CPU clock while the voltage identifier scaled freely, and its own ceiling is 1.325 V (REPORTED, the strongest
 * warning in any source). This driver therefore never forces an absolute CPU voltage (queue 3 0x0F, 0x10, 0x4D,
 * 0x4E stay rejected), its clock control is a limit before it is ever a raise, and it reads the applied voltage
 * back after every change and reverts above BC250_CPU_REFUSE_MV.
 */
#ifndef BC250_CPU_H
#define BC250_CPU_H

#include "bc250_clock.h"

/* ---- the two queues ---------------------------------------------------------------------------- */

#define BC250_CPU_QUEUE_GFX	0u	/* the firmware's queue 0: ours since 0.7.174, the GFX clock transaction's */
#define BC250_CPU_QUEUE_CPU	3u	/* the firmware's queue 3: the CPU clock, undervolt and thermal surface */

/* The mailbox registers of queue 3, as BAR5 byte offsets. The firmware's queue descriptor table at 0x700C gives
 * the MP1 SMN addresses 0x03B10A20 (command, C2PMSG_72), 0x03B10A80 (response, C2PMSG_96) and 0x03B10A88
 * (argument, C2PMSG_98); within MP1 the SMN address and the BAR5 offset differ by the constant 0x03AB8000, which
 * the queue 0 constants of generated/smu_registers.h already encode. The three expressions below are therefore
 * written as offsets from those constants and asserted against the read values in bc250_cpu.c, so a regenerated
 * register header cannot silently move them. All three are already in the escape read allowlist
 * (driver/kmd/regs.generated.h), which is how the lab can read the queue before any message is ever sent.
 *
 * A hazard not to copy: the community's mailbox helper always writes its argument at command + 4, which for
 * queue 3 is C2PMSG_99 - queue 4's argument register. Our transport takes all three registers from the
 * descriptor table instead. */
struct bc250_cpu_queue { unsigned int msg_reg, param_reg, resp_reg; };
void bc250_cpu_queue3(struct bc250_cpu_queue *queue);

/* ---- the messages, by queue -------------------------------------------------------------------- */

/* Queue 0, AMD-named in driver/amdgpu-import/smu_v11_8_ppsmc.h (MIT), which is why they are named and not
 * guessed. QueryCorePstate and GetEnabledSmuFeatures are the first CPU reads this project ever sends;
 * SetCoreEnableMask is the 8-core unlock, and unlike the community's generic SMN write it can put the stock
 * mask back. */
#define BC250_CPU_MSG_QUERY_CORE_PSTATE		0x0Cu	/* in: core id; out: the P-state. Status 0xFF above core 7 */
#define BC250_CPU_MSG_SET_CORE_ENABLE_MASK	0x2Cu	/* in: mask & 0xFF. The unlock, and the way back */
/* The two soft CCLK limits are named here and are NOT on the allowlist (0.7.211): no code path sends either,
 * and an entry with no caller that admits a CPU *minimum* of 4000 MHz under no undervolt rule is the bricking
 * shape through the one message bc250_cpu_settings_check does not cover. They come back when trial B6 needs
 * them, behind that check and capped at the recorded baseline. */
#define BC250_CPU_MSG_SET_SOFT_MIN_CCLK		0x35u	/* (core << 20) | MHz. Rejected: no caller */
#define BC250_CPU_MSG_SET_SOFT_MAX_CCLK		0x36u	/* same layout, not queue 3's 0x36. Rejected */
#define BC250_CPU_MSG_GET_ENABLED_FEATURES	0x3Du	/* in: 0; out: the feature bits. Changes nothing */

/* Queue 3. Getters first; the three setters are the whole write surface of this driver on the CPU rail. */
#define BC250_CPU_MSG_READ_CPU_MV		0x36u	/* out: the CPU voltage now, mV. The 1300 mV rule's number */
#define BC250_CPU_MSG_READ_GPU_MV		0x37u	/* out: the GPU voltage, mV: the cross-check against GetGfxVid */
#define BC250_CPU_MSG_READ_PSTATE_MHZ		0x3Bu	/* in: P-state 0..7; out: its clock. The recorded baseline */
#define BC250_CPU_MSG_READ_CAP_C		0x40u	/* out: the CPU temperature cap in force. 0x8B's readback */
#define BC250_CPU_MSG_READ_SOC_MHZ		0x42u	/* in: (index & 0xFFFF) << 16, 0..19; out: a SoC DPM clock */
#define BC250_CPU_MSG_READ_CORE_MHZ		0x43u	/* in: core 0..7; out: its effective clock. Clock stretching */
#define BC250_CPU_MSG_SET_CURVE_SCALE		0x50u	/* in: a signed 16-bit scale, packed low. Undervolt only */
#define BC250_CPU_MSG_SET_CAP_C			0x8Bu	/* in: whole Celsius */
#define BC250_CPU_MSG_SET_MAX_MHZ		0x8Fu	/* in: plain MHz. The maximum boost clock */

#define BC250_CPU_CORES		8u	/* the part is sold with 6 of these 8 enabled; QueryCorePstate refuses above 7 */
#define BC250_CPU_PSTATES	8u
#define BC250_CPU_SOC_CLOCKS	20u

/* The allowlist. write=1 is a setter, write=0 a getter; a getter sent as a setter and a setter sent as a getter
 * are both refused, so the caller cannot mislabel a message to get past the gate. driver/kmd/smu.c consults the
 * list of the domain the transaction declared, so a GFX clock transaction can never send a CPU message and a CPU
 * transaction can never send a clock message. bc250_clock_message_allowed() stays exactly as it is. */
int bc250_cpu_message_allowed(unsigned int queue, unsigned int message, int write);
/* The per-message argument check: ranges, the reserved bits, and the signed packing of the curve scale. A
 * message on the allowlist with an argument outside its form is refused before the mailbox. */
int bc250_cpu_argument_allowed(unsigned int queue, unsigned int message, unsigned int parameter);

/* ---- the admitted ranges ----------------------------------------------------------------------- */

/* The clock control is a limit before it is ever a raise (docs/design/tuner.md, part B). BC250_CPU_MAX_MHZ is the
 * highest value the release admits: a lowering of the firmware's own ceiling, which lowers the voltage the
 * firmware chooses and so cannot reach the bricking hazard. BC250_CPU_MAX_MHZ_LAB is the lab's own bound under
 * the owner's pre-approval of 2026-10-05, and nothing admits it without an undervolt already in force and a
 * voltage readback under BC250_CPU_REFUSE_MV.
 *
 * BC250_CPU_MAX_MHZ is 3500 MHz, the only stock boost figure any source gives (the community's own stock
 * constant, and its detect tool's floor). 3600 stood here until 0.7.211 and no source carried it, so the
 * release admitted a 100 MHz raise over stock with no undervolt at all - the shape that destroyed a board.
 * Our own record for unit A is an effective 2.74 to 2.79 GHz, which trial B4 settles; until it does, the
 * release asks for no more than stock. A clock above the release bound needs an undervolt of at least
 * BC250_CPU_LAB_MIN_UV_STEPS and not merely one step: one step is about 5 mV at 3500 MHz by the community's
 * own fitted model, which is not "an undervolt already in force". */
#define BC250_CPU_MIN_MHZ		2800u
#define BC250_CPU_MAX_MHZ		3500u
#define BC250_CPU_MAX_MHZ_LAB		4000u
#define BC250_CPU_LAB_MIN_UV_STEPS	4u	/* about 13 to 17 mV: what the lab bound needs first */
/* The undervolt is a step count, not a millivolt count: one step of the firmware's curve scale removes about
 * 0.004325 x f - 10 mV, so 3.0 mV at 3000 MHz and 7.3 mV at 4000 MHz (REPORTED, the community's own fitted
 * model). The reference Control Center's "6.25 mV a step" is exact near 3745 MHz only. The applied voltage
 * therefore comes from BC250_CPU_MSG_READ_CPU_MV, never from arithmetic on the step count. */
#define BC250_CPU_UV_MAX_STEPS		16u	/* about 50 to 115 mV; the reference tool's own GUI stops at 40 */
#define BC250_CPU_TEMP_MIN_C		85u
#define BC250_CPU_TEMP_MAX_C		100u	/* the firmware default, and the value a restore puts back */
/* Above this the driver reverts at once. The reported bricking ceiling is 1325 mV; we never go near it. */
#define BC250_CPU_REFUSE_MV		1300u
#define BC250_CPU_PLAUSIBLE_MIN_MV	700u	/* a readback outside this band is not a voltage we understand */
#define BC250_CPU_PLAUSIBLE_MAX_MV	1600u
/* Clock stretching: the effective clock this far or further under the target is the failure sign. The reference
 * GUI aborts at 200 MHz under, its detect tool at 50 MHz under. */
#define BC250_CPU_STRETCH_MHZ		200u
/* No CPU message while the GPU is this busy or busier: "no mailbox traffic during sustained compute"
 * (docs/design/rejected-options.md). The same share the idle state leaves on. */
#define BC250_CPU_GPU_BUSY_PERMILLE	500u
#define BC250_CPU_MESSAGE_GAP_MS	100u	/* one setter per this, and the lock is released between them */
/* A getter changes nothing, so it needs the firmware's mailbox turnaround and not a settling time. This keeps a
 * whole read stage (three single getters, eight P-states, eight cores) inside a fifth of a second. */
#define BC250_CPU_GETTER_GAP_MS		10u

/* What a start applies, what a trial carries and what the driver caches. Each value has an "is given" flag, so
 * one of the three can change alone. */
struct bc250_cpu_settings {
	int		max_given;	unsigned int max_mhz;
	int		uv_given;	unsigned int uv_steps;	/* 0..BC250_CPU_UV_MAX_STEPS, sent negated */
	int		temp_given;	unsigned int temp_c;
};

/* Why a request was refused. Shared with the escape and the CLI. */
enum bc250_cpu_error {
	BC250_CPU_OK = 0,
	BC250_CPU_ERROR_CLOCK = 1,	/* max_mhz outside the admitted range */
	BC250_CPU_ERROR_UV = 2,		/* uv_steps above BC250_CPU_UV_MAX_STEPS */
	BC250_CPU_ERROR_TEMP = 3,	/* temp_c outside MIN..MAX_TEMP_C */
	BC250_CPU_ERROR_NOTHING = 4,	/* no value given, or none of them differs from what is applied */
	BC250_CPU_ERROR_PLAN = 5,	/* the plan would need more steps than BC250_CPU_PLAN_MAX */
	BC250_CPU_ERROR_NO_CEILING = 6,	/* a clock limit, and this start does not know the firmware's own ceiling:
					 * the limit could not be given back (struct bc250_cpu_baseline) */
	BC250_CPU_ERROR_COUNT
};
/* Ranges alone, against the release bound or the lab bound (lab=1 admits up to BC250_CPU_MAX_MHZ_LAB and needs
 * an undervolt: uv_steps must then be at least 1). */
enum bc250_cpu_error bc250_cpu_settings_check(const struct bc250_cpu_settings *s, int lab);

/* ---- the order a change is sent in ------------------------------------------------------------- */

/* The predicted voltage must never transiently exceed the ceiling (REPORTED, and the reference implementation's
 * own rule), so every step that lowers the voltage goes before every step that raises it, and each of the two
 * controls is judged on its own:
 *   lowers the voltage   a deeper undervolt; a lower clock limit, the first limit of all included
 *   raises it            a shallower undervolt; a higher clock limit
 * A deeper undervolt with a higher limit therefore sends the undervolt first, and the usual way back - a lower
 * clock with less undervolt - sends the clock first. The temperature cap is its own step: a lowering (more
 * protection) goes first of all, a raise (less protection) last of all. Every step is one message,
 * BC250_CPU_MESSAGE_GAP_MS apart, with a temperature read before each. */
#define BC250_CPU_PLAN_MAX	3u
enum bc250_cpu_step_kind {
	BC250_CPU_STEP_NONE = 0,
	BC250_CPU_STEP_TEMP = 1,
	BC250_CPU_STEP_UV = 2,
	BC250_CPU_STEP_CLOCK = 3
};
/* cools: the step lowers the dissipation (a deeper undervolt, a lower clock limit, a tightened temperature
 * cap), so driver/kmd/smu.c admits it at or above BC250_CLOCK_HOT_MC. Every other step waits to cool. */
struct bc250_cpu_step { unsigned int queue, message, parameter, kind, cools; };
struct bc250_cpu_plan { struct bc250_cpu_step step[BC250_CPU_PLAN_MAX]; unsigned int count; };
/* The steps that take the chip from *from to *to, in the safe order. A value *to does not give keeps whatever
 * *from has. Returns BC250_CPU_ERROR_NOTHING when nothing would change. */
enum bc250_cpu_error bc250_cpu_plan(const struct bc250_cpu_settings *from, const struct bc250_cpu_settings *to,
				    int lab, struct bc250_cpu_plan *plan);
/* The signed curve scale as the mailbox argument: a step count of n is sent as -n, packed into the low 16 bits. */
unsigned int bc250_cpu_scale_argument(unsigned int uv_steps);

/* The target a revert or a reset must ask for, named in full (0.7.211). bc250_cpu_plan lets a target inherit
 * every value it does not give from *from, which is right for a SET of one control and wrong for the way back:
 * a revert to a state that gave nothing inherits the trial itself, the plan is BC250_CPU_ERROR_NOTHING, and the
 * trial stays in the chip while the driver reports that it came back. This names every control *from carries:
 *   *before    the state before the trial. Every value it gives is used as it stands.
 *   *baseline  what the read stage of this start answered before the first write of this start.
 * A control neither of them names goes back to the firmware's own default: no undervolt, and the cap at
 * BC250_CPU_TEMP_MAX_C. The clock limit has no such default - the firmware's own ceiling is what the baseline
 * read is for - so the return value is 0 when *from carries a clock limit that neither *before nor *baseline
 * can name. *out then holds every control that can be named, and the caller must say which one stays. */
int bc250_cpu_restore_target(const struct bc250_cpu_settings *from, const struct bc250_cpu_settings *before,
			     const struct bc250_cpu_settings *baseline, struct bc250_cpu_settings *out);

/* What the read stage learned about the clock the chip runs at by itself (0.7.216.23, BD-094). Two answers of the
 * firmware carry it, and they are not the same number:
 *   table_mhz  the highest P-state clock (BC250_CPU_MSG_READ_PSTATE_MHZ). It is the top of the named P-states,
 *              3200 MHz on unit A, and it is NOT the boost: the firmware grants one busy core 3481 to 3500 MHz
 *              above it. This is the clock a loaded core is judged against in the undervolt search.
 *   boost_mhz  the highest per-core clock (BC250_CPU_MSG_READ_CORE_MHZ) that is a clock of this part. A core
 *              answers the clock of the moment, so an idle core answers far under the floor and counts as no
 *              answer. Any answer inside the band was given to a core that was boosting, so it is a lower bound
 *              of the firmware's own ceiling.
 *   mhz        the clock limit a restore asks for: the higher of the two, clamped to BC250_CPU_MAX_MHZ (the
 *              release bound, so a restore never asks for more than stock). 0 when neither answered.
 *   boost_given  1 when boost_mhz carries an answer. Without it mhz is the P-state table alone, and sending it
 *              as a restore CUTS the boost the firmware had given: that is BD-094 (0x8F 3200 held one busy
 *              thread near 3180 MHz until a restart, K220). The driver therefore takes no clock limit it could
 *              only give back from the table (bc250_cpu_boost_probe_needed, driver/kmd/cpu.c).
 * No number here is invented by the driver; the release bound only clamps. */
struct bc250_cpu_baseline {
	unsigned int	mhz;		/* the clock limit of a restore, 0 for none */
	unsigned int	table_mhz;	/* the P-state table's top, in band, 0 for none */
	unsigned int	boost_mhz;	/* the highest per-core answer in band, 0 for none */
	int		boost_given;	/* 1 when boost_mhz answered: a restore cannot cut the boost */
};

/* One read stage into *out. *previous is what this start recorded already (NULL or a zeroed record for none):
 * every top in it is a floor, so a later read stage of the same start raises the baseline and never lowers it.
 * An answer outside BC250_CPU_MIN_MHZ..BC250_CPU_MAX_MHZ_LAB is not a clock of this part and counts as no answer
 * (a core that did not answer reads 0, an idle core reads far under the floor, and junk never clamps to the
 * release bound). pstate_mhz or core_mhz may be NULL with a count of 0 for a stage that did not read it. */
void bc250_cpu_baseline_read(const unsigned int *pstate_mhz, unsigned int pstates,
			     const unsigned int *core_mhz, unsigned int cores,
			     const struct bc250_cpu_baseline *previous, struct bc250_cpu_baseline *out);

/* 1 when the boost ceiling of this start is still unknown, so the read stage owes a boost probe: a short busy
 * window on one core, after which the per-core clocks answer the ceiling (BC250_CPU_BOOST_PROBE_MS,
 * BC250_CPU_BOOST_PROBE_ROUNDS; driver/kmd/cpu.c CpuBoostProbe). The firmware has no message for the ceiling
 * itself - the read allowlist of docs/hardware.md holds no GetMaxBoostMHz - so a busy window is the only way to
 * make it answer. A record that already carries a per-core answer owes nothing. */
int bc250_cpu_boost_probe_needed(const struct bc250_cpu_baseline *baseline);

/* The boost probe's bounds: one busy window before the per-core clocks are read again, and at most this many
 * windows. The probe stops at the first answer inside the band, so the usual cost is one window plus the
 * getters of one core. Worst case it keeps one core of six busy for about a fifth of a second, once per start,
 * and only on a start whose cores were all idle when the stage read them. */
#define BC250_CPU_BOOST_PROBE_MS	25u
#define BC250_CPU_BOOST_PROBE_ROUNDS	2u

/* ---- what the hardware said, and the three failure signs --------------------------------------- */

struct bc250_cpu_sample {
	unsigned int	voltage_mv;			/* BC250_CPU_MSG_READ_CPU_MV */
	unsigned int	core_mhz[BC250_CPU_CORES];	/* BC250_CPU_MSG_READ_CORE_MHZ, 0 for a core that did not answer */
	unsigned int	cores;				/* how many of the array are filled */
	unsigned int	target_mhz;			/* the clock the cores should reach: the applied limit, or the
						 * recorded baseline when no limit is applied. 0: unknown */
	int		temperature_mc;
	int		temperature_valid;
	int		loaded;				/* the caller loads the CPU over this sample (0.7.211) */
	unsigned int	whea_events;			/* machine-check events since the trial began */
	unsigned int	checksum_errors;		/* wrong answers from the load client */
};
/* Why a trial must stop. Three signs, and all three are available on Windows: a WHEA event, clock stretching
 * (two independent reads: the SMU's own per-core clock and the Windows processor counters), and a wrong answer
 * from the load. A temperature at or above the lab's 87 C stops a trial as well.
 *
 * Two of the five need the caller and are 0 until it gives them (0.7.211): whea_events and checksum_errors
 * arrive on the escape, and clock stretching is judged only over a sample the caller marked loaded. An idle
 * core sits far under any limit, so an unloaded sample would otherwise fail every step of the search. */
enum bc250_cpu_fail {
	BC250_CPU_FAIL_NONE = 0,
	BC250_CPU_FAIL_WHEA = 1,
	BC250_CPU_FAIL_CHECKSUM = 2,
	BC250_CPU_FAIL_STRETCH = 3,
	BC250_CPU_FAIL_VOLTAGE = 4,	/* the readback is above BC250_CPU_REFUSE_MV or outside the plausible band */
	BC250_CPU_FAIL_HOT = 5,
	BC250_CPU_FAIL_COUNT
};
enum bc250_cpu_fail bc250_cpu_check_sample(const struct bc250_cpu_sample *s);

/* ---- the guided undervolt search --------------------------------------------------------------- */

/* "Find my setting": one step at a time, BC250_CPU_SEARCH_LOAD_MS of load per step, at most
 * BC250_CPU_SEARCH_MAX_STEPS steps, so the whole run fits inside a three-minute lab trial. It stops at the first
 * failure sign, steps one back and reports the last step that passed. It never persists anything: the step found
 * is offered, and a person presses Keep. The reference wizard walks two steps at a time with 12 s of load; one
 * step at a time is slower and finds the boundary exactly. */
#define BC250_CPU_SEARCH_MAX_STEPS	8u
#define BC250_CPU_SEARCH_LOAD_MS	15000u
struct bc250_cpu_search {
	int		running;
	unsigned int	step;			/* the step under test, 1.. */
	unsigned int	max_steps, load_ms;
	unsigned int	best;			/* the deepest step that passed, 0 for none */
	unsigned int	baseline_mv, baseline_mhz;
	unsigned int	fail;			/* enum bc250_cpu_fail of the step that failed, 0 for none */
	unsigned int	tested;
};
enum bc250_cpu_search_action {
	BC250_CPU_SEARCH_APPLY = 0,	/* apply search->step as a trial and load for load_ms, then call _next again */
	BC250_CPU_SEARCH_DONE = 1	/* restore the baseline; search->best is the answer to offer */
};
void bc250_cpu_search_begin(struct bc250_cpu_search *s, unsigned int max_steps, unsigned int load_ms,
			    unsigned int baseline_mv, unsigned int baseline_mhz);
/* The first call (sample NULL) asks for step 1. Every later call judges the sample of the step just loaded. */
enum bc250_cpu_search_action bc250_cpu_search_next(struct bc250_cpu_search *s, const struct bc250_cpu_sample *sample);

/* ---- the core-enable mask ---------------------------------------------------------------------- */

/* The part is sold with 6 of its 8 cores enabled; the stock mask is 0x77, which masks the fourth core of each
 * four-core complex off. Our route is the AMD-named queue 0 message, which can write the stock mask back; the
 * community's generic SMN write through queue 3 0x98 can only ever write 0xFF and hangs the firmware with
 * argument 0, so it stays rejected (docs/design/rejected-options.md). Nobody has reported a result from 0x2C on
 * this part, so one lab trial settles whether it is a live route or a no-op on a harvested die - and either
 * answer closes an open question. The cores appear after a reset, so this is the CU mode's shape: a registry
 * value, a two-step boot guard, and "after the next restart" in the window. */
#define BC250_CPU_MASK_STOCK	0x77u
#define BC250_CPU_MASK_FULL	0xFFu
int bc250_cpu_mask_allowed(unsigned int mask);
/* How many cores a mask names, for the log and the report. */
unsigned int bc250_cpu_mask_cores(unsigned int mask);

#endif
