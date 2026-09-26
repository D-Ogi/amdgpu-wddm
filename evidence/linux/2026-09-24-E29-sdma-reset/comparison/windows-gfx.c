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
#include "cyan_skillfish_ip_offset.h"

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

/* PROVENANCE: gfx_v10_0_set/unset_safe_mode default (GC10.1), AMD MIT.
 * Explicit caller-requested scope, independent of upstream cg_flags policy.
 * Unlike upstream's void entry, require ACK before declaring entry complete.
 * A request that times out still requires a paired exit attempt. */
int bc250_gfx_rlc_safe_enter(struct amdgpu_device *adev, bool *requested)
{
 u32 i, control;
 if (!requested) return BC250_EINVAL;
 *requested=false;
 if (!adev || !adev->usec_timeout) return BC250_EINVAL;
 control=RREG32_SOC15(GC,0,mmRLC_CNTL);
 if (!REG_GET_FIELD(control,RLC_CNTL,RLC_ENABLE_F32)) return BC250_EBUSY;
 *requested=true;
 WREG32_SOC15(GC,0,mmRLC_SAFE_MODE,
  RLC_SAFE_MODE__CMD_MASK | (1u << RLC_SAFE_MODE__MESSAGE__SHIFT));
 for(i=0;i<adev->usec_timeout;i++) {
  if (!REG_GET_FIELD(RREG32_SOC15(GC,0,mmRLC_SAFE_MODE),RLC_SAFE_MODE,CMD)) return 0;
  bc250_shim_udelay(1);
 }
 return BC250_ETIME;
}
void bc250_gfx_rlc_safe_exit(struct amdgpu_device *adev, bool requested)
{
 if (adev && requested) WREG32_SOC15(GC,0,mmRLC_SAFE_MODE,RLC_SAFE_MODE__CMD_MASK);
}

/* PROVENANCE: Linux amdgpu gfx_v10_0_rlc_reset, AMD MIT. This callback is
 * not part of ordinary resume; the opt-in reload experiment below owns its use. */
static void bc250_rlc_reset(struct amdgpu_device *adev)
{
	WREG32_FIELD15(GC, 0, GRBM_SOFT_RESET, SOFT_RESET_RLC, 1);
	bc250_shim_udelay(50);
	WREG32_FIELD15(GC, 0, GRBM_SOFT_RESET, SOFT_RESET_RLC, 0);
	bc250_shim_udelay(50);
}

/* PROVENANCE: gfx_v10_0_soft_reset assertion/deassertion sequence, AMD MIT.
 * M352 experiment: caller already checked halts; restrict the mask to RLC.
 * Keep post-write reads before the delays. Log only after reset is released. */
static void bc250_rlc_reset_readback(struct amdgpu_device *adev)
{
	u32 initial, asserted, released, tmp;
	const u32 mask = GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK;
	initial = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
	tmp = initial | mask;
	WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
	tmp = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
	asserted = tmp;
	bc250_shim_udelay(50);
	tmp &= ~mask;
	WREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET, tmp);
	released = RREG32_SOC15(GC, 0, mmGRBM_SOFT_RESET);
	bc250_shim_udelay(50);
	dev_info(adev->dev, "RLC reset readback: initial %08x asserted %08x released %08x\n",
		 initial, asserted, released);
}

/* Declared addition, M349: diagnostic reload guard, not an idle/DMA verdict.
 * Caller owns an unpublished startup and serializes the register sequence.
 * 0 = not selected; 1 = reset performed and busy cleared; negative = refused.
 * Neither result proves that the following PSP reload will succeed. */
static int bc250_rlc_reload_reset(struct amdgpu_device *adev, bool readback)
{
	u32 control, status, me, mec, sdma0, sdma1;
	const u32 me_halt = CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK;
	const u32 mec_halt = CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK;
	if (adev == NULL) return BC250_EINVAL;
	control = RREG32_SOC15(GC, 0, mmRLC_CNTL);
	status = RREG32_SOC15(GC, 0, mmGRBM_STATUS2);
	if (REG_GET_FIELD(control, RLC_CNTL, RLC_ENABLE_F32) || !REG_GET_FIELD(status, GRBM_STATUS2, RLC_BUSY))
		return 0;
	me = RREG32_SOC15(GC, 0, mmCP_ME_CNTL);
	mec = RREG32_SOC15(GC, 0, mmCP_MEC_CNTL);
	sdma0 = RREG32_SOC15(GC, 0, mmSDMA0_F32_CNTL);
	sdma1 = RREG32_SOC15(GC, 0, mmSDMA1_F32_CNTL);
	dev_info(adev->dev, "RLC reload reset preflight: ME %08x MEC %08x SDMA %08x %08x\n",me,mec,sdma0,sdma1);
	if ((me & me_halt) != me_halt || (mec & mec_halt) != mec_halt ||
	    !(sdma0 & SDMA0_F32_CNTL__HALT_MASK) || !(sdma1 & SDMA1_F32_CNTL__HALT_MASK))
		return BC250_EBUSY;
	if (readback) bc250_rlc_reset_readback(adev);
	else bc250_rlc_reset(adev);
	status = RREG32_SOC15(GC, 0, mmGRBM_STATUS2);
	return REG_GET_FIELD(status, GRBM_STATUS2, RLC_BUSY) ? BC250_ETIME : 1;
}

int bc250_gfx_rlc_reload_reset(struct amdgpu_device *adev)
{
	return bc250_rlc_reload_reset(adev, false);
}

