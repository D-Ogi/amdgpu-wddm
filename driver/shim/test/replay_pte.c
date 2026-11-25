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
		if (c->kind != BC250_PTE_DIRECTORY || f.pde_pte)
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

/* ---------------------------------------------------------------------------------------------
 * 7. Large pages: LargePage on a level 1 entry, through bc250_pte_from_dxgk_level()
 * ------------------------------------------------------------------------------------------- */

/* Unit A's VRAM: vram_base_offset 0x270000000, 8 GiB (fact M808 names the base). Only the M808
 * control below uses it; the other cases use the test's own segment. */
#define UNITA_VRAM_BASE	0x0000000270000000ull
#define UNITA_VRAM_SIZE	0x0000000200000000ull
#define M808_VRAM_2M	0x004000046fc004e1ull   /* fact M808: amdgpu's 2 MiB VRAM entry on unit A */

struct huge_case {
	const char *name;
	u32 level;
	u32 large_pages;
	u32 unita;              /* 1: unit A's 8 GiB segment at 0x270000000, 0: the test's own segment */
	struct dxgk_in in;
	u64 expect;             /* 0: must be refused, with nothing written */
};

/* A readable, writable, executable, coherent 2 MiB VRAM page; VRAM offsets are bytes here. */
#define HUGE_VRAM(off)   { 1, 0, 1, 0, 0, VRAM_SEGMENT, 1, 0, 0, 0, 0, (off) }

/*
 * Expected entries written out in full: address | VALID 0x01 | SNOOPED 0x04 | EXECUTABLE 0x10 |
 * READABLE 0x20 | WRITEABLE 0x40 | FRAG 9 (0x480) | PDE_PTE (bit 54, 0x0040000000000000), MTYPE_NC.
 */
static const struct huge_case g_huge[] = {
	{ "level 1, VRAM, rwx, coherent", 1, 1, 0, HUGE_VRAM(0x200000ull), 0x00400004002004F5ull },
	{ "level 1, VRAM, read only, no execute, not coherent", 1, 1, 0,
	  { 1, 0, 0, 1, 1, VRAM_SEGMENT, 1, 0, 0, 0, 0, 0x200000ull }, 0x00400004002004A1ull },
	{ "level 1, VRAM, the last 2 MiB of the segment", 1, 1, 0,
	  HUGE_VRAM(VRAM_SIZE - 0x200000ull), 0x004000040FE004F5ull },
	{ "level 1 without LargePage: still a directory entry", 1, 1, 0,
	  RWX_VRAM(0x3000ull), 0x0000000400003005ull },
	{ "refused: large pages not reported", 1, 0, 0, HUGE_VRAM(0x200000ull), 0 },
	{ "refused: level 2 in a segment smaller than 1 GiB", 2, 1, 0, HUGE_VRAM(0), 0 },
	{ "refused: LargePage at level 3", 3, 1, 0, HUGE_VRAM(0x200000ull), 0 },
	{ "level 0 LargePage: the 64 KB leaf fragment, as before", 0, 1, 0, HUGE_VRAM(0x210000ull),
	  0x0000000400210275ull },
	{ "refused: an address not 2 MiB aligned", 1, 1, 0, HUGE_VRAM(0x201000ull), 0 },
	{ "refused: an address 64 KB but not 2 MiB aligned", 1, 1, 0, HUGE_VRAM(0x210000ull), 0 },
	{ "refused: host memory", 1, 1, 0,
	  { 1, 0, 1, 0, 0, SYS_SEGMENT, 1, 0, 0, 0, 0, 0x0000001234600000ull }, 0 },
	{ "refused: past the end of the segment", 1, 1, 0, HUGE_VRAM(VRAM_SIZE), 0 },
	/* Unit A's segment: 0x270000000 is 2 MiB aligned and not 1 GiB aligned. FRAG 18 is 0x900. */
	{ "unit A, level 1, segment offset 0", 1, 1, 1, HUGE_VRAM(0), 0x00400002700004F5ull },
	{ "unit A, level 2, rwx, coherent: a 1 GiB page", 2, 1, 1, HUGE_VRAM(0x10000000ull),
	  0x0040000280000975ull },
	{ "unit A, level 2, read only, no execute, not coherent", 2, 1, 1,
	  { 1, 0, 0, 1, 1, VRAM_SEGMENT, 1, 0, 0, 0, 0, 0x10000000ull }, 0x0040000280000921ull },
	{ "unit A, level 2, the last whole 1 GiB of the segment", 2, 1, 1, HUGE_VRAM(0x190000000ull),
	  0x0040000400000975ull },
	{ "refused: unit A, level 2, offset 0 is not a 1 GiB address", 2, 1, 1, HUGE_VRAM(0), 0 },
	{ "refused: unit A, level 2, 2 MiB but not 1 GiB aligned", 2, 1, 1, HUGE_VRAM(0x10200000ull), 0 },
	{ "refused: unit A, level 2, past the end of the segment", 2, 1, 1, HUGE_VRAM(0x1D0000000ull), 0 },
	{ "refused: unit A, level 2, large pages not reported", 2, 0, 1, HUGE_VRAM(0x10000000ull), 0 },
};

