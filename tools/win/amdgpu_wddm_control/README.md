# amdgpu-wddm Control

The control application for testers of the amdgpu-wddm driver on the ASRock BC-250. It runs on the tester's own PC,
by hand, like the vendor's control panel: a normal window, no service, no autostart, no network, no lab tools.

| Page | What it shows or changes |
|---|---|
| Overview | KMD version, driver package version and date, GPU name, video memory in use, test signing, whether the desktop composes on the GPU (the release's desktop router wins: `DwmForceCpu` 1 reads
"CPU route (GPU route disabled, BD-058)"), a one-line Recovery summary, clock, voltage, temperature, load, DPM mode, ceiling and the current limit; the installed components (the KMD service image, every `*DriverName` value of the display driver key, Vulkan driver manifests and their libraries) with file version and SHA-256 |
| Performance | DPM on or off and the ceiling (1000-2000 MHz on the 100 MHz grid, default 1500); temperature protection as read-only text |
| Applications | D3D12 application profiles: the `Experiment` value of `HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<exe>` as check boxes with plain descriptions; names outside the catalog are shown and kept, not edited |
| Recovery | The driver's start and recovery states, each in plain English with one recommended action, and five safe actions (below) |
| Diagnostics | "Create bug report": a zip on the Desktop, after the tester has seen the file list and each file's text |

## How it talks to the driver

Through `bc250control.dll`, built from `tools/win/bc250kmd_cli/bc250kmd_cli.c` with `BC250_CONTROL_DLL`. Only
software-state escapes, each with `NoAdapterSynchronization` alone: the DPM snapshot (`Bc250Dpm`), start health
(`Bc250StartHealth`, op READ), the GPU DWM interop decision (`Bc250Interop`), dxgkrnl's segment statistics
(`Bc250VideoMemory`) and the log ring (`Bc250LogRead`, `GET_LOG`, answered without the adapter lock from 0.7.184.1).
No `HardwareAccess` escape and no `LOG_SUMMARY`: a Level Two escape idles the GPU, and an overlay that polled one every
5 s stalled a running game for 300 ms each time (BD-054). Live values are read every 2 s, and only while Overview or
Performance is shown and the window is not minimized.

The one escape that changes the driver is the start-health CONFIRM of "Confirm this start" (Recovery), the same
request the release's logon task sends: administrator only, `HardwareAccess`, once per action. DPM settings are
`DpmMode` and `DpmMaxMHz` under
`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`, which the KMD reads at device start
(`docs/design/dpm.md`, "Settings and boot guard"): the page says to restart Windows. The runtime tuning escapes
(`dpm tune`, `dpm floor`) are lab tools and not offered. The boot guard can set `DpmMode` back to 0 after an unclean
or unconfirmed start; the page shows the last start's reason.

