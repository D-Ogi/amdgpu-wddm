// BD-029 step 1: separate a private allocation's CPU view from its device
// page addresses. See docs/design/iommu-dma-remapping.md (M448).
// Graphics remapping needs Dxgkrnl memory objects and ADLs, not a generic
// IoGetDmaAdapter/MapTransfer substitution. This is not remapping support.
#ifndef BC250_DMA_H
#define BC250_DMA_H

typedef ULONGLONG BC250_DMA_ADDR;

// Legacy backend only: the caller owns a contiguous nonpaged allocation.
// Do not use this to manufacture addresses in an advertised remapping mode.
static __inline BC250_DMA_ADDR Bc250LegacyDmaAddress(void* VirtualAddress)
{
    return (BC250_DMA_ADDR)MmGetPhysicalAddress(VirtualAddress).QuadPart;
}

// A retained device view belongs to the same owner as its backing allocation.
// Pages, when non-NULL, is a borrowed immutable list retained until all GPU
// consumers and translations retire. Otherwise Base describes contiguous
// device addresses. GPU GART/MC addresses and CPU pointers stay separate.
// A future ADL backend must also retain its Dxgkrnl handles/domain lifetime;
// this view alone neither creates a mapping nor establishes backing identity.
typedef struct _BC250_DMA_PAGES {
    const BC250_DMA_ADDR* Pages;
    BC250_DMA_ADDR Base;
    unsigned Count;
} BC250_DMA_PAGES;

static __inline int Bc250DmaPage(const BC250_DMA_PAGES* View, unsigned Index,
                               BC250_DMA_ADDR* Address)
{
    BC250_DMA_ADDR value, offset;
    if (!View || !Address || Index >= View->Count) return 0;
    if (View->Pages) value = View->Pages[Index];
    else {
        offset = (BC250_DMA_ADDR)Index * PAGE_SIZE;
        if (View->Base > ~(BC250_DMA_ADDR)0 - offset) return 0;
        value = View->Base + offset;
    }
    if (value & (PAGE_SIZE - 1)) return 0;
    *Address = value;
    return 1;
}

#endif
