# SDMA INDIRECT positive control

Plan written 2026-09-24 before candidate128 deployment.

Hypothesis: SDMA0 can fetch a retained GTT indirect buffer at VMID0 and execute
its fill/copy payload with the same complete output as direct-ring commands.
This does not yet establish nonzero-VMID translation or change OS paging.

## Procedure

- Candidate0.7.128.1 SYS SHA256
  98931C11FAF37DE35A230022839C5B68C69A6A1999CD39438987F2BD20DA4897.
- Retain Mesa main D3D/LLVM23.1.2 and RADV main from M412/M414. Use Windows,
  1000MHz/820mV, STOP and85C limit. Preserve current127 logs before transition.
- Host actual-source coordinator20scenarios and AMD packet oracle18474checks
  must pass, then install through one controlled PnP disable/install/enable.
  Do not reboot Windows routinely. Preserve display gates and restore the
  exact current D3D registration after INF installation, before enabling.
- Opt-in EnableSdmaIbControl=1 runs after engine initialization, before WDDM
  publication. External full-WDDM diagnostic escapes remain blocked. Run
  direct4096, indirect4096, indirect65536, direct65536, stopping at first failure.
- Each trial seeds source with byte offset, destination0xEE; GPU fills source
  0xA5 then copies it. Require a fresh outer fence and compare every byte.
  IB uses existing AMD fill/copy emitters,8DWORD payload padding, VMID0/CSA0.
  IB and scratch stay owned through confirmed engine retirement; timeout
  does not free or permit rewriting an unfinished IB.
- Collect persisted startup log, loaded version/hash, live counters, DWM
  module/health and a scanout. Clear the optional test gate after collection.
  Restore DWM only if needed. Run the accepted current-Mesa content controls
  after the new KMD starts, checking native exit and reference outputs.

## Expected outcomes

All four fences and complete byte comparisons establish VMID0 execution.
A compiler/build failure is host evidence only. A timeout, wrong content or
backend fault rejects the control and stops startup; preserve its last log
before recovery. Never infer VMID translation from VMID0 or fence alone.
Any new hardware result gets a new immutable directory and facts entry.

## Result

M417 passes all four hardware controls and subsequent shader/model regressions.
See [immutable results](../../../evidence/windows/2026-09-24-E27-m9-recovery/candidate07128-sdma-ib-complete/RESULT.md).
OS-owned DMA ranges, nonzero VMID/root/CSA policy and M9 resource/cache/lifetime
acceptance remain subsequent integration work.
