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
 *   the bit is then a cross-check. On a DIRECTORY entry the same bit could instead mean what AMD
 *   calls PDE_PTE - a directory entry that maps a 2 MB page itself - and nothing in the header or
 *   the reference pages says which, so it is refused. Nothing is lost: VidMm only uses what the
 *   driver reported support for, and refusing is loud.
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

/* The flags a leaf entry of each aperture carries, without an address. bc250_pte_gart_flags() is
 * the value fact M37 measured, 0x0003000000000077; both are exposed so a test can compare them
 * with what bc250_gart.c produces and with each other. */
u64 bc250_pte_gart_flags(void);
u64 bc250_pte_vm_flags(int writeable, int executable, int system, int snooped);

/* The inverse: split an entry back into its fields. Total, never fails; `kind` decides only how
 * much of the low end of the address field is read. */
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
