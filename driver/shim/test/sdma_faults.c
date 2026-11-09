/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Fault injection for the SDMA bring-up (driver/shim/bc250_sdma.c), host side.
 *
 * The replay tests answer one question - does the shim write what Linux wrote, in that order - and
 * they answer it on the path where everything works. This one asks the other question: what does
 * bc250_sdma_setup(), bc250_sdma_start(), bc250_sdma_fence_page_alloc() and the undo path do when
 * something does not work. Nothing here compares anything with a trace, and nothing here may change
 * what the success path writes: the register sequences are confirmed write-for-write against unit
 * A's Linux trace and the replay says EXACT MATCH. This file only fails the things around them.
 *
 * Two models, both declared here rather than borrowed, so that this program links against
 * driver/shim alone and cannot disturb the replay's backend:
 *
 *   1. The allocator. bc250_shim_mem_alloc/_free with a live-object table, a fault at the Nth call,
 *      and a "succeeded but could not map it" mode for the cpu == NULL case the shim's own contract
 *      allows (bc250_shim.h:31-38). Freed memory is not given back to the C library: it is filled
 *      with a poison byte and kept, so that a write into it after the free is a fact this program
 *      can state rather than a crash it might get away with. The free itself is modelled EXACTLY as
 *      the two shipped backends do it - a zeroed struct is a no-op, and an object whose cpu is NULL is
 *      looked up by its mc instead of by cpu, since that is all the caller was ever handed for it
 *      (driver/shim/test/backend_mem.c, driver/kmd/gpumem.c). It used to be modelled as an early
 *      return on cpu == NULL, which is what D-02 (below) was; the model was fixed alongside the two
 *      real backends, not left kinder than they now are.
 *
 *   2. The register file. One value per dword index, reads answer the last write, plus a forced-read
 *      table for the one thing E15 measured that a register file cannot hold: an SDMA engine goes on
 *      answering with the write pointer it holds however often the bring-up writes 0 into it (facts
 *      M59/M60, backend_mem.h). That is how a corrupt preserved write pointer is put in front of the
 *      adoption code in bc250_sdma_gfx_resume_instance().
 *
 * The register offsets come from AMD's headers through bc250_sdma_reg_offset(), as everywhere else.
 * The two MC bases below are this program's own and are deliberately NOT unit A's: nothing here
 * compares an address with anything, and an invented address that looked like a measured one would
 * be a trap for the next reader.
 *
 * Expected failures. An expectation that states how a CONFIRMED defect should behave is written with
 * check_defect() and its defect id: while the defect stands the suite stays green and prints the
 * defect on every run; the day the defect is fixed the expectation passes, which this program
 * reports as a failure (XPASS) so that whoever fixed it comes back here and turns the marker off.
 * The report is driver/shim/test/sdma_faults_report.md.
 *
 * All seven defects below were fixed on 2026-09-22 (see the report's "Fixes" section for what changed
 * and driver/shim/bc250_sdma.c for the code); every check_defect() that named one has been turned into
 * a plain check(), which is why none of them are marked XPASS-pending any more. The list stays here as
 * a record of what each one was.
 *
 *   D-01  bc250_sdma_setup() accepted a write-back page with cpu == NULL although the ring buffer's
 *         own cpu == NULL was refused eight lines below. Fixed: it is refused the same way.
 *   D-02  an allocation whose cpu is NULL could never be given back: bc250_shim_mem_free() looked it
 *         up by that pointer and returned at once. Every "allocated but not mapped" refusal in the
 *         shim therefore leaked the object it just got. Fixed in the free() of both shipped backends
 *         (and this file's model of them): an object with cpu == NULL is looked up by mc instead.
 *   D-03  bc250_sdma_fence_page_alloc() returned BC250_EINVAL without freeing or clearing
 *         adev->sdma.fence_mem; the idempotence test one line above keys on fence_mem.cpu, so the next
 *         call allocated a second page on top of the first. Fixed: the refusal frees the page (D-02)
 *         and clears the descriptor.
 *   D-04  bc250_sdma_setup() on an adev that was already set up re-allocated over the three live
 *         allocations and leaked them. Fixed: a second setup on the same adev is refused.
 *   D-05  bc250_sdma_start() did not check that the setup succeeded. Fixed: it refuses an adev whose
 *         rings have no funcs or no size.
 *   D-06  bc250_sdma_teardown() left ring->ring and ring->wptr_cpu_addr pointing at freed memory, so a
 *         ring test run after it wrote into a page that was gone. Fixed: the teardown nulls them.
 *   D-07  bc250_sdma_ring_test() folded an out-of-range engine number onto a valid scratch slot with
 *         `slot = ring->me & 0x1u` instead of refusing it. Fixed: an instance number this driver does
 *         not have is refused.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bc250_sdma.h"
#include "bc250_gmc.h"                  /* BC250_EINVAL, BC250_EBUSY, BC250_ETIME */
#include "bc250_gfx.h"                  /* BC250_ENOMEM */
#include "nv.h"                         /* cyan_skillfish_reg_base_init() */

#include "gc/gc_10_1_0_offset.h"
#include "gc/gc_10_1_0_sh_mask.h"

/* The bases the two arenas hand MC addresses out of. Page aligned, non-zero, and nothing else about
 * them matters; see the header comment. */
#define FAULT_VRAM_BASE   0x0000001000000000ULL
#define FAULT_GTT_BASE    0x0000002000000000ULL

#define FAULT_POISON      0xDEu

/* ---------------------------------------------------------------------------------------------
 * The instrumented allocator
 * ------------------------------------------------------------------------------------------- */

#define FAULT_MAX_OBJ 64

struct fault_obj {
	unsigned int	id;             /* the call that produced it, 0-based */
	void		*cpu;           /* NULL for an object the backend could not map */
	u64		mc;
	u32		size;
	int		domain;
	int		live;           /* 1 = the driver still owns it */
	int		poisoned;       /* 1 = freed, filled with FAULT_POISON and kept */
};

static struct fault_obj g_obj[FAULT_MAX_OBJ];
static unsigned int g_obj_count;
static unsigned int g_alloc_calls;
static unsigned int g_double_free;      /* a free of something already given back */
static unsigned int g_unknown_free;     /* a free of something this file never handed out */
static int g_fail_at = -1;              /* this allocation call returns BC250_ENOMEM */
static int g_null_cpu_at = -1;          /* this allocation call returns 0 with cpu == NULL */
static u64 g_vram_next = FAULT_VRAM_BASE;
static u64 g_gtt_next = FAULT_GTT_BASE;
static int g_verbose;

static void fault_alloc_reset(void)
{
	unsigned int i;

	for (i = 0; i < g_obj_count; i++)
		free(g_obj[i].cpu);
	memset(g_obj, 0, sizeof(g_obj));
	g_obj_count = 0;
	g_alloc_calls = 0;
	g_double_free = 0;
	g_unknown_free = 0;
	g_fail_at = -1;
	g_null_cpu_at = -1;
	g_vram_next = FAULT_VRAM_BASE;
	g_gtt_next = FAULT_GTT_BASE;
}

/* How many allocations the driver still owns, an object whose cpu is NULL included: it exists in
 * the allocator whether or not the caller can address it. */
static unsigned int live_objects(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < g_obj_count; i++)
		if (g_obj[i].live)
			n++;
	return n;
}

