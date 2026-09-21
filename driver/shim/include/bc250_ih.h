/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The interrupt ring (OSSSYS / IH 5.0), milestone M6.
 *
 * Transcribed from navi10_ih.c and amdgpu_ih.c at kernel tag v6.18, commit
 * 7d0a66e4bb9081d75c82ec4957c50034cb0ea449. Both are in driver/amdgpu-import/reference/ and are
 * imported byte-identical for reading, not for compiling: navi10_ih.c reaches into the IP-block
 * machinery, the BO allocator and the Linux interrupt subsystem, none of which exists here.
 *
 * What this is for. Every ring this driver brought up in M5 signals completion the same way: the
 * engine writes a 32-byte interrupt vector into a ring in system memory and raises the line. Until
 * something reads that ring, a fence is only a value in memory that nobody is waiting on. This is
 * the reader.
 *
 * Three of the functions below run in the miniport's DPC, at DISPATCH_LEVEL, and that constrains
 * them absolutely: no lock, no allocation, no sleep, no call that might do any of those. They use
 * RREG32/WREG32, one doorbell write and volatile reads of the GTT ring, and nothing else. They are
 * marked in the list. The rest run at PASSIVE_LEVEL during bring-up and teardown.
 *
 * What is deliberately left out, with reasons, so that a reader comparing against navi10_ih.c does
 * not have to guess:
 *
 *   - ih1 and ih2. navi10_ih_sw_init() sets both ring_size to 0 unconditionally (navi10_ih.c:580),
 *     so every loop over them is dead and no mmIH_*_RING1/RING2 register is ever written.
 *   - ih_soft, the software ring amdgpu delegates retry page faults to (gmc_v10_0.c:119-130). It
 *     has no registers at all and is only needed for recoverable page faults, which this driver
 *     does not have.
 *   - The IH_CHICKEN block (navi10_ih.c:331-350). It is guarded by
 *     `load_type == AMDGPU_FW_LOAD_DIRECT && use_bus_addr`; this part loads firmware through the
 *     PSP and its ring is in GTT, so both are false.
 *   - force_update_wptr_for_self_int() (navi10_ih.c:105). Returns immediately below OSSSYS 5.0.3;
 *     this is 5.0.x below that, so every IH_CNTL2 and RING1/2 write inside it is dead.
 *   - Every amdgpu_sriov_vf() arm, including psp_reg_program() of IH_RB_CNTL and
 *     navi10_ih_irq_rearm(). This is a bare-metal function.
 *   - IH_STORM_CLIENT_LIST_CNTL. navi10_ih.c never touches it; only the 6.x and 7.x IH blocks do.
 *   - pci_set_master(), which navi10_ih_irq_init() calls at :364. That is the bus driver's job and
 *     under Windows PnP has already done it; doing it here would reach outside BAR5.
 */
#ifndef BC250_IH_H
#define BC250_IH_H

#include "amdgpu.h"

/* [amdgpu] amdgpu_ih.h:30 IH_RING_SIZE. Unit A's traced IH_RB_CNTL has RB_SIZE = 16, and the field
 * is order_base_2(ring_size / 4), so 2^16 dwords = 256 KB - the same number. */
#define BC250_IH_RING_SIZE	(256u * 1024u)

/* [amdgpu] amdgpu_irq.h:46 struct amdgpu_iv_entry, reduced to the fields the decode produces.
 * Upstream also carries a pointer back to the ring and to the IH block; a DPC consumer does not
 * need either, and leaving them out keeps this a plain value type the miniport can copy. */
struct bc250_iv_entry {
	u32	client_id;      /* SOC15_IH_CLIENTID_*, which block sent it */
	u32	src_id;         /* the block's own source number */
	u32	ring_id;        /* for CP: me, pipe and queue packed - see bc250_ih_eop_ring_id() */
	u32	vmid;
	u32	vmid_src;
	u64	timestamp;      /* 48 bits, assembled from two dwords */
	u32	timestamp_src;
	u32	pasid;
	u32	node_id;
	u32	src_data[4];
};

/* ---------------------------------------------------------------------------------------------
 * Bring-up and teardown. PASSIVE_LEVEL.
 * ------------------------------------------------------------------------------------------- */

