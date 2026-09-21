/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * SDMA 5.0 ring bring-up for Cyan Skillfish. See include/bc250_sdma.h for what this is, what is
 * deliberately missing and why it is transcribed rather than imported.
 *
 * Every block names the upstream function and line it follows, in
 * driver/amdgpu-import/sdma_v5_0.c (unmodified copy of the kernel's
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c at v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449) or in nbio_v2_3.c at the same tag.
 */
#include "bc250_sdma.h"
/* The BC250_E* return codes are split across the two earlier headers - BC250_EINVAL and
 * BC250_ETIME in bc250_gmc.h from M4, BC250_ENOMEM and BC250_EIO in bc250_gfx.h - so both are
 * included here rather than a third copy being made. */
#include "bc250_gmc.h"
#include "bc250_gfx.h"

/* Our own file, so register offsets come through the SOC15 macros over AMD's headers. No address,
 * mask or shift below is written out; every one of them is a name from these. */
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"

/* The one NBIO register the ring setup writes, in its own file because it is nbio_v2_3.c's. */
#include "bc250_nbio.h"

/* Imported data, unmodified: the two UTCL2 cache-policy enums the UTCL1_PAGE write uses. */
#include "sdma_common.h"

/* AMD's SDMA 5.0 packet definitions, imported unmodified at v6.18 (see the PROVENANCE row). Until
 * M6 nothing here emitted an SDMA packet and the note below said to import this header the day one
 * was needed rather than write the bits out. That day is the fence and the ring test at the end of
 * this file, so it is imported, and no opcode, field shift or mask below is typed. */
#include "navi10_sdma_pkt_open.h"

/* ---------------------------------------------------------------------------------------------
 * Constants
 * ------------------------------------------------------------------------------------------- */

/* sdma_v5_0.c:57-60. The E03 trace confirms the first of them: SDMA0_CHICKEN_BITS is at byte
 * offset 0x049F4 and SDMA1_CHICKEN_BITS at 0x061F4, which is 0x600 dwords apart. */
#define SDMA1_REG_OFFSET		0x600
#define SDMA0_HYP_DEC_REG_START		0x5880
#define SDMA0_HYP_DEC_REG_END		0x5893
#define SDMA1_HYP_DEC_REG_OFFSET	0x20

/* amdgpu_ring.c:114, as in bc250_gfx.c: ring_size = roundup_pow_of_two(max_dw * 4 * 2) with the
 * max_dw of 1024 that sdma_v5_0_sw_init() passes to amdgpu_ring_init(). 8 KB.
 *
 * Not taken on trust either: the traced SDMA0_GFX_RB_CNTL write is 0x80840016, whose RB_SIZE field
 * is 11, and order_base_2(ring_size / 4) = 11 gives exactly 8 KB. */
#define BC250_SDMA_RING_MAX_DW	1024u
#define BC250_SDMA_RING_SIZE	(BC250_SDMA_RING_MAX_DW * 4u * 2u)

/* sdma_v5_0.c:1941-1944 sdma_v5_0_ring_funcs: align_mask 0xf, and `.nop` is
 * SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP). Now that the header is imported the fill value is that
 * expression rather than the zero it works out to. */
#define BC250_SDMA_ALIGN_MASK	0xfu
#define BC250_SDMA_NOP		SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP)

/* nbio_v2_3.c:266 nbio_v2_3_sdma_doorbell_range(..., 20): the doorbell window size, in doorbells,
 * that amdgpu reserves per SDMA engine. The trace writes SIZE = 20 into both
 * BIF_SDMA*_DOORBELL_RANGE. */
#define BC250_SDMA_DOORBELL_RANGE_SIZE	20

/* [shim] the writeback page, cut the same way bc250_gfx.c cuts its own. */
#define BC250_SDMA_WB_SLOT_BYTES	8u
#define BC250_SDMA_WB_BYTES		(AMDGPU_MAX_SDMA_INSTANCES * 2u * BC250_SDMA_WB_SLOT_BYTES)

