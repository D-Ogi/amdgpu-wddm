/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host replay of amdgpu's interrupt-ring bring-up on unit A, and a host proof that a fence
 * submitted on a CP ring comes back as a decodable interrupt vector (milestone M6, ADR 0002).
 *
 * Runs driver/shim/bc250_ih.c and the two NBIO functions it calls against a backend that answers
 * reads with what unit A's hardware returned during experiment E03, compares every register write
 * with what amdgpu wrote in the same window, and then exercises the three DPC functions -
 * bc250_ih_get_wptr(), bc250_ih_decode(), bc250_ih_set_rptr() - against vectors the test builds and
 * against vectors the CP stub delivers.
 *
 *   replay_ih <sweep-run1.log> <sweep-run2.log> <trace-ih.txt> [-v]
 *
 * The trace extract is
 *   python tools/trace/extract_phase.py <evidence>/amdgpu-events.txt \
 *          --match '^(OSSSYS|NBIO)\.' --reads --no-fold --precision 6 --since 0.2520 --until 0.2540
 *
 * Twenty accesses, fifteen of them writes, and they are the whole of navi10_ih_irq_init() on this
 * part. The window is early - 0.2528 s, before the GFX window this milestone's sibling replays -
 * because the IH block comes up with the rest of the common IP, long before the CP.
 *
 * Four things about the numbers below.
 *
 * 1. Reads. As in replay_gfx.c, the FIRST read of each register in the window is answered from the
 *    trace's own read record and everything after that from what the run itself wrote. A read with
 *    neither is counted and reported, and must be 0.
 *
 * 2. A bit that does not stay written. The trace puts 0xC03101A0 into IH_RB_CNTL and reads back
 *    0x403101A0 seven microseconds later: bit 31, WPTR_OVERFLOW_CLEAR, is a pulse the hardware
 *    retires, not a setting. The final read-modify-write of the sequence depends on that, so the
 *    backend is told about the bit through backend_add_selfclear() - the write is still compared
 *    exactly as the driver issued it, and only the value a later read returns is corrected. The
 *    declaration is made from those two trace lines and nothing else, and one of the controls below
 *    removes it and has to fail.
 *
 * 3. Addresses. The ring and the write-back page are allocated by this test, so the four registers
 *    carrying their addresses cannot equal the trace. For those the offset is compared, the value is
 *    not, and each is separately checked against the address this test handed out. INTERRUPT_CNTL2
 *    is NOT one of them: it carries the dummy page, which is an input this test is given rather than
 *    an address it chooses, so it is compared in full.
 *
 * 4. The interrupt itself. No hardware here raises one. Vectors arrive in two ways, both of them
 *    declared: the decode tests write entries into the ring directly, and the end-to-end test lets
 *    the CP stub in backend_mem.c deliver one in response to a RELEASE_MEM the shim emitted. The
 *    stub's encoder and bc250_ih_decode() were written from the same upstream lines in opposite
 *    directions, so a field either of them has wrong shows up rather than cancelling out.
 *
 * Nothing in this file decides what the hardware is told; that is bc250_ih.c's and bc250_nbio.c's
 * job, and bc250_gfx.c's for the fence.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_mem.h"
#include "backend_trace.h"
#include "bc250_dispatch.h"
#include "bc250_gfx.h"
#include "bc250_gmc.h"
#include "bc250_ih.h"
#include "bc250_nbio.h"
#include "bc250_sdma.h"

#include <oss/osssys_5_0_0_offset.h>
#include <oss/osssys_5_0_0_sh_mask.h>
#include "soc15_common.h"
#include "soc15_ih_clientid.h"
#include "irqsrcs_gfx_10_1.h"
#include "irqsrcs_sdma0_5_0.h"
#include "irqsrcs_sdma1_5_0.h"

/* ---------------------------------------------------------------------------------------------
 * Unit A, experiment E03 (evidence/linux/2026-09-21-E03-init-trace), kernel 6.18.52-0-lts.
 * The same three inputs replay.c and replay_gfx.c use; see replay.c for where each comes from.
 * ------------------------------------------------------------------------------------------- */
#define UNITA_GART_TABLE_MC                     0x000000F5FFE00000ULL
#define UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB   0x002708C9u
#define UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32    0x0007E3C1u
#define UNITA_DOORBELL_BASE                     0x00000000D0000000ULL

/* evidence/linux/2026-09-21-E03-init-trace/dmesg.txt:1038. Only the fence part of this test needs
 * them, and only to get rings allocated; nothing here reads a shader-engine register. */
#define UNITA_MAX_SHADER_ENGINES   2u
#define UNITA_MAX_SH_PER_SE        2u
#define UNITA_MAX_CU_PER_SH        10u
#define UNITA_MAX_BACKENDS_PER_SE  2u

/* Unit A ran with MSI, and the trace says so rather than the kernel log: IH_RB_CNTL comes out of
 * navi10_ih_enable_ring() as 0xC03101A0, and bit 21 of that is RPTR_REARM, which is
 * `!!adev->irq.msi_enabled` (navi10_ih.c:277). With MSI off the same code produces 0xC01101A0.
 * One of the controls below runs it that way and has to fail. */
#define UNITA_MSI  true

/* ---------------------------------------------------------------------------------------------
 * The reference trace
 * ------------------------------------------------------------------------------------------- */

#define MAX_TRACE 128

struct trace_entry {
	u32 byte_offset;
	u32 value;
	char name[64];
};

static struct trace_entry g_trace[MAX_TRACE];
static unsigned int g_trace_count;

static int load_trace_writes(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		double t;
		char kind[8], name[256];
		unsigned int off = 0, val = 0;

		if (sscanf(line, "%lf %7s %255s 0x%x %x", &t, kind, name, &off, &val) != 5)
			continue;
		if (kind[0] != 'W')
			continue;
		if (g_trace_count >= MAX_TRACE)
			break;
		g_trace[g_trace_count].byte_offset = off;
		g_trace[g_trace_count].value = val;
		strncpy(g_trace[g_trace_count].name, name, sizeof(g_trace[0].name) - 1);
		g_trace[g_trace_count].name[sizeof(g_trace[0].name) - 1] = '\0';
		g_trace_count++;
	}
	fclose(f);
	return (int)g_trace_count;
}

/* The registers whose value carries an address this test chose. See point 3 of the header. */
static const char *const g_address_registers[] = {
	"OSSSYS.IH_RB_BASE",		"OSSSYS.IH_RB_BASE_HI",
	"OSSSYS.IH_RB_WPTR_ADDR_LO",	"OSSSYS.IH_RB_WPTR_ADDR_HI"
};

static int is_address_register(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(g_address_registers); i++)
		if (strcmp(name, g_address_registers[i]) == 0)
			return 1;
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The comparison
 * ------------------------------------------------------------------------------------------- */

struct result {
	unsigned int produced;
	unsigned int compared;
	unsigned int mismatches;
	unsigned int address_exceptions;
	unsigned int address_failures;
	unsigned int unknown_reads;
	int          hw_init_rc;
};

static void compare_window(struct result *out, int quiet)
{
	const struct bc250_reg_write *w = backend_writes();
	unsigned int i, n;

	out->produced = backend_write_count();
	n = out->produced < g_trace_count ? out->produced : g_trace_count;
	out->compared = n;

	for (i = 0; i < n; i++) {
		int addr = is_address_register(g_trace[i].name);

		if (w[i].byte_offset != g_trace[i].byte_offset) {
			out->mismatches++;
		} else if (addr) {
			out->address_exceptions++;
			continue;
		} else if (w[i].value == g_trace[i].value) {
			continue;
		} else {
			out->mismatches++;
		}

		if (!quiet)
			printf("  [%2u] shim 0x%05X = %08X   trace %-28s 0x%05X = %08X   bits %08X\n",
			       i, w[i].byte_offset, w[i].value,
			       g_trace[i].name, g_trace[i].byte_offset, g_trace[i].value,
			       w[i].byte_offset == g_trace[i].byte_offset
				       ? (w[i].value ^ g_trace[i].value) : 0u);
	}

	if (out->produced != g_trace_count && !quiet)
		printf("  produced %u writes, the window has %u\n", out->produced, g_trace_count);
}

/* The last value the run wrote to one byte offset, or 0 with *found cleared. */
static u32 shim_wrote(u32 byte_offset, int *found)
{
	const struct bc250_reg_write *w = backend_writes();
	unsigned int n = backend_write_count();
	unsigned int i;
	u32 value = 0;

	*found = 0;
	for (i = 0; i < n; i++) {
		if (w[i].byte_offset == byte_offset) {
			value = w[i].value;
			*found = 1;
		}
	}
	return value;
}

/* Each address register checked against the allocation it is supposed to describe, so that "not
 * compared with the trace" does not mean "not checked". Returns the number that failed. */
static unsigned int check_addresses(struct amdgpu_device *adev, int quiet)
{
	const struct amdgpu_ih_ring *ih = &adev->irq.ih;
	unsigned int bad = 0;
	int found;
	u32 got, want;
	unsigned int i;

	struct {
		const char *name;
		u32 offset;
		u32 want;
	} expect[4];

	expect[0].name = "IH_RB_BASE";
	expect[0].offset = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_BASE) * 4u;
	expect[0].want = (u32)(ih->gpu_addr >> 8);
	expect[1].name = "IH_RB_BASE_HI";
	expect[1].offset = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_BASE_HI) * 4u;
	expect[1].want = (u32)((ih->gpu_addr >> 40) & 0xffu);
	expect[2].name = "IH_RB_WPTR_ADDR_LO";
	expect[2].offset = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_WPTR_ADDR_LO) * 4u;
	expect[2].want = lower_32_bits(ih->wptr_addr);
	expect[3].name = "IH_RB_WPTR_ADDR_HI";
	expect[3].offset = SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_WPTR_ADDR_HI) * 4u;
	expect[3].want = upper_32_bits(ih->wptr_addr) & 0xFFFFu;

	for (i = 0; i < ARRAY_SIZE(expect); i++) {
		got = shim_wrote(expect[i].offset, &found);
		want = expect[i].want;
		if (!found || got != want) {
			bad++;
			if (!quiet)
				printf("  %s = %08X, but the allocation says %08X%s\n",
				       expect[i].name, got, want, found ? "" : " (never written)");
		}
	}
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * One run of the bring-up
 * ------------------------------------------------------------------------------------------- */

