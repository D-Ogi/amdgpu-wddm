/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * SDMA 5.0 ring bring-up for Cyan Skillfish (milestone M5 part B, stage 9, ADR 0002).
 *
 * This is what amdgpu's sdma_v5_0_hw_init() does on this part: the golden register table, unhalt,
 * context switching, and the two GFX ring buffers with their doorbells and UTCL1 settings. It is
 * transcribed from driver/amdgpu-import/sdma_v5_0.c, an unmodified copy of the kernel's
 * drivers/gpu/drm/amd/amdgpu/sdma_v5_0.c at tag v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449, for the same reason gfx_v10_0.c is transcribed rather
 * than imported: the file cannot compile outside the kernel (see driver/amdgpu-import/PROVENANCE.md).
 * Every function below names the upstream function and line it follows.
 *
 * Not here, and why:
 *   - firmware loading. The AMDGPU_FW_LOAD_DIRECT branch of sdma_v5_0_start() calls
 *     sdma_v5_0_load_microcode(); on this part the PSP has already loaded SDMA0 and SDMA1
 *     (M5 part A), so that branch is not this path.
 *   - the RLC queues. sdma_v5_0_rlc_resume() is a stub upstream ("XXX todo") and the trace has no
 *     SDMA*_RLC* access beyond the golden table, so there is nothing to follow.
 *   - the ring test. sdma_v5_0_ring_test_ring() writes an SDMA WRITE packet and polls memory; it
 *     touches no register, so a register replay cannot check it, and emitting the packet would
 *     mean importing navi10_sdma_pkt_open.h for one opcode. The miniport needs it before it can
 *     trust the engines; it is called out here rather than written as something the replay would
 *     silently not cover.
 *
 * The caller must run bc250_gmc_setup() and bc250_gmc_gart_enable() first: the ring buffers and
 * their writeback slots are GART memory. Nothing here takes a lock.
 */
#ifndef BC250_SDMA_H
#define BC250_SDMA_H

#include "amdgpu.h"

/*
 * Allocate the two ring buffers and the writeback page, and fill in adev->sdma. Reads and writes no
 * register. Stands in for the part of sdma_v5_0_sw_init() that survives: the doorbell index, the
 * ring size and the writeback slots.
 *
 * Returns 0 or a negative code; on failure it has already released what it allocated.
 */
int bc250_sdma_setup(struct amdgpu_device *adev);

/* Release what bc250_sdma_setup() allocated. Safe on a partially set up adev. */
void bc250_sdma_teardown(struct amdgpu_device *adev);

/* sdma_v5_0.c:1468 sdma_v5_0_hw_init(): golden registers, then start. */
int bc250_sdma_hw_init(struct amdgpu_device *adev);

/* The two halves, exposed so the replay test can compare them one at a time. */
int bc250_sdma_init_golden_registers(struct amdgpu_device *adev);
int bc250_sdma_start(struct amdgpu_device *adev);

/* sdma_v5_0.c:1480 sdma_v5_0_hw_fini(): stop context switching, then halt the engines. Does not
 * free memory. */
void bc250_sdma_hw_fini(struct amdgpu_device *adev);

/* [amdgpu] sdma_v5_0.c:218 sdma_v5_0_get_reg_offset(): the register window of one SDMA instance.
 * Exposed because bc250_irq.c needs it for SDMA0_CNTL, and two copies of an address calculation is
 * how the two drift apart. */
u32 bc250_sdma_reg_offset(struct amdgpu_device *adev, u32 instance, u32 internal_offset);

#endif /* BC250_SDMA_H */