/* Blocks that were written after they were freed. */
static unsigned int poison_violations(void)
{
	unsigned int i, j, n = 0;

	for (i = 0; i < g_obj_count; i++) {
		if (!g_obj[i].poisoned || g_obj[i].cpu == NULL)
			continue;
		for (j = 0; j < g_obj[i].size; j++) {
			if (((const unsigned char *)g_obj[i].cpu)[j] != FAULT_POISON) {
				n++;
				if (g_verbose)
					printf("      allocation %u was written %u bytes in after it"
					       " was freed\n", g_obj[i].id, j);
				break;
			}
		}
	}
	return n;
}

int bc250_shim_mem_alloc(struct amdgpu_device *adev, enum bc250_mem_domain domain,
			 unsigned int size, unsigned int align, struct bc250_mem *out)
{
	unsigned int call = g_alloc_calls++;
	struct fault_obj *obj;
	u64 *next;

	(void)adev;

	if (out == NULL || size == 0)
		return BC250_EINVAL;
	if (g_obj_count >= FAULT_MAX_OBJ) {
		fprintf(stderr, "sdma_faults: more than %u allocations\n", FAULT_MAX_OBJ);
		exit(2);
	}
	if ((int)call == g_fail_at)
		return BC250_ENOMEM;

	if (align == 0)
		align = 4;
	next = (domain == BC250_MEM_VRAM) ? &g_vram_next : &g_gtt_next;
	*next = (*next + align - 1u) & ~((u64)align - 1u);

	obj = &g_obj[g_obj_count];
	obj->id = call;
	obj->mc = *next;
	obj->size = size;
	obj->domain = (int)domain;
	obj->live = 1;
	obj->poisoned = 0;
	/* Zeroed, as both shipped backends zero what they hand out. */
	obj->cpu = calloc(1, size);
	if (obj->cpu == NULL)
		return BC250_ENOMEM;
	*next += size;
	g_obj_count++;

	out->mc = obj->mc;
	out->size = size;
	/* The mode the shim's own contract allows and neither shipped backend produces: the memory
	 * exists and the GPU can reach it, the CPU cannot. The object stays in the table, because it
	 * exists in the allocator whatever the caller was handed. */
	out->cpu = ((int)call == g_null_cpu_at) ? NULL : obj->cpu;
	return 0;
}

void bc250_shim_mem_free(struct amdgpu_device *adev, struct bc250_mem *m)
{
	unsigned int i;

	(void)adev;

	/* D-02, fixed: now exactly backend_mem.c and gpumem.c after their own fix of the same defect
	 * (driver/shim/test/sdma_faults_report.md, "Fixes"). A zeroed struct is still a safe no-op; an
	 * object whose cpu is NULL still exists in this table and is looked up by its mc instead, since
	 * that is all the caller was ever handed for it. */
	if (m == NULL || (m->cpu == NULL && m->mc == 0 && m->size == 0))
		return;

	for (i = 0; i < g_obj_count; i++) {
		if (m->cpu != NULL ? g_obj[i].cpu != m->cpu : g_obj[i].mc != m->mc)
			continue;
		if (!g_obj[i].live) {
			g_double_free++;
			break;
		}
		g_obj[i].live = 0;
		g_obj[i].poisoned = 1;
		/* g_obj[i].cpu is this file's own host allocation and is never NULL, whatever cpu ==
		 * NULL the caller was handed at alloc time (see bc250_shim_mem_alloc above). */
		memset(g_obj[i].cpu, FAULT_POISON, g_obj[i].size);
		break;
	}
	if (i == g_obj_count)
		g_unknown_free++;

	memset(m, 0, sizeof(*m));
}

/* ---------------------------------------------------------------------------------------------
 * The register file
 * ------------------------------------------------------------------------------------------- */

#define FAULT_REG_SLOTS   4096u         /* open addressing, far more than the sequence touches */
#define FAULT_MAX_WRITES  4096u
#define FAULT_MAX_FORCED  16u

struct reg_cell {
	u32 index;
	u32 value;
	int used;
};

static struct reg_cell g_reg[FAULT_REG_SLOTS];
static u32 g_write_off[FAULT_MAX_WRITES];       /* dword index, in order */
static unsigned int g_write_count;
static u32 g_forced_index[FAULT_MAX_FORCED];
static u32 g_forced_value[FAULT_MAX_FORCED];
static unsigned int g_forced_count;

static struct reg_cell *reg_cell(u32 index)
{
	u32 slot = (index * 2654435761u) % FAULT_REG_SLOTS;
	unsigned int tries;

	for (tries = 0; tries < FAULT_REG_SLOTS; tries++) {
		if (!g_reg[slot].used || g_reg[slot].index == index)
			return &g_reg[slot];
		slot = (slot + 1u) % FAULT_REG_SLOTS;
	}
	fprintf(stderr, "sdma_faults: the register table is full\n");
	exit(2);
}

static void reg_reset(void)
{
	memset(g_reg, 0, sizeof(g_reg));
	memset(g_forced_index, 0, sizeof(g_forced_index));
	memset(g_forced_value, 0, sizeof(g_forced_value));
	g_forced_count = 0;
	g_write_count = 0;
}

static u32 reg_get(u32 index)
{
	struct reg_cell *c = reg_cell(index);

	return c->used ? c->value : 0u;
}

/* The engine answers this register with this value whatever is written into it: E15's measurement,
 * and the only way to put a write pointer the driver did not choose in front of the adoption code. */
static void reg_force(u32 index, u32 value)
{
	if (g_forced_count >= FAULT_MAX_FORCED) {
		fprintf(stderr, "sdma_faults: the forced-read table is full\n");
		exit(2);
	}
	g_forced_index[g_forced_count] = index;
	g_forced_value[g_forced_count] = value;
	g_forced_count++;
}

/* The engine model of the reset cases (section 7 below, bc250_sdma_reset_retained_instance). Off
 * for every other case. When on: SDMA0_FREEZE reads FROZEN once FREEZE is set, unless engine 0 is
 * hung; STATUS1 reads all idle bits unless engine 0 is hung; a GRBM_SOFT_RESET write that sets
 * SOFT_RESET_SDMA0 counts a pulse and, unless the case says the hang survives it, ends the hang. */
static int g_reset_model, g_sdma0_hung, g_hang_survives_pulse;
static unsigned int g_sdma0_pulses, g_sdma1_pulses;
static u32 g_freeze_index, g_status1_index, g_grbm_reset_index;

