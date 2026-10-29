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
`-ToggleName`. These names are in the display language of the console user. If it does not find the check box,
it sets the window style with `SetWindowPos` and reports `method=window`.

Each step line gives the deltas of the counters that tell the causes apart:

| Delta | Meaning |
|---|---|
| `dwm` count above 1, or a dwm.exe error event | The compositor restarted: the BD-168 class, not a flip refusal |
| `flag_shared_transition` and `edge_a`, `e_refused`, `writer_busy` or `e5` in one step | The driver failed a transition flip. The OS does not go back to composition after that (`d3dkmddi.md:12793`) |
| `fmt_changes` and `flag_immediate` in one step | An immediate flip changed the plane format |
| `vis_false` | The OS blanked the source |

Tell the operator on the overlay before the run (`tools\win\bc250mon\mon.py status`). Take a camera frame or a
screenshot during A2 and B1. The overlay window covers part of the client, so in B0 DWM can already compose the
client or draw into its buffer (BD-197 trial 06.11 and x1). The B0 deltas give which one.

Results: `C:\BC250\tmp\topmost\<utc>\`, with `report.json` and one summary file for each read.
Exit codes: 0 all steps ran, 1 a step failed or the bound was exceeded, 2 refused before the first step.
