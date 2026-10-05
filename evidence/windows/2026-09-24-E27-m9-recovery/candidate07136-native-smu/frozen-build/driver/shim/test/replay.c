/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host replay of amdgpu's GART bring-up on unit A (milestone M4, ADR 0002).
 *
 * Runs the code the driver itself will run - driver/shim/bc250_gmc.c over the unmodified AMD
 * sources in driver/amdgpu-import/ - against a backend that answers reads from unit A's own
 * pre-driver register state, and compares every write it produces, in order and by byte offset,
 * with the writes amdgpu actually made on that unit during experiment E03.
 *
 *   replay <sweep-run1.log> <sweep-run2.log> <trace-extract.txt> [-v]
 *
 * The trace extract is
 *   python tools/trace/extract_phase.py <evidence>/amdgpu-events.txt \
 *          --match 'GCVM|GCMC|MMVM|MMMC' --until 0.26
 *
 * Nothing in this file decides what the hardware is told; that is bc250_gmc.c's job. What is here
 * is the three run-specific addresses of that amdgpu run, and the comparison.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_trace.h"
#include "bc250_gmc.h"

/* ---------------------------------------------------------------------------------------------
 * Unit A, experiment E03 (evidence/linux/2026-09-21-E03-init-trace), kernel 6.18.52-0-lts.
 *
 * Allocations of that one amdgpu run. No register state predicts them, so they are inputs.
 *
 *   GART table   dmesg.txt: "PCIE GART of 512M enabled (table at 0x000000F5FFE00000)."
 *                The kernel log line, not the register trace.
 *   scratch page the traced GCMC_VM_SYSTEM_APERTURE_DEFAULT_ADDR_LSB, 0x002708C9. amdgpu writes
 *                amdgpu_gmc_vram_mc2pa(mem_scratch.gpu_addr) >> 12 there, so the MC address is
 *                that value shifted back and run through mc2pa in reverse - which is done below,
 *                once the setup has read the VRAM base off the hardware.
 *   dummy page   the traced GCVM_L2_PROTECTION_FAULT_DEFAULT_ADDR_LO32, 0x0007E3C1. amdgpu writes
 *                dummy_page_addr >> 12 there. It is a system-memory DMA address.
 * ------------------------------------------------------------------------------------------- */
#define UNITA_GART_TABLE_MC                     0x000000F5FFE00000ULL
#define UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB   0x002708C9u
#define UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32    0x0007E3C1u

/* ---------------------------------------------------------------------------------------------
 * The reference trace
 * ------------------------------------------------------------------------------------------- */

#define MAX_TRACE 4096

struct trace_entry {
	u32 byte_offset;
	u32 value;
	char name[64];
};

static struct trace_entry g_trace[MAX_TRACE];
static unsigned int g_trace_count;

/* Lines of tools/trace/extract_phase.py:
 *     "   0.039  W  GC.GCVM_CONTEXT0_PAGE_TABLE_BASE_ADDR_LO32   0x0a3ac  6FE00001   x2"
 * The trailing xN is a run of identical consecutive accesses and is expanded back out here. */
static int load_trace(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		double t;
		char kind[8], name[256], rep[32];
		unsigned int off = 0, val = 0, n = 1, i;
		int fields = sscanf(line, "%lf %7s %255s 0x%x %x %31s", &t, kind, name, &off, &val, rep);

		if (fields < 5)
			continue;
		if (kind[0] != 'W')
			continue;               /* the extract only lists writes unless --reads */
		if (fields == 6 && rep[0] == 'x')
			n = (unsigned int)strtoul(rep + 1, NULL, 10);
		for (i = 0; i < n && g_trace_count < MAX_TRACE; i++) {
			g_trace[g_trace_count].byte_offset = off;
			g_trace[g_trace_count].value = val;
			strncpy(g_trace[g_trace_count].name, name, sizeof(g_trace[0].name) - 1);
			g_trace[g_trace_count].name[sizeof(g_trace[0].name) - 1] = '\0';
			g_trace_count++;
		}
	}
	fclose(f);
	return (int)g_trace_count;
}

/* ---------------------------------------------------------------------------------------------
 * One run of the real bring-up code
 * ------------------------------------------------------------------------------------------- */

struct result {
	unsigned int produced;
	unsigned int compared;
	unsigned int mismatches;
	unsigned int unknown_reads;
	unsigned int disable_writes;
	int setup_rc;
	int enable_rc;
};

