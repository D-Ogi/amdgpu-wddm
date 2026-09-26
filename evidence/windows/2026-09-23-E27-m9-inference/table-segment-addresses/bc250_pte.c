/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * DXGK_PTE -> AMD page table entry. See include/bc250_pte.h for what this is, which parts are
 * measured and which are transcribed, and the three DDI questions the public record does not
 * close.
 *
 * All kernel citations are at tag v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
 *
 * Like driver/shim/bc250_gart.c this file touches no register, and like it, its mistakes are the
 * kind the hardware cannot report: a wrong entry does not raise a fault the driver sees, it makes
 * the GPU read or write a page that is not the one VidMm meant. So every input is checked before
 * anything is produced, and a refusal writes 0 rather than a half-formed entry.
 */
#include "bc250_pte.h"
#include "bc250_gart.h"		/* bc250_gart_pte_flags(), so the GART flags exist once */
#include "bc250_gmc.h"		/* BC250_EINVAL */

/* AMD's own header, unmodified, for MTYPE_UC and MTYPE_NC. gmc_v10_0.c gets them from the same
 * file. No memory type is written as a number here. */
#include "navi10_enum.h"

#define BC250_PTE_4K_MASK	(AMDGPU_GPU_PAGE_SIZE - 1)
#define BC250_PTE_64K_MASK	((1ULL << BC250_PTE_LARGE_PAGE_SHIFT) - 1)

/* ---------------------------------------------------------------------------------------------
 * The two sets of leaf flags
 * ------------------------------------------------------------------------------------------- */

/*
 * The GART flags, fact M37, 0x0003000000000077. This calls into bc250_gart.c rather than
 * rebuilding them so that there is one statement of the measured value in the tree and not two
 * that can drift apart. bc250_gart_pte_flags() takes an adev only to look like the upstream call
 * it follows and ignores it (bc250_gart.c:44), so NULL is not a hazard here; replay_pte.c calls
 * both this and bc250_gart_pte_flags() with a real device and compares them anyway.
 */
u64 bc250_pte_gart_flags(void)
{
	return bc250_gart_pte_flags(NULL);
}

/*
 * The flags for a leaf entry of a VMID 1..15 page table, which is the transcribed half.
 *
 * The chain upstream is the same three functions the GART uses, with one difference that matters:
 * gmc_v10_0_get_vm_pte() (gmc_v10_0.c:492-519) runs last on a VM mapping and its default arm
 * replaces the memory type with MTYPE_NC (gmc_v10_0.c:504-508). Note that AMDGPU_PTE_MTYPE_NV10()
 * overwrites bits 50:48 rather than or-ing into them, so it has to be applied to the whole word
 * and not to a fragment. The same function also clears AMDGPU_PTE_EXECUTABLE when the mapping did
 * not ask for it (gmc_v10_0.c:498-501), which here is the `executable` parameter's job.
 *
 * READABLE is unconditional because every mapping VidMm makes is at least readable; there is no
 * write-only page in this model and no DXGK_PTE bit that could ask for one.
 */
u64 bc250_pte_vm_flags(int writeable, int executable, int system, int snooped)
{
	u64 flags = AMDGPU_PTE_VALID | AMDGPU_PTE_READABLE;

	if (system)
		flags |= AMDGPU_PTE_SYSTEM;
	if (snooped)
		flags |= AMDGPU_PTE_SNOOPED;
	if (writeable)
		flags |= AMDGPU_PTE_WRITEABLE;
	if (executable)
		flags |= AMDGPU_PTE_EXECUTABLE;

	return AMDGPU_PTE_MTYPE_NV10(flags, MTYPE_NC);
}

/* ---------------------------------------------------------------------------------------------
 * The translation
 * ------------------------------------------------------------------------------------------- */

/* The physical address a DXGK_PTE names, in bytes, with the segment resolved. Returns 0 on
 * success. `system` comes back 1 for host memory and 0 for local VRAM. */
