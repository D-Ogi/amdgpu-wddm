# Logical page-table construction state groundwork

2026-09-23, HEAD bed764d plus current uncommitted source. Host-only; no lab access.

Microsoft primary references read2026-09-23:
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/examples
- https://learn.microsoft.com/en-us/windows-hardware/drivers/display/system-paging-process
- https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/ns-d3dkmddi-_dxgk_buildpagingbuffer_updatepagetable

The documented transfer sequence builds source/destination scratch-area PTE
updates, TLB invalidation and a virtual transfer before submitting the buffer.
Fill follows the same ordering. Paging-process initialization is immediate on
CPU; this does not make later scratch mappings immutable. Thus M191 represents
a documented construction dependency, not a reason to assume GPU execution has
already updated a CPU-visible table. No corresponding new lab trace was taken.

New paging_pt_shadow.c/h retains encoded logical entries in caller-owned storage.
Physical table identity uses an open-addressed hash table; partial initialization
uses per-entry Known bits so a known zero differs from an absent entry. There are
no allocations or GPU-memory writes in Apply/Read. Caller must serialize, own
storage, fully validate encoding and publish only accepted command batches.
No deletion: intended only for pinned paging-process tables until instance reset.
App tables must not be registered accidentally. Missing/full/invalid statuses are
internal and MUST NOT be forwarded directly as BuildPagingBuffer return statuses.

544 host checks pass: unknown vs zero, partial512-entry update split480+32,
collisions, missing registration, full capacity, no partial mutation on bad
input, existing-table update at capacity, reset and48-bit address bounds.
Kernel-flag compilation and full build/sign pass. Dev package retains0773 and is
NOT FOR DEPLOYMENT: scratch/build/paging-pt-shadow-dev. SYS SHA256:
0BC9CB4AC023A5461326F9559A39FBDD86CCA3F2D53A8455624AC5C4ADF9320E.
Official installed0773 remains unchanged; no runtime path calls this module yet.

Integration requirements:
1. Reserve/size storage before publishing WDDM readiness; fail StartDevice on
   allocation failure. Capacity must cover the advertised paging VA hierarchy.
2. Register actual pinned paging-process tables during CPU_VIRTUAL initialization
   with trustworthy physical identity and initialization coverage. Handle D0/reset.
3. Commit logical GPU updates only for successfully accepted command batches,
   including multipass and private/DMA capacity; no advance on rejected buffers.
4. Resolve transfer addresses from logical state; never overwrite live GPU PTEs
   merely to expose future mappings to the builder. Account for copy-PTE ops.
5. Test the complete DDI sequence, batch failure and lifetime, then hardware.
6. M190 cache-alias policy remains separate; the helper itself maps no VRAM and
   does not alone remove the current retained NC mapping or bootstrap CPU writes.
