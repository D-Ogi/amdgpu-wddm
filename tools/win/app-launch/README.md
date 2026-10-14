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