/* gfxhub_v2_0_gart_disable() and mmhub_v2_0_gart_disable() each clear 16 CONTEXTn_CNTL registers,
 * then write MC_VM_MX_L1_TLB_CNTL, VM_L2_CNTL and VM_L2_CNTL3: 19 per hub, 38 in all. The E03
 * window ends with the GPU running, so there is no traced counterpart to compare against - this is
 * a smoke check that the disable path runs at all and reaches as many registers as its two
 * upstream functions write. */
#define EXPECTED_DISABLE_WRITES 38

static void run_one(const char *title, bool noretry, struct result *res)
{
	struct amdgpu_device adev;
	struct amdgpu_bo gart_bo;
	struct bc250_gmc_inputs in;
	const struct bc250_reg_write *w;
	unsigned int i, n;

	memset(&adev, 0, sizeof(adev));
	memset(&gart_bo, 0, sizeof(gart_bo));
	memset(&in, 0, sizeof(in));
	adev.dev = (void *)"BC250-A";

	backend_reset_state();
	backend_reset_writes();

	/* The scratch page's MC address needs the VRAM base, which only the hardware knows, so the
	 * setup runs twice: once to learn the layout, then again with the address filled in. Both
	 * passes only read. */
	in.gart_table_mc = UNITA_GART_TABLE_MC;
	in.dummy_page_dma = (u64)UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32 << 12;
	in.noretry = noretry;
	res->setup_rc = bc250_gmc_setup(&adev, &in, &gart_bo);
	if (res->setup_rc == 0) {
		in.mem_scratch_mc = ((u64)UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB << 12)
				    - adev.vm_manager.vram_base_offset + adev.gmc.vram_start;
		res->setup_rc = bc250_gmc_setup(&adev, &in, &gart_bo);
	}

	backend_reset_writes();         /* the setup only reads; start the record at gart_enable */
	res->enable_rc = res->setup_rc == 0 ? bc250_gmc_gart_enable(&adev) : res->setup_rc;

	w = backend_writes();
	res->produced = backend_write_count();
	res->unknown_reads = backend_unknown_reads();
	res->mismatches = 0;
	n = res->produced < g_trace_count ? res->produced : g_trace_count;
	res->compared = n;

	printf("\n== %s ==\n", title);
	printf("  setup %d, gart_enable %d\n", res->setup_rc, res->enable_rc);
	for (i = 0; i < n; i++) {
		if (w[i].byte_offset == g_trace[i].byte_offset && w[i].value == g_trace[i].value)
			continue;
		res->mismatches++;
		if (res->mismatches <= 40) {
			printf("  [%3u] shim 0x%05X = %08X   trace %-46s 0x%05X = %08X",
			       i, w[i].byte_offset, w[i].value,
			       g_trace[i].name, g_trace[i].byte_offset, g_trace[i].value);
			if (w[i].byte_offset == g_trace[i].byte_offset)
				printf("   differing bits %08X", w[i].value ^ g_trace[i].value);
			printf("\n");
		}
	}
	if (res->mismatches > 40)
		printf("  ... %u further mismatches not listed\n", res->mismatches - 40);
	if (res->mismatches == 0)
		printf("  the %u writes are identical to the first %u writes of the trace,"
		       " offset for offset and value for value\n", n, n);

	/* Unloading is not part of the traced window, so all that is checked is that the disable
	 * path runs on the same adev and reaches the register count its sources imply. */
	backend_reset_writes();
	if (res->setup_rc == 0)
		bc250_gmc_gart_disable(&adev);
	res->disable_writes = backend_write_count();
	printf("  gart_disable: %u writes (expected %u: 19 per hub)%s\n",
	       res->disable_writes, EXPECTED_DISABLE_WRITES,
	       res->disable_writes == EXPECTED_DISABLE_WRITES ? "" : "   <-- unexpected");
}

