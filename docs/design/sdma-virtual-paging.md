# SDMA GPU-VA paging path

Status: proposed integration, with packet/submission groundwork verified on
host in [M416](../../evidence/windows/2026-09-24-E27-m9-recovery/sdma-virtual-ib/RESULT.md).
M417 adds [VMID0 hardware controls](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07128-sdma-ib-complete/RESULT.md)
on128: direct/indirect4KiB/64KiB data and fresh fences pass before WDDM
publication. No runtime paging switch: deployed128 ordinary paging remains
physical SDMA commands generated from captured page identities.

M419 adds [VMID2 hardware evidence](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07129-sdma-va-complete/RESULT.md):
GPU-ordered root/TLB/IB, system-backed commands, mapped CSA and distinct
VRAM/system patterns at the same source VA pass. CPU remaps the owned PTE
between real fences. This does not test GPU-written PTE updates or preemption.
OS paging remains on the original physical path.

M420 selects aperture2 for OS paging DMA and observes nonzero addresses with
passing shader/model regressions. M421 supplies a typed64-byte private record
and mixed ring consumer; host tests and WDK build pass. The native builder is
not yet implemented and the M421 DEV image is not deployed. See
[M420](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07130-paging-dma-va/RESULT.md)
and [M421](../../evidence/windows/2026-09-24-E27-m9-recovery/paging-native-records/RESULT.md).

## Required outcome

Ordinary TransferVirtual/FillVirtual should consume GPU virtual addresses in
the paging context and rely on ordered GPU page-table updates. Their builder
should emit bounded command bytes without a per-operation page-graph allocation.
This can remove the ordinary path's allocation failure and large capture cost;
it does not automatically solve table-copy aliasing or every transfer dependency.
Do not silently substitute empty work for the existing capture path.

## Established inputs

- AMD sdma_v5_0_ring_emit_ib selects VMID on INDIRECT, carrying an IB address,
  length and CSA. M416 compares the exact reference function and verifies the
  host ring commit. Nonzero-VMID memory operations still need hardware proof.
- Local Microsoft ref/ddi-display/d3dkmddi.md, DXGKARG_BUILDPAGINGBUFFER,
  DmaBufferGpuVirtualAddress, and DXGKARG_SUBMITCOMMANDVIRTUAL,
  DmaBufferVirtualAddress describe the OS DMA buffer's GPU address.
- WddmPublishPagingRecordCore already writes command bytes into that DMA buffer.
  Private records identify exact accepted ranges and are retained with submission.
  The OS buffer is a candidate IB backing; validate address/context and lifetime
  instead of inventing another allocating per-operation IB pool.
- Current node1 copies private physical commands directly into SDMA0 and appends
  a fence. Its interrupt/DPC completion and one-in-flight ownership stay useful.
  Graphics currently changes VMID1 roots, so it cannot share a concurrently active
  paging VMID without explicit serialization.

Contract review also uses the local Microsoft conceptual guide at workspace
`ref/windows-driver-docs/windows-driver-docs-pr/display/tile-resources.md`,
staging `110f60eaf2ac5836e644d320c1e92c1011f2af5e`
([public source](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/tile-resources)).
It distinguishes shared-system paging, where VidMm protects referenced pages
until the paging work completes, from companion-context tile mappings that may
wait indefinitely and require CopyPageTableEntries to resolve current backing
at execution. CPU_VIRTUAL uses a separate deferred UpdatePageTable path.
Do not generalize ordinary transfer capture guarantees to future sparse mapping
work, or declare the current CPU_VIRTUAL path defective from the companion-context
requirements alone. See [M12.1](sparse-wddm.md) for that integration boundary.

## Next implementation and positive controls

1. Done for VMID0 in M417. Exercise the new INDIRECT path using an owned, retained diagnostic IB at
   VMID0. Place distinctive fill/copy commands inside it, verify every output
   byte and the outer fence. Existing direct-ring control is the comparison.
