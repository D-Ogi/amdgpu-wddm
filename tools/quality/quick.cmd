@echo off
setlocal
rem Fast local gates only: no lab, CTS, benchmark or soak test.
set "BC250_WORKSPACE=%~1"
if not defined BC250_WORKSPACE set "BC250_WORKSPACE=%BC250_ROOT%"
if not defined BC250_WORKSPACE for %%I in ("%~dp0..\..\..") do set "BC250_WORKSPACE=%%~fI"
set "BC250_QUALITY_OUT=%~2"
if not defined BC250_QUALITY_OUT set "BC250_QUALITY_OUT=%BC250_WORKSPACE%\scratch\quality\fast"
set "BC250_QUALITY_PROFILE=%~4"
if not defined BC250_QUALITY_PROFILE set "BC250_QUALITY_PROFILE=Full"
set "BC250_QUALITY_KITS=%~5"
if not defined BC250_QUALITY_KITS set "BC250_QUALITY_KITS=%BC250_WORKSPACE%\toolchain\nuget"
set "BC250_QUALITY_REPO=%~3"
if defined BC250_QUALITY_REPO set "BC250_QUALITY_REPO=%BC250_QUALITY_REPO%\."
set "TEMP=%BC250_WORKSPACE%\scratch\tmp"
set "TMP=%TEMP%"
pwsh -NoProfile -File "%~dp0quick.ps1" -Workspace "%BC250_WORKSPACE%\." -Out "%BC250_QUALITY_OUT%\." -RepoRoot "%BC250_QUALITY_REPO%" -Profile "%BC250_QUALITY_PROFILE%" -Kits "%BC250_QUALITY_KITS%\."
exit /b %errorlevel%