static void check_huge(void)
{
	struct bc250_pte_fields f;
	char text[128];
	unsigned int i;

	printf("\n== large pages (LargePage on a level 1 or level 2 entry)\n");
	for (i = 0; i < sizeof(g_huge) / sizeof(g_huge[0]); i++) {
		const struct huge_case *c = &g_huge[i];
		struct bc250_pte_context ctx = vm_ctx();
		DXGK_PTE pte;
		u64 got = 0xDEADBEEFDEADBEEFull;
		int rc;

		ctx.large_pages = c->large_pages;
		if (c->unita) {
			ctx.vram_base = UNITA_VRAM_BASE;
			ctx.vram_size = UNITA_VRAM_SIZE;
		}
		make_pte(&c->in, &pte);
		rc = bc250_pte_from_dxgk_level(&ctx, c->level, (u64)pte.Flags, (u64)pte.PageAddress, &got);
		bc250_pte_describe(got, c->level ? BC250_PTE_DIRECTORY : BC250_PTE_LEAF, text, sizeof(text));
		printf("  %-58s rc %2d %016llX  %s\n", c->name, rc, (unsigned long long)got, text);
		/* The level 0 row is a control of the old leaf path: LargePage on a leaf is the 64 KB
		 * fragment, which no VidMm sends to this driver, and the level entry point leaves it as it was. */
		if (c->expect == 0) {
			if (rc != BC250_EINVAL || got != 0)
				fail("a large page that must be refused was accepted, or wrote an entry");
		} else if (rc != 0 || got != c->expect) {
			printf("        expected %016llX\n", (unsigned long long)c->expect);
			g_failures++;
		}
	}

	/* The positive control: the same mapping as fact M808's (VRAM, read and write, no execute, not
	 * coherent, 2 MiB at 0x46FC00000 on unit A's base) must give amdgpu's entry bit for bit. */
	{
		struct bc250_pte_context ctx = vm_ctx();
		struct dxgk_in in = { 1, 0, 0, 0, 1, VRAM_SEGMENT, 1, 0, 0, 0, 0, 0 };
		DXGK_PTE pte;
		u64 got = 0;

		ctx.vram_base = UNITA_VRAM_BASE;
		ctx.vram_size = UNITA_VRAM_SIZE;
		ctx.large_pages = 1;
		in.address = 0x000000046fc00000ull - UNITA_VRAM_BASE;
		make_pte(&in, &pte);
		if (bc250_pte_from_dxgk_level(&ctx, 1, (u64)pte.Flags, (u64)pte.PageAddress, &got) != 0 ||
		    got != M808_VRAM_2M) {
			printf("  M808 control %016llX, amdgpu wrote %016llX\n", (unsigned long long)got,
			       (unsigned long long)M808_VRAM_2M);
			fail("the 2 MiB entry is not the one amdgpu wrote for the same mapping");
		}
		/* The same context, read as page frame numbers, which is what VidMm sends (fact M72). */
		ctx.units = BC250_PTE_ADDR_PAGES;
		in.address = (0x000000046fc00000ull - UNITA_VRAM_BASE) >> 12;
		make_pte(&in, &pte);
		got = 0;
		if (bc250_pte_from_dxgk_level(&ctx, 1, (u64)pte.Flags, (u64)pte.PageAddress, &got) != 0 ||
		    got != M808_VRAM_2M)
			fail("the 2 MiB entry differs when the address is a page frame number");
		bc250_pte_describe(got, BC250_PTE_DIRECTORY, text, sizeof(text));
		printf("  M808 control: %s\n", text);
		if (strcmp(text, "004000046FC004E1 VALID R W PDE_PTE FRAG=09 MTYPE=NC addr=00046FC00000") != 0)
			fail("the 2 MiB entry does not describe itself as a page");

		/* Decode reads a PDE_PTE directory entry as a page: its address, flags and fragment. */
		bc250_pte_decode(M808_VRAM_2M, BC250_PTE_DIRECTORY, &f);
		if (f.address != 0x000000046fc00000ull || !f.valid || !f.readable || !f.writeable ||
		    f.executable || f.system || !f.pde_pte || f.fragment != BC250_PTE_HUGE_PAGE_FRAG ||
		    f.mtype != MTYPE_NC)
			fail("decode does not read a 2 MiB entry as a page");
		bc250_pte_decode(0x0040000280000975ull, BC250_PTE_DIRECTORY, &f);
		if (f.address != 0x0000000280000000ull || !f.valid || !f.readable || !f.writeable ||
		    !f.executable || !f.snooped || f.system || !f.pde_pte ||
		    f.fragment != BC250_PTE_GIANT_PAGE_FRAG || f.mtype != MTYPE_NC)
			fail("decode does not read a 1 GiB entry as a page");
	}

	/* The entry point without a level keeps refusing LargePage on a directory, whatever the
	 * context says: it cannot know that the entry is level 1. */
	{
		struct bc250_pte_context ctx = vm_ctx();
		struct dxgk_in in = HUGE_VRAM(0x200000ull);
		DXGK_PTE pte;
		u64 got = 1;

		ctx.large_pages = 1;
		make_pte(&in, &pte);
		if (bc250_pte_from_dxgk(&ctx, BC250_PTE_DIRECTORY, (u64)pte.Flags, (u64)pte.PageAddress,
					&got) != BC250_EINVAL || got != 0)
			fail("bc250_pte_from_dxgk accepted LargePage on a directory entry");
	}

	/* A table segment page is not a large page, even level 1 and aligned. */
	{
		struct bc250_pte_context ctx = vm_ctx();
		u64 got = 1;

		ctx.large_pages = 1;
		ctx.table_segment = 3;
		ctx.table_base = VRAM_BASE + VRAM_SIZE;
		ctx.table_size = 4ull * 1024 * 1024;
		if (bc250_pte_from_dxgk_level(&ctx, 1, BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_LARGEPAGE |
					      (3ull << BC250_DXGK_PTE_SEGMENT_SHIFT), 0, &got) != BC250_EINVAL ||
		    got != 0)
			fail("a large page in the page table segment was accepted");
	}

	/* Zero on a level 1 entry keeps the PRT terminal, with or without LargePage. */
	{
		struct bc250_pte_context ctx = vm_ctx();
		u64 got = 0;

		ctx.large_pages = 1;
		if (bc250_pte_from_dxgk_level(&ctx, 1, BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_ZERO |
					      BC250_DXGK_PTE_LARGEPAGE, 0, &got) != 0 ||
		    got != 0x00C8000000000006ull)
			fail("Zero with LargePage at level 1 is not the PRT terminal");
	}

	/* The observer: bc250_pte_large_page_bytes accepts exactly the entries the encoder writes. The
	 * entries the encoder wrote above are read back as pages of their size; an entry that the encoder
	 * refuses (a page in the table segment, a range that runs past the segment while its first 4 KB
	 * page is inside it, the fragment of another level, a host page, the cap off, level 0 or 3, an
	 * unaligned address, a directory without PDE_PTE) is 0. */
	{
		struct bc250_pte_context ctx = vm_ctx();
		static const struct { const char *name; u32 level; u32 cap; u64 entry; u64 expect; } cases[] = {
			{ "2 MiB page of fact M808",                          1, 1, M808_VRAM_2M,           BC250_PTE_HUGE_PAGE_BYTES },
			{ "1 GiB page at 0x280000000",                        2, 1, 0x0040000280000975ull,  BC250_PTE_GIANT_PAGE_BYTES },
			{ "1 GiB page, last one of the segment",              2, 1, 0x0040000400000975ull,  BC250_PTE_GIANT_PAGE_BYTES },
			{ "refused: large pages not reported",                1, 0, M808_VRAM_2M,           0 },
			{ "refused: level 0",                                 0, 1, M808_VRAM_2M,           0 },
			{ "refused: level 3",                                 3, 1, 0x0040000280000975ull,  0 },
			{ "refused: level 2 entry with the level 1 fragment", 2, 1, 0x00400002800004E1ull,  0 },
			{ "refused: level 1 entry with the level 2 fragment", 1, 1, 0x004000046FC00975ull,  0 },
			{ "refused: 1 GiB range past the segment end",        2, 1, 0x0040000440000975ull,  0 },
			{ "refused: 1 GiB range, 2 MiB aligned only",         2, 1, 0x0040000280200975ull,  0 },
			{ "refused: 2 MiB page in the table segment",         1, 1, 0x00400004700004E1ull,  0 },
			{ "refused: 2 MiB page below the segment",            1, 1, 0x004000026FE004E1ull,  0 },
			{ "refused: host page with PDE_PTE",                  1, 1, M808_VRAM_2M | AMDGPU_PTE_SYSTEM, 0 },
			{ "refused: directory entry without PDE_PTE",         1, 1, 0x000000046FC00001ull,  0 },
			{ "refused: not valid",                               1, 1, M808_VRAM_2M & ~1ull,   0 },
		};

		ctx.large_pages = 1;
		ctx.vram_base = UNITA_VRAM_BASE;
		ctx.vram_size = UNITA_VRAM_SIZE;
		ctx.table_segment = 3;
		ctx.table_base = UNITA_VRAM_BASE + UNITA_VRAM_SIZE;
		ctx.table_size = 64ull * 1024 * 1024;
		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
			u64 bytes;

			ctx.large_pages = cases[i].cap;
			bc250_pte_decode(cases[i].entry, cases[i].level ? BC250_PTE_DIRECTORY : BC250_PTE_LEAF, &f);
			bytes = bc250_pte_large_page_bytes(&ctx, cases[i].level, &f);
			printf("  observer %-52s %016llX -> %llu\n", cases[i].name,
			       (unsigned long long)cases[i].entry, (unsigned long long)bytes);
			if (bytes != cases[i].expect) {
				printf("        expected %llu\n", (unsigned long long)cases[i].expect);
				g_failures++;
			}
		}
		if (bc250_pte_large_page_bytes(NULL, 1, &f) != 0 || bc250_pte_large_page_bytes(&ctx, 1, NULL) != 0)
			fail("the observer accepted a null context or null fields");
	}
	printf("  %u cases, the M808 control, and the old entry point, table segment, Zero and observer controls\n",
	       (unsigned int)(sizeof(g_huge) / sizeof(g_huge[0])));
}

