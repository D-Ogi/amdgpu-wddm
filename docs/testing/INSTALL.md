# amdgpu-wddm tester release: installation guide

This package installs the amdgpu-wddm GPU driver on an ASRock BC-250 board (AMD Cyan Skillfish GPU, gfx1013,
PCI device 1002:13FE) with Windows 11 x64. The driver is experimental. It is signed with a test certificate, so
Windows must run in test mode.

Use this package only on a BC-250. The installer stops on every other computer and changes nothing there.

## What you need

- An ASRock BC-250 with Windows 11 x64 (build 22000 or later) installed and working on Microsoft Basic Display
  Adapter.
- A local account with administrator rights.
- Secure Boot set to off in the BIOS setup (see "Secure Boot" below).
- If BitLocker is on: your BitLocker recovery key, or permission to suspend BitLocker for two restarts.
- The Microsoft Visual C++ 2015-2022 Redistributable (x64). If it is missing, the installer stops and tells you.
  Get it from Microsoft: https://aka.ms/vs/17/release/vc_redist.x64.exe (download it on any computer and copy it to the BC-250).
- 2 GB of free space on the system drive.
- A keyboard and a monitor on the BC-250.
- An internet connection during the installation: the installer downloads the AMD GPU firmware (see "GPU firmware"
  below). Without one, download the files on another computer and give the installer the folder.

## Before you start

1. Make a backup of all data on the BC-250 that you want to keep.
2. Write down your BitLocker recovery key if BitLocker is on: open a command prompt as administrator and type
   `manage-bde -protectors -get C:`.
3. Unpack the zip file to a local folder, for example `C:\amdgpu-wddm-tester`. Do not run the installer from the
   zip file or from a network drive.
4. Optional: do a dry run first. In the unpacked folder, open a command prompt and type `install.cmd -DryRun`.
   The dry run does all checks and shows all changes, but it changes nothing.

## Secure Boot

Windows does not load test-signed drivers while Secure Boot is on. To turn it off:

1. Restart the BC-250 and push `Del` (or `F2`) to open the BIOS setup.
2. Find the Secure Boot setting (on most firmware versions: Security > Secure Boot).
3. Set Secure Boot to Disabled.
4. Save and exit (usually `F10`).

The installer does not change BIOS or firmware settings.

## Install

1. In the unpacked folder, right-click `install.cmd` and select "Run as administrator".
2. Read the preflight table. Every line must show `ok` or `warn`. A `fail` line stops the installer, and the
   installer tells you what to correct. When it stops at preflight, it has changed nothing.
3. Phase 1: the installer makes a System Restore point (if System Protection is on) and asks for permission to
   turn on test signing. Type `YES` to continue. If BitLocker is on, the installer asks if you have the recovery key
   or if it must suspend BitLocker for two restarts.
4. Let the computer restart. After you log on, the installer starts again by itself (accept the administrator
   prompt). If it does not start, run `install.cmd` again.
5. Phase 2: the installer downloads the GPU firmware and checks it, then installs the certificate, the driver, the
   user-mode drivers and the firmware, and sets the registry values. The screen does not change: the GPU changes to
   the new driver at the next start. Then the computer restarts again.
6. Phase 3: after the second logon, the installer verifies the driver and shows a pass or fail table.

You can run the verification again at any time with `verify.cmd` in `C:\Program Files\amdgpu-wddm`.

To install a newer package, unpack it to a new folder and run its `install.cmd`. You do not have to uninstall
first. The installer shows `upgrading <old> -> <new>`, keeps the files that did not change, replaces the others and
restarts the computer. Running `install.cmd` of the version that is already installed and verified does nothing;
use `install.cmd -Repair` to install it again.

An upgrade keeps the settings you changed: the driver settings (for example `DpmMaxMHz`), `DwmForceCpu`, the D3D11
allowlist and the application profiles. The installer writes a new default only over a value that the previous
installer wrote and that you did not change. The upgrade output shows each value as `new`, `unchanged`, `KEPT`
(your value) or `->` (new default). A value given on the command line (`-DpmMaxMHz`) is always written. The
installer always writes its own values: the file paths, the graphics registration and the start counter
`UnconfirmedStarts`. The Control application can set the defaults again.

