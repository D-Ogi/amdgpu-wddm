/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * NBIO 2.3 doorbell plumbing. See include/bc250_nbio.h for what this is and why only two functions
 * of nbio_v2_3.c are here.
 *
 * Both follow drivers/gpu/drm/amd/amdgpu/nbio_v2_3.c at v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
 */
#include "bc250_nbio.h"
#include "bc250_gmc.h"		/* BC250_EINVAL */

#include <nbio_2_3_offset.h>
#include <nbio_2_3_sh_mask.h>
#include "soc15_common.h"

/* nbio_v2_3.c:189 nbio_v2_3_sdma_doorbell_range().
 *
 * Two of the four instance cases upstream are SDMA2 and SDMA3, which this part does not have. The
 * else arm of `use_doorbell` is kept: a ring set up without a doorbell has to tell the register so,
 * and leaving a stale SIZE behind would let the GPU keep claiming a window it no longer uses.
 *
 * Checked against unit A: the traced writes are BIF_SDMA0_DOORBELL_RANGE = 0x00140800 and
 * BIF_SDMA1_DOORBELL_RANGE = 0x00140850. OFFSET starts at bit 2 and SIZE at bit 16, so those are
 * doorbell indices 0x200 and 0x214 with a window of 20, which is what the SDMA setup passes. */
void bc250_nbio_sdma_doorbell_range(struct amdgpu_device *adev, int instance,
				    bool use_doorbell, int doorbell_index,
				    int doorbell_size)
{
	u32 reg = instance == 0 ? SOC15_REG_OFFSET(NBIO, 0, mmBIF_SDMA0_DOORBELL_RANGE) :
				  SOC15_REG_OFFSET(NBIO, 0, mmBIF_SDMA1_DOORBELL_RANGE);
	u32 doorbell_range = RREG32(reg);

	if (use_doorbell) {
		doorbell_range = REG_SET_FIELD(doorbell_range,
					       BIF_SDMA0_DOORBELL_RANGE, OFFSET,
					       doorbell_index);
		doorbell_range = REG_SET_FIELD(doorbell_range,
					       BIF_SDMA0_DOORBELL_RANGE, SIZE,
					       doorbell_size);
	} else {
		doorbell_range = REG_SET_FIELD(doorbell_range,
					       BIF_SDMA0_DOORBELL_RANGE, SIZE,
					       0);
	}

	WREG32(reg, doorbell_range);
}

/* nbio_v2_3.c:162 nbio_v2_3_enable_doorbell_selfring_aperture().
 *
 * The trace writes BASE_LOW = 0xD0000000, BASE_HIGH = 0 and CNTL = 0x00000003, and the CNTL value
 * is the EN and MODE bits with SIZE 0, which is exactly what the enable arm builds. The two base
 * registers carry an address we supply, so the replay test lists them among the values it compares
 * against our own allocation rather than against the trace.
 *
 * Deviation: upstream returns void; this refuses an enable with no base address. See the header for
 * why. Disabling needs no base and is never refused - EN = 0 is the safe direction. */
int bc250_nbio_enable_doorbell_selfring_aperture(struct amdgpu_device *adev, bool enable)
{
	u32 tmp = 0;

	if (adev == NULL)
		return BC250_EINVAL;
	if (enable && adev->doorbell.base == 0)
		return BC250_EINVAL;

	if (enable) {
		tmp = REG_SET_FIELD(tmp, BIF_BX_PF_DOORBELL_SELFRING_GPA_APER_CNTL,
				    DOORBELL_SELFRING_GPA_APER_EN, 1) |
		      REG_SET_FIELD(tmp, BIF_BX_PF_DOORBELL_SELFRING_GPA_APER_CNTL,
				    DOORBELL_SELFRING_GPA_APER_MODE, 1) |
		      REG_SET_FIELD(tmp, BIF_BX_PF_DOORBELL_SELFRING_GPA_APER_CNTL,
				    DOORBELL_SELFRING_GPA_APER_SIZE, 0);

		WREG32_SOC15(NBIO, 0, mmBIF_BX_PF_DOORBELL_SELFRING_GPA_APER_BASE_LOW,
			     lower_32_bits(adev->doorbell.base));
		WREG32_SOC15(NBIO, 0, mmBIF_BX_PF_DOORBELL_SELFRING_GPA_APER_BASE_HIGH,
			     upper_32_bits(adev->doorbell.base));
	}

	WREG32_SOC15(NBIO, 0, mmBIF_BX_PF_DOORBELL_SELFRING_GPA_APER_CNTL, tmp);
	return 0;
}

