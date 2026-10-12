@echo off
rem amdgpu-wddm tester installer at the root of the package.
rem   Double-click it:      the setup window opens (setup.exe). It asks for the administrator prompt itself and
rem                         shows every check and every change before it changes anything.
rem   With arguments:       the engine runs in this console, as before. Example: install.cmd -DryRun
rem   install.cmd -Console: the engine in this console (the window is not opened). Every other switch on the
rem                         line still reaches the engine, so install.cmd -Console -DryRun is a dry run.
rem Exit code 10 = this window handed over to an elevated one (UAC), which shows the result.
set AMDGPU_WDDM_LAUNCHER=%~f0
set AMDGPU_WDDM_LAUNCHER_FIXED=
rem -Console selects the console and nothing else. It is taken out of the command line here, and every other
rem argument goes to the engine in its own order and with its quotation marks. "shift /1" keeps %0, so %~dp0
rem below still names this folder.
set "AMDGPU_WDDM_CONSOLE="
set "AMDGPU_WDDM_ARGS="
:readargs
if "%~1"=="" goto :readargs_done
if /i "%~1"=="-Console" (set "AMDGPU_WDDM_CONSOLE=1") else (set "AMDGPU_WDDM_ARGS=%AMDGPU_WDDM_ARGS% %1")
shift /1
goto :readargs
:readargs_done
if defined AMDGPU_WDDM_CONSOLE (
    call :engine%AMDGPU_WDDM_ARGS%
    goto :finish
)
rem No argument and the window is in this package: the tester who clicks the obvious file gets the window.
rem Where setup.exe is depends on the shape of the package, in the same order as Get-PackageSetupExe in
rem installer\common.ps1: this folder's own copy first (a repair set and a prepared folder keep one), then the
rem folder above, which is where the unpacked release keeps setup.exe beside the folder files\. The folder above
rem is read only when this folder is named files\, so a foreign setup.exe beside a prepared folder cannot stand
rem in for the package's own.
if not defined AMDGPU_WDDM_ARGS if exist "%~dp0setup.exe" (
    start "" "%~dp0setup.exe"
    exit /b 0
)
for %%I in ("%~dp0.") do set AMDGPU_WDDM_ROOTNAME=%%~nxI
if not defined AMDGPU_WDDM_ARGS if /i "%AMDGPU_WDDM_ROOTNAME%"=="files" if exist "%~dp0..\setup.exe" (
    start "" "%~dp0..\setup.exe"
    exit /b 0
)
call :engine%AMDGPU_WDDM_ARGS%

:finish
if %ERRORLEVEL%==10 exit /b 0
echo.
pause
exit /b %ERRORLEVEL%

:engine
%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" %*
exit /b %ERRORLEVEL%
