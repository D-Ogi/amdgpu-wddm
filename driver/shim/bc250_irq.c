/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * CP and SDMA interrupt enabling. See include/bc250_irq.h for what this is, what it leaves to the
 * miniport, and the full list of upstream functions it follows.
 *
 * All citations are at kernel tag v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449, in
 * driver/amdgpu-import/gfx_v10_0.c and driver/amdgpu-import/sdma_v5_0.c, except the one function
 * that comes from amdkfd and says so where it is defined.
 *
 * Deviation from Linux (ADR 0002): upstream's callbacks take a `struct amdgpu_irq_src *` and an
 * unsigned `type` that they mostly ignore, because amdgpu_irq_get() passes them through. There is
 * no irq source object here, so the arguments that carried no information are gone and the ones
 * that did - which me, which pipe, which SDMA instance - became real parameters.
 *
 * Nothing here takes a lock. bc250_irq_init_mec_pipes() drives GRBM_GFX_CNTL, which upstream does
 * under adev->srbm_mutex; the serialization note in bc250_gfx.h applies to it too.
 */
#include "bc250_irq.h"
#include "bc250_gmc.h"		/* BC250_EINVAL */
#include "bc250_sdma.h"		/* bc250_sdma_reg_offset() */
#include "nv.h"			/* nv_grbm_select() */

/* Our own file, so register offsets come through the SOC15 macros over AMD's headers. */
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"

/* ---------------------------------------------------------------------------------------------
 * gfx_v10_0.c:5376 gfx_v10_0_get_cpg_int_cntl() and :5392 gfx_v10_0_get_cpc_int_cntl()
 *
 * A zero return means "this part has no such register", and every caller tests for it. Note the
 * asymmetry upstream: the CPG helper is called with me starting at 0 and the CPC helper with me
 * starting at 1, because MECs are numbered from 1. The loops below keep that.
 * ------------------------------------------------------------------------------------------- */
static u32 bc250_get_cpg_int_cntl(struct amdgpu_device *adev, int me, int pipe)
{
	if (me != 0)
		return 0;

	switch (pipe) {
	case 0:
		return SOC15_REG_OFFSET(GC, 0, mmCP_INT_CNTL_RING0);
	case 1:
		return SOC15_REG_OFFSET(GC, 0, mmCP_INT_CNTL_RING1);
	default:
		return 0;
	}
}

static u32 bc250_get_cpc_int_cntl(struct amdgpu_device *adev, int me, int pipe)
{
	/*
	 * amdgpu controls only the first MEC. That's why this function only
	 * handles the setting of interrupts for this specific MEC. All other
	 * pipes' interrupts are set by amdkfd.
	 */
	if (me != 1)
		return 0;

	switch (pipe) {
	case 0:
		return SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE0_INT_CNTL);
	case 1:
		return SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE1_INT_CNTL);
	case 2:
		return SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE2_INT_CNTL);
	case 3:
		return SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE3_INT_CNTL);
	default:
		return 0;
	}
}

/* ---------------------------------------------------------------------------------------------
 * amdgpu_amdkfd_gfx_v10.c:140 kgd_init_interrupts()
 *
 * This one is amdkfd's, not amdgpu's, and it is here because this driver owns the compute pipes
 * that KFD owns on Linux. It is the first thing in the traced window: MEC1 pipes 0 to 3, each
 * selected through GRBM_GFX_CNTL and then written through CPC_INT_CNTL with the timestamp and
 * opcode-error bits, which is 0x05000000.
 *
 * Upstream takes `pipe_id` running over all pipes of all MECs and splits it; the caller in
 * kfd_device_queue_manager.c only ever passes pipes of MEC1 that have a queue, which on this part
 * is all four. The split is kept rather than hard-coding mec = 1, so that a part with two MECs
 * would still land in the right place.
 * ------------------------------------------------------------------------------------------- */