int bc250_gfx_rlc_reload_reset_readback(struct amdgpu_device *adev)
{
	return bc250_rlc_reload_reset(adev, true);
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
	 * Upstream as written: the sample, not a reset, whatever the comment above says. It was a
	 * declared deviation here for one release - `mqd->cp_hqd_pq_rptr = 0` - because on unit A's
	 * second bring-up in one boot the register held 0x737 and the re-initialised KIQ then walked
	 * 262144 - 1847 = 260297 dwords of stale ring, 5222 us against 349 us cold (E12 run 001).
	 *
	 * That reading was wrong and hardware refuted it (E12 run 002; see bc250_kiq_dequeue()). The
	 * register value was a symptom: what the CP actually resumed from was the MEC's own internal
	 * copy of base and read pointer, which no write to this register can reach while the engine is
	 * halted. Forcing 0 here changed only what the register said, which is why run 001 still took
	 * 4.9 ms, and it would have gone on hiding the real fault. With the KIQ dequeued in the
	 * teardown the register is 0 when this samples it, and the sample is correct again.
	 */
	mqd->cp_hqd_pq_rptr = RREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR);

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
static void bc250_kiq_checkpoint(bc250_gfx_checkpoint_fn checkpoint, const char *phase);
static void bc250_kiq_setting(struct amdgpu_ring *ring, bc250_gfx_checkpoint_fn checkpoint)
{
	struct amdgpu_device *adev = ring->adev;
	uint32_t tmp;

	/* Diagnostic callbacks bracket the original AMD accesses; no extra MMIO. */
	bc250_kiq_checkpoint(checkpoint, "scheduler-read");
	tmp = RREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS);
	tmp &= 0xffffff00;
	tmp |= (ring->me << 5) | (ring->pipe << 3) | (ring->queue);
	bc250_kiq_checkpoint(checkpoint, "scheduler-write");
	WREG32_SOC15(GC, 0, mmRLC_CP_SCHEDULERS, tmp | 0x80);
	bc250_kiq_checkpoint(checkpoint, "scheduler-done");
}

/* gfx_v10_0.c:6601 gfx_v10_0_cp_compute_enable(), defined below; the recovery branch of
 * bc250_kiq_init_register() needs it before its definition. */
static void bc250_cp_compute_enable(struct amdgpu_device *adev, bool enable);

/* gfx_v10_0.c:7021 gfx_v10_0_kiq_init_register(). The SR-IOV "inactivate the queue" write at the
 * top is not taken. */
static void bc250_kiq_checkpoint(bc250_gfx_checkpoint_fn checkpoint, const char *phase)
{
	if (checkpoint)
		checkpoint(phase);
}