unsigned int bc250_shim_rreg(struct amdgpu_device *adev, unsigned int dword_index)
{
	unsigned int i;
	u32 v;

	(void)adev;
	for (i = 0; i < g_forced_count; i++)
		if (g_forced_index[i] == dword_index)
			return g_forced_value[i];
	v = reg_get(dword_index);
	if (g_reset_model && dword_index == g_freeze_index && (v & SDMA0_FREEZE__FREEZE_MASK) && !g_sdma0_hung)
		v |= SDMA0_FREEZE__FROZEN_MASK;
	if (g_reset_model && dword_index == g_status1_index)
		v = g_sdma0_hung ? 0u : 0x3FFu;
	return v;
}

void bc250_shim_wreg(struct amdgpu_device *adev, unsigned int dword_index, unsigned int value)
{
	struct reg_cell *c = reg_cell(dword_index);

	(void)adev;
	if (g_reset_model && dword_index == g_grbm_reset_index) {
		if (value & GRBM_SOFT_RESET__SOFT_RESET_SDMA0_MASK) {
			g_sdma0_pulses++;
			if (!g_hang_survives_pulse)
				g_sdma0_hung = 0;
		}
		if (value & GRBM_SOFT_RESET__SOFT_RESET_SDMA1_MASK)
			g_sdma1_pulses++;
	}
	c->index = dword_index;
	c->value = value;
	c->used = 1;
	if (g_write_count < FAULT_MAX_WRITES)
		g_write_off[g_write_count] = dword_index;
	g_write_count++;
}

void bc250_shim_udelay(unsigned int usec)
{
	(void)usec;                     /* nothing here changes with time */
}

static unsigned int g_doorbells;
static unsigned long long g_last_doorbell;

void bc250_shim_wdoorbell64(struct amdgpu_device *adev, unsigned int index, unsigned long long value)
{
	(void)adev;
	(void)index;
	g_doorbells++;
	g_last_doorbell = value;
}

/* bc250_gfx.c's RLC safe-mode scope, which bc250_sdma_quiesce_for_reload (M370) and the reset paths
 * that call it take. No case here reaches them; every call is counted and the verdict fails on one,
 * so a case that does reach them has to model the scope here rather than pass a silent stub. */
static unsigned int g_rlc_safe_calls;
/* The reset cases model the scope: entries and exits are counted on their own, a requested scope is
 * reported back, and one entry (1-based) can be refused, as an RLC that never acknowledges would. */
static unsigned int g_scope_entries, g_scope_exits, g_scope_refuse_at, g_scope_open;

int bc250_gfx_rlc_safe_enter(struct amdgpu_device *adev, bool *requested)
{
	(void)adev;
	if (g_reset_model) {
		g_scope_entries++;
		if (requested)
			*requested = g_scope_entries != g_scope_refuse_at;
		if (g_scope_entries == g_scope_refuse_at)
			return BC250_ETIME;
		g_scope_open++;
		return 0;
	}
	g_rlc_safe_calls++;
	if (requested)
		*requested = false;
	return BC250_EINVAL;
}

void bc250_gfx_rlc_safe_exit(struct amdgpu_device *adev, bool requested)
{
	(void)adev;
	if (g_reset_model) {
		g_scope_exits++;
		if (requested && g_scope_open)
			g_scope_open--;
		return;
	}
	(void)requested;
	g_rlc_safe_calls++;
}

void bc250_shim_log(int level, void *dev, const char *fmt, ...)
{
	static const char *const tag[] = { "info", "warn", "err " };
	va_list ap;

	(void)dev;
	if (!g_verbose)
		return;
	va_start(ap, fmt);
	printf("      [%s] ", tag[level < 0 ? 0 : (level > 2 ? 2 : level)]);
	vprintf(fmt, ap);
	va_end(ap);
}

/* ---------------------------------------------------------------------------------------------
 * The set of registers a bring-up that works writes
 *
 * "No register write outside the trace-confirmed set on the error paths" needs a set to compare
 * against, and the honest one is not typed here: it is what a successful bc250_sdma_hw_init() writes
 * in this program, which is the sequence the replay compares with unit A's trace write for write.
 * Anything an error path touches that a working bring-up does not is then a write that never went
 * past a trace.
 * ------------------------------------------------------------------------------------------- */

#define FAULT_MAX_CONFIRMED 1024u
static u32 g_confirmed[FAULT_MAX_CONFIRMED];
static unsigned int g_confirmed_count;

static int confirmed_has(u32 index)
{
	unsigned int i;

	for (i = 0; i < g_confirmed_count; i++)
		if (g_confirmed[i] == index)
			return 1;
	return 0;
}

static void confirmed_add_current_writes(void)
{
	unsigned int i;

	for (i = 0; i < g_write_count && i < FAULT_MAX_WRITES; i++) {
		if (confirmed_has(g_write_off[i]))
			continue;
		if (g_confirmed_count >= FAULT_MAX_CONFIRMED) {
			fprintf(stderr, "sdma_faults: the confirmed-write table is full\n");
			exit(2);
		}
		g_confirmed[g_confirmed_count++] = g_write_off[i];
	}
}

static unsigned int writes_outside_confirmed(void)
{
	unsigned int i, n = 0;

	for (i = 0; i < g_write_count && i < FAULT_MAX_WRITES; i++)
		if (!confirmed_has(g_write_off[i]))
			n++;
	return n;
}

/* ---------------------------------------------------------------------------------------------
 * Expectations
 * ------------------------------------------------------------------------------------------- */

static unsigned int g_checks, g_failures, g_xfail, g_xpass;

static void check(int ok, const char *what)
{
	g_checks++;
	if (ok) {
		if (g_verbose)
			printf("    ok    %s\n", what);
		return;
	}
	g_failures++;
	printf("    FAIL  %s\n", what);
}

/* An expectation that a CONFIRMED defect breaks. `ok` is what the code SHOULD do. Kept for the next
 * confirmed defect although nothing calls it right now - the seven of 2026-09-22 are all fixed and
 * their check_defect() calls turned into plain check() (see the header comment) - so /W4 sees it as
 * unreferenced. Warn, not error: the day it is used again the pragma is the thing to delete. */
#pragma warning(push)
#pragma warning(disable: 4505)
static void check_defect(int ok, const char *defect, const char *what)
{
	g_checks++;
	if (!ok) {
		g_xfail++;
		printf("    %s  (expected) %s\n", defect, what);
		return;
	}
	g_xpass++;
	g_failures++;
	printf("    XPASS %s no longer reproduces: %s\n", defect, what);
	printf("          fix confirmed - turn this expectation into a plain check()\n");
}
#pragma warning(pop)

static void note(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	printf("    note  ");
	vprintf(fmt, ap);
	va_end(ap);
}

/* ---------------------------------------------------------------------------------------------
 * The device, and the state a case starts from
 * ------------------------------------------------------------------------------------------- */

