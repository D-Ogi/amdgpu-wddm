# Diagnostic GPU Present producer

The default-off EnableGpuPresentBlit gate selects a typed LB7A producer in
DxgkDdiPresent. Source and destination are entries1 and2 of the measured WDDM2
DXGK_PRESENTALLOCATIONINFO layout. Both opened objects must belong to the context's
DDI device, resolve through GetHandleData to live CreateAllocation objects, and
match those objects' surface metadata. Different opened handles alone are not a
non-aliasing proof: the underlying allocation identities must differ. This KMD
clears ExistingSysMem/ExistingKernelSysMem for these VidMm-managed allocations.
Unknown callback results are retained for CPU compatibility but rejected here.

The initial producer accepts equal supported32-bit linear formats. It checks VA
alignment/range, physical-adapter index and disjointness from the command buffer.
The list builder checks complete source/destination geometry, disjoint GPU spans
and all dirty rectangles before writing. Dirty rectangles are copied into a
bounded-size allocation for a stable per-call snapshot; no pixel data is copied
by this producer. This snapshot is released before returning from Present.

One BGP1 record describes one complete aligned OS DMA buffer. Remaining dwords
are filled with the imported CP NOP encoding, preventing another Present record
from being appended to the same IB. The private record is published only after
command construction and a memory barrier. MultipassOffset is a packet ordinal;
More becomes STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER. An empty list intersection
produces a NOP-only IB with ordinary hardware completion. Small or unaligned DMA
capacity is returned as insufficient without publishing a record.

With the gate enabled, UMD contexts request a Present allocation list as well as
the existing BC2S private capacity. No interop capability is advertised. Runtime
allocation admission, authoritative GetHandleData identity for CDD/shared opens,
CPU/GPU producer visibility, lifetime and scheduling across contexts, multipass
callbacks, private-data delivery, and the exact signed build still require lab
validation. BC2A allocations have no validated linear-surface format metadata
here and are rejected. CDD staging support is not established by this code.
These are remaining G0 requirements, not reasons to declare the current subset
complete. Do not enable the gate as a substitute for the CDD/DWM interop contract.

The main development tree contains runtime changes beyond deployed KMD153.
Any lab candidate must be reconciled against that exact baseline in an isolated
source tree and assigned its own artifact identity before deployment.