static int bc250_kiq_init_register(struct amdgpu_ring *ring, bc250_gfx_checkpoint_fn checkpoint)
{
	struct amdgpu_device *adev = ring->adev;
	struct v10_compute_mqd *mqd = (struct v10_compute_mqd *)ring->mqd_ptr;
	u32 j;

	bc250_kiq_checkpoint(checkpoint, "disable-wptr-poll");
	/* disable wptr polling */
	WREG32_FIELD15(GC, 0, CP_PQ_WPTR_POLL_CNTL, EN, 0);

	/* disable the queue if it's active.
	 *
	 * Reaching this branch at all means a previous instance left this queue up, because a cold
	 * boot reads CP_HQD_ACTIVE = 0 and skips it - which is why the 354-write comparison against
	 * unit A is unaffected by everything below. After bc250_gfx_hw_fini() it is not reached
	 * either: that dequeues the KIQ itself (bc250_kiq_dequeue()). What is left is the case where
	 * the last instance did not, and that case is not hypothetical - 0.6.1 and everything before
	 * it halted the MEC with the KIQ still up, so the first 0.6.2 start on a machine that ran
	 * 0.6.1 lands here.
	 *
	 * DECLARED DEVIATION (driver/amdgpu-import/PROVENANCE.md), two parts, both only in this
	 * branch.
	 *
	 * 1. Un-halt the MEC if it is halted, for the length of the handshake, then halt it again.
	 *
	 *    Upstream does the handshake unconditionally and never looks at its outcome - j is unused
	 *    after the loop at gfx_v10_0.c:7035-7041 - because on the parts it exercises GFX power is
	 *    dropped across suspend or a reset intervenes, so an active queue is not something it
	 *    meets. On this part the engine that services the request can be halted here: upstream
	 *    un-halts one step later, in kcq_resume (gfx_v10_0.c:7208, called at :7243), after
	 *    kiq_resume (:7239). A halted MEC answers nothing, so without this the poll would spin out
	 *    its full timeout and the queue would stay up.
	 *
	 *    What this replaced, and why: 0.6.1 wrote CP_HQD_ACTIVE = 0 here instead - upstream's own
	 *    SR-IOV "inactivate the queue" write (gfx_v10_0.c:7028-7029) - on the argument that a
	 *    halted MEC has no state to corrupt. Hardware refuted it (E12 run 002, unit A): the write
	 *    lands in the register and not in the engine, which keeps its own copy of base and read
	 *    pointer and resumes from it when something else un-halts it two steps later. The queue
	 *    was never taken down, and the fetch went to the previous instance's ring address, which
	 *    by then was unmapped: UTCL2 fault at 0x444000, KIQ ring test timeout, -62.
	 *
	 *    Un-halting is not free: the engine resumes that stale fetch the moment it starts, and if
	 *    the address is gone it faults. But that fetch happens either way - it is what un-halting
	 *    in kcq_resume did in run 002 - so doing it here costs one fault that was already coming
	 *    and buys the one mechanism that clears the engine's copy. The fault itself is survivable
	 *    on this part and measured to be: run 002 took it, the machine stayed up, the interrupt
	 *    arrived on our own IH ring, and the teardown afterwards freed everything.
	 *
	 *    Expect a burst rather than a single vector. The engine is live for the whole poll below,
	 *    so it retries the fetch for as long as the dequeue takes, and each retry is another
	 *    UTCL2 vector; run 002 saw one because the MEC was un-halted by kcq_resume and nothing
	 *    then asked it to stop. A drained interrupt ring and a log line per vector are what tell
	 *    the two apart; the count is not a failure by itself.
	 *
	 *    Not taken instead: GRBM_SOFT_RESET.SOFT_RESET_CP with the engines halted, AMD's own
	 *    sequence from gfx_v10_0_soft_reset() (gfx_v10_0.c:7660-7685). It would also discard the
	 *    engine's state, and it is the only other mechanism that can - kgd_gfx_v10_hqd_reset()
	 *    (amdgpu_amdkfd_gfx_v10.c:1080-1085) returns 0 without touching anything, and
	 *    SOFT_RESET_CPC exists only from gfx11 on. It is not used here because upstream never
	 *    soft-resets the CP without reloading its firmware afterwards (the recovery path re-runs
	 *    hw_init, and with PSP-loaded microcode that means a PSP load), and this part cannot load
	 *    CP firmware a second time in one boot: facts M35. If the reset clears the CP's instruction
	 *    memory, that trades a recoverable queue for a device that needs a reboot. Whether it does
	 *    is measurable - reset, then read CP_ME_RAM_RADDR/CP_MEC_ME1_UCODE_ADDR back, or simply
	 *    try a ring test - and until someone measures it on unit A this stays written down rather
	 *    than shipped.
	 *
	 * 2. Zero the read pointer rather than write back what the MQD sampled.
	 *
	 *    bc250_compute_mqd_init() keeps upstream's sample (gfx_v10_0.c:6998), which is right
	 *    wherever the teardown left the register at 0. Here it is not: the previous instance left
	 *    its own read pointer behind - 0x737 on unit A - and writing that back tells a queue whose
	 *    write pointer restarts at 0 that 262144 - 1847 = 260297 dwords are pending. E12 run 001
	 *    measured the walk: stage 6 took 5222 us against 349 us cold. The queue has just been
	 *    dequeued and ring->wptr is 0, so 0 is the only consistent value, and the MQD is corrected
	 *    with the register so the two agree.
	 */
	bc250_kiq_checkpoint(checkpoint, "read-active");
	if (RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1) {
		/* Either engine halted counts as halted, and the restore below halts both. That is
		 * asymmetric only in appearance: bc250_cp_compute_enable() is upstream's own
		 * function and it writes both bits together, so the two states it can leave behind
		 * are "both running" and "both halted". Anything that had set exactly one of them
		 * did not come from this driver. */
		bool mec_halted = (RREG32_SOC15(GC, 0, mmCP_MEC_CNTL) &
				   (CP_MEC_CNTL__MEC_ME1_HALT_MASK |
				    CP_MEC_CNTL__MEC_ME2_HALT_MASK)) != 0;
		bool timed_out;

		bc250_kiq_checkpoint(checkpoint, "active-queue-recovery");
		if (mec_halted) {
			u32 cntl;

			dev_err(adev->dev,
				"KIQ still active and the MEC halted: the last instance did not take"
				" this queue down. Un-halting to dequeue it; a stale fetch may fault\n");
			bc250_cp_compute_enable(adev, true);

			/* Read it back. CP_MEC_CNTL is written here with this queue's selection in
			 * force - me 2, pipe 1, queue 0 - which upstream never does: its own un-halt
			 * is in kcq_resume, outside any selection. One register carries both MEs'
			 * halt bits, so it should not be per-instance and the write should land as
			 * written; but CPC_INT_CNTL was measured to alias under a selection on this
			 * part (fact M40), and a silently ignored un-halt here would show up only as
			 * the full timeout below. Cheaper to ask. */
			cntl = RREG32_SOC15(GC, 0, mmCP_MEC_CNTL);
			if (cntl & (CP_MEC_CNTL__MEC_ME1_HALT_MASK |
				    CP_MEC_CNTL__MEC_ME2_HALT_MASK)) {
				dev_err(adev->dev,
					"CP_MEC_CNTL reads %08X after the un-halt: the MEC is still"
					" halted and cannot answer a dequeue\n", cntl);
				bc250_cp_compute_enable(adev, false);
				return BC250_EIO;
			}
		}

		WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST, 1);
		for (j = 0; j < adev->usec_timeout; j++) {
			if (!(RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1))
				break;
			bc250_shim_udelay(1);
		}
		timed_out = j >= adev->usec_timeout;
		WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST, mqd->cp_hqd_dequeue_request);

		/* Back to the state the rest of this function expects, which is also the state
		 * kcq_resume's own un-halt expects to find. */
		if (mec_halted)
			bc250_cp_compute_enable(adev, false);

		mqd->cp_hqd_pq_rptr = 0;
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR, mqd->cp_hqd_pq_rptr);
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO, mqd->cp_hqd_pq_wptr_lo);
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_HI, mqd->cp_hqd_pq_wptr_hi);

		/* Upstream carries on regardless; a dequeue that never completed means the queue is
		 * still running and reprogramming it underneath would be worse than stopping. */
		if (timed_out)
			return BC250_ETIME;
	}

	bc250_kiq_checkpoint(checkpoint, "program-hqd");
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

	bc250_kiq_checkpoint(checkpoint, "activate-hqd");
	/* activate the queue */
	WREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE, mqd->cp_hqd_active);

	if (ring->use_doorbell)
		WREG32_FIELD15(GC, 0, CP_PQ_STATUS, DOORBELL_ENABLE, 1);

	return 0;
}

/* gfx_v10_0.c:7130 gfx_v10_0_kiq_init_queue(), cold-boot arm (not in reset, not SR-IOV). */
static int bc250_kiq_init_queue(struct amdgpu_ring *ring, bc250_gfx_checkpoint_fn checkpoint)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_mqd_prop prop;
	int r;

	bc250_kiq_checkpoint(checkpoint, "scheduler");
	bc250_kiq_setting(ring, checkpoint);

	bc250_kiq_checkpoint(checkpoint, "clear-mqd");
	memset(ring->mqd_ptr, 0, sizeof(struct v10_compute_mqd));
	bc250_kiq_checkpoint(checkpoint, "select-queue");
	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	/* amdgpu_ring.c amdgpu_ring_init_mqd(): reset the write pointer, then build the MQD. */
	bc250_kiq_checkpoint(checkpoint, "build-mqd");
	bc250_ring_to_mqd_prop(adev, ring, &prop);
	ring->wptr = 0;
	bc250_compute_mqd_init(adev, (struct v10_compute_mqd *)ring->mqd_ptr, &prop);

	r = bc250_kiq_init_register(ring, checkpoint);
	bc250_kiq_checkpoint(checkpoint, "restore-selection");
	nv_grbm_select(adev, 0, 0, 0, 0);
	bc250_kiq_checkpoint(checkpoint, "complete");

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