static const struct amdgpu_ring_funcs bc250_ring_funcs_sdma = {
	AMDGPU_RING_TYPE_SDMA, BC250_SDMA_ALIGN_MASK, BC250_SDMA_NOP
};

/* ---------------------------------------------------------------------------------------------
 * sdma_v5_0.c:218 sdma_v5_0_get_reg_offset()
 *
 * The hypervisor-decode branch is kept although nothing this sequence touches falls in that window
 * (the registers used below are internal offsets 0x1c to 0xb5, the window starts at 0x5880), so
 * that a later caller reaching for one of those registers gets the right answer instead of a
 * plausible wrong one.
 * ------------------------------------------------------------------------------------------- */
u32 bc250_sdma_reg_offset(struct amdgpu_device *adev, u32 instance, u32 internal_offset)
{
	u32 base;

	if (internal_offset >= SDMA0_HYP_DEC_REG_START &&
	    internal_offset <= SDMA0_HYP_DEC_REG_END) {
		base = adev->reg_offset[GC_HWIP][0][1];
		if (instance == 1)
			internal_offset += SDMA1_HYP_DEC_REG_OFFSET;
	} else {
		base = adev->reg_offset[GC_HWIP][0][0];
		if (instance == 1)
			internal_offset += SDMA1_REG_OFFSET;
	}

	return base + internal_offset;
}

/* ---------------------------------------------------------------------------------------------
 * Golden registers
 *
 * soc15.c:471 soc15_program_register_sequence() again, over the imported
 * golden_settings_sdma_cyan_skillfish table (sdma_v5_0.c:187-215, extracted by
 * tools/import/extract_table.py). sdma_v5_0_init_golden_registers() picks this table on the
 * IP_VERSION(5, 0, 5) arm, which is Cyan Skillfish.
 *
 * Checked against unit A: all 28 entries reproduce the trace exactly, offset for offset and value
 * for value, and the next traced access after the table is the SDMA0_F32_CNTL read of the unhalt
 * below (P:\BC-250\scratch\m5-gfx\check_golden_sdma.py).
 *
 * The same two upstream branches are left out as in bc250_gfx.c's copy of this loop, for the same
 * reasons: every entry is GC_HWIP, and the RLC register-access path is off on this part.
 * ------------------------------------------------------------------------------------------- */

/* [amdgpu] soc15.h. Declared again here rather than shared with bc250_gfx.c: both are private
 * copies of an upstream declaration, and a shim-wide header for it would invite treating it as our
 * own type. */
struct soc15_reg_golden {
	u32 hwip;
	u32 instance;
	u32 segment;
	u32 reg;
	u32 and_mask;
	u32 or_mask;
};

#define SOC15_REG_GOLDEN_VALUE(ip, inst, reg, and_mask, or_mask) \
	{ ip##_HWIP, inst, reg##_BASE_IDX, reg, and_mask, or_mask }

static const struct soc15_reg_golden golden_settings_sdma_cyan_skillfish[] = {
#include "generated/sdma5_golden_cyan_skillfish.inc"
};

int bc250_sdma_init_golden_registers(struct amdgpu_device *adev)
{
	const struct soc15_reg_golden *entry;
	u32 tmp, reg;
	unsigned int i;

	if (adev == NULL)
		return BC250_EINVAL;

	for (i = 0; i < ARRAY_SIZE(golden_settings_sdma_cyan_skillfish); ++i) {
		entry = &golden_settings_sdma_cyan_skillfish[i];
		reg = adev->reg_offset[entry->hwip][entry->instance][entry->segment] + entry->reg;

		if (entry->and_mask == 0xffffffff) {
			tmp = entry->or_mask;
		} else {
			tmp = RREG32(reg);
			tmp &= ~(entry->and_mask);
			tmp |= (entry->or_mask & entry->and_mask);
		}

		WREG32(reg, tmp);
	}

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * sdma_v5_0.c:656 sdma_v5_0_enable()
 *
 * The !enable arm calls sdma_v5_0_gfx_stop() and sdma_v5_0_rlc_stop(); the first is transcribed
 * below and the second is an upstream stub, so the disable path is written out in
 * bc250_sdma_hw_fini() instead of carrying an `enable` flag through three functions.
 * ------------------------------------------------------------------------------------------- */
static void bc250_sdma_enable(struct amdgpu_device *adev, bool enable)
{
	u32 f32_cntl;
	int i;

	for (i = 0; i < adev->sdma.num_instances; i++) {
		f32_cntl = RREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_F32_CNTL));
		f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, enable ? 0 : 1);
		WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_F32_CNTL), f32_cntl);
	}
}

