# Tester release package

Builds the folder and zip that external testers run on their own ASRock BC-250 (Windows 11 x64). The tester guide
is `docs/testing/INSTALL.md`; it is copied into the package as `INSTALL.md`. The package's own third-party list is
`docs/testing/THIRD-PARTY.md` (one row per bundled component with its exact commit), with the licence texts in
`docs/testing/licenses/`; when a bundled binary changes, its row changes in the same commit.

| File | Purpose |
|---|---|
| `new-release-cert.ps1` | Creates the release test-signing certificate once, in a private directory outside the repository (never in the repo or the package). Separate from the lab certificate. |
| `release-sources.json` | The registered lab artifacts that make up the release, with SHA256. The build refuses any other byte. |
| `build-release.ps1` | Copies the sources, re-signs the KMD (.sys signature, new catalog via Inf2Cat), writes `manifest.json`, zips. Gates: source hashes, signer, no key material, scripts parse under PowerShell 5.1, every licence file named in `THIRD-PARTY.md` present and none of them a web page. |
| `installer\` | What the tester runs: `install.cmd`, `uninstall.cmd`, `verify.cmd`, `prepare-offline.cmd` (package root) and the PowerShell 5.1 scripts; with `-SetupApp`, also the setup window `setup\amdgpu_wddm_setup.exe` (`tools/win/amdgpu_wddm_setup`). |
| `test-parse51.ps1` | Gate: every script parses under Windows PowerShell 5.1. |
| `test-cli-commands.ps1` | Gate (build and `test-dryrun.ps1`): the packaged `bc250kmd_cli.exe`, run without arguments, lists every form of `cli-commands.json` (what the installer and the lab kits call), and it and both copies of `bc250control.dll` come from one control-app build folder. |
| `test-dryrun.ps1` | Host test on a PC without a BC-250: install and uninstall dry runs refuse cleanly and change nothing; `-DryRunIgnoreBoard` walks every phase. Never run the real install on a development PC. |
| `test-firmware.ps1` | Called by `test-dryrun.ps1` under 5.1, inside a scratch folder: both download hosts answer, a real download of the 8 firmware files and `LICENSE.amdgpu` with each SHA256 checked, the same from a folder (`-FirmwareDir`), and the refusal of a file whose SHA256 is not the pinned one. Installs nothing. |
| `test-registry-defaults.ps1` | Called by `test-dryrun.ps1` under 5.1: the upgrade rule for the registry defaults (new, unchanged, new default over a value the previous installer wrote, a tester's value kept, command line, installer-owned), the driver's own safety closures (BD-069: a repair reopens them and clears the record, another install keeps them and reports them, a value the tester set by hand stays kept), the `Release\AppliedDefaults` round trip, and a write and read-back in the scratch key `HKCU:\Software\amdgpu-wddm-installer-test`, removed at the end. |
| `test-session-checks.ps1` | Called by `test-dryrun.ps1` under 5.1 (BD-060): the INF `Reboot` directive (found through `[Manufacturer]` and its models, added once after each install section header, line endings kept, present in the packaged INF), the pnputil outcomes 3010 / 0 / 259 (only what the exit code establishes; no installer function for the KMD's BD-059 session marker), the install inputs that the argument-free run after a restart takes from the state (an offline fresh install and an upgrade, both resume phases, the resumed run's own arguments, another package version, cleared at completion, a tester.10 state), the `resume` action, and the DWM baseline (`dwm-session.ps1`, files under `-WorkRoot` only): one whole record per boot, session and logon (a record with a missing or malformed field is ignored and replaced by the next recording), a replacement only when another instance than the recorded one runs, unknown history without a record, the upgrade's before/after observation; plus a read-only reading of this computer's own session. |
| `test-engine-units.ps1` | Called by `test-dryrun.ps1` under 5.1: the RunOnce command line, the continuation closure and its command (setup window, or Windows PowerShell with `install.ps1 -HoldWindow`, run for real from folders with spaces and cmd metacharacters), the kept repair set (each set against its own manifest's firmware), the running-release witness writer under the engine lock, the compatibility record, the restart-boundary decision, the mutation record and the job object's child closure. |
| `test-engine-events.ps1` | G-EVT and G-STAGE: plan and dry runs with `-Gui`, every event line and the terminal result against the contract (`docs/gui/interfaces-setup.md`), the deadline and the child-process closure (section 10), the footprint unchanged. |
| `test-offline.ps1` | G-OFF: `prepare-offline.ps1` builds a prepared folder; install dry runs from it and from a kept repair set with every download failing as if offline; a missing or changed firmware file is refused before any change. |
| `test-wow64.ps1` | Called by `test-dryrun.ps1` under 5.1 (BD-064): every image under `payload\wow64` and `payload\syswow64` is x86 and every other payload image x64, the x86 entry points are exported undecorated, the `manifest.json` install paths, and verify's 32-bit registration check (`Test-WowRegistration`) against the scratch key `HKCU:\Software\amdgpu-wddm-installer-test-wow` and files under `-WorkRoot`, both removed at the end. |
| `test-filesafe.ps1` | Called by `test-dryrun.ps1` under 5.1, inside a scratch folder: equal-SHA256 skip, replacement of a file in use by rename, a re-run over a partial install, and the failed-step message with its re-run hint. |

```
pwsh -File tools\release\new-release-cert.ps1                 # once
pwsh -File tools\release\build-release.ps1 [-SetupApp <setup build folder>]   # the control app comes from its release-sources.json rows
pwsh -File tools\release\test-dryrun.ps1 -Package <BC250_ROOT>\scratch\release\out\amdgpu-wddm-tester-<version>
```

v0 ships the binaries as they run on the lab (tester.11: KMD 0.7.200.1 re-signed, code unchanged, INF DriverVer 0.7.200.100 so that it outranks every lab build of 0.7.200 and the bound package is identifiable, plus the INF `Reboot` directive; desktop on the GPU route, DwmForceCpu 0, gated by RequireKmdSwitches); a rebuild from
the exact commits with `/Brepro` is planned for v1. Install paths differ from the lab's: everything under
`%ProgramFiles%\amdgpu-wddm`, except the firmware, which the KMD reads from the compiled-in `C:\BC250\firmware`.

v1 items: the KMD reads its firmware from the driver store (INF `DestinationDirs`) instead of the hard-coded
`C:\BC250\firmware`; reproducible rebuild of every component; the control application edits the D3D11 allowlist; the
.sys file version matches the INF DriverVer (the release re-stamps only the INF: 0.7.200.100 against the file's
0.7.200.1). Done for tester.11 (KMD 5116c058 = 0aec4c58 + the DPM warm zone, with the INF strings of 1fccb978): the adapter string is "BC-250 GPU (amdgpu-wddm)" (it was
"BC-250 GPU (bc250kmd, display-only, lab build)") and the INF Provider is "amdgpu-wddm" (it was "BC-250 lab (D-Ogi)").

No device restart and no DWM restart under a running desktop (BD-060): WinUI pointer-input loss after the desktop
compositor (DWM) is terminated and restarted reproduces on this Windows build (22631) also with Microsoft Basic
Display; restarting Windows recovers (sign-out is not verified).
`build-release.ps1` adds the INF `Reboot` directive to each install section of the packaged INF, so Windows does not
restart a started GPU for the package (Windows 8 and later, INF Reboot directive) and the GPU changes driver at the
restart that ends phase 2; `install.ps1` refuses a package INF without it. pnputil's exit code is read only for what
it establishes (0 completed, 3010 restart required, 259 no device change reported); the binding check decides. When
the GPU still names the old service after 3010, phase 2 continues after the restart (`driver-pending-restart`,
action `resume`), and the inputs of the install (`-FirmwareDir`, `-DpmMaxMHz`, `-CuMode`, `-NoControlApp`,
`-NoReboot`, `-Force`) come from the state, as after the test-signing restart; they are cleared when phase 2
completes. A newer package started over an older one's unfinished phase 1 takes the installation over: the state
names it from then on (its inputs come back after its own restarts), test signing is still finished first, and the
run after the test-signing restart is the newer package's (RunOnce armed again). `-Repair` is one of the inputs that
come back, because a repair reopens the driver's own closures (below). The installer never changes the KMD's BD-059
session marker. The control application (INSTALL.md) or a repair install opens a closed GPU desktop path again.

The driver's own safety closures (BD-069): the KMD writes a release default away itself after a start it must not
repeat, and records that act next to the value (`InteropClosedReason` for `EnableGpuPresentBlit` and
`EnableCddDwmInterop`, `DpmLastReason` 3 or 4 for `DpmMode`). `common.ps1` `$script:DriverClosures` holds the value
each closure writes, its record and its reason words. A value at the closure value with its record is the driver's
act: `install.cmd -Repair` writes the release default again and clears `InteropClosedReason` (decision `reopened`),
every other install keeps it and reports it with its remedy (decision `driver-closed`), and a value the tester set
by hand, with no record, stays `kept` as before. The reason code stays in the installer's log. The window and the
tester's report get the act in plain words. The KMD's own INF stays without `Reboot`, because the lab's deployment kits
restart the device in place. Uninstall still moves the GPU to Microsoft Basic Display Adapter at once and asks for a
restart. Install records the session's DWM (process ID and creation time) before the driver package, right after it
(before the restart-pending branch) and at the end of phase 2, each with its boot, in `dwm_observations` in
`state.json` (last 12); the device outcome after the package is a separate field: a device-loss event and a DWM
process restart are separate observations. The start-confirm task records the session's DWM once per logon in
`%ProgramData%\amdgpu-wddm\dwm-baseline.json` (boot, session and logon time; last 16 records; a record with a missing
or malformed field does not count and the next recording replaces it; `installer/dwm-session.ps1`, also in
`payload\tools`). `verify.cmd` reports `DWM restarted in this session` as a
warning only when it sees another instance than the recorded one, passes when the recorded one runs, and says
`unknown history` without a record; no time heuristic. The warning helps to attribute a failure, never replaces a bug
report.

Registry defaults: `installer/registry-defaults.json` is the one table. `install.ps1` applies it and
`build-release.ps1` copies its `defaults` object into `manifest.json`, where the control application's reset reads
it. An upgrade writes a default only where the value is absent or still equal to what the previous installer wrote
(`Release\AppliedDefaults`; `legacy_applied` for tester.1 to tester.7, which kept no record), so a tester's own
settings stay. Installer-owned and always written: the paths into the install root (`CpuUmdPath`, `GpuUmdPath` and the
x86 router's `CpuUmdPathWow`, `GpuUmdPathWow`), the graphics registration (`UserModeDriverName`, `VulkanDriverName`, the
Khronos entry, and their 32-bit counterparts `UserModeDriverNameWow`, `VulkanDriverNameWow`, the WOW6432Node Khronos entry), `UnconfirmedStarts` and the
`Release` key. When a default changes, edit `defaults` only; `legacy_applied` stays as tester.7 wrote it.

install.ps1 exit codes: 0 done (or already installed and verified, or a restart asked for), 2 refused (preflight, or
no administrator rights in a setup-window run), 3 verification failed, 4 not confirmed, 5 test signing not active,
6 a step failed (run again), 7 verify before the pending restart, 8 stopped at a safe point (cancelled, or past `-DeadlineUtc`),
9 another install action holds the engine lock, 10 handed to an elevated window.

Setup window and engine (`docs/gui/interfaces-setup.md`): the setup window runs `install.ps1` with `-Gui` (events
and a terminal result bound to its invocation id, cancel at safe points, no prompts) and `-Plan` (read only: checks,
decision, settings-impact plan). Restarts are planned, never forced: on the command line `install.ps1` asks "Restart
now?" and then asks Windows for a normal restart (ExitWindowsEx with a planned reason; programs may keep unsaved
work); the setup window asks the same in its own words. Each install stages a continuation closure (the whole package
and its firmware folder) in the installer's state folder; RunOnce runs the closure's setup window with `--continue`,
or, for a command-line run, Windows PowerShell directly with the closure's `installer\install.ps1 -HoldWindow` (no
cmd.exe, so a folder such as `C:\AMD&GPU` is safe). After completion the closure stays as the repair set (this release and
the previous one) and `Release\RepairSetup` names its setup window. `prepare-offline.cmd` makes a folder with the
package and the firmware for a BC-250 without internet. `build-release.ps1` writes `compatibility.json` (engine
contract, the INF `Reboot` directive, firmware list, settings table schema, the continuation) and refuses a package
whose record does not verify; it takes the release notes from `docs/testing/release-notes/<version>.md` (sections New,
Fixed, Known issues, Settings affected required) as `RELEASE-NOTES.md`. Verify and the start-confirm task write the
running-release witness `%ProgramData%\amdgpu-wddm\installer\running-release.json` (the loaded KMD image, its reply
and the boot).

`build-release.ps1` and `test-dryrun.ps1` need PowerShell 7 (`headless.ps1` starts every child process without a
window, with stdin closed and a time bound). The installer itself is Windows PowerShell 5.1.

PROVENANCE: vulkaninfo.exe 1.4.335 from Khronos Vulkan-Tools (LunarG build), Apache-2.0.
PROVENANCE: linux-firmware `amdgpu/cyan_skillfish2_*.bin` at 2b8daaf611fbade74f26a5b58ec1defe6a02f5e0, redistributable per `LICENSE.amdgpu`. Not in the package and never committed: the installer downloads the files (kernel.org, GitLab mirror) or takes them from `-FirmwareDir`, checked against `tools/firmware/cyan_skillfish2.json`.
