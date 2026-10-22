/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * DXGK_PTE -> AMD page table entry (milestone M7 Stage B preparation).
 *
 * Under WDDM's GpuMmu model the video memory manager owns the page tables and tells the miniport
 * what to put in them, one abstract entry at a time, through
 * DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE (d3dkmddi.h:4693-4709). Every one of those entries has to
 * become a 64-bit AMD entry in the format the hardware walks. That translation is this file. It is
 * pure arithmetic: no register is read or written, nothing is allocated, and every function is
 * safe to call at any IRQL.
 *
 * ---------------------------------------------------------------------------------------------
 * What is measured, and what is transcription
 *
 * MEASURED, on unit A, and the reason this is a translation rather than a research problem:
 *
 *   - The VMID 0 GART entry format. Fact M37: rings, write-back slots and EOP buffers in system
 *     memory, entered into the table of fact M33 with flags 0x0003000000000077 - valid, system,
 *     snooped, read, write, execute, MTYPE UC - and the CP fetched and executed from them. The
 *     address is the CPU physical address, because there is no DMA remapping for this device
 *     (fact M47: the firmware publishes no IVRS table). driver/shim/bc250_gart.c writes those
 *     entries and its host check compares them; bc250_pte_gart_flags() below is that same code
 *     path reached from here, not a second copy.
 *
 * TRANSCRIPTION ONLY, from the kernel at tag v6.18 - nothing on this list has been executed on
 * this ASIC by us, and each says what would settle it:
 *
 *   - The multi-level table for VMIDs 1..15, which is what VidMm's per-process page tables will
 *     be. gmc_v10_0.c:832 calls amdgpu_vm_adjust_size(adev, 256 * 1024, 9, 3, 48), which with the
 *     default module parameters resolves to: 256 TB of VA, num_level 3 (four levels of tables),
 *     block_size 9, fragment_size 9 (amdgpu_vm.c:2343-2426). The VA splits PDB2 = VA[47:39],
 *     PDB1 = VA[38:30], PDB0 = VA[29:21], PTB = VA[20:12] (amdgpu_vm_pt.c:50-64 and :102-111),
 *     512 entries of 8 bytes at every level, so every table is exactly one 4 KB page.
 *     GCVM_CONTEXT1_CNTL gets PAGE_TABLE_DEPTH = num_level = 3 and PAGE_TABLE_BLOCK_SIZE =
 *     block_size - 9 = 0 (gfxhub_v2_0.c:292-310), against PAGE_TABLE_DEPTH = 0 and no block size
 *     at all for VMID 0 (gfxhub_v2_0.c:254-264) - the GART is one flat array and has no
 *     directories, which is why none of this was needed until now.
 *     Settled by: bringing a VMID 1 context up and having the GPU touch one page through it.
 *   - The PDE format: VALID | SYSTEM | SNOOPED and the address in bits 47:6, and nothing else.
 *     amdgpu_gmc_get_pde_for_bo() (amdgpu_gmc.c:111-129) uses amdgpu_ttm_tt_pde_flags(), NOT
 *     pte_flags, so a directory entry carries no read/write/execute bit and no MTYPE.
 *     gmc_v10_0_get_vm_pde() (gmc_v10_0.c:469-490) would add the block-fragment-size and
 *     translate-further bits, but only when adev->gmc.translate_further is set, and nothing in
 *     gmc_v10_0.c ever sets it - on gfx10 that function only converts an MC address to a physical
 *     one. So the BFS and TF bits are deliberately not produced here.
 *   - The default memory type for a VM entry is NC, not the UC the GART uses:
 *     gmc_v10_0_get_vm_pte()'s default arm (gmc_v10_0.c:504-508) replaces whatever MTYPE the flags
 *     carried. Note AMDGPU_PTE_MTYPE_NV10() replaces bits 50:48 rather than or-ing them.
 *   - 64 KB pages as AMDGPU_PTE_FRAG(4). The fragment field means the entry covers
 *     1 << (12 + frag) bytes (amdgpu_vm_pt.c:738-743), so 4 gives 64 KB, and every entry inside
 *     the fragment must repeat the same flags over contiguous memory. That is the same shape as
 *     VidMm's 64 KB pages, but the correspondence is an inference.
 *     Settled by: one 64 KB mapping, read back through the witness.
 *
 * ---------------------------------------------------------------------------------------------
 * The three things the DDI does not say unambiguously
 *
 * Each is a field of struct bc250_pte_context rather than a constant here, so that settling one
 * on hardware is a change at the call site and not in this file.
 *
 *  1. The units of DXGK_PTE::PageAddress. The WDK comment is
 *     "High 52 bits of 64 bit physical address. Low 12 bits are zero."
 *     (d3dukmdt.h:337-338), which reads both ways: a byte address whose low 12 bits happen to be
 *     zero, or the address already shifted right by 12. Microsoft's reference page for DXGK_PTE
 *     adds a sentence the header leaves out - "The address is an offset from the start of the
 *     segment, defined by Segment, or a system memory address" - and every other segment offset in
 *     the DDI is a byte count (D3DGPU_PHYSICAL_ADDRESS::SegmentOffset is bytes), which is why
 *     BC250_PTE_ADDR_BYTES is the default. Two third-party drivers in the wild disagree with each
 *     other on this exact point, so it is a parameter and the wrong choice is refused rather than
 *     silently mis-mapped: under BC250_PTE_ADDR_BYTES an address with any of the low 12 bits set
 *     is an error.
 *     SETTLED (facts M72, E18 run 001): it is a page frame number. driver/kmd/vidmm.c passes
 *     BC250_PTE_ADDR_PAGES; the default below stays BYTES so that a caller has to say so.
 *     Questions 2 and 3 were settled by the same run the way the text below expects.
 *     Was to be settled by: logging one PageAddress for a system-memory allocation in Stage B and comparing
 *     its magnitude with installed RAM. 0x1_2345_6000 is a byte address; 0x12_3456 is a frame
 *     number.
 *  2. Which segment id means system memory. The WDK is explicit and is what this driver goes by:
 *     d3dkmddi.h:2639-2643, under the comment "System Memory SegmentId define for WDDM v2",
 *     defines DXGK_SEGMENT_ID_INVALID as 0 and DXGK_SEGMENT_ID_SYSTEMMEMORY as 31, and the segment
 *     set mask two lines below it (DXGK_SEGMENT_SET_SYSTEMMEMORY = 0x80000000, d3dkmddi.h:2648)
 *     is bit 31 of that same numbering. A five-bit Segment field that reserves its largest value
 *     for host memory and zero for "none" is consistent with both. Against that, Microsoft's
 *     reference page for DXGK_PTE can be read as saying segment zero is the system memory one,
 *     which would collide with DXGK_SEGMENT_ID_INVALID; the header wins, but not by enough to
 *     hard-code. So the context names both ids, VRAM and system, and any third id is refused. A
 *     wrong guess is then a failed translation and not a wrong page.
 *     Settled by: logging the Segment values VidMm sends in Stage B. The driver reports its own
 *     segment ids in DXGKDDI_QUERYADAPTERINFO, so the VRAM half is ours to choose; only the
 *     system-memory id is Microsoft's to tell us.
 *  3. Which page table level is the leaf. Microsoft's GPU Virtual Address page states it - "The
 *     levels are numbered from zero. Level zero is assigned to the leaf level" - and the 64 KB
 *     pages page agrees ("should be used only for PTEs of the level 1 page table, page directory
 *     in the old terminology"). It is still the caller that maps DXGK_BUILDPAGINGBUFFER_
 *     UPDATEPAGETABLE::PageTableLevel to BC250_PTE_LEAF or BC250_PTE_DIRECTORY, so if it is ever
 *     the other way round the fix is one line in driver/kmd and none here.
 *
 * ---------------------------------------------------------------------------------------------
 * Why this file has no WDDM header in it
 *
 * The caller passes the two 64-bit halves of a DXGK_PTE - its Flags union and its PageAddress
 * union - rather than the struct. The shim compiles both as user-mode C for the host tests and
 * with the WDK kernel flags, and d3dukmdt.h cannot be included directly in either (it refuses
 * unless one of four other headers has been included first, d3dukmdt.h:18-23). Passing the two
 * qwords means driver/kmd hands over `pte->Flags` and `pte->PageAddress` with no field-by-field
 * copy and nothing retyped.
 *
 * The bit positions below are therefore ours, and they are checked rather than trusted: the host
 * test driver/shim/test/replay_pte.c includes the real header through <d3dkmthk.h>, builds a real
 * DXGK_PTE, sets one field at a time and asserts that each one lands exactly where the macro here
 * says. If Microsoft moves a bit, that test fails.
 */
#ifndef BC250_PTE_H
#define BC250_PTE_H

#include "amdgpu.h"

/* ---------------------------------------------------------------------------------------------
 * The DXGK_PTE side
 *
 * d3dukmdt.h:315-340 (Windows Kits 10.0.26100.0), the Flags union, LSB first:
 *
 *     Valid:1  Zero:1  CacheCoherent:1  ReadOnly:1  NoExecute:1  Segment:5  LargePage:1
 *     PhysicalAdapterIndex:6  PageTablePageSize:2  SystemReserved0:1  Reserved:44
 *
 * checked field by field against that header in the host test.
 * ------------------------------------------------------------------------------------------- */
#define BC250_DXGK_PTE_VALID		(1ULL << 0)
#define BC250_DXGK_PTE_ZERO		(1ULL << 1)
#define BC250_DXGK_PTE_CACHECOHERENT	(1ULL << 2)
#define BC250_DXGK_PTE_READONLY		(1ULL << 3)
#define BC250_DXGK_PTE_NOEXECUTE	(1ULL << 4)
#define BC250_DXGK_PTE_SEGMENT_SHIFT	5u
#define BC250_DXGK_PTE_SEGMENT_MASK	(0x1FULL << BC250_DXGK_PTE_SEGMENT_SHIFT)
#define BC250_DXGK_PTE_LARGEPAGE	(1ULL << 10)
#define BC250_DXGK_PTE_ADAPTER_SHIFT	11u
#define BC250_DXGK_PTE_ADAPTER_MASK	(0x3FULL << BC250_DXGK_PTE_ADAPTER_SHIFT)
#define BC250_DXGK_PTE_PAGESIZE_SHIFT	17u
#define BC250_DXGK_PTE_PAGESIZE_MASK	(0x3ULL << BC250_DXGK_PTE_PAGESIZE_SHIFT)
#define BC250_DXGK_PTE_SYSRESERVED0	(1ULL << 19)
#define BC250_DXGK_PTE_RESERVED_MASK	0xFFFFFFFFFFF00000ULL   /* bits 63:20 */

/* DXGK_PTE_PAGE_SIZE, d3dukmdt.h:305-309. */
#define BC250_DXGK_PAGE_TABLE_PAGE_4KB	0u
#define BC250_DXGK_PAGE_TABLE_PAGE_64KB	1u

/* What a large page is on the Windows side. VidMm's 64 KB page support passes a second array of
 * entries, each covering 64 KB (DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE::pPageTableEntries64KB,
 * d3dkmddi.h:4693-4709), which on this hardware is AMDGPU_PTE_FRAG(4). */
#define BC250_PTE_LARGE_PAGE_SHIFT	16u
#define BC250_PTE_LARGE_PAGE_FRAG	(BC250_PTE_LARGE_PAGE_SHIFT - 12u)

/* What a large page is on a directory entry. A directory entry with LargePage maps the range of the
 * table below it (DXGK_PTE reference page, d3dukmdt.h). DXGK_GPUMMUCAPS::LargePageSupported says that
 * "all levels of page tables, except the leaf one, support large pages", so a driver that reports it
 * must accept the bit at every directory level that VidMm can use:
 *
 *   level 1 (amdgpu PDB0): 512 pages of 4 KB, 2 MiB. amdgpu calls it a huge page.
 *   level 2 (amdgpu PDB1): 512 of those, 1 GiB. amdgpu calls it a giant page.
 *   level 3 (amdgpu PDB2): 512 GiB. The allocation size must be the range of the lower level, and
 *                          no segment of this driver can hold a 512 GiB allocation, so VidMm cannot
 *                          ask for one. It is refused.
 *
 * amdgpu writes the same kind of entry at both levels: AMDGPU_PDE_PTE on any level above the leaf
 * (amdgpu_vm_pt.c:688) and AMDGPU_PTE_FRAG(frag) of the whole range on every level (:896). It picks
 * the level whose range the fragment covers (:842-855), with max_frag 31 from Vega10 on (:757-764,
 * "the L2 is now directly feed with small/huge/giant pages from the walker"). Fact M808 (E17) read a
 * level 1 entry on unit A, 0x004000046fc004e1: VALID READABLE WRITEABLE PDE_PTE MTYPE_NC FRAG 9. No
 * level 2 entry was read on unit A. */
#define BC250_PTE_LEVEL_SHIFT(level)	(12u + 9u * (u32)(level))   /* bytes an entry covers, log2 */
#define BC250_PTE_HUGE_PAGE_LEVEL	1u
#define BC250_PTE_HUGE_PAGE_SHIFT	BC250_PTE_LEVEL_SHIFT(BC250_PTE_HUGE_PAGE_LEVEL)   /* 21 */
#define BC250_PTE_HUGE_PAGE_BYTES	(1ULL << BC250_PTE_HUGE_PAGE_SHIFT)
#define BC250_PTE_HUGE_PAGE_FRAG	(BC250_PTE_HUGE_PAGE_SHIFT - 12u)
#define BC250_PTE_GIANT_PAGE_LEVEL	2u
#define BC250_PTE_GIANT_PAGE_SHIFT	BC250_PTE_LEVEL_SHIFT(BC250_PTE_GIANT_PAGE_LEVEL) /* 30 */
#define BC250_PTE_GIANT_PAGE_BYTES	(1ULL << BC250_PTE_GIANT_PAGE_SHIFT)
#define BC250_PTE_GIANT_PAGE_FRAG	(BC250_PTE_GIANT_PAGE_SHIFT - 12u)

/* ---------------------------------------------------------------------------------------------
 * The context: what DXGK_PTE does not carry and this driver has to supply
 * ------------------------------------------------------------------------------------------- */

enum bc250_pte_addr_units {
	BC250_PTE_ADDR_BYTES = 0,       /* PageAddress is a byte address, low 12 bits zero */
	BC250_PTE_ADDR_PAGES = 1        /* PageAddress is that address >> 12 */
};

enum bc250_pte_kind {
	BC250_PTE_LEAF = 0,             /* PageTableLevel 0: the entry names a page */
	BC250_PTE_DIRECTORY = 1         /* any level above it: the entry names a lower table */
};

enum bc250_pte_aperture {
	BC250_PTE_VM = 0,               /* a per-process page table, VMIDs 1..15: MTYPE NC */
	BC250_PTE_GART = 1              /* the flat VMID 0 aperture: MTYPE UC, fact M37 */
};

struct bc250_pte_context {
	enum bc250_pte_addr_units units;
	enum bc250_pte_aperture aperture;

	u32 system_segment;             /* the segment id that means host memory */
	u32 vram_segment;               /* the segment id this driver reports for local VRAM */

	/* What a VRAM segment offset is measured from, and how far it reaches. `vram_base` is the
	 * address the GPU's page table walker needs for the first byte of that segment - the same
	 * quantity amdgpu keeps as vm_manager.vram_base_offset - and `vram_size` bounds it so an
	 * offset past the end of the segment is refused instead of pointing into somebody else's
	 * memory. Both zero means no VRAM segment is mapped and any entry naming one is refused. */
	u64 vram_base;
	u64 vram_size;

	/* An upper bound on a system memory address, for the same reason. 0 means do not check. */
	u64 system_limit;

	/* Optional dedicated local page-table storage. A zero size disables it.
	 * Both directory pointers and paging-process leaf mappings may name it.
	 * Segment enumeration, ownership and cache policy belong to the KMD. */
	u32 table_segment;
	u64 table_base;
	u64 table_size;

	/* Nonzero when the driver reported DXGK_GPUMMUCAPS::LargePageSupported for this start. Only then
	 * does bc250_pte_from_dxgk_level accept LargePage on a level 1 or level 2 entry. The caller sets
	 * it only when vram_base is 1 GiB aligned. VidMm knows only segment offsets, and without
	 * AllowNonAlignedLargePageAddress it sets LargePage only on an offset aligned to the leaf table
	 * coverage. The walker reads the physical address, base plus offset. With the base aligned, an
	 * aligned offset is an aligned address for both page sizes, and an unaligned one is refused. */
	u32 large_pages;
};

/* Every field of an AMD entry, for the inverse direction. */
struct bc250_pte_fields {
	u64 address;                    /* bits 47:12 for a page, 47:6 for a directory */
	u32 mtype;                      /* bits 50:48, MTYPE_NC and friends from navi10_enum.h */
	u32 fragment;                   /* bits 11:7 */
	int valid, system, snooped, tmz, executable, readable, writeable;
	int prt, pde_pte, log, tf, noalloc;
};

/* ---------------------------------------------------------------------------------------------
 * The entry points
 * ------------------------------------------------------------------------------------------- */

/*
 * Translate one abstract entry into the 64-bit value the hardware walks.
 *
 *   ctx           the driver's segment layout and the two readings above. Not stored.
 *   kind          leaf or directory, which the caller derives from PageTableLevel.
 *   dxgk_flags    the DXGK_PTE Flags union, verbatim.
 *   dxgk_address  the DXGK_PTE PageAddress/PageTableAddress union, verbatim.
 *   out           the AMD entry.
 *
 * An entry whose Valid bit is clear translates to 0 and returns 0 without looking at anything else
 * in it. That is not a shortcut: from Vega on, the way to say "not present" is every flag bit
 * clear, because the SYSTEM bit clear is what the walker tests (amdgpu_gart.c:308-309, whose
 * comment says exactly that). Writing a valid-looking entry with the valid bit clear would be a
 * different statement.
 *
 * Two of the DXGK_PTE fields are answered by refusing rather than by guessing:
 *
 *   LargePage on a leaf becomes AMDGPU_PTE_FRAG(4), a 64 KB page, and the address must be 64 KB
 *   aligned. VidMm's 64 KB pages arrive in their own array (pPageTableEntries64KB,
 *   d3dkmddi.h:4707) so the caller knows which array it is walking without reading the bit, and
 *   the bit is then a cross-check. The DXGK_PTE reference page forbids the bit on a leaf ("This
 *   flag cannot be set for the leaf page tables"), and this driver reports no 64 KB pages, so the
 *   path is not reachable from VidMm today.
 *
 *   LargePage on a DIRECTORY entry is defined by the same reference page: "When set the page table
 *   address (PageTableAddress + SegmentId) is the memory address of an allocation. The allocation
 *   size is equal to the virtual address range, covered by the lower page table level." That is
 *   what AMD calls PDE_PTE: a directory entry that maps the memory itself. bc250_pte_from_dxgk
 *   refuses it, because it does not know the level. bc250_pte_from_dxgk_level accepts it on a level
 *   1 entry (2 MiB) or a level 2 entry (1 GiB) when the context reports large pages (large_pages).
 *
 *   PageTablePageSize (DXGK_PTE_PAGE_SIZE, d3dukmdt.h:305-309, :329) names the size of the page
 *   table page a directory entry points at. On gfx10 every level holds 512 entries of 8 bytes
 *   (amdgpu_vm_pt.c:102-111 with block_size 9), so every table is 4 KB and anything else is
 *   refused, on a leaf as well, where the field has no meaning at all.
 *
 * Returns 0, or BC250_EINVAL with *out set to 0 for: a null argument, a context whose two segment
 * ids are the same, a reserved or system-reserved bit set, a PhysicalAdapterIndex other than 0
 * (this driver is one adapter and links no others), a segment id that is neither of the two the
 * context names, a VRAM entry when the context maps no VRAM segment, an address that does not meet
 * the alignment its units, kind and page size require, an address past the end of the segment it
 * names, an address with bits above the 48 the field carries, a large page on a directory entry,
 * or a page table page size other than 4 KB.
 */
int bc250_pte_from_dxgk(const struct bc250_pte_context *ctx, enum bc250_pte_kind kind,
			u64 dxgk_flags, u64 dxgk_address, u64 *out);

/*
 * The same translation for a DXGK page table level (DXGK_BUILDPAGINGBUFFER_UPDATEPAGETABLE::
 * PageTableLevel): level 0 is a leaf, every other level a directory. The one difference from
 * bc250_pte_from_dxgk is a VALID level 1 or level 2 entry with LargePage set, when ctx->large_pages
 * is nonzero. Such an entry maps the range of the level below (2 MiB or 1 GiB) of the VRAM segment
 * itself and becomes
 *
 *     address | bc250_pte_vm_flags(...) | AMDGPU_PDE_PTE | AMDGPU_PTE_FRAG(9 or 18)
 *
 * with the same read, write, execute and snoop bits as a 4 KB leaf of the same DXGK_PTE. Refused
 * as well as everything bc250_pte_from_dxgk refuses: LargePage at level 3, in host memory
 * (SysMemLargePageSupported is not reported), in the page table segment, or with large_pages 0; an
 * address that is not aligned to the page size (AllowNonAlignedLargePageAddress is not reported);
 * and a range that does not lie inside the VRAM segment.
 */
int bc250_pte_from_dxgk_level(const struct bc250_pte_context *ctx, u32 level,
			      u64 dxgk_flags, u64 dxgk_address, u64 *out);

/*
 * The observer's half of the contract above: the size in bytes of the large page a decoded level 1
 * or level 2 entry maps, or 0 when bc250_pte_from_dxgk_level cannot have written that entry. A walker
 * that meets PDE_PTE on a directory entry ends at the page only when this is non-zero, so that a
 * malformed entry is refused and not misread. Checked: large_pages set in the context, the level, a
 * valid non-system entry with PDE_PTE, the fragment of the level's whole range, an address aligned
 * to that range, and the whole range inside the VRAM segment (the page table segment holds tables,
 * not allocations, so a page there is never a large page).
 */
u64 bc250_pte_large_page_bytes(const struct bc250_pte_context *ctx, u32 level,
			       const struct bc250_pte_fields *fields);

/* The flags a leaf entry of each aperture carries, without an address. bc250_pte_gart_flags() is
 * the value fact M37 measured, 0x0003000000000077; both are exposed so a test can compare them
 * with what bc250_gart.c produces and with each other. */
u64 bc250_pte_gart_flags(void);
u64 bc250_pte_vm_flags(int writeable, int executable, int system, int snooped);

/* The inverse: split an entry back into its fields. Total, never fails; `kind` decides only how
 * much of the low end of the address field is read. A directory entry with PDE_PTE set is a page,
 * and decodes like a leaf: a page address and the leaf flags and fragment it carries. */
void bc250_pte_decode(u64 entry, enum bc250_pte_kind kind, struct bc250_pte_fields *out);

/*
 * One line of text for a log, for example
 *
 *   0003001234567077 VALID SYSTEM SNOOPED EXE R W MTYPE=UC addr=001234567000
 *
 * Writes at most `len` bytes including the terminator and always terminates when len is non-zero.
 * Returns the number of characters it wanted to write, so a caller can notice truncation. No
 * allocation and no CRT: this has to be callable from a miniport at DISPATCH_LEVEL.
 */
unsigned int bc250_pte_describe(u64 entry, enum bc250_pte_kind kind, char *out, unsigned int len);

#endif /* BC250_PTE_H */