The window runs as the invoking user. A change starts an elevated copy of the program (one UAC prompt) with one verb,
`--action <name>` or `--apply-profiles <exe> <list> ...` (an empty list removes the application's key), which checks
its arguments with the same functions as the window and reads the value back.

## Settings rule

Every settings page follows one rule (owner, 2026-10-03). A box is checked only when its value is written; nothing is
written for an unchecked box; unchecking removes the value (all switches of an application unchecked removes its key),
so no empty value and no implicit default is left behind. A driver's built-in default is described in text ("driver
default when unchecked: ..."), never shown as a checked box. Values the installer wrote (the `witcher3.exe` profile)
show as checked because they are written. Changes stay on the page, marked "Unsaved changes", until Apply; Revert
reads the stored values again. `Profiles.PlanWrite` and `DpmSettings.PlanWrites` hold the rule and the unit tests
check it (unchecked: nothing written; checked, saved, unchecked: removed; the exact set written, in any stored order).
A `DpmMode` 0 the driver stores after a fallback means the same as no value and is left alone.

The window scales every pixel size and font by one factor (system DPI / 96). Check boxes are never disabled: a
disabled check box draws its text etched in system colours, which on the dark theme gave the faded, doubled switch
titles seen at 120/144 DPI on the lab; the Applications page hides the switches until an application is selected.

## Recovery

`src/Recovery.cs` holds the rules as pure functions over one snapshot (the interop, start-health and DPM escapes, the
`Parameters` values, the release's `DesktopRouter` key, the modules of the running DWM, the start-confirm task's
last result and `start-confirm.log`, and the `"defaults"` of `<InstallDir>\manifest.json`). The window, the dry run and
the elevated helper plan every action through it, so the confirmation dialog, `--dry-run` and the helper's writes are
the same list. The states: driver start (confirmed or not, `UnconfirmedStarts` against the limit of 2, `LastStage`),
GPU desktop path (`EnableGpuPresentBlit`, `EnableCddDwmInterop`, `InteropClosedReason`, the escape's effective
switches, users and last session end), desktop composition (`DwmForceCpu` and the route DWM really loaded), clock
control (`DpmMode`, `DpmPending`, `DpmConfirmed`, the running mode and the reason of a fallback), the start
confirmation task, and the desktop compositor (below).

No action restarts, stops or signals DWM (BD-060): on this Windows 11 build a forced DWM restart in a session leaves
WinUI 3 content (the Explorer command bar, Task Manager) without mouse input until a new boot, and restarting Explorer
does not help. Every change is written and takes effect at the next restart of Windows; the window then offers
"Restart now", a normal restart through `ExitWindowsEx` (`EWX_REBOOT`, planned) after a confirmation, so programs can
keep unsaved work. A unit test reads every source file and fails on `Kill()` outside the bug report's own child tool,
on `TerminateProcess`, `taskkill`, `shutdown.exe` or a forced restart, and on any use of the `dwm` process other than
reading it.

| Action (`--action`) | Writes | Takes effect | Refused when |
|---|---|---|---|
| `reopen-gpu-path` | `EnableGpuPresentBlit` 1, `EnableCddDwmInterop` 1, delete `InteropClosedReason` | next restart (offered) | open already, or reopened and waiting for the restart |
| `desktop-gpu` | `DwmForceCpu` 0 | next restart (offered) | the driver is not running; the effective switches (escape and `InteropLastState` & 3) are not both on (points to `reopen-gpu-path`); the GPU desktop files are missing; DWM is on the GPU route already; the GPU route is selected already (restart Windows) |
| `desktop-cpu` | `DwmForceCpu` 1 | next restart (offered) | DWM is on the CPU route already; the CPU route is selected already (restart Windows) |
| `confirm-start` | start-health CONFIRM (clears `UnconfirmedStarts` and `DpmPending` in the KMD); not undoable | at once | the driver is not running; confirmed already; not eligible by `Test-StartConfirmEligible` of the installer's `start-confirm-core.ps1` (flags 7, completions, ready >= 60 s, last completion <= 5 s; the helper retries the reading for 10 s) |
| `enable-dpm [--ceiling N]` | `DpmMode` 1, and `DpmMaxMHz` N only when a ceiling is chosen | next restart (offered) | N is not 1000-2000 on the 100 MHz grid; stored already |
| `set-clocks --mode 1\|unset --ceiling N\|unset` | `DpmMode` 1 or removed, `DpmMaxMHz` N or removed (the Performance page) | next restart (offered) | as above, or a mode other than 1 (the fixed clock is the unchecked default) |
| `reset-defaults` | the manifest's defaults of `EnableGpuPresentBlit`, `EnableCddDwmInterop`, `DpmMode`, `DpmMaxMHz` and `DwmForceCpu`; delete `InteropClosedReason` | next restart (offered) | the manifest has no `"defaults"`, or one is missing or out of range; all at their defaults already |
| `undo` | the values the newest undoable backup found | next restart (offered) | nothing to undo; the backup names a value outside the list; it would put DWM on the GPU route with the switches closed |

The only values any action or undo may write are those in the table (`Recovery.Allowed`): never the temperature
limits, firmware paths, other KMD service values, BIOS or firmware settings, or test signing. The reset takes no
value from this app: without `"defaults"` in the manifest it is refused.

The elevated helper plans again from its own reading, saves the old values to
`%ProgramData%\amdgpu-wddm\control\backup-<utc>.json` (the directory is writable by administrators and SYSTEM only,
since undo applies these files), writes, logs every step to `control-actions.log` in the same directory, reads every
value back (a failed write restores the backup at once) and reports the result. Exit codes: 0 done, 1 failed,
2 usage, 3 refused, 5 needs administrator (4, "fell back to the CPU route", belonged to the DWM watchdog of 0.3 and
earlier, which restarted DWM).

```powershell
amdgpu_wddm_control.exe --action reopen-gpu-path --dry-run --out plan.txt    # states and plan, nothing written, no UAC
amdgpu_wddm_control.exe --action desktop-gpu --dry-run --snapshot snapshot.json --out plan.txt
```

The exe is a window program: from PowerShell use `--out`, or `Start-Process -Wait`. A real run (no `--dry-run`) writes
its log lines and `exit N` to `--out` as well.

A close mark (`InteropClosedReason` 4) means different things by driver version. From KMD 0.7.198
(`BC250_KMD_VERSION` 0x000700C6) BD-059 is fixed and a normal restart keeps the path open, so the page says the last
session ended without a clean shutdown (power loss, crash or reset); on 0.7.197 and earlier it names BD-059. The
version comes from the driver's interop, start health or DPM reply; without one, both causes are named.

## Desktop compositor restarted (BD-060)

The Overview's "Desktop compositor" row, the first Recovery state, `--status` and the bug report's
`recovery-states.txt` say whether the interactive session's DWM is the session's first one. Winlogon starts the first
DWM with the session; a DWM that started more than 120 s after the session's `winlogon.exe` replaced it (killed,
crashed, or the display driver was restarted, for example by a driver update without a reboot). Without a readable
winlogon start, the reference is the boot with a 300 s bound. A restarted compositor reads "Desktop compositor
restarted in this session (DWM started <time> after the session began). Some Windows 11 apps, for example the Explorer
command bar and Task Manager, ignore mouse clicks until Windows restarts (BD-060).", with "Restart Windows" as the
remedy. From session 0 (an elevated script over SSH) the console session is read.

