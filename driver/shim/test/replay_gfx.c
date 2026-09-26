/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host replay of amdgpu's GFX10 CP/KIQ and SDMA bring-up on unit A (milestone M5 part B, ADR 0002).
 *
 * Runs the code the driver itself will run - driver/shim/bc250_gfx.c, bc250_sdma.c, bc250_nbio.c
 * and bc250_ring.c over the unmodified AMD data in driver/amdgpu-import/ - against a backend that
 * answers reads with what unit A's hardware returned during experiment E03, and compares every
 * register write it produces, in order and by byte offset, with the writes amdgpu made in the same
 * window.
 *
 *   replay_gfx <sweep-run1.log> <sweep-run2.log> <trace-gfx.txt> <trace-irq.txt> [-v] [--dump DIR]
 *
 * The two trace extracts are
 *   python tools/trace/extract_phase.py <evidence>/amdgpu-events.txt \
 *          --match '^(GC|NBIO)\.' --reads --no-fold --precision 6 --since 0.5496 --until 0.5505
 *   python tools/trace/extract_phase.py <evidence>/amdgpu-events.txt \
 *          --match '^(GC|NBIO)\.' --reads --no-fold --precision 6 --since 1.5601 --until 1.5609
 *
 * The second window is a second later, after the interrupt ring and the rest of the IP blocks have
 * come up, and it is compared separately because that is how it happens: stages 1 to 9 are one
 * hw_init, and the interrupt enables are the fence driver and the late init reaching back.
 *
 * Three things are worth knowing before reading the numbers this prints.
 *
 * 1. Reads. The GMC window this test's M4 sibling replays starts before amdgpu touched anything, so
 *    the pre-driver sweep answers every read. This window starts half a second later, after the
 *    GART is up and the PSP has loaded the CP, RLC and SDMA firmware, and the sweep no longer
 *    describes the hardware. So the FIRST read of each register in the window is answered from the
 *    trace's own read record - the value unit A's hardware returned at that moment - and every read
 *    after that is answered from whatever the run itself has written since, exactly as on hardware.
 *    Registers the trace never reads fall back to the sweep, and a read with neither is counted and
 *    reported. Writes in the extract are never used as answers: the run has to produce those.
 *
 *    One consequence is worth naming rather than leaving to be rediscovered. A register that settles
 *    over repeated reads cannot settle here, because after the first read the replay only knows what
 *    the run wrote. CP_STAT is the one this sequence polls: unit A read it four times, 0x8C028000,
 *    0x8C028000, 0x80808000, 0x00000000, the CP going idle. Here it stays at the first value, so
 *    gfx_v10_0_cp_gfx_enable()'s wait always runs out and prints its message (visible with -v).
 *    Upstream returns 0 either way and the transcription does too, so no write moves and no
 *    comparison is affected - but a timeout in this test's log is not evidence of one on hardware.
 *
 * 2. Addresses. The MQDs, ring buffers, EOP buffers, writeback slots and the clear-state buffer are
 *    allocated by this test, not by amdgpu, so the registers that carry their addresses cannot equal
 *    the trace and are listed by name below. For those the offset is still compared, the value is
 *    not, and each one is separately checked against the address this test actually handed out, so
 *    "not compared with the trace" does not mean "not checked".
 *
 * 3. The GRBM CAM. gfx_v10_0_check_grbm_cam_remapping() writes a pattern to the UMD alias of
 *    VGT_ESGS_RING_SIZE and reads it back through the register itself; if it comes back, the CAM
 *    has already been programmed. Unit A answers yes, and the trace shows the aliasing directly:
 *
 *        0.549735  W  GC.VGT_ESGS_RING_SIZE_UMD  0x30900  DEADBEEF
 *        0.549737  R  GC.VGT_ESGS_RING_SIZE      0x088c8  DEADBEEF
 *
 *    A flat array of registers has no such alias, so the probe would answer no, take the branch
 *    that unit A did not take, and stop the sequence. The one aliasing is declared to the backend
 *    below, from those two lines and nothing else.
 *
 * 4. The CP. Eleven ring tests in this window submit a PM4 packet and poll SCRATCH_REG0 for the
 *    value it writes. A replayed register file has no CP to execute that packet, so backend_mem.c
 *    decodes the packet off the ring and applies it. That stub is the one place this test pretends
 *    to be hardware; it is declared in backend_mem.c, it only ever handles PACKET3_SET_UCONFIG_REG,
 *    and the count it fired is printed and checked. One of the control runs turns it off, and that
 *    run has to fail.
 *
 * 5. The interrupt window. The second extract is not all ours, and two runs of writes inside it are
 *    dropped before the comparison, each by a rule that says what it is dropping and fails loudly if
 *    the window stops looking like that:
 *
 *      - the leading GRBM_GFX_CNTL pair, 0x00000008 then 0x00000000. That is a select of me 2,
 *        pipe 0 with no register access between the two halves, so nothing is configured by it and
 *        nothing in this milestone produces it. It is left unexplained rather than imitated.
 *      - the sixteen GCVM_CONTEXTn_CNTL read-modify-writes at 1.5608, which are gmc_v10_0's late
 *        init writing back what it already set. That is M4's code, closed on hardware in commit
 *        1850ca6, and out of this milestone's scope.
 *
 *    One more thing about this window is a stand-in rather than a reproduction: the four
 *    CP_ME1_PIPEn_INT_CNTL registers are read back at 1.5606 with the value amdkfd's per-pipe enable
 *    put there through CPC_INT_CNTL under a GRBM selection, and the backend does not model that
 *    aliasing (see bc250_irq.h for why the trace cannot settle whether it exists). Those reads are
 *    answered from the trace's own read record, like every other first read in a window.
 *
 * 6. The SDMA engines. Since experiment E15 the backend keeps, per SDMA engine, the write pointer
 *    the engine holds: it survives the undo and the next bring-up's writes of 0, and a doorbell at
 *    or below it executes nothing. That is the fifth declared model, it is measured hardware
 *    behaviour rather than a convenience, and backend_mem.h sets out the four things the sweeps
 *    showed. The arm that exercises it is check_sdma_pointers().
 *
 * 7. Stage C. The last arm of this file is not a replay at all and says so in its own banner: ADR
 *    0008's indirect buffer and per-VMID page directory are things amdgpu does per job, so no
 *    bring-up window contains them. They are checked against AMD's packet and register names, and
 *    one submission is run end to end through the CP stub, which follows an IB at VMID 0.
 *
 * Nothing in this file decides what the hardware is told; that is bc250_gfx.c's, bc250_sdma.c's and
 * bc250_irq.c's job. What is here is the inputs of that one amdgpu run, the comparison, and the
 * dumps.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "backend_mem.h"
#include "backend_trace.h"
#include "bc250_gart.h"
#include "bc250_gfx.h"
#include "../generated/rlc_cg_flags.h"
#include "bc250_gmc.h"
#include "bc250_irq.h"
#include "bc250_nbio.h"
#include "bc250_sdma.h"
#include "nv.h"

#include "gc/gc_10_1_0_offset.h"
#include "cyan_skillfish_ip_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"
#include "soc15_common.h"
#include "v10_structs.h"
#include "nvd.h"
#include "soc15_ih_clientid.h"

/* ---------------------------------------------------------------------------------------------
 * Unit A, experiment E03 (evidence/linux/2026-09-21-E03-init-trace), kernel 6.18.52-0-lts.
 * ------------------------------------------------------------------------------------------- */

/* The same three run-specific addresses replay.c uses, because the memory layout has to be learnt
 * from bc250_gmc_setup() before this test can hand out MC addresses of its own. See replay.c for
 * where each comes from. */
#define UNITA_GART_TABLE_MC                     0x000000F5FFE00000ULL
#define UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB   0x002708C9u
#define UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32    0x0007E3C1u

/* The doorbell BAR's physical address, which the self-ring aperture is pointed at. Like the GART
 * table address above it is an input, not something this test allocates: on Windows the miniport
 * takes it out of the translated resource list, and on unit A it is what the trace writes.
 *     1.560720  W  NBIO.BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_BASE_LOW  0x03850  D0000000
 *     1.560721  W  NBIO.BIF_BX_DEV0_EPF0_VF0_DOORBELL_SELFRING_GPA_APER_BASE_HIGH 0x0384c  00000000 */
#define UNITA_DOORBELL_BASE                     0x00000000D0000000ULL

/* evidence/linux/2026-09-21-E03-init-trace/dmesg.txt:1038
 *     amdgpu: SE 2, SH per SE 2, CU per SH 10, active_cu_number 24
 * max_backends_per_se is not on that line and is not observable in the register trace; see
 * struct bc250_gfx_inputs. Two is what every Navi part with this SE/SH layout reports, and the
 * value only scales a mask that ends up in the clear-state PM4 stream, which the trace does not
 * contain. It is stated here so that the test says what it was given. */
/* The fault unit A raised in experiment E12 run 002 when the second bring-up un-halted the MEC:
 * client 27, source 0, src_data[0] = 0x444 - the first run's KIQ base 0x442000 plus the offset its
 * read pointer had reached, an address no register in either run names
 * (P:\BC-250\scratch\e12\run002\out\ih-state-failed-153224.txt, last vector). The client id is
 * SOC15_IH_CLIENTID_UTCL2 from AMD's header; the source id has no name in any header we have, so it
 * is written here as the number that was measured and nowhere else. */
#define UNITA_UTCL2_FAULT_SRCID    0u

#define UNITA_MAX_SHADER_ENGINES   2u
#define UNITA_MAX_SH_PER_SE        2u
#define UNITA_MAX_CU_PER_SH        10u
#define UNITA_MAX_BACKENDS_PER_SE  2u

/* ---------------------------------------------------------------------------------------------
 * The reference trace
 * ------------------------------------------------------------------------------------------- */

#define MAX_TRACE 2048

struct trace_entry {
	u32 byte_offset;
	u32 value;
	char name[64];
};

static struct trace_entry g_trace[MAX_TRACE];		/* the bring-up window, 0.5496 to 0.5505 */
static unsigned int g_trace_count;

static struct trace_entry g_trace_irq[MAX_TRACE];	/* the interrupt window, 1.5601 to 1.5609 */
static unsigned int g_trace_irq_count;
static unsigned int g_trace_irq_dropped;

static int load_trace_writes(const char *path, struct trace_entry *out, unsigned int *count)
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
		if (*count >= MAX_TRACE)
			break;
		out[*count].byte_offset = off;
		out[*count].value = val;
		strncpy(out[*count].name, name, sizeof(out[0].name) - 1);
		out[*count].name[sizeof(out[0].name) - 1] = '\0';
		(*count)++;
	}
	fclose(f);
	return (int)*count;
}

/*
 * Every offset the trace windows name, reads as well as writes, with the name the tracer gave it.
 *
 * This is a different question from the comparison above and needs its own table. The comparison
 * asks "did we write what amdgpu wrote"; this asks "is this register one amdgpu touched at all in
 * the windows the miniport's allow-list is generated from". A register amdgpu only read is on that
 * list, so filtering to writes here would report registers as new that are not.
 */
#define MAX_SEEN 4096

static struct { u32 byte_offset; char name[64]; } g_seen[MAX_SEEN];
static unsigned int g_seen_count;

static int load_trace_offsets(const char *path)
{
	char line[512];
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		double t;
		char kind[8], name[256];
		unsigned int off = 0, val = 0, i;

		if (sscanf(line, "%lf %7s %255s 0x%x %x", &t, kind, name, &off, &val) != 5)
			continue;
		for (i = 0; i < g_seen_count; i++)
			if (g_seen[i].byte_offset == off)
				break;
		if (i < g_seen_count || g_seen_count >= MAX_SEEN)
			continue;
		g_seen[g_seen_count].byte_offset = off;
		strncpy(g_seen[g_seen_count].name, name, sizeof(g_seen[0].name) - 1);
		g_seen[g_seen_count].name[sizeof(g_seen[0].name) - 1] = '\0';
		g_seen_count++;
	}
	fclose(f);
	return (int)g_seen_count;
}

/* The name the trace gave this offset, or NULL if no window names it. */
static const char *seen_name(u32 byte_offset)
{
	unsigned int i;

	for (i = 0; i < g_seen_count; i++)
		if (g_seen[i].byte_offset == byte_offset)
			return g_seen[i].name;
	return NULL;
}

/* The two runs of writes the interrupt window contains that this milestone does not produce, dropped
 * here rather than silently skipped at comparison time. See point 5 of the header comment for what
 * each one is; both are recognised by shape, and a window that no longer has that shape fails.
 *
 * Returns 0, or -1 if the extract did not look the way it is described. */
static int filter_irq_window(void)
{
	unsigned int i, out = 0;

	/* The leading GRBM_GFX_CNTL pair. */
	if (g_trace_irq_count < 2 ||
	    strcmp(g_trace_irq[0].name, "GC.GRBM_GFX_CNTL") != 0 || g_trace_irq[0].value != 0x00000008u ||
	    strcmp(g_trace_irq[1].name, "GC.GRBM_GFX_CNTL") != 0 || g_trace_irq[1].value != 0x00000000u) {
		fprintf(stderr, "the interrupt window does not start with the unexplained"
				" GRBM_GFX_CNTL 8/0 pair this filter was written for\n");
		return -1;
	}

	for (i = 2; i < g_trace_irq_count; i++) {
		/* gmc_v10_0's late init, M4's code. */
		if (strncmp(g_trace_irq[i].name, "GC.GCVM_CONTEXT", 15) == 0)
			continue;
		g_trace_irq[out++] = g_trace_irq[i];
	}

	g_trace_irq_dropped = g_trace_irq_count - out;
	g_trace_irq_count = out;

	if (g_trace_irq_dropped != 2u + 16u) {
		fprintf(stderr, "the interrupt window dropped %u writes, not the 2 + 16 described\n",
			g_trace_irq_dropped);
		return -1;
	}
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The registers whose value carries an address this test chose
 *
 * Listed by the name the trace gives them, the way experiments/E09-gart-enable/compare.py lists
 * its own exceptions. For each one the byte offset is still compared; the value is not, and is
 * instead checked against the allocation it should describe (see check_addresses()).
 * ------------------------------------------------------------------------------------------- */
static const char *const g_address_registers[] = {
	/* the KIQ's HQD, programmed through MMIO because the KIQ has no KIQ to map it */
	"GC.CP_HQD_EOP_BASE_ADDR",		"GC.CP_HQD_EOP_BASE_ADDR_HI",
	"GC.CP_MQD_BASE_ADDR",			"GC.CP_MQD_BASE_ADDR_HI",
	"GC.CP_HQD_PQ_BASE",			"GC.CP_HQD_PQ_BASE_HI",
	"GC.CP_HQD_PQ_RPTR_REPORT_ADDR",	"GC.CP_HQD_PQ_RPTR_REPORT_ADDR_HI",
	"GC.CP_HQD_PQ_WPTR_POLL_ADDR",		"GC.CP_HQD_PQ_WPTR_POLL_ADDR_HI",
	/* the RLC clear-state buffer */
	"GC.RLC_CSIB_ADDR_LO",			"GC.RLC_CSIB_ADDR_HI",
	/* the two SDMA rings and their writeback slots */
	"GC.SDMA0_GFX_RB_BASE",			"GC.SDMA0_GFX_RB_BASE_HI",
	"GC.SDMA0_GFX_RB_RPTR_ADDR_LO",		"GC.SDMA0_GFX_RB_RPTR_ADDR_HI",
	"GC.SDMA0_GFX_RB_WPTR_POLL_ADDR_LO",	"GC.SDMA0_GFX_RB_WPTR_POLL_ADDR_HI",
	"GC.SDMA1_GFX_RB_BASE",			"GC.SDMA1_GFX_RB_BASE_HI",
	"GC.SDMA1_GFX_RB_RPTR_ADDR_LO",		"GC.SDMA1_GFX_RB_RPTR_ADDR_HI",
	"GC.SDMA1_GFX_RB_WPTR_POLL_ADDR_LO",	"GC.SDMA1_GFX_RB_WPTR_POLL_ADDR_HI"
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
 * One run
 * ------------------------------------------------------------------------------------------- */

struct run_opts {
	bool async_gfx_ring;
	bool pp_gfxoff;
	int  cp_stub;
	int  irq_per_ring;      /* the control described at irq_hw_init_per_ring() */
};

/* The control for the one judgement call in bc250_irq_hw_init(): it walks the four compute pipes
 * rather than the eight compute rings, because amdgpu_irq_get() refcounts per interrupt type and two
 * rings share each pipe's type. The naive reading - one enable per ring, as the fence driver's loop
 * literally reads - is written out here, in the test rather than in the shim, so that the four-not-
 * eight claim is something this run demonstrates instead of something the comment asserts. */
static int irq_hw_init_per_ring(struct amdgpu_device *adev)
{
	u32 j;
	int i, r;

	bc250_irq_set_gfx_eop(adev, 0, 0, BC250_IRQ_STATE_ENABLE);

	for (j = 0; j < adev->gfx.num_compute_rings; j++)
		bc250_irq_set_compute_eop(adev, 1, (int)adev->gfx.compute_ring[j].pipe,
					  BC250_IRQ_STATE_ENABLE);

	r = bc250_irq_set_kiq(adev, BC250_IRQ_STATE_ENABLE);
	if (r)
		return r;

	for (i = 0; i < adev->sdma.num_instances; i++) {
		r = bc250_irq_set_sdma_trap(adev, i, BC250_IRQ_STATE_ENABLE);
		if (r)
			return r;
	}
	return 0;
}

struct phase {
	unsigned int produced;
	unsigned int compared;
	unsigned int mismatches;         /* offset differs, or value differs on a non-address register */
	unsigned int address_exceptions; /* offset matched, value not compared */
};

struct result {
	struct phase bringup;            /* stages 1 to 9, against the 0.5496 window */
	struct phase irq;                /* stage 10, against the 1.5601 window */
	unsigned int stage0_bad;         /* nv_common_hw_init's three NBIO writes */
	unsigned int address_failures;   /* an address register that does not describe our allocation */
	unsigned int unknown_reads;
	unsigned int doorbells;
	unsigned int stub_hits;
	unsigned int stub_rejects;
	unsigned int packets;
	int gfx_setup_rc, sdma_setup_rc, gfx_rc, sdma_rc, irq_rc;
};

/* Compare what the run just produced with one trace window, in order, offset for offset and value
 * for value, excepting the registers that carry an address this test chose. */
static void compare_phase(const char *what, const struct trace_entry *tr, unsigned int tn,
			  struct phase *out)
{
	const struct bc250_reg_write *w = backend_writes();
	unsigned int i, n;

	out->produced = backend_write_count();
	n = out->produced < tn ? out->produced : tn;
	out->compared = n;

	for (i = 0; i < n; i++) {
		int addr = is_address_register(tr[i].name);

		if (w[i].byte_offset != tr[i].byte_offset) {
			out->mismatches++;
		} else if (addr) {
			out->address_exceptions++;
			continue;
		} else if (w[i].value == tr[i].value) {
			continue;
		} else {
			out->mismatches++;
		}

		if (out->mismatches <= 40) {
			printf("  %s [%3u] shim 0x%05X = %08X   trace %-38s 0x%05X = %08X",
			       what, i, w[i].byte_offset, w[i].value,
			       tr[i].name, tr[i].byte_offset, tr[i].value);
			if (w[i].byte_offset == tr[i].byte_offset)
				printf("   differing bits %08X", w[i].value ^ tr[i].value);
			printf("\n");
		}
	}
	if (out->mismatches > 40)
		printf("  %s ... %u further mismatches not listed\n", what, out->mismatches - 40);

	if (out->produced != tn)
		printf("  %s produced %u writes, the window has %u\n", what, out->produced, tn);
	else if (out->mismatches == 0 && n > 0)
		printf("  %s: all %u writes identical, %u of them address exceptions\n",
		       what, n, out->address_exceptions);
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

/* Check one address-carrying register against the allocation it is meant to describe. `shift` is
 * how the hardware stores the address (8 for the registers that hold MC >> 8), `mask` what the
 * upstream code ANDs the low half with. */
static unsigned int check_one_address(const char *label, struct amdgpu_device *adev,
				      u32 reg_lo, u32 reg_hi, u64 expect_mc,
				      unsigned int shift, u32 mask_lo, int verbose)
{
	int flo = 0, fhi = 0;
	u32 lo = shim_wrote(reg_lo, &flo);
	u32 hi = shim_wrote(reg_hi, &fhi);
	u64 stored = expect_mc >> shift;
	u32 want_lo = (u32)(stored & 0xffffffffULL) & mask_lo;
	u32 want_hi = (u32)(stored >> 32);

	(void)adev;
	if (!flo || !fhi) {
		printf("    %-34s NOT WRITTEN\n", label);
		return 1;
	}
	if (lo != want_lo || hi != want_hi) {
		printf("    %-34s shim %08X:%08X, our allocation says %08X:%08X   <-- wrong\n",
		       label, hi, lo, want_hi, want_lo);
		return 1;
	}
	if (verbose)
		printf("    %-34s %08X:%08X  = MC 0x%llX\n", label, hi, lo,
		       (unsigned long long)expect_mc);
	return 0;
}

static unsigned int check_addresses(struct amdgpu_device *adev, int verbose)
{
	struct amdgpu_ring *kiq = &adev->gfx.kiq[0].ring;
	unsigned int bad = 0;
	int i;

	printf("  address-carrying registers, checked against this test's own allocations:\n");

	/* CP_HQD_* and CP_MQD_* are written once, by the KIQ, while GRBM_GFX_CNTL selects it. */
	bad += check_one_address("CP_HQD_EOP_BASE_ADDR", adev,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_EOP_BASE_ADDR) * 4u,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_EOP_BASE_ADDR_HI) * 4u,
				 kiq->eop_gpu_addr, 8, 0xffffffffu, verbose);
	bad += check_one_address("CP_MQD_BASE_ADDR", adev,
				 SOC15_REG_OFFSET(GC, 0, mmCP_MQD_BASE_ADDR) * 4u,
				 SOC15_REG_OFFSET(GC, 0, mmCP_MQD_BASE_ADDR_HI) * 4u,
				 kiq->mqd_gpu_addr, 0, 0xfffffffcu, verbose);
	bad += check_one_address("CP_HQD_PQ_BASE", adev,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_BASE) * 4u,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_BASE_HI) * 4u,
				 kiq->gpu_addr, 8, 0xffffffffu, verbose);
	bad += check_one_address("CP_HQD_PQ_RPTR_REPORT_ADDR", adev,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR_REPORT_ADDR) * 4u,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR_REPORT_ADDR_HI) * 4u,
				 kiq->rptr_gpu_addr, 0, 0xfffffffcu, verbose);
	bad += check_one_address("CP_HQD_PQ_WPTR_POLL_ADDR", adev,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_WPTR_POLL_ADDR) * 4u,
				 SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_WPTR_POLL_ADDR_HI) * 4u,
				 kiq->wptr_gpu_addr, 0, 0xfffffffcu, verbose);
	bad += check_one_address("RLC_CSIB_ADDR", adev,
				 SOC15_REG_OFFSET(GC, 0, mmRLC_CSIB_ADDR_LO) * 4u,
				 SOC15_REG_OFFSET(GC, 0, mmRLC_CSIB_ADDR_HI) * 4u,
				 adev->gfx.rlc.clear_state_gpu_addr, 0, 0xffffffffu, verbose);

	for (i = 0; i < adev->sdma.num_instances; i++) {
		struct amdgpu_ring *r = &adev->sdma.instance[i].ring;
		char label[64];

		sprintf(label, "SDMA%d_GFX_RB_BASE", i);
		bad += check_one_address(label, adev,
					 bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_BASE) * 4u,
					 bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_BASE_HI) * 4u,
					 r->gpu_addr, 8, 0xffffffffu, verbose);
		sprintf(label, "SDMA%d_GFX_RB_RPTR_ADDR", i);
		bad += check_one_address(label, adev,
					 bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR_ADDR_LO) * 4u,
					 bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR_ADDR_HI) * 4u,
					 r->rptr_gpu_addr, 0, 0xfffffffcu, verbose);
		sprintf(label, "SDMA%d_GFX_RB_WPTR_POLL_ADDR", i);
		bad += check_one_address(label, adev,
					 bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_POLL_ADDR_LO) * 4u,
					 bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_POLL_ADDR_HI) * 4u,
					 r->wptr_gpu_addr, 0, 0xffffffffu, verbose);
	}

	printf("    %u of %u wrong\n", bad, 12u);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * The same bring-up from the state Windows actually leaves behind
 *
 * Everything above answers reads from unit A under Linux. But the driver will run on unit A under
 * Windows, and experiment E10 (facts M34, M35) showed the starting state is not the same: the PSP
 * does not just load firmware, it starts the RLC and releases the SDMA halt while doing so. After
 * the ten LOAD_IP_FW and with no other write, SDMA0/1_F32_CNTL are already 0, RLC_CNTL is already 1
 * with a running RLC, and 31 GC registers already read their Linux after-init values. The CP is
 * still halted.
 *
 * So this pass answers the reads from evidence/windows/2026-09-21-E10-run-001/sweep-GC-loaded-*.log
 * instead, runs the identical code, and reports every write that comes out different. Any
 * read-modify-write whose result depends on the starting state shows up here - which is the point,
 * because the alternative is finding it on the bench.
 *
 * A difference here is NOT a failure. The trace is Linux's; this pass asks what changes when the
 * same code starts somewhere else, and the answer is information for the hardware run. The test's
 * verdict does not depend on it.
 *
 * One class of register cannot be compared this way at all, and is excluded by name rather than
 * reported as a difference. CP_HQD_* and CP_MQD_* are GRBM-windowed: which physical queue slot they
 * address depends on the GRBM_GFX_CNTL selection in force when they are read. The E03 trace reads
 * them from inside the KIQ's own selection, me 2 pipe 1 queue 0. The Windows sweep walks the
 * register space with no selection at all - it contains no GRBM_GFX_CNTL write - so it read the
 * default slot and reported 0. Overlaying those zeros would produce three confident-looking
 * differences (CP_MQD_CONTROL, CP_HQD_PQ_CONTROL, CP_HQD_PERSISTENT_STATE) that say nothing about
 * unit A's state under Windows and would send someone to the bench after a ghost.
 *
 * What would settle it: a sweep that selects me 2 pipe 1 queue 0 through GRBM_GFX_CNTL before
 * reading the HQD block, and 0 afterwards. Until then this pass has nothing to say about them.
 * ------------------------------------------------------------------------------------------- */
