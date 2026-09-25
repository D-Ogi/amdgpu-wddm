/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Host test of the DXGK_PTE -> AMD page table entry translation (driver/shim/bc250_pte.c).
 *
 *   replay_pte [-v]
 *
 * There is no trace and no sweep here, and there is nothing to replay: the unit under test reads
 * no register, writes no register and allocates nothing, so its entire input space is its
 * arguments. What takes the place of a replay is holding it against the two things it is not free
 * to invent.
 *
 *   1. The real DXGK_PTE. The Windows half of the translation is a bit layout that belongs to
 *      Microsoft, so this file includes the WDK header through <d3dkmthk.h>, builds a real
 *      DXGK_PTE, sets one field at a time and asserts that each one lands exactly where the
 *      BC250_DXGK_PTE_* macro says it does. Every case below then goes through a real DXGK_PTE
 *      rather than through a hand-assembled flags word, so the translation is exercised against
 *      the compiler's idea of the struct and not against ours. If Microsoft moves a bit, this
 *      test fails rather than the driver mapping the wrong page.
 *
 *   2. The measured GART entry. bc250_pte_gart_flags() has to equal bc250_gart_pte_flags() from
 *      driver/shim/bc250_gart.c - the path that filled unit A's table with 0x0003000000000077 and
 *      that the CP then fetched rings and MQDs through, facts M33 and M37 - and both have to equal
 *      that number. bc250_gart.c is linked into this test for exactly that comparison.
 *
 * After that: a table of translations with the expected 64-bit entry written out in full, a table
 * of inputs that must be refused with nothing produced, the round trip back through
 * bc250_pte_decode(), and four controls, each of which is a specific wrong implementation this
 * suite would otherwise not notice.
 *
 * The VRAM base and size below are the test's own numbers and nothing else: on hardware they come
 * from the miniport, which gets them from the frame buffer registers. What is under test is the
 * arithmetic that uses them.
 */
#include <windows.h>
#include <d3dkmthk.h>		/* the only legal route to d3dukmdt.h in user mode, d3dukmdt.h:18-23 */

#include <stdio.h>
#include <string.h>

#include "bc250_gart.h"
#include "bc250_gmc.h"		/* BC250_EINVAL */
#include "bc250_pte.h"
#include "navi10_enum.h"	/* MTYPE_UC, MTYPE_NC */

/* The test's own segment layout. 31 is DXGK_SEGMENT_ID_SYSTEMMEMORY (d3dkmddi.h:2643), which is a
 * kernel-mode header and so is not visible here; the number is written out with that citation
 * rather than included, and the driver passes whatever it decides through the context anyway. */
#define SYS_SEGMENT	31u
#define VRAM_SEGMENT	1u
#define VRAM_BASE	0x0000000400000000ull
#define VRAM_SIZE	0x0000000010000000ull

#define GART_MEASURED	0x0003000000000077ull   /* fact M37 */

static int g_verbose;
static int g_failures;

static void fail(const char *what)
{
	printf("  FAIL %s\n", what);
	g_failures++;
}

/* ---------------------------------------------------------------------------------------------
 * 1. The DXGK_PTE layout, field by field, against the real struct
 * ------------------------------------------------------------------------------------------- */

enum probe_field {
	F_VALID, F_ZERO, F_COHERENT, F_READONLY, F_NOEXEC, F_SEGMENT, F_LARGEPAGE,
	F_ADAPTER, F_PAGESIZE, F_SYSRESERVED0, F_RESERVED, F_COUNT
};

/* Set one field to all ones for its width and return the Flags word it produced. A switch rather
 * than a table because a bitfield has no address to take. */
static u64 probe(enum probe_field which)
{
	DXGK_PTE pte;

	memset(&pte, 0, sizeof(pte));
	switch (which) {
	case F_VALID:		pte.Valid = 1; break;
	case F_ZERO:		pte.Zero = 1; break;
	case F_COHERENT:	pte.CacheCoherent = 1; break;
	case F_READONLY:	pte.ReadOnly = 1; break;
	case F_NOEXEC:		pte.NoExecute = 1; break;
	case F_SEGMENT:		pte.Segment = 0x1F; break;
	case F_LARGEPAGE:	pte.LargePage = 1; break;
	case F_ADAPTER:		pte.PhysicalAdapterIndex = 0x3F; break;
	case F_PAGESIZE:	pte.PageTablePageSize = 0x3; break;
	case F_SYSRESERVED0:	pte.SystemReserved0 = 1; break;
	case F_RESERVED:	pte.Reserved = 0xFFFFFFFFFFFull; break;
	default:		break;
	}
	return (u64)pte.Flags;
}

