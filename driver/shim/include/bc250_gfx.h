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
 * the KIQ for the gfx and compute rings, the nine queues' pointer registers put back, the KIQ's own
 * HQD dequeued while the MEC still runs, the CP and MEC halted, then the RLC stopped.
 *
 * Frees nothing. After it, bc250_gfx_hw_init() can run again on the same adev with no PSP reload,
 * which is the point: a second PSP load in one boot leaves the RLC disabled and busy (facts M35).
 *
 * Returns the first failure, or 0 if the whole undo ran. Every step is attempted whatever the
 * earlier ones did, so the value is for the caller to report and not to act on: there is nothing
 * left to try. The one that matters is BC250_ETIME from the KIQ dequeue, which says the MEC did not
 * answer the handshake and the next bring-up will have to recover instead (bc250_kiq_dequeue()).
 *
 * The RLC stop and the two queue steps are ours, not upstream's - see the comments on the functions
 * for why they are here.
 */
int bc250_gfx_hw_fini(struct amdgpu_device *adev);

/* Stop the RLC and nothing else. For the kmd, before a second PSP firmware load in one boot. */
void bc250_gfx_rlc_stop(struct amdgpu_device *adev);

/* ---------------------------------------------------------------------------------------------
 * The fence: raising one end-of-pipe interrupt on purpose (milestone M6)
 *
 * This is the smallest submission that makes the hardware both write a value the driver can read
 * and raise an interrupt, which together are the proof that the rings this file brought up and the
 * interrupt ring in bc250_ih.c are connected to each other. Nothing above needs it; it exists to be
 * fired once, by hand, with the IH ring already up.
 *
 * gfx and compute rings get a RELEASE_MEM (gfx_v10_0.c:8712); the KIQ gets two WRITE_DATA packets
 * (gfx_v10_0.c:8780), because it has no end-of-pipe and signals by writing CPC_INT_STATUS.
 *
 * What comes back on the interrupt ring:
 *   gfx ring      client SOC15_IH_CLIENTID_GRBM_CP, src GFX_10_1__SRCID__CP_EOP_INTERRUPT,
 *                 ring_id me 0   -> bc250_ih_is_gfx_eop()
 *   compute ring  the same client and source, ring_id me 1 -> bc250_ih_is_compute_eop(), with
 *                 pipe and queue in the rest of ring_id (bc250_ih_eop_ring_id())
 *   KIQ           src GFX_10_1__SRCID__CP_IB2_INTERRUPT_PKT -> bc250_ih_is_kiq()
 * The matching enable has to be on first: bc250_irq_hw_init(), or one of the bc250_irq_set_*()
 * calls for the single ring being fired.
 * ------------------------------------------------------------------------------------------- */

/* Flags are AMDGPU_FENCE_FLAG_64BIT and AMDGPU_FENCE_FLAG_INT, in amdgpu.h. Without _INT the packet
 * still writes the value but raises nothing, which is the useful control: the same code path, one
 * bit different, and no interrupt should arrive. */

/* How many dwords bc250_gfx_emit_fence() will write for this ring and these flags. */
unsigned int bc250_gfx_fence_size(const struct amdgpu_ring *ring, unsigned int flags);

/* Emit into a ring amdgpu_ring_alloc() has already reserved space in. Returns 0, or BC250_EINVAL
 * with nothing written: a misaligned `addr` (4 bytes, or 8 with _64BIT), or _64BIT on the KIQ,
 * which upstream BUG_ON()s in both cases. */
int bc250_gfx_emit_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags);

/* alloc + emit + commit. The one call the miniport makes; the doorbell is rung on return. */
int bc250_gfx_signal_fence(struct amdgpu_ring *ring, u64 addr, u64 seq, unsigned int flags);

/*
 * The GTT slots the fence value lands in, one page cut into 8-byte slots so that a 64-bit fence is
 * legal in any of them. Allocated on its own, not by bc250_gfx_setup(): the traced bring-up does
 * not need it, and allocating it there would move every MC address that window programs.
 *
 * The slot number is the caller's to choose - there is no per-ring slot, because there is no fence
 * driver here to own one. Ring index is the obvious choice and is what the replay test uses.
 */
#define BC250_GFX_FENCE_SLOTS	16u

int  bc250_gfx_fence_page_alloc(struct amdgpu_device *adev);
void bc250_gfx_fence_page_free(struct amdgpu_device *adev);

/* The MC address to pass as `addr`, or 0 if the page is not allocated or the slot is out of range. */
u64  bc250_gfx_fence_addr(struct amdgpu_device *adev, unsigned int slot);

/* What the CP wrote there. A 32-bit fence leaves the upper half at whatever it was, so a caller
 * that did not pass _64BIT should look at the low 32 bits only. */
