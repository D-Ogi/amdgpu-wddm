@echo off
if not defined BC250_ROOT set "BC250_ROOT=%~dp0..\..\.."
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=%BC250_ROOT%\scratch\tmp
set TMP=%BC250_ROOT%\scratch\tmp
C:\msys64\mingw64\bin\ninja.exe -C %BC250_ROOT%\scratch\mesa-wddm2-build -j 8 src/amd/vulkan/vulkan_radeon.dll