static void register_rings(struct amdgpu_device *adev);

/* GRBM-windowed, so a sweep taken without a selection cannot describe them. See above. */
static int is_grbm_windowed(const char *name)
{
	return strncmp(name, "GC.CP_HQD_", 10) == 0 || strncmp(name, "GC.CP_MQD_", 10) == 0;
}

/* The single declared hardware reaction, in one place because every run has to declare the same one.
 * backend_trace.h says what it models; the condition says when the modelled hardware can act.
 *
 * All five offsets and masks come from AMD's headers over adev->reg_offset, not from the trace. */
static void declare_dequeue_reaction(struct amdgpu_device *adev)
{
	backend_add_reaction(SOC15_REG_OFFSET(GC, 0, mmCP_HQD_DEQUEUE_REQUEST) * 4u, 1u,
			     SOC15_REG_OFFSET(GC, 0, mmCP_MEC_CNTL) * 4u,
			     CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK, 0u,
			     SOC15_REG_OFFSET(GC, 0, mmCP_HQD_ACTIVE) * 4u, 0u);
}

/* The fourth declared model: the MEC's own copy of a queue's base and read pointer, which survives
 * a halt and which no register write reaches while the engine is stopped. backend_mem.h sets out
 * the four things that were measured on unit A in E12 run 002 and the one that was not.
 *
 * As with the reaction above, every offset and mask is resolved through AMD's headers. */
static void declare_mec_fetch_state(struct amdgpu_device *adev)
{
	struct backend_mec_regs r;

	memset(&r, 0, sizeof(r));
	r.mec_cntl            = SOC15_REG_OFFSET(GC, 0, mmCP_MEC_CNTL) * 4u;
	r.mec_halt_mask       = CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK;
	r.grbm_gfx_cntl       = SOC15_REG_OFFSET(GC, 0, mmGRBM_GFX_CNTL) * 4u;
	r.hqd_active          = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_ACTIVE) * 4u;
	r.hqd_pq_base         = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_BASE) * 4u;
	r.hqd_pq_base_hi      = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_BASE_HI) * 4u;
	r.hqd_pq_rptr         = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR) * 4u;
	r.hqd_dequeue_request = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_DEQUEUE_REQUEST) * 4u;
	r.fault_client_id     = SOC15_IH_CLIENTID_UTCL2;
	r.fault_src_id        = UNITA_UTCL2_FAULT_SRCID;
	backend_add_mec_fetch_state(&r);
}

static void check_windows_state(const char *sweep, struct amdgpu_device *adev, int verbose)
{
	struct amdgpu_bo gart_bo;
	struct bc250_gmc_inputs gin;
	struct bc250_gfx_inputs fin;
	const struct bc250_reg_write *w;
	unsigned int i, n, differing = 0, extra = 0, windowed = 0;
	int loaded, r;

	printf("\n== the same bring-up, but reads answered from unit A under Windows ==\n");

	loaded = backend_load_sweep_replace(sweep);
	if (loaded < 0) {
		printf("  cannot read %s\n", sweep);
		return;
	}
	printf("  %d registers overlaid from %s\n", loaded, sweep);

	memset(adev, 0, sizeof(*adev));
	memset(&gart_bo, 0, sizeof(gart_bo));
	memset(&gin, 0, sizeof(gin));
	memset(&fin, 0, sizeof(fin));
	adev->dev = (void *)"BC250-A-windows";
	adev->doorbell.base = UNITA_DOORBELL_BASE;

	backend_reset_state();
	backend_reset_writes();
	backend_mem_reset();
	backend_cp_stub_enable(1);

	gin.gart_table_mc = UNITA_GART_TABLE_MC;
	gin.dummy_page_dma = (u64)UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32 << 12;
	gin.noretry = true;
	if (bc250_gmc_setup(adev, &gin, &gart_bo) != 0) {
		printf("  bc250_gmc_setup failed against the Windows state\n");
		return;
	}
	gin.mem_scratch_mc = ((u64)UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB << 12)
			     - adev->vm_manager.vram_base_offset + adev->gmc.vram_start;
	(void)bc250_gmc_setup(adev, &gin, &gart_bo);
	backend_mem_set_bases(adev->gmc.vram_start, adev->gmc.gart_start);
	backend_add_alias(SOC15_REG_OFFSET(GC, 0, mmVGT_ESGS_RING_SIZE_UMD) * 4u,
			  SOC15_REG_OFFSET(GC, 0, mmVGT_ESGS_RING_SIZE) * 4u);
	backend_cp_stub_expect(SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0));
	declare_dequeue_reaction(adev);

	fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
	fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
	fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
	fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
	fin.async_gfx_ring = true;
	fin.pp_gfxoff = true;

	if (bc250_gfx_setup(adev, &fin) != 0 || bc250_sdma_setup(adev) != 0) {
		printf("  setup failed against the Windows state\n");
		return;
	}
	register_rings(adev);

	backend_reset_writes();
	r = bc250_gfx_hw_init(adev);
	if (r == 0)
		r = bc250_sdma_hw_init(adev);

	w = backend_writes();
	n = backend_write_count();
	printf("  bring-up returned %d, %u writes (against the trace's %u)\n", r, n, g_trace_count);

	for (i = 0; i < n; i++) {
		if (i >= g_trace_count) {
			extra++;
			continue;
		}
		if (w[i].byte_offset != g_trace[i].byte_offset) {
			/* Once the two sequences diverge in position, comparing further entries
			 * pairwise says nothing. Report and stop. */
			printf("    from write %u the sequences no longer line up: shim 0x%05X,"
			       " trace %s 0x%05X\n",
			       i, w[i].byte_offset, g_trace[i].name, g_trace[i].byte_offset);
			differing++;
			break;
		}
		if (w[i].value == g_trace[i].value || is_address_register(g_trace[i].name))
			continue;
		if (is_grbm_windowed(g_trace[i].name)) {
			windowed++;
			continue;
		}
		differing++;
		if (differing <= 20 || verbose)
			printf("    %-40s 0x%05X   Linux %08X   Windows %08X   differ %08X\n",
			       g_trace[i].name, w[i].byte_offset, g_trace[i].value, w[i].value,
			       w[i].value ^ g_trace[i].value);
	}
	if (differing > 20 && !verbose)
		printf("    ... %u further differences, -v to see them all\n", differing - 20);
	if (extra)
		printf("    and %u writes past the end of the trace window\n", extra);

	printf("  %u writes differ from the Linux trace when the run starts from the Windows state\n",
	       differing);
	printf("  %u GRBM-windowed writes not compared: the sweep took no selection, see the comment\n",
	       windowed);
	printf("  (this is information for the hardware run, not a failure)\n");
}

/* ---------------------------------------------------------------------------------------------
 * What the teardown touches that no trace window does
 *
 * The miniport passes register accesses through a generated allow-list, and that list is generated
 * from the windows of the E03 trace where amdgpu did this work: 0.0375-0.0385, 0.5496-0.551 and
 * 1.560-1.562. Anything outside it faults the sequence.
 *
 * The teardown has no window. Unit A was never torn down while the tracer ran, so there is no
 * recording of it to generate from and no way to know what it touches except to run it and look.
 * That is what this does: it records every offset bc250_sdma_hw_fini() and bc250_gfx_hw_fini() read
 * or write, and reports the ones no window names.
 *
 * Neither function touches NBIO, so the 0.0375-0.0385 window - which run_gfx.ps1 does not extract,
 * because stage 0 is checked against its three quoted writes instead - cannot change the answer.
 * ------------------------------------------------------------------------------------------- */
struct named_reg { const char *name; u32 byte_offset; };

static unsigned int survey_fini_registers(const char *what, struct amdgpu_device *adev, int verbose)
{
	struct named_reg known[32];
	unsigned int nknown = 0, i, k, n, uncovered = 0, unnamed = 0;

#define NAME_GC(reg) do { \
	known[nknown].name = "GC." #reg; \
	known[nknown].byte_offset = SOC15_REG_OFFSET(GC, 0, mm##reg) * 4u; \
	nknown++; \
} while (0)

	/* Everything the two fini paths can reach, by AMD's header name. An offset that turns up in
	 * the survey and is not here is printed bare and counted: the table has to keep up with the
	 * code, and this is what makes it. */
	NAME_GC(CP_INT_CNTL_RING0);
	NAME_GC(CP_ME1_PIPE0_INT_CNTL);
	NAME_GC(CP_ME1_PIPE1_INT_CNTL);
	NAME_GC(CP_ME1_PIPE2_INT_CNTL);
	NAME_GC(CP_ME1_PIPE3_INT_CNTL);
	NAME_GC(SCRATCH_REG0);
	NAME_GC(CP_MEC_CNTL);
	NAME_GC(CP_ME_CNTL);
	NAME_GC(CP_STAT);
	NAME_GC(RLC_CNTL);
	NAME_GC(CP_HQD_ACTIVE);
	NAME_GC(CP_HQD_DEQUEUE_REQUEST);
	NAME_GC(GRBM_GFX_CNTL);
#undef NAME_GC

	/* Four the halt sequence writes, and the four pointer registers the fini reads afterwards to
	 * decide whether the engine drained (bc250_sdma_hw_fini()). The reads are the same offsets the
	 * bring-up writes, so a trace window already names them; they are listed anyway, because this
	 * table is what the fini path is supposed to be able to account for. */
	for (i = 0; i < (unsigned int)adev->sdma.num_instances && nknown + 8 <= ARRAY_SIZE(known); i++) {
		static const char *nm[2][8] = {
			{ "GC.SDMA0_CNTL", "GC.SDMA0_GFX_RB_CNTL", "GC.SDMA0_GFX_IB_CNTL",
			  "GC.SDMA0_F32_CNTL", "GC.SDMA0_GFX_RB_RPTR", "GC.SDMA0_GFX_RB_RPTR_HI",
			  "GC.SDMA0_GFX_RB_WPTR", "GC.SDMA0_GFX_RB_WPTR_HI" },
			{ "GC.SDMA1_CNTL", "GC.SDMA1_GFX_RB_CNTL", "GC.SDMA1_GFX_IB_CNTL",
			  "GC.SDMA1_F32_CNTL", "GC.SDMA1_GFX_RB_RPTR", "GC.SDMA1_GFX_RB_RPTR_HI",
			  "GC.SDMA1_GFX_RB_WPTR", "GC.SDMA1_GFX_RB_WPTR_HI" }
		};
		u32 internal[8];

		internal[0] = mmSDMA0_CNTL;
		internal[1] = mmSDMA0_GFX_RB_CNTL;
		internal[2] = mmSDMA0_GFX_IB_CNTL;
		internal[3] = mmSDMA0_F32_CNTL;
		internal[4] = mmSDMA0_GFX_RB_RPTR;
		internal[5] = mmSDMA0_GFX_RB_RPTR_HI;
		internal[6] = mmSDMA0_GFX_RB_WPTR;
		internal[7] = mmSDMA0_GFX_RB_WPTR_HI;

		if (i > 1)
			break;
		for (k = 0; k < 8; k++) {
			known[nknown].name = nm[i][k];
			known[nknown].byte_offset =
				bc250_sdma_reg_offset(adev, i, internal[k]) * 4u;
			nknown++;
		}
	}

	printf("\n== registers %s touches that no trace window names ==\n", what);

	n = backend_touched_count();
	if (backend_touched_overflow() != 0) {
		printf("  the survey table overflowed by %u offsets; raise MAX_TOUCHED   <-- wrong\n",
		       backend_touched_overflow());
		return 1;
	}

	for (i = 0; i < n; i++) {
		u32 off = backend_touched_offset(i);
		const char *tn = seen_name(off);
		const char *mine = NULL;

		for (k = 0; k < nknown; k++)
			if (known[k].byte_offset == off)
				mine = known[k].name;

		if (tn != NULL) {
			if (verbose)
				printf("    covered   %-34s 0x%05X %s\n",
				       tn, off, backend_touched_written(i) ? "R/W" : "R");
			continue;
		}
		uncovered++;
		if (mine == NULL)
			unnamed++;
		printf("    NEW       %-34s 0x%05X %s\n",
		       mine != NULL ? mine : "(no name in the table)", off,
		       backend_touched_written(i) ? "R/W" : "R");
	}

	printf("  %u offsets touched, %u of them named by a trace window, %u new\n",
	       n, n - uncovered, uncovered);
	if (unnamed != 0)
		printf("  %u of the new ones this test cannot name   <-- extend the table above\n",
		       unnamed);
	return unnamed;
}

/* ---------------------------------------------------------------------------------------------
 * The undo, and whether the bring-up survives it
 *
 * On Windows the sequence has to be re-runnable without reloading firmware: a second PSP load in one
 * boot leaves the RLC disabled and busy (facts M35, experiment E10), so the kmd gets exactly one
 * load per boot and everything after it must be undoable with registers and the KIQ alone.
 *
 * This runs the teardown and then the whole bring-up a second time on the same adev, with the
 * register state the teardown left behind. What is required is that the second run succeeds - every
 * ring test included, which means the CP really did come back. The second run is NOT required to
 * emit the same writes as the first: several are read-modify-write, and it starts from a different
 * state by construction, so a difference there is information rather than a fault. The count is
 * printed for that reason.
 * ------------------------------------------------------------------------------------------- */
/* The engines have to read halted after the undo. Every expected bit comes from AMD's own field
 * masks, so the check is on the field the transcription set and not on a value copied from a log. */
static unsigned int check_halted(struct amdgpu_device *adev)
{
	const u32 mec_halt = CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK;
	const u32 me_halt = CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK |
			    CP_ME_CNTL__CE_HALT_MASK;
	u32 me, mec, rlc, f32[2];
	unsigned int i, bad = 0;

	me = bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_ME_CNTL));
	mec = bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_MEC_CNTL));
	rlc = bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmRLC_CNTL));
	for (i = 0; i < 2; i++)
		f32[i] = bc250_shim_rreg(adev, bc250_sdma_reg_offset(adev, i, mmSDMA0_F32_CNTL));

	printf("  CP_ME_CNTL %08X, CP_MEC_CNTL %08X, RLC_CNTL %08X, SDMA0/1_F32_CNTL %08X %08X\n",
	       me, mec, rlc, f32[0], f32[1]);

	if ((me & me_halt) != me_halt) {
		printf("    CP_ME_CNTL: ME/PFP/CE not all halted   <-- wrong\n");
		bad++;
	}
	if ((mec & mec_halt) != mec_halt) {
		printf("    CP_MEC_CNTL: MEC_ME1/ME2 not both halted   <-- wrong\n");
		bad++;
	}
	for (i = 0; i < 2; i++) {
		if ((f32[i] & SDMA0_F32_CNTL__HALT_MASK) == 0) {
			printf("    SDMA%u_F32_CNTL: HALT clear   <-- wrong\n", i);
			bad++;
		}
	}
	if ((rlc & RLC_CNTL__RLC_ENABLE_F32_MASK) != 0) {
		printf("    RLC_CNTL: RLC_ENABLE_F32 still set   <-- wrong\n");
		bad++;
	}
	return bad;
}

