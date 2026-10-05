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

/* AMD stop_queue register body, not a complete reset/lifetime operation.
 * Caller serializes, owns any required RLC safe-mode entry/exit, and retains
 * all backing. Success leaves FREEZE set, F32 halted and UTC_L1 disabled.
 * Failure may also leave FREEZE set. Caller must pair unfreeze with proper
 * ring/translation restoration before admitting work. */
/* AMD per-engine reset register sequence only; caller composes the lifecycle.
 * Requires quiesced queues, serialized admission and retained mappings/backing.
 * Reestablish halt/queue/cache state or restore rings before subsequent use.
 * Success is not independent hardware reset/readiness proof. */
int bc250_sdma_soft_reset_instance(struct amdgpu_device *adev, u32 instance_id);
int bc250_sdma_quiesce_instance(struct amdgpu_device *adev, u32 instance);
int bc250_sdma_unfreeze_instance(struct amdgpu_device *adev, u32 instance);
/* Caller owns halted engines/backing; does not reset or reload firmware. */
int bc250_sdma_quiesce_for_reload(struct amdgpu_device *adev);
/* Quiesce/scope exit, per-engine reset, then a second quiescence/unfreeze scope.
 * Caller retains all backing and serializes admission through retirement. */
int bc250_sdma_reset_for_reload(struct amdgpu_device *adev);


/* [amdgpu] sdma_v5_0.c:218 sdma_v5_0_get_reg_offset(): the register window of one SDMA instance.
 * Exposed because bc250_irq.c needs it for SDMA0_CNTL, and two copies of an address calculation is
 * how the two drift apart. */
u32 bc250_sdma_reg_offset(struct amdgpu_device *adev, u32 instance, u32 internal_offset);

/* sdma_v5_0.c:1941-1944 sdma_v5_0_ring_funcs, the one copy: align_mask 0xf, nop
 * SDMA_PKT_NOP_HEADER_OP(SDMA_OP_NOP), AMDGPU_RING_TYPE_SDMA. Exposed for driver/shim/bc250_sdma_paging.c,
 * which needs the same align_mask a real SDMA0 ring uses to size its throwaway ring. */
const struct amdgpu_ring_funcs *bc250_sdma_ring_funcs(void);

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

/* sdma_v5_0_ring_emit_ib: includes position-dependent padding so the six-word
 * packet ends on an eight-DWORD boundary. Caller owns the IB/CSA mappings and
 * VMID setup until the actual outer fence, including after a timeout. */
// GFXHUB/SDMA0 engine0, nonzero VMID, local VRAM root. Does not commit or wait on CPU.
#define BC250_SDMA_VM_FLUSH_DWORDS 21u
#define BC250_SDMA_PAGING_VMID 2u
int bc250_sdma_emit_vm_flush(struct amdgpu_ring *ring, u32 vmid, u64 root_phys);
int bc250_sdma_submit_vm_ib(struct amdgpu_ring *ring, u64 root_phys, u64 gpu_addr, u32 length_dw,
                            u32 vmid, u64 csa_addr, u64 fence_addr, u64 seq, unsigned int flags);

unsigned int bc250_sdma_ib_size(const struct amdgpu_ring *ring);
int bc250_sdma_emit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw,
                       u32 vmid, u64 csa_addr);
int bc250_sdma_submit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw,
                         u32 vmid, u64 csa_addr, u64 fence_addr, u64 seq,
                         unsigned int flags);

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
/* The same test, split (review 23 MUST-FIX) so a caller that must hold a lock across the ring push
 * (ADR 0008 stage D's Sdma0RingLock) is not forced to hold it across the poll too: _submit() does the
 * WRITE_LINEAR and commit and hands back the scratch slot to watch; _wait() polls it for up to
 * adev->usec_timeout. bc250_sdma_ring_test() above is unchanged in behaviour, now built from these two. */
int bc250_sdma_ring_test_submit(struct amdgpu_ring *ring, volatile u32 **out_slot_cpu);
int bc250_sdma_ring_test_wait(struct amdgpu_device *adev, volatile u32 *slot_cpu);

