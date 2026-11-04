/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host check of the per-queue resets of driver/shim/bc250_gfx.c (bc250_gfx_reset_kcq and
 * bc250_gfx_reset_kgq, docs/design/queue-reset.md section 8, the shim half).
 *
 * The model: a register file, a KIQ ring and one compute and one gfx ring in host memory. A ring test
 * writes 0xCAFEDEAD to SCRATCH_REG0 and polls for 0xDEADBEEF. The model answers each ring test in
 * the order the code runs them, and a case can make any one of them stay silent. CP_HQD_ACTIVE reads
 * 1 under the queue select until the model lets it go.
 *
 * What is checked:
 *   1. the packet words on the KIQ: UNMAP_QUEUES and MAP_QUEUES for compute, and the 19 dwords of
 *      reset_kgq for gfx, against an encoding written out here from nvd.h.
 *   2. the order: unmap, HQD poll, map, ring test of the queue. No map after a failed unmap.
 *   3. each step fails in turn: the step is named, no later step runs, the select is back at 0.
 *   4. success: the ring is at 0 and clean, the MQD read pointer is 0.
 *
 * Nothing here needs a device or the lab.
 */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "bc250_gfx.h"
#include "nv.h"
#include "nvd.h"
#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"
#include "v10_structs.h"
#include <navi10_enum.h>

static unsigned int g_checks, g_failures, g_verbose;

static void check(int ok, const char *what)
{
	g_checks++;
	if (!ok) {
		g_failures++;
		printf("  FAIL  %s\n", what);
	} else if (g_verbose)
		printf("  ok    %s\n", what);
}

/* ---------------------------------------------------------------------------------------------
 * The model
 * ------------------------------------------------------------------------------------------- */

static struct amdgpu_device g_adev;
static u32 g_regs[0x40000];

#define RING_DW 1024u
static u32 g_kiq_mem[RING_DW], g_kcq_mem[RING_DW], g_kgq_mem[RING_DW];
static u64 g_kiq_wb, g_kcq_wb, g_kgq_wb;
static struct v10_compute_mqd g_kcq_mqd;
static struct v10_gfx_mqd g_kgq_mqd;
static const struct amdgpu_ring_funcs g_kiq_funcs = { AMDGPU_RING_TYPE_KIQ, 0u, 0xFFFF1000u };
static const struct amdgpu_ring_funcs g_kcq_funcs = { AMDGPU_RING_TYPE_COMPUTE, 0u, 0xFFFF1000u };
static const struct amdgpu_ring_funcs g_kgq_funcs = { AMDGPU_RING_TYPE_GFX, 0u, 0xFFFF1000u };
static struct amdgpu_ring *g_kiq, *g_kcq, *g_kgq;

static u32 g_scratch, g_grbm_cntl, g_hqd_polls_until_idle, g_hqd_selected_reads;
static unsigned int g_ring_tests, g_silent_test;	/* 1-based index of the ring test that fails */
static int g_select_left_set;
static u32 g_dequeue_writes, g_dequeue_last, g_fallback_frees;	/* the fallback of 5.3 */


unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int i)
{
	adev = &g_adev;
	if (i == (u32)SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0))
		return g_scratch;
	if (i == (u32)SOC15_REG_OFFSET(GC, 0, mmCP_HQD_ACTIVE)) {
		if (g_grbm_cntl == 0)
			return 0;
		g_hqd_selected_reads++;
		if (g_hqd_polls_until_idle == 0)
			return 0;
		g_hqd_polls_until_idle--;
		return 1;
	}
	return i < 0x40000u ? g_regs[i] : 0;
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int i, unsigned int v)
{
	adev = &g_adev;
	if (i == (u32)SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0)) {
		if (v == 0xCAFEDEADu)
			g_ring_tests++;
		g_scratch = v;
		return;
	}
	if (i == (u32)SOC15_REG_OFFSET(GC, 0, mmGRBM_GFX_CNTL))
		g_grbm_cntl = v;
	if (i == (u32)SOC15_REG_OFFSET(GC, 0, mmCP_HQD_DEQUEUE_REQUEST)) {
		g_dequeue_writes++;
		g_dequeue_last = v;
		/* a case can let the direct request end what the KIQ unmap did not */
		if (v == 1 && g_fallback_frees && g_grbm_cntl != 0)
			g_hqd_polls_until_idle = 2;
	}
	if (i < 0x40000u)
		g_regs[i] = v;
}

