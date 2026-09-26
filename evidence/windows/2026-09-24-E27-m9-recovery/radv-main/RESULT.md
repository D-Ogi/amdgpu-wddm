# M414-M415 - Current Mesa RADV/WDDM2 compute migration

Unit A, 2026-09-24. Mesa26.3.0-devel main
f333dd6d1c85297ac41773eaeb9b02f16acf1919 with the WDDM2 port and BC250 integration.
[Source, patch and build recipe](../../../../experiments/E27-m9-inference/radv-main/README.md).
PROVENANCE: Mesa upstream and Collabora lfrb/mesa, MIT.
The public lfrb/wddm2 branch still pointed to801c9763; its44commit delta from
b860e013 was rebased onto current main, then the existing BC250 changes applied.
No ready newer native port was found. No upstream MR or message was sent.

## Build and runtime changes

Adapt current physical/logical device ownership to the reference-counted Windows
winsys, current debug bitsets and command-state bits, compiler compatibility
configuration, acquire-mem emission and queue priority discovery. Keep current
Linux amdgpu backend semantics and current compiler/core code. The shared
Windows/runtime refactor also includes DZN source adaptations; DZN/Linux were
not built in this experiment. Consolidated patch reverse-check passes.
MSVC19.44.35221, Meson1.12.0, WDK26100, debugoptimized/MD, ACO; LLVM disabled
for RADV. The independent D3D desktop continues llvmpipe with LLVM23.1.2.

First candidate749EDF... built but control1 ended during loader startup, with
no completion marker. Windows Terminal crashed at14:34:08 (Application1000,
0xc0000005); task result0xC000013A, process absent. Observer was explicitly
stopped after collecting evidence. This is not a shader failure or pass.
Headless conhost removes that dependency in subsequent trials.

Control2 completed six CPU-matching shaders and all18submissions before an
MLP pipeline compiler assertion. Limited noninvasive stacks plus the exact
local PDB locate ac_nir_fixup_smem_loads_null_prt.c:103. Windows had advertised
sparse support without the newly required mirrored-VA PRT control bit.
Gate sparse when this hardware workaround is required but the VA policy is
absent. Do not invent a bit or remove the compiler assertion. Full sparse
support remains future mapping work. Terminated only stalled vkcompute1868;
exit-1/worker1 retained as failure. No hardware reset.

Accepted candidate DLL SHA256:
DB886B8D53E6BEE89665287AF5EE19A1868E5874868C795F6B11472FBB4A3986.
Installed separately at C:\BC250\m9\radv-main-icd2; selected per process.
Project launcher C:\BC250\m9\radv-main\run.cmd uses that manifest and the
existing Vulkan loader directory. Old ICDs remain available. System-wide ICD
registration and DWM registration are unchanged.

## Content controls: M414

Control3 returns native0 for the worker and all three applications. All8M8
shader GPU hashes equal CPU hashes; MLP argmax also matches. stories15M96tokens
(7/7layers) and TinyLlama64tokens (23/23layers) match the immutable E14 Linux
outputs after CR normalization. Actual DLL loader and BC250 submission witnesses
are retained; validation.json independently checks outputs locally.
Final control GFX12741/12741, SDMA45222/45222, zero timeouts/refusals, no TDR.
DWM4448 remains responsive at14:44:21, same start13:58:18 and boot11:44:14.

## Paired performance: M415

New then old ICD, fresh processes, same headless worker; b9564/3b3da01dc,
ngl99,t6,pp512,tg128,r3, shader cache disabled,1000MHz/820mV. KMD127 and
M412 desktop/overlay remain active. Exact application/model/KMD/ICD hashes
are in logs; all native applications and workers return0.

| Model/workload | New main tokens/s | Old WDDM2 tokens/s | Observed difference |
|---|---:|---:|---:|
| stories15M prompt |31766.21 +/-729.95|30004.56 +/-998.49|+5.87%|
| stories15M generation |471.30 +/-3.04|441.11 +/-12.67|+6.84%|
| TinyLlama prompt |1115.94 +/-7.46|1078.17 +/-9.28|+3.50%|
| TinyLlama generation |114.56 +/-0.44|111.38 +/-0.80|+2.86%|

The +/- values are the benchmark's reported sample standard deviations.
Raw JSON preserves three samples per row. This is one ordered pair, without
randomization or multiple paired sessions; do not generalize into a guaranteed
speedup. M413 had a different launcher and an observer anomaly; its results
remain separate. No matched new Linux measurement or Windows superiority claim.
The PowerShell summary printer emitted empty table rows because it did not
unroll the JSON array; raw native JSON is intact and parsed independently here.

New-ICD benchmark final GFX23106/23106, SDMA66651/66651; paired old final
GFX33471/33471, SDMA88076/88076; zero timeouts/refusals, no TDR. Capture plans
146reserved/0heap, peak1plan/5505328bytes (cumulative, not a universal bound).
Boot11:44:14/DWM4448 retained through14:49:35, guard0, temperature66.6C then.
No OS, device, DWM or AC reset occurred during this migration.

## Scope and collection

This accepts the tested ordinary-buffer compute/inference path on current Mesa.
It does not establish Vulkan conformance, sparse mappings, Windows graphics WSI,
D3D GPU acceleration, all KMD DMA/cache/PFN/lifetime contracts or full M9/M13.
The old desktop fluid-mouse confirmation and manual menu/move/resize acceptance
are not replaced by DWM process uptime. Windows Terminal's crash remains a
separate desktop compatibility issue.

Launcher initially missed the loader PATH (0xc0000135); adding the same m8 PATH
used by the workloads fixed loading. A --version probe was rejected by
llama-bench; the supported --list-devices check then returns0 and sees RADV.
These are retained launcher checks, not extra inference successes. Failed
scheduled tasks were removed; no test process remains at final inspection.

Text copies preserve native newlines, decode UTF16 summaries to UTF8, and redact
only PCI/interface identities and the unrelated WER report identifier. No full
process dump was collected. Build inputs/source patches are versioned separately;
large DLL/PDB artifacts remain in workspace/isolated lab directories. Manifest
covers immutable files recursively except the manifest itself.
