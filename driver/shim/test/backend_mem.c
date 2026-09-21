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
 * encoder does. Same rule for the interrupt client and source ids the end-of-pipe delivery below
 * puts into a vector: they come from AMD's headers, not from numbers typed here. */
#include "nvd.h"
#include "soc15_ih_clientid.h"
#include "irqsrcs_gfx_10_1.h"
#include <navi10_enum.h>                /* CACHE_FLUSH_AND_INV_TS_EVENT */
#include "gc/gc_10_1_0_offset.h"        /* mmCPC_INT_STATUS */
#include "soc15_common.h"               /* SOC15_REG_OFFSET() */

/* ---------------------------------------------------------------------------------------------
 * The three dwords of a fence that unit A's own Linux driver pins
 *
 * Until E13 the stub checked the shape of a fence packet - opcode, count, DATA_SEL, INT_SEL - and
 * nothing else, so a wrong cache-flush event or a fence aimed at the wrong register would have gone
 * through. amdgpu on unit A emits these three words and no others, in three boots:
 *
 *   evidence/linux/2026-09-21-E13-reference-2/boot3-readonly/rings-after-ib/
 *       amdgpu_ring_gfx_0.0.0.txt        dw 0x0511 and 0x0611
 *       amdgpu_ring_comp_1.<p>.<q>.txt   dw 0x010C and 0x020C, all eight rings
 *           c0064900 06603514 22000000 <addr_lo> 00000000 <seq> 00000000 00000000
 *       amdgpu_ring_kiq_0.2.1.0.txt      dw 0x0709 and 0x0E09, the interrupting fences only
 *           c0033700 00100000 000030b5 00000000 20000000
 *
 * They are written here as the expressions that produce them, out of AMD's own headers, so that the
 * check cannot drift away from the encoder by way of a constant typed twice. The comments give the
 * value each one has on this chip; the compiler, not this file, decides it. */
#define FENCE_RELEASE_MEM_CNTL                                                  \
	((u32)(PACKET3_RELEASE_MEM_GCR_SEQ |                                    \
	       PACKET3_RELEASE_MEM_GCR_GL2_WB |                                 \
	       PACKET3_RELEASE_MEM_GCR_GLM_INV |                                \
	       PACKET3_RELEASE_MEM_GCR_GLM_WB |                                 \
	       PACKET3_RELEASE_MEM_CACHE_POLICY(3) |                            \
	       PACKET3_RELEASE_MEM_EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT) |   \
	       PACKET3_RELEASE_MEM_EVENT_INDEX(5)))           /* 06603514 */

/* The two WRITE_DATA control dwords the KIQ fence builds: to memory for the sequence number, to a
 * register for the interrupt. ENGINE_SEL(0) contributes nothing and is kept because the encoder
 * writes it. */
#define FENCE_WRITE_DATA_CNTL_MEM                                               \
	((u32)(WRITE_DATA_ENGINE_SEL(0) | WRITE_DATA_DST_SEL(5) | WR_CONFIRM))  /* 00100500 */
#define FENCE_WRITE_DATA_CNTL_REG                                               \
	((u32)(WRITE_DATA_ENGINE_SEL(0) | WRITE_DATA_DST_SEL(0) | WR_CONFIRM))  /* 00100000 */

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

/* Every doorbell, with the width of the store. The width is recorded because it is the one thing
 * about a doorbell a register trace can never show and upstream is specific about: the IH ring's
 * read pointer is a 32-bit store and everything else on this part is 64-bit. See bc250_shim.h. */
#define MAX_DOORBELLS 64u
static struct backend_doorbell g_doorbell[MAX_DOORBELLS];

/* The interrupt ring the stub delivers end-of-pipe vectors into, when a test has attached one. NULL
 * for every run that is only about register writes, which is all of replay_gfx.c. */
static struct amdgpu_device *g_ih_adev;
static u32 g_ih_wptr;                   /* byte offset, the stub's own copy of the hardware's */
static unsigned int g_ih_delivered;
static u64 g_ih_timestamp;

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

	g_ih_adev = NULL;
	g_ih_wptr = 0;
	g_ih_delivered = 0;
	g_ih_timestamp = 0;
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

/* The CPU side of an MC address, if `size` bytes from it lie inside one allocation this file handed
 * out. NULL otherwise, which is how the stub refuses a packet pointing somewhere the GPU could not
 * have written. Linear: there are at most a few dozen allocations. */