static unsigned int check_rerun(struct amdgpu_device *adev, unsigned int first_writes, int verbose)
{
	unsigned int hits_before, writes, bad = 0;
	u64 kiq_before;
	int r, r_sdma;

	printf("\n== teardown, then the whole bring-up again ==\n");

	hits_before = backend_cp_stub_count();
	kiq_before = adev->gfx.kiq[0].ring.gpu_addr;
	printf("  before: %u doorbells, %u packets decoded, KIQ wptr %llu\n",
	       backend_doorbell_count(), backend_packet_count(),
	       (unsigned long long)adev->gfx.kiq[0].ring.wptr);

	backend_touched_start();
	r_sdma = bc250_sdma_hw_fini(adev);
	{
		const u32 rlc = SOC15_REG_OFFSET(GC, 0, mmRLC_CNTL);
		u32 before = RREG32(rlc);
		void *csb = adev->gfx.rlc.cs_ptr;
		u64 csb_mc = adev->gfx.rlc.clear_state_mem.mc;
		r = bc250_gfx_hw_fini_keep_rlc(adev);
		if (!(before & RLC_CNTL__RLC_ENABLE_F32_MASK) || RREG32(rlc) != before ||
		    csb == NULL || adev->gfx.rlc.cs_ptr != csb || adev->gfx.rlc.clear_state_mem.mc != csb_mc)
			bad++;
		/* Existing halt checker also requires RLC off, so use it below
		 * after the explicit final phase. Verify CP/SDMA here independently. */
		if ((RREG32_SOC15(GC,0,mmCP_ME_CNTL) & (CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK)) !=
		    (CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK)) bad++;
		if ((RREG32_SOC15(GC,0,mmCP_MEC_CNTL) & (CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK)) !=
		    (CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK)) bad++;
		if (!(RREG32_SOC15(GC,0,mmSDMA0_F32_CNTL) & SDMA0_F32_CNTL__HALT_MASK) ||
		    !(RREG32_SOC15(GC,0,mmSDMA1_F32_CNTL) & SDMA1_F32_CNTL__HALT_MASK)) bad++;
		bc250_gfx_rlc_stop(adev);
		if (RREG32(rlc) & RLC_CNTL__RLC_ENABLE_F32_MASK) bad++;
		printf("  split CP/RLC stop: %u failures before storage destruction\n",bad);
	}
	backend_touched_stop();

	/* Nothing has submitted on the SDMA rings in this arm, so both engines are where the bring-up
	 * left them and the drain check has nothing to complain about. A failure here would mean the
	 * fini's own reads are not answering. */
	if (r_sdma != 0) {
		printf("  bc250_sdma_hw_fini returned %d with no SDMA submission in this arm   <--"
		       " wrong\n", r_sdma);
		bad++;
	}
	printf("  after the undo: %u doorbells, %u packets, KIQ wptr %llu\n",
	       backend_doorbell_count(), backend_packet_count(),
	       (unsigned long long)adev->gfx.kiq[0].ring.wptr);

	/* The undo reports its first failure now. On a replay every step of it should succeed, and
	 * the one that can fail quietly on hardware is the KIQ dequeue: BC250_ETIME here would say
	 * the MEC did not answer the handshake, which is precisely what the arm below then measures
	 * the consequences of. */
	if (r != 0) {
		printf("  bc250_gfx_hw_fini returned %d   <-- wrong: every step of the undo should"
		       " succeed here\n", r);
		bad++;
	}
	bad += check_halted(adev);
	bad += survey_fini_registers("the teardown", adev, verbose);

	/*
	 * Give the memory back and allocate it again, which is what the miniport does: its FINI
	 * escape frees once the engines read halted, and the next RUN sets up from scratch
	 * (driver/kmd/gfx.c, TearDown() and SetUp()).
	 *
	 * This is not decoration. gpumem does not reuse a freed GART range, so the second bring-up's
	 * rings land at addresses the first run never used - unit A's KIQ moved from 0x442000 to
	 * 0x564000 between the two runs of E12 run 002 - and that is the whole reason the stale MEC
	 * fetch showed itself as a page fault rather than as a long walk through someone else's NOPs.
	 * Until this test did the same, it re-ran the stages on the first run's buffers and could not
	 * see the failure at all.
	 */
	bc250_sdma_teardown(adev);
	bc250_gfx_teardown(adev);
	{
		struct bc250_gfx_inputs fin;

		memset(&fin, 0, sizeof(fin));
		fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
		fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
		fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
		fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
		fin.async_gfx_ring = true;
		fin.pp_gfxoff = true;
		if (bc250_gfx_setup(adev, &fin) != 0 || bc250_sdma_setup(adev) != 0) {
			printf("  the second setup failed; nothing to re-run\n");
			return bad + 1;
		}
		register_rings(adev);
		printf("  the rings were freed and allocated again: the KIQ moved from 0x%llX"
		       " to 0x%llX\n", (unsigned long long)kiq_before,
		       (unsigned long long)adev->gfx.kiq[0].ring.gpu_addr);
	}

	printf("\n== the second bring-up ==\n");

	/* The stages one at a time rather than bc250_gfx_hw_init(), so that a failure names itself. */
	backend_reset_writes();
	backend_touched_start();
	{
		static const struct {
			const char *name;
			int (*fn)(struct amdgpu_device *);
		} stage[] = {
			{ "init_golden_registers", bc250_gfx_init_golden_registers },
			{ "constants_init",        bc250_gfx_constants_init },
			{ "rlc_resume",            bc250_gfx_rlc_resume },
			{ "cp_resume",             bc250_gfx_cp_resume },
			{ "sdma_hw_init",          bc250_sdma_hw_init }
		};
		bool remapped = false;
		unsigned int s;

		unsigned int before = 0;

		r = bc250_gfx_grbm_cam_probe(adev, &remapped);
		if (r)
			printf("  grbm_cam_probe returned %d\n", r);
		if (verbose)
			printf("    grbm_cam_probe        %3u writes\n", backend_write_count());
		before = backend_write_count();
		for (s = 0; r == 0 && s < ARRAY_SIZE(stage); s++) {
			r = stage[s].fn(adev);
			if (r)
				printf("  %s returned %d   <-- this is where it stops\n",
				       stage[s].name, r);
			if (verbose) {
				printf("    %-21s %3u writes\n", stage[s].name,
				       backend_write_count() - before);
				before = backend_write_count();
			}
		}
	}
	backend_touched_stop();
	writes = backend_write_count();
	printf("  after the re-run: %u doorbells, %u packets, KIQ wptr %llu, stub hits %u\n",
	       backend_doorbell_count(), backend_packet_count(),
	       (unsigned long long)adev->gfx.kiq[0].ring.wptr, backend_cp_stub_count());

	if (r != 0) {
		printf("  the second bring-up returned %d   <-- wrong\n", r);
		bad++;
	} else {
		printf("  the second bring-up succeeded, %u writes (the first made %u)\n",
		       writes, first_writes);
	}

	/*
	 * Did the teardown leave a live KIQ fetcher behind?
	 *
	 * This is what experiment E12 run 002 found on unit A and what the MEC fetch-state model
	 * reproduces here. The first bring-up activated the KIQ and the engine took its own copy of
	 * the ring's base and read pointer. The teardown halted the MEC without ever telling the
	 * engine to let the queue go, so the copy survived; the second bring-up allocated its rings
	 * at fresh addresses, wrote the new base into CP_HQD_PQ_BASE while the engine was halted -
	 * where it does not reach it - and un-halted. The engine resumed at the OLD base plus the old
	 * read pointer, which is no longer mapped, and UTCL2 raised a fault. The queue never ran
	 * again and the KIQ ring test timed out after 165 ms.
	 *
	 * The test is therefore not about a register value at all. It is: after a teardown and a
	 * second bring-up, no stale fetch happened.
	 */
	if (backend_mec_faults() != 0) {
		printf("  the MEC resumed fetching at 0x%llX after the re-init, which is in no"
		       " allocation   <-- the teardown left the KIQ's fetcher live\n",
		       (unsigned long long)backend_mec_fault_address());
		bad++;
	} else {
		printf("  no stale KIQ fetch survived the teardown            : yes\n");
	}

	/*
	 * And the other nine tenths of the same question: what read pointer did the second bring-up's
	 * MQDs sample?
	 *
	 * bc250_compute_mqd_init() takes CP_HQD_PQ_RPTR as upstream does, under each queue's own
	 * selection, so each of the eight compute MQDs is built from whatever that queue's register
	 * holds. After a bring-up that ran the ring tests the queues idle at rptr == wptr != 0 (fact
	 * M40), which the CP stub now reproduces per queue, and UNMAP_QUEUES is not known to clear
	 * them. A non-zero sample here would tell a queue whose write pointer restarts at 0 that most
	 * of the ring is pending - the 5222 us walk of E12 run 001, nine times over. The fix is
	 * bc250_kcq_clear_pointers() in the teardown; this is what notices if it is not there.
	 */
	{
		unsigned int i, stale = 0;

		for (i = 0; i < adev->gfx.num_compute_rings; i++) {
			const struct v10_compute_mqd *mqd =
				(const struct v10_compute_mqd *)adev->gfx.compute_ring[i].mqd_ptr;

			if (mqd == NULL || mqd->cp_hqd_pq_rptr == 0)
				continue;
			printf("    compute ring %u (me %u pipe %u queue %u) sampled rptr %u\n", i,
			       adev->gfx.compute_ring[i].me, adev->gfx.compute_ring[i].pipe,
			       adev->gfx.compute_ring[i].queue, mqd->cp_hqd_pq_rptr);
			stale++;
		}
		if (stale != 0) {
			printf("  %u of the eight compute MQDs sampled a read pointer the first run"
			       " left behind   <-- the teardown did not put them back\n", stale);
			bad++;
		} else {
			printf("  all nine queues sampled a read pointer of 0        : yes\n");
		}
	}

	/* The teardown's own ring test plus eleven more from the second bring-up. If the CP had not
	 * come back, these would have timed out and r would already be non-zero; counting them says
	 * which of the two it was. */
	printf("  ring tests passed across the undo and the re-run: %u\n",
	       backend_cp_stub_count() - hits_before);
	if (backend_cp_stub_rejects() != 0) {
		printf("  %u malformed ring-test packets   <-- wrong\n", backend_cp_stub_rejects());
		bad++;
	}

	/* Where the second run's write stream differs in SHAPE from the first's, per register. The two
	 * are not required to be identical - several writes are read-modify-write and the second run
	 * starts from a different state - but every difference should be one this file can name, so it
	 * is listed rather than left as a bare count. */
	{
		const struct bc250_reg_write *w = backend_writes();
		unsigned int n = backend_write_count(), i, k, shown = 0;

		printf("  where the two runs differ in how often they write a register:\n");
		/* Both directions. Walking only the second run's offsets would hide a register the
		 * first run wrote and the second does not, which is the more interesting case. */
		for (i = 0; i < n + g_trace_count; i++) {
			unsigned int mine = 0, theirs = 0, seen_before = 0;
			u32 off = i < n ? w[i].byte_offset : g_trace[i - n].byte_offset;

			for (k = 0; k < i; k++)
				if ((k < n ? w[k].byte_offset : g_trace[k - n].byte_offset) == off)
					seen_before = 1;
			if (seen_before)
				continue;
			for (k = 0; k < n; k++)
				if (w[k].byte_offset == off)
					mine++;
			for (k = 0; k < g_trace_count; k++)
				if (g_trace[k].byte_offset == off)
					theirs++;
			if (mine == theirs)
				continue;
			printf("    %-34s 0x%05X   first %u, second %u\n",
			       seen_name(off) ? seen_name(off) : "(unnamed)", off, theirs, mine);
			shown++;
		}
		if (shown == 0)
			printf("    none - the two runs write the same registers the same number of times\n");
		{
			unsigned int distinct = 0, sum_mine = 0, sum_theirs = 0;

			for (i = 0; i < n + g_trace_count; i++) {
				unsigned int seen_before = 0;
				u32 off = i < n ? w[i].byte_offset : g_trace[i - n].byte_offset;

				for (k = 0; k < i; k++)
					if ((k < n ? w[k].byte_offset
						   : g_trace[k - n].byte_offset) == off)
						seen_before = 1;
				if (seen_before)
					continue;
				distinct++;
				for (k = 0; k < n; k++)
					if (w[k].byte_offset == off)
						sum_mine++;
				for (k = 0; k < g_trace_count; k++)
					if (g_trace[k].byte_offset == off)
						sum_theirs++;
			}
			printf("    (%u distinct offsets; they account for %u of the second run's %u"
			       " writes and %u of the first's %u)\n",
			       distinct, sum_mine, n, sum_theirs, g_trace_count);
		}
	}

	bad += survey_fini_registers("the second bring-up", adev, verbose);
	if (r != 0)
		printf("  incomplete: the run above stopped early, so a second bring-up that got"
		       " further could touch more\n");

	printf("  %u wrong\n", bad);
	return bad;
}

/*
 * The read pointer unit A's KIQ held when a second bring-up sampled it, and what it cost.
 *
 * evidence, E12 run 001: CP_HQD_PQ_RPTR read 0x737 = 1847 on the second bring-up in one boot,
 * gfx_v10_0_kiq_init_register()'s active branch wrote it back, and the queue was told 262144 - 1847
 * = 260297 dwords were pending against a ring whose write pointer restarts at 0. Stage 6 took
 * 5222 us against 349 us cold. Used below only to give the recovery branch a stale value to correct;
 * on hardware the CP puts it there itself.
 */
#define UNITA_STALE_KIQ_RPTR 0x737u

/*
 * A compute queue that does not answer the unmap.
 *
 * bc250_gfx_unmap_queues() failing is not fatal to the teardown - it is logged and the undo carries
 * on, because a half-undone GPU is worse than a fully undone one. That leaves
 * bc250_kcq_clear_pointers() in front of a queue the CP may still be fetching from, and zeroing the
 * read pointer of a running queue is the one thing worse than leaving a stale one: it tells the CP
 * that the whole ring is pending. So the clear reads CP_HQD_ACTIVE first and skips a queue that
 * still says 1 (bc250_gfx.c:2117-2123).
 *
 * The arm reproduces it with backend_hqd_pin(): one queue keeps its active bit through the
 * UNMAP_QUEUES that names it, everything else runs exactly as it did in the re-run above. What has
 * to happen is a per-queue statement, which is why the backend keeps CP_HQD_PQ_RPTR per queue: the
 * pinned queue's read pointer must still be what the CP left there, and the other seven must be 0.
 *
 * Both halves matter. A teardown that skipped every queue would pass a test that only looked at the
 * pinned one, and a teardown that zeroed every queue would pass a test that only looked at the
 * other seven.
 */
static unsigned int check_stuck_queue(struct amdgpu_device *adev, int verbose)
{
	struct bc250_gfx_inputs fin;
	struct amdgpu_ring *stuck;
	unsigned int i, bad = 0, writes_rptr = 0, writes_wptr = 0;
	u32 before = 0, after = 0;
	int r;

	printf("\n== a compute queue that does not answer the unmap ==\n");

	/* A fresh bring-up, so that every queue has run its ring test and has a read pointer worth
	 * clearing. The re-run arm left the device up; take it down the same way the miniport does
	 * and start again. */
	(void)bc250_gfx_hw_fini(adev);
	bc250_sdma_teardown(adev);
	bc250_gfx_teardown(adev);
	memset(&fin, 0, sizeof(fin));
	fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
	fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
	fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
	fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
	fin.async_gfx_ring = true;
	fin.pp_gfxoff = true;
	if (bc250_gfx_setup(adev, &fin) != 0 || bc250_sdma_setup(adev) != 0) {
		printf("  the setup failed; nothing to take down\n");
		return bad + 1;
	}
	register_rings(adev);
	r = bc250_gfx_hw_init(adev);
	if (r != 0) {
		printf("  the bring-up returned %d; this arm needs a running device\n", r);
		return bad + 1;
	}

	stuck = &adev->gfx.compute_ring[5];             /* me 1, pipe 1, queue 1 */
	nv_grbm_select(adev, stuck->me, stuck->pipe, stuck->queue, 0);
	before = bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR));
	nv_grbm_select(adev, 0, 0, 0, 0);
	if (before == 0) {
		printf("  compute ring 5's read pointer is already 0; the bring-up did not run its"
		       " ring test   <-- wrong\n");
		bad++;
	}

	backend_hqd_pin(stuck, 1);
	printf("  compute ring 5 (%u.%u.%u) will keep CP_HQD_ACTIVE = 1 through the unmap, read"
	       " pointer %u\n", stuck->me, stuck->pipe, stuck->queue, before);

	backend_reset_writes();
	(void)bc250_gfx_hw_fini(adev);
	backend_hqd_pin(stuck, 0);

	/* What the teardown wrote. The KIQ's own dequeue zeroes the same three registers for its own
	 * queue, so the expected count is seven compute queues plus the KIQ. */
	{
		const struct bc250_reg_write *w = backend_writes();
		unsigned int n = backend_write_count();
		u32 off_rptr = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR) * 4u;
		u32 off_wptr = SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_WPTR_LO) * 4u;

		for (i = 0; i < n; i++) {
			if (w[i].byte_offset == off_rptr && w[i].value == 0)
				writes_rptr++;
			if (w[i].byte_offset == off_wptr && w[i].value == 0)
				writes_wptr++;
		}
		if (writes_rptr != 8u || writes_wptr != 8u) {
			printf("  the teardown zeroed %u read pointers and %u write pointers, not the"
			       " seven queues plus the KIQ   <-- wrong\n", writes_rptr, writes_wptr);
			bad++;
		} else if (verbose) {
			printf("    eight of each, which is seven compute queues and the KIQ\n");
		}
	}

	/* And the per-queue statement the write count cannot make: this queue's own read pointer. */
	nv_grbm_select(adev, stuck->me, stuck->pipe, stuck->queue, 0);
	after = bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR));
	nv_grbm_select(adev, 0, 0, 0, 0);
	if (after != before) {
		printf("  the stuck queue's read pointer went from %u to %u   <-- wrong: the CP may"
		       " still be fetching from it\n", before, after);
		bad++;
	} else {
		printf("  the queue that did not answer kept its read pointer  : yes\n");
	}

	for (i = 0; i < adev->gfx.num_compute_rings; i++) {
		struct amdgpu_ring *ring = &adev->gfx.compute_ring[i];
		u32 v;

		if (ring == stuck)
			continue;
		nv_grbm_select(adev, ring->me, ring->pipe, ring->queue, 0);
		v = bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR));
		nv_grbm_select(adev, 0, 0, 0, 0);
		if (v == 0)
			continue;
		printf("    compute ring %u kept read pointer %u\n", i, v);
		bad++;
	}
	if (bad == 0)
		printf("  the other seven were put back to 0                   : yes\n");

	/* Leave the device as this arm found it: running, with the KIQ active. The arm that follows
	 * starts from exactly that state, and this one's teardown dequeued the KIQ. */
	r = bc250_gfx_hw_init(adev);
	if (r == 0)
		r = bc250_sdma_hw_init(adev);
	if (r != 0) {
		printf("  bringing the device back up for the next arm returned %d   <-- wrong\n", r);
		bad++;
	}

	printf("  %u wrong\n", bad);
	return bad;
}

/*
 * The one case the teardown fix does not cover: an instance that stopped without taking the KIQ
 * down. It is not hypothetical - that is exactly what 0.6.1 does, so the first 0.6.2 start on the
 * lab machine goes through here - and it is what the recovery branch at the top of
 * bc250_kiq_init_register() is for.
 *
 * The starting state is built out of two register writes, both through the shim's own write path so
 * that the MEC model follows them by its own rules instead of being poked into place: the KIQ is
 * left active exactly as the bring-up above left it, with the engine holding its copy of base and
 * read pointer, and then the MEC is halted. As far as this queue is concerned that IS 0.6.1's
 * teardown; the UNMAP_QUEUES for the other eight rings and the RLC stop change nothing about it.
 *
 * Then the rings are freed and allocated again, as the miniport does, and the bring-up runs. What
 * has to happen: the recovery branch finds the queue active with the engine halted, un-halts, the
 * engine takes its one stale fetch into the freed ring and faults, the dequeue is answered because
 * the engine is now running, the read pointer is corrected to 0 instead of the 0x737 the register
 * offers, and the rest of the bring-up succeeds.
 *
 * Exactly one fault is the point. Zero would mean the arm did not reproduce the condition; two or
 * more would mean the recovery did not clear the engine's copy and the later un-halt in kcq_resume
 * hit it again.
 */
static unsigned int check_unclean_start(struct amdgpu_device *adev, int verbose)
{
	struct amdgpu_ring *kiq = &adev->gfx.kiq[0].ring;
	struct bc250_gfx_inputs fin;
	unsigned int faults_before, faults, bad = 0;
	u64 stale_base, stale_end;
	u32 rptr;
	int r;

	printf("\n== a previous instance that left the KIQ up (0.6.1's teardown) ==\n");

	stale_base = kiq->gpu_addr;
	stale_end = stale_base + kiq->ring_size;
	faults_before = backend_mec_faults();

	nv_grbm_select(adev, kiq->me, kiq->pipe, kiq->queue, 0);
	if ((bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_HQD_ACTIVE)) & 1u) == 0) {
		printf("  the KIQ is not active here, so this arm tests nothing   <-- wrong\n");
		nv_grbm_select(adev, 0, 0, 0, 0);
		return 1;
	}
	bc250_shim_wreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR), UNITA_STALE_KIQ_RPTR);
	nv_grbm_select(adev, 0, 0, 0, 0);
	bc250_shim_wreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_MEC_CNTL),
			CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK);
	printf("  the KIQ at 0x%llX left active, read pointer 0x%X, MEC halted\n",
	       (unsigned long long)stale_base, UNITA_STALE_KIQ_RPTR);

	bc250_sdma_teardown(adev);
	bc250_gfx_teardown(adev);
	memset(&fin, 0, sizeof(fin));
	fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
	fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
	fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
	fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
	fin.async_gfx_ring = true;
	fin.pp_gfxoff = true;
	if (bc250_gfx_setup(adev, &fin) != 0 || bc250_sdma_setup(adev) != 0) {
		printf("  the setup failed; nothing to bring up\n");
		return bad + 1;
	}
	register_rings(adev);

	backend_reset_writes();
	backend_touched_start();
	r = bc250_gfx_hw_init(adev);
	if (r == 0)
		r = bc250_sdma_hw_init(adev);
	backend_touched_stop();

	if (r != 0) {
		printf("  the bring-up returned %d   <-- wrong: the recovery did not work\n", r);
		bad++;
	} else {
		printf("  the bring-up recovered and succeeded, %u writes, KIQ now at 0x%llX\n",
		       backend_write_count(), (unsigned long long)kiq->gpu_addr);
	}

	faults = backend_mec_faults() - faults_before;
	if (faults != 1) {
		printf("  %u stale fetches, not the one this arm expects   <-- wrong\n", faults);
		bad++;
	} else if (backend_mec_fault_address() < stale_base ||
		   backend_mec_fault_address() >= stale_end) {
		printf("  the one stale fetch was at 0x%llX, which is not inside the ring the last"
		       " instance left behind (0x%llX..0x%llX)   <-- wrong\n",
		       (unsigned long long)backend_mec_fault_address(),
		       (unsigned long long)stale_base, (unsigned long long)stale_end);
		bad++;
	} else {
		printf("  one stale fetch, at 0x%llX, inside the freed ring, and the queue came"
		       " back: yes\n", (unsigned long long)backend_mec_fault_address());
	}

	/* What the recovery had to achieve: the MQD it built holds 0 rather than the 0x737 the last
	 * instance left, and the register no longer holds it either.
	 *
	 * The register is not required to read 0 here, and this is not a weaker test than it looks.
	 * Between the recovery and this line the queue ran eleven ring tests, and the CP stub now
	 * models what the CP does to a queue's read pointer - it leaves it at the write pointer - per
	 * queue rather than in one shared register. So 0 would mean the queue had consumed nothing.
	 * The stale value is what must be gone. */
	nv_grbm_select(adev, kiq->me, kiq->pipe, kiq->queue, 0);
	rptr = bc250_shim_rreg(adev, SOC15_REG_OFFSET(GC, 0, mmCP_HQD_PQ_RPTR));
	nv_grbm_select(adev, 0, 0, 0, 0);
	if (rptr == UNITA_STALE_KIQ_RPTR ||
	    ((struct v10_compute_mqd *)kiq->mqd_ptr)->cp_hqd_pq_rptr != 0) {
		printf("  the read pointer survived: register 0x%X, MQD 0x%X   <-- wrong\n", rptr,
		       ((struct v10_compute_mqd *)kiq->mqd_ptr)->cp_hqd_pq_rptr);
		bad++;
	} else {
		printf("  the stale read pointer is gone: MQD 0, register 0x%X (what the queue has"
		       " consumed since): yes\n", rptr);
	}

	bad += survey_fini_registers("the recovering bring-up", adev, verbose);
	printf("  %u wrong\n", bad);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * The SDMA engines across a second bring-up (experiment E15)
 *
 * The arm above settles the KIQ. This one settles the question backend_mem.h's MEC block left
 * open - "whether ME or an SDMA engine nevertheless caches a base across a halt is UNTESTED" - for
 * the two SDMA engines, and the answer measured on unit A is that they do, in the form of a write
 * pointer that survives the undo and that no register write of the next bring-up can move.
 *
 * What happens here: a bring-up, unit A's own submissions on both engines, the undo, rings freed
 * and allocated again at fresh addresses, a second bring-up, and then a ring test on each engine.
 * What has to hold:
 *
 *   - the first bring-up writes 0 into all four pointer registers and 0 again into RB_WPTR under
 *     MINOR_PTR_UPDATE, which is upstream's fresh arm untouched;
 *   - after its submissions the engines stand where unit A's did, 0x100 and 0xC0 bytes;
 *   - the second bring-up still writes 0 into all four of them - the sequence the host replay
 *     compares with the Linux trace does not move - and then writes the pointer it adopted;
 *   - what it adopted is what the first run left, to the byte;
 *   - both ring tests pass, and no doorbell was ignored for being at or below the engine.
 *
 * Without the adoption in bc250_sdma_gfx_resume_instance() the last two fail exactly as unit A
 * failed: the ring test rings 0x40 against an engine standing at 0x100, the doorbell is ignored,
 * nothing executes and the scratch slot keeps 0xCAFEDEAD until the poll runs out.
 * ------------------------------------------------------------------------------------------- */

/* Unit A, E15: the first bring-up ran a ring test and three fences on SDMA0, a ring test and two on
 * SDMA1. Every submission is padded to the sixteen-dword fetch size the ring's align_mask sets, so
 * the engines finished at 4 * 16 * 4 and 3 * 16 * 4 bytes - and that is what the sweeps read back.
 * The counts are here so this arm walks unit A's own numbers rather than some convenient ones. */
static const unsigned int g_sdma_arm_fences[2] = { 3u, 2u };
static const u64 g_sdma_arm_final[2] = { 0x100u, 0x0C0u };
#define SDMA_ARM_SEQ_BASE   0x00000000C0FFEE10ULL

/* Slots 0 and 1 belong to the two engines' ring tests (bc250_sdma_ring_test() takes them by
 * ring->me), so the fences start at 2. Four apiece is more than either engine uses. */
#define SDMA_ARM_FENCE_SLOT(inst, k)  (2u + (unsigned int)(inst) * 4u + (unsigned int)(k))

/* Every value the run wrote to one byte offset, oldest first; the return is how many there were,
 * which can exceed `max`. */
static unsigned int writes_to(u32 byte_offset, u32 *out, unsigned int max)
{
	const struct bc250_reg_write *w = backend_writes();
	unsigned int n = backend_write_count(), i, k = 0;

	for (i = 0; i < n; i++) {
		if (w[i].byte_offset != byte_offset)
			continue;
		if (k < max)
			out[k] = w[i].value;
		k++;
	}
	return k;
}

/*
 * What one bring-up wrote into an engine's four pointer registers.
 *
 * The read pointer is written once and is always 0; the write pointer is written twice, 0 first -
 * upstream's fresh arm, which must not move, because the first bring-up's write sequence is what
 * the 354-write comparison at the top of this file is made of - and then whatever the driver
 * decided under MINOR_PTR_UPDATE. `expect` is that second value per engine, in bytes: 0 on a cold
 * bring-up, the adopted pointer on a second one.
 */
static unsigned int check_sdma_pointer_writes(const char *label, struct amdgpu_device *adev,
					      const u64 *expect, int verbose)
{
	unsigned int bad = 0;
	int i;

	for (i = 0; i < adev->sdma.num_instances; i++) {
		static const char *const once_name[2] = { "RB_RPTR", "RB_RPTR_HI" };
		u32 off_once[2], off_twice[2], want_second[2];
		u32 v[4];
		unsigned int k, n;

		off_once[0] = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR) * 4u;
		off_once[1] = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR_HI) * 4u;
		off_twice[0] = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR) * 4u;
		off_twice[1] = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_HI) * 4u;
		want_second[0] = (u32)(expect[i] & 0xffffffffu);
		want_second[1] = (u32)(expect[i] >> 32);

		for (k = 0; k < 2; k++) {
			n = writes_to(off_once[k], v, (unsigned int)ARRAY_SIZE(v));
			if (n != 1u || v[0] != 0) {
				printf("    %s: SDMA%d_GFX_%s written %u times%s   <-- wrong:"
				       " upstream writes it once, with 0\n", label, i,
				       once_name[k], n, n ? "" : " (not at all)");
				bad++;
			}
		}

		for (k = 0; k < 2; k++) {
			n = writes_to(off_twice[k], v, (unsigned int)ARRAY_SIZE(v));
			if (n != 2u) {
				printf("    %s: SDMA%d_GFX_RB_WPTR%s written %u times   <--"
				       " wrong: the fresh 0 and the value under MINOR_PTR_UPDATE\n",
				       label, i, k ? "_HI" : "", n);
				bad++;
				continue;
			}
			if (v[0] != 0) {
				printf("    %s: SDMA%d_GFX_RB_WPTR%s initialised with %08X, not"
				       " 0   <-- wrong: upstream's fresh arm must not move\n",
				       label, i, k ? "_HI" : "", v[0]);
				bad++;
			}
			if (v[1] != want_second[k]) {
				printf("    %s: SDMA%d_GFX_RB_WPTR%s programmed %08X, expected"
				       " %08X   <-- wrong\n", label, i, k ? "_HI" : "", v[1],
				       want_second[k]);
				bad++;
			}
		}

		if (verbose && bad == 0)
			printf("    %s: SDMA%d pointer registers 0, 0, 0/%08X, 0/%08X\n", label, i,
			       want_second[0], want_second[1]);
	}
	return bad;
}

