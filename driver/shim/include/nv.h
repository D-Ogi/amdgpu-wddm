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

#endif /* BC250_SHIM_NV_H */