struct run_opts {
	bool msi;
	int  selfclear;         /* 0 removes the declared self-clearing bit; see the controls */
};

/* Everything up to, but not including, the first register write of navi10_ih_irq_init(). Returns 0,
 * or -1 if the memory layout could not be learnt. */
static int prepare(struct amdgpu_device *adev, const struct run_opts *opt)
{
	struct amdgpu_bo gart_bo;
	struct bc250_gmc_inputs gin;

	memset(adev, 0, sizeof(*adev));
	memset(&gart_bo, 0, sizeof(gart_bo));
	memset(&gin, 0, sizeof(gin));
	adev->dev = (void *)"BC250-A";
	adev->doorbell.base = UNITA_DOORBELL_BASE;

	backend_reset_state();
	backend_reset_writes();
	backend_mem_reset();
	backend_clear_aliases();

	gin.gart_table_mc = UNITA_GART_TABLE_MC;
	gin.dummy_page_dma = (u64)UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32 << 12;
	gin.noretry = true;
	if (bc250_gmc_setup(adev, &gin, &gart_bo) != 0)
		return -1;
	gin.mem_scratch_mc = ((u64)UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB << 12)
			     - adev->vm_manager.vram_base_offset + adev->gmc.vram_start;
	(void)bc250_gmc_setup(adev, &gin, &gart_bo);

	backend_mem_set_bases(adev->gmc.vram_start, adev->gmc.gart_start);

	/* The one declared self-clearing bit, named through AMD's headers rather than by its offset
	 * and its mask. See point 2 of the header comment for the two trace lines behind it. */
	if (opt->selfclear)
		backend_add_selfclear(SOC15_REG_OFFSET(OSSSYS, 0, mmIH_RB_CNTL) * 4u,
				      IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK);

	if (bc250_ih_setup(adev, opt->msi) != 0)
		return -1;

	/* The setup only allocates. The record starts at the first write of the bring-up. */
	backend_reset_writes();
	return 0;
}

static void run_one(const char *title, const struct run_opts *opt, struct amdgpu_device *adev,
		    struct result *res, int quiet)
{
	memset(res, 0, sizeof(*res));

	if (!quiet)
		printf("\n== %s ==\n", title);

	if (prepare(adev, opt) != 0) {
		if (!quiet)
			printf("  could not learn the memory layout; nothing to compare\n");
		res->mismatches = 1;
		res->hw_init_rc = -1;
		return;
	}

	res->hw_init_rc = bc250_ih_hw_init(adev);

	compare_window(res, quiet);
	res->address_failures = check_addresses(adev, quiet);
	res->unknown_reads = backend_unknown_reads();

	if (!quiet) {
		printf("  bc250_ih_hw_init                    : %d\n", res->hw_init_rc);
		printf("  writes produced / window            : %u / %u\n",
		       res->produced, g_trace_count);
		printf("  mismatches                          : %u\n", res->mismatches);
		printf("  address exceptions (offset only)    : %u\n", res->address_exceptions);
		printf("  address registers wrong             : %u\n", res->address_failures);
		printf("  reads with no recorded value        : %u\n", res->unknown_reads);
	}
}

/* ---------------------------------------------------------------------------------------------
 * The decode, against vectors this test builds
 *
 * The encoder here is separate from the CP stub's on purpose: this one sets every field to a
 * different value, including the ones no real vector on this part uses, so that a decode that
 * shifted a field by a few bits cannot pass by accident.
 * ------------------------------------------------------------------------------------------- */

struct iv_fields {
	u32 client_id, src_id, ring_id, vmid, vmid_src;
	u64 timestamp;          /* 48 bits */
	u32 timestamp_src, pasid, node_id;
	u32 src_data[4];
};

/* amdgpu_ih.c:263 amdgpu_ih_decode_iv_helper(), written backwards. */
static void put_iv(struct amdgpu_ih_ring *ih, u32 at, const struct iv_fields *f)
{
	volatile u32 *p = (volatile u32 *)ih->ring + ((at & ih->ptr_mask) >> 2);
	u32 i;

	p[0] = (f->client_id & 0xffu) | ((f->src_id & 0xffu) << 8) |
	       ((f->ring_id & 0xffu) << 16) | ((f->vmid & 0xfu) << 24) |
	       ((f->vmid_src & 0x1u) << 31);
	p[1] = (u32)(f->timestamp & 0xffffffffu);
	p[2] = (u32)((f->timestamp >> 32) & 0xffffu) | ((f->timestamp_src & 0x1u) << 31);
	p[3] = (f->pasid & 0xffffu) | ((f->node_id & 0xffu) << 16);
	for (i = 0; i < 4; i++)
		p[4 + i] = f->src_data[i];
}

static unsigned int check_field(const char *what, u64 got, u64 want)
{
	if (got == want)
		return 0;
	printf("  %-22s decoded %llu, put in %llu\n", what,
	       (unsigned long long)got, (unsigned long long)want);
	return 1;
}

