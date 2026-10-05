# OS paging DMA GPU-addressability

Plan written2026-09-24 before candidate130 deployment.

Hypothesis: selecting prepared aperture segment2 for PagingBufferSegmentId
allows VidMm to supply mapped GPU addresses for its paging DMA buffers.
Current segment0 choice has historical SubmitCommandVirtual VA0 observations.
The local Microsoft DXGK_QUERYSEGMENTOUT contract permits aperture or0;
DXGK_QUERYSEGMENTOUT4 identifies the one-based segment. M66's rejected
segment1 was local memory without Aperture, not the current segment2.

## Procedure and acceptance

- Candidate0.7.130.1 SYS BB96304B7A6F4C84819871CD5EAA3C240BFD3EF3BC9C9D0C9CD8F4F145F71D64.
- Host KMD paging routing/segment tests and WDK build must pass. Updated segment
  assertion requires prepared aperture2; no-segment startup stays0.
- Unit A Windows,1000MHz/820mV, STOP/85C limits. Preserve129logs and M412D3D/M414RADV.
  One PnP disable/install/enable, no planned OS/DWM/AC reset.
- Keep current copied-private physical SDMA submission behavior. This isolates
  OS DMA addressability from native-IB routing. Existing six startup controls
  establish SDMA health, then Windows paging runs normally.
- Count nonzero/zero BuildPagingBuffer GPU addresses. First8nonzero buffer
  observations compare the paging logical walk of base+write-offset against
  MmGetPhysicalAddress(pDmaBuffer). This is CPU mapping identity evidence, not
  GPU fetch or cache-coherency proof. Record actual root/address and equality.
- Run current8shader/twoE14model content regressions; inspect true exits,
  loaded modules, all bytes/reference text, completion counters and TDR.
  Inspect actual SubmitCommandVirtual addresses as well as Build observations.

## Expected outcomes

Nonzero addresses with matching CPU physical identity support native OS IB
integration. If addresses stay0, preserve the observation, determine missing
mapping policy from local contracts/source and adapt the integration. Never
replace zero by a fabricated VA. A startup/content failure rejects promotion;
preserve evidence and recover via the established device/plug procedures.

## Result

M420 provides nonzero OS DMA VA and passing workload controls. Detailed
CPU-PA probe results wrapped before collection, so that comparison remains
unproved. See [result and limits](../../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07130-paging-dma-va/RESULT.md).
Full M9 and native OS IB execution remain unproved.