static void *mc_to_cpu(u64 mc, u32 size)
{
	unsigned int i;

	for (i = 0; i < g_alloc_count; i++) {
		if (mc >= g_alloc[i].mc && mc + size <= g_alloc[i].mc + g_alloc[i].size)
			return (char *)g_alloc[i].cpu + (size_t)(mc - g_alloc[i].mc);
	}
	return NULL;
}

unsigned int backend_doorbell_count(void) { return g_doorbells; }
const struct backend_doorbell *backend_doorbells(void) { return g_doorbell; }

static void doorbell_record(unsigned int index, unsigned long long value, unsigned int width)
{
	if (g_doorbells < MAX_DOORBELLS) {
		g_doorbell[g_doorbells].index = index;
		g_doorbell[g_doorbells].value = value;
		g_doorbell[g_doorbells].width = width;
	}
	g_doorbells++;
}

/* amdgpu's WDOORBELL32. No CP ring is rung this way, so there is nothing to decode: the only caller
 * on this part is bc250_ih_set_rptr(), and what the test checks is that it was 32 bits wide. */
void bc250_shim_wdoorbell32(struct amdgpu_device *adev, unsigned int index, unsigned int value)
{
	(void)adev;
	doorbell_record(index, value, 32);
}
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
/* ---------------------------------------------------------------------------------------------
 * The second half of the stub: a fence, and the interrupt it raises
 *
 * bc250_gfx_emit_fence() submits one RELEASE_MEM (gfx and compute rings) or two WRITE_DATA packets
 * (the KIQ). On hardware the CP writes the sequence number to memory and, if the packet asked for
 * it, raises an end-of-pipe interrupt, which arrives as a 32-byte vector in the IH ring. Both
 * halves are modelled here so that a host run can go all the way from "emit a fence" to
 * "bc250_ih_decode() returns a gfx EOP", which is the thing the first hardware run will check.
 *
 * The same discipline as above applies: this executes the packet it was given, it does not invent
 * one. The address has to resolve to an allocation this file handed out, the data-select has to be
 * one of the two the emitter can produce, and a packet that does not check out is a rejection.
 * A test that wants the interrupt half has to attach a ring with backend_ih_attach(); without one,
 * the fence value is still written and nothing is delivered.
 * ------------------------------------------------------------------------------------------- */

void backend_ih_attach(struct amdgpu_device *adev)
{
	g_ih_adev = adev;
	g_ih_wptr = 0;
	g_ih_delivered = 0;
}

void backend_ih_detach(void) { g_ih_adev = NULL; }
unsigned int backend_ih_delivered(void) { return g_ih_delivered; }
u32 backend_ih_wptr(void) { return g_ih_wptr; }

/* Put one interrupt vector in the ring and publish the write pointer, the way the IH block does:
 * eight dwords at the write pointer, then the write pointer into its write-back slot.
 *
 * The field packing is amdgpu_ih_decode_iv_helper()'s, read backwards. That is deliberate and it is
 * the point of doing it here rather than by handing bc250_ih_decode() a struct: the decode under
 * test and this encoder were written from the same upstream lines, so a field either side got wrong
 * shows up as a mismatch instead of cancelling out. */
static void deliver_iv(u32 client_id, u32 src_id, u32 ring_id, const u32 src_data[4])
{
	struct amdgpu_ih_ring *ih;
	volatile u32 *at;
	u32 index, k;

	if (g_ih_adev == NULL)
		return;
	ih = &g_ih_adev->irq.ih;
	if (ih->ring == NULL || ih->ring_size == 0)
		return;

	index = (g_ih_wptr & ih->ptr_mask) >> 2;
	at = (volatile u32 *)ih->ring + index;

	g_ih_timestamp++;
	at[0] = (client_id & 0xffu) | ((src_id & 0xffu) << 8) | ((ring_id & 0xffu) << 16);
	at[1] = (u32)(g_ih_timestamp & 0xffffffffu);
	at[2] = (u32)((g_ih_timestamp >> 32) & 0xffffu);
	at[3] = 0;
	for (k = 0; k < 4; k++)
		at[4 + k] = src_data != NULL ? src_data[k] : 0;

	g_ih_wptr = (g_ih_wptr + 32u) & ih->ptr_mask;
	if (ih->wptr_cpu != NULL)
		*ih->wptr_cpu = g_ih_wptr;
	g_ih_delivered++;
}

