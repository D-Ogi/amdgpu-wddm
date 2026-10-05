@echo off
rem Builds fake-deqp.exe, the stand-in for deqp-vk.exe that run-tests.sh drives. The source is next to this
rem script and the output goes where the caller says, which must be outside drive C: and outside this repository.
rem
rem   build-fake.cmd <out dir>
setlocal
if "%~1"=="" echo usage: build-fake.cmd ^<out dir^> & exit /b 2
set OUT=%~1
if not exist "%OUT%" mkdir "%OUT%"
set TEMP=%OUT%
set TMP=%OUT%
set VSCMD_SKIP_SENDTELEMETRY=1
set VSPATH=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" echo vswhere found no Visual Studio & exit /b 2
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%OUT%"
cl /nologo /W3 /O2 /MT "%~dp0fake-deqp.c" /Fe:fake-deqp.exe /link /NOLOGO
exit /b %errorlevel%
