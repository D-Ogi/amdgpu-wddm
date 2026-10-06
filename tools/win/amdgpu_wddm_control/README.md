# amdgpu-wddm Control

The control application for testers of the amdgpu-wddm driver on the ASRock BC-250. It runs on the tester's own PC,
by hand, like the vendor's control panel: a normal window, no service, no autostart, no network, no lab tools.

The window speaks to ordinary users (GUI-PLAN-v7, phase 1): plain words, no driver internals, error codes or internal
versions. A unit test and the render gate check every visible text against `PlainWords.Internals`; technical detail
goes only into the support report and the Support page (shown when "Show support options" is on). Texts come from
`strings/strings.<lang>.txt` (English is the source; Polish, Japanese and Korean are machine translations marked `mt`).

| Page | What it shows or changes |
|---|---|
| Home | One status card with one recommended action, Getting started, recent games, monitor, driver and graphics summaries |
| Games | One entry per game file name (recent launches, stored profiles, the release's defaults, added games); per game the D3D12 application profile as grouped check boxes with origin and cost, Apply/Discard, Undo/Redo and "Recommended" |
| Graphics | Automatic clocks and the ceiling (1000-2000 MHz on the 100 MHz grid), the compute unit choice (section 7 of the plan), Restore defaults (keep or reset the games) with undo; features the driver cannot do yet as "Coming later" |
| Display | The monitors Windows reports, Identify, a link to the Windows display settings; mode, scaling, HDR and VRR as "Coming later" |
| Performance | Live temperature, clock and load (every 2 s, only while shown and not minimized), the shader cache sizes; power, fan and voltage as "No reading" or "Coming later" |
| Driver | The installed release, whether Windows runs it, the update check (Settings can turn the check at start off) |
| Settings | Language, update check at start, recent launches, Nagi, tips, animations, support options, what data the app keeps |
| Help | Four symptom guides, one explanation per setting (through search), Repair, Restart Windows, the support report, About |
| Support | The driver's start and recovery states in English, the technical actions (below), the installed components |

`--recovery` opens a small recovery view that never loads `bc250control.dll`: the installed release, the symptom
guides, Repair, Restart Windows and the support report.

## How it talks to the driver

Through `bc250control.dll`, built from `tools/win/bc250kmd_cli/bc250kmd_cli.c` with `BC250_CONTROL_DLL`. Only
software-state escapes, each with `NoAdapterSynchronization` alone: the DPM snapshot (`Bc250Dpm`), start health
(`Bc250StartHealth`, op READ), the GPU DWM interop decision (`Bc250Interop`), dxgkrnl's segment statistics
(`Bc250VideoMemory`) and the log ring (`Bc250LogRead`, `GET_LOG`, answered without the adapter lock from 0.7.184.1).
No `HardwareAccess` escape and no `LOG_SUMMARY`: a Level Two escape idles the GPU, and an overlay that polled one every
5 s stalled a running game for 300 ms each time (BD-054). Live values are read every 2 s, and only while Overview or
Performance is shown and the window is not minimized.

The one escape that changes the driver is the start-health CONFIRM of "Confirm this start" (Recovery), the same
request the release's logon task sends: administrator only, once per action, and from 0.7.213 with
`NoAdapterSynchronization` as well - it writes the registry and touches no register. DPM settings are
`DpmMode` and `DpmMaxMHz` under
`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`, which the KMD reads at device start
(`docs/design/dpm.md`, "Settings and boot guard"): the page says to restart Windows. The runtime tuning escapes
(`dpm tune`, `dpm floor`) are lab tools and not offered. The boot guard can set `DpmMode` back to 0 after an unclean
or unconfirmed start; the page shows the last start's reason.

The window runs as the invoking user. A change starts an elevated copy of the program (one UAC prompt) with one verb,
`--action <name>` (a game's settings: `--action game-profile --image <name.exe> --value <switches>`, an empty value
removes the game's key; `game-undo` and `game-redo` take `--image`), which checks its arguments with the same
functions as the window and reads the value back.

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
switches, users and last session end), desktop composition (the selected route, `DwmForceCpu`, and the active route,
the modules the running DWM loaded), clock
control (`DpmMode`, `DpmPending`, `DpmConfirmed`, the running mode and the reason of a fallback), the start
confirmation task, and the desktop compositor (below).

No action restarts, stops or signals DWM (BD-060): on this Windows 11 build a forced DWM restart in a session leaves
WinUI 3 content (the Explorer command bar, Task Manager) without mouse input until a new boot, and restarting Explorer
does not help. Every change is written and takes effect at the next restart of Windows; the window then offers
"Restart now", a normal restart through `ExitWindowsEx` (`EWX_REBOOT`, planned) after a confirmation, so programs can
keep unsaved work. The helper reports such a change as "written, pending until the next restart of Windows", never as
a completed switch. A unit test reads every source file and fails on `Kill()` outside the bug report's own child tool,
on `TerminateProcess` outside the operator escape (below), on `taskkill`, `shutdown.exe` or a forced restart, on any
use of the `dwm` process other than reading it, on a sign-out remedy, and when the window names the escape.

The desktop composition state keeps the selected route and the active route apart. The helper removes
`route-written.json` (control directory, owned by Administrators) before it writes `DwmForceCpu` and writes it again,
with the time and the value, only after the value reads back; a failed or rolled-back attempt leaves no record, and a
backup alone is never taken as a write. When that record holds the value selected now and is newer than the running
DWM, the route is "pending until Windows restarts". When the DWM that started after the write loaded the selected
route, the state says so ("the DWM that started after the change loaded it"); when it loaded the other route, the
state is a warning that asks for a bug report. Without such a record the timing is unknown and neither is said. The
active route comes from the modules of the one DWM the state names (process id, session and creation time checked
through the same process handle before and after the module list; a change during the read gives "unknown"), never
from another session's DWM. The module list counts only when it is complete: the buffer grows to the size the system
reports (at most 4 tries, at most 16384 modules), and a failed enumeration or a module name that cannot be read
gives "unknown", because a list without the Zink library would read as the CPU route. Reading it needs administrator; without it the state says that the active route cannot
be read, and never calls a route active. A healthy GPU route recommends nothing (the CPU route stays available on the
Actions page); `desktop-cpu` is recommended only when the GPU route may be failing, that is when a DWM replacement was
observed in this session while the GPU route is selected.

| Action (`--action`) | Writes | Takes effect | Refused when |
|---|---|---|---|
| `reopen-gpu-path` | `EnableGpuPresentBlit` 1, `EnableCddDwmInterop` 1, delete `InteropClosedReason` | next restart (offered) | open already, or reopened and waiting for the restart |
| `desktop-gpu` | `DwmForceCpu` 0 | next restart (offered) | the driver is not running; the effective switches (escape and `InteropLastState` & 3) are not both on (points to `reopen-gpu-path`); the GPU desktop files are missing; DWM is on the GPU route already; the GPU route is selected already (restart Windows) |
| `desktop-cpu` | `DwmForceCpu` 1 | next restart (offered) | DWM is on the CPU route already; the CPU route is selected already (restart Windows) |
| `confirm-start` | start-health CONFIRM (clears `UnconfirmedStarts` and `DpmPending` in the KMD); not undoable | at once | the driver is not running; confirmed already; not eligible by `Test-StartConfirmEligible` of the installer's `start-confirm-core.ps1` (flags 7, completions, ready >= 60 s, last completion <= 5 s; the helper retries the reading for 10 s) |
| `enable-dpm [--ceiling N]` | `DpmMode` 1, and `DpmMaxMHz` N only when a ceiling is chosen | next restart (offered) | N is not 1000-2000 on the 100 MHz grid; stored already |
| `set-clocks --mode 1\|unset --ceiling N\|unset` | `DpmMode` 1 or removed, `DpmMaxMHz` N or removed (the Performance page) | next restart (offered) | as above, or a mode other than 1 (the fixed clock is the unchecked default) |
| `reset-defaults [--games keep\|reset]` | the manifest's defaults of `EnableGpuPresentBlit`, `EnableCddDwmInterop`, `DpmMode`, `DpmMaxMHz` and `DwmForceCpu`; delete `InteropClosedReason`; Standard (24) graphics cores; with `--games reset` every game profile set to the release's recommendation or removed | next restart (offered) | the manifest has no `"defaults"`, or one is missing or out of range; `--games reset` without the release's game list; all at their defaults already |
| `undo` | the values the newest undoable backup found (not per-game changes, not the graphics-core part) | next restart (offered) | nothing to undo; the backup names a value outside the list; it would put DWM on the GPU route with the switches closed |
| `cu-mode --cu 24\|40` | the graphics-core values through `CuMode.Plan` (plan section 7); not undoable, the old values are kept for diagnosis | next restart (offered) | no choice; the stored state cannot be read or is the choice already |
| `cu-confirm` | the start-health confirmation of a 40-core start | at once | the driver is not running; no 40-core start waits |
| `game-profile --image <exe> --value <list>` | `Experiment` of `HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<exe>` (an empty list removes the key) | next start of the game | not a file name or a catalog list; stored already |
| `game-undo\|game-redo --image <exe>` | the game's value before its newest change (undo) or before its newest undo (redo) | next start of the game | nothing to undo or redo for that game |
| `restart-compositor --accept-bd060` | nothing; stops the active session's DWM, Windows starts a new one (operator escape, not in the window, not undoable) | at once | `--accept-bd060` is missing |

The only values any action or undo may write are those in the table (`Recovery.Allowed`): never the temperature
limits, firmware paths, other KMD service values, BIOS or firmware settings, or test signing. The reset takes no
value from this app: without `"defaults"` in the manifest it is refused.

The elevated helper plans again from its own reading, saves the old values to
`%ProgramData%\amdgpu-wddm\control\backup-<utc>.json` (the directory is writable by administrators and SYSTEM only,
since undo applies these files), writes, logs every step to `control-actions.log` in the same directory, reads every
value back (a failed write restores the backup at once) and reports the result. Exit codes: 0 done, 1 failed,
2 usage, 3 refused, 5 needs administrator (4, "fell back to the CPU route", belonged to the DWM watchdog of 0.3 and
earlier, which restarted DWM).

### Operator escape: restart-compositor

For a desktop that does not respond (no window draws, no input) when Windows cannot be restarted the normal way, an
operator can restart the desktop compositor from an elevated prompt or over SSH:

```powershell
amdgpu_wddm_control.exe --action restart-compositor --accept-bd060 --out escape.txt
```

Without `--accept-bd060` the action is refused (exit 3). It changes no setting: for the CPU route after the restart,
run `desktop-cpu` first. The helper records the running DWM and opens one handle to it (query, terminate and
synchronize access), checks the process id, session, creation time and that it still runs through that handle, and
terminates that same handle; any mismatch refuses and stops nothing. It then waits up to 20 s for a DWM in the same
session created after the stopped one, records it for that session and logs both. Warning:
after a DWM restart some Windows 11 apps, for example the Explorer command bar and Task Manager, can ignore mouse
clicks until Windows restarts (BD-060). Restart Windows as soon as you can. The window never offers this action.

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

## Desktop compositor replaced (BD-060)

The Overview's "Desktop compositor" row, the first Recovery state, `--status` and the bug report's
`recovery-states.txt` report only a DWM replacement that an observer saw. Each observer reads the active interactive
session's DWM (process id and creation time, from the system's process list; from session 0 the console session),
the boot (`BootId` under `Session Manager\Memory Management\PrefetchParameters`) and the session start (creation time
of the session's first `winlogon.exe`), and records it in `dwm-observations.json` (schema 2, one record per session
epoch, the newest 8): the user's copy in `%LOCALAPPDATA%\amdgpu-wddm`, the administrator's copy in
`%ProgramData%\amdgpu-wddm\control` (owned by Administrators). Each write re-reads the copy, merges and replaces it
(a unique temporary file and `File.Replace`) under one named mutex per copy; a lock not taken within 2 s skips that
write and the file keeps its history. Both copies are merged; only the record of this boot, session and session start
counts, so a new session starts a new baseline. Every entry is validated before it counts: a process id above 0, a
session above 0, creation, first-seen and session-start times that parse; a damaged entry is dropped and a live reading
with a damaged field records nothing, so neither can make a replacement. Every stored epoch and instance is checked
before the epochs are ordered and kept, so a damaged one is dropped (and the file rewritten without it) instead of
stopping later writes. The installer's record, `%ProgramData%\amdgpu-wddm\dwm-baseline.json` (written by its
start-confirm task at each administrator logon, format in `tools/release/installer/dwm-session.ps1`), is merged too and
never written: its record of this epoch (the same session, boot time and logon time within 2 s) counts as an
observation by "the installer's start-confirm task at logon" at its `recorded_utc`; a record of another epoch is not a
replacement, and a damaged file or record is ignored. Two readings name the same DWM when the process ids match and
the creation times are at most 1 s apart. The observers: the window (every 2 s while it is open), `--status`, the bug report,
the elevated helper and the operator escape. The dry run reads but records nothing.