/* ---------------------------------------------------------------------------------------------
 * sdma_v5_0.c:598 sdma_v5_0_ctx_switch_enable()
 *
 * amdgpu_sdma_phase_quantum is a module parameter, default 32. The loop upstream turns that number
 * into a (VALUE, UNIT) pair by halving it until it fits the VALUE field; 32 fits, so UNIT stays 0
 * and the register value is 32 << VALUE_SHIFT. The trace writes 0x00002000 to all three quantum
 * registers, which is exactly that, so unit A ran with the default and the clamp loop is
 * transcribed rather than folded away.
 * ------------------------------------------------------------------------------------------- */
#define BC250_SDMA_PHASE_QUANTUM	32u

static void bc250_sdma_ctx_switch_enable(struct amdgpu_device *adev, bool enable)
{
	u32 f32_cntl = 0, phase_quantum = 0;
	int i;

	{
		unsigned int value = BC250_SDMA_PHASE_QUANTUM;
		unsigned int unit = 0;

		while (value > (SDMA0_PHASE0_QUANTUM__VALUE_MASK >>
				SDMA0_PHASE0_QUANTUM__VALUE__SHIFT)) {
			value = (value + 1) >> 1;
			unit++;
		}
		if (unit > (SDMA0_PHASE0_QUANTUM__UNIT_MASK >>
			    SDMA0_PHASE0_QUANTUM__UNIT__SHIFT)) {
			value = (SDMA0_PHASE0_QUANTUM__VALUE_MASK >>
				 SDMA0_PHASE0_QUANTUM__VALUE__SHIFT);
			unit = (SDMA0_PHASE0_QUANTUM__UNIT_MASK >>
				SDMA0_PHASE0_QUANTUM__UNIT__SHIFT);
			dev_warn(adev->dev,
				 "clamping sdma_phase_quantum to %uK clock cycles\n",
				 value << unit);
		}
		phase_quantum =
			value << SDMA0_PHASE0_QUANTUM__VALUE__SHIFT |
			unit  << SDMA0_PHASE0_QUANTUM__UNIT__SHIFT;
	}

	for (i = 0; i < adev->sdma.num_instances; i++) {
		f32_cntl = RREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_CNTL));
		f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_CNTL,
					 AUTO_CTXSW_ENABLE, enable ? 1 : 0);

		if (enable) {
			WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_PHASE0_QUANTUM),
					phase_quantum);
			WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_PHASE1_QUANTUM),
					phase_quantum);
			WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_PHASE2_QUANTUM),
					phase_quantum);
		}

		WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_CNTL), f32_cntl);
	}
}

/* ---------------------------------------------------------------------------------------------
 * sdma_v5_0.c:688 sdma_v5_0_gfx_resume_instance(), restore = false
 *
 * The restore arm reprograms the pointers from a saved wptr after an engine reset; bring-up is the
 * fresh case, so the branch is not carried. The SR-IOV guards are all on the bare-metal side, which
 * is this one (amdgpu_sriov_vf() is false on this device, see amdgpu.h).
 *
 * The write pointer of an SDMA ring counts bytes, not dwords, which is where the `<< 2` comes from;
 * bc250_ring.c keeps the dword convention of the CP rings and says so.
 * ------------------------------------------------------------------------------------------- */
