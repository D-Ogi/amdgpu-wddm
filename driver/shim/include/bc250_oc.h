#include "bc250_board_envelope.h"
/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * BC-250 overclock tiers by cooling type (docs/design/dpm.md, "Overclock tiers"; docs/hardware.md). Pure: no locks,
 * no registry, no SMU. driver/kmd/oc_tier.c reads the stored tier and the board it was chosen on, and
 * driver/shim/test/oc_test.c runs exactly what the miniport runs.
 *
 * A tier is a choice of the person who owns the board, made in the control application after it asked for the
 * cooling type and three confirmations. The driver never takes a number from that person: the tier selects one of
 * the compiled envelopes below, and the GPU clock, the GPU voltage and the CPU boost limit all come from it. The
 * thermal rules (87 C, 90 C, the fan) and the power supply rule do not change with the tier.
 *
 * Every number of an envelope is third-party: community reports of BC-250 boards on air and on all-in-one liquid
 * coolers, collected in the project's research notes. Nobody has measured one on unit A. The community also
 * reports a board-level over-current lock on 40-CU boards from about 1850-2200 MHz (facts M790); a tier does not
 * remove that hazard, it only admits clocks inside its band.
 */
#ifndef BC250_OC_H
#define BC250_OC_H

/* What a tier admits. gpu_max_level is the index of gpu_max_mhz in bc250_clock_points. A CPU clock limit above
 * BC250_CPU_MAX_MHZ also needs the undervolt of bc250_oc_cpu_min_uv_steps. */
struct bc250_oc_envelope {
	unsigned int tier;
	unsigned int gpu_max_mhz, gpu_max_mv, gpu_max_level;
	unsigned int cpu_max_mhz, cpu_uv_max_steps;
};

/* The envelope of a tier. An unknown tier gets the envelope of BC250_OC_TIER_NONE and the return value 0. */
static __inline int bc250_oc_envelope_of(unsigned int tier, struct bc250_oc_envelope *e)
{
	e->tier = BC250_OC_TIER_NONE;
	e->gpu_max_mhz = BC250_CLOCK_CEILING_MHZ;
	e->gpu_max_mv = BC250_CLOCK_CEILING_MV;
	e->cpu_max_mhz = BC250_CPU_MAX_MHZ;
	e->cpu_uv_max_steps = BC250_CPU_UV_MAX_STEPS;
	if (tier == BC250_OC_TIER_AIR) {
		e->tier = tier;
		e->gpu_max_mhz = BC250_OC_AIR_GPU_MAX_MHZ;
		e->gpu_max_mv = BC250_OC_AIR_GPU_MAX_MV;
		e->cpu_max_mhz = BC250_OC_AIR_CPU_MAX_MHZ;
		e->cpu_uv_max_steps = BC250_CPU_OC_UV_MAX_STEPS;
	} else if (tier == BC250_OC_TIER_AIO) {
		e->tier = tier;
		e->gpu_max_mhz = BC250_OC_AIO_GPU_MAX_MHZ;
		e->gpu_max_mv = BC250_OC_AIO_GPU_MAX_MV;
		e->cpu_max_mhz = BC250_OC_AIO_CPU_MAX_MHZ;
		e->cpu_uv_max_steps = BC250_CPU_OC_UV_MAX_STEPS;
	}
	e->gpu_max_level = (e->gpu_max_mhz - BC250_CLOCK_MIN_MHZ) / BC250_CLOCK_STEP_MHZ;
	return e->tier == tier;
}

/* The least undervolt, in curve-scale steps, that a CPU clock limit needs under a tier. 0 up to the release bound
 * BC250_CPU_MAX_MHZ. Above it, the community's own pairing of clock and curve scale at the same clock: SetMaxBoost
 * alone lets the firmware's voltage scale up with the clock, which is how a board was destroyed (docs/hardware.md),
 * so a tier clock never goes out without the depth that keeps the reported readback near 1.2 V. The 1300 mV
 * readback refusal of the CPU surface stays the last line. The tier none answers BC250_CPU_LAB_MIN_UV_STEPS above
 * the release bound, which is the lab bound's own rule. An answer above the tier's cpu_uv_max_steps means that
 * clock is outside the tier. */
static __inline unsigned int bc250_oc_cpu_min_uv_steps(unsigned int tier, unsigned int mhz)
{
	if (mhz <= BC250_CPU_MAX_MHZ) return 0u;
	if (tier != BC250_OC_TIER_AIR && tier != BC250_OC_TIER_AIO) return BC250_CPU_LAB_MIN_UV_STEPS;
	if (mhz <= 3600u) return 8u;
	if (mhz <= 3700u) return 16u;
	if (mhz <= 3800u) return 20u;
	if (mhz <= 3900u) return 24u;
	if (mhz <= 4000u) return 36u;
	return BC250_CPU_OC_UV_MAX_STEPS;
}

/* ---- the stored tier and the board it belongs to ----------------------------------------------- */

/* Why this start runs the tier it runs. Shared with the escape, the record OcTierLastReason and the application. */
enum bc250_oc_reason {
	BC250_OC_REASON_NONE = 0,		/* no tier is stored: the default envelope */
	BC250_OC_REASON_OK = 1,			/* the stored tier runs */
	BC250_OC_REASON_INVALID = 2,		/* OcTier is not a tier: the default envelope, the value is kept */
	BC250_OC_REASON_UNSUPPORTED = 3,	/* this device is not a positively identified BC-250 */
	BC250_OC_REASON_UNBOUND = 4,		/* a tier without the board it was chosen on: deleted */
	BC250_OC_REASON_BOARD_CHANGED = 5,	/* the tier was chosen on another board: deleted */
	BC250_OC_REASON_COUNT
};

struct bc250_oc_request {
	int		supported;		/* a positively identified BC-250 with a qualified firmware identity */
	unsigned int	current_board;		/* bc250_oc_board_fingerprint of this start, never 0 when supported */
	int		tier_present;		/* OcTier */
	unsigned int	tier;
	int		board_present;		/* OcTierBoard: the fingerprint the application wrote with the tier */
	unsigned int	board;
};

struct bc250_oc_decision {
	unsigned int	tier;			/* the tier this start runs */
	unsigned int	reason;			/* enum bc250_oc_reason */
	int		clear;			/* persist: delete OcTier and OcTierBoard */
};

void bc250_oc_decide(const struct bc250_oc_request *r, struct bc250_oc_decision *d);

/* The board a tier belongs to: FNV-1a over the SMBIOS system UUID and the qualified firmware release
 * (driver/kmd/board_identity.c). Never 0, so 0 can mean "no board". A firmware update is a change of the board for
 * this purpose: the person answers the questions again. Two boards that carry the same vendor UUID are not told
 * apart; the tier is a promise about one machine's cooling, not a hardware lock. */
unsigned int bc250_oc_board_fingerprint(const unsigned char machine_id[16], unsigned int bios_id);

#endif
