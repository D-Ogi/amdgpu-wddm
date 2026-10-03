@echo off
rem amdgpu-wddm tester installer. Right-click and choose 'Run as administrator', or double-click and accept the prompt.
rem Arguments pass through, for example: install.cmd -DryRun
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" %*
echo.
pause