| Verdict | When | Text |
|---|---|---|
| `observed` (warning, "Restart Windows") | more than one DWM seen in this session | "The desktop compositor (DWM) was replaced in this session: N replacement(s) seen; the DWM now running (process P) started T. After a DWM restart some Windows 11 apps, for example the Explorer command bar and Task Manager, can ignore mouse clicks until Windows restarts (BD-060): restart Windows. If nobody restarted DWM on purpose, create a bug report: a DWM crash can be a driver defect." |
| `unknown-history` (information) | one DWM seen so far | "No replacement of the desktop compositor (DWM) seen since T (first seen by O, D after the session began). A replacement before that would not be seen." |
| `unknown` (information) | the session's DWM, winlogon or the boot cannot be read | "The desktop compositor (DWM) of the active session cannot be read." |

A first observation is never "restarted" and never "healthy": a DWM that started long after its session can be the
session's first one, and the history before the first observation is unknown. Restarting Windows is the only remedy
named. The release's start-confirm task can run `--status` at logon to start the record early.

```powershell
amdgpu_wddm_control.exe --status --out status.txt    # no window, no UAC; records the session's DWM
```

The second line of the file is for scripts such as the installer's verify:
`dwm-restart: observed|unknown-history|unknown (boot B, session N, session start <utc>, DWM process P started <utc>,
instances seen K, watched since <utc> by <observer>)`, with `-` for a value that cannot be read, followed by every
Recovery state. `AMDGPU_WDDM_CONTROL_STATE=<dir>` moves both copies of the record to one directory (the build's
gates use it, so a build records nothing in the profile of the PC that builds).

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
write `--out`), `--smoke-recovery <file>` (the recovery view built without a window; fails when `bc250control.dll`
is loaded), `--smoke-perf <file>` (the window built hidden: no live timer, no ticks, no animation frame, start under
5 s, private memory under 300 MB) and `--smoke-render <dir> <scale> [--lang xx] [--text-scale f] [--nagi]
[--switch-to xx] [--fixture <snapshot.json>]` in 23 runs: four languages at 1, 1.25, 1.5 and 2, with
`test/snapshot-bd059.json` as the fixture, text at 150 %, Nagi shown, and two language switches. Every page is drawn
to a PNG without a window at the default size and at the minimum size, and the whole page as `<page>-full.png`. The
build fails when two sibling controls overlap, a text is cut, a page is wider than the window or its last control
lies outside the scroll range, a visible text names a driver internal, a control lacks an accessible name or tab
stop, the art shows while "Show Nagi" is off, or a language switch leaves old texts behind. The unit tests also check
every string table: each id in each language, no stale translation (G-STR), no internals in the texts (G-NOINT).

