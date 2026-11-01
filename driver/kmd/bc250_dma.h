// BD-029 step 1: typed DMA address for IOMMU preparation.
//
// Every address the GPU reads through a GART PTE or a ring/fence/IH base
// must be a device-logical address, not a CPU physical address, when DMA
// remapping is active. Today they are identical (no IOMMU). This header
// marks the boundary and provides the single replacement point.
//
// Consumers: GART dummy page, private GTT backing (gpumem.c), ring/MQD/
// write-back pool (gfx.c), IH (bc250_ih.c), paging DDI (wddm.c, vidmm.c).
// This commit migrates the dummy page and the private GTT backing.
// The remaining consumers keep their raw MmGetPhysicalAddress calls until
// the DMA adapter and mapping infrastructure (design step 2-3) exists.
#ifndef BC250_DMA_H
#define BC250_DMA_H

typedef ULONGLONG BC250_DMA_ADDR;

// Returns the DMA address for a contiguous nonpaged allocation.
// Identity mapping today; DMA adapter MapTransfer under IOMMU (step 3).
static __inline BC250_DMA_ADDR Bc250DmaAddress(void* VirtualAddress)
{
    return (BC250_DMA_ADDR)MmGetPhysicalAddress(VirtualAddress).QuadPart;
}

#endif
