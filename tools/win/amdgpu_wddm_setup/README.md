# Nagi Setup

The setup window of a tester release: install, update and repair the Nagi graphics driver (aka amdgpu-wddm) on the
ASRock BC-250, and
prepare a folder for a PC without internet. It is a face for the release's PowerShell engine
(`tools/release/installer/install.ps1`, `prepare-offline.ps1`): every decision, check and change is the engine's, the
window shows them in plain words. Contract: `docs/gui/interfaces-setup.md`.

| Screen | What it shows |
|---|---|
| Welcome | What the release is, where its files come from (this package or a prepared folder, checked before use), the entry to prepare an offline folder |
| Checking | The engine's plan run (`-Plan`): read only, nothing changes |
| Review | Checks that need attention, what the run will do (restarts, firmware source), what it will change (Windows test mode, BitLocker paused for those restarts) as plain information, the settings each run keeps, changes or sets (from the engine's settings-impact plan), the release notes. It asks for nothing: see "Nothing is asked of the user" below |
| Install | Stages as the engine reports them, cancel while the engine allows it |
| Restart | A planned restart: a 15-second countdown and then the restart, with "Restart now" and "Later" offered and neither required. After sign-in the release's RunOnce entry starts the window with `--continue` |
| Done | One of five outcomes (A3): success, information, problem, cancelled, restart; whether anything changed; the next step (retry, repair from Nagi Control App, support file) |
| Prepare | `--prepare-offline`: a destination, an optional firmware folder, the copy, the result |

The step rail marks the step in which a run stopped: a refusal at Check shows Check as current and stopped, and
Review, Install and Finish as not done. On a problem the default button is Repair when offered, else Try again, else
Close. Try again is not the default when every failed check is one a new try cannot change while setup is open
(`ResultView.RetryCannotFix`: `gpu.missing`, `gpu.several`, `windows.not-x64`, `windows.too-old`, `package.damaged`,
`firmware.no-list`, `lab-install.present`, `secureboot.on`) or the setup files are damaged. The engine does not mark
causes as transient, so the window decides by check id.

## Nothing is asked of the user

The owner, 2026-10-12, seeing a greyed-out Install button under a consent check box: "nie mozna wymagac, ze
uzytkownik zaznaczy ten checkbox! ... Oczywiste jest, ze uzytkownik chce wlaczyc test mode i wlasnie robi tym
instalatorem! Przestan wymagac potwierdzen od uzytkownika!" A person who opened this installer wants to install, so
no screen asks for a permission, and the primary action is never off. The Review screen does carry choices between
good outcomes, each already answered: the optional Windows tuning, the two places of the shortcut of
Nagi Control App (both on) and the Nagi wallpaper (on). `InstallFlow.PlanOptionIds` names every control a screen may grow. The Review screen
states what will change as information. `src/InstallFlow.cs` holds the rules the window and the engine share: the
arguments every run carries (`-AcceptTestSigning`, and `-BitLocker Suspend` when BitLocker protects the system
volume), the information lines, and the ids no string table may carry again. The render gate G-CONSENT refuses a
screen that grows a permission control or a disabled primary action, and the unit tests hold the rules themselves.
Preflight refusals that protect the machine are unchanged - they are the engine's findings, not questions.

## One file, or setup.exe beside files\

A release ships the package as a zip and as one file, `amdgpu-wddm-setup-<version>.exe`: this window with the whole
package appended after the end of its program image (`src/Payload.cs`, written by
`tools/release/make-setup-exe.ps1`). With no `files\` folder beside it, the window unpacks that package to
`%ProgramData%\amdgpu-wddm\setup\<version>\` - a folder with its own permission list (administrators and SYSTEM
full control, Users read and run, no inheritance from `%ProgramData%`) - refuses a payload, a file list or a file
whose SHA-256 does not match and a path that would leave the folder, and installs from there, including the
continuation after each restart. The shared parent `%ProgramData%\amdgpu-wddm` keeps the rules it inherits, because
a strict list on it broke every uninstall of 0.7.216.100-tester.28 (BD-129). It is checked as a location instead:
a regular folder, no link, owned by SYSTEM, Administrators, TrustedInstaller or the account running setup. Anything
else ends the run with a message. One unpacking at a time, held by a `Global\` mutex: a second copy of the
file says setup is already running rather than resetting the folder the first one installs from. The folder is
removed when the run ends, also after a failure, and kept only while a restart is pending. The window that
finishes the install after the restart removes it by version name, because the continuation runs from the closure
and carries no payload of its own. `uninstall.ps1` removes whatever an interrupted run left there. With a `files\` folder beside it,
or with `--package <dir>`, the window uses that folder and behaves exactly as before. `--smoke-payload <dir>` reads
and unpacks the payload into a plain folder with no administrator rights, for the gate
`tools/release/test-setup-exe.ps1`.

The window draws its own title bar and every widget in it (`src/Theme.cs` for the tokens, `src/Controls.cs` for the
widgets, `src/Chrome.cs` for the frame), in the visual language of the accepted control-app mockup: a dark window
with rounded corners, a step rail with an icon per step, cards with a border and a soft shadow, Fluent glyphs, a red
gradient on the primary action and the guide's tip in a speech bubble over her picture.

A tester clicks `setup.exe` in the unpacked folder, which is this window. The package root's `install.cmd` opens it
as well when it is started with no argument, so an advanced user in the folder `files` also reaches the window.
`install.cmd` with a switch, and `install.cmd -Console`, keep the console engine. The window is
never larger than the work area of its screen (it has no frame of Windows and cannot be resized), the primary action
is the rightmost button of the bar, and the first Tab lands on the screen and not on a glyph of the title bar.

Ordinary words only: no driver internals, codes or file names in the window (G-NOINT, checked in every language and
every screen by the build). The details are in the support file, a zip that the user saves on request: the window's
summary and, per engine run, the command line, events, result, engine output and log, with the user name, computer
name and profile path redacted. A run that found a driver which did not start also brings the engine's
own diagnostics of that start, as `run-<n>-diagnostics.txt` and through the same redaction. The engine collects
it by itself (owner, 2026-10-12), so nobody has to ask the tester for the readings one at a time.

## How it runs the engine

Each run gets a fresh invocation id and its own folder (`--run-root`, default `%TEMP%\amdgpu-wddm-setup`). The window
starts Windows PowerShell 5.1 from System32 hidden, with `-Gui -InvocationId <id> -EventsFile -ResultFile`, reads the
complete event lines as they come and, at the end, the terminal result. A result counts only if its invocation id is
this run's (A2): a stale or missing result reads "the installer stopped before it finished", and the window then says
that changes may have been made; "nothing changed" is said only on a bound result that reports no change (A3).
Cancel writes `<events>.cancel`, and only while the engine says it can stop. The window cannot be closed while the engine runs.

The window reads the events every 250 ms. An event changes the progress screen in place (`src/WorkingView.cs`): the
mark of a stage, the note, the Cancel button, the value of the strip or a new check row. The window builds the
screen again only when its frame changes: the title, the stage list or the notice of cancelled removals. The strip
then stays the same control. Until 2026-10-14 each batch of events built the whole screen again, also for the
`step` event of each change. Each new control showed the background for one frame (owner: "co 6-8 sekund niebieski
pasek od instalatora robi krótkie mignięcie"). `--smoke-progress <dir>` plays an install run of 30 s and a plan run
of 10 s against the window, which is not shown. The strip must stay one control with one window handle, and no
event may build the screen again. The strip is drawn again only when its value changes or its band moves.

The window starts as the invoking user (`asInvoker`) and relaunches itself elevated at once (one UAC prompt);
`--prepare-offline` stays unelevated.

## Start

The owner, 2026-10-14: "instalator z exe także ładuje się dość długo - także potrzebuje od razu okienka ładowania".
Every run with a window opens a small loading window first (`src/LoadingWindow.cs`): Nagi's welcome picture, the
name of the program, one line of text and the progress strip. The slow steps come only after Windows has drawn it.
`src/StartSequence.cs` holds that order, and the unit tests check it against a recorder:

1. The loading window.
2. The look at the file (the first opening of the single file is the one Windows Defender scans, up to 1.2 s on the
   development PC), on a worker thread.
3. Without administrator rights: a line that tells what Windows asks and which answer continues, then the UAC
   prompt. The loading window
   stays until the elevated copy has its own loading window on the screen. The first copy gives the elevated copy
   the name of an event in the session namespace (`--loading-handover amdgpu-wddm-setup-loading-<32 hex>`), and the
   elevated copy sets it when its window is drawn. Without the event the first copy closes after 1.5 s, and in any
   case after 30 s or when the elevated copy ends.
4. With a payload: the lock of one unpacking, then the unpacking (368 MB, 4-12 s) with a progress bar on a worker
   thread. `Payload.Extract` reports bytes done of payload length plus twice the size of the files (the copy and the
   check). A person who closes the loading window stops the unpacking first, and the folder is removed.
5. The setup window, drawn before the loading window closes. A refused prompt, another unpacking run and a damaged
   payload are told in a message box over the loading window, with the exit codes as before (5, 8, 7).

`--smoke-loading <dir>` builds the loading window headless in each of its stages, with the layout and G-NOINT checks
of the setup screens. It also records `first-window-ms`: the time from the start of the process until the first
loading window has its handle and its layout. Nothing is drawn on a screen there.
`tools/win/app-launch/measure-launch.ps1` measures the time to the first window on the lab, with a real screen.

## Build and test

```
pwsh tools\win\amdgpu_wddm_setup\build.ps1 -Out P:\BC-250\scratch\build\gui-setup\setup-app
pwsh tools\release\build-release.ps1 ... -SetupApp P:\BC-250\scratch\build\gui-setup\setup-app
```

The build is the gate (its header lists the five steps): unit tests, the compile, the engine client against a
scripted fake engine (`test/fake-engine.ps1`) and every screen drawn headless at 96, 120, 144 and 192 DPI in four
languages, at a 150 % text size, without the guide character and in the high-contrast palette (`--high-contrast`,
which a PC without high contrast on could otherwise never draw). No step shows a window. `tools/release/test-dryrun.ps1` also runs the built window
against the real engine (plan and dry run). Every release package carries the window from 0.7.216.100-tester.28 on:
`build-release.ps1` refuses to build without `-SetupApp`, and `test-dryrun.ps1` fails a package without it. The
last step of the build is `tools/release/test-setup-exe.ps1`: the exe just built is packed around a package of its
own, reads it back, and refuses nine damaged, doctored or empty exes.

The icon of the window, and of the release's single setup file, is
`tools/win/amdgpu_wddm_control/art/icon/nagi.ico`, the icon of the control application as well. `build.ps1` passes
it to the compiler (`/win32icon`) and stops when the file is missing. The same file is embedded as the resource
`nagi.ico`, which `SetupForm.SetWindowIcon` reads, so the taskbar and Alt+Tab show the size that was drawn for them
(16 to 256 px) instead of one 32 x 32 image rescaled on a 150 % or 200 % screen. The build stops when the resource
is not in the built image.

The guide panel on the right shows the guide character, from the art of
`tools/win/amdgpu_wddm_control/art/nagi`: the build embeds the expressions this window's causes can ask for
(`SetupGuide.ArtExpressions`, which its unit tests check against every `SetupCause` and print for the build) at
`@256` alone, under the names that application's `Guide.ArtFile` map gives them, and refuses an exe that lacks one.
The smallest slot of this window is 176 device pixels at 96 DPI, larger than the `@128` picture, so a `@128`
resource could never be drawn. Her pictures are stills, and the setup window never animates her. She is on unless the control application's
`ShowNagi` is the DWORD 0, so she is on the Welcome screen of a first install. Two render runs use `--no-nagi`, and
every other run must draw her picture.

A small screen gets no guide panel at all. The content column then takes the width the panel would have had. The
owner asked for this on 2026-10-12, after GitHub #38 from a tester whose 800x600 screen let the picture cover the
text. Small is a working area of at most 1024 logical pixels wide, or at most 768 high (`SmallScreen.cs`). A screen
that small has no room for a 300 px column beside the page. The window asks the rule again when it moves to another
monitor, and after a screen or DPI change. Six render runs name the screen the window is drawn for
(`--work-area <w>x<h>`, in logical units: 800x600, 1024x768, 1280x720, 1366x768 and 1920x1080). G-SMALL holds both
directions of the rule.

Translations: `strings/strings.<lang>.txt`. A new or redone translation gets `?` as its hash and `strings/stamp.ps1`
stamps it; a line whose English text changed fails G-STR until it is retranslated. Polish, Japanese and Korean are
machine-translated (`mt`) until a reviewer marks them `rev`.

Mierz siły na zamiary: the setup window promises only what the engine reported.
(Measure your strength against your intentions.)