/* ---------------------------------------------------------------------------------------------
 * Copy and fill (ADR 0013): the two packets BuildPagingBuffer will need once node 1 is wired into
 * the full WDDM table as the paging node, ahead of that wiring. Not a positive control on the
 * table - a positive control that never touches it (docs/adr/0013-node-layout-3d-plus-sdma.md).
 *
 * reference/sdma_v5_0.c:2018 sdma_v5_0_emit_copy_buffer() and :2045 sdma_v5_0_emit_fill_buffer(),
 * split into packets the way amdgpu_copy_buffer() and amdgpu_ttm_fill_mem() (amdgpu_ttm.c, same
 * tag) split them: one packet per up-to-copy_max_bytes (fill_max_bytes) chunk, both 0x400000 on
 * this engine (sdma_v5_0_buffer_funcs, reference/sdma_v5_0.c:2057-2065). amdgpu_ttm.c is not
 * imported - it drags in the TTM job, fence and reservation machinery the same way amdgpu_gfx.c
 * does for the CP side, which driver/amdgpu-import/PROVENANCE.md already explains staying clear
 * of - so the three-line splitting loop is transcribed instead of imported.
 * driver/shim/bc250_sdma_copy.c.
 * ------------------------------------------------------------------------------------------- */

/* Dwords one call of bc250_sdma_emit_copy_linear()/bc250_sdma_emit_fill() will write for this many
 * bytes: the split packet count times the upstream dwords-per-packet (7, 5;
 * sdma_v5_0_buffer_funcs.copy_num_dw/.fill_num_dw). 0 if bytes is 0, so a caller can add it
 * straight into an amdgpu_ring_alloc() budget and treat 0 as "nothing to send". */
unsigned int bc250_sdma_copy_linear_size(unsigned int bytes);
unsigned int bc250_sdma_fill_size(unsigned int bytes);

/* reference/sdma_v5_0.c:2018 sdma_v5_0_emit_copy_buffer(), looped as amdgpu_copy_buffer()
 * (amdgpu_ttm.c) loops it. Writes into a ring the caller has already reserved
 * bc250_sdma_copy_linear_size(bytes) dwords in with amdgpu_ring_alloc(); does not commit. Returns
 * BC250_EINVAL without writing a dword if bytes is 0 or the ring is not an SDMA ring. copy_flags is
 * not a parameter: this driver never asks for TMZ, so SDMA_PKT_COPY_LINEAR_HEADER_TMZ is always 0,
 * exactly where upstream's own callers leave it when they do not pass AMDGPU_COPY_FLAGS_TMZ. */
int bc250_sdma_emit_copy_linear(struct amdgpu_ring *ring, u64 src_mc, u64 dst_mc, unsigned int bytes);

/* reference/sdma_v5_0.c:2045 sdma_v5_0_emit_fill_buffer(), split the same way
 * (amdgpu_ttm_fill_mem(), amdgpu_ttm.c, fill_max_bytes). Same contract as the copy above. */
int bc250_sdma_emit_fill(struct amdgpu_ring *ring, u64 dst_mc, u32 value, unsigned int bytes);

/* [shim] the positive control, one allocation and one commit: fill(src, pattern) -> copy(src ->
 * dst) -> the same fence bc250_sdma_signal_fence() emits. Does not poll - the caller reads the
 * fence slot back exactly as it does after bc250_sdma_signal_fence(), through
 * bc250_sdma_fence_addr()/bc250_sdma_fence_read(), which is what "returns what to poll" means here:
 * nothing new to poll, the existing fence machinery is it. Returns BC250_EINVAL without writing
 * anything on a bad argument, undoes the reservation and returns the emitter's or amdgpu_ring_alloc's
 * code if either fails partway. */
int bc250_sdma_copy_test(struct amdgpu_ring *ring, u64 src_mc, u64 dst_mc, unsigned int bytes,
                         u32 pattern, u64 fence_addr, u64 seq, unsigned int flags);

/* [shim] the two VRAM scratch regions BC250_ESCAPE_RUN_SDMACOPY needs, out of the same VRAM pool
 * bc250_gfx_fence_page_alloc() and friends allocate from (bc250_shim_mem_alloc(), BC250_MEM_VRAM).
 * Both or neither: a partial allocation is freed before this returns, exactly as
 * bc250_sdma_fence_page_alloc() leaves nothing half-allocated behind. Returns BC250_EINVAL on a bad
 * argument, or bc250_shim_mem_alloc()'s own code (typically BC250_ENOMEM) if the pool has no room. */
int bc250_sdma_copy_regions_alloc(struct amdgpu_device *adev, unsigned int bytes,
                                  struct bc250_mem *src, struct bc250_mem *dst);
void bc250_sdma_copy_regions_free(struct amdgpu_device *adev, struct bc250_mem *src, struct bc250_mem *dst);

#endif /* BC250_SDMA_H */