/* ---------------------------------------------------------------------------------------------
 * The indirect buffer: the first submission whose packets are not in the ring (ADR 0008 stage C)
 *
 * gfx_v10_0.c:8640 gfx_v10_0_ring_emit_ib_gfx(), the gfx variant, used by the gfx ring funcs
 * (:9862). What upstream's function does that is left out, one line each for why:
 *
 *   AMDGPU_IB_FLAG_CE        picks PACKET3_INDIRECT_BUFFER_CNST instead of our header. Only a
 *                            user-mode driver's constant-engine stream sets it; nothing here has a
 *                            CE stream.
 *   mcbp / IB_FLAG_PREEMPT   INDIRECT_BUFFER_PRE_ENB, INDIRECT_BUFFER_PRE_RESUME and
 *                            gfx_v10_0_ring_emit_de_meta(). The whole branch is guarded by
 *                            ring->adev->gfx.mcbp, mid-command-buffer preemption, which this
 *                            driver neither sets nor has a field for.
 *   INDIRECT_BUFFER_VALID    gfx_v10_0_ring_emit_ib_compute() (:8677) sets it and the gfx emitter
 *                            does not; it is a CE/DE flag, not a "this buffer is good" bit. The
 *                            compute variant is not transcribed at all, because nothing in this
 *                            driver submits an IB on a compute ring - the M6 dispatch writes its
 *                            packets straight into the ring (bc250_dispatch.h, point 1).
 *   __BIG_ENDIAN             the two swap bits in the low half of the address dword.
 *
 * Deviations of the usual kind: upstream BUG_ON()s a misaligned address, and a BUG() in a Windows
 * miniport is a bugcheck on a caller's mistake, so this refuses with nothing written. Two bounds
 * upstream does not check at all are checked here, because the length and the VMID reach this from
 * user mode through an escape and the fields they go into are narrow - IB_SIZE is 20 bits and VMID
 * 4 (nvd.h:235, :239), so a larger value would quietly become a different packet.
 *
 * What upstream emits AROUND an IB, and why none of it is here. amdgpu_ib_schedule()
 * (amdgpu_ib.c:124-326) is the complete list, and every part of it that a gfx ring has is guarded
 * by `job &&`: CONTEXT_CONTROL (:244, gfx_v10_0_ring_emit_cntxcntl() at :8813), the TMZ frame
 * control (:253 and :273), the VM flush (:222 amdgpu_vm_flush) and the trailing SWITCH_BUFFER
 * (:307). A ring test passes job = NULL (gfx_v10_0_ring_test_ib(), :4104) and therefore gets one IB
 * packet and one fence - exactly what bc250_gfx_submit_ib() below builds. Stage C has no context to
 * switch away from and does its VM flush by MMIO before the ring write
 * (bc250_gmc_set_vmid_pd()), so the ring-test shape is the right one and not merely the small one.
 *
 * The one thing amdgpu_ib_schedule() emits without a job is init_cond_exec (:235, the gfx ring's
 * gfx_v10_0_ring_emit_init_cond_exec() at :8847), a PACKET3_COND_EXEC over ring->cond_exe_gpu_addr.
 * It is deliberately not emitted: that packet discards the dwords behind it unless the dword at
 * that address is non-zero, upstream allocates the slot and seeds it in amdgpu_ring_init(), and
 * this driver has no such allocation. A COND_EXEC over an address we do not own would be a way of
 * throwing the submission away for reasons nobody could see. It costs nothing to leave out while
 * there is neither preemption nor a conditional-execution patch site.
 * ------------------------------------------------------------------------------------------- */

/* Upstream writes neither bound down, because upstream BUG_ON()s neither. Rather than typing
 * 0xFFFFF next to a macro that already says it, the length is asked whether it survives its own
 * encoding: PACKET3_INDIRECT_BUFFER__IB_SIZE() masks to the field and shifts by nothing
 * (nvd.h:235), so a value that comes back unchanged is a value the packet can carry. The VMID's
 * bound is AMDGPU_NUM_VMID, which is the number of contexts the hub has. */
static int bc250_ib_length_fits(u32 length_dw)
{
	return PACKET3_INDIRECT_BUFFER__IB_SIZE(length_dw) == length_dw;
}

unsigned int bc250_gfx_ib_size(const struct amdgpu_ring *ring)
{
	if (ring == NULL || ring->funcs == NULL)
		return 0;

	/* gfx_v10_0.c:9861 emit_ib_size = 4 for the gfx ring funcs: the header, the two address
	 * halves and the control dword. Compute and the KIQ answer 0 rather than 4, because this file
	 * has no emitter for them - see the block above. */
	if (ring->funcs->type != AMDGPU_RING_TYPE_GFX)
		return 0;
	return 4u;
}

int bc250_gfx_emit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw, u32 vmid)
{
	u32 control;

	if (ring == NULL || ring->adev == NULL || ring->funcs == NULL || ring->ring == NULL)
		return BC250_EINVAL;
	if (ring->funcs->type != AMDGPU_RING_TYPE_GFX)
		return BC250_EINVAL;

	/* All three checked before a single dword is written, so that a refusal leaves the ring
	 * exactly as it was, as bc250_gfx_emit_fence() does. */
	if ((gpu_addr & 0x3u) != 0)             /* upstream: BUG_ON(ib->gpu_addr & 0x3) */
		return BC250_EINVAL;
	if (length_dw == 0 || !bc250_ib_length_fits(length_dw))
		return BC250_EINVAL;
	if (vmid >= AMDGPU_NUM_VMID)
		return BC250_EINVAL;

	/* Upstream writes `control |= ib->length_dw | (vmid << 24)` (:8653). The same value, through
	 * AMD's own field macros, so that the two widths refused above are the widths the packet has
	 * and not a pair of numbers this file believes in. */
	control = PACKET3_INDIRECT_BUFFER__IB_SIZE(length_dw) | PACKET3_INDIRECT_BUFFER__VMID(vmid);

	amdgpu_ring_write(ring, PACKET3(PACKET3_INDIRECT_BUFFER, 2));
	amdgpu_ring_write(ring, lower_32_bits(gpu_addr));
	amdgpu_ring_write(ring, upper_32_bits(gpu_addr));
	amdgpu_ring_write(ring, control);

	return 0;
}