/* ---------------------------------------------------------------------------------------------
 * 8. Large pages on a segment base that is 2 MiB aligned only (huge_pages_only)
 * ------------------------------------------------------------------------------------------- */

/* The unit with the 512 MiB carve-out at 0x450000000 (its tester.38 log: "vram: 0x450000000 +
 * 0x20000000", "vidmm: VRAM segment 1 at physical 0x453050000 + 0x1A240000", "vidmm: table segment
 * 3 at physical 0x46D290000 + 0xD70000"). The 2 MiB layout moves the segment base up to the next
 * 2 MiB address and keeps the table segment; driver/kmd/test/vram_geometry_test.c computes these
 * numbers from the production layout code. */
#define SMALL_PLAIN_BASE	0x0000000453050000ull   /* tester.38: not a 2 MiB address */
#define SMALL_VRAM_BASE		0x0000000453200000ull
#define SMALL_VRAM_SIZE		0x000000001A090000ull   /* 208 pages of 2 MiB and 576 KiB */
#define SMALL_TABLE_BASE	0x000000046D290000ull
#define SMALL_TABLE_SIZE	0x0000000000D70000ull
#define SMALL_HUGE_PAGES	208u

/* The context vidmm.c builds for that start: page frame numbers, host memory in segment 0. */
static struct bc250_pte_context small_ctx(void)
{
	struct bc250_pte_context ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.units = BC250_PTE_ADDR_PAGES;
	ctx.aperture = BC250_PTE_VM;
	ctx.system_segment = 0;
	ctx.vram_segment = VRAM_SEGMENT;
	ctx.vram_base = SMALL_VRAM_BASE;
	ctx.vram_size = SMALL_VRAM_SIZE;
	ctx.table_segment = 3;
	ctx.table_base = SMALL_TABLE_BASE;
	ctx.table_size = SMALL_TABLE_SIZE;
	ctx.large_pages = 1;
	ctx.huge_pages_only = 1;
	return ctx;
}

/* DXGK_PTE flags of a valid large page in `segment`: writable, executable, not coherent. */
static u64 small_flags(u32 segment)
{
	return BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_LARGEPAGE | ((u64)segment << BC250_DXGK_PTE_SEGMENT_SHIFT);
}