/* The CP: a doorbell on a ring retires a pending ring test, unless the case silences that one. */
void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index, unsigned long long value)
{
	(void)adev; (void)index; (void)value;
	if (g_scratch == 0xCAFEDEADu && g_ring_tests != g_silent_test)
		g_scratch = 0xDEADBEEFu;
}

void bc250_shim_wdoorbell32(struct amdgpu_device *adev, unsigned int index, unsigned int value)
{ bc250_shim_wdoorbell64(adev, index, value); }

int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain, unsigned int size,
			 unsigned int align, struct bc250_mem *out)
{
	(void)adev; (void)domain; (void)size; (void)align;
	memset(out, 0, sizeof(*out));
	return -12;
}

void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *m)
{ (void)adev; if (m) memset(m, 0, sizeof(*m)); }

void bc250_shim_udelay(unsigned int usec) { (void)usec; }

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

static void ring_init(struct amdgpu_ring *r, const struct amdgpu_ring_funcs *f, u32 *mem, u64 *wb,
		      u32 me, u32 pipe, u32 queue, u32 doorbell, u64 gpu)
{
	memset(r, 0, sizeof(*r));
	r->adev = &g_adev;
	r->funcs = f;
	r->ring = mem;
	r->ring_size = RING_DW * 4u;
	r->max_dw = RING_DW;
	r->buf_mask = RING_DW - 1u;
	r->ptr_mask = ~(u64)0;
	r->use_doorbell = true;
	r->doorbell_index = doorbell;
	r->wptr_cpu_addr = wb;
	r->me = me;
	r->pipe = pipe;
	r->queue = queue;
	r->gpu_addr = gpu;
	r->mqd_gpu_addr = gpu + 0x100000u;
}

static void model_reset(void)
{
	memset(&g_adev, 0, sizeof(g_adev));
	cyan_skillfish_reg_base_init(&g_adev);
	g_adev.ip_versions[GC_HWIP][0] = IP_VERSION(10, 1, 3);
	g_adev.usec_timeout = 100000;
	g_adev.gfx.num_compute_rings = 1;
	g_adev.gfx.num_gfx_rings = 1;
	g_adev.gfx.async_gfx_ring = true;
	memset(g_regs, 0, sizeof(g_regs));
	{	/* the stale read pointers the hung queues leave behind (0x737: E12 run 001) */
		struct amdgpu_device *adev = &g_adev;
		g_regs[SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR)] = 0x737u;
		g_regs[SOC15_REG_OFFSET(GC, 0, mmCP_GFX_HQD_RPTR)] = 0x737u;
	}
	memset(g_kiq_mem, 0xCC, sizeof(g_kiq_mem));
	memset(g_kcq_mem, 0x77, sizeof(g_kcq_mem));
	memset(g_kgq_mem, 0x77, sizeof(g_kgq_mem));
	memset(&g_kcq_mqd, 0x55, sizeof(g_kcq_mqd));
	memset(&g_kgq_mqd, 0x55, sizeof(g_kgq_mqd));
	g_kiq = &g_adev.gfx.kiq[0].ring;
	g_kcq = &g_adev.gfx.compute_ring[0];
	g_kgq = &g_adev.gfx.gfx_ring[0];
	ring_init(g_kiq, &g_kiq_funcs, g_kiq_mem, &g_kiq_wb, 2, 1, 0, 0x10, 0x400000000ULL);
	ring_init(g_kcq, &g_kcq_funcs, g_kcq_mem, &g_kcq_wb, 1, 0, 0, 0x18, 0x500000000ULL);
	ring_init(g_kgq, &g_kgq_funcs, g_kgq_mem, &g_kgq_wb, 0, 0, 0, 0x08, 0x600000000ULL);
	g_kcq->mqd_ptr = &g_kcq_mqd;
	g_kgq->mqd_ptr = &g_kgq_mqd;
	g_kcq->wptr = 0x40; g_kcq_wb = 0x40;
	g_kgq->wptr = 0x80; g_kgq_wb = 0x80;
	g_scratch = 0;
	g_grbm_cntl = 0;
	g_hqd_polls_until_idle = 3;
	g_hqd_selected_reads = 0;
	g_ring_tests = 0;
	g_silent_test = 0;
	g_dequeue_writes = 0;
	g_dequeue_last = 0xFFFFFFFFu;
	g_fallback_frees = 0;
	{	/* the ring base and IB pointer that the witness must report */
		struct amdgpu_device *adev = &g_adev;
		g_regs[SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_BASE)] = 0x05000000u;
		g_regs[SOC15_REG_OFFSET(GC, 0, mmCP_HQD_IB_RPTR)] = 0x123u;
	}
	(void)g_select_left_set;
}

