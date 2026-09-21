/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The few shim entry points that cannot be macros. Everything here is backend-independent: the
 * register traffic itself goes to bc250_shim_rreg / bc250_shim_wreg, which a backend provides
 * (driver/shim/test/backend_trace.c on the host, driver/kmd later).
 */
#include "amdgpu.h"

/* SR-IOV. The BC-250's GPU is a bare-metal PCI function (1002:13FE, facts M01); there is no
 * virtual function and no RLC register-access path. These stay functions rather than constants so
 * that the `if (amdgpu_sriov_vf(adev))` early-outs inside the imported files keep their shape. */
bool amdgpu_sriov_vf(struct amdgpu_device *adev)
{
	(void)adev;
	return false;
}

bool amdgpu_sriov_runtime(struct amdgpu_device *adev)
{
	(void)adev;
	return false;
}

bool amdgpu_sriov_fullaccess(struct amdgpu_device *adev)
{
	(void)adev;
	return false;
}

/* Never reached while amdgpu_sriov_vf() is false. They exist because soc15_common.h names them in
 * the untaken branch of its register macros. */
void amdgpu_sriov_wreg(struct amdgpu_device *adev, u32 offset, u32 value,
		       u32 acc_flags, u32 hwip, u32 xcc_id)
{
	(void)adev; (void)offset; (void)value; (void)acc_flags; (void)hwip; (void)xcc_id;
	bc250_shim_log(2, NULL, "shim: amdgpu_sriov_wreg called, but this is not an SR-IOV function\n");
}

u32 amdgpu_sriov_rreg(struct amdgpu_device *adev, u32 offset, u32 acc_flags,
		      u32 hwip, u32 xcc_id)
{
	(void)adev; (void)offset; (void)acc_flags; (void)hwip; (void)xcc_id;
	bc250_shim_log(2, NULL, "shim: amdgpu_sriov_rreg called, but this is not an SR-IOV function\n");
	return 0;
}

/*
 * amdgpu_gmc_vram_mc2pa - copied from amdgpu_gmc.c (MIT), unchanged:
 *
 *	return mc_addr - adev->gmc.vram_start + adev->vm_manager.vram_base_offset;
 */
u64 amdgpu_gmc_vram_mc2pa(struct amdgpu_device *adev, u64 mc_addr)
{
	return mc_addr - adev->gmc.vram_start + adev->vm_manager.vram_base_offset;
}

/*
 * amdgpu_gmc_pd_addr - MC address of the page directory root, with its flags.
 *
 * Upstream this walks TTM: amdgpu_gmc_pd_addr() -> amdgpu_gmc_get_pde_for_bo() ->
 * amdgpu_bo_gpu_offset() + amdgpu_ttm_tt_pde_flags() + adev->gmc.gmc_funcs->get_vm_pde().
 *
 * Deviation from Linux (ADR 0002): the shim has no TTM and no memory manager, so the three steps
 * are collapsed into what they come out as on GMC v10 with a VRAM-resident, non-system page
 * directory, which is the only case a GART table hits:
 *
 *   amdgpu_ttm_tt_pde_flags()  -> AMDGPU_PTE_VALID (the buffer is in VRAM and resident)
 *   gmc_v10_0_get_vm_pde()     -> *addr = adev->vm_manager.vram_base_offset
 *                                        + *addr - adev->gmc.vram_start
 *   amdgpu_gmc_pd_addr()       -> pd_addr | flags
 *
 * which is exactly amdgpu_gmc_vram_mc2pa() of the buffer's MC address, plus the valid bit.
 * The caller fills bo->gpu_addr with the MC address the allocator gave the GART table.
 */
u64 amdgpu_gmc_pd_addr(struct amdgpu_bo *bo)
{
	u64 pd_addr = amdgpu_gmc_vram_mc2pa(bo->adev, bo->gpu_addr);

	return pd_addr | AMDGPU_PTE_VALID;
}