static void check_huge_only(void)
{
	struct bc250_pte_context ctx = small_ctx();
	struct bc250_pte_fields f;
	u64 got = 0, offset;
	unsigned int pages = 0, i;

	printf("\n== large pages on a 2 MiB aligned base (level 1 only)\n");

	/* Every 2 MiB page of the segment: accepted at level 1, the address is base plus offset, the
	 * entry is amdgpu's huge page (PDE_PTE, fragment 9, not SYSTEM), and the observer reads it
	 * back as 2 MiB. Expected value written out: address | VALID 0x01 | EXECUTABLE 0x10 |
	 * READABLE 0x20 | WRITEABLE 0x40 | FRAG 9 (0x480) | PDE_PTE (bit 54), MTYPE_NC. */
	for (offset = 0; offset + BC250_PTE_HUGE_PAGE_BYTES <= SMALL_VRAM_SIZE; offset += BC250_PTE_HUGE_PAGE_BYTES) {
		got = 0;
		if (bc250_pte_from_dxgk_level(&ctx, 1, small_flags(VRAM_SEGMENT), offset >> 12, &got) != 0 ||
		    got != ((SMALL_VRAM_BASE + offset) | 0x00400000000004F1ull)) {
			printf("  offset %llX entry %016llX\n", (unsigned long long)offset, (unsigned long long)got);
			fail("a 2 MiB page of the small segment is not encoded as base plus offset");
			break;
		}
		bc250_pte_decode(got, BC250_PTE_DIRECTORY, &f);
		if (bc250_pte_large_page_bytes(&ctx, 1, &f) != BC250_PTE_HUGE_PAGE_BYTES ||
		    f.address != SMALL_VRAM_BASE + offset || (f.address & (BC250_PTE_HUGE_PAGE_BYTES - 1)) != 0)
			fail("the observer does not read back a 2 MiB page of the small segment");
		pages++;
	}
	printf("  %u pages of 2 MiB accepted, first %016llX, last %016llX\n", pages,
	       (unsigned long long)(SMALL_VRAM_BASE | 0x00400000000004F1ull), (unsigned long long)got);
	if (pages != SMALL_HUGE_PAGES || got != 0x004000046D0004F1ull)
		fail("the small segment does not hold exactly 208 pages of 2 MiB, the last at 0x46D000000");

	/* The same context without LargePage still names a table of the table segment: the level 1
	 * path of 4 KB mappings is as it was. */
	got = 0;
	if (bc250_pte_from_dxgk_level(&ctx, 1, BC250_DXGK_PTE_VALID | (3ull << BC250_DXGK_PTE_SEGMENT_SHIFT),
				      0x10, &got) != 0 || got != (SMALL_TABLE_BASE + 0x10000ull + 1ull))
		fail("a level 1 pointer to a table is not what it was");

	/* Refusals at level 1, each with nothing written. */
	{
		static const struct { const char *name; u32 level; u32 segment; u64 offset; } bad[] = {
			{ "the 576 KiB tail: the range runs past the segment end", 1, VRAM_SEGMENT, 0x1A000000ull },
			{ "past the end of the segment",                           1, VRAM_SEGMENT, 0x1A200000ull },
			{ "an offset 4 KB above a 2 MiB offset",                   1, VRAM_SEGMENT, 0x00201000ull },
			{ "an offset 64 KB above a 2 MiB offset",                  1, VRAM_SEGMENT, 0x00210000ull },
			{ "an offset 1 MiB above a 2 MiB offset",                  1, VRAM_SEGMENT, 0x00300000ull },
			{ "the table segment",                                     1, 3,            0x00000000ull },
			{ "the aperture segment",                                  1, 2,            0x00200000ull },
			{ "host memory, a 2 MiB aligned address",                  1, 0,            0x109200000ull },
			{ "level 2, offset 0",                                     2, VRAM_SEGMENT, 0x00000000ull },
			{ "level 2, the offset of the first 1 GiB boundary above", 2, VRAM_SEGMENT, 0x2CE00000ull },
			{ "level 3",                                               3, VRAM_SEGMENT, 0x00000000ull },
		};

		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			got = 0xDEADBEEFDEADBEEFull;
			if (bc250_pte_from_dxgk_level(&ctx, bad[i].level, small_flags(bad[i].segment),
						      bad[i].offset >> 12, &got) != BC250_EINVAL || got != 0) {
				printf("  %s: entry %016llX\n", bad[i].name, (unsigned long long)got);
				fail("a large page that must be refused on the 2 MiB base was accepted");
			} else {
				printf("  refused: %s\n", bad[i].name);
			}
		}
	}

	/* Every 4 KB offset of the first 4 MiB that is not a 2 MiB offset is refused: an unaligned
	 * large page would map another range than VidMm meant. */
	for (offset = 0x1000; offset < 0x400000ull; offset += 0x1000) {
		if ((offset & (BC250_PTE_HUGE_PAGE_BYTES - 1)) == 0)
			continue;
		got = 1;
		if (bc250_pte_from_dxgk_level(&ctx, 1, small_flags(VRAM_SEGMENT), offset >> 12, &got) != BC250_EINVAL ||
		    got != 0) {
			fail("an unaligned large page offset was accepted on the 2 MiB base");
			break;
		}
	}

	/* The control of the layout: on the base tester.38 had (0x453050000) every 2 MiB offset is an
	 * unaligned address, so every large page is refused. The base alignment is what the pages
	 * need, and the encoder fails closed if the layout ever does not give it. */
	{
		struct bc250_pte_context plain = small_ctx();

		plain.vram_base = SMALL_PLAIN_BASE;
		plain.vram_size = 0x1A240000ull;
		for (offset = 0; offset + BC250_PTE_HUGE_PAGE_BYTES <= plain.vram_size; offset += BC250_PTE_HUGE_PAGE_BYTES) {
			got = 1;
			if (bc250_pte_from_dxgk_level(&plain, 1, small_flags(VRAM_SEGMENT), offset >> 12, &got) !=
			    BC250_EINVAL || got != 0) {
				fail("a large page was accepted on a base that is not a 2 MiB address");
				break;
			}
		}
	}

	/* The control of huge_pages_only itself: unit A's segment holds 1 GiB pages, and the flag is
	 * what refuses them, not the size of the segment. Level 1 stays. */
	{
		struct bc250_pte_context big = small_ctx();

		big.vram_base = UNITA_VRAM_BASE;
		big.vram_size = UNITA_VRAM_SIZE;
		big.table_size = 0;
		big.huge_pages_only = 0;
		got = 0;
		if (bc250_pte_from_dxgk_level(&big, 2, small_flags(VRAM_SEGMENT), 0x10000000ull >> 12, &got) != 0 ||
		    got != 0x0040000280000971ull)       /* FRAG 18 is 0x900 */
			fail("the 1 GiB control page is not accepted without huge_pages_only");
		bc250_pte_decode(got, BC250_PTE_DIRECTORY, &f);
		if (bc250_pte_large_page_bytes(&big, 2, &f) != BC250_PTE_GIANT_PAGE_BYTES)
			fail("the observer does not read the 1 GiB control page without huge_pages_only");
		big.huge_pages_only = 1;
		if (bc250_pte_large_page_bytes(&big, 2, &f) != 0)
			fail("the observer reads a 1 GiB page with huge_pages_only");
		got = 1;
		if (bc250_pte_from_dxgk_level(&big, 2, small_flags(VRAM_SEGMENT), 0x10000000ull >> 12, &got) !=
		    BC250_EINVAL || got != 0)
			fail("huge_pages_only does not refuse a 1 GiB page that the segment could hold");
		got = 0;
		if (bc250_pte_from_dxgk_level(&big, 1, small_flags(VRAM_SEGMENT), 0, &got) != 0 ||
		    got != 0x00400002700004F1ull)
			fail("huge_pages_only refuses a 2 MiB page");
		bc250_pte_decode(got, BC250_PTE_DIRECTORY, &f);
		if (bc250_pte_large_page_bytes(&big, 1, &f) != BC250_PTE_HUGE_PAGE_BYTES)
			fail("the observer does not read a 2 MiB page with huge_pages_only");
		/* The flag alone reports nothing: without large_pages every level is refused. */
		big.large_pages = 0;
		got = 1;
		if (bc250_pte_from_dxgk_level(&big, 1, small_flags(VRAM_SEGMENT), 0, &got) != BC250_EINVAL || got != 0 ||
		    bc250_pte_large_page_bytes(&big, 1, &f) != 0)
			fail("huge_pages_only without large_pages accepts a large page");
	}
	printf("  208 pages, 11 refusals, the unaligned sweep, and the plain-base and 1 GiB controls\n");
}