static int bc250_sdma_gfx_resume_instance(struct amdgpu_device *adev, int i)
{
	struct amdgpu_ring *ring;
	u32 rb_cntl, ib_cntl;
	u32 rb_bufsz;
	u32 doorbell;
	u32 doorbell_offset;
	u32 temp;
	u32 wptr_poll_cntl;
	u64 wptr_gpu_addr;

	ring = &adev->sdma.instance[i].ring;

	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_SEM_WAIT_FAIL_TIMER_CNTL), 0);

	/* Set ring buffer size in dwords */
	rb_bufsz = order_base_2(ring->ring_size / 4);
	rb_cntl = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_CNTL));
	rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_SIZE, rb_bufsz);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_CNTL), rb_cntl);

	/* Initialize the ring buffer's read and write pointers */
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR), 0);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR_HI), 0);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR), 0);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_HI), 0);

	/* setup the wptr shadow polling */
	wptr_gpu_addr = ring->wptr_gpu_addr;
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO),
			lower_32_bits(wptr_gpu_addr));
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI),
			upper_32_bits(wptr_gpu_addr));
	wptr_poll_cntl = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
					 mmSDMA0_GFX_RB_WPTR_POLL_CNTL));
	wptr_poll_cntl = REG_SET_FIELD(wptr_poll_cntl,
				       SDMA0_GFX_RB_WPTR_POLL_CNTL,
				       F32_POLL_ENABLE, 1);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_POLL_CNTL),
			wptr_poll_cntl);

	/* set the wb address whether it's enabled or not */
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR_ADDR_HI),
			upper_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFF);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR_ADDR_LO),
			lower_32_bits(ring->rptr_gpu_addr) & 0xFFFFFFFC);

	rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RPTR_WRITEBACK_ENABLE, 1);

	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_BASE),
			(u32)(ring->gpu_addr >> 8));
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_BASE_HI),
			(u32)(ring->gpu_addr >> 40));

	ring->wptr = 0;

	/* before programing wptr to a less value, need set minor_ptr_update first */
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_MINOR_PTR_UPDATE), 1);

	/* only bare-metal use register write for wptr */
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR),
	       lower_32_bits(ring->wptr << 2));
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_HI),
	       upper_32_bits(ring->wptr << 2));

	doorbell = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_DOORBELL));
	doorbell_offset = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
					  mmSDMA0_GFX_DOORBELL_OFFSET));

	if (ring->use_doorbell) {
		doorbell = REG_SET_FIELD(doorbell, SDMA0_GFX_DOORBELL, ENABLE, 1);
		doorbell_offset = REG_SET_FIELD(doorbell_offset, SDMA0_GFX_DOORBELL_OFFSET,
						OFFSET, ring->doorbell_index);
	} else {
		doorbell = REG_SET_FIELD(doorbell, SDMA0_GFX_DOORBELL, ENABLE, 0);
	}
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_DOORBELL), doorbell);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_DOORBELL_OFFSET),
			doorbell_offset);

	bc250_nbio_sdma_doorbell_range(adev, i, ring->use_doorbell,
				       (int)ring->doorbell_index, BC250_SDMA_DOORBELL_RANGE_SIZE);

	/* set minor_ptr_update to 0 after wptr programed */
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_MINOR_PTR_UPDATE), 0);

	/* set utc l1 enable flag always to 1 */
	temp = RREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_CNTL));
	temp = REG_SET_FIELD(temp, SDMA0_CNTL, UTC_L1_ENABLE, 1);

	/* enable MCBP */
	temp = REG_SET_FIELD(temp, SDMA0_CNTL, MIDCMD_PREEMPT_ENABLE, 1);
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_CNTL), temp);

	/* Set up RESP_MODE to non-copy addresses */
	temp = RREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_UTCL1_CNTL));
	temp = REG_SET_FIELD(temp, SDMA0_UTCL1_CNTL, RESP_MODE, 3);
	temp = REG_SET_FIELD(temp, SDMA0_UTCL1_CNTL, REDO_DELAY, 9);
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_UTCL1_CNTL), temp);

	/* program default cache read and write policy.
	 *
	 * The two constants come from the imported sdma_common.h and are 2 (NOA) and 3 (BYPASS). The
	 * trace agrees: UTCL1_PAGE reads 0x004C5C00, and 0x4C5C00 & 0xFF0FFF | (2 << 12) | (3 << 14)
	 * is 0x004CEC00, which is what it writes. */
	temp = RREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_UTCL1_PAGE));
	/* clean read policy and write policy bits */
	temp &= 0xFF0FFF;
	temp |= ((CACHE_READ_POLICY_L2__DEFAULT << 12) | (CACHE_WRITE_POLICY_L2__DEFAULT << 14));
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_UTCL1_PAGE), temp);

	/* unhalt engine */
	temp = RREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_F32_CNTL));
	temp = REG_SET_FIELD(temp, SDMA0_F32_CNTL, HALT, 0);
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_F32_CNTL), temp);

	/* enable DMA RB */
	rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 1);
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_CNTL), rb_cntl);

	ib_cntl = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_IB_CNTL));
	ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 1);
	/* enable DMA IBs */
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_IB_CNTL), ib_cntl);

	/* Upstream ends with amdgpu_ring_test_helper(ring). See the note in bc250_sdma.h: the SDMA
	 * ring test touches no register, so it cannot be part of a register replay, and it is the
	 * miniport's job to run one before it trusts the engine. */
	return 0;
}

