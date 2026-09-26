@echo off
rem Run from the workspace root. The supplied build CMD must preserve its exit code.
if "%~3"=="" (
  echo Usage: build-radv-checked.cmd build.cmd compile_commands.json output-directory
  exit /b 2
)
call "%~1"
if errorlevel 1 exit /b 1
python "%~dp0verify-prototype-gate.py" --compile-commands "%~2" --out "%~3"
if errorlevel 1 exit /b 1
for %%I in ("%~dp0..\..\..") do set "BC250_WORKSPACE=%%~fI"
call "%~dp0..\..\tools\quality\quick.cmd" "%BC250_WORKSPACE%" "%~3\quick"
exit /b %errorlevel%