/* ---------------------------------------------------------------------------------------------
 * Expected KIQ words, from nvd.h: PACKET3(op, n) = 0xC0000000 | (n << 16) | (op << 8)
 * ------------------------------------------------------------------------------------------- */

static const u32 RING_TEST_WORDS = 3u;	/* SET_UCONFIG_REG, offset, 0xDEADBEEF */

static int ring_test_at(const u32 *w)
{
	return w[0] == (u32)PACKET3(PACKET3_SET_UCONFIG_REG, 1) && w[2] == 0xDEADBEEFu;
}

static int unmap_at(const u32 *w, u32 eng_sel, u32 doorbell)
{
	return w[0] == (u32)PACKET3(PACKET3_UNMAP_QUEUES, 4) &&
	       w[1] == (PACKET3_UNMAP_QUEUES_ACTION(1) | PACKET3_UNMAP_QUEUES_ENGINE_SEL(eng_sel) |
			PACKET3_UNMAP_QUEUES_NUM_QUEUES(1)) &&	/* action 1 = RESET_QUEUES, nvd.h:628 */
	       w[2] == PACKET3_UNMAP_QUEUES_DOORBELL_OFFSET0(doorbell) &&
	       w[3] == 0 && w[4] == 0 && w[5] == 0;
}

static int map_at(const u32 *w, const struct amdgpu_ring *r, u32 eng_sel)
{
	return w[0] == (u32)PACKET3(PACKET3_MAP_QUEUES, 5) &&
	       w[1] == (PACKET3_MAP_QUEUES_QUEUE(r->queue) | PACKET3_MAP_QUEUES_PIPE(r->pipe) |
			PACKET3_MAP_QUEUES_ME(r->me == 1 ? 0u : 1u) | PACKET3_MAP_QUEUES_ENGINE_SEL(eng_sel) |
			PACKET3_MAP_QUEUES_NUM_QUEUES(1)) &&
	       w[2] == PACKET3_MAP_QUEUES_DOORBELL_OFFSET(r->doorbell_index) &&
	       w[3] == lower_32_bits(r->mqd_gpu_addr) && w[4] == upper_32_bits(r->mqd_gpu_addr) &&
	       w[5] == lower_32_bits(r->wptr_gpu_addr) && w[6] == upper_32_bits(r->wptr_gpu_addr);
}
/* ---------------------------------------------------------------------------------------------
 * The cases
 * ------------------------------------------------------------------------------------------- */

