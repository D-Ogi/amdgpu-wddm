@echo off
rem amdgpu-wddm tester installer. Double-click it and accept the administrator prompt. Example: install.cmd -DryRun
rem Arguments pass through. Exit code 10 = this window handed over to an elevated one (UAC), which shows the result.
set AMDGPU_WDDM_LAUNCHER=%~f0
set AMDGPU_WDDM_LAUNCHER_FIXED=
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" %*
if %ERRORLEVEL%==10 exit /b 0
echo.
pause
