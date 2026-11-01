// BD-029: separate a private allocation's CPU view from its device
// page addresses. See docs/design/iommu-dma-remapping.md (M448).
// ADL consumption gives the OS path a typed device-address layer.
// Full remapping support still needs caps, domain lifetime and
// nonidentity acceptance (steps after this source-only preparation).
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

// Read one device-page address from a DXGK_ADL at the given page offset.
// The ADL is OS-owned and immutable for the duration of the BuildPagingBuffer
// call. Contiguous ADLs give a base page number; non-contiguous give a per-
// page array. Both are page frame numbers that need shifting to byte addresses.
// Returns 0 on out-of-range or overflow.
static __inline int Bc250DmaAdlPage(const void* Adl, unsigned AdlOffset,
                                    unsigned PageIndex, BC250_DMA_ADDR* Address)
{
    // DXGK_ADL layout (d3dkmddi.h WDDM 2.9+):
    //   UINT32 PageCount;
    //   DXGK_ADL_FLAGS Flags; // .Contiguous bit 0
    //   union { DXGK_PAGE_NUMBER BasePageNumber; const DXGK_PAGE_NUMBER* Pages; }
    // DXGK_PAGE_NUMBER is UINT_PTR.
    typedef unsigned __int64 PFN;
    const unsigned char* raw = (const unsigned char*)Adl;
    unsigned pageCount;
    unsigned contiguous;
    unsigned absIndex;
    PFN pfn;
    if (!Adl || !Address) return 0;
    pageCount = *(const unsigned*)raw;
    contiguous = *(const unsigned*)(raw + 4) & 1u;
    absIndex = AdlOffset + PageIndex;
    if (absIndex < AdlOffset || absIndex >= pageCount) return 0;
    if (contiguous) {
        PFN base = *(const PFN*)(raw + 8);
        if (base > ~(PFN)0 - absIndex) return 0;
        pfn = base + absIndex;
    } else {
        const PFN* pages = *(const PFN* const*)(raw + 8);
        if (!pages) return 0;
        pfn = pages[absIndex];
    }
    if (pfn > (~(BC250_DMA_ADDR)0 / PAGE_SIZE)) return 0;
    *Address = (BC250_DMA_ADDR)pfn * PAGE_SIZE;
    return 1;
}

#endif