/* The ring_id the CP puts in a vector. gfx_v10_0.c:9195-9197 packs me, pipe and queue into it; this
 * is that expression solved for ring_id, from the ring the doorbell belonged to.
 *
 * Against unit A, evidence/linux/2026-09-21-E13-reference-2/boot3-readonly/amdgpu-events-ib.txt
 * lines 14-25: the gfx ring gives ring 0, the eight compute rings give 4, 5, 6, 7 and 20, 21, 22,
 * 23, and the KIQ - named kiq_0.2.1.0 in the ring dumps, so me 2, pipe 1, queue 0 - gives 9. This
 * expression reproduces all ten. */
static u32 eop_ring_id(const struct amdgpu_ring *ring)
{
	return ((ring->me & 0x3u) << 2) | (ring->pipe & 0x3u) | ((ring->queue & 0x7u) << 4);
}

static void deliver_eop(const struct amdgpu_ring *ring)
{
	if (ring == NULL)
		return;
	deliver_iv(SOC15_IH_CLIENTID_GRBM_CP, GFX_10_1__SRCID__CP_EOP_INTERRUPT,
		   eop_ring_id(ring), NULL);
}

/* PACKET3_RELEASE_MEM, as gfx_v10_0_ring_emit_fence() builds it. Returns 1 if it was executed. */
static int stub_release_mem(const struct backend_packet *p, const struct amdgpu_ring *ring)
{
	u32 dw2, data_sel, int_sel;
	u64 addr, seq;
	void *dst;

	/* Seven body dwords: two control, the 64-bit address, the 64-bit data, and a trailing zero. */
	if (p->count != 6 || p->body_dwords < 7) {
		fprintf(stderr, "backend_mem: RELEASE_MEM count %u, body %u; expected 6 and 7\n",
			p->count, p->body_dwords);
		g_stub_rejects++;
		return 0;
	}

	/* The first body dword carries the cache operations and the event the CP raises. It is the
	 * same on every fence unit A's Linux driver emits, so it is checked whole rather than by
	 * field: a wrong event type or a missing GCR bit is a wrong fence even when the sequence
	 * number still lands. */
	if (p->body[0] != FENCE_RELEASE_MEM_CNTL) {
		fprintf(stderr, "backend_mem: RELEASE_MEM control dword %08X; amdgpu emits %08X on"
				" unit A (E13 rings-after-ib)\n",
			p->body[0], FENCE_RELEASE_MEM_CNTL);
		g_stub_rejects++;
		return 0;
	}

	dw2 = p->body[1];
	data_sel = (dw2 >> 29) & 0x7u;          /* PACKET3_RELEASE_MEM_DATA_SEL(x) is (x) << 29 */
	int_sel = (dw2 >> 24) & 0x7u;           /* PACKET3_RELEASE_MEM_INT_SEL(x)  is (x) << 24 */

	addr = (u64)p->body[2] | ((u64)p->body[3] << 32);
	seq = (u64)p->body[4] | ((u64)p->body[5] << 32);

	if (data_sel != 1u && data_sel != 2u) {
		fprintf(stderr, "backend_mem: RELEASE_MEM DATA_SEL %u; the emitter only makes 1 and 2\n",
			data_sel);
		g_stub_rejects++;
		return 0;
	}

	dst = mc_to_cpu(addr, data_sel == 2u ? 8u : 4u);
	if (dst == NULL) {
		fprintf(stderr, "backend_mem: RELEASE_MEM writes 0x%llX, which is in no allocation\n",
			(unsigned long long)addr);
		g_stub_rejects++;
		return 0;
	}

	if (data_sel == 2u)
		*(volatile u64 *)dst = seq;
	else
		*(volatile u32 *)dst = (u32)seq;

	/* INT_SEL 2 is "interrupt when the data write is confirmed", which is what the emitter puts
	 * there for AMDGPU_FENCE_FLAG_INT; 0 is no interrupt, and that is the control the test runs. */
	if (int_sel != 0)
		deliver_eop(ring);

	return 1;
}

/* PACKET3_WRITE_DATA, the two shapes gfx_v10_0_ring_emit_fence_kiq() builds: DST_SEL 5 writes the
 * sequence number to memory, DST_SEL 0 writes CPC_INT_STATUS, which is how the KIQ raises its
 * interrupt. Returns 1 if it was executed. */