static void device_init(struct amdgpu_device *adev)
{
	memset(adev, 0, sizeof(*adev));
	adev->dev = (void *)"BC250-A-faults";
	/* The register bases, from the imported table over AMD's headers - the same call
	 * bc250_gmc_setup() makes, which is not run here because no register this file touches needs
	 * the memory controller. */
	cyan_skillfish_reg_base_init(adev);
	adev->usec_timeout = 100000;    /* bc250_gmc.c:47, amdgpu's default */
}

static void case_begin(const char *title, struct amdgpu_device *adev)
{
	printf("\n== %s ==\n", title);
	fault_alloc_reset();
	reg_reset();
	g_doorbells = 0;
	device_init(adev);
}

static u32 sdma_reg(struct amdgpu_device *adev, int i, u32 internal)
{
	return bc250_sdma_reg_offset(adev, (u32)i, internal);
}

static int engine_halted(struct amdgpu_device *adev, int i)
{
	return (reg_get(sdma_reg(adev, i, mmSDMA0_F32_CNTL)) & SDMA0_F32_CNTL__HALT_MASK) != 0;
}

static int engine_rb_enabled(struct amdgpu_device *adev, int i)
{
	return (reg_get(sdma_reg(adev, i, mmSDMA0_GFX_RB_CNTL)) &
		SDMA0_GFX_RB_CNTL__RB_ENABLE_MASK) != 0;
}

static int engine_ib_enabled(struct amdgpu_device *adev, int i)
{
	return (reg_get(sdma_reg(adev, i, mmSDMA0_GFX_IB_CNTL)) &
		SDMA0_GFX_IB_CNTL__IB_ENABLE_MASK) != 0;
}

static void print_engines(struct amdgpu_device *adev, const char *when)
{
	int i;

	for (i = 0; i < AMDGPU_MAX_SDMA_INSTANCES; i++)
		note("%s: sdma%d halt %d, RB_ENABLE %d, IB_ENABLE %d\n", when, i,
		     engine_halted(adev, i), engine_rb_enabled(adev, i), engine_ib_enabled(adev, i));
}

/* The engine keeps `bytes` in its write pointer whatever the bring-up writes there: E15, points 1
 * to 3 of the SDMA model in backend_mem.h. */
static void engine_keeps_wptr(struct amdgpu_device *adev, int i, u64 bytes)
{
	reg_force(sdma_reg(adev, i, mmSDMA0_GFX_RB_WPTR), lower_32_bits(bytes));
	reg_force(sdma_reg(adev, i, mmSDMA0_GFX_RB_WPTR_HI), upper_32_bits(bytes));
}

/* What the whole sequence looks like when nothing is injected, and the set of registers it writes. */
static void learn_confirmed_writes(struct amdgpu_device *adev)
{
	int r;

	case_begin("a bring-up with nothing injected: the reference", adev);
	r = bc250_sdma_setup(adev);
	check(r == 0, "bc250_sdma_setup returns 0");
	r = bc250_sdma_hw_init(adev);
	check(r == 0, "bc250_sdma_hw_init returns 0");
	confirmed_add_current_writes();
	note("%u register writes over %u distinct registers, and that is the confirmed set\n",
	     g_write_count, g_confirmed_count);
	check(live_objects() == 3, "three live allocations: the write-back page and two ring buffers");
	check(!engine_halted(adev, 0) && !engine_halted(adev, 1), "both engines un-halted");
	check(engine_rb_enabled(adev, 0) && engine_rb_enabled(adev, 1), "both rings enabled");
	check(engine_ib_enabled(adev, 0) && engine_ib_enabled(adev, 1), "both IB paths enabled");

	r = bc250_sdma_hw_fini(adev);
	check(r == 0, "bc250_sdma_hw_fini returns 0 with both engines drained");
	check(engine_halted(adev, 0) && engine_halted(adev, 1), "both engines halted by the undo");
	check(!engine_rb_enabled(adev, 0) && !engine_rb_enabled(adev, 1), "both rings disabled");
	bc250_sdma_teardown(adev);
	check(live_objects() == 0, "the teardown gives all three back");
	check(g_double_free == 0 && g_unknown_free == 0, "no double free, no stray free");
	check(poison_violations() == 0, "nothing written into freed memory");
}

/* ---------------------------------------------------------------------------------------------
 * 1. Each allocation fails in turn
 *
 * bc250_sdma_setup() makes three: the write-back page, then engine 0's ring buffer, then engine 1's.
 * bc250_sdma_fence_page_alloc() makes a fourth, on its own.
 * ------------------------------------------------------------------------------------------- */

static void case_alloc_failure(struct amdgpu_device *adev, int which, const char *label)
{
	char title[128];
	int r;

	sprintf(title, "1. allocation %d fails (%s)", which, label);
	case_begin(title, adev);
	g_fail_at = which;

	r = bc250_sdma_setup(adev);
	check(r == BC250_ENOMEM, "bc250_sdma_setup hands back the allocator's error");
	check(live_objects() == 0, "every earlier allocation released, none left live");
	check(g_double_free == 0, "no allocation released twice");
	check(g_unknown_free == 0, "nothing released that was never handed out");
	check(poison_violations() == 0, "nothing written into freed memory");
	check(g_write_count == 0, "the failed setup wrote no register at all");
	check(adev->sdma.wb_mem.cpu == NULL && adev->sdma.wb_mem.size == 0,
	      "adev->sdma.wb_mem is cleared");
	check(adev->sdma.instance[0].ring.ring_mem.size == 0 &&
	      adev->sdma.instance[1].ring.ring_mem.size == 0,
	      "both ring_mem descriptors are cleared");

	/* The undo after a failed setup, and then the undo again. Neither may free anything twice. */
	bc250_sdma_teardown(adev);
	bc250_sdma_teardown(adev);
	check(live_objects() == 0 && g_double_free == 0,
	      "two more teardowns after the failed setup change nothing");

	/* And the retry a caller that gets -ENOMEM would make once memory is there again. */
	g_fail_at = -1;
	r = bc250_sdma_setup(adev);
	check(r == 0, "a retry after the failure sets up cleanly");
	check(live_objects() == 3, "and owns exactly three allocations");
	bc250_sdma_teardown(adev);
	check(live_objects() == 0, "which the teardown gives back");
}

