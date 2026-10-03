@echo off
rem amdgpu-wddm: checks the installed driver (driver bound, version, start health, D3D12 FL 12_1, Vulkan).
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" -Verify %*
echo.
pause