int bc250_irq_init_mec_pipes(struct amdgpu_device *adev)
{
	u32 pipe_id;
	u32 pipes;

	if (adev == NULL)
		return BC250_EINVAL;

	pipes = adev->gfx.mec.num_mec * adev->gfx.mec.num_pipe_per_mec;

	for (pipe_id = 0; pipe_id < pipes; pipe_id++) {
		u32 mec = (pipe_id / adev->gfx.mec.num_pipe_per_mec) + 1;
		u32 pipe = (pipe_id % adev->gfx.mec.num_pipe_per_mec);

		nv_grbm_select(adev, mec, pipe, 0, 0);

		WREG32_SOC15(GC, 0, mmCPC_INT_CNTL,
			     CP_INT_CNTL_RING0__TIME_STAMP_INT_ENABLE_MASK |
			     CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK);

		nv_grbm_select(adev, 0, 0, 0, 0);
	}

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * gfx_v10_0.c:9051 gfx_v10_0_set_gfx_eop_interrupt_state()
 * ------------------------------------------------------------------------------------------- */
void bc250_irq_set_gfx_eop(struct amdgpu_device *adev, u32 me, u32 pipe, enum bc250_irq_state state)
{
	u32 cp_int_cntl, cp_int_cntl_reg;

	if (me != 0) {
		dev_warn(adev->dev, "invalid me %u\n", me);
		return;
	}

	cp_int_cntl_reg = bc250_get_cpg_int_cntl(adev, (int)me, (int)pipe);
	if (cp_int_cntl_reg == 0) {
		dev_warn(adev->dev, "invalid pipe %u\n", pipe);
		return;
	}

	cp_int_cntl = RREG32_SOC15_IP(GC, cp_int_cntl_reg);
	cp_int_cntl = REG_SET_FIELD(cp_int_cntl, CP_INT_CNTL_RING0,
				    TIME_STAMP_INT_ENABLE,
				    state == BC250_IRQ_STATE_ENABLE ? 1 : 0);
	WREG32_SOC15_IP(GC, cp_int_cntl_reg, cp_int_cntl);
}

/* ---------------------------------------------------------------------------------------------
 * gfx_v10_0.c:9092 gfx_v10_0_set_compute_eop_interrupt_state()
 * ------------------------------------------------------------------------------------------- */
void bc250_irq_set_compute_eop(struct amdgpu_device *adev, int me, int pipe,
			       enum bc250_irq_state state)
{
	u32 mec_int_cntl, mec_int_cntl_reg;

	mec_int_cntl_reg = bc250_get_cpc_int_cntl(adev, me, pipe);
	if (mec_int_cntl_reg == 0) {
		dev_warn(adev->dev, "invalid me %d pipe %d\n", me, pipe);
		return;
	}

	mec_int_cntl = RREG32_SOC15_IP(GC, mec_int_cntl_reg);
	mec_int_cntl = REG_SET_FIELD(mec_int_cntl, CP_ME1_PIPE0_INT_CNTL,
				     TIME_STAMP_INT_ENABLE,
				     state == BC250_IRQ_STATE_ENABLE ? 1 : 0);
	WREG32_SOC15_IP(GC, mec_int_cntl_reg, mec_int_cntl);
}

/* ---------------------------------------------------------------------------------------------
 * gfx_v10_0.c:9411 gfx_v10_0_kiq_set_interrupt_state(), type AMDGPU_CP_KIQ_IRQ_DRIVER0
 *
 * The only type the KIQ has; upstream BUG()s on anything else, so there is nothing to select on and
 * the type argument is gone. Two registers: the global CPC_INT_CNTL and the KIQ's own pipe, which
 * the upstream arithmetic reaches as mmCP_ME2_PIPE0_INT_CNTL + ring->pipe. On unit A the KIQ is me
 * 2, pipe 1, so that lands on CP_ME2_PIPE1_INT_CNTL, which is what the trace writes.
 * ------------------------------------------------------------------------------------------- */
int bc250_irq_set_kiq(struct amdgpu_device *adev, enum bc250_irq_state state)
{
	uint32_t tmp, target;
	struct amdgpu_ring *ring;

	if (adev == NULL)
		return BC250_EINVAL;

	ring = &adev->gfx.kiq[0].ring;

	if (ring->me == 1)
		target = SOC15_REG_OFFSET(GC, 0, mmCP_ME1_PIPE0_INT_CNTL);
	else
		target = SOC15_REG_OFFSET(GC, 0, mmCP_ME2_PIPE0_INT_CNTL);
	target += ring->pipe;

	tmp = RREG32_SOC15(GC, 0, mmCPC_INT_CNTL);
	tmp = REG_SET_FIELD(tmp, CPC_INT_CNTL, GENERIC2_INT_ENABLE,
			    state == BC250_IRQ_STATE_ENABLE ? 1 : 0);
	WREG32_SOC15(GC, 0, mmCPC_INT_CNTL, tmp);

	tmp = RREG32_SOC15_IP(GC, target);
	tmp = REG_SET_FIELD(tmp, CP_ME2_PIPE0_INT_CNTL, GENERIC2_INT_ENABLE,
			    state == BC250_IRQ_STATE_ENABLE ? 1 : 0);
	WREG32_SOC15_IP(GC, target, tmp);

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * gfx_v10_0.c:9227 gfx_v10_0_set_priv_reg_fault_state() and :9273
 * gfx_v10_0_set_bad_op_fault_state()
 *
 * The two are the same walk - every ME pipe, then every MEC pipe - over a different field, so the
 * walk is written once and the field is the parameter. Upstream repeats it because REG_SET_FIELD is
 * a macro over the field's name and cannot take it as an argument; here the two callers pass the
 * mask and shift the macro would have produced, from the same header names.
 * ------------------------------------------------------------------------------------------- */
static void bc250_set_cpg_cpc_bit(struct amdgpu_device *adev,
				  u32 cpg_mask, u32 cpc_mask, enum bc250_irq_state state)
{
	u32 cp_int_cntl_reg, cp_int_cntl;
	int i, j;

	for (i = 0; i < (int)adev->gfx.me.num_me; i++) {
		for (j = 0; j < (int)adev->gfx.me.num_pipe_per_me; j++) {
			cp_int_cntl_reg = bc250_get_cpg_int_cntl(adev, i, j);

			if (cp_int_cntl_reg) {
				cp_int_cntl = RREG32_SOC15_IP(GC, cp_int_cntl_reg);
				if (state == BC250_IRQ_STATE_ENABLE)
					cp_int_cntl |= cpg_mask;
				else
					cp_int_cntl &= ~cpg_mask;
				WREG32_SOC15_IP(GC, cp_int_cntl_reg, cp_int_cntl);
			}
		}
	}

	if (cpc_mask == 0)
		return;

	for (i = 0; i < (int)adev->gfx.mec.num_mec; i++) {
		for (j = 0; j < (int)adev->gfx.mec.num_pipe_per_mec; j++) {
			/* MECs start at 1 */
			cp_int_cntl_reg = bc250_get_cpc_int_cntl(adev, i + 1, j);

			if (cp_int_cntl_reg) {
				cp_int_cntl = RREG32_SOC15_IP(GC, cp_int_cntl_reg);
				if (state == BC250_IRQ_STATE_ENABLE)
					cp_int_cntl |= cpc_mask;
				else
					cp_int_cntl &= ~cpc_mask;
				WREG32_SOC15_IP(GC, cp_int_cntl_reg, cp_int_cntl);
			}
		}
	}
}

int bc250_irq_set_priv_reg_fault(struct amdgpu_device *adev, enum bc250_irq_state state)
{
	if (adev == NULL)
		return BC250_EINVAL;

	bc250_set_cpg_cpc_bit(adev,
			      CP_INT_CNTL_RING0__PRIV_REG_INT_ENABLE_MASK,
			      CP_ME1_PIPE0_INT_CNTL__PRIV_REG_INT_ENABLE_MASK,
			      state);
	return 0;
}

int bc250_irq_set_bad_op_fault(struct amdgpu_device *adev, enum bc250_irq_state state)
{
	if (adev == NULL)
		return BC250_EINVAL;

	bc250_set_cpg_cpc_bit(adev,
			      CP_INT_CNTL_RING0__OPCODE_ERROR_INT_ENABLE_MASK,
			      CP_ME1_PIPE0_INT_CNTL__OPCODE_ERROR_INT_ENABLE_MASK,
			      state);
	return 0;
}

/* gfx_v10_0.c:9318 gfx_v10_0_set_priv_inst_fault_state(): the ME pipes only. There is no
 * PRIV_INSTR_INT_ENABLE field on CP_ME1_PIPE0_INT_CNTL, which is why upstream has no second loop
 * here and why the CPC mask below is zero. */
int bc250_irq_set_priv_inst_fault(struct amdgpu_device *adev, enum bc250_irq_state state)
{
	if (adev == NULL)
		return BC250_EINVAL;

	bc250_set_cpg_cpc_bit(adev,
			      CP_INT_CNTL_RING0__PRIV_INSTR_INT_ENABLE_MASK,
			      0,
			      state);
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * sdma_v5_0.c:1681 sdma_v5_0_set_trap_irq_state()
 *
 * Upstream selects the instance from the irq type; here the instance is the argument.
 * ------------------------------------------------------------------------------------------- */
int bc250_irq_set_sdma_trap(struct amdgpu_device *adev, int instance, enum bc250_irq_state state)
{
	u32 sdma_cntl;
	u32 reg_offset;

	if (adev == NULL || instance < 0 || instance >= AMDGPU_MAX_SDMA_INSTANCES)
		return BC250_EINVAL;

	reg_offset = bc250_sdma_reg_offset(adev, (u32)instance, mmSDMA0_CNTL);

	sdma_cntl = RREG32(reg_offset);
	sdma_cntl = REG_SET_FIELD(sdma_cntl, SDMA0_CNTL, TRAP_ENABLE,
				  state == BC250_IRQ_STATE_ENABLE ? 1 : 0);
	WREG32(reg_offset, sdma_cntl);

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The two drivers
 * ------------------------------------------------------------------------------------------- */

/* amdgpu_fence.c:585 amdgpu_fence_driver_hw_init(): one amdgpu_irq_get() per ring, in the order the
 * rings were registered. On this part that is the gfx ring, the eight compute rings, the KIQ, then
 * the two SDMA engines, and the trace has exactly that order.
 *
 * The compute loop walks pipes, not rings, on purpose: amdgpu_irq_get() refcounts per interrupt
 * type and only calls the callback when a type goes from 0 to 1. Two compute rings share each
 * pipe's type, so eight amdgpu_irq_get() calls upstream produce four register accesses, which is
 * what the trace shows. Walking the eight rings here would produce eight and would not match. */
int bc250_irq_hw_init(struct amdgpu_device *adev)
{
	u32 j;
	int i;
	int r;

	if (adev == NULL)
		return BC250_EINVAL;

	bc250_irq_set_gfx_eop(adev, 0, 0, BC250_IRQ_STATE_ENABLE);

	for (j = 0; j < adev->gfx.mec.num_pipe_per_mec; j++)
		bc250_irq_set_compute_eop(adev, 1, (int)j, BC250_IRQ_STATE_ENABLE);

	r = bc250_irq_set_kiq(adev, BC250_IRQ_STATE_ENABLE);
	if (r)
		return r;

	for (i = 0; i < adev->sdma.num_instances; i++) {
		r = bc250_irq_set_sdma_trap(adev, i, BC250_IRQ_STATE_ENABLE);
		if (r)
			return r;
	}

	return 0;
}

/* gfx_v10_0.c:7833 gfx_v10_0_late_init() */
int bc250_irq_late_init(struct amdgpu_device *adev)
{
	int r;

	r = bc250_irq_set_priv_reg_fault(adev, BC250_IRQ_STATE_ENABLE);
	if (r)
		return r;

	r = bc250_irq_set_priv_inst_fault(adev, BC250_IRQ_STATE_ENABLE);
	if (r)
		return r;

	return bc250_irq_set_bad_op_fault(adev, BC250_IRQ_STATE_ENABLE);
}