static int bc250_pte_address(const struct bc250_pte_context *ctx, u64 dxgk_address,
			     u64 dxgk_flags, u64 *phys, int *system)
{
	u64 offset, local_base, local_size;
	u32 segment;

	/* Question 1 of the header: what the units are. Under BC250_PTE_ADDR_BYTES the header's
	 * "Low 12 bits are zero" is a statement about the value, so a value that breaks it is an
	 * error and not something to round away. Under BC250_PTE_ADDR_PAGES the same sentence says
	 * the field has already been shifted, and then the top twelve bits must be clear instead. */
	if (ctx->units == BC250_PTE_ADDR_PAGES) {
		if ((dxgk_address >> (64u - 12u)) != 0)
			return BC250_EINVAL;
		offset = dxgk_address << 12;
	} else {
		if ((dxgk_address & BC250_PTE_4K_MASK) != 0)
			return BC250_EINVAL;
		offset = dxgk_address;
	}

	segment = (u32)((dxgk_flags & BC250_DXGK_PTE_SEGMENT_MASK) >> BC250_DXGK_PTE_SEGMENT_SHIFT);

	/* A configured second local segment must be unambiguous and page aligned.
	 * Do not silently reinterpret system/application addresses on bad layout. */
	if (ctx->table_size && (ctx->table_segment > 31u ||
	    ctx->table_segment == ctx->system_segment || ctx->table_segment == ctx->vram_segment ||
	    ((ctx->table_base | ctx->table_size) & BC250_PTE_4K_MASK) != 0 ||
	    ctx->table_size - 1 > ~ctx->table_base))
		return BC250_EINVAL;
	if (ctx->table_size && ctx->vram_size &&
	    ((ctx->table_base <= ctx->vram_base && ctx->vram_base-ctx->table_base < ctx->table_size) ||
	     (ctx->vram_base <= ctx->table_base && ctx->table_base-ctx->vram_base < ctx->vram_size)))
		return BC250_EINVAL;

	if (segment == ctx->system_segment) {
		/* A system-memory entry carries a host physical address directly. There is no DMA
		 * remapping on this device to turn it into something else: fact M47, the firmware
		 * publishes no IVRS table, which is also why bc250_gart.c writes CPU physical
		 * addresses into the GART and the CP fetched from them. */
		if (ctx->system_limit != 0 && offset >= ctx->system_limit)
			return BC250_EINVAL;
		*phys = offset;
		*system = 1;
		return 0;
	}

	local_base = ctx->vram_base;
	local_size = ctx->vram_size;
	if (ctx->table_size && segment == ctx->table_segment) {
		local_base = ctx->table_base;
		local_size = ctx->table_size;
	} else if (segment != ctx->vram_segment) {
		return BC250_EINVAL;
	}
	/* Each local segment has its own offset origin. Use physical addresses,
	 * not MC addresses, for both directory pointers and leaf mappings. */
	if (!local_size || offset >= local_size || offset > ~local_base)
		return BC250_EINVAL;
	*phys = local_base + offset;
	*system = 0;
	return 0;
}

