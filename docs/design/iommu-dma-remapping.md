# Graphics IOMMU and DMA remapping

Implementation plan from BD-029 / M448, 2026-09-24. Support is not implemented.
The [source review](../../evidence/windows/2026-09-24-E27-m9-recovery/iommu-architecture-review/REVIEW.md)
records exact Microsoft/AMD references and current address consumers.

Keep GpuMmu; process-VA IoMmu/PASID is a different model. First separate CPU
views/backing identity, device-logical page lists, GPU GART/MC addresses and
segment offsets, retaining lifetime and command-retirement ownership.
Private GTT allocation and the dummy page are the first bounded migration points.
Ring `.mc` addresses already refer to GART; do not replace every address with a PFN.

Then negotiate a supported modern graphics interface and implement Dxgkrnl
physical/adapter memory objects, CPU views and ADLs. The exact new callbacks
require PASSIVE_LEVEL. Memory and mappings remain owned until all GPU consumers
stop; no legacy allocator fallback inside an advertised remapping session.

Migrate OS aperture operations to MapApertureSegment2 and establish the precise
new-interface GPUVA/PTE and DMA-buffer pairing. Preserve alias proofs: distinct
logical addresses can name the same backing page. CPU-visible mappings are
borrowed according to their documented lifetime, not derived by inverting IOVAs.

Before declaring support, implement pre-start physical/IOMMU capabilities,
reserved-range reporting and Begin/EndExclusiveAccess. The latter must silence
all system-memory traffic, including autonomous IH/writeback/firmware consumers.
No firmware toggle or support declaration is part of this source-only plan.

Acceptance requires a real remapping-mode witness and a paired allocation with
nonidentical CPU/device addresses, followed by content, fragmented-page,
alias, lifetime and domain-transition controls. Host nonidentity fixtures and
successful operation under identity mapping are prerequisites, not that proof.
Independent cache-attribute/coherence work remains required.
