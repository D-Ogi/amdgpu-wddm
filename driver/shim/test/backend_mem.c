/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * The other half of the host replay backend: memory, doorbells, and a CP stub.
 *
 * backend_trace.c answers register reads and records register writes. This file answers the three
 * calls the GFX and SDMA bring-up makes that are not register access:
 *
 *   bc250_shim_mem_alloc / _free   VRAM and GART-mapped system pages. Here they are host malloc
 *                                  with an MC address handed out of a bump arena, so that every
 *                                  address the shim programs into a register is an address this
 *                                  file chose and the test can check it against what it expects
 *                                  instead of against the trace.
 *   bc250_shim_wdoorbell64         the only way a GFX10 compute or KIQ ring can be told its write
 *                                  pointer. Recorded, and handed to the CP stub.
 *
 * ---------------------------------------------------------------------------------------------
 * The CP stub, declared plainly because it is the one place this replay pretends to be hardware.
 *
 * gfx_v10_0_ring_test_ring() writes 0xCAFEDEAD to SCRATCH_REG0, submits a three-dword PM4 packet
 * that sets the same register to 0xDEADBEEF, rings the doorbell and polls. On unit A the CP
 * executed the packet; against a replayed register file nothing would ever change the value and
 * every one of the eleven ring tests in the window would time out, which would stop the sequence at
 * the first one.
 *
 * So this file executes that one packet, and only that one: on a doorbell it decodes the dwords the
 * ring grew by, and a PACKET3_SET_UCONFIG_REG of one register is applied to the register file
 * through backend_poke() - not through bc250_shim_wreg(), so it can never appear in the comparison
 * as if the shim had written it. Every other packet is logged and otherwise ignored.
 *
 * That makes the ring tests pass for the right reason and not by decree: if the shim built the
 * packet wrongly - wrong opcode, wrong register offset, wrong count, or wrote it at the wrong place
 * in the ring - the stub would not find it, the poll would time out, and the test would fail. The
 * control run turns the stub off and must fail, which is what says the tests were being driven by
 * the packets all along.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_mem.h"
#include "backend_trace.h"

/* AMD's PM4 definitions, imported unmodified, so the decoder uses the same opcode numbers the
 * encoder does. */
#include "nvd.h"

#define MAX_ALLOCS   64u
#define MAX_RINGS    16u
/* Enough for the whole window: the clear-state stream alone is about forty packets. */
#define MAX_PACKETS  256u

struct allocation {
	void *cpu;
	u64   mc;
	u32   size;
};

static struct allocation g_alloc[MAX_ALLOCS];
static unsigned int g_alloc_count;

static u64 g_vram_base, g_vram_next;
static u64 g_gtt_base, g_gtt_next;

struct ring_slot {
	struct amdgpu_ring *ring;
	u64 decoded_to;                 /* how far into the ring the stub has looked, in dwords */
};

static struct ring_slot g_ring[MAX_RINGS];
static unsigned int g_ring_count;

static unsigned int g_doorbells;
static unsigned int g_stub_hits;
static unsigned int g_stub_rejects;
static u32 g_scratch_reg0_dword;        /* set by backend_cp_stub_expect(), 0 = not told yet */
static int g_stub_on = 1;

static struct backend_packet g_packet[MAX_PACKETS];
static unsigned int g_packet_count;

void backend_mem_set_bases(u64 vram_base, u64 gtt_base)
{
	g_vram_base = g_vram_next = vram_base;
	g_gtt_base = g_gtt_next = gtt_base;
}

void backend_mem_reset(void)
{
	unsigned int i;

	for (i = 0; i < g_alloc_count; i++)
		free(g_alloc[i].cpu);
	g_alloc_count = 0;
	g_vram_next = g_vram_base;
	g_gtt_next = g_gtt_base;

	g_ring_count = 0;
	g_doorbells = 0;
	g_stub_hits = 0;
	g_stub_rejects = 0;
	g_packet_count = 0;
}

void backend_ring_register(struct amdgpu_ring *ring)
{
	if (g_ring_count >= MAX_RINGS) {
		fprintf(stderr, "backend_mem: more than %u rings, raise MAX_RINGS\n", MAX_RINGS);
		exit(2);
	}
	g_ring[g_ring_count].ring = ring;
	g_ring[g_ring_count].decoded_to = 0;
	g_ring_count++;
}