int bc250_pte_from_dxgk(const struct bc250_pte_context *ctx, enum bc250_pte_kind kind,
			u64 dxgk_flags, u64 dxgk_address, u64 *out)
{
	u64 phys = 0, flags;
	int system = 0, rc;

	if (ctx == NULL || out == NULL)
		return BC250_EINVAL;

	*out = 0;

	/* An entry VidMm marks not present translates to a flags word of 0 and nothing else is
	 * read. amdgpu_gart.c:308-309 states the rule and the reason: "Starting from VEGA10, system
	 * bit must be 0 to mean invalid", so clearing every flag is how absence is spelled on this
	 * hardware, not a valid entry pointing somewhere harmless. The early return is also what
	 * makes an unmap safe to hand over verbatim: the fields beside Valid may be anything at
	 * all, and none of them can turn into a refusal here. */
	if ((dxgk_flags & BC250_DXGK_PTE_VALID) == 0)
		return 0;

	/* A context that names the same id for both segments cannot be resolved, and the ambiguity
	 * would land on whichever branch happens to be tested first. Refuse the context instead. */
	if (ctx->system_segment == ctx->vram_segment)
		return BC250_EINVAL;

	/* Fields we do not understand must be zero rather than ignored. `Zero` is named for its
	 * only legal value; SystemReserved0 and Reserved belong to Microsoft. */
	if ((dxgk_flags & (BC250_DXGK_PTE_ZERO | BC250_DXGK_PTE_SYSRESERVED0 |
			   BC250_DXGK_PTE_RESERVED_MASK)) != 0)
		return BC250_EINVAL;

	/* One adapter. A non-zero index would be a linked-adapter entry naming another GPU's
	 * memory, which this driver never reports support for and could not map. */
	if ((dxgk_flags & BC250_DXGK_PTE_ADAPTER_MASK) != 0)
		return BC250_EINVAL;

	/* Every page table on gfx10 is one 4 KB page: 512 entries of 8 bytes at all four levels -
	 * amdgpu_vm_pt_entries_mask() (amdgpu_vm_pt.c:102-111) returns 0x1ff for every directory
	 * level and AMDGPU_VM_PTE_COUNT(adev) - 1 at the leaf, which with block_size 9
	 * (gmc_v10_0.c:832) is the same 0x1ff. A 64 KB table page has nowhere to go. */
	if ((dxgk_flags & BC250_DXGK_PTE_PAGESIZE_MASK) !=
	    ((u64)BC250_DXGK_PAGE_TABLE_PAGE_4KB << BC250_DXGK_PTE_PAGESIZE_SHIFT))
		return BC250_EINVAL;

	rc = bc250_pte_address(ctx, dxgk_address, dxgk_flags, &phys, &system);
	if (rc != 0)
		return rc;

	if (kind == BC250_PTE_DIRECTORY) {
		/* See the header: on a directory entry LargePage has two possible readings and the
		 * DDI names neither, so it does not get a guess. */
		if ((dxgk_flags & BC250_DXGK_PTE_LARGEPAGE) != 0)
			return BC250_EINVAL;

		/* The PDE address field reaches six bits lower than a PTE's. Upstream asserts the
		 * same condition the other way round, BUG_ON(*addr & 0xFFFF00000000003FULL) at
		 * gmc_v10_0.c:474; a driver that BUG_ONs on what an outside caller sent it would be
		 * a worse driver, so this refuses. */
		if ((phys & ~AMDGPU_PDE_ADDR_MASK) != 0)
			return BC250_EINVAL;

		/* A directory entry carries VALID, SYSTEM and SNOOPED and nothing else: no read,
		 * write or execute bit and no memory type. amdgpu_gmc_get_pde_for_bo()
		 * (amdgpu_gmc.c:111-129) builds it from amdgpu_ttm_tt_pde_flags(), not from
		 * amdgpu_ttm_tt_pte_flags(). gmc_v10_0_get_vm_pde() (gmc_v10_0.c:469-490) would add
		 * the block-fragment-size and translate-further bits on top, but only when
		 * adev->gmc.translate_further is set, and nothing in gmc_v10_0.c ever sets it, so
		 * those bits are deliberately absent here too. */
		flags = AMDGPU_PTE_VALID;
		if (system)
			flags |= AMDGPU_PTE_SYSTEM;
		if ((dxgk_flags & BC250_DXGK_PTE_CACHECOHERENT) != 0)
			flags |= AMDGPU_PTE_SNOOPED;

		*out = (phys & AMDGPU_PDE_ADDR_MASK) | flags;
		return 0;
	}

	if ((phys & ~AMDGPU_PTE_ADDR_MASK) != 0)
		return BC250_EINVAL;            /* bits 63:48, which the field cannot carry */

	if (ctx->aperture == BC250_PTE_GART) {
		/* The measured VMID 0 entry. Its flags do not vary: the aperture is one flat table
		 * of cached, coherent, readable, writable, executable host pages, which is what fact
		 * M37 put in it and what the CP fetched from. A DXGK_PTE that asks for anything
		 * else - VRAM, read-only, no-execute, uncoherent - is not something this aperture
		 * can express, so it is refused rather than quietly widened or narrowed. */
		if (system == 0 ||
		    (dxgk_flags & BC250_DXGK_PTE_CACHECOHERENT) == 0 ||
		    (dxgk_flags & BC250_DXGK_PTE_READONLY) != 0 ||
		    (dxgk_flags & BC250_DXGK_PTE_NOEXECUTE) != 0)
			return BC250_EINVAL;
		flags = bc250_pte_gart_flags();
	} else {
		flags = bc250_pte_vm_flags((dxgk_flags & BC250_DXGK_PTE_READONLY) == 0,
					   (dxgk_flags & BC250_DXGK_PTE_NOEXECUTE) == 0,
					   system,
					   (dxgk_flags & BC250_DXGK_PTE_CACHECOHERENT) != 0);
	}

	if ((dxgk_flags & BC250_DXGK_PTE_LARGEPAGE) != 0) {
		/* 64 KB, as a fragment of 4: the entry covers 1 << (12 + frag) bytes
		 * (amdgpu_vm_pt.c:738-743). The fragment field is a promise about the entries
		 * around this one as much as about this one - all of them must carry the same flags
		 * over physically contiguous memory - which the caller keeps by walking VidMm's
		 * 64 KB array as a unit. Here the part that can be checked is checked: an address
		 * that is not 64 KB aligned cannot be the start of such a range. */
		if ((phys & BC250_PTE_64K_MASK) != 0)
			return BC250_EINVAL;
		flags |= AMDGPU_PTE_FRAG(BC250_PTE_LARGE_PAGE_FRAG);
	}

	*out = (phys & AMDGPU_PTE_ADDR_MASK) | flags;
	return 0;
}

/* ---------------------------------------------------------------------------------------------
 * The inverse, for logging
 * ------------------------------------------------------------------------------------------- */

