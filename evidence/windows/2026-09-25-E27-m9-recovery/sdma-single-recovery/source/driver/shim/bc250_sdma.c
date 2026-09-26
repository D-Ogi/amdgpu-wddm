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

/* [shim] the one copy of bc250_ring_funcs_sdma, handed out rather than duplicated: ADR 0008 stage D
 * (driver/shim/bc250_sdma_paging.c) needs the same align_mask/nop a real SDMA0 ring uses to size its
 * throwaway ring the way amdgpu_ring_alloc() expects, and a second copy of these three values is how
 * they drift apart. */
const struct amdgpu_ring_funcs *bc250_sdma_ring_funcs(void)
{
	return &bc250_ring_funcs_sdma;
}

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
 * sdma_v5_0.c:688 sdma_v5_0_gfx_resume_instance()
 *
 * Upstream carries a `restore` flag. The fresh case writes 0 into both pointers
 * (sdma_v5_0.c:722-725) and leaves ring->wptr at 0 (:754-755); the restart case, which exists for
 * engine reset, programs RPTR and WPTR from the ring's saved write pointer instead (:717-720) and
 * keeps ring->wptr. A bring-up is the fresh case, so only that arm used to be carried here.
 *
 * Experiment E15 on unit A (2026-09-21, bc250kmd 0.6.2.0) says that is not enough on this part. A
 * second GFX bring-up inside one device start - bring-up, `gfx fini` undo, bring-up again, no GPU
 * reset in between and no PSP firmware reload - leaves both SDMA engines executing nothing, and the
 * register sweeps taken through the read-only witness driver say why:
 *
 *   - the first bring-up ran four submissions on SDMA0 and three on SDMA1, and left
 *     SDMA0_GFX_RB_RPTR, SDMA0_GFX_RB_WPTR and SDMA0_RB_RPTR_FETCH all reading 0x100 bytes, and
 *     SDMA1's three 0xC0;
 *   - the undo does not move them. It writes SDMAx_CNTL (context switching off), RB_CNTL with
 *     RB_ENABLE clear, IB_CNTL with IB_ENABLE clear and F32_CNTL with HALT set, which is amdgpu's
 *     own unload (fact M54), and afterwards the pointer registers still read 0x100 and 0xC0;
 *   - the writes of 0 below, and the later write of 0 under MINOR_PTR_UPDATE = 1, do not take. A
 *     sweep taken right after the second bring-up and before any submission reads 0x100 and 0xC0
 *     again, on both engines, over two bring-ups. SDMA0_GFX_RB_BASE, written in the same sequence,
 *     does take the new ring address.
 *
 * The engine's 64-bit write pointer is monotonic: a doorbell carrying a value at or below the
 * pointer the engine holds says nothing new and is ignored. That is what upstream's own comment
 * "before programing wptr to a less value, need set minor_ptr_update first" is about, and here even
 * that path did not lower it. So the ring test's doorbell of 0x40 - sixteen dwords - was ignored,
 * nothing executed, and the scratch slot kept 0xCAFEDEAD until the poll ran out.
 *
 * Two things make this the engine's own state and not a register that failed to write:
 * bc250_sdma_enable(adev, true) un-halts the engines before the ring is programmed, which is
 * upstream's order, and the SDMA firmware is not reloaded, so each engine resumes holding what it
 * had. Which is also the way out: an engine that remembers its write pointer is an engine we can
 * ask where it stands.
 *
 * The deviation, and it is the only one. The fresh-case writes of 0 stay exactly as they are,
 * because the first bring-up's write sequence is what the host replay compares with the Linux
 * trace. After them the write pointer is read back, and if it is non-zero the engine kept its
 * pointers: this then takes upstream's `restore` arm with the hardware's own value standing in for
 * the saved ring->wptr, because this driver frees its ring state together with the ring buffer and
 * has no saved write pointer to restore from. The engine is the only thing that still knows.
 *
 * The write pointer and not the read pointer. They are equal in everything E15 measured, but if the
 * two ever differ the engine walks the ring from its read pointer up to its write pointer, and
 * those dwords are NOPs: bc250_sdma_setup() runs amdgpu_ring_clear_ring() over every freshly
 * allocated ring buffer and BC250_SDMA_NOP is what it fills with. Adopting the read pointer instead
 * would leave the engine's own write pointer above ours and the next doorbell would be ignored
 * exactly as it is now.
 *
 * One loose end, named here because it is a gap in the evidence and not in the code. The undo
 * leaves SDMAx_GFX_RB_WPTR_POLL_CNTL and the two WPTR_POLL_ADDR halves exactly as the bring-up set
 * them, which is what upstream's hw_fini does too - it touches CNTL, RB_CNTL, IB_CNTL and F32_CNTL
 * and nothing else. So between bc250_sdma_enable(adev, true) and the rewrite of POLL_ADDR a few
 * writes below, F32_POLL_ENABLE is still set and the poll address still names the PREVIOUS
 * instance's write-back page, which the miniport has by then given back. Whether an engine with
 * RB_ENABLE clear reads that address at all is not known; E15 saw no fault vector across three
 * bring-ups, which is evidence of absence only as far as three bring-ups go. Closing the window
 * would mean writing POLL_ADDR before the un-halt, which is a second deviation from upstream's
 * order and has nothing measured behind it, so it is written down rather than done.
 *
 * The SR-IOV guards are all on the bare-metal side, which is this one (amdgpu_sriov_vf() is false on
 * this device, see amdgpu.h).
 *
 * The write pointer of an SDMA ring counts bytes, not dwords, which is where the `<< 2` comes from;
 * bc250_ring.c keeps the dword convention of the CP rings and says so.
 * ------------------------------------------------------------------------------------------- */
