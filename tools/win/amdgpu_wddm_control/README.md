# amdgpu-wddm Control

The control application for testers of the amdgpu-wddm driver on the ASRock BC-250. It runs on the tester's own PC,
by hand, like the vendor's control panel: a normal window, no service, no autostart, no network, no lab tools.

| Page | What it shows or changes |
|---|---|
| Overview | KMD version, driver package version and date, GPU name, video memory in use, test signing, whether the desktop composes on the GPU, clock, voltage, temperature, load, DPM mode, ceiling and the current limit; the installed components (the KMD service image, every `*DriverName` value of the display driver key, Vulkan driver manifests and their libraries) with file version and SHA-256 |
| Performance | DPM on or off and the ceiling (1000-2000 MHz on the 100 MHz grid, default 1500); temperature protection as read-only text |
| Applications | D3D12 application profiles: the `Experiment` value of `HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<exe>` as check boxes with plain descriptions; names outside the catalog are shown and kept, not edited |
| Diagnostics | "Create bug report": a zip on the Desktop, after the tester has seen the file list and each file's text |

## How it talks to the driver

Through `bc250control.dll`, built from `tools/win/bc250kmd_cli/bc250kmd_cli.c` with `BC250_CONTROL_DLL`. Only
software-state escapes, each with `NoAdapterSynchronization` alone: the DPM snapshot (`Bc250Dpm`), start health
(`Bc250StartHealth`, op READ), the GPU DWM interop decision (`Bc250Interop`), dxgkrnl's segment statistics
(`Bc250VideoMemory`) and the log ring (`Bc250LogRead`, `GET_LOG`, answered without the adapter lock from 0.7.184.1).
No `HardwareAccess` escape and no `LOG_SUMMARY`: a Level Two escape idles the GPU, and an overlay that polled one every
5 s stalled a running game for 300 ms each time (BD-054). Live values are read every 2 s, and only while Overview or
Performance is shown and the window is not minimized.

The app never changes the driver through an escape. DPM settings are `DpmMode` and `DpmMaxMHz` under
`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`, which the KMD reads at device start
(`docs/design/dpm.md`, "Settings and boot guard"): the page says to restart Windows. The runtime tuning escapes
(`dpm tune`, `dpm floor`) are lab tools and not offered. The boot guard can set `DpmMode` back to 0 after an unclean
or unconfirmed start; the page shows the last start's reason.

The window runs as the invoking user. A change starts an elevated copy of the program (one UAC prompt) with one verb,
`--write-dpm <mode> <MHz>`, `--write-profile <exe> <list>` or `--remove-profile <exe>`, which checks its arguments
with the same functions as the window and reads the value back.

## Bug report

`driver-state.txt` (the snapshots above), `driver-log.txt` (the whole log ring), `installed-files.txt` (Device
Manager status and problem code, the display driver key, every component with version and SHA-256), `settings.txt`
(the driver's `Parameters` values and the D3D12 profiles), `system.txt` (Windows build, test signing, app version),
`events.txt` (System events of the display stack and Application crash records that name it, last 24 hours),
`dxdiag.txt` (`dxdiag /t`), `d3d12-caps.json` (`amdgpu_wddm_d3d12caps.exe 0` when installed next to the app) and
`vulkan-summary.txt` (`vulkaninfo --summary` when found). Every text passes `Redactor` first: user name and profile
path, computer name, MAC addresses, e-mail addresses and the values of lines that name a serial number, machine id,
product id or UUID are replaced. Versions, hashes, LUIDs and PnP hardware ids stay.

## Build

```powershell
pwsh tools\win\amdgpu_wddm_control\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Out <BC250_ROOT>\scratch\release\control-app\build\app
```

Compilers from the installed Visual Studio (Roslyn `csc` for .NET Framework 4.8, which Windows 10 and 11 carry, and
`cl` for the DLL), headers and import libraries from the SDK NuGet packages. Deterministic output. The gates, each of
which stops the build: `test/UnitTests.cs` (reply offsets computed from the text of `driver/kmd/bc250kmd_escape.h`,
the profile catalog against every `ddi_experiment("...")` call in `driver/umd/d3d12`, profile editing, DPM ranges
against `docs/design/dpm.md`, redaction), the compiles with warnings as errors, `--smoke` (the pages built and
refreshed once without a window, which on a PC without a BC-250 must say "Driver not found") and `--smoke-report` (a
report without dxdiag, capability tools and events, checked for its files and for this PC's user and computer name).

The output folder holds `amdgpu_wddm_control.exe` and `bc250control.dll`; the release installer puts both, and
`amdgpu_wddm_d3d12caps.exe`, in one directory.
