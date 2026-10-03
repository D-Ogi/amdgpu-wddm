# amdgpu-wddm 0.7.197.100: release note for testers

This is the first tester release of amdgpu-wddm, an experimental Windows driver for the GPU of the ASRock BC-250
(AMD Cyan Skillfish, gfx1013). It is for people who want to test the driver on their own BC-250 and report what
happens. It is not ready for daily use. Read `INSTALL.md` in this package before you install.

Package version: `0.7.197.100-tester.7`. Its drivers, programs and installer passed the installation test on our
BC-250 as `0.7.197.100-tester.6` (upgrade, restart, all 8 verification checks, start confirmation). This package
adds `THIRD-PARTY.md` and the licence texts in `licenses\`, and replaces a wrong `LICENSE.amdgpu` file.

## What works

- The driver installs on the BC-250 and replaces Microsoft Basic Display Adapter. The installer also upgrades an
  installed release: run `install.cmd` of the newer package.
- Direct3D 12 applications run on the GPU through the Windows Direct3D 12 runtime, at feature level 12_1.
- Vulkan applications run on the GPU.
- Automatic clock control (DPM) is on: the GPU clock follows the load, up to 1500 MHz. The thermal limits of the
  driver do not change.
- The control application ("amdgpu-wddm Control" in the Start menu) shows the driver state and makes bug reports.

## Known limits

- The desktop is drawn by the CPU, not the GPU (defect BD-058: on the GPU route, the desktop stops when a File
  Explorer window opens). Desktop animations can be slower than with a normal driver.
- Direct3D 11 applications and games run on the CPU (software rendering), so they are slow. Use the Direct3D 12
  mode of a game if it has one.
- 32-bit applications do not have a driver yet.
- Secure Boot must be off, and Windows runs in test mode ("Test Mode" shows on the desktop), because the driver is
  signed with a test certificate.
- The Microsoft Visual C++ 2015-2022 Redistributable (x64) must be installed. The installer stops and tells you if
  it is missing.
- After each logon, a small blue window shows for about one minute. Do not close it: it confirms the driver start.
  Without the confirmation, the driver falls back to Microsoft Basic Display Adapter after two restarts.
- The adapter name in Windows is "BC-250 GPU (bc250kmd, display-only, lab build)". The words "display-only"
  and "lab build" are wrong for this release; a later release corrects the name.

## How to report a bug

1. Open "amdgpu-wddm Control" from the Start menu and make a bug report. Read the files that the application shows
   before it writes them.
2. The application writes one zip file to your desktop. It sends nothing.
3. Write down what you did, what you expected and what happened, with the name, version and settings of the game or
   application and the time of the problem.
4. Send the zip file and your notes to the project's issue tracker.

## If the computer does not start correctly

1. If the screen stays black after a restart, wait two minutes. Then restart with the power button. After two
   failed starts, Windows uses Microsoft Basic Display Adapter again.
2. To start Windows without the driver: hold `Shift` and select Restart, then Troubleshoot > Advanced options >
   Startup Settings > Restart > `4` (Safe Mode).
3. In Safe Mode, run `uninstall.cmd` in `C:\Program Files\amdgpu-wddm`. It removes the driver and its files.
4. You can also use the System Restore point "amdgpu-wddm before install".