/* Unit A's own traffic on one engine: a ring test, then its fences. Each of them is a doorbell, and
 * each doorbell is what moves the engine. */
static unsigned int sdma_arm_submit(struct amdgpu_device *adev, int inst, int verbose)
{
	struct amdgpu_ring *ring = &adev->sdma.instance[inst].ring;
	unsigned int k, bad = 0;
	int r;

	r = bc250_sdma_ring_test(ring);
	if (r != 0) {
		printf("    sdma%d: the ring test returned %d   <-- wrong\n", inst, r);
		return 1;
	}

	for (k = 0; k < g_sdma_arm_fences[inst]; k++) {
		unsigned int slot = SDMA_ARM_FENCE_SLOT(inst, k);
		u64 seq = SDMA_ARM_SEQ_BASE + slot;

		r = bc250_sdma_signal_fence(ring, bc250_sdma_fence_addr(adev, slot), seq, 0);
		if (r != 0) {
			printf("    sdma%d: fence %u returned %d   <-- wrong\n", inst, k, r);
			bad++;
			continue;
		}
		if ((u32)bc250_sdma_fence_read(adev, slot) != (u32)seq) {
			printf("    sdma%d: fence %u did not land in its slot   <-- wrong\n",
			       inst, k);
			bad++;
		}
	}
	if (verbose && bad == 0)
		printf("    sdma%d: a ring test and %u fences, all executed\n", inst,
		       g_sdma_arm_fences[inst]);
	return bad;
}

static unsigned int check_sdma_pointers(struct amdgpu_device *adev, int verbose)
{
	struct bc250_gfx_inputs fin;
	u64 final_ptr[AMDGPU_MAX_SDMA_INSTANCES];
	u64 base_before[AMDGPU_MAX_SDMA_INSTANCES];
	u64 fresh[AMDGPU_MAX_SDMA_INSTANCES];
	u64 one_submission;
	unsigned int bad = 0, ignored_before;
	int i, r;

	printf("\n== the SDMA engines across a second bring-up (experiment E15) ==\n");

	memset(final_ptr, 0, sizeof(final_ptr));
	memset(fresh, 0, sizeof(fresh));

	/* A device of this arm's own. The arms above leave one running, and what is measured here is
	 * what TWO bring-ups in one device start do to the engines. */
	(void)bc250_gfx_hw_fini(adev);
	bc250_sdma_teardown(adev);
	bc250_gfx_teardown(adev);
	memset(&fin, 0, sizeof(fin));
	fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
	fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
	fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
	fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
	fin.async_gfx_ring = true;
	fin.pp_gfxoff = true;
	if (bc250_gfx_setup(adev, &fin) != 0 || bc250_sdma_setup(adev) != 0) {
		printf("  the setup failed; nothing to bring up\n");
		return 1;
	}
	register_rings(adev);
	if (bc250_sdma_fence_page_alloc(adev) != 0) {
		printf("  bc250_sdma_fence_page_alloc failed; the ring test has no scratch slot\n");
		return 1;
	}

	backend_reset_writes();
	r = bc250_gfx_hw_init(adev);
	if (r == 0)
		r = bc250_sdma_hw_init(adev);
	if (r != 0) {
		printf("  the first bring-up returned %d   <-- wrong\n", r);
		return bad + 1;
	}
	bad += check_sdma_pointer_writes("first bring-up", adev, fresh, verbose);

	ignored_before = backend_sdma_ignored_doorbells();
	for (i = 0; i < adev->sdma.num_instances; i++)
		bad += sdma_arm_submit(adev, i, verbose);

	for (i = 0; i < adev->sdma.num_instances; i++) {
		struct amdgpu_ring *ring = &adev->sdma.instance[i].ring;

		final_ptr[i] = backend_sdma_engine_wptr(ring);
		base_before[i] = ring->gpu_addr;
		printf("  sdma%d: %u submissions leave the engine at 0x%llX bytes\n", i,
		       1u + g_sdma_arm_fences[i], (unsigned long long)final_ptr[i]);
		if (final_ptr[i] != (ring->wptr << 2)) {
			printf("    the driver thinks it is at 0x%llX   <-- wrong\n",
			       (unsigned long long)(ring->wptr << 2));
			bad++;
		}
		if (i < (int)ARRAY_SIZE(g_sdma_arm_final) && final_ptr[i] != g_sdma_arm_final[i]) {
			printf("    unit A's engine finished at 0x%llX   <-- wrong: this arm is not"
			       " walking the same submissions\n",
			       (unsigned long long)g_sdma_arm_final[i]);
			bad++;
		}
	}

	/*
	 * The undo says whether the engines drained, not only whether they halted, and that is a
	 * separate statement from everything above: HALT stops an engine wherever it stands, and one
	 * stopped with its read pointer behind its write pointer is still holding packets that name
	 * this instance's ring, fence slots and copy destinations - which the miniport is about to
	 * give back to the page allocator.
	 *
	 * Nothing in driver/shim can produce that state, because the stub executes a submission
	 * whole. backend_sdma_hold_back() is the test speaking for hardware, as backend_hqd_pin() is
	 * for a queue that will not answer an unmap: sdma0 answers with its read pointer one
	 * submission behind, and the fini has to notice.
	 */
	one_submission = ((u64)adev->sdma.instance[0].ring.funcs->align_mask + 1u) * 4u;
	backend_sdma_hold_back(&adev->sdma.instance[0].ring, one_submission);
	r = bc250_sdma_hw_fini(adev);
	if (r == 0) {
		printf("  the undo passed an engine that halted one submission short of its write"
		       " pointer   <-- wrong\n");
		bad++;
	} else {
		printf("  an engine halted mid-queue makes the undo return %d        : yes\n", r);
	}
	backend_sdma_hold_back(&adev->sdma.instance[0].ring, 0);

	/* And with both engines drained it has to pass, or the check above would be worthless. The
	 * halt sequence is register writes and is safe to run twice; the engines are already down. */
	r = bc250_sdma_hw_fini(adev);
	if (r != 0) {
		printf("  the undo returned %d with both engines drained   <-- wrong\n", r);
		bad++;
	}

	/* Then the rings freed and allocated again, exactly as the miniport does it. */
	(void)bc250_gfx_hw_fini(adev);
	bc250_sdma_teardown(adev);
	bc250_gfx_teardown(adev);
	if (bc250_gfx_setup(adev, &fin) != 0 || bc250_sdma_setup(adev) != 0) {
		printf("  the second setup failed; nothing to re-run\n");
		return bad + 1;
	}
	register_rings(adev);

	printf("\n  == the second bring-up, rings at fresh addresses ==\n");
	backend_reset_writes();
	backend_touched_start();
	r = bc250_gfx_hw_init(adev);
	if (r == 0)
		r = bc250_sdma_hw_init(adev);
	backend_touched_stop();
	if (r != 0) {
		printf("  the second bring-up returned %d   <-- wrong\n", r);
		return bad + 1;
	}

	bad += check_sdma_pointer_writes("second bring-up", adev, final_ptr, verbose);

	for (i = 0; i < adev->sdma.num_instances; i++) {
		struct amdgpu_ring *ring = &adev->sdma.instance[i].ring;

		if (ring->gpu_addr == base_before[i]) {
			printf("  sdma%d's ring did not move; a stale pointer would find valid memory"
			       " and show nothing   <-- wrong\n", i);
			bad++;
		}
		if ((ring->wptr << 2) != final_ptr[i]) {
			printf("  sdma%d adopted 0x%llX, the first run left 0x%llX   <-- wrong\n", i,
			       (unsigned long long)(ring->wptr << 2),
			       (unsigned long long)final_ptr[i]);
			bad++;
		} else {
			printf("  sdma%d: ring 0x%llX -> 0x%llX, write pointer adopted at 0x%llX\n",
			       i, (unsigned long long)base_before[i],
			       (unsigned long long)ring->gpu_addr,
			       (unsigned long long)final_ptr[i]);
		}
		/* And into the slot the engine polls, which F32_POLL_ENABLE has just turned on over a
		 * page that was allocated a moment ago and reads 0. */
		if (ring->wptr_cpu_addr != NULL &&
		    *(volatile u64 *)ring->wptr_cpu_addr != final_ptr[i]) {
			printf("  sdma%d's write-back slot holds 0x%llX, not the adopted 0x%llX"
			       "   <-- wrong\n", i,
			       (unsigned long long)*(volatile u64 *)ring->wptr_cpu_addr,
			       (unsigned long long)final_ptr[i]);
			bad++;
		}
	}

	/* And the thing all of it is for. */
	for (i = 0; i < adev->sdma.num_instances; i++) {
		r = bc250_sdma_ring_test(&adev->sdma.instance[i].ring);
		if (r != 0) {
			printf("  sdma%d: the ring test after the second bring-up returned %d"
			       "   <-- wrong: the engine executed nothing\n", i, r);
			bad++;
		}
	}
	if (backend_sdma_ignored_doorbells() != ignored_before) {
		printf("  %u doorbells were at or below the engine's pointer and executed nothing"
		       "   <-- wrong\n", backend_sdma_ignored_doorbells() - ignored_before);
		bad++;
	} else if (bad == 0) {
		printf("  both engines ran again, and no doorbell was ignored  : yes\n");
	}

	/*
	 * The two refusals. A kept pointer that is not a whole submission, and the all-ones a faulted
	 * register sequence or a dead bus answers with, both have to stop the bring-up rather than be
	 * rounded into something plausible - a pointer we do not understand is not one to build on.
	 * A working engine never reaches either state, so the backend says them for the hardware.
	 *
	 * Last in this arm, because a refused bring-up leaves the engine half programmed. The control
	 * runs that follow start from backend_mem_reset() and a fresh adev, so nothing inherits it.
	 */
	{
		static const struct { const char *what; u64 wptr; } refuse[] = {
			{ "half a submission",        0x120u },
			{ "one dword past a whole one", 0x104u },
			{ "an odd byte",              0x101u },
			{ "all-ones, which is a dead read", 0xffffffffu }
		};
		struct amdgpu_ring *ring = &adev->sdma.instance[0].ring;
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(refuse); k++) {
			backend_sdma_force_engine_wptr(ring, refuse[k].wptr);
			r = bc250_sdma_hw_init(adev);
			if (r != BC250_EINVAL) {
				printf("    sdma0 kept 0x%llX (%s) and the bring-up returned %d, not"
				       " BC250_EINVAL   <-- wrong\n",
				       (unsigned long long)refuse[k].wptr, refuse[k].what, r);
				bad++;
			} else if (verbose) {
				printf("    sdma0 kept 0x%llX (%s): refused\n",
				       (unsigned long long)refuse[k].wptr, refuse[k].what);
			}
		}
		backend_sdma_force_engine_wptr(ring, final_ptr[0]);
		printf("  four write pointers no engine should ever hold, all refused  : %s\n",
		       bad == 0 ? "yes" : "no");
	}

	bad += survey_fini_registers("the SDMA second bring-up", adev, verbose);

	bc250_sdma_fence_page_free(adev);
	printf("  %u wrong\n", bad);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * Stage 0: nv_common_hw_init's NBIO writes
 *
 * Half a second before the GFX window, at t = 0.0383, and compared against its own three traced
 * writes rather than folded into either of the two windows, because that is what it is: a different
 * IP block, run once per boot.
 *
 * The values are quoted from the trace and checked as a whole, not built from it: the offsets come
 * out of AMD's headers through the SOC15 macros, and the two HDP values come out of
 * BC250_MMIO_REG_HOLE_OFFSET plus the KFD offsets.
 * ------------------------------------------------------------------------------------------- */
