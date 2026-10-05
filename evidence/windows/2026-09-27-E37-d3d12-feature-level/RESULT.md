# E37: D3D12 feature level through vkd3d-proton on the WDDM RADV port

M564, unit A, 2026-09-27 (lab clock 01:23-02:00 local, boot 2026-09-26T18:44:31+02:00, no reset, KMD
unchanged, CPU DWM 84 untouched, registered ICD 9C40083C restored and hash-checked after every run).

## Result

vkd3d-proton 472989aa (d3d12.dll 7B77ED5C, d3d12core.dll 90B1DAD6) with DXVK 3.1.1 dxgi.dll 2E674A56 next
to a 144 KB probe (`fl-probe.c`: CreateDXGIFactory2, D3D12CreateDevice at 11_0, CheckFeatureSupport,
JSON on stdout, no rendering) reports for "AMD BC-250 (RADV GFX1013)", vendor 0x1002 device 0x13fe:

| Field | Value | Note |
|---|---|---|
| max feature level | 11_1 | 12_0 needs TiledResourcesTier >= 2 (vkd3d-proton device.c 9605-9617) |
| TiledResourcesTier | 0 | sparse binding still gated in RADV (RADV_EXPERIMENTAL=sparse, M481-M491) |
| ResourceBindingTier | 3 | |
| ConservativeRasterizationTier | 3 | |
| ROVsSupported | true | EXT_fragment_shader_interlock via POPS |
| TypedUAVLoadAdditionalFormats | true | |
| highest shader model | 6.8 | |
| RaytracingTier | 1.1 | |
| MeshShaderTier | 0 | |
| VariableShadingRateTier | 0 | |
| SamplerFeedbackTier | 0.9 | |
| EnhancedBarriers | true | |
| dedicated video memory | 8406929408 | shared system memory 4203462656 |

Identical values from two ICDs: the E36 consolidated build FAD08ECB (run 006, `out006/`) and the D3DKMT
enumeration candidate 93B1D1FD (run 005, `out005/`). Every requirement of FL 12_0 and 12_1 except tiled
resources is met; the feature level rises to 12_1 when sparse leaves its gate, nothing else is missing.

Run 005 also passed the E14 compute smoke with the candidate in the registered ICD path through the
recorded runner (8 tests, 0 mismatches, live module witness 93B1D1FD, `out005/smoke-receipt.json`), so plain
Vulkan enumeration through D3DKMTEnumAdapters2 finds the adapter.

## The hang of the pre-M496 baseline (runs 001, 003, 004)

With the registered baseline ICD 9C40083C, and with the M491 sparse candidate 4D027149, the probe never
returned (60 s, session 0 and interactive session alike, empty stdout and stderr, `run-fl001-session0.log`,
`run-fl003-interactive.log`). Run 004 wrote a minidump of the hung process before killing it
(`out004/cdb-stack.txt`, cdb -z on the development PC): the main thread waits in
`ntdll!RtlAcquireSRWLockExclusive` under DXVK's `dxgi!CreateDXGIFactory2`, called from
`vulkan_radeon!vk_dxgi_adapter_foreach` (inline `vk_dxgi_get_factory`); the other five threads are idle
thread-pool workers. DXVK holds its instance singleton lock (`Singleton<DxvkInstance>::acquire`, an SRW lock)
while it creates the Vulkan instance; the ICD's adapter lookup re-enters DXVK's dxgi.dll and takes the same
lock on the same thread. The DXVK log (`out004/fl-probe_dxgi.log`) shows both CreateDXGIFactory2 calls.

That baseline was built 2026-09-25 07:53 from `scratch/mesa-radv-main-20260924`, whose vk_dxgi.cpp:33 does
`LoadLibraryA("DXGI.DLL")`, the defect M496 fixed the same evening by loading DXGI through the full System32
path (candidate 98C53B48, not promoted; baseline restored afterwards). Run 006 confirms that M496's approach
is sufficient for this probe. The same probe with the same DLLs completes in about one second on the
development PC (`devpc-control-nvidia.json`), which separated the probe from the lab defect.

## Candidate: adapter enumeration without DXGI (patches/, not promoted, not pushed)

Two commits on a local fork branch (`amdgpu-wddm/radv-wddm2-kmt-enum`, base 940ab0eb):
`0001` guards the generated dispatch header's fallback `STATUS_*` / `NT_SUCCESS` macros (typo `NSTATUS`, seven
C4005 warnings in every TU that includes windows.h first); `0002` makes `vk_dxgi_adapter_foreach` enumerate
through `D3DKMTEnumAdapters2` + `QueryAdapterInfo` (adapter type, PCI ids, segment sizes, registry
description), so the ICD loads no DXGI and no user-mode module during physical device enumeration. Built with
`tools/build/build-mesa.ps1 -Config radv`, zero warnings, ICD 93B1D1FD. This is a robustness change, not the
fix of a defect present in current code (M496 already avoids the re-entry); the WSI interop helpers
(`vk_dxgi_find_adapter`, `vk_dxgi_create_d3d12_device`) still load DXGI and D3D12 by System32 path.

## Limits

Feature level and option tiers only; no D3D12 rendering, no vkd3d-proton test suite, no application. The
values come from vkd3d-proton's mapping of RADV's Vulkan features and were not cross-checked against
`vulkaninfo`. Sparse was not enabled in these runs (RADV_EXPERIMENTAL=sparse was passed only to the pre-M496
ICDs that hung, runs 001/003), so the FL with the gate open is not measured here. No promotion, no
publication.

PROVENANCE: Mesa MIT; vkd3d-proton LGPL-2.1 and DXVK zlib as standalone runtime DLLs next to the probe, no
code copied; probe and scripts original.
