/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * GFX10 CP, KIQ and ring bring-up for Cyan Skillfish. See include/bc250_gfx.h for what this is and
 * why it is transcribed rather than imported.
 *
 * Every block names the upstream function and line it follows, in
 * driver/amdgpu-import/gfx_v10_0.c (reference-only copy of the kernel's
 * drivers/gpu/drm/amd/amdgpu/gfx_v10_0.c at v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449)
 * or in amdgpu_gfx.c / amdgpu_ring.c at the same tag. Where a branch is not taken on this part, the
 * comment says which branch and why, so that a reader comparing with upstream can see that the
 * omission is a decision and not an oversight.
 *
 * Nothing here takes a lock. See the serialization note in bc250_gfx.h.
 */
#include "bc250_gfx.h"
#include "bc250_irq.h"	/* the three fault sources, turned off by bc250_gfx_hw_fini() */
#include "bc250_gmc.h"
#include "nv.h"

/* Our own file, so register offsets come through the SOC15 macros over AMD's headers. No address,
 * mask or shift below is written out; every one of them is a name from these three. */
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include <navi10_enum.h>
#include "soc15_common.h"

/* Imported data, unmodified: the MQD layouts, the PM4 packet definitions and the clear-state
 * tables. clearstate_gfx10.h defines static const arrays, so this is the one translation unit that
 * may include it. */
#include "v10_structs.h"
#include "nvd.h"
#include "clearstate_defs.h"
#include "clearstate_gfx10.h"

/* ---------------------------------------------------------------------------------------------
 * Constants, all from gfx_v10_0.c or confirmed by unit A's E03 trace
 * ------------------------------------------------------------------------------------------- */

/* gfx_v10_0.c:57 */
#define GFX10_MEC_HPD_SIZE	2048
/* Confirmed by the trace: CP_HQD_EOP_CONTROL = 0x00000008, and EOP_SIZE is
 * order_base_2(GFX10_MEC_HPD_SIZE / 4) - 1 = 8. */

/* gfx_v10_0.c:5169 */
#define DEFAULT_SH_MEM_BASES	(0x6000)

/* gfx_v10_0.c:3668 */
#define DEFAULT_SH_MEM_CONFIG \
	((SH_MEM_ADDRESS_MODE_64 << SH_MEM_CONFIG__ADDRESS_MODE__SHIFT) | \
	 (SH_MEM_ALIGNMENT_MODE_UNALIGNED << SH_MEM_CONFIG__ALIGNMENT_MODE__SHIFT) | \
	 (SH_MEM_RETRY_MODE_ALL << SH_MEM_CONFIG__RETRY_MODE__SHIFT) | \
	 (3 << SH_MEM_CONFIG__INITIAL_INST_PREFETCH__SHIFT))

/* amdgpu_ring.c:114 ring->ring_size = roundup_pow_of_two(max_dw * 4 * sched_hw_submission).
 * Every ring here is created with max_dw = 1024 (amdgpu_gfx.c:331 for the KIQ, gfx_v10_0_sw_init
 * for the others). sched_hw_submission is amdgpu_sched_hw_submission, default 2, raised to 256 for
 * the KIQ. So the KIQ ring is 1 MB and the rest are 8 KB.
 *
 * The KIQ size is not taken on trust: the traced CP_HQD_PQ_CONTROL is 0xD0308911, whose QUEUE_SIZE
 * field is 17, and order_base_2(queue_size / 4) - 1 = 17 gives exactly 1 MB. */
#define BC250_RING_MAX_DW	1024u
#define BC250_KIQ_RING_SIZE	(BC250_RING_MAX_DW * 4u * 256u)
#define BC250_RING_SIZE		(BC250_RING_MAX_DW * 4u * 2u)

/* amdgpu_ring.h: the align mask and NOP of the GFX10 ring function tables (gfx_v10_0.c:9829,
 * :9887, :9927). All three CP ring types pad to 8 dwords with PACKET2 NOPs. */
#define BC250_CP_ALIGN_MASK	0xffu
#define BC250_CP_NOP		0xffff1000u

/* [shim] Writeback slots. amdgpu hands out 4-byte slots from a shared writeback buffer
 * (amdgpu_device_wb_get); the shim takes one GTT page and cuts it up, which is the same thing with
 * a fixed layout. Each ring gets an 8-byte read-pointer slot and an 8-byte write-pointer slot. */
#define BC250_WB_SLOT_BYTES	8u
#define BC250_WB_RINGS		10u    /* KIQ + 8 compute + 1 gfx */
#define BC250_WB_BYTES		(BC250_WB_RINGS * 2u * BC250_WB_SLOT_BYTES)

static const struct amdgpu_ring_funcs bc250_ring_funcs_gfx = {
	AMDGPU_RING_TYPE_GFX, BC250_CP_ALIGN_MASK, BC250_CP_NOP
};
static const struct amdgpu_ring_funcs bc250_ring_funcs_compute = {
	AMDGPU_RING_TYPE_COMPUTE, BC250_CP_ALIGN_MASK, BC250_CP_NOP
};
static const struct amdgpu_ring_funcs bc250_ring_funcs_kiq = {
	AMDGPU_RING_TYPE_KIQ, BC250_CP_ALIGN_MASK, BC250_CP_NOP
};

/* ---------------------------------------------------------------------------------------------
 * nv.c:317 nv_grbm_select()
 * ------------------------------------------------------------------------------------------- */
void nv_grbm_select(struct amdgpu_device *adev, u32 me, u32 pipe, u32 queue, u32 vmid)
{
	u32 grbm_gfx_cntl = 0;

	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, PIPEID, pipe);
	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, MEID, me);
	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, VMID, vmid);
	grbm_gfx_cntl = REG_SET_FIELD(grbm_gfx_cntl, GRBM_GFX_CNTL, QUEUEID, queue);

	WREG32_SOC15(GC, 0, mmGRBM_GFX_CNTL, grbm_gfx_cntl);
}

/* ---------------------------------------------------------------------------------------------
 * Stage 1: golden registers
 *
 * soc15.c:471 soc15_program_register_sequence(), over the imported
 * golden_settings_gc_10_0_cyan_skillfish table (gfx_v10_0.c:3582-3615, extracted by
 * tools/import/extract_table.py). soc15.c itself cannot be imported: it includes the whole gfx9-era
 * register world and defines the SOC15 IP block, BACO reset and the virt ops.
 *
 * Two branches of the upstream loop are not taken here and are left out with this note rather than
 * written as dead code: the non-GC arm of `entry->hwip == GC_HWIP`, because every entry of this
 * table is GC; and the WREG32_RLC special case for four register offsets, because it resolves to a
 * plain write when the RLC register-access path is off, which it is on this part
 * (adev->gfx.rlc.rlcg_reg_access_supported is false).
 * ------------------------------------------------------------------------------------------- */

/* [amdgpu] soc15.h: struct soc15_reg_golden and the macro the extracted table is written in. */
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

static const struct soc15_reg_golden golden_settings_gc_10_0_cyan_skillfish[] = {
#include "generated/gfx10_golden_cyan_skillfish.inc"
};

int bc250_gfx_init_golden_registers(struct amdgpu_device *adev)
{
	const struct soc15_reg_golden *entry;
	u32 tmp, reg;
	unsigned int i;

	if (adev == NULL)
		return BC250_EINVAL;

	for (i = 0; i < ARRAY_SIZE(golden_settings_gc_10_0_cyan_skillfish); ++i) {
		entry = &golden_settings_gc_10_0_cyan_skillfish[i];
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

	/* gfx_v10_0.c:3892 gfx_v10_0_init_spm_golden_registers(): its switch has no case for
	 * IP_VERSION(10, 1, 3), so it does nothing on this part. */
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Stage 2: gfx_v10_0.c:7280 gfx_v10_0_check_grbm_cam_remapping(), default branch
 * ------------------------------------------------------------------------------------------- */
int bc250_gfx_grbm_cam_probe(struct amdgpu_device *adev, bool *already_remapped)
{
	uint32_t data, pattern = 0xDEADBEEF;

	if (adev == NULL || already_remapped == NULL)
		return BC250_EINVAL;

	/* Check if mmVGT_ESGS_RING_SIZE_UMD has been remapped to mmVGT_ESGS_RING_SIZE. */
	data = RREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE);
	WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE, 0);
	WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE_UMD, pattern);

	if (RREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE) == pattern) {
		WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE_UMD, data);
		*already_remapped = true;
		return 0;
	}

	WREG32_SOC15(GC, 0, mmVGT_ESGS_RING_SIZE, data);
	*already_remapped = false;

	/* gfx_v10_0.c:7324 gfx_v10_0_setup_grbm_cam_remapping() would run here. On unit A the probe
	 * returns true - the firmware has already set the CAM up - so the programming never happens
	 * in the traced window and is not transcribed. If a unit ever returns false, this is where it
	 * has to be added, and the caller is told so rather than the driver carrying on quietly. */
	dev_warn(adev->dev, "GRBM CAM is not remapped; setup_grbm_cam_remapping is not implemented\n");
	return BC250_EIO;
}

/* ---------------------------------------------------------------------------------------------
 * Stage 3: constants
 * ------------------------------------------------------------------------------------------- */

/* gfx_v10_0.c:5055 gfx_v10_0_select_se_sh() */
static void bc250_select_se_sh(struct amdgpu_device *adev, u32 se_num, u32 sh_num, u32 instance)
{
	u32 data;

	if (instance == 0xffffffff)
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(0, GRBM_GFX_INDEX, INSTANCE_INDEX, instance);

	if (se_num == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SE_INDEX, se_num);

	if (sh_num == 0xffffffff)
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_BROADCAST_WRITES, 1);
	else
		data = REG_SET_FIELD(data, GRBM_GFX_INDEX, SA_INDEX, sh_num);

	WREG32_SOC15(GC, 0, mmGRBM_GFX_INDEX, data);
}

/* amdgpu_gfx.h amdgpu_gfx_create_bitmask() */
static u32 bc250_create_bitmask(u32 bit_width)
{
	return (u32)((1ULL << bit_width) - 1);
}

/* gfx_v10_0.c:5082 gfx_v10_0_get_rb_active_bitmap() */
static u32 bc250_get_rb_active_bitmap(struct amdgpu_device *adev)
{
	u32 data, mask;

	data = RREG32_SOC15(GC, 0, mmCC_RB_BACKEND_DISABLE);
	data |= RREG32_SOC15(GC, 0, mmGC_USER_RB_BACKEND_DISABLE);

	data &= CC_RB_BACKEND_DISABLE__BACKEND_DISABLE_MASK;
	data >>= GC_USER_RB_BACKEND_DISABLE__BACKEND_DISABLE__SHIFT;

	mask = bc250_create_bitmask(adev->gfx.config.max_backends_per_se /
				    adev->gfx.config.max_sh_per_se);

	return (~data) & mask;
}

/* gfx_v10_0.c:5098 gfx_v10_0_setup_rb(). The gfx_v10_3_get_disabled_sa() branch is for
 * IP_VERSION(10, 3, 0/3/6) only and is left out. */
static void bc250_setup_rb(struct amdgpu_device *adev)
{
	u32 i, j, data;
	u32 active_rbs = 0;
	u32 rb_bitmap_width_per_sh = adev->gfx.config.max_backends_per_se /
				     adev->gfx.config.max_sh_per_se;

	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			bc250_select_se_sh(adev, i, j, 0xffffffff);
			data = bc250_get_rb_active_bitmap(adev);
			active_rbs |= data << ((i * adev->gfx.config.max_sh_per_se + j) *
					       rb_bitmap_width_per_sh);
		}
	}
	bc250_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);

	adev->gfx.config.backend_enable_mask = active_rbs;
	adev->gfx.config.num_rbs = hweight32(active_rbs);
}

