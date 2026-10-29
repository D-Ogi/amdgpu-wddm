# Task Manager "Always on top" over the flip path (BD-187)

A field report (GitHub #43, tester.31) says: Task Manager works until its "Always on top" option is on. Then the
screen flashes a few times and goes black. This trial sets that option on the lab and reads the kernel driver's
flip counters before and after each step. It does not change the driver.

```
python tools\win\target.py push tools\win\topmost-flip\topmost-flip.ps1 tools\win\topmost-flip\topmost-helper.ps1 <client dir>\amdgpu_wddm_d3d12_queue.exe tools\win\d3d12queue\controller.ps1 --to C:\BC250\tmp\topmost-kit
python tools\win\target.py ps tools\win\topmost-flip\topmost-flip.ps1 -Kit C:\BC250\tmp\topmost-kit -FlipClient C:\BC250\tmp\topmost-kit\amdgpu_wddm_d3d12_queue.exe
```

The client is the `-Flip` build of `tools\win\d3d12queue` (`build.ps1 -Flip`). Its `controller.ps1` must be in
the same folder. Without `-FlipClient`, the trial runs arm A only. `-Plan` prints the schedule and starts nothing.
`-SelfTest` checks the summary parser on sample text and touches nothing. The bound is 170 s.

## Steps

| Step | What happens | Summary read |
|---|---|---|
| A1 | Task Manager starts in the console session | s1 |
| A2 | The option is on for `-OnSeconds` (25 s) | s2 |
| A3 | The option is off for `-OffSeconds` (10 s) | s3 |
| B0 | The flip client presents a borderless chain over the whole output, 8 s | s4 |
| B1 | The option is on, so Task Manager is above the client | s5 |
| B2 | The option is off | s6 |
| end | The client ends. Task Manager closes with the option off | s7 |

The helper (`topmost-helper.ps1`) runs as the console user with the highest run level, because Task Manager
runs elevated. It sets the option through UI Automation: the page `-SettingsName` and the check box
`-ToggleName`. These names are in the display language of the console user. If the option does not set
`WS_EX_TOPMOST`, the helper sets it with `SetWindowPos` and reports `method=window`, the return value, the Win32
error and the z-order band of the window.

Before each command, the helper sends `WM_NULL` to the window with `SendMessageTimeout` (2 s). A window that
does not answer is hung: the helper does not call UI Automation or `SetWindowPos` for it, and reports `hung=True`.
At the start, the window gets 10 s to answer. If it does not, the trial prints `taskmgr ui hung` and continues
with the flip client only. UI Automation runs in its own runspace and stops after 6 s (`-UiaSeconds`).

Each step line gives the deltas of the counters that tell the causes apart:

| Delta | Meaning |
|---|---|
| `dwm` count above 1, or a dwm.exe error event | The compositor restarted: the BD-168 class, not a flip refusal |
| `flag_shared_transition` and `edge_a`, `e_refused`, `writer_busy` or `e5` in one step | The driver failed a transition flip. The OS does not go back to composition after that (`d3dkmddi.md:12793`) |
| `fmt_changes` and `flag_immediate` in one step | An immediate flip changed the plane format |
| `vis_false` | The OS blanked the source |

Tell the operator on the overlay before the run (`tools\win\bc250mon\mon.py status`). The overlay window
covers part of the client, so in B0 DWM can already compose the client or draw into its buffer (BD-197 trial
06.11 and x1). The B0 deltas give which one.

## Camera step (optional)

A screenshot (`mon.py screenshot`) copies the screen through GDI. It gives the order of the windows in DWM, not
the buffer that the plane scans out. The kernel driver counters give which buffer the plane got, not what is in
it. Only the camera gives what is on the monitor. Use `-Plan` to get the times of the steps, then take frames
with `bash scratch\labcam\snap.sh` from the development PC:

1. One frame during A2, about 10 s after the option is on.
2. One frame during B1, about 10 s after the option is on: is Task Manager on the monitor above the flip client?
3. One frame during B2.
4. One frame after the run, to make sure that the desktop is on the monitor again.

Compare each frame with the screenshot of the same step. Is a window in the screenshot and not in the camera
frame? Then the plane scans out a buffer without that window in it.

Results: `C:\BC250\tmp\topmost\<utc>\`, with `report.json` and one summary file for each read.
Exit codes: 0 all steps ran, 1 a step failed or the bound was exceeded, 2 refused before the first step.
