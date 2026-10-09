/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Packet check for the gfx job frame and for the VM flush on the ring: option (b) of
 * docs/design/gfx-submit-root-serialization.md.
 *
 * Two questions, and the first one is the more important of the two:
 *
 *   1. With the gate off (bc250_gfx_submit_job), is the frame the same 23 dwords as before this
 *      change? The golden array below is written out dword for dword from the packet constants of
 *      driver/amdgpu-import/nvd.h. A dump of a 0x116 is read against that frame, so it may not
 *      move.
 *   2. With the gate on (bc250_gfx_submit_job_vm), are the 29 dwords in front of the frame what
 *      gmc_v10_0_emit_flush_gpu_tlb() and gfx_v10_0_ring_emit_vm_flush() emit: the register pair
 *      of the context, the invalidate request of the hub, the acknowledge wait with one bit per
 *      VMID, and the engine the caller named? And is the frame behind them still the golden one?
 *
 * The register offsets the packets carry are not written here. The hub is filled in by the
 * imported gfxhub_v2_0_init() over AMD's own headers, and this file computes what it expects with
 * SOC15_REG_OFFSET over the same headers and the mm* names of the three distances. Two
 * derivations, one source; a mistake in either one of them fails the check.
 *
 * Nothing here needs a device, a trace or the lab. Links against driver/shim/bc250_gfx.c,
 * bc250_ring.c, bc250_gmc.c, bc250_nbio.c, shim.c and the imported hub sources, with a plain shim
 * backend below, the same shape driver/shim/test/paging_packets.c supplies for the same reason.
 *
 * The negative control is run_gfx_vm_flush.ps1 -WrongAckMask: it compiles a copy of bc250_gfx.c
 * whose acknowledge wait masks every bit instead of the VMID's own. That build must fail here.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "bc250_gfx.h"
#include "bc250_gmc.h"
#include "nv.h"
#include "gfxhub_v2_0.h"
#include "mmhub_v2_0.h"
#include "nvd.h"
#include "gc/gc_10_1_0_offset.h"
#include "soc15_common.h"
#include <navi10_enum.h>

/* ---------------------------------------------------------------------------------------------
 * The harness
 * ------------------------------------------------------------------------------------------- */

static unsigned int g_checks, g_failures, g_verbose, g_doorbells;

static void check(int ok, const char *what)
{
	g_checks++;
	if (!ok) {
		g_failures++;
		printf("  FAIL  %s\n", what);
	} else if (g_verbose)
		printf("  ok    %s\n", what);
}

/* struct amdgpu_device is large (facts M104), so it is static and not a local. */
static struct amdgpu_device g_adev;
static u32 g_ring_memory[2048];
static struct amdgpu_ring_funcs g_gfx_funcs = { AMDGPU_RING_TYPE_GFX, 255u, 0x80000000u };
static struct amdgpu_ring g_ring;

static u32 g_regs[0x40000];

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index)
{
	(void)adev;
	return dword_index < (sizeof(g_regs) / sizeof(g_regs[0])) ? g_regs[dword_index] : 0;
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value)
{
	(void)adev;
	if (dword_index < (sizeof(g_regs) / sizeof(g_regs[0])))
		g_regs[dword_index] = value;
}

int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain, unsigned int size,
			 unsigned int align, struct bc250_mem *out)
{
	(void)adev; (void)domain; (void)size; (void)align;
	memset(out, 0, sizeof(*out));
	return -12;
}

void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *m)
{
	(void)adev;
	if (m) memset(m, 0, sizeof(*m));
}

void bc250_shim_udelay(unsigned int usec) { (void)usec; }

void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index, unsigned long long value)
{ (void)adev; (void)index; (void)value; g_doorbells++; }

void bc250_shim_wdoorbell32(struct amdgpu_device *adev, unsigned int index, unsigned int value)
{ (void)adev; (void)index; (void)value; g_doorbells++; }