/* gfx_v10_0.c:5134 gfx_v10_0_init_pa_sc_tile_steering_override(). The >= IP_VERSION(10, 3, 0)
 * early return does not apply to 10.1.3. This value never reaches a register: it is written into
 * the clear-state PM4 stream by gfx_v10_0_cp_gfx_start(). */
static u32 bc250_init_pa_sc_tile_steering_override(struct amdgpu_device *adev)
{
	uint32_t num_sc;
	uint32_t enabled_rb_per_sh;
	uint32_t active_rb_bitmap;
	uint32_t num_rb_per_sc;
	uint32_t num_packer_per_sc;
	uint32_t pa_sc_tile_steering_override;

	num_sc = adev->gfx.config.max_shader_engines * adev->gfx.config.max_sh_per_se *
		 adev->gfx.config.num_sc_per_sh;
	active_rb_bitmap = bc250_get_rb_active_bitmap(adev);
	enabled_rb_per_sh = hweight32(active_rb_bitmap);
	num_rb_per_sc = enabled_rb_per_sh / adev->gfx.config.num_sc_per_sh;
	num_packer_per_sc = adev->gfx.config.num_packer_per_sc;

	pa_sc_tile_steering_override = 0;
	pa_sc_tile_steering_override |=
		(order_base_2(num_sc) << PA_SC_TILE_STEERING_OVERRIDE__NUM_SC__SHIFT) &
		PA_SC_TILE_STEERING_OVERRIDE__NUM_SC_MASK;
	pa_sc_tile_steering_override |=
		(order_base_2(num_rb_per_sc) << PA_SC_TILE_STEERING_OVERRIDE__NUM_RB_PER_SC__SHIFT) &
		PA_SC_TILE_STEERING_OVERRIDE__NUM_RB_PER_SC_MASK;
	pa_sc_tile_steering_override |=
		(order_base_2(num_packer_per_sc) << PA_SC_TILE_STEERING_OVERRIDE__NUM_PACKER_PER_SC__SHIFT) &
		PA_SC_TILE_STEERING_OVERRIDE__NUM_PACKER_PER_SC_MASK;

	return pa_sc_tile_steering_override;
}

/* gfx_v10_0.c:5173 gfx_v10_0_debug_trap_config_init() */
static void bc250_debug_trap_config_init(struct amdgpu_device *adev, u32 first_vmid, u32 last_vmid)
{
	uint32_t data;
	uint32_t trap_config_vmid_mask = 0;
	u32 i;

	for (i = first_vmid; i < last_vmid; i++)
		trap_config_vmid_mask |= (1u << i);

	data = REG_SET_FIELD(0, SPI_GDBG_TRAP_CONFIG, VMID_SEL, trap_config_vmid_mask);
	data = REG_SET_FIELD(data, SPI_GDBG_TRAP_CONFIG, TRAP_EN, 1);
	WREG32(SOC15_REG_OFFSET(GC, 0, mmSPI_GDBG_TRAP_CONFIG), data);
	WREG32(SOC15_REG_OFFSET(GC, 0, mmSPI_GDBG_TRAP_MASK), 0);

	WREG32(SOC15_REG_OFFSET(GC, 0, mmSPI_GDBG_TRAP_DATA0), 0);
	WREG32(SOC15_REG_OFFSET(GC, 0, mmSPI_GDBG_TRAP_DATA1), 0);
}

/* gfx_v10_0.c:5196 gfx_v10_0_init_compute_vmid() */
static void bc250_init_compute_vmid(struct amdgpu_device *adev)
{
	u32 i;
	uint32_t sh_mem_bases;

	/*
	 * Configure apertures:
	 * LDS:         0x60000000'00000000 - 0x60000001'00000000 (4GB)
	 * Scratch:     0x60000001'00000000 - 0x60000002'00000000 (4GB)
	 * GPUVM:       0x60010000'00000000 - 0x60020000'00000000 (1TB)
	 */
	sh_mem_bases = DEFAULT_SH_MEM_BASES | (DEFAULT_SH_MEM_BASES << 16);

	for (i = adev->vm_manager.first_kfd_vmid; i < AMDGPU_NUM_VMID; i++) {
		nv_grbm_select(adev, 0, 0, 0, i);
		/* CP and shaders */
		WREG32_SOC15(GC, 0, mmSH_MEM_CONFIG, DEFAULT_SH_MEM_CONFIG);
		WREG32_SOC15(GC, 0, mmSH_MEM_BASES, sh_mem_bases);
	}
	nv_grbm_select(adev, 0, 0, 0, 0);

	for (i = adev->vm_manager.first_kfd_vmid; i < AMDGPU_NUM_VMID; i++) {
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_BASE, 2 * i, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_SIZE, 2 * i, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_GWS_VMID0, i, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_OA_VMID0, i, 0);
	}

	bc250_debug_trap_config_init(adev, adev->vm_manager.first_kfd_vmid, AMDGPU_NUM_VMID);
}

/* gfx_v10_0.c:5234 gfx_v10_0_init_gds_vmid() */
static void bc250_init_gds_vmid(struct amdgpu_device *adev)
{
	u32 vmid;

	for (vmid = 1; vmid < AMDGPU_NUM_VMID; vmid++) {
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_BASE, 2 * vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_VMID0_SIZE, 2 * vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_GWS_VMID0, vmid, 0);
		WREG32_SOC15_OFFSET(GC, 0, mmGDS_OA_VMID0, vmid, 0);
	}
}

/* gfx_v10_0.c:5338 gfx_v10_0_constants_init().
 *
 * gfx_v10_0_get_cu_info() and gfx_v10_0_get_tcc_info() are register reads whose results never reach
 * a register on this path; they are folded into bc250_get_cu_tcc_info() below so that the read
 * sequence the trace shows is reproduced. */
static void bc250_get_cu_tcc_info(struct amdgpu_device *adev)
{
	u32 i, j;

	/* gfx_v10_0.c:10115 gfx_v10_0_get_cu_info(): per SE/SA, read the shader-array config and the
	 * RB config. The shim does not model the CU mask; what matters for the register stream is
	 * that the same reads happen under the same GRBM_GFX_INDEX selections. */
	for (i = 0; i < adev->gfx.config.max_shader_engines; i++) {
		for (j = 0; j < adev->gfx.config.max_sh_per_se; j++) {
			bc250_select_se_sh(adev, i, j, 0xffffffff);
			(void)RREG32_SOC15(GC, 0, mmCC_GC_SHADER_ARRAY_CONFIG);
			(void)RREG32_SOC15(GC, 0, mmGC_USER_SHADER_ARRAY_CONFIG);
		}
	}
	bc250_select_se_sh(adev, 0xffffffff, 0xffffffff, 0xffffffff);

	/* gfx_v10_0.c:5320 gfx_v10_0_get_tcc_info() */
	adev->gfx.config.tcc_disabled_mask =
		RREG32_SOC15(GC, 0, mmCGTS_TCC_DISABLE) |
		((u64)RREG32_SOC15(GC, 0, mmCGTS_USER_TCC_DISABLE) << 16);
}

int bc250_gfx_constants_init(struct amdgpu_device *adev)
{
	u32 tmp;
	u32 i;

	if (adev == NULL)
		return BC250_EINVAL;

	WREG32_FIELD15(GC, 0, GRBM_CNTL, READ_TIMEOUT, 0xff);

	bc250_setup_rb(adev);
	bc250_get_cu_tcc_info(adev);
	adev->gfx.config.pa_sc_tile_steering_override =
		bc250_init_pa_sc_tile_steering_override(adev);

	/* Where unit A's kernel and mainline v6.18 differ, and the trace decides.
	 *
	 * Mainline v6.18 gfx_v10_0_constants_init() goes straight from the tile-steering override
	 * to the SH_MEM loop. The kernel that produced the E03 trace, Alpine 6.18.52-0-lts, has an
	 * extra write here (6.18.52 gfx_v10_0.c:5352, absent from v6.18:5348):
	 *
	 *     WREG32_FIELD15(GC, 0, DB_RING_CONTROL, COUNTER_CONTROL,
	 *                    (adev->gfx.me.num_pipe_per_me > 1) ? 0 : 1);
	 *
	 * and the trace has it: DB_RING_CONTROL read 0x00000001, written 0x00000001, between the
	 * GC_USER_RB_BACKEND_DISABLE read and the first SH_MEM_CONFIG write. The written value says
	 * num_pipe_per_me was 1. The two versions of the file are diffed in
	 * P:\BC-250\scratch\m5-gfx\gfx_v10_0.v6.18-vs-6.18.52.diff; this is the only one of the
	 * nine hunks that changes a register this sequence touches. */
	WREG32_FIELD15(GC, 0, DB_RING_CONTROL, COUNTER_CONTROL,
		       (adev->gfx.me.num_pipe_per_me > 1) ? 0 : 1);

	/* where to put LDS, scratch, GPUVM in FSA64 space */
	for (i = 0; i < adev->vm_manager.id_mgr[AMDGPU_GFXHUB(0)].num_ids; i++) {
		nv_grbm_select(adev, 0, 0, 0, i);
		/* CP and shaders */
		WREG32_SOC15(GC, 0, mmSH_MEM_CONFIG, DEFAULT_SH_MEM_CONFIG);
		if (i != 0) {
			tmp = REG_SET_FIELD(0, SH_MEM_BASES, PRIVATE_BASE,
					    (adev->gmc.private_aperture_start >> 48));
			tmp = REG_SET_FIELD(tmp, SH_MEM_BASES, SHARED_BASE,
					    (adev->gmc.shared_aperture_start >> 48));
			WREG32_SOC15(GC, 0, mmSH_MEM_BASES, tmp);
		}
	}
	nv_grbm_select(adev, 0, 0, 0, 0);

	bc250_init_compute_vmid(adev);
	bc250_init_gds_vmid(adev);

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Stage 4: RLC
 * ------------------------------------------------------------------------------------------- */

/* gfx_v10_0.c:4325 gfx_v10_0_get_csb_buffer(), with amdgpu_gfx.c's three csb helpers inlined at
 * the points it calls them (they are four, three and four writes respectively). */
static void bc250_get_csb_buffer(struct amdgpu_device *adev, u32 *buffer)
{
	const struct cs_section_def *sect;
	const struct cs_extent_def *ext;
	u32 count = 0;
	int ctx_reg_offset;
	u32 i;   /* upstream `int i`; cs_extent_def::reg_count is unsigned, and MSVC /W4 says so */

	if (adev->gfx.rlc.cs_data == NULL || buffer == NULL)
		return;

	/* amdgpu_gfx.c amdgpu_gfx_csb_preamble_start() */
	buffer[count++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
	buffer[count++] = PACKET3_PREAMBLE_BEGIN_CLEAR_STATE;
	buffer[count++] = PACKET3(PACKET3_CONTEXT_CONTROL, 1);
	buffer[count++] = 0x80000000;
	buffer[count++] = 0x80000000;

	/* amdgpu_gfx.c amdgpu_gfx_csb_data_parser() */
	for (sect = adev->gfx.rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id != SECT_CONTEXT)
				continue;
			buffer[count++] = PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count);
			buffer[count++] = ext->reg_index - PACKET3_SET_CONTEXT_REG_START;
			for (i = 0; i < ext->reg_count; i++)
				buffer[count++] = ext->extent[i];
		}
	}

	ctx_reg_offset = (int)(SOC15_REG_OFFSET(GC, 0, mmPA_SC_TILE_STEERING_OVERRIDE) -
			       PACKET3_SET_CONTEXT_REG_START);
	buffer[count++] = PACKET3(PACKET3_SET_CONTEXT_REG, 1);
	buffer[count++] = (u32)ctx_reg_offset;
	buffer[count++] = adev->gfx.config.pa_sc_tile_steering_override;

	/* amdgpu_gfx.c amdgpu_gfx_csb_preamble_end() */
	buffer[count++] = PACKET3(PACKET3_PREAMBLE_CNTL, 0);
	buffer[count++] = PACKET3_PREAMBLE_END_CLEAR_STATE;
	buffer[count++] = PACKET3(PACKET3_CLEAR_STATE, 0);
	buffer[count++] = 0;
}

