# M768: system D3D12 reaches the native diagnostic UMD after PnP name refresh

Source 6e015467, adapter026 /W4 /WX build. Registration002 retained the original
three UserModeDriverName entries and appended the explicit native UMD path.
The identified adapter's pnputil restart completed with exit0. KMT DX12 name
query then returned success and amdgpu_wddm_d3d12.dll, unlike M765 without a
lifecycle refresh. This measures this refresh path, not all possible mechanisms.

The client loaded System32 d3d12.dll and selected the BC-250 through DXGI.
Its stderr contains the native OpenAdapter12 entry, followed by GetCaps1074
(size8) and GetCaps1007 (size4), both E_NOTIMPL, two GetSupportedVersions calls
and CloseAdapter. WDK10.0.26100 names those caps 0081_3DPIPELINESUPPORT1 and
3DPIPELINESUPPORT. D3D12CreateDevice FL11_0 returned887A0004. No device/queue,
GPU rendering, feature-level or game success is claimed.

The registration is intentionally retained for subsequent native integration.
The CPU desktop UMD, system Vulkan ICD and KMD171 binary hashes are unchanged.
Boot and DWM process are unchanged; the adapter's start generation changed.
Postflight health flags15 and completed-primary progress confirmed;66.8C.
All five child Jobs closed; supervisor91.0060616s. Task independently Missing.

Evidence selection: runtime/probe logs, completion receipts, technical postflight
fields and file hashes. Raw PnP instance output, complete parameter values and
process/task inventories remain in the local scratch directory. Scripts and
binaries are identified by the immutable stage manifest; the candidate hash is
in postflight-selected.json. This selection makes no performance claim.
