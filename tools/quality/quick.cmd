@echo off
rem Fast local gates only: no lab, CTS, benchmark or soak test.
set "BC250_WORKSPACE=%~1"
if not defined BC250_WORKSPACE set "BC250_WORKSPACE=%BC250_ROOT%"
if not defined BC250_WORKSPACE for %%I in ("%~dp0..\..\..") do set "BC250_WORKSPACE=%%~fI"
set "BC250_QUALITY_OUT=%~2"
if not defined BC250_QUALITY_OUT set "BC250_QUALITY_OUT=%BC250_WORKSPACE%\scratch\quality\fast"
for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set "BC250_VS=%%I"
if not defined BC250_VS exit /b 2
call "%BC250_VS%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1
set "TEMP=%BC250_WORKSPACE%\scratch\tmp"
set "TMP=%TEMP%"
set "INCLUDE=%BC250_WORKSPACE%\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um;%BC250_WORKSPACE%\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared;%INCLUDE%"
pwsh -NoProfile -File "%~dp0quick.ps1" -Workspace "%BC250_WORKSPACE%" -Out "%BC250_QUALITY_OUT%" -RepoRoot "%~3"
exit /b %errorlevel%
