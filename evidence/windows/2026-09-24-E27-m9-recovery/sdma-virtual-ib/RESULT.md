# M416 - SDMA indirect submission source and host controls

Source/host only, 2026-09-24. No lab operation, no KMD deployment. Current
hardware remains the accepted127/M412 desktop and M414-M415 Vulkan baseline.
PROVENANCE: imported AMD Linux v6.18 sdma_v5_0.c, MIT.

Added bc250_sdma_ib_size, bc250_sdma_emit_ib and bc250_sdma_submit_ib to the
existing shim. The INDIRECT packet carries the selected VMID, aligned IB GPU
address, DWORD length and caller-owned CSA address. Position-dependent NOPs
make its end an8DWORD boundary. The submission reserves the complete IB,
outer fence and commit padding, then publishes one byte-valued SDMA doorbell.
This is not connected to the production WDDM paging route yet.

The host generator extracts the actual sdma_v5_0_ring_emit_ib body from the
imported reference. Its test adapter supplies job/IB types, the requested CSA,
and single-word NOP mode. Packet generation itself is not reimplemented as
an expected-value loop. Tests span all16VMIDs,32start positions (ordinary,
ring-wrap and above2^32), four sizes up to the packet field limit, CSA0/nonzero,
outer fence ordering, doorbell units and exact reservation coverage.

First compilation exposed missing RLC lifecycle symbols in the old copy-packet
harness, after earlier SDMA lifecycle changes. Added aborting stubs, as the
paging packet harness already uses; RLC is not executed or validated here.
Initial packet comparisons passed16426checks. An additional reservation check
then failed896cases: final commit padding could exceed the reservation from
an unaligned starting pointer. Include that exact padding before reserving.
The final18474checks pass, zero failures. A separate copied-source mutation
forcing VMID0 fails3840checks. Original production source is not mutated.
Full WDK26100 DEV build/sign succeeds after the final fix. Exact source and
artifact hashes are in sources.json. Development SYS retains version127 but
has a different hash and was NOT installed; do not confuse it with the lab SYS.

Local Microsoft d3dkmddi.md describes BuildPagingBuffer.DmaBufferGpuVirtualAddress
as the address where the DMA buffer was paged in, and SubmitCommandVirtual's
DmaBufferVirtualAddress as the buffer address in the submitting process context.
The existing WddmPublishPagingRecordCore already copies actual command bytes
into the OS DMA buffer. This provides a candidate IB owner without allocating
a new per-operation driver pool. It does not by itself prove SDMA fetch,
translation, mapping lifetime or alignment on live WDDM submissions.

Before production routing changes, require a GPU content control: VMID0 IB
fetch first, then a distinct paging VMID with known-pattern local/system data,
root/TLB setup and an actual completion fence. Physical PTE/GART commands must
remain in their physical address namespace; do not reinterpret an entire mixed
buffer under a nonzero VMID. Define IB alignment, OS-buffer and context/root
retention, CSA/preemption, translation hazards and stop/generation ownership.
See the linked design for the next integration step. Current captured-plan
fallback allocation and all remaining M9 acceptance requirements stay open.