static int bc250_sdma_gfx_resume_instance(struct amdgpu_device *adev, int i, bool reset_transport)
{
	struct amdgpu_ring *ring;
	u32 rb_cntl, ib_cntl;
	u32 rb_bufsz;
	u32 doorbell;
	u32 doorbell_offset;
	u32 temp;
	u32 wptr_poll_cntl;
	u64 wptr_gpu_addr;
	u64 hw_wptr;
	u64 adopted = 0;                        /* the engine's own write pointer, in bytes, or 0 */

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

	/* [shim] Did they take? On a second bring-up in one device start they do not, and the engine
	 * goes on holding the write pointer it reached; see the block comment for what E15 measured.
	 * Two reads here, and the read pointer only in the branch that reports it, so a bring-up that
	 * adopts nothing gains two reads and not a single write.
	 *
	 * These two reads must stay BELOW the writes of 0, and that is load-bearing rather than
	 * tidy. The miniport can run this sequence as a PLAN, which executes no write and answers a
	 * read from its own recorded write list (driver/kmd/gfx.c GfxPlanAnswers, :504-518). Here
	 * that list already holds the 0 this function just planned, so a PLAN reads 0, takes the
	 * fresh arm, and its write list comes out exactly as it did before this change. Moved above
	 * the writes, the same read would fall through to real hardware and a PLAN would start
	 * predicting an adoption it cannot carry out. The corollary is that a PLAN can never exercise
	 * the adoption arm at all - the only thing that does is the host replay's
	 * check_sdma_pointers() (driver/shim/test/replay_gfx.c). */
	/* Recovery discards transport; never use the E15 startup adoption policy. */
	hw_wptr = reset_transport ? 0 : (u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR)) |
		  ((u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
								 mmSDMA0_GFX_RB_WPTR_HI)) << 32);
	if (hw_wptr != 0) {
		u64 hw_rptr;

		/* All-ones in either half is not a pointer: it is what a faulted register sequence
		 * or a bus with nothing on the other end answers. Tested outright rather than left
		 * to the alignment test below, which would reject it only because 0xFFFFFFFF happens
		 * to be odd - the right answer for a reason that says nothing, and one that would
		 * stop holding if either the value or the alignment rule moved. There is no upper
		 * bound worth testing next to it: the pointer is 64-bit and monotonic, it does not
		 * have to fit the ring (buf_mask is what wraps it into the buffer), and ring->wptr
		 * counts in the same 64 bits. */
		if (lower_32_bits(hw_wptr) == 0xffffffffu || upper_32_bits(hw_wptr) == 0xffffffffu) {
			dev_err(adev->dev,
				"sdma%d read back write pointer 0x%llX; all-ones in either half is a"
				" dead read, not a pointer\n", i, hw_wptr);
			return BC250_EINVAL;
		}

		/* The invariant is not four bytes but sixteen dwords. amdgpu_ring_commit() pads every
		 * submission to ring->funcs->align_mask + 1 (sdma_v5_0.c:1941-1944 sets the mask to
		 * 0xf), so a pointer this engine reached can only be a multiple of that. Anything
		 * else is a pointer we do not understand, and one we do not understand is not one to
		 * build on: refuse, do not round. The dword test is separate and first because
		 * `hw_wptr >> 2` throws away the two bits it would catch. */
		if ((hw_wptr & 0x3u) != 0 ||
		    ((hw_wptr >> 2) & (u64)ring->funcs->align_mask) != 0) {
			dev_err(adev->dev,
				"sdma%d kept write pointer 0x%llX, which is not a whole %u-dword"
				" submission; refusing to adopt it\n",
				i, hw_wptr, (unsigned int)ring->funcs->align_mask + 1u);
			return BC250_EINVAL;
		}

		hw_rptr = (u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
									 mmSDMA0_GFX_RB_RPTR)) |
			  ((u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
									 mmSDMA0_GFX_RB_RPTR_HI)) << 32);
		adopted = hw_wptr;
		dev_info(adev->dev,
			 "sdma%d kept its pointers across the undo (rptr 0x%llX, wptr 0x%llX);"
			 " adopting the write pointer\n", i, hw_rptr, hw_wptr);
	}

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

	/* sdma_v5_0.c:754-755 `if (!restore) ring->wptr = 0;`, with the engine's own value where
	 * upstream has the caller's saved one. Everything downstream counts on from here: the WPTR
	 * write below, the write-back slot, and every doorbell amdgpu_ring_commit() will ring. */
	if (adopted != 0)
		ring->wptr = adopted >> 2;
	else
		ring->wptr = 0;

	/* before programing wptr to a less value, need set minor_ptr_update first */
	WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_MINOR_PTR_UPDATE), 1);

	/* only bare-metal use register write for wptr */
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR),
	       lower_32_bits(ring->wptr << 2));
	WREG32(bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_HI),
	       upper_32_bits(ring->wptr << 2));

	/* [shim] And into the write-back slot the engine polls, which is where amdgpu_ring_commit()
	 * puts it and which nothing has written yet: F32_POLL_ENABLE went on a few writes above, the
	 * page bc250_sdma_setup() cut this slot out of is freshly allocated and reads 0, and the first
	 * commit after this bring-up would be the first to fill it. Before RB_ENABLE, so the engine
	 * never sees the ring enabled with a shadow that contradicts where it stands. */
	if (adopted != 0 && ring->wptr_cpu_addr != NULL)
		*(volatile u64 *)ring->wptr_cpu_addr = ring->wptr << 2;

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

	/* The rings bc250_sdma_setup() filled in, or nothing at all: num_instances is set before its
	 * allocations and bc250_sdma_teardown() does not put it back, so a setup that failed leaves
	 * two engines' worth of zeroed rings behind. Enabling those would point the engines at MC 0,
	 * and the adoption arm in bc250_sdma_gfx_resume_instance() would dereference ring->funcs.
	 * D-05. */
	for (i = 0; i < adev->sdma.num_instances; i++)
		if (adev->sdma.instance[i].ring.funcs == NULL ||
		    adev->sdma.instance[i].ring.ring_size == 0)
			return BC250_EINVAL;

	/* unhalt the MEs */
	bc250_sdma_enable(adev, true);
	/* enable sdma ring preemption */
	bc250_sdma_ctx_switch_enable(adev, true);

	for (i = 0; i < adev->sdma.num_instances; i++) {
		r = bc250_sdma_gfx_resume_instance(adev, i, false);
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

/* PROVENANCE: Linux amdgpu v6.18 sdma_v5_0_stop_queue, AMD MIT.
 * Register body adapted mechanically. RLC safe-mode scope belongs to the
 * caller, as do serialization, retained backing, unfreeze and ring restore.
 * Bounds are checked before touching registers; timeout uses the shim's
 * BC250_ETIME code. The composite reload helper owns the paired RLC scope. */
static void bc250_sdma_stop_selected_rings(struct amdgpu_device *adev, uint32_t inst_mask)
{
	u32 rb_cntl, ib_cntl;
	int i;

	for (i = 0; i < adev->sdma.num_instances; i++) if (inst_mask & (1u << i)) {
		rb_cntl = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, i, mmSDMA0_GFX_RB_CNTL));
		rb_cntl = REG_SET_FIELD(rb_cntl, SDMA0_GFX_RB_CNTL, RB_ENABLE, 0);
		WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, i, mmSDMA0_GFX_RB_CNTL), rb_cntl);
		ib_cntl = RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, i, mmSDMA0_GFX_IB_CNTL));
		ib_cntl = REG_SET_FIELD(ib_cntl, SDMA0_GFX_IB_CNTL, IB_ENABLE, 0);
		WREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, i, mmSDMA0_GFX_IB_CNTL), ib_cntl);
	}
}