/* gfx_v10_0.c:4295 gfx_v10_0_get_csb_size() */
static u32 bc250_get_csb_size(struct amdgpu_device *adev)
{
	u32 count = 0;
	const struct cs_section_def *sect;
	const struct cs_extent_def *ext;

	/* begin clear state */
	count += 2;
	/* context control state */
	count += 3;

	for (sect = adev->gfx.rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT)
				count += 2 + ext->reg_count;
			else
				return 0;
		}
	}

	/* set PA_SC_TILE_STEERING_OVERRIDE */
	count += 3;
	/* end clear state */
	count += 2;
	/* clear state */
	count += 2;

	return count;
}

/* gfx_v10_0.c:5446 gfx_v10_0_init_csb(), else branch (the RLC-indirect one is 10.1.2 only). */
static void bc250_init_csb(struct amdgpu_device *adev)
{
	bc250_get_csb_buffer(adev, adev->gfx.rlc.cs_ptr);

	WREG32_SOC15(GC, 0, mmRLC_CSIB_ADDR_HI, (u32)(adev->gfx.rlc.clear_state_gpu_addr >> 32));
	WREG32_SOC15(GC, 0, mmRLC_CSIB_ADDR_LO,
		     (u32)(adev->gfx.rlc.clear_state_gpu_addr & 0xfffffffc));
	WREG32_SOC15(GC, 0, mmRLC_CSIB_LENGTH, adev->gfx.rlc.clear_state_size);
}

/* gfx_v10_0.c:8297 gfx_v10_0_update_spm_vmid_internal(), non-SR-IOV arm. */
static void bc250_update_spm_vmid(struct amdgpu_device *adev, unsigned int vmid)
{
	u32 reg, pre_data, data;

	reg = SOC15_REG_OFFSET(GC, 0, mmRLC_SPM_MC_CNTL);
	pre_data = RREG32(reg);

	data = pre_data & (~RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK);
	data |= (vmid & RLC_SPM_MC_CNTL__RLC_SPM_VMID_MASK) << RLC_SPM_MC_CNTL__RLC_SPM_VMID__SHIFT;

	if (pre_data != data)
		WREG32_SOC15(GC, 0, mmRLC_SPM_MC_CNTL, data);
}

/* gfx_v10_0.c:5467 gfx_v10_0_rlc_stop() */
static void bc250_rlc_stop(struct amdgpu_device *adev)
{
	u32 tmp = RREG32_SOC15(GC, 0, mmRLC_CNTL);

	tmp = REG_SET_FIELD(tmp, RLC_CNTL, RLC_ENABLE_F32, 0);
	WREG32_SOC15(GC, 0, mmRLC_CNTL, tmp);
}

/* gfx_v10_0.c:5484 gfx_v10_0_rlc_smu_handshake_cntl() */
static void bc250_rlc_smu_handshake_cntl(struct amdgpu_device *adev, bool enable)
{
	uint32_t rlc_pg_cntl;

	rlc_pg_cntl = RREG32_SOC15(GC, 0, mmRLC_PG_CNTL);

	if (!enable)
		rlc_pg_cntl |= 0x800000;
	else
		rlc_pg_cntl &= ~0x800000u;
	WREG32_SOC15(GC, 0, mmRLC_PG_CNTL, rlc_pg_cntl);
}

/* gfx_v10_0.c:5505 gfx_v10_0_rlc_start(). `pp_gfxoff` stands in for
 * (amdgpu_pp_feature_mask & PP_GFXOFF_MASK); on unit A it was set, so the handshake write does not
 * happen and the trace has exactly one RLC_PG_CNTL write. */
static void bc250_rlc_start(struct amdgpu_device *adev, bool pp_gfxoff)
{
	if (!pp_gfxoff)
		bc250_rlc_smu_handshake_cntl(adev, false);

	WREG32_FIELD15(GC, 0, RLC_CNTL, RLC_ENABLE_F32, 1);
	bc250_shim_udelay(50);
}

/* gfx_v10_0.c:5557 gfx_v10_0_rlc_resume(), else branch.
 *
 * The first branch (autoload) is not taken: amdgpu_psp.c:235 sets psp->autoload_supported = false
 * for MP0 IP_VERSION(11, 0, 8) with AMD_APU_IS_CYAN_SKILLFISH2. The two microcode loads inside the
 * else branch are DIRECT and RLC_BACKDOOR_AUTO only; this part loads firmware through the PSP, so
 * neither runs. */
int bc250_gfx_rlc_resume(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return BC250_EINVAL;
	if (adev->gfx.rlc.cs_ptr == NULL)
		return BC250_EINVAL;

	bc250_rlc_stop(adev);

	/* disable CG */
	WREG32_SOC15(GC, 0, mmRLC_CGCG_CGLS_CTRL, 0);

	/* disable PG */
	WREG32_SOC15(GC, 0, mmRLC_PG_CNTL, 0);

	bc250_init_csb(adev);

	bc250_update_spm_vmid(adev, 0xf);

	bc250_rlc_start(adev, adev->gfx.pp_gfxoff);

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Stage 5-8: the CP
 * ------------------------------------------------------------------------------------------- */

/* amdgpu_gfx.c:190 amdgpu_gfx_is_high_priority_compute_queue(). "Policy: use 1st queue as high
 * priority compute queue if we have more than one compute queue."
 *
 * MEASURED on unit A, and the reason this function exists rather than a comment saying it cannot
 * happen. Compute MQDs never reach a register, so the E03 register trace cannot show this; the MQD
 * dumps in evidence/linux/2026-09-21-E03-init-trace/rings/ can and do. Of the eight,
 * amdgpu_mqd_comp_1.0.0 alone has cp_hqd_pipe_priority 2, cp_hqd_queue_priority 15 and
 * CP_HQD_PQ_CONTROL 0xF030890A; the other seven have 0, 0 and 0xD030890A. The one that differs is
 * compute_ring[0], exactly as the policy says. */
static bool bc250_is_high_priority_compute_queue(struct amdgpu_device *adev,
						 const struct amdgpu_ring *ring)
{
	return adev->gfx.num_compute_rings > 1 && ring == &adev->gfx.compute_ring[0];
}

/* amdgpu_ring.c:702 amdgpu_ring_to_mqd_prop(), reduced to the kernel-queue case.
 *
 * The graphics half of upstream's condition, amdgpu_gfx_is_high_priority_graphics_queue(), needs
 * adev->gfx.num_gfx_rings > 1 as well as pipe 1 queue 0. This part brings up one gfx ring, so it is
 * false here whatever the pipe is, and it is left out rather than written and never taken. If a
 * second gfx ring is ever added, it has to come back. */
static void bc250_ring_to_mqd_prop(struct amdgpu_device *adev, struct amdgpu_ring *ring,
				   struct amdgpu_mqd_prop *prop)
{
	bool is_high_prio_compute = ring->funcs->type == AMDGPU_RING_TYPE_COMPUTE &&
				    bc250_is_high_priority_compute_queue(adev, ring);

	memset(prop, 0, sizeof(*prop));

	prop->mqd_gpu_addr = ring->mqd_gpu_addr;
	prop->hqd_base_gpu_addr = ring->gpu_addr;
	prop->rptr_gpu_addr = ring->rptr_gpu_addr;
	prop->wptr_gpu_addr = ring->wptr_gpu_addr;
	prop->queue_size = ring->ring_size;
	prop->eop_gpu_addr = ring->eop_gpu_addr;
	prop->use_doorbell = ring->use_doorbell;
	prop->doorbell_index = ring->doorbell_index;
	prop->kernel_queue = true;

	/* "map_queues packet doesn't need activate the queue, so only kiq need set this field."
	 * Unit A's compute MQDs all read cp_hqd_active 1 in the dump, which is the CP writing it back
	 * after MAP_QUEUES, not the driver: the dumps were taken after the queues were running. */
	prop->hqd_active = ring->funcs->type == AMDGPU_RING_TYPE_KIQ;

	prop->allow_tunneling = is_high_prio_compute;
	if (is_high_prio_compute) {
		prop->hqd_pipe_priority = AMDGPU_GFX_PIPE_PRIO_HIGH;
		prop->hqd_queue_priority = AMDGPU_GFX_QUEUE_PRIORITY_MAXIMUM;
	}
}

/* gfx_v10_0.c:6905 gfx_v10_0_compute_mqd_init(). __BIG_ENDIAN arm left out: x64 only. */
static void bc250_compute_mqd_init(struct amdgpu_device *adev, struct v10_compute_mqd *mqd,
				   const struct amdgpu_mqd_prop *prop)
{
	uint64_t hqd_gpu_addr, wb_gpu_addr, eop_base_addr;
	uint32_t tmp;

	mqd->header = 0xC0310800;
	mqd->compute_pipelinestat_enable = 0x00000001;
	mqd->compute_static_thread_mgmt_se0 = 0xffffffff;
	mqd->compute_static_thread_mgmt_se1 = 0xffffffff;
	mqd->compute_static_thread_mgmt_se2 = 0xffffffff;
	mqd->compute_static_thread_mgmt_se3 = 0xffffffff;
	mqd->compute_misc_reserved = 0x00000003;

	eop_base_addr = prop->eop_gpu_addr >> 8;
	mqd->cp_hqd_eop_base_addr_lo = lower_32_bits(eop_base_addr);
	mqd->cp_hqd_eop_base_addr_hi = upper_32_bits(eop_base_addr);

	/* set the EOP size, register value is 2^(EOP_SIZE+1) dwords */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_EOP_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_HQD_EOP_CONTROL, EOP_SIZE,
			    (order_base_2(GFX10_MEC_HPD_SIZE / 4) - 1));
	mqd->cp_hqd_eop_control = tmp;

	/* enable doorbell? */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL);
	if (prop->use_doorbell) {
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_OFFSET,
				    prop->doorbell_index);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 1);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_SOURCE, 0);
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_HIT, 0);
	} else {
		tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_DOORBELL_CONTROL, DOORBELL_EN, 0);
	}
	mqd->cp_hqd_pq_doorbell_control = tmp;

	/* disable the queue if it's active */
	mqd->cp_hqd_dequeue_request = 0;
	mqd->cp_hqd_pq_rptr = 0;
	mqd->cp_hqd_pq_wptr_lo = 0;
	mqd->cp_hqd_pq_wptr_hi = 0;

	/* set the pointer to the MQD */
	mqd->cp_mqd_base_addr_lo = (u32)(prop->mqd_gpu_addr & 0xfffffffc);
	mqd->cp_mqd_base_addr_hi = upper_32_bits(prop->mqd_gpu_addr);

	/* set MQD vmid to 0 */
	tmp = RREG32_SOC15(GC, 0, mmCP_MQD_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_MQD_CONTROL, VMID, 0);
	mqd->cp_mqd_control = tmp;

	/* set the pointer to the HQD, this is similar CP_RB0_BASE/_HI */
	hqd_gpu_addr = prop->hqd_base_gpu_addr >> 8;
	mqd->cp_hqd_pq_base_lo = lower_32_bits(hqd_gpu_addr);
	mqd->cp_hqd_pq_base_hi = upper_32_bits(hqd_gpu_addr);

	/* set up the HQD, this is similar to CP_RB0_CNTL */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, QUEUE_SIZE,
			    (order_base_2(prop->queue_size / 4) - 1));
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, RPTR_BLOCK_SIZE,
			    (order_base_2(AMDGPU_GPU_PAGE_SIZE / 4) - 1));
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, UNORD_DISPATCH, 1);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, TUNNEL_DISPATCH, prop->allow_tunneling);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, PRIV_STATE, 1);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PQ_CONTROL, KMD_QUEUE, 1);
	mqd->cp_hqd_pq_control = tmp;

	/* set the wb address whether it's enabled or not */
	wb_gpu_addr = prop->rptr_gpu_addr;
	mqd->cp_hqd_pq_rptr_report_addr_lo = (u32)(wb_gpu_addr & 0xfffffffc);
	mqd->cp_hqd_pq_rptr_report_addr_hi = upper_32_bits(wb_gpu_addr) & 0xffff;

	/* only used if CP_PQ_WPTR_POLL_CNTL.EN = 1 */
	wb_gpu_addr = prop->wptr_gpu_addr;
	mqd->cp_hqd_pq_wptr_poll_addr_lo = (u32)(wb_gpu_addr & 0xfffffffc);
	mqd->cp_hqd_pq_wptr_poll_addr_hi = upper_32_bits(wb_gpu_addr) & 0xffff;

	/* reset read and write pointers, similar to CP_RB0_WPTR/_RPTR
	 *
	 * DECLARED DEVIATION (driver/amdgpu-import/PROVENANCE.md). Upstream is
	 *
	 *     mqd->cp_hqd_pq_rptr = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR);   gfx_v10_0.c:6998
	 *
	 * which samples the register rather than resetting it, whatever its own comment - the one
	 * kept above - says. The sample is under a GRBM selection of this queue's slot, so it picks up
	 * whatever read pointer the CP left there.
	 *
	 * That is clean only on a part that loses GFX power between loads. This one does not: on unit
	 * A's second bring-up in one boot the register held 0x737, gfx_v10_0_kiq_init_register()
	 * wrote it back (:7044, in the CP_HQD_ACTIVE branch that only a re-init enters), and the CP
	 * was told there were 262144 - 1847 = 260297 dwords of work pending against a ring that
	 * restarts at write pointer 0. It walked every one of them: stage 6 took 5222 us against
	 * 349 us cold (E12 run 001).
	 *
	 * Zero is the only value consistent with the rest of this function, which sets
	 * cp_hqd_pq_wptr_lo and _hi to 0 four lines above, and with amdgpu_ring_init_mqd()'s
	 * ring->wptr = 0. It is also what AMD themselves write from the next generation on:
	 * gfx_v11_0.c:4337 and gfx_v12_0.c:3216 use regCP_HQD_PQ_RPTR_DEFAULT, which
	 * gc_11_0_0_default.h:2091 defines as 0.
	 *
	 * A cold boot is unaffected, because there the register reads 0 and the sample was already 0.
	 */
	mqd->cp_hqd_pq_rptr = 0;

	/* set the vmid for the queue */
	mqd->cp_hqd_vmid = 0;

	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_PERSISTENT_STATE);
	tmp = REG_SET_FIELD(tmp, CP_HQD_PERSISTENT_STATE, PRELOAD_SIZE, 0x53);
	mqd->cp_hqd_persistent_state = tmp;

	/* set MIN_IB_AVAIL_SIZE */
	tmp = RREG32_SOC15(GC, 0, mmCP_HQD_IB_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_HQD_IB_CONTROL, MIN_IB_AVAIL_SIZE, 3);
	mqd->cp_hqd_ib_control = tmp;

	mqd->cp_hqd_pipe_priority = prop->hqd_pipe_priority;
	mqd->cp_hqd_queue_priority = prop->hqd_queue_priority;

	mqd->cp_hqd_active = prop->hqd_active;
}

