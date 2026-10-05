# M419 - SDMA VMID2 translation and same-VA remap

Date:2026-09-24. Unit A, Windows,1000MHz/820mV.

## Result

Candidate0.7.129.1 executes a retained system-backed SDMA INDIRECT buffer
through VMID2. GPU packets assign the local physical root, invalidate its TLB,
then fetch the IB and signal an outer fence. The first trial copies a VRAM
pattern. After its fence, CPU remaps the same source VA to a differently
patterned system page; the second trial returns the new pattern. Both4096-byte
destination comparisons pass, including2048copied bytes and2048fill bytes.
Fences5/5 and6/6 arrive. Four preceding VMID0 direct/IB controls pass too.

The source VA0x40000000 is distinct from all relevant physical addresses:
root0x46E02C000, IB0x26FF39000, system data0x26FF18000 and VRAM source0x46E00C000.
CSA is a zeroed, mapped, retained system page at VA0x40003000. The trial does
not force preemption. Root/PDE/local-leaf addresses use AMD MC-to-PA conversion.

Host executable AMD-reference comparison:25194checks,0failures. Omitting the
VM flush in a copied test source yields960failures, as expected. Actual-source
startup coordinator:21scenarios,0failures, including VA-control-only admission.
WDK build/signing succeeds. Source and source hashes accompany these logs.

## Runtime regression and retained state

One PnP disable/install/enable at16:03, no OS/DWM/AC restart. Loaded SYS SHA256:
C4DC9A5870C0EB2D349CAB522077338B2DAC814C7A85661C71458D232AD36AE7.
Windows boot11:44:14 and DWM4448/start13:58:18 remain through16:06:22.
M412 D3D D438EA42314A3ADD6817665A6133628A64404B5282FB82227230199DA22F768D
and M414 RADV DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986
remain the active modules. Desktop rendering remains CPU llvmpipe/LLVM23.1.2.

All8shader CPU hashes match, including MLP argmax; stories15M7/7 and
TinyLlama23/23offload output exactly matches E14 after CR normalization.
Each native exit and worker exit is0. Final GFX2354/2354, SDMA22319/22319,
zero timeout/refusal and noTDR. Full gate0 is consumed one-shot state,
guard0, both optional SDMA control gates0, display gates1. Final temperature66.9C.
Scanout was captured privately; no new visual/input acceptance is claimed.

## Harness anomalies and limits

The earlier sdma-va129-build.log belongs to an incorrectly sequenced build:
the edit script stopped before writing KMD changes, but its caller continued
building old revision128 into a129-named directory. That artifact was not
deployed. sdma-va129-build2.log is the accepted129build in bc250kmd-07129-v2.
The pre-run source review also corrected fill-byte replication into a32-bit
SDMA fill pattern; no faulty-pattern hardware trial occurred.

The regression control wrapper's first command had a mangled UTF-8 BOM and
failed to set ErrorActionPreference. The shell continued. Its raw log retains
that diagnostic in the original non-UTF-8 bytes. Do not call the wrapper clean
merely because SSH returned0. Acceptance here independently checks all native
exit files,8CPU hashes, full model outputs, actual ICD witnesses and final
hardware counters in validation.json. Future script generation must decode
UTF-8 BOM with utf-8-sig and emit BOM-free ASCII or PowerShell-compatible UTF-8.

The first evidence collection stopped on that raw log's encoding; its partial
directory is retained at ../candidate07129-sdma-va with an explanation. This
complete collection preserves raw host/SSH log bytes, redacting only PCI
instance/interface identifiers. Artifact text logs are decoded from their
recorded UTF-16/UTF-8 encoding with original line endings retained.
No hardware workload was repeated to repair collection.

Ordinary WDDM paging still uses its prior captured physical commands. This is
owned-page translation/remap evidence, not OS-owned DMA IB integration, a
GPU-written PTE-remap test, general cache/PFN ownership, preemption acceptance,
or complete M9. VMID2 is now excluded from graphics diagnostic submission.
The next integration must preserve mixed physical/virtual command order and
supply a valid CSA policy for the OS system paging address space.
