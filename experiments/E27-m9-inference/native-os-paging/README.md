# Native OS paging IB experiment

Plan written 2026-09-24 before candidate131 deployment.

Hypothesis: ordinary system-context TransferVirtual/FillVirtual can execute
from the OS DMA buffer through SDMA VMID2 with GPU-ordered root/TLB/IB/fence,
without allocating or retaining a per-operation physical capture graph.

Candidate0.7.131.1 SYS SHA256 1F218318C57D4BC46D976B1AB4D43B659DB62233022FD1B9C0687EB1FEF9533B.
Source includes typed native private metadata, bounded multi-buffer encoder,
ordinary allocation/data classification, disjoint source/destination physical
bounds, permission-aware logical walk and CPU/GPU DMA physical identity checks.
IB payload and aligned64-byte CSA are separate ranges in retained OS DMA.
Page-table targets, aliases and unavailable native mappings keep the existing
physical path. This does not close the general alias/resource/lifetime audit.

## Procedure and acceptance

- Host extracted-route tests decode every packet across more than4GiB, varying
  offsets and budgets. Real page-table walkers exercise publication, readonly
  CSA, differing CPU/GPU backing, table/alias and private-space cases.
  Full WDK26100 build must pass. Current875844 checks pass.
- Local MS sources: staging110f60ea system-paging-process.md and local enriched
  DDI BuildPagingBuffer, FillVirtual, TransferVirtual, SubmitCommandVirtual.
  No new online contract assumption. Page permissions come from AMD PTE decoder.
- Unit A Windows,1000MHz/820mV, STOP/85C checks. Preserve130 logs, M412 desktop
  and M414 RADV. One PnP transition; no planned OS/DWM/AC reset.
- Existing six startup controls precede OS publication. Observe matched CPU/GPU
  DMA addresses and native transfer/fill counters. Nonzero address or successful
  build alone is not evidence that native work executed.
- Run eight shader content controls and both E14 model references through the
  current RADV launcher. Verify worker/native exits, loaded ICD, GPU offload,
  full model output, fences/errors/TDR and unchanged OS/DWM identity.
- Native counters must increase and all corresponding paging fences retire;
  zero native counts mean the new route was not exercised, even if regressions
  pass. Inspect eligibility/mapping rather than reporting native acceptance.
- Preserve raw logs and source hashes. On fault retain evidence, inspect live
  state and use the existing recovery procedure; no speculative repeated start.

## Additional residency control (planned before execution)

After model controls, run the existing gpu-residency0798-v4 probe
(E9566E495A4FB5BB0F37B8A2583AB10F573D0F9ECEE2B84D94E264242529E9C3)
on1GiB VRAM. This independently checks GPU readback across three explicit
eviction/restoration cycles. Check every reported byte, native exit, paging
native counters and fences; no device reset between the model and residency runs.
The probe uses the existing direct UMD interface, not RADV.

## Expected outcomes

Native counts plus complete fences/content controls support this positive
ordinary path. They do not prove forced preemption/CSA context restoration,
all alias cases, all PFN/cache ownership, or the full startup failure lifecycle.
Full M9 remains open until its acceptance index requirements are individually met.

## Result

M422 passes native model/shader and mixed1GiB residency controls. Native
transfer count stays21during residency; do not claim those large transfers
used the new path. See [full result and limits](../../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07131-native-os-paging/RESULT.md).