/* gfx_v10_0.c:6743 gfx_v10_0_gfx_mqd_set_priority(), with priority always low here. */
static void bc250_gfx_mqd_set_priority(struct amdgpu_device *adev, struct v10_gfx_mqd *mqd)
{
	u32 tmp;

	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_QUEUE_PRIORITY);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_QUEUE_PRIORITY, PRIORITY_LEVEL, 0);
	mqd->cp_gfx_hqd_queue_priority = tmp;
}

/* gfx_v10_0.c:6761 gfx_v10_0_gfx_mqd_init() */
static void bc250_gfx_mqd_init(struct amdgpu_device *adev, struct v10_gfx_mqd *mqd,
			       const struct amdgpu_mqd_prop *prop)
{
	uint64_t hqd_gpu_addr, wb_gpu_addr;
	uint32_t tmp;
	uint32_t rb_bufsz;

	/* set up gfx hqd wptr */
	mqd->cp_gfx_hqd_wptr = 0;
	mqd->cp_gfx_hqd_wptr_hi = 0;

	/* set the pointer to the MQD */
	mqd->cp_mqd_base_addr = (u32)(prop->mqd_gpu_addr & 0xfffffffc);
	mqd->cp_mqd_base_addr_hi = upper_32_bits(prop->mqd_gpu_addr);

	/* set up mqd control */
	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_MQD_CONTROL);
	tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, VMID, 0);
	tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, PRIV_STATE, 1);
	tmp = REG_SET_FIELD(tmp, CP_GFX_MQD_CONTROL, CACHE_POLICY, 0);
	mqd->cp_gfx_mqd_control = tmp;

	/* set up gfx_hqd_vmid with 0x0 to indicate the ring buffer's vmid */
	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_VMID);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_VMID, VMID, 0);
	mqd->cp_gfx_hqd_vmid = 0;

	bc250_gfx_mqd_set_priority(adev, mqd);

	/* set up time quantum */
	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_QUANTUM);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_QUANTUM, QUANTUM_EN, 1);
	mqd->cp_gfx_hqd_quantum = tmp;

	/* set up gfx hqd base. this is similar as CP_RB_BASE */
	hqd_gpu_addr = prop->hqd_base_gpu_addr >> 8;
	mqd->cp_gfx_hqd_base = lower_32_bits(hqd_gpu_addr);
	mqd->cp_gfx_hqd_base_hi = upper_32_bits(hqd_gpu_addr);

	/* set up hqd_rptr_addr/_hi, similar as CP_RB_RPTR */
	wb_gpu_addr = prop->rptr_gpu_addr;
	mqd->cp_gfx_hqd_rptr_addr = (u32)(wb_gpu_addr & 0xfffffffc);
	mqd->cp_gfx_hqd_rptr_addr_hi = upper_32_bits(wb_gpu_addr) & 0xffff;

	/* set up rb_wptr_poll addr */
	wb_gpu_addr = prop->wptr_gpu_addr;
	mqd->cp_rb_wptr_poll_addr_lo = (u32)(wb_gpu_addr & 0xfffffffc);
	mqd->cp_rb_wptr_poll_addr_hi = upper_32_bits(wb_gpu_addr) & 0xffff;

	/* set up the gfx_hqd_control, similar as CP_RB0_CNTL */
	rb_bufsz = order_base_2(prop->queue_size / 4) - 1;
	tmp = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_CNTL);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BUFSZ, rb_bufsz);
	tmp = REG_SET_FIELD(tmp, CP_GFX_HQD_CNTL, RB_BLKSZ, rb_bufsz - 2);
	mqd->cp_gfx_hqd_cntl = tmp;

	/* set up cp_doorbell_control */
	tmp = RREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL);
	if (prop->use_doorbell) {
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET,
				    prop->doorbell_index);
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
	} else {
		tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
	}
	mqd->cp_rb_doorbell_control = tmp;

	/* reset read and write pointers, similar to CP_RB0_WPTR/_RPTR */
	mqd->cp_gfx_hqd_rptr = RREG32_SOC15(GC, 0, mmCP_GFX_HQD_RPTR);

	/* active the queue */
	mqd->cp_gfx_hqd_active = 1;
}

/* gfx_v10_0.c:6714 gfx_v10_0_kiq_setting(), default branch. One write, with 0x80 already set:
 * the trace has exactly one RLC_CP_SCHEDULERS write, 0x58504840 -> 0x585048C8, which is
 * (me 2 << 5) | (pipe 1 << 3) | queue 0 | 0x80. */
static void bc250_kiq_setting(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t tmp;

	tmp = RREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS);
	tmp &= 0xffffff00;
	tmp |= (ring->me << 5) | (ring->pipe << 3) | (ring->queue);
	WREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS, tmp | 0x80);
}

/* gfx_v10_0.c:7021 gfx_v10_0_kiq_init_register(). The SR-IOV "inactivate the queue" write at the
 * top is not taken. */
static int bc250_kiq_init_register(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_compute_mqd *mqd = (struct v10_compute_mqd *)ring->mqd_ptr;
	u32 j;

	/* disable wptr polling */
	WREG32_FIELD15(GC, 0, CP_PQ_WPTR_POLL_CNTL, EN, 0);

	/* disable the queue if it's active.
	 *
	 * DECLARED DEVIATION (driver/amdgpu-import/PROVENANCE.md). Upstream always asks the CP to
	 * dequeue and then polls CP_HQD_ACTIVE until it clears. That handshake needs a running MEC, and
	 * on the path this driver has to support there is none: bc250_gfx_hw_fini() halts the MEC, and
	 * nothing unhalts it before here, because gfx_v10_0_cp_compute_enable(adev, true) runs from
	 * kcq_resume (gfx_v10_0.c:7208, called at :7243) one step AFTER kiq_resume (:7239). Upstream
	 * does not notice, because it never looks at the poll's outcome - j is unused after the loop at
	 * :7037-7041 - and on the parts it exercises GFX power is dropped across suspend or a reset
	 * intervenes, so CP_HQD_ACTIVE reads 0 and this branch is never taken at all. This part keeps
	 * GFX powered, so it lands in the case upstream has no mechanism for.
	 *
	 * So: read CP_MEC_CNTL, and if the engine that would service the request is halted, take
	 * upstream's own "inactivate the queue" write instead - gfx_v10_0.c:7028-7029, which upstream
	 * reserves for SR-IOV - on a measured condition rather than on amdgpu_sriov_vf(). It is safe
	 * precisely because the MEC is halted: there is no running CP whose state could be left
	 * inconsistent, and the whole HQD is reprogrammed and re-activated a few lines below.
	 *
	 * The CP_HQD_DEQUEUE_REQUEST restore stays in the live-CP arm only. It exists to undo the
	 * request this function made; where no request was made there is nothing to undo, and leaving
	 * it alone puts the register in exactly the state the first, trace-matching bring-up leaves it
	 * in - it is the one register in this branch no trace window names, so not writing it keeps a
	 * second bring-up inside the miniport's generated table.
	 *
	 * On a cold boot none of this happens: CP_HQD_ACTIVE reads 0 and the branch is skipped, which
	 * is why the 354-write comparison against unit A is unaffected either way. */
	if (RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1) {
		bool mec_halted = (RREG32_SOC15(GC, 0, mmCP_MEC_CNTL) &
				   (CP_MEC_CNTL__MEC_ME1_HALT_MASK |
				    CP_MEC_CNTL__MEC_ME2_HALT_MASK)) != 0;
		bool timed_out = false;

		if (mec_halted) {
			WREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE, 0);
		} else {
			WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST, 1);
			for (j = 0; j < adev->usec_timeout; j++) {
				if (!(RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1))
					break;
				bc250_shim_udelay(1);
			}
			timed_out = j >= adev->usec_timeout;
			WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST,
				     mqd->cp_hqd_dequeue_request);
		}

		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR, mqd->cp_hqd_pq_rptr);
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO, mqd->cp_hqd_pq_wptr_lo);
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_HI, mqd->cp_hqd_pq_wptr_hi);

		/* Upstream carries on regardless; a dequeue that never completed on a LIVE CP means the
		 * queue is still running and reprogramming it underneath would be worse than stopping. */
		if (timed_out)
			return BC250_ETIME;
	}

	/* disable doorbells */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL, 0);

	/* write the EOP addr */
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_BASE_ADDR, mqd->cp_hqd_eop_base_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_BASE_ADDR_HI, mqd->cp_hqd_eop_base_addr_hi);

	/* set the EOP size, register value is 2^(EOP_SIZE+1) dwords */
	WREG32_SOC15(GC, 0, mmCP_HQD_EOP_CONTROL, mqd->cp_hqd_eop_control);

	/* set the pointer to the MQD */
	WREG32_SOC15(GC, 0, mmCP_MQD_BASE_ADDR, mqd->cp_mqd_base_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_MQD_BASE_ADDR_HI, mqd->cp_mqd_base_addr_hi);

	/* set MQD vmid to 0 */
	WREG32_SOC15(GC, 0, mmCP_MQD_CONTROL, mqd->cp_mqd_control);

	/* set the pointer to the HQD, this is similar CP_RB0_BASE/_HI */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_BASE, mqd->cp_hqd_pq_base_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_BASE_HI, mqd->cp_hqd_pq_base_hi);

	/* set up the HQD, this is similar to CP_RB0_CNTL */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_CONTROL, mqd->cp_hqd_pq_control);

	/* set the wb address whether it's enabled or not */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR_REPORT_ADDR, mqd->cp_hqd_pq_rptr_report_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI, mqd->cp_hqd_pq_rptr_report_addr_hi);

	/* only used if CP_PQ_WPTR_POLL_CNTL.EN = 1 */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_POLL_ADDR, mqd->cp_hqd_pq_wptr_poll_addr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_POLL_ADDR_HI, mqd->cp_hqd_pq_wptr_poll_addr_hi);

	/* enable the doorbell if requested */
	if (ring->use_doorbell) {
		WREG32_SOC15(GC, 0, mmCP_MEC_DOORBELL_RANGE_LOWER,
			     (adev->doorbell_index.kiq * 2) << 2);
		WREG32_SOC15(GC, 0, mmCP_MEC_DOORBELL_RANGE_UPPER,
			     (adev->doorbell_index.userqueue_end * 2) << 2);
	}

	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL, mqd->cp_hqd_pq_doorbell_control);

	/* reset read and write pointers, similar to CP_RB0_WPTR/_RPTR */
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO, mqd->cp_hqd_pq_wptr_lo);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_HI, mqd->cp_hqd_pq_wptr_hi);

	/* set the vmid for the queue */
	WREG32_SOC15(GC, 0, mmCP_HQD_VMID, mqd->cp_hqd_vmid);

	WREG32_SOC15(GC, 0, mmCP_HQD_PERSISTENT_STATE, mqd->cp_hqd_persistent_state);

	/* activate the queue */
	WREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE, mqd->cp_hqd_active);

	if (ring->use_doorbell)
		WREG32_FIELD15(GC, 0, CP_PQ_STATUS, DOORBELL_ENABLE, 1);

	return 0;
}