/* AMD sdma_v5_0_soft_reset_engine, mechanically extracted (M372).
 * Return 0 means the register sequence ran; it does not prove reset completion.
 * Caller retains all backing and owns queue quiescence/restoration. */
int bc250_sdma_soft_reset_instance(struct amdgpu_device *adev, u32 instance_id)
{
	u32 grbm_soft_reset;
	u32 tmp;

	/* [shim] Fixed two-instance device; reject before shifting or MMIO. */
	if (!adev || instance_id >= (u32)adev->sdma.num_instances || instance_id >= 2)
		return BC250_EINVAL;

	grbm_soft_reset = REG_SET_FIELD(0,
					GRBM_SOFT_RESET, SOFT_RESET_SDMA0,
					1);
	grbm_soft_reset <<= instance_id;

	tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
	tmp |= grbm_soft_reset;
	WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
	tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
	/* M373: observe existing readbacks without adding MMIO. */
	dev_info(adev->dev, "SDMA%u reset asserted readback 0x%08x\n", instance_id, tmp);

	bc250_shim_udelay(50);

	tmp &= ~grbm_soft_reset;
	WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
	tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
	dev_info(adev->dev, "SDMA%u reset released readback 0x%08x\n", instance_id, tmp);
	return 0;
}

int bc250_sdma_quiesce_instance(struct amdgpu_device *adev, u32 instance)
{
	u32 f32_cntl, freeze, cntl, stat1_reg;
	int i, r = 0;
	u32 j;

	if (!adev || instance >= (u32)adev->sdma.num_instances || instance >= 2 || !adev->usec_timeout)
		return BC250_EINVAL;

	i = (int)instance;
	/* Caller owns the enclosing RLC safe-mode scope. */

	/* stop queue */
	bc250_sdma_stop_selected_rings(adev, 1 << i);

	/* engine stop SDMA1_F32_CNTL.HALT to 1 and SDMAx_FREEZE freeze bit to 1 */
	freeze = RREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_FREEZE));
	freeze = REG_SET_FIELD(freeze, SDMA0_FREEZE, FREEZE, 1);
	WREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_FREEZE), freeze);

	for (j = 0; j < adev->usec_timeout; j++) {
		freeze = RREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_FREEZE));
		if (REG_GET_FIELD(freeze, SDMA0_FREEZE, FROZEN) & 1)
			break;
		bc250_shim_udelay(1);
	}

	/* check sdma copy engine all idle if frozen not received*/
	if (j == adev->usec_timeout) {
		stat1_reg = RREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_STATUS1_REG));
		if ((stat1_reg & 0x3FF) != 0x3FF) {
			dev_err(adev->dev, "cannot soft reset as sdma not idle\n");
			r = BC250_ETIME;
			goto err0;
		}
	}

	f32_cntl = RREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_F32_CNTL));
	f32_cntl = REG_SET_FIELD(f32_cntl, SDMA0_F32_CNTL, HALT, 1);
	WREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_F32_CNTL), f32_cntl);

	cntl = RREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_CNTL));
	cntl = REG_SET_FIELD(cntl, SDMA0_CNTL, UTC_L1_ENABLE, 0);
	WREG32(bc250_sdma_reg_offset(adev, i, mmSDMA0_CNTL), cntl);