/* Allocate the ring and the write-back page and fill in adev->irq.ih. Writes no register.
 *
 * `msi` becomes IH_RB_CNTL.RPTR_REARM, which is upstream's `!!adev->irq.msi_enabled`
 * (navi10_ih.c:277). Unit A ran with MSI and the traced value has the bit set; under Windows it
 * depends on the INF's MessageSignaledInterruptProperties, so the caller states it rather than
 * this code assuming. */
int  bc250_ih_setup(struct amdgpu_device *adev, bool msi);
void bc250_ih_teardown(struct amdgpu_device *adev);

/* navi10_ih_irq_init(). Programs the ring and enables it. bc250_ih_setup() must have run, and the
 * GART must be up: the ring is GTT memory, so the IH block walks M4's page table to reach it.
 * adev->dummy_page_addr must be set - nbio_v2_3_ih_control() points the dummy read at it. */
int  bc250_ih_hw_init(struct amdgpu_device *adev);

/* navi10_ih_irq_disable(). Leaves the ring allocated, so hw_init can run again. */
void bc250_ih_hw_fini(struct amdgpu_device *adev);

/* ---------------------------------------------------------------------------------------------
 * The DPC trio. DISPATCH_LEVEL.
 *
 * THE CONTRACT, which is a promise about the code and not only about the caller. These three
 * functions reach into `adev` at exactly two places and nowhere else:
 *
 *   adev->irq.ih      every field they read or write
 *   adev->backend     through RREG32_SOC15 / WREG32_SOC15 and bc250_shim_wdoorbell32(), and
 *                     through nothing else
 *
 * They call no bc250_shim_log, no bc250_shim_udelay, no bc250_shim_mem_alloc/free, take no lock,
 * allocate nothing and never sleep. Every read of memory the GPU writes - the ring and the two
 * write-back slots - goes through a volatile pointer. This is checked, not asserted: the host test
 * surveys every register offset the three touch (driver/shim/test/replay_ih.c), and the list it
 * produces is below.
 *
 * WHY IT MATTERS. In the miniport `adev->backend` is the sequence object of whichever escape is
 * running, swapped under a fast mutex at APC_LEVEL, and the kernel doorbell write records into
 * shared state. A DPC can use neither. So the miniport gives the DPC a `struct amdgpu_device` of
 * its own: zeroed, `backend` set to a DPC-only sequence, and `irq.ih` copied by value from the
 * escape's adev after bc250_ih_hw_init() returned 0.
 *
 * COPYING adev->irq.ih BY VALUE IS SAFE, with one rule. struct amdgpu_ih_ring has no pointer into
 * itself and no pointer into `adev`: `ring`, `wptr_cpu` and `rptr_cpu` point into the two GTT
 * allocations, which outlive the copy because bc250_ih_teardown() is the only thing that frees
 * them. The rule is that the copy must never be handed to bc250_ih_setup() or bc250_ih_teardown():
 * `ring_mem` and `wb_mem` are copied too, so a teardown through the copy would free memory the
 * original still points at. Owning adev only. The copy's `rptr` field then drifts from the
 * original's, which is correct and is why the caller carries the read pointer itself.
 *
 * WHY THE TRIO DOES NOT NEED adev->reg_offset. The register offsets it uses are dword indices in
 * `ih->ih_regs`, resolved once in bc250_ih_setup() and copied with the rest of the struct. That is
 * upstream's own arrangement (struct amdgpu_ih_regs, amdgpu_ih.h:29, filled by
 * navi10_ih_init_register_offset() at navi10_ih.c:49 and read back in navi10_ih_set_rptr()), not
 * something invented for this driver - but it is what makes a DPC-only device possible, because
 * SOC15_REG_OFFSET() resolves through adev->reg_offset[][] and would dereference a null pointer on
 * a zeroed one.
 *
 * THE REGISTERS THE TRIO CAN TOUCH, the whole list:
 *
 *   OSSSYS IH_RB_CNTL   read and written, by get_wptr, only on the overflow path (the
 *                       set-then-clear pulse of WPTR_OVERFLOW_CLEAR)
 *   OSSSYS IH_RB_WPTR   read, by get_wptr, only when the write-back slot says overflow
 *   OSSSYS IH_RB_RPTR   written, by set_rptr, only when ih->use_doorbell is false
 *   the doorbell        one 32-bit write at ih->doorbell_index, by set_rptr, when use_doorbell
 *
 * With a doorbell and no overflow - the ordinary case - the trio touches NO register at all.
 * ------------------------------------------------------------------------------------------- */

