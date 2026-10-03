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
  Get it from Microsoft (search for "Visual C++ Redistributable latest supported downloads").
- 2 GB of free space on the system drive.
- A keyboard and a monitor on the BC-250. The installer does not need a network connection or another computer.

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
5. Phase 2: the installer installs the certificate, the driver and the user-mode drivers, and sets the registry
   values. The screen can go black for some seconds when the driver starts. Then the computer restarts again.
6. Phase 3: after the second logon, the installer verifies the driver and shows a pass or fail table.

You can run the verification again at any time with `verify.cmd` in `C:\Program Files\amdgpu-wddm`.

"Test Mode" is shown in the lower-right corner of the desktop while test signing is on. This is normal.

## The small blue window after logon

After each logon, a small blue window with a clock is shown in the top-left corner for up to two minutes. It is
the "amdgpu-wddm start confirm" scheduled task. The driver counts each start that is not confirmed. After two
unconfirmed starts, Windows uses Microsoft Basic Display Adapter again, to prevent a start loop. The task keeps the
desktop drawing for some seconds and then confirms the start. Do not close the window. If you log on with an
account that is not an administrator, the task does not run, and the driver falls back after two restarts.

## What the installer changes

| Item | Location |
|---|---|
| Test signing | `bcdedit /set {current} testsigning on` (only after you type YES) |
| Test certificate | LocalMachine Root and TrustedPublisher stores |
| Kernel-mode driver | driver package `bc250kmd.inf` (driver store, service `bc250kmd`) |
| Driver settings | `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters` |
| User-mode drivers | `C:\Program Files\amdgpu-wddm\` (`d3d12`, `desktop`, `d3d11`, `vulkan`, `tools`, `control`) and `C:\Windows\System32\bc250umd.dll` |
| Graphics registration | the GPU's software key (`UserModeDriverName`, `VulkanDriverName`), `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` |
| Router policy and profiles | `HKLM\SOFTWARE\amdgpu-wddm` |
| GPU firmware | `C:\BC250\firmware` (8 files from linux-firmware; `LICENSE.amdgpu` in the package) |
| Start confirmation | scheduled task "amdgpu-wddm start confirm" |
| Installer state and logs | `C:\ProgramData\amdgpu-wddm` |

Default driver settings: automatic clock control (DPM) on, maximum 1500 MHz. The thermal limits of the driver do not
change. The driver writes no debug log files. To change the maximum clock at install time, use
`install.cmd -DpmMaxMHz <1000-2000>`.

## What works and what does not

- Windows desktop composition (DWM) runs on the GPU.
- Direct3D 12 applications run on the GPU through the Windows Direct3D 12 runtime, at feature level 12_1.
- Vulkan applications run on the GPU through the Vulkan ICD.
- Direct3D 11 applications run on the CPU by default. Only applications on the allowlist in
  `HKLM\SOFTWARE\amdgpu-wddm\AppRouter` (value `Allow`) use the GPU. The control application can change the list.
- 32-bit applications do not have a driver yet.
- The Witcher 3 (Direct3D 12 version) has an application profile.

## If something fails

- Preflight `fail`: correct the item that the table names, then run `install.cmd` again.
- The screen stays black after a restart: wait two minutes. If it stays black, restart the computer with the power
  button. After two failed starts, Windows uses Microsoft Basic Display Adapter again.
- To start Windows without the driver: hold `Shift` and select Restart, then Troubleshoot > Advanced options >
  Startup Settings > Restart > `3` (Enable low-resolution video), or `4` (Safe Mode). Then run `uninstall.cmd`.
- Verification fails: run `verify.cmd`, then make a diagnostics bundle (see "How to report a bug").

## Uninstall

1. Run `uninstall.cmd` as administrator (in the package folder or in `C:\Program Files\amdgpu-wddm`).
2. Type `YES`. The uninstaller removes the driver, the files, the registry values, the task and the certificate.
   The GPU goes back to Microsoft Basic Display Adapter.
3. If the installer turned on test signing, the uninstaller asks if it must turn it off. Use
   `uninstall.cmd -DisableTestSigning` or `-KeepTestSigning` to answer in advance.
4. Restart the computer. If BitLocker is on, have the recovery key ready: changing the boot options again can cause
   a recovery key prompt.

You can also use the System Restore point "amdgpu-wddm before install" if the installer made one.

## How to report a bug

1. Open "amdgpu-wddm Control" from the Start menu and select the diagnostics bundle function. It makes one zip file.
2. Write down what you did, what you expected, and what happened. Add the name and the version of the game or
   application, the settings, and the time of the problem.
3. Send the zip file and your notes to the project's issue tracker.

The diagnostics bundle contains technical data only:

- the driver version, the package manifest version and the installer state (`C:\ProgramData\amdgpu-wddm`);
- the driver settings in `HKLM\SYSTEM\CurrentControlSet\Services\bc250kmd\Parameters` and the router policy;
- the driver's start health, clock and temperature readings;
- the installer logs, the verification results and `start-confirm.log`;
- the Windows build number and the System event log entries of the display driver (crash codes, TDR events).

It does not contain user names, file names of your documents, network names, serial numbers or MAC addresses.
Examine the zip file before you send it. If you find personal data in it, remove that data, and tell us in the bug
report so that we can correct the bundle.