void bc250_shim_log(int level, void *dev, const char *fmt, ...)
{
	va_list ap;

	if (!g_verbose)
		return;
	(void)level; (void)dev;
	printf("        log: ");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

/* ---------------------------------------------------------------------------------------------
 * The golden frame
 *
 * The 23 dwords bc250_gfx_submit_job() writes for the inputs below, from the packet constants of
 * driver/amdgpu-import/nvd.h and third_party/linux-amdgpu/navi10_enum.h:
 *
 *   PACKET3(op, n)  = 0xC0000000 | (n << 16) | (op << 8)
 *   PFP_SYNC_ME     0x42   CONTEXT_CONTROL 0x28   FRAME_CONTROL  0x90
 *   INDIRECT_BUFFER 0x3F   RELEASE_MEM     0x49   SWITCH_BUFFER  0x8B
 *   WRITE_DATA      0x37   WAIT_REG_MEM    0x3C
 *
 * The RELEASE_MEM body: GCR_SEQ (1 << 22) | GCR_GL2_WB (1 << 21) | GCR_GLM_INV (1 << 13) |
 * GCR_GLM_WB (1 << 12) | CACHE_POLICY(3) (<< 25) | EVENT_INDEX(5) (<< 8) |
 * EVENT_TYPE(CACHE_FLUSH_AND_INV_TS_EVENT = 0x14), then DATA_SEL(1) (<< 29) | INT_SEL(2) (<< 24).
 * ------------------------------------------------------------------------------------------- */

#define TEST_IB_ADDR		0x0000000123456000ULL
#define TEST_IB_LENGTH_DW	0x100u
#define TEST_FENCE_ADDR		0x00000000ABCDE000ULL
#define TEST_SEQ		0x1234ULL
#define TEST_ROOT		0x00000002AB000000ULL
#define TEST_VMID		1u

static const u32 g_golden_frame[] = {
	0xC0004200u, 0x00000000u,                           /* PFP_SYNC_ME */
	0xC0012800u, 0x81018003u, 0x00000000u,              /* CONTEXT_CONTROL */
	0xC0009000u, 0x00000000u,                           /* FRAME_CONTROL start */
	0xC0023F00u, 0x23456000u, 0x00000001u, 0x01000100u, /* INDIRECT_BUFFER, VMID 1, 0x100 dwords */
	0xC0009000u, 0x10000000u,                           /* FRAME_CONTROL end */
	0xC0064900u, 0x06603514u, 0x22000000u,              /* RELEASE_MEM, 32-bit data, interrupt */
	0xABCDE000u, 0x00000000u, 0x00001234u, 0x00000000u, 0x00000000u,
	0xC0008B00u, 0x00000000u,                           /* SWITCH_BUFFER */
};
#define GOLDEN_FRAME_DWORDS	((unsigned int)(sizeof(g_golden_frame) / sizeof(g_golden_frame[0])))

/* One reservation of the gfx ring: amdgpu_ring_alloc() rounds every request up to
 * align_mask + 1 dwords, so the 29 dwords of the flush cost no ring room at all. */
#define RESERVATION_DWORDS	256u

/* ---------------------------------------------------------------------------------------------
 * What the flush is expected to carry, derived from AMD's headers a second time
 * ------------------------------------------------------------------------------------------- */

struct expected_flush {
	u32 root_lo_reg, root_hi_reg, req_reg, ack_reg, request, root_lo, root_hi, ack_bit;
};

static void expected_flush_of(struct expected_flush *e, u32 vmid, u64 root_phys, u32 eng)
{
	/* SOC15_REG_OFFSET() resolves the register base through a variable of this name, the same
	 * way every use of it in the shim and in the imported sources does. */
	struct amdgpu_device *adev = &g_adev;
	const u32 ctx_lo = SOC15_REG_OFFSET(GC, 0, mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32);
	const u32 ctx_hi = SOC15_REG_OFFSET(GC, 0, mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_HI32);
	const u32 ctx_addr_distance = mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32 -
				      mmGCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32;
	const u32 eng_distance = mmGCVM_INVALIDATE_ENG1_REQ - mmGCVM_INVALIDATE_ENG0_REQ;
	const u64 value = root_phys | AMDGPU_PTE_VALID;

	e->root_lo_reg = ctx_lo + ctx_addr_distance * vmid;
	e->root_hi_reg = ctx_hi + ctx_addr_distance * vmid;
	e->req_reg = SOC15_REG_OFFSET(GC, 0, mmGCVM_INVALIDATE_ENG0_REQ) + eng_distance * eng;
	e->ack_reg = SOC15_REG_OFFSET(GC, 0, mmGCVM_INVALIDATE_ENG0_ACK) + eng_distance * eng;
	e->request = g_adev.vmhub[AMDGPU_GFXHUB(0)].vmhub_funcs->get_invalidate_req(vmid, 0);
	e->root_lo = lower_32_bits(value);
	e->root_hi = upper_32_bits(value);
	e->ack_bit = 1u << vmid;
}

/* The 29 dwords of gmc_v10_0_emit_flush_gpu_tlb() as this driver writes them: two WRITE_DATA for
 * the root pair, then the request write, the acknowledge-reset cycle and the acknowledge wait. */
static void expected_flush_dwords(const struct expected_flush *e, u32 *out)
{
	const u32 wreg = 0xC0033700u;           /* PACKET3(WRITE_DATA, 3) */
	const u32 cmd = 0x40100000u;            /* WRITE_DATA_ENGINE_SEL(1) | WR_CONFIRM */
	const u32 wait = 0xC0053C00u;           /* PACKET3(WAIT_REG_MEM, 5) */
	const u32 mode = 0x00000003u;           /* register space, wait_reg_mem, function equal, ME */
	const u32 interval = 0x00000020u;

	out[0] = wreg;  out[1] = cmd;  out[2] = e->root_lo_reg; out[3] = 0; out[4] = e->root_lo;
	out[5] = wreg;  out[6] = cmd;  out[7] = e->root_hi_reg; out[8] = 0; out[9] = e->root_hi;
	out[10] = wreg; out[11] = cmd; out[12] = e->req_reg;    out[13] = 0; out[14] = e->request;
	out[15] = wait; out[16] = mode; out[17] = e->req_reg; out[18] = 0;
	out[19] = 0; out[20] = 0; out[21] = interval;
	out[22] = wait; out[23] = mode; out[24] = e->ack_reg; out[25] = 0;
	out[26] = e->ack_bit; out[27] = e->ack_bit; out[28] = interval;
}

/* ---------------------------------------------------------------------------------------------
 * Setup
 * ------------------------------------------------------------------------------------------- */

static void ring_reset(void)
{
	memset(g_ring_memory, 0xCC, sizeof(g_ring_memory));
	memset(&g_ring, 0, sizeof(g_ring));
	g_ring.adev = &g_adev;
	g_ring.funcs = &g_gfx_funcs;
	g_ring.ring = g_ring_memory;
	g_ring.ring_size = sizeof(g_ring_memory);
	g_ring.max_dw = 2048;
	g_ring.buf_mask = 2047;
	g_ring.ptr_mask = ~(u64)0;
	g_ring.track_rptr = false;
	g_ring.use_doorbell = false;
}

static int tail_untouched(unsigned int from)
{
	unsigned int i;

	for (i = from; i < 2048u; i++)
		if (g_ring_memory[i] != 0xCCCCCCCCu)
			return 0;
	return 1;
}

static int same_dwords(const u32 *got, const u32 *want, unsigned int count, const char *what)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		if (got[i] != want[i]) {
			printf("  FAIL  %s: dword %u is 0x%08lX, expected 0x%08lX\n", what, i,
			       (unsigned long)got[i], (unsigned long)want[i]);
			return 0;
		}
	return 1;
}

