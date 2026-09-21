/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/* Memory, doorbell and CP backend for the host replay: see backend_mem.c. */
#ifndef BC250_BACKEND_MEM_H
#define BC250_BACKEND_MEM_H

#include "amdgpu.h"

/* Where the two arenas hand out MC addresses. The test passes what bc250_gmc_setup() read off unit
 * A, so the addresses the shim then programs into registers are addresses that unit A's memory
 * controller would have accepted. */
void backend_mem_set_bases(u64 vram_base, u64 gtt_base);

/* Free every allocation and rewind both arenas. */
void backend_mem_reset(void);

/* Tell the CP stub which rings exist, so that a doorbell can be resolved to a ring buffer. Call
 * after bc250_gfx_setup()/bc250_sdma_setup() and before the bring-up. */
void backend_ring_register(struct amdgpu_ring *ring);

/* Every doorbell the run rang, with the width of the store.
 *
 * The width is here because it is the one property of a doorbell no register trace can show and
 * upstream is specific about: `WDOORBELL32` for the IH ring's read pointer (navi10_ih.c:474, :499)
 * and `WDOORBELL64` everywhere else on this part. A 64-bit store at the IH's index would also write
 * the next dword, which is inside the two-entry window `BIF_IH_DOORBELL_RANGE` opens, so it would
 * reach the hardware. Recorded so a test can insist rather than trust the source. */
struct backend_doorbell {
	unsigned int index;
	unsigned int width;             /* 32 or 64 */
	unsigned long long value;
};

const struct backend_doorbell *backend_doorbells(void);

/* Counters, for the test to print and to check. */
unsigned int backend_doorbell_count(void);
unsigned int backend_cp_stub_count(void);

/* Tell the stub which dword index SCRATCH_REG0 is, resolved through AMD's headers by the caller, so
 * that it can verify the packet rather than merely apply it. Without this the register is not
 * checked; with it, a SET_UCONFIG_REG naming any other register is rejected. */
void backend_cp_stub_expect(u32 scratch_reg0_dword_index);

/* Packets that looked like a ring test - a SET_UCONFIG_REG - but did not pass the stub's checks.
 * Must be 0: a rejection means the shim built a malformed ring test, which would otherwise show up
 * only as a timeout with no explanation. */
unsigned int backend_cp_stub_rejects(void);
unsigned int backend_packet_count(void);

/* Turn the CP stub off, so that a ring test times out instead of being satisfied. The control run
 * uses this to show that the ring tests are really being driven by the packets. */
void backend_cp_stub_enable(int on);

/*
 * Give the CP stub an interrupt ring to deliver end-of-pipe vectors into, so that a fence submitted
 * on a ring arrives as a decodable vector the way it will on hardware. `adev` must already have been
 * through bc250_ih_setup(); the stub writes into adev->irq.ih and nowhere else, and it never calls
 * anything in driver/shim.
 *
 * Without an attached ring the stub still writes the fence value and delivers nothing, which is what
 * every run that is only about register writes wants. backend_mem_reset() detaches.
 */
void backend_ih_attach(struct amdgpu_device *adev);
void backend_ih_detach(void);

/* How many vectors the stub has delivered, and where its write pointer stands (a byte offset, the
 * same units as the hardware's). For the test to check against what the decode consumed. */
unsigned int backend_ih_delivered(void);
u32 backend_ih_wptr(void);

/* The decoded PM4 log: one entry per packet the stub saw go past a doorbell. */
struct backend_packet {
	unsigned int doorbell_index;
	unsigned int opcode;
	unsigned int count;             /* the PACKET3 count field, so the packet is count + 2 dwords */
	unsigned int first_dword;       /* index in the ring of the header */
	u32 body[8];                    /* up to 8 body dwords, enough for every packet here */
	unsigned int body_dwords;
};

const struct backend_packet *backend_packets(void);