2. Reserve a distinct paging VMID in both normal submission and diagnostic
   policies. Initialize its root, limits, invalidation engine and ownership
   from the existing AMD helpers. Put root/TLB changes in GPU command order;
   do not introduce long DISPATCH_LEVEL CPU polling.
3. Establish a valid nonzero-VMID IB mapping and CSA/preemption policy. In Linux,
   amdgpu_sdma_get_csa_mc_addr returns0 for VMID0 or disabled gfx.mcbp; that rule
   is not proof that any arbitrary nonzero-VMID Windows submission can use0.
   A known-pattern alias control must distinguish VA translation from accidentally
   accessing the same MC address. Include system-backed pages and a later mapping.
4. Preserve physical PTE/GART operations in VMID0. Introduce an explicit record
   kind for virtual copy/fill IB ranges, rather than interpreting the entire mixed
   paging buffer in the new VMID. Honor32-byte IB base alignment, packet size,
   padding and OS-reported exact subranges. Validate that every emitted IB range
   is inside its retained DMA buffer and mapped through the captured context root.
5. Route ordinary supported virtual work through those IB ranges only after the
   controls pass. Preserve immediate CPU_VIRTUAL table initialization and logical
   publication semantics. Hardware fences, not timeout or buffer construction,
   release execution ownership. Stop/device-generation code retains resources
   until retirement is established. Then test actual residency and inference.

The scope includes overlap/table hazards and unsupported physical delivery;
those cannot be declared solved by testing disjoint copy/fill. Existing graph
handling remains until an explicit replacement is proved. A current main Mesa
upgrade and working model outputs do not close this DMA resource contract.

## OS integration constraints identified after M419

The local Microsoft `display/system-paging-process.md` (same110f60ea snapshot)
identifies OS-owned paging DMA buffers and the paging process address space.
Use its CPU_VIRTUAL initialization contract; the private diagnostic root from
M419 is not a substitute for the OS root or its memory ownership.

Local `ref/ddi-display/d3dkmddi.md`, DXGKCB_CREATECONTEXTALLOCATION, explicitly
limits GPU context allocations to non-system contexts. Therefore do not use
that callback as the system paging CSA solution. Establish valid backing and
mapping for the system-context path before enabling native IB submission.
An embedded, aligned CSA region in the retained OS DMA allocation is a candidate
requiring proof; a zero CSA is not justified by the VMID0 control. Keep CSA and
IB payload outside each other's ranges and retain both until actual completion.

Typed private records must distinguish direct VMID0 packets from VMID2 IB
ranges. Validate all selected ranges before ring publication, preserve exact
OS submission boundaries, and keep root/TLB/IB/fence ordering GPU-side.
The queue must retain the OS DMA execution lifetime while software may retain
only copied private metadata. Removing capture graphs must not bypass logical
page-table publication or silently weaken alias/overlap semantics.


## M422 - Native ordinary OS integration measured

[Candidate131](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07131-native-os-paging/RESULT.md) implements steps4-5 for admitted system-context
ordinary copy/fill, with per-page DMA identity and GPU permission checks.
Native21transfers/89fills execute alongside physical table/alias commands,
with matching shader/model and1GiB mixed-path readback. CSA storage is mapped
and writable; forced preemption/restoration is still unproved. Large residency
transfers remain on the physical path; diagnose admission before removing
capture resources. Logical page-table publication and full M9 gates remain.


### M423 - Fragmented native DMA admission fixed

[Candidate132](../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07132-exact-dma-disjoint/RESULT.md) checks actual pages when coarse DMA/data bounds
overlap. It admits1GiB residency transfers natively, with0capture plans in
64MiB/model/1GiB controls, correct full GPU readbacks and noTDR. This supersedes
M422's measured large-transfer exclusion. Preallocated arenas and physical
fallback still exist; complete resource/cache/lifetime/startup gates stay open.
