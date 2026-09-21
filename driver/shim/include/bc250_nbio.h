/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The two NBIO 2.3 registers this milestone writes (ADR 0002).
 *
 * Transcribed from nbio_v2_3.c at kernel tag v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449. That file is not in driver/amdgpu-import/ because these
 * two functions are all of it we need: the rest is PCIe link training, clock gating, the RAS
 * controller and the HDP register remap, none of which this sequence touches.
 *
 * Both are doorbell plumbing. GFX10 compute and KIQ rings have no MMIO write-pointer path at all
 * (gfx_v10_0_ring_set_wptr_compute() calls BUG() if the ring has no doorbell), so the doorbell
 * aperture has to be right before any of the rings can be driven.
 */
#ifndef BC250_NBIO_H
#define BC250_NBIO_H

#include "amdgpu.h"

/* [amdgpu] nbio_v2_3.c:551 MMIO_REG_HOLE_OFFSET, (0x80000 - PAGE_SIZE) with a 4 KB page. BAR5 on
 * 1002:13FE is 512 KB (facts M14), so this is its last page. */
#define BC250_MMIO_REG_HOLE_OFFSET	(0x80000u - 4096u)

/*
 * Stage 0, from nv_common_hw_init(). Run once per boot, before anything rings a doorbell.
 *
 * bc250_nbio_hw_init() is the one the miniport calls; the three below it are separate because they
 * are separate upstream and because a caller that only wants the doorbell aperture back after a
 * suspend should not have to take the rest. See bc250_nbio.c for the traced accesses behind each.
 */
int  bc250_nbio_hw_init(struct amdgpu_device *adev);
void bc250_nbio_set_reg_remap(struct amdgpu_device *adev);
void bc250_nbio_remap_hdp_registers(struct amdgpu_device *adev);
void bc250_nbio_enable_doorbell_aperture(struct amdgpu_device *adev, bool enable);

/* [amdgpu] nbio_v2_3.c:189 nbio_v2_3_sdma_doorbell_range(). Called by the SDMA ring setup, once per
 * instance, with doorbell_size 20. */
void bc250_nbio_sdma_doorbell_range(struct amdgpu_device *adev, int instance,
				    bool use_doorbell, int doorbell_index,
				    int doorbell_size);

/* [amdgpu] nbio_v2_3.c:162 nbio_v2_3_enable_doorbell_selfring_aperture(). Points the self-ring
 * aperture at adev->doorbell.base, so that the GPU can ring its own doorbells through the GPA
 * window. The caller must have filled in adev->doorbell.base: on Windows that is the physical
 * address of the doorbell BAR out of the miniport's translated resource list, and on unit A the
 * trace shows it as 0xD0000000.
 *
 * Upstream calls this from nv_common_hw_init(), not from the gfx or SDMA IP block, which is why it
 * is exposed rather than folded into either.
 *
 * Deviation from upstream: this returns int and refuses an enable with adev->doorbell.base == 0
 * (BC250_EINVAL, no register written). Upstream returns void because on Linux the field is filled by
 * amdgpu_device_doorbell_init() long before any IP block runs; in the miniport it is 0 until the
 * caller reads it out of the translated resource list, and EN = 1 over a zero base would point the
 * window the GPU writes its own doorbells through at physical address 0. Refusing is the same rule
 * bc250_psp.c applies to its own missing preconditions. */
int bc250_nbio_enable_doorbell_selfring_aperture(struct amdgpu_device *adev, bool enable);

#endif /* BC250_NBIO_H */
