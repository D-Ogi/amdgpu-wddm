@echo off
rem amdgpu-wddm uninstaller. Arguments pass through, for example: uninstall.cmd -DryRun
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\uninstall.ps1" %*
echo.
pause
