# CPU reader lifetime and cache-policy review

2026-09-23, development PC host test, HEAD bed764d plus uncommitted worktree.
Driver remains unchanged from candidate0773. No lab access/mutation this turn.

The harness extracts actual VidMmTranslate, VidMmUpdatePageTable and VidMmStop
wrappers. Windows SRW locks model kernel push locks. The walker holds a fake
mapping until explicitly released; the CPU writer models its shared snapshot.
Two readers overlap, a writer waits for both, and stop cannot unmap a held read.
Post-stop and non-PASSIVE translation refuse and clear outputs. Both deliberate
mutations fail: exclusive-lock removal9 violations, reader-lock removal5;
actual code passes0. The compiler initially rejected redundant _Out_ definition;
removing it fixes the harness, not the driver. Final log is test.txt.

Limitations: no actual kernel scheduling, GPU DMA, PTE visibility/coherency or
hardware mapping acceptance. Actual page decoding has separate M187 coverage.

## Cache-policy review before full WDDM activation

Sources: pinned Microsoft DDI repo7515063cea4c9e98db6a92986c5b4ddb0463fd16,
enriched WDK26100 d3dkmddi.md; current primary pages read2026-09-23:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ne-wdm-_memory_caching_type
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_allocationinfoflags_wddm2_0
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/mapping-virtual-addresses-to-a-memory-segment

Microsoft requires consistent caching behavior across aliases of physical
addresses. Cached=0 describes write-combined backing-store allocation, not proof
of actual local-resident mapping cache attributes on this lab. Current source
retains PAGE_NOCACHE across the whole local VidMm segment while advertising
CPU visibility and creating allocations with Cached=0. WddmSegment excludes
firmware framebuffer and the reserved top range; this is not evidence that all
user mappings are disjoint. Existing short CPU Present/IB/DCN mappings need the
same policy review. Do not blindly change one mapping to WC while other aliases
remain NC. Actual alias attributes were not measured; no corruption attributed.
Full WDDM/GPU-VA activation deferred pending a consistent mapping policy.