static void case_fence_page_failure(struct amdgpu_device *adev)
{
	int r;

	case_begin("1d. the fence page allocation fails", adev);
	r = bc250_sdma_setup(adev);
	check(r == 0, "the setup itself succeeds");
	g_fail_at = 3;                  /* the fourth call: the fence page */
	r = bc250_sdma_fence_page_alloc(adev);
	check(r == BC250_ENOMEM, "bc250_sdma_fence_page_alloc hands back the allocator's error");
	check(live_objects() == 3, "the rings and the write-back page are untouched");
	check(adev->sdma.fence_mem.cpu == NULL, "fence_mem stays empty");
	bc250_sdma_fence_page_free(adev);
	check(g_double_free == 0 && live_objects() == 3, "freeing a page that was never allocated is a no-op");

	g_fail_at = -1;
	r = bc250_sdma_fence_page_alloc(adev);
	check(r == 0 && live_objects() == 4, "a retry allocates exactly one page");
	r = bc250_sdma_fence_page_alloc(adev);
	check(r == 0 && live_objects() == 4, "and a second call is idempotent, as the header says");
	bc250_sdma_fence_page_free(adev);
	bc250_sdma_teardown(adev);
	check(live_objects() == 0 && g_double_free == 0 && poison_violations() == 0,
	      "everything given back once");
}

/* ---------------------------------------------------------------------------------------------
 * 2. The allocation succeeds and hands back cpu == NULL
 * ------------------------------------------------------------------------------------------- */

static void case_null_cpu_wb(struct amdgpu_device *adev)
{
	int r;

	case_begin("2a. the write-back page comes back unmapped (cpu == NULL)", adev);
	g_null_cpu_at = 0;

	/* Before D-01 was fixed, this refusal did not happen: bc250_sdma_setup() returned 0,
	 * bc250_sdma_start() went on to program SDMA0_GFX_RB_WPTR_POLL_ADDR_LO/HI with the page's MC
	 * address and set F32_POLL_ENABLE - the engine told to poll a shadow the driver could never
	 * write - and D-02 then leaked the page on top of that. See sdma_faults_report.md for the
	 * measurements; there is nothing left to demonstrate once the setup itself refuses. */
	r = bc250_sdma_setup(adev);
	check(r != 0, "bc250_sdma_setup refuses a write-back page it cannot address");  /* D-01, fixed */

	bc250_sdma_teardown(adev);
	check(live_objects() == 0,
	      "and the teardown releases the unmapped write-back page");  /* D-02, fixed */
	note("live allocations after the teardown: %u\n", live_objects());
}

static void case_null_cpu_ring(struct amdgpu_device *adev, int which)
{
	char title[128];
	int r;

	sprintf(title, "2b. engine %d's ring buffer comes back unmapped (cpu == NULL)", which);
	case_begin(title, adev);
	g_null_cpu_at = 1 + which;      /* call 0 is the write-back page */

	r = bc250_sdma_setup(adev);
	check(r == BC250_EINVAL, "bc250_sdma_setup refuses a ring buffer it cannot address");
	check(adev->sdma.instance[which].ring.ring == NULL,
	      "and leaves that ring's CPU pointer NULL");
	check(g_double_free == 0 && poison_violations() == 0, "no double free, no write after free");
	check(live_objects() == 0,
	      "the unwind releases the unmapped ring buffer too");  /* D-02, fixed */
	note("live allocations after the unwind: %u (the write-back page and the other ring went"
	     " back; this one cannot)\n", live_objects());
}

static void case_null_cpu_fence_page(struct amdgpu_device *adev)
{
	int r;

	case_begin("2c. the fence page comes back unmapped (cpu == NULL)", adev);
	r = bc250_sdma_setup(adev);
	check(r == 0, "the setup itself succeeds");

	g_null_cpu_at = 3;              /* the fourth call */
	r = bc250_sdma_fence_page_alloc(adev);
	check(r == BC250_EINVAL, "bc250_sdma_fence_page_alloc refuses a page it cannot address");
	check(adev->sdma.fence_mem.mc == 0 && adev->sdma.fence_mem.size == 0,
	      "and clears the descriptor instead of leaving a dangling MC address");  /* D-03, fixed */
	check(live_objects() == 3, "the refused page is given back, not left live");  /* D-03, fixed */

	/* The second half of D-03: the idempotence test one line above used to key on fence_mem.cpu,
	 * which was NULL, so the next call - driver/kmd/gfx.c:384 makes exactly this call whenever
	 * SdmaFencePage is still FALSE - would allocate a second page over the first. Fixed: the
	 * descriptor is cleared on the refusal, so this call allocates cleanly. */
	g_null_cpu_at = -1;
	r = bc250_sdma_fence_page_alloc(adev);
	check(r == 0, "a later call succeeds");
	check(live_objects() == 4,
	      "and does not leave the earlier, refused page behind");  /* D-03, fixed */
	note("live allocations: %u\n", live_objects());

	bc250_sdma_fence_page_free(adev);
	bc250_sdma_teardown(adev);
	note("after fence_page_free and teardown, %u allocation(s) still live\n", live_objects());
}

/* ---------------------------------------------------------------------------------------------
 * 3. An engine number that is not an engine
 * ------------------------------------------------------------------------------------------- */

#define SLOT0_SENTINEL   0xA1A1A1A1u
#define SLOT1_SENTINEL   0xB2B2B2B2u

static void case_engine_number(struct amdgpu_device *adev)
{
	struct amdgpu_ring probe;
	volatile u32 *slots;
	u64 packet_addr;
	int r;

	case_begin("3. a ring whose me is 2", adev);
	check(bc250_sdma_setup(adev) == 0, "setup");
	check(bc250_sdma_fence_page_alloc(adev) == 0, "fence page");
	check(bc250_sdma_hw_init(adev) == 0, "bring-up");

	/* A ring object as a caller would build it from a bad index: engine 1's buffer, but an engine
	 * number of 2. Nothing in driver/shim produces this - bc250_sdma_setup() sets me to 0 and 1 -
	 * and driver/kmd/gfx.c:449 reaches the ring test through a checked table, so this is a caller
	 * one mistake away, not today's caller. */
	probe = adev->sdma.instance[1].ring;
	probe.me = 2;

	slots = (volatile u32 *)adev->sdma.fence_mem.cpu;
	slots[0] = SLOT0_SENTINEL;      /* engine 0's ring-test scratch dword */
	slots[2] = SLOT1_SENTINEL;      /* engine 1's, eight bytes on */

	r = bc250_sdma_ring_test(&probe);
	check(r == BC250_EINVAL,
	      "bc250_sdma_ring_test refuses an engine number it has no slot for");  /* D-07, fixed */
	note("it returned %d, and slot 0 still reads %08X\n", r, slots[0]);
	check(slots[0] == SLOT0_SENTINEL,
	      "and does not take engine 0's scratch slot");  /* D-07, fixed */
	check(slots[2] == SLOT1_SENTINEL, "engine 1's slot is untouched either way");

	/* The refusal is before amdgpu_ring_alloc(), so no packet is built at all: ring[1] and ring[2]
	 * are still whatever amdgpu_ring_clear_ring() filled the ring with at bc250_sdma_setup(), not
	 * a packet naming engine 0's slot. D-07, fixed. */
	packet_addr = (u64)probe.ring[1] | ((u64)probe.ring[2] << 32);
	check(packet_addr != bc250_sdma_fence_addr(adev, 0),
	      "and the ring carries no packet naming engine 0's scratch address");
	note("probe.ring[1..2] read %08X %08X (the NOP fill, not a WRITE_LINEAR destination)\n",
	     probe.ring[1], probe.ring[2]);

	/* The register window of the same bad instance number, for completeness. Upstream folds it
	 * the same way (sdma_v5_0.c:218: `if (instance == 1)` and nothing else), so this is a note
	 * and not a finding against us. */
	note("bc250_sdma_reg_offset(adev, 2, mmSDMA0_F32_CNTL) == instance %d's window"
	     " (upstream folds it the same way)\n",
	     sdma_reg(adev, 2, mmSDMA0_F32_CNTL) == sdma_reg(adev, 0, mmSDMA0_F32_CNTL) ? 0 : 1);

	check(writes_outside_confirmed() == 0, "no register outside the confirmed set was written");
	bc250_sdma_fence_page_free(adev);
	bc250_sdma_teardown(adev);
	check(live_objects() == 0 && poison_violations() == 0, "everything given back");
}

