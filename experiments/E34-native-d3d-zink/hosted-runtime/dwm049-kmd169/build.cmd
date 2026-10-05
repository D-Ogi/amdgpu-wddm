@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set TEMP=P:\BC-250\scratch\tmp
set TMP=P:\BC-250\scratch\tmp
cd /d P:\BC-250\scratch\g0-hosted\dwm049
set INCLUDE=P:\BC-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\um;P:\BC-250\toolchain\nuget\microsoft.windows.wdk.x64\c\Include\10.0.26100.0\shared;%INCLUDE%
cl /nologo /O2 /MD /W4 /WX /LD router.cpp /Fe:router.dll

if errorlevel 1 exit /b 1
cl /nologo /O2 /MT /W4 /WX composition-control.cpp /Fe:composition-control.exe /link /SUBSYSTEM:WINDOWS user32.lib gdi32.lib dwmapi.lib