u64  bc250_gfx_fence_read(struct amdgpu_device *adev, unsigned int slot);

/* One ring test: write 0xCAFEDEAD to SCRATCH_REG0, submit a SET_UCONFIG_REG packet writing
 * 0xDEADBEEF, poll for it. gfx_v10_0.c:4033 gfx_v10_0_ring_test_ring(). Returns 0 or BC250_ETIME.
 * Exposed because it is the only part of the sequence that needs a live CP, so a host replay has
 * to account for it explicitly rather than quietly. */
int bc250_gfx_ring_test(struct amdgpu_ring *ring);

/* ---------------------------------------------------------------------------------------------
 * The indirect buffer (ADR 0008 stage C)
 *
 * Everything the driver has submitted so far was written into the ring itself. An IB is a pointer
 * to packets somewhere else, fetched by the CP through a VMID - which is what makes it the one
 * packet that can reach a process's own address space, and the reason it comes before anything
 * that involves a page table of VidMm's. The long form of what is emitted and what upstream emits
 * around it is at the head of the implementation in bc250_gfx.c.
 *
 * The gfx ring only. gfx_v10_0_ring_emit_ib_compute() is a different packet (it sets
 * INDIRECT_BUFFER_VALID) and nothing here submits an IB on a compute ring, so it is not
 * transcribed and the functions below refuse any other ring type.
 * ------------------------------------------------------------------------------------------- */

/* How many dwords bc250_gfx_emit_ib() writes: 4 on a gfx ring, 0 on anything else, which is also
 * how a caller asks whether this ring takes an IB at all. */
unsigned int bc250_gfx_ib_size(const struct amdgpu_ring *ring);

/* Emit into a ring amdgpu_ring_alloc() has already reserved space in. Returns 0, or BC250_EINVAL
 * with nothing written: a gpu_addr that is not dword aligned (which upstream BUG_ON()s), a
 * length_dw of 0 or beyond the packet's 20-bit field, a vmid of 16 or more, or a ring that is not
 * a gfx ring. */
int bc250_gfx_emit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw, u32 vmid);

/* alloc + emit_ib + emit_fence + commit: the one call the miniport makes for a submission, the
 * shape of bc250_gfx_signal_fence(). The doorbell is rung on return and nothing is waited for.
 * `flags` is the fence's: AMDGPU_FENCE_FLAG_INT for an end-of-pipe interrupt behind the IB. */
int bc250_gfx_submit_ib(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw, u32 vmid,
			u64 fence_addr, u64 seq, unsigned int flags);

/* A real job, not the ring test. amdgpu_ib_schedule's order for a gfx job whose
 * ib_flags do not ask for a memory sync: PFP_SYNC_ME, CONTEXT_CONTROL with the
 * context-switch load bits, FRAME_CONTROL start (non-TMZ), the IB, FRAME_CONTROL
 * end, the same RELEASE_MEM fence, SWITCH_BUFFER. VMID 0 stays on submit_ib. */
int bc250_gfx_submit_job(struct amdgpu_ring *ring, u64 gpu_addr, u32 length_dw, u32 vmid,
			 u64 fence_addr, u64 seq, unsigned int flags);

/* The first SET_SH_REG of COMPUTE_PGM_LO in a PM4 dword stream. *hi_written is 0 when the
 * packet stored only LO, which leaves COMPUTE_PGM_HI at whatever the preamble wrote.
 * *byte_addr is (hi << 40) | (lo << 8) when HI was in the packet, otherwise just lo << 8.
 * Returns 1 when the register was found. */
int bc250_pm4_shader_addr(const u32 *dw, u32 ndw, u64 *byte_addr, u32 *lo, u32 *hi,
			  u32 *hi_written);

/* One GTT page to build an indirect buffer in, and the ring test as an indirect buffer: the first
 * submission of stage C, whose result is already known from the same hardware by another route.
 * bc250_gfx_ib_ring_test_build() seeds SCRATCH_REG0 with 0xCAFEDEAD and writes the three dwords;
 * bc250_gfx_ib_ring_test_result() returns 0 once the register holds 0xDEADBEEF, BC250_ETIME while
 * it does not. Both read or write one register, so both belong to the caller's sequence. */
int  bc250_gfx_ib_page_alloc(struct amdgpu_device *adev);
void bc250_gfx_ib_page_free(struct amdgpu_device *adev);
u64  bc250_gfx_ib_addr(const struct amdgpu_device *adev);
int  bc250_gfx_ib_ring_test_build(struct amdgpu_device *adev, u32 *length_dw);
int  bc250_gfx_ib_ring_test_result(struct amdgpu_device *adev);

#endif /* BC250_GFX_H */
