/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0 */
/*
 * GART page table entries. See include/bc250_gart.h for what this is, the four declared deviations
 * from Linux, and the full list of upstream functions it follows.
 *
 * All citations are at kernel tag v6.18, commit 7d0a66e4bb9081d75c82ec4957c50034cb0ea449.
 *
 * This file writes memory and no registers. It is the one part of the shim whose mistakes the
 * hardware cannot report: a wrong PTE does not raise a fault the driver sees, it makes the GPU read
 * or write a page of host memory that is not the buffer. Hence the validation, and hence the host
 * check in driver/shim/test/replay_gfx.c.
 */
#include "bc250_gart.h"
#include "bc250_gmc.h"		/* BC250_EINVAL */

/* AMD's own header, unmodified, for MTYPE_UC. gmc_v10_0.c gets it from the same file. */
#include "navi10_enum.h"

/* ---------------------------------------------------------------------------------------------
 * The flags
 *
 * Three upstream functions in a row, for a cached kernel buffer in the GTT domain:
 *
 *   amdgpu_ttm.c:1362 amdgpu_ttm_tt_pde_flags(ttm, mem)
 *       mem->mem_type != TTM_PL_SYSTEM                  -> AMDGPU_PTE_VALID
 *       mem->mem_type == TTM_PL_TT                      -> AMDGPU_PTE_SYSTEM
 *       ttm->caching == ttm_cached                      -> AMDGPU_PTE_SNOOPED
 *   gmc_v10_0.c:761 adev->gart.gart_pte_flags
 *       AMDGPU_PTE_MTYPE_NV10(0ULL, MTYPE_UC) | AMDGPU_PTE_EXECUTABLE
 *   amdgpu_ttm.c:1395 amdgpu_ttm_tt_pte_flags(adev, ttm, mem)
 *       the two above, plus AMDGPU_PTE_READABLE, plus AMDGPU_PTE_WRITEABLE unless the ttm_tt is
 *       read-only, which a kernel buffer of ours never is.
 *
 * MTYPE_UC on top of SNOOPED is not a contradiction and is worth a sentence, because it looks like
 * one: SNOOPED is about coherency, whether the GPU's access participates in the host's cache
 * protocol, and MTYPE_UC is about the GPU's own caches, whether it may keep the line in L2. amdgpu
 * asks for both on every GART page of every GFX10 part, and it has to: the CPU writes a ring and the
 * GPU must see it without either side flushing anything by hand.
 * ------------------------------------------------------------------------------------------- */
u64 bc250_gart_pte_flags(struct amdgpu_device *adev)
{
	u64 flags;

	(void)adev;

	/* amdgpu_ttm_tt_pde_flags() for TTM_PL_TT, cached */
	flags = AMDGPU_PTE_VALID | AMDGPU_PTE_SYSTEM | AMDGPU_PTE_SNOOPED;

	/* adev->gart.gart_pte_flags */
	flags |= AMDGPU_PTE_MTYPE_NV10(0ULL, MTYPE_UC) | AMDGPU_PTE_EXECUTABLE;

	/* amdgpu_ttm_tt_pte_flags() */
	flags |= AMDGPU_PTE_READABLE;
	flags |= AMDGPU_PTE_WRITEABLE;

	return flags;
}

/* amdgpu_gmc.c:162 amdgpu_gmc_set_pte_pde(), the value half. GART has no PDEs, so this is the
 * whole of it: mask the address to the field it fits in, or in the flags. */
u64 bc250_gart_pte(u64 addr, u64 flags)
{
	return (addr & AMDGPU_PTE_ADDR_MASK) | flags;
}

/* ---------------------------------------------------------------------------------------------
 * The common walk
 *
 * amdgpu_gart.c:350 amdgpu_gart_map() and :300 amdgpu_gart_unbind() are the same loop over a
 * different source of page_base, so it is written once. Upstream's inner loop over
 * AMDGPU_GPU_PAGES_IN_CPU_PAGE is kept even though it runs once on every target this driver has;
 * dropping it would make the two functions stop looking like the ones they came from, for no gain.
 *
 * `dma_addr` NULL means unbind: every page takes adev->dummy_page_addr and flags 0.
 * ------------------------------------------------------------------------------------------- */
static int bc250_gart_walk(struct amdgpu_device *adev, u64 gart_offset, unsigned int pages,
			   const u64 *dma_addr, void *table_cpu)
{
	volatile u64 *table;
	u64 flags, span;
	unsigned int i, j, t;

	if (adev == NULL || table_cpu == NULL || pages == 0)
		return BC250_EINVAL;
	if ((gart_offset & (AMDGPU_GPU_PAGE_SIZE - 1)) != 0)
		return BC250_EINVAL;

	/* The range must lie inside the aperture the hub registers describe. gart_size comes from
	 * bc250_gmc_setup(), which read it off the hardware, so this is checked against unit A and
	 * not against a constant. */
	span = (u64)pages * AMDGPU_GPU_PAGE_SIZE;
	if (span / AMDGPU_GPU_PAGE_SIZE != pages)
		return BC250_EINVAL;                    /* pages * 4096 overflowed */
	if (gart_offset > adev->gmc.gart_size || span > adev->gmc.gart_size - gart_offset)
		return BC250_EINVAL;

	/* Validate every address before writing any entry, so that a refused bind leaves the table
	 * exactly as it was rather than half done. */
	if (dma_addr != NULL) {
		for (i = 0; i < pages; i++) {
			if ((dma_addr[i] & (AMDGPU_GPU_PAGE_SIZE - 1)) != 0)
				return BC250_EINVAL;    /* not page aligned */
			if ((dma_addr[i] & ~AMDGPU_PTE_ADDR_MASK) != 0)
				return BC250_EINVAL;    /* bits a PTE cannot carry */
		}
	} else if ((adev->dummy_page_addr & ~AMDGPU_PTE_ADDR_MASK) != 0 ||
		   (adev->dummy_page_addr & (AMDGPU_GPU_PAGE_SIZE - 1)) != 0) {
		return BC250_EINVAL;
	}

	/* amdgpu_gart.c:311: "Starting from VEGA10, system bit must be 0 to mean invalid", which is
	 * why an unbind writes flags 0 rather than a valid entry pointing at the dummy page. */
	flags = (dma_addr != NULL) ? bc250_gart_pte_flags(adev) : 0;

	table = (volatile u64 *)table_cpu;
	t = (unsigned int)(gart_offset / AMDGPU_GPU_PAGE_SIZE);

	for (i = 0; i < pages; i++) {
		u64 page_base = (dma_addr != NULL) ? dma_addr[i] : adev->dummy_page_addr;

		for (j = 0; j < AMDGPU_GPU_PAGES_IN_CPU_PAGE; j++, t++) {
			/* One volatile 64-bit store per entry. The table is mapped uncached, so this
			 * is the write that reaches memory; there is nothing to flush afterwards and
			 * nothing left for the caller to guess about. */
			table[t] = bc250_gart_pte(page_base, flags);
			page_base += AMDGPU_GPU_PAGE_SIZE;
		}
	}

	return 0;
}

int bc250_gart_bind(struct amdgpu_device *adev, u64 gart_offset, unsigned int pages,
		    const u64 *dma_addr, void *table_cpu)
{
	if (dma_addr == NULL)
		return BC250_EINVAL;
	return bc250_gart_walk(adev, gart_offset, pages, dma_addr, table_cpu);
}

int bc250_gart_unbind(struct amdgpu_device *adev, u64 gart_offset, unsigned int pages,
		      void *table_cpu)
{
	return bc250_gart_walk(adev, gart_offset, pages, NULL, table_cpu);
}
