# amdgpu-wddm 0.7.198.100: release note for testers

This is a tester release of amdgpu-wddm, an experimental Windows driver for the GPU of the ASRock BC-250
(AMD Cyan Skillfish, gfx1013). It is for people who want to test the driver on their own BC-250 and report what
happens. It is not ready for daily use. Read `INSTALL.md` in this package before you install.

Package version: `0.7.198.100-tester.10`. New since `0.7.197.100-tester.6`:

- The GPU draws the desktop. Defect BD-058 is fixed: the desktop no longer stops when a File Explorer window opens.
- Kernel-mode driver 0.7.198.2: a normal restart keeps the GPU desktop path open (defect BD-059).
- An upgrade keeps the settings you changed (`INSTALL.md`, "Install").
- The verification has two more checks, "full WDDM gate" and "GPU desktop path".
- The control application has a Recovery page.
- The package does not contain the AMD GPU firmware. The installer downloads it from the linux-firmware project
  during the installation and checks every file (`INSTALL.md`, "GPU firmware"). A BC-250 without internet can use
  files downloaded on another computer: `install.cmd -FirmwareDir <folder>`.
- `THIRD-PARTY.md` lists every file of the package with its licence; the licence texts are in `licenses\`, and the
  installer copies both to `C:\Program Files\amdgpu-wddm\licenses`.

## What works

- The driver installs on the BC-250 and replaces Microsoft Basic Display Adapter. The installer also upgrades an
  installed release: run `install.cmd` of the newer package. The upgrade keeps the settings you changed.
- The desktop is drawn by the GPU.
- Direct3D 12 applications run on the GPU through the Windows Direct3D 12 runtime, at feature level 12_1.
- Vulkan applications run on the GPU.
- Automatic clock control (DPM) is on: the GPU clock follows the load, up to 1500 MHz. The thermal limits of the
  driver do not change.
- The control application ("amdgpu-wddm Control" in the Start menu) shows the driver state, makes bug reports and
  has a Recovery page.

## Known limits

- If the driver closes the GPU desktop path, the desktop is drawn by the CPU, and desktop animations are slower. The
  driver does this by itself, for example when the last start ended in a crash or a power loss. The button "Reopen the GPU
  desktop path" on the Recovery page of amdgpu-wddm Control opens it again.
- Direct3D 11 applications and games run on the CPU (software rendering), so they are slow. Use the Direct3D 12
  mode of a game if it has one.
- 32-bit applications do not have a driver yet.
- Secure Boot must be off, and Windows runs in test mode ("Test Mode" shows on the desktop), because the driver is
  signed with a test certificate.
- The installation needs internet access for the GPU firmware, or the `-FirmwareDir` folder.
- The Microsoft Visual C++ 2015-2022 Redistributable (x64) must be installed. The installer stops and tells you if
  it is missing.
- After each logon, a small blue window shows for about one minute. Do not close it: it confirms the driver start.
  Without the confirmation, the driver falls back to Microsoft Basic Display Adapter after two restarts.
- If the desktop compositor (DWM) restarts, the screen goes black for a moment and comes back. After that, some
  Windows 11 apps (the command bar of File Explorer, Task Manager) ignore mouse clicks until you restart the
  computer; the keyboard still works. This is Windows behaviour, also with Microsoft Basic Display Adapter. The
  installer does not restart the compositor, and `verify.cmd` shows a warning when it was restarted.

## How to report a bug

Bug reports go to GitHub Issues: https://github.com/D-Ogi/amdgpu-wddm/issues/new/choose (select "Bug report").

1. Open "amdgpu-wddm Control" from the Start menu and make a bug report. Read the files that the application shows
   before it writes them.
2. The application writes one zip file to your desktop. It sends nothing.
3. Fill in the form: the package version, the game or application and its API (Direct3D 12, Direct3D 11, Vulkan),
   what you did, what you expected and what happened, and the time of the problem.
4. Attach the zip file to the issue.

## If the desktop is black or the computer does not start correctly

1. If the screen stays black after a restart, wait two minutes. Then restart with the power button. After two
   failed starts, Windows uses Microsoft Basic Display Adapter again.
2. If the desktop is black after an update, but you can still start programs (for example `Win` + type
   "amdgpu-wddm Control"), select "Desktop on the CPU route" on its Recovery page.
3. To start Windows without the driver: hold `Shift` and select Restart, then Troubleshoot > Advanced options >
   Startup Settings > Restart > `4` (Safe Mode).
4. In Safe Mode, run `uninstall.cmd` in `C:\Program Files\amdgpu-wddm`. It removes the driver and its files.
5. You can also use the System Restore point "amdgpu-wddm before install".