/* gfx_v10_0.c:7130 gfx_v10_0_kiq_init_queue(), cold-boot arm (not in reset, not SR-IOV). */
static int bc250_kiq_init_queue(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_mqd_prop prop;
	int r;

	bc250_kiq_setting(ring);

	memset(ring->mqd_ptr, 0, sizeof(struct v10_compute_mqd));
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	/* amdgpu_ring.c amdgpu_ring_init_mqd(): reset the write pointer, then build the MQD. */
	bc250_ring_to_mqd_prop(adev, ring, &prop);
	ring->wptr = 0;
	bc250_compute_mqd_init(adev, (struct v10_compute_mqd *)ring->mqd_ptr, &prop);

	r = bc250_kiq_init_register(ring);
	nv_grbm_select(adev, 0, 0, 0, 0);

	return r;
}

/* gfx_v10_0.c:7169 gfx_v10_0_kcq_init_queue(), cold-boot arm. No HQD registers: the KIQ's
 * MAP_QUEUES packet programs them from the MQD. */
static int bc250_kcq_init_queue(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_mqd_prop prop;

	memset(ring->mqd_ptr, 0, sizeof(struct v10_compute_mqd));
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	bc250_ring_to_mqd_prop(adev, ring, &prop);
	ring->wptr = 0;
	bc250_compute_mqd_init(adev, (struct v10_compute_mqd *)ring->mqd_ptr, &prop);

	nv_grbm_select(adev, 0, 0, 0, 0);
	return 0;
}

/* gfx_v10_0.c:6601 gfx_v10_0_cp_compute_enable(), default (non Sienna Cichlid) branch. */
static void bc250_cp_compute_enable(struct amdgpu_device *adev, bool enable)
{
	if (enable)
		WREG32_SOC15(GC, 0, mmCP_MEC_CNTL, 0);
	else
		WREG32_SOC15(GC, 0, mmCP_MEC_CNTL,
			     (CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK));
	bc250_shim_udelay(50);
}

/* gfx_v10_0.c:6071 gfx_v10_0_cp_gfx_enable(), default branch (the 10.1.2 RLC write is not ours). */
static int bc250_cp_gfx_enable(struct amdgpu_device *adev, bool enable)
{
	u32 i;
	u32 tmp = RREG32_SOC15(GC, 0, mmCP_ME_CNTL);

	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, ME_HALT, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, PFP_HALT, enable ? 0 : 1);
	tmp = REG_SET_FIELD(tmp, CP_ME_CNTL, CE_HALT, enable ? 0 : 1);

	WREG32_SOC15(GC, 0, mmCP_ME_CNTL, tmp);

	for (i = 0; i < adev->usec_timeout; i++) {
		if (RREG32_SOC15(GC, 0, mmCP_STAT) == 0)
			break;
		bc250_shim_udelay(1);
	}

	if (i >= adev->usec_timeout)
		dev_err(adev->dev, "failed to %s cp gfx\n", enable ? "unhalt" : "halt");

	/* Upstream returns 0 regardless, and the caller carries on. Kept, with the message. */
	return 0;
}

/* gfx_v10_0.c:6453 gfx_v10_0_cp_gfx_set_doorbell(), default branch. The !amdgpu_async_gfx_ring
 * block at the top writes CP_RB_DOORBELL_CONTROL; on unit A async_gfx_ring is on, and the trace
 * confirms it - there is no CP_RB_DOORBELL_CONTROL write in the window. */
static void bc250_cp_gfx_set_doorbell(struct amdgpu_device *adev, struct amdgpu_ring *ring,
				      bool async_gfx_ring)
{
	u32 tmp;

	if (!async_gfx_ring) {
		tmp = RREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL);
		if (ring->use_doorbell) {
			tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_OFFSET,
					    ring->doorbell_index);
			tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 1);
		} else {
			tmp = REG_SET_FIELD(tmp, CP_RB_DOORBELL_CONTROL, DOORBELL_EN, 0);
		}
		WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_CONTROL, tmp);
	}

	tmp = REG_SET_FIELD(0, CP_RB_DOORBELL_RANGE_LOWER, DOORBELL_RANGE_LOWER,
			    ring->doorbell_index);
	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_RANGE_LOWER, tmp);

	WREG32_SOC15(GC, 0, mmCP_RB_DOORBELL_RANGE_UPPER,
		     CP_RB_DOORBELL_RANGE_UPPER__DOORBELL_RANGE_UPPER_MASK);
}

/* gfx_v10_0.c:6844 gfx_v10_0_kgq_init_queue(), cold-boot arm. */
static int bc250_kgq_init_queue(struct amdgpu_ring *ring, bool async_gfx_ring)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_mqd_prop prop;

	memset(ring->mqd_ptr, 0, sizeof(struct v10_gfx_mqd));
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	bc250_ring_to_mqd_prop(adev, ring, &prop);
	ring->wptr = 0;
	bc250_gfx_mqd_init(adev, (struct v10_gfx_mqd *)ring->mqd_ptr, &prop);

	if (ring->doorbell_index == adev->doorbell_index.gfx_ring0 << 1)
		bc250_cp_gfx_set_doorbell(adev, ring, async_gfx_ring);

	nv_grbm_select(adev, 0, 0, 0, 0);
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * KIQ PM4, gfx_v10_0.c:3707 gfx10_kiq_set_resources() and :3726 gfx10_kiq_map_queues()
 * ------------------------------------------------------------------------------------------- */

static void bc250_kiq_set_resources(struct amdgpu_ring *kiq_ring, uint64_t queue_mask)
{
	/* Cleaner shader MC address. gfx10 has no cleaner shader
	 * (adev->gfx.cleaner_shader_size is 0 for this IP), so the address is 0, which is what
	 * upstream writes too. */
	u64 shader_mc_addr = 0;

	amdgpu_ring_write(kiq_ring, PACKET3(PACKET3_SET_RESOURCES, 6));
	amdgpu_ring_write(kiq_ring, PACKET3_SET_RESOURCES_VMID_MASK(0) |
			  PACKET3_SET_RESOURCES_QUEUE_TYPE(0)); /* vmid_mask:0 queue_type:0 (KIQ) */
	amdgpu_ring_write(kiq_ring, lower_32_bits(queue_mask));	/* queue mask lo */
	amdgpu_ring_write(kiq_ring, upper_32_bits(queue_mask));	/* queue mask hi */
	amdgpu_ring_write(kiq_ring, lower_32_bits(shader_mc_addr)); /* cleaner shader addr lo */
	amdgpu_ring_write(kiq_ring, upper_32_bits(shader_mc_addr)); /* cleaner shader addr hi */
	amdgpu_ring_write(kiq_ring, 0);	/* oac mask */
	amdgpu_ring_write(kiq_ring, 0);	/* gds heap base:0, gds heap size:0 */
}

static void bc250_kiq_map_queues(struct amdgpu_ring *kiq_ring, struct amdgpu_ring *ring)
{
	uint64_t mqd_addr = ring->mqd_gpu_addr;
	uint64_t wptr_addr = ring->wptr_gpu_addr;
	uint32_t eng_sel = 0;

	switch (ring->funcs->type) {
	case AMDGPU_RING_TYPE_COMPUTE:
		eng_sel = 0;
		break;
	case AMDGPU_RING_TYPE_GFX:
		eng_sel = 4;
		break;
	default:
		dev_err(ring->adev->dev, "map_queues on a ring type that cannot be mapped\n");
		break;
	}

	amdgpu_ring_write(kiq_ring, PACKET3(PACKET3_MAP_QUEUES, 5));
	/* Q_sel:0, vmid:0, vidmem: 1, engine:0, num_Q:1 */
	amdgpu_ring_write(kiq_ring,
			  PACKET3_MAP_QUEUES_QUEUE_SEL(0) |
			  PACKET3_MAP_QUEUES_VMID(0) |
			  PACKET3_MAP_QUEUES_QUEUE(ring->queue) |
			  PACKET3_MAP_QUEUES_PIPE(ring->pipe) |
			  PACKET3_MAP_QUEUES_ME((ring->me == 1 ? 0 : 1)) |
			  PACKET3_MAP_QUEUES_QUEUE_TYPE(0) |
			  PACKET3_MAP_QUEUES_ALLOC_FORMAT(0) |
			  PACKET3_MAP_QUEUES_ENGINE_SEL(eng_sel) |
			  PACKET3_MAP_QUEUES_NUM_QUEUES(1));
	amdgpu_ring_write(kiq_ring, PACKET3_MAP_QUEUES_DOORBELL_OFFSET(ring->doorbell_index));
	amdgpu_ring_write(kiq_ring, lower_32_bits(mqd_addr));
	amdgpu_ring_write(kiq_ring, upper_32_bits(mqd_addr));
	amdgpu_ring_write(kiq_ring, lower_32_bits(wptr_addr));
	amdgpu_ring_write(kiq_ring, upper_32_bits(wptr_addr));
}

/* gfx_v10_0.c:3766 gfx10_kiq_unmap_queues(), the RESET_QUEUES action only.
 *
 * Upstream's other action, PREEMPT_QUEUES_NO_UNMAP, carries a fence address and a sequence number so
 * that the caller can wait for the preemption; it belongs to the scheduler's preempt path, which
 * does not exist here. RESET_QUEUES takes three zero dwords in those slots, and that is what this
 * writes, so the packet is the same length either way. */
