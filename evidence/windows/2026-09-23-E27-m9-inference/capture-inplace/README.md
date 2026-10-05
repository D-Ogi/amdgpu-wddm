# M322: caller-owned capture storage core

Refactor only, not deployed. GfxPagingCaptureStorageSize computes the exact footprint for both endpoint alignments. GfxPagingCaptureVirtualGraphInPlace validates size/alignment, initializes reused storage, and performs the existing identity capture/planning without allocating or freeing. Storage belongs to its caller on success and failure. GfxPagingCaptureVirtualGraph remains a transitional allocator used by the current DDI; it delegates to the core and releases its own allocation on failure. No DDI resource-policy completion claim.

Actual-source routing328789checks PASS, including existing byte-oracle graph/linear/self-table/multipass fixtures. New controls check exact footprint, empty/overflow ranges,1GiB footprint, short-storage refusal without clearing existing identity, and reuse of poisoned storage while the host allocator is forced to fail. Subsequent actual packet/shadow oracles validate the reconstructed captures. WDK build and diff whitespace check pass. This does not prove OS context admission/lifetime or availability under pressure.

Reservation design inputs: Microsoft System Paging Process documents1GiB system paging VA and chunked transfers for allocations that exceed scratch space; local WDK docs identify SystemContext as a paging-engine context and hSystemContext as the paging operation context. At current84bytes/page, a full aligned1GiB range needs21MiB plus the capture header. This is a documented geometry-derived bound, not an arbitrary retry cap. It does not prove only one capture can be live per context. The general BuildPagingBuffer transfer-end-before-next-start statement uses the Transfer description; do not silently assume it proves every VirtualTransfer interleaving rule.

Next: prove appropriate reservation ownership/concurrency, allocate before context admission where failure is reportable, use the in-place core from the actual DDI, and reclaim only after accepted construction/cancellation drain. Multiple contexts need distinct ownership. Keep the existing multi-capture behavior until a documented replacement is established. Unsupported dependency/cache-domain statuses remain separate unresolved cases; neither empty success nor indefinite capacity retry is a solution.

References checked2026-09-23:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_buildpagingbuffer
- Local ref/ddi-display/d3dkmddi.md, DXGK_CREATECONTEXTFLAGS.SystemContext and DXGKARG_BUILDPAGINGBUFFER.hSystemContext, WDK26100 declarations.

Installed07102 session remains unchanged from M320-M321; development hash is recorded separately.