`-NagiArt <dir>` embeds the guide character's art (`nagi.<expression>@128.png` and `@256.png`, from the files that
`Guide.ArtFile` names) as resources. The art is not part of this repository; without `-NagiArt` the guide panel shows
text only.

The output folder holds `amdgpu_wddm_control.exe`, `bc250control.dll` and `bc250kmd_cli.exe` (the same translation unit
as the DLL, without `BC250_CONTROL_DLL`, so the CLI and the DLL of a release come from one build); the release
installer puts the first two, and `amdgpu_wddm_d3d12caps.exe`, in one directory, and the CLI and a copy of the DLL in
`tools`.

## Review oracles (G-CU, G-VER, G-ART)

`test/OracleTests.cs` checks this application against three oracles that the reviewer wrote from the plan alone:
`oracle-cu.json` (the compute-unit page), `oracle-ver.json` (the version and update rules) and `oracle-art.json` (the
guide art). They are **not** in this repository and are never copied into it, because a copy here would let the
implementation be fitted to them. `build.ps1 -Oracle <dir>` passes the directory, `AMDGPU_WDDM_ORACLE` carries it to
the tests, and without it the three checks print "oracles: not given" and skip.

The directory is `<BC250_ROOT>\scratch\gui\codex`, the reviewer's working directory, beside the trial kits that landed
as `tools/win/gui-trials` (`BC250_ROOT` is the workspace root, by default the parent directory of this repository).
Its `T1-manifest.json` records what the tests must read:

| File | Bytes | SHA-256 |
|---|---|---|
| `oracle-cu.json` | 192848 | `0A04EB75A8BB4ACC59AAFC77DCDC1635151D83AE7AB95B96BE81D6552735A2D5` |
| `oracle-ver.json` | 117736 | `5A1C982B876B67354A8BC986562BA2C53847B5FBF43CF88B6719FD0225D71D71` |
| `oracle-art.json` | 1074541 | `C85905FD7044BD876AF4126DBD4C731C4FCD4F204780A43CCA253D0545251291` |

Check the three hashes before a release build and pass `-Oracle`. A skipped oracle check is silent, so a lost
directory looks exactly like a passing build.
