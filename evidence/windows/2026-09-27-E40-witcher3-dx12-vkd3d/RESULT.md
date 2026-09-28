# E40: The Witcher 3 4.0 DX12 build reaches its main menu through vkd3d-proton on the WDDM RADV port

M571, unit A, 2026-09-27 (lab clock 02:45-02:48 local, UTC+2; boot 2026-09-26T18:44:31+02:00 unchanged,
KMD unchanged, registered ICD 93B1D1FD (E38) used as is, no manifest change, CPU UMD 8279AC7F untouched,
CPU DWM 5748 unchanged, no reboot).

## Setup (run001/, scripts/)

`scripts/run-w3dx12-001.ps1`, derived from the DX11 run `witcher3-gpu001` (DXVK d3d11), as an
interactive scheduled task:

- executable `bin\x64_dx12\witcher3.exe` 609BCA70, version 4.0.0.103190, launched directly (Steam running);
- next to it the E37 package: vkd3d-proton 472989aa `d3d12.dll` 7B77ED5C and `d3d12core.dll` 90B1DAD6, DXVK
  3.1.1 `dxgi.dll` 2E674A56; no originals existed; all three removed in finally after hash checks;
- environment: `RADV_EXPERIMENTAL=sparse` (E39), `VKD3D_DEBUG=warn`, vkd3d and Mesa shader caches and the
  DXVK/vkd3d logs under the run directory, `dxgi.maxFrameRate = 30`;
- 150 s bound with STOP flag, Tctl, module witness, window title and working set every 3 s, overlay
  screenshots at 21, 62 and 122 s; `user.settings` backed up (hash 58327E6D) and compared afterwards.

## Result

The game stayed alive and responding for the whole bound (window "The Witcher 3" from 10 s, working set
1.0-1.3 GB) and was killed by the script at 152 s, `result.json`. Module witness at 3 s: our `d3d12.dll`,
`d3d12core.dll` and `dxgi.dll` from the game directory, `vulkan-1.dll` from System32 and
`vulkan_radeon.dll` 93B1D1FD from the registered path; also the game's own `sl.interposer.dll` (Streamline)
and `libxess.dll`. The DXVK DXGI log shows the adapter "AMD BC-250 (RADV GFX1013) (radv 26.2.99)", the
vkd3d log the device with SM 6.8, DXR 1.1, descriptor buffers and the game-specific workarounds.

All three screenshots (1920x1200, hashes in `capture-hashes.json`, images kept outside Git) show the main
menu with the "Patch 4.0 - Highlights" panel: text, icons and the background scene rendered correctly and
identically across the three captures, plus the game's own "settings reset to defaults" notice on the
first one. This is the first D3D12 frame content on the port; E37/E39 only reported capabilities.

Notable log lines (`run001/vkd3d.log`, 152 lines):

| line | count | reading |
|---|---|---|
| `d3d12_shared_fence_open_export_kmt: Failed to get exported semaphore handle, vr -13` | 4 | the game (or Streamline) creates a shared D3D12 fence; exporting it as a KMT handle fails in the port, the game continues |
| `d3d12_state_object_find_association: Conflicting local root signatures ...` | 8 | a DXR state object was created (ray-tracing pipelines compiled) |
| `Mapping VK_FORMAT_D24_UNORM_S8_UINT to VK_FORMAT_D32_SFLOAT_S8_UINT` | 4 | standard RADV format substitution |
| `d3d12core_GetDebugInterface: Returning DXGI_ERROR_SDK_COMPONENT_MISSING` | 1 | no debug layer, expected |

Tctl 67.4 before, 69.2 after; `after-health.txt` shows the KMD healthy with 4903 completed submissions on
the same generation.

## Restore

`d3d12.dll`, `d3d12core.dll`, `dxgi.dll` removed from `bin\x64_dx12` (hash-checked as ours), `user.settings`
unchanged (the game announces a reset but had not rewritten the file when killed), no witcher3 process,
task unregistered, registered ICD 93B1D1FD, DWM 5748.

## Limits

Menu only, no input, no gameplay, no frame timing, no Linux comparison. Correct-looking menu frames say
nothing yet about the 3D scene, ray tracing at runtime or sparse under load. The shared-fence export
failure is an open item for the port (external semaphore KMT handles). Fullscreen mode and resolution were
the game defaults.

PROVENANCE: Mesa MIT; vkd3d-proton LGPL-2.1 and DXVK zlib as standalone runtime DLLs next to the game, no
code copied; the game is the owner's licensed copy, screenshots private; scripts original, derived from the
recorded DX11 procedure.