static void check_table_segment(void)
{
	struct bc250_pte_context ctx=vm_ctx();
	struct bc250_pte_fields decoded;
	u64 entry,flags=BC250_DXGK_PTE_VALID | (3ull<<BC250_DXGK_PTE_SEGMENT_SHIFT);
	unsigned units,kind,page;
	if (bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)
		fail("disabled table segment must refuse its ID");
	ctx.table_segment=3;ctx.table_base=VRAM_BASE+VRAM_SIZE;ctx.table_size=512ull*4096;
	for(units=0;units<2;units++) for(kind=0;kind<2;kind++) for(page=0;page<512;page++) {
		u64 address=units?page:(u64)page*4096;
		ctx.units=(enum bc250_pte_addr_units)units;
		if (bc250_pte_from_dxgk(&ctx,(enum bc250_pte_kind)kind,flags,address,&entry)!=0) {
			fail("dedicated table segment accepts bounded page");continue;
		}
		bc250_pte_decode(entry,(enum bc250_pte_kind)kind,&decoded);
		if (!decoded.valid || decoded.system || decoded.address!=ctx.table_base+(u64)page*4096)
			fail("dedicated table offset resolves independent physical origin");
	}
	ctx.units=BC250_PTE_ADDR_BYTES;
	if (bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,ctx.table_size,&entry)==0 || entry)
		fail("dedicated table segment end refuses with zero output");
	if (bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,BC250_DXGK_PTE_VALID | ((u64)VRAM_SEGMENT<<BC250_DXGK_PTE_SEGMENT_SHIFT),4096,&entry)!=0)
		fail("application local segment remains usable");
	bc250_pte_decode(entry,BC250_PTE_LEAF,&decoded);
	if(decoded.address!=VRAM_BASE+4096)fail("application segment retains its origin");
	ctx.table_segment=SYS_SEGMENT;
	if(bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)fail("table/system ID collision refused");
	ctx.table_segment=VRAM_SEGMENT;
	if(bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)fail("table/application ID collision refused");
	ctx.table_segment=32;
	if(bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)fail("unrepresentable table ID refused");
	ctx.table_segment=3;ctx.table_base=VRAM_BASE;
	if(bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)fail("overlapping local segments refused");
	ctx.table_base=VRAM_BASE+VRAM_SIZE;ctx.table_base++;
	if(bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)fail("unaligned table base refused");
	ctx.table_base--;ctx.table_size--;
	if(bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)fail("unaligned table extent refused");
	ctx.table_size=8192;ctx.table_base=~0ull-4095;
	if(bc250_pte_from_dxgk(&ctx,BC250_PTE_LEAF,flags,0,&entry)==0)fail("table extent wrap refused");
	printf("  dedicated table segment:2048 address/unit/kind combinations plus boundary controls\n");
}

/* ---------------------------------------------------------------------------------------------
 * 9. 64 KB page tables: one entry of an update with Use64KBPages, through bc250_pte_from_dxgk_64k()
 * ------------------------------------------------------------------------------------------- */

/* Evidence E17 (evidence/linux/2026-10-06-E17-pte-l25-l38), buffer k64 on unit A under amdgpu: a
 * 64 KB buffer in host memory. The trace has amdgpu_vm_set_ptes addr=010f1e0000 incr=4096 flags=267
 * count=16, and these are the 16 entries read back from the table (out/e17/hold/pt/
 * vram-00046ffdb000.hex, offsets 0x80 to 0xF8), copied here and not computed. */
static const u64 g_k64[BC250_PTE_64K_SLOTS] = {
	0x000000010f1e0267ull, 0x000000010f1e1267ull, 0x000000010f1e2267ull, 0x000000010f1e3267ull,
	0x000000010f1e4267ull, 0x000000010f1e5267ull, 0x000000010f1e6267ull, 0x000000010f1e7267ull,
	0x000000010f1e8267ull, 0x000000010f1e9267ull, 0x000000010f1ea267ull, 0x000000010f1eb267ull,
	0x000000010f1ec267ull, 0x000000010f1ed267ull, 0x000000010f1ee267ull, 0x000000010f1ef267ull,
};
#define K64_ADDRESS	0x000000010f1e0000ull

/* The context of the miniport for these cases: page frame numbers, host memory as segment 0 (E18
 * run 001), the test's VRAM segment, 64 KB tables reported. */
static struct bc250_pte_context ctx_64k(void)
{
	struct bc250_pte_context ctx = vm_ctx();

	ctx.units = BC250_PTE_ADDR_PAGES;
	ctx.system_segment = 0;
	ctx.system_limit = 0x0000000480000000ull;
	ctx.system_64k = 1;
	return ctx;
}

struct page64_case {
	const char *name;
	u64 flags;              /* DXGK_PTE.Flags */
	u64 pfn;                /* DXGK_PTE.PageAddress, a page frame number */
	int refused;            /* 1: BC250_EINVAL and 16 zero slots */
	u32 fragment;           /* expected *fragment */
	u64 first;              /* expected slot 0 */
	u64 step;               /* expected slot n - slot n-1 */
};

#define P64_VALID	BC250_DXGK_PTE_VALID
#define P64_COH		BC250_DXGK_PTE_CACHECOHERENT
#define P64_RO		BC250_DXGK_PTE_READONLY
#define P64_NX		BC250_DXGK_PTE_NOEXECUTE
#define P64_SEG(n)	((u64)(n) << BC250_DXGK_PTE_SEGMENT_SHIFT)

/* Expected slots written out in full: VALID 0x01, SYSTEM 0x02, SNOOPED 0x04, EXECUTABLE 0x10,
 * READABLE 0x20, WRITEABLE 0x40, FRAG 4 = 0x200. */