/* sdma_v5_0.c:927 sdma_v5_0_start(), the AMDGPU_FW_LOAD_PSP path: unhalt, enable context
 * switching, then bring the two GFX rings up. sdma_v5_0_rlc_resume() at the end is an upstream
 * stub. */
int bc250_sdma_start(struct amdgpu_device *adev)
{
	int i, r;

	if (adev == NULL)
		return BC250_EINVAL;

	/* unhalt the MEs */
	bc250_sdma_enable(adev, true);
	/* enable sdma ring preemption */
	bc250_sdma_ctx_switch_enable(adev, true);

	for (i = 0; i < adev->sdma.num_instances; i++) {
		r = bc250_sdma_gfx_resume_instance(adev, i);
		if (r)
			return r;
	}

	return 0;
}

/* sdma_v5_0.c:1468 sdma_v5_0_hw_init() */
int bc250_sdma_hw_init(struct amdgpu_device *adev)
{
	int r;

	r = bc250_sdma_init_golden_registers(adev);
	if (r)
		return r;

	return bc250_sdma_start(adev);
}

/* sdma_v5_0.c:1480 sdma_v5_0_hw_fini(), with sdma_v5_0_gfx_stop() (sdma_v5_0.c:563) written out
 * where sdma_v5_0_enable(adev, false) would call it. sdma_v5_0_rlc_stop() is an upstream stub. */
void bc250_sdma_hw_fini(struct amdgpu_device *adev)
{
	u32 rb_cntl, ib_cntl;
	int i;

	if (adev == NULL)
		return;

	bc250_sdma_ctx_switch_enable(adev, false);

	for (i = 0; i < adev->sdma.num_instances; i++) {
		rb_cntl = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_CNTL));
		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 0);
		WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_CNTL), rb_cntl);
		ib_cntl = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_IB_CNTL));
		ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 0);
		WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_IB_CNTL), ib_cntl);
	}

	bc250_sdma_enable(adev, false);
}

/* ---------------------------------------------------------------------------------------------
 * Setup and teardown
 *
 * sdma_v5_0.c:1100 sdma_v5_0_sw_init(), minus the interrupt registration, the firmware, the sysfs
 * node and the IP-dump buffer.
 *
 * The doorbell indices are AMD's own, from the imported amdgpu_doorbell.h, and the trace confirms
 * both: SDMA0_GFX_DOORBELL_OFFSET is written 0x00000800 and the OFFSET field starts at bit 2, so
 * the index is 0x200 = AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE0 (0x100) << 1; SDMA1 is written 0x850,
 * giving 0x214 = 0x10A << 1.
 * ------------------------------------------------------------------------------------------- */
