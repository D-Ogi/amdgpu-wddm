@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set PATH=P:\bc-250\toolchain\winflexbison-2.5.25;%PATH%
set INCLUDE=P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um;P:\bc-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared;%INCLUDE%
set PYTHONPATH=P:\bc-250\scratch\py
set TEMP=P:\bc-250\scratch\tmp
set TMP=P:\bc-250\scratch\tmp
C:\msys64\mingw64\bin\ninja.exe -C P:\bc-250\scratch\mesa-d3d-build src/gallium/targets/d3d10umd/bc250d3d.dll