int bc250_gfx_submit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw, u32 vmid,
			u64 fence_addr, u64 seq, unsigned int flags)
{
	unsigned int ndw = bc250_gfx_ib_size(ring);
	int r;

	if (ndw == 0)
		return BC250_EINVAL;
	ndw += bc250_gfx_fence_size(ring, flags);

	r = amdgpu_ring_alloc(ring, ndw);
	if (r)
		return r;

	/* Both emitters refuse before writing anything, so an undo here really does put the ring
	 * back: the write pointer returns to where it was and whatever dwords were written sit above
	 * it, where the CP never looks. */
	r = bc250_gfx_emit_ib(ring, gpu_addr, length_dw, vmid);
	if (r) {
		amdgpu_ring_undo(ring);
		return r;
	}
	r = bc250_gfx_emit_fence(ring, fence_addr, seq, flags);
	if (r) {
		amdgpu_ring_undo(ring);
		return r;
	}

	amdgpu_ring_commit(ring);
	return 0;
}

/* gfx_v10_0_ring_emit_cntxcntl with AMDGPU_HAVE_CTX_SWITCH and without
 * AMDGPU_PREAMBLE_IB_PRESENT. BC2S leaves ib_flags at 0, so the preamble bit
 * (0x10000000, which would make this 0x91018003) stays clear.
 *   0x80000000 load_enable, else the packet is NOPs
 *   0x00008001 load_global_config | load_global_uconfig
 *   0x01000000 load_cs_sh_regs
 *   0x00010002 load_per_context_state | load_gfx_sh_regs */
#define BC250_JOB_CONTEXT_CONTROL 0x81018003u

/* PFP_SYNC_ME, CONTEXT_CONTROL, FRAME_CONTROL start, the 4-dword IB,
 * FRAME_CONTROL end, SWITCH_BUFFER. The fence size is added by the caller. */
#define BC250_JOB_FRAME_DWORDS (2u + 3u + 2u + 4u + 2u + 2u)

int bc250_gfx_submit_job(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw, u32 vmid,
			 u64 fence_addr, u64 seq, unsigned int flags)
{
	unsigned int ndw;
	int r;

	/* The ring test stays on bc250_gfx_submit_ib. A job without a VMID has no
	 * page tables for the external context load to be about. */
	if (vmid == 0)
		return BC250_EINVAL;

	ndw = bc250_gfx_ib_size(ring);
	if (ndw == 0)
		return BC250_EINVAL;
	ndw = BC250_JOB_FRAME_DWORDS + bc250_gfx_fence_size(ring, flags);

	r = amdgpu_ring_alloc(ring, ndw);
	if (r)
		return r;

	/* gfx_v10_0_ring_emit_vm_flush's PFP half. The TLB flush itself is MMIO,
	 * done by the caller before this, on every job. */
	amdgpu_ring_write(ring, PACKET3(PACKET3_PFP_SYNC_ME, 0));
	amdgpu_ring_write(ring, 0);

	/* gfx_v10_0_ring_emit_cntxcntl. The IB's own CONTEXT_CONTROL does not
	 * replace this one: that body is 0x80000000, 0x80000000. */
	amdgpu_ring_write(ring, PACKET3(PACKET3_CONTEXT_CONTROL, 1));
	amdgpu_ring_write(ring, BC250_JOB_CONTEXT_CONTROL);
	amdgpu_ring_write(ring, 0);

	/* gfx_v10_0_ring_emit_frame_cntl(start, secure=false). */
	amdgpu_ring_write(ring, PACKET3(PACKET3_FRAME_CONTROL, 0));
	amdgpu_ring_write(ring, FRAME_CMD(0));

	r = bc250_gfx_emit_ib(ring, gpu_addr, length_dw, vmid);
	if (r) {
		amdgpu_ring_undo(ring);
		return r;
	}

	amdgpu_ring_write(ring, PACKET3(PACKET3_FRAME_CONTROL, 0));
	amdgpu_ring_write(ring, FRAME_CMD(1));

	r = bc250_gfx_emit_fence(ring, fence_addr, seq, flags);
	if (r) {
		amdgpu_ring_undo(ring);
		return r;
	}

	/* After the fence, so it cannot be why this fence fails to arrive.
	 * gfx_v10_0_ring_emit_sb. */
	amdgpu_ring_write(ring, PACKET3(PACKET3_SWITCH_BUFFER, 0));
	amdgpu_ring_write(ring, 0);

	amdgpu_ring_commit(ring);
	return 0;
}

/* COMPUTE_PGM_LO is addr >> 8 and COMPUTE_PGM_HI is bits 47:40. A SET_SH_REG count is the
 * number of values; the register offset is one extra dword, so the packet is count + 2
 * dwords, the same as every other type-3 packet. */
