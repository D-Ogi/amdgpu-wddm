# E39: D3D12 feature level 12_1 with the RADV sparse flag

M570, unit A, 2026-09-27 (lab clock 02:31-02:33 local, UTC+2; boot 2026-09-26T18:44:31+02:00 unchanged, KMD
unchanged, registered ICD 93B1D1FD from E38 used as is, no swap, no UMD change, CPU DWM 8008 unchanged).

## Question

E37 (M564) measured feature level 11_1 with TiledResourcesTier 0 as the only gap to 12_1, while sparse in
the WDDM port is gated. vkd3d-proton derives the tier from Vulkan alone (`libs/vkd3d/device.c` 8898-8921:
sparseBinding, sparseResidencyAliased/Buffer/Image2D, residencyStandard2DBlockShape and a sparse-binding
queue for tier 1; shaderResourceResidency, shaderResourceMinLod, no residencyAlignedMipSize,
residencyNonResidentStrict and filterMinmaxSingleComponentFormats for tier 2; sparseResidencyImage3D and
residencyStandard3DBlockShape for tier 4). The port enables sparse when `pdev->info.has_sparse` or
`RADV_EXPERIMENTAL_SPARSE` is set (`radv_physical_device.c:110`). Does the flag alone give 12_1?

## Runs (out007/)

`scripts/run-fl007.ps1`: the E37 probe (`fl-probe.exe` 30F69C05, vkd3d-proton 472989aa d3d12.dll 7B77ED5C,
d3d12core.dll 90B1DAD6, DXVK 3.1.1 dxgi.dll 2E674A56 next to it), interactive scheduled task, 30 s limit,
minidump on timeout, run twice:

| run | environment | max feature level | TiledResourcesTier | everything else |
|---|---|---|---|---|
| A-default | as E37 runs 005/006 | 11_1 | 0 | as E37 |
| B-sparse | `RADV_EXPERIMENTAL=sparse` | 12_1 | 4 | identical to A |

`diff` of the two JSON outputs shows exactly those two fields. Both runs: device "AMD BC-250 (RADV GFX1013)",
ResourceBindingTier 3, ConservativeRasterizationTier 3, ROVs, typed UAV loads, SM 6.8, RaytracingTier 1.1,
mesh 0, VRS 0, sampler feedback 0.9, heap tier 2, enhanced barriers, wave 32-64, exit 0 in about one
second, Tctl 67.8-68.0. Registered hash 93B1D1FD before, during and after.

## Reading

The feature level reported to D3D12 applications depends only on the port's sparse gate. Tier 4 means the
port also advertises 3D sparse residency and the standard 3D block shape. Any D3D12 application launched
with the flag in its environment sees 12_1 today; whether sparse is correct under load is a separate
question (the paused 19078-case sparse CTS stands at 12129 done, 9962 Pass, 2167 NotSupported, 0 Fail,
6949 remaining, on the earlier candidate 4D027149 and KMD 151; the M484 and M485 TDR items are preserved).
Removing the gate by default waits for that evidence.

## Limits

Feature level and option tiers only, no rendering, no sparse allocation exercised by the probe beyond
device creation. Same boot and DLLs as E37. The DWM was Codex's CPU DWM 8008 at the time; irrelevant to a
probe without a window.

PROVENANCE: Mesa MIT; vkd3d-proton LGPL-2.1 and DXVK zlib as standalone runtime DLLs next to the probe, no
code copied; probe and scripts original.
