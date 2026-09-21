/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * GFX10 CP, KIQ and ring bring-up for Cyan Skillfish (milestone M5 part B, ADR 0002).
 *
 * This is what amdgpu's gfx_v10_0_hw_init() does on this part, minus everything that is not the
 * hardware: no firmware loading (the PSP has already done it), no scheduler, no interrupts, no
 * buffer-object manager. Every function in bc250_gfx.c names the upstream function and line it
 * follows, at kernel tag v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449. The upstream file
 * itself is in driver/amdgpu-import/gfx_v10_0.c, reference only, so those citations can be checked
 * without a kernel checkout.
 *
 * Why this is transcribed rather than imported: gfx_v10_0.c calls about ninety functions it does
 * not define - the BO allocator, the writeback allocator, amdgpu_ring_init, the DRM scheduler,
 * dma_fence, request_firmware, irq registration, sysfs, debugfs, delayed work. See
 * driver/amdgpu-import/PROVENANCE.md. The data it uses - the golden register table, the clear-state
 * tables, the MQD layouts, the PM4 definitions - is imported unmodified and used as it is.
 *
 * The caller must:
 *   - zero `adev` and run bc250_gmc_setup() and bc250_gmc_gart_enable() first (the rings and their
 *     writeback slots live in GART memory, so the GART has to be up);
 *   - provide bc250_shim_mem_alloc/free, bc250_shim_wdoorbell64 and the M4 backend functions;
 *   - serialize: none of this takes a lock. Upstream holds adev->srbm_mutex around every GRBM
 *     select, adev->grbm_idx_mutex around the RB discovery and kiq->ring_lock around KIQ
 *     submission. A second thread touching GRBM_GFX_CNTL or the KIQ ring during bring-up would
 *     corrupt both.
 */
#ifndef BC250_GFX_H
#define BC250_GFX_H

#include "amdgpu.h"

/* Negative return codes, the errno values upstream returns, so that a caller that logs the number
 * sees the same number amdgpu would have logged. bc250_gmc.h defines EINVAL and ETIME. */
#define BC250_ENOMEM	(-12)
#define BC250_EIO	(-5)

/*
 * What bc250_gfx_setup() cannot read off the hardware.
 *
 * The first four come from the GC discovery table, which the driver reads from the IP discovery
 * ROM. On unit A they are in the kernel log of experiment E03
 * (evidence/linux/2026-09-21-E03-init-trace/dmesg.txt:1038):
 *
 *     amdgpu: SE 2, SH per SE 2, CU per SH 10, active_cu_number 24
 *
 * `max_backends_per_se` is not in that line and is not observable in the register trace either: it
 * only scales the mask in gfx_v10_0_get_rb_active_bitmap() and, through num_rbs, the
 * PA_SC_TILE_STEERING_OVERRIDE value that ends up in the clear-state PM4 stream. It is declared
 * here rather than guessed inside the code, and the replay test says what it was given.
 *
 * The two booleans are Linux module parameters. Both are stated by the trace, not assumed:
 *   async_gfx_ring  the window has no CP_RB0_BASE/CP_RB0_CNTL programming at all, only CP_GFX_MQD
 *                   reads and a KIQ MAP_QUEUES, which is the async path. Upstream default is 1.
 *   pp_gfxoff       gfx_v10_0_rlc_start() writes RLC_PG_CNTL a second time when this is false. The
 *                   trace has exactly one RLC_PG_CNTL write, the zero from rlc_resume, so it was
 *                   true. Upstream default is on.
 */
struct bc250_gfx_inputs {
	u32  max_shader_engines;
	u32  max_sh_per_se;
	u32  max_cu_per_sh;
	u32  max_backends_per_se;

	bool async_gfx_ring;
	bool pp_gfxoff;
};

/*
 * Fill adev's gfx state and allocate everything the CP needs: the KIQ, the eight compute queues
 * and the gfx queue with their ring buffers, MQDs and EOP buffers, and the RLC clear-state buffer.
 * Reads registers, writes none. Returns 0 or a negative code; on failure it has already released
 * whatever it managed to allocate.
 *
 * This stands in for the parts of gfx_v10_0_early_init() and gfx_v10_0_sw_init() that survive on
 * this part, which are exactly the ones that decide sizes and addresses.
 */
int bc250_gfx_setup(struct amdgpu_device *adev, const struct bc250_gfx_inputs *in);

/* Release what bc250_gfx_setup() allocated. Safe on a partially set up adev. */
void bc250_gfx_teardown(struct amdgpu_device *adev);

/*
 * The bring-up, in gfx_v10_0_hw_init()'s order (gfx_v10_0.c:7474):
 *   golden registers -> GRBM CAM probe -> constants -> RLC -> CP.
 * Returns 0 or a negative code.
 */
int bc250_gfx_hw_init(struct amdgpu_device *adev);

/* The stages, exposed so the replay test can compare them one at a time and so the miniport can
 * stop after any of them. bc250_gfx_hw_init() is these five in order. */
int bc250_gfx_init_golden_registers(struct amdgpu_device *adev);
int bc250_gfx_grbm_cam_probe(struct amdgpu_device *adev, bool *already_remapped);
int bc250_gfx_constants_init(struct amdgpu_device *adev);
int bc250_gfx_rlc_resume(struct amdgpu_device *adev);
int bc250_gfx_cp_resume(struct amdgpu_device *adev);

/*
 * The undo, in gfx_v10_0_hw_fini()'s order: the three fault interrupts off, UNMAP_QUEUES through
 * the KIQ for the gfx and compute rings, the CP and MEC halted, then the RLC stopped.
 *
 * Frees nothing. After it, bc250_gfx_hw_init() can run again on the same adev with no PSP reload,
 * which is the point: a second PSP load in one boot leaves the RLC disabled and busy (facts M35).
 *
 * The RLC stop is ours, not upstream's - see the comment on the function for why it is here.
 */
void bc250_gfx_hw_fini(struct amdgpu_device *adev);

/* Stop the RLC and nothing else. For the kmd, before a second PSP firmware load in one boot. */
void bc250_gfx_rlc_stop(struct amdgpu_device *adev);

/* One ring test: write 0xCAFEDEAD to SCRATCH_REG0, submit a SET_UCONFIG_REG packet writing
 * 0xDEADBEEF, poll for it. gfx_v10_0.c:4033 gfx_v10_0_ring_test_ring(). Returns 0 or BC250_ETIME.
 * Exposed because it is the only part of the sequence that needs a live CP, so a host replay has
 * to account for it explicitly rather than quietly. */
int bc250_gfx_ring_test(struct amdgpu_ring *ring);

#endif /* BC250_GFX_H */
