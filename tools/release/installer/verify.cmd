@echo off
rem amdgpu-wddm: checks the installed driver (driver bound, version, start health, D3D12 FL 12_1, Vulkan).
rem Arguments pass through. Exit code 10 = this window handed over to an elevated one (UAC), which shows the result.
set AMDGPU_WDDM_LAUNCHER=%~f0
set AMDGPU_WDDM_LAUNCHER_FIXED=Verify
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" -Verify %*
if %ERRORLEVEL%==10 exit /b 0
echo.
pause