err0:
	/* Safe-mode exit remains the caller's responsibility on both paths. */
	return r;
}

/* sdma_v5_0_restore_queue: unfreeze fragment only. Caller must still
 * reprogram/restore the ring and translation visibility before admission. */
int bc250_sdma_unfreeze_instance(struct amdgpu_device *adev, u32 instance)
{
 u32 freeze;
 if (!adev || instance >= (u32)adev->sdma.num_instances || instance >= 2)
  return BC250_EINVAL;
 freeze=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_FREEZE));
 freeze=REG_SET_FIELD(freeze,SDMA0_FREEZE,FREEZE,0);
 WREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_FREEZE),freeze);
 return 0;
}

/* M370: compose AMD queue-quiescence with a paired RLC scope. Caller has
 * already halted CP/SDMA and owns all backing. Unfreeze while engines remain
 * halted and UTC/rings disabled, so FREEZE need not survive PSP firmware load.
 * This composition is a Windows lifecycle policy, not an upstream reset call. */
int bc250_sdma_quiesce_for_reload(struct amdgpu_device *adev)
{
 bool requested=false;
 u32 i, control, rb, ib, freeze;
 int result;
 if (!adev || adev->sdma.num_instances!=2) return BC250_EINVAL;
 result=bc250_gfx_rlc_safe_enter(adev,&requested);
 if (result) goto Done;
 for(i=0;i<2;i++) {
  result=bc250_sdma_quiesce_instance(adev,i);
  if(result) goto Done;
 }
 for(i=0;i<2;i++) {
  result=bc250_sdma_unfreeze_instance(adev,i);
  if(result) goto Done;
  control=RREG32(bc250_sdma_reg_offset(adev,i,mmSDMA0_F32_CNTL));
  rb=RREG32(bc250_sdma_reg_offset(adev,i,mmSDMA0_GFX_RB_CNTL));
  ib=RREG32(bc250_sdma_reg_offset(adev,i,mmSDMA0_GFX_IB_CNTL));
  freeze=RREG32(bc250_sdma_reg_offset(adev,i,mmSDMA0_FREEZE));
  if (!REG_GET_FIELD(control,SDMA0_F32_CNTL,HALT) ||
      REG_GET_FIELD(rb,SDMA0_GFX_RB_CNTL,RB_ENABLE) ||
      REG_GET_FIELD(ib,SDMA0_GFX_IB_CNTL,IB_ENABLE) ||
      REG_GET_FIELD(freeze,SDMA0_FREEZE,FREEZE)) { result=BC250_EIO;goto Done; }
  control=RREG32(bc250_sdma_reg_offset(adev,i,mmSDMA0_CNTL));
  if(REG_GET_FIELD(control,SDMA0_CNTL,UTC_L1_ENABLE)) { result=BC250_EIO;goto Done; }
 }
Done:
 bc250_gfx_rlc_safe_exit(adev,requested);
 return result;
}

/* M373: Windows stop/reload composition. The reset remains between the
 * source's two safe-mode scopes. All backing is retained by the caller.
 * Unlike live Linux queue restoration, re-quiesce after reset and verify the
 * halted/queue-off/cache-off state before unfreezing and retiring mappings. */
int bc250_sdma_reset_for_reload(struct amdgpu_device *adev)
{
 bool requested=false;
 u32 i;
 int result;
 if (!adev || adev->sdma.num_instances!=2) return BC250_EINVAL;
 result=bc250_gfx_rlc_safe_enter(adev,&requested);
 if (!result) {
  for(i=0;i<2;i++) {
   result=bc250_sdma_quiesce_instance(adev,i);
   if(result) break;
  }
 }
 bc250_gfx_rlc_safe_exit(adev,requested);
 dev_info(adev->dev,"SDMA pre-reset quiescence result %d\n",result);
 if(result) return result;
 for(i=0;i<2;i++) {
  result=bc250_sdma_soft_reset_instance(adev,i);
  if(result) return result;
 }
 result=bc250_sdma_quiesce_for_reload(adev);
 dev_info(adev->dev,"SDMA post-reset quiescence result %d\n",result);
 return result;
}

/* Single-instance transport recovery, composed from AMD sdma_v5_0 stop/reset/
 * restore. The September 2026 reset series describes clearing obsolete ring/WB
 * contents before restart. No scheduler replay or fence completion occurs here.
 * [shim] Re-quiesce after reset as reset_for_reload does, before changing memory,
 * instead of assuming reset defaults. Keep firmware and the other engine live. */
