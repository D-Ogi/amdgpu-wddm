# Two live restarts of the GPU device with KMD 0.7.216.3 (2026-10-07, BD-090)

Unit A, Windows 11, desktop on the GPU route. Kernel driver 0.7.216.3, built from branch `kmd/bd090-stop-restart`
at commit `b24bcee7` (`scripts/LAB-PLAN.txt`). `Parameters\KeepLog` 1, so the KMD writes a ring file at every
stop. Each run is a script with a time limit, started over SSH (`python tools/win/target.py ps <script>`). The plan of the trials is
`scripts/LAB-PLAN.txt`.

## Files

| File | Script | Time (UTC) | Content |
|---|---|---|---|
| `hang-probe-2163.txt` | `hang-probe.ps1` | 05:05:03 | a Windows restart that did not finish: session-1 processes, wait reasons, device state, System events |
| `step0-2163.txt` | `post-deploy.ps1` | after the boot of 05:06:25 | boot time, driver, guard values, DWM, the restart events of the last 30 min |
| `trial1-2163.txt` | `trial-restart.ps1 -Tag t1` | stop at 05:09:29 (ring file name) | restart 1: `pnputil /restart-device` of the GPU, then the checks |
| `trial2-2163.txt` | `trial-restart.ps1 -Tag t2` | between 05:11:52 and 05:14:50 (file times) | restart 2, the same checks |
| `scripts/` | | | the scripts above, `confirm-state.ps1`, `after-trials.ps1`, `LAB-PLAN.txt` |

The two screenshots of the lab desktop after the restarts stay on the development PC and are not part of this
directory: `scratch/screens/20261007-071201.png` (05:12:01Z) and `scratch/screens/20261007-071453.png`
(05:14:53Z). The file names are in lab local time (UTC+2).

## What it shows

1. **The earlier restart took about 4.5 min (`hang-probe-2163.txt`, `step0-2163.txt`).** Event 1074 (restart by
   `shutdown.exe`) at 05:01:59Z. At 05:02:09Z `WerFault.exe` and `PickerHost.exe` delay the shutdown (`winsrvext`
   100, after 5016 ms). At 05:02:09Z an application popup: `M365Copilot.exe` "The instruction at
   0x00007FFCDC51594F referenced memory at 0x0000000000000008". At 05:02:14Z `M365Copilot.exe` delays the shutdown
   (`winsrvext` 101, after 10016 ms). At 05:05:03Z the GPU device is `Error CM_PROB_FAILED_POST_START`,
   `LogonUI` runs in session 1 and DWM is PID 1932. Event 6006 at 05:06:04Z, boot at 05:06:25.5Z. The restart
   before it took 15 s from event 1074 (04:39:23Z) to event 6006 (04:39:38Z). These files do not show why the
   device was at Code 43.
2. **State before the trials (`step0-2163.txt`).** Device OK, `CM_PROB_NONE`, driver 0.7.216.3. `GuardBoot
   Confirmed` 1. `StageHistory` `10 20 30 31 32 33 34 35 39 50`. DWM PID 1940, started 05:07:08Z.
3. **Restart 1 (`trial1-2163.txt`).**
   - `pnputil` returns "Device restarted successfully." after 1 s.
   - 15 s later the device is OK, `CM_PROB_NONE`, `UnconfirmedStarts` 1, `StageHistory` unchanged.
   - DWM is PID 1940, start 05:07:08Z, the same process as before.
   - The stop's ring file `ring-20261007-050929-494.log` (91108 bytes) shows the stop sequence. The fan goes back
     to the board after 1 takeover. The KMD turns off the DP audio stream and the scanout status reads
     `0x00000000`. The last completed fence is 6919, and the PSP unload follows.
   - 14 more ring files follow 0.1 to 0.4 s later. Each holds `guard: count 0 -> 1 flushed, durable required 1` at
     0.562 s.
   - The live ring of the new image has `guard: healthy start confirmation persistence 0x00000000` and
     `guard: boot marked confirmed 0x00000000` at 74.838 s.
   - The health check of the script failed: `health ok=False` with an empty `bc250kmd_cli health` answer after
     12 tries.
4. **Restart 2 (`trial2-2163.txt`).**
   - Before: DWM PID 1940, `UnconfirmedStarts` 0, `GuardBoot` 1.
   - `pnputil` returns after 1 s. 15 s later the device is OK, `UnconfirmedStarts` 1. DWM is still PID 1940.
   - The live ring: `guard: count 0 -> 1 flushed` at 0.625 s, `guard: boot marked confirmed 0x00000000` at
     71.761 s.
   - `health ok=False` with an empty answer, as in restart 1.
   - The file has no `kept stop log` header lines, so the stop's ring file of restart 2 is not in it. The
     `guard: orderly stop in a confirmed boot` line that `LAB-PLAN.txt` expects for restart 2 is not in it.
5. **The screenshots.** Both show the Windows desktop with the lab overlay, not a black screen. The overlay gives
   DWM PID 1940 since 07:07:08 local time. Its log has "full-WDDM start confirmed after 60 s observed progress" at
   07:10:44 and 07:13:29 local time. It also has `bc250kmd_cli info: no display adapter with hardware` at 07:09:30
   and 07:12:17 local time.

## Operator notes without a file here

The operator reports a third step after these trials: `shutdown /r /t 0`, and the new boot came about 30 s later.
The operator also connects the Code 43 of item 1 with the driver install of BD-091. No file of this directory
records these two statements.

Privacy changes: see `REDACTED.txt`.