static void hub_init(void)
{
	memset(&g_adev, 0, sizeof(g_adev));
	/* bc250_gmc_setup()'s first three steps, which is all the hub offsets need. */
	cyan_skillfish_reg_base_init(&g_adev);
	g_adev.ip_versions[GC_HWIP][0] = IP_VERSION(10, 1, 3);
	g_adev.ip_versions[MMHUB_HWIP][0] = IP_VERSION(2, 0, 3);
	g_adev.gfxhub.funcs = &gfxhub_v2_0_funcs;
	g_adev.mmhub.funcs = &mmhub_v2_0_funcs;
	g_adev.gfxhub.funcs->init(&g_adev);
	g_adev.mmhub.funcs->init(&g_adev);
}

/* ---------------------------------------------------------------------------------------------
 * The cases
 * ------------------------------------------------------------------------------------------- */

/* Gate 0: the frame of 0.7.216.24, dword for dword. */
static void case_gate0_frame(void)
{
	printf("\ngate 0: bc250_gfx_submit_job writes the frame of 0.7.216.24\n");
	ring_reset();
	check(bc250_gfx_submit_job(&g_ring, TEST_IB_ADDR, TEST_IB_LENGTH_DW, TEST_VMID,
				   TEST_FENCE_ADDR, TEST_SEQ, AMDGPU_FENCE_FLAG_INT) == 0,
	      "the job is accepted");
	check(same_dwords(g_ring_memory, g_golden_frame, GOLDEN_FRAME_DWORDS, "gate 0 frame"),
	      "every dword of the frame is the golden one");
	check(GOLDEN_FRAME_DWORDS == 23u, "the frame is 23 dwords");
	check(tail_untouched(RESERVATION_DWORDS) && g_ring.wptr == RESERVATION_DWORDS,
	      "the frame uses one reservation and writes nothing past it");
}

