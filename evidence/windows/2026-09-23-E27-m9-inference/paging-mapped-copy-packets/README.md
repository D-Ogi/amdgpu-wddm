# Combined mapped-copy packet construction

2026-09-23. Source/host only, no lab access. Linux amdgpu MIT reference revision
7d0a66e4bb9081d75c82ec4957c50034cb0ea449. SDMA5 ring_emit_pipeline_sync polls the
preceding fence's memory slot; ring_emit_fence is already in the shim. VM SDMA
updates commit through job fences (amdgpu_vm_sdma_commit), not just a CPU write order.

New mapped-copy constructor emits map PTEs, fence+memory poll, GART invalidate,
COPY_LINEAR, fence+memory poll, zero PTEs, fence+memory poll, GART invalidate.
Three distinct consecutive marker values avoid one phase observing another phase's
value. Caller must ensure freshness versus old batches and retain scratch memory
outside the temporary mapping window. Noninterrupting markers are distinct from the
final OS completion fence, which the eventual submit path must append separately.
The packet constructor does not allocate a GART window, pin pages, or reset scratch.

The whole transaction is validated/reserved before writing any output. Shared PTE
emitter handles explicit mappings and zero cleanup. For two pages:83DWORDs,96DWORD
aligned reservation. Existing copy/fill/PTE/flush controls plus transaction checks
now total335passing assertions. Tests verify exact map/barrier/TLB/copy/cleanup
packet offsets, addresses, distinct values, no interrupting phase fence, final
invalidation, trailing guards, every capacity0..95, token wrap/zero and misalignment.
These are packet/ordering checks, not a GPU memory-coherency or timeout model.

Full KMD compile/sign passes, development output scratch/build/paging-mapped-copy-dev
retains0.7.65.1 (not a new release or deployment). SYS SHA256
3294E8FBED1461EF21459ECDA5415D1B5ACEB285AAC50ABFEB881356902E34D1.
No runtime calls yet. Still required: reserve/bound window and scratch lifetime,
construct PTE flags from AMD, integrate system translations and multipass budgeting,
append real final completion fence, handle timeout without reusing mappings/pages,
and validate actual GPU/cache behavior. RAM paging remains unsupported.
No redactions.
