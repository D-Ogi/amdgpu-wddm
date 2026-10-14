# E59: the D3D12 conformance client passes on the published tester.30 triplet (M900)

Unit A, Windows 11 23H2 (22631), 2026-10-13 15:09:46 UTC. This run repeats the conformance client of E50 section 4
(M781, M782) on a release install. Until this run the client had not run on a triplet registered after 2026-10-01.

| item | identity |
|---|---|
| release | `0.7.216.101-tester.30`, clean install from the published package |
| kernel driver | 0.7.216.64 candidate image `97B836CC` (BD-155 round of that hour) in place of the shipped `71C6B09C`. The user-mode files are the package's |
| D3D12 shell | `amdgpu_wddm_d3d12.dll`, SHA-256 `81D22B566E3193BB90BDDBFD2710A597DDADF05B795E202307B90EB71D5EB353` |
| D3D12 engine | `amdgpu_wddm_vkd3d.dll`, SHA-256 `80217882D8BEB3BB0115E006034A380D12ED03BD10398EBC372A78027178CBA6` |
| D3D12 ICD | `amdgpu_wddm_radv.dll`, SHA-256 `EF863272C5EDD49BB7B22FED9F3FC26C3552AC143205B84EEF6221DF896480C7` |
| client | `amdgpu_wddm_conformance.exe`, SHA-256 `F1FA4329F6EA88CCAFCB082629990EE3375718647D8BB3C360E3A379DC030575`, built from `tools/win/conformance-clients` of main |
| runtime | `C:\Windows\system32\d3d12.dll`, `D3D12Core.dll` and `dxgi.dll`. No DLL is next to the executable |

The client runs as one process with a 150 s deadline (`run-conformance.ps1`). It ended with exit 0 after 1 s.
`run-20261013T150946Z.txt` is the whole output: the per-call trace of the last subtest and `conformance.json`.

## Result

| subtest | result | checked | negative controls |
|---|---|---|---|
| rov | PASS, 0 mismatches | 12288 (ROV 4096, render target 8192) | reversed-order oracle 4096 of 4096 differ, the same shader without ROV 4096 of 4096 differ |
| conservative (tier 3, inner coverage) | PASS, 0 mismatches | 12288 (standard, conservative, inner 4096 each) | the standard and conservative oracles differ in 98 pixels. 82 inner pixels are expected and set |
| dxr-indirect | PASS, 0 mismatches | 694 (5 variants of 128, 54 GPU-written arguments) | 12 hits, 52 misses. The count-1 image differs from the count-2 oracle in 32 of 128 words |

Device and caps that the client read: device created at FL 12_1, `MaxSupportedFeatureLevel` 12_1, `ROVsSupported` 1,
`ConservativeRasterizationTier` 3, `RaytracingTier` 1.1, `HighestShaderModel` 6_6. Adapter `VEN_1002 DEV_13FE`.

Each negative control shows that its comparison can fail: an oracle that is wrong on purpose gives mismatches on the
same image. So the 0 mismatches are a result of the comparison, not of a comparison that never runs.

## Limits

One run. Offscreen, no Present. The kernel driver is a candidate image, not the shipped one: the user-mode triplet
is the release's, and the kernel driver change of that image is scanout code (BD-155) that this
client does not reach. This is a three-subtest client, not a D3D12 conformance suite.