"Test Mode" is shown in the lower-right corner of the desktop while test signing is on. This is normal.

## GPU firmware

The driver loads eight AMD firmware files from `C:\BC250\firmware`. This package does not contain them. The
installer downloads them, and the AMD licence text `LICENSE.amdgpu`, from the linux-firmware project at commit
`2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`, at every installation and upgrade. It checks the SHA256 of each file
before it copies anything. If a file has a different SHA256, the installer stops and changes nothing.

The installer tries the first address of each file, then the second (3 tries each):

- `https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/<path>?id=2b8daaf611fbade74f26a5b58ec1defe6a02f5e0`
- `https://gitlab.com/kernel-firmware/linux-firmware/-/raw/2b8daaf611fbade74f26a5b58ec1defe6a02f5e0/<path>`

| File | `<path>` | SHA256 |
|---|---|---|
| `cyan_skillfish2_ce.bin` | `amdgpu/cyan_skillfish2_ce.bin` | `5946efbf7e46ccfe7e9f965da56c35b1187469d1c1a49c8a9eff433eb3ab4723` |
| `cyan_skillfish2_me.bin` | `amdgpu/cyan_skillfish2_me.bin` | `d4ed4d968de0d7720f7c8215d42fdae727bf99cdf43cee6f8f864dc2584b17e2` |
| `cyan_skillfish2_mec.bin` | `amdgpu/cyan_skillfish2_mec.bin` | `b1c1f843a8faaa2de537d6ba05052a3776b5a1b73f8a3c3f5ed9d568489776a4` |
| `cyan_skillfish2_mec2.bin` | `amdgpu/cyan_skillfish2_mec2.bin` | `b1c1f843a8faaa2de537d6ba05052a3776b5a1b73f8a3c3f5ed9d568489776a4` |
| `cyan_skillfish2_pfp.bin` | `amdgpu/cyan_skillfish2_pfp.bin` | `50e56dc1913571ff589425d2b059bddfc5e8a2b4bc22d3eb78cac75d1e0479a1` |
| `cyan_skillfish2_rlc.bin` | `amdgpu/cyan_skillfish2_rlc.bin` | `20acefdb6128a36275f4382425a109a7c9927f6053eab685bb51164ff1d18cfb` |
| `cyan_skillfish2_sdma.bin` | `amdgpu/cyan_skillfish2_sdma.bin` | `15d0d3626da7f2513f03b13bb7e02eeeeccab276276ae27d0b6067b5f25e9e95` |
| `cyan_skillfish2_sdma1.bin` | `amdgpu/cyan_skillfish2_sdma1.bin` | `bd1c0b0f6a6a4f17ede034f2c844b6553fc444e08915c7bb500c09fde59d6255` |
| `LICENSE.amdgpu` | `LICENSES/LICENSE.amdgpu` | `572872598565dc3513470de971a32bf9db301f47afeef3636345eadae33b2eee` |

The same list is in `manifest.json` (section "firmware"). A dry run (`install.cmd -DryRun`) shows the addresses
and the SHA256 values and downloads nothing.

Without internet on the BC-250:

1. On a computer with internet, download the nine files from the addresses above (replace `<path>`). Keep the file
   names of the first column.
2. Copy them to one folder on the BC-250, for example `C:\amdgpu-wddm-firmware`.
3. Run `install.cmd -FirmwareDir C:\amdgpu-wddm-firmware`. The installer checks the SHA256 of each file the same
   way and uses the folder also after the restarts during the installation. The same goes for `-DpmMaxMHz`,
   `-CuMode`, `-NoControlApp` and `-NoReboot`.

## The small blue window after logon