unsigned int backend_doorbell_count(void) { return g_doorbells; }
unsigned int backend_cp_stub_count(void)  { return g_stub_hits; }
unsigned int backend_packet_count(void)   { return g_packet_count; }
void backend_cp_stub_enable(int on)       { g_stub_on = on; }
const struct backend_packet *backend_packets(void) { return g_packet; }

/* --- the memory backend ---------------------------------------------------------------------- */

int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain,
			 unsigned int size, unsigned int align, struct bc250_mem *out)
{
	u64 *next;
	void *cpu;

	(void)adev;

	if (out == NULL || size == 0)
		return -22;                             /* -EINVAL, as bc250_gmc.h spells it */
	if (g_alloc_count >= MAX_ALLOCS) {
		fprintf(stderr, "backend_mem: more than %u allocations, raise MAX_ALLOCS\n", MAX_ALLOCS);
		exit(2);
	}
	if (align == 0)
		align = 4;

	next = (domain == BC250_MEM_VRAM) ? &g_vram_next : &g_gtt_next;
	*next = (*next + align - 1u) & ~((u64)align - 1u);

	/* Zeroed, because both the kernel's buffer objects and the miniport's allocations are: the
	 * MQD builders assume every field they do not set is already 0. */
	cpu = calloc(1, size);
	if (cpu == NULL)
		return -12;                             /* -ENOMEM */

	out->cpu = cpu;
	out->mc = *next;
	out->size = size;
	*next += size;

	g_alloc[g_alloc_count].cpu = cpu;
	g_alloc[g_alloc_count].mc = out->mc;
	g_alloc[g_alloc_count].size = size;
	g_alloc_count++;
	return 0;
}

void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *m)
{
	unsigned int i;

	(void)adev;
	if (m == NULL || m->cpu == NULL)
		return;

	for (i = 0; i < g_alloc_count; i++) {
		if (g_alloc[i].cpu == m->cpu) {
			free(g_alloc[i].cpu);
			g_alloc[i] = g_alloc[g_alloc_count - 1];
			g_alloc_count--;
			break;
		}
	}
	memset(m, 0, sizeof(*m));
}

/* --- the doorbell and the CP stub -------------------------------------------------------------- */

/* What a ring test's packet must be. gfx_v10_0_ring_test_ring() writes 0xCAFEDEAD to SCRATCH_REG0
 * and submits one PACKET3_SET_UCONFIG_REG that sets the same register to 0xDEADBEEF; nothing else in
 * this window submits a SET_UCONFIG_REG at all. */
#define RING_TEST_VALUE   0xDEADBEEFu

void backend_cp_stub_expect(u32 scratch_reg0_dword_index)
{
	g_scratch_reg0_dword = scratch_reg0_dword_index;
}

unsigned int backend_cp_stub_rejects(void) { return g_stub_rejects; }

/*
 * Execute one decoded packet, if it is the one the stub knows. Returns 1 if it did.
 *
 * This is a check, not an answer. Every condition below has to hold before anything is written, and
 * a packet that looks like a ring test but is malformed is counted as a rejection rather than
 * quietly ignored - so a shim that built the right packet at the wrong place, or the wrong packet at
 * the right place, fails loudly instead of merely timing out:
 *
 *   - the opcode is PACKET3_SET_UCONFIG_REG and the count says exactly one register;
 *   - the register it names, after adding PACKET3_SET_UCONFIG_REG_START, is SCRATCH_REG0, which the
 *     test resolves through AMD's headers and hands to backend_cp_stub_expect();
 *   - the value is 0xDEADBEEF;
 *   - the packet was found inside the dwords the doorbell announced, which is what the decode loop
 *     in bc250_shim_wdoorbell64() guarantees by never looking past the write pointer.
 *
 * The last one is the point of doing this at the doorbell rather than by scanning the ring: a packet
 * written past the write pointer is a packet the CP would never have seen.
 */