int bc250_pm4_shader_addr(const u32 *dw, u32 ndw, u64 *byte_addr, u32 *lo, u32 *hi,
			  u32 *hi_written)
{
	/* mmCOMPUTE_PGM_LO is the offset inside the GC block. BASE_IDX 0 is
	 * GC_BASE__INST0_SEG0. The packet counts from PACKET3_SET_SH_REG_START.
	 * SOC15_REG_OFFSET would add the same base, but it needs adev. */
	const u32 pgm = GC_BASE__INST0_SEG0 + mmCOMPUTE_PGM_LO - PACKET3_SET_SH_REG_START;
	u32 i = 0;

	if (byte_addr == NULL || lo == NULL || hi == NULL || hi_written == NULL)
		return 0;
	*byte_addr = 0;
	*lo = 0;
	*hi = 0;
	*hi_written = 0;
	if (dw == NULL || ndw == 0)
		return 0;

	while (i < ndw) {
		u32 word = dw[i];
		u32 op, count, total;

		if ((word >> 30) != 3u) {
			i++;
			continue;
		}
		op = (word >> 8) & 0xffu;
		count = (word >> 16) & 0x3fffu;
		total = count + 2u;
		if (total < 2u || i + total > ndw)
			break;
		if (op == PACKET3_SET_SH_REG && count >= 1u &&
		    (dw[i + 1u] & 0xffffu) == pgm) {
			*lo = dw[i + 2u];
			if (count >= 2u) {
				*hi = dw[i + 3u] & 0xffu;
				*hi_written = 1u;
				*byte_addr = ((u64)(*hi) << 40) | ((u64)(*lo) << 8);
			} else {
				*byte_addr = (u64)(*lo) << 8;
			}
			return 1;
		}
		i += total;
	}
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * One indirect buffer to submit: the ring test, moved out of the ring
 *
 * The first IB has to be something whose result is already known from the same hardware by another
 * route, or a failure says nothing about the IB. bc250_gfx_ring_test() is that route: the same
 * PACKET3_SET_UCONFIG_REG writing 0xDEADBEEF into SCRATCH_REG0, and it has passed eleven times per
 * bring-up since M5. Putting those three dwords in a GTT page and reaching them through
 * PACKET3_INDIRECT_BUFFER changes exactly one thing - where the CP fetched them from - so a run
 * that leaves 0xCAFEDEAD in the register says the fetch failed and nothing else.
 *
 * Upstream's own first IB is gfx_v10_0_ring_test_ib() (gfx_v10_0.c:4071), which writes to memory
 * with a WRITE_DATA instead. A register is used here on purpose: the readback then does not depend
 * on the GTT mapping being right as well, which is the other half of what stage C is testing.
 * ------------------------------------------------------------------------------------------- */

int bc250_gfx_ib_page_alloc(struct amdgpu_device *adev)
{
	int r;

	if (adev == NULL)
		return BC250_EINVAL;
	if (adev->gfx.ib_mem.cpu != NULL)
		return 0;                               /* already there; idempotent */

	r = bc250_shim_mem_alloc(adev, BC250_MEM_GTT, AMDGPU_GPU_PAGE_SIZE, AMDGPU_GPU_PAGE_SIZE,
				 &adev->gfx.ib_mem);
	if (r)
		return r;
	if (adev->gfx.ib_mem.cpu == NULL)
		return BC250_EINVAL;
	return 0;
}

void bc250_gfx_ib_page_free(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return;
	bc250_shim_mem_free(adev, &adev->gfx.ib_mem);
}

u64 bc250_gfx_ib_addr(const struct amdgpu_device *adev)
{
	return (adev == NULL) ? 0 : adev->gfx.ib_mem.mc;
}

int bc250_gfx_ib_ring_test_build(struct amdgpu_device *adev, u32 *length_dw)
{
	volatile u32 *ib;
	u32 scratch;

	if (adev == NULL || length_dw == NULL)
		return BC250_EINVAL;
	*length_dw = 0;
	if (adev->gfx.ib_mem.cpu == NULL)
		return BC250_EINVAL;

	/* The same register bc250_gfx_ring_test() uses, named the same way, and seeded with the same
	 * value it seeds, so that a stale 0xDEADBEEF from an earlier ring test cannot be read as this
	 * submission's result. */
	scratch = SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0);
	WREG32(scratch, 0xCAFEDEAD);

	ib = (volatile u32 *)adev->gfx.ib_mem.cpu;
	ib[0] = PACKET3(PACKET3_SET_UCONFIG_REG, 1);
	ib[1] = scratch - PACKET3_SET_UCONFIG_REG_START;
	ib[2] = 0xDEADBEEF;
	*length_dw = 3u;
	return 0;
}

int bc250_gfx_ib_ring_test_result(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return BC250_EINVAL;
	return RREG32(SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0)) == 0xDEADBEEF ? 0 : BC250_ETIME;
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
/* The caller owns sequencing and serialization. Splitting at these boundaries
 * permits the Windows startup owner to persist progress at PASSIVE_LEVEL after
 * releasing its locks. No register, queue or packet sequence is duplicated. */
int bc250_gfx_cp_resume_step_traced(struct amdgpu_device *adev, unsigned int step,
                                  bc250_gfx_checkpoint_fn checkpoint)
{
	u32 i;
	int r;

	if (adev == NULL || adev->usec_timeout == 0)
		return BC250_EINVAL;
	switch (step) {
	case BC250_CP_KIQ_INIT:
		/* gfx_v10_0_kiq_resume() -> gfx_v10_0_kiq_init_queue() */
		dev_info(adev->dev, "CP resume kiq-init begin\n");
		r = bc250_kiq_init_queue(&adev->gfx.kiq[0].ring, checkpoint);
		dev_info(adev->dev, "CP resume kiq-init end result=%d\n", r);
		if (r)
			return r;
		return 0;
	case BC250_CP_COMPUTE_INIT:
		/* gfx_v10_0_kcq_resume() */
		dev_info(adev->dev, "CP resume compute-unhalt begin\n");
		bc250_cp_compute_enable(adev, true);
		dev_info(adev->dev, "CP resume compute-unhalt end\n");
		for (i = 0; i < adev->gfx.num_compute_rings; i++) {
			dev_info(adev->dev, "CP resume kcq-init ring=%u begin\n", i);
			r = bc250_kcq_init_queue(&adev->gfx.compute_ring[i]);
			dev_info(adev->dev, "CP resume kcq-init ring=%u end result=%d\n", i, r);
			if (r)
				return r;
		}
		return 0;
	case BC250_CP_KCQ_ENABLE:
		dev_info(adev->dev, "CP resume kcq-enable begin\n");
		r = bc250_enable_kcq(adev);
		dev_info(adev->dev, "CP resume kcq-enable end result=%d\n", r);
		if (r)
			return r;
		return 0;
	case BC250_CP_GFX_QUEUE_INIT:
		/* gfx_v10_0_cp_async_gfx_ring_resume() */
		for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
			dev_info(adev->dev, "CP resume kgq-init ring=%u begin\n", i);
			r = bc250_kgq_init_queue(&adev->gfx.gfx_ring[i], adev->gfx.async_gfx_ring);
			dev_info(adev->dev, "CP resume kgq-init ring=%u end result=%d\n", i, r);
			if (r)
				return r;
		}
		return 0;
	case BC250_CP_KGQ_ENABLE:
		dev_info(adev->dev, "CP resume kgq-enable begin\n");
		r = bc250_enable_kgq(adev);
		dev_info(adev->dev, "CP resume kgq-enable end result=%d\n", r);
		if (r)
			return r;
		return 0;
	case BC250_CP_GFX_START:
		dev_info(adev->dev, "CP resume gfx-start begin\n");
		r = bc250_cp_gfx_start(adev);
		dev_info(adev->dev, "CP resume gfx-start end result=%d\n", r);
		if (r)
			return r;
		return 0;
	case BC250_CP_GFX_TEST:
		for (i = 0; i < adev->gfx.num_gfx_rings; i++) {
			dev_info(adev->dev, "CP resume gfx-test ring=%u begin\n", i);
			r = bc250_gfx_ring_test(&adev->gfx.gfx_ring[i]);
			dev_info(adev->dev, "CP resume gfx-test ring=%u end result=%d\n", i, r);
			if (r)
				return r;
		}
		return 0;
	case BC250_CP_COMPUTE_TEST:
		for (i = 0; i < adev->gfx.num_compute_rings; i++) {
			dev_info(adev->dev, "CP resume compute-test ring=%u begin\n", i);
			r = bc250_gfx_ring_test(&adev->gfx.compute_ring[i]);
			dev_info(adev->dev, "CP resume compute-test ring=%u end result=%d\n", i, r);
			if (r)
				return r;
		}
		return 0;
	default:
		return BC250_EINVAL;
	}
}