static int ring_is_clean(const u32 *mem, u32 nop)
{
	u32 i;
	for (i = 0; i < RING_DW; i++)
		if (mem[i] != nop && !(i < RING_TEST_WORDS))
			return 0;
	return 1;
}

static void case_kcq_success(void)
{
	struct bc250_qreset_result res;
	int r;

	printf("\ncompute: unmap, HQD idle, MQD, map, ring test\n");
	model_reset();
	r = bc250_gfx_reset_kcq(g_kcq, &res);
	check(r == 0 && res.step == BC250_QRESET_STEP_DONE, "the reset passes and says DONE");
	check(unmap_at(g_kiq_mem, 0, g_kcq->doorbell_index), "UNMAP_QUEUES RESET_QUEUES for the queue, 6 dwords");
	check(ring_test_at(g_kiq_mem + 6), "the KIQ ring test follows the unmap");
	check(g_kiq_mem[9] == (u32)PACKET3(PACKET3_MAP_QUEUES, 5), "MAP_QUEUES follows the KIQ ring test");
	check(map_at(g_kiq_mem + 9, g_kcq, 0), "MAP_QUEUES names the queue, its doorbell and its MQD");
	check(ring_test_at(g_kiq_mem + 16), "the KIQ ring test follows the map");
	check(g_kiq_mem[19] == 0xCCCCCCCCu, "nothing else is written to the KIQ");
	check(g_ring_tests == 3, "three ring tests: KIQ, KIQ, the queue");
	check(ring_test_at(g_kcq_mem) && g_kcq->wptr == RING_TEST_WORDS,
	      "the queue restarts at 0: the ring test is at dword 0");
	check(ring_is_clean(g_kcq_mem, g_kcq_funcs.nop), "the rest of the ring is cleared to NOPs");
	check(g_kcq_mqd.cp_hqd_pq_rptr == 0, "the MQD read pointer is 0");
	check(g_hqd_selected_reads >= 4, "the HQD poll ran under the queue select until it read 0");
	check(g_grbm_cntl == 0, "the queue select is back at 0");
	check(res.hqd_active_before == 1, "the witness before reads the HQD active");
	check(res.before.pq_base == 0x05000000u && res.before.ib_rptr == 0x123u,
	      "the witness before reads PQ_BASE and IB_RPTR");
	check(res.after.active == 0, "the witness after the poll reads the HQD inactive");
	check(res.fallback == BC250_QRESET_FALLBACK_NONE && g_dequeue_writes == 0,
	      "no direct dequeue request when the unmap made the HQD inactive");
}

/* The fallback of queue-reset.md 5.3: the HQD stays active after the KIQ unmap. */
static void case_kcq_fallback(int frees)
{
	struct bc250_qreset_result res;
	int r;

	model_reset();
	g_hqd_polls_until_idle = 0xFFFFFFFFu;
	g_fallback_frees = (u32)frees;
	r = bc250_gfx_reset_kcq(g_kcq, &res);
	check(g_dequeue_writes == 2 && g_dequeue_last == 0,
	      "the fallback writes DEQUEUE_REQUEST 1, then puts it back to 0");
	check(res.after.active == 1, "the witness after the first poll reads the HQD still active");
	check(g_grbm_cntl == 0, "the queue select is back at 0 after the fallback");
	if (frees) {
		check(r == 0 && res.step == BC250_QRESET_STEP_DONE && res.fallback == BC250_QRESET_FALLBACK_IDLE,
		      "a fallback that ends the HQD lets the reset go on to DONE");
		check(map_at(g_kiq_mem + 9, g_kcq, 0), "MAP_QUEUES follows the fallback");
	} else {
		check(r != 0 && res.step == BC250_QRESET_STEP_HQD && res.fallback == BC250_QRESET_FALLBACK_ACTIVE,
		      "a fallback that does not end the HQD fails at HQD");
		check(g_kiq_mem[9] == 0xCCCCCCCCu && g_kcq->wptr == 0x40,
		      "no MAP_QUEUES and an untouched ring after a failed fallback");
	}
}