int bc250_sdma_setup(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	int i;
	int r;

	if (adev == NULL)
		return BC250_EINVAL;

	adev->sdma.num_instances = AMDGPU_MAX_SDMA_INSTANCES;

	/* nv_set_ip_blocks() -> nv_init_doorbell_index(), the NAVI10 assignment. */
	adev->doorbell_index.sdma_engine[0] = AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE0;
	adev->doorbell_index.sdma_engine[1] = AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE1;

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_SDMA_WB_BYTES, AMDGPU_GPU_PAGE_SIZE,
				 &adev->sdma.wb_mem);
	if (r)
		goto fail;

	for (i = 0; i < adev->sdma.num_instances; i++) {
		ring = &adev->sdma.instance[i].ring;
		ring->adev = adev;
		ring->funcs = &bc250_ring_funcs_sdma;
		/* sdma_v5_0.c:1878 sdma_v5_0_sw_init(): ring->me is the engine index, and
		 * sdma_v5_0_ring_get_wptr() and the trap handler both read it back. Nothing in the
		 * bring-up used it, so it was not set; the ring test and the fence do. */
		ring->me = (u32)i;
		ring->use_doorbell = true;
		ring->doorbell_index = adev->doorbell_index.sdma_engine[i] << 1;

		r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_SDMA_RING_SIZE,
					 AMDGPU_GPU_PAGE_SIZE, &ring->ring_mem);
		if (r)
			goto fail;
		if (ring->ring_mem.cpu == NULL) {
			r = BC250_EINVAL;
			goto fail;
		}

		ring->ring = (u32 *)ring->ring_mem.cpu;
		ring->gpu_addr = ring->ring_mem.mc;
		ring->ring_size = BC250_SDMA_RING_SIZE;
		ring->buf_mask = (BC250_SDMA_RING_SIZE / 4u) - 1u;
		ring->ptr_mask = 0xffffffffffffffffULL;   /* sdma5 rings support 64 bit pointers */
		ring->max_dw = BC250_SDMA_RING_MAX_DW;

		ring->rptr_gpu_addr = adev->sdma.wb_mem.mc +
				      (u64)(i * 2) * BC250_SDMA_WB_SLOT_BYTES;
		ring->wptr_gpu_addr = adev->sdma.wb_mem.mc +
				      (u64)(i * 2 + 1) * BC250_SDMA_WB_SLOT_BYTES;
		if (adev->sdma.wb_mem.cpu != NULL)
			ring->wptr_cpu_addr = (char *)adev->sdma.wb_mem.cpu +
					      (size_t)(i * 2 + 1) * BC250_SDMA_WB_SLOT_BYTES;

		amdgpu_ring_clear_ring(ring);
	}

	return 0;

fail:
	bc250_sdma_teardown(adev);
	return r;
}

void bc250_sdma_teardown(struct amdgpu_device *adev)
{
	int i;

	if (adev == NULL)
		return;

	for (i = 0; i < AMDGPU_MAX_SDMA_INSTANCES; i++)
		bc250_shim_mem_free(adev, &adev->sdma.instance[i].ring.ring_mem);

	bc250_shim_mem_free(adev, &adev->sdma.wb_mem);
}