static void check_layout(void)
{
	static const struct {
		const char *name;
		u64 ours;
	} expect[F_COUNT] = {
		{ "Valid",                BC250_DXGK_PTE_VALID },
		{ "Zero",                 BC250_DXGK_PTE_ZERO },
		{ "CacheCoherent",        BC250_DXGK_PTE_CACHECOHERENT },
		{ "ReadOnly",             BC250_DXGK_PTE_READONLY },
		{ "NoExecute",            BC250_DXGK_PTE_NOEXECUTE },
		{ "Segment",              BC250_DXGK_PTE_SEGMENT_MASK },
		{ "LargePage",            BC250_DXGK_PTE_LARGEPAGE },
		{ "PhysicalAdapterIndex", BC250_DXGK_PTE_ADAPTER_MASK },
		{ "PageTablePageSize",    BC250_DXGK_PTE_PAGESIZE_MASK },
		{ "SystemReserved0",      BC250_DXGK_PTE_SYSRESERVED0 },
		{ "Reserved",             BC250_DXGK_PTE_RESERVED_MASK },
	};
	DXGK_PTE pte;
	volatile size_t sz;      /* through volatile so the comparison is not constant (MSVC C4127) */
	u64 covered = 0;
	int i;

	printf("\n== DXGK_PTE layout, against d3dukmdt.h:315-340 as the compiler sees it\n");
	for (i = 0; i < (int)F_COUNT; i++) {
		u64 got = probe((enum probe_field)i);

		if (g_verbose || got != expect[i].ours)
			printf("  %-22s header %016llX  ours %016llX\n", expect[i].name,
			       (unsigned long long)got, (unsigned long long)expect[i].ours);
		if (got != expect[i].ours)
			g_failures++;
		covered |= got;
	}
	printf("  eleven fields checked, %s\n", g_failures == 0 ? "every one where we said" : "SEE ABOVE");

	/* The fields have to tile the whole word, or one of ours is the wrong width. */
	if (covered != ~0ull)
		fail("the eleven fields do not cover all 64 bits of Flags");

	sz = sizeof(DXGK_PTE);
	if (sz != 16)
		fail("DXGK_PTE is not 16 bytes");

	/* The second union is one qword at offset 8 and PageAddress and PageTableAddress are the
	 * same storage, which is why bc250_pte_from_dxgk() takes one address argument for both. */
	memset(&pte, 0, sizeof(pte));
	pte.PageTableAddress = 0xAABBCCDDEEFF0000ull;
	if (pte.PageAddress != 0xAABBCCDDEEFF0000ull || pte.Flags != 0)
		fail("PageAddress and PageTableAddress are not the same storage");

	{
		/* through volatile so the comparison is not a constant expression (MSVC C4127) */
		volatile int page_size[2] = { DXGK_PTE_PAGE_TABLE_PAGE_4KB, DXGK_PTE_PAGE_TABLE_PAGE_64KB };

		if (page_size[0] != (int)BC250_DXGK_PAGE_TABLE_PAGE_4KB ||
		    page_size[1] != (int)BC250_DXGK_PAGE_TABLE_PAGE_64KB)
			fail("DXGK_PTE_PAGE_SIZE does not enumerate what we say it does");
	}
}

/* ---------------------------------------------------------------------------------------------
 * 2. The measured GART flags, against the code path that produced them on unit A
 * ------------------------------------------------------------------------------------------- */