static void case_kcq_fail(unsigned int silent, u32 polls, u32 want_step, const char *what)
{
	struct bc250_qreset_result res;
	int r;

	model_reset();
	g_silent_test = silent;
	g_hqd_polls_until_idle = polls;
	r = bc250_gfx_reset_kcq(g_kcq, &res);
	check(r != 0 && res.step == want_step, what);
	check(g_grbm_cntl == 0, "the queue select is back at 0 after the failure");
	if (want_step == BC250_QRESET_STEP_UNMAP || want_step == BC250_QRESET_STEP_HQD)
		check(g_kiq_mem[9] == 0xCCCCCCCCu, "no MAP_QUEUES after a failed unmap or HQD poll");
	if (want_step == BC250_QRESET_STEP_UNMAP || want_step == BC250_QRESET_STEP_HQD)
		check(g_kcq->wptr == 0x40, "the ring is not touched before the HQD is proven idle");
	if (want_step == BC250_QRESET_STEP_MAP)
		check(g_ring_tests == 2, "no ring test of the queue after a failed map");
}

static void case_kgq_success(void)
{
	struct bc250_qreset_result res;
	struct amdgpu_device *adev = &g_adev;
	u64 active;
	u32 vmid = 13;
	int r;

	printf("\ngfx: CP_VMID_RESET and both waits, MQD, map, ring test\n");
	model_reset();
	r = bc250_gfx_reset_kgq(g_kgq, vmid, &res);
	active = g_kgq->mqd_gpu_addr + offsetof(struct v10_gfx_mqd, cp_gfx_hqd_active);
	check(r == 0 && res.step == BC250_QRESET_STEP_DONE, "the reset passes and says DONE");
	check(BC250_KGQ_RESET_DWORDS == 19u, "the KIQ submission is 19 dwords");
	check(g_kiq_mem[0] == 0xC0033700u && g_kiq_mem[1] == 0x00100000u &&
	      g_kiq_mem[2] == SOC15_REG_OFFSET(GC, 0, mmCP_VMID_RESET) && g_kiq_mem[3] == 0 &&
	      g_kiq_mem[4] == ((1u << vmid) | (1u << CP_VMID_RESET__PIPE0_QUEUES__SHIFT)),
	      "WRITE_DATA CP_VMID_RESET = RESET_REQUEST(1 << vmid) | PIPE0_QUEUES(1 << queue), ME, confirm");
	check(g_kiq_mem[5] == 0xC0053C00u && g_kiq_mem[6] == 0x13u &&
	      g_kiq_mem[7] == lower_32_bits(active) && g_kiq_mem[8] == upper_32_bits(active) &&
	      g_kiq_mem[9] == 0 && g_kiq_mem[10] == 1 && g_kiq_mem[11] == 0x20,
	      "WAIT_REG_MEM on the MQD dword cp_gfx_hqd_active: memory, equal, 0, mask 1");
	check(g_kiq_mem[12] == 0xC0053C00u && g_kiq_mem[13] == 0x3u &&
	      g_kiq_mem[14] == SOC15_REG_OFFSET(GC, 0, mmCP_VMID_RESET) && g_kiq_mem[15] == 0 &&
	      g_kiq_mem[16] == 0 && g_kiq_mem[17] == 0xFFFFFFFFu && g_kiq_mem[18] == 0x20,
	      "WAIT_REG_MEM CP_VMID_RESET == 0, mask 0xFFFFFFFF");
	check(ring_test_at(g_kiq_mem + 19), "the KIQ ring test follows the reset");
	check(g_kiq_mem[22] == (u32)PACKET3(PACKET3_MAP_QUEUES, 5) && map_at(g_kiq_mem + 22, g_kgq, 4),
	      "MAP_QUEUES with engine select 4 follows");
	check(ring_test_at(g_kiq_mem + 29) && g_kiq_mem[32] == 0xCCCCCCCCu, "the KIQ ring test ends it");
	check(ring_test_at(g_kgq_mem) && g_kgq->wptr == RING_TEST_WORDS, "the gfx ring restarts at 0");
	check(g_kgq_mqd.cp_gfx_hqd_rptr == 0, "the MQD read pointer is 0");
	check(g_grbm_cntl == 0, "the queue select is back at 0");

	model_reset();
	g_kgq->pipe = 1;
	g_kgq->queue = 0;
	check(bc250_gfx_vmid_reset_value(g_kgq, 3) ==
	      ((1u << 3) | (1u << CP_VMID_RESET__PIPE1_QUEUES__SHIFT)), "pipe 1 uses PIPE1_QUEUES");
}