/* ---------------------------------------------------------------------------------------------
 * Fences and the ring test (milestone M6)
 *
 * sdma_v5_0.c:523 sdma_v5_0_ring_emit_fence() and sdma_v5_0.c:1012 sdma_v5_0_ring_test_ring().
 *
 * What the packets are is not taken on trust. Unit A's own Linux driver emitted both of them while
 * E13 was recording, and the dwords below are the dwords in those rings:
 *
 *   evidence/linux/2026-09-21-E13-reference-2/boot3-readonly/rings-after-ib/amdgpu_ring_sdma1.txt
 *     dw 0x0000  00000002 004017c0 00000000 00000000 deadbeef      the ring test
 *     dw 0x0005  000a0000 then ten zero dwords                     the pad to sixteen
 *     dw 0x0020  00030005 00401760 00000000 00000001               the fence
 *     dw 0x0024  00000006 00000000                                 the trap
 *
 * and the same shapes nineteen times over in amdgpu_ring_sdma0.txt. Every fence in both rings is
 * the 32-bit form - one FENCE packet carrying the low dword of the sequence - so nothing on this
 * chip asks for AMDGPU_FENCE_FLAG_64BIT; the 64-bit arm below is transcribed anyway, because
 * leaving out an arm of an upstream function is how a driver grows a hole.
 *
 * Deviations, both of the same kind as bc250_gfx_emit_fence()'s:
 *   - upstream BUG_ON()s a misaligned address, and it does so after the header dword is already in
 *     the ring. A BUG() in a Windows miniport is a bugcheck on a caller's mistake, and a partly
 *     written packet is worse than no packet, so the condition is upstream's, checked before
 *     anything is written, and a refusal is BC250_EINVAL with the ring untouched.
 *   - upstream takes the ring test's scratch dword from the device write-back pool, which this
 *     driver does not have. bc250_sdma_fence_page_alloc() cuts one GTT page into slots instead,
 *     exactly as bc250_gfx.c does for the graphics fence, and it is not allocated by
 *     bc250_sdma_setup(): a page the bring-up does not allocate cannot move an address the
 *     bring-up programs.
 * ------------------------------------------------------------------------------------------- */

/* sdma_v5_0.c:1021 and :1044. Upstream's own two constants, kept as they are written there. */
#define BC250_SDMA_TEST_BEFORE	0xCAFEDEADu
#define BC250_SDMA_TEST_AFTER	0xDEADBEEFu

unsigned int bc250_sdma_fence_size(const struct amdgpu_ring *ring, unsigned int flags)
{
	unsigned int ndw;

	if (ring == NULL || ring->funcs == NULL || ring->funcs->type != AMDGPU_RING_TYPE_SDMA)
		return 0;

	ndw = 4u;                               /* header, address low and high, sequence */
	if (flags & AMDGPU_FENCE_FLAG_64BIT)
		ndw += 4u;                      /* a second fence for the sequence's high dword */
	if (flags & AMDGPU_FENCE_FLAG_INT)
		ndw += 2u;                      /* the trap that raises the interrupt */
	return ndw;
}

int bc250_sdma_emit_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags)
{
	bool write64bit = (flags & AMDGPU_FENCE_FLAG_64BIT) != 0;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL)
		return BC250_EINVAL;
	if (ring->funcs->type != AMDGPU_RING_TYPE_SDMA)
		return BC250_EINVAL;

	/* "zero in first two bits", twice: upstream checks the address again after adding 4 for the
	 * second fence. Adding 4 cannot break a 4-byte alignment, so the second check can only fail
	 * where the first already has; it is kept so that both BUG_ON()s are accounted for. */
	if ((addr & 0x3u) != 0)
		return BC250_EINVAL;
	if (write64bit && (((addr + 4u) & 0x3u) != 0))
		return BC250_EINVAL;

	/* write the fence */
	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE) |
				SDMA_PKT_FENCE_HEADER_MTYPE(0x3)); /* Ucached(UC) */
	amdgpu_ring_write(ring, lower_32_bits(addr));
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, lower_32_bits(seq));

	/* optionally write high bits as well */
	if (write64bit) {
		addr += 4;
		amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_FENCE) |
					SDMA_PKT_FENCE_HEADER_MTYPE(0x3));
		amdgpu_ring_write(ring, lower_32_bits(addr));
		amdgpu_ring_write(ring, upper_32_bits(addr));
		amdgpu_ring_write(ring, upper_32_bits(seq));
	}

	if (flags & AMDGPU_FENCE_FLAG_INT) {
		/* generate an interrupt */
		amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_TRAP));
		amdgpu_ring_write(ring, SDMA_PKT_TRAP_INT_CONTEXT_INT_CONTEXT(0));
	}

	return 0;
}