static void bc250_kiq_unmap_queues(struct amdgpu_ring *kiq_ring, struct amdgpu_ring *ring)
{
	uint32_t eng_sel = ring->funcs->type == AMDGPU_RING_TYPE_GFX ? 4 : 0;

	amdgpu_ring_write(kiq_ring, PACKET3(PACKET3_UNMAP_QUEUES, 4));
	/* Q_sel: 0, vmid: 0, engine: 0, num_Q: 1 */
	amdgpu_ring_write(kiq_ring,
			  PACKET3_UNMAP_QUEUES_ACTION(BC250_RESET_QUEUES) |
			  PACKET3_UNMAP_QUEUES_QUEUE_SEL(0) |
			  PACKET3_UNMAP_QUEUES_ENGINE_SEL(eng_sel) |
			  PACKET3_UNMAP_QUEUES_NUM_QUEUES(1));
	amdgpu_ring_write(kiq_ring,
			  PACKET3_UNMAP_QUEUES_DOORBELL_OFFSET0(ring->doorbell_index));
	amdgpu_ring_write(kiq_ring, 0);
	amdgpu_ring_write(kiq_ring, 0);
	amdgpu_ring_write(kiq_ring, 0);
}

/* gfx_v10_0.c:3873 gfx_v10_0_kiq_pm4_funcs: set_resources_size = 8, map_queues_size = 7,
 * unmap_queues_size = 6. */
#define BC250_SET_RESOURCES_SIZE	8u
#define BC250_MAP_QUEUES_SIZE		7u
#define BC250_UNMAP_QUEUES_SIZE		6u

/* gfx_v10_0.c:4033 gfx_v10_0_ring_test_ring() */
int bc250_gfx_ring_test(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev;
	uint32_t scratch;
	uint32_t tmp;
	unsigned int i;
	int r;

	if (ring == NULL || ring->adev == NULL)
		return BC250_EINVAL;
	adev = ring->adev;
	scratch = SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0);

	WREG32(scratch, 0xCAFEDEAD);
	r = amdgpu_ring_alloc(ring, 3);
	if (r) {
		dev_err(adev->dev, "cp failed to lock ring (%d)\n", r);
		return r;
	}

	amdgpu_ring_write(ring, PACKET3(PACKET3_SET_UCONFIG_REG, 1));
	amdgpu_ring_write(ring, scratch - PACKET3_SET_UCONFIG_REG_START);
	amdgpu_ring_write(ring, 0xDEADBEEF);
	amdgpu_ring_commit(ring);

	for (i = 0; i < adev->usec_timeout; i++) {
		tmp = RREG32(scratch);
		if (tmp == 0xDEADBEEF)
			break;
		bc250_shim_udelay(1);
	}

	if (i >= adev->usec_timeout)
		return BC250_ETIME;

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The fence: the smallest thing that ends in an end-of-pipe interrupt
 *
 * gfx_v10_0.c:8712 gfx_v10_0_ring_emit_fence(), used by both the gfx ring funcs (:9863) and the
 * compute ring funcs (:9908), and gfx_v10_0.c:8780 gfx_v10_0_ring_emit_fence_kiq(), used by the KIQ
 * ring funcs (:9945). Which one a ring gets is ring->funcs->emit_fence upstream; here it is one
 * function that branches on ring->funcs->type, because the shim's amdgpu_ring_funcs carries no
 * callbacks (see amdgpu.h).
 *
 * Deviations, all of the same kind - upstream stops the machine where the shim refuses:
 *   - upstream BUG_ON()s a misaligned address and a 64-bit KIQ fence. A BUG() in a Windows miniport
 *     is a bugcheck on a caller's mistake, so each becomes BC250_EINVAL with nothing written. The
 *     conditions are upstream's, unchanged.
 *   - upstream's emitters return void and are called inside an amdgpu_ring_alloc() the fence driver
 *     already did. bc250_gfx_emit_fence() keeps that shape; bc250_gfx_signal_fence() is the alloc,
 *     emit and commit together, which is what the miniport actually wants and what makes the
 *     dword count a thing the code computes rather than the caller guesses.
 * ------------------------------------------------------------------------------------------- */

unsigned int bc250_gfx_fence_size(const struct amdgpu_ring *ring, unsigned int flags)
{
	if (ring == NULL || ring->funcs == NULL)
		return 0;

	/* KIQ: one WRITE_DATA of 5 dwords, and a second one only if an interrupt was asked for. */
	if (ring->funcs->type == AMDGPU_RING_TYPE_KIQ)
		return (flags & AMDGPU_FENCE_FLAG_INT) ? 10u : 5u;

	/* gfx and compute: PACKET3(PACKET3_RELEASE_MEM, 6), one header and seven body dwords. */
	return 8u;
}

int bc250_gfx_emit_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags)
{
	bool write64bit = (flags & AMDGPU_FENCE_FLAG_64BIT) != 0;
	bool int_sel = (flags & AMDGPU_FENCE_FLAG_INT) != 0;
	struct amdgpu_device *adev;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL)
		return BC250_EINVAL;
	adev = ring->adev;      /* SOC15_REG_OFFSET() resolves through it, as upstream's does */

	if (ring->funcs->type == AMDGPU_RING_TYPE_KIQ) {
		/* gfx_v10_0.c:8786 "we only allocate 32bit for each seq wb address" */
		if (write64bit)
			return BC250_EINVAL;
		if ((addr & 0x3u) != 0)
			return BC250_EINVAL;

		amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
		amdgpu_ring_write(ring, (WRITE_DATA_ENGINE_SEL(0) |
					 WRITE_DATA_DST_SEL(5) | WR_CONFIRM));
		amdgpu_ring_write(ring, lower_32_bits(addr));
		amdgpu_ring_write(ring, upper_32_bits(addr));
		amdgpu_ring_write(ring, lower_32_bits(seq));

		if (int_sel) {
			/* The KIQ has no end-of-pipe of its own: it raises its interrupt by writing
			 * CPC_INT_STATUS, and the comment upstream leaves on the value - "src_id is
			 * 178" - is why bc250_ih_is_kiq() matches CP_IB2_INTERRUPT_PKT, which is 178.
			 * The register is named through AMD's headers, not by its address. */
			amdgpu_ring_write(ring, PACKET3(PACKET3_WRITE_DATA, 3));
			amdgpu_ring_write(ring, (WRITE_DATA_ENGINE_SEL(0) |
						 WRITE_DATA_DST_SEL(0) | WR_CONFIRM));
			amdgpu_ring_write(ring, SOC15_REG_OFFSET(GC, 0, mmCPC_INT_STATUS));
			amdgpu_ring_write(ring, 0);
			amdgpu_ring_write(ring, 0x20000000);
		}
		return 0;
	}

	/* The alignment upstream BUG_ON()s, checked before a single dword is written so that a
	 * refusal leaves the ring exactly as it was. */
	if (write64bit) {
		if ((addr & 0x7u) != 0)
			return BC250_EINVAL;
	} else {
		if ((addr & 0x3u) != 0)
			return BC250_EINVAL;
	}

	/* RELEASE_MEM - flush caches, send int */
	amdgpu_ring_write(ring, PACKET3(PACKET3_RELEASE_MEM, 6));
	amdgpu_ring_write(ring, (PACKET3_RELEASE_MEM_GCR_SEQ |
				 PACKET3_RELEASE_MEM_GCR_GL2_WB |
				 PACKET3_RELEASE_MEM_GCR_GLM_INV | /* must be set with GLM_WB */
				 PACKET3_RELEASE_MEM_GCR_GLM_WB |
				 PACKET3_RELEASE_MEM_CACHE_POLICY(3) |
				 PACKET3_RELEASE_MEM_EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |
				 PACKET3_RELEASE_MEM_EVENT_INDEX(5)));
	amdgpu_ring_write(ring, (PACKET3_RELEASE_MEM_DATA_SEL(write64bit ? 2 : 1) |
				 PACKET3_RELEASE_MEM_INT_SEL(int_sel ? 2 : 0)));
	amdgpu_ring_write(ring, lower_32_bits(addr));
	amdgpu_ring_write(ring, upper_32_bits(addr));
	amdgpu_ring_write(ring, lower_32_bits(seq));
	amdgpu_ring_write(ring, upper_32_bits(seq));
	amdgpu_ring_write(ring, 0);

	return 0;
}

int bc250_gfx_signal_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags)
{
	unsigned int ndw = bc250_gfx_fence_size(ring, flags);
	int r;

	if (ndw == 0)
		return BC250_EINVAL;

	r = amdgpu_ring_alloc(ring, ndw);
	if (r)
		return r;

	r = bc250_gfx_emit_fence(ring, addr, seq, flags);
	if (r) {
		amdgpu_ring_undo(ring);
		return r;
	}

	amdgpu_ring_commit(ring);
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The fence slots
 *
 * Upstream takes one writeback slot per ring out of adev->wb (amdgpu_device_wb_get(),
 * amdgpu_fence_driver_init_ring()). There is no such pool here, so this is one GTT page cut into
 * 8-byte slots - 8 and not 4 so that a 64-bit RELEASE_MEM is legal in every slot.
 * ------------------------------------------------------------------------------------------- */

int bc250_gfx_fence_page_alloc(struct amdgpu_device *adev)
{
	int r;

	if (adev == NULL)
		return BC250_EINVAL;
	if (adev->gfx.fence_mem.cpu != NULL)
		return 0;                               /* already there; idempotent */

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, AMDGPU_GPU_PAGE_SIZE, AMDGPU_GPU_PAGE_SIZE,
				 &adev->gfx.fence_mem);
	if (r)
		return r;
	if (adev->gfx.fence_mem.cpu == NULL)
		return BC250_EINVAL;
	return 0;
}

void bc250_gfx_fence_page_free(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return;
	bc250_shim_mem_free(adev, &adev->gfx.fence_mem);
}

u64 bc250_gfx_fence_addr(struct amdgpu_device *adev, unsigned int slot)
{
	if (adev == NULL || adev->gfx.fence_mem.cpu == NULL || slot >= BC250_GFX_FENCE_SLOTS)
		return 0;
	return adev->gfx.fence_mem.mc + (u64)slot * 8u;
}

u64 bc250_gfx_fence_read(struct amdgpu_device *adev, unsigned int slot)
{
	const volatile u64 *p;

	if (adev == NULL || adev->gfx.fence_mem.cpu == NULL || slot >= BC250_GFX_FENCE_SLOTS)
		return 0;
	p = (const volatile u64 *)adev->gfx.fence_mem.cpu;
	return p[slot];
}

/* amdgpu_gfx.c:656 amdgpu_gfx_enable_kcq(), without MES, without the HDP flush (which returns
 * early for an APU: amdgpu_device_flush_hdp() does nothing when adev->flags & AMD_IS_APU) and
 * without the spinlock. */
static int bc250_enable_kcq(struct amdgpu_device *adev)
{
	struct amdgpu_ring *kiq_ring = &adev->gfx.kiq[0].ring;
	uint64_t queue_mask = 0;
	u32 i;
	int r;

	/* amdgpu_queue_mask_bit_to_set_resource_bit(): set_resource_bit = mec * 32 + pipe * 8 +
	 * queue, and the queue bitmap is indexed the same way, so the mask is the bitmap. */
	queue_mask = adev->gfx.mec_queue_bitmap;

	r = amdgpu_ring_alloc(kiq_ring, BC250_MAP_QUEUES_SIZE * adev->gfx.num_compute_rings +
					BC250_SET_RESOURCES_SIZE);
	if (r) {
		dev_err(adev->dev, "failed to lock KIQ (%d)\n", r);
		return r;
	}

	bc250_kiq_set_resources(kiq_ring, queue_mask);
	for (i = 0; i < adev->gfx.num_compute_rings; i++)
		bc250_kiq_map_queues(kiq_ring, &adev->gfx.compute_ring[i]);

	amdgpu_ring_commit(kiq_ring);

	r = bc250_gfx_ring_test(kiq_ring);
	if (r)
		dev_err(adev->dev, "KCQ enable failed\n");
	return r;
}

