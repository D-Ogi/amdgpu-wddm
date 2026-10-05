/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * CP and SDMA interrupt enabling for Cyan Skillfish (milestone M5 part B, stage 10, ADR 0002).
 *
 * This is the register half of what Linux does through amdgpu_irq_get(): the callbacks that turn
 * one interrupt source on in the CP and SDMA blocks. The refcounting, the IH ring, the source
 * registration and the handlers are not here - on Windows the interrupt arrives through the
 * miniport's own ISR/DPC path, and only these register writes are the same on both systems.
 *
 * The functions follow, at kernel tag v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449:
 *   gfx_v10_0_set_gfx_eop_interrupt_state()      gfx_v10_0.c:9051
 *   gfx_v10_0_set_compute_eop_interrupt_state()  gfx_v10_0.c:9092
 *   gfx_v10_0_kiq_set_interrupt_state()          gfx_v10_0.c:9411
 *   gfx_v10_0_set_priv_reg_fault_state()         gfx_v10_0.c:9227
 *   gfx_v10_0_set_bad_op_fault_state()           gfx_v10_0.c:9273
 *   gfx_v10_0_set_priv_inst_fault_state()        gfx_v10_0.c:9318
 *   sdma_v5_0_set_trap_irq_state()               sdma_v5_0.c:1681
 *   kgd_init_interrupts()                        amdgpu_amdkfd_gfx_v10.c:140
 *
 * The last one is amdkfd's, not amdgpu's. On Linux the compute pipes belong to KFD, which enables
 * their timestamp and opcode-error interrupts itself; this driver owns those pipes, so it has to do
 * that work, and the honest way to say so is to name the function it came from.
 *
 * The caller must have run bc250_gfx_setup() and bc250_sdma_setup() first: which registers these
 * functions walk comes from adev->gfx.me, adev->gfx.mec, adev->gfx.kiq[0].ring and
 * adev->sdma.num_instances. On a zeroed adev every loop below runs zero times and nothing is
 * written, which is a silent no-op rather than an error, so the order matters.
 */
#ifndef BC250_IRQ_H
#define BC250_IRQ_H

#include "amdgpu.h"

/*
 * [amdgpu] amdgpu_irq.h: the three states an interrupt source can be put in. Only the two the
 * register callbacks act on are here; upstream's AMDGPU_IRQ_STATE_UNKNOWN is the default arm of
 * every switch and writes nothing.
 */
enum bc250_irq_state {
	BC250_IRQ_STATE_DISABLE,
	BC250_IRQ_STATE_ENABLE
};

/*
 * amdkfd's per-pipe enable, in the order the E03 trace has it: MEC1 pipes 0 to 3, each selected
 * through GRBM_GFX_CNTL and written through CPC_INT_CNTL.
 *
 * What the trace says, and what it does not: the four CP_ME1_PIPE0..3_INT_CNTL registers already
 * read 0x05000000 when amdgpu first touches them, which is exactly the value these four writes
 * carry, and their reset default is 0 (gc_10_1_0_default.h:2441). That fits CPC_INT_CNTL under a
 * GRBM selection of me 1, pipe n being the same storage as CP_ME1_PIPEn_INT_CNTL, but it is not
 * proof: the dump covers 0.5496 s to 0.5504 s and 1.5602 s to 1.5609 s, and something in the second
 * in between could have written them. Either way the pipes need those two bits and nothing else in
 * this driver sets them, so the function stays.
 */
int bc250_irq_init_mec_pipes(struct amdgpu_device *adev);

/*
 * What amdgpu_fence_driver_hw_init() produces on this part: one amdgpu_irq_get() per ring, in ring
 * order - the gfx ring, the eight compute rings, the KIQ, then the two SDMA engines.
 *
 * The eight compute rings share four interrupt types (one per pipe, two queues each) and
 * amdgpu_irq_get() only calls the callback on the first enable of a type, which is why the trace
 * has four CP_ME1_PIPEn_INT_CNTL accesses and not eight. That refcount is reproduced here by
 * walking the pipes rather than the rings.
 */
int bc250_irq_hw_init(struct amdgpu_device *adev);

/* gfx_v10_0.c:7833 gfx_v10_0_late_init(): the three fault sources, in its order - privileged
 * register, privileged instruction, bad opcode. */
int bc250_irq_late_init(struct amdgpu_device *adev);

/*
 * THE INTERRUPT STAGE, as one thing the miniport can call.
 *
 * Four calls, in this order, and the order is unit A's (E03 trace 1.5601 to 1.5609 s), not a
 * grouping chosen here. All four return int; every one of them must be propagated, because a
 * failure means a source the driver believes is armed is not.
 *
 *     1  bc250_irq_init_mec_pipes(adev)
 *     2  bc250_irq_hw_init(adev)
 *     3  bc250_nbio_enable_doorbell_selfring_aperture(adev, true)     [bc250_nbio.h]
 *     4  bc250_irq_late_init(adev)
 *
 * Step 3 is NBIO's, not the gfx block's - upstream runs it from nv_common_hw_init() - and it really
 * does fall between the two irq calls in the trace, which is why it is listed here rather than left
 * for the caller to place.
 *
 * What has to be in `adev` before the stage runs:
 *
 *   bc250_gfx_setup() and bc250_sdma_setup() have run.  Steps 1, 2 and 4 decide which registers to
 *       walk from adev->gfx.me, adev->gfx.mec, adev->gfx.kiq[0].ring and adev->sdma.num_instances.
 *       On a zeroed adev every loop runs zero times, writes nothing, and returns 0 - a silent
 *       no-op, not an error. That is the one failure mode of this stage that does not announce
 *       itself, so the caller checks the write count as well as the return value.
 *
 *   adev->doorbell.base is the physical address of the doorbell aperture, taken from the
 *       miniport's translated resource list (BAR2 on this function; unit A's trace shows
 *       0xD0000000). Step 3 needs it and nothing else does. It is the one precondition that IS
 *       checked: bc250_nbio_enable_doorbell_selfring_aperture() returns BC250_EINVAL and writes
 *       nothing rather than pointing the window the GPU rings its own doorbells through at
 *       physical address 0.
 *
 * What the stage does NOT need: the IH ring. These are the source enables in the CP and SDMA
 * blocks, and they are just bits - with the ring off they arm sources whose vectors go nowhere.
 * Running the stage before bc250_ih_hw_init() is therefore safe and is what unit A does; running it
 * after is equally fine. The one order that matters is that the ring is up before anything is
 * submitted that would raise a vector (bc250_gfx_signal_fence(), bc250_gfx.h).
 */

/* The individual callbacks, exposed so the replay test can compare them one at a time and so the
 * miniport can turn a single source off. `state` is BC250_IRQ_STATE_ENABLE or _DISABLE. */
void bc250_irq_set_gfx_eop(struct amdgpu_device *adev, u32 me, u32 pipe, enum bc250_irq_state state);
void bc250_irq_set_compute_eop(struct amdgpu_device *adev, int me, int pipe, enum bc250_irq_state state);
int  bc250_irq_set_kiq(struct amdgpu_device *adev, enum bc250_irq_state state);
int  bc250_irq_set_priv_reg_fault(struct amdgpu_device *adev, enum bc250_irq_state state);
int  bc250_irq_set_bad_op_fault(struct amdgpu_device *adev, enum bc250_irq_state state);
int  bc250_irq_set_priv_inst_fault(struct amdgpu_device *adev, enum bc250_irq_state state);
int  bc250_irq_set_sdma_trap(struct amdgpu_device *adev, int instance, enum bc250_irq_state state);

#endif /* BC250_IRQ_H */