/* Returns the number of failures. */
static unsigned int test_decode(struct amdgpu_device *adev)
{
	struct amdgpu_ih_ring *ih = &adev->irq.ih;
	struct bc250_iv_entry e;
	struct iv_fields f;
	unsigned int bad = 0;
	u32 rptr, me, pipe, queue, instance;

	printf("\n== the decode ==\n");

	/* 1. Every field distinct, so nothing can pass by lining up with a neighbour. */
	memset(&f, 0, sizeof(f));
	f.client_id = 0xA5;
	f.src_id = 0x5A;
	f.ring_id = 0x3C;
	f.vmid = 0xD;
	f.vmid_src = 1;
	f.timestamp = 0x0000123456789ABCULL;
	f.timestamp_src = 1;
	f.pasid = 0xBEEF;
	f.node_id = 0x77;
	f.src_data[0] = 0x11111111;
	f.src_data[1] = 0x22222222;
	f.src_data[2] = 0x33333333;
	f.src_data[3] = 0x44444444;
	put_iv(ih, 0, &f);

	rptr = 0;
	memset(&e, 0xCC, sizeof(e));
	if (bc250_ih_decode(adev, &rptr, &e) != 0) {
		printf("  bc250_ih_decode refused a well-formed entry\n");
		return 1;
	}
	bad += check_field("client_id", e.client_id, f.client_id);
	bad += check_field("src_id", e.src_id, f.src_id);
	bad += check_field("ring_id", e.ring_id, f.ring_id);
	bad += check_field("vmid", e.vmid, f.vmid);
	bad += check_field("vmid_src", e.vmid_src, f.vmid_src);
	bad += check_field("timestamp", e.timestamp, f.timestamp);
	bad += check_field("timestamp_src", e.timestamp_src, f.timestamp_src);
	bad += check_field("pasid", e.pasid, f.pasid);
	bad += check_field("node_id", e.node_id, f.node_id);
	bad += check_field("src_data[0]", e.src_data[0], f.src_data[0]);
	bad += check_field("src_data[3]", e.src_data[3], f.src_data[3]);
	bad += check_field("rptr after one entry", rptr, 32);
	printf("  all thirteen fields of one vector round-trip: %s\n", bad ? "NO" : "yes");

	/* 2. Routing. A gfx and a compute end-of-pipe carry the same client and source id and differ
	 * only in ring_id, which is the reason bc250_ih_is_*_eop() are functions. */
	memset(&f, 0, sizeof(f));
	f.client_id = SOC15_IH_CLIENTID_GRBM_CP;
	f.src_id = GFX_10_1__SRCID__CP_EOP_INTERRUPT;
	f.ring_id = 0;                                  /* me 0, pipe 0, queue 0 */
	put_iv(ih, 32, &f);
	f.ring_id = (1u << 2) | 2u | (1u << 4);         /* me 1, pipe 2, queue 1 */
	put_iv(ih, 64, &f);
	f.src_id = GFX_10_1__SRCID__CP_IB2_INTERRUPT_PKT;
	f.ring_id = 0;
	put_iv(ih, 96, &f);
	memset(&f, 0, sizeof(f));
	f.client_id = SOC15_IH_CLIENTID_SDMA1;
	f.src_id = SDMA1_5_0__SRCID__SDMA_TRAP;
	put_iv(ih, 128, &f);

	rptr = 32;
	(void)bc250_ih_decode(adev, &rptr, &e);
	if (!bc250_ih_is_gfx_eop(&e) || bc250_ih_is_compute_eop(&e) || bc250_ih_is_kiq(&e)) {
		printf("  a me-0 end-of-pipe is not routed as the gfx ring's\n");
		bad++;
	}
	(void)bc250_ih_decode(adev, &rptr, &e);
	bc250_ih_eop_ring_id(&e, &me, &pipe, &queue);
	if (!bc250_ih_is_compute_eop(&e) || bc250_ih_is_gfx_eop(&e) ||
	    me != 1 || pipe != 2 || queue != 1) {
		printf("  a me-1 pipe-2 queue-1 end-of-pipe decodes as me %u pipe %u queue %u\n",
		       me, pipe, queue);
		bad++;
	}
	(void)bc250_ih_decode(adev, &rptr, &e);
	if (!bc250_ih_is_kiq(&e) || bc250_ih_is_gfx_eop(&e)) {
		printf("  the KIQ's vector is not routed to the KIQ\n");
		bad++;
	}
	instance = 99;
	(void)bc250_ih_decode(adev, &rptr, &e);
	if (!bc250_ih_is_sdma_trap(&e, &instance) || instance != 1) {
		printf("  an SDMA1 trap decodes as instance %u\n", instance);
		bad++;
	}
	printf("  gfx / compute / KIQ / SDMA1 routed apart : %s\n", bad ? "NO" : "yes");

	/* 3. Wrap-around. The last entry of the ring, decoded, must leave the read pointer at 0 and
	 * not at ring_size - a mask applied to the wrong side would only show up here. */
	memset(&f, 0, sizeof(f));
	f.client_id = SOC15_IH_CLIENTID_SDMA0;
	f.src_id = SDMA0_5_0__SRCID__SDMA_TRAP;
	f.src_data[0] = 0xFEEDFACE;
	put_iv(ih, ih->ring_size - 32u, &f);

	rptr = ih->ring_size - 32u;
	memset(&e, 0, sizeof(e));
	(void)bc250_ih_decode(adev, &rptr, &e);
	instance = 99;
	if (rptr != 0 || e.src_data[0] != 0xFEEDFACE ||
	    !bc250_ih_is_sdma_trap(&e, &instance) || instance != 0) {
		printf("  the last entry of the ring decodes wrong: rptr %u, src_data0 %08X\n",
		       rptr, e.src_data[0]);
		bad++;
	}
	printf("  the last entry wraps to 0               : %s\n",
	       rptr == 0 ? "yes" : "NO");

	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * get_wptr and set_rptr, including the overflow arm
 * ------------------------------------------------------------------------------------------- */

/* The whole list of registers the DPC trio is allowed to touch, by the name the trace gives them.
 * The header states this list; this is the survey that checks it, the same way the GFX test surveys
 * what teardown touches. The miniport gives its DPC a sequence with exactly these on it. */
static const char *const g_dpc_registers[] = {
	"OSSSYS.IH_RB_CNTL",	/* read and written, overflow path only */
	"OSSSYS.IH_RB_WPTR",	/* read, overflow path only */
	"OSSSYS.IH_RB_RPTR"	/* written, only when the doorbell is not used */
};

static unsigned int survey_dpc_registers(struct amdgpu_device *adev, int quiet)
{
	unsigned int i, j, bad = 0;

	if (backend_touched_overflow()) {
		printf("  the touched-register table overflowed\n");
		return 1;
	}
	if (!quiet)
		printf("  registers the trio touched                : %u\n", backend_touched_count());

	for (i = 0; i < backend_touched_count(); i++) {
		u32 off = backend_touched_offset(i);
		const char *name = NULL;

		for (j = 0; j < ARRAY_SIZE(g_dpc_registers); j++) {
			u32 want = 0;

			if (j == 0) want = adev->irq.ih.ih_regs.ih_rb_cntl * 4u;
			if (j == 1) want = adev->irq.ih.ih_regs.ih_rb_wptr * 4u;
			if (j == 2) want = adev->irq.ih.ih_regs.ih_rb_rptr * 4u;
			if (off == want) {
				name = g_dpc_registers[j];
				break;
			}
		}
		if (name == NULL) {
			printf("  the DPC trio touched 0x%05X, which is not on its list\n", off);
			bad++;
		} else if (!quiet) {
			printf("    0x%05X %-20s %s\n", off, name,
			       backend_touched_written(i) ? "read and written" : "read only");
		}
	}
	return bad;
}

static unsigned int test_wptr_rptr(struct amdgpu_device *adev, int quiet)
{
	struct amdgpu_ih_ring *ih = &adev->irq.ih;
	unsigned int bad = 0, doorbells_before;
	u32 wptr_off = ih->ih_regs.ih_rb_wptr * 4u;
	u32 cntl_off = ih->ih_regs.ih_rb_cntl * 4u;
	u32 wptr;
	bool overflowed = true;

	if (!quiet)
		printf("\n== the write and read pointers ==\n");

	backend_touched_start();

	/* The fast path: the write-back slot, no register read, no write. */
	backend_reset_writes();
	*ih->wptr_cpu = 3u * 32u;
	wptr = bc250_ih_get_wptr(adev, &overflowed);
	if (wptr != 96u || overflowed || backend_write_count() != 0) {
		printf("  the plain path returned %u, overflowed %d, and made %u register writes\n",
		       wptr, (int)overflowed, backend_write_count());
		bad++;
	} else if (!quiet) {
		printf("  three vectors pending, read from memory, no register touched: yes\n");
	}

	/* The overflow arm. The write-back says overflow; the register has to agree, which is
	 * upstream's "double check that the overflow wasn't already cleared". */
	backend_reset_writes();
	*ih->wptr_cpu = (17u * 32u) | IH_RB_WPTR__RB_OVERFLOW_MASK;
	backend_poke(wptr_off, (17u * 32u) | IH_RB_WPTR__RB_OVERFLOW_MASK);
	overflowed = false;
	wptr = bc250_ih_get_wptr(adev, &overflowed);

	if (!overflowed) {
		printf("  an overflow was not reported\n");
		bad++;
	}
	if (wptr != 17u * 32u) {
		printf("  the overflow path returned %u, not the write pointer with bit 0 cleared\n",
		       wptr);
		bad++;
	}
	/* Recovery: parsing restarts one whole entry past the write pointer, which is the oldest
	 * entry the hardware has not yet trampled. */
	if (ih->rptr != (17u * 32u + 32u)) {
		printf("  the read pointer was moved to %u, not to wptr + 32\n", ih->rptr);
		bad++;
	}
	/* The acknowledge is a pulse: set, then clear, and both writes are needed. */
	{
		const struct bc250_reg_write *w = backend_writes();
		unsigned int n = backend_write_count();

		if (n != 2 || w[0].byte_offset != cntl_off || w[1].byte_offset != cntl_off ||
		    (w[0].value & IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK) == 0 ||
		    (w[1].value & IH_RB_CNTL__WPTR_OVERFLOW_CLEAR_MASK) != 0) {
			printf("  the overflow acknowledge was not a set-then-clear pulse of"
			       " IH_RB_CNTL (%u writes)\n", n);
			bad++;
		} else if (!quiet) {
			printf("  an overflow is acknowledged with a two-write pulse   : yes\n");
		}
	}

	/* The overflow contract the header states: the caller's read pointer is stale and has to be
	 * taken from adev->irq.ih.rptr, or the loop walks entries the hardware already overwrote. */
	{
		u32 stale = 2u * 32u;
		u32 resynced = overflowed ? ih->rptr : stale;

		if (resynced != 17u * 32u + 32u) {
			printf("  the documented overflow resync does not reach the new read"
			       " pointer (%u)\n", resynced);
			bad++;
		} else if (!quiet) {
			printf("  on overflow the caller picks rptr up from adev    : yes\n");
		}
	}

	/* set_rptr publishes the slot and then rings the doorbell, and does not touch IH_RB_RPTR
	 * while the doorbell is in use. The doorbell must be 32 bits wide: see bc250_shim.h. */
	backend_reset_writes();
	doorbells_before = backend_doorbell_count();
	bc250_ih_set_rptr(adev, 5u * 32u);
	if (*ih->rptr_cpu != 5u * 32u ||
	    backend_doorbell_count() != doorbells_before + 1u ||
	    backend_write_count() != 0) {
		printf("  set_rptr wrote slot %u, %u doorbells, %u register writes\n",
		       (unsigned int)*ih->rptr_cpu, backend_doorbell_count() - doorbells_before,
		       backend_write_count());
		bad++;
	} else if (!quiet) {
		printf("  set_rptr: write-back slot then doorbell, no MMIO   : yes\n");
	}
	{
		const struct backend_doorbell *d = &backend_doorbells()[doorbells_before];

		if (d->width != 32u || d->index != ih->doorbell_index || d->value != 5u * 32u) {
			printf("  the read pointer's doorbell was %u bits at index 0x%X = %llu;"
			       " upstream's WDOORBELL32 is 32 bits at 0x%X\n",
			       d->width, d->index, (unsigned long long)d->value,
			       ih->doorbell_index);
			bad++;
		} else if (!quiet) {
			printf("  it is a 32-bit doorbell at 0x%X, not 64            : yes\n",
			       d->index);
		}
	}

	backend_touched_stop();
	bad += survey_dpc_registers(adev, quiet);

	/* The third register on the list only appears without a doorbell, so the survey above can
	 * never see it and the list would otherwise be two measured entries and one asserted one.
	 * This is the run that measures it. */
	{
		bool saved = ih->use_doorbell;

		backend_reset_writes();
		doorbells_before = backend_doorbell_count();
		ih->use_doorbell = false;
		backend_touched_start();
		bc250_ih_set_rptr(adev, 6u * 32u);
		backend_touched_stop();
		ih->use_doorbell = saved;

		if (backend_touched_count() != 1u ||
		    backend_touched_offset(0) != ih->ih_regs.ih_rb_rptr * 4u ||
		    backend_doorbell_count() != doorbells_before ||
		    backend_write_count() != 1u) {
			printf("  without a doorbell set_rptr touched %u registers and rang %u"
			       " doorbells; it should write IH_RB_RPTR and nothing else\n",
			       backend_touched_count(),
			       backend_doorbell_count() - doorbells_before);
			bad++;
		} else if (!quiet) {
			printf("    0x%05X OSSSYS.IH_RB_RPTR     written, doorbell off only\n",
			       backend_touched_offset(0));
		}
		/* Put the read pointer back where the doorbell path left it. */
		bc250_ih_set_rptr(adev, 5u * 32u);
	}

	/*
	 * The miniport's DPC does not get the escape's adev: that one carries a backend swapped under
	 * a fast mutex at APC_LEVEL. It gets a device of its own, zeroed except for irq.ih copied by
	 * value. Run the trio through exactly that and it must behave identically.
	 *
	 * This is the test of the header's contract, not a restatement of it: a device with no
	 * reg_offset table, no gmc, no gfx and no doorbell_index would break anything that reached
	 * for them. It works because the register offsets ride along inside ih_regs.
	 */
	{
		static struct amdgpu_device dpc;
		bool of = true;
		u32 w, r2;
		struct bc250_iv_entry e;

		memset(&dpc, 0, sizeof(dpc));
		dpc.irq.ih = adev->irq.ih;      /* by value, and nothing else */

		*ih->wptr_cpu = 4u * 32u;
		backend_reset_writes();
		doorbells_before = backend_doorbell_count();

		w = bc250_ih_get_wptr(&dpc, &of);
		r2 = 0;
		while (r2 != w) {
			if (bc250_ih_decode(&dpc, &r2, &e) != 0) {
				printf("  the DPC-only device could not decode\n");
				bad++;
				break;
			}
		}
		bc250_ih_set_rptr(&dpc, r2);

		if (w != 128u || of || r2 != 128u || backend_write_count() != 0 ||
		    backend_doorbell_count() != doorbells_before + 1u) {
			printf("  a zeroed device with only irq.ih copied behaves differently:"
			       " wptr %u, overflowed %d, rptr %u, %u writes\n",
			       w, (int)of, r2, backend_write_count());
			bad++;
		} else if (!quiet) {
			printf("  a zeroed adev with only irq.ih copied works       : yes\n");
		}

		/* And the rule that goes with the copy: it must never be torn down. Nothing to run
		 * here - the check is that adev still owns the memory the copy points at. */
		if (dpc.irq.ih.ring != adev->irq.ih.ring ||
		    dpc.irq.ih.ring_mem.cpu != adev->irq.ih.ring_mem.cpu) {
			printf("  the copy does not point at the same ring\n");
			bad++;
		}
	}

	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * End to end: a fence emitted on a ring, an interrupt decoded off the ring
 *
 * This is the rehearsal of the first hardware run. The shim emits the packet, the CP stub executes
 * it and delivers a vector, and the shim's own decode reads it back. The stub is the one thing here
 * that is not the driver, and it is declared in backend_mem.c.
 * ------------------------------------------------------------------------------------------- */

#define FENCE_SLOT_GFX      0u
#define FENCE_SLOT_COMPUTE  1u
#define FENCE_SLOT_KIQ      2u
#define FENCE_SEQ           0x00000000DEADC0DEULL

static unsigned int fence_one(struct amdgpu_device *adev, struct amdgpu_ring *ring,
			      const char *what, unsigned int slot, unsigned int flags,
			      u32 *rptr, unsigned int expect_vectors)
{
	struct bc250_iv_entry e;
	unsigned int before = backend_ih_delivered();
	unsigned int delivered;
	u64 addr = bc250_gfx_fence_addr(adev, slot);
	u64 got;
	unsigned int bad = 0;
	int r;

	r = bc250_gfx_signal_fence(ring, addr, FENCE_SEQ, flags);
	if (r != 0) {
		printf("  %s: bc250_gfx_signal_fence returned %d\n", what, r);
		return 1;
	}

	got = bc250_gfx_fence_read(adev, slot);
	if ((flags & AMDGPU_FENCE_FLAG_64BIT) ? (got != FENCE_SEQ)
					      : ((u32)got != (u32)FENCE_SEQ)) {
		printf("  %s: the fence slot holds %016llX, not %016llX\n",
		       what, (unsigned long long)got, (unsigned long long)FENCE_SEQ);
		bad++;
	}

	delivered = backend_ih_delivered() - before;
	if (delivered != expect_vectors) {
		printf("  %s: %u vectors delivered, expected %u\n", what, delivered, expect_vectors);
		return bad + 1;
	}
	if (expect_vectors == 0)
		return bad;

	if (bc250_ih_decode(adev, rptr, &e) != 0) {
		printf("  %s: the delivered vector would not decode\n", what);
		return bad + 1;
	}

	if (ring->funcs->type == AMDGPU_RING_TYPE_KIQ) {
		u32 me = 0, pipe = 0, queue = 0;

		/* gfx_v10_0_kiq_irq() (gfx_v10_0.c:9456-9471) decodes me, pipe and queue out of the
		 * KIQ vector for a debug line and then processes the ring whatever they are, and
		 * bc250_ih_is_kiq() matches the client and the source id the same way. The fields are
		 * still checked here, because unit A does fill them: E13 amdgpu-events-ib.txt:23 is
		 * client_id 20 src_id 178 ring 9, and 9 is this ring's me 2, pipe 1, queue 0. */
		bc250_ih_eop_ring_id(&e, &me, &pipe, &queue);
		if (!bc250_ih_is_kiq(&e) || me != ring->me || pipe != ring->pipe ||
		    queue != ring->queue)
			bad++;
	} else if (ring->funcs->type == AMDGPU_RING_TYPE_GFX) {
		if (!bc250_ih_is_gfx_eop(&e))
			bad++;
	} else {
		u32 me = 0, pipe = 0, queue = 0;

		bc250_ih_eop_ring_id(&e, &me, &pipe, &queue);
		if (!bc250_ih_is_compute_eop(&e) || me != ring->me ||
		    pipe != ring->pipe || queue != ring->queue)
			bad++;
	}

	if (bad)
		printf("  %s: the vector decoded as client %02X src %u ring_id %02X, which is not"
		       " this ring's end-of-pipe\n", what, e.client_id, e.src_id, e.ring_id);
	return bad;
}

static unsigned int test_fence(struct amdgpu_device *adev, int quiet)
{
	struct bc250_gfx_inputs fin;
	struct amdgpu_ring *compute;
	unsigned int bad = 0, rejects_before;
	u32 rptr = 0;
	u32 i;
	int r;

	printf("\n== a fence, and the interrupt it raises ==\n");

	memset(&fin, 0, sizeof(fin));
	fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
	fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
	fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
	fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
	fin.async_gfx_ring = true;
	fin.pp_gfxoff = true;

	r = bc250_gfx_setup(adev, &fin);
	if (r != 0) {
		printf("  bc250_gfx_setup returned %d; no rings to fence on\n", r);
		return 1;
	}
	backend_ring_register(&adev->gfx.gfx_ring[0]);
	for (i = 0; i < adev->gfx.num_compute_rings; i++)
		backend_ring_register(&adev->gfx.compute_ring[i]);
	backend_ring_register(&adev->gfx.kiq[0].ring);

	r = bc250_gfx_fence_page_alloc(adev);
	if (r != 0) {
		printf("  bc250_gfx_fence_page_alloc returned %d\n", r);
		return 1;
	}

	/* No register write happens from here on; the rest is memory and one doorbell each. */
	backend_reset_writes();
	backend_ih_attach(adev);
	rejects_before = backend_cp_stub_rejects();
	compute = &adev->gfx.compute_ring[3];   /* me 1, pipe 3, queue 0 */

	bad += fence_one(adev, &adev->gfx.gfx_ring[0], "gfx ring", FENCE_SLOT_GFX,
			 AMDGPU_FENCE_FLAG_64BIT | AMDGPU_FENCE_FLAG_INT, &rptr, 1);
	bad += fence_one(adev, compute, "compute ring 3", FENCE_SLOT_COMPUTE,
			 AMDGPU_FENCE_FLAG_64BIT | AMDGPU_FENCE_FLAG_INT, &rptr, 1);
	bad += fence_one(adev, &adev->gfx.kiq[0].ring, "KIQ", FENCE_SLOT_KIQ,
			 AMDGPU_FENCE_FLAG_INT, &rptr, 1);

	if (backend_write_count() != 0) {
		printf("  emitting fences produced %u register writes; it should produce none\n",
		       backend_write_count());
		bad++;
	}
	if (backend_cp_stub_rejects() != rejects_before) {
		printf("  the CP stub rejected %u of the fence packets\n",
		       backend_cp_stub_rejects() - rejects_before);
		bad++;
	}
	if (!quiet && bad == 0)
		printf("  gfx, compute 3 and the KIQ each land a value and one vector : yes\n");

	/* The control that says the interrupt is the packet's doing. Same call, same ring, same
	 * slot, one bit fewer: the value still lands and nothing is delivered. */
	{
		u64 addr = bc250_gfx_fence_addr(adev, FENCE_SLOT_GFX);
		unsigned int before = backend_ih_delivered();

		*(volatile u64 *)((char *)adev->gfx.fence_mem.cpu + FENCE_SLOT_GFX * 8u) = 0;
		r = bc250_gfx_signal_fence(&adev->gfx.gfx_ring[0], addr, FENCE_SEQ,
					   AMDGPU_FENCE_FLAG_64BIT);
		if (r != 0 || bc250_gfx_fence_read(adev, FENCE_SLOT_GFX) != FENCE_SEQ) {
			printf("  the no-interrupt fence did not write its value (%d)\n", r);
			bad++;
		}
		if (backend_ih_delivered() != before) {
			printf("  a fence without AMDGPU_FENCE_FLAG_INT still raised an"
			       " interrupt\n");
			bad++;
		} else if (!quiet) {
			printf("  the same fence without _INT raises nothing            : yes\n");
		}
	}

	/* The refusals upstream spells BUG_ON(). Nothing may be written to the ring. */
	{
		u64 wptr_before = adev->gfx.gfx_ring[0].wptr;

		if (bc250_gfx_emit_fence(&adev->gfx.gfx_ring[0],
					 bc250_gfx_fence_addr(adev, FENCE_SLOT_GFX) + 4u,
					 FENCE_SEQ, AMDGPU_FENCE_FLAG_64BIT) != BC250_EINVAL ||
		    adev->gfx.gfx_ring[0].wptr != wptr_before) {
			printf("  a 64-bit fence on a 4-byte-aligned address was not refused\n");
			bad++;
		}
		wptr_before = adev->gfx.kiq[0].ring.wptr;
		if (bc250_gfx_emit_fence(&adev->gfx.kiq[0].ring,
					 bc250_gfx_fence_addr(adev, FENCE_SLOT_KIQ),
					 FENCE_SEQ, AMDGPU_FENCE_FLAG_64BIT) != BC250_EINVAL ||
		    adev->gfx.kiq[0].ring.wptr != wptr_before) {
			printf("  a 64-bit fence on the KIQ was not refused\n");
			bad++;
		} else if (!quiet) {
			printf("  a misaligned fence and a 64-bit KIQ fence are refused : yes\n");
		}
	}

	backend_ih_detach();
	bc250_gfx_fence_page_free(adev);
	bc250_gfx_teardown(adev);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * The SDMA engines: a ring test, a fence, and the trap it raises
 *
 * M36 said "SDMA: rings programmed and engines released, not tested". This is the test. It is the
 * same shape as the CP one above - emit, let the stub execute, read the value back, decode the
 * vector - with one addition: the dwords bc250_sdma.c emits are compared against the dwords unit
 * A's own Linux driver left in the same two rings, so this is not only self-consistent.
 *
 *   evidence/linux/2026-09-21-E13-reference-2/boot3-readonly/rings-after-ib/amdgpu_ring_sdma1.txt
 *     dw 0x0000  00000002 004017c0 00000000 00000000 deadbeef
 *     dw 0x0020  00030005 00401760 00000000 00000001
 *     dw 0x0024  00000006 00000000
 *
 * Only the addresses differ here, because this driver cuts its slots out of its own page. Every
 * other dword is asserted literally.
 * ------------------------------------------------------------------------------------------- */

#define SDMA_TEST_HDR       0x00000002u  /* SDMA_OP_WRITE, SDMA_SUBOP_WRITE_LINEAR */
#define SDMA_TEST_COUNT     0x00000000u  /* SDMA_PKT_WRITE_UNTILED_DW_3_COUNT(0), one data dword */
#define SDMA_TEST_VALUE     0xDEADBEEFu
#define SDMA_FENCE_HDR      0x00030005u  /* SDMA_OP_FENCE with MTYPE(3), Ucached */
#define SDMA_TRAP_HDR       0x00000006u  /* SDMA_OP_TRAP */
#define SDMA_TRAP_CONTEXT   0x00000000u
#define SDMA_FENCE_SLOT(i)  (2u + (unsigned int)(i))
#define SDMA_FENCE_SEQ      0x00000000C0FFEE01ULL

/* The dwords a ring holds between `from` and the write pointer, so the test can read back what the
 * emitter put there rather than what it meant to put there. */
static u32 sdma_ring_dw(const struct amdgpu_ring *ring, u64 at)
{
	return ring->ring[(size_t)(at & ring->buf_mask)];
}

static unsigned int sdma_expect(const char *what, u32 got, u32 want)
{
	if (got == want)
		return 0;
	printf("  %s: %08X, unit A emits %08X\n", what, got, want);
	return 1;
}

static unsigned int sdma_one(struct amdgpu_device *adev, unsigned int inst, u32 *rptr, int quiet)
{
	struct amdgpu_ring *ring = &adev->sdma.instance[inst].ring;
	struct bc250_iv_entry e;
	const struct backend_doorbell *db;
	unsigned int bad = 0, before, delivered, n;
	u64 at, addr;
	u32 instance = 0xffffffffu;
	int r;

	/* --- the ring test ------------------------------------------------------------------ */
	at = ring->wptr;
	before = backend_doorbell_count();
	r = bc250_sdma_ring_test(ring);
	if (r != 0) {
		printf("  sdma%u: bc250_sdma_ring_test returned %d\n", inst, r);
		return 1;
	}

	bad += sdma_expect("sdma ring-test header", sdma_ring_dw(ring, at), SDMA_TEST_HDR);
	bad += sdma_expect("sdma ring-test address high", sdma_ring_dw(ring, at + 2u), 0u);
	bad += sdma_expect("sdma ring-test count", sdma_ring_dw(ring, at + 3u), SDMA_TEST_COUNT);
	bad += sdma_expect("sdma ring-test value", sdma_ring_dw(ring, at + 4u), SDMA_TEST_VALUE);
	if (sdma_ring_dw(ring, at + 1u) != lower_32_bits(bc250_sdma_fence_addr(adev, inst))) {
		printf("  sdma%u: the ring test names an address that is not its scratch slot\n", inst);
		bad++;
	}

	/* The write pointer an SDMA engine is handed counts bytes. This is the assertion that says
	 * so: the doorbell this commit rang must be the ring's dword write pointer times four. */
	n = backend_doorbell_count();
	db = backend_doorbells();
	if (n == before) {
		printf("  sdma%u: the ring test rang no doorbell\n", inst);
		bad++;
	} else if (db[n - 1].value != (ring->wptr << 2)) {
		printf("  sdma%u: the doorbell carries 0x%llX; an SDMA engine is given wptr << 2,"
		       " which is 0x%llX\n", inst,
		       (unsigned long long)db[n - 1].value, (unsigned long long)(ring->wptr << 2));
		bad++;
	} else if (db[n - 1].width != 64) {
		printf("  sdma%u: the doorbell is %u bits; sdma_v5_0_ring_set_wptr() uses 64\n",
		       inst, db[n - 1].width);
		bad++;
	}

	/* --- the fence and its trap --------------------------------------------------------- */
	at = ring->wptr;
	addr = bc250_sdma_fence_addr(adev, SDMA_FENCE_SLOT(inst));
	before = backend_ih_delivered();
	r = bc250_sdma_signal_fence(ring, addr, SDMA_FENCE_SEQ, AMDGPU_FENCE_FLAG_INT);
	if (r != 0) {
		printf("  sdma%u: bc250_sdma_signal_fence returned %d\n", inst, r);
		return bad + 1;
	}

	bad += sdma_expect("sdma fence header", sdma_ring_dw(ring, at), SDMA_FENCE_HDR);
	bad += sdma_expect("sdma fence sequence", sdma_ring_dw(ring, at + 3u),
			   lower_32_bits(SDMA_FENCE_SEQ));
	bad += sdma_expect("sdma trap header", sdma_ring_dw(ring, at + 4u), SDMA_TRAP_HDR);
	bad += sdma_expect("sdma trap context", sdma_ring_dw(ring, at + 5u), SDMA_TRAP_CONTEXT);
	if (((u64)sdma_ring_dw(ring, at + 1u) | ((u64)sdma_ring_dw(ring, at + 2u) << 32)) != addr) {
		printf("  sdma%u: the fence names an address that is not its slot\n", inst);
		bad++;
	}
	if (bc250_sdma_fence_size(ring, AMDGPU_FENCE_FLAG_INT) != 6u) {
		printf("  sdma%u: bc250_sdma_fence_size says %u dwords, the packets are 6\n",
		       inst, bc250_sdma_fence_size(ring, AMDGPU_FENCE_FLAG_INT));
		bad++;
	}

	if ((u32)bc250_sdma_fence_read(adev, SDMA_FENCE_SLOT(inst)) != (u32)SDMA_FENCE_SEQ) {
		printf("  sdma%u: the fence slot holds %08X, not %08X\n", inst,
		       (u32)bc250_sdma_fence_read(adev, SDMA_FENCE_SLOT(inst)),
		       (u32)SDMA_FENCE_SEQ);
		bad++;
	}

	delivered = backend_ih_delivered() - before;
	if (delivered != 1u) {
		printf("  sdma%u: %u vectors delivered, expected 1\n", inst, delivered);
		return bad + 1;
	}
	if (bc250_ih_decode(adev, rptr, &e) != 0) {
		printf("  sdma%u: the delivered vector would not decode\n", inst);
		return bad + 1;
	}
	if (!bc250_ih_is_sdma_trap(&e, &instance) || instance != inst) {
		printf("  sdma%u: the vector decoded as client %02X src %u instance %u, which is not"
		       " this engine's trap\n", inst, e.client_id, e.src_id, instance);
		bad++;
	}

	if (!quiet && bad == 0)
		printf("  sdma%u: ring test, fence and trap all as unit A emits them  : yes\n", inst);
	return bad;
}

static unsigned int test_sdma(struct amdgpu_device *adev, int quiet)
{
	unsigned int bad = 0, i;
	u32 rptr = 0;
	int r;

	printf("\n== the SDMA engines: a ring test, a fence, and its trap ==\n");

	r = bc250_sdma_setup(adev);
	if (r != 0) {
		printf("  bc250_sdma_setup returned %d; no rings to test\n", r);
		return 1;
	}
	for (i = 0; i < (unsigned int)adev->sdma.num_instances; i++)
		backend_ring_register(&adev->sdma.instance[i].ring);

	r = bc250_sdma_fence_page_alloc(adev);
	if (r != 0) {
		printf("  bc250_sdma_fence_page_alloc returned %d\n", r);
		bc250_sdma_teardown(adev);
		return 1;
	}

	/* Nothing from here on may touch a register: this is memory and two doorbells per engine. */
	backend_reset_writes();
	backend_ih_attach(adev);

	for (i = 0; i < (unsigned int)adev->sdma.num_instances; i++)
		bad += sdma_one(adev, i, &rptr, quiet);

	if (backend_write_count() != 0) {
		printf("  the SDMA ring test and fence produced %u register writes; they should"
		       " produce none\n", backend_write_count());
		bad++;
	}

	/* The control that says the trap is the TRAP packet's doing and not the fence's. */
	{
		struct amdgpu_ring *ring = &adev->sdma.instance[0].ring;
		u64 addr = bc250_sdma_fence_addr(adev, SDMA_FENCE_SLOT(0));
		unsigned int before = backend_ih_delivered();

		*(volatile u32 *)((char *)adev->sdma.fence_mem.cpu +
				  SDMA_FENCE_SLOT(0) * 8u) = 0;
		r = bc250_sdma_signal_fence(ring, addr, SDMA_FENCE_SEQ, 0);
		if (r != 0 ||
		    (u32)bc250_sdma_fence_read(adev, SDMA_FENCE_SLOT(0)) != (u32)SDMA_FENCE_SEQ) {
			printf("  the no-interrupt SDMA fence did not write its value (%d)\n", r);
			bad++;
		}
		if (bc250_sdma_fence_size(ring, 0) != 4u) {
			printf("  a fence without _INT should be four dwords, not %u\n",
			       bc250_sdma_fence_size(ring, 0));
			bad++;
		}
		if (backend_ih_delivered() != before) {
			printf("  an SDMA fence without AMDGPU_FENCE_FLAG_INT still raised an"
			       " interrupt\n");
			bad++;
		} else if (!quiet) {
			printf("  the same fence without _INT raises nothing            : yes\n");
		}
	}

	/* The refusal upstream spells BUG_ON(addr & 0x3). Nothing may be written to the ring. */
	{
		struct amdgpu_ring *ring = &adev->sdma.instance[0].ring;
		u64 wptr_before = ring->wptr;

		if (bc250_sdma_emit_fence(ring, bc250_sdma_fence_addr(adev, SDMA_FENCE_SLOT(0)) + 1u,
					  SDMA_FENCE_SEQ, AMDGPU_FENCE_FLAG_INT) != BC250_EINVAL ||
		    ring->wptr != wptr_before) {
			printf("  a fence on an unaligned address was not refused\n");
			bad++;
		} else if (!quiet) {
			printf("  a fence on an unaligned address is refused            : yes\n");
		}
	}

	backend_ih_detach();
	bc250_sdma_fence_page_free(adev);
	bc250_sdma_teardown(adev);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * The compute dispatch
 *
 * bc250_gfx_dispatch_memset() builds its packets out of AMD's register headers; libdrm writes the
 * same packets out as numbers. This compares the two dword for dword, which is the only way a host
 * run can say the offsets were resolved correctly - a wrong offset would still be a well-formed
 * packet and the stub would still execute it.
 *
 * The table is libdrm's numbers, from the packet-by-packet reading in
 * P:\BC-250\scratch\m6\DISPATCH-NOTES.md section 3.3, each row citing
 * tests/amdgpu/shader_test_util.c at tag libdrm-2.4.114. Ten of the 82 dwords are addresses or
 * caller arguments and cannot be constants; they are marked and checked against what this test
 * allocated and asked for.
 *
 * Every constant in it is also a measurement: the same 72 dwords were submitted to unit A under
 * Linux and filled their buffer (fact M49), and the dump of what was submitted is
 * evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/dispatch/g1.txt:14-85 for
 * one workgroup and g16.txt for sixteen. The only dwords that differ between that run and this
 * table are the two addresses, the fill value and, with them, NUM_RECORDS and DIM_X.
 *
 * One of the constants can be cross-checked inside our own tree, which is worth having: the uconfig
 * write in packet 5 is `0xc0017900, 0x7b, 0x20`, and the same three dwords appear verbatim in
 * preamblecache_gfx10[] in third_party/libdrm/shader_code_gfx10.h - a different file, generated by a
 * different path, agreeing on the header encoding and on the offset arithmetic.
 * ------------------------------------------------------------------------------------------- */

#define DISPATCH_FENCE_SLOT	6u
#define DISPATCH_SEQ		0x00000000D15A7C41ULL
#define DISPATCH_VALUE		0x22222222u      /* shader_test_util.c:441-444 */

/* Marks a dword this test fills in from its own allocations rather than from libdrm. */
#define D_COMPUTED		0xFFFFFFFFu

enum dispatch_fill {
	FILL_NONE = 0,
	FILL_PGM_LO, FILL_PGM_HI,
	FILL_DST_LO, FILL_DST_HI,
	FILL_VALUE,
	FILL_RECORDS,
	FILL_GROUPS
};

struct dispatch_dword {
	u32 want;
	enum dispatch_fill fill;
	const char *what;
};

static const struct dispatch_dword g_dispatch_ref[] = {
	/* ours: what amdgpu_ib_schedule() puts in front of an IB, gfx_v10_0.c:9473-9494 */
	{ 0xC0065800, FILL_NONE, "ACQUIRE_MEM header" },
	{ 0x00000000, FILL_NONE, "CP_COHER_CNTL" },
	{ 0xFFFFFFFF, FILL_NONE, "CP_COHER_SIZE" },
	{ 0x00FFFFFF, FILL_NONE, "CP_COHER_SIZE_HI" },
	{ 0x00000000, FILL_NONE, "CP_COHER_BASE" },
	{ 0x00000000, FILL_NONE, "CP_COHER_BASE_HI" },
	{ 0x0000000A, FILL_NONE, "POLL_INTERVAL" },
	{ 0x0000C3B1, FILL_NONE, "GCR_CNTL" },

	/* 1: shader_test_util.c:218-220, COMPUTE_START_X/Y/Z */
	{ 0xC0037602, FILL_NONE, "SET_SH_REG count 3, compute bit" },
	{ 0x00000204, FILL_NONE, "offset COMPUTE_START_X" },
	{ 0x00000000, FILL_NONE, "COMPUTE_START_X" },
	{ 0x00000000, FILL_NONE, "COMPUTE_START_Y" },
	{ 0x00000000, FILL_NONE, "COMPUTE_START_Z" },
	/* 2: :223-225 */
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000218, FILL_NONE, "offset COMPUTE_TMPRING_SIZE" },
	{ 0x00000000, FILL_NONE, "COMPUTE_TMPRING_SIZE" },
	/* 3: :240-242 */
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x0000022A, FILL_NONE, "offset COMPUTE_SHADER_CHKSUM" },
	{ 0x00000000, FILL_NONE, "COMPUTE_SHADER_CHKSUM" },
	/* 4: :244-246, COMPUTE_REQ_CTRL and the five dwords after it */
	{ 0xC0067602, FILL_NONE, "SET_SH_REG count 6" },
	{ 0x00000222, FILL_NONE, "offset COMPUTE_REQ_CTRL" },
	{ 0x00000000, FILL_NONE, "COMPUTE_REQ_CTRL" },
	{ 0x00000000, FILL_NONE, "(unnamed, mm 0x1bc3)" },
	{ 0x00000000, FILL_NONE, "COMPUTE_USER_ACCUM_0" },
	{ 0x00000000, FILL_NONE, "COMPUTE_USER_ACCUM_1" },
	{ 0x00000000, FILL_NONE, "COMPUTE_USER_ACCUM_2" },
	{ 0x00000000, FILL_NONE, "COMPUTE_USER_ACCUM_3" },
	/* 5: :248-250, the one packet without the compute bit */
	{ 0xC0017900, FILL_NONE, "SET_UCONFIG_REG count 1, no compute bit" },
	{ 0x0000007B, FILL_NONE, "offset CP_COHER_START_DELAY" },
	{ 0x00000020, FILL_NONE, "CP_COHER_START_DELAY" },
	/* 6 and 7: :331-339, the CU masks through index 3 */
	{ 0xC0029B02, FILL_NONE, "SET_SH_REG_INDEX count 2" },
	{ 0x30000216, FILL_NONE, "index 3, offset STATIC_THREAD_MGMT_SE0" },
	{ 0xFFFFFFFF, FILL_NONE, "SE0 mask" },
	{ 0xFFFFFFFF, FILL_NONE, "SE1 mask" },
	{ 0xC0029B02, FILL_NONE, "SET_SH_REG_INDEX count 2" },
	{ 0x30000219, FILL_NONE, "index 3, offset STATIC_THREAD_MGMT_SE2" },
	{ 0xFFFFFFFF, FILL_NONE, "SE2 mask" },
	{ 0xFFFFFFFF, FILL_NONE, "SE3 mask" },
	/* 8: :413-416, the program address */
	{ 0xC0027602, FILL_NONE, "SET_SH_REG count 2" },
	{ 0x0000020C, FILL_NONE, "offset COMPUTE_PGM_LO" },
	{ D_COMPUTED, FILL_PGM_LO, "COMPUTE_PGM_LO = shader >> 8" },
	{ D_COMPUTED, FILL_PGM_HI, "COMPUTE_PGM_HI = shader >> 40" },
	/* 9..13: :418-423, the five-entry register table of shader_code_gfx9.h:34-40 */
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000212, FILL_NONE, "offset COMPUTE_PGM_RSRC1" },
	{ 0x000C0041, FILL_NONE, "COMPUTE_PGM_RSRC1" },
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000213, FILL_NONE, "offset COMPUTE_PGM_RSRC2" },
	{ 0x00000090, FILL_NONE, "COMPUTE_PGM_RSRC2" },
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000207, FILL_NONE, "offset COMPUTE_NUM_THREAD_X" },
	{ 0x00000040, FILL_NONE, "COMPUTE_NUM_THREAD_X" },
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000208, FILL_NONE, "offset COMPUTE_NUM_THREAD_Y" },
	{ 0x00000001, FILL_NONE, "COMPUTE_NUM_THREAD_Y" },
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000209, FILL_NONE, "offset COMPUTE_NUM_THREAD_Z" },
	{ 0x00000001, FILL_NONE, "COMPUTE_NUM_THREAD_Z" },
	/* 14: :426-428 */
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000228, FILL_NONE, "offset COMPUTE_PGM_RSRC3" },
	{ 0x00000000, FILL_NONE, "COMPUTE_PGM_RSRC3" },
	/* 15: :431-436, the buffer descriptor */
	{ 0xC0047602, FILL_NONE, "SET_SH_REG count 4" },
	{ 0x00000240, FILL_NONE, "offset COMPUTE_USER_DATA_0" },
	{ D_COMPUTED, FILL_DST_LO, "V# base low" },
	{ D_COMPUTED, FILL_DST_HI, "V# base high | STRIDE 16" },
	{ D_COMPUTED, FILL_RECORDS, "NUM_RECORDS = the bytes this dispatch covers / 16" },
	{ 0x1104BFAC, FILL_NONE, "V# word 3" },
	/* 16: :439-444, the fill pattern */
	{ 0xC0047602, FILL_NONE, "SET_SH_REG count 4" },
	{ 0x00000244, FILL_NONE, "offset COMPUTE_USER_DATA_4" },
	{ D_COMPUTED, FILL_VALUE, "USER_DATA_4" },
	{ D_COMPUTED, FILL_VALUE, "USER_DATA_5" },
	{ D_COMPUTED, FILL_VALUE, "USER_DATA_6" },
	{ D_COMPUTED, FILL_VALUE, "USER_DATA_7" },
	/* 17: :553-555 */
	{ 0xC0017602, FILL_NONE, "SET_SH_REG count 1" },
	{ 0x00000215, FILL_NONE, "offset COMPUTE_RESOURCE_LIMITS" },
	{ 0x00000000, FILL_NONE, "COMPUTE_RESOURCE_LIMITS" },
	/* 18: :558-562 */
	{ 0xC0031502, FILL_NONE, "DISPATCH_DIRECT count 3, compute bit" },
	{ D_COMPUTED, FILL_GROUPS, "DIM_X" },
	{ 0x00000001, FILL_NONE, "DIM_Y" },
	{ 0x00000001, FILL_NONE, "DIM_Z" },
	{ 0x00000001, FILL_NONE, "DISPATCH_INITIATOR = COMPUTE_SHADER_EN" },

	/* ours: the waves have to retire before the fence behind this may signal */
	{ 0xC0004600, FILL_NONE, "EVENT_WRITE header" },
	{ 0x00000407, FILL_NONE, "CS_PARTIAL_FLUSH, event index 4" }
};