/* ---------------------------------------------------------------------------------------------
 * The MEC's own fetch state: a declared model of measured hardware
 *
 * The fourth declared model, after backend_add_alias(), backend_add_reaction() and
 * backend_add_selfclear(), and the first one that models an engine remembering something rather
 * than a register changing. Without it the replay cannot see the class of bug that experiment E12
 * run 002 found on unit A, because a replayed register file has no MEC and forgets everything the
 * moment a register is overwritten.
 *
 * What was measured, on unit A, 2026-09-21, E12 run 002 (P:\BC-250\scratch\e12\run002\out):
 *
 *  1. The MEC keeps, per HQD slot, its own copy of that queue's ring base and read pointer. It is
 *     taken when the queue is activated, and the engine advances the read pointer as it fetches.
 *  2. Writes to the HQD registers while the MEC is halted do not reach that copy. The second
 *     bring-up wrote CP_HQD_ACTIVE = 0, CP_HQD_PQ_RPTR = 0 and CP_HQD_PQ_BASE = 0x5640 with
 *     CP_MEC_CNTL = 0x50000000 (gfx-run-x-153134.txt), and the engine still fetched from the first
 *     run's base.
 *  3. On un-halt the MEC resumes from that copy. The first run's KIQ was at 0x442000 and its read
 *     pointer stood near dword 0x800; the second run's fetch faulted at 0x444000, which is that
 *     base plus that offset and an address no register in either run names. UTCL2 delivered it as
 *     client 27, source 0, src_data[0] = address >> 12 (ih-state-failed-153224.txt, last vector),
 *     and the queue never ran again: the KIQ ring test timed out after 165 ms.
 *  4. A dequeue request serviced while the MEC is RUNNING clears CP_HQD_ACTIVE and the copy with
 *     it. This one is not measured on unit A - it is the handshake that
 *     gfx_v10_0_kiq_init_register()'s active branch (gfx_v10_0.c:7035-7041) and
 *     kgd_hqd_destroy() (amdgpu_amdkfd_gfx_v10.c:607-615) both perform, and it is what the fix in
 *     bc250_gfx_hw_fini() relies on. The model asserts it; the next hardware run tests it.
 *
 * A cold boot is reproduced by the same rules: the queue is activated while the MEC is halted, the
 * engine has no copy yet, and on un-halt it takes the register values. That is why point 2 is about
 * an EXISTING copy and not about activation in general.
 *
 * What this does NOT model, deliberately: the graphics ring on CP_RB0 and the two SDMA rings. Both
 * are a different shape - a base in a plain register, rewritten every bring-up, with no MQD and no
 * activation step to take a copy at - and the gfx and compute queues are taken down by UNMAP_QUEUES
 * in the teardown, which a running CP services. Whether ME or an SDMA engine nevertheless caches a
 * base across a halt is UNTESTED: E12 run 002 failed at the KIQ before it reached either, and run
 * 001's second set of rings landed on the first set's addresses, where a stale fetch would have
 * found valid memory and shown nothing. The measurement that settles it is the same run repeated
 * with the KIQ fixed: the second bring-up then reaches the gfx ring test and the SDMA ring test
 * with rings at addresses the first run never used, so a cached base shows up as a fault at an old
 * address or as a ring test that times out.
 *
 * Every offset is passed in by the test, resolved through AMD's headers, so this file still types
 * no register address.
 * ------------------------------------------------------------------------------------------- */

struct backend_mec_regs {
	u32 mec_cntl;                   /* CP_MEC_CNTL */
	u32 mec_halt_mask;              /* its halt bits: the engine is stopped while any is set */
	u32 grbm_gfx_cntl;              /* GRBM_GFX_CNTL, whose value names the HQD slot */
	u32 hqd_active;                 /* CP_HQD_ACTIVE */
	u32 hqd_pq_base;                /* CP_HQD_PQ_BASE, the base >> 8 */
	u32 hqd_pq_base_hi;             /* CP_HQD_PQ_BASE_HI */
	u32 hqd_pq_rptr;                /* CP_HQD_PQ_RPTR, in dwords */
	u32 hqd_dequeue_request;        /* CP_HQD_DEQUEUE_REQUEST */
	u32 fault_client_id;            /* SOC15_IH_CLIENTID_UTCL2 */
	u32 fault_src_id;               /* 0, as unit A sent it */
};

/* Turn the model on. Returns 0, or -1 if it is already on. backend_mem_reset() turns it off. */
int backend_add_mec_fetch_state(const struct backend_mec_regs *regs);

/* How many stale fetches faulted, and the address of the last one. A test that expects the fault
 * asserts both; a test that expects none asserts the count is 0. */
unsigned int backend_mec_faults(void);
u64 backend_mec_fault_address(void);

/* Called by backend_trace.c for every register write, so the model can follow the engine. Does
 * nothing unless backend_add_mec_fetch_state() has been called. */
void backend_mec_observe(u32 byte_offset, u32 value);

#endif /* BC250_BACKEND_MEM_H */