int bc250_sdma_reset_retained_instance(struct amdgpu_device *adev, u32 instance,
                                     struct bc250_sdma_reset_receipt *receipt)
{
 struct amdgpu_ring *ring;
 volatile u64 *rptr_cpu;
 u64 rptr_offset,wptr_offset;
 u32 rb,ib,control,halt,freeze;
 bool requested=false;
 int result;
 if (!receipt) return BC250_EINVAL;
 memset(receipt,0,sizeof(*receipt));
 receipt->instance=instance;
 if (!adev || instance>=2 || instance>=(u32)adev->sdma.num_instances ||
     !adev->usec_timeout) return BC250_EINVAL;
 ring=&adev->sdma.instance[instance].ring;
 if (ring->adev!=adev || ring->me!=instance || ring->funcs!=&bc250_ring_funcs_sdma ||
     !ring->ring || ring->ring_size!=BC250_SDMA_RING_SIZE ||
     ring->buf_mask!=BC250_SDMA_RING_SIZE/sizeof(u32)-1u ||
     !adev->sdma.wb_mem.cpu || !ring->wptr_cpu_addr ||
     ring->rptr_gpu_addr<adev->sdma.wb_mem.mc || ring->wptr_gpu_addr<adev->sdma.wb_mem.mc)
  return BC250_EINVAL;
 rptr_offset=ring->rptr_gpu_addr-adev->sdma.wb_mem.mc;
 wptr_offset=ring->wptr_gpu_addr-adev->sdma.wb_mem.mc;
 /* Only the two WB slots assigned by setup belong to this engine. */
 if (rptr_offset!=(u64)instance*2u*BC250_SDMA_WB_SLOT_BYTES ||
     wptr_offset!=rptr_offset+BC250_SDMA_WB_SLOT_BYTES ||
     wptr_offset+sizeof(u64)>adev->sdma.wb_mem.size ||
     ring->wptr_cpu_addr!=(char*)adev->sdma.wb_mem.cpu+(size_t)wptr_offset)
  return BC250_EINVAL;
 rptr_cpu=(volatile u64*)((char*)adev->sdma.wb_mem.cpu+(size_t)rptr_offset);
 receipt->previous_wptr=ring->wptr;
 receipt->stage=BC250_SDMA_RESET_STOP;
 result=bc250_gfx_rlc_safe_enter(adev,&requested);
 if (!result) result=bc250_sdma_quiesce_instance(adev,instance);
 bc250_gfx_rlc_safe_exit(adev,requested);
 if (result) return result;
 receipt->stage=BC250_SDMA_RESET_PULSE;
 result=bc250_sdma_soft_reset_instance(adev,instance);
 if (result) return result;
 receipt->stage=BC250_SDMA_RESET_CLEAR;
 requested=false;
 result=bc250_gfx_rlc_safe_enter(adev,&requested);
 if (result) goto Done;
 result=bc250_sdma_quiesce_instance(adev,instance);
 if (result) goto Done;
 rb=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_RB_CNTL));
 ib=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_IB_CNTL));
 control=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_CNTL));
 halt=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_F32_CNTL));
 if (rb==~0u || ib==~0u || control==~0u || halt==~0u ||
     REG_GET_FIELD(rb,SDMA0_GFX_RB_CNTL,RB_ENABLE) ||
     REG_GET_FIELD(ib,SDMA0_GFX_IB_CNTL,IB_ENABLE) ||
     REG_GET_FIELD(control,SDMA0_CNTL,UTC_L1_ENABLE) ||
     !REG_GET_FIELD(halt,SDMA0_F32_CNTL,HALT)) {result=BC250_EIO;goto Done;}
 amdgpu_ring_clear_ring(ring);
 ring->wptr=0;
 ring->wptr_old=0;
 ring->count_dw=0;
 *rptr_cpu=0;
 *(volatile u64*)ring->wptr_cpu_addr=0;
 /* Backend MMIO stores must order CPU ring/WB writes before queue enable.
  * The KMD uses WRITE_REGISTER_ULONG, not a no-fence accessor. */
 receipt->stage=BC250_SDMA_RESET_RESTORE;
 result=bc250_sdma_unfreeze_instance(adev,instance);
 if (!result) result=bc250_sdma_gfx_resume_instance(adev,(int)instance,true);
Done:
 bc250_gfx_rlc_safe_exit(adev,requested);
 if (result) return result;
 receipt->stage=BC250_SDMA_RESET_VERIFY;
 receipt->rptr=(u64)RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_RB_RPTR)) |
  ((u64)RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_RB_RPTR_HI))<<32);
 receipt->wptr=(u64)RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_RB_WPTR)) |
  ((u64)RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_RB_WPTR_HI))<<32);
 rb=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_RB_CNTL));
 ib=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_GFX_IB_CNTL));
 control=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_CNTL));
 halt=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_F32_CNTL));
 freeze=RREG32(bc250_sdma_reg_offset(adev,instance,mmSDMA0_FREEZE));
 if (receipt->rptr || receipt->wptr || *rptr_cpu || *(volatile u64*)ring->wptr_cpu_addr ||
     rb==~0u || ib==~0u || control==~0u || halt==~0u || freeze==~0u ||
     !REG_GET_FIELD(rb,SDMA0_GFX_RB_CNTL,RB_ENABLE) ||
     !REG_GET_FIELD(ib,SDMA0_GFX_IB_CNTL,IB_ENABLE) ||
     !REG_GET_FIELD(control,SDMA0_CNTL,UTC_L1_ENABLE) ||
     REG_GET_FIELD(halt,SDMA0_F32_CNTL,HALT) ||
     REG_GET_FIELD(freeze,SDMA0_FREEZE,FREEZE)) return BC250_EIO;
 receipt->stage=BC250_SDMA_RESET_PROGRAMMED;
 return 0;
}