/* navi10_ih_get_wptr(). Returns the ring's write pointer as a BYTE offset, already masked.
 *
 * May write IH_RB_CNTL: an overflow is acknowledged with a two-write pulse of WPTR_OVERFLOW_CLEAR,
 * and on overflow the read pointer is moved to the oldest entry that was not overwritten. That is
 * upstream's recovery and it is why this is not a pure read. `*overflowed`, if not NULL, says
 * whether that happened, so the miniport can count it; upstream only logs.
 *
 * ON OVERFLOW THE CALLER MUST RESYNC. The vectors between the old read pointer and the new one were
 * overwritten by the hardware and are gone; that is what an overflow means. This function has put
 * the new read pointer in adev->irq.ih.rptr, and the caller's own copy is now stale, so the loop is
 *
 *     wptr = bc250_ih_get_wptr(adev, &overflowed);
 *     if (overflowed)
 *             rptr = adev->irq.ih.rptr;
 *     while (rptr != wptr)
 *             bc250_ih_decode(adev, &rptr, &e);
 *     bc250_ih_set_rptr(adev, rptr);
 *
 * Without the resync the loop would walk from the old read pointer through entries the hardware has
 * already trampled and decode them as if they were new. */
u32  bc250_ih_get_wptr(struct amdgpu_device *adev, bool *overflowed);

/* amdgpu_ih_decode_iv_helper(). Reads the 32-byte vector at *rptr, fills `out`, and advances *rptr
 * by 32 with the ring's mask applied.
 *
 * The read pointer is the caller's, not adev's: upstream keeps it in the ring struct and mutates it
 * from one thread, which is a rule a DPC cannot be given by a comment. Passing it makes the loop
 * `while (rptr != wptr) decode` and the single writer of the hardware pointer both visible at the
 * call site. Returns 0, or BC250_EINVAL if anything needed is missing. */
int  bc250_ih_decode(struct amdgpu_device *adev, u32 *rptr, struct bc250_iv_entry *out);

/* navi10_ih_set_rptr(). Publishes the read pointer: the write-back slot first, then the doorbell.
 * Call it once at the end of a DPC, not once per entry.
 *
 * The doorbell is a 32-BIT store (bc250_shim_wdoorbell32), which is upstream's WDOORBELL32 at
 * navi10_ih.c:499 and the only narrow doorbell in this driver. A 64-bit store here would also write
 * the next dword, and BIF_IH_DOORBELL_RANGE opens a two-entry window, so that dword is inside it. */
void bc250_ih_set_rptr(struct amdgpu_device *adev, u32 rptr);

/* ---------------------------------------------------------------------------------------------
 * Routing: what a vector means
 *
 * These are pure functions over a decoded entry, so the miniport can count and route without
 * repeating the bit arithmetic. The packing is the CP's and is taken from gfx_v10_0_eop_irq()
 * (gfx_v10_0.c:9195-9197), which is the only statement of it in the kernel.
 * ------------------------------------------------------------------------------------------- */
void bc250_ih_eop_ring_id(const struct bc250_iv_entry *e, u32 *me, u32 *pipe, u32 *queue);

/* True if this vector is the end-of-pipe of the gfx ring / of a compute ring / the KIQ's own.
 * A gfx EOP and a compute EOP carry the SAME client and source id and are told apart only by the
 * me field of ring_id, which is why these are functions and not comparisons. */
bool bc250_ih_is_gfx_eop(const struct bc250_iv_entry *e);
bool bc250_ih_is_compute_eop(const struct bc250_iv_entry *e);
bool bc250_ih_is_kiq(const struct bc250_iv_entry *e);

/* True for an SDMA trap; *instance gets 0 or 1. The two instances share a source id and are told
 * apart by client id (SDMA0 = 0x08, SDMA1 = 0x09), not by ring_id. */
bool bc250_ih_is_sdma_trap(const struct bc250_iv_entry *e, u32 *instance);

#endif /* BC250_IH_H */
