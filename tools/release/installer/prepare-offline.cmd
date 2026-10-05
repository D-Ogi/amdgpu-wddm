@echo off
rem amdgpu-wddm: prepares a folder for an installation without internet, on a PC that has internet. Example:
rem   prepare-offline.cmd -Destination E:\amdgpu-wddm-offline
rem The folder gets this package and the GPU firmware, each SHA256 checked. On the BC-250, run install.cmd from that
rem folder: it takes the firmware from its firmware\ folder and needs no network. No administrator rights needed.
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\prepare-offline.ps1" %*
echo.
pause