/* sdma_v5_0.c:1480 sdma_v5_0_hw_fini(), with sdma_v5_0_gfx_stop() (sdma_v5_0.c:563) written out
 * where sdma_v5_0_enable(adev, false) would call it. sdma_v5_0_rlc_stop() is an upstream stub.
 * [shim] Iterate actual instances here. The v6.18 reference computes an instance
 * bitmask and then passes 1 << inst_mask to gfx_stop; with two instances that
 * selects bit 3 instead of engines 0 and 1 (M368 source review). Do not copy
 * that shift into our bounded loop. This is ordinary fini, not AMD's separate
 * stop_queue freeze/UTC_L1-disable preparation for an engine reset.
 *
 * [shim] and a verdict, which upstream has no use for and this driver does. The halt sequence is
 * unchanged and still runs to the end whatever it finds; what is added is four reads per engine
 * AFTER it. Setting F32_CNTL.HALT proves the engine stopped, not that it finished: an engine whose
 * read pointer is still behind its write pointer stopped holding packets it had not executed, and
 * those packets name addresses - a ring, a fence slot, a WRITE_LINEAR destination - that the caller
 * is about to give back to the page allocator. So an engine that halts with rptr != wptr is
 * reported as BC250_EBUSY, and driver/kmd/gfx.c's Fini() turns that into pages that stay.
 *
 * Both engines are always read, so the log names both rather than the first; the return is the
 * first failure. Nothing here frees memory, and the return value says nothing about whether the
 * halt itself was written - that is what the caller's own read of F32_CNTL is for. */
int bc250_sdma_hw_fini(struct amdgpu_device *adev)
{
	u32 rb_cntl, ib_cntl;
	u64 rptr, wptr;
	int i, r = 0;

	if (adev == NULL)
		return BC250_EINVAL;

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

	for (i = 0; i < adev->sdma.num_instances; i++) {
		rptr = (u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
								      mmSDMA0_GFX_RB_RPTR)) |
		       ((u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
								      mmSDMA0_GFX_RB_RPTR_HI)) << 32);
		wptr = (u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
								      mmSDMA0_GFX_RB_WPTR)) |
		       ((u64)RREG32_SOC15_IP(GC, bc250_sdma_reg_offset(adev, (u32)i,
								      mmSDMA0_GFX_RB_WPTR_HI)) << 32);
		if (rptr == wptr)
			continue;

		dev_err(adev->dev,
			"sdma%d halted with rptr 0x%llX behind wptr 0x%llX: it stopped owing work,"
			" and the packets it has not read name addresses of ours\n", i, rptr, wptr);
		if (r == 0)
			r = BC250_EBUSY;
	}

	return r;
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

	/* Not idempotent, unlike bc250_sdma_fence_page_alloc(): allocating over the descriptors below
	 * would lose three pages, and the engines may still be fetching from the rings they name. A
	 * caller that wants a second bring-up runs bc250_sdma_teardown() first, which is what
	 * driver/kmd/gfx.c's Fini() does. D-04. */
	if (adev->sdma.wb_mem.size != 0)
		return BC250_EINVAL;

	adev->sdma.num_instances = AMDGPU_MAX_SDMA_INSTANCES;

	/* nv_set_ip_blocks() -> nv_init_doorbell_index(), the NAVI10 assignment. */
	adev->doorbell_index.sdma_engine[0] = AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE0;
	adev->doorbell_index.sdma_engine[1] = AMDGPU_NAVI10_DOORBELL_sDMA_ENGINE1;

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_SDMA_WB_BYTES, AMDGPU_GPU_PAGE_SIZE,
				 &adev->sdma.wb_mem);
	if (r)
		goto fail;
	/* The write pointer every engine polls lives in this page (the shadow set up in
	 * bc250_sdma_gfx_resume_instance() below). Without a CPU mapping there is nothing to publish
	 * it into, and a ring buffer's own cpu == NULL is refused eight lines below for exactly that
	 * reason (bc250_shim.h: the shim refuses rather than guesses). D-01. */
	if (adev->sdma.wb_mem.cpu == NULL) {
		r = BC250_EINVAL;
		goto fail;
	}

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
	struct amdgpu_ring *ring;
	int i;

	if (adev == NULL)
		return;

	for (i = 0; i < AMDGPU_MAX_SDMA_INSTANCES; i++) {
		ring = &adev->sdma.instance[i].ring;
		bc250_shim_mem_free(adev, &ring->ring_mem);
		/* The rule bc250_ring_alloc_mem() states for the CP rings (bc250_gfx.c:1702-1704): a
		 * ring without a buffer is unusable by construction, not by the caller remembering.
		 * Upstream does the same - amdgpu_bo_free_kernel() nulls ring->ring and zeroes
		 * ring->gpu_addr (amdgpu_object.c:528-532). D-06. */
		ring->ring = NULL;
		ring->gpu_addr = 0;
		ring->wptr_cpu_addr = NULL;
	}

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
 * One difference against that dump, in the padding and nowhere else. Upstream's pad at dw 0x0005 is
 * a single NOP packet declaring ten dwords of payload (`000a0000` is SDMA_OP_NOP with COUNT 10,
 * then ten zeros), because sdma_v5_0_ring_insert_nop() writes a burst NOP when the slack is more
 * than one dword. Ours is eleven separate one-dword NOPs: the shim uses the generic
 * amdgpu_ring_insert_nop() for every ring, and upstream's SDMA version of it is reached through a
 * `.insert_nop` callback, which ADR 0002 does not import - the shim's amdgpu_ring_funcs carries no
 * callbacks at all. Same length, same effect, different bytes, and the packets that matter are byte
 * for byte upstream's; recorded here so that the next person to diff a ring dump does not read it
 * as a defect.
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

/* AMD sdma_v5_0_ring_emit_ib, with admission checks instead of silently
 * rounding an IB address or truncating a VMID. CSA comes from the caller's
 * preemption policy; it must be valid in the submitted context when required. */
unsigned int bc250_sdma_ib_size(const struct amdgpu_ring *ring)
{
    if (!ring || !ring->funcs || ring->funcs->type != AMDGPU_RING_TYPE_SDMA)
        return 0;
    return ((2u - lower_32_bits(ring->wptr)) & 7u) + 6u;
}