static const struct page64_case g_page64[] = {
	{ "host, rw, no execute, coherent: evidence E17 k64", P64_VALID | P64_COH | P64_NX,
	  K64_ADDRESS >> 12, 0, 4, 0x000000010f1e0267ull, 0x1000 },
	{ "host, rwx, coherent", P64_VALID | P64_COH, K64_ADDRESS >> 12, 0, 4,
	  0x000000010f1e0277ull, 0x1000 },
	{ "host, read only, no execute, not coherent", P64_VALID | P64_RO | P64_NX, 0x200000ull >> 12,
	  0, 4, 0x0000000000200223ull, 0x1000 },
	{ "host, the last 64 KB below the end of host memory", P64_VALID | P64_COH | P64_NX,
	  (0x0000000480000000ull - 0x10000ull) >> 12, 0, 4, 0x000000047fff0267ull, 0x1000 },
	{ "not valid: 16 empty slots", P64_COH | P64_NX, K64_ADDRESS >> 12, 0, 0, 0, 0 },
	{ "Zero: 16 PRT leaves, no fragment", P64_VALID | BC250_DXGK_PTE_ZERO, 0, 0, 0,
	  0x0088000000000006ull, 0 },
	{ "host page not 64 KB aligned: mapped, no fragment", P64_VALID | P64_COH | P64_NX,
	  (K64_ADDRESS >> 12) + 1, 0, 0, 0x000000010f1e1067ull, 0x1000 },
	{ "VRAM segment: mapped, no fragment", P64_VALID | P64_NX | P64_SEG(VRAM_SEGMENT),
	  0x10000ull >> 12, 0, 0, 0x0000000400010061ull, 0x1000 },
	{ "refused: LargePage on a 64 KB leaf entry", P64_VALID | P64_COH | BC250_DXGK_PTE_LARGEPAGE,
	  K64_ADDRESS >> 12, 1, 0, 0, 0 },
	{ "refused: PageTablePageSize on a leaf entry", P64_VALID | P64_COH |
	  ((u64)BC250_DXGK_PAGE_TABLE_PAGE_64KB << BC250_DXGK_PTE_PAGESIZE_SHIFT),
	  K64_ADDRESS >> 12, 1, 0, 0, 0 },
	{ "refused: a reserved bit", P64_VALID | P64_COH | (1ull << 20), K64_ADDRESS >> 12, 1, 0, 0, 0 },
	{ "refused: another physical adapter", P64_VALID | P64_COH |
	  (1ull << BC250_DXGK_PTE_ADAPTER_SHIFT), K64_ADDRESS >> 12, 1, 0, 0, 0 },
	{ "refused: a segment this driver does not report", P64_VALID | P64_COH | P64_SEG(5),
	  K64_ADDRESS >> 12, 1, 0, 0, 0 },
	{ "refused: page 15 is past the end of host memory", P64_VALID | P64_COH | P64_NX,
	  (0x0000000480000000ull - 0xF000ull) >> 12, 1, 0, 0, 0 },
	{ "refused: the first page is past the end of host memory", P64_VALID | P64_COH | P64_NX,
	  0x0000000480000000ull >> 12, 1, 0, 0, 0 },
	{ "refused: a VRAM page whose 64 KB runs past the segment", P64_VALID | P64_SEG(VRAM_SEGMENT),
	  (VRAM_SIZE - 0x8000ull) >> 12, 1, 0, 0, 0 },
	{ "refused: a page frame number that does not fit an address", P64_VALID | P64_COH,
	  0xFFFFFFFFFFFFFFF8ull, 1, 0, 0, 0 },
};