static void check_gart_flags(void)
{
	struct amdgpu_device adev;
	u64 from_gart, from_pte;

	memset(&adev, 0, sizeof(adev));
	from_gart = bc250_gart_pte_flags(&adev);
	from_pte = bc250_pte_gart_flags();

	printf("\n== the GART leaf flags\n");
	printf("  bc250_gart_pte_flags()  %016llX\n", (unsigned long long)from_gart);
	printf("  bc250_pte_gart_flags()  %016llX\n", (unsigned long long)from_pte);
	printf("  fact M37 on unit A      %016llX\n", (unsigned long long)GART_MEASURED);
	if (from_gart != GART_MEASURED || from_pte != GART_MEASURED)
		fail("the GART flags are not the ones the hardware ran with");
}

/* ---------------------------------------------------------------------------------------------
 * 3. Translations
 * ------------------------------------------------------------------------------------------- */

/* What a DXGK_PTE says, in the order the header declares it. Built into a real DXGK_PTE by
 * make_pte() so that no case bypasses Microsoft's layout. */
struct dxgk_in {
	unsigned int valid, zero, coherent, readonly, noexec;
	unsigned int segment, largepage, adapter, pagesize, sysreserved;
	unsigned int reserved;          /* bit 20, the lowest of the Reserved field */
	u64 address;
};

static void make_pte(const struct dxgk_in *in, DXGK_PTE *pte)
{
	memset(pte, 0, sizeof(*pte));
	pte->Valid = in->valid;
	pte->Zero = in->zero;
	pte->CacheCoherent = in->coherent;
	pte->ReadOnly = in->readonly;
	pte->NoExecute = in->noexec;
	pte->Segment = in->segment;
	pte->LargePage = in->largepage;
	pte->PhysicalAdapterIndex = in->adapter;
	pte->PageTablePageSize = in->pagesize;
	pte->SystemReserved0 = in->sysreserved;
	pte->Reserved = in->reserved;
	pte->PageAddress = in->address;
}

/* A readable, writable, executable, coherent system-memory leaf: the common case. */
#define RWX_SYSTEM(addr) { 1, 0, 1, 0, 0, SYS_SEGMENT, 0, 0, 0, 0, 0, (addr) }
#define RWX_VRAM(off)    { 1, 0, 1, 0, 0, VRAM_SEGMENT, 0, 0, 0, 0, 0, (off) }

static struct bc250_pte_context vm_ctx(void)
{
	struct bc250_pte_context ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.units = BC250_PTE_ADDR_BYTES;
	ctx.aperture = BC250_PTE_VM;
	ctx.system_segment = SYS_SEGMENT;
	ctx.vram_segment = VRAM_SEGMENT;
	ctx.vram_base = VRAM_BASE;
	ctx.vram_size = VRAM_SIZE;
	ctx.system_limit = 0;
	return ctx;
}

struct good_case {
	const char *name;
	enum bc250_pte_kind kind;
	enum bc250_pte_aperture aperture;
	enum bc250_pte_addr_units units;
	struct dxgk_in in;
	u64 expect;
};

/*
 * Every expected entry is written out in full rather than computed, so that a change to the flag
 * builders shows up here as a difference and not as the same arithmetic done twice.
 *
 * The VM leaf flags, from the named constants: VALID 0x01, SYSTEM 0x02, SNOOPED 0x04,
 * EXECUTABLE 0x10, READABLE 0x20, WRITEABLE 0x40, and MTYPE_NC = 0 in bits 50:48, which is why a
 * VM entry has nothing above bit 47 and a GART entry carries 0x0003 up there for MTYPE_UC.
 */
