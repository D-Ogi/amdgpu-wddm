# Installer clean-slate test on unit A (2026-10-06)

The owner's definition of an installer test: remove everything of amdgpu-wddm from the lab with the release's own
uninstaller, then install the release fresh from its package. The control application, the Tuner and every other
component must install and work. The lab harness `C:\BC250` stays. Unit A, Windows 11, tester package
0.7.213.102-tester.17 (KMD 0.7.213.1, INF 0.7.213.102), three builds of the same version:

| Build | Package SHA256 (first 8) | Changes |
|---|---|---|
| r17b | `5E485118` | the uninstaller refuses a machine with lab folders unless `-Force` |
| r17d | `B8B9753B` | delete-at-restart fixed (`MoveFileEx` with a real null target), per-user data removed |
| r17e | `A32A4DA2` | per-user data removed also when no installation is left |

`scripts/clean-slate.ps1` runs one step per call: `uninstall` (`uninstall.ps1 -Yes -KeepTestSigning -NoReboot
-Force`, inventory before and after), `inventory`, `install` (`install.ps1 -Force -AcceptTestSigning -NoReboot
-CuMode 40`, inventory after). `scripts/restart-now.ps1` restarts between the steps. `scripts/r17-check.ps1` reads
the installed state after the restart. `scripts/repair-inuse.ps1` lists the deletes Windows holds for the restart.

## r17b: uninstall of the lab's accumulated state (`r17b/`)

- Before: 47 `bc250kmd.inf` packages in the driver store (KMD 0.7.159.1 to 0.7.213.101), 78 files in the install
  root, 286 in `C:\ProgramData\amdgpu-wddm`, 91 in the user's `%LOCALAPPDATA%\amdgpu-wddm`.
- After the uninstaller and a restart: 0 driver packages, GPU on Microsoft Basic Display Adapter, no keys, tasks,
  Vulkan or H.264 encoder registrations, no certificates. Left: 6 files in the install root (DLLs that DWM held),
  the 91 per-user files, and the control application's own data (kept by design).
- Cause of the 6 files: the uninstaller passed PowerShell `$null` as the new name to `MoveFileEx`. PowerShell turns
  `$null` into an empty string for a `string` parameter, so the call failed with `ERROR_PATH_NOT_FOUND` and nothing
  was scheduled. Fixed in r17d with `[NullString]::Value`. The 6 files were removed by hand.

## r17d: fresh install and check (`r17d/`)

- `clean17d-install.txt`: one driver package in the store (`0.7.213.102`), all components copied.
- `r17-check-1.txt` after the restart: device OK at 0.7.213.102, DWM on the GPU route (`bc250d3d_zink`,
  `bc250d3d_router`, `amdgpu_wddm_radv`), start confirmed, 40 CU, idle 500 MHz, fan read, Start menu shortcuts,
  start-confirm task result 0, control application status complete, no bugcheck.
- `clean17d-uninstall.txt`: an uninstall run with no installation left removed nothing, and the per-user data
  stayed. Fixed in r17e.

## r17e: uninstall, restart, fresh install (`r17e/`)

1. `1-uninstall.txt`: the uninstaller found 7 files in use and scheduled them. Its own footprint table reports
   every item gone except the install root (held files) and the control data (kept).
2. `2-scheduled-deletes.txt`: `PendingFileRenameOperations` holds 10 entries of ours (7 files, 3 folders).
3. `3-inventory-after-restart.txt`: install root gone, per-user data gone, System32/SysWOW64 stubs gone, no keys,
   no driver package, no task, no certificate, GPU on Microsoft Basic Display Adapter. Only
   `C:\ProgramData\amdgpu-wddm\control` (11 files) stays, by design.
4. `4-install.txt`: fresh install, one driver package, all registrations written.
5. `5-check-after-restart.txt`: same result as r17d's check, with the same component hashes (control `1FE6A77B`,
   cli `C4A26B99`, D3D11 `D42801CC`, D3D12 `477373C1`, desktop `A57F7376`, router `0F611676`, ICD JSON
   `234175BD`, MFT `D3E29E6E`). The last section (`verify` report) failed to read: the script's filter matched the
   `verify` folder, which the SSH account may not read. That line is a defect of the check script, not of the
   installation.

An install of r17e over r17d with `-Repair` before step 1 copied only the changed installer file; every payload file
had the same hash, so no in-use replacement happened in that run.