/* Gate 1: the same frame, with the flush in front of it. */
static void case_gate1_frame(void)
{
	struct expected_flush e;
	u32 want[BC250_GFX_VM_FLUSH_DWORDS];

	printf("\ngate 1: bc250_gfx_submit_job_vm puts the flush in front of the same frame\n");
	check(BC250_GFX_VM_FLUSH_DWORDS == 29u, "the flush is 29 dwords");
	ring_reset();
	check(bc250_gfx_submit_job_vm(&g_ring, TEST_IB_ADDR, TEST_IB_LENGTH_DW, TEST_VMID,
				      TEST_FENCE_ADDR, TEST_SEQ, AMDGPU_FENCE_FLAG_INT, TEST_ROOT,
				      BC250_INV_ENG_GFX_RING) == 0,
	      "the job is accepted");
	expected_flush_of(&e, TEST_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING);
	expected_flush_dwords(&e, want);
	check(same_dwords(g_ring_memory, want, BC250_GFX_VM_FLUSH_DWORDS, "gate 1 flush"),
	      "the flush is what gmc_v10_0_emit_flush_gpu_tlb emits");
	check(same_dwords(g_ring_memory + BC250_GFX_VM_FLUSH_DWORDS, g_golden_frame,
			  GOLDEN_FRAME_DWORDS, "gate 1 frame"),
	      "the frame behind the flush is the golden one, unchanged");
	check(g_ring.wptr == RESERVATION_DWORDS,
	      "the flush costs no ring room: one reservation covers both");
	check(tail_untouched(RESERVATION_DWORDS), "nothing is written past the reservation");
}

/* The fields the design note names as the way to get this wrong: the register of the context pair,
 * the engine distance, the acknowledge bit and the request value. Every VMID and every engine. */