static int bc250_sdma_ib_valid(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw, u32 vmid)
{
    return bc250_sdma_ib_size(ring) != 0 && ring->ring && !(gpu_addr & 31u) &&
           length_dw && length_dw <= SDMA_PKT_INDIRECT_IB_SIZE_ib_size_mask &&
           vmid <= SDMA_PKT_INDIRECT_HEADER_vmid_mask;
}

int bc250_sdma_emit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw,
                       u32 vmid, u64 csa_addr)
{
    unsigned int padding;
    if (!bc250_sdma_ib_valid(ring, gpu_addr, length_dw, vmid))
        return BC250_EINVAL;
    padding = bc250_sdma_ib_size(ring) - 6u;
    amdgpu_ring_insert_nop(ring, padding);
    amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_INDIRECT) |
                           SDMA_PKT_INDIRECT_HEADER_VMID(vmid));
    amdgpu_ring_write(ring, lower_32_bits(gpu_addr));
    amdgpu_ring_write(ring, upper_32_bits(gpu_addr));
    amdgpu_ring_write(ring, length_dw);
    amdgpu_ring_write(ring, lower_32_bits(csa_addr));
    amdgpu_ring_write(ring, upper_32_bits(csa_addr));
    return 0;
}

int bc250_sdma_submit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw,
                         u32 vmid, u64 csa_addr, u64 fence_addr, u64 seq,
                         unsigned int flags)
{
    unsigned int ndw;
    int result;
    if (!bc250_sdma_ib_valid(ring, gpu_addr, length_dw, vmid) || (fence_addr & 3u))
        return BC250_EINVAL;
    ndw = bc250_sdma_ib_size(ring) + bc250_sdma_fence_size(ring, flags);
    ndw += (0u - lower_32_bits(ring->wptr + ndw)) & ring->funcs->align_mask;
    result = amdgpu_ring_alloc(ring, ndw);
    if (result)
        return result;
    result = bc250_sdma_emit_ib(ring, gpu_addr, length_dw, vmid, csa_addr);
    if (!result)
        result = bc250_sdma_emit_fence(ring, fence_addr, seq, flags);
    if (result) {
        amdgpu_ring_undo(ring);
        return result;
    }
    amdgpu_ring_commit(ring);
    return 0;
}

/* PROVENANCE: Linux v6.18 amdgpu, MIT, reference/sdma_v5_0.c register
 * emitters and reference/gmc_v10_0.c:gmc_v10_0_emit_flush_gpu_tlb.
 * Adaptation: GFXHUB, SDMA0 invalidate engine0 only (CPU uses17); no semaphore
 * on this hub. Caller owns the nonzero VMID and root until actual completion. */
static void bc250_sdma_vm_wreg(struct amdgpu_ring *ring,
				     uint32_t reg, uint32_t val)
{
	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_SRBM_WRITE) |
			  SDMA_PKT_SRBM_WRITE_HEADER_BYTE_EN(0xf));
	amdgpu_ring_write(ring, reg);
	amdgpu_ring_write(ring, val);
}

static void bc250_sdma_vm_reg_wait(struct amdgpu_ring *ring, uint32_t reg,
					 uint32_t val, uint32_t mask)
{
	amdgpu_ring_write(ring, SDMA_PKT_HEADER_OP(SDMA_OP_POLL_REGMEM) |
			  SDMA_PKT_POLL_REGMEM_HEADER_HDP_FLUSH(0) |
			  SDMA_PKT_POLL_REGMEM_HEADER_FUNC(3)); /* equal */
	amdgpu_ring_write(ring, reg << 2);
	amdgpu_ring_write(ring, 0);
	amdgpu_ring_write(ring, val); /* reference */
	amdgpu_ring_write(ring, mask); /* mask */
	amdgpu_ring_write(ring, SDMA_PKT_POLL_REGMEM_DW5_RETRY_COUNT(0xfff) |
			  SDMA_PKT_POLL_REGMEM_DW5_INTERVAL(10));
}

static void bc250_sdma_vm_reg_write_reg_wait(struct amdgpu_ring *ring,
						   uint32_t reg0, uint32_t reg1,
						   uint32_t ref, uint32_t mask)
{
	bc250_sdma_vm_wreg(ring, reg0, ref);
	/* wait for a cycle to reset vm_inv_eng*_ack */
	bc250_sdma_vm_reg_wait(ring, reg0, 0, 0);
	bc250_sdma_vm_reg_wait(ring, reg1, mask, mask);
}

int bc250_sdma_emit_vm_flush(struct amdgpu_ring *ring, u32 vmid, u64 root_phys)
{
    const struct amdgpu_vmhub *hub;
    u64 lo, hi;
    u32 request;
    if (!ring || !ring->adev || !ring->ring || !ring->funcs ||
        ring->funcs->type!=AMDGPU_RING_TYPE_SDMA || ring->me!=0 ||
        vmid==0 || vmid>=AMDGPU_NUM_VMID || (root_phys & (AMDGPU_GPU_PAGE_SIZE-1)))
        return BC250_EINVAL;
    hub=&ring->adev->vmhub[AMDGPU_GFXHUB(0)];
    if (!hub->vmhub_funcs || !hub->vmhub_funcs->get_invalidate_req || !hub->ctx_addr_distance)
        return BC250_EINVAL;
    lo=(u64)hub->ctx0_ptb_addr_lo32+(u64)hub->ctx_addr_distance*vmid;
    hi=(u64)hub->ctx0_ptb_addr_hi32+(u64)hub->ctx_addr_distance*vmid;
    if (lo>SDMA_PKT_SRBM_WRITE_ADDR_addr_mask || hi>SDMA_PKT_SRBM_WRITE_ADDR_addr_mask ||
        hub->vm_inv_eng0_req>SDMA_PKT_SRBM_WRITE_ADDR_addr_mask || hub->vm_inv_eng0_ack>(~0u>>2))
        return BC250_EINVAL;
    // amdgpu_gmc_pd_addr / existing bc250_gmc_set_vmid_pd: local root plus VALID.
    root_phys|=AMDGPU_PTE_VALID;
    request=hub->vmhub_funcs->get_invalidate_req(vmid,0);
    bc250_sdma_vm_wreg(ring,(u32)lo,lower_32_bits(root_phys));
    bc250_sdma_vm_wreg(ring,(u32)hi,upper_32_bits(root_phys));
    bc250_sdma_vm_reg_write_reg_wait(ring,hub->vm_inv_eng0_req,hub->vm_inv_eng0_ack,request,1u<<vmid);
    return 0;
}