/* ---------------------------------------------------------------------------------------------
 * 4. A preserved write pointer that is not a pointer
 * ------------------------------------------------------------------------------------------- */

static void corrupt_wptr_case(struct amdgpu_device *adev, int inst, u64 kept,
			      int expect_adopt, const char *label)
{
	char title[160];
	struct amdgpu_ring *ring;
	int r;

	sprintf(title, "4. sdma%d comes back holding %s", inst, label);
	case_begin(title, adev);
	check(bc250_sdma_setup(adev) == 0, "setup");
	engine_keeps_wptr(adev, inst, kept);

	r = bc250_sdma_start(adev);
	ring = &adev->sdma.instance[inst].ring;

	if (expect_adopt) {
		check(r == 0, "the bring-up accepts it");
		check(ring->wptr == (kept >> 2), "ring->wptr is the engine's own pointer, in dwords");
		check(reg_get(sdma_reg(adev, inst, mmSDMA0_GFX_RB_WPTR)) == lower_32_bits(kept) &&
		      reg_get(sdma_reg(adev, inst, mmSDMA0_GFX_RB_WPTR_HI)) == upper_32_bits(kept),
		      "and it is programmed back into the engine");
		check(ring->wptr_cpu_addr != NULL &&
		      *(volatile u64 *)ring->wptr_cpu_addr == kept,
		      "the write-back shadow carries it too, before RB_ENABLE");
		note("indexing stays inside the ring: (wptr & buf_mask) = 0x%X of 0x%X dwords\n",
		     (unsigned int)(ring->wptr & ring->buf_mask), ring->buf_mask + 1u);
	} else {
		check(r == BC250_EINVAL, "the bring-up refuses it");
		check(!engine_rb_enabled(adev, inst), "that engine's ring is not enabled");
		check(writes_outside_confirmed() == 0,
		      "and the refusal wrote no register outside the confirmed set");
	}

	bc250_sdma_teardown(adev);
	check(live_objects() == 0 && poison_violations() == 0, "everything given back");
}

static void case_corrupt_wptr(struct amdgpu_device *adev)
{
	/* Refusals. The all-ones halves first, because they are what a faulted register sequence or a
	 * dead bus answers with; then the two alignments bc250_sdma.c:376-383 insists on. */
	corrupt_wptr_case(adev, 1, 0xFFFFFFFFFFFFFFFFULL, 0, "all ones in both halves");
	corrupt_wptr_case(adev, 1, 0x00000000FFFFFFFFULL, 0, "all ones in the low half");
	corrupt_wptr_case(adev, 1, 0xFFFFFFFF00000040ULL, 0, "all ones in the high half");
	corrupt_wptr_case(adev, 0, 0x0000000000000102ULL, 0, "a pointer that is not dword aligned");
	corrupt_wptr_case(adev, 0, 0x0000000000000110ULL, 0, "four dwords, not a whole submission");
	corrupt_wptr_case(adev, 1, 0x0000000000000004ULL, 0, "one dword");

	/* Adoptions. Both are deliberate: the pointer is 64-bit and monotonic, it does not have to
	 * fit the ring, and amdgpu_ring_write() wraps it with buf_mask (bc250_sdma.c:356-362). The
	 * second one is as far out as a 64-bit pointer goes without tripping the all-ones test. */
	corrupt_wptr_case(adev, 0, 0x0000000000002040ULL, 1, "a pointer past the end of the ring");
	corrupt_wptr_case(adev, 1, 0x00007FFFFFFFFFC0ULL, 1, "a wildly large but aligned pointer");
}

/* ---------------------------------------------------------------------------------------------
 * 5. Engine 1 refuses after engine 0 is already running
 * ------------------------------------------------------------------------------------------- */

static void case_second_engine_fails(struct amdgpu_device *adev)
{
	int r;

	case_begin("5. sdma1 refuses after sdma0 has started", adev);
	check(bc250_sdma_setup(adev) == 0, "setup");
	engine_keeps_wptr(adev, 1, 0x0000000000000102ULL);      /* not a whole submission */

	r = bc250_sdma_start(adev);
	check(r == BC250_EINVAL, "bc250_sdma_start reports the refusal");

	/* What the caller is holding at that moment. */
	check(engine_rb_enabled(adev, 0) && engine_ib_enabled(adev, 0) && !engine_halted(adev, 0),
	      "sdma0 is left running on its ring");
	check(!engine_rb_enabled(adev, 1), "sdma1's ring is not enabled");
	check(!engine_halted(adev, 1),
	      "but sdma1 is un-halted: bc250_sdma_enable(true) ran before the refusal");
	print_engines(adev, "after the refused start");
	note("upstream does the same: sdma_v5_0_start() un-halts both engines, sdma_v5_0_gfx_resume()"
	     " returns on the first failing instance and re-halts nothing\n");
	check(live_objects() == 3, "the start frees nothing, as it allocates nothing");
	check(writes_outside_confirmed() == 0, "no register outside the confirmed set was written");

	/* What a following undo does with that state. */
	r = bc250_sdma_hw_fini(adev);
	check(r == BC250_EBUSY,
	      "bc250_sdma_hw_fini reports BC250_EBUSY: sdma1 halted with rptr behind wptr");
	check(engine_halted(adev, 0) && engine_halted(adev, 1), "both engines halted all the same");
	check(!engine_rb_enabled(adev, 0) && !engine_rb_enabled(adev, 1), "both rings disabled");
	note("driver/kmd/gfx.c Fini() turns that BC250_EBUSY into pages that are kept, so the three"
	     " allocations below outlive the device\n");

	/* And what a second setup does, which is the other half of the question. D-04, fixed: it now
	 * refuses instead of re-allocating over the three live allocations and leaking them. */
	r = bc250_sdma_setup(adev);
	check(r == BC250_EINVAL, "a second bc250_sdma_setup on the same adev is refused");
	check(live_objects() == 3, "and leaves the first setup's three allocations alone");
	note("live allocations after the refused second setup: %u\n", live_objects());

	bc250_sdma_teardown(adev);
	note("and after the teardown: %u\n", live_objects());
	check(g_double_free == 0 && g_unknown_free == 0 && poison_violations() == 0,
	      "no double free and nothing written into freed memory");
}