static unsigned int check_stage0(struct amdgpu_device *adev, int verbose)
{
	/* evidence/linux/2026-09-21-E03-init-trace, 0.038310 to 0.038314. */
	static const struct {
		const char *name;
		u32 offset, value;
	} want[] = {
		{ "NBIO.REMAP_HDP_MEM_FLUSH_CNTL",           0x03934, 0x0007F000 },
		{ "NBIO.REMAP_HDP_REG_FLUSH_CNTL",           0x03938, 0x0007F004 },
		{ "NBIO.RCC_DEV0_EPF0_RCC_DOORBELL_APER_EN", 0x03780, 0x00000001 }
	};
	const struct bc250_reg_write *w;
	unsigned int n, i, bad = 0;

	printf("\n== stage 0: nv_common_hw_init, t = 0.0383 ==\n");

	backend_reset_writes();
	if (bc250_nbio_hw_init(adev) != 0) {
		printf("  bc250_nbio_hw_init failed\n");
		return 1;
	}

	w = backend_writes();
	n = backend_write_count();
	if (n != ARRAY_SIZE(want)) {
		printf("  produced %u writes, the trace has %u\n", n, (unsigned int)ARRAY_SIZE(want));
		bad++;
	}
	for (i = 0; i < n && i < ARRAY_SIZE(want); i++) {
		if (w[i].byte_offset == want[i].offset && w[i].value == want[i].value) {
			if (verbose)
				printf("    %-42s 0x%05X = %08X\n",
				       want[i].name, w[i].byte_offset, w[i].value);
			continue;
		}
		printf("    shim 0x%05X = %08X   trace %-42s 0x%05X = %08X   <-- wrong\n",
		       w[i].byte_offset, w[i].value, want[i].name, want[i].offset, want[i].value);
		bad++;
	}
	if (bad == 0)
		printf("  all 3 writes identical to the trace\n");
	printf("  %u wrong\n", bad);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * The GART page table entries
 *
 * driver/shim/bc250_gart.c writes memory and no registers, so no trace can check it and none of the
 * comparison above reaches it. It is checked here instead, against the formula in
 * amdgpu_gmc_set_pte_pde() and the flag chain in amdgpu_ttm_tt_pte_flags(), because the kernel-side
 * GTT allocator calls it and the first ring test is the first time the GPU walks an entry this
 * driver wrote.
 *
 * Stated plainly, because it is a limit of the evidence and not of the code: the E03 material
 * contains no GART table content and no bind. amdgpu's own gart_enable leaves the table full of the
 * dummy page and the traced window never binds a real one, so there is nothing on unit A to compare
 * these values with. What follows checks the formula, the flags, the bounds and the refusals. The
 * first confirmation from hardware will be a ring test passing with its ring in GTT.
 * ------------------------------------------------------------------------------------------- */

/* The value the header of bc250_gart.h documents, repeated here so that the two have to agree:
 * VALID | SYSTEM | SNOOPED | EXECUTABLE | READABLE | WRITEABLE | MTYPE_NV10(MTYPE_UC). If someone
 * changes a flag in bc250_gart.c, one of these two says so. */
#define EXPECT_GART_PTE_FLAGS	0x0003000000000077ULL

static unsigned int check_gart(struct amdgpu_device *adev, int verbose)
{
	static u64 dma[8];
	u64 flags, *table;
	unsigned int entries, bad = 0, i;
	u64 off;
	int rc;

	printf("\n== GART page table entries ==\n");
	printf("  the E03 trace contains no GART table content and no bind, so these are checked\n"
	       "  against the formula and the flag chain, not against unit A\n");

	entries = (unsigned int)(adev->gmc.gart_size / AMDGPU_GPU_PAGE_SIZE);
	table = (u64 *)calloc(entries, sizeof(u64));
	if (table == NULL) {
		printf("  cannot allocate a %u-entry table\n", entries);
		return 1;
	}

	flags = bc250_gart_pte_flags(adev);
	printf("  flags for a cached kernel GTT page     : %016llX\n", (unsigned long long)flags);
	if (flags != EXPECT_GART_PTE_FLAGS) {
		printf("    expected %016llX   <-- wrong\n", EXPECT_GART_PTE_FLAGS);
		bad++;
	}

	/* The formula: address masked to its field, flags or'ed in. The third case is the one that
	 * matters - bits outside the mask must be dropped, not carried. */
	if (bc250_gart_pte(0x0000000012345000ULL, flags) != (0x0000000012345000ULL | flags))
		bad++, printf("    PTE formula wrong for a plain address\n");
	if (bc250_gart_pte(0x0000FFFFFFFFF000ULL, 0) != 0x0000FFFFFFFFF000ULL)
		bad++, printf("    PTE formula wrong at the top of the field\n");
	if (bc250_gart_pte(0xFFFF000000000FFFULL, 0) != 0)
		bad++, printf("    PTE formula does not drop bits outside the address field\n");

	/* A real bind of eight pages at a non-zero offset, with deliberately non-contiguous bus
	 * addresses, so that a loop that ignored dma_addr[i] would be caught. */
	off = 16u * AMDGPU_GPU_PAGE_SIZE;
	for (i = 0; i < ARRAY_SIZE(dma); i++)
		dma[i] = 0x0000000100000000ULL + (u64)(i * 3u + 1u) * AMDGPU_GPU_PAGE_SIZE;

	rc = bc250_gart_bind(adev, off, (unsigned int)ARRAY_SIZE(dma), dma, table);
	if (rc != 0) {
		printf("  bc250_gart_bind returned %d   <-- wrong\n", rc);
		bad++;
	} else {
		unsigned int base = (unsigned int)(off / AMDGPU_GPU_PAGE_SIZE);

		for (i = 0; i < ARRAY_SIZE(dma); i++) {
			u64 want = (dma[i] & AMDGPU_PTE_ADDR_MASK) | flags;

			if (table[base + i] != want) {
				printf("    entry %u: %016llX, expected %016llX   <-- wrong\n",
				       base + i, (unsigned long long)table[base + i],
				       (unsigned long long)want);
				bad++;
			}
		}
		if (table[base - 1] != 0 || table[base + ARRAY_SIZE(dma)] != 0) {
			printf("    the bind wrote outside the range it was given   <-- wrong\n");
			bad++;
		}
		if (verbose)
			printf("    8 pages at GART offset 0x%llX -> MC 0x%llX, first PTE %016llX\n",
			       (unsigned long long)off,
			       (unsigned long long)(adev->gmc.gart_start + off),
			       (unsigned long long)table[base]);
	}

	/* Unbind writes the dummy page with flags 0, which is what makes an entry invalid from Vega
	 * on. Checking the flags are really zero is the point: a "valid entry pointing at the dummy
	 * page" would look almost right and would let a stale access succeed. */
	rc = bc250_gart_unbind(adev, off, (unsigned int)ARRAY_SIZE(dma), table);
	if (rc != 0) {
		printf("  bc250_gart_unbind returned %d   <-- wrong\n", rc);
		bad++;
	} else {
		unsigned int base = (unsigned int)(off / AMDGPU_GPU_PAGE_SIZE);
		u64 want = adev->dummy_page_addr & AMDGPU_PTE_ADDR_MASK;

		for (i = 0; i < ARRAY_SIZE(dma); i++) {
			if (table[base + i] != want) {
				printf("    unbound entry %u: %016llX, expected %016llX   <-- wrong\n",
				       base + i, (unsigned long long)table[base + i],
				       (unsigned long long)want);
				bad++;
			}
		}
	}

	/* The refusals. Each must return BC250_EINVAL and leave the table alone; the last two are the
	 * ones that would otherwise corrupt host memory rather than merely misdraw. */
	{
		static const struct {
			const char *what;
			u64 offset;
			unsigned int pages;
			int use_bad_dma;
		} refuse[] = {
			{ "a misaligned GART offset",       0x800ULL,  1, 0 },
			{ "zero pages",                     0ULL,      0, 0 },
			{ "a range past the aperture end",  0ULL,      0, 0 },   /* pages filled below */
			{ "an offset past the aperture end", 0ULL,     1, 0 },   /* offset filled below */
			{ "a misaligned bus address",       0ULL,      1, 1 },
			{ "a bus address above 48 bits",    0ULL,      1, 2 }
		};
		static u64 one[1];
		unsigned int k;

		for (k = 0; k < ARRAY_SIZE(refuse); k++) {
			u64 o = refuse[k].offset;
			unsigned int p = refuse[k].pages;
			u64 before;

			if (k == 2)
				p = entries + 1u;
			if (k == 3)
				o = adev->gmc.gart_size;

			one[0] = 0x0000000100000000ULL;
			if (refuse[k].use_bad_dma == 1)
				one[0] |= 0x800ULL;
			if (refuse[k].use_bad_dma == 2)
				one[0] = 0x0001000000000000ULL;

			before = table[o / AMDGPU_GPU_PAGE_SIZE < entries
				       ? o / AMDGPU_GPU_PAGE_SIZE : 0];
			rc = bc250_gart_bind(adev, o, p, one, table);
			if (rc != BC250_EINVAL) {
				printf("    %-34s accepted (returned %d)   <-- wrong\n",
				       refuse[k].what, rc);
				bad++;
			} else if (table[o / AMDGPU_GPU_PAGE_SIZE < entries
					 ? o / AMDGPU_GPU_PAGE_SIZE : 0] != before) {
				printf("    %-34s refused but wrote anyway   <-- wrong\n",
				       refuse[k].what);
				bad++;
			} else if (verbose) {
				printf("    %-34s refused, table untouched\n", refuse[k].what);
			}
		}
		printf("  six bad inputs, all refused with the table untouched\n");
	}

	free(table);
	printf("  %u wrong\n", bad);
	return bad;
}

/* ---------------------------------------------------------------------------------------------
 * Stage C: the indirect buffer, and one VMID's page directory (ADR 0008)
 *
 * Neither is in any trace and neither can be. amdgpu programs a VMID's page directory root per job,
 * from amdgpu_vm_flush(), and every window recorded on unit A is a bring-up; it submits no indirect
 * buffer in them either. So this checks the three things that would otherwise be checked by eye:
 *
 *   - the dwords bc250_gfx_emit_ib() writes, against the packet AMD's own macros build. Four of
 *     them, and each is a separate way of being wrong: a different header would be a different
 *     packet, a swapped address pair would fetch from nowhere, a length in the VMID field would be
 *     a huge IB at VMID 0, and a VMID in the length field a three-dword IB at VMID 0;
 *   - that every refusal leaves the ring exactly as it was, which is what makes a refusal safe to
 *     ignore in the miniport (gfx.c returns a status and writes nothing);
 *   - the registers bc250_gmc_set_vmid_pd() names, by their CONTEXTn names rather than by the
 *     ctx_addr_distance arithmetic the code uses, so the two have to agree.
 *
 * And one end-to-end run: the ring test built as an indirect buffer, submitted, fetched by the CP
 * stub and applied. The stub follows an IB at VMID 0 only - see backend_mem.h - so the same
 * submission at VMID 1 is the control, and it has to come back with the register untouched.
 * ------------------------------------------------------------------------------------------- */

/* The slot the miniport gives the submission path (BC250_SUBMIT_FENCE_SLOT in driver/kmd/gfx.c);
 * the number matters here only in that it must not be one of the ring-indexed ones. */
#define STAGE_C_FENCE_SLOT	10u
#define STAGE_C_SEQ_BASE	0x00000000C0FFEE20ULL

/* Emit one IB into a ring and compare the four dwords with the packet, then put the ring back. */
static unsigned int check_ib_dwords(const char *label, struct amdgpu_ring *ring, u64 addr,
				    u32 length_dw, u32 vmid, int verbose)
{
	u32 want[4];
	u64 at;
	unsigned int bad = 0, k;
	int r;

	want[0] = (u32)PACKET3(PACKET3_INDIRECT_BUFFER, 2);
	want[1] = lower_32_bits(addr);
	want[2] = upper_32_bits(addr);
	want[3] = PACKET3_INDIRECT_BUFFER__IB_SIZE(length_dw) | PACKET3_INDIRECT_BUFFER__VMID(vmid);

	r = amdgpu_ring_alloc(ring, bc250_gfx_ib_size(ring));
	if (r != 0) {
		printf("    %s: amdgpu_ring_alloc returned %d   <-- wrong\n", label, r);
		return 1;
	}

	at = ring->wptr;
	r = bc250_gfx_emit_ib(ring, addr, length_dw, vmid);
	if (r != 0) {
		printf("    %s: bc250_gfx_emit_ib returned %d   <-- wrong\n", label, r);
		amdgpu_ring_undo(ring);
		return 1;
	}
	if (ring->wptr - at != 4u) {
		printf("    %s: wrote %llu dwords, not the 4 bc250_gfx_ib_size() promises   <-- wrong\n",
		       label, (unsigned long long)(ring->wptr - at));
		bad++;
	}
	for (k = 0; k < ARRAY_SIZE(want); k++) {
		u32 got = ring->ring[(size_t)((at + k) & ring->buf_mask)];

		if (got != want[k]) {
			printf("    %s: dword %u is %08X, the packet says %08X   <-- wrong\n",
			       label, k, got, want[k]);
			bad++;
		}
	}
	if (verbose && bad == 0)
		printf("    %s: %08X %08X %08X %08X\n", label, want[0], want[1], want[2], want[3]);

	amdgpu_ring_undo(ring);
	return bad;
}

/* Every way of asking for an IB that cannot be encoded. Each must return a code and leave the write
 * pointer where it was: the miniport turns a refusal into a status and carries on, which is only
 * sound if the ring really is untouched. */
static unsigned int check_ib_refusals(struct amdgpu_device *adev, u64 addr, int verbose)
{
	struct amdgpu_ring *gfx = &adev->gfx.gfx_ring[0];
	static const struct {
		const char *what;
		u64 addr_or;
		u32 length_dw;
		u32 vmid;
		int compute;
	} bad_input[] = {
		{ "an address with bit 0 set",       1u,  3u,        0u,  0 },
		{ "an address with bit 1 set",       2u,  3u,        0u,  0 },
		{ "a length of 0",                   0u,  0u,        0u,  0 },
		{ "a length past the 20-bit field",  0u,  0x100000u, 0u,  0 },
		{ "vmid 16",                         0u,  3u,        16u, 0 },
		{ "a compute ring",                  0u,  3u,        0u,  1 },
	};
	unsigned int bad = 0, k;

	for (k = 0; k < ARRAY_SIZE(bad_input); k++) {
		struct amdgpu_ring *ring = bad_input[k].compute ? &adev->gfx.compute_ring[0] : gfx;
		u64 before = ring->wptr;
		int r;

		/* The allocation is the gfx ring's either way: a compute ring has no IB size here, so
		 * asking bc250_gfx_ib_size() for one would give 0 and there would be nothing to undo. */
		if (amdgpu_ring_alloc(gfx, bc250_gfx_ib_size(gfx)) != 0) {
			printf("    %-34s could not reserve ring space   <-- wrong\n",
			       bad_input[k].what);
			bad++;
			continue;
		}
		r = bc250_gfx_emit_ib(ring, addr | bad_input[k].addr_or, bad_input[k].length_dw,
				      bad_input[k].vmid);
		if (r == 0) {
			printf("    %-34s accepted   <-- wrong\n", bad_input[k].what);
			bad++;
		} else if (ring->wptr != before) {
			printf("    %-34s refused but moved the write pointer   <-- wrong\n",
			       bad_input[k].what);
			bad++;
		} else if (verbose) {
			printf("    %-34s refused (%d), ring untouched\n", bad_input[k].what, r);
		}
		amdgpu_ring_undo(gfx);
	}
	return bad;
}

/* COMPUTE_PGM_LO is addr >> 8. A two-value packet also stores bits 47:40 in PGM_HI. */
static unsigned int check_shader_addr(void)
{
	const u32 pgm = GC_BASE__INST0_SEG0 + mmCOMPUTE_PGM_LO - PACKET3_SET_SH_REG_START;
	u32 both[7], only[3];
	u64 addr = 0;
	u32 lo = 0, hi = 1, wrote = 1;
	unsigned int bad = 0;

	both[0] = (u32)PACKET3(PACKET3_CONTEXT_CONTROL, 1);
	both[1] = 0x80000000u;
	both[2] = 0x80000000u;
	both[3] = (u32)PACKET3(PACKET3_SET_SH_REG, 2);
	both[4] = pgm;
	both[5] = 0x01000400u;
	both[6] = 0;
	if (!bc250_pm4_shader_addr(both, 7, &addr, &lo, &hi, &wrote) || !wrote ||
	    lo != 0x01000400u || hi != 0 || addr != 0x100040000ull) {
		printf("    shader addr both: lo %08X hi %08X wrote %u addr 0x%llX   <-- wrong\n",
		       lo, hi, wrote, (unsigned long long)addr);
		bad++;
	}
	only[0] = (u32)PACKET3(PACKET3_SET_SH_REG, 1);
	only[1] = pgm;
	only[2] = 0x01000400u;
	wrote = 1;
	hi = 1;
	if (!bc250_pm4_shader_addr(only, 3, &addr, &lo, &hi, &wrote) || wrote ||
	    lo != 0x01000400u || addr != 0x100040000ull) {
		printf("    shader addr lo-only: lo %08X hi %08X wrote %u addr 0x%llX   <-- wrong\n",
		       lo, hi, wrote, (unsigned long long)addr);
		bad++;
	}
	return bad;
}

/* The job frame is not the ring test. VMID 0 is refused. VMID 1 writes PFP_SYNC_ME,
 * CONTEXT_CONTROL 0x81018003, FRAME_CONTROL start, the IB, FRAME_CONTROL end, then the fence. */
static unsigned int check_job_submit(struct amdgpu_device *adev, int verbose)
{
	struct amdgpu_ring *ring = &adev->gfx.gfx_ring[0];
	u64 ib_addr, fence_addr, at;
	u32 length_dw = 0;
	u32 dw;
	int r;
	unsigned int bad = 0;

	r = bc250_gfx_ib_ring_test_build(adev, &length_dw);
	if (r != 0 || length_dw != 3u)
		return 1;
	ib_addr = bc250_gfx_ib_addr(adev);
	fence_addr = bc250_gfx_fence_addr(adev, STAGE_C_FENCE_SLOT);
	at = ring->wptr;
	r = bc250_gfx_submit_job(ring, ib_addr, length_dw, 0, fence_addr, STAGE_C_SEQ_BASE + 2u,
				 AMDGPU_FENCE_FLAG_INT);
	if (r == 0 || ring->wptr != at) {
		printf("    job vmid 0: accepted or moved the ring (%d)   <-- wrong\n", r);
		return 1;
	}
	at = ring->wptr;
	r = bc250_gfx_submit_job(ring, ib_addr, length_dw, 1, fence_addr, STAGE_C_SEQ_BASE + 3u,
				 AMDGPU_FENCE_FLAG_INT);
	if (r != 0) {
		printf("    job vmid 1: bc250_gfx_submit_job returned %d   <-- wrong\n", r);
		return 1;
	}
	dw = ring->ring[(size_t)(at & ring->buf_mask)];
	if (dw != (u32)PACKET3(PACKET3_PFP_SYNC_ME, 0)) {
		printf("    job: first dword %08X is not PFP_SYNC_ME   <-- wrong\n", dw);
		bad++;
	}
	dw = ring->ring[(size_t)((at + 2u) & ring->buf_mask)];
	if (dw != (u32)PACKET3(PACKET3_CONTEXT_CONTROL, 1) ||
	    ring->ring[(size_t)((at + 3u) & ring->buf_mask)] != 0x81018003u ||
	    ring->ring[(size_t)((at + 4u) & ring->buf_mask)] != 0u) {
		printf("    job: CONTEXT_CONTROL is not 81018003 00000000   <-- wrong\n");
		bad++;
	}
	dw = ring->ring[(size_t)((at + 5u) & ring->buf_mask)];
	if (dw != (u32)PACKET3(PACKET3_FRAME_CONTROL, 0) ||
	    ring->ring[(size_t)((at + 6u) & ring->buf_mask)] != (u32)FRAME_CMD(0)) {
		printf("    job: FRAME_CONTROL start is wrong   <-- wrong\n");
		bad++;
	}
	dw = ring->ring[(size_t)((at + 7u) & ring->buf_mask)];
	if (dw != (u32)PACKET3(PACKET3_INDIRECT_BUFFER, 2)) {
		printf("    job: IB header %08X   <-- wrong\n", dw);
		bad++;
	}
	dw = ring->ring[(size_t)((at + 11u) & ring->buf_mask)];
	if (dw != (u32)PACKET3(PACKET3_FRAME_CONTROL, 0) ||
	    ring->ring[(size_t)((at + 12u) & ring->buf_mask)] != (u32)FRAME_CMD(1)) {
		printf("    job: FRAME_CONTROL end is wrong   <-- wrong\n");
		bad++;
	}
	dw = ring->ring[(size_t)((at + 13u) & ring->buf_mask)];
	if (dw != (u32)PACKET3(PACKET3_RELEASE_MEM, 6)) {
		printf("    job: fence header %08X is not RELEASE_MEM   <-- wrong\n", dw);
		bad++;
	}
	dw = ring->ring[(size_t)((at + 21u) & ring->buf_mask)];
	if (dw != (u32)PACKET3(PACKET3_SWITCH_BUFFER, 0) ||
	    ring->ring[(size_t)((at + 22u) & ring->buf_mask)] != 0u) {
		printf("    job: SWITCH_BUFFER is not after the fence   <-- wrong\n");
		bad++;
	}
	if (verbose && bad == 0)
		printf("    job vmid 1: frame dwords match\n");
	(void)verbose;
	return bad;
}

/* One submission: the IB packet, the fence behind it, the doorbell, and what the CP stub made of
 * them. `vmid` 0 is the run that has to work; 1 is the control the stub cannot follow. */
static unsigned int check_ib_submit(struct amdgpu_device *adev, u32 vmid, int verbose)
{
	struct amdgpu_ring *ring = &adev->gfx.gfx_ring[0];
	const u32 scratch = SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0);
	unsigned int followed, skipped, hits, bad = 0, k;
	u64 seq = STAGE_C_SEQ_BASE + vmid;
	u64 at, ib_addr, fence_addr;
	u32 length_dw = 0;
	int r;

	r = bc250_gfx_ib_ring_test_build(adev, &length_dw);
	if (r != 0 || length_dw != 3u) {
		printf("    vmid %u: bc250_gfx_ib_ring_test_build returned %d, %u dwords   <-- wrong\n",
		       vmid, r, length_dw);
		return 1;
	}
	ib_addr = bc250_gfx_ib_addr(adev);
	fence_addr = bc250_gfx_fence_addr(adev, STAGE_C_FENCE_SLOT);
	if (ib_addr == 0 || fence_addr == 0) {
		printf("    vmid %u: no IB page (0x%llX) or no fence slot (0x%llX)   <-- wrong\n",
		       vmid, (unsigned long long)ib_addr, (unsigned long long)fence_addr);
		return 1;
	}

	/* The IB half is asserted dword by dword in check_ib_dwords(); the fence half has its own
	 * checks in the stub, against the three dwords unit A's own driver emits. What is asserted
	 * here is the thing only the pair can be wrong about: that they are adjacent and in that
	 * order, which is the whole of what "submit" adds to "emit". */
	followed = backend_ib_followed();
	skipped = backend_ib_skipped();
	hits = backend_cp_stub_count();
	at = ring->wptr;

	r = bc250_gfx_submit_ib(ring, ib_addr, length_dw, vmid, fence_addr, seq,
				AMDGPU_FENCE_FLAG_INT);
	if (r != 0) {
		printf("    vmid %u: bc250_gfx_submit_ib returned %d   <-- wrong\n", vmid, r);
		return 1;
	}

	if (ring->ring[(size_t)(at & ring->buf_mask)] != (u32)PACKET3(PACKET3_INDIRECT_BUFFER, 2) ||
	    ring->ring[(size_t)((at + 4u) & ring->buf_mask)] != (u32)PACKET3(PACKET3_RELEASE_MEM, 6)) {
		printf("    vmid %u: the submission is not an INDIRECT_BUFFER followed by a"
		       " RELEASE_MEM (%08X, %08X)   <-- wrong\n", vmid,
		       ring->ring[(size_t)(at & ring->buf_mask)],
		       ring->ring[(size_t)((at + 4u) & ring->buf_mask)]);
		bad++;
	}

	if (vmid == 0) {
		if (backend_ib_followed() != followed + 1u || backend_ib_skipped() != skipped) {
			printf("    vmid 0: the stub followed %u IBs and skipped %u   <-- wrong:"
			       " expected one followed, none skipped\n",
			       backend_ib_followed() - followed, backend_ib_skipped() - skipped);
			bad++;
		}
		/* Three hits: the SET_UCONFIG_REG inside the buffer, the fetch that reached it, and
		 * the RELEASE_MEM behind the IB in the ring. Counting them is what separates "the
		 * scratch register happens to hold 0xDEADBEEF" from "this submission put it there". */
		if (backend_cp_stub_count() != hits + 3u) {
			printf("    vmid 0: the stub satisfied %u packets, expected 3   <-- wrong\n",
			       backend_cp_stub_count() - hits);
			bad++;
		}
		if (bc250_gfx_ib_ring_test_result(adev) != 0) {
			printf("    vmid 0: SCRATCH_REG0 reads %08X, so the IB was not fetched"
			       "   <-- wrong\n", bc250_shim_rreg(adev, scratch));
			bad++;
		}
	} else {
		if (backend_ib_skipped() != skipped + 1u || backend_ib_followed() != followed) {
			printf("    vmid %u: the stub followed the buffer   <-- wrong: it has no"
			       " address space for a non-zero VMID\n", vmid);
			bad++;
		}
		if (bc250_gfx_ib_ring_test_result(adev) == 0) {
			printf("    vmid %u: SCRATCH_REG0 took the IB's value although the buffer was"
			       " never fetched   <-- wrong\n", vmid);
			bad++;
		}
	}

	/* The fence is in the ring either way, so it lands either way: the IB is what the VMID
	 * decides, not the packet behind it. */
	if ((u32)bc250_gfx_fence_read(adev, STAGE_C_FENCE_SLOT) != (u32)seq) {
		printf("    vmid %u: the fence slot holds %08X, not %08X   <-- wrong\n", vmid,
		       (u32)bc250_gfx_fence_read(adev, STAGE_C_FENCE_SLOT), (u32)seq);
		bad++;
	}

	if (verbose && bad == 0) {
		printf("    vmid %u: IB 0x%llX x%u dwords, fence 0x%llX = %08X;", vmid,
		       (unsigned long long)ib_addr, length_dw, (unsigned long long)fence_addr,
		       (u32)seq);
		for (k = 0; k < 12u; k++)
			printf(" %08X", ring->ring[(size_t)((at + k) & ring->buf_mask)]);
		printf("\n");
	}
	return bad;
}

/* bc250_gmc_set_vmid_pd(): the two halves of the root, then the invalidation. */
static unsigned int check_vmid_pd(struct amdgpu_device *adev, u32 vmid, u32 lo_offset,
				  u32 hi_offset, u64 pd_phys, int verbose)
{
	const u32 req_offset = SOC15_REG_OFFSET(GC, 0, mmGCVM_INVALIDATE_ENG17_REQ) * 4u;
	const u32 ack_offset = SOC15_REG_OFFSET(GC, 0, mmGCVM_INVALIDATE_ENG17_ACK) * 4u;
	u32 v[4], req, per_vmid;
	unsigned int bad = 0, n;
	int r;

	/* The acknowledge register is the hardware answering, and a replayed register file never
	 * does: without this the poll would run out its 100 000 iterations and return BC250_ETIME.
	 * All ones rather than the one bit, so that a call which invalidated the wrong VMID would
	 * still be caught by the REQ check below and not by the poll. */
	backend_poke(ack_offset, 0xFFFFFFFFu);

	r = bc250_gmc_set_vmid_pd(adev, vmid, pd_phys, 0);
	if (r != 0) {
		printf("    vmid %u: bc250_gmc_set_vmid_pd returned %d   <-- wrong\n", vmid, r);
		return 1;
	}

	n = writes_to(lo_offset, v, (unsigned int)ARRAY_SIZE(v));
	if (n == 0 || v[n - 1u] != lower_32_bits(pd_phys | AMDGPU_PTE_VALID)) {
		printf("    vmid %u: PAGE_TABLE_BASE_ADDR_LO32 (0x%05X) last written %08X, expected"
		       " %08X   <-- wrong\n", vmid, lo_offset, n ? v[n - 1u] : 0,
		       lower_32_bits(pd_phys | AMDGPU_PTE_VALID));
		bad++;
	}
	n = writes_to(hi_offset, v, (unsigned int)ARRAY_SIZE(v));
	if (n == 0 || v[n - 1u] != upper_32_bits(pd_phys | AMDGPU_PTE_VALID)) {
		printf("    vmid %u: PAGE_TABLE_BASE_ADDR_HI32 (0x%05X) last written %08X, expected"
		       " %08X   <-- wrong\n", vmid, hi_offset, n ? v[n - 1u] : 0,
		       upper_32_bits(pd_phys | AMDGPU_PTE_VALID));
		bad++;
	}

	n = writes_to(req_offset, v, (unsigned int)ARRAY_SIZE(v));
	req = n ? v[n - 1u] : 0;
	per_vmid = (req & GCVM_INVALIDATE_ENG0_REQ__PER_VMID_INVALIDATE_REQ_MASK) >>
		   GCVM_INVALIDATE_ENG0_REQ__PER_VMID_INVALIDATE_REQ__SHIFT;
	if (n == 0 || per_vmid != (1u << vmid)) {
		printf("    vmid %u: INVALIDATE_ENG17_REQ last written %08X, PER_VMID %04X, expected"
		       " %04X   <-- wrong\n", vmid, req, per_vmid, 1u << vmid);
		bad++;
	}
	if (verbose && bad == 0)
		printf("    vmid %2u: 0x%05X = %08X, 0x%05X = %08X, ENG17_REQ = %08X\n", vmid,
		       lo_offset, lower_32_bits(pd_phys | AMDGPU_PTE_VALID), hi_offset,
		       upper_32_bits(pd_phys | AMDGPU_PTE_VALID), req);
	return bad;
}

static unsigned int check_stage_c(struct amdgpu_device *adev, int verbose)
{
	static const struct { const char *what; u32 vmid; u64 pd; } refuse[] = {
		{ "vmid 0, the GART's own",         0u,  0x0000000012345000ULL },
		{ "vmid 16, past the hub",          16u, 0x0000000012345000ULL },
		{ "a root that is not page aligned", 1u, 0x0000000012345800ULL },
	};
	struct amdgpu_ring *gfx = &adev->gfx.gfx_ring[0];
	unsigned int bad = 0, k, before;
	u64 ib_addr;
	int r;

	printf("\n== stage C: the indirect buffer and a VMID's page directory ==\n");
	printf("  no trace holds either of these, so they are checked against AMD's own packet and\n"
	       "  register names; the end-to-end run goes through the CP stub's IB support\n");

	/* The sizes, which are also how a caller asks whether a ring takes an IB at all. */
	if (bc250_gfx_ib_size(gfx) != 4u) {
		printf("    bc250_gfx_ib_size(gfx) is %u, not 4   <-- wrong\n",
		       bc250_gfx_ib_size(gfx));
		bad++;
	}
	if (bc250_gfx_ib_size(&adev->gfx.compute_ring[0]) != 0u ||
	    bc250_gfx_ib_size(&adev->gfx.kiq[0].ring) != 0u || bc250_gfx_ib_size(NULL) != 0u) {
		printf("    bc250_gfx_ib_size() answers for a ring this file has no IB emitter for"
		       "   <-- wrong\n");
		bad++;
	}

	r = bc250_gfx_ib_page_alloc(adev);
	if (r != 0) {
		printf("    bc250_gfx_ib_page_alloc returned %d   <-- wrong\n", r);
		return bad + 1u;
	}
	if (bc250_gfx_fence_page_alloc(adev) != 0) {
		printf("    bc250_gfx_fence_page_alloc failed   <-- wrong\n");
		return bad + 1u;
	}
	ib_addr = bc250_gfx_ib_addr(adev);

	/* Four dwords, at three VMIDs and two addresses, so that neither field can be reading the
	 * other's value by accident. */
	bad += check_ib_dwords("vmid 0, the IB page", gfx, ib_addr, 3u, 0u, verbose);
	bad += check_ib_dwords("vmid 1, the IB page", gfx, ib_addr, 3u, 1u, verbose);
	bad += check_ib_dwords("vmid 15, a 40-bit address", gfx, 0x000000AB12345678ULL & ~3ULL,
			       0xFFFFFu, 15u, verbose);
	bad += check_ib_refusals(adev, ib_addr, verbose);

	/* End to end, then the control at a VMID the stub has no address space for. */
	bad += check_ib_submit(adev, 0u, verbose);
	bad += check_ib_submit(adev, 1u, verbose);
	bad += check_shader_addr();
	bad += check_job_submit(adev, verbose);

	/* The page directory. The offsets are the CONTEXTn names; the code reaches the same
	 * registers as CONTEXT0 plus hub->ctx_addr_distance * vmid, so a mistake in either the
	 * distance or the names shows up here. */
	bad += check_vmid_pd(adev, 1u, SOC15_REG_OFFSET(GC, 0, mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_LO32) * 4u,
			     SOC15_REG_OFFSET(GC, 0, mmGCVM_CONTEXT1_PAGE_TABLE_BASE_ADDR_HI32) * 4u,
			     0x000000F5FF123000ULL, verbose);
	bad += check_vmid_pd(adev, 15u, SOC15_REG_OFFSET(GC, 0, mmGCVM_CONTEXT15_PAGE_TABLE_BASE_ADDR_LO32) * 4u,
			     SOC15_REG_OFFSET(GC, 0, mmGCVM_CONTEXT15_PAGE_TABLE_BASE_ADDR_HI32) * 4u,
			     0x000000F5FF456000ULL, verbose);

	for (k = 0; k < ARRAY_SIZE(refuse); k++) {
		before = backend_write_count();

		r = bc250_gmc_set_vmid_pd(adev, refuse[k].vmid, refuse[k].pd, 0);
		if (r == 0) {
			printf("    %-34s accepted   <-- wrong\n", refuse[k].what);
			bad++;
		} else if (backend_write_count() != before) {
			printf("    %-34s refused but wrote %u registers   <-- wrong\n",
			       refuse[k].what, backend_write_count() - before);
			bad++;
		} else if (verbose) {
			printf("    %-34s refused (%d), nothing written\n", refuse[k].what, r);
		}
	}

	/* Both pages go back, so that what runs after this sees the device it would have seen: the
	 * teardown-and-rerun arm below is about the addresses a second bring-up gets. */
	bc250_gfx_ib_page_free(adev);
	bc250_gfx_fence_page_free(adev);
	printf("  %u wrong\n", bad);
	return bad;
}

/* The fifth declared model: the SDMA engines' own write pointer, measured in E15. backend_mem.h
 * sets out the four things the sweeps showed. The pointer survives without being declared - it is
 * what the hardware does, so the backend does it for every SDMA ring - and what this call adds is
 * the ability to ANSWER a read of the pointer registers with it, which needs to know which offset
 * is which. Every offset comes from AMD's headers through bc250_sdma_reg_offset(). */
