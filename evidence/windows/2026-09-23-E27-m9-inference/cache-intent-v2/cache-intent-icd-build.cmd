@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" > NUL
set TEMP=P:\bc-250\scratch\tmp
set TMP=%TEMP%
C:\msys64\mingw64\bin\ninja.exe -C P:\bc-250\scratch\mesa-wddm2-build src/amd/vulkan/vulkan_radeon.dll