static const struct good_case g_good[] = {
	{ "VM leaf, system memory, rwx, coherent",
	  BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  RWX_SYSTEM(0x0000001234567000ull), 0x0000001234567077ull },

	{ "VM leaf, VRAM, rwx, coherent (offset + vram_base)",
	  BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  RWX_VRAM(0x2000ull), 0x0000000400002075ull },

	{ "VM leaf, VRAM, read only, no execute, not coherent",
	  BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  { 1, 0, 0, 1, 1, VRAM_SEGMENT, 0, 0, 0, 0, 0, 0x2000ull }, 0x0000000400002021ull },

	{ "VM leaf, system memory, 64 KB large page (FRAG 4)",
	  BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  { 1, 0, 1, 0, 0, SYS_SEGMENT, 1, 0, 0, 0, 0, 0x0000001234560000ull },
	  0x0000001234560277ull },

	{ "VM directory, system memory, coherent",
	  BC250_PTE_DIRECTORY, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  RWX_SYSTEM(0x0000000089ABC000ull), 0x0000000089ABC007ull },

	{ "VM directory, VRAM, coherent",
	  BC250_PTE_DIRECTORY, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  RWX_VRAM(0x3000ull), 0x0000000400003005ull },

	{ "VM directory, VRAM, not coherent",
	  BC250_PTE_DIRECTORY, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  { 1, 0, 0, 0, 0, VRAM_SEGMENT, 0, 0, 0, 0, 0, 0x3000ull }, 0x0000000400003001ull },

	{ "not valid: every flag clear, and the rest of the entry ignored",
	  BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  { 0, 0, 1, 1, 1, 7, 1, 3, 3, 1, 1, 0x0000001234567800ull }, 0ull },

	{ "GART leaf, system memory: the entry fact M37 measured",
	  BC250_PTE_LEAF, BC250_PTE_GART, BC250_PTE_ADDR_BYTES,
	  RWX_SYSTEM(0x0000001234567000ull), 0x0003001234567077ull },

	{ "VM leaf, the same page read as a frame number instead of a byte address",
	  BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_PAGES,
	  RWX_SYSTEM(0x0000000001234567ull), 0x0000001234567077ull },

	{ "VM leaf, the highest address the 48-bit field can carry",
	  BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  RWX_SYSTEM(0x0000FFFFFFFFF000ull), 0x0000FFFFFFFFF077ull },
};

/* Match the real WDK Zero flag to the Linux GFX10 null-BO encoding.
 * An arbitrary PageAddress must never become a backing page. Directory Zero
 * terminates the walk rather than following a pointer to address zero. */
static void check_zero_prt(void)
{
	struct bc250_pte_context ctx = vm_ctx();
	unsigned int level, readonly, noexec, valid;
	for (valid=0; valid<2; valid++) for (level=0; level<4; level++)
	for (readonly=0; readonly<2; readonly++)
	for (noexec=0; noexec<2; noexec++) {
		DXGK_PTE pte;
		u64 out=~0ull;
		u64 expected=level ? 0x00C8000000000006ull : 0x0088000000000006ull;
		struct bc250_pte_fields fields;
		enum bc250_pte_kind kind=level ? BC250_PTE_DIRECTORY : BC250_PTE_LEAF;
		memset(&pte,0,sizeof(pte));
		pte.Valid=valid; pte.Zero=1; pte.ReadOnly=readonly; pte.NoExecute=noexec;
		pte.PageAddress=~0ull; pte.Segment=7;
		if (bc250_pte_from_dxgk(&ctx,kind,pte.Flags,pte.PageAddress,&out)!=0 ||
		    out!=expected) fail("Zero terminal encoding differs from GFX10 null BO");
		bc250_pte_decode(out,kind,&fields);
		if (fields.valid || fields.address || !fields.prt || !fields.system ||
		    !fields.snooped || !fields.log || fields.readable || fields.writeable ||
		    fields.executable || fields.pde_pte!=(level!=0))
			fail("Zero entry carries a backing address or wrong terminal flags");
		pte.Zero=0; pte.Valid=1;
		if (bc250_pte_from_dxgk(&ctx,kind,pte.Flags,pte.PageAddress,&out)==0)
			fail("ordinary entry must still resolve its backing address");
	}
	printf("  Zero/PRT: 4 levels, protection variants and ordinary-map controls PASS\n");
}

static void check_good(void)
{
	unsigned int i;

	printf("\n== translations\n");
	for (i = 0; i < sizeof(g_good) / sizeof(g_good[0]); i++) {
		const struct good_case *c = &g_good[i];
		struct bc250_pte_context ctx = vm_ctx();
		char text[128];
		DXGK_PTE pte;
		u64 got = 0xDEADBEEFDEADBEEFull;
		int rc;

		ctx.aperture = c->aperture;
		ctx.units = c->units;
		make_pte(&c->in, &pte);

		rc = bc250_pte_from_dxgk(&ctx, c->kind, (u64)pte.Flags, (u64)pte.PageAddress, &got);
		bc250_pte_describe(got, c->kind, text, sizeof(text));

		printf("  %-58s %016llX  %s\n", c->name, (unsigned long long)got, text);
		if (rc != 0) {
			fail("refused a translation that should have worked");
			continue;
		}
		if (got != c->expect) {
			printf("        expected %016llX\n", (unsigned long long)c->expect);
			g_failures++;
		}
	}
}