static void declare_sdma_pointer_state(struct amdgpu_device *adev, int i)
{
	struct backend_sdma_regs r;

	memset(&r, 0, sizeof(r));
	r.rb_rptr           = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR) * 4u;
	r.rb_rptr_hi        = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_RPTR_HI) * 4u;
	r.rb_wptr           = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR) * 4u;
	r.rb_wptr_hi        = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_GFX_RB_WPTR_HI) * 4u;
	/* The fetch pointer has no GFX in its name; regcalc says mmSDMA0_GFX_RB_RPTR_FETCH does not
	 * exist, and this one does. Nothing in driver/shim reads it, and the model answers it anyway
	 * so that the three pointer registers agree with each other. */
	r.rb_rptr_fetch     = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_RB_RPTR_FETCH) * 4u;
	r.rb_rptr_fetch_hi  = bc250_sdma_reg_offset(adev, (u32)i, mmSDMA0_RB_RPTR_FETCH_HI) * 4u;
	backend_add_sdma_pointer_state(&adev->sdma.instance[i].ring, &r);
}

static void register_rings(struct amdgpu_device *adev)
{
	u32 i;
	int k;

	backend_ring_register(&adev->gfx.kiq[0].ring);
	for (i = 0; i < adev->gfx.num_compute_rings; i++)
		backend_ring_register(&adev->gfx.compute_ring[i]);
	for (i = 0; i < adev->gfx.num_gfx_rings; i++)
		backend_ring_register(&adev->gfx.gfx_ring[i]);

	/* The SDMA rings too, since E15. Nothing submits on them in most of this file, so before
	 * that arm they were left out and their doorbells resolved to no ring at all; a model of what
	 * the engines remember cannot be checked against a ring the backend has never heard of. */
	for (k = 0; k < adev->sdma.num_instances; k++) {
		backend_ring_register(&adev->sdma.instance[k].ring);
		declare_sdma_pointer_state(adev, k);
	}
}

/* Opt-in CP1 tracing still passes the complete Linux write-trace oracle. */
static int trace_cp1;
static unsigned int checkpoint_count, checkpoint_bad;
static void check_checkpoint(const char *phase)
{
	static const char *expected[] = {
		"scheduler", "scheduler-read", "scheduler-write", "scheduler-done", "clear-mqd", "select-queue", "build-mqd",
		"disable-wptr-poll", "read-active", "program-hqd", "activate-hqd",
		"restore-selection", "complete"
	};
	if (checkpoint_count >= sizeof(expected) / sizeof(expected[0]) ||
	    strcmp(phase, expected[checkpoint_count]) != 0)
		checkpoint_bad++;
	checkpoint_count++;
}

static int traced_hw_init(struct amdgpu_device *adev)
{
	bool remapped = false;
	unsigned int step;
	int rc;
	checkpoint_count = checkpoint_bad = 0;
	rc = bc250_gfx_init_golden_registers(adev);
	if (!rc) rc = bc250_gfx_grbm_cam_probe(adev, &remapped);
	if (!rc) rc = bc250_gfx_constants_init(adev);
	if (!rc) rc = bc250_gfx_rlc_resume(adev);
	for (step = BC250_CP_KIQ_INIT; !rc && step <= BC250_CP_COMPUTE_TEST; step++)
		rc = bc250_gfx_cp_resume_step_traced(adev, step, check_checkpoint);
	printf("  CP1 checkpoints: %u, ordering errors: %u\n", checkpoint_count, checkpoint_bad);
	if (checkpoint_count != 13 || checkpoint_bad) return BC250_EIO;
	return rc;
}

static void run_one(const char *title, const struct run_opts *opt, struct amdgpu_device *adev,
		    struct result *res, int verbose)
{
	struct amdgpu_bo gart_bo;
	struct bc250_gmc_inputs gin;
	struct bc250_gfx_inputs fin;

	memset(adev, 0, sizeof(*adev));
	memset(&gart_bo, 0, sizeof(gart_bo));
	memset(&gin, 0, sizeof(gin));
	memset(&fin, 0, sizeof(fin));
	memset(res, 0, sizeof(*res));
	adev->dev = (void *)"BC250-A";
	adev->doorbell.base = UNITA_DOORBELL_BASE;

	backend_reset_state();
	backend_reset_writes();
	backend_mem_reset();
	backend_cp_stub_enable(opt->cp_stub);
	backend_clear_aliases();

	/* The memory layout, read off the hardware exactly as M4 does it. Only reads. */
	gin.gart_table_mc = UNITA_GART_TABLE_MC;
	gin.dummy_page_dma = (u64)UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32 << 12;
	gin.noretry = true;
	if (bc250_gmc_setup(adev, &gin, &gart_bo) != 0) {
		printf("\n== %s ==\n  bc250_gmc_setup failed; cannot learn the memory layout\n", title);
		res->gfx_setup_rc = -1;
		return;
	}
	gin.mem_scratch_mc = ((u64)UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB << 12)
			     - adev->vm_manager.vram_base_offset + adev->gmc.vram_start;
	(void)bc250_gmc_setup(adev, &gin, &gart_bo);

	/* VRAM allocations start at the frame buffer, GART-mapped ones at the GART aperture. Both
	 * come from the hardware through the setup above, so every address this test hands out is an
	 * address unit A's memory controller would have accepted. */
	backend_mem_set_bases(adev->gmc.vram_start, adev->gmc.gart_start);

	/* The one declared hardware aliasing, see point 3 of the header comment. It is registered
	 * here because the two offsets are mmVGT_ESGS_RING_SIZE_UMD and mmVGT_ESGS_RING_SIZE resolved
	 * through AMD's own headers over adev->reg_offset, which bc250_gmc_setup() has just filled
	 * in - not numbers copied out of the trace lines. */
	backend_add_alias(SOC15_REG_OFFSET(GC, 0, mmVGT_ESGS_RING_SIZE_UMD) * 4u,
			  SOC15_REG_OFFSET(GC, 0, mmVGT_ESGS_RING_SIZE) * 4u);

	/* Tell the CP stub which register a ring test is allowed to name, from AMD's headers rather
	 * than from a number, so that it can reject a malformed packet instead of applying it. */
	backend_cp_stub_expect(SOC15_REG_OFFSET(GC, 0, mmSCRATCH_REG0));

	/* The one declared hardware reaction; see backend_add_reaction() for what it is for. It
	 * changes nothing in the first bring-up, which never writes CP_HQD_DEQUEUE_REQUEST because
	 * CP_HQD_ACTIVE reads 0 on a cold boot, and the comparison above is unaffected either way:
	 * a reaction is applied to the register state and is never recorded as a write. */
	declare_dequeue_reaction(adev);

	/* The MEC's own fetch state, the fourth declared model, measured in E12 run 002. It has to be
	 * on before the first bring-up, because what it reproduces is the first run's engine state
	 * surviving into the second. Every offset comes from AMD's headers over adev->reg_offset. */
	declare_mec_fetch_state(adev);

	/* Stage 0, before anything else touches the GPU. rmmio_base stays 0: the only thing it feeds
	 * is rmmio_remap.bus_addr, which no register carries and the trace therefore cannot show. */
	res->stage0_bad = check_stage0(adev, verbose);

	fin.max_shader_engines = UNITA_MAX_SHADER_ENGINES;
	fin.max_sh_per_se = UNITA_MAX_SH_PER_SE;
	fin.max_cu_per_sh = UNITA_MAX_CU_PER_SH;
	fin.max_backends_per_se = UNITA_MAX_BACKENDS_PER_SE;
	fin.async_gfx_ring = opt->async_gfx_ring;
	fin.pp_gfxoff = opt->pp_gfxoff;

	res->gfx_setup_rc = bc250_gfx_setup(adev, &fin);
	res->sdma_setup_rc = res->gfx_setup_rc ? res->gfx_setup_rc : bc250_sdma_setup(adev);
	if (res->sdma_setup_rc == 0)
		register_rings(adev);

	/* The setup reads registers and allocates; the record starts at the first write of the
	 * bring-up, which is the first entry of the golden table. */
	backend_reset_writes();
	res->gfx_rc = res->sdma_setup_rc ? res->sdma_setup_rc :
		(trace_cp1 ? traced_hw_init(adev) : bc250_gfx_hw_init(adev));
	res->sdma_rc = res->gfx_rc ? res->gfx_rc : bc250_sdma_hw_init(adev);

	res->doorbells = backend_doorbell_count();
	res->stub_hits = backend_cp_stub_count();
	res->stub_rejects = backend_cp_stub_rejects();
	res->packets = backend_packet_count();

	printf("\n== %s ==\n", title);
	printf("  gfx_setup %d, sdma_setup %d, gfx_hw_init %d, sdma_hw_init %d\n",
	       res->gfx_setup_rc, res->sdma_setup_rc, res->gfx_rc, res->sdma_rc);

	compare_phase("bring-up", g_trace, g_trace_count, &res->bringup);

	if (res->sdma_rc == 0)
		res->address_failures = check_addresses(adev, verbose);

	/* Stage 10, a second later on unit A: the fence driver and the late init turning the CP and
	 * SDMA interrupt sources on, and the self-ring doorbell aperture that nv_common_hw_init()
	 * opens between them. Same adev, same register state - this is what the next thing the driver
	 * does writes, not a fresh start. */
	if (res->sdma_rc != 0) {
		res->irq_rc = res->sdma_rc;
	} else {
		backend_reset_writes();
		res->irq_rc = bc250_irq_init_mec_pipes(adev);
		if (res->irq_rc == 0)
			res->irq_rc = opt->irq_per_ring ? irq_hw_init_per_ring(adev)
							: bc250_irq_hw_init(adev);
		if (res->irq_rc == 0)
			res->irq_rc = bc250_nbio_enable_doorbell_selfring_aperture(adev, true);
		if (res->irq_rc == 0)
			res->irq_rc = bc250_irq_late_init(adev);

		printf("  irq_init_mec_pipes + irq_hw_init + selfring aperture + irq_late_init: %d\n",
		       res->irq_rc);
		compare_phase("interrupts", g_trace_irq, g_trace_irq_count, &res->irq);
	}

	res->unknown_reads = backend_unknown_reads();
}

/* ---------------------------------------------------------------------------------------------
 * The dumps
 *
 * What the sequence puts in memory, written out so that it can be read against the kernel's own
 * structure definitions (driver/amdgpu-import/v10_structs.h) and packet definitions
 * (driver/amdgpu-import/nvd.h) rather than taken on trust.
 * ------------------------------------------------------------------------------------------- */

struct mqd_field {
	const char *name;
	unsigned int offset;            /* byte offset inside struct v10_compute_mqd */
};

#define MQD_FIELD(f) { #f, (unsigned int)offsetof(struct v10_compute_mqd, f) }

/* Exactly the fields gfx_v10_0_compute_mqd_init() assigns, in the order it assigns them. Anything
 * outside this set must still be zero, and dump_compute_mqd() checks that. */
static const struct mqd_field g_mqd_fields[] = {
	MQD_FIELD(header),
	MQD_FIELD(compute_pipelinestat_enable),
	MQD_FIELD(compute_static_thread_mgmt_se0),
	MQD_FIELD(compute_static_thread_mgmt_se1),
	MQD_FIELD(compute_static_thread_mgmt_se2),
	MQD_FIELD(compute_static_thread_mgmt_se3),
	MQD_FIELD(compute_misc_reserved),
	MQD_FIELD(cp_hqd_eop_base_addr_lo),
	MQD_FIELD(cp_hqd_eop_base_addr_hi),
	MQD_FIELD(cp_hqd_eop_control),
	MQD_FIELD(cp_hqd_pq_doorbell_control),
	MQD_FIELD(cp_hqd_dequeue_request),
	MQD_FIELD(cp_hqd_pq_rptr),
	MQD_FIELD(cp_hqd_pq_wptr_lo),
	MQD_FIELD(cp_hqd_pq_wptr_hi),
	MQD_FIELD(cp_mqd_base_addr_lo),
	MQD_FIELD(cp_mqd_base_addr_hi),
	MQD_FIELD(cp_mqd_control),
	MQD_FIELD(cp_hqd_pq_base_lo),
	MQD_FIELD(cp_hqd_pq_base_hi),
	MQD_FIELD(cp_hqd_pq_control),
	MQD_FIELD(cp_hqd_pq_rptr_report_addr_lo),
	MQD_FIELD(cp_hqd_pq_rptr_report_addr_hi),
	MQD_FIELD(cp_hqd_pq_wptr_poll_addr_lo),
	MQD_FIELD(cp_hqd_pq_wptr_poll_addr_hi),
	MQD_FIELD(cp_hqd_vmid),
	MQD_FIELD(cp_hqd_persistent_state),
	MQD_FIELD(cp_hqd_ib_control),
	MQD_FIELD(cp_hqd_pipe_priority),
	MQD_FIELD(cp_hqd_queue_priority),
	MQD_FIELD(cp_hqd_active)
};

/* ---------------------------------------------------------------------------------------------
 * The compute MQDs against unit A's own
 *
 * evidence/linux/2026-09-21-E03-init-trace/rings/ holds what amdgpu actually left in memory on unit
 * A: amdgpu_mqd_comp_1.{0..3}.{0,1}.hex.txt, one 2048-byte MQD per compute ring, read back out of
 * debugfs. The register trace cannot reach these - a compute MQD is written by the CPU into memory
 * and handed to the CP by address - so this is the only evidence that says whether the MQDs this
 * driver builds are the ones amdgpu built.
 *
 * Two kinds of field cannot be compared and are listed rather than quietly skipped:
 *
 *   - the ten that carry an address, because this test allocates its own buffers;
 *   - cp_hqd_active, because the dumps were taken after the queues had been mapped and run. amdgpu
 *     writes 0 there for a compute ring (only the KIQ gets 1) and all eight dumps read 1, which is
 *     the CP writing it back. The MQD is the queue's save area, so the CP owns it once mapped.
 *
 * Everything else must be equal, and the same reasoning says why the dumps have non-zero dwords
 * outside the fields the MQD init assigns: those are the CP's too. They are counted and reported,
 * not compared.
 * ------------------------------------------------------------------------------------------- */

#define MQD_DWORDS  (sizeof(struct v10_compute_mqd) / 4u)

static const char *const g_mqd_address_fields[] = {
	"cp_hqd_eop_base_addr_lo",	"cp_hqd_eop_base_addr_hi",
	"cp_mqd_base_addr_lo",		"cp_mqd_base_addr_hi",
	"cp_hqd_pq_base_lo",		"cp_hqd_pq_base_hi",
	"cp_hqd_pq_rptr_report_addr_lo","cp_hqd_pq_rptr_report_addr_hi",
	"cp_hqd_pq_wptr_poll_addr_lo",	"cp_hqd_pq_wptr_poll_addr_hi",
	/* not an address, but owned by the CP after MAP_QUEUES; see the comment above */
	"cp_hqd_active"
};

static int mqd_field_excepted(const char *name)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(g_mqd_address_fields); i++)
		if (strcmp(name, g_mqd_address_fields[i]) == 0)
			return 1;
	return 0;
}

/* One `OFFSET dword dword dword dword` hex dump into an array. Returns the dword count, or -1. */
static int load_hex_dump(const char *path, u32 *out, unsigned int max)
{
	char line[512];
	unsigned int n = 0;
	FILE *f = fopen(path, "r");

	if (f == NULL)
		return -1;
	while (fgets(line, (int)sizeof(line), f) != NULL) {
		char *p = line;
		unsigned int off, v;
		int consumed;

		if (sscanf(p, "%x%n", &off, &consumed) != 1)
			continue;
		p += consumed;
		while (n < max && sscanf(p, " %x%n", &v, &consumed) == 1) {
			out[n++] = v;
			p += consumed;
		}
	}
	fclose(f);
	return (int)n;
}

static unsigned int compare_one_mqd(const char *path, const char *label,
				    const struct amdgpu_ring *ring, int verbose,
				    unsigned int *cp_written)
{
	static u32 real[MQD_DWORDS];
	const u32 *ours = (const u32 *)ring->mqd_ptr;
	unsigned char named[MQD_DWORDS];
	unsigned int i, bad = 0, excepted = 0, k;
	int n;

	memset(real, 0, sizeof(real));
	memset(named, 0, sizeof(named));

	n = load_hex_dump(path, real, (unsigned int)MQD_DWORDS);
	if (n < 0) {
		printf("  %-20s cannot read %s\n", label, path);
		return 1;
	}
	if ((unsigned int)n != MQD_DWORDS) {
		printf("  %-20s %s holds %d dwords, expected %u\n",
		       label, path, n, (unsigned int)MQD_DWORDS);
		return 1;
	}

	for (i = 0; i < ARRAY_SIZE(g_mqd_fields); i++) {
		unsigned int dw = g_mqd_fields[i].offset / 4u;

		named[dw] = 1;
		if (mqd_field_excepted(g_mqd_fields[i].name)) {
			excepted++;
			continue;
		}
		if (ours[dw] == real[dw])
			continue;
		bad++;
		printf("    %-20s %-32s ours %08X   unit A %08X\n",
		       label, g_mqd_fields[i].name, ours[dw], real[dw]);
	}

	for (k = 0; k < MQD_DWORDS; k++)
		if (!named[k] && real[k] != 0)
			(*cp_written)++;

	if (verbose && bad == 0)
		printf("    %-20s %u fields equal, %u excepted\n", label,
		       (unsigned int)ARRAY_SIZE(g_mqd_fields) - excepted, excepted);
	return bad;
}

/*
 * The dumps are named amdgpu_mqd_comp_<me>.<pipe>.<queue>.hex.txt and a directory listing sorts them
 * by pipe and then queue. The rings are not in that order: amdgpu fills compute_ring[] queue-major,
 * so ring 1 is pipe 1 queue 0 and ring 4 is pipe 0 queue 1. Pairing file i with ring i therefore
 * compares the wrong pairs, which is how this started - six doorbell offsets appeared to differ and
 * were in fact the same eight offsets read in two different orders.
 *
 * So each file is matched to the ring that says it is that me, pipe and queue, and a file with no
 * such ring is an error rather than a skip. Nothing here depends on the order of either list.
 */
static unsigned int compare_mqds_with_unit_a(const char *dir, struct amdgpu_device *adev,
					     int verbose)
{
	static const struct { unsigned int me, pipe, queue; } id[] = {
		{ 1, 0, 0 }, { 1, 0, 1 }, { 1, 1, 0 }, { 1, 1, 1 },
		{ 1, 2, 0 }, { 1, 2, 1 }, { 1, 3, 0 }, { 1, 3, 1 }
	};
	unsigned int bad = 0, cp_written = 0, i, j;

	printf("\n== the compute MQDs against unit A's own ==\n");
	if (adev->gfx.num_compute_rings != ARRAY_SIZE(id)) {
		printf("  %u compute rings but %u dumps; not compared\n",
		       adev->gfx.num_compute_rings, (unsigned int)ARRAY_SIZE(id));
		return 1;
	}

	for (i = 0; i < ARRAY_SIZE(id); i++) {
		char path[1024], label[64];
		struct amdgpu_ring *ring = NULL;

		for (j = 0; j < adev->gfx.num_compute_rings; j++) {
			struct amdgpu_ring *r = &adev->gfx.compute_ring[j];

			if (r->me == id[i].me && r->pipe == id[i].pipe && r->queue == id[i].queue) {
				ring = r;
				break;
			}
		}
		if (ring == NULL) {
			printf("    no ring is me %u pipe %u queue %u   <-- wrong\n",
			       id[i].me, id[i].pipe, id[i].queue);
			bad++;
			continue;
		}

		sprintf(path, "%s/amdgpu_mqd_comp_%u.%u.%u.hex.txt",
			dir, id[i].me, id[i].pipe, id[i].queue);
		sprintf(label, "me%u p%u q%u = ring %u", id[i].me, id[i].pipe, id[i].queue, j);
		bad += compare_one_mqd(path, label, ring, verbose, &cp_written);
	}

	printf("  %u of %u fields per MQD compared, %u excepted (10 addresses + cp_hqd_active)\n",
	       (unsigned int)(ARRAY_SIZE(g_mqd_fields) - ARRAY_SIZE(g_mqd_address_fields)),
	       (unsigned int)ARRAY_SIZE(g_mqd_fields),
	       (unsigned int)ARRAY_SIZE(g_mqd_address_fields));
	printf("  dwords the CP wrote into unit A's MQDs outside those fields: %u over 8 MQDs\n",
	       cp_written);
	printf("  %u fields differ\n", bad);
	return bad;
}

static unsigned int dump_compute_mqd(FILE *f, const char *label, const struct amdgpu_ring *ring)
{
	const u32 *raw = (const u32 *)ring->mqd_ptr;
	unsigned int dwords = (unsigned int)(sizeof(struct v10_compute_mqd) / 4u);
	unsigned int i, k, stray = 0;
	unsigned char named[sizeof(struct v10_compute_mqd) / 4u];

	memset(named, 0, sizeof(named));

	fprintf(f, "%s: struct v10_compute_mqd at MC 0x%llX, %u bytes\n",
		label, (unsigned long long)ring->mqd_gpu_addr,
		(unsigned int)sizeof(struct v10_compute_mqd));
	fprintf(f, "  me %u pipe %u queue %u, doorbell index 0x%X\n",
		ring->me, ring->pipe, ring->queue, ring->doorbell_index);
	fprintf(f, "  fields gfx_v10_0_compute_mqd_init() assigns (offsets from"
		   " driver/amdgpu-import/v10_structs.h):\n");
	for (i = 0; i < ARRAY_SIZE(g_mqd_fields); i++) {
		unsigned int dw = g_mqd_fields[i].offset / 4u;

		named[dw] = 1;
		fprintf(f, "    +0x%04X  %-36s %08X\n",
			g_mqd_fields[i].offset, g_mqd_fields[i].name, raw[dw]);
	}

	fprintf(f, "  every other dword must be zero:");
	for (k = 0; k < dwords; k++) {
		if (named[k] || raw[k] == 0)
			continue;
		if (stray == 0)
			fprintf(f, "\n");
		stray++;
		fprintf(f, "    +0x%04X  UNEXPECTED NON-ZERO  %08X\n", k * 4u, raw[k]);
	}
	if (stray == 0)
		fprintf(f, " yes, all %u of them\n", dwords - (unsigned int)ARRAY_SIZE(g_mqd_fields));
	fprintf(f, "\n");
	return stray;
}

/* Names for the opcodes this window emits. Anything else prints as a number, which is itself the
 * finding: the sequence should emit nothing else. */
static const char *packet_name(unsigned int op)
{
	switch (op) {
	case PACKET3_NOP:		return "NOP";
	case PACKET3_SET_BASE:		return "SET_BASE";
	case PACKET3_CLEAR_STATE:	return "CLEAR_STATE";
	case PACKET3_CONTEXT_CONTROL:	return "CONTEXT_CONTROL";
	case PACKET3_PREAMBLE_CNTL:	return "PREAMBLE_CNTL";
	case PACKET3_SET_CONTEXT_REG:	return "SET_CONTEXT_REG";
	case PACKET3_SET_UCONFIG_REG:	return "SET_UCONFIG_REG";
	case PACKET3_SET_RESOURCES:	return "SET_RESOURCES";
	case PACKET3_MAP_QUEUES:	return "MAP_QUEUES";
	default:			return NULL;
	}
}

/* Walk a ring from dword 0 to its write pointer, decoding PACKET3 headers. `body_lines` caps how
 * many body dwords are printed per packet; the clear-state packets are hundreds of dwords long and
 * the point of printing them is the shape, plus the first and last few values. */
