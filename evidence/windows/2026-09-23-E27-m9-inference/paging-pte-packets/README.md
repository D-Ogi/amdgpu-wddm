# SDMA PTE packet construction, local groundwork

2026-09-23. No lab access or runtime path change. Source base HEAD bed764d with
captured worktree changes. New bc250_sdma_paging_write_ptes constructs AMD SDMA
WRITE_LINEAR commands for arbitrary64-bit page-entry arrays. Linux amdgpu MIT,
reference/sdma_v5_0.c:sdma_v5_0_vm_write_pte, v6.18 commit
7d0a66e4bb9081d75c82ec4957c50034cb0ea449. Deviation: explicit values instead of
value+increment so scattered OS pages can be represented. Zero entries permit cleanup.
The caller remains responsible for selecting a reserved table range and TLB ordering.

The existing packet harness now has64checks, all passing. Added golden command words,
scattered high/low halves, zero entry, untouched tail, capacities0..15 with aligned
reservation16, exact capacity, destination alignment/overflow, empty/excessive count,
NULL array and no writes on refusal. These checks verify packet construction only.
No PTE was written to GPU memory. Existing copy/fill controls remain in the same suite.

Full KMD compile/link/sign also passes at scratch/build/paging-pte-dev (development
output retaining version0.7.65.1; not a new release and not for deployment).
SYS DD008289F7A36A953CEDE0F29865D05908F0572FF88145A5BBB1B627FF7A49E4.
The new helper is not called by the runtime paging builder; system RAM transfer is
still unsupported. Required follow-up: reserve GART window, import/validate ordered
GPU TLB invalidation including semaphore/engine ownership, compose map/copy/unmap,
account complete command budget and validate on hardware before1GiB acceptance.
No redactions.
