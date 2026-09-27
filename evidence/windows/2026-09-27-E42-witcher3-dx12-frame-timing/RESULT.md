# E42: The Witcher 3 DX12 build, PresentMon frame-timing attempt (M577)

Unit A, Windows 11, 2026-09-27 01:29-01:38 UTC (03:29-03:38 local). Registered ICD 93B1D1FD (E38 baseline),
UMD 8279AC7F, KMD 152 (generation 575721647), CPU DWM 4400, lab operating point 1000 MHz / VID 116 before and
after both runs. No UMD/ICD/DWM/KMD change; interactive scheduled tasks as in E40/E41.

## Setup

- Game: `bin\x64_dx12\witcher3.exe` 4.0.0.103190 with vkd3d-proton 472989aa (`d3d12.dll` 7B77ED5C,
  `d3d12core.dll` 90B1DAD6) and DXVK 3.1.1 `dxgi.dll` 2E674A56 placed next to the exe for the run and removed
  after (E40 package). Environment as E41 (`RADV_EXPERIMENTAL=sparse`, vkd3d and DXVK logs into the run
  directory) but without a DXVK frame cap; `VKD3D_FRAME_RATE` unset.
- PresentMon 2.6.0 console build (MIT, `scripts/presentmon-PROVENANCE.md`) started in the same interactive
  task before the game with `--process_name witcher3.exe --session_name BC250W3 --stop_existing_session
  --no_console_stats --qpc_time_ms --terminate_on_proc_exit`:
  - run 003 additionally `--write_display_metadata` (display and GPU tracking on);
  - run 004 additionally `--no_track_display --no_track_gpu --no_track_input` (present cadence only),
    plus `logman query BC250W3 -ets` into the run log and every dxgi/d3d12/dcomp module the game loads.
- Steering by the interactive input task (`input-dx12-003.ps1`): Escape at the start panel, E to select
  CONTINUE (latest save, Kaer Morhen), then the scene left running; stop file ends the run, the finally
  block restores everything.

## Runs

| run | bound | end | scene | auto screenshots | Tctl peak | PresentMon CSV | PresentMon exit |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 003 | 600 s | stop file at 225 s | menu, load, Kaer Morhen interior with HUD | 7 | 77.8 | 0 bytes | did not terminate on game exit, stopped after 15 s |
| 004 | 600 s | stop file at 162 s | same | 5 | 79.9 | 0 bytes | same |

Both runs rendered normally (screenshots `auto-*.png` and the manual `run00N-s*.png`, private, hashes in
`capture-hashes.json`). The game process stayed responsive, working set 3.3-3.9 GB in the scene, no device
loss, KMD health `completed` advancing (8910 after run 003, 15063 after run 004), `user.settings` unchanged
(58327E6D), save-file count unchanged (115).

The run-004 log shows the ETW session `BC250W3` in state Running with the providers Microsoft-Windows-D3D9,
Microsoft-Windows-Kernel-Process, Microsoft-Windows-DxgKrnl, two GUID-only providers and
Microsoft-Windows-DXGI, and the game loading, besides the app-local DXVK `dxgi.dll`, the system
`C:\Windows\system32\dxgi.dll` (DDC578A2) and `DComp.DLL` (285F0317) at 3 s, next to `vulkan_radeon.dll`
93B1D1FD. PresentMon's stdout and stderr stayed empty (UTF-16 BOM only). After each run: no PresentMon
process, `logman query BC250W3 -ets` reports no session, no test task, game directory clean.

vkd3d log of run 003: same kinds of entries as E41 (DXR state objects, D24S8 to D32S8 mapping, and four
`d3d12_shared_fence_open_export_kmt: Failed to get exported semaphore handle, vr -13`).

## Reading

PresentMon builds its rows from the DXGI runtime's Present events and the dxgkrnl present/flip events.
The port's Win32 WSI (`src/vulkan/wsi/wsi_common_win32.cpp` on fork `amdgpu-wddm/radv-wddm2-kmt-enum`
c34ab7cd) takes its DXGI/DirectComposition path only when `wsi_device->win32.get_d3d12_command_queue` is
set; the RADV WDDM winsys sets no such hook, so `supports_dxgi` is false and every swapchain uses the
CPU-image path: per present a CPU copy (with a B8G8R8A8 swizzle for R8G8B8A8 chains) of the rendered image
into a DIB section, `BitBlt` into the window DC, `GdiFlush`, `DwmFlush`. That path issues no DXGI present
and no dxgkrnl present, so PresentMon has nothing to record; the loaded system `dxgi.dll` and `DComp.DLL`
come from the factory/device probe at WSI init, not from a swapchain. `--terminate_on_proc_exit` not firing
is consistent with PresentMon never having associated a tracked swapchain with the process.

Consequences:

- Frame timing for the D3D12 path needs an in-stack instrument. The fork branch
  `amdgpu-wddm/radv-wddm2-present-log` (0f9811a5, on top of the fence fix 59f53088) adds an optional
  per-present CSV (`BC250_WSI_PRESENT_LOG`) with the copy, BitBlt and DwmFlush times; candidate ICD
  6661C2D2, not yet run on the lab.
- The CPU-image present path is itself a cost to measure: at 1920x1200 every present copies about 9 MB on
  the CPU and blits through GDI before DwmFlush. This is a hypothesis about the frame-time budget until the
  present log measures it (no fact row for it yet).

## Files

- `run003/`, `run004/`: run logs (UTF-8 copies of the task logs), `modules.json`, `result.json`,
  `launch.json`, `vkd3d.log`, `witcher3_dxgi.log`, KMD health and clock before/after, `dxvk.conf`, done
  files, PresentMon stdout/stderr, `presentmon.csv.size` (the CSV itself was empty), input logs.
- `scripts/`: the run, worker, launch, stop, status, input, precheck and postcheck scripts, the pull and
  PresentMon CSV analysis scripts (unused on real data here, checked on synthetic input), the finish script,
  PresentMon provenance.
- `capture-hashes.json`: SHA-256 of the private screenshots; `manifest.json`: artifacts and file hashes.