int main(int argc, char **argv)
{
	struct result derived, control;
	unsigned int i;
	int verbose = 0;
	const char *sweep1, *sweep2, *trace;

	if (argc < 4) {
		fprintf(stderr, "usage: replay <sweep-run1.log> <sweep-run2.log> <trace-extract.txt> [-v]\n");
		return 2;
	}
	sweep1 = argv[1];
	sweep2 = argv[2];
	trace = argv[3];
	for (i = 4; i < (unsigned int)argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			verbose = 1;
	backend_set_verbose(verbose);

	if (backend_load_sweep(sweep1) < 0) {
		fprintf(stderr, "cannot read %s\n", sweep1);
		return 2;
	}
	if (backend_load_sweep(sweep2) < 0) {
		fprintf(stderr, "cannot read %s\n", sweep2);
		return 2;
	}
	if (load_trace(trace) < 0) {
		fprintf(stderr, "cannot read %s\n", trace);
		return 2;
	}

	printf("BC-250 M4 shim replay: amdgpu GART bring-up on unit A (experiment E03)\n");
	printf("  reference trace entries in the window: %u\n", g_trace_count);

	/* noretry is the one policy bit in struct bc250_gmc_inputs. Unit A ran Alpine's
	 * 6.18.52-0-lts, where amdgpu_gmc_noretry_set() makes it true for every GC >= 10.1.0
	 * (mainline v6.18 still had that threshold at 10.3.0, which is why the number has to come
	 * from the kernel that ran, not from the tag we import the hub code at). The control below
	 * is the other value: it must differ, and only in the bit that value reaches. */
	run_one("derived: gmc.noretry = true (kernel 6.18.52 amdgpu_gmc_noretry_set, GC 10.1.3)",
		true, &derived);
	run_one("control: gmc.noretry = false (the value mainline v6.18 would have used)",
		false, &control);

	printf("\n== summary ==\n");
	printf("  writes produced by the shim      : %u (both runs)\n", derived.produced);
	printf("  writes in the trace window       : %u\n", g_trace_count);
	printf("  compared in order                : %u\n", derived.compared);
	printf("  mismatches, derived              : %u\n", derived.mismatches);
	printf("  mismatches, control              : %u (expected: 30, one bit each)\n",
	       control.mismatches);
	printf("  gart_disable writes (not traced) : %u of %u expected\n",
	       derived.disable_writes, EXPECTED_DISABLE_WRITES);
	printf("  reads with no value on unit A    : %u", derived.unknown_reads);
	if (derived.unknown_reads)
		printf(" (first at byte offset 0x%05X)", backend_first_unknown_read());
	printf("\n");

	if (g_trace_count > derived.produced) {
		unsigned int extra = g_trace_count - derived.produced;

		printf("  extra writes in the trace after the compared prefix: %u\n", extra);
		printf("    they are the repeated engine-17 invalidations that follow gart_enable;\n"
		       "    amdgpu_gart_invalidate_tlb() walks adev->vmhubs_mask, so each round is\n"
		       "    GFXHUB request, then MMHUB request and semaphore release:\n");
		for (i = derived.produced; i < g_trace_count && i < derived.produced + 6; i++)
			printf("      [%3u] %-46s 0x%05X = %08X\n", i, g_trace[i].name,
			       g_trace[i].byte_offset, g_trace[i].value);
		if (extra > 6)
			printf("      ... %u more of the same three registers\n", extra - 6);
	}
	if (derived.produced > g_trace_count)
		printf("  the shim produced %u writes the trace window does not contain\n",
		       derived.produced - g_trace_count);

	/* bc250_gmc_gart_enable returns 0 even when a flush times out, exactly as upstream carries
	 * on; the timeout is expected here because the backend keeps the pre-driver acknowledge
	 * value. What matters is that the write sequence is the same either way, which is what the
	 * "always release the semaphore" rule in bc250_gmc_flush_gpu_tlb buys. */
	printf("\n  note: the two TLB flushes time out against a replayed acknowledge register and\n"
	       "        still emit their three writes, semaphore release included\n");

	printf("  verdict: ");
	if (derived.setup_rc != 0)
		printf("INCONCLUSIVE - bc250_gmc_setup returned %d\n", derived.setup_rc);
	else if (derived.unknown_reads != 0)
		printf("INCONCLUSIVE - the replay had to invent %u read values\n", derived.unknown_reads);
	else if (derived.mismatches != 0)
		printf("NO MATCH - %u of %u writes differ\n", derived.mismatches, derived.compared);
	else if (control.mismatches == 0)
		printf("SUSPECT - the control matched too, so the comparison is not discriminating\n");
	else if (derived.disable_writes != EXPECTED_DISABLE_WRITES)
		printf("EXACT MATCH over %u writes, but gart_disable wrote %u registers, not %u\n",
		       derived.compared, derived.disable_writes, EXPECTED_DISABLE_WRITES);
	else
		printf("EXACT MATCH over %u writes; the control differs in %u, as it should\n",
		       derived.compared, control.mismatches);

	return (derived.setup_rc == 0 && derived.unknown_reads == 0 &&
		derived.mismatches == 0 && control.mismatches != 0 &&
		derived.disable_writes == EXPECTED_DISABLE_WRITES) ? 0 : 1;
}
