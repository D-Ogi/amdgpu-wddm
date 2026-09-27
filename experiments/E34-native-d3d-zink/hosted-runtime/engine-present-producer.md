# Diagnostic GPU Present producer

The default-off EnableGpuPresentBlit gate selects a typed LB7A producer in
DxgkDdiPresent. Source and destination are entries1 and2 of the measured WDDM2
DXGK_PRESENTALLOCATIONINFO layout. Both opened objects must belong to the context's
DDI device, resolve through paired AcquireHandleData/ReleaseHandleData to live CreateAllocation objects, and
match those objects' surface metadata. Different opened handles alone are not a
non-aliasing proof: the underlying allocation identities must differ. Both descriptors are now copied
under one list lock, so no object pointer escapes into command construction. This KMD
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
produces an acquire plus NOP IB with ordinary hardware completion. Exhausted remaining DMA/private capacity requests buffer rotation without publishing a record.
Malformed pointers or alignment fail separately (M637).

BC2C UMD contexts are rejected by both GPU Present producer and consumer until
source and destination residency on their exact submitting device is established.
Their allocation-list sizing remains unchanged. No interop capability is advertised. Runtime
allocation admission, complete standard-GDI allocation policy,
CPU/GPU producer visibility, lifetime and scheduling across contexts, multipass
callbacks, private-data delivery, and the exact signed build still require lab
validation. BC2A allocations have no validated linear-surface format metadata
here and are rejected. CDD staging support is not established by this code.
These are remaining G0 requirements, not reasons to declare the current subset
complete. Do not enable the gate as a substitute for the CDD/DWM interop contract.

The main development tree contains runtime changes beyond deployed KMD153.
Any lab candidate must be reconciled against that exact baseline in an isolated
source tree and assigned its own artifact identity before deployment.

Each IB now starts with the eight-DWORD GFX10 ACQUIRE_MEM sequence from the
imported gfx_v10_0_emit_mem_sync. The minimum capacity is64 bytes (16 DWORDs),
leaving room for acquire, at least one copy packet, and alignment padding.
MultipassOffset still counts copy packets only. The production wrapper is
host-tested with independent pixel decoding at seven aligned capacities.
Cache invalidation does not establish producer completion or residency: WDDM2
Present allocation lists do not make allocations resident. Exact source and
destination residency on the submitting device remain an admission obligation.

At each Present callback entry the recognition word is cleared before early
returns or alternate paths can reuse a stale BGP1. Submit never clears it, so
scheduler resubmission retains the record. DestroyAllocation clears backing
snapshots in all opened objects under the adapter list lock before freeing the
allocation. These measures prevent stale recognition and pool-address reuse;
they do not establish residency, OS handle resolution or DDI lifetime guarantees.

M632 validates13 acquired bindings on159; M633 validates shared imports and
normal owner exit on CPU/hosted GPU. Neither executes BGP1. M634 corrects
standard-GDI CPU visibility and refuses unimplemented ownership types; that
source change is not deployed. Interop capability, CDD device residency and
actual Blt admission remain the next runtime prerequisites.

M638 source inspection corrects the assumption that a UMD-created DWM texture
must be BC2A. The current frontend creates shared/presentable surfaces as typed
LB7A and imports that runtime allocation into Zink. Keep BC2A rejection; first
identify the actual CDD destination and context in the bounded interop capture.
A BC2A texture ABI extension is not established as a prerequisite for that path.
UMD-device MakeResident still does not prove CDD-device residency.