/* nbio_v2_3.c:206 nbio_v2_3_ih_control(). Traced on unit A at 0.252835-0.252836:
 *     W  NBIO.INTERRUPT_CNTL2  0x03848  007E3C10      (the dummy page, >> 8)
 *     R  NBIO.INTERRUPT_CNTL   0x03844  00000000
 *     W  NBIO.INTERRUPT_CNTL   0x03844  00000000
 *
 * INTERRUPT_CNTL2 is a whole-register write of the dummy read target; INTERRUPT_CNTL is a
 * read-modify-write clearing IH_DUMMY_RD_OVERRIDE (so the dummy read follows MSI rather than the
 * override bit) and IH_REQ_NONSNOOP_EN (the ring is snooped system memory, not VRAM). Upstream's
 * two comments on those choices are kept at the writes. */
void bc250_nbio_ih_control(struct amdgpu_device *adev)
{
	u32 interrupt_cntl;

	WREG32_SOC15(NBIO, 0, mmINTERRUPT_CNTL2, (u32)(adev->dummy_page_addr >> 8));

	interrupt_cntl = RREG32_SOC15(NBIO, 0, mmINTERRUPT_CNTL);
	/* IH_DUMMY_RD_OVERRIDE = 0: dummy read disabled with msi, enabled without msi */
	interrupt_cntl = REG_SET_FIELD(interrupt_cntl, INTERRUPT_CNTL, IH_DUMMY_RD_OVERRIDE, 0);
	/* IH_REQ_NONSNOOP_EN = 1 would be for a ring in non-cacheable memory, e.g. VRAM */
	interrupt_cntl = REG_SET_FIELD(interrupt_cntl, INTERRUPT_CNTL, IH_REQ_NONSNOOP_EN, 0);
	WREG32_SOC15(NBIO, 0, mmINTERRUPT_CNTL, interrupt_cntl);
}

/* nbio_v2_3.c:186 nbio_v2_3_ih_doorbell_range(). Traced at 0.252841-0.252842:
 *     R  NBIO.BIF_IH_DOORBELL_RANGE  0x03bc8  00000000
 *     W  NBIO.BIF_IH_DOORBELL_RANGE  0x03bc8  00020BC0
 *
 * OFFSET starts at bit 2 and SIZE at bit 16, so 0x00020BC0 is index 0x2F0 with a window of 2 -
 * which is AMDGPU_NAVI10_DOORBELL_IH (0x178) doubled, exactly what bc250_ih_setup() computes. */
void bc250_nbio_ih_doorbell_range(struct amdgpu_device *adev, bool use_doorbell,
				  int doorbell_index)
{
	u32 ih_doorbell_range = RREG32_SOC15(NBIO, 0, mmBIF_IH_DOORBELL_RANGE);

	if (use_doorbell) {
		ih_doorbell_range = REG_SET_FIELD(ih_doorbell_range, BIF_IH_DOORBELL_RANGE,
						  OFFSET, doorbell_index);
		ih_doorbell_range = REG_SET_FIELD(ih_doorbell_range, BIF_IH_DOORBELL_RANGE,
						  SIZE, 2);
	} else {
		ih_doorbell_range = REG_SET_FIELD(ih_doorbell_range, BIF_IH_DOORBELL_RANGE,
						  SIZE, 0);
	}

	WREG32_SOC15(NBIO, 0, mmBIF_IH_DOORBELL_RANGE, ih_doorbell_range);
}

/* ---------------------------------------------------------------------------------------------
 * Stage 0: what nv_common_hw_init() does at t = 0.0383 on unit A, half a second before the GFX
 * block is touched.
 *
 * This is here because Windows does not do it. The E02 sweep of unit A under Windows reads
 * RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN as 0 (evidence/windows/2026-09-21-E02-run-001/sweep-NBIO.log),
 * and with that bit clear a doorbell write never reaches the CP: every ring in milestone M5 is
 * driven by a doorbell and the first KIQ ring test would time out. It is stage 0 rather than part
 * of the GFX bring-up because that is where it belongs - nv_common is a different IP block, it runs
 * once per boot, and the GFX block may be torn down and brought up again without repeating it.
 *
 * Of the three, only bc250_nbio_enable_doorbell_aperture() is needed on this part, and it is the one
 * the miniport calls. The two HDP remap functions are transcribed because nv_common_hw_init() runs
 * them and unit A's trace shows them, not because anything here depends on them - see the comment
 * above bc250_nbio_remap_hdp_registers() for why the alias is never used on an APU.
 * ------------------------------------------------------------------------------------------- */

/* nbio_v2_3.c:553 nbio_v2_3_set_reg_remap(). Writes no register; it only decides where in BAR5 the
 * HDP flush registers will be aliased.
 *
 * The SR-IOV arm and the large-page arm are both dropped: there is no virtual function here, and
 * every Windows target of this driver has a 4 KB page. What is left is the one case unit A shows,
 * and the traced values confirm it - 0x0007F000 and 0x0007F004 are MMIO_REG_HOLE_OFFSET plus the
 * two KFD offsets, with MMIO_REG_HOLE_OFFSET = 0x80000 - 4096 = 0x7F000. */