```powershell
amdgpu_wddm_control.exe --status --out status.txt    # no window, nothing written, no UAC
```

The second line of the file is for scripts such as the installer's verify:
`compositor-restarted: yes|no|unknown (session N, DWM started <utc>, <s> s after the session, <s> s after boot)`,
followed by every Recovery state.

## Bug report

`driver-state.txt` (the snapshots above), `driver-log.txt` (the whole log ring), `installed-files.txt` (Device
Manager status and problem code, the display driver key, every component with version and SHA-256), `settings.txt`
(the driver's `Parameters` values and the D3D12 profiles), `system.txt` (Windows build, test signing, app version),
`events.txt` (System events of the display stack and Application crash records that name it, last 24 hours),
`manifest-check.txt` (every component of `<InstallDir>\manifest.json` hashed and marked OK, MISMATCH, MISSING or
UNRESOLVED), `release/start-confirm.log`, `release/installer-state.json` and the three newest `install-*.log` and
`verify-*.json` (from `%ProgramData%\amdgpu-wddm`, the last 2 MB of each), `dxdiag.txt` (`dxdiag /t`), `d3d12-caps.json` (`amdgpu_wddm_d3d12caps.exe 0` when installed next to the app) and
`vulkan-summary.txt` (`vulkaninfo --summary` when found). Every text passes `Redactor` first: user name and profile
path, computer name, MAC addresses, e-mail addresses and the values of lines that name a serial number, machine id,
product id or UUID are replaced. Versions, hashes, LUIDs and PnP hardware ids stay.

## Build

```powershell
pwsh tools\win\amdgpu_wddm_control\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget -Out <BC250_ROOT>\scratch\release\control-app\build\app
```

Compilers from the installed Visual Studio (Roslyn `csc` for .NET Framework 4.8, which Windows 10 and 11 carry, and
`cl` for the DLL and the CLI), headers and import libraries from the SDK NuGet packages. Deterministic output. The gates, each of
which stops the build: `test/UnitTests.cs` (reply offsets computed from the text of `driver/kmd/bc250kmd_escape.h`,
the profile catalog against every `ddi_experiment("...")` call in `driver/umd/d3d12`, profile editing, DPM ranges
against `docs/design/dpm.md`, redaction, the manifest, and every Recovery rule and refusal, with its constants checked
against `bc250kmd_escape.h`, `bc250kmd.h`, `interop_policy.h`, `interop.c` and `dpm.c`; with
`-StartConfirmCore <path>` also against the installer's `start-confirm-core.ps1`), the compiles with warnings as
errors, `--smoke` (the pages built and refreshed once without a window, which on a PC without a BC-250 must say
"Driver not found"), `--smoke-report` (a report without dxdiag, capability tools and events, checked for its files and
for this PC's user and computer name) and the Recovery dry runs (every action planned from `test/snapshot-bd059.json`,
the state BD-059 leaves behind on KMD 0.7.197, against its expected writes or refusal; a snapshot without `--dry-run`
refused; one dry run of this PC; where the build is not elevated, one real run that must stop with exit 5 and still
write `--out`) and `--smoke-render <dir> <scale>` at 1, 1.25 and 1.5 (every page drawn to a PNG without a
window, with an unsaved demo profile on Applications, and the whole page as `<page>-full.png`; the build fails when
two sibling controls overlap, a page is wider than the window or its last control lies outside the scroll range).

The output folder holds `amdgpu_wddm_control.exe`, `bc250control.dll` and `bc250kmd_cli.exe` (the same translation unit
as the DLL, without `BC250_CONTROL_DLL`, so the CLI and the DLL of a release come from one build); the release
installer puts the first two, and `amdgpu_wddm_d3d12caps.exe`, in one directory, and the CLI and a copy of the DLL in
`tools`.
