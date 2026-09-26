# SDMA indirect submission groundwork for native GPU-VA paging

Hypothesis: the existing SDMA ring can encode AMD's VMID-selected INDIRECT
packet and its outer completion fence without CPU page capture. This host
stage proves packet/submission construction only, not GPU translation.
Current BuildPagingBuffer still uses captured physical plans and may allocate
on reservation exhaustion; it remains unchanged during this stage.

Use the imported Linux v6.18 sdma_v5_0_ring_emit_ib function as an executable
packet oracle. Compare all VMIDs, ring positions including wrap, multiple IB
sizes and CSA values. Check the outer fence and byte-valued doorbell publication.
A forced-VMID0 mutation must fail the comparisons. Keep KMD and lab unchanged.
Build the modified shim with the existing host toolchain and full WDK DEV build.

Before runtime use: reserve an IB backing allocation with the submitting owner,
retain it until an actual fence; assign a paging VMID distinct from graphics,
program/flush the paging root in GPU order, ensure the IB itself is mapped in
that VMID, define CSA backing/preemption, and check local/system copy results.
Do not infer translated copies from packet agreement or a fence alone. Start
with known-pattern physical/VMID0 control, then nonzero-VMID aliases and negative
mapping controls. No production routing switch until those controls pass.

PROVENANCE: AMD Linux amdgpu v6.18 imported reference, MIT.