static int stub_write_data(const struct backend_packet *p, const struct amdgpu_ring *ring)
{
	u32 cntl, dst_sel;
	u64 addr;

	if (p->count != 3 || p->body_dwords < 4) {
		fprintf(stderr, "backend_mem: WRITE_DATA count %u, body %u; expected 3 and 4\n",
			p->count, p->body_dwords);
		g_stub_rejects++;
		return 0;
	}

	cntl = p->body[0];
	dst_sel = (cntl >> 8) & 0xfu;           /* WRITE_DATA_DST_SEL(x) is (x) << 8 */
	addr = (u64)p->body[1] | ((u64)p->body[2] << 32);

	/* Both control dwords are pinned, not just DST_SEL: without WR_CONFIRM the CP would not wait
	 * for the write, and the fence would be signalled before its value was visible. */
	if (cntl != FENCE_WRITE_DATA_CNTL_MEM && cntl != FENCE_WRITE_DATA_CNTL_REG) {
		fprintf(stderr, "backend_mem: WRITE_DATA control dword %08X; amdgpu emits %08X to"
				" memory and %08X to a register on unit A (E13 rings-after-ib)\n",
			cntl, FENCE_WRITE_DATA_CNTL_MEM, FENCE_WRITE_DATA_CNTL_REG);
		g_stub_rejects++;
		return 0;
	}

	if (dst_sel == 5u) {                    /* memory */
		void *dst = mc_to_cpu(addr, 4u);

		if (dst == NULL) {
			fprintf(stderr, "backend_mem: WRITE_DATA writes 0x%llX, which is in no"
					" allocation\n", (unsigned long long)addr);
			g_stub_rejects++;
			return 0;
		}
		*(volatile u32 *)dst = p->body[3];
		return 1;
	}

	if (dst_sel == 0u) {                    /* a register; the KIQ's interrupt trigger */
		u32 reg_dword = p->body[1];     /* DST_SEL 0 puts a dword index where the address goes */
		const struct amdgpu_device *adev = ring != NULL ? ring->adev : NULL;

		if (adev == NULL) {
			fprintf(stderr, "backend_mem: WRITE_DATA to a register from a ring with no"
					" device; the register cannot be named\n");
			g_stub_rejects++;
			return 0;
		}

		/* There is exactly one register a fence may write, and naming it through AMD's header
		 * rather than by its number is what makes this a check and not a restatement. On unit
		 * A amdgpu writes dword 0x30b5 here (E13 amdgpu_ring_kiq_0.2.1.0.txt dw 0x0709 and
		 * 0x0E09), which is mmCPC_INT_STATUS over the GC base of this SoC. */
		if (reg_dword != SOC15_REG_OFFSET(GC, 0, mmCPC_INT_STATUS)) {
			fprintf(stderr, "backend_mem: a fence writes register dword 0x%X; the only one"
					" it may write is mmCPC_INT_STATUS, dword 0x%X\n",
				reg_dword, SOC15_REG_OFFSET(GC, 0, mmCPC_INT_STATUS));
			g_stub_rejects++;
			return 0;
		}

		backend_poke(reg_dword * 4u, p->body[3]);
		/* Unit A sends this vector with ring_id 9, the KIQ's me 2, pipe 1, queue 0: E13
		 * amdgpu-events-ib.txt:23, client_id 20 src_id 178 ring 9. It used to be sent as 0
		 * here, which nothing reads - neither gfx_v10_0_kiq_irq() (gfx_v10_0.c:9456) nor
		 * bc250_ih_is_kiq() looks at ring_id for the KIQ - but a test that delivers a vector
		 * hardware does not send is testing the wrong thing. */
		deliver_iv(SOC15_IH_CLIENTID_GRBM_CP, GFX_10_1__SRCID__CP_IB2_INTERRUPT_PKT,
			   eop_ring_id(ring), NULL);
		return 1;
	}

	fprintf(stderr, "backend_mem: WRITE_DATA DST_SEL %u; the emitter only makes 0 and 5\n", dst_sel);
	g_stub_rejects++;
	return 0;
}

static int stub_execute(const struct backend_packet *p, const struct amdgpu_ring *ring)
{
	u32 reg_dword, value;

	if (p->opcode == PACKET3_RELEASE_MEM)
		return stub_release_mem(p, ring);
	if (p->opcode == PACKET3_WRITE_DATA)
		return stub_write_data(p, ring);

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
	doorbell_record(index, value, 64);

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
				g_stub_hits += (unsigned int)stub_execute(p, ring);
		}

		at += 1u + body;
	}
	slot->decoded_to = value;
}
