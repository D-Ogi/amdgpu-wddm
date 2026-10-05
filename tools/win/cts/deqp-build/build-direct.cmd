@echo off
rem cts-direct.dll: it exports vkGetInstanceProcAddr and forwards to the sibling amdgpu_wddm_radv.dll's
rem vk_icdGetInstanceProcAddr, so the CTS reaches the ICD with no Vulkan loader between them. The source is
rem next to this script and the flags are the ones of the E34 build: /W4 /WX /wd4191 /O2 /MT /LD.
rem
rem   build-direct.cmd <out dir> [temp dir]
rem
rem The output directory must lie outside drive C: and outside this repository.
setlocal
if "%~1"=="" echo usage: build-direct.cmd ^<out dir^> [temp dir] & exit /b 2
set OUT=%~1
set WORKTMP=%~2
if "%WORKTMP%"=="" set WORKTMP=%OUT%
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%WORKTMP%" mkdir "%WORKTMP%"
set TEMP=%WORKTMP%
set TMP=%WORKTMP%
set VSCMD_SKIP_SENDTELEMETRY=1
set VSPATH=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" echo vswhere found no Visual Studio & exit /b 2
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cd /d "%OUT%"
cl /nologo /W4 /WX /wd4191 /O2 /MT /LD "%~dp0cts-direct.c" /Fe:cts-direct.dll /link /NOLOGO
exit /b %errorlevel%
