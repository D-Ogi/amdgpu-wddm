# M448: graphics-specific IOMMU migration review

2026-09-24. Source/contract review only. No driver, capability, INF, firmware or
lab change. [Detailed review and pinned references](REVIEW.md).

Main checked the local Microsoft DMA-remapping guide against the report.
WDDM distinguishes shared process VA/PASID, identity isolation and nonidentity
remapping. The existing GpuMmu path needs graphics memory-object/ADL ownership,
paging integration, address-domain separation and complete DMA quiescence.
Missing IoGetDmaAdapter or an INF declaration alone does not prove actual mode.

The proposed implementation sequence is a design, not implemented support.
OS-owned GPUVA/PTE and DMA-buffer backing identity still require precise contracts
for the chosen new interface. No IOMMU toggle is authorized by this review.
Current lab state remains M441 candidate136; full M9 acceptance remains open.
