# SDMA GART invalidation command construction

2026-09-23. Local source/host tests only. Upstream Linux amdgpu MIT at
7d0a66e4bb9081d75c82ec4957c50034cb0ea449 (local ref/linux-src HEAD verified).
sdma_v5_0_set_ring_funcs selects GFXHUB for SDMA; gmc_v10_0_use_invalidate_semaphore
only selects MMHUB. amdgpu_gmc_allocate_vm_inv_eng starts its ring pool at engine0,
excludes firmware2/3, and reserves17 for CPU GART flushes.

New bc250_sdma_paging_invalidate_gart builds15DW (16DW reservation): SRBM_WRITE
request, POLL_REGMEM request with zero mask for ACK reset cycle, then ACK bit0 poll.
Upstream packet encodings/retry settings are preserved. Engine0 is explicitly
reserved for the planned SDMA0 paging stream; existing CPU flushes use17. Register
IDs and request values come from the initialized GFXHUB descriptor and AMD encoder.
No MMIO, semaphore reads or table mutation happens during construction. VMID0 root
is unchanged, a deliberate difference from upstream's per-process root-switch helper.
This helper is not called by runtime code; the reservation is a design constraint
for integration, not a new runtime resource allocator.

Packet suite passes122checks including existing copy/fill/PTE controls. Added checks
cover exact write/read/ACK word order, request callback arguments, reserved capacity,
untouched tail, every undersized buffer, uninitialized hub, poll address overflow and
SRBM register-field overflow. Tests use synthetic register IDs, not hardware offsets.
This verifies encoding/ordering, not that the GPU acknowledges an invalidation or
honors PTE-write/cache/transfer dependencies. The eventual combined sequence still
requires upstream-based memory pipeline synchronization and hardware positive control.

Full KMD compile/link/sign passes in scratch/build/paging-tlb-dev, retained0.7.65.1
(development output, not release/deployment). SYS SHA256
37CE6992F1B16A4E466A2B4CD0169FC25ADEA9D5FB9B47356179EFB30AF7ACAA.
No lab access. System RAM paging remains unsupported. No redactions.
