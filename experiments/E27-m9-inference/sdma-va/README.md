# SDMA translated indirect-buffer control

Plan written 2026-09-24 before candidate129 deployment.

Hypothesis: SDMA0 can execute an IB through a dedicated VMID2 with a GPU-ordered
root assignment and TLB invalidation, fetching system-backed commands and copying
from a VRAM source, then from a system page remapped at the same source VA.

## Procedure

- Candidate0.7.129.1 SYS C4DC9A5870C0EB2D349CAB522077338B2DAC814C7A85661C71458D232AD36AE7.
  Windows unit A, 1000MHz/820mV, STOP flag and85C limit. Preserve M412 desktop
  and M414 RADV. Single PnP disable/install/enable, no planned OS/AC reset.
- Host packet oracle uses executable AMD Linux v6.18 emitters for root/TLB/IB
  and fence order:25194 checks. Missing-VM-flush mutation must fail. Actual-source
  startup coordinator must invoke the optional VA control before OS publication.
- EnableSdmaVaControl admits four prior VMID0 direct/IB controls, then VMID2.
  Do not expose a live diagnostic escape beside OS work. VMID2 is reserved from
  graphics submissions. Retain the existing engine-retirement ownership policy.
- Four VRAM pages contain the known four-level page tables. Root/PDE/local PTE
  addresses use amdgpu_gmc_vram_mc2pa. Three contiguous owned system pages hold
  sourceB, destination and a zeroed CSA. Retained GTT IB is separately mapped.
  Use VA0x40000000 deliberately distinct from physical and GART MC addresses.
- First map source VA to VRAM containing byte(i XOR0x5A). IB fills a4096-byte
  destination0x33, then copies2048source bytes. Require fresh outer fence and
  compare every destination byte. Change the source PTE only after that fence.
- Second map the same VA to system sourceB containing byte(i XOR0xC3); new
  GPU-ordered root/TLB/IB sequence fills destination0x66 then copies2048bytes.
  Require another fresh fence and all4096expected bytes. CPU initializes/remaps
  the PTE: this does not establish GPU-written PTE ordering or OS DMA integration.
- CSA is a retained mapped page, based on Linux's64-byte SDMA engine save area;
  this control does not force preemption and cannot accept preemption behavior.
- Preserve startup log, version/SYS/D3D hashes, DWM/boot state and temperatures.
  Clear optional gates after collection. Run current RADV shader and E14 model
  content regressions only after startup controls pass.

## Expected outcomes

Both full byte comparisons and fresh fences prove translated execution including
same-VA remapping for these owned pages. Fence alone, build success or VMID0
controls cannot prove it. Failure stops startup; preserve persisted logs and
recover with existing retirement/plug procedures. No uncompleted IB/page may be
rewritten or freed merely because a wait timed out.

OS-owned DMA ranges, general fragmented PFN/cache ownership and M9 resource
closure remain separate implementation and acceptance requirements.

## Result

M419 passes both translated byte oracles and subsequent shader/model controls.
See [results and disclosed harness issues](../../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07129-sdma-va-complete/RESULT.md).
The ordinary OS paging path is not changed by this control.