static void check_page64(void)
{
	u64 out[BC250_PTE_64K_SLOTS], leaf;
	struct bc250_pte_fields f;
	u32 fragment, n;
	unsigned int i;

	printf("\n== 64 KB page tables (one entry of an update with Use64KBPages)\n");
	for (i = 0; i < sizeof(g_page64) / sizeof(g_page64[0]); i++) {
		const struct page64_case *c = &g_page64[i];
		struct bc250_pte_context ctx = ctx_64k();
		int rc, bad = 0;

		memset(out, 0xA5, sizeof(out));
		fragment = 99;
		rc = bc250_pte_from_dxgk_64k(&ctx, c->flags, c->pfn, out, &fragment);
		printf("  %-58s rc %d frag %u slot0 %016llX slot15 %016llX\n", c->name, rc, fragment,
		       (unsigned long long)out[0], (unsigned long long)out[15]);
		if ((rc != 0) != (c->refused != 0) || fragment != c->fragment)
			bad = 1;
		for (n = 0; n < BC250_PTE_64K_SLOTS; n++) {
			if (out[n] != (c->refused ? 0 : c->first + n * c->step))
				bad = 1;
			/* The mapping does not depend on the fragment: without the fragment field, slot n is
			 * the 4 KB leaf of page n from the entry point every other leaf uses. */
			if (!c->refused && (c->flags & BC250_DXGK_PTE_VALID) != 0 &&
			    (c->flags & BC250_DXGK_PTE_ZERO) == 0 &&
			    (bc250_pte_from_dxgk(&ctx, BC250_PTE_LEAF, c->flags, c->pfn + n, &leaf) != 0 ||
			     leaf != (out[n] & ~(u64)AMDGPU_PTE_FRAG(0x1f))))
				bad = 1;
		}
		if (bad) {
			printf("        expected rc %s frag %u slot0 %016llX step 0x%llX\n",
			       c->refused ? "EINVAL" : "0", c->fragment, (unsigned long long)c->first,
			       (unsigned long long)c->step);
			g_failures++;
		}
	}

	/* The positive control, slot by slot against the table amdgpu wrote on unit A. */
	{
		struct bc250_pte_context ctx = ctx_64k();

		if (bc250_pte_from_dxgk_64k(&ctx, P64_VALID | P64_COH | P64_NX, K64_ADDRESS >> 12, out,
					    &fragment) != 0 || fragment != BC250_PTE_LARGE_PAGE_FRAG ||
		    memcmp(out, g_k64, sizeof(g_k64)) != 0)
			fail("the 16 entries are not the ones amdgpu wrote for a 64 KB host buffer (E17 k64)");
		/* The same page as a byte address gives the same entries. */
		ctx.units = BC250_PTE_ADDR_BYTES;
		if (bc250_pte_from_dxgk_64k(&ctx, P64_VALID | P64_COH | P64_NX, K64_ADDRESS, out,
					    &fragment) != 0 || memcmp(out, g_k64, sizeof(g_k64)) != 0)
			fail("the 64 KB entries differ when the address is a byte address");
		bc250_pte_decode(out[0], BC250_PTE_LEAF, &f);
		if (!f.valid || !f.system || !f.snooped || !f.readable || !f.writeable || f.executable ||
		    f.pde_pte || f.fragment != BC250_PTE_LARGE_PAGE_FRAG || f.address != K64_ADDRESS ||
		    f.mtype != MTYPE_NC)
			fail("decode does not read slot 0 as the k64 leaf");
	}

	/* The setting off: the context of a start that did not report 64 KB tables refuses the entry
	 * that the control above accepts, and writes nothing. */
	{
		struct bc250_pte_context ctx = ctx_64k();

		ctx.system_64k = 0;
		memset(out, 0xA5, sizeof(out));
		fragment = 99;
		if (bc250_pte_from_dxgk_64k(&ctx, P64_VALID | P64_COH | P64_NX, K64_ADDRESS >> 12, out,
					    &fragment) != BC250_EINVAL || fragment != 0)
			fail("a 64 KB entry was accepted by a context that does not report 64 KB tables");
		for (n = 0; n < BC250_PTE_64K_SLOTS; n++)
			if (out[n] != 0)
				fail("a refused 64 KB entry left something in its slots");
		ctx.system_64k = 1;
		ctx.aperture = BC250_PTE_GART;
		if (bc250_pte_from_dxgk_64k(&ctx, P64_VALID | P64_COH, K64_ADDRESS >> 12, out, &fragment) == 0)
			fail("a 64 KB entry was accepted for the GART aperture");
		ctx.aperture = BC250_PTE_VM;
		if (bc250_pte_from_dxgk_64k(NULL, P64_VALID, 0, out, &fragment) == 0 ||
		    bc250_pte_from_dxgk_64k(&ctx, P64_VALID, 0, NULL, &fragment) == 0 ||
		    bc250_pte_from_dxgk_64k(&ctx, P64_VALID, 0, out, NULL) == 0)
			fail("a null argument was not refused");
	}

	/* PageTablePageSize on the level 1 entry that points at a 64 KB table: the same directory entry
	 * as for a 4 KB table when 64 KB tables are reported, refused in every other place. */
	{
		struct bc250_pte_context ctx = ctx_64k();
		const u64 table = P64_VALID | P64_SEG(VRAM_SEGMENT);
		const u64 size64 = (u64)BC250_DXGK_PAGE_TABLE_PAGE_64KB << BC250_DXGK_PTE_PAGESIZE_SHIFT;
		u64 plain = 0, typed = 1;

		if (bc250_pte_from_dxgk_level(&ctx, 1, table, 0x3000ull >> 12, &plain) != 0 ||
		    bc250_pte_from_dxgk_level(&ctx, 1, table | size64, 0x3000ull >> 12, &typed) != 0 ||
		    plain != typed || plain != 0x0000000400003001ull)
			fail("a level 1 entry of a 64 KB table is not the directory entry of a 4 KB table");
		if (bc250_pte_from_dxgk_level(&ctx, 2, table | size64, 0x3000ull >> 12, &typed) == 0 ||
		    bc250_pte_from_dxgk_level(&ctx, 3, table | size64, 0x3000ull >> 12, &typed) == 0 ||
		    bc250_pte_from_dxgk_level(&ctx, 0, table | size64, 0x3000ull >> 12, &typed) == 0)
			fail("PageTablePageSize = 64 KB was accepted at a level other than 1");
		ctx.large_pages = 1;
		if (bc250_pte_from_dxgk_level(&ctx, 1, table | size64 | BC250_DXGK_PTE_LARGEPAGE,
					      0x200000ull >> 12, &typed) == 0)
			fail("PageTablePageSize = 64 KB was accepted on a large page");
		ctx.large_pages = 0;
		ctx.system_64k = 0;
		if (bc250_pte_from_dxgk_level(&ctx, 1, table | size64, 0x3000ull >> 12, &typed) == 0 || typed != 0)
			fail("PageTablePageSize = 64 KB was accepted without 64 KB tables reported");
	}
	printf("  %u cases, the E17 k64 control in both address units, the setting off, null arguments\n"
	       "  and PageTablePageSize at every level\n",
	       (unsigned int)(sizeof(g_page64) / sizeof(g_page64[0])));
}

/* ---------------------------------------------------------------------------------------------
 * 10. Both on one start: a 2 MiB aligned base (huge_pages_only) with 64 KB page tables (system_64k)
 * ------------------------------------------------------------------------------------------- */

/* The context of the unit of section 8 on a start that also reports 64 KB tables. A level 1 entry
 * is then a 2 MiB page, a pointer to a 64 KB leaf table or a pointer to a 4 KB leaf table. Each
 * answer must be the one of the feature alone, and what was refused by one stays refused. */