/* amdgpu_gfx.c:720 amdgpu_gfx_enable_kgq(). No SET_RESOURCES here, only MAP_QUEUES with
 * eng_sel = 4. */
static int bc250_enable_kgq(struct amdgpu_device *adev)
{
	struct amdgpu_ring *kiq_ring = &adev->gfx.kiq[0].ring;
	u32 i;
	int r;

	r = amdgpu_ring_alloc(kiq_ring, BC250_MAP_QUEUES_SIZE * adev->gfx.num_gfx_rings);
	if (r) {
		dev_err(adev->dev, "failed to lock KIQ (%d)\n", r);
		return r;
	}

	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		bc250_kiq_map_queues(kiq_ring, &adev->gfx.gfx_ring[i]);

	amdgpu_ring_commit(kiq_ring);

	r = bc250_gfx_ring_test(kiq_ring);
	if (r)
		dev_err(adev->dev, "KGQ enable failed\n");
	return r;
}

/* gfx_v10_0.c:6362 gfx_v10_0_cp_gfx_start(). The second gfx ring does not exist on this part
 * (num_gfx_rings is 1), so its CLEAR_STATE submission is left out. */
static int bc250_cp_gfx_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	const struct cs_section_def *sect;
	const struct cs_extent_def *ext;
	int r;
	u32 i;   /* upstream `int i`, see bc250_get_csb_buffer() */
	int ctx_reg_offset;

	/* init the CP */
	WREG32_SOC15(GC, 0, mmCP_MAX_CONTEXT, adev->gfx.config.max_hw_contexts - 1);
	WREG32_SOC15(GC, 0, mmCP_DEVICE_ID, 1);

	bc250_cp_gfx_enable(adev, true);

	ring = &adev->gfx.gfx_ring[0];
	r = amdgpu_ring_alloc(ring, bc250_get_csb_size(adev) + 4);
	if (r) {
		dev_err(adev->dev, "cp failed to lock ring (%d)\n", r);
		return r;
	}

	amdgpu_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	amdgpu_ring_write(ring, PACKET3_PREAMBLE_BEGIN_CLEAR_STATE);

	amdgpu_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	amdgpu_ring_write(ring, 0x80000000);
	amdgpu_ring_write(ring, 0x80000000);

	for (sect = adev->gfx.rlc.cs_data; sect->section != NULL; ++sect) {
		for (ext = sect->section; ext->extent != NULL; ++ext) {
			if (sect->id == SECT_CONTEXT) {
				amdgpu_ring_write(ring,
						  PACKET3(PACKET3_SET_CONTEXT_REG, ext->reg_count));
				amdgpu_ring_write(ring, ext->reg_index -
						  PACKET3_SET_CONTEXT_REG_START);
				for (i = 0; i < ext->reg_count; i++)
					amdgpu_ring_write(ring, ext->extent[i]);
			}
		}
	}

	ctx_reg_offset = (int)(SOC15_REG_OFFSET(GC, 0, mmPA_SC_TILE_STEERING_OVERRIDE) -
			       PACKET3_SET_CONTEXT_REG_START);
	amdgpu_ring_write(ring, PACKET3(PACKET3_SET_CONTEXT_REG, 1));
	amdgpu_ring_write(ring, (u32)ctx_reg_offset);
	amdgpu_ring_write(ring, adev->gfx.config.pa_sc_tile_steering_override);

	amdgpu_ring_write(ring, PACKET3(PACKET3_PREAMBLE_CNTL, 0));
	amdgpu_ring_write(ring, PACKET3_PREAMBLE_END_CLEAR_STATE);

	amdgpu_ring_write(ring, PACKET3(PACKET3_CLEAR_STATE, 0));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_write(ring, PACKET3(PACKET3_SET_BASE, 2));
	amdgpu_ring_write(ring, PACKET3_BASE_INDEX(CE_PARTITION_BASE));
	amdgpu_ring_write(ring, 0x8000);
	amdgpu_ring_write(ring, 0x8000);

	amdgpu_ring_commit(ring);
	return 0;
}

/* gfx_v10_0.c:7220 gfx_v10_0_cp_resume().
 *
 * Left out, with reasons: gfx_v10_0_enable_gui_idle_interrupt() runs only when the part is not an
 * APU, and this one is; the two microcode loaders are DIRECT only; gfx_v10_0_cp_gfx_resume() is the
 * !async_gfx_ring path. */
int bc250_gfx_cp_resume(struct amdgpu_device *adev)
{
	u32 i;
	int r;

	if (adev == NULL || adev->usec_timeout == 0)
		return BC250_EINVAL;      /* the three polls below; see bc250_gfx_setup() */

	/* gfx_v10_0_kiq_resume() -> gfx_v10_0_kiq_init_queue() */
	r = bc250_kiq_init_queue(&adev->gfx.kiq[0].ring);
	if (r)
		return r;

	/* gfx_v10_0_kcq_resume() */
	bc250_cp_compute_enable(adev, true);
	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		r = bc250_kcq_init_queue(&adev->gfx.compute_ring[i]);
		if (r)
			return r;
	}
	r = bc250_enable_kcq(adev);
	if (r)
		return r;

	/* gfx_v10_0_cp_async_gfx_ring_resume() */
	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		r = bc250_kgq_init_queue(&adev->gfx.gfx_ring[i], adev->gfx.async_gfx_ring);
		if (r)
			return r;
	}
	r = bc250_enable_kgq(adev);
	if (r)
		return r;
	r = bc250_cp_gfx_start(adev);
	if (r)
		return r;

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		r = bc250_gfx_ring_test(&adev->gfx.gfx_ring[i]);
		if (r)
			return r;
	}
	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		r = bc250_gfx_ring_test(&adev->gfx.compute_ring[i]);
		if (r)
			return r;
	}

	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * Setup and teardown
 * ------------------------------------------------------------------------------------------- */

static int bc250_ring_alloc_mem(struct amdgpu_device *adev, struct amdgpu_ring *ring,
				u32 ring_size, u32 mqd_size, u32 eop_size)
{
	int r;

	/* Every early return below leaves ring->ring NULL, so that a ring whose allocation did not
	 * finish cannot be written to: bc250_gfx_unmap_queues() and the commit path both test it. */
	ring->ring = NULL;

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, ring_size, AMDGPU_GPU_PAGE_SIZE,
				 &ring->ring_mem);
	if (r)
		return r;
	if (ring->ring_mem.cpu == NULL)
		return BC250_EINVAL;
	ring->ring = (u32 *)ring->ring_mem.cpu;
	ring->gpu_addr = ring->ring_mem.mc;
	ring->ring_size = ring_size;
	ring->buf_mask = (ring_size / 4u) - 1u;
	ring->ptr_mask = 0xffffffffffffffffULL;  /* gfx10 rings support 64 bit pointers */
	ring->max_dw = BC250_RING_MAX_DW;

	/* The MQD lives in VRAM: the traced CP_MQD_BASE_ADDR pair is 0xF400_8CD000, inside the
	 * frame buffer (vram_start 0xF400000000, M4 facts). */
	r = bc250_shim_mem_alloc(adev, BC250_MEM_VRAM, mqd_size, AMDGPU_GPU_PAGE_SIZE,
				 &ring->mqd_mem);
	if (r)
		goto fail;
	if (ring->mqd_mem.cpu == NULL) {
		r = BC250_EINVAL;
		goto fail;
	}
	ring->mqd_ptr = ring->mqd_mem.cpu;
	ring->mqd_gpu_addr = ring->mqd_mem.mc;

	if (eop_size) {
		r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, eop_size, AMDGPU_GPU_PAGE_SIZE,
					 &ring->eop_mem);
		if (r)
			goto fail;
		ring->eop_gpu_addr = ring->eop_mem.mc;
	}

	amdgpu_ring_clear_ring(ring);
	return 0;

fail:
	/* The ring buffer was allocated and something after it was not. Take the pointer back so the
	 * ring is unusable by construction rather than by the caller remembering to check r; the
	 * memory itself is given back by bc250_gfx_teardown() with the rest. */
	ring->ring = NULL;
	return r;
}

