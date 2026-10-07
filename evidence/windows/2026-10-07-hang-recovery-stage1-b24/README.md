# Hang recovery stage 1 on KMD 0.7.216.17 (2026-10-07, trials b24)

Unit A, Windows 11, desktop on the GPU route. Kernel driver 0.7.216.17 from branch `kmd/hang-recovery-b22`, commit
`416f2cec`, `bc250kmd.sys` SHA-256 `4E033FB2...` (`b24-01-deploy-17.txt`). `TdrDelay` 10. The hang client
(`bc250hang.exe`) and the design of the hang recovery are on that branch. Every step was one
bounded script over SSH. The times below are UTC.

These are the outputs of the helper scripts only. The process dump of DWM (390651266 bytes) and the kernel dump of
the same day stay on the development PC and are not in this repository.

Fact M835 in [`docs/facts/kmd.md`](../../../docs/facts/kmd.md) cites this directory.

## Files

| File | Time | Content |
|---|---|---|
| `b24-01-deploy-17.txt` | about 14:23 | installation of 0.7.216.17, then a Windows restart |
| `b24-02-record.txt` | 14:29:30 | the hang record before the switch: last verdict 2 of the trial of 13:37:39 (95 kills, not drained) |
| `b24-03-switch1.txt` | 14:29:34 | `HangRecoveryMode` 1 written, then a Windows restart (boot 14:30:08) |
| `b24-04-log.txt` | 14:31:36 | the KMD log of the new start holds the `HangRecoveryMode 1` line |
| `b24-05-short.txt` | 14:31:39 | short client, 200 ms dispatch: completed, device alive, no recovery attempt |
| `b24-06-D1-long.txt` | 14:32:11 to 14:32:26 | trial D1: one long dispatch that does not end |
| `b24-07-D2-short.txt` | 14:34:33 | trial D2: short client after D1 |
| `b24-08-record-final.txt` | 14:34:40 | the hang record after D1 and D2 |
| `b24-09-after.txt` | about 14:39 | processes, event 4101, processor power events of the boot |
| `b24-10-after-log.txt` | about 14:44 | KMD log tail after the trials |
| `b24-11-dwm.txt` | about 14:44 | Application events, DWM CPU time over 5 s, monitor and display device state |
| `b24-12-dwm-spin.txt` | about 14:45 | the threads of DWM PID 1936 and the graphics modules it holds |
| `b24-13-dwm-restart.txt` | about 14:46 | DWM restarted (new PID 8932 at 14:46:10), DPM line, display visibility lines |

## What the files show

1. **D1 recovers without a bugcheck.** The client submits one dispatch at 14:32:12.082. Its wait ends after
   14137.7 ms with `DXGI_ERROR_DEVICE_HUNG` (`0x887a0006`). The KMD log has a hardware fence timeout after 500 ms
   on sequence 8756, then `soft recovery: seq 8756 retired after 1 kill(s) of VMID 12 waves in 0 us; ring
   reopened` and `ResetEngine node 0: SOFT RECOVERED, aborted fence 1143`. The hang record has verdict 1
   (drained), `Recovered` 1. Event 4101 at 14:32:27: "Display driver bc250kmd stopped responding and has
   successfully recovered". `b24-08-record-final.txt` at 14:34:40 still gives boot 14:30:08, so Windows did not
   restart.
2. **D2 passes after D1.** The short client completes its dispatch in 95.0 ms with `S_OK`, and the device stays
   alive.
3. **DWM spins after D1.** DWM keeps PID 1936 through D1 and D2. It uses 5.02 s of CPU time in 5 s
   (`b24-11-dwm.txt`). One thread runs and the others wait (`b24-12-dwm-spin.txt`). DWM holds `d3d11.dll`,
   `dcomp.dll`, our `bc250d3d_router.dll`, `bc250d3d_zink.dll` and `amdgpu_wddm_radv.dll`. The GPU stays at its
   idle point, 500 MHz and busy 0.0 % (`b24-10-after-log.txt`).
4. **A DWM restart brings the display back.** The new DWM uses 0.02 s of CPU time in 5 s. The KMD log then has
   `display visibility: call 13 ... visible 1 previous 0 hardware 1 blanked 1`, and the next call reads
   `blanked 0` (`b24-13-dwm-restart.txt`).

## What these files do not show

- What blanked the display source before the DWM restart. The operator saw a black desktop after D1. No
  screenshot is in this directory.
- What the DWM thread does in its loop. The process dump holds that and is not here.
- That stage 1 recovers other hang shapes. D1 is one long compute dispatch of one client.

## Privacy

Nothing was changed before the copy. Checked and found clean: no IPv4 address, MAC address, serial number, UUID,
host name or user name.

`sha256.txt` holds the SHA-256 of every file of this directory.