int bc250_gfx_cp_resume_step(struct amdgpu_device *adev, unsigned int step)
{
	return bc250_gfx_cp_resume_step_traced(adev, step, NULL);
}

int bc250_gfx_cp_resume(struct amdgpu_device *adev)
{
	unsigned int step;
	int r;
	for (step = BC250_CP_KIQ_INIT; step <= BC250_CP_COMPUTE_TEST; step++) {
		r = bc250_gfx_cp_resume_step(adev, step);
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
 * DECLARED ADDITION (driver/amdgpu-import/PROVENANCE.md): take the KIQ's own HQD down while the MEC
 * is still running.
 *
 * Nothing upstream does this, and this driver has to, for a reason experiment E12 run 002 measured
 * on unit A rather than reasoned about. The MEC keeps its own copy of an active queue's ring base
 * and read pointer. Writes to the CP_HQD_* registers while the engine is halted do not reach that
 * copy; on un-halt the engine resumes from it. So a teardown that only halts the MEC leaves the KIQ
 * fetching, and the next bring-up - whose rings are at new addresses, because the miniport frees and
 * re-allocates and gpumem does not hand back a freed GART range - has the engine walk off into
 * memory that is no longer mapped. Unit A: first KIQ at 0x442000, second at 0x564000, UTCL2 fault at
 * 0x444000 = the first base plus the first read pointer, an address no register in either run names,
 * and the KIQ ring test then timed out after 165 ms (-62).
 *
 * The handshake below is the one a running MEC answers and the only one that clears that copy:
 * CP_HQD_DEQUEUE_REQUEST = 1 (DRAIN_PIPE), then poll CP_HQD_ACTIVE to 0. It is upstream's own, from
 * kgd_hqd_destroy() (amdgpu_amdkfd_gfx_v10.c:606-619) and from the active branch of
 * gfx_v10_0_kiq_init_register() (gfx_v10_0.c:7031-7041); what is new here is only WHERE it is
 * called. Upstream never needs it because the KIQ's own HQD is never taken down at all: its hw_fini
 * unmaps the other queues through the KIQ and halts, and it gets away with that on the parts it
 * exercises because GFX power is dropped across suspend or a full reset intervenes before the next
 * hw_init, either of which discards the engine's copy. This part keeps GFX powered and has no reset
 * in the path, so the copy survives into the next bring-up. There is no documented MEC reset to use
 * instead: kgd_gfx_v10_hqd_reset() (amdgpu_amdkfd_gfx_v10.c:1080-1085) returns 0 without touching
 * anything, and GRBM_SOFT_RESET.SOFT_RESET_CPC exists only from gfx11 on.
 *
 * The pointer registers are zeroed afterwards, with the queue already dequeued, because that is the
 * state the next kiq_init_register() assumes when it samples them (gfx_v10_0.c:6998). It is also why
 * mqd->cp_hqd_pq_rptr can go back to upstream's sample: the value it samples is put there here.
 *
 * A failure is reported and the teardown carries on. The caller then halts the MEC, which leaves the
 * engine copy alive - but the alternative is to stop half way through the undo, and the next
 * bring-up has the fallback in bc250_kiq_init_register() for exactly this case.
 */
static int bc250_kiq_dequeue(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring = &adev->gfx.kiq[0].ring;
	u32 j;
	int r = 0;

	if (ring->ring == NULL)
		return BC250_EINVAL;

	nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);

	if (RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1) {
		WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST, 1);

		/* Tested before the delay rather than after it, so that the register is read at
		 * least once whatever the budget is. The two other polls in this file
		 * (bc250_gfx_cp_resume(), bc250_gfx_setup()) refuse adev->usec_timeout == 0 at the
		 * entry point instead; the teardown cannot, because it has to run whatever state
		 * the caller is in, and a budget of 0 must not be reported as a dequeue the engine
		 * refused. */
		for (j = 0; ; j++) {
			if (!(RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1))
				break;
			if (j >= adev->usec_timeout) {
				r = BC250_ETIME;
				break;
			}
			bc250_shim_udelay(1);
		}
		/* Put the request back the way the MQD's own field has it, which is what upstream's
		 * restore writes (gfx_v10_0.c:7043): a left-over request is a request the next
		 * activation would find pending. */
		WREG32_SOC15(GC, 0, mmCP_HQD_DEQUEUE_REQUEST, 0);
	}

	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_DOORBELL_CONTROL, 0);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR, 0);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO, 0);
	WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_HI, 0);

	nv_grbm_select(adev, 0, 0, 0, 0);
	return r;
}

/*
 * DECLARED ADDITION (driver/amdgpu-import/PROVENANCE.md): zero the eight compute queues' pointer
 * registers, each under its own selection, once they are unmapped.
 *
 * Same argument as the two lines in bc250_kiq_dequeue() above, for the other nine tenths of the
 * problem. bc250_compute_mqd_init() samples CP_HQD_PQ_RPTR (:765) and bc250_kcq_init_queue()
 * selects each ring's own slot (:1089) before calling it, so each of the eight MQDs is built from
 * whatever that slot's read pointer happens to hold. After a bring-up that ran the ring tests the
 * queues idle at rptr == wptr != 0 (fact M40), and nothing in the undo puts them back: UNMAP_QUEUES
 * takes the queue off the pipe, and whether the CP clears the slot's pointer registers as it does
 * so is not something we have measured or found stated. Upstream never has to care - it has no
 * second bring-up without a power cycle or a reset in between - so this is not a deviation from its
 * behaviour but a step it has no need for.
 *
 * With this here, the sentence in bc250_compute_mqd_init()'s comment ("the value it samples is put
 * there here") is true for all nine queues rather than for the KIQ alone.
 *
 * Call it after the queues are unmapped, which bc250_gfx_unmap_queues() guarantees by ending in a
 * KIQ ring test: the CP has consumed and completed the UNMAP_QUEUES packets before it returns, so
 * nothing is going to write these registers behind us. No handshake is needed and none is possible
 * - unlike the KIQ's own HQD, these queues have already been dequeued by a running CP.
 */
