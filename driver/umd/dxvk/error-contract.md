# Native DDI error boundary

The engine COM API and the runtime DDI have different permitted error channels.
`ID3D11DeviceContext::Map` in the engine can return `E_OUTOFMEMORY`. The native
`ResourceMap` DDI (including discard variants) cannot forward that HRESULT through
`pfnSetErrorCb`: it permits `D3DDDIERR_DEVICEREMOVED`, plus
`DXGI_DDI_ERR_WASSTILLDRAWING` only when `D3D10_DDI_MAP_FLAG_DONOTWAIT` is present.

The shell maps failed engine Map results to device removal except for that legal
polling case. It marks the hosted bridge device-lost before notifying the runtime.
Output mapping fields remain zero on failure; no pointer from a failed Map is published.
Busy without DONOTWAIT is terminal. Resource creation retains its separate OOM policy.

Source contract: local `ref/ddi-display/d3d10umddi.md`, ResourceMap remarks (WDK/SDK
10.0.26100), and `ref/windows-driver-docs/windows-driver-docs-pr/display/handling-errors.md`
(snapshot110f60ea). Public reference:
https://learn.microsoft.com/windows-hardware/drivers/ddi/d3d10umddi/nc-d3d10umddi-pfnd3d10ddi_resourcemap

Validation: required `test-umd-ddi-draw.ps1` tests OOM, unexpected failures, all device-loss
statuses, legal polling and busy without DONOTWAIT. The full UMD build also runs exact
engine-pair and session-lifetime gates. Host tests pass with the DC65/C388 pair.
Native injected Map/Update error propagation has not yet been measured. M752 predates
this change and proves successful resize/Present, not this failure path.