int bc250_sdma_submit_vm_ib(struct amdgpu_ring *ring, u64 root_phys, u64 gpu_addr, u32 length_dw,
                            u32 vmid, u64 csa_addr, u64 fence_addr, u64 seq, unsigned int flags)
{
    // Reserve root LO/HI writes (6), invalidate write+two polls (15), IB and fence.
    unsigned int ndw=BC250_SDMA_VM_FLUSH_DWORDS;
    int result;
    if (!bc250_sdma_ib_valid(ring,gpu_addr,length_dw,vmid) || (fence_addr & 3u))
        return BC250_EINVAL;
    ndw+=((2u-lower_32_bits(ring->wptr+ndw))&7u)+6u;
    ndw+=bc250_sdma_fence_size(ring,flags);
    ndw+=(0u-lower_32_bits(ring->wptr+ndw))&ring->funcs->align_mask;
    result=amdgpu_ring_alloc(ring,ndw);
    if (result) return result;
    result=bc250_sdma_emit_vm_flush(ring,vmid,root_phys);
    if (!result) result=bc250_sdma_emit_ib(ring,gpu_addr,length_dw,vmid,csa_addr);
    if (!result) result=bc250_sdma_emit_fence(ring,fence_addr,seq,flags);
    if (result) {amdgpu_ring_undo(ring);return result;}
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
	if (adev->sdma.fence_mem.cpu == NULL) {
		/* Give it back and leave the descriptor empty: the idempotence test above keys on
		 * fence_mem.cpu, so a descriptor left half full would have the next call allocate a
		 * second page over this one's. D-03. */
		bc250_shim_mem_free(adev, &adev->sdma.fence_mem);
		adev->sdma.fence_mem.mc = 0;
		adev->sdma.fence_mem.size = 0;
		return BC250_EINVAL;
	}
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
 * slot that already held the answer cannot pass the test.
 *
 * Split into submit/wait (review 23 MUST-FIX, driver/kmd/gfx.c's GfxFenceEscape): a caller that must
 * hold a spinlock across the ring push (Sdma0RingLock, ADR 0008 stage D - the same reason
 * bc250_sdma_signal_fence()/bc250_sdma_copy_test() are two steps, not one, at every call site that
 * already uses them under that lock) cannot also hold it across the poll below, which can spend the
 * whole of adev->usec_timeout (100 ms) in bc250_shim_udelay() - far longer than a spinlock should
 * ever be held, and inconsistent with the driver's own pattern two call sites over. Both halves are
 * used everywhere bc250_sdma_ring_test() itself is used, below - this file's existing tests
 * (sdma_faults.c, replay_gfx.c, replay_ih.c) exercise the refactor for free, since the monolithic
 * function's own behaviour is unchanged. */
int bc250_sdma_ring_test_submit(struct amdgpu_ring *ring, volatile u32 **out_slot_cpu)
{
	struct amdgpu_device *adev;
	volatile u32 *slot_cpu;
	unsigned int slot;
	u64 gpu_addr;
	int r;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL || out_slot_cpu == NULL)
		return BC250_EINVAL;
	if (ring->funcs->type != AMDGPU_RING_TYPE_SDMA)
		return BC250_EINVAL;

	adev = ring->adev;
	if (adev->sdma.fence_mem.cpu == NULL)
		return BC250_EINVAL;            /* bc250_sdma_fence_page_alloc() was not called */

	/* One scratch slot per engine, and no folding: an engine number this driver does not have is a
	 * caller's mistake, and masking it would have that caller seed, submit against and poll a slot
	 * another engine is using. D-07. */
	if (ring->me >= AMDGPU_MAX_SDMA_INSTANCES)
		return BC250_EINVAL;
	slot = ring->me;
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

	*out_slot_cpu = slot_cpu;
	return 0;
}

int bc250_sdma_ring_test_wait(struct amdgpu_device *adev, volatile u32 *slot_cpu)
{
	unsigned int i;

	if (adev == NULL || slot_cpu == NULL)
		return BC250_EINVAL;
	for (i = 0; i < adev->usec_timeout; i++) {
		if (*slot_cpu == BC250_SDMA_TEST_AFTER)
			break;
		bc250_shim_udelay(1);
	}

	return (i < adev->usec_timeout) ? 0 : BC250_ETIME;
}

int bc250_sdma_ring_test(struct amdgpu_ring *ring)
{
	volatile u32 *slot_cpu = NULL;
	int r = bc250_sdma_ring_test_submit(ring, &slot_cpu);

	if (r != 0)
		return r;
	return bc250_sdma_ring_test_wait(ring->adev, slot_cpu);
}
