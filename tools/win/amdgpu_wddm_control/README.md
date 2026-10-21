# Nagi Control App

The control application for testers of the Nagi graphics driver (aka amdgpu-wddm) on the ASRock BC-250. Earlier
releases called it "amdgpu-wddm Control". The executable keeps its name, `amdgpu_wddm_control.exe`. It runs on the tester's own PC,
by hand, like the vendor's control panel: a normal window, no service, no autostart, no network, no lab tools.

The window speaks to ordinary users (GUI-PLAN-v7, phase 1): plain words, no driver internals, error codes or internal
versions. A unit test and the render gate check every visible text against `PlainWords.Internals`; technical detail
goes only into the support report and the Support page (shown when "Show support options" is on). Texts come from
`strings/strings.<lang>.txt` (English is the source). A Polish speaker read every Polish line against one
terminology, where one English term keeps one Polish word, so each line carries the status `rev` (2026-10-12). Japanese and Korean are machine translations marked
`mt`, or `mt-safe` where the English is a name or a number.

| Page | What it shows or changes |
|---|---|
| Home | One status card with one recommended action, Getting started, recent games, monitor, driver and graphics summaries |
| Games | One entry per game file name: recent games, stored profiles, graphics settings, the release's defaults and added games. Per game: the D3D12 application profile as grouped check boxes with origin and cost, and the highest shader model. Also the game's own graphics and Vulkan settings (below), Apply/Discard, Undo/Redo and "Recommended" |
| Graphics | Automatic clocks and the ceiling (1000-2000 MHz on the 100 MHz grid) and the compute unit choice (section 7 of the plan). The number of cores runs from 24 to 40 in twos. Behind "Choose the pairs of cores yourself" sits a map of the four groups of five pairs. There a person turns on the pairs the firmware leaves off, one at a time (owner, 2026-10-14: a board with one or two broken units). "Advanced tuning", closed by default: the graphics voltage curve, the processor's clock limit, undervolt and temperature cap, and the processor core count (`docs/design/tuner.md`). Each tuning change is a trial that the driver takes back by itself unless kept. The settings for all games (below). Restore defaults (keep or reset the games) with undo. Sharpening and anti-aliasing as "Coming later" |
| Display | The monitors Windows reports. Per monitor: the resolution, refresh rate and GPU scaling from the modes Windows lists. A change stays only when the person keeps it within 15 s (below). Identify and a link to the Windows display settings. HDR and VRR as "Coming later" |
| Performance | Live load, temperature, clock, video memory, power and fan (every 2 s, only while shown and not minimized), the shader cache sizes. Each row says "No reading" when the driver gives no fresh value: power needs KMD 0.7.215 (the SMU metrics table, RUN_DPM ABI 3), the fan KMD 0.7.213.1. Voltage goes only into the support report. The ray tracing benchmark (below) |
| Driver | The installed release, whether Windows runs it, the update check (Settings can turn the check at start off) |
| Settings | Language, update check at start, recent launches, Nagi, tips, animations, support options, what data the app keeps |
| Help | Four symptom guides, one explanation per setting (through search), Repair, Restart Windows, the support report, About |
| Support | The driver's start and recovery states in English, the technical actions (below), the installed components |

`--recovery` opens a small recovery view that never loads `bc250control.dll`: the installed release, the symptom
guides, Repair, Restart Windows and the support report.

## Start