/* ---------------------------------------------------------------------------------------------
 * 6. The undo path: after a failed setup, twice over, and what the rings still look like
 * ------------------------------------------------------------------------------------------- */

static void case_fini_after_failed_setup(struct amdgpu_device *adev)
{
	int r, i;

	case_begin("6a. start and fini after a setup that failed", adev);
	g_fail_at = 0;                  /* the write-back page: the rings never get their funcs */
	r = bc250_sdma_setup(adev);
	check(r == BC250_ENOMEM, "the setup fails");
	check(adev->sdma.num_instances == AMDGPU_MAX_SDMA_INSTANCES,
	      "num_instances is left at 2 by the failed setup");
	for (i = 0; i < AMDGPU_MAX_SDMA_INSTANCES; i++) {
		check(adev->sdma.instance[i].ring.funcs == NULL,
		      "and the rings have no funcs, because the failure came before the loop");
		check(adev->sdma.instance[i].ring.ring_size == 0, "and no size");
	}

	/* bc250_sdma_start() in that state. The engine holds nothing here, so the adoption arm - the
	 * one that would dereference ring->funcs->align_mask at bc250_sdma.c:377 - is not taken and
	 * this is safe to run. On a re-init, where the engine does hold a pointer (facts M59/M60),
	 * the same call reaches that dereference with funcs == NULL. That path is NOT exercised here:
	 * it is a null dereference, and in the miniport a bugcheck. */
	g_fail_at = -1;
	r = bc250_sdma_start(adev);
	check(r == BC250_EINVAL,
	      "bc250_sdma_start refuses an adev whose setup did not finish");  /* D-05, fixed */
	note("it returned %d; SDMA0_GFX_RB_BASE = %08X and RB_CNTL = %08X\n", r,
	     reg_get(sdma_reg(adev, 0, mmSDMA0_GFX_RB_BASE)),
	     reg_get(sdma_reg(adev, 0, mmSDMA0_GFX_RB_CNTL)));
	check(!engine_rb_enabled(adev, 0) && !engine_rb_enabled(adev, 1),
	      "neither engine was enabled on a ring that does not exist");  /* D-05, fixed */
	check(writes_outside_confirmed() == 0,
	      "the refusal wrote no register at all, let alone one outside the confirmed set");

	r = bc250_sdma_hw_fini(adev);
	note("bc250_sdma_hw_fini after that returns %d\n", r);
	check(engine_halted(adev, 0) && engine_halted(adev, 1), "and halts both engines");
	check(writes_outside_confirmed() == 0, "writing nothing outside the confirmed set");

	bc250_sdma_teardown(adev);
	bc250_sdma_teardown(adev);
	check(live_objects() == 0 && g_double_free == 0, "two teardowns, nothing freed twice");
}

static void case_teardown_leaves_ring_usable(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring;
	int r;

	case_begin("6b. the rings after a teardown", adev);
	check(bc250_sdma_setup(adev) == 0, "setup");
	check(bc250_sdma_fence_page_alloc(adev) == 0, "fence page");
	check(bc250_sdma_hw_init(adev) == 0, "bring-up");

	/* The documented order is the other way round - the fence page goes before the teardown, and
	 * driver/kmd/gfx.c:150-154 does it in that order - so this is a caller's mistake. What the
	 * case is about is what the mistake costs: whether the teardown leaves behind a ring that
	 * still looks usable. bc250_ring_alloc_mem() states the opposite rule for the CP rings
	 * ("leaves ring->ring NULL, so that a ring whose allocation did not finish cannot be written
	 * to", bc250_gfx.c:1702-1704). */
	bc250_sdma_teardown(adev);
	ring = &adev->sdma.instance[0].ring;
	check(live_objects() == 1, "only the fence page is still live");
	check(ring->ring == NULL, "the teardown leaves ring->ring NULL");  /* D-06, fixed */
	check(ring->wptr_cpu_addr == NULL,
	      "and the write-pointer shadow pointer NULL");  /* D-06, fixed */

	r = bc250_sdma_ring_test(ring);
	check(r == BC250_EINVAL, "a ring test after the teardown is refused");  /* D-06, fixed */
	note("it returned %d\n", r);
	check(poison_violations() == 0,
	      "and nothing was written into the freed ring buffer");  /* D-06, fixed */
	note("%u freed allocation(s) were written into\n", poison_violations());

	bc250_sdma_fence_page_free(adev);
	check(live_objects() == 0, "the fence page goes back");
	check(g_double_free == 0 && g_unknown_free == 0, "no double free, no stray free");
}

/* ---------------------------------------------------------------------------------------------
 * 7. The queue reset of one engine (bc250_sdma_reset_retained_instance), which ResetFromTimeout
 *    runs on SDMA0 when a paging packet did not retire (docs/design/queue-reset.md section 13).
 *    amdgpu_sdma_reset_engine() goes on to the soft reset when the stop times out: an engine that a
 *    hang holds does not freeze. Until 2026-11-08 this helper returned at that point, so the case it
 *    exists for never reached the pulse. The negative control of run_sdma_faults.ps1 puts that
 *    early return back and must fail here.
 * ------------------------------------------------------------------------------------------- */

#define RESET_POISON 0xDEADBEEFu

static void reset_case_begin(const char *title, struct amdgpu_device *adev, int hung, int survives,
			     unsigned int refuse_at)
{
	struct amdgpu_ring *ring;
	u32 i;

	case_begin(title, adev);
	check(bc250_sdma_setup(adev) == 0, "setup");
	check(bc250_sdma_hw_init(adev) == 0, "bring-up");
	/* SOC15_REG_OFFSET(GC, 0, mmGRBM_SOFT_RESET), written out: soc15_common.h is not included here. */
	g_grbm_reset_index = (u32)(adev->reg_offset[GC_HWIP][0][mmGRBM_SOFT_RESET_BASE_IDX] + mmGRBM_SOFT_RESET);
	g_freeze_index = sdma_reg(adev, 0, mmSDMA0_FREEZE);
	g_status1_index = sdma_reg(adev, 0, mmSDMA0_STATUS1_REG);
	/* The state a hang leaves: a ring full of old packets and non-zero pointers everywhere. */
	ring = &adev->sdma.instance[0].ring;
	for (i = 0; i < ring->ring_size / 4u; i++)
		ring->ring[i] = RESET_POISON;
	ring->wptr = 0x40;
	*(volatile u64 *)ring->wptr_cpu_addr = 0x100;
	*(volatile u64 *)((char *)adev->sdma.wb_mem.cpu) = 0x80;	/* engine 0's read pointer slot */
	g_reset_model = 1;
	g_sdma0_hung = hung;
	g_hang_survives_pulse = survives;
	g_sdma0_pulses = g_sdma1_pulses = 0;
	g_scope_entries = g_scope_exits = g_scope_open = 0;
	g_scope_refuse_at = refuse_at;
}

