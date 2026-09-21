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

/*
 * sdma_v5_0.c:1480 sdma_v5_0_hw_fini(): stop context switching, then halt the engines. Does not
 * free memory, and always runs the whole halt sequence whatever it finds.
 *
 * [shim] Returns 0, or BC250_EBUSY if an engine halted with its read pointer still behind its write
 * pointer. HALT says the engine stopped, not that it drained, and an engine that stopped mid-queue
 * is still holding packets that name a ring, a fence slot or a copy destination the caller is about
 * to hand back. The caller decides what to do with that; driver/kmd/gfx.c's Fini() keeps the pages.
 * Both engines are read and both are logged; the return is the first failure.
 */
int bc250_sdma_hw_fini(struct amdgpu_device *adev);

/* [amdgpu] sdma_v5_0.c:218 sdma_v5_0_get_reg_offset(): the register window of one SDMA instance.
 * Exposed because bc250_irq.c needs it for SDMA0_CNTL, and two copies of an address calculation is
 * how the two drift apart. */
u32 bc250_sdma_reg_offset(struct amdgpu_device *adev, u32 instance, u32 internal_offset);

/* ---------------------------------------------------------------------------------------------
 * Fences and the ring test (milestone M6)
 *
 * The SDMA half of what bc250_gfx.h declares for the CP rings, and the same flags: the two
 * AMDGPU_FENCE_FLAG_* bits are amdgpu_ring.h's, declared once in amdgpu.h.
 *
 * Which vector a fence produces. An SDMA fence with AMDGPU_FENCE_FLAG_INT ends in an SDMA_OP_TRAP,
 * and the trap arrives as source id 224 under client id SOC15_IH_CLIENTID_SDMA0 or _SDMA1 by
 * engine - not under one client with the engine in ring_id, which is how the CP does it.
 * bc250_ih_is_sdma_trap() is the routing helper, and it hands back the instance.
 * Measured on unit A: evidence/linux/2026-09-21-E13-reference-2/boot3-readonly/amdgpu-events-ib.txt
 * lines 24 and 25, client_id 8 src_id 224 and client_id 9 src_id 224, both with ring 0.
 * ------------------------------------------------------------------------------------------- */

/* Slots 0 and 1 are the two engines' ring-test scratch dwords; 2 upwards are fence slots. One page
 * of eight-byte slots, as bc250_gfx.h cuts its own. */
#define BC250_SDMA_FENCE_SLOTS	16u

/* How many dwords bc250_sdma_emit_fence() will write for these flags. 0 if the ring is not an SDMA
 * ring, which is the same refusal the emitter makes. */
unsigned int bc250_sdma_fence_size(const struct amdgpu_ring *ring, unsigned int flags);

/* sdma_v5_0.c:523 sdma_v5_0_ring_emit_fence(). Writes into a ring the caller has already reserved
 * space in with amdgpu_ring_alloc(); it does not commit. Returns BC250_EINVAL without writing a
 * single dword if the ring is wrong or the address is not 4-byte aligned. */
int bc250_sdma_emit_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags);

/* [shim] the alloc, the emit and the commit together, which is what a caller actually wants. On a
 * refusal from the emitter the reservation is undone and the ring is left as it was. */
int bc250_sdma_signal_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags);

/* The scratch and fence page. Not allocated by bc250_sdma_setup(); whoever wants a fence or a ring
 * test allocates it, and frees it before bc250_sdma_teardown(). Idempotent. */
int bc250_sdma_fence_page_alloc(struct amdgpu_device *adev);
void bc250_sdma_fence_page_free(struct amdgpu_device *adev);

/* The GPU address of a slot, and the 64 bits the engine last wrote there. Both return 0 if the page
 * is not allocated or the slot is out of range. */
u64 bc250_sdma_fence_addr(struct amdgpu_device *adev, unsigned int slot);
u64 bc250_sdma_fence_read(struct amdgpu_device *adev, unsigned int slot);

/* sdma_v5_0.c:1012 sdma_v5_0_ring_test_ring(): a WRITE_LINEAR of one dword into this engine's
 * scratch slot, then a poll. Needs the page above. Returns 0, BC250_EINVAL or BC250_ETIME. */
int bc250_sdma_ring_test(struct amdgpu_ring *ring);

#endif /* BC250_SDMA_H */
