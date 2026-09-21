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

/*
 * Make one compute queue refuse to answer an UNMAP_QUEUES: its CP_HQD_ACTIVE goes on reading 1
 * although the packet named it. The test speaking for hardware, like backend_poke(), and the only
 * way to reach the arm of bc250_kcq_clear_pointers() that leaves a still-active queue alone - a
 * failed unmap is logged and survived by the teardown, so the queue it failed on is exactly the one
 * whose read pointer must not be zeroed underneath a CP that is still fetching from it.
 *
 * Nothing in driver/shim can reach this. Call with 0 to let go again; backend_mem_reset() drops it
 * with the rest of the ring table.
 */
void backend_hqd_pin(const struct amdgpu_ring *ring, int pinned);

/* How many DISPATCH_DIRECT packets the stub has accepted and carried out. A dispatch that fails any
 * of the stub's checks is a rejection (backend_cp_stub_rejects()) and is not counted here, so a test
 * asserts both: the count it expects, and no rejections. */
unsigned int backend_dispatch_count(void);

/*
 * PACKET3_INDIRECT_BUFFER (ADR 0008 stage C): how many the stub followed, and how many it saw and
 * did not follow.
 *
 * Following one means reading the dwords at the address the packet names and executing them as if
 * they had been in the ring, which is what the CP does. That is only modelled for VMID 0: every
 * address this file hands out is an MC address of the flat GART aperture, and a buffer named
 * through any other VMID would be reached through a page table walk that nothing here has. So an
 * IB at a non-zero VMID is COUNTED and not executed, rather than quietly ignored - a test that
 * submits one asserts the skip, and a test that expects its packets to run would otherwise see a
 * silent nothing and call it a pass.
 *
 * A malformed IB - the wrong count, a length that runs off the end of the buffer, an address in no
 * allocation, or an IB inside an IB - is a rejection like any other (backend_cp_stub_rejects()).
 */
unsigned int backend_ib_followed(void);
unsigned int backend_ib_skipped(void);

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
 * One thing the model does beyond the fetch state, because it is the same selection bookkeeping: it
 * answers reads of CP_HQD_PQ_RPTR from the queue GRBM_GFX_CNTL names, through
 * backend_set_read_hook(). The replayed register file holds one value per offset, so without this
 * the nine queues share a read pointer, and a test cannot tell a teardown that put all nine back
 * from one that put a single queue back. The value it answers with is where the CP stub left that
 * queue - at its write pointer, which is where unit A's queues idle after the ring tests (fact M40)
 * - and only for queues a ring test has actually run on, so a cold boot reads the seeded sweep value
 * exactly as it did before.
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

/* ---------------------------------------------------------------------------------------------
 * The SDMA engines' own write pointer: the fifth declared model
 *
 * The same kind of thing as the MEC's fetch state above, for the other pair of engines and for the
 * other half of the question that model left UNTESTED: "whether ME or an SDMA engine nevertheless
 * caches a base across a halt is UNTESTED ... the measurement that settles it is the same run
 * repeated with the KIQ fixed". That run is experiment E15, and it settles it for SDMA.
 *
 * What was measured, on unit A, 2026-09-21, E15 (bc250kmd 0.6.2.0, one device start, bring-up,
 * `gfx fini` undo, bring-up again, no GPU reset and no PSP firmware reload):
 *
 *  1. Each SDMA engine keeps its own 64-bit write pointer, in bytes. After the first bring-up's
 *     four submissions on SDMA0 and three on SDMA1, SDMA0_GFX_RB_RPTR = RB_WPTR = 0x100 and SDMA1's
 *     read 0xC0 - four and three sixteen-dword submissions.
 *  2. The undo does not move it. SDMAx_CNTL, RB_CNTL with RB_ENABLE clear, IB_CNTL with IB_ENABLE
 *     clear, F32_CNTL with HALT set - amdgpu's own unload, fact M54 - and the pointer registers
 *     still read 0x100 and 0xC0 afterwards.
 *  3. Writes to the pointer registers do not reach it. The second bring-up writes RB_RPTR,
 *     RPTR_HI, RB_WPTR and WPTR_HI with 0, and RB_WPTR with 0 again under MINOR_PTR_UPDATE = 1; a
 *     sweep taken after that bring-up and before any submission reads 0x100 and 0xC0 again, on both
 *     engines, over two bring-ups. RB_BASE, written in the same sequence, does take the new ring
 *     address.
 *  4. INFERRED, not measured. The pointer is monotonic: a doorbell carrying a value at or below it
 *     executes nothing. What was observed is one failure and its shape - the second bring-up's
 *     SDMA ring test rang 0x40, the engine stood at 0x100, and the scratch slot kept 0xCAFEDEAD
 *     until the poll ran out - plus upstream's own comment at sdma_v5_0.c:757, "before programing
 *     wptr to a less value, need set minor_ptr_update first", which says the hardware treats a
 *     lower write pointer as a thing needing special handling. Monotonicity is the simplest rule
 *     that produces that failure; "ignores a doorbell while RB_ENABLE was clear when it was rung"
 *     and several others produce it too. What would settle it is a bring-up that rings a doorbell
 *     ABOVE the kept pointer without adopting it and sees whether the engine executes the gap.
 *
 * So the model: one pointer per engine, in bytes, which survives backend_ring_register() for the
 * same ring object and survives the teardown, because the engine does. A doorbell at or below the
 * pointer executes nothing; above it, the stub executes from the pointer to the new value, and the
 * pointer follows. The old assumption this replaces was that a re-initialised SDMA ring restarts at
 * 0, which is what the CP rings do and what the hardware refutes for these two.
 *
 * The scope of the survival is UNTESTED at both ends, and the model is the optimistic reading of
 * it. Only backend_mem_reset() clears the pointer, which asserts that nothing short of a new boot
 * does - but E15 was one device start, three bring-ups, one PSP load. Whether the pointer survives
 * a Windows device restart (PnP stop and start, no power change), and whether it survives a PSP
 * firmware reload without a reboot, were not measured; either could reset the engine and make the
 * adoption arm dead code in exactly the case it was written for. A driver that adopts a pointer the
 * engine no longer holds would read 0 and take the fresh arm, so the failure mode of being wrong
 * here is benign, which is why the model states the stronger claim rather than the safer one.
 *
 * It is on for every SDMA ring, without being asked: it is what the hardware does, and a test that
 * had to opt in would be a test that could forget to. The register offsets below are the one part
 * that has to be declared, because answering a read needs to know which offset is which, and this
 * file types no register address.
 *
 * Every offset is passed in by the test, resolved through AMD's headers.
 * ------------------------------------------------------------------------------------------- */