static void case_fields_against_reference(void)
{
	struct expected_flush e;
	u32 want[BC250_GFX_VM_FLUSH_DWORDS];
	u32 vmid, eng;

	printf("\nthe fields of the flush, for every VMID and every engine\n");
	for (vmid = 1; vmid < AMDGPU_NUM_VMID; vmid++) {
		ring_reset();
		check(bc250_gfx_emit_vm_flush(&g_ring, vmid, TEST_ROOT, BC250_INV_ENG_GFX_RING) == 0,
		      "a flush of every VMID 1..15 is accepted");
		expected_flush_of(&e, vmid, TEST_ROOT, BC250_INV_ENG_GFX_RING);
		expected_flush_dwords(&e, want);
		check(same_dwords(g_ring_memory, want, BC250_GFX_VM_FLUSH_DWORDS, "per-VMID flush"),
		      "the context pair follows ctx_addr_distance and the wait masks this VMID's bit");
		check(g_ring_memory[26] == (1u << vmid) && g_ring_memory[27] == (1u << vmid),
		      "the acknowledge reference and mask are the VMID's bit, not a wider mask");
		check(tail_untouched(BC250_GFX_VM_FLUSH_DWORDS), "the flush writes 29 dwords and no more");
	}
	for (eng = 0; eng < BC250_INV_ENG_COUNT; eng++) {
		ring_reset();
		check(bc250_gfx_emit_vm_flush(&g_ring, TEST_VMID, TEST_ROOT, eng) == 0,
		      "a flush on every engine 0..17 is accepted");
		expected_flush_of(&e, TEST_VMID, TEST_ROOT, eng);
		expected_flush_dwords(&e, want);
		check(same_dwords(g_ring_memory, want, BC250_GFX_VM_FLUSH_DWORDS, "per-engine flush"),
		      "the request and acknowledge registers follow eng_distance");
	}
	/* The acknowledge-reset cycle between the request and the wait: a wait on the request
	 * register with reference 0 and mask 0, which passes at once (sdma_v5_0_ring_emit_
	 * reg_write_reg_wait, "wait for a cycle to reset vm_inv_eng*_ack"). */
	ring_reset();
	check(bc250_gfx_emit_vm_flush(&g_ring, TEST_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING) == 0,
	      "the flush for the cycle check is accepted");
	expected_flush_of(&e, TEST_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING);
	check(g_ring_memory[17] == e.req_reg && g_ring_memory[19] == 0 && g_ring_memory[20] == 0,
	      "the acknowledge-reset cycle reads the request register with reference 0 and mask 0");
	check(g_ring_memory[24] == e.ack_reg && e.ack_reg != e.req_reg,
	      "the acknowledge wait reads the acknowledge register, not the request one");
}

/* The invalidation engines have one owner each (bc250_gmc.h), so the gfx ring never waits on an
 * acknowledge that the CPU path or the paging stream produced. */
static void case_engine_ownership(void)
{
	const struct amdgpu_vmhub *hub = &g_adev.vmhub[AMDGPU_GFXHUB(0)];
	struct expected_flush gfx, cpu, sdma;

	printf("\nthe invalidation engines have one owner each\n");
	check(BC250_INV_ENG_GFX_RING != BC250_INV_ENG_CPU &&
	      BC250_INV_ENG_GFX_RING != BC250_INV_ENG_SDMA_PAGING &&
	      BC250_INV_ENG_CPU != BC250_INV_ENG_SDMA_PAGING,
	      "the three engine numbers are different");
	check(BC250_INV_ENG_GFX_RING != 2u && BC250_INV_ENG_GFX_RING != 3u,
	      "the gfx ring's engine is not one of the two firmware engines of the upstream mask");
	check(BC250_INV_ENG_CPU < BC250_INV_ENG_COUNT && BC250_INV_ENG_GFX_RING < BC250_INV_ENG_COUNT,
	      "every owner's engine exists on the hub");
	expected_flush_of(&gfx, TEST_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING);
	expected_flush_of(&cpu, TEST_VMID, TEST_ROOT, BC250_INV_ENG_CPU);
	expected_flush_of(&sdma, TEST_VMID, TEST_ROOT, BC250_INV_ENG_SDMA_PAGING);
	check(gfx.req_reg != cpu.req_reg && gfx.ack_reg != cpu.ack_reg,
	      "the gfx ring and the CPU path write different request and acknowledge registers");
	check(gfx.req_reg != sdma.req_reg && gfx.ack_reg != sdma.ack_reg,
	      "the gfx ring and the paging stream write different request and acknowledge registers");
	check(hub->eng_distance != 0 && hub->ctx_addr_distance != 0,
	      "the imported hub init filled in both distances");
}

