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

#endif /* BC250_BACKEND_MEM_H */
