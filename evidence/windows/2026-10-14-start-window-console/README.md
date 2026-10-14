# Start window console: powershell.exe against the control application as the task's program (lab, 2026-10-14)

Date: 2026-10-14, 11:08:24-11:09:16Z on the development PC clock (the trace times are on the lab clock,
11:08:43-11:09:22Z). Unit A, KMD rc4 of tester.31 (SYS B0E01A86, native mode, read on the lab at 11:07Z), Windows
Terminal 1.24.12741.0 installed. The kit did not record the Windows build. Owner's report: a console window came up
with the start window of the logon task. Fact M899 found a Windows Terminal window of the task's `powershell.exe` at
the close of the start window, in DWM's frames.

## Method

`tools/win/app-launch/console-proof.ps1` of this commit, run over ssh with `-Kit C:\BC250\tmp\console-kit` (the
command is in the script's header). The kit:

| File | SHA-256 | Source |
|---|---|---|
| `amdgpu_wddm_control.exe` | 0232F4F78521A5A4AD085770BB817BA5B063EB9E1651F135E600A21CE77660FC | this commit (`StartWindowHost.cs`), built on the development PC |
| `bc250control.dll` | 150CFA0F271CD9A53E743241F579F1F3E386840048A33295A28BEEFEB54F8273 | the same build (the installed rc4 DLL has the same hash) |
| `common.ps1` | 869E3592F7FA02CC036871E7194FA51C18A85C912907FAF630EBDC95616DD617 | `tools/release/installer/common.ps1` of this commit |
| `start-window.ps1` | 608DC6F5BD22B4A682735FBD9A449DB62E188AE954073721EBE2472850C026E0 | the installed script of rc4, copied |

The script made a copy of an install under `C:\BC250\tmp\console-proof\20261014-110839`: `control\` (the application
and the DLL) and `tools\start-window.ps1`. For each form, `Get-LogonTaskPlan` of `common.ps1` made the plan of the
window task, and `Test-LogonTaskPlan` held its rules before the run. The script then registered one task with the
plan's program and arguments, for the user at the console (`BC250-A\bc250`, session 1, Interactive, Limited, as the
logon task), and started it on demand. The window ran for 15 s. WMI `Win32_ProcessStartTrace` recorded every process
that Windows created in session 1 until the task ended.

| Form | Program of the task | Arguments |
|---|---|---|
| a | `powershell.exe` | `-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File "<work>\tools\start-window.ps1" -Seconds 15` (the plan without `-WindowHost`: rc4 and older, and an install with `-NoControlApp`) |
| b | `<work>\control\amdgpu_wddm_control.exe` | `--start-window -Seconds 15` (the plan with `-WindowHost`) |

A console window that Windows Terminal hosts comes with an `OpenConsole.exe` for the console and a
`WindowsTerminal.exe` when no Terminal process runs. Form a is the positive control of the trace. The console user
has no `DelegationConsole` and no `DelegationTerminal` value under `HKU\<sid>\Console\%%Startup`: the default terminal
is the one Windows chooses. Before the run, session 1 had no `OpenConsole.exe` and no `WindowsTerminal.exe`.

## Results

| | Form a | Form b |
|---|---|---|
| plan rules | held (with the reason in `console`) | held |
| task result | 0, 19 s | 0, 16.8 s |
| processes created in session 1 | 21 | 20 |
| `OpenConsole.exe` | 1 (pid 8716, `-Embedding`) | 0 |
| `WindowsTerminal.exe` | 1 (pid 9952, `-Embedding`) | 0 |
| parent of the script's `powershell.exe` | the task (pid 2124) | the application (pid 8728) |

1. Form a: `powershell.exe`, its `conhost.exe`, `OpenConsole.exe` and `WindowsTerminal.exe` have the same creation
   time in the trace (11:08:43.909Z, one batch of WMI events). Windows Terminal takes the console when the script
   starts, so its window is there for the life of the script, not only at the close.
2. Form b: the application started `powershell.exe -NoProfile -ExecutionPolicy Bypass -File
   "<work>\tools\start-window.ps1" -Seconds 15` (no `-WindowStyle`). Windows created a `conhost.exe` for it, and no
   `OpenConsole.exe` and no `WindowsTerminal.exe`. The task returned the script's exit code 0.
3. The other processes are the same in both forms: `bc250kmd_cli.exe` and its `conhost.exe` from the monitor overlay
   (pid 5248), and the sign-in notice that the script starts (`amdgpu_wddm_control.exe --start-notice`), which ends
   with nothing on the screen while the driver runs.
4. The verdict line of the script: `form a console window, form b no console window, 49.7 s`.

## Conclusion

When the window task runs `powershell.exe` itself, Windows Terminal opens a window for its console at the start of
the script, and `-WindowStyle Hidden` does not hide that window (M899). When the task runs Nagi Control App with
`--start-window`, the application starts `powershell.exe` with `CREATE_NO_WINDOW`, and Windows makes no Terminal
window. The installer registers that form when it installs the control application.

## Limits

- One run of each form. The script started the task on demand in the logged-on session, not at a sign-in. The
  sign-in run of the installed task belongs to the release validation.
- The trace sees processes, not windows. It shows that Windows Terminal ran for form a and did not run for form b. The
  frames of DWM at the close of form a's window are in M899.
- The default terminal was the one Windows chooses. With the console host (conhost) as the default terminal, form a
  would have a conhost window instead, which this run did not test.

## Files

- `console-proof.txt`: the output of the run, with the start and end times on the development PC.
- `trace-a.csv`, `trace-b.csv`: the processes that Windows created in session 1 for each form, with the lab's UTC time
  of the WMI event.
- `sha256.txt`: SHA-256 of the files of this folder.