/* ---------------------------------------------------------------------------------------------
 * 4. Inputs that must be refused, with nothing produced
 * ------------------------------------------------------------------------------------------- */

/* A context defect is expressed by patching the context rather than the entry. */
enum ctx_defect {
	CTX_PLAIN = 0,
	CTX_SAME_SEGMENTS,      /* system_segment == vram_segment */
	CTX_NO_VRAM,            /* vram_size 0: no VRAM segment is mapped */
	CTX_SYSTEM_LIMIT,       /* a bound on host addresses */
	CTX_ODD_VRAM_BASE       /* a VRAM base that is not 64-byte aligned */
};

struct bad_case {
	const char *name;
	enum bc250_pte_kind kind;
	enum bc250_pte_aperture aperture;
	enum bc250_pte_addr_units units;
	enum ctx_defect defect;
	struct dxgk_in in;
};

static const struct bad_case g_bad[] = {
	{ "a byte address with the low 12 bits set", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN, RWX_SYSTEM(0x0000001234567800ull) },

	{ "a frame number that would shift out of 64 bits", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_PAGES, CTX_PLAIN, RWX_SYSTEM(0x0010000000000000ull) },

	{ "a byte address above the 48 bits the field carries", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN, RWX_SYSTEM(0x0001000000000000ull) },

	{ "Zero is not supported by the fixed GART", BC250_PTE_LEAF, BC250_PTE_GART, BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 1, 1, 0, 0, SYS_SEGMENT, 0, 0, 0, 0, 0, 0x1000ull } },

	{ "SystemReserved0 set", BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 0, 0, SYS_SEGMENT, 0, 0, 0, 1, 0, 0x1000ull } },

	{ "a Reserved bit set", BC250_PTE_LEAF, BC250_PTE_VM, BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 0, 0, SYS_SEGMENT, 0, 0, 0, 0, 1, 0x1000ull } },

	{ "another adapter's memory (PhysicalAdapterIndex 1)", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 0, 0, SYS_SEGMENT, 0, 1, 0, 0, 0, 0x1000ull } },

	{ "a 64 KB page table page", BC250_PTE_DIRECTORY, BC250_PTE_VM, BC250_PTE_ADDR_BYTES,
	  CTX_PLAIN, { 1, 0, 1, 0, 0, SYS_SEGMENT, 0, 0, BC250_DXGK_PAGE_TABLE_PAGE_64KB, 0, 0,
		       0x1000ull } },

	{ "a segment the context does not name", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 0, 0, 7, 0, 0, 0, 0, 0, 0x1000ull } },

	{ "a VRAM offset one page past the end of the segment", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN, RWX_VRAM(VRAM_SIZE) },

	{ "a VRAM entry when the context maps no VRAM", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_NO_VRAM, RWX_VRAM(0x1000ull) },

	{ "a host address past the context's system limit", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_SYSTEM_LIMIT, RWX_SYSTEM(0x0000000800000000ull) },

	{ "a context whose two segment ids are the same", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_SAME_SEGMENTS, RWX_SYSTEM(0x1000ull) },

	{ "a directory address that is not 64-byte aligned", BC250_PTE_DIRECTORY, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_ODD_VRAM_BASE, RWX_VRAM(0x1000ull) },

	{ "LargePage on a directory entry", BC250_PTE_DIRECTORY, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 0, 0, SYS_SEGMENT, 1, 0, 0, 0, 0, 0x0000001234560000ull } },

	{ "a large page whose address is not 64 KB aligned", BC250_PTE_LEAF, BC250_PTE_VM,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 0, 0, SYS_SEGMENT, 1, 0, 0, 0, 0, 0x0000001234561000ull } },

	{ "GART asked for VRAM, which that aperture cannot express", BC250_PTE_LEAF,
	  BC250_PTE_GART, BC250_PTE_ADDR_BYTES, CTX_PLAIN, RWX_VRAM(0x1000ull) },

	{ "GART asked for a read-only page", BC250_PTE_LEAF, BC250_PTE_GART,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 1, 0, SYS_SEGMENT, 0, 0, 0, 0, 0, 0x1000ull } },

	{ "GART asked for a no-execute page", BC250_PTE_LEAF, BC250_PTE_GART,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 1, 0, 1, SYS_SEGMENT, 0, 0, 0, 0, 0, 0x1000ull } },

	{ "GART asked for an uncoherent page", BC250_PTE_LEAF, BC250_PTE_GART,
	  BC250_PTE_ADDR_BYTES, CTX_PLAIN,
	  { 1, 0, 0, 0, 0, SYS_SEGMENT, 0, 0, 0, 0, 0, 0x1000ull } },
};

