/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * Writing GART page table entries (milestone M5 part B, ADR 0002).
 *
 * M4 brought the GART aperture up: the table's MC address is in the hub registers, the aperture is
 * open, and the TLB can be flushed. What M4 never did was put an entry in the table - amdgpu's own
 * gart_enable leaves it filled with the dummy page, and nothing in the traced window binds a real
 * one. This is that missing half, and it is needed now because the rings, the writeback slots and
 * the MQDs of milestone M5 live in system memory: the first ring test is the first time the GPU
 * walks a page table entry this driver wrote.
 *
 * The functions follow, at kernel tag v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449:
 *   amdgpu_gart_bind()            amdgpu_gart.c:387
 *   amdgpu_gart_map()             amdgpu_gart.c:350
 *   amdgpu_gart_unbind()          amdgpu_gart.c:300
 *   amdgpu_gmc_set_pte_pde()      amdgpu_gmc.c:162
 *   amdgpu_ttm_tt_pte_flags()     amdgpu_ttm.c:1395
 *   amdgpu_ttm_tt_pde_flags()     amdgpu_ttm.c:1362
 *   gmc_v10_0_gart_init()         gmc_v10_0.c:761   (adev->gart.gart_pte_flags)
 *
 * Deviations from Linux, all of them declared (ADR 0002):
 *
 * 1. The table's CPU mapping is a parameter rather than adev->gart.ptr. The shim owns no memory and
 *    maps nothing; the caller mapped the table and passes it in. It must be the mapping of the very
 *    table whose MC address went into bc250_gmc_setup().
 * 2. The flags are computed here instead of being passed in. Upstream they come from a ttm_tt and a
 *    ttm_resource, neither of which exists on this side; every caller here is the one case those
 *    two would have described, a cached kernel buffer in the GTT domain, so that case is written out
 *    once in bc250_gart_pte_flags() with the three upstream functions it comes from.
 * 3. Bad input is refused rather than ignored. amdgpu_gart_bind() returns void and amdgpu_gart_map()
 *    silently truncates an address that does not fit the 48-bit field; here both are errors. A
 *    wrong PTE is not a wrong pixel, it is the GPU reading or writing host memory that is not the
 *    buffer, so it is worth the return value.
 * 4. No TLB flush. Upstream amdgpu_gart_unbind() ends in amdgpu_gart_invalidate_tlb(); here the
 *    caller runs bc250_gmc_flush_gpu_tlb() itself, inside its own sequence, because on the kernel
 *    side a flush is a register sequence that has to be scheduled rather than a function call.
 *
 * Nothing here touches a register, so none of it appears in a register trace.
 */
#ifndef BC250_GART_H
#define BC250_GART_H

#include "amdgpu.h"

/*
 * The PTE flags for a cached, readable, writable, executable kernel buffer in the GTT domain, which
 * is every buffer this driver binds. On GC 10.1.3 that is
 *
 *     VALID | SYSTEM | SNOOPED | EXECUTABLE | READABLE | WRITEABLE | MTYPE_NV10(MTYPE_UC)
 *     = 0x0003000000000077
 *
 * and bc250_gart_pte_flags() builds it from the named constants rather than from that number. The
 * replay test prints both and compares them, so the constant above is a comment, not an input.
 */
u64 bc250_gart_pte_flags(struct amdgpu_device *adev);

/* One page table entry, exactly as amdgpu_gmc_set_pte_pde() forms it: the address masked to its
 * 48-bit page-aligned field, or'ed with the flags. Exposed so the host check can test the formula
 * against known inputs without a table. */
u64 bc250_gart_pte(u64 addr, u64 flags);

/*
 * Bind `pages` system pages into the GART table.
 *
 *   gart_offset  byte offset inside the GART aperture, page aligned. The MC address the GPU will
 *                use for the first page is adev->gmc.gart_start + gart_offset.
 *   pages        number of 4 KB pages.
 *   dma_addr     one bus address per page, each page aligned, as the caller's DMA mapping returned
 *                them. Not necessarily contiguous.
 *   table_cpu    the caller's CPU mapping of the GART table. Must be the table whose MC address
 *                went into bc250_gmc_setup(). Mapped uncached on Windows; each entry is written
 *                with one volatile 64-bit store, so nothing is left in a write-combine buffer for
 *                the caller to guess about.
 *
 * Returns 0, or BC250_EINVAL for a null argument, a misaligned offset or address, zero pages, a
 * range that runs past adev->gmc.gart_size, or an address with bits outside the field a PTE can
 * carry. Writes nothing at all on error, so a refused bind leaves the table as it was.
 *
 * Does not flush the TLB. The caller runs bc250_gmc_flush_gpu_tlb() afterwards, and must, before
 * the GPU touches the range.
 */
int bc250_gart_bind(struct amdgpu_device *adev, u64 gart_offset, unsigned int pages,
		    const u64 *dma_addr, void *table_cpu);

/*
 * Point `pages` entries back at the dummy page, which is what upstream calls unbound. Takes
 * adev->dummy_page_addr, which bc250_gmc_setup() already needs and already has.
 *
 * Note the flags: upstream writes 0, not "valid and pointing at the dummy page". The comment at
 * amdgpu_gart.c:311 says why - from Vega on, the SYSTEM bit clear is what makes an entry invalid,
 * and clearing every flag is how that is spelled. That is kept.
 *
 * Same validation and the same silence about the TLB as bc250_gart_bind().
 */
int bc250_gart_unbind(struct amdgpu_device *adev, u64 gart_offset, unsigned int pages,
		      void *table_cpu);

#endif /* BC250_GART_H */