void bc250_nbio_set_reg_remap(struct amdgpu_device *adev)
{
	adev->rmmio_remap.reg_offset = BC250_MMIO_REG_HOLE_OFFSET;
	adev->rmmio_remap.bus_addr = adev->rmmio_base + BC250_MMIO_REG_HOLE_OFFSET;
}

/* nbio_v2_3.c:66 nbio_v2_3_remap_hdp_registers().
 *
 * Traced on unit A at 0.038310 and 0.038312:
 *     W  NBIO.REMAP_HDP_MEM_FLUSH_CNTL  0x03934  0007F000
 *     W  NBIO.REMAP_HDP_REG_FLUSH_CNTL  0x03938  0007F004
 *
 * These two program the alias amdgpu programs, and on this part nothing ever uses it. An earlier
 * version of this comment claimed the GFX bring-up needed them to avoid a flush race; that was
 * wrong, and the correction is worth keeping because the reasoning is not obvious:
 *
 *   - amdgpu_device_flush_hdp() (amdgpu_device.c:7278) begins with
 *         #ifdef CONFIG_X86_64
 *             if ((adev->flags & AMD_IS_APU) && !amdgpu_passthrough(adev))
 *                     return;
 *     so on an x86-64 APU that is not passed through it flushes nothing at all.
 *   - 1002:13FE is registered CHIP_CYAN_SKILLFISH|AMD_IS_APU (amdgpu_drv.c:2181), so that is us.
 *   - The trace agrees: in 146135 traced accesses, the aliased dwords 0x1FC00 and 0x1FC01 are never
 *     read or written. The only appearance of 0x0007F000/0x0007F004 anywhere is the two writes
 *     below. amdgpu sets the alias up and then never stores to it.
 *
 * One caller does bypass that early return - smu_cmn_update_table() calls amdgpu_asic_flush_hdp()
 * directly (smu_cmn.c:983), and this part does use swsmu. It is unreachable here: the flush is only
 * on the drv2smu arm, every drv2smu caller passes SMU_TABLE_OVERDRIVE, SMU_TABLE_I2C_COMMANDS or
 * SMU_TABLE_WIFIBAND, and cyan_skillfish_table_map maps SMU_METRICS alone
 * (cyan_skillfish_ppt.c:83-85), so the table_id < 0 check returns -EINVAL first. The other bypass,
 * exposing the alias page to userspace for KFD to flush itself (amdgpu_ttm.c:1864-1909), needs a
 * KFD client and rmmio_remap.bus_addr, neither of which this driver has.
 *
 * So these stay as a faithful transcription of nv_common_hw_init() and are not part of experiment
 * E11: the miniport calls bc250_nbio_enable_doorbell_aperture() on its own, not bc250_nbio_hw_init().
 *
 * bc250_nbio_set_reg_remap() must have run first. */
void bc250_nbio_remap_hdp_registers(struct amdgpu_device *adev)
{
	WREG32_SOC15(NBIO, 0, mmREMAP_HDP_MEM_FLUSH_CNTL,
		     adev->rmmio_remap.reg_offset + KFD_MMIO_REMAP_HDP_MEM_FLUSH_CNTL);
	WREG32_SOC15(NBIO, 0, mmREMAP_HDP_REG_FLUSH_CNTL,
		     adev->rmmio_remap.reg_offset + KFD_MMIO_REMAP_HDP_REG_FLUSH_CNTL);
}

/* nbio_v2_3.c:155 nbio_v2_3_enable_doorbell_aperture().
 *
 * Traced on unit A at 0.038313 and 0.038314:
 *     R  NBIO.RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN  0x03780  00000000
 *     W  NBIO.RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN  0x03780  00000001
 *
 * One bit, BIF_DOORBELL_APER_EN, and nothing in milestone M5 works without it. */
void bc250_nbio_enable_doorbell_aperture(struct amdgpu_device *adev, bool enable)
{
	WREG32_FIELD15(NBIO, 0, RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN, BIF_DOORBELL_APER_EN,
		       enable ? 1 : 0);
}

/* The three in the order nv_common_hw_init() runs them (nv.c:1000-1009): set the remap, program it,
 * then open the doorbell aperture. Upstream's nv_program_aspm() and nbio_v2_3_init_registers() sit
 * between them and are not here - both go through the PCIE index/data pair rather than BAR5, which
 * is a different access path the shim does not provide, and neither touches GFX, SDMA or doorbells.
 * That omission is deliberate and is the reason this is not called bc250_nv_common_hw_init(). */
int bc250_nbio_hw_init(struct amdgpu_device *adev)
{
	if (adev == NULL)
		return BC250_EINVAL;

	bc250_nbio_set_reg_remap(adev);
	bc250_nbio_remap_hdp_registers(adev);
	bc250_nbio_enable_doorbell_aperture(adev, true);
	return 0;
}