static int stub_execute(const struct backend_packet *p)
{
	u32 reg_dword, value;

	if (p->opcode != PACKET3_SET_UCONFIG_REG)
		return 0;                       /* not a ring test; SET_RESOURCES and the rest */

	if (p->count != 1 || p->body_dwords < 2) {
		g_stub_rejects++;
		return 0;
	}

	reg_dword = p->body[0] + PACKET3_SET_UCONFIG_REG_START;
	value = p->body[1];

	if (g_scratch_reg0_dword != 0 && reg_dword != g_scratch_reg0_dword) {
		fprintf(stderr, "backend_mem: SET_UCONFIG_REG names dword 0x%X, not SCRATCH_REG0 0x%X\n",
			reg_dword, g_scratch_reg0_dword);
		g_stub_rejects++;
		return 0;
	}
	if (value != RING_TEST_VALUE) {
		fprintf(stderr, "backend_mem: ring test writes %08X, not %08X\n",
			value, RING_TEST_VALUE);
		g_stub_rejects++;
		return 0;
	}

	backend_poke(reg_dword * 4u, value);
	return 1;
}

void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index, unsigned long long value)
{
	unsigned int i;
	struct ring_slot *slot = NULL;
	struct amdgpu_ring *ring;
	u64 at;

	(void)adev;
	g_doorbells++;

	for (i = 0; i < g_ring_count; i++) {
		if (g_ring[i].ring->doorbell_index == index) {
			slot = &g_ring[i];
			break;
		}
	}
	if (slot == NULL)
		return;                 /* a ring the test did not register: nothing to decode */

	ring = slot->ring;
	if (ring->ring == NULL)
		return;

	/* A doorbell that does not advance the write pointer means the ring was re-initialised: the
	 * MQD builders set ring->wptr = 0, exactly as amdgpu_ring_init_mqd() does, and MAP_QUEUES then
	 * gives the CP an MQD whose cp_hqd_pq_rptr is 0 too, so the CP starts reading from the
	 * beginning again. This does the same.
	 *
	 * The test is `<=`, not `<`, and that matters: a compute ring's only submission is a 3-dword
	 * ring test, so after a teardown and a second bring-up its write pointer is 3 again - the same
	 * value, not a smaller one. With `<` the decoder saw nothing new and every ring test in the
	 * second bring-up timed out. `<=` is sound because a doorbell is only ever rung after the
	 * write pointer has advanced, so a value that has not advanced cannot mean anything else. */
	if (value <= slot->decoded_to)
		slot->decoded_to = 0;

	/* The doorbell value is the write pointer. For the CP rings it counts dwords, which is what
	 * bc250_ring.c writes; an SDMA ring would count bytes, and no SDMA ring is rung here. */
	for (at = slot->decoded_to; at < value; ) {
		u32 header = ring->ring[(size_t)(at & ring->buf_mask)];
		unsigned int count, body, k;
		struct backend_packet *p;

		/* The pad amdgpu_ring_insert_nop() writes is ring->funcs->nop, which on the CP rings
		 * is 0xffff1000: a type-3 header whose count field is 0x3fff. Decoding it as a packet
		 * would step 16 KB forward and lose everything behind it, so it is recognised as the
		 * one-dword filler it is, exactly as the dump in replay_gfx.c does. */
		if (header == ring->funcs->nop) {
			at++;
			continue;
		}
		if (CP_PACKET_GET_TYPE(header) != PACKET_TYPE3) {
			at++;
			continue;
		}

		count = CP_PACKET_GET_COUNT(header);
		body = count + 1u;      /* a PACKET3 is one header plus count + 1 body dwords */

		if (g_packet_count < MAX_PACKETS) {
			p = &g_packet[g_packet_count++];
			memset(p, 0, sizeof(*p));
			p->doorbell_index = index;
			p->opcode = CP_PACKET3_GET_OPCODE(header);
			p->count = count;
			p->first_dword = (unsigned int)(at & ring->buf_mask);
			p->body_dwords = body < 8u ? body : 8u;
			for (k = 0; k < p->body_dwords; k++)
				p->body[k] = ring->ring[(size_t)((at + 1u + k) & ring->buf_mask)];

			if (g_stub_on)
				g_stub_hits += (unsigned int)stub_execute(p);
		}

		at += 1u + body;
	}
	slot->decoded_to = value;
}