struct backend_sdma_regs {
	u32 rb_rptr, rb_rptr_hi;        /* SDMAx_GFX_RB_RPTR, _HI */
	u32 rb_wptr, rb_wptr_hi;        /* SDMAx_GFX_RB_WPTR, _HI */
	/* SDMAx_RB_RPTR_FETCH, _HI - note the name has no GFX in it. Nothing in driver/shim reads
	 * them today; they are here because the sweeps that measured this read them, and a model that
	 * answers two of the three pointer registers and not the third would be a trap for the next
	 * person. 0 leaves them out. */
	u32 rb_rptr_fetch, rb_rptr_fetch_hi;
};

/*
 * Let the model answer reads of one engine's pointer registers. `ring` is that engine's GFX ring,
 * and the offsets are that instance's. Idempotent by ring, because a teardown and a fresh setup
 * hand back the same ring object. Returns 0, or -1 if the table is full.
 *
 * Without this call the pointer still survives and still decides what a doorbell executes; only the
 * reads fall back to the register file, which is what every run that programs no SDMA ring wants.
 */
int backend_add_sdma_pointer_state(struct amdgpu_ring *ring, const struct backend_sdma_regs *regs);

/* Where this engine's pointer stands, in bytes, and whether it has executed anything at all. For a
 * test to compare what a second bring-up adopted with what the first run left behind. */
u64 backend_sdma_engine_wptr(const struct amdgpu_ring *ring);
int backend_sdma_engine_ran(const struct amdgpu_ring *ring);

/*
 * Make one engine answer with its read pointer `bytes` behind its write pointer: an engine that was
 * halted part-way through what it had been given. The test speaking for hardware, like
 * backend_hqd_pin(), and the only way to reach the arm of bc250_sdma_hw_fini() that reports
 * BC250_EBUSY - the stub executes a submission whole, so nothing in driver/shim can produce it.
 *
 * RB_RPTR and RB_RPTR_FETCH move back, RB_WPTR does not, which is what "stopped owing work" reads
 * like. 0 lets go again; backend_mem_reset() drops it with the rest of the ring table.
 */
void backend_sdma_hold_back(const struct amdgpu_ring *ring, u64 bytes);

/*
 * Put one engine's pointer at `bytes`, whatever the stub has executed. The same thing again: the
 * test speaking for hardware, for the states a working engine never reaches - a pointer that is not
 * a whole submission, and the all-ones a faulted register sequence or a dead bus answers with. Both
 * are refusals in bc250_sdma_gfx_resume_instance(), and without this nothing would ever take them.
 *
 * Marks the engine as having run, because a pointer is only answered for once it has.
 */
void backend_sdma_force_engine_wptr(const struct amdgpu_ring *ring, u64 bytes);

/* Doorbells this engine ignored because they were at or below its pointer. The count a test asserts
 * is 0 once the driver adopts the pointer, and non-zero when it does not. */
unsigned int backend_sdma_ignored_doorbells(void);

#endif /* BC250_BACKEND_MEM_H */