int bc250_gfx_setup(struct amdgpu_device *adev, const struct bc250_gfx_inputs *in)
{
	struct amdgpu_ring *ring;
	u32 i, csb_dwords;
	int r;

	if (adev == NULL || in == NULL)
		return BC250_EINVAL;
	/* Every poll in this file spends adev->usec_timeout iterations of bc250_shim_udelay(1); with a
	 * budget of 0 they all fall straight through and report a timeout that never happened. Refuse
	 * here, as bc250_psp.c:298 does, so the cause is named before anything is allocated. */
	if (adev->usec_timeout == 0)
		return BC250_EINVAL;

	/* gfx_v10_0_gpu_early_init(), IP_VERSION(10, 1, 3) arm: the only field of it this sequence
	 * uses is max_hw_contexts, which becomes CP_MAX_CONTEXT - 1 (the trace writes 7). */
	adev->gfx.config.max_hw_contexts = 8;
	adev->gfx.config.num_sc_per_sh = 1;
	adev->gfx.config.num_packer_per_sc = 1;

	adev->gfx.config.max_shader_engines = in->max_shader_engines;
	adev->gfx.config.max_sh_per_se = in->max_sh_per_se;
	adev->gfx.config.max_backends_per_se = in->max_backends_per_se;

	adev->gfx.async_gfx_ring = in->async_gfx_ring;
	adev->gfx.pp_gfxoff = in->pp_gfxoff;

	/* gfx_v10_0_sw_init(): one ME with one pipe, one MEC with four. The DB_RING_CONTROL write
	 * above reads num_pipe_per_me and the trace's value of 1 says the same thing; the four counts
	 * together decide which CP_*_INT_CNTL registers bc250_irq.c walks, and the trace agrees there
	 * too (see the note on struct amdgpu_me in amdgpu.h). */
	adev->gfx.me.num_me = 1;
	adev->gfx.me.num_pipe_per_me = 1;
	adev->gfx.mec.num_mec = 1;
	adev->gfx.mec.num_pipe_per_mec = 4;

	adev->gfx.num_gfx_rings = 1;      /* GFX10_NUM_GFX_RINGS_NV1X, gfx_v10_0.c:55 */
	adev->gfx.num_compute_rings = 8;

	/* amdgpu_gfx_compute_queue_acquire() with the multipipe policy: queues are spread across the
	 * four pipes of MEC1, queue-major. Bits are pipe * num_queue_per_pipe + queue with
	 * num_queue_per_pipe = 8, so pipes 0-3 and queues 0-1 give bits 0,1,8,9,16,17,24,25. */
	adev->gfx.mec_queue_bitmap = 0x03030303ULL;

	/* nv_set_ip_blocks() -> nv_init_doorbell_index(): the NAVI10 assignment, imported unmodified
	 * in amdgpu_doorbell.h. */
	adev->doorbell_index.kiq = AMDGPU_NAVI10_DOORBELL_KIQ;
	adev->doorbell_index.mec_ring0 = AMDGPU_NAVI10_DOORBELL_MEC_RING0;
	adev->doorbell_index.userqueue_start = AMDGPU_NAVI10_DOORBELL_USERQUEUE_START;
	adev->doorbell_index.userqueue_end = AMDGPU_NAVI10_DOORBELL_USERQUEUE_END;
	adev->doorbell_index.gfx_ring0 = AMDGPU_NAVI10_DOORBELL_GFX_RING0;

	/* gmc_v10_0_sw_init(): the apertures SH_MEM_BASES is built from, and the VMID split. */
	adev->gmc.shared_aperture_start = 0x2000000000000000ULL;
	adev->gmc.shared_aperture_end = adev->gmc.shared_aperture_start + (4ULL << 30) - 1;
	adev->gmc.private_aperture_start = 0x1000000000000000ULL;
	adev->gmc.private_aperture_end = adev->gmc.private_aperture_start + (4ULL << 30) - 1;
	adev->vm_manager.first_kfd_vmid = 8;
	adev->vm_manager.id_mgr[AMDGPU_GFXHUB(0)].num_ids = 8;

	/* The writeback page: one GTT page cut into read- and write-pointer slots. */
	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, BC250_WB_BYTES, AMDGPU_GPU_PAGE_SIZE,
				 &adev->gfx.wb_mem);
	if (r)
		goto fail;

	/* KIQ: me 2, pipe 1, queue 0, which is what the traced GRBM_GFX_CNTL 0x00000009 selects. */
	ring = &adev->gfx.kiq[0].ring;
	ring->adev = adev;
	ring->funcs = &bc250_ring_funcs_kiq;
	ring->me = 2;
	ring->pipe = 1;
	ring->queue = 0;
	ring->use_doorbell = true;
	ring->doorbell_index = adev->doorbell_index.kiq * 2;
	r = bc250_ring_alloc_mem(adev, ring, BC250_KIQ_RING_SIZE,
				 (u32)sizeof(struct v10_compute_mqd), GFX10_MEC_HPD_SIZE);
	if (r)
		goto fail;

	/* gfx_v10_0_sw_init(): compute rings walk queue-major over the pipes of MEC1, which is the
	 * order the trace shows (GRBM_GFX_CNTL 0x004, 0x005, 0x006, 0x007, then 0x104 ... 0x107). */
	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		ring = &adev->gfx.compute_ring[i];
		ring->adev = adev;
		ring->funcs = &bc250_ring_funcs_compute;
		ring->me = 1;
		ring->pipe = i % 4u;
		ring->queue = i / 4u;
		ring->use_doorbell = true;
		ring->doorbell_index = (adev->doorbell_index.mec_ring0 + i) * 2;
		r = bc250_ring_alloc_mem(adev, ring, BC250_RING_SIZE,
					 (u32)sizeof(struct v10_compute_mqd), GFX10_MEC_HPD_SIZE);
		if (r)
			goto fail;
	}

	for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
		ring = &adev->gfx.gfx_ring[i];
		ring->adev = adev;
		ring->funcs = &bc250_ring_funcs_gfx;
		ring->me = 0;
		ring->pipe = 0;
		ring->queue = 0;
		ring->use_doorbell = true;
		ring->doorbell_index = adev->doorbell_index.gfx_ring0 << 1;
		r = bc250_ring_alloc_mem(adev, ring, BC250_RING_SIZE,
					 (u32)sizeof(struct v10_gfx_mqd), 0);
		if (r)
			goto fail;
	}

	/* Writeback slots, in ring order: KIQ, the eight compute rings, the gfx ring. */
	{
		u64 mc = adev->gfx.wb_mem.mc;
		char *cpu = (char *)adev->gfx.wb_mem.cpu;
		u32 slot = 0;

		for (i = 0; i < BC250_WB_RINGS; i++) {
			struct amdgpu_ring *r_i;

			if (i == 0)
				r_i = &adev->gfx.kiq[0].ring;
			else if (i <= adev->gfx.num_compute_rings)
				r_i = &adev->gfx.compute_ring[i - 1];
			else
				r_i = &adev->gfx.gfx_ring[i - 1 - adev->gfx.num_compute_rings];

			r_i->rptr_gpu_addr = mc + slot * BC250_WB_SLOT_BYTES;
			slot++;
			r_i->wptr_gpu_addr = mc + slot * BC250_WB_SLOT_BYTES;
			r_i->wptr_cpu_addr = cpu ? cpu + slot * BC250_WB_SLOT_BYTES : NULL;
			slot++;
		}
	}

	/* gfx_v10_0_rlc_init() -> amdgpu_gfx_rlc_init_csb(): the clear-state buffer, sized by
	 * gfx_v10_0_get_csb_size() over the imported gfx10_cs_data. On unit A that is 949 dwords,
	 * which is the traced RLC_CSIB_LENGTH of 0x3B5. */
	adev->gfx.rlc.cs_data = gfx10_cs_data;
	csb_dwords = bc250_get_csb_size(adev);
	if (csb_dwords == 0) {
		r = BC250_EINVAL;
		goto fail;
	}
	adev->gfx.rlc.clear_state_size = csb_dwords;
	r = bc250_shim_mem_alloc(adev, BC250_MEM_VRAM, csb_dwords * 4u, AMDGPU_GPU_PAGE_SIZE,
				 &adev->gfx.rlc.clear_state_mem);
	if (r)
		goto fail;
	if (adev->gfx.rlc.clear_state_mem.cpu == NULL) {
		r = BC250_EINVAL;
		goto fail;
	}
	adev->gfx.rlc.cs_ptr = (u32 *)adev->gfx.rlc.clear_state_mem.cpu;
	adev->gfx.rlc.clear_state_gpu_addr = adev->gfx.rlc.clear_state_mem.mc;

	/* max_cu_per_sh is carried in struct bc250_gfx_inputs for completeness (it is on the same
	 * dmesg line as the other three) but nothing in this sequence reads it: the CU mask only
	 * matters to the shader compiler and to KFD. */
	return 0;

fail:
	bc250_gfx_teardown(adev);
	return r;
}

void bc250_gfx_teardown(struct amdgpu_device *adev)
{
	u32 i;

	if (adev == NULL)
		return;

	bc250_shim_mem_free(adev, &adev->gfx.rlc.clear_state_mem);
	adev->gfx.rlc.cs_ptr = NULL;
	bc250_shim_mem_free(adev, &adev->gfx.wb_mem);

	for (i = 0; i < ARRAY_SIZE(adev->gfx.gfx_ring); i++) {
		bc250_shim_mem_free(adev, &adev->gfx.gfx_ring[i].ring_mem);
		bc250_shim_mem_free(adev, &adev->gfx.gfx_ring[i].mqd_mem);
		bc250_shim_mem_free(adev, &adev->gfx.gfx_ring[i].eop_mem);
	}
	for (i = 0; i < ARRAY_SIZE(adev->gfx.compute_ring); i++) {
		bc250_shim_mem_free(adev, &adev->gfx.compute_ring[i].ring_mem);
		bc250_shim_mem_free(adev, &adev->gfx.compute_ring[i].mqd_mem);
		bc250_shim_mem_free(adev, &adev->gfx.compute_ring[i].eop_mem);
	}
	bc250_shim_mem_free(adev, &adev->gfx.kiq[0].ring.ring_mem);
	bc250_shim_mem_free(adev, &adev->gfx.kiq[0].ring.mqd_mem);
	bc250_shim_mem_free(adev, &adev->gfx.kiq[0].ring.eop_mem);
}

/* gfx_v10_0.c:7474 gfx_v10_0_hw_init(). Left out, with reasons: amdgpu_gfx_cleaner_shader_init()
 * is a no-op for gfx10 (cleaner_shader_size is 0); the AMDGPU_FW_LOAD_DIRECT block is not this
 * part's path; gfx_v10_0_tcp_harvest() is 10.1.10/10.1.1/10.1.2 only; the pbb-mode and
 * power-brake calls are 10.3 only. */
int bc250_gfx_hw_init(struct amdgpu_device *adev)
{
	bool remapped = false;
	int r;

	r = bc250_gfx_init_golden_registers(adev);
	if (r)
		return r;

	r = bc250_gfx_grbm_cam_probe(adev, &remapped);
	if (r)
		return r;

	r = bc250_gfx_constants_init(adev);
	if (r)
		return r;

	r = bc250_gfx_rlc_resume(adev);
	if (r)
		return r;

	return bc250_gfx_cp_resume(adev);
}

/* amdgpu_gfx.c:501 amdgpu_gfx_disable_kcq() and :551 amdgpu_gfx_disable_kgq(), which are the same
 * function over a different ring array: one UNMAP_QUEUES per ring in a single KIQ submission, then a
 * ring test to confirm the CP consumed them.
 *
 * The MES arm is gone (no MES on this part) and so is the reset check; what is left is the path
 * unit A would take. The ring test at the end is upstream's, and its comment says why it is there:
 * "Ring test will do a basic scratch register change check. Just run this to ensure that unmap
 * queues that is submitted before got processed successfully." Without it, this function would
 * return before the CP had acted on the packets.
 */
static int bc250_gfx_unmap_queues(struct amdgpu_device *adev, struct amdgpu_ring *rings,
				  u32 count)
{
	struct amdgpu_ring *kiq_ring = &adev->gfx.kiq[0].ring;
	u32 i;
	int r;

	if (count == 0)
		return 0;
	if (kiq_ring->ring == NULL)
		return BC250_EINVAL;

	r = amdgpu_ring_alloc(kiq_ring, BC250_UNMAP_QUEUES_SIZE * count);
	if (r)
		return r;

	for (i = 0; i < count; i++)
		bc250_kiq_unmap_queues(kiq_ring, &rings[i]);

	amdgpu_ring_commit(kiq_ring);

	return bc250_gfx_ring_test(kiq_ring);
}

/*
 * gfx_v10_0.c:7529 gfx_v10_0_hw_fini(), in its order.
 *
 * This exists to make the bring-up re-runnable without reloading firmware, which matters on Windows
 * for a reason Linux does not have: a second PSP load in one boot leaves the RLC disabled and busy
 * (facts M35, E10). So the undo has to put the CP, the queues and the RLC back to a state the same
 * hw_init can start from, using only registers and the KIQ.
 *
 * What it does, and what each step is for:
 *
 *   1. the three fault interrupt sources off, mirroring upstream's three amdgpu_irq_put() calls;
 *   2. UNMAP_QUEUES for the gfx ring, if the async gfx ring path mapped one, then for the eight
 *      compute rings. The CP keeps per-queue state that MAP_QUEUES sets up, and mapping a queue
 *      twice without unmapping it is not something the trace shows anyone doing;
 *   3. the CP and the MEC halted;
 *   4. the RLC stopped. This is NOT in upstream's hw_fini, and it is here deliberately: upstream
 *      leaves the RLC running because its next hw_init calls rlc_resume(), which stops it first.
 *      Ours does too, so for a plain re-run this is redundant - but the kmd also needs a way to
 *      stop the RLC before asking the PSP to load firmware again, and that is the one case where
 *      nothing else would stop it. bc250_gfx_rlc_stop() is exposed for exactly that.
 *
 * Errors are reported but do not stop the teardown: a half-undone GPU is worse than a fully
 * undone one, and every step after a failure is still worth attempting.
 */
void bc250_gfx_hw_fini(struct amdgpu_device *adev)
{
	int r;

	if (adev == NULL)
		return;

	(void)bc250_irq_set_priv_reg_fault(adev, BC250_IRQ_STATE_DISABLE);
	(void)bc250_irq_set_priv_inst_fault(adev, BC250_IRQ_STATE_DISABLE);
	(void)bc250_irq_set_bad_op_fault(adev, BC250_IRQ_STATE_DISABLE);

	if (adev->gfx.async_gfx_ring) {
		r = bc250_gfx_unmap_queues(adev, adev->gfx.gfx_ring, adev->gfx.num_gfx_rings);
		if (r)
			dev_err(adev->dev, "KGQ disable failed (%d)\n", r);
	}

	r = bc250_gfx_unmap_queues(adev, adev->gfx.compute_ring, adev->gfx.num_compute_rings);
	if (r)
		dev_err(adev->dev, "KCQ disable failed (%d)\n", r);

	bc250_cp_compute_enable(adev, false);
	(void)bc250_cp_gfx_enable(adev, false);

	bc250_rlc_stop(adev);
}

/* The RLC stop on its own, for the one caller that needs it without the rest: the kmd, before
 * asking the PSP to load firmware a second time in one boot. See bc250_gfx_hw_fini() above. */
void bc250_gfx_rlc_stop(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return;
	bc250_rlc_stop(adev);
}