static void check_bad(void)
{
	unsigned int i;
	u64 out = 1;

	printf("\n== inputs that must be refused\n");
	for (i = 0; i < sizeof(g_bad) / sizeof(g_bad[0]); i++) {
		const struct bad_case *b = &g_bad[i];
		struct bc250_pte_context ctx = vm_ctx();
		DXGK_PTE pte;
		u64 got = 0xDEADBEEFDEADBEEFull;
		int rc;

		ctx.aperture = b->aperture;
		ctx.units = b->units;
		switch (b->defect) {
		case CTX_SAME_SEGMENTS:	ctx.vram_segment = ctx.system_segment; break;
		case CTX_NO_VRAM:	ctx.vram_size = 0; break;
		case CTX_SYSTEM_LIMIT:	ctx.system_limit = 0x0000000400000000ull; break;
		case CTX_ODD_VRAM_BASE:	ctx.vram_base = VRAM_BASE + 1; break;
		default:		break;
		}
		make_pte(&b->in, &pte);

		rc = bc250_pte_from_dxgk(&ctx, b->kind, (u64)pte.Flags, (u64)pte.PageAddress, &got);
		if (g_verbose)
			printf("  %-58s rc %d, out %016llX\n", b->name, rc,
			       (unsigned long long)got);
		if (rc != BC250_EINVAL) {
			printf("  %-58s ACCEPTED, entry %016llX\n", b->name,
			       (unsigned long long)got);
			g_failures++;
		} else if (got != 0) {
			printf("  %-58s refused but wrote %016llX\n", b->name,
			       (unsigned long long)got);
			g_failures++;
		}
	}
	printf("  %u inputs, all refused with nothing written\n",
	       (unsigned int)(sizeof(g_bad) / sizeof(g_bad[0])));

	/* Null arguments, which have no DXGK_PTE to build. */
	if (bc250_pte_from_dxgk(NULL, BC250_PTE_LEAF, 1, 0x1000, &out) != BC250_EINVAL || out != 1)
		fail("a null context was not refused, or it wrote through the output");
	{
		struct bc250_pte_context ctx = vm_ctx();

		if (bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, 1, 0x1000, NULL) != BC250_EINVAL)
			fail("a null output pointer was not refused");
	}
}

/* ---------------------------------------------------------------------------------------------
 * 5. The inverse
 * ------------------------------------------------------------------------------------------- */