The app shows a small loading window first: Nagi, the name of the app and one line of text (owner, 2026-10-14). The
loading window reads nothing from the computer. When it is on the screen, the app reads the state of the computer on
a worker thread (`src/WindowReading.cs`: the device through WMI, the driver's answers, the scheduled task, the cached
update check, the recent launches and the benchmark's game folder) and builds the main window at the same time. The
main window opens with that reading, and the loading window closes after it (`src/LaunchSequence.cs` holds the order,
`src/Launch.cs` does it). A start that takes more than 8 s says so and offers to close the loading window. Before
this change, all of that ran on the thread that draws, before the first picture: on a PC without a BC-250 about
0.3 s from the start of the process to the loading window, against about 1.1 s to the old first window.

One window per person and session (`src/SingleInstance.cs`). The first start of the window owns the named mutex
`Local\amdgpu-wddm-control-window`. A later start finds it, sets the event `Local\amdgpu-wddm-control-window-activate`
and exits 0 without a window. The first start then brings its window to the front: the loading window while it
still starts, else the main window, restored when it is minimised, or the dialog that is open over it. Only the plain
start (no arguments) takes part. `--action`, `--start-notice`, `--recovery` and the gates do not.
`tools/win/app-launch/measure-launch.ps1` measures the time to the first window on the lab.

## How it talks to the driver

Through `bc250control.dll`, built from `tools/win/bc250kmd_cli/bc250kmd_cli.c` with `BC250_CONTROL_DLL`. Only
software-state escapes, each with `NoAdapterSynchronization` alone: the DPM snapshot (`Bc250Dpm`), start health
(`Bc250StartHealth`, op READ), the GPU DWM interop decision (`Bc250Interop`), dxgkrnl's segment statistics
(`Bc250VideoMemory`) and the log ring (`Bc250LogRead`, `GET_LOG`, answered without the adapter lock from 0.7.184.1).
No `HardwareAccess` escape and no `LOG_SUMMARY`: a Level Two escape idles the GPU, and an overlay that polled one every
5 s stalled a running game for 300 ms each time (BD-054). The one exception is named below. Live values are read
every 2 s, and only while Overview or Performance is shown and the window is not minimized.

The one escape that changes the driver is the start-health CONFIRM of "Confirm this start" (Recovery), the same
request the release's logon task sends: administrator only, once per action, and from 0.7.213 with
`NoAdapterSynchronization` instead of `HardwareAccess` - it writes the registry and touches no register. A driver of
0.7.212 or older refuses that word, and that driver is what runs while a release defers the device restart. The DLL
then sends the same request once more with the old `HardwareAccess` word, so the Recovery action still confirms the
start. This is the one exception to the paragraph above, and it costs one Level Two escape per Recovery action,
never on a schedule. DPM settings are
`DpmMode` and `DpmMaxMHz` under
`HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters`, which the KMD reads at device start
(`docs/design/dpm.md`, "Settings and boot guard"): the page says to restart Windows. The runtime tuning escapes
(`dpm tune`, `dpm floor`) are lab tools and not offered. The boot guard can set `DpmMode` back to 0 after an unclean
or unconfirmed start; the page shows the last start's reason.

The window runs as the invoking user. A change starts an elevated copy of the program (one UAC prompt) with one verb,
`--action <name>`. The copy checks its arguments with the same functions as the window and reads the value back. A
game's settings use `--action game-profile --image <name.exe>`. Add `--value <switches>` for its switches (an empty
value removes the game's key), `--gfx <settings>` for its graphics settings, or both. `game-undo` and `game-redo` take
`--image`. The settings for all games use `--action graphics-defaults --gfx <settings>`.

## Graphics settings for games

The registry contract of 2026-10-08 (`src/GraphicsSettings.cs`). The driver's readers own the meaning. This app writes
only these names, only under these keys, and only values inside the contract:

| Key (below `HKLM\SOFTWARE\amdgpu-wddm`) | Value | Type | Values | In the window |
|---|---|---|---|---|
| `Graphics`, `Graphics\Applications\<exe>` | `FrameRateLimit` | DWORD | 0 (no limit), 20-300 | Frame rate limit |
| | `VSync` | DWORD | 0, 1 | Vertical sync (VSync): Always off, Always on |
| | `Anisotropy` | DWORD | 1, 2, 4, 8, 16 | Texture filtering |
| | `MaxFrameLatency` | DWORD | 1-3 | Low latency: frames ahead |
| | `PerformanceOverlay` | DWORD | 1 on, 0 off | Performance overlay (DirectX 11 games for now) |
| | `RenderOnCpu` | DWORD | 1 on, 0 off | Run DirectX 11 on the processor, a last resort. On the Games page. The card for all games shows it only when it is stored |
| `Vulkan` | `WsiRoute` | REG_SZ | `dxgi` (default, BD-197), `gdi` | Vulkan presentation for all games: Fast (flip) or Compatible. The ICD reads no `WsiRoute` of `Vulkan\Applications\<exe>`, so the Games page has no such row |
| `Vulkan`, `Vulkan\Applications\<exe>` | `MemoryOverflow` | REG_SZ | `allow` (default), `strict` | When the VRAM is full (DirectX 11 and 12 games only): use system memory, or report out of memory (not shown now, see below) |

The system Vulkan ICD (64-bit, and 32-bit from tester.31) reads `WsiRoute` for all games only. When the value is
absent, it presents through DXGI (`dxgi`, "Fast (flip)"). That is the owner's decision of 2026-10-07, and it is
the default again since BD-197, after the route presented on the lab (Quake II RTX and vkcube). `gdi`
("Compatible") is the rollback that a person stores, and a stored `gdi` stays `gdi`. A DXGI swapchain that fails,
or that does not present its first frame in time, goes to GDI by itself, for that swapchain only
(docs/design/vulkan-wsi-dxgi.md, "The switch and the fallback").
`MemoryOverflow` is not offered (`GraphicsSettings.AwaitingIcd`): the system Vulkan ICD does not read it, only the RADV
inside the D3D11 and D3D12 drivers reads it. A control that does nothing, or that works only for some programs, is not
shown. So the window has no row and the search no entry for it, and `--gfx` refuses its name. The support report
still lists its stored values. The row comes back when its ICD makes it a control that works for every program.

An absent value is "Application decides" or the default that the window names. A game's own value wins over the value
for all games. The settings rule applies. "Application decides" and "Same as all games" write nothing and remove a
stored value. The helper removes a game's key when it has no value and no subkey left (`delete key`). It removes no
other key. A stored value outside the contract shows as "Stored value not valid". It stays as it is until the person
chooses another value. The reader takes `WsiRoute` `dxgi-composition`, but this app does not offer it, so it shows as
"Set outside this app". The Vulkan readers ignore case and take an empty string as absent. They take an invalid
`WsiRoute` as `gdi` and an invalid `MemoryOverflow` as `allow`, and the window says so. What the Graphics readers do
with an invalid DWORD is not known here, so the window does not guess. The changes apply the next time a game starts,
without a restart of Windows.

The highest shader model of a game is two switches of its D3D12 profile. `shader-model-68-off` gives 6.7.
`shader-model-67-off` gives 6.6 and wins when both are named. 6.8 removes both. Until the shell that reads them is
merged, `Profiles.AwaitingShell` names them. The ShellTokens test then checks that the shell does not read them yet.

## Display modes

The Display page offers only the modes that `EnumDisplaySettingsEx` lists for a monitor at its current colour depth,
without interlaced modes. It offers three GPU scaling choices: full screen (`DISPLAYCONFIG_SCALING_STRETCHED`), keep
the aspect ratio (`ASPECTRATIOCENTEREDMAX`) and centred (`CENTERED`). Another scaling that Windows chose shows as "As
Windows set it", and the page never writes it. Apply works as the display settings of Windows. First
`ChangeDisplaySettingsEx` with `CDS_TEST`. Then the change without saving it, and `SetDisplayConfig` without
`SDC_SAVE_TO_DATABASE` for the scaling. Then "Keep these display settings?" with a 15 s countdown. Keep saves the
change (`CDS_UPDATEREGISTRY`, `SDC_SAVE_TO_DATABASE`). Revert, closing the question or the end of the countdown puts back
the display configuration from before the change. These are per-user settings: no elevation, no backup and no Recovery
action. `src/DisplayModes.cs` is the only file that changes a display setting (G-SRC).

## Ray tracing benchmark

The Performance page carries a card that measures how fast the card traces light (owner, 2026-10-11). It measures a
copy of Quake II RTX that the person already has. The package never holds the game and never downloads it.

Where the game is. The card takes the folder the person chose (`BenchmarkFolder`, section 5 of
`docs/gui/interfaces.md`) when there is one. Otherwise it reads `SteamPath` of `HKCU\Software\Valve\Steam`, every
library of `steamapps\libraryfolders.vdf`, and the manifest `steamapps\appmanifest_1089130.acf` of each library, and
takes `steamapps\common\<installdir>` of the first library whose manifest names application 1089130. The app parses
that Valve key-value text itself (`Vdf` in `src/Benchmark.cs`). It never reads text that Windows translated. A folder
counts as a usable copy only with `q2rtx.exe` in it and `demos/q2demo1.dm2` in the directory of `baseq2\pak0.pak`,
which is the scene the test runs. Without the scene the card says so and offers nothing to measure. With no copy at
all the card offers the game's Steam page and the maker's own release page, and nothing else can be opened from it.

One run, two passes. Quake II RTX traces its rays through one of two Vulkan contracts, its own `ray_tracing_api`:
`pipeline` and `query`. A run measures both, in that order, and the card names them "First way" and "Second way" with
the technical name under each. Each pass starts `q2rtx.exe` once with the settings of the lab harness
(`scratch/pathtrace/lab/pt-run.ps1`, outside this repository) on the command line only: windowed 1280x720, no dynamic
resolution, no upscaler, full view, no sound, no vertical sync, one timed run of the demo, then quit. The game's own
files are never written: `+set basedir` points at the game folder, which is only read, and `+set homedir` points at
`%LOCALAPPDATA%\amdgpu-wddm\benchmarks\q2rtx-home`, the game's one writable directory, where its log is written as
`baseq2\logs\q2rtx-bench-<pass>.log`. The result comes from that log's own line `<N> frames, <S> seconds: <F> fps`,
which the game writes with C's `printf`, so the number is read with a dot whatever the language of Windows.

What stops a pass. Each pass is bounded to three minutes. The chip's temperature and clock come from the driver
snapshot the Performance page already polls, every 2 s. A pass stops at 89 C at once, or at 87 C held for 10 s (the
rule of the lab trials, owner 2026-10-04). The card then says the chip got too hot. "Stop the test" ends it as well.
Where that snapshot carries no temperature, the card says so instead of promising a stop it cannot make, and the
three minutes of each pass are the whole bound.

A pass ends through a Win32 job object created for it, with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`: the game and
anything it started die with that handle, and a run can never outlive the window. Because that job is the pass's only
way to end the game, it is built before the game is started: a pass that cannot get one never starts the game, and a
game that could not be put into it is ended at once by the handle this app started it with. That one ending is why
`BenchmarkRun.cs` stands beside `RecoveryActions.cs` and `BugReport.cs` in the G-SRC list of files that may end a
process. No process this app did not start is ever touched. After the job is ended the pass waits up to 5 s for the
game to be gone. A game still there ends the run, and so does a pass that ran out of time. Two copies of the game
must never trace at once. One run at a time, on a worker thread. The window only reports.

What is never measured. A result is read only from a log this pass itself wrote: the pass removes the log of the
previous run first, refuses to start when that log cannot be removed (another copy of the game holds it open), and
rejects a log older than the start of the pass. A number that was measured elsewhere is worse than no number.

What it shows. Each pass gives its pictures a second, its highest temperature and its average clock, with "Not
available" where the driver gave no reading, and the date and driver release of the run. A pass that was stopped
before its game ran has no reading of its own: the one sample taken before a start belongs to the decision to start,
and only the pass that was refused because the chip was too hot keeps it (`PassTelemetry.Keep`). The card shows one run in
full: the run of this session while there is one, otherwise the newest saved run. The state line above it follows
that run (`BenchState.Id`): the way that measures now, "You stopped the test." for a run the person ended, "The test
is finished." for a run of this session, and the saved line for a run read back from the history after a new start of
the application. "Earlier runs" lists the saved runs before the run above it (`BenchHistory.Earlier`), so no run
appears twice. The newest ten runs are kept
in `%LOCALAPPDATA%\amdgpu-wddm\benchmarks\q2rtx-history.json` (section 7 of `docs/gui/interfaces.md`). On a board the
app has positively identified as a BC-250 (the identification of the board memory card) the card also shows what this
release measured on the project's own board, for comparison: 57.8 to 69.7 pictures a second at 1280x720, with the
release those numbers were measured with named in the same sentence. The number is a band because twenty passes of
the two ways over two lab rounds landed inside it, with the two samples of the validation suite, and a pass follows
the clock the chip reached in a scene of about ten seconds. The card says that in one line under the band.

The card holds still while a run goes on. A rebuilt page keeps the card the person reads at the same place, not its
own scroll offset (`PagePlace.Keep`), because a card above can get a line at any moment: the case fan card does when
the game loads the chip, and "Start the test" and "Stop the test" moved 23 px down under the pointer of a person who
was aiming at Stop. "Start the test" brings the buttons of the card into reach by the same arithmetic
(`PagePlace.Bring`), and a page that shows them already does not move at all. The gate `--smoke-scroll` builds every
state of a run without a window and measures the two buttons against the view.

`src/Benchmark.cs` holds every rule as pure functions over text, bytes and streams, which the host tests decide
without a game, a driver or a process (`test/BenchmarkTests.cs`). `src/BenchmarkRun.cs` runs the passes and
`src/MainForm.Bench.cs` draws the card.

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

After an unclean end the driver closes the GPU desktop path and the automatic clock at its next start, and keeps
them closed at every later start: the desktop is then drawn by the processor at the base clock, and before BD-181
nothing said so. `src/ClosureNotice.cs` reads those closures with the release installer's own rule and `HomeStatus`
puts them on the home page as the first card, in plain words: what the driver switched back, what it costs, and one
button, the `reopen-closed` action, that reverts every closure at once and then offers the restart. The line of the
status card below it is kept as the cause of the verdict and not drawn a second time (`StatusItem.Hidden`). A value a
person set by hand has no record of the driver next to it, so it is a choice and the notice never mentions it. Where
the installed release names no defaults the notice cannot be formed. The single `reopen-gpu-path` line then stays as
before. The same action is in "Support: actions" as "Reopen what the driver closed".

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
| `reopen-closed` | every value the driver closed by itself, back to the installed release's default, then the desktop record the driver left. The values are `EnableGpuPresentBlit` and `EnableCddDwmInterop`, whose record `InteropClosedReason` is deleted after them, and `DpmMode`. Its record `DpmClosedReason` stays: the state catalog lets only the driver write or delete it, and the driver deletes it at the next start that reads a `DpmMode` other than 0. The rule is `Get-DriverClosure` of the release installer, value for value (`ClosureNotice.cs`). The value must be the one the driver writes when it closes (0). The installed release must name a default for it that is in range and is not that value. A record of the driver's own must sit next to it. That is the durable record with any reason, or `DpmLastReason` with 3, 4 or 8 for a driver before 0.7.208.1. The legacy record belongs to the last start and is not deleted. A 0 that a person wrote by hand has no such record. Such a value is never touched. The graphics cores are not here. Their release default is 24. That is also the value the driver closes with, so a repair install reopens nothing there either (`CuMode` in `common.ps1`) | next restart (offered) | the driver closed nothing by itself. The installed release has no list of defaults |
| `desktop-gpu` | `DwmForceCpu` 0 | next restart (offered) | the driver is not running; the effective switches (escape and `InteropLastState` & 3) are not both on (points to `reopen-gpu-path`); the GPU desktop files are missing; DWM is on the GPU route already; the GPU route is selected already (restart Windows) |
| `desktop-cpu` | `DwmForceCpu` 1 | next restart (offered) | DWM is on the CPU route already; the CPU route is selected already (restart Windows) |
| `confirm-start` | start-health CONFIRM (clears `UnconfirmedStarts` and `DpmPending` in the KMD); not undoable | at once | the driver is not running; confirmed already; not eligible by `Test-StartConfirmEligible` of the installer's `start-confirm-core.ps1` (flags 7, completions, ready >= 60 s, last completion <= 5 s; the helper retries the reading for 10 s) |
| `enable-dpm [--ceiling N]` | `DpmMode` 1, and `DpmMaxMHz` N only when a ceiling is chosen | next restart (offered) | N is not 1000-2000 on the 100 MHz grid; stored already |
| `set-clocks --mode 1\|unset --ceiling N\|unset` | `DpmMode` 1 or removed, `DpmMaxMHz` N or removed (the Performance page) | next restart (offered) | as above, or a mode other than 1 (the fixed clock is the unchecked default) |
| `reset-defaults [--games keep\|reset]` | the manifest's defaults of `EnableGpuPresentBlit`, `EnableCddDwmInterop`, `DpmMode`, `DpmMaxMHz` and `DwmForceCpu`; delete `InteropClosedReason`; the release's graphics cores. The release's `CpuTune` when the stored value is another one or absent, and `CpuTune` deleted when the release names none. Every graphics setting for all games removed. With `--games reset`, every game profile set to the release's recommendation or removed, and every game's graphics settings removed with their empty keys | next restart (offered) | the manifest has no `"defaults"`, or one is missing or out of range; `--games reset` without the release's game list; all at their defaults already |
| `undo` | the values the newest undoable backup found (not per-game changes, not the graphics-core part) | next restart (offered), or the next game start when it puts back only graphics settings | nothing to undo; the backup names a value outside the list; it would put DWM on the GPU route with the switches closed |
| `cu-mode --cu 24\|26\|...\|40 [--cu-map <hex>]` | the graphics-core values through `CuMode.Plan` (plan section 7). The number of cores is 24 to 40 in twos, because the hardware switches them on in pairs. 24 is the stock path. Every number above it is `CuMode` 40 with a `CuDisableWgp` mask: the documented default set of that number, or the set `--cu-map` names (docs/design/cu-mode.md). `CuMode` is written as 24 or 40, and never deleted (BD-077). A deleted value is a key the release installer never configured, so it writes the release default over it. An earlier confirmation goes. `CuModeChosen` records the whole request, so the next upgrade keeps the number a person asked for. A chosen 24 takes the driver's own `CuModeClosedReason` with it. Not undoable: the old values are kept for diagnosis | next restart (offered) | no choice. The stored state cannot be read. The mask names cores the firmware of the board keeps on, or leaves another number of cores than `--cu`. A chosen set without a reading of the board. The choice is there already: the same request with its record already on the key |
| `cu-confirm` | the start-health confirmation of a 40-core start | at once | the driver is not running; no 40-core start waits |
| `game-profile --image <exe> [--value <list>] [--gfx <settings>]` | `Experiment` of `HKLM\SOFTWARE\amdgpu-wddm\D3D12\Applications\<exe>` (an empty list removes the key), and the game's graphics settings (`Name=value` or `Name=unset`, comma-separated) | next start of the game | not a file name, not a catalog list, not a list of settings in the contract, the settings cannot be read, or stored already |
| `game-undo\|game-redo --image <exe>` | the game's values before its newest change (undo) or before its newest undo (redo), its graphics settings included | next start of the game | nothing to undo or redo for that game |
| `graphics-defaults --gfx <settings>` | the graphics settings for all games under `Graphics` and `Vulkan` (as `--gfx` above) | next game start | not a list of settings in the contract, the settings cannot be read, or stored already |
| `game-log-on --minutes N` | `GameLogUntil` of `HKLM\SOFTWARE\amdgpu-wddm\Diagnostics` (REG_QWORD, the UTC FILETIME at which recording stops), and the folder `%ProgramData%\amdgpu-wddm\logs\games` with its access list, pruned to 512 MiB | next game start | N is not 1-60, or the value cannot be read |
| `game-log-off` | delete `GameLogUntil` | next game start | nothing is recording, or the value cannot be read |
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

### Record game logs

The driver's diagnostic lines went to the environment variable `AMDGPU_WDDM_LOG`
(`driver/umd/d3d12/stdio-log.h`), which nobody can set for a game that Steam or Epic starts. The Help page's
"Record game logs" switch (`src/GameLog.cs`) replaces it: `game-log-on` stores the instant at which recording
stops, and every game that starts before that instant writes one file of its own in
`%ProgramData%\amdgpu-wddm\logs\games`. The first of its modules to read the switch opens the file and marks the
process with `AMDGPU_WDDM_LOG` and `AMDGPU_WDDM_GAMELOG=<pid>;<instant>`. The other modules, and the Vulkan ICD,
join that file under the same bounds, while a process the game starts later sees a marker that is not its own and
records separately or not at all. The folder's access list lets any user create a file there and nobody but
its owner change it afterwards, so a game without administrator rights can write its log and cannot touch
anybody else's.

Three bounds hold a switch somebody forgets: the instant (the sink checks it on every line), 64 MiB a file, and
the folder pruned to 512 MiB oldest-first before each recording starts. A support report takes the newest five
files, at most 20 MiB in total, redacted like every other entry, with a `game-logs/README.txt` that lists what it
took and what it left out. An `AMDGPU_WDDM_LOG` a process already carries always wins over the switch.

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
gates use it, so a build records nothing in the profile of the PC that builds). Only a lab build reads it (BD-167).
`build.ps1` without `-Lab` runs its gates on a lab twin of the application.

## Bug report

An unconfirmed or failed start automatically saves a local report when the control window opens.
The Recovery window also collects this report without a driver connection.
Collection stops after 20 seconds and keeps at most 4 MiB of text.
The window shows the saved path. Nothing is sent.

`startup-diagnostics.txt` includes StartRecord, StopRecord, device status and current-boot events for the display driver.
It includes matching SetupAPI sections from the last 1 MiB and at most three kept KMD logs.
Each kept log contributes at most 512 KiB. Redirected file paths are omitted.
The automatic archive stays under `%LOCALAPPDATA%\amdgpu-wddm\diagnostics`.
The manual report includes the same diagnostics. Its optional capability tests and dxdiag have their own time limits.

`driver-state.txt` (the snapshots above), `driver-log.txt` (the whole log ring), `installed-files.txt` (Device
Manager status and problem code, the display driver key, every component with version and SHA-256), `settings.txt`
(the driver's `Parameters` values, the D3D12 profiles, and the stored and effective graphics settings for all games
and per game), `system.txt` (Windows build, test signing, app version),
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
5 s, private memory under 300 MB), `--smoke-loading <file>` (the loading window in both its states, four
languages, four scales, text at 100, 150 and 225 %, a 1024x768 and a large screen, with the checks of the sign-in
notice) and `--smoke-render <dir> <scale> [--lang xx] [--text-scale f] [--no-nagi]
[--switch-to xx] [--fixture <snapshot.json>] [--work-area <w>x<h>]` in 39 runs: four languages at 1, 1.25, 1.5 and 2, with
`test/snapshot-bd059.json` and the tuning snapshots as the fixtures, text at 150 %, two runs with the guide
character off (she is on in every other run, as she is after an install), two language
switches, and six runs that name the screen the window is drawn for (`--work-area`, in logical units: 800x600,
1024x768, 1280x720, 1366x768 and 1920x1080), so that a render never reads the screen of the PC it runs on. Every page is drawn to a PNG without a window at the default size and at the minimum size, and the whole
page as `<page>-full.png`. The build fails when two sibling controls overlap, a text is cut, a page is wider than
the page column it was built at (`LayoutRules.PageWidthFinding`: the column width of this moment is not the measure,
because the window manager clamps a client size larger than the desktop), its last control
lies outside the scroll range, a visible text names a driver internal, a control lacks an accessible name or tab
stop, the art shows while "Show Nagi" is off, the guide panel draws no picture while "Show Nagi" is on and the side
panel is in the window, the side panel shows on a small screen or is missing on a screen with room for it
(G-SMALL), the window is wider or taller than the screen it is drawn for or its page does not follow the column
left after the window was clamped (G-ORDER), or a language switch leaves old texts behind. The unit tests also check
every string table: each id in each language, no stale translation (G-STR), no internals in the texts (G-NOINT).

Two more gates hold what the start window flash of tester.31 rc4 taught (fact M898):

- `--smoke-labels <file>` builds every window and page without showing it: the main window with this PC's state and
  with the three recorded snapshots, the sign-in notice, the loading window, the dialogs and the recovery view. It
  fails on a `Label` with AutoSize off and a `Dock`, because `Label.AdjustSize` gives such a label its requested size
  (100x23) at each change of its text before the parent docks it again. On the labels whose text a timer changes, it
  also traces the window positions while their digits change, and fails on any size other than the final one. It
  passes only when its negative control (the rc4 window, a docked Label) fails and its positive control (the
  application's own label) passes. `tools/quality/winforms_labels.py` reads the same layout in the PowerShell and C#
  of the rest of the repository.
- The image does not depend on the line ends of the checkout. `csc` embeds the string tables and `app.manifest` byte
  for byte, so the build embeds copies of them with LF line ends. Step 3b builds the image again from copies of the
  sources with every text input in CRLF and in LF and compares the bytes. A control build with the CRLF tables as
  given must differ.

### The icon of the application

`art/icon/nagi.ico` is the icon of `amdgpu_wddm_control.exe`, of the shortcuts the installer writes and of the
release's single setup file. It holds the guide character's head at 16, 20, 24, 32, 40, 48, 64, 128 and 256 pixels,
built from her `@256` art by `art/icon/make-icon.py`. `PROVENANCE.md` beside it records the source picture and the
bytes of the file. `build.ps1` passes it to the compiler (`/win32icon`) and stops when the file is missing, so a
build without the icon cannot ship. It is embedded as the resource `nagi.ico` as well, which `MainForm` reads for
the window, the taskbar and Alt+Tab: those take the size that was drawn for them instead of one 32 x 32 image
rescaled on a 150 % or 200 % screen. The build stops when the resource is not in the built image.

### The guide character

`art/nagi` holds her art: nine expressions at `@128` and `@256`, the files that `Guide.ArtFile` names, with their
`PROVENANCE.md`, which records the bytes of every file. `build.ps1` embeds that directory by default as the
resources `nagi.<expression>@128.png` and `@256.png`, compares every file against that byte list
(`art/check-art.ps1`), and refuses the built exe unless all 18 are in it. The unit tests also decode each file at
its named size, so a truncated picture is a failure and not a text-only panel. `tools/win/amdgpu_wddm_setup` embeds
the same files under the same names, reading the same map and the same byte list, for the expressions its own
screens reach. `tools/release/build-release.ps1` refuses a package whose control application lacks one of the 18, or
whose setup window, where it ships, lacks one of its own.

"Show Nagi" in Settings is on unless the user turns it off (`ShowNagi` absent or any value but the DWORD 0), so an
installed application shows her without a setting change. With her off, or with no art embedded
(`build.ps1 -NagiArt ''`, whose render runs therefore all use `--no-nagi`), the panel is the text-only panel and
says the same words.

The art that ships is stills: it holds no `-fNN` frame files, so no release animates her. `GuidePanel` plays a frame
sequence when a set has frames, and `Guide.MayAnimate` then still asks "Reduce animations" and Windows' own
animation setting first.

Her picture lives in the side panel, which the window drops below a client width of 1000 logical pixels
(`MainForm.SideShown`), as the nav bar and the page column need that room. A narrower window therefore has no guide
panel at all, with her picture and its text, and the pages carry that help in their own cards and dialogs instead.

The panel also goes on a small screen, whatever "Show Nagi" says (owner, 2026-10-12, after GitHub #38 from a tester
on an 800x600 screen): a working area of at most 1024 logical pixels wide or at most 768 high has no room for a
300 px column beside the page. The rule is `LayoutRules.SmallScreen`, the measure is the working area of the screen
the window stands on in logical units, and the window asks it again when it is moved to another monitor and after a
display or DPI change. The setting itself is not written: on a larger screen it decides as before, and Settings
carries one line under it that says the guide is hidden on a small screen.

The window's own size comes from that screen as well (`MainForm.ApplyScreenSize`): the design size of 1240x760
logical pixels where the screen has room for it, the screen's working area less a margin where it has not, and the
smallest size a person may drag the window to never larger than the size it opens at. A fixed smallest width of
900 px made the window wider than an 800x600 screen, which Windows could not clamp, so the page lost its right
edge whatever the small-screen rule answered. The shell is built before the window has a handle, and the window
rebuilds the page for the column that is left after Windows clamps it and `FitToScreens` places it. The render gate
G-ORDER holds that order: the page is built first and the client clamped afterwards, as in the live window, and a
page wider than the column it ends up in is a finding.

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
