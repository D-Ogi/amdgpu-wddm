# E41: The Witcher 3 DX12 build loads a save and renders the 3D scene through vkd3d-proton on the WDDM RADV port

M573, unit A, 2026-09-27 (lab clock 03:03-03:06 local, UTC+2; boot 2026-09-26T18:44:31+02:00 unchanged, KMD
unchanged, registered ICD 93B1D1FD (E38) used as is, CPU UMD 8279AC7F untouched, CPU DWM 5748 unchanged).

## Setup (run002/, scripts/)

E40's run 001 procedure (`scripts/run-w3dx12-002.ps1`: vkd3d-proton 472989aa d3d12.dll 7B77ED5C, d3d12core.dll
90B1DAD6, DXVK 3.1.1 dxgi.dll 2E674A56 next to `bin\x64_dx12\witcher3.exe`, `RADV_EXPERIMENTAL=sparse`,
`dxgi.maxFrameRate = 30`) with a 600 s bound, an overlay screenshot every 30 s, and a stop file for a graceful
end. Input from the development PC through the interactive task `BC250-Witcher-Input`
(`scripts/input-dx12.ps1`, the recorded DX11 input script plus Key and Hold actions; `run002/input-002.log`),
with half-scale overlay screenshots between steps (`manual-*.png`, private, hashes in `capture-hashes.json`).

## Sequence and result

| lab time | step | what the screen showed |
|---|---|---|
| 03:04:10 | launch | main menu behind the telemetry consent dialog; the animated 3D menu scene (camp at night) rendered |
| 03:04:30 | Escape | consent declined; full menu scene visible, CONTINUE highlighted |
| 03:04:51 | Click (332,444) | no effect: the cursor stayed at the corner, the absolute mouse move did not register |
| 03:05:18 | Key E | CONTINUE selected; loading screen with the game's tips |
| 03:05:50 | screenshot | in-game 3D scene at Kaer Morhen: character, lighting, shadows, HUD and minimap correct |
| 03:06:00 | Hold W 3 s | the game opened its pause menu (focus change from the input task), scene still rendered behind |
| 03:06:15 | stop file | graceful end, game killed by the worker at 139 s |

The game stayed responsive throughout (`result.json`, `end_reason` stop-file). Working set rose to 3.8 GB after
the load. Tctl 67.6 at launch, peak 77.8 during loading and the first in-game seconds, 72.0 at the end
(41 samples in `run002/run-w3dx12-002.log`; the 85 C stop was never reached).

Module witness (`modules.json`): our `d3d12.dll`, `d3d12core.dll`, `dxgi.dll` from the game directory,
`vulkan-1.dll` from System32, `vulkan_radeon.dll` 93B1D1FD from the registered path.

The vkd3d log (`run002/vkd3d.log`, 165 lines) shows the same lines as E40: DXR state objects created (8
local-root-signature warnings), four failed shared-fence KMT handle exports (`vr -13`), D24S8 mapped to
D32S8, DSV format unknown on four pipelines. Nothing new appeared during the load or in the scene; no device
loss, no GPU timeout, KMD health unchanged (generation 575721647, 10133 completed submissions after).

## Restore

DLLs removed from `bin\x64_dx12` after hash checks, `user.settings` unchanged (58327E6D), save-file count
unchanged (115 files in `gamesaves`), no witcher3 process, task `BC250-M12-witcher3-dx12-002` unregistered,
stop file removed, registered ICD 93B1D1FD, DWM 5748.

## Limits

About one minute of in-game rendering, no measured frame time (the DXVK cap of 30 fps was set; no HUD in
a D3D12 path), no ray tracing settings touched (defaults after the game's settings reset), no Linux
comparison, no repeated loads. Absolute mouse input through `mouse_event` does not reach the game; keyboard
does. The pause on the W hold is an input-task artefact (focus change), not a rendering event.

PROVENANCE: Mesa MIT; vkd3d-proton LGPL-2.1 and DXVK zlib as standalone runtime DLLs next to the game, no
code copied; the game is the owner's licensed copy, screenshots private; scripts original, derived from the
recorded DX11 procedure.