/* [shim] alloc, emit and commit together, the same shape as bc250_gfx_signal_fence(). */
int bc250_sdma_signal_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags)
{
	unsigned int ndw = bc250_sdma_fence_size(ring, flags);
	int r;

	if (ndw == 0)
		return BC250_EINVAL;

	r = amdgpu_ring_alloc(ring, ndw);
	if (r)
		return r;

	r = bc250_sdma_emit_fence(ring, addr, seq, flags);
	if (r) {
		amdgpu_ring_undo(ring);
		return r;
	}

	amdgpu_ring_commit(ring);
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The scratch and fence page
 *
 * One GTT page cut into eight-byte slots, the arrangement bc250_gfx.c already uses. Slots 0 and 1
 * belong to the two engines' ring tests and the rest are fence slots, so a ring test and a fence
 * can be outstanding at once without the caller having to think about it.
 * ------------------------------------------------------------------------------------------- */

int bc250_sdma_fence_page_alloc(struct amdgpu_device *adev)
{
	int r;

	if (adev == NULL)
		return BC250_EINVAL;
	if (adev->sdma.fence_mem.cpu != NULL)
		return 0;                       /* already there; idempotent */

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, AMDGPU_GPU_PAGE_SIZE, AMDGPU_GPU_PAGE_SIZE,
				 &adev->sdma.fence_mem);
	if (r)
		return r;
	if (adev->sdma.fence_mem.cpu == NULL)
		return BC250_EINVAL;
	return 0;
}

void bc250_sdma_fence_page_free(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return;
	bc250_shim_mem_free(adev, &adev->sdma.fence_mem);
}

u64 bc250_sdma_fence_addr(struct amdgpu_device *adev, unsigned int slot)
{
	if (adev == NULL || adev->sdma.fence_mem.cpu == NULL || slot >= BC250_SDMA_FENCE_SLOTS)
		return 0;
	return adev->sdma.fence_mem.mc + (u64)slot * 8u;
}

u64 bc250_sdma_fence_read(struct amdgpu_device *adev, unsigned int slot)
{
	if (adev == NULL || adev->sdma.fence_mem.cpu == NULL || slot >= BC250_SDMA_FENCE_SLOTS)
		return 0;
	return *(volatile u64 *)((char *)adev->sdma.fence_mem.cpu + (size_t)slot * 8u);
}

/* sdma_v5_0.c:1012 sdma_v5_0_ring_test_ring(). One WRITE_LINEAR of a single dword into a slot the
 * driver can read, then poll it. The value the engine has to overwrite is put there first, so a
 * slot that already held the answer cannot pass the test. */
int bc250_sdma_ring_test(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev;
	volatile u32 *slot_cpu;
	unsigned int slot;
	u64 gpu_addr;
	unsigned int i;
	int r;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL)
		return BC250_EINVAL;
	if (ring->funcs->type != AMDGPU_RING_TYPE_SDMA)
		return BC250_EINVAL;

	adev = ring->adev;
	if (adev->sdma.fence_mem.cpu == NULL)
		return BC250_EINVAL;            /* bc250_sdma_fence_page_alloc() was not called */

	slot = ring->me & 0x1u;                 /* one scratch slot per engine */
	gpu_addr = bc250_sdma_fence_addr(adev, slot);
	if (gpu_addr == 0)
		return BC250_EINVAL;
	slot_cpu = (volatile u32 *)((char *)adev->sdma.fence_mem.cpu + (size_t)slot * 8u);
	*slot_cpu = BC250_SDMA_TEST_BEFORE;

	r = amdgpu_ring_alloc(ring, 20);
	if (r)
		return r;

	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_WRITE) |
				SDMA_PKT_HEADER_SUB_OP(SDMA_SUBOP_WRITE_LINEAR));
	amdgpu_ring_write(ring, lower_32_bits(gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(gpu_addr));
	amdgpu_ring_write(ring, SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(0));
	amdgpu_ring_write(ring, BC250_SDMA_TEST_AFTER);
	amdgpu_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (*slot_cpu == BC250_SDMA_TEST_AFTER)
			break;
		bc250_shim_udelay(1);
	}

	return (i < adev->usec_timeout) ? 0 : BC250_ETIME;
}