static void dump_ring_pm4(FILE *f, const char *label, const struct amdgpu_ring *ring,
			  unsigned int body_lines)
{
	u64 at = 0;
	unsigned int packets = 0;

	fprintf(f, "%s: ring at MC 0x%llX, %u bytes, write pointer %llu dwords\n",
		label, (unsigned long long)ring->gpu_addr, ring->ring_size,
		(unsigned long long)ring->wptr);

	while (at < ring->wptr) {
		u32 header = ring->ring[(size_t)(at & ring->buf_mask)];
		unsigned int op, count, body, k, shown;
		const char *name;

		if (header == ring->funcs->nop) {
			u64 run = 0;

			while (at + run < ring->wptr &&
			       ring->ring[(size_t)((at + run) & ring->buf_mask)] == ring->funcs->nop)
				run++;
			fprintf(f, "  [%5llu] %llu x NOP pad (%08X)\n",
				(unsigned long long)at, (unsigned long long)run, ring->funcs->nop);
			at += run;
			continue;
		}
		if (CP_PACKET_GET_TYPE(header) != PACKET_TYPE3) {
			fprintf(f, "  [%5llu] not a type-3 packet: %08X\n",
				(unsigned long long)at, header);
			break;
		}

		op = CP_PACKET3_GET_OPCODE(header);
		count = CP_PACKET_GET_COUNT(header);
		body = count + 1u;
		name = packet_name(op);
		packets++;

		fprintf(f, "  [%5llu] %08X  %-20s count %u, %u body dwords\n",
			(unsigned long long)at, header,
			name ? name : "UNKNOWN OPCODE", count, body);
		if (name == NULL)
			fprintf(f, "            opcode 0x%02X is not one this sequence should emit\n", op);

		shown = body < body_lines ? body : body_lines;
		for (k = 0; k < shown; k++)
			fprintf(f, "            +%-4u %08X\n", k,
				ring->ring[(size_t)((at + 1u + k) & ring->buf_mask)]);
		if (shown < body)
			fprintf(f, "            ... %u more, last %08X\n", body - shown,
				ring->ring[(size_t)((at + body) & ring->buf_mask)]);

		at += 1u + body;
	}
	fprintf(f, "  %u packets\n\n", packets);
}

static FILE *open_dump(const char *dir, const char *name)
{
	char path[1024];
	FILE *f;

	sprintf(path, "%s/%s", dir, name);
	f = fopen(path, "w");
	if (f == NULL)
		fprintf(stderr, "cannot write %s\n", path);
	else
		printf("  wrote %s\n", path);
	return f;
}

static unsigned int write_dumps(const char *dir, struct amdgpu_device *adev)
{
	unsigned int stray = 0;
	FILE *f;
	u32 i;

	printf("\n== dumps ==\n");

	f = open_dump(dir, "kiq-mqd.txt");
	if (f) {
		fprintf(f, "# KIQ and compute MQDs built by driver/shim/bc250_gfx.c, milestone M5 part B.\n"
			   "# Field offsets are offsetof() over struct v10_compute_mqd in\n"
			   "# driver/amdgpu-import/v10_structs.h, an unmodified copy of the kernel's.\n\n");
		stray += dump_compute_mqd(f, "KIQ", &adev->gfx.kiq[0].ring);
		for (i = 0; i < adev->gfx.num_compute_rings; i++) {
			char label[32];

			sprintf(label, "compute ring %u", i);
			stray += dump_compute_mqd(f, label, &adev->gfx.compute_ring[i]);
		}
		fclose(f);
	}

	f = open_dump(dir, "kiq-pm4.txt");
	if (f) {
		fprintf(f, "# The PM4 stream driver/shim/bc250_gfx.c puts on the KIQ ring, decoded with the\n"
			   "# opcode definitions of driver/amdgpu-import/nvd.h.\n"
			   "#\n"
			   "# Expected, in order: SET_RESOURCES, then one MAP_QUEUES per compute ring, then\n"
			   "# the SET_UCONFIG_REG of the ring test that follows them; then one MAP_QUEUES for\n"
			   "# the gfx ring and its ring test. Each submission is padded to 8 dwords.\n\n");
		dump_ring_pm4(f, "KIQ ring", &adev->gfx.kiq[0].ring, 8);
		fclose(f);
	}

	f = open_dump(dir, "gfx-clear-state.txt");
	if (f) {
		fprintf(f, "# The clear-state stream driver/shim/bc250_gfx.c puts on the gfx ring, from the\n"
			   "# imported gfx10_cs_data table (driver/amdgpu-import/clearstate_gfx10.h).\n"
			   "#\n"
			   "# RLC_CSIB_LENGTH in the trace is 0x%03X dwords; gfx_v10_0_get_csb_size() over the\n"
			   "# same table gives %u, and the buffer below is that plus the framing packets.\n\n",
			adev->gfx.rlc.clear_state_size, adev->gfx.rlc.clear_state_size);
		dump_ring_pm4(f, "gfx ring", &adev->gfx.gfx_ring[0], 4);
		fclose(f);
	}

	return stray;
}

/* ---------------------------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------------------------- */

/* Isolated register model for M349. Clearing busy on reset is a model
 * assumption, deliberately disabled in the stuck control; not hardware proof. */
static u32 reload_reset_offset, reload_busy_offset;
static char reload_accesses[16];
static unsigned int reload_access_count;
static int reload_clear_busy;
static void reload_access(char kind)
{
	if (reload_access_count < sizeof(reload_accesses)-1)
		reload_accesses[reload_access_count++] = kind;
	reload_accesses[reload_access_count] = 0;
}
static int reload_read_hook(u32 offset, u32 *value)
{
	(void)value;
	if (offset == reload_reset_offset) reload_access('R');
	return 0;
}
static void reload_reset_hook(u32 offset, u32 value)
{
	if (offset == reload_reset_offset) reload_access('W');
	if (reload_clear_busy && offset == reload_reset_offset && !(value & GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK))
		backend_poke(reload_busy_offset, 0);
}
static unsigned int check_rlc_reload_reset(struct amdgpu_device *adev)
{
	const u32 me_halt = CP_ME_CNTL__ME_HALT_MASK | CP_ME_CNTL__PFP_HALT_MASK | CP_ME_CNTL__CE_HALT_MASK;
	const u32 mec_halt = CP_MEC_CNTL__MEC_ME1_HALT_MASK | CP_MEC_CNTL__MEC_ME2_HALT_MASK;
	const u32 me_bits[] = {CP_ME_CNTL__ME_HALT_MASK, CP_ME_CNTL__PFP_HALT_MASK, CP_ME_CNTL__CE_HALT_MASK};
	const u32 mec_bits[] = {CP_MEC_CNTL__MEC_ME1_HALT_MASK, CP_MEC_CNTL__MEC_ME2_HALT_MASK};
	const u32 preserved = GRBM_SOFT_RESET__SOFT_RESET_CP_MASK;
	unsigned int k, variant, bad = 0;
	reload_reset_offset = SOC15_REG_OFFSET(GC, 0, mmGRBM_SOFT_RESET) * 4;
	reload_busy_offset = SOC15_REG_OFFSET(GC, 0, mmGRBM_STATUS2) * 4;
	backend_set_read_hook(reload_read_hook);
	for (variant = 0; variant < 2; variant++)
	for (k = 0; k < 11; k++) {
		int result, expected = k < 2 ? 0 : k < 9 ? BC250_EBUSY : k == 9 ? 1 : BC250_ETIME;
		unsigned int writes;
		backend_set_write_hook(reload_reset_hook);
		reload_clear_busy = k != 10;
		reload_access_count = 0;
		reload_accesses[0] = 0;
		backend_poke(SOC15_REG_OFFSET(GC, 0, mmRLC_CNTL)*4, k == 1 ? RLC_CNTL__RLC_ENABLE_F32_MASK : 0);
		backend_poke(reload_busy_offset, k == 0 ? 0 : GRBM_STATUS2__RLC_BUSY_MASK);
		backend_poke(SOC15_REG_OFFSET(GC, 0, mmCP_ME_CNTL)*4, me_halt & ~(k >= 2 && k < 5 ? me_bits[k-2] : 0));
		backend_poke(SOC15_REG_OFFSET(GC, 0, mmCP_MEC_CNTL)*4, mec_halt & ~(k >= 5 && k < 7 ? mec_bits[k-5] : 0));
		backend_poke(SOC15_REG_OFFSET(GC, 0, mmSDMA0_F32_CNTL)*4, k == 7 ? 0 : SDMA0_F32_CNTL__HALT_MASK);
		backend_poke(SOC15_REG_OFFSET(GC, 0, mmSDMA1_F32_CNTL)*4, k == 8 ? 0 : SDMA1_F32_CNTL__HALT_MASK);
		backend_poke(reload_reset_offset,preserved);
		backend_reset_writes();
		result = variant ? bc250_gfx_rlc_reload_reset_readback(adev) : bc250_gfx_rlc_reload_reset(adev);
		if (strcmp(reload_accesses, k < 9 ? "" : variant ? "RWRWR" : "RWRW") != 0) bad++;
		writes = backend_write_count();
		if (result != expected || writes != (k < 9 ? 0u : 2u)) bad++;
		if (k >= 9 && writes == 2) {
			const struct bc250_reg_write *w = backend_writes();
			if (w[0].byte_offset != reload_reset_offset || w[1].byte_offset != reload_reset_offset ||
			    w[0].value != (preserved | GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK) ||
			    w[1].value != preserved) bad++;
		}
	}
	backend_set_write_hook(NULL);
	backend_set_read_hook(NULL);
	printf("  RLC reload reset: 22 scenarios, %u failures (both access orders checked; busy-clear is modeled)\n",bad);
	return bad;
}

/* M355: compare original accesses with and without the observer. */
static u32 tlb_test_req, tlb_test_ack, tlb_test_sem;
static int tlb_test_stuck;
static unsigned int tlb_test_reads, tlb_test_writes, tlb_test_events, tlb_test_bad;
static u64 tlb_test_hash;
static void tlb_test_access(u32 offset, u32 value, u32 kind)
{
	tlb_test_hash = (tlb_test_hash * 1099511628211ULL) ^ offset;
	tlb_test_hash = (tlb_test_hash * 1099511628211ULL) ^ value ^ kind;
}
static int tlb_test_read(u32 offset, u32 *value)
{
	if (offset != tlb_test_req && offset != tlb_test_ack && offset != tlb_test_sem) return 0;
	*value = offset == tlb_test_sem ? 1 : offset == tlb_test_ack ? (tlb_test_stuck ? 0 : 1) : 0;
	tlb_test_reads++;
	tlb_test_access(offset,*value,1);
	return 1;
}
static void tlb_test_write(u32 offset, u32 value)
{
	tlb_test_writes++;
	tlb_test_access(offset,value,2);
}
static void tlb_test_observe(struct amdgpu_device *adev, const char *phase, u32 value)
{
	(void)adev;
	if (!strcmp(phase,"after-invalidate-request")) {
		if (tlb_test_events != 0 || tlb_test_writes != 1) tlb_test_bad++;
	} else if (!strcmp(phase,"after-invalidate-request-read")) {
		if (tlb_test_events != 1 || value != 0) tlb_test_bad++;
	} else if (!strcmp(phase,"after-invalidate-ack")) {
		if (value != (tlb_test_stuck ? 0u : 1u)) tlb_test_bad++;
	} else tlb_test_bad++;
	tlb_test_events++;
}
static unsigned int check_tlb_observation(struct amdgpu_device *adev)
{
	unsigned int hubid, stuck, variant, bad = 0;
	u32 saved_timeout = adev->usec_timeout;
	adev->usec_timeout = 3;
	backend_set_read_hook(tlb_test_read);
	backend_set_write_hook(tlb_test_write);
	for (hubid=0; hubid<2; hubid++) for (stuck=0; stuck<2; stuck++) {
		u32 vmhub = hubid ? AMDGPU_MMHUB0(0) : AMDGPU_GFXHUB(0);
		struct amdgpu_vmhub *hub = &adev->vmhub[vmhub];
		u64 baseline = 0;
		tlb_test_req = (hub->vm_inv_eng0_req + hub->eng_distance*17)*4;
		tlb_test_ack = (hub->vm_inv_eng0_ack + hub->eng_distance*17)*4;
		tlb_test_sem = (hub->vm_inv_eng0_sem + hub->eng_distance*17)*4;
		tlb_test_stuck = (int)stuck;
		for (variant=0; variant<2; variant++) {
			int result;
			tlb_test_hash=0; tlb_test_reads=0; tlb_test_writes=0; tlb_test_events=0; tlb_test_bad=0;
			backend_reset_writes();
			result = variant ? bc250_gmc_flush_gpu_tlb_observed(adev,0,vmhub,0,tlb_test_observe) :
				bc250_gmc_flush_gpu_tlb(adev,0,vmhub,0);
			if (result != (stuck ? BC250_ETIME : 0) || tlb_test_bad) bad++;
			if (tlb_test_reads != (stuck ? 4u : 2u) || tlb_test_writes != (hubid ? 2u : 1u)) bad++;
			if (variant) {
				if (tlb_test_hash != baseline || tlb_test_events != (hubid ? 2u : 3u)) bad++;
			} else { baseline=tlb_test_hash; if (tlb_test_events) bad++; }
			if (hubid) {
				const struct bc250_reg_write *w=backend_writes();
				if (backend_write_count()!=2 || w[1].byte_offset!=tlb_test_sem || w[1].value!=0) bad++;
			}
		}
	}
	adev->usec_timeout=saved_timeout;
	backend_set_read_hook(NULL); backend_set_write_hook(NULL);
	printf("  TLB observation: 8 scenarios, %u failures (identical traced/untraced register accesses)\n",bad);
	return bad;
}

/* M363: extraction must preserve the complete enable's register traffic.
 * This is a local ordering model, not proof that delayed visibility works on GPU. */
static u32 phase_req[2], phase_ack[2], phase_sem[2];
static unsigned int phase_reads, phase_requests, phase_events, phase_bad;
static int phase_read(u32 offset, u32 *value)
{
 unsigned int i;
 for (i=0;i<2;i++) {
  if (offset==phase_ack[i] || offset==phase_sem[i] || offset==phase_req[i]) {
   phase_reads++;
   *value = offset==phase_req[i] ? 0 : 1;
   return 1;
  }
 }
 return 0;
}
static void phase_write(u32 offset, u32 value)
{
 (void)value;
 if (offset==phase_req[0] || offset==phase_req[1]) phase_requests++;
}
static void phase_observe(struct amdgpu_device *adev, const char *phase, u32 value)
{
 static const char *expected[]={"startup-after-gfxhub-enable", "startup-after-mmhub-enable",
  "startup-after-fault-defaults"};
 (void)adev;
 if (phase_events>=3 || strcmp(phase,expected[phase_events]) || value!=0) phase_bad++;
 phase_events++;
}
static unsigned int check_gart_phase_extraction(void)
{
 struct amdgpu_device model;
 struct amdgpu_bo bo;
 struct bc250_gmc_inputs in;
 struct bc250_reg_write baseline[1024];
 unsigned int variant,i,count=0,bad=0;
 for (variant=0;variant<2;variant++) {
  memset(&model,0,sizeof(model));memset(&bo,0,sizeof(bo));memset(&in,0,sizeof(in));
  backend_set_read_hook(NULL);backend_set_write_hook(NULL);
  backend_reset_state();backend_reset_writes();
  in.gart_table_mc=UNITA_GART_TABLE_MC;
  in.dummy_page_dma=(u64)UNITA_TRACED_FAULT_DEFAULT_ADDR_LO32<<12;
  in.noretry=true;
  if (bc250_gmc_setup(&model,&in,&bo)!=0) return 1;
  in.mem_scratch_mc=((u64)UNITA_TRACED_SYS_APERTURE_DEFAULT_LSB<<12)
    -model.vm_manager.vram_base_offset+model.gmc.vram_start;
  if (bc250_gmc_setup(&model,&in,&bo)!=0) return 1;
  for(i=0;i<2;i++) {
   struct amdgpu_vmhub *hub=&model.vmhub[i ? AMDGPU_MMHUB0(0) : AMDGPU_GFXHUB(0)];
   phase_req[i]=(hub->vm_inv_eng0_req+hub->eng_distance*17)*4;
   phase_ack[i]=(hub->vm_inv_eng0_ack+hub->eng_distance*17)*4;
   phase_sem[i]=(hub->vm_inv_eng0_sem+hub->eng_distance*17)*4;
  }
  phase_reads=phase_requests=phase_events=phase_bad=0;
  backend_set_read_hook(phase_read);backend_set_write_hook(phase_write);
  if (!variant) {
   if(bc250_gmc_gart_enable(&model)!=0) bad++;
   count=backend_write_count();
   if(count>sizeof(baseline)/sizeof(baseline[0])) {bad++;break;}
   memcpy(baseline,backend_writes(),count*sizeof(baseline[0]));
  } else {
   if(bc250_gmc_gart_configure_observed(&model,phase_observe)!=0) bad++;
   if(phase_reads || phase_requests || phase_bad || phase_events!=3) bad++;
   if(bc250_gmc_flush_gpu_tlb(&model,0,AMDGPU_MMHUB0(0),0)!=0) bad++;
   if(bc250_gmc_flush_gpu_tlb(&model,0,AMDGPU_GFXHUB(0),0)!=0) bad++;
   if(count!=backend_write_count()) bad++;
   else for(i=0;i<count;i++) {
    const struct bc250_reg_write *actual=&backend_writes()[i];
    if(actual->byte_offset!=baseline[i].byte_offset || actual->value!=baseline[i].value) bad++;
   }
  }
  if(phase_requests!=2 || phase_reads!=4) bad++;
 }
 backend_set_read_hook(NULL);backend_set_write_hook(NULL);
 printf("  GART phase extraction: 2 paths, %u writes compared, %u failures\n",count,bad);
 return bad;
}

/* M369: real AMD-derived queue preparation against two-instance hardware
 * models. No RLC safe-mode or physical DMA completion is modeled here. */
static u32 sq_reg[6], sq_seed[6];
static unsigned int sq_mode, sq_freeze_reads;
static int sq_read(u32 offset, u32 *value)
{
 unsigned int i;
 for(i=0;i<6;i++) if(offset==sq_reg[i]) {
  *value=sq_seed[i];
  if(i==2) {sq_freeze_reads++;if(sq_mode==0)*value|=SDMA0_FREEZE__FROZEN_MASK;}
  if(i==5)*value=sq_mode==1?0x3FF:0;
  return 1;
 }
 return 0;
}
static unsigned int check_sdma_quiescence(struct amdgpu_device *adev)
{
 unsigned int instance,mode,i,bad=0;
 u32 timeout=adev->usec_timeout;
 u32 names[]={mmSDMA0_GFX_RB_CNTL,mmSDMA0_GFX_IB_CNTL,mmSDMA0_FREEZE,
  mmSDMA0_F32_CNTL,mmSDMA0_CNTL,mmSDMA0_STATUS1_REG};
 adev->usec_timeout=3;
 for(instance=0;instance<2;instance++) for(mode=0;mode<3;mode++) {
  const struct bc250_reg_write *w;
  u32 expected[5];int result;
  for(i=0;i<6;i++) {sq_reg[i]=bc250_sdma_reg_offset(adev,instance,names[i])*4;sq_seed[i]=0xA5000000u;}
  sq_seed[0]|=SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK;
  sq_seed[1]|=SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK;
  sq_seed[4]|=SDMA0_CNTL__UTC_L1_ENABLE_MASK;
  sq_mode=mode;sq_freeze_reads=0;
  backend_set_read_hook(sq_read);backend_reset_writes();
  result=bc250_sdma_quiesce_instance(adev,instance);
  if(result!=(mode==2?BC250_ETIME:0))bad++;
  if(sq_freeze_reads!=(mode==0?2u:4u))bad++;
  if(backend_write_count()!=(mode==2?3u:5u)){bad++;continue;}
  expected[0]=sq_seed[0]&~SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK;
  expected[1]=sq_seed[1]&~SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK;
  expected[2]=sq_seed[2]|SDMA0_FREEZE__FREEZE_MASK|(mode==0?SDMA0_FREEZE__FROZEN_MASK:0);
  expected[3]=sq_seed[3]|SDMA0_F32_CNTL__HALT_MASK;
  expected[4]=sq_seed[4]&~SDMA0_CNTL__UTC_L1_ENABLE_MASK;
  w=backend_writes();
  for(i=0;i<backend_write_count();i++)if(w[i].byte_offset!=sq_reg[i]||w[i].value!=expected[i])bad++;
  if(mode!=2){
   sq_seed[2]|=SDMA0_FREEZE__FREEZE_MASK;
   backend_reset_writes();
   if(bc250_sdma_unfreeze_instance(adev,instance)!=0)bad++;
   w=backend_writes();
   if(backend_write_count()!=1||w[0].byte_offset!=sq_reg[2]||
      w[0].value!=(expected[2]&~SDMA0_FREEZE__FREEZE_MASK))bad++;
  }
 }
 backend_reset_writes();
 if(bc250_sdma_quiesce_instance(adev,2)!=BC250_EINVAL||backend_write_count()!=0)bad++;
 backend_set_read_hook(NULL);adev->usec_timeout=timeout;
 printf("  SDMA quiescence: 6 scenarios, %u failures (freeze/idle/order/unfreeze)\n",bad);
 return bad;
}

/* M370: composite scope uses modeled ACK and persistent register state. */
static u32 qs_control,qs_safe,qs_regs[2][6],qs_values[2][6];
static unsigned int qs_mode,qs_requested,qs_acked,qs_exits,qs_bad,qs_writes;
static int qs_read(u32 off,u32 *v)
{
 unsigned i,j;
 if(off==qs_control){*v=qs_mode==3?0:RLC_CNTL__RLC_ENABLE_F32_MASK;return 1;}
 if(off==qs_safe){*v=qs_mode==1?RLC_SAFE_MODE__CMD_MASK:0;if(qs_mode!=1)qs_acked=1;return 1;}
 for(i=0;i<2;i++)for(j=0;j<6;j++)if(off==qs_regs[i][j]){
  *v=qs_values[i][j];
  if(j==2 && qs_mode!=2 && (*v&SDMA0_FREEZE__FREEZE_MASK))*v|=SDMA0_FREEZE__FROZEN_MASK;
  return 1;
 }
 return 0;
}
static void qs_write(u32 off,u32 value)
{
 unsigned i,j;
 if(off==qs_safe){
  if(value==(RLC_SAFE_MODE__CMD_MASK|(1u<<RLC_SAFE_MODE__MESSAGE__SHIFT)))qs_requested++;
  else if(value==RLC_SAFE_MODE__CMD_MASK){if(qs_requested!=1)qs_bad++;qs_exits++;}
  else qs_bad++;
  return;
 }
 for(i=0;i<2;i++)for(j=0;j<6;j++)if(off==qs_regs[i][j]){
  if(!qs_acked||qs_exits)qs_bad++;
  qs_values[i][j]=value;qs_writes++;return;
 }
 qs_bad++;
}
static unsigned int check_sdma_reload_scope(struct amdgpu_device *adev)
{
 unsigned mode,i,j,bad=0;u32 timeout=adev->usec_timeout;u64 saved_cg=adev->cg_flags;
 u32 names[]={mmSDMA0_GFX_RB_CNTL,mmSDMA0_GFX_IB_CNTL,mmSDMA0_FREEZE,
  mmSDMA0_F32_CNTL,mmSDMA0_CNTL,mmSDMA0_STATUS1_REG};
 qs_control=SOC15_REG_OFFSET(GC,0,mmRLC_CNTL)*4;qs_safe=SOC15_REG_OFFSET(GC,0,mmRLC_SAFE_MODE)*4;
 adev->usec_timeout=3;adev->cg_flags=AMD_CG_SUPPORT_GFX_MGCG;
 for(mode=0;mode<4;mode++){
  int rc;qs_mode=mode;qs_requested=qs_acked=qs_exits=qs_bad=qs_writes=0;
  for(i=0;i<2;i++)for(j=0;j<6;j++){
   qs_regs[i][j]=bc250_sdma_reg_offset(adev,i,names[j])*4;qs_values[i][j]=0;
  }
  for(i=0;i<2;i++){
   qs_values[i][3]=SDMA0_F32_CNTL__HALT_MASK;
   qs_values[i][4]=SDMA0_CNTL__UTC_L1_ENABLE_MASK;
  }
  backend_set_read_hook(qs_read);backend_set_write_hook(qs_write);backend_reset_writes();
  rc=bc250_sdma_quiesce_for_reload(adev);
  if(rc!=(mode==0?0:mode==3?BC250_EBUSY:BC250_ETIME)||qs_bad)bad++;
  if(qs_requested!=(mode==3?0u:1u)||qs_exits!=(mode==3?0u:1u))bad++;
  if(mode==0){
   if(qs_writes!=12)bad++;
   for(i=0;i<2;i++)if((qs_values[i][2]&SDMA0_FREEZE__FREEZE_MASK)||
    !(qs_values[i][3]&SDMA0_F32_CNTL__HALT_MASK)||
    (qs_values[i][4]&SDMA0_CNTL__UTC_L1_ENABLE_MASK))bad++;
  }else if(mode!=2 && qs_writes)bad++;
 }
 backend_set_read_hook(NULL);backend_set_write_hook(NULL);adev->usec_timeout=timeout;adev->cg_flags=saved_cg;
 printf("  SDMA reload scope: 4 scenarios, %u failures (ACK before SDMA, paired exit, halted unfreeze)\n",bad);
 return bad;
}