/* Every refusal leaves the ring exactly as it was. */
static void case_refusals(void)
{
	struct amdgpu_vmhub saved;
	struct amdgpu_vmhub *hub = &g_adev.vmhub[AMDGPU_GFXHUB(0)];
	struct amdgpu_ring_funcs sdma_funcs = { AMDGPU_RING_TYPE_SDMA, 7u, 0u };

	printf("\nrefusals write nothing\n");
	ring_reset();
	check(bc250_gfx_emit_vm_flush(&g_ring, 0, TEST_ROOT, BC250_INV_ENG_GFX_RING) == BC250_EINVAL,
	      "VMID 0 is refused: the GART aperture's root is not ours to move");
	check(bc250_gfx_emit_vm_flush(&g_ring, AMDGPU_NUM_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING) ==
	      BC250_EINVAL, "a VMID of 16 is refused before the shift");
	check(bc250_gfx_emit_vm_flush(&g_ring, ~0u, TEST_ROOT, BC250_INV_ENG_GFX_RING) == BC250_EINVAL,
	      "a very large VMID is refused before the shift");
	check(bc250_gfx_emit_vm_flush(&g_ring, TEST_VMID, TEST_ROOT | 0x800ULL,
				      BC250_INV_ENG_GFX_RING) == BC250_EINVAL,
	      "a root that is not page aligned is refused");
	check(bc250_gfx_emit_vm_flush(&g_ring, TEST_VMID, TEST_ROOT, BC250_INV_ENG_COUNT) ==
	      BC250_EINVAL, "an engine the hub does not have is refused");
	check(bc250_gfx_emit_vm_flush(NULL, TEST_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING) ==
	      BC250_EINVAL, "no ring is refused");
	check(tail_untouched(0) && g_ring.wptr == 0, "none of those refusals wrote a dword");

	g_ring.funcs = &sdma_funcs;
	check(bc250_gfx_emit_vm_flush(&g_ring, TEST_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING) ==
	      BC250_EINVAL, "a ring that is not a gfx ring is refused");
	check(bc250_gfx_submit_job_vm(&g_ring, TEST_IB_ADDR, TEST_IB_LENGTH_DW, TEST_VMID,
				      TEST_FENCE_ADDR, TEST_SEQ, AMDGPU_FENCE_FLAG_INT, TEST_ROOT,
				      BC250_INV_ENG_GFX_RING) == BC250_EINVAL,
	      "a job on a ring that is not a gfx ring is refused");
	g_ring.funcs = &g_gfx_funcs;

	saved = *hub;
	memset(hub, 0, sizeof(*hub));
	check(bc250_gfx_emit_vm_flush(&g_ring, TEST_VMID, TEST_ROOT, BC250_INV_ENG_GFX_RING) ==
	      BC250_EINVAL, "a hub whose init has not run is refused");
	*hub = saved;
	check(tail_untouched(0) && g_ring.wptr == 0, "the ring is still untouched");

	/* A bad flush descriptor must not leave half a frame behind either: the job reserves,
	 * refuses inside the flush and undoes the reservation. */
	ring_reset();
	check(bc250_gfx_submit_job_vm(&g_ring, TEST_IB_ADDR, TEST_IB_LENGTH_DW, TEST_VMID,
				      TEST_FENCE_ADDR, TEST_SEQ, AMDGPU_FENCE_FLAG_INT,
				      TEST_ROOT | 0x800ULL, BC250_INV_ENG_GFX_RING) == BC250_EINVAL,
	      "a job with a root that is not page aligned is refused");
	check(g_ring.wptr == 0, "the refused job left the write pointer where it was");
	ring_reset();
	check(bc250_gfx_submit_job_vm(&g_ring, TEST_IB_ADDR, TEST_IB_LENGTH_DW, 0, TEST_FENCE_ADDR,
				      TEST_SEQ, AMDGPU_FENCE_FLAG_INT, TEST_ROOT,
				      BC250_INV_ENG_GFX_RING) == BC250_EINVAL,
	      "a job at VMID 0 stays on bc250_gfx_submit_ib with either gate");
	check(tail_untouched(0) && g_ring.wptr == 0, "it wrote nothing");
}

int main(int argc, char **argv)
{
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			g_verbose = 1;

	printf("The gfx job frame and the VM flush on the ring (option (b) of\n");
	printf("docs/design/gfx-submit-root-serialization.md): driver/shim/bc250_gfx.c against the\n");
	printf("frozen frame of 0.7.216.24 and against gmc_v10_0_emit_flush_gpu_tlb().\n");

	hub_init();
	case_gate0_frame();
	case_gate1_frame();
	case_fields_against_reference();
	case_engine_ownership();
	case_refusals();

	printf("\n== verdict ==\n");
	printf("  %u checks, %u failures\n", g_checks, g_failures);
	printf("  %s\n", g_failures == 0 ? "PASS" : "FAIL");
	return g_failures == 0 ? 0 : 1;
}
