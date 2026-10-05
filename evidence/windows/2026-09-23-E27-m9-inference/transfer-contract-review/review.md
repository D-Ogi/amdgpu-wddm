# M225: paging contract scope review

Date: 2026-09-23. Source snapshots and SHA-256 hashes accompany this note.
Documentation repository revision: 7515063cea4c9e98db6a92986c5b4ddb0463fd16.
No driver changes, tests, deployment, lab access or GPU measurements.

## Findings and limits

- BuildPagingBuffer remarks enumerate within-segment, between-segment and
  segment/system transfers. They do not enumerate system/system transfers.
  This omission alone is not an explicit prohibition.
- The argument reference independently allows SegmentId zero for Source and
  Destination. Neither inspected page establishes a non-overlap guarantee,
  distinct PFNs, or a ban on both endpoints being MDLs. Arbitrary cross-page
  alias handling remains unresolved; do not remove the M224 guard on this basis.
- Fill remarks explicitly state that VidMm never requests an aperture-segment
  fill. Aperture Fill is therefore not a missing required operation under this
  documented contract. Map/unmap aperture support remains a separate requirement.
- TransferEnd-before-next-TransferStart orders transfer requests. It does not
  establish GPU completion or retirement of the private staging page.
- AllocationIsIdle guarantees allocation idleness for the duration of the
  BuildPagingBuffer call. It is not a guarantee of whole-engine idleness or of
  staging-resource retirement after the callback.
- The documented return set remains SUCCESS, ALLOCATION_BUSY and
  INSUFFICIENT_DMA_BUFFER. Temporary INVALID_PARAMETER refusals must not be
  described as complete DDI compliance.

## Next implementation priorities

Preserve the unresolved multi-MDL restriction as an explicit coverage gap.
Prioritize virtual Transfer table-write consistency and required aperture map/
unmap behavior; neither depends on proving arbitrary MDL pairs impossible.
Track source lifetime, cache coherency, staging retirement and restricted DDI
return handling separately from host packet-construction correctness.
