/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Stand-in for drivers/gpu/drm/amd/amdgpu/nv.h. cyan_skillfish_reg_init.c includes it for the
 * prototype of the one function it defines; the real nv.h also declares the whole NV ASIC family
 * (soc15 common IP setup, PCIe/SMN accessors, ASIC reset), none of which is imported yet.
 */
#ifndef BC250_SHIM_NV_H
#define BC250_SHIM_NV_H

#include "amdgpu.h"

int cyan_skillfish_reg_base_init(struct amdgpu_device *adev);

/* [amdgpu] nv.c:317 nv_grbm_select(): point GRBM_GFX_CNTL at one me/pipe/queue/vmid, so that the
 * CP_HQD_* and SH_MEM_* register windows address that one queue or VMID. One WREG32_SOC15; nv.c
 * itself is not importable (it drags in the whole NV SoC IP-block registration, atombios, smu,
 * vcn and jpeg). Implemented in driver/shim/bc250_gfx.c. */
void nv_grbm_select(struct amdgpu_device *adev, u32 me, u32 pipe, u32 queue, u32 vmid);

#endif /* BC250_SHIM_NV_H */