static void bc250_kcq_clear_pointers(struct amdgpu_device *adev)
{
	u32 i;

	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		struct amdgpu_ring *ring = &adev->gfx.compute_ring[i];

		if (ring->ring == NULL)
			continue;

		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		/* A failed unmap is logged and survived by the caller, so the premise above may not hold:
		 * a queue the CP still runs is left alone, a zeroed read pointer would make it re-walk its ring. */
		if (RREG32_SOC15(GC, 0, mmCP_HQD_ACTIVE) & 1) {
			dev_err(adev->dev, "compute ring %u (%u.%u.%u) still active after the unmap, pointers left alone\n",
				i, ring->me, ring->pipe, ring->queue);
			continue;
		}
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_RPTR, 0);
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_LO, 0);
		WREG32_SOC15(GC, 0, mmCP_HQD_PQ_WPTR_HI, 0);
	}

	nv_grbm_select(adev, 0, 0, 0, 0);
}

/*
 * gfx_v10_0.c:7529 gfx_v10_0_hw_fini(), in its order.
 *
 * This exists to make the bring-up re-runnable without reloading firmware. A second PSP load in
 * the E10 Windows experiment left RLC disabled and busy (M35); E13 also observed Linux reload
 * failure (M55). Generic Linux retirement has a conditional RLC stop in SMU cleanup (M333),
 * but that is not a successful reload reference. The register/KIQ undo below does not prove
 * that PSP firmware can safely be reloaded afterwards (M330-M332).
 *
 * What it does, and what each step is for:
 *
 *   1. the three fault interrupt sources off, mirroring upstream's three amdgpu_irq_put() calls;
 *   2. UNMAP_QUEUES for the gfx ring, if the async gfx ring path mapped one, then for the eight
 *      compute rings. The CP keeps per-queue state that MAP_QUEUES sets up, and mapping a queue
 *      twice without unmapping it is not something the trace shows anyone doing;
 *   2a. the eight compute queues' pointer registers zeroed, so that the next bring-up's MQDs sample
 *      a read pointer this code put there. See bc250_kcq_clear_pointers() above;
 *   2b. the KIQ's own HQD dequeued while the MEC still runs, which is not upstream's and is the one
 *      step without which a second bring-up meets a live fetcher. See bc250_kiq_dequeue() above;
 *   3. the CP and the MEC halted;
 *   4. the RLC stopped. Upstream GFX hw_fini does not do this here; the generic SMU
 *      cleanup can call the RLC stop callback (witnessed on unit A in E28/M334).
 *      The kmd has no corresponding SMU teardown layer, so it stops RLC here before
 *      PSP retirement. Clearing enable alone does not prove safe firmware reload;
 *      E28 also failed to reload under Linux after its witnessed stop.
 *
 * Errors are reported but do not stop the teardown: a half-undone GPU is worse than a fully
 * undone one, and every step after a failure is still worth attempting. The return value is the
 * first failure seen, for a caller that has somewhere to put it - the miniport's Fini keeps the
 * pages and surfaces it - and 0 when the whole undo ran. It is deliberately not a reason for the
 * caller to do anything differently: there is nothing left to try.
 */
static int bc250_gfx_hw_fini_impl(struct amdgpu_device *adev, bool stop_rlc)
{
	int first = 0;
	int r;

	if (adev == NULL)
		return BC250_EINVAL;

	(void)bc250_irq_set_priv_reg_fault(adev, BC250_IRQ_STATE_DISABLE);
	(void)bc250_irq_set_priv_inst_fault(adev, BC250_IRQ_STATE_DISABLE);
	(void)bc250_irq_set_bad_op_fault(adev, BC250_IRQ_STATE_DISABLE);

	if (adev->gfx.async_gfx_ring) {
		r = bc250_gfx_unmap_queues(adev, adev->gfx.gfx_ring, adev->gfx.num_gfx_rings);
		if (r) {
			dev_err(adev->dev, "KGQ disable failed (%d)\n", r);
			if (!first)
				first = r;
		}
	}

	r = bc250_gfx_unmap_queues(adev, adev->gfx.compute_ring, adev->gfx.num_compute_rings);
	if (r) {
		dev_err(adev->dev, "KCQ disable failed (%d)\n", r);
		if (!first)
			first = r;
	}

	bc250_kcq_clear_pointers(adev);

	/* Step 3 below halts the MEC, so this is the last moment a running engine can answer. */
	r = bc250_kiq_dequeue(adev);
	if (r) {
		dev_err(adev->dev, "KIQ dequeue failed (%d)\n", r);
		if (!first)
			first = r;
	}

	bc250_cp_compute_enable(adev, false);
	(void)bc250_cp_gfx_enable(adev, false);

	if (stop_rlc) bc250_rlc_stop(adev);
	return first;
}

int bc250_gfx_hw_fini(struct amdgpu_device *adev)
{
	return bc250_gfx_hw_fini_impl(adev, true);
}

/* M357: phase extraction for a future owner-scoped GTT retirement. Caller
 * must retain every buffer and firmware object, then explicitly stop RLC
 * before PSP unload/storage destruction. This alone is not a quiet verdict. */
int bc250_gfx_hw_fini_keep_rlc(struct amdgpu_device *adev)
{
	return bc250_gfx_hw_fini_impl(adev, false);
}

/* The RLC stop on its own, for the one caller that needs it without the rest: the kmd, before
 * asking the PSP to load firmware a second time in one boot. See bc250_gfx_hw_fini() above. */
void bc250_gfx_rlc_stop(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return;
	bc250_rlc_stop(adev);
}
