@echo off
rem Release build of deqp-vk from VK-GL-CTS vulkan-cts-1.4.6.2 (commit f6a29701). The option set is the one the
rem pinned binary of 2026-10-01 was built with; only the paths come from the command line.
rem
rem   build-cts.cmd <source tree> <build dir> <temp dir>
rem
rem The three directories must lie outside drive C: (owner's rule) and outside this repository. CMake, Ninja
rem and Python come from the environment when it sets them: BC250_CMAKE, BC250_NINJA, BC250_PYTHON. The
rem defaults are the versions of the pinned build (CMake 3.25.2 and Ninja 1.11.1 from msys64, Python 3.14.0).
setlocal
if "%~3"=="" echo usage: build-cts.cmd ^<source tree^> ^<build dir^> ^<temp dir^> & exit /b 2
set SRC=%~1
set BUILD=%~2
set WORKTMP=%~3
if not exist "%SRC%\CMakeLists.txt" echo no CMakeLists.txt in %SRC% & exit /b 2
if not exist "%WORKTMP%" mkdir "%WORKTMP%"
if "%BC250_CMAKE%"=="" set BC250_CMAKE=C:\msys64\mingw64\bin\cmake.exe
if "%BC250_NINJA%"=="" set BC250_NINJA=C:/msys64/mingw64/bin/ninja.exe
if "%BC250_PYTHON%"=="" set BC250_PYTHON=C:/Python314/python.exe
set VSCMD_SKIP_SENDTELEMETRY=1
set VSPATH=
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath`) do set VSPATH=%%i
if "%VSPATH%"=="" echo vswhere found no Visual Studio & exit /b 2
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
set TEMP=%WORKTMP%
set TMP=%WORKTMP%
set PYTHONPYCACHEPREFIX=%WORKTMP%\pycache
set PYTHONDONTWRITEBYTECODE=1
set CMAKE_BUILD_PARALLEL_LEVEL=12
rem zlib and libpng come from external\, not from msys64, so the binary does not depend on an msys install.
"%BC250_CMAKE%" -S "%SRC%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM=%BC250_NINJA% ^
  -DCMAKE_BUILD_TYPE=Release -DDEQP_TARGET=default -DDEQP_DISABLE_VK_VIDEO_TESTS=ON ^
  -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=ON -DCMAKE_DISABLE_FIND_PACKAGE_PNG=ON ^
  -DCMAKE_IGNORE_PREFIX_PATH=C:/msys64/mingw64 -DPython3_EXECUTABLE=%BC250_PYTHON%
if errorlevel 1 exit /b 1
"%BC250_CMAKE%" --build "%BUILD%" --target deqp-vk --parallel 12
exit /b %errorlevel%