static void check_huge_and_page64(void)
{
	struct bc250_pte_context both = small_ctx(), huge = small_ctx(), page64 = small_ctx();
	const u64 table = P64_VALID | P64_SEG(3);
	const u64 size64 = (u64)BC250_DXGK_PAGE_TABLE_PAGE_64KB << BC250_DXGK_PTE_PAGESIZE_SHIFT;
	const u64 host = P64_VALID | P64_COH | P64_NX;
	const u64 last64 = SMALL_VRAM_SIZE - 0x10000ull;	/* the last 64 KB of the segment, in its 576 KiB tail */
	u64 out[BC250_PTE_64K_SLOTS], other[BC250_PTE_64K_SLOTS], got, want;
	struct bc250_pte_fields f;
	u32 fragment, n;

	printf("\n== a 2 MiB aligned base and 64 KB page tables on one start\n");
	both.system_limit = huge.system_limit = page64.system_limit = 0x0000000450000000ull;
	both.system_64k = 1;				/* huge: the 2 MiB base alone */
	page64.system_64k = 1;				/* page64: 64 KB tables alone, on the plain base */
	page64.large_pages = page64.huge_pages_only = 0;
	page64.vram_base = SMALL_PLAIN_BASE;
	page64.vram_size = 0x1A240000ull;

	/* The 2 MiB page: the entry of section 8, with and without 64 KB tables. */
	got = want = 0;
	if (bc250_pte_from_dxgk_level(&both, 1, small_flags(VRAM_SEGMENT), 0x400000ull >> 12, &got) != 0 ||
	    bc250_pte_from_dxgk_level(&huge, 1, small_flags(VRAM_SEGMENT), 0x400000ull >> 12, &want) != 0 ||
	    got != want || got != 0x00400004536004F1ull)
		fail("the 2 MiB page changes when 64 KB tables are reported");
	bc250_pte_decode(got, BC250_PTE_DIRECTORY, &f);
	if (bc250_pte_large_page_bytes(&both, 1, &f) != BC250_PTE_HUGE_PAGE_BYTES)
		fail("the observer does not read the 2 MiB page when 64 KB tables are reported");

	/* The pointer to a leaf table: one directory entry for both table types, and not a page. */
	got = want = 0;
	if (bc250_pte_from_dxgk_level(&both, 1, table | size64, 0x3000ull >> 12, &got) != 0 ||
	    bc250_pte_from_dxgk_level(&both, 1, table, 0x3000ull >> 12, &want) != 0 ||
	    got != want || got != (SMALL_TABLE_BASE + 0x3000ull + 1ull))
		fail("a level 1 pointer to a 64 KB table is not the pointer to a 4 KB table on the 2 MiB base");
	bc250_pte_decode(got, BC250_PTE_DIRECTORY, &f);
	if (f.pde_pte || bc250_pte_large_page_bytes(&both, 1, &f) != 0)
		fail("the observer reads a pointer to a leaf table as a page");
	got = 1;
	if (bc250_pte_from_dxgk_level(&huge, 1, table | size64, 0x3000ull >> 12, &got) != BC250_EINVAL || got != 0)
		fail("PageTablePageSize = 64 KB was accepted on the 2 MiB base without 64 KB tables");

	/* A 64 KB entry of host memory: evidence E17 k64, the same 16 entries on either base. */
	if (bc250_pte_from_dxgk_64k(&both, host, K64_ADDRESS >> 12, out, &fragment) != 0 ||
	    fragment != BC250_PTE_LARGE_PAGE_FRAG || memcmp(out, g_k64, sizeof(g_k64)) != 0 ||
	    bc250_pte_from_dxgk_64k(&page64, host, K64_ADDRESS >> 12, other, &fragment) != 0 ||
	    memcmp(out, other, sizeof(out)) != 0)
		fail("the 64 KB host page changes with the base of the VRAM segment");
	bc250_pte_decode(out[0], BC250_PTE_LEAF, &f);
	if (f.pde_pte || f.fragment != BC250_PTE_LARGE_PAGE_FRAG)
		fail("a leaf of a 64 KB host page does not decode as a leaf with fragment 4");
	/* A 64 KB entry of VRAM: 16 plain leaves from the base of the layout in force, the last one the
	 * last page of the segment. Expected: address | VALID 0x01 | READABLE 0x20 | WRITEABLE 0x40. */
	if (bc250_pte_from_dxgk_64k(&both, P64_VALID | P64_NX | P64_SEG(VRAM_SEGMENT), last64 >> 12, out,
				    &fragment) != 0 || fragment != 0)
		fail("a 64 KB entry in the tail of the 2 MiB aligned segment was not mapped without a fragment");
	for (n = 0; n < BC250_PTE_64K_SLOTS; n++)
		if (out[n] != ((SMALL_VRAM_BASE + last64 + n * 0x1000ull) | 0x61ull))
			fail("a 64 KB entry of VRAM does not start at the 2 MiB aligned base");
	if (out[15] != 0x000000046D28F061ull)
		fail("the last leaf of the segment is not the page below the table segment");
	if (bc250_pte_from_dxgk_64k(&huge, host, K64_ADDRESS >> 12, out, &fragment) != BC250_EINVAL || out[0] != 0)
		fail("a 64 KB entry was accepted on the 2 MiB base without 64 KB tables");

	/* Refusals with both on, each with nothing written. */
	{
		static const struct { const char *name; u32 level; u64 flags; u64 offset; } bad[] = {
			{ "a 2 MiB page with PageTablePageSize = 64 KB", 1,
			  BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_LARGEPAGE | P64_SEG(VRAM_SEGMENT) |
			  ((u64)BC250_DXGK_PAGE_TABLE_PAGE_64KB << BC250_DXGK_PTE_PAGESIZE_SHIFT), 0x400000ull },
			{ "level 2, a 1 GiB page", 2,
			  BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_LARGEPAGE | P64_SEG(VRAM_SEGMENT), 0 },
			{ "level 2, a pointer with PageTablePageSize = 64 KB", 2,
			  BC250_DXGK_PTE_VALID | P64_SEG(3) |
			  ((u64)BC250_DXGK_PAGE_TABLE_PAGE_64KB << BC250_DXGK_PTE_PAGESIZE_SHIFT), 0x3000ull },
			{ "level 1, a 2 MiB page 64 KB above a 2 MiB offset", 1,
			  BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_LARGEPAGE | P64_SEG(VRAM_SEGMENT), 0x00210000ull },
			{ "level 1, a 2 MiB page of host memory", 1,
			  BC250_DXGK_PTE_VALID | BC250_DXGK_PTE_LARGEPAGE, 0x109200000ull },
			{ "level 0, PageTablePageSize = 64 KB on a 4 KB leaf", 0,
			  BC250_DXGK_PTE_VALID | P64_SEG(VRAM_SEGMENT) |
			  ((u64)BC250_DXGK_PAGE_TABLE_PAGE_64KB << BC250_DXGK_PTE_PAGESIZE_SHIFT), 0x1000ull },
		};
		unsigned int i;

		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			got = 0xDEADBEEFDEADBEEFull;
			if (bc250_pte_from_dxgk_level(&both, bad[i].level, bad[i].flags, bad[i].offset >> 12,
						      &got) != BC250_EINVAL || got != 0) {
				printf("  %s: entry %016llX\n", bad[i].name, (unsigned long long)got);
				fail("an entry that must be refused with both features on was accepted");
			} else {
				printf("  refused: %s\n", bad[i].name);
			}
		}
		memset(out, 0xA5, sizeof(out));
		if (bc250_pte_from_dxgk_64k(&both, host | BC250_DXGK_PTE_LARGEPAGE, K64_ADDRESS >> 12, out,
					    &fragment) != BC250_EINVAL || out[0] != 0 || out[15] != 0)
			fail("LargePage on a 64 KB entry was accepted with large pages reported");
		memset(out, 0xA5, sizeof(out));
		if (bc250_pte_from_dxgk_64k(&both, P64_VALID | P64_SEG(VRAM_SEGMENT), (last64 + 0x8000ull) >> 12,
					    out, &fragment) != BC250_EINVAL || out[0] != 0 || out[7] != 0)
			fail("a 64 KB entry that runs past the 2 MiB aligned segment was accepted");
	}
	printf("  the 2 MiB page, both table pointers, 64 KB host and VRAM entries, 8 refusals\n");
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
	check_huge();
	check_huge_only();
	check_table_segment();
	check_page64();
	check_huge_and_page64();

	printf("\n%s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
	return g_failures == 0 ? 0 : 1;
}