After each logon, a small blue window with a clock is shown in the top-left corner for up to two minutes. It is
the "amdgpu-wddm start confirm" scheduled task. The driver counts each start that is not confirmed. After two
unconfirmed starts, Windows uses Microsoft Basic Display Adapter again, to prevent a start loop. The task keeps the
desktop drawing for at least one minute and then confirms the start. The same confirmation keeps the automatic
clock control (DPM) on: if a start is not confirmed, the next start runs at the fixed 1000 MHz clock, and DPM stays
off until you set it again in the control application. Do not close the window. If you log on with an account that
is not an administrator, the task does not run, and the driver falls back after two restarts. The task writes its
results to `C:\ProgramData\amdgpu-wddm\start-confirm.log`.

## What the installer changes

| Item | Location |
|---|---|
| Test signing | `bcdedit /set {current} testsigning on` (only after you type YES) |
| Test certificate | LocalMachine Root and TrustedPublisher stores |
| Kernel-mode driver | driver package `bc250kmd.inf` (driver store, service `bc250kmd`) |
| Driver settings | `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters` |
| User-mode drivers | `C:\Program Files\amdgpu-wddm\` (`d3d12`, `desktop`, `d3d11`, `vulkan`, `wow64`, `tools`, `control`, `mft`). Nothing goes into `C:\Windows\System32` or `C:\Windows\SysWOW64`. An install over an earlier release removes the `bc250umd.dll` that it put there |
| H.264 encoder (Media Foundation) | `HKLM\SOFTWARE\Classes\CLSID\{A32438F0-0D79-4CA9-A5BF-9F3C80837253}` with its `InprocServer32`, `HKLM\SOFTWARE\Classes\MediaFoundation\Transforms` (the encoder's own key and its membership in the video encoder category). These keys are for the whole computer. `uninstall.cmd` removes them, and so does a later release that does not install the encoder |
| Licence texts | `C:\Program Files\amdgpu-wddm\licenses` (the package's `licenses\` and `THIRD-PARTY.md`) |
| Graphics registration | the GPU's software key (`UserModeDriverName`, `VulkanDriverName`, and for 32-bit applications `UserModeDriverNameWow`, `VulkanDriverNameWow`; the first entry of each `UserModeDriverName` value, the Direct3D 9 entry, is empty), `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`, `HKLM\SOFTWARE\WOW6432Node\Khronos\Vulkan\Drivers` |
| Router policy and profiles | `HKLM\SOFTWARE\amdgpu-wddm` |
| GPU firmware | `C:\BC250\firmware` (8 files and `LICENSE.amdgpu`, downloaded from linux-firmware; see "GPU firmware"). A new folder is writable by administrators only; the access rights of `C:\BC250` itself do not change. Download staging: `C:\ProgramData\amdgpu-wddm\installer\firmware-staging`, removed after the copy |
| Start confirmation | scheduled task "amdgpu-wddm start confirm" |
| Installer state and logs | `C:\ProgramData\amdgpu-wddm` (the Control application keeps its setting backups and action log in its `control` folder) |

Default driver settings: automatic clock control (DPM) on, maximum 1500 MHz. The thermal limits of the driver do not
change. The driver writes no debug log files. To change the maximum clock at install time, use
`install.cmd -DpmMaxMHz <1000-2000>`. An upgrade keeps a maximum clock that you set (see "Install").

## What works and what does not

- Desktop composition (DWM) runs on the GPU (`DwmForceCpu` 0 in `HKLM\SOFTWARE\amdgpu-wddm\DesktopRouter`). When the
  driver has closed the GPU desktop path (for example after a start that ended in a crash or a power loss), DWM uses
  the CPU route by itself. "Reopen the GPU desktop path" on the Recovery page of amdgpu-wddm Control opens it again,
  and so does `install.cmd -Repair`. `DwmForceCpu` 1 keeps the
  desktop on the CPU route. An upgrade keeps the value you set.
- Direct3D 12 applications run on the GPU through the Windows Direct3D 12 runtime, at feature level 12_1.
- Vulkan applications run on the GPU through the Vulkan ICD.
- Direct3D 11 applications and games run on the GPU. The value `Mode` in
  `HKLM\SOFTWARE\amdgpu-wddm\AppRouter` holds `gpu-default`. These programs stay on the CPU route (software
  rendering): the components of Windows and the packaged Microsoft applications, the processes of the logon and lock
  screens, a program whose image the router cannot resolve, and a program in the `Deny` list (by default
  `witcher3.exe`, which has a Direct3D 12 mode). An entry in the `Allow` list puts a Windows component on the GPU
  (by default `dxdiag.exe`). `Mode` `allowlist` gives the behaviour of the earlier releases: the CPU route for every
  program, and the GPU only for the programs in `Allow`. Use the Direct3D 12 mode of a game if it has one.
- Direct3D 9 applications run on the GPU through D3D9On12, the Direct3D 9 layer of Windows on top of our Direct3D 12
  driver. The installer leaves the Direct3D 9 entry of `UserModeDriverName` empty, and Windows then loads
  `d3d9on12.dll` by itself.
- 32-bit Direct3D 10/11/12 and Vulkan applications use 32-bit builds of the same drivers (folder `wow64`), with the
  same routing rules as 64-bit applications. 32-bit Direct3D 9 applications use D3D9On12 on the 32-bit Direct3D 12
  driver, as 64-bit ones do.
- The Witcher 3 (Direct3D 12 version) has an application profile.
- A program that records video can use the H.264 encoder of the GPU through Media Foundation. The encoder works only
  while Windows runs the BC-250 on this driver: if the driver did not start, the encoder refuses, and the program
  uses an encoder of Windows. `verify.cmd` shows the line `H.264 encoder` with the file that the registration names.

## If something fails

- Preflight `fail`: correct the item that the table names, then run `install.cmd` again.
- Preflight `fail` on `GPU firmware`, or the installer stops at the firmware step: the computer cannot reach the
  download addresses, or a file has a different SHA256. Connect the computer to the internet, or use
  `-FirmwareDir` (see "GPU firmware").
- The installer stops with `stopped at step: ...`: the message names the step and the cause. Correct the cause if
  you can, then run `install.cmd` again from the same package folder. The installer skips the steps that are
  complete and continues at the step that failed.
- A file that Windows uses (for example a user-mode driver that the desktop has loaded) cannot be overwritten. The
  installer renames it to `<name>.old-<time>`, copies the new file and deletes the old copy at the next restart.
- The screen stays black after a restart: wait two minutes. If it stays black, restart the computer with the power
  button. After two failed starts, Windows uses Microsoft Basic Display Adapter again.
- The desktop is black after an update, but programs still start: open amdgpu-wddm Control (`Win`, then type its
  name) and select "Desktop on the CPU route" on its Recovery page. Otherwise use Safe Mode and `uninstall.cmd`
  (next item).
- To start Windows without the driver: hold `Shift` and select Restart, then Troubleshoot > Advanced options >
  Startup Settings > Restart > `3` (Enable low-resolution video), or `4` (Safe Mode). Then run `uninstall.cmd`.
- Verification fails: run `verify.cmd`, then make a diagnostics bundle (see "How to report a bug").
- After an installation, `verify.cmd` fails `GPU desktop path` with `closed by the driver` or `last boot died in a
  session`, and the desktop runs on the CPU route: run `install.cmd -Repair`. It sets the switches of the release back
  and restarts Windows. You can also open amdgpu-wddm Control, select "Reopen the GPU desktop path" on its Recovery
  page and restart Windows. A normal installation, an upgrade and a plain re-run of `install.cmd` keep the closed path
  and name it in the report, because the driver closed it for a reason. If the driver closes the path again, make a
  bug report instead of repairing a second time.
- Windows 11 apps (the command bar of File Explorer, Task Manager) ignore mouse clicks, often after the screen went
  black for a moment, or `verify.cmd` shows `[warn] DWM restarted in this session`: WinUI pointer-input loss after the
  desktop compositor (DWM) is terminated and restarted reproduces on this Windows build also with Microsoft Basic
  Display; restart Windows to recover. If the compositor crashed, make a bug report: a crash can still be a driver
  defect. `verify.cmd` reports only a replacement it saw: the start-confirm task records the compositor at each
  logon, and without that record the line says `unknown history`.

## Uninstall

1. Run `uninstall.cmd` as administrator (in the package folder or in `C:\Program Files\amdgpu-wddm`).
2. Type `YES`. The uninstaller removes the driver, the files, the registry values, the task and the certificate.
   The GPU goes back to Microsoft Basic Display Adapter. The uninstaller keeps `C:\ProgramData\amdgpu-wddm\control`
   (the Control application's setting backups and action log). Delete it yourself if you do not need it.
3. If the installer turned on test signing, the uninstaller asks if it must turn it off. Use
   `uninstall.cmd -DisableTestSigning` or `-KeepTestSigning` to answer in advance.
4. Restart the computer. If BitLocker is on, have the recovery key ready: changing the boot options again can cause
   a recovery key prompt.

You can also use the System Restore point "amdgpu-wddm before install" if the installer made one.

## How to report a bug

1. Open "amdgpu-wddm Control" from the Start menu and select the bug report function.
2. The application shows the list of files and the text of each file before it writes anything. Read them.
3. The application writes one zip file to your desktop: `amdgpu-wddm-report-YYYYMMDD-HHMMSS.zip`. It sends
   nothing.
4. Open a bug report on GitHub: https://github.com/D-Ogi/amdgpu-wddm/issues/new/choose, then "Bug report". Fill in
   the package version, the game or application and its API, the settings, what you did, what you expected, what
   happened, and the time of the problem.
5. Attach the zip file to the issue.

The zip file contains these files:

| File | Contents |
|---|---|
| `driver-state.txt` | driver version; clock control (mode, maximum, clocks, voltage, temperature, load, throttle reason); start health; desktop composition (CPU or GPU); video memory statistics |
| `driver-log.txt` | the driver's internal log ring |
| `installed-files.txt` | release version and folder; Device Manager status and problem code; the GPU's driver key (INF version and date, driver name values); driver, Vulkan manifest and library files with file version and SHA-256 |
| `settings.txt` | the driver settings (`Services\bc250kmd\Parameters`) and the Direct3D 12 application profiles |
| `system.txt` | Windows build, test signing state, application version, UTC time |
| `events.txt` | System log entries of the last 24 hours from the display driver, DirectX kernel, DWM, crash reporting, power and Plug and Play, and application errors that name the driver, DWM, Direct3D or Vulkan |
| `dxdiag.txt` | `dxdiag /t` output (only if you select it; it takes about one minute) |
| `d3d12-caps.json`, `vulkan-summary.txt` | Direct3D 12 and Vulkan capabilities (only if you select them) |

Before the preview, the application removes from every file: your user name, your profile path (shown as
`%USERPROFILE%`), the computer name, MAC addresses, e-mail addresses, and every value on a line that names a serial
number, machine ID, product ID or UUID. Versions, hashes and hardware IDs stay, because we need them.
If you find personal data in the preview, do not send the file, and tell us in the bug report so that we can
correct the application.

## Optional Windows tuning

The setup window offers Windows tuning. This option starts unchecked.
Select it to apply the control application's recommended background settings through its shared backend.
The package manifest and command-line plan list the choices from that application's catalog.
Use the control application's Windows settings page for individual changes and recovery.

For a command-line installation, add `-ApplySystemTuning` to `install.cmd`.
The choice survives an installation restart or a failed attempt.
Use `-SkipSystemTuning` to cancel a saved choice when retrying.
The setup executable accepts `--apply-system-tuning`.
Plan and dry-run modes show the choice without running the tuning backend.

The uninstaller offers to restore machine-wide settings before removal.
Use `uninstall.cmd -RestoreSystemTuning` to select this action or `-KeepSystemTuning` to keep the settings.
A failed restoration stops removal.
The uninstaller preserves machine and user recovery records, including those belonging to other accounts.
It also keeps verified recovery scripts under `%ProgramData%\amdgpu-wddm\system-tuning-recovery`.

After removal, restore each account's settings from that account:

```powershell
powershell -NoProfile -File "$env:ProgramData\amdgpu-wddm\system-tuning-recovery\system-tuning.ps1" -Action RestoreAll -Scope User
```

Elevation with another account does not restore the original account's settings.
Machine-wide recovery uses the same command with `-Scope Machine` from an administrator terminal.