static unsigned int compare_dispatch(const struct amdgpu_ring *ring, u64 from, u32 value,
				     u32 groups, u64 shader, u64 dst, int quiet)
{
	unsigned int bad = 0, i;

	for (i = 0; i < ARRAY_SIZE(g_dispatch_ref); i++) {
		u32 got = ring->ring[(size_t)((from + i) & ring->buf_mask)];
		u32 want = g_dispatch_ref[i].want;

		switch (g_dispatch_ref[i].fill) {
		case FILL_PGM_LO: want = (u32)(shader >> 8); break;
		case FILL_PGM_HI: want = (u32)(shader >> 40); break;
		case FILL_DST_LO: want = (u32)dst; break;
		case FILL_DST_HI: want = (u32)(dst >> 32) | 0x100000u; break;
		case FILL_VALUE:  want = value; break;
		case FILL_RECORDS: want = groups * BC250_DISPATCH_THREADS_PER_GROUP; break;
		case FILL_GROUPS: want = groups; break;
		case FILL_NONE:   break;
		}
		if (got == want)
			continue;
		printf("  dword %2u %-40s: %08X, libdrm emits %08X\n", i,
		       g_dispatch_ref[i].what, got, want);
		bad++;
	}
	if (bad == 0 && !quiet)
		printf("  all %u dwords equal libdrm's own sequence          : yes\n",
		       (unsigned int)ARRAY_SIZE(g_dispatch_ref));
	return bad;
}