static void check_decode(void)
{
	struct bc250_pte_fields f;
	char text[128];
	unsigned int want;
	unsigned int i;

	printf("\n== decode and describe\n");

	/* Every successful case must survive the round trip: what decode() reports has to rebuild
	 * the entry it was given. */
	for (i = 0; i < sizeof(g_good) / sizeof(g_good[0]); i++) {
		const struct good_case *c = &g_good[i];
		u64 rebuilt;

		if (c->expect == 0)
			continue;
		bc250_pte_decode(c->expect, c->kind, &f);
		rebuilt = f.address;
		if (f.valid)      rebuilt |= AMDGPU_PTE_VALID;
		if (f.system)     rebuilt |= AMDGPU_PTE_SYSTEM;
		if (f.snooped)    rebuilt |= AMDGPU_PTE_SNOOPED;
		if (f.tmz)        rebuilt |= AMDGPU_PTE_TMZ;
		if (f.executable) rebuilt |= AMDGPU_PTE_EXECUTABLE;
		if (f.readable)   rebuilt |= AMDGPU_PTE_READABLE;
		if (f.writeable)  rebuilt |= AMDGPU_PTE_WRITEABLE;
		if (f.prt)        rebuilt |= AMDGPU_PTE_PRT;
		if (f.pde_pte)    rebuilt |= AMDGPU_PDE_PTE;
		if (f.log)        rebuilt |= AMDGPU_PTE_LOG;
		if (f.tf)         rebuilt |= AMDGPU_PTE_TF;
		if (f.noalloc)    rebuilt |= AMDGPU_PTE_NOALLOC;
		rebuilt |= AMDGPU_PTE_FRAG(f.fragment);
		if (c->kind != BC250_PTE_DIRECTORY)
			rebuilt = AMDGPU_PTE_MTYPE_NV10(rebuilt, f.mtype);
		if (rebuilt != c->expect) {
			printf("  round trip %016llX -> %016llX\n",
			       (unsigned long long)c->expect, (unsigned long long)rebuilt);
			g_failures++;
		}
	}
	printf("  round trip through bc250_pte_decode(): every entry above rebuilt exactly\n");

	/* The GART entry's text, which is what a Stage B log line will look like. */
	want = bc250_pte_describe(GART_MEASURED | 0x1234567000ull, BC250_PTE_LEAF, text, sizeof(text));
	printf("  %s\n", text);
	if (strcmp(text, "0003001234567077 VALID SYSTEM SNOOPED EXE R W MTYPE=UC addr=001234567000") != 0 ||
	    want != strlen(text))
		fail("the GART entry does not describe itself as expected");

	want = bc250_pte_describe(0, BC250_PTE_LEAF, text, sizeof(text));
	printf("  %s\n", text);
	if (strcmp(text, "0000000000000000 INVALID") != 0 || want != strlen(text))
		fail("an empty entry does not describe itself as invalid");

	/* Truncation: terminate inside the buffer, report the full length, write nothing past it. */
	memset(text, '#', sizeof(text));
	want = bc250_pte_describe(GART_MEASURED, BC250_PTE_LEAF, text, 8);
	if (strlen(text) != 7 || text[7] != '\0' || text[8] != '#' || want < 8)
		fail("bc250_pte_describe() does not truncate inside the buffer");
	if (bc250_pte_describe(GART_MEASURED, BC250_PTE_LEAF, NULL, 0) != want)
		fail("bc250_pte_describe() does not measure with a null buffer");
	printf("  truncation: 8 bytes holds \"%s\", full length %u\n", text, want);
}

/* ---------------------------------------------------------------------------------------------
 * 6. Controls
 *
 * Each one is a wrong implementation that everything above would still pass.
 * ------------------------------------------------------------------------------------------- */