/* M372: access-order model, not an emulation of an effective engine reset. */
static u32 sr_offset, sr_value, sr_extra;
static unsigned sr_reads, sr_bad, sr_len;
static char sr_order[16];
static void sr_event(char c)
{
 if(sr_len<sizeof(sr_order)-1)sr_order[sr_len++]=c;else sr_bad++;
 sr_order[sr_len]=0;
}
static int sr_read(u32 off,u32 *value)
{
 sr_event('R');if(off!=sr_offset)sr_bad++;
 if(++sr_reads==2)sr_value|=sr_extra;
 *value=sr_value;return 1;
}
static void sr_write(u32 off,u32 value)
{
 sr_event('W');if(off!=sr_offset)sr_bad++;
 sr_value=value;
}
static void sr_delay(unsigned usec)
{
 sr_event('D');if(usec!=50)sr_bad++;
}
static unsigned check_sdma_engine_reset(struct amdgpu_device *adev)
{
 const u32 masks[]={GRBM_SOFT_RESET__SOFT_RESET_SDMA0_MASK,
  GRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK};
 unsigned instance,variant,bad=0;
 sr_offset=SOC15_REG_OFFSET(GC,0,mmGRBM_SOFT_RESET)*4;
 backend_set_read_hook(sr_read);backend_set_write_hook(sr_write);backend_set_delay_hook(sr_delay);
 for(instance=0;instance<2;instance++)for(variant=0;variant<3;variant++){
  const u32 initial=variant?GRBM_SOFT_RESET__SOFT_RESET_CP_MASK:0;
  const struct bc250_reg_write *w;
  int rc;
  sr_value=initial;sr_extra=variant==2?GRBM_SOFT_RESET__SOFT_RESET_RLC_MASK:0;
  sr_reads=sr_bad=sr_len=0;sr_order[0]=0;backend_reset_writes();
  rc=bc250_sdma_soft_reset_instance(adev,instance);w=backend_writes();
  if(rc||sr_bad||strcmp(sr_order,"RWRDWR")||backend_write_count()!=2)bad++;
  else if(w[0].byte_offset!=sr_offset||w[1].byte_offset!=sr_offset||
   w[0].value!=(initial|masks[instance])||w[1].value!=(initial|sr_extra)||
   sr_value!=(initial|sr_extra))bad++;
 }
 sr_reads=sr_bad=sr_len=0;sr_order[0]=0;backend_reset_writes();
 if(bc250_sdma_soft_reset_instance(adev,2)!=BC250_EINVAL||
    bc250_sdma_soft_reset_instance(NULL,0)!=BC250_EINVAL||sr_len||backend_write_count())bad++;
 backend_set_read_hook(NULL);backend_set_write_hook(NULL);backend_set_delay_hook(NULL);
 printf("  SDMA engine reset: 6 sequences + bounds, %u failures (RWRDWR, 50us, instance masks, readback preservation)\n",bad);
 return bad;
}


/* M373: reset deliberately destroys the prior halt/queue/cache state. */
static unsigned rr_mode,rr_entries,rr_exits,rr_ack,rr_bad,rr_resets,rr_delays,rr_gated;
static u32 rr_reset;
static int rr_read(u32 off,u32 *v)
{
 unsigned i,j;
 if(off==qs_control){*v=RLC_CNTL__RLC_ENABLE_F32_MASK;return 1;}
 if(off==qs_safe){
  unsigned blocked=(rr_mode==1&&rr_entries==1)||(rr_mode==2&&rr_entries==2);
  *v=blocked?RLC_SAFE_MODE__CMD_MASK:0;rr_ack=!blocked;return 1;
 }
 if(off==sr_offset){*v=rr_reset;return 1;}
 for(i=0;i<2;i++)for(j=0;j<6;j++)if(off==qs_regs[i][j]){
  *v=qs_values[i][j];
  if(j==2 && !(rr_mode==3&&rr_entries==2) && (*v&SDMA0_FREEZE__FREEZE_MASK))
   *v|=SDMA0_FREEZE__FROZEN_MASK;
  return 1;
 }
 rr_bad++;*v=0;return 1;
}
static void rr_write(u32 off,u32 value)
{
 unsigned i,j;
 if(off==qs_safe){
  if(value==(RLC_SAFE_MODE__CMD_MASK|(1u<<RLC_SAFE_MODE__MESSAGE__SHIFT))){
   if(rr_entries!=rr_exits)rr_bad++;
   rr_entries++;rr_ack=0;
  }else if(value==RLC_SAFE_MODE__CMD_MASK){
   if(rr_entries!=rr_exits+1)rr_bad++;
   rr_exits++;rr_ack=0;
  }else rr_bad++;
  return;
 }
 if(off==sr_offset){
  const u32 masks[]={GRBM_SOFT_RESET__SOFT_RESET_SDMA0_MASK,GRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK};
  if(rr_gated?(rr_entries!=1||rr_exits!=1):(rr_entries||rr_exits))rr_bad++;
  for(i=0;i<2;i++)if((rr_reset&masks[i])&&!(value&masks[i])){
   /* Model changed reset defaults, not a claim about unit A reset values. */
   qs_values[i][0]=SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK;
   qs_values[i][1]=SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK;
   qs_values[i][2]=0;qs_values[i][3]=0;
   qs_values[i][4]=SDMA0_CNTL__UTC_L1_ENABLE_MASK;
   rr_resets++;
  }
  rr_reset=value;return;
 }
 for(i=0;i<2;i++)for(j=0;j<6;j++)if(off==qs_regs[i][j]){
  if(rr_gated?(!rr_ack||rr_entries!=rr_exits+1):(rr_entries||rr_exits))rr_bad++;
  qs_values[i][j]=value;return;
 }
 rr_bad++;
}
static void rr_delay(unsigned usec)
{
 if(usec==50){if(rr_gated?(rr_entries!=1||rr_exits!=1):(rr_entries||rr_exits))rr_bad++;rr_delays++;}
}
static unsigned check_sdma_reset_reload(struct amdgpu_device *adev)
{
 unsigned mode,i,j,bad=0;u32 timeout=adev->usec_timeout;u64 saved_cg=adev->cg_flags;
 u32 names[]={mmSDMA0_GFX_RB_CNTL,mmSDMA0_GFX_IB_CNTL,mmSDMA0_FREEZE,
  mmSDMA0_F32_CNTL,mmSDMA0_CNTL,mmSDMA0_STATUS1_REG};
 adev->usec_timeout=3;
 for(mode=0;mode<8;mode++){
  int rc;
  adev->cg_flags=mode==4?0:mode==5?AMD_CG_SUPPORT_GFX_MGLS:
   mode==6?AMD_CG_SUPPORT_GFX_CGCG:mode==7?AMD_CG_SUPPORT_GFX_3D_CGCG:AMD_CG_SUPPORT_GFX_MGCG;
  rr_gated=(mode!=4 && mode!=5);
  rr_mode=mode;rr_entries=rr_exits=rr_ack=rr_bad=rr_resets=rr_delays=0;rr_reset=0;
  for(i=0;i<2;i++)for(j=0;j<6;j++){
   qs_regs[i][j]=bc250_sdma_reg_offset(adev,i,names[j])*4;qs_values[i][j]=0;
  }
  for(i=0;i<2;i++)qs_values[i][3]=SDMA0_F32_CNTL__HALT_MASK;
  backend_set_read_hook(rr_read);backend_set_write_hook(rr_write);backend_set_delay_hook(rr_delay);
  backend_reset_writes();rc=bc250_sdma_reset_for_reload(adev);
  if(rc!=((mode>=1 && mode<=3)?BC250_ETIME:0)||rr_bad)bad++;
  if(rr_entries!=(!rr_gated?0u:mode==1?1u:2u)||rr_exits!=rr_entries||
   rr_resets!=(mode==1?0u:2u)||rr_delays!=rr_resets)bad++;
  if(mode==0 || mode>=4)for(i=0;i<2;i++)if(
   (qs_values[i][0]&SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK)||
   (qs_values[i][1]&SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK)||
   (qs_values[i][2]&SDMA0_FREEZE__FREEZE_MASK)||
   !(qs_values[i][3]&SDMA0_F32_CNTL__HALT_MASK)||
   (qs_values[i][4]&SDMA0_CNTL__UTC_L1_ENABLE_MASK))bad++;
 }
 backend_set_read_hook(NULL);backend_set_write_hook(NULL);backend_set_delay_hook(NULL);
 adev->usec_timeout=timeout;adev->cg_flags=saved_cg;
 printf("  SDMA reset reload: 8 scenarios, %u failures (clock-gating policy, separate scopes, disturbed post-reset state)\n",bad);
 return bad;
}

int main(int argc, char **argv)
{
	struct amdgpu_device adev;
	struct result derived, ctl_async, ctl_gfxoff, ctl_nostub, ctl_perring;
	struct run_opts opt;
	unsigned int i, stray = 0, gart_bad = 0, mqd_bad = 0, rerun_bad = 0, unclean_bad = 0;
	unsigned int stuck_bad = 0, sdma_ptr_bad = 0, stagec_bad = 0, reload_bad = 0;
	int verbose = 0, exact, controls_fail;
	const char *sweep1, *sweep2, *trace, *trace_irq, *dumpdir = NULL, *ringsdir = NULL;
	const char *winsweep = NULL;
	int seeded, seeded_irq;

	if (argc < 5) {
		fprintf(stderr, "usage: replay_gfx <sweep-run1.log> <sweep-run2.log> <trace-gfx.txt>"
				" <trace-irq.txt> [-v] [--dump DIR]\n");
		return 2;
	}
	sweep1 = argv[1];
	sweep2 = argv[2];
	trace = argv[3];
	trace_irq = argv[4];
	for (i = 5; i < (unsigned int)argc; i++) {
		if (strcmp(argv[i], "--cp1-checkpoints") == 0)
			trace_cp1 = 1;
		else if (strcmp(argv[i], "-v") == 0)
			verbose = 1;
		else if (strcmp(argv[i], "--dump") == 0 && i + 1 < (unsigned int)argc)
			dumpdir = argv[++i];
		else if (strcmp(argv[i], "--rings") == 0 && i + 1 < (unsigned int)argc)
			ringsdir = argv[++i];
		else if (strcmp(argv[i], "--windows-sweep") == 0 && i + 1 < (unsigned int)argc)
			winsweep = argv[++i];
	}
	backend_set_verbose(verbose);

	if (backend_load_sweep(sweep1) < 0) {
		fprintf(stderr, "cannot read %s\n", sweep1);
		return 2;
	}
	if (backend_load_sweep(sweep2) < 0) {
		fprintf(stderr, "cannot read %s\n", sweep2);
		return 2;
	}
	/* Seed and load in the order the two windows happen: the later window's first read of a
	 * register the earlier one already read must not displace it, because by then the run has
	 * written that register itself and the run's own value is the right answer. */
	seeded = backend_seed_reads(trace);
	if (seeded < 0 || load_trace_writes(trace, g_trace, &g_trace_count) < 0) {
		fprintf(stderr, "cannot read %s\n", trace);
		return 2;
	}
	seeded_irq = backend_seed_reads(trace_irq);
	if (seeded_irq < 0 || load_trace_writes(trace_irq, g_trace_irq, &g_trace_irq_count) < 0) {
		fprintf(stderr, "cannot read %s\n", trace_irq);
		return 2;
	}
	if (filter_irq_window() != 0)
		return 2;
	if (load_trace_offsets(trace) < 0 || load_trace_offsets(trace_irq) < 0) {
		fprintf(stderr, "cannot re-read the trace windows for the offset survey\n");
		return 2;
	}

	printf("BC-250 M5 part B shim replay: GFX10 CP/KIQ, SDMA and interrupt bring-up on unit A"
	       " (experiment E03)\n");
	printf("  writes in the bring-up window       : %u\n", g_trace_count);
	printf("  writes in the interrupt window      : %u (after dropping the %u declared above)\n",
	       g_trace_irq_count, g_trace_irq_dropped);
	printf("  registers seeded from traced reads  : %d + %d\n", seeded, seeded_irq);
	printf("  distinct offsets the windows name   : %u (reads and writes, for the fini survey)\n",
	       g_seen_count);
	printf("  address-carrying registers excepted : %u names\n",
	       (unsigned int)ARRAY_SIZE(g_address_registers));

	opt.async_gfx_ring = true;
	opt.pp_gfxoff = true;
	opt.cp_stub = 1;
	opt.irq_per_ring = 0;
	run_one("derived: async_gfx_ring = true, pp_gfxoff = true (what the trace says)",
		&opt, &adev, &derived, verbose);

	if (dumpdir != NULL && derived.irq_rc == 0)
		stray = write_dumps(dumpdir, &adev);

	if (derived.irq_rc == 0)
		gart_bad = check_gart(&adev, verbose);

	/* Before the teardown arms, because it wants the device the bring-up left and gives back the
	 * two pages it borrows so that those arms see the same one. */
	if (derived.irq_rc == 0)
		stagec_bad = check_stage_c(&adev, verbose);

	if (ringsdir != NULL && derived.irq_rc == 0)
		mqd_bad = compare_mqds_with_unit_a(ringsdir, &adev, verbose);

	/* Last, because it deliberately changes the register state and the MQDs it has just checked. */
	if (derived.irq_rc == 0)
		rerun_bad = check_rerun(&adev, derived.bringup.produced, verbose);

	/* And after that, because it starts from the device the re-run left running. */
	if (derived.irq_rc == 0 && rerun_bad == 0)
		stuck_bad = check_stuck_queue(&adev, verbose);

	if (derived.irq_rc == 0 && rerun_bad == 0 && stuck_bad == 0)
		unclean_bad = check_unclean_start(&adev, verbose);

	/* And after that, because it takes the device down and builds two bring-ups of its own. */
	if (derived.irq_rc == 0 && rerun_bad == 0 && stuck_bad == 0 && unclean_bad == 0)
		sdma_ptr_bad = check_sdma_pointers(&adev, verbose);

	/* Four controls, one per claim that is not forced by the register sequence itself.
	 *
	 * The first two are the two module parameters struct bc250_gfx_inputs carries: its comment
	 * says the trace shows both were on, and these runs are what makes that a measurement rather
	 * than an assumption. The third turns the CP stub off, which is what says the eleven ring
	 * tests were being driven by the packets the shim built and not waved through. The fourth is
	 * the per-ring compute enable, which is what says the interrupt window's four CP_ME1_PIPEn
	 * accesses are four because of amdgpu_irq_get()'s refcount. All four must fail. */
	opt.async_gfx_ring = false;
	opt.pp_gfxoff = true;
	opt.cp_stub = 1;
	opt.irq_per_ring = 0;
	run_one("control: async_gfx_ring = false (the non-async gfx ring path)",
		&opt, &adev, &ctl_async, 0);

	opt.async_gfx_ring = true;
	opt.pp_gfxoff = false;
	opt.cp_stub = 1;
	opt.irq_per_ring = 0;
	run_one("control: pp_gfxoff = false (rlc_start writes RLC_PG_CNTL a second time)",
		&opt, &adev, &ctl_gfxoff, 0);

	opt.async_gfx_ring = true;
	opt.pp_gfxoff = true;
	opt.cp_stub = 0;
	opt.irq_per_ring = 0;
	run_one("control: the CP stub off (no packet is executed, so every ring test times out)",
		&opt, &adev, &ctl_nostub, 0);

	opt.async_gfx_ring = true;
	opt.pp_gfxoff = true;
	opt.cp_stub = 1;
	opt.irq_per_ring = 1;
	run_one("control: one compute EOP enable per ring instead of per pipe",
		&opt, &adev, &ctl_perring, 0);

	/* Last of all: it overlays the Windows register state, which every pass before it depends on
	 * not having. */
	if (winsweep != NULL && derived.irq_rc == 0)
		check_windows_state(winsweep, &adev, verbose);

	printf("\n== summary ==\n");
	printf("  bring-up: produced %u, window %u, compared %u, mismatches %u\n",
	       derived.bringup.produced, g_trace_count, derived.bringup.compared,
	       derived.bringup.mismatches);
	printf("  interrupts: produced %u, window %u, compared %u, mismatches %u\n",
	       derived.irq.produced, g_trace_irq_count, derived.irq.compared,
	       derived.irq.mismatches);
	printf("  address exceptions (offset only)    : %u\n", derived.bringup.address_exceptions);
	printf("  address registers wrong             : %u\n", derived.address_failures);
	printf("  reads with no value on unit A       : %u", derived.unknown_reads);
	if (derived.unknown_reads)
		printf(" (first at byte offset 0x%05X)", backend_first_unknown_read());
	printf("\n");
	printf("  doorbells rung                      : %u\n", derived.doorbells);
	printf("  ring tests the CP stub satisfied    : %u (expected 11)\n", derived.stub_hits);
	printf("  ring-test packets the stub rejected : %u (must be 0)\n", derived.stub_rejects);
	printf("  PM4 packets decoded at a doorbell   : %u\n", derived.packets);
	if (dumpdir != NULL)
		printf("  unexpected non-zero MQD dwords      : %u\n", stray);
	printf("  stage 0 writes wrong                : %u\n", derived.stage0_bad);
	printf("  GART page table checks wrong        : %u\n", gart_bad);
	printf("  stage C (IB and VMID root) wrong    : %u\n", stagec_bad);
	if (ringsdir != NULL)
		printf("  MQD fields differing from unit A    : %u\n", mqd_bad);
	printf("  teardown-and-rerun checks wrong     : %u\n", rerun_bad);
	printf("  stuck-queue checks wrong            : %u\n", stuck_bad);
	printf("  unclean-start recovery wrong        : %u\n", unclean_bad);
	printf("  SDMA pointer-adoption checks wrong  : %u\n", sdma_ptr_bad);
	printf("  mismatches, control async_gfx_ring  : %u\n", ctl_async.bringup.mismatches);
	printf("  mismatches, control pp_gfxoff       : %u\n", ctl_gfxoff.bringup.mismatches);
	printf("  control with the CP stub off        : gfx_hw_init returned %d\n", ctl_nostub.gfx_rc);
	printf("  mismatches, control irq per ring    : %u (%u writes, not %u)\n",
	       ctl_perring.irq.mismatches, ctl_perring.irq.produced, g_trace_irq_count);

	/* A count that matches only because the shorter of the two lists ran out is not a match. */
	exact = derived.bringup.produced == g_trace_count &&
		derived.irq.produced == g_trace_irq_count &&
		derived.bringup.mismatches == 0 && derived.irq.mismatches == 0;

	controls_fail = ctl_async.bringup.mismatches != 0 &&
			ctl_gfxoff.bringup.mismatches != 0 &&
			ctl_nostub.gfx_rc != 0 &&
			(ctl_perring.irq.mismatches != 0 ||
			 ctl_perring.irq.produced != g_trace_irq_count);

	reload_bad = check_rlc_reload_reset(&adev);
	reload_bad += check_tlb_observation(&adev);
	reload_bad += check_gart_phase_extraction();
	reload_bad += check_sdma_quiescence(&adev);
	reload_bad += check_sdma_reload_scope(&adev);
	reload_bad += check_sdma_engine_reset(&adev);
	reload_bad += check_sdma_reset_reload(&adev);
	printf("\n  verdict: ");
	if (derived.irq_rc != 0)
		printf("INCONCLUSIVE - the bring-up returned %d\n", derived.irq_rc);
	else if (derived.unknown_reads != 0)
		printf("INCONCLUSIVE - the replay had to invent %u read values\n", derived.unknown_reads);
	else if (!exact)
		printf("NO MATCH - %u + %u writes differ, %u + %u produced against %u + %u traced\n",
		       derived.bringup.mismatches, derived.irq.mismatches,
		       derived.bringup.produced, derived.irq.produced,
		       g_trace_count, g_trace_irq_count);
	else if (derived.address_failures != 0)
		printf("NO MATCH - %u address registers do not describe this test's allocations\n",
		       derived.address_failures);
	else if (derived.stub_hits != 11 || derived.stub_rejects != 0)
		printf("NO MATCH - the CP stub satisfied %u ring tests (expected 11) and rejected %u\n",
		       derived.stub_hits, derived.stub_rejects);
	else if (stray != 0)
		printf("NO MATCH - %u MQD dwords outside the fields the MQD init sets are non-zero\n", stray);
	else if (derived.stage0_bad != 0)
		printf("NO MATCH - %u of stage 0's three NBIO writes differ\n", derived.stage0_bad);
	else if (gart_bad != 0)
		printf("NO MATCH - %u GART page table checks failed\n", gart_bad);
	else if (stagec_bad != 0)
		printf("NO MATCH - %u stage C checks failed (the IB emitters or the VMID page"
		       " directory)\n", stagec_bad);
	else if (mqd_bad != 0)
		printf("NO MATCH - %u compute MQD fields differ from unit A's own MQDs\n", mqd_bad);
	else if (stuck_bad != 0)
		printf("BRING-UP EXACT over %u + %u writes, BUT the teardown mishandles a compute queue\n"
		       "           that does not answer its unmap (%u failing check above)\n",
		       derived.bringup.compared, derived.irq.compared, stuck_bad);
	else if (unclean_bad != 0)
		printf("BRING-UP EXACT over %u + %u writes, BUT it does not recover from a start where\n"
		       "           the last instance left the KIQ up (%u failing check above)\n",
		       derived.bringup.compared, derived.irq.compared, unclean_bad);
	else if (sdma_ptr_bad != 0)
		printf("BRING-UP EXACT over %u + %u writes, BUT the SDMA engines do not run again after\n"
		       "           a second bring-up: they keep the write pointer the first run left and\n"
		       "           the driver does not adopt it (%u failing check above)\n",
		       derived.bringup.compared, derived.irq.compared, sdma_ptr_bad);
	else if (rerun_bad != 0)
		printf("BRING-UP EXACT over %u + %u writes, BUT it does not survive its own teardown\n"
		       "           (%u failing check above. The first bring-up, which is what experiment\n"
		       "           E11 runs, is unaffected: this is H9's second run.)\n",
		       derived.bringup.compared, derived.irq.compared, rerun_bad);
	else if (!controls_fail)
		printf("SUSPECT - a control run matched, so the comparison is not discriminating\n");
	else
		printf("EXACT MATCH over %u + %u writes (%u address exceptions);"
		       " the four controls all fail, as they should\n",
		       derived.bringup.compared, derived.irq.compared,
		       derived.bringup.address_exceptions);

	return (derived.irq_rc == 0 && derived.unknown_reads == 0 && exact &&
		derived.address_failures == 0 && derived.stub_hits == 11 &&
		derived.stub_rejects == 0 && stray == 0 &&
		derived.stage0_bad == 0 && gart_bad == 0 && mqd_bad == 0 && rerun_bad == 0 &&
		stuck_bad == 0 && unclean_bad == 0 && sdma_ptr_bad == 0 && stagec_bad == 0 &&
		controls_fail && reload_bad == 0) ? 0 : 1;
}