void bc250_pte_decode(u64 entry, enum bc250_pte_kind kind, struct bc250_pte_fields *out)
{
	if (out == NULL)
		return;

	out->address = entry & (kind == BC250_PTE_DIRECTORY ? AMDGPU_PDE_ADDR_MASK : AMDGPU_PTE_ADDR_MASK);
	out->mtype = (u32)((entry & AMDGPU_PTE_MTYPE_NV10_MASK) >> 48);
	out->fragment = (u32)((entry >> 7) & 0x1fu);

	out->valid      = (entry & AMDGPU_PTE_VALID) != 0;
	out->system     = (entry & AMDGPU_PTE_SYSTEM) != 0;
	out->snooped    = (entry & AMDGPU_PTE_SNOOPED) != 0;
	out->tmz        = (entry & AMDGPU_PTE_TMZ) != 0;
	out->executable = (entry & AMDGPU_PTE_EXECUTABLE) != 0;
	out->readable   = (entry & AMDGPU_PTE_READABLE) != 0;
	out->writeable  = (entry & AMDGPU_PTE_WRITEABLE) != 0;
	out->prt        = (entry & AMDGPU_PTE_PRT) != 0;
	out->pde_pte    = (entry & AMDGPU_PDE_PTE) != 0;
	out->log        = (entry & AMDGPU_PTE_LOG) != 0;
	out->tf         = (entry & AMDGPU_PTE_TF) != 0;
	out->noalloc    = (entry & AMDGPU_PTE_NOALLOC) != 0;

	/* A directory entry has no flags below bit 3, so the fragment field it appears to carry is
	 * part of its address. Report it as zero rather than as a number that means nothing. */
	if (kind == BC250_PTE_DIRECTORY) {
		out->fragment = 0;
		out->executable = 0;
		out->readable = 0;
		out->writeable = 0;
		out->tmz = 0;
	}
}

/* A miniport cannot call the CRT at DISPATCH_LEVEL, and there is no reason for a formatter this
 * small to. Both helpers append into a caller buffer, count what they would have written whether
 * it fitted or not, and never write past `len`. */
struct bc250_pte_sink {
	char *out;
	unsigned int len;
	unsigned int want;
};

static void sink_char(struct bc250_pte_sink *s, char c)
{
	if (s->len != 0 && s->want + 1u < s->len)
		s->out[s->want] = c;
	s->want++;
}

static void sink_str(struct bc250_pte_sink *s, const char *text)
{
	while (*text != '\0')
		sink_char(s, *text++);
}

static void sink_hex(struct bc250_pte_sink *s, u64 value, unsigned int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	unsigned int i;

	for (i = digits; i > 0; i--)
		sink_char(s, hex[(value >> ((i - 1u) * 4u)) & 0xfu]);
}

unsigned int bc250_pte_describe(u64 entry, enum bc250_pte_kind kind, char *out, unsigned int len)
{
	/* MTYPE 0..7 in the names amdgpu uses for them, which is the set gmc_v10_0_get_vm_pte()
	 * switches over (gmc_v10_0.c:503-517): NC, WC, CC from TCC_MTYPE and UC from MTYPE, which
	 * share a C namespace and so form one run of four. The upper four cannot be produced here
	 * and are printed as their number. */
	static const char *const mtype_name[8] = {
		"NC", "WC", "CC", "UC", "?4", "?5", "?6", "?7"
	};
	struct bc250_pte_fields f;
	struct bc250_pte_sink s;

	s.out = out;
	s.len = (out == NULL) ? 0u : len;
	s.want = 0;

	bc250_pte_decode(entry, kind, &f);

	sink_hex(&s, entry, 16);

	if (entry == 0) {
		sink_str(&s, " INVALID");
	} else {
		if (f.valid)
			sink_str(&s, " VALID");
		if (f.system)
			sink_str(&s, " SYSTEM");
		if (f.snooped)
			sink_str(&s, " SNOOPED");
		if (f.tmz)
			sink_str(&s, " TMZ");
		if (f.executable)
			sink_str(&s, " EXE");
		if (f.readable)
			sink_str(&s, " R");
		if (f.writeable)
			sink_str(&s, " W");
		if (f.prt)
			sink_str(&s, " PRT");
		if (f.pde_pte)
			sink_str(&s, " PDE_PTE");
		if (f.log)
			sink_str(&s, " LOG");
		if (f.tf)
			sink_str(&s, " TF");
		if (f.noalloc)
			sink_str(&s, " NOALLOC");
		if (f.fragment != 0) {
			sink_str(&s, " FRAG=");
			sink_hex(&s, f.fragment, 2);
		}
		if (kind != BC250_PTE_DIRECTORY) {
			sink_str(&s, " MTYPE=");
			sink_str(&s, mtype_name[f.mtype & 7u]);
		}
		sink_str(&s, " addr=");
		sink_hex(&s, f.address, 12);
	}

	if (s.len != 0)
		s.out[(s.want < s.len) ? s.want : (s.len - 1u)] = '\0';

	return s.want;
}