static unsigned int test_dispatch(struct amdgpu_device *adev, int quiet)
{
	struct bc250_gfx_inputs fin;
	struct amdgpu_ring *ring;
	unsigned int bad = 0, i, vectors_before, rejects_before, expect_rejects = 0;
	u64 from, shader, dst, fence_addr;
	u32 first_bad = 0;
	int r;

	printf("\n== a compute dispatch: 64 threads a workgroup, 16 bytes a thread ==\n");

	memset(&fin, 0, sizeof(fin));
	fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
	fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
	fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
	fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
	fin.async_gfx_ring = true;
	fin.pp_gfxoff = true;

	if (bc250_gfx_setup(adev, &fin) != 0) {
		printf("  bc250_gfx_setup failed; no ring to dispatch on\n");
		return 1;
	}
	backend_ring_register(&adev->gfx.gfx_ring[0]);
	for (i = 0; i < adev->gfx.num_compute_rings; i++)
		backend_ring_register(&adev->gfx.compute_ring[i]);
	backend_ring_register(&adev->gfx.kiq[0].ring);

	if (bc250_gfx_fence_page_alloc(adev) != 0 || bc250_gfx_dispatch_setup(adev) != 0) {
		printf("  the dispatch buffers could not be allocated\n");
		bc250_gfx_teardown(adev);
		return 1;
	}

	shader = bc250_gfx_dispatch_shader_addr(adev);
	dst = bc250_gfx_dispatch_dst_addr(adev);
	fence_addr = bc250_gfx_fence_addr(adev, DISPATCH_FENCE_SLOT);
	printf("  shader at 0x%llX (%u bytes), destination at 0x%llX (%u bytes)\n",
	       (unsigned long long)shader, 9u * 4u, (unsigned long long)dst,
	       BC250_DISPATCH_DST_BYTES);

	if ((shader & 0xffu) != 0) {
		printf("  the shader is not 256-byte aligned, so COMPUTE_PGM_LO cannot name it\n");
		bad++;
	}

	/* Nothing below may write a register: a dispatch is memory and one doorbell. */
	backend_reset_writes();
	backend_ih_attach(adev);
	ring = &adev->gfx.compute_ring[0];
	vectors_before = backend_ih_delivered();
	rejects_before = backend_cp_stub_rejects();
	from = ring->wptr;

	r = bc250_gfx_dispatch_memset(ring, DISPATCH_VALUE, 1u, fence_addr, DISPATCH_SEQ,
				      AMDGPU_FENCE_FLAG_64BIT | AMDGPU_FENCE_FLAG_INT);
	if (r != 0) {
		printf("  bc250_gfx_dispatch_memset returned %d\n", r);
		bad++;
	}

	/* The write pointer moves further than the submission: amdgpu_ring_commit() pads to the
	 * CP's fetch size (bc250_ring.c:86-91, align_mask 0xff on a compute ring). So the check is
	 * that the submission is the size it reserved and that everything from there to the pad
	 * boundary is a NOP - a dword left over from a previous submission inside the padding would
	 * be fetched and executed. */
	{
		unsigned int size = bc250_gfx_dispatch_size(ring, AMDGPU_FENCE_FLAG_64BIT |
								  AMDGPU_FENCE_FLAG_INT);
		u64 align = ring->funcs->align_mask;
		u64 want = ((from + size + align) & ~align) - from;
		unsigned int nops = 0;

		for (i = size; i < (unsigned int)want; i++)
			if (ring->ring[(size_t)((from + i) & ring->buf_mask)] != ring->funcs->nop)
				nops++;

		if (ring->wptr - from != want || nops != 0) {
			printf("  the submission is %llu dwords, not %u padded to %llu, and %u of the"
			       " padding dwords are not NOPs\n", (unsigned long long)(ring->wptr - from),
			       size, (unsigned long long)want, nops);
			bad++;
		}
	}

	bad += compare_dispatch(ring, from, DISPATCH_VALUE, 1u, shader, dst, quiet);

	/* The submission as dwords, in the layout the Linux reference prints (the "# raw:" block of
	 * evidence/linux/2026-09-21-E13-reference-2/boot4-readonly-after-windows/dispatch/g1.txt), so
	 * that dispatch.py's output and ours can be diffed as text. The eight ACQUIRE_MEM dwords in
	 * front are ours and have no counterpart there: the 72 that follow are the comparable ones,
	 * which is why the block starts where it does. */
	if (!quiet) {
		unsigned int k;

		printf("\n  the 72 dwords, to diff against dispatch.py --dry:\n");
		for (k = 0; k < 72u; k++) {
			if (k % 8u == 0u)
				printf("  ");
			printf("%08x%s", ring->ring[(size_t)((from + 8u + k) & ring->buf_mask)],
			       (k % 8u == 7u || k == 71u) ? "\n" : " ");
		}
		printf("\n");
	}

	if (backend_dispatch_count() != 1u) {
		printf("  the CP stub carried out %u dispatches, not one\n",
		       backend_dispatch_count());
		bad++;
	}
	if (bc250_gfx_dispatch_check(adev, DISPATCH_VALUE, 1u, &first_bad) != 0) {
		printf("  the memory is wrong at byte 0x%X   <-- the dispatch did not do what it"
		       " was asked\n", first_bad);
		bad++;
	} else if (!quiet) {
		printf("  one workgroup wrote its 1024 bytes and nothing else : yes\n");
	}
	if (bc250_gfx_fence_read(adev, DISPATCH_FENCE_SLOT) != DISPATCH_SEQ) {
		printf("  the fence behind the dispatch did not land its value\n");
		bad++;
	}
	if (backend_ih_delivered() != vectors_before + 1u) {
		printf("  the dispatch raised %u vectors, not one\n",
		       backend_ih_delivered() - vectors_before);
		bad++;
	} else if (!quiet) {
		printf("  the fence behind it landed and raised one vector    : yes\n");
	}
	if (backend_write_count() != 0) {
		printf("  the dispatch produced %u register writes; it should produce none\n",
		       backend_write_count());
		bad++;
	}

	/* The whole buffer, which is libdrm's own dispatch: 16 workgroups over 0x4000 bytes. */
	{
		u32 other = 0x5A5A5A5Au;

		bc250_gfx_dispatch_reseed(adev);
		r = bc250_gfx_dispatch_memset(ring, other, BC250_DISPATCH_MAX_GROUPS, fence_addr,
					      DISPATCH_SEQ + 1u,
					      AMDGPU_FENCE_FLAG_64BIT | AMDGPU_FENCE_FLAG_INT);
		if (r != 0 ||
		    bc250_gfx_dispatch_check(adev, other, BC250_DISPATCH_MAX_GROUPS, &first_bad) != 0) {
			printf("  the 16-workgroup dispatch is wrong at byte 0x%X (%d)\n",
			       first_bad, r);
			bad++;
		} else if (!quiet) {
			printf("  16 workgroups fill the whole 0x4000 bytes           : yes\n");
		}
	}

	/* Controls. Each one must be refused with nothing written to the ring. */
	{
		u64 wptr_before = ring->wptr;

		if (bc250_gfx_dispatch_memset(ring, DISPATCH_VALUE, 0, fence_addr, DISPATCH_SEQ, 0)
			    != BC250_EINVAL ||
		    bc250_gfx_dispatch_memset(ring, DISPATCH_VALUE, BC250_DISPATCH_MAX_GROUPS + 1u,
					      fence_addr, DISPATCH_SEQ, 0) != BC250_EINVAL ||
		    bc250_gfx_dispatch_memset(&adev->gfx.gfx_ring[0], DISPATCH_VALUE, 1u, fence_addr,
					      DISPATCH_SEQ, 0) != BC250_EINVAL ||
		    ring->wptr != wptr_before) {
			printf("  a dispatch of 0 or 17 workgroups, or one on the gfx ring, was not"
			       " refused\n");
			bad++;
		} else if (!quiet) {
			printf("  0 groups, 17 groups and the gfx ring are refused    : yes\n");
		}
	}

	/* The control that says the check is looking: corrupt one dword of what the dispatch wrote
	 * and it has to be found, at its own offset. */
	{
		volatile u32 *p = (volatile u32 *)adev->gfx.dispatch_dst.cpu;
		u32 keep = p[7];

		p[7] = 0;
		if (bc250_gfx_dispatch_check(adev, 0x5A5A5A5Au, BC250_DISPATCH_MAX_GROUPS,
					     &first_bad) != BC250_EIO || first_bad != 7u * 4u) {
			printf("  a corrupted dword was not reported at byte 0x1C (got 0x%X)\n",
			       first_bad);
			bad++;
		} else if (!quiet) {
			printf("  one wrong dword is found, at its own offset         : yes\n");
		}
		p[7] = keep;
	}

	/* The control that says the bytes came from the dispatch. Same call, same buffers, with the
	 * CP stub turned off: nothing executes the packets, so the destination is still the seed. If
	 * this passed, the check above would be reading something that was already there. */
	{
		unsigned int before = backend_dispatch_count();

		bc250_gfx_dispatch_reseed(adev);
		backend_cp_stub_enable(0);
		r = bc250_gfx_dispatch_memset(ring, DISPATCH_VALUE, 1u, fence_addr, DISPATCH_SEQ, 0);
		backend_cp_stub_enable(1);
		if (r != 0 ||
		    bc250_gfx_dispatch_check(adev, DISPATCH_VALUE, 1u, &first_bad) != BC250_EIO ||
		    first_bad != 0u || backend_dispatch_count() != before) {
			printf("  with the CP stub off the destination still changed (%d, 0x%X)\n",
			       r, first_bad);
			bad++;
		} else if (!quiet) {
			printf("  with no CP, nothing is written and check() fails    : yes\n");
		}
	}

	/* The control that says the stub is checking the shader and not only the packets: one word
	 * of the shader buffer changed, and the dispatch has to be refused rather than carried out.
	 * This is what makes "the shader was copied correctly" a tested statement. */
	{
		volatile u32 *code = (volatile u32 *)adev->gfx.dispatch_shader.cpu;
		u32 keep = code[4];
		unsigned int rej = backend_cp_stub_rejects();
		unsigned int before = backend_dispatch_count();

		bc250_gfx_dispatch_reseed(adev);
		code[4] = 0xDEADC0DEu;
		printf("  (the stub's complaint on stderr below belongs to this control)\n");
		r = bc250_gfx_dispatch_memset(ring, DISPATCH_VALUE, 1u, fence_addr, DISPATCH_SEQ, 0);
		code[4] = keep;
		if (r != 0 || backend_cp_stub_rejects() != rej + 1u ||
		    backend_dispatch_count() != before ||
		    bc250_gfx_dispatch_check(adev, DISPATCH_VALUE, 1u, &first_bad) != BC250_EIO) {
			printf("  a corrupted shader was dispatched anyway (%d)\n", r);
			bad++;
		} else if (!quiet) {
			printf("  a shader that is not libdrm's is refused            : yes\n");
		}
		expect_rejects++;
	}

	if (backend_cp_stub_rejects() - rejects_before != expect_rejects) {
		printf("  the CP stub rejected %u packets, %u of them deliberately\n",
		       backend_cp_stub_rejects() - rejects_before, expect_rejects);
		bad++;
	}

	backend_ih_detach();
	bc250_gfx_dispatch_teardown(adev);
	bc250_gfx_fence_page_free(adev);
	bc250_gfx_teardown(adev);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	static struct amdgpu_device adev;
	struct run_opts opt;
	struct result res, ctl;
	unsigned int failures = 0, decode_bad, ptr_bad, fence_bad, sdma_bad, dispatch_bad;
	int verbose = 0, seeded, i;
	const char *sweep1 = NULL, *sweep2 = NULL, *trace = NULL;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			verbose = 1;
		else if (sweep1 == NULL)
			sweep1 = argv[i];
		else if (sweep2 == NULL)
			sweep2 = argv[i];
		else if (trace == NULL)
			trace = argv[i];
	}
	if (trace == NULL) {
		fprintf(stderr, "usage: replay_ih <sweep-run1.log> <sweep-run2.log>"
				" <trace-ih.txt> [-v]\n");
		return 2;
	}

	backend_set_verbose(verbose);

	if (backend_load_sweep(sweep1) < 0 || backend_load_sweep(sweep2) < 0) {
		fprintf(stderr, "could not read the register sweeps\n");
		return 2;
	}
	if (load_trace_writes(trace) < 0) {
		fprintf(stderr, "could not read %s\n", trace);
		return 2;
	}
	seeded = backend_seed_reads(trace);
	if (seeded < 0) {
		fprintf(stderr, "could not seed reads from %s\n", trace);
		return 2;
	}
	printf("trace window: %u writes, %d offsets seeded with their traced read values\n",
	       g_trace_count, seeded);

	/* The one run under test. */
	opt.msi = UNITA_MSI;
	opt.selfclear = 1;
	run_one("navi10_ih_irq_init, replayed", &opt, &adev, &res, 0);

	if (res.hw_init_rc != 0 || res.mismatches != 0 || res.address_failures != 0 ||
	    res.unknown_reads != 0 || res.produced != g_trace_count)
		failures++;

	/* fini then init again. Upstream's disable leaves the ring allocated on purpose, and the
	 * miniport needs the second init to work: a PSP reload in one boot forces exactly that. */
	{
		unsigned int again;

		printf("\n== hw_fini, then hw_init again on the same adev ==\n");
		backend_reset_writes();
		bc250_ih_hw_fini(&adev);
		again = backend_write_count();
		printf("  the disable writes %u registers\n", again);

		backend_reset_writes();
		res.hw_init_rc = bc250_ih_hw_init(&adev);
		printf("  the second bc250_ih_hw_init returns %d and writes %u registers\n",
		       res.hw_init_rc, backend_write_count());
		if (res.hw_init_rc != 0 || backend_write_count() != g_trace_count) {
			printf("  the second bring-up is not the same shape as the first\n");
			failures++;
		}
	}

	decode_bad = test_decode(&adev);
	if (decode_bad)
		failures++;

	ptr_bad = test_wptr_rptr(&adev, 0);
	if (ptr_bad)
		failures++;

	sdma_bad = test_sdma(&adev, 0);
	if (sdma_bad)
		failures++;

	fence_bad = test_fence(&adev, 0);
	if (fence_bad)
		failures++;

	dispatch_bad = test_dispatch(&adev, 0);
	if (dispatch_bad)
		failures++;

	bc250_ih_teardown(&adev);

	/* ------------------------------------------------------------------------------------
	 * The controls. Each one changes exactly one thing that the run above depends on, and each
	 * one has to fail; a control that passes means the run above was not testing what it claims.
	 * ---------------------------------------------------------------------------------- */
	printf("\n== controls, each of which must FAIL ==\n");

	opt.msi = false;
	opt.selfclear = 1;
	run_one("MSI off", &opt, &adev, &ctl, 1);
	printf("  MSI off                          : %s (%u mismatches)\n",
	       ctl.mismatches ? "fails, as it should" : "PASSES, which is wrong", ctl.mismatches);
	if (ctl.mismatches == 0)
		failures++;
	bc250_ih_teardown(&adev);

	opt.msi = UNITA_MSI;
	opt.selfclear = 0;
	run_one("WPTR_OVERFLOW_CLEAR not modelled", &opt, &adev, &ctl, 1);
	printf("  the self-clearing bit not modelled: %s (%u mismatches)\n",
	       ctl.mismatches ? "fails, as it should" : "PASSES, which is wrong", ctl.mismatches);
	if (ctl.mismatches == 0)
		failures++;
	bc250_ih_teardown(&adev);

	printf("\n");
	if (failures == 0)
		printf("EXACT MATCH over %u writes (%u address exceptions); the decode, the pointers,"
		       " the fence and a compute dispatch that is libdrm's own dword for dword all"
		       " check out, and both controls fail as they should\n",
		       res.compared, res.address_exceptions);
	else
		printf("%u checks failed\n", failures);

	return failures == 0 ? 0 : 1;
}
