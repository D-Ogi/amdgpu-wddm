# Hang recovery stage 1 with the hosted zink b25 (2026-10-07, trials b25)

Unit A, Windows 11, desktop on the GPU route. Kernel driver 0.7.216.17 from branch `kmd/hang-recovery-b22`, commit
`416f2cec`, `bc250kmd.sys` SHA-256 `4E033FB2...` (`b25-01-deploy.txt`). Hosted desktop UMD zink b25 from Mesa
branch `amdgpu-wddm/hang-recovery-zink`, commit `12133799`, `bc250d3d_zink.dll` SHA-256 `7C4E5E7E...`
(`b25-02-record.txt`). The hosted ICD is the one of release tester.20. `TdrDelay` 10. The hang client
(`bc250hang.exe`) and the design of the hang recovery are on the KMD branch. Every step was one bounded script over
SSH. The times below are UTC.

These are the outputs of the helper scripts, the hang records and KMD log rings that the helper saved, and the
diagnostic logs of the hosted UMD in the two DWM processes of the day.

Fact M836 in [`docs/facts/kmd.md`](../../../docs/facts/kmd.md) cites this directory.

## Files

| File | Time | Content |
|---|---|---|
| `b25-01-deploy.txt` | boot 14:52:28 | installation of 0.7.216.17, then a Windows restart |
| `b25-02-record.txt` | 15:25:00 | the hang record before the switch (boot 15:24:15, after the zink b25 install): switch 0, zink `7C4E5E7E` |
| `b25-03-switch1.txt` | 15:25:10 | `HangRecoveryMode` 1 written, then a Windows restart (boot 15:26:02) |
| `b25-04-log.txt` | 15:26:40 | the KMD log of the new start holds the `HangRecoveryMode 1` line |
| `b25-05-record.txt` | 15:26:43 | the hang record with the switch on |
| `b25-06-C4-short.txt` | 15:27:00 | trial C4: short client, 200 ms dispatch: completed, device alive, no recovery attempt |
| `b25-07-D1-long.txt` | 15:27:27 to 15:27:47 | trial D1: one long dispatch that does not end, the verdict and the desktop check |
| `b25-08-zinkdiag.txt` | about 15:28 | the diagnostic logs of the hosted UMD, the device-lost lines, the DWM process |
| `b25-09-D2-short.txt` | 15:28:08 | trial D2: short client after D1 |
| `b25-10-switch0.txt` | 15:28:59 | `HangRecoveryMode` 0 written |
| `b25-11-restore-t20.txt` | after 15:29 | installation of the release driver 0.7.216.100 (KMD 0.7.216.14) again |
| `hangb22/record-*.json` | 15:25 to 15:28 | the hang records that the helper saved before and after each client |
| `hangb22/run-*.txt` | 15:27 to 15:28 | the client outputs of C4, D1 and D2 (their `.err` files were empty and are not here) |
| `hangb22/kmdlog-*.txt` | 15:26 to 15:28 | the KMD log rings that the helper saved |
| `zinkdiag/bc250d3d_zink-dwm.exe-1904.log` | boot 15:24:15 | the hosted UMD log of DWM in the boot before the switch |
| `zinkdiag/bc250d3d_zink-dwm.exe-1876.log` | boot 15:26:02 | the hosted UMD log of DWM in the boot of the trials |

## What the files show

1. **D1 recovers without a bugcheck.** The client submits one dispatch at 15:27:27.428. Its wait ends after
   14590.4 ms with `DXGI_ERROR_DEVICE_HUNG` (`0x887a0006`). The KMD log has `soft recovery: seq 8654 retired after
   1 kill(s) of VMID 12 waves in 1011 us; ring reopened` and `ResetEngine node 0: SOFT RECOVERED, aborted fence
   1123`. The hang record has verdict 1, 1 kill, 1011 us. The helper gives `VERDICT DRAINED`.
2. **The desktop stays.** DWM keeps PID 1876 through C4, D1 and D2. The KMD log has `display visibility: call 11
   ... visible 1 previous 1 hardware 1 blanked 0` 5.3 s after the recovery, and no blank line in the current start.
   DWM uses 0.022 of a core over 5 s. The helper gives `DESKTOP-OK`.
3. **D2 passes after D1.** The short client completes its dispatch in 92.1 ms with `S_OK`.
4. **DWM loses its hosted device once.** Lines 760 to 764 of `zinkdiag/bc250d3d_zink-dwm.exe-1876.log`:
   `async wait event: 0x2`, `BC250 hosted op=36 count=1 hr=80004001`, `GetDeviceState: 0xC00000BB`,
   `MESA: error: ZINK: vkQueueSubmit failed (VK_ERROR_DEVICE_LOST)` and `BC250 hosted device lost: reason=5 (stop)
   ... SetErrorCb`. Then the runtime frees the resources of the lost device, and DWM makes a new hosted device and
   draws frames again in the same process.

## What these files do not show

- Which wait gave the `0x2`. The log has no line that names the wait. `docs/design/hang-recovery.md` explains it
  from the source: the CPU wait of the hosted ICD winsys before it reuses a gather slot, one wait of 10 s.
- That the change of 0.7.216.18 and zink b26 keeps the hosted device of DWM. It has not run on the lab yet.
- That stage 1 recovers other hang shapes. D1 is one long compute dispatch of one client.

## Privacy

Nothing was changed before the copy. Checked and found clean: no IPv4 address, MAC address, serial number, UUID,
host name or user name.

`sha256.txt` holds the SHA-256 of every file of this directory.
