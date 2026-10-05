# Physical page-list address groundwork

2026-09-23, base bed764d plus ongoing uncommitted M9 work. No lab access.
PagingPageListAddress resolves one within-page byte slice from an indexed PFN/page
list or a contiguous BasePageNumber range. FirstPage (MdlOffset/AdlOffset) is distinct
from ByteOffset (progress). It checks index/count/within-page bounds, null pointers,
contiguous arithmetic and page-number shift overflow, clearing Address on refusal.
The result is a device-programmable address, not a CPU-mapping authorization.

20196 independently computed address checks cover17 permuted pages, every valid
starting index and33 intra-page offsets for both fragmented and contiguous lists;
13 additional boundary controls pass. Existing stream tests, including fragmented
copy/fill and1GiB exact-once progress, pass.8939 packet/routing regressions pass.
Kernel compilation and full dev build/sign pass. No real MDL/ADL adapter, WDDM
physical Transfer/Fill dispatch or packet publication is wired by this change.

Dev SYS5B63CDD753F48DE14A660BF8BE5515B295703EBD1E0FFDF13B90A08BFDAE66EB
scratch/build/page-list-dev retains0781: DO NOT DEPLOY. Official0781 unchanged.

Local MS contracts inspected: enriched d3dkmddi.md, WDK26100, original docs commit
7515063cea4c9e98db6a92986c5b4ddb0463fd16. DXGKARG_BUILDPAGINGBUFFER Transfer uses MDL
pointers when SegmentId0; MdlOffset is a page index. TransferOffset applies only
to segment locations, never MDLs. SegmentAddress already includes segment base;
adding the base again is wrong. ADL Pages may be discontinuous and describe logical
IOMMU addresses; they must not be CPU-mapped. MapApertureSegment2 uses ADL and is a
WDDM2.9 extension, distinct from the existing WDDM2.0 Transfer arm.

Next: adapt actual MDL PFNs and byte extents, segment MC address bounds and flags,
then emit through existing GART-window/direct SDMA paths with source/destination
identity separated. Preserve accepted multipass publication. Review SIZE_T transfer
sizes against existing unsigned stream/progress limits rather than silently
truncating. OS page lifetime, ADL logical DMA semantics, restricted DDI return
policy, M190 cache aliases, GPU recovery and hardware/performance remain open.