static void reset_case_end(struct amdgpu_device *adev)
{
	g_reset_model = 0;
	(void)bc250_sdma_hw_fini(adev);
	bc250_sdma_teardown(adev);
	check(live_objects() == 0, "the teardown gives everything back");
}

static int ring_holds(struct amdgpu_ring *ring, u32 value)
{
	u32 i;

	for (i = 0; i < ring->ring_size / 4u; i++)
		if (ring->ring[i] != value)
			return 0;
	return 1;
}

static void case_reset_hung_engine(struct amdgpu_device *adev)
{
	struct bc250_sdma_reset_receipt receipt;
	struct amdgpu_ring *ring;
	int r;

	reset_case_begin("7a. queue reset of a hung engine 0: stop times out, the pulse ends the hang", adev,
			 1, 0, 0);
	ring = &adev->sdma.instance[0].ring;
	r = bc250_sdma_reset_retained_instance(adev, 0, &receipt);
	note("rc %d, stage %u, stop %d, pulses %u/%u, scopes %u/%u\n", r, receipt.stage,
	     receipt.stop_result, g_sdma0_pulses, g_sdma1_pulses, g_scope_entries, g_scope_exits);
	check(r == 0, "the reset returns 0");
	check(receipt.stop_result == BC250_ETIME, "the first stop timed out, as a hung engine does");
	check(receipt.stage == BC250_SDMA_RESET_PROGRAMMED, "every stage ran");
	check(g_sdma0_pulses == 1 && g_sdma1_pulses == 0, "one pulse of SDMA0, none of SDMA1");
	check(g_scope_entries == 2 && g_scope_exits == 2 && g_scope_open == 0,
	      "two safe-mode scopes, both closed");
	check(ring->wptr == 0 && *(volatile u64 *)ring->wptr_cpu_addr == 0 &&
	      *(volatile u64 *)adev->sdma.wb_mem.cpu == 0, "software pointers and write-back slots at 0");
	check(ring->ring[0] != RESET_POISON && ring_holds(ring, ring->ring[0]), "the old packets are gone");
	check(!engine_halted(adev, 0) && engine_rb_enabled(adev, 0) && engine_ib_enabled(adev, 0),
	      "engine 0 runs again");
	check(!engine_halted(adev, 1) && engine_rb_enabled(adev, 1), "engine 1 was not touched");
	reset_case_end(adev);
}

static void case_reset_idle_engine(struct amdgpu_device *adev)
{
	struct bc250_sdma_reset_receipt receipt;
	int r;

	reset_case_begin("7b. queue reset of an idle engine 0: the stop freezes it", adev, 0, 0, 0);
	r = bc250_sdma_reset_retained_instance(adev, 0, &receipt);
	check(r == 0 && receipt.stop_result == 0 && receipt.stage == BC250_SDMA_RESET_PROGRAMMED,
	      "the reset returns 0 with a clean stop");
	check(g_sdma0_pulses == 1 && g_scope_entries == 2 && g_scope_exits == 2, "one pulse, two scopes");
	reset_case_end(adev);
}

static void case_reset_hang_survives(struct amdgpu_device *adev)
{
	struct bc250_sdma_reset_receipt receipt;
	struct amdgpu_ring *ring;
	int r;

	reset_case_begin("7c. a hang the pulse does not end: refused before memory changes", adev, 1, 1, 0);
	ring = &adev->sdma.instance[0].ring;
	r = bc250_sdma_reset_retained_instance(adev, 0, &receipt);
	check(r == BC250_ETIME, "the second stop times out and is the result");
	check(receipt.stop_result == BC250_ETIME && receipt.stage == BC250_SDMA_RESET_CLEAR,
	      "the receipt names the clear stage");
	check(g_sdma0_pulses == 1 && g_scope_exits == g_scope_entries && g_scope_open == 0,
	      "one pulse, every scope closed");
	check(ring_holds(ring, RESET_POISON) && ring->wptr == 0x40, "the ring and its pointers are untouched");
	reset_case_end(adev);
}

static void case_reset_scope_refused(struct amdgpu_device *adev)
{
	struct bc250_sdma_reset_receipt receipt;
	int r;

	reset_case_begin("7d. the RLC refuses the first safe-mode scope: no register of the engine changes", adev,
			 1, 0, 1);
	r = bc250_sdma_reset_retained_instance(adev, 0, &receipt);
	check(r == BC250_ETIME && receipt.stage == BC250_SDMA_RESET_STOP && receipt.stop_result == 0,
	      "refused at the stop stage, no stop ran");
	check(g_sdma0_pulses == 0 && g_scope_exits == 1, "no pulse; the refused scope is closed again");
	reset_case_end(adev);
}

/* ------------------------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	static struct amdgpu_device adev;       /* static: ~50 KB, more than a stack frame wants */
	int i;

	for (i = 1; i < argc; i++)
		if (strcmp(argv[i], "-v") == 0)
			g_verbose = 1;

	printf("SDMA fault injection: driver/shim/bc250_sdma.c against a failing allocator,\n");
	printf("an allocator that cannot map what it hands out, and engines that come back\n");
	printf("holding a write pointer that is not a pointer.\n");

	learn_confirmed_writes(&adev);

	case_alloc_failure(&adev, 0, "the write-back page");
	case_alloc_failure(&adev, 1, "engine 0's ring buffer");
	case_alloc_failure(&adev, 2, "engine 1's ring buffer");
	case_fence_page_failure(&adev);

	case_null_cpu_wb(&adev);
	case_null_cpu_ring(&adev, 0);
	case_null_cpu_ring(&adev, 1);
	case_null_cpu_fence_page(&adev);

	case_engine_number(&adev);
	case_corrupt_wptr(&adev);
	case_second_engine_fails(&adev);
	case_fini_after_failed_setup(&adev);
	case_teardown_leaves_ring_usable(&adev);

	case_reset_hung_engine(&adev);
	case_reset_idle_engine(&adev);
	case_reset_hang_survives(&adev);
	case_reset_scope_refused(&adev);

	fault_alloc_reset();
	check(g_rlc_safe_calls == 0, "no case entered the RLC safe-mode scope this suite does not model");

	printf("\n== verdict ==\n");
	printf("  %u checks, %u failures, %u expected failures (confirmed defects), %u XPASS\n",
	       g_checks, g_failures, g_xfail, g_xpass);
	if (g_xpass)
		printf("  an expectation marked as a defect now passes: update sdma_faults.c\n");
	printf("  %s\n", g_failures == 0 ? "PASS" : "FAIL");
	return g_failures == 0 ? 0 : 1;
}