static void check_controls(void)
{
	struct bc250_pte_context ctx = vm_ctx();
	struct dxgk_in in = RWX_SYSTEM(0x0000001234567000ull);
	DXGK_PTE pte;
	/* `small` is not available as a name here: windows.h pulls in rpcndr.h, which defines it. */
	u64 as_bytes = 0, as_pages = 0, vm = 0, gart = 0, page4k = 0, page64k = 0;

	printf("\n== controls\n");

	/* (a) The units question is real and visible. A driver that quietly took the other reading
	 * would place the page 4096 times too far up, which is the kind of wrong that never faults
	 * and always corrupts. The two readings must produce different entries, and the difference
	 * must be exactly the shift. The address here is small enough that both readings land
	 * inside the 48-bit field, so neither is refused and the two entries can be compared. */
	in.address = 0x0000000001234000ull;
	make_pte(&in, &pte);
	ctx.units = BC250_PTE_ADDR_BYTES;
	(void)bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, (u64)pte.Flags, (u64)pte.PageAddress, &as_bytes);
	ctx.units = BC250_PTE_ADDR_PAGES;
	(void)bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, (u64)pte.Flags, (u64)pte.PageAddress, &as_pages);
	printf("  the same DXGK_PTE read as bytes %016llX, as a frame number %016llX\n",
	       (unsigned long long)as_bytes, (unsigned long long)as_pages);
	if ((as_bytes & AMDGPU_PTE_ADDR_MASK) != 0x1234000ull ||
	    (as_pages & AMDGPU_PTE_ADDR_MASK) != 0x1234000000ull)
		fail("the two readings of PageAddress are not distinguishable");

	/* (b) The memory type really does differ between the two apertures, and only there. A GART
	 * entry is MTYPE_UC because that is what was measured; a VM entry is MTYPE_NC because
	 * gmc_v10_0_get_vm_pte() says so. If someone made them share one constant, this catches it
	 * whichever way round the mistake went. */
	ctx = vm_ctx();
	ctx.aperture = BC250_PTE_VM;
	(void)bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, (u64)pte.Flags, (u64)pte.PageAddress, &vm);
	ctx.aperture = BC250_PTE_GART;
	(void)bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, (u64)pte.Flags, (u64)pte.PageAddress, &gart);
	printf("  VM %016llX, GART %016llX, difference %016llX\n", (unsigned long long)vm,
	       (unsigned long long)gart, (unsigned long long)(vm ^ gart));
	if ((vm ^ gart) != AMDGPU_PTE_MTYPE_NV10_SHIFT(MTYPE_UC) ||
	    (vm & AMDGPU_PTE_MTYPE_NV10_MASK) != AMDGPU_PTE_MTYPE_NV10_SHIFT(MTYPE_NC) ||
	    (gart & AMDGPU_PTE_MTYPE_NV10_MASK) != AMDGPU_PTE_MTYPE_NV10_SHIFT(MTYPE_UC))
		fail("the two apertures do not differ exactly in the memory type");

	/* (c) LargePage is not ignored. A translation that dropped it would produce a valid 4 KB
	 * entry, the GPU would fetch the first 4 KB of a 64 KB mapping correctly and then read
	 * whatever the next entry happened to say, which is the worst failure mode available. */
	ctx = vm_ctx();
	in.address = 0x0000001234560000ull;
	in.largepage = 0;
	make_pte(&in, &pte);
	(void)bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, (u64)pte.Flags, (u64)pte.PageAddress, &page4k);
	in.largepage = 1;
	make_pte(&in, &pte);
	(void)bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, (u64)pte.Flags, (u64)pte.PageAddress, &page64k);
	printf("  4 KB %016llX, 64 KB %016llX, difference %016llX\n", (unsigned long long)page4k,
	       (unsigned long long)page64k, (unsigned long long)(page4k ^ page64k));
	if ((page4k ^ page64k) != AMDGPU_PTE_FRAG(BC250_PTE_LARGE_PAGE_FRAG) ||
	    ((page64k >> 7) & 0x1full) != 4ull || ((page4k >> 7) & 0x1full) != 0ull)
		fail("LargePage does not become a fragment of 4, which is 1 << (12 + 4) = 64 KB");

	/* (d) A directory entry carries no permissions. amdgpu_gmc_get_pde_for_bo() builds one from
	 * the PDE flags, not the PTE flags, so read, write and execute have to disappear; a
	 * translation that reused the leaf builder would set them and the walker would apply them
	 * to everything below. */
	{
		u64 dir = 0;

		in.largepage = 0;
		in.address = 0x0000001234560000ull;
		make_pte(&in, &pte);
		(void)bc250_pte_from_dxgk(&ctx, BC250_PTE_DIRECTORY, (u64)pte.Flags,
					  (u64)pte.PageAddress, &dir);
		printf("  the same page as a directory entry %016llX\n", (unsigned long long)dir);
		if ((dir & (AMDGPU_PTE_READABLE | AMDGPU_PTE_WRITEABLE | AMDGPU_PTE_EXECUTABLE |
			    AMDGPU_PTE_MTYPE_NV10_MASK)) != 0)
			fail("a directory entry carries permissions or a memory type");
	}
}

int main(int argc, char **argv)
{
	g_verbose = argc > 1 && strcmp(argv[1], "-v") == 0;

	printf("DXGK_PTE -> AMD page table entry, host test\n");
	printf("segments: system %u, VRAM %u at 0x%llX for 0x%llX bytes (the test's own numbers)\n",
	       SYS_SEGMENT, VRAM_SEGMENT, (unsigned long long)VRAM_BASE,
	       (unsigned long long)VRAM_SIZE);

	check_layout();
	check_gart_flags();
	check_good();
	check_zero_prt();
	check_bad();
	check_decode();
	check_controls();

	printf("\n%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
