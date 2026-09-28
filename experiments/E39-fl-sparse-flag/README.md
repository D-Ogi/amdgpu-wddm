# E39: D3D12 feature level with the RADV sparse flag

Question: is the D3D12 feature level 12_1 reachable on the WDDM RADV port today, before the full sparse
CTS run is completed, by enabling the port's sparse gate (`RADV_EXPERIMENTAL=sparse`)? vkd3d-proton derives
TiledResourcesTier from the Vulkan sparse features and the sparse-binding queue alone
(`libs/vkd3d/device.c`, `d3d12_device_determine_tiled_resources_tier`), and the port gates sparse on
`has_sparse` or that experimental flag (`radv_physical_device.c`).

Scope: the E37 probe, run twice on the promoted registered ICD 93B1D1FD (E38) without any swap: A with the
default environment as a control, B with the flag. Interactive scheduled task, 30 s limit, minidump on
timeout.

Result: `evidence/windows/2026-09-27-E39-fl-sparse-flag/RESULT.md` (M570): A stays at 11_1 / tier 0
(E37 reproduced on the new baseline), B reports 12_1 / tier 4, everything else identical.

Open: correctness of sparse under load (remaining 6949 CTS cases of the paused 19078-case run, the
preserved M484/M485 TDR items) before the gate is removed by default; a D3D12 application with the flag
set in its environment.