static void case_kgq_fail(unsigned int silent, u32 want_step, const char *what)
{
	struct bc250_qreset_result res;
	int r;

	model_reset();
	g_silent_test = silent;
	r = bc250_gfx_reset_kgq(g_kgq, 5, &res);
	check(r != 0 && res.step == want_step, what);
	if (want_step == BC250_QRESET_STEP_VMID_RESET) {
		check(g_kiq_mem[22] == 0xCCCCCCCCu, "no MAP_QUEUES after a failed VMID reset");
		check(g_kgq->wptr == 0x80, "the ring is not touched before the reset is proven");
	}
	check(g_grbm_cntl == 0, "the queue select is back at 0 after the failure");
}

static void case_args(void)
{
	struct bc250_qreset_result res;

	printf("\narguments\n");
	model_reset();
	check(bc250_gfx_reset_kcq(g_kgq, &res) != 0 && res.step == BC250_QRESET_STEP_ARGS,
	      "a compute reset of a gfx ring is refused");
	check(bc250_gfx_reset_kgq(g_kcq, 1, &res) != 0 && res.step == BC250_QRESET_STEP_ARGS,
	      "a gfx reset of a compute ring is refused");
	check(bc250_gfx_reset_kgq(g_kgq, 0, &res) != 0 && res.step == BC250_QRESET_STEP_ARGS,
	      "VMID 0 is refused");
	check(bc250_gfx_reset_kgq(g_kgq, 16, &res) != 0 && res.step == BC250_QRESET_STEP_ARGS,
	      "VMID 16 is refused");
	check(g_kiq_mem[0] == 0xCCCCCCCCu, "a refused call writes nothing to the KIQ");
}

int main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "-v") == 0)
		g_verbose = 1;

	case_kcq_success();
	printf("\ncompute: each step fails in turn\n");
	case_kcq_fail(1, 3, BC250_QRESET_STEP_UNMAP, "a silent KIQ after the unmap fails at UNMAP");
	case_kcq_fail(0, 0xFFFFFFFFu, BC250_QRESET_STEP_HQD, "an HQD that stays active fails at HQD");
	case_kcq_fail(2, 3, BC250_QRESET_STEP_MAP, "a silent KIQ after the map fails at MAP");
	printf("\ncompute: the fallback of 5.3\n");
	case_kcq_fallback(0);
	case_kcq_fallback(1);
	case_kcq_fail(3, 3, BC250_QRESET_STEP_RING_TEST, "a silent queue fails at RING_TEST");
	case_kgq_success();
	printf("\ngfx: each step fails in turn\n");
	case_kgq_fail(1, BC250_QRESET_STEP_VMID_RESET, "a silent KIQ after the VMID reset fails at VMID_RESET");
	case_kgq_fail(2, BC250_QRESET_STEP_MAP, "a silent KIQ after the map fails at MAP");
	case_kgq_fail(3, BC250_QRESET_STEP_RING_TEST, "a silent queue fails at RING_TEST");
	case_args();

	printf("\n%u checks, %u failures\n", g_checks, g_failures);
	return g_failures ? 1 : 0;
}
