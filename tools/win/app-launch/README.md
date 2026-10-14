# Time to the first window

How soon Nagi Control App and the single-file setup show their first window on the lab, cold and warm, and whether a
second start of the control app opens nothing and brings the first one forward. The owner asked for the three
changes this measures on 2026-10-14: a loading window as soon as possible in both programs, and one control app per
session.

```
python tools\win\target.py ps tools\win\app-launch\measure-launch.ps1 -ControlExe <exe> -SetupExe <exe>
```

One command, bounded to 170 s. Give one or both programs. A comparison of two builds is two commands. Announce the
run on the overlay first (`tools\win\bc250mon\mon.py status`): windows open and close on the lab's screen.

| Row | What it is |
|---|---|
| `cold` | One start after the standby list of the memory manager is purged. The program's own files come from the disk. Files that a running process maps stay in memory, and Windows Defender keeps what it learned about the setup file |
| `warm` | `-Warm` starts (default 3), one second apart |
| `first` | The first visible window of the process: the loading window of a build that has one |
| `main` | The first visible window at least `-MainHeight` logical pixels high (default 420): the main window |
| `alone` | The main window is the only window of the process left: the loading window has closed |
| `second-main` | A second start while the first shows its main window minimised. It passes when the second process ends by itself with 0 in 10 s, shows no window, and the first window comes back |
| `second-loading` | A second start while the first still shows its loading window. It passes when the second process ends with 0, shows no window, and the first reaches its main window |

The times count from the moment before the process is created. A watcher with no window of its own (C# in the
script, compiled on the lab by `Add-Type`) looks at the process's top-level windows once a millisecond. It runs in
the console session through a scheduled task: the control app with the limited run level, as from the Start menu,
and setup with the highest run level, as after a Yes in the prompt of Windows. Nobody can answer that prompt from
here. Before the prompt, setup shows the same loading window as after it.

The script refuses to run while either program already runs, because it stops no process that it did not start. It
also refuses setup when the console user is not an administrator. It closes each main window with `WM_CLOSE` and
ends a process only when the process stays more than 10 s after that. It removes only the setup folders under
`%ProgramData%\amdgpu-wddm\setup` that appeared during its run. The console user must be able to read the
programs' folders: the install folder and a folder under `C:\Users\Public` are readable. The results stay in
`C:\BC250\tmp\app-launch\<utc>\` and on standard output.

Exit codes: 0 every start reached its main window and every second-start check passed, 1 a check failed or a run
did not fit in the bound, 2 refused before any start.

The headless counterpart on the development PC is the `--smoke-loading` step of each program's `build.ps1`. It
records `first-window-ms` without a real screen.

# Flashes of the start window

What DWM composes over the small blue window after logon (`start-window.ps1`), frame by frame. The owner saw that
window flash for one or two frames every 6 to 8 seconds (2026-10-14). The measurement puts each flash next to the
poll of the start confirmation (`start-confirm.ps1`), which runs at the same time after a logon.

```
pwsh tools\win\app-launch\build.ps1 -Kits <BC250_ROOT>\toolchain\nuget
python tools\win\target.py push <build>\flash-watch.exe tools\win\app-launch\poll-repro.ps1 tools\win\app-launch\dpm-sample.ps1 <fixed>\start-window.ps1 --to C:\BC250\tmp\flash-kit
python tools\win\target.py ps tools\win\app-launch\measure-flash.ps1 -Kit C:\BC250\tmp\flash-kit
```

One command on the lab, bounded to 170 s, three cases of 45 s (`-CaseSeconds`). Announce the run on the overlay
first: the blue window opens three times on the lab's screen.

| Case | Window | Poll |
|---|---|---|
| `a` | The installed `start-window.ps1` | `poll-repro.ps1` |
| `b` | The installed `start-window.ps1` | None |
| `c` | The `start-window.ps1` of the kit | `poll-repro.ps1` |

Case `b` has no poll, but it is not free of escapes: in all three cases `dpm-sample.ps1` reads the DPM state once a
second, and the overlay (when it runs) reads it too.

| Part | What it does |
|---|---|
| `flash-watch.exe` | Starts the window with the arguments of the logon task. Reads the window's rectangle from each frame that Desktop Duplication delivers, and the rectangle of the overlay (`bc250mon`) when it runs. If duplication does not start, it reads the screen through GDI every 2 ms. It writes one CSV row per frame and one log line per flash. It runs in the console session through a scheduled task, with the limited run level, as the logon task does |
| `poll-repro.ps1` | Does the calls of one poll of `start-confirm.ps1` in the same order, every 5 s: the start health READ through `bc250control.dll`, `Win32_PnPEntity` through CIM, `bc250kmd_cli info` and `bc250kmd_cli stages`. It records the start and end of each step. It only reads, and it changes nothing in the driver |
| `dpm-sample.ps1` | Reads the GPU clock and the state of the DPM governor once a second, for all the cases. The read is `Bc250Dpm` of `bc250control.dll`, the same read that the overlay makes. It sends no SMU message and changes nothing |
| `measure-flash.ps1` | Runs the cases and puts each flash next to the poll steps and next to the clock changes |

A flash is a content of a rectangle that DWM showed for less than 60 ms between two other contents. The countdown
changes the text once a second, so a regular content stays for about 1000 ms. Each flash line gives its kind:
`navy-only` (the background without the text), `black`, `not-the-window` (less than half of the rectangle is
navy), `earlier-content` (a content that was on the screen before), or `other-content`. The line also gives the
composed frame that had the flash: its number in the CSV, its present time and the number of presents in it. All
the times are QueryPerformanceCounter ticks of the lab, so a flash, a poll step and a clock sample are on one clock.

A clock change is a change of the mode, the throttle, the target, current or observed clock, or the counters of
raises, lowers and idle entries and exits between two samples. The change occurred at some time between the two
samples, so each flash line tells if a change is in the same 1 s interval as the flash, or how far the nearest change
is. Each case also gives the percentage of the intervals that have a change, to compare with the flashes.

Desktop Duplication shows what DWM composes. It does not show what the monitor then gets. A flash that is on the
screen but not in these frames comes from a later stage: the present of DWM's frame, or the scanout.

The script refuses to run while the logon tasks or their scripts run, and when the console has no signed-in user.
It runs the two window scripts as copies in its work folder, where no control application is next to them, so the
window starts no sign-in notice. It stops only the processes that it started, and it removes its task on every
exit path. The results stay in `C:\BC250\tmp\flash\<utc>\` (CSV, logs, the bitmaps of the flashed rectangles) and
on standard output.

Exit codes: 0 every case ran, 1 a case failed or did not fit in the bound, 2 refused before any case.

The host check of the window itself is in `tools\release\test-session-checks.ps1` ("the start window draws every
frame in one piece"). It makes the window without showing it, and checks that `WM_ERASEBKGND` draws nothing, that
the window looks as before pixel for pixel, and that a change of the text invalidates the window once.
